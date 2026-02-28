#include "signal_handler.h"
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <cstdlib>
#include <cstring>

// global — only accessible within this translation unit
static RegionManager* g_region_manager = nullptr;

void handler(int signo, siginfo_t* info, void* context) {
    void* faulting_addr = info->si_addr;
    auto [fault_type, region_id] = g_region_manager->is_guard_page(faulting_addr);

    if (fault_type == FaultType::NO_FAULT) {
        struct sigaction sa;
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGSEGV, &sa, nullptr);
        raise(SIGSEGV);
        return;
    }

    const char* header = "\n=== GUARD PAGE VIOLATION ===\n";
    write(STDERR_FILENO, header, strlen(header));

    const char* type_str = (fault_type == FaultType::OVERFLOW)
        ? "Type:   OVERFLOW (past end of region)\n"
        : "Type:   UNDERFLOW (before start of region)\n";
    write(STDERR_FILENO, type_str, strlen(type_str));

    if (region_id.has_value()) {
        const Region& r = g_region_manager->regions[region_id.value()];
        write(STDERR_FILENO, "Region: ", 8);
        size_t name_len = 0;
        while (name_len < sizeof(r.name) && r.name[name_len] != '\0') name_len++;
        write(STDERR_FILENO, r.name, name_len);
        write(STDERR_FILENO, "\n", 1);
    }

    const char* trace_header = "Stack trace:\n";
    write(STDERR_FILENO, trace_header, strlen(trace_header));
    void* buffer[32];
    int frames = backtrace(buffer, 32);
    backtrace_symbols_fd(buffer, frames, STDERR_FILENO);

    _exit(1);
}

void install_signal_handler(RegionManager* rm) {
    g_region_manager = rm;

    stack_t alt_stack;
    alt_stack.ss_sp = malloc(SIGSTKSZ);
    alt_stack.ss_size = SIGSTKSZ;
    alt_stack.ss_flags = 0;
    sigaltstack(&alt_stack, nullptr);

    struct sigaction sa;
    sa.sa_sigaction = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
}