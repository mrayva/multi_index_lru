// Copyright 2026 multi_index_lru contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <multi_index_lru/expirable_container.hpp>
#include <multi_index_lru/zerialize_cache.hpp>

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

namespace {

// =============================================================================
// Basic ExpirableContainer Tests
// =============================================================================

struct IdTag {};
struct EmailTag {};
struct NameTag {};

struct User {
    int id;
    std::string email;
    std::string name;

    bool operator==(const User& other) const {
        return id == other.id && email == other.email && name == other.name;
    }
};

using UserCache = multi_index_lru::ExpirableContainer<
    User,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<IdTag>,
            boost::multi_index::member<multi_index_lru::detail::TimestampedValue<User>, 
                User, &multi_index_lru::detail::TimestampedValue<User>::value>,
            boost::multi_index::composite_key_compare<
                std::less<int>>>,
        boost::multi_index::hashed_unique<
            boost::multi_index::tag<EmailTag>,
            boost::multi_index::member<multi_index_lru::detail::TimestampedValue<User>,
                User, &multi_index_lru::detail::TimestampedValue<User>::value>,
            boost::hash<std::string>>>>;

// Simpler index definition using id directly
struct ExpirableUserValue {
    int id;
    std::string email;
    std::string name;
};

struct ManualClock {
    using duration = std::chrono::milliseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<ManualClock, duration>;
    [[maybe_unused]] static constexpr bool is_steady = true;

