/// Tests for zerialize adapter (ZerializeEntry, key extractors, EntryBuilder)

#include <multi_index_lru/container.hpp>
#include <multi_index_lru/zerialize_cache.hpp>

#include <gtest/gtest.h>

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

// =============================================================================
// Mock deserializer that simulates zerialize API
// Supports all 5 zerialize format patterns: JSON, MsgPack, CBOR, Flex, ZERA
// =============================================================================

struct MockData {
    int64_t id;
    int64_t tenant_id;
    int64_t user_id;
    char email[64];
    char name[64];
    double score;
    bool active;
};

inline std::vector<uint8_t> make_mock_data(
    int64_t id, int64_t tenant_id, int64_t user_id,
    const std::string& email, const std::string& name,
    double score = 0.0, bool active = true) {
    
    MockData data{};
    data.id = id;
    data.tenant_id = tenant_id;
    data.user_id = user_id;
    std::strncpy(data.email, email.c_str(), sizeof(data.email) - 1);
    std::strncpy(data.name, name.c_str(), sizeof(data.name) - 1);
    data.score = score;
    data.active = active;
    
    std::vector<uint8_t> result(sizeof(data));
    std::memcpy(result.data(), &data, sizeof(data));
    return result;
}

// Base mock deserializer - simulates zerialize's uniform API
class MockDeserializer {
public:
    explicit MockDeserializer(std::span<const uint8_t> data) {
        if (data.size() >= sizeof(MockData)) {
            data_ = *reinterpret_cast<const MockData*>(data.data());
        }
    }

    MockDeserializer operator[](const std::string& key) const {
        MockDeserializer sub = *this;
        sub.current_key_ = key;
        return sub;
    }

    bool isMap() const { return current_key_.empty(); }
    bool isInt() const { return current_key_ == "id" || current_key_ == "tenant_id" || current_key_ == "user_id"; }
    bool isString() const { return current_key_ == "email" || current_key_ == "name"; }
    bool isFloat() const { return current_key_ == "score"; }
    bool isBool() const { return current_key_ == "active"; }

    int64_t asInt64() const {
        if (current_key_ == "id") return data_.id;
        if (current_key_ == "tenant_id") return data_.tenant_id;
        if (current_key_ == "user_id") return data_.user_id;
        return 0;
    }

    int32_t asInt32() const { return static_cast<int32_t>(asInt64()); }
    uint64_t asUInt64() const { return static_cast<uint64_t>(asInt64()); }
    uint32_t asUInt32() const { return static_cast<uint32_t>(asInt64()); }

    std::string asString() const {
        if (current_key_ == "email") return std::string(data_.email);
        if (current_key_ == "name") return std::string(data_.name);
        return "";
    }

    double asDouble() const {
        if (current_key_ == "score") return data_.score;
        return 0.0;
    }

    bool asBool() const {
        if (current_key_ == "active") return data_.active;
        return false;
    }

private:
    MockData data_{};
    std::string current_key_;
};

// Simulate different zerialize format deserializers
// In real code these would be zerialize::JSON::Deserializer, etc.
namespace mock_zerialize {
    namespace JSON { using Deserializer = MockDeserializer; }
    namespace MsgPack { using Deserializer = MockDeserializer; }
    namespace CBOR { using Deserializer = MockDeserializer; }
    namespace Flex { using Deserializer = MockDeserializer; }
    namespace ZERA { using Deserializer = MockDeserializer; }
}

// =============================================================================
// Test: ZerializeEntry basic functionality
// =============================================================================

TEST(ZerializeEntryTest, BasicConstruction) {
    using namespace multi_index_lru;
    using Entry = EntryWithKeys_t<int64_t, std::string>;
    
    auto keys = std::make_tuple(42LL, std::string("test@example.com"));
    std::vector<uint8_t> data = {1, 2, 3, 4};
    
    Entry entry(keys, data);
    
    EXPECT_EQ(std::get<0>(entry.keys), 42);
    EXPECT_EQ(std::get<1>(entry.keys), "test@example.com");
    EXPECT_EQ(entry.data.size(), 4);
}

TEST(ZerializeEntryTest, SpanConstruction) {
    using namespace multi_index_lru;
    using Entry = EntryWithKeys_t<int64_t>;
    
    std::vector<uint8_t> original = {10, 20, 30};
    std::span<const uint8_t> span(original);
    
    Entry entry(std::make_tuple(100LL), span);
    
    EXPECT_EQ(entry.data.size(), 3);
    EXPECT_EQ(entry.data[0], 10);
}

