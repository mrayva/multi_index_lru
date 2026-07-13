#include <multi_index_lru/container.hpp>

#include <benchmark/benchmark.h>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

struct KeyTag {};

struct Entry {
    std::uint64_t key;
    std::array<std::byte, 64> payload{};
};

using Cache = multi_index_lru::Container<
    Entry,
    boost::multi_index::indexed_by<boost::multi_index::hashed_unique<
        boost::multi_index::tag<KeyTag>,
        boost::multi_index::member<Entry, std::uint64_t, &Entry::key>>>>;

void Fill(Cache& cache, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        cache.emplace(Entry{static_cast<std::uint64_t>(i)});
    }
}

void InsertAndEvict(benchmark::State& state, std::size_t node_pool_capacity) {
    const auto capacity = static_cast<std::size_t>(state.range(0));
    Cache cache(capacity, node_pool_capacity);
    Fill(cache, capacity);
    std::uint64_t key = capacity;

    for (auto _ : state) {
        benchmark::DoNotOptimize(cache.emplace(Entry{key++}));
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_InsertEvictWithPool(benchmark::State& state) {
    InsertAndEvict(state, static_cast<std::size_t>(state.range(0)));
}

void BM_InsertEvictWithoutPool(benchmark::State& state) {
    InsertAndEvict(state, 0);
}

void BM_FindAndRefresh(benchmark::State& state) {
    const auto capacity = static_cast<std::size_t>(state.range(0));
    Cache cache(capacity);
    Fill(cache, capacity);
    std::uint64_t key = 0;

    for (auto _ : state) {
        benchmark::DoNotOptimize(cache.find<KeyTag>(key));
        key = (key + 1) % capacity;
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_FindWithoutRefresh(benchmark::State& state) {
    const auto capacity = static_cast<std::size_t>(state.range(0));
    Cache cache(capacity);
    Fill(cache, capacity);
    std::uint64_t key = 0;

    for (auto _ : state) {
        benchmark::DoNotOptimize(cache.find_no_update<KeyTag>(key));
        key = (key + 1) % capacity;
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_InsertEvictWithPool)->Arg(1024)->Arg(65536);
BENCHMARK(BM_InsertEvictWithoutPool)->Arg(1024)->Arg(65536);
BENCHMARK(BM_FindAndRefresh)->Arg(1024)->Arg(65536);
BENCHMARK(BM_FindWithoutRefresh)->Arg(1024)->Arg(65536);

}  // namespace