    static time_point now() noexcept { return now_; }
    static void reset() noexcept { now_ = time_point{}; }
    static void advance(duration amount) noexcept { now_ += amount; }

private:
    static inline time_point now_{};
};

using SimpleUserCache = multi_index_lru::ExpirableContainer<
    ExpirableUserValue,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<IdTag>,
            boost::multi_index::member<
                multi_index_lru::detail::TimestampedValue<ExpirableUserValue>,
                ExpirableUserValue,
                &multi_index_lru::detail::TimestampedValue<ExpirableUserValue>::value>>>>;

// Even simpler - use global key extractor
template <typename T>
struct IdExtractor {
    using result_type = int;
    result_type operator()(const T& wrapped) const { return wrapped.value.id; }
};

using EasierUserCache = multi_index_lru::ExpirableContainer<
    ExpirableUserValue,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<IdTag>,
            IdExtractor<multi_index_lru::detail::TimestampedValue<ExpirableUserValue>>>>>;

using ManualUserCache = multi_index_lru::ExpirableContainer<
    ExpirableUserValue,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<IdTag>,
            IdExtractor<multi_index_lru::detail::TimestampedValue<ExpirableUserValue, ManualClock>>>>,
    std::allocator<ExpirableUserValue>,
    ManualClock>;

class ExpirableBasicTest : public ::testing::Test {
protected:
    EasierUserCache cache{3, 1h};  // Long TTL for non-TTL tests
};

TEST_F(ExpirableBasicTest, InsertAndFind) {
    EXPECT_TRUE(cache.insert(ExpirableUserValue{1, "alice@test.com", "Alice"}));
    EXPECT_TRUE(cache.insert(ExpirableUserValue{2, "bob@test.com", "Bob"}));
    
    EXPECT_EQ(cache.size(), 2);
    EXPECT_FALSE(cache.empty());
    
    auto it = cache.find<IdTag>(1);
    EXPECT_NE(it, cache.end<IdTag>());
    EXPECT_EQ(it->id, 1);
    EXPECT_EQ(it->name, "Alice");
}

TEST_F(ExpirableBasicTest, LRUEviction) {
    cache.insert(ExpirableUserValue{1, "alice@test.com", "Alice"});
    cache.insert(ExpirableUserValue{2, "bob@test.com", "Bob"});
    cache.insert(ExpirableUserValue{3, "charlie@test.com", "Charlie"});
    
    // Access Alice to make her recently used
    EXPECT_NE(cache.find<IdTag>(1), cache.end<IdTag>());
    
    // Insert fourth - Bob should be evicted (LRU)
    cache.insert(ExpirableUserValue{4, "david@test.com", "David"});
    
    EXPECT_EQ(cache.size(), 3);
    EXPECT_NE(cache.find<IdTag>(1), cache.end<IdTag>());  // Alice remains
    EXPECT_EQ(cache.find<IdTag>(2), cache.end<IdTag>());  // Bob evicted
    EXPECT_NE(cache.find<IdTag>(3), cache.end<IdTag>());  // Charlie remains
    EXPECT_NE(cache.find<IdTag>(4), cache.end<IdTag>());  // David added
}

TEST_F(ExpirableBasicTest, Contains) {
    cache.insert(ExpirableUserValue{1, "alice@test.com", "Alice"});
    
    EXPECT_TRUE(cache.contains<IdTag>(1));
    EXPECT_FALSE(cache.contains<IdTag>(999));
}

TEST_F(ExpirableBasicTest, Erase) {
    cache.insert(ExpirableUserValue{1, "alice@test.com", "Alice"});
    cache.insert(ExpirableUserValue{2, "bob@test.com", "Bob"});
    
    EXPECT_TRUE(cache.erase<IdTag>(1));
    EXPECT_EQ(cache.size(), 1);
    EXPECT_EQ(cache.find<IdTag>(1), cache.end<IdTag>());

    auto next = cache.erase(cache.find_no_update<IdTag>(2));
    EXPECT_EQ(next, cache.end<IdTag>());
    EXPECT_TRUE(cache.empty());
    
    EXPECT_FALSE(cache.erase<IdTag>(999));  // Non-existent
}

TEST_F(ExpirableBasicTest, Clear) {
    cache.insert(ExpirableUserValue{1, "alice@test.com", "Alice"});
    cache.insert(ExpirableUserValue{2, "bob@test.com", "Bob"});
    
    cache.clear();
    
    EXPECT_TRUE(cache.empty());
    EXPECT_EQ(cache.size(), 0);
}

TEST_F(ExpirableBasicTest, SetCapacity) {
    cache.insert(ExpirableUserValue{1, "alice@test.com", "Alice"});
    cache.insert(ExpirableUserValue{2, "bob@test.com", "Bob"});
    cache.insert(ExpirableUserValue{3, "charlie@test.com", "Charlie"});
    
    EXPECT_EQ(cache.capacity(), 3);
    
    cache.set_capacity(2);
    
    EXPECT_EQ(cache.capacity(), 2);
    EXPECT_LE(cache.size(), 2);
}

TEST_F(ExpirableBasicTest, ConfiguresRetainedNodePool) {
    EasierUserCache bounded(3, 1h, 1);
    EXPECT_EQ(bounded.node_pool_capacity(), 1);

    bounded.insert(ExpirableUserValue{1, "alice@test.com", "Alice"});
    bounded.insert(ExpirableUserValue{2, "bob@test.com", "Bob"});
    bounded.clear();
    EXPECT_EQ(bounded.node_pool_size(), 1);

    bounded.set_node_pool_capacity(0);
    EXPECT_EQ(bounded.node_pool_size(), 0);
}

TEST_F(ExpirableBasicTest, ParameterValidation) {
    EXPECT_THROW((EasierUserCache{0, 1h}), std::invalid_argument);
    EXPECT_THROW((EasierUserCache{3, 0ms}), std::invalid_argument);

    EasierUserCache cache(3, 1h);
    EXPECT_THROW(cache.set_capacity(0), std::invalid_argument);
    EXPECT_THROW(cache.set_ttl(0ms), std::invalid_argument);
}

// =============================================================================
// TTL Expiration Tests
// =============================================================================

TEST(ExpirableTTLTest, ItemsExpire) {
    ManualClock::reset();
    ManualUserCache cache(100, 50ms);
    
    cache.insert(ExpirableUserValue{1, "alice@test.com", "Alice"});
    cache.insert(ExpirableUserValue{2, "bob@test.com", "Bob"});
    
    // Items should still exist
    EXPECT_NE(cache.find<IdTag>(1), cache.end<IdTag>());
    EXPECT_NE(cache.find<IdTag>(2), cache.end<IdTag>());
    
    // Wait for TTL to expire
    ManualClock::advance(70ms);
    
    // Items should be expired and removed on access
    EXPECT_EQ(cache.find<IdTag>(1), cache.end<IdTag>());
    EXPECT_EQ(cache.find<IdTag>(2), cache.end<IdTag>());
    EXPECT_EQ(cache.size(), 0);
}

TEST(ExpirableTTLTest, AccessRefreshesTTL) {
    ManualClock::reset();
    ManualUserCache cache(100, 100ms);
    
    cache.insert(ExpirableUserValue{1, "alice@test.com", "Alice"});
    
    // Wait 60ms
    ManualClock::advance(60ms);
    
    // Access refreshes TTL
    EXPECT_NE(cache.find<IdTag>(1), cache.end<IdTag>());
    
    // Wait another 60ms (total 120ms from insert, but only 60ms from access)
    ManualClock::advance(60ms);
    
    // Should still exist (TTL refreshed)
    EXPECT_NE(cache.find<IdTag>(1), cache.end<IdTag>());
    
    // Wait full TTL
    ManualClock::advance(120ms);
    
    EXPECT_EQ(cache.find<IdTag>(1), cache.end<IdTag>());
}

TEST(ExpirableTTLTest, FindNoUpdateDoesNotRefresh) {
    ManualClock::reset();
    ManualUserCache cache(100, 80ms);
    
    cache.insert(ExpirableUserValue{1, "alice@test.com", "Alice"});
    
    // Wait 50ms
    ManualClock::advance(50ms);
    
    // find_no_update does NOT refresh TTL
    auto it = cache.find_no_update<IdTag>(1);
    EXPECT_NE(it, cache.end<IdTag>());
    
    // Wait another 50ms (total 100ms > 80ms TTL)
    ManualClock::advance(50ms);
    
    // Should now be expired
    EXPECT_EQ(cache.find<IdTag>(1), cache.end<IdTag>());
}

TEST(ExpirableTTLTest, ContainsNoUpdateDoesNotRefresh) {
    ManualClock::reset();
    ManualUserCache cache(100, 80ms);

    cache.insert(ExpirableUserValue{1, "alice@test.com", "Alice"});
    ManualClock::advance(50ms);

    EXPECT_TRUE(cache.contains_no_update<IdTag>(1));
    EXPECT_FALSE(cache.contains_no_update<IdTag>(999));

    ManualClock::advance(50ms);

    // contains() checks TTL and should remove the expired element.
    EXPECT_FALSE(cache.contains<IdTag>(1));
}

TEST(ExpirableTTLTest, CleanupExpired) {
    ManualClock::reset();
    ManualUserCache cache(100, 50ms);
    
    cache.insert(ExpirableUserValue{1, "alice@test.com", "Alice"});
    cache.insert(ExpirableUserValue{2, "bob@test.com", "Bob"});
    
    EXPECT_EQ(cache.size(), 2);
    
    // Wait for expiration
    ManualClock::advance(70ms);
    
    // Size still shows 2 (not cleaned up yet)
    EXPECT_EQ(cache.size(), 2);
    
    cache.cleanup_expired();
    
    EXPECT_EQ(cache.size(), 0);
}

TEST(ExpirableTTLTest, SetTTL) {
    EasierUserCache cache(100, 1h);
    
    EXPECT_EQ(cache.ttl(), 1h);
    
    cache.set_ttl(30min);
    
    EXPECT_EQ(cache.ttl(), 30min);
}

// =============================================================================
// ExpirableContainer with non-unique indices (equal_range)
// =============================================================================

struct NameExtractor {
    using result_type = std::string;
    template <typename T>
    result_type operator()(const T& wrapped) const { return wrapped.value.name; }
};

using MultiNameCache = multi_index_lru::ExpirableContainer<
    ExpirableUserValue,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<IdTag>,
            IdExtractor<multi_index_lru::detail::TimestampedValue<ExpirableUserValue>>>,
        boost::multi_index::ordered_non_unique<
            boost::multi_index::tag<NameTag>,
            NameExtractor>>>;

using ManualMultiNameCache = multi_index_lru::ExpirableContainer<
    ExpirableUserValue,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<IdTag>,
            IdExtractor<multi_index_lru::detail::TimestampedValue<ExpirableUserValue, ManualClock>>>,
        boost::multi_index::ordered_non_unique<
            boost::multi_index::tag<NameTag>,
            NameExtractor>>,
    std::allocator<ExpirableUserValue>,
    ManualClock>;

TEST(ExpirableEqualRangeTest, BasicEqualRange) {
    MultiNameCache cache(10, 1h);
    
    cache.insert(ExpirableUserValue{1, "john1@test.com", "John"});
    cache.insert(ExpirableUserValue{2, "john2@test.com", "John"});
    cache.insert(ExpirableUserValue{3, "john3@test.com", "John"});
    cache.insert(ExpirableUserValue{4, "alice@test.com", "Alice"});
    
    auto [begin, end] = cache.equal_range<NameTag>(std::string("John"));
    
    int count = 0;
    for (auto it = begin; it != end; ++it) {
        EXPECT_EQ(it->name, "John");
        ++count;
    }
    EXPECT_EQ(count, 3);
}

TEST(ExpirableEqualRangeTest, EqualRangeRemovesExpired) {
    ManualClock::reset();
    ManualMultiNameCache cache(10, 50ms);
    
    cache.insert(ExpirableUserValue{1, "john1@test.com", "John"});
    cache.insert(ExpirableUserValue{2, "john2@test.com", "John"});
    
    ManualClock::advance(70ms);
    
    auto [begin, end] = cache.equal_range<NameTag>(std::string("John"));
    
    // Expired items should be removed
    EXPECT_EQ(begin, end);
    EXPECT_EQ(cache.size(), 0);
}

TEST(ExpirableEqualRangeTest, EqualRangeNoUpdateConstOverload) {
    MultiNameCache cache(10, 1h);
    cache.insert(ExpirableUserValue{1, "john1@test.com", "John"});
    cache.insert(ExpirableUserValue{2, "john2@test.com", "John"});

    const auto& const_cache = cache;
    auto [begin, end] = const_cache.equal_range_no_update<NameTag>(std::string("John"));

    int count = 0;
    for (auto it = begin; it != end; ++it) {
        ++count;
    }
    EXPECT_EQ(count, 2);
}

// =============================================================================
// ExpirableContainer with Zerialize Integration
// =============================================================================

// Mock deserializer (same as in zerialize_test.cpp)
class MockDeserializer {
public:
    MockDeserializer() = default;
    explicit MockDeserializer(std::span<const uint8_t> data) : data_(data) {}
    
