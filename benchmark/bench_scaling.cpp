// bench_scaling.cpp -- Fixed total ops, vary thread count, measure throughput.
//
// Each thread does (kTotalOps / thread_count) operations so the total
// work is constant regardless of parallelism.  This makes it easy to
// observe how throughput scales with thread count.

#include "bench_common.h"
#include <benchmark/benchmark.h>

static const int64_t kTotalOps     = 1000000;
static const int     kPrefillCount = 50000;
static const int     kKeyRange     = 200000;

// ---------------------------------------------------------------------------
// ConcurrentHashMap
// ---------------------------------------------------------------------------
static void BM_ConcurrentHashMap(benchmark::State& state) {
    auto& map = ConcurrentHolder::get();

    if (state.thread_index() == 0) {
        ConcurrentHolder::reset();
        ConcurrentHolder::prefill(kPrefillCount);
    }

    const int64_t ops_per_thread = kTotalOps / state.threads();
    FastRng rng(31 + state.thread_index());

    for (auto _ : state) {
        for (int64_t i = 0; i < ops_per_thread; ++i) {
            int key = static_cast<int>(rng.next_in_range(kKeyRange));
            uint32_t r = rng.next_in_range(100);
            if (r < 70) {
                auto result = map.find(key);
                benchmark::DoNotOptimize(result);
            } else if (r < 85) {
                benchmark::DoNotOptimize(map.insert(key, key));
            } else {
                benchmark::DoNotOptimize(map.erase(key));
            }
        }
    }

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * ops_per_thread);

    if (state.thread_index() == 0) {
        ConcurrentHolder::reset();
    }
}

BENCHMARK(BM_ConcurrentHashMap)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(16)
    ->UseRealTime();

// ---------------------------------------------------------------------------
// StdMutexMap (baseline)
// ---------------------------------------------------------------------------
static void BM_StdMutexMap(benchmark::State& state) {
    auto& map = BaselineHolder::get();

    if (state.thread_index() == 0) {
        BaselineHolder::reset();
        BaselineHolder::prefill(kPrefillCount);
    }

    const int64_t ops_per_thread = kTotalOps / state.threads();
    FastRng rng(31 + state.thread_index());

    for (auto _ : state) {
        for (int64_t i = 0; i < ops_per_thread; ++i) {
            int key = static_cast<int>(rng.next_in_range(kKeyRange));
            uint32_t r = rng.next_in_range(100);
            if (r < 70) {
                auto result = map.find(key);
                benchmark::DoNotOptimize(result);
            } else if (r < 85) {
                benchmark::DoNotOptimize(map.insert(key, key));
            } else {
                benchmark::DoNotOptimize(map.erase(key));
            }
        }
    }

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * ops_per_thread);

    if (state.thread_index() == 0) {
        BaselineHolder::reset();
    }
}

BENCHMARK(BM_StdMutexMap)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(16)
    ->UseRealTime();

BENCHMARK_MAIN();
