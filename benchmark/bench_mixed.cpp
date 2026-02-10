// bench_mixed.cpp -- 33% find / 33% insert / 17% erase / 17% get_or_set.

#include "bench_common.h"
#include <benchmark/benchmark.h>

static const int kPrefillCount = 50000;
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

    FastRng rng(7 + state.thread_index());
    int64_t ops = 0;

    for (auto _ : state) {
        int key = static_cast<int>(rng.next_in_range(kKeyRange));
        uint32_t r = rng.next_in_range(100);
        if (r < 33) {
            auto result = map.find(key);
            benchmark::DoNotOptimize(result);
        } else if (r < 66) {
            benchmark::DoNotOptimize(map.insert(key, key));
        } else if (r < 83) {
            benchmark::DoNotOptimize(map.erase(key));
        } else {
            auto val = map.get_or_set(key, key);
            benchmark::DoNotOptimize(val);
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

    FastRng rng(7 + state.thread_index());
    int64_t ops = 0;

    for (auto _ : state) {
        int key = static_cast<int>(rng.next_in_range(kKeyRange));
        uint32_t r = rng.next_in_range(100);
        if (r < 33) {
            auto result = map.find(key);
            benchmark::DoNotOptimize(result);
        } else if (r < 66) {
            benchmark::DoNotOptimize(map.insert(key, key));
        } else if (r < 83) {
            benchmark::DoNotOptimize(map.erase(key));
        } else {
            auto val = map.get_or_set(key, key);
            benchmark::DoNotOptimize(val);
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