    MockDeserializer operator[](const std::string& key) const {
        MockDeserializer child;
        child.data_ = data_;
        child.field_ = key;
        return child;
    }
    
    bool isMap() const { return true; }
    int64_t asInt64() const { return field_ == "id" ? 42 : (field_ == "count" ? 100 : 0); }
    std::string asString() const { return field_ == "name" ? "Alice" : (field_ == "category" ? "test" : ""); }
    double asDouble() const { return field_ == "score" ? 3.14 : 0.0; }
    bool asBool() const { return field_ == "active"; }

private:
    std::span<const uint8_t> data_;
    std::string field_;
};

// Zerialize entry type
using ZEntry = multi_index_lru::ZerializeEntry<std::tuple<int64_t, std::string>>;

// Key extractors for ExpirableContainer with ZerializeEntry
template <typename T>
struct ZIdExtractor {
    using result_type = int64_t;
    result_type operator()(const T& wrapped) const { 
        return std::get<0>(wrapped.value.keys); 
    }
};

template <typename T>
struct ZNameExtractor {
    using result_type = std::string;
    result_type operator()(const T& wrapped) const { 
        return std::get<1>(wrapped.value.keys); 
    }
};

using ZerializeExpirableCache = multi_index_lru::ExpirableContainer<
    ZEntry,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<IdTag>,
            ZIdExtractor<multi_index_lru::detail::TimestampedValue<ZEntry>>>,
        boost::multi_index::ordered_non_unique<
            boost::multi_index::tag<NameTag>,
            ZNameExtractor<multi_index_lru::detail::TimestampedValue<ZEntry>>>>>;

using ManualZerializeExpirableCache = multi_index_lru::ExpirableContainer<
    ZEntry,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<IdTag>,
            ZIdExtractor<multi_index_lru::detail::TimestampedValue<ZEntry, ManualClock>>>,
        boost::multi_index::ordered_non_unique<
            boost::multi_index::tag<NameTag>,
            ZNameExtractor<multi_index_lru::detail::TimestampedValue<ZEntry, ManualClock>>>>,
    std::allocator<ZEntry>,
    ManualClock>;

TEST(ExpirableZerializeTest, BasicOperations) {
    ZerializeExpirableCache cache(3, 1h);
    
    auto builder = multi_index_lru::make_entry_builder<ZEntry>(
        multi_index_lru::int64_field("id"),
        multi_index_lru::string_field("name")
    );
    
    std::vector<uint8_t> data1 = {1, 2, 3};
    auto entry1 = builder.template build<MockDeserializer>(data1);
    
    EXPECT_TRUE(cache.insert(entry1));
    EXPECT_EQ(cache.size(), 1);
    
    auto it = cache.find<IdTag>(static_cast<int64_t>(42));
    EXPECT_NE(it, cache.end<IdTag>());
    EXPECT_EQ(std::get<0>(it->keys), 42);
    EXPECT_EQ(std::get<1>(it->keys), "Alice");
}

TEST(ExpirableZerializeTest, TTLExpiration) {
    ManualClock::reset();
    ManualZerializeExpirableCache cache(100, 50ms);
    
    auto builder = multi_index_lru::make_entry_builder<ZEntry>(
        multi_index_lru::int64_field("id"),
        multi_index_lru::string_field("name")
    );
    
    std::vector<uint8_t> data = {1, 2, 3};
    cache.insert(builder.template build<MockDeserializer>(data));
    
    EXPECT_NE(cache.find<IdTag>(static_cast<int64_t>(42)), cache.end<IdTag>());
    
    ManualClock::advance(70ms);
    
    EXPECT_EQ(cache.find<IdTag>(static_cast<int64_t>(42)), cache.end<IdTag>());
}

TEST(ExpirableZerializeTest, LRUWithTTL) {
    ZerializeExpirableCache cache(2, 1h);
    
    // Insert two entries
    ZEntry e1(std::make_tuple(int64_t{1}, std::string{"Alice"}), std::vector<uint8_t>{1});
    ZEntry e2(std::make_tuple(int64_t{2}, std::string{"Bob"}), std::vector<uint8_t>{2});
    
    cache.insert(e1);
    cache.insert(e2);
    
    // Access e1 to make it recently used
    cache.find<IdTag>(static_cast<int64_t>(1));
    
    // Insert third - e2 should be evicted (LRU)
    ZEntry e3(std::make_tuple(int64_t{3}, std::string{"Charlie"}), std::vector<uint8_t>{3});
    cache.insert(e3);
    
    EXPECT_EQ(cache.size(), 2);
    EXPECT_NE(cache.find<IdTag>(static_cast<int64_t>(1)), cache.end<IdTag>());
    EXPECT_EQ(cache.find<IdTag>(static_cast<int64_t>(2)), cache.end<IdTag>());
    EXPECT_NE(cache.find<IdTag>(static_cast<int64_t>(3)), cache.end<IdTag>());
}

// =============================================================================
// TimestampedKey extractor tests
// =============================================================================

TEST(TimestampedKeyTest, ExtractsThroughWrapper) {
    using Entry = multi_index_lru::ZerializeEntry<std::tuple<int64_t, std::string>>;
    using Wrapped = multi_index_lru::detail::TimestampedValue<Entry>;
    
    Entry e(std::make_tuple(int64_t{42}, std::string{"Test"}), std::vector<uint8_t>{});
    Wrapped wrapped(e);
    
    multi_index_lru::timestamped_key<0, Entry> key0;
    multi_index_lru::timestamped_key<1, Entry> key1;
    
    EXPECT_EQ(key0(wrapped), 42);
    EXPECT_EQ(key1(wrapped), "Test");
}

}  // namespace
