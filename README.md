# multi_index_lru

A standalone, header-only C++20 LRU (Least Recently Used) cache container based on [Boost.MultiIndex](https://www.boost.org/doc/libs/release/libs/multi_index/doc/index.html).

This library provides an LRU cache that supports multiple indices for efficient lookup by different keys, while automatically managing cache eviction based on access patterns.

## Features

- **Header-only**: Just include and use
- **Multiple indices**: Look up items by different keys (ID, name, email, etc.)
- **Composite keys**: Index by combinations of fields (e.g., tenant_id + user_id)
- **LRU eviction**: Automatically evicts least recently used items when capacity is exceeded
- **Access tracking**: `find()` operations automatically refresh the item's position in the LRU order
- **TTL expiration**: Items automatically expire after a configurable time-to-live
- **Zerialize support**: Cache serialized binary data (MsgPack, CBOR, JSON, Flex, ZERA) with extracted indices
- **C++20**: Modern C++ with concepts, `[[nodiscard]]`, etc.
- **Based on Boost.MultiIndex**: Leverages the battle-tested Boost library

## Requirements

- C++20 compatible compiler (GCC 10+, Clang 12+, MSVC 2019+)
- Boost 1.74+ (only headers needed, no linking required)
- CMake 3.20+ (for building tests/examples)
- [zerialize](https://github.com/colinator/zerialize) (optional, for binary format caching)

## Installation

### CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
    multi_index_lru
    GIT_REPOSITORY https://github.com/mrayva/multi_index_lru.git
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

---

## ExpirableContainer (TTL-based expiration)

`ExpirableContainer` extends `Container` with time-to-live (TTL) semantics. Items automatically expire after a configurable duration. Accessing items via `find()` refreshes their expiration timer.

### Basic Usage

```cpp
#include <multi_index_lru/expirable_container.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>

using namespace std::chrono_literals;

struct Session {
    std::string session_id;
    int user_id;
    std::string username;
};

struct SessionIdTag {};
struct UserIdTag {};

// Key extractors that work through TimestampedValue wrapper
struct SessionIdExtractor {
    using result_type = std::string;
    template <typename T>
    result_type operator()(const T& wrapped) const { 
        return wrapped.value.session_id; 
    }
};

struct UserIdExtractor {
    using result_type = int;
    template <typename T>
    result_type operator()(const T& wrapped) const { 
        return wrapped.value.user_id; 
    }
};

using SessionCache = multi_index_lru::ExpirableContainer<
    Session,
    boost::multi_index::indexed_by<
        boost::multi_index::hashed_unique<
            boost::multi_index::tag<SessionIdTag>,
            SessionIdExtractor>,
        boost::multi_index::ordered_non_unique<
            boost::multi_index::tag<UserIdTag>,
            UserIdExtractor>>>;

int main() {
    // Capacity 1000, 30-minute session timeout
    SessionCache cache(1000, 30min);
    
    cache.insert(Session{"sess-001", 1, "alice"});
    cache.insert(Session{"sess-002", 1, "alice"});  // Alice has 2 sessions
    cache.insert(Session{"sess-003", 2, "bob"});
    
    // Find refreshes TTL (keeps session alive)
    auto it = cache.find<SessionIdTag>(std::string("sess-001"));
    if (it != cache.end<SessionIdTag>()) {
        std::cout << "Session for: " << it->username << "\n";
    }
    
    // find_no_update does NOT refresh TTL
    auto it2 = cache.find_no_update<SessionIdTag>(std::string("sess-002"));
    
    // Find all sessions for a user
    auto [begin, end] = cache.equal_range<UserIdTag>(1);
    for (auto iter = begin; iter != end; ++iter) {
        std::cout << "Session: " << iter->session_id << "\n";
    }
    
    // Periodic cleanup of expired items
    cache.cleanup_expired();
    
    // Change TTL for future accesses
    cache.set_ttl(1h);
}
```

### Key Differences from Container

| Feature | `Container` | `ExpirableContainer` |
|---------|-------------|---------------------|
| Eviction | LRU only | LRU + TTL |
| `find()` | Updates LRU position | Updates LRU + refreshes TTL |
| `find_no_update()` | Doesn't update LRU | Doesn't update LRU or TTL |
| `cleanup_expired()` | N/A | Removes expired items |
| Key extractors | Use `member<>` directly | Must drill through `TimestampedValue` |

### Key Extractors for ExpirableContainer

Since `ExpirableContainer` wraps values in `TimestampedValue<Value>`, key extractors must access `wrapped.value`:

```cpp
// Custom extractor for regular struct
struct IdExtractor {
    using result_type = int;
    template <typename T>
    result_type operator()(const T& wrapped) const { 
        return wrapped.value.id; 
    }
};

// For zerialize entries, use timestamped_key<N, Entry>
using Entry = multi_index_lru::ZerializeEntry<std::tuple<int64_t, std::string>>;

using ExpirableZerializeCache = multi_index_lru::ExpirableContainer<
    Entry,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<IdTag>,
            multi_index_lru::timestamped_key<0, Entry>>,  // Use timestamped_key
        boost::multi_index::ordered_non_unique<
            boost::multi_index::tag<NameTag>,
            multi_index_lru::timestamped_key<1, Entry>>>>;
```

### API Reference - ExpirableContainer

```cpp
template <typename Value, typename IndexSpecifierList, typename Allocator = std::allocator<Value>>
class ExpirableContainer;
```

#### Constructor

- `ExpirableContainer(size_type max_size, duration_type ttl)` - Create with capacity and TTL

#### TTL-specific Methods

- `void cleanup_expired()` - Remove all expired items (call periodically)
- `duration_type ttl() const` - Get current TTL
- `void set_ttl(duration_type new_ttl)` - Change TTL for future accesses

#### Lookup Methods

- `template<typename Tag> auto find(const auto& key)` - Find, check TTL, refresh if not expired
- `template<typename Tag> auto find_no_update(const auto& key)` - Find without checking/refreshing TTL
- `template<typename Tag> auto equal_range(const auto& key)` - Range query, removes expired, refreshes others
- `template<typename Tag> auto equal_range_no_update(const auto& key)` - Range query without updates

---

## Zerialize Integration

The library provides adapters for caching serialized binary data from [zerialize](https://github.com/colinator/zerialize), supporting all 5 formats:

| Format | Deserializer | Zero-Copy | Best For |
|--------|--------------|-----------|----------|
| **JSON** | `zerialize::JSON::Deserializer` | No (parsed) | Human-readable, debugging |
| **MsgPack** | `zerialize::MsgPack::Deserializer` | Yes | Compact binary, wide support |
| **CBOR** | `zerialize::CBOR::Deserializer` | Yes | IoT, constrained environments |
| **Flex** | `zerialize::Flex::Deserializer` | Yes | Schema-less with fast access |
| **ZERA** | `zerialize::ZERA::Deserializer` | Yes | High-performance, tensors |

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  ZerializeEntry<tuple<int64_t, int64_t, string>>           │
│  ├── keys: (tenant_id, user_id, email)  ← extracted once   │
│  └── data: vector<uint8_t>              ← original bytes   │
└─────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│  multi_index_lru::Container with indices on extracted keys │
│  ├── composite_key<key<0>, key<1>>  → TenantUserTag        │
│  └── key<2>                         → EmailTag             │
└─────────────────────────────────────────────────────────────┘
```

### Zerialize Cache Example

```cpp
#include <multi_index_lru/container.hpp>
#include <multi_index_lru/zerialize_cache.hpp>
#include <zerialize/zerialize.hpp>  // Your zerialize installation

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>

using namespace multi_index_lru;

// Step 1: Define entry with key types
// Keys will be extracted from serialized data at insert time
using MyEntry = EntryWithKeys_t<int64_t, int64_t, std::string>;

// Step 2: Define index tags
struct TenantUserTag {};  // Composite key
struct EmailTag {};       // Single key

// Step 3: Define cache with indices
using MyCache = Container<
    MyEntry,
    boost::multi_index::indexed_by<
        // Composite unique index: (tenant_id, user_id)
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<TenantUserTag>,
            boost::multi_index::composite_key<
                MyEntry,
                key<0, MyEntry>,  // tenant_id (first key)
                key<1, MyEntry>   // user_id (second key)
            >
        >,
        // Single hashed index: email
        boost::multi_index::hashed_unique<
            boost::multi_index::tag<EmailTag>,
            key<2, MyEntry>  // email (third key)
        >
    >
>;

// Step 4: Create builder with field extractors
auto builder = make_entry_builder<MyEntry>(
    int64_field("tenant_id"),   // Extract "tenant_id" as int64 → key<0>
    int64_field("user_id"),     // Extract "user_id" as int64 → key<1>
    string_field("email")       // Extract "email" as string → key<2>
);

int main() {
    MyCache cache(10000);  // Capacity: 10,000 entries
    
    // Insert from serialized binary data
    // In real code: std::span<const uint8_t> payload = nats_msg.data();
    std::span<const uint8_t> payload = get_msgpack_data();
    cache.emplace(builder.build<zerialize::MsgPack::Deserializer>(payload));
    
    // Lookup by composite key (tenant_id=1, user_id=100)
    auto it = cache.find<TenantUserTag>(std::make_tuple(1LL, 100LL));
    if (it != cache.end<TenantUserTag>()) {
        // Access extracted keys directly
        std::cout << "Email: " << std::get<2>(it->keys) << "\n";
        
        // Deserialize full data on demand
        auto reader = it->deserialize<zerialize::MsgPack::Deserializer>();
        std::cout << "Name: " << reader["name"].asString() << "\n";
    }
    
    // Lookup by email
    auto it2 = cache.find<EmailTag>(std::string("alice@example.com"));
    if (it2 != cache.end<EmailTag>()) {
        std::cout << "Found user in tenant " << std::get<0>(it2->keys) << "\n";
    }
}
```

### Field Extractors

The library provides convenient field extractors for common types:

```cpp
// Type-specific extractors
int64_field("field_name")   // Extract as int64_t
int32_field("field_name")   // Extract as int32_t
uint64_field("field_name")  // Extract as uint64_t
string_field("field_name")  // Extract as std::string
double_field("field_name")  // Extract as double
bool_field("field_name")    // Extract as bool

// Generic extractor
field<T>("field_name")      // Extract as type T

// Nested field extractor (for reader["a"]["b"]["c"])
nested_field<T>({"a", "b", "c"})
```

### Composite Keys

For multi-field unique constraints:

```cpp
// Composite key on (region, account_id, resource_id)
using Entry = EntryWithKeys_t<std::string, int64_t, int64_t, std::string>;

struct ResourceTag {};

using Cache = Container<
    Entry,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<ResourceTag>,
            boost::multi_index::composite_key<
                Entry,
                key<0, Entry>,  // region
                key<1, Entry>,  // account_id
                key<2, Entry>   // resource_id
            >
        >
    >
>;

// Lookup
cache.find<ResourceTag>(std::make_tuple("us-east-1", 12345LL, 67890LL));
```

### Format-Agnostic Caching

Store data in one format, deserialize in the same format:

```cpp
// Template function works with any zerialize format
template <typename Format>
void process_message(MyCache& cache, std::span<const uint8_t> data) {
    using Deserializer = typename Format::Deserializer;
    
    // Build entry - extracts keys using Format's deserializer
    auto entry = builder.build<Deserializer>(data);
    cache.emplace(std::move(entry));
}

// Usage with different formats
process_message<zerialize::MsgPack>(cache, msgpack_data);
process_message<zerialize::CBOR>(cache, cbor_data);
process_message<zerialize::JSON>(cache, json_data);
process_message<zerialize::Flex>(cache, flex_data);
process_message<zerialize::ZERA>(cache, zera_data);
```

---

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

### ZerializeEntry

```cpp
template <typename Keys>
struct ZerializeEntry;

// Convenience alias
template <typename... KeyTypes>
using EntryWithKeys_t = ZerializeEntry<std::tuple<KeyTypes...>>;
```

#### Members

- `keys` - Tuple of extracted keys
- `data` - `std::vector<uint8_t>` containing original serialized bytes
- `deserialize<Deserializer>()` - Deserialize stored data
- `raw_data()` - Get `std::span<const uint8_t>` of stored bytes

### Key Extractor

```cpp
template <std::size_t N, typename Entry>
struct key;
```

Use with `boost::multi_index::composite_key` or as a direct key extractor.

### EntryBuilder

```cpp
template <typename Entry, typename... Extractors>
class EntryBuilder;

// Create with make_entry_builder
auto builder = make_entry_builder<Entry>(extractor1, extractor2, ...);

// Build entry from data
Entry entry = builder.build<Deserializer>(data_span);
```

---

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
- `MULTI_INDEX_LRU_USE_BOOST_DEVELOP` - Use Boost.MultiIndex develop branch (default: OFF)

### Using Boost.MultiIndex Develop Branch

The develop branch of Boost.MultiIndex contains a [major refactoring](https://bannalia.blogspot.com/2025/12/boostmultiindex-refactored.html) for Boost 1.91 that:

- Replaces Boost.MPL with Boost.Mp11 and C++11 variadic templates
- Significantly reduces type name lengths (better error messages, smaller binaries)
- Makes `composite_key` truly variadic (no more `BOOST_MULTI_INDEX_LIMIT_COMPOSITE_KEY_SIZE`)

To use the develop branch:

```bash
mkdir build && cd build
cmake .. -DMULTI_INDEX_LRU_USE_BOOST_DEVELOP=ON
cmake --build .
```

This will automatically fetch the required Boost modules from GitHub. Note that installation (`cmake --install`) is not available when using this option.

## How It Works

The container wraps a `boost::multi_index_container` and automatically prepends a `sequenced` index to track access order. When elements are accessed via `find()` or `contains()`, they are moved to the front of the sequence. When the container exceeds capacity during insertion, the element at the back (least recently used) is evicted.

For zerialize data, the `ZerializeEntry` stores both the original serialized bytes and extracted keys. Keys are extracted once at insertion time, enabling O(1) or O(log n) lookups without repeated deserialization. Full deserialization only happens on demand via `deserialize<Format>()`.

## Credits

Based on the multi-index-lru library from [userver](https://github.com/userver-framework/userver), adapted to be a standalone header-only library without userver dependencies.

## License

Apache License 2.0 - see [LICENSE](LICENSE) for details.

This project is derived from the userver framework, which is also licensed under Apache 2.0.
See [NOTICE](NOTICE) for attribution.
