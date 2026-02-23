/// Example: Using multi_index_lru with zerialize data and composite keys
///
/// This example demonstrates:
/// 1. Storing serialized binary data (MsgPack/CBOR/etc) in LRU cache
/// 2. Indexing by extracted fields (including composite keys)
/// 3. Looking up and deserializing on demand

#include <multi_index_lru/container.hpp>
#include <multi_index_lru/zerialize_cache.hpp>

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <iostream>
#include <string>

// Mock zerialize deserializer for demonstration
// In real code, use zerialize::MsgPack::Deserializer, zerialize::CBOR::Deserializer, etc.
namespace mock_zerialize {

// Simple POD struct for binary serialization demo
struct SerializedData {
    int64_t tenant_id;
    int64_t user_id;
    char email[64];
    char name[64];
};

class Deserializer {
public:
    explicit Deserializer(std::span<const uint8_t> data) {
        // In real impl, this would parse MsgPack/CBOR/etc.
        // For demo, we'll use a simple binary format
        if (data.size() >= sizeof(SerializedData)) {
            data_ = *reinterpret_cast<const SerializedData*>(data.data());
        }
    }

    Deserializer operator[](const std::string& key) const {
        // Return a sub-deserializer focused on that field
        // For simplicity, we return self and check key in asXxx()
        Deserializer sub = *this;
        sub.current_key_ = key;
        return sub;
    }

    bool isMap() const { return current_key_.empty(); }

    int64_t asInt64() const {
        if (current_key_ == "tenant_id") return data_.tenant_id;
        if (current_key_ == "user_id") return data_.user_id;
        return 0;
    }

    std::string asString() const {
        if (current_key_ == "email") return std::string(data_.email);
        if (current_key_ == "name") return std::string(data_.name);
        return "";
    }

private:
    SerializedData data_{};
    std::string current_key_;
};

// Helper to serialize demo data
inline std::vector<uint8_t> serialize(int64_t tenant_id, int64_t user_id,
                                       const std::string& email, const std::string& name) {
    SerializedData data{};
    data.tenant_id = tenant_id;
    data.user_id = user_id;
    std::strncpy(data.email, email.c_str(), sizeof(data.email) - 1);
    std::strncpy(data.name, name.c_str(), sizeof(data.name) - 1);

    // Simple binary serialization for demo
    std::vector<uint8_t> result(sizeof(data));
    std::memcpy(result.data(), &data, sizeof(data));
    return result;
}

}  // namespace mock_zerialize

// For real zerialize, you would use:
// #include <zerialize/zerialize.hpp>
// using Deserializer = zerialize::MsgPack::Deserializer;

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
            // Composite unique index: (tenant_id, user_id)
            boost::multi_index::ordered_unique<
                boost::multi_index::tag<TenantUserTag>,
                boost::multi_index::composite_key<
                    MyEntry,
                    key<0, MyEntry>,  // tenant_id
                    key<1, MyEntry>   // user_id
                >
            >,
            // Single index: email
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
    // Step 5: Use the cache!
    // =========================================================================
    MyCache cache(1000);  // Capacity: 1000 entries

    // Simulated incoming binary data (would be MsgPack/CBOR in real use)
    // In real code: std::span<const uint8_t> incoming_data = nats_message.payload();

    // Insert entries - the builder extracts keys from serialized data
    {
        // Simulate receiving serialized user data
        auto data1 = mock_zerialize::serialize(1, 100, "alice@example.com", "Alice");
        auto data2 = mock_zerialize::serialize(1, 101, "bob@example.com", "Bob");
        auto data3 = mock_zerialize::serialize(2, 100, "charlie@other.com", "Charlie");

        // Build entries and insert
        cache.emplace(builder.build<mock_zerialize::Deserializer>(data1));
        cache.emplace(builder.build<mock_zerialize::Deserializer>(data2));
        cache.emplace(builder.build<mock_zerialize::Deserializer>(data3));
    }

    std::cout << "Cache size: " << cache.size() << "\n\n";

    // =========================================================================
    // Lookup by composite key (tenant_id, user_id)
    // =========================================================================
    {
        auto it = cache.find<TenantUserTag>(std::make_tuple(1, 100));
        if (it != cache.end<TenantUserTag>()) {
            std::cout << "Found by (tenant=1, user=100):\n";
            std::cout << "  Email key: " << std::get<2>(it->keys) << "\n";

            // Deserialize to access full data
            auto reader = it->deserialize<mock_zerialize::Deserializer>();
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