TEST(ZerializeEntryTest, Deserialization) {
    using namespace multi_index_lru;
    using Entry = EntryWithKeys_t<int64_t, std::string>;
    
    auto data = make_mock_data(1, 100, 200, "alice@test.com", "Alice", 95.5, true);
    Entry entry(std::make_tuple(1LL, std::string("alice@test.com")), data);
    
    auto reader = entry.deserialize<MockDeserializer>();
    EXPECT_EQ(reader["name"].asString(), "Alice");
    EXPECT_EQ(reader["score"].asDouble(), 95.5);
    EXPECT_TRUE(reader["active"].asBool());
}

TEST(ZerializeEntryTest, RawDataAccess) {
    using namespace multi_index_lru;
    using Entry = EntryWithKeys_t<int64_t>;
    
    std::vector<uint8_t> original = {1, 2, 3, 4, 5};
    Entry entry(std::make_tuple(1LL), original);
    
    auto span = entry.raw_data();
    EXPECT_EQ(span.size(), 5);
    EXPECT_EQ(span[0], 1);
    EXPECT_EQ(span[4], 5);
}

// =============================================================================
// Test: Key extractors
// =============================================================================

TEST(KeyExtractorTest, SingleKey) {
    using namespace multi_index_lru;
    using Entry = EntryWithKeys_t<int64_t, std::string, double>;
    
    Entry entry(std::make_tuple(42LL, std::string("test"), 3.14), std::vector<uint8_t>{});
    
    key<0, Entry> extractor0;
    key<1, Entry> extractor1;
    key<2, Entry> extractor2;
    
    EXPECT_EQ(extractor0(entry), 42);
    EXPECT_EQ(extractor1(entry), "test");
    EXPECT_DOUBLE_EQ(extractor2(entry), 3.14);
}

// =============================================================================
// Test: Field extractors
// =============================================================================

TEST(FieldExtractorTest, Int64Field) {
    using namespace multi_index_lru;
    
    auto data = make_mock_data(123, 456, 789, "test@test.com", "Test", 0.0, true);
    MockDeserializer reader(data);
    
    auto extractor = int64_field("id");
    EXPECT_EQ(extractor(reader), 123);
    
    auto extractor2 = int64_field("tenant_id");
    EXPECT_EQ(extractor2(reader), 456);
}

TEST(FieldExtractorTest, StringField) {
    using namespace multi_index_lru;
    
    auto data = make_mock_data(1, 2, 3, "hello@world.com", "Hello World", 0.0, true);
    MockDeserializer reader(data);
    
    auto extractor = string_field("email");
    EXPECT_EQ(extractor(reader), "hello@world.com");
    
    auto extractor2 = string_field("name");
    EXPECT_EQ(extractor2(reader), "Hello World");
}

TEST(FieldExtractorTest, DoubleField) {
    using namespace multi_index_lru;
    
    auto data = make_mock_data(1, 2, 3, "x@y.com", "X", 99.5, true);
    MockDeserializer reader(data);
    
    auto extractor = double_field("score");
    EXPECT_DOUBLE_EQ(extractor(reader), 99.5);
}

TEST(FieldExtractorTest, BoolField) {
    using namespace multi_index_lru;
    
    auto data = make_mock_data(1, 2, 3, "x@y.com", "X", 0.0, false);
    MockDeserializer reader(data);
    
    auto extractor = bool_field("active");
    EXPECT_FALSE(extractor(reader));
}

// =============================================================================
// Test: EntryBuilder
// =============================================================================

TEST(EntryBuilderTest, BuildFromSpan) {
    using namespace multi_index_lru;
    using Entry = EntryWithKeys_t<int64_t, int64_t, std::string>;
    
    auto builder = make_entry_builder<Entry>(
        int64_field("tenant_id"),
        int64_field("user_id"),
        string_field("email")
    );
    
    auto data = make_mock_data(1, 100, 200, "test@example.com", "Test", 0.0, true);
    auto entry = builder.build<MockDeserializer>(data);
    
    EXPECT_EQ(std::get<0>(entry.keys), 100);  // tenant_id
    EXPECT_EQ(std::get<1>(entry.keys), 200);  // user_id
    EXPECT_EQ(std::get<2>(entry.keys), "test@example.com");  // email
    EXPECT_FALSE(entry.data.empty());
}

TEST(EntryBuilderTest, BuildFromReader) {
    using namespace multi_index_lru;
    using Entry = EntryWithKeys_t<int64_t, std::string>;
    
    auto builder = make_entry_builder<Entry>(
        int64_field("id"),
        string_field("name")
    );
    
    auto data = make_mock_data(42, 1, 2, "x@y.com", "Alice", 0.0, true);
    MockDeserializer reader(data);
    auto entry = builder.build(reader, std::span<const uint8_t>(data));
    
    EXPECT_EQ(std::get<0>(entry.keys), 42);
    EXPECT_EQ(std::get<1>(entry.keys), "Alice");
}

// =============================================================================
// Test: Integration with Container - Single Index
// =============================================================================

TEST(ZerializeCacheTest, SingleIndex) {
    using namespace multi_index_lru;
    
    struct IdTag {};
    using Entry = EntryWithKeys_t<int64_t>;
    using Cache = Container<
        Entry,
        boost::multi_index::indexed_by<
            boost::multi_index::hashed_unique<
                boost::multi_index::tag<IdTag>,
                key<0, Entry>
            >
        >
    >;
    
    auto builder = make_entry_builder<Entry>(int64_field("id"));
    
    Cache cache(10);
    
    cache.emplace(builder.build<MockDeserializer>(make_mock_data(1, 0, 0, "", "", 0, true)));
    cache.emplace(builder.build<MockDeserializer>(make_mock_data(2, 0, 0, "", "", 0, true)));
    cache.emplace(builder.build<MockDeserializer>(make_mock_data(3, 0, 0, "", "", 0, true)));
    
    EXPECT_EQ(cache.size(), 3);
    
    auto it = cache.find<IdTag>(2LL);
    ASSERT_NE(it, cache.end<IdTag>());
    EXPECT_EQ(std::get<0>(it->keys), 2);
}

// =============================================================================
// Test: Integration with Container - Composite Key
// =============================================================================

TEST(ZerializeCacheTest, CompositeKey) {
    using namespace multi_index_lru;
    
    struct TenantUserTag {};
    struct EmailTag {};
    
    using Entry = EntryWithKeys_t<int64_t, int64_t, std::string>;
    using Cache = Container<
        Entry,
        boost::multi_index::indexed_by<
            boost::multi_index::ordered_unique<
                boost::multi_index::tag<TenantUserTag>,
                boost::multi_index::composite_key<
                    Entry,
                    key<0, Entry>,
                    key<1, Entry>
                >
            >,
            boost::multi_index::hashed_unique<
                boost::multi_index::tag<EmailTag>,
                key<2, Entry>
            >
        >
    >;
    
    auto builder = make_entry_builder<Entry>(
        int64_field("tenant_id"),
        int64_field("user_id"),
        string_field("email")
    );
    
    Cache cache(100);
    
    // Insert entries
    cache.emplace(builder.build<MockDeserializer>(
        make_mock_data(1, 1, 100, "alice@t1.com", "Alice", 0, true)));
    cache.emplace(builder.build<MockDeserializer>(
        make_mock_data(2, 1, 101, "bob@t1.com", "Bob", 0, true)));
    cache.emplace(builder.build<MockDeserializer>(
        make_mock_data(3, 2, 100, "charlie@t2.com", "Charlie", 0, true)));
    
    EXPECT_EQ(cache.size(), 3);
    
    // Find by composite key (tenant_id=1, user_id=100)
    auto it = cache.find<TenantUserTag>(std::make_tuple(1LL, 100LL));
    ASSERT_NE(it, cache.end<TenantUserTag>());
    EXPECT_EQ(std::get<2>(it->keys), "alice@t1.com");
    
    // Find by email
    auto it2 = cache.find<EmailTag>(std::string("bob@t1.com"));
    ASSERT_NE(it2, cache.end<EmailTag>());
    EXPECT_EQ(std::get<1>(it2->keys), 101);  // user_id
}

// =============================================================================
// Test: LRU behavior with zerialize entries
// =============================================================================

TEST(ZerializeCacheTest, LRUEviction) {
    using namespace multi_index_lru;
    
    struct IdTag {};
    using Entry = EntryWithKeys_t<int64_t>;
    using Cache = Container<
        Entry,
        boost::multi_index::indexed_by<
            boost::multi_index::ordered_unique<
                boost::multi_index::tag<IdTag>,
                key<0, Entry>
            >
        >
    >;
    
    auto builder = make_entry_builder<Entry>(int64_field("id"));
    Cache cache(3);
    
    cache.emplace(builder.build<MockDeserializer>(make_mock_data(1, 0, 0, "", "", 0, true)));
    cache.emplace(builder.build<MockDeserializer>(make_mock_data(2, 0, 0, "", "", 0, true)));
    cache.emplace(builder.build<MockDeserializer>(make_mock_data(3, 0, 0, "", "", 0, true)));
    
    // Access 1 and 3 to make them recent
    cache.find<IdTag>(1LL);
    cache.find<IdTag>(3LL);
    
    // Add 4 - should evict 2 (LRU)
    cache.emplace(builder.build<MockDeserializer>(make_mock_data(4, 0, 0, "", "", 0, true)));
    
    EXPECT_TRUE(cache.contains<IdTag>(1LL));
    EXPECT_FALSE(cache.contains<IdTag>(2LL));  // Evicted
    EXPECT_TRUE(cache.contains<IdTag>(3LL));
    EXPECT_TRUE(cache.contains<IdTag>(4LL));
}

// =============================================================================
// Test: Different zerialize format types (simulated)
// =============================================================================

template <typename Deserializer>
void test_format() {
    using namespace multi_index_lru;
    
    struct IdTag {};
    using Entry = EntryWithKeys_t<int64_t, std::string>;
    using Cache = Container<
        Entry,
        boost::multi_index::indexed_by<
            boost::multi_index::hashed_unique<
                boost::multi_index::tag<IdTag>,
                key<0, Entry>
            >
        >
    >;
    
    auto builder = make_entry_builder<Entry>(
        int64_field("id"),
        string_field("name")
    );
    
    Cache cache(10);
    auto data = make_mock_data(42, 0, 0, "", "TestName", 0, true);
    cache.emplace(builder.template build<Deserializer>(data));
    
    auto it = cache.template find<IdTag>(42LL);
    ASSERT_NE(it, cache.template end<IdTag>());
    
    // Deserialize using the same format
    auto reader = it->template deserialize<Deserializer>();
    EXPECT_EQ(reader["name"].asString(), "TestName");
}

TEST(ZerializeFormatsTest, JSON) {
    test_format<mock_zerialize::JSON::Deserializer>();
}

TEST(ZerializeFormatsTest, MsgPack) {
    test_format<mock_zerialize::MsgPack::Deserializer>();
}

TEST(ZerializeFormatsTest, CBOR) {
    test_format<mock_zerialize::CBOR::Deserializer>();
}

TEST(ZerializeFormatsTest, Flex) {
    test_format<mock_zerialize::Flex::Deserializer>();
}

TEST(ZerializeFormatsTest, ZERA) {
    test_format<mock_zerialize::ZERA::Deserializer>();
}

// =============================================================================
// Test: Multiple key types
// =============================================================================

TEST(ZerializeCacheTest, MixedKeyTypes) {
    using namespace multi_index_lru;
    
    struct IdTag {};
    struct ScoreTag {};
    struct ActiveTag {};
    
    using Entry = EntryWithKeys_t<int64_t, double, bool, std::string>;
    using Cache = Container<
        Entry,
        boost::multi_index::indexed_by<
            boost::multi_index::hashed_unique<
                boost::multi_index::tag<IdTag>,
                key<0, Entry>
            >,
            boost::multi_index::ordered_non_unique<
                boost::multi_index::tag<ScoreTag>,
                key<1, Entry>
            >
        >
    >;
    
    auto builder = make_entry_builder<Entry>(
        int64_field("id"),
        double_field("score"),
        bool_field("active"),
        string_field("name")
    );
    
    Cache cache(10);
    
    cache.emplace(builder.build<MockDeserializer>(
        make_mock_data(1, 0, 0, "", "Alice", 95.5, true)));
    cache.emplace(builder.build<MockDeserializer>(
        make_mock_data(2, 0, 0, "", "Bob", 87.0, false)));
    cache.emplace(builder.build<MockDeserializer>(
        make_mock_data(3, 0, 0, "", "Charlie", 95.5, true)));
    
    EXPECT_EQ(cache.size(), 3);
    
    // Find by ID
    auto it = cache.find<IdTag>(2LL);
    ASSERT_NE(it, cache.end<IdTag>());
    EXPECT_EQ(std::get<3>(it->keys), "Bob");
    EXPECT_FALSE(std::get<2>(it->keys));  // active = false
}

// =============================================================================
// Test: Capacity changes
// =============================================================================

TEST(ZerializeCacheTest, SetCapacity) {
    using namespace multi_index_lru;
    
    struct IdTag {};
    using Entry = EntryWithKeys_t<int64_t>;
    using Cache = Container<
        Entry,
        boost::multi_index::indexed_by<
            boost::multi_index::ordered_unique<
                boost::multi_index::tag<IdTag>,
                key<0, Entry>
            >
        >
    >;
    
    auto builder = make_entry_builder<Entry>(int64_field("id"));
    Cache cache(10);
    
    for (int i = 1; i <= 5; ++i) {
        cache.emplace(builder.build<MockDeserializer>(
            make_mock_data(i, 0, 0, "", "", 0, true)));
    }
    
    EXPECT_EQ(cache.size(), 5);
    
    cache.set_capacity(2);
    EXPECT_EQ(cache.size(), 2);
    EXPECT_EQ(cache.capacity(), 2);
}

}  // namespace
