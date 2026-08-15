/// Shared test helper for the "real zerialize" integration test suites
/// (zerialize_real_test.cpp, zerialize_beve_test.cpp). Extracted so both
/// can use the exact same round-trip check without duplicating it --
/// nothing in here is C++23-specific, so it's safe to include from both
/// the C++20 base suite and the separate C++23 BEVE target.

#pragma once

#include <multi_index_lru/container.hpp>
#include <multi_index_lru/zerialize_cache.hpp>

#include <zerialize/zerialize.hpp>

#include <gtest/gtest.h>

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace zerialize_test_utils {

// Round-trips a small composite-keyed cache entry through a real zerialize
// protocol: encode real protocol bytes, extract keys at insert time
// (EntryBuilder, the same path production code uses), find by both a
// composite and a single index, then fully deserialize on a cache hit and
// read a field that was never extracted as a key.
template <typename Protocol>
void test_real_format() {
    namespace z = zerialize;
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

    using Deserializer = typename Protocol::Deserializer;

    auto make_bytes = [](int64_t tenant_id, int64_t user_id,
                          const std::string& email, const std::string& name) {
        auto buf = z::serialize<Protocol>(
            z::zmap<"tenant_id", "user_id", "email", "name">(tenant_id, user_id, email, name));
        auto span = buf.buf();
        return std::vector<uint8_t>(span.begin(), span.end());
    };

    Cache cache(100);
    cache.emplace(builder.template build<Deserializer>(make_bytes(1, 100, "alice@t1.com", "Alice")));
    cache.emplace(builder.template build<Deserializer>(make_bytes(1, 101, "bob@t1.com", "Bob")));
    cache.emplace(builder.template build<Deserializer>(make_bytes(2, 100, "charlie@t2.com", "Charlie")));

    EXPECT_EQ(cache.size(), 3u);

    auto it = cache.template find<TenantUserTag>(std::make_tuple(int64_t(1), int64_t(100)));
    ASSERT_NE(it, cache.template end<TenantUserTag>());
    EXPECT_EQ(std::get<2>(it->keys), "alice@t1.com");

    auto it2 = cache.template find<EmailTag>(std::string("bob@t1.com"));
    ASSERT_NE(it2, cache.template end<EmailTag>());
    EXPECT_EQ(std::get<1>(it2->keys), 101);

    // Cache hit: fully deserialize and read a field that was never
    // extracted as an index key.
    auto reader = it->template deserialize<Deserializer>();
    EXPECT_EQ(reader["name"].asString(), "Alice");
}

}  // namespace zerialize_test_utils
