cmake_minimum_required(VERSION 3.20)
project(latency_benchmarking_suite CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# you want these. all of them. no exceptions (pun intended).
set(RELEASE_FLAGS "-O3 -march=native -mtune=native -funroll-loops -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${RELEASE_FLAGS}")

# debug: ASan + UBSan. run this before shipping.
set(CMAKE_CXX_FLAGS_DEBUG
    "${CMAKE_CXX_FLAGS_DEBUG} -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all")

# main binary
add_executable(latency_bench
    src/main.cpp
    src/tick_to_trade.cpp
    src/orderbook.cpp
    src/cancel_replace.cpp
    src/contention.cpp
)

target_include_directories(latency_bench PRIVATE include)

# pthreads required for contention benchmarks
find_package(Threads REQUIRED)
target_link_libraries(latency_bench PRIVATE Threads::Threads)

# sanitized variant
add_executable(latency_bench_san
    src/main.cpp
    src/tick_to_trade.cpp
    src/orderbook.cpp
    src/cancel_replace.cpp
    src/contention.cpp
)
target_include_directories(latency_bench_san PRIVATE include)
target_link_libraries(latency_bench_san PRIVATE Threads::Threads)
target_compile_options(latency_bench_san PRIVATE
    -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all)
target_link_options(latency_bench_san PRIVATE
    -fsanitize=address,undefined)

# print compile flags so it's obvious what you built with
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "CXX flags (Release): ${CMAKE_CXX_FLAGS_RELEASE}")
