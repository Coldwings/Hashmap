// bench_read_heavy.cpp -- 95% find + 5% insert on a pre-filled map.

#include "bench_common.h"
#include <benchmark/benchmark.h>

static const int kPrefillCount = 100000;
static const int kKeyRange     = 200000;

// ---------------------------------------------------------------------------
// ConcurrentHashMap
// ---------------------------------------------------------------------------
static void BM_ConcurrentHashMap(benchmark::State& state) {
    auto& map = ConcurrentHolder::get();

    if (state.thread_index() == 0) {
        ConcurrentHolder::reset();
        ConcurrentHolder::prefill(kPrefillCount);
    }

    FastRng rng(42 + state.thread_index());
    int64_t ops = 0;

    for (auto _ : state) {
        int key = static_cast<int>(rng.next_in_range(kKeyRange));
        if (rng.next_in_range(100) < 95) {
            auto result = map.find(key);
            benchmark::DoNotOptimize(result);
        } else {
            map.insert(key, key);
        }
        ++ops;
    }

    state.SetItemsProcessed(ops);

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

    FastRng rng(42 + state.thread_index());
    int64_t ops = 0;

    for (auto _ : state) {
        int key = static_cast<int>(rng.next_in_range(kKeyRange));
        if (rng.next_in_range(100) < 95) {
            auto result = map.find(key);
            benchmark::DoNotOptimize(result);
        } else {
            map.insert(key, key);
        }
        ++ops;
    }

    state.SetItemsProcessed(ops);

    if (state.thread_index() == 0) {
        BaselineHolder::reset();
    }
}

BENCHMARK(BM_StdMutexMap)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(16)
    ->UseRealTime();

BENCHMARK_MAIN();
