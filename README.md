# multi_index_lru

A standalone, header-only C++20 LRU (Least Recently Used) cache container based on [Boost.MultiIndex](https://www.boost.org/doc/libs/release/libs/multi_index/doc/index.html).

This library provides an LRU cache that supports multiple indices for efficient lookup by different keys, while automatically managing cache eviction based on access patterns.

## Features

- **Header-only**: Just include and use
- **Multiple indices**: Look up items by different keys (ID, name, email, etc.)
- **LRU eviction**: Automatically evicts least recently used items when capacity is exceeded
- **Access tracking**: `find()` operations automatically refresh the item's position in the LRU order
- **C++20**: Modern C++ with concepts, `[[nodiscard]]`, etc.
- **Based on Boost.MultiIndex**: Leverages the battle-tested Boost library

## Requirements

- C++20 compatible compiler (GCC 10+, Clang 12+, MSVC 2019+)
- Boost 1.74+ (only headers needed, no linking required)
- CMake 3.20+ (for building tests/examples)

## Installation

### CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
    multi_index_lru
    GIT_REPOSITORY https://github.com/yourusername/multi_index_lru.git
    GIT_TAG main
)
FetchContent_MakeAvailable(multi_index_lru)

target_link_libraries(your_target PRIVATE multi_index_lru::multi_index_lru)
```

### Manual

Just copy the `include/multi_index_lru` directory to your project and ensure Boost headers are available.

## Quick Start

```cpp
#include <multi_index_lru/container.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <string>

// Define your data structure
struct CacheEntry {
    std::string key;
    int value;
};

// Define a tag for index access
struct KeyTag {};

// Define the cache type
using MyCache = multi_index_lru::Container<
    CacheEntry,
    boost::multi_index::indexed_by<
        boost::multi_index::hashed_unique<
            boost::multi_index::tag<KeyTag>,
            boost::multi_index::member<CacheEntry, std::string, &CacheEntry::key>>>>;

int main() {
    MyCache cache(1000);  // Capacity of 1000 items
    
    // Insert items
    cache.emplace(CacheEntry{"key1", 42});
    cache.insert(CacheEntry{"key2", 100});
    
    // Find by key (also refreshes LRU position)
    auto it = cache.find<KeyTag>(std::string("key1"));
    if (it != cache.end<KeyTag>()) {
        std::cout << "Found: " << it->value << "\n";
    }
    
    // Check existence
    if (cache.contains<KeyTag>(std::string("key2"))) {
        // ...
    }
    
    // Erase
    cache.erase<KeyTag>(std::string("key1"));
}
```

## Multiple Indices Example

```cpp
struct User {
    int id;
    std::string email;
    std::string name;
};

struct IdTag {};
struct EmailTag {};
struct NameTag {};

using UserCache = multi_index_lru::Container<
    User,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<IdTag>,
            boost::multi_index::member<User, int, &User::id>>,
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<EmailTag>,
            boost::multi_index::member<User, std::string, &User::email>>,
        boost::multi_index::ordered_non_unique<
            boost::multi_index::tag<NameTag>,
            boost::multi_index::member<User, std::string, &User::name>>>>;

UserCache cache(1000);
cache.emplace(User{1, "alice@example.com", "Alice"});

// Find by any index
auto by_id = cache.find<IdTag>(1);
auto by_email = cache.find<EmailTag>(std::string("alice@example.com"));
auto by_name = cache.find<NameTag>(std::string("Alice"));
```

## API Reference

### Container

```cpp
template <typename Value, typename IndexSpecifierList, typename Allocator = std::allocator<Value>>
class Container;
```

#### Constructor

- `explicit Container(size_type max_size)` - Create container with given capacity

#### Insertion

- `template<typename... Args> bool emplace(Args&&... args)` - Emplace element, returns true if newly inserted
- `bool insert(const Value& value)` - Insert copy
- `bool insert(Value&& value)` - Insert with move

#### Lookup

- `template<typename Tag> auto find(const auto& key)` - Find by key, refreshes LRU position
- `template<typename Tag> bool contains(const auto& key)` - Check existence, refreshes LRU

#### Removal

- `template<typename Tag> bool erase(const auto& key)` - Erase by key
- `void clear()` - Remove all elements

#### Capacity

- `size_type size() const` - Current element count
- `bool empty() const` - Check if empty
- `size_type capacity() const` - Maximum capacity
- `void set_capacity(size_type new_capacity)` - Change capacity (evicts if needed)

#### Iteration

- `auto begin() / end()` - Iterate in LRU order (most recent first)
- `template<typename Tag> auto end()` - End iterator for specific index

#### Access to underlying container

- `auto& get_container()` - Access the underlying `boost::multi_index_container`

## Building

```bash
mkdir build && cd build
cmake ..
cmake --build .
ctest  # Run tests
```

### CMake Options

- `MULTI_INDEX_LRU_BUILD_TESTS` - Build tests (default: ON)
- `MULTI_INDEX_LRU_BUILD_EXAMPLES` - Build examples (default: ON)

## How It Works

The container wraps a `boost::multi_index_container` and automatically prepends a `sequenced` index to track access order. When elements are accessed via `find()` or `contains()`, they are moved to the front of the sequence. When the container exceeds capacity during insertion, the element at the back (least recently used) is evicted.

## Credits

Based on the multi-index-lru library from [userver](https://github.com/userver-framework/userver), adapted to be a standalone header-only library without userver dependencies.

## License

Apache License 2.0 - see [LICENSE](LICENSE) for details.

This project is derived from the userver framework, which is also licensed under Apache 2.0.
See [NOTICE](NOTICE) for attribution.
