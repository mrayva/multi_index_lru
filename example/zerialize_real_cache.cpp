/// Example: multi_index_lru with the REAL zerialize library.
///
/// This is the real-dependency counterpart to zerialize_cache.cpp, which
/// deliberately uses a hand-written mock so it builds with zero external
/// dependencies. Build this one with -DMULTI_INDEX_LRU_BUILD_ZERIALIZE_TESTS=ON
/// (see the top-level CMakeLists.txt), which FetchContents
/// github.com/mrayva/zerialize.
///
/// Same scenario as zerialize_cache.cpp -- composite-keyed cache entries,
/// extracted at insert time, deserialized on demand -- but every step below
/// is real zerialize::MsgPack encoding/decoding, not a raw struct memcpy.

#include <multi_index_lru/container.hpp>
#include <multi_index_lru/zerialize_cache.hpp>

#include <zerialize/zerialize.hpp>
#include <zerialize/protocols/msgpack.hpp>

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <iostream>
#include <string>

namespace z = zerialize;

int main() {
    using namespace multi_index_lru;

    // =========================================================================
    // Step 1: Define the entry type with key types
    // Keys: (tenant_id: int64, user_id: int64, email: string)
    // =========================================================================
    using MyEntry = EntryWithKeys_t<int64_t, int64_t, std::string>;

    // =========================================================================
    // Step 2: Define index tags
    // =========================================================================
    struct TenantUserTag {};  // Composite key: (tenant_id, user_id)
    struct EmailTag {};       // Single key: email

    // =========================================================================
    // Step 3: Define the cache with indices using boost::multi_index
    // =========================================================================
    using MyCache = Container<
        MyEntry,
        boost::multi_index::indexed_by<
            boost::multi_index::ordered_unique<
                boost::multi_index::tag<TenantUserTag>,
                boost::multi_index::composite_key<
                    MyEntry,
                    key<0, MyEntry>,  // tenant_id
                    key<1, MyEntry>   // user_id
                >
            >,
            boost::multi_index::hashed_unique<
                boost::multi_index::tag<EmailTag>,
                key<2, MyEntry>  // email
            >
        >
    >;

    // =========================================================================
    // Step 4: Create entry builder with field extractors
    // =========================================================================
    auto builder = make_entry_builder<MyEntry>(
        int64_field("tenant_id"),
        int64_field("user_id"),
        string_field("email")
    );

    // =========================================================================
    // Step 5: Use the cache with real zerialize::MsgPack-encoded payloads
    // =========================================================================
    MyCache cache(1000);  // Capacity: 1000 entries

    auto encode = [](int64_t tenant_id, int64_t user_id,
                      const std::string& email, const std::string& name) {
        // In real code: this is what you'd receive as, say, a NATS message
        // payload, already zerialize::MsgPack-encoded by an upstream
        // producer.
        auto buf = z::serialize<z::MsgPack>(
            z::zmap<"tenant_id", "user_id", "email", "name">(tenant_id, user_id, email, name));
        auto span = buf.buf();
        return std::vector<uint8_t>(span.begin(), span.end());
    };

    {
        auto data1 = encode(1, 100, "alice@example.com", "Alice");
        auto data2 = encode(1, 101, "bob@example.com", "Bob");
        auto data3 = encode(2, 100, "charlie@other.com", "Charlie");

        // Build entries and insert -- the builder extracts keys by actually
        // parsing the MsgPack bytes, not reinterpreting a raw struct.
        cache.emplace(builder.build<z::MsgPack::Deserializer>(data1));
        cache.emplace(builder.build<z::MsgPack::Deserializer>(data2));
        cache.emplace(builder.build<z::MsgPack::Deserializer>(data3));
    }

    std::cout << "Cache size: " << cache.size() << "\n\n";

    // =========================================================================
    // Lookup by composite key (tenant_id, user_id)
    // =========================================================================
    {
        auto it = cache.find<TenantUserTag>(std::make_tuple(int64_t(1), int64_t(100)));
        if (it != cache.end<TenantUserTag>()) {
            std::cout << "Found by (tenant=1, user=100):\n";
            std::cout << "  Email key: " << std::get<2>(it->keys) << "\n";

            // Deserialize to access full data (real MsgPack parse).
            auto reader = it->deserialize<z::MsgPack::Deserializer>();
            std::cout << "  Name: " << reader["name"].asString() << "\n\n";
        }
    }

    // =========================================================================
    // Lookup by email
    // =========================================================================
    {
        auto it = cache.find<EmailTag>(std::string("bob@example.com"));
        if (it != cache.end<EmailTag>()) {
            std::cout << "Found by email 'bob@example.com':\n";
            std::cout << "  Tenant ID: " << std::get<0>(it->keys) << "\n";
            std::cout << "  User ID: " << std::get<1>(it->keys) << "\n\n";
        }
    }

    // =========================================================================
    // LRU eviction works as expected
    // =========================================================================
    cache.set_capacity(2);
    std::cout << "After reducing capacity to 2:\n";
    std::cout << "  Size: " << cache.size() << "\n";
    std::cout << "  Contains alice: " << cache.contains<EmailTag>(std::string("alice@example.com")) << "\n";
    std::cout << "  Contains bob: " << cache.contains<EmailTag>(std::string("bob@example.com")) << "\n";

    return 0;
}
