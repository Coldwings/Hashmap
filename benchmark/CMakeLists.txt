include(FetchContent)
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.8.3
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

function(chm_add_bench name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE concurrent_hashmap benchmark::benchmark)
endfunction()

chm_add_bench(bench_read_heavy bench_read_heavy.cpp)
chm_add_bench(bench_write_heavy bench_write_heavy.cpp)
chm_add_bench(bench_mixed bench_mixed.cpp)
chm_add_bench(bench_contention bench_contention.cpp)
chm_add_bench(bench_scaling bench_scaling.cpp)
