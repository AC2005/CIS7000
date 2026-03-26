# Development with Docker

## Build and Run
```bash
# Build the container
docker-compose build

# Start the development environment
docker-compose up -d

# Enter the container
docker-compose exec trading-dev bash

# Inside container - build the project
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Or use the aliases
build-release  # Release build
build-debug    # Debug build
build-asan     # With AddressSanitizer
build-tsan     # With ThreadSanitizer
```

## Running the System
```bash
# Start supervisor (spawns both processes)
./build/supervisor

# Or run processes individually
./build/market_data_receiver &
./build/strategy_engine &
```

## Testing
```bash
# Run unit tests
./build/unit_tests

# Run with sanitizers
build-asan && ./build/unit_tests
build-tsan && ./build/unit_tests

# Run benchmarks
./build/benchmarks

# Memory leak check
valgrind --leak-check=full ./build/unit_tests
```

## Cleanup
```bash
# Stop container
docker-compose down

# Clean shared memory (if processes crash)
docker-compose exec trading-dev rm -f /dev/shm/trading_*
```

## Notes

- Huge pages enabled (128 x 2MB = 256MB)
- Shared memory size: 2GB
- Real-time priority available (requires privileged mode)
- perf tools available for profiling

## Troubleshooting

**Huge pages not working:**
```bash
# Inside container
cat /proc/meminfo | grep Huge
# If zero, check host system huge page configuration
```

**Permission denied for SCHED_FIFO:**
```bash
# Verify container has CAP_SYS_NICE
docker-compose exec trading-dev capsh --print | grep sys_nice
```

**Shared memory full:**
```bash
# Check usage
df -h /dev/shm
# Clean up
rm /dev/shm/trading_*
```
```

And a `.dockerignore`:
```
# .dockerignore
build/
.git/
.vscode/
*.swp
*.swo
*~
.DS_Store
*.o
*.a