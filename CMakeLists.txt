cmake_minimum_required(VERSION 3.14)
project(ConcurrentHashMap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Header-only library
add_library(concurrent_hashmap INTERFACE)
target_include_directories(concurrent_hashmap INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Options
option(CHM_BUILD_TESTS "Build tests" ON)
option(CHM_BUILD_BENCHMARKS "Build benchmarks" ON)
option(CHM_SANITIZER "Sanitizer type: none, asan, tsan" "none")

# Sanitizer flags
if(CHM_SANITIZER STREQUAL "asan")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer -g")
    set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=address,undefined")
elseif(CHM_SANITIZER STREQUAL "tsan")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread -fno-omit-frame-pointer -g")
    set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=thread")
endif()

if(CHM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CHM_BUILD_BENCHMARKS AND NOT CHM_SANITIZER STREQUAL "asan" AND NOT CHM_SANITIZER STREQUAL "tsan")
    add_subdirectory(benchmark)
endif()
