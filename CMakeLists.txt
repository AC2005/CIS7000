cmake_minimum_required(VERSION 3.16)
project(TradingSystem CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(ENABLE_ASAN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
endif()

if(ENABLE_TSAN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
endif()

find_package(Threads REQUIRED)

include_directories(
    ${PROJECT_SOURCE_DIR}/include
)

set(SOURCES
    src/virtual_memory_manager.cpp
    src/slab_allocator.cpp
    src/signal_handler.cpp
    src/shared_memory.cpp
    src/order_book.cpp
)

add_executable(virtual_memory_manager_test
    tests/virtual_memory_manager_test.cpp
    ${SOURCES}
)

add_executable(slab_allocator_test
    tests/slab_allocator_test.cpp
    ${SOURCES}
)

add_executable(ring_buffer_test
    tests/ring_buffer_test.cpp
    ${SOURCES}
)

add_executable(allocator_benchmark
    tests/allocator_benchmark.cpp
    ${SOURCES}
)

add_executable(market_data_receiver
    src/market_data_receiver.cpp
    ${SOURCES}
)

add_executable(strategy_engine
    src/strategy_engine.cpp
    ${SOURCES}
)

add_executable(seqlock_test
    tests/seqlock_test.cpp
    ${SOURCES}
)

foreach(target
    virtual_memory_manager_test
    slab_allocator_test
    ring_buffer_test
    allocator_benchmark
    market_data_receiver
    strategy_engine
    seqlock_test
)
    target_link_libraries(${target} PRIVATE Threads::Threads rt)
endforeach()