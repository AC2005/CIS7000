#include "heartbeat.h"
#include "shared_memory.h"
#include "ring_buffer.h"
#include "seqlock.h"
#include "order_book.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>

static constexpr const char* SHM_HEARTBEAT  = "/trading_heartbeat";
static constexpr uint64_t    HEARTBEAT_STALE = 500'000'000ULL;  // 500ms
static constexpr const char* RECEIVER_BIN    = "./market_data_receiver";
static constexpr const char* ENGINE_BIN      = "./strategy_engine";

static volatile bool g_running = true;
static SharedMemory* g_shm_hb  = nullptr;

static void on_signal(int signo) {
    fprintf(stderr, "\nsupervisor: caught signal %d, shutting down\n", signo);
    g_running = false;
}

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
}

// process table — tracks pids of managed processes
struct ManagedProcess {
    const char* name;
    const char* binary;
    pid_t       pid;
    uint64_t    last_restart_ns;
    size_t      restart_count;
    ProcessRole role;
};

static ManagedProcess g_processes[] = {
    { "market_data_receiver", RECEIVER_BIN, -1, 0, 0, ProcessRole::MARKET_DATA_RECEIVER },
    { "strategy_engine",      ENGINE_BIN,   -1, 0, 0, ProcessRole::STRATEGY_ENGINE      },
};
static constexpr size_t NUM_PROCESSES = sizeof(g_processes) / sizeof(g_processes[0]);

// spawn a managed process — fork + exec
static pid_t spawn(ManagedProcess& proc) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("supervisor: fork failed");
        return -1;
    }

    if (pid == 0) {
        // child — exec the binary
        char* argv[] = { (char*)proc.binary, nullptr };
        execv(proc.binary, argv);
        // if we get here exec failed
        perror("supervisor: execv failed");
        _exit(1);
    }

    // parent
    uint64_t now = now_ns();
    proc.pid              = pid;
    proc.last_restart_ns  = now;
    proc.restart_count++;

    printf("supervisor: spawned %s  pid=%d  restart_count=%zu\n",
           proc.name, pid, proc.restart_count);
    return pid;
}

// reap any dead children without blocking
static void reap_children() {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (size_t i = 0; i < NUM_PROCESSES; i++) {
            if (g_processes[i].pid == pid) {
                if (WIFEXITED(status)) {
                    printf("supervisor: %s (pid=%d) exited with code %d\n",
                           g_processes[i].name, pid, WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    printf("supervisor: %s (pid=%d) killed by signal %d\n",
                           g_processes[i].name, pid, WTERMSIG(status));
                }
                g_processes[i].pid = -1;
                break;
            }
        }
    }
}

// check if a pid is still running
static bool pid_alive(pid_t pid) {
    if (pid <= 0) return false;
    // kill(pid, 0) checks existence without sending a signal
    return kill(pid, 0) == 0;
}

int main() {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGCHLD, SIG_DFL);  // let waitpid handle child cleanup

    printf("supervisor: starting\n");

    // create heartbeat shared memory
    SharedMemory shm_hb = SharedMemory::create(SHM_HEARTBEAT,
                              sizeof(HeartbeatRegion));
    g_shm_hb = &shm_hb;

    HeartbeatRegion* hb_region = static_cast<HeartbeatRegion*>(shm_hb.get_ptr());

    // initialize heartbeats
    hb_region->beats[(size_t)ProcessRole::MARKET_DATA_RECEIVER].init("receiver");
    hb_region->beats[(size_t)ProcessRole::STRATEGY_ENGINE].init("engine");

    printf("supervisor: heartbeat region created at %p\n", (void*)hb_region);

    // initial spawn — start receiver first, then engine
    spawn(g_processes[0]);
    usleep(200'000);  // 200ms — give receiver time to create shared memory
    spawn(g_processes[1]);

    printf("supervisor: monitoring processes (heartbeat threshold: %zu ms)\n\n",
           (size_t)(HEARTBEAT_STALE / 1'000'000));

    // ---- supervisor main loop ----
    while (g_running) {
        usleep(100'000);  // check every 100ms

        reap_children();

        for (size_t i = 0; i < NUM_PROCESSES; i++) {
            ManagedProcess& proc = g_processes[i];
            Heartbeat&       hb   = hb_region->beats[(size_t)proc.role];

            bool process_running = pid_alive(proc.pid);
            bool heartbeat_stale = hb.is_stale(HEARTBEAT_STALE);

            if (!process_running || heartbeat_stale) {
                const char* reason = !process_running
                    ? "process died"
                    : "heartbeat stale";

                printf("supervisor: %s detected (%s)  pid=%d  "
                       "restarts=%zu\n",
                       proc.name, reason, proc.pid,
                       proc.restart_count);

                // increment generation so other processes know a restart happened
                hb.generation.fetch_add(1, std::memory_order_release);
                hb.alive.store(false, std::memory_order_release);

                // kill stale process if it's somehow still running
                if (process_running && proc.pid > 0) {
                    kill(proc.pid, SIGTERM);
                    usleep(50'000);
                    if (pid_alive(proc.pid)) kill(proc.pid, SIGKILL);
                }

                // wait a moment before restarting
                usleep(50'000);
                spawn(proc);
            }
        }

        // periodic status print every 5 seconds
        static uint64_t last_status = 0;
        uint64_t now = now_ns();
        if (now - last_status > 5'000'000'000ULL) {
            last_status = now;
            printf("supervisor: status\n");
            for (size_t i = 0; i < NUM_PROCESSES; i++) {
                printf("  %s  pid=%d  restarts=%zu  ",
                       g_processes[i].name,
                       g_processes[i].pid,
                       g_processes[i].restart_count);
                hb_region->beats[(size_t)g_processes[i].role].print();
            }
        }
    }

    // graceful shutdown — send SIGTERM to all managed processes
    printf("supervisor: shutting down managed processes\n");
    for (size_t i = 0; i < NUM_PROCESSES; i++) {
        if (g_processes[i].pid > 0) {
            printf("supervisor: sending SIGTERM to %s (pid=%d)\n",
                   g_processes[i].name, g_processes[i].pid);
            kill(g_processes[i].pid, SIGTERM);
        }
    }

    // wait for children to exit
    usleep(500'000);
    reap_children();

    g_shm_hb = nullptr;
    printf("supervisor: done\n");
    return 0;
}