// bench_contention.cpp -- 100 hot keys, all threads contend on them.

#include "bench_common.h"
#include <benchmark/benchmark.h>

static const int kHotKeys = 100;

// ---------------------------------------------------------------------------
// ConcurrentHashMap
// ---------------------------------------------------------------------------
static void BM_ConcurrentHashMap(benchmark::State& state) {
    auto& map = ConcurrentHolder::get();

    if (state.thread_index() == 0) {
        ConcurrentHolder::reset();
        for (int i = 0; i < kHotKeys; ++i) {
            map.insert(i, i);
        }
    }

    FastRng rng(99 + state.thread_index());
    int64_t ops = 0;

    for (auto _ : state) {
        int key = static_cast<int>(rng.next_in_range(kHotKeys));
        uint32_t r = rng.next_in_range(100);
        if (r < 40) {
            auto result = map.find(key);
            benchmark::DoNotOptimize(result);
        } else if (r < 60) {
            benchmark::DoNotOptimize(map.insert_or_assign(key, key + 1));
        } else if (r < 80) {
            auto val = map.get_or_set(key, key);
            benchmark::DoNotOptimize(val);
        } else {
            benchmark::DoNotOptimize(map.contains(key));
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
        for (int i = 0; i < kHotKeys; ++i) {
            map.insert(i, i);
        }
    }

    FastRng rng(99 + state.thread_index());
    int64_t ops = 0;

    for (auto _ : state) {
        int key = static_cast<int>(rng.next_in_range(kHotKeys));
        uint32_t r = rng.next_in_range(100);
        if (r < 40) {
            auto result = map.find(key);
            benchmark::DoNotOptimize(result);
        } else if (r < 60) {
            benchmark::DoNotOptimize(map.insert_or_assign(key, key + 1));
        } else if (r < 80) {
            auto val = map.get_or_set(key, key);
            benchmark::DoNotOptimize(val);
        } else {
            benchmark::DoNotOptimize(map.contains(key));
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
