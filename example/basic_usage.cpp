/// Basic usage example for multi_index_lru::Container

#include <multi_index_lru/container.hpp>

#include <iostream>
#include <string>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>

// Define a simple key-value structure
struct CacheEntry {
    std::string key;
    int value;
};

// Tag for accessing by key
struct KeyTag {};

// Define the cache type
using SimpleCache = multi_index_lru::Container<
    CacheEntry,
    boost::multi_index::indexed_by<
        boost::multi_index::hashed_unique<
            boost::multi_index::tag<KeyTag>,
            boost::multi_index::member<CacheEntry, std::string, &CacheEntry::key>>>>;

int main() {
    // Create a cache with capacity of 3 items
    SimpleCache cache(3);

    // Insert some entries
    cache.emplace(CacheEntry{"apple", 1});
    cache.emplace(CacheEntry{"banana", 2});
    cache.emplace(CacheEntry{"cherry", 3});

    std::cout << "Cache size: " << cache.size() << "\n";

    // Find by key
    auto it = cache.find<KeyTag>(std::string("banana"));
    if (it != cache.end<KeyTag>()) {
        std::cout << "Found: " << it->key << " = " << it->value << "\n";
    }

    // Insert a fourth item - "apple" was least recently used, so it gets evicted
    // (we accessed "banana" most recently via find)
    cache.emplace(CacheEntry{"date", 4});

    std::cout << "After adding 'date':\n";
    std::cout << "  contains 'apple': " << cache.contains<KeyTag>(std::string("apple")) << "\n";
    std::cout << "  contains 'banana': " << cache.contains<KeyTag>(std::string("banana")) << "\n";
    std::cout << "  contains 'cherry': " << cache.contains<KeyTag>(std::string("cherry")) << "\n";
    std::cout << "  contains 'date': " << cache.contains<KeyTag>(std::string("date")) << "\n";

    // Iterate in LRU order (most recent first)
    std::cout << "Items in LRU order (most recent first):\n";
    for (const auto& entry : cache.get_container().get<0>()) {
        std::cout << "  " << entry.key << " = " << entry.value << "\n";
    }

    return 0;
}
