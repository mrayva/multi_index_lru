/// Integration tests against the REAL zerialize library, not the
/// mock_zerialize namespace in zerialize_test.cpp.
///
/// zerialize_test.cpp's ZerializeFormatsTest suite is misleading if read on
/// its own: test_format<mock_zerialize::JSON::Deserializer>(),
/// test_format<mock_zerialize::MsgPack::Deserializer>(), and the CBOR/Flex/
/// ZERA/BSON/Ion cases all instantiate the *same* MockDeserializer type
/// under seven differently-named aliases (see zerialize_test.cpp's
/// `namespace mock_zerialize { namespace JSON { using Deserializer =
/// MockDeserializer; } ... }`). That gives zero signal about whether any
/// real zerialize protocol actually satisfies the ZerializeDeserializer
/// concept and works correctly through EntryBuilder/ZerializeEntry here --
/// this file closes that gap by exercising the real thing.
///
/// Only compiled when MULTI_INDEX_LRU_BUILD_ZERIALIZE_TESTS is ON, which
/// FetchContents github.com/mrayva/zerialize (see the top-level
/// CMakeLists.txt) -- that in turn vendors its own protocol dependencies
/// (yyjson, msgpack-c, jsoncons, flatbuffers) via its own CMakeLists.txt,
/// so nothing further is needed here.
///
/// BEVE is intentionally not exercised: enabling it in zerialize raises the
/// required C++ standard from C++20 to C++23 (glaze's own CMakeLists.txt
/// hard-requires it), and this project is C++20. Covering it would mean
/// silently bumping multi_index_lru's own compiler requirement whenever
/// this already-opt-in test option is turned on -- a second, separate
/// opt-in would be the right way to add that later, not bundled into this
/// one.

#include <multi_index_lru/container.hpp>
#include <multi_index_lru/zerialize_cache.hpp>

#include <zerialize/zerialize.hpp>
#include <zerialize/protocols/json.hpp>
#include <zerialize/protocols/msgpack.hpp>
#include <zerialize/protocols/cbor.hpp>
#include <zerialize/protocols/flex.hpp>
#include <zerialize/protocols/zera.hpp>
#include <zerialize/protocols/bson.hpp>
#include <zerialize/protocols/ion.hpp>

#include <gtest/gtest.h>

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace {

namespace z = zerialize;
using namespace multi_index_lru;

// Round-trips a small composite-keyed cache entry through a real zerialize
// protocol: encode real protocol bytes, extract keys at insert time
// (EntryBuilder, the same path production code uses), find by both a
// composite and a single index, then fully deserialize on a cache hit and
// read a field that was never extracted as a key.
template <typename Protocol>
void test_real_format() {
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

TEST(ZerializeRealFormatsTest, JSON)    { test_real_format<z::JSON>(); }
TEST(ZerializeRealFormatsTest, MsgPack) { test_real_format<z::MsgPack>(); }
TEST(ZerializeRealFormatsTest, CBOR)    { test_real_format<z::CBOR>(); }
TEST(ZerializeRealFormatsTest, Flex)    { test_real_format<z::Flex>(); }
TEST(ZerializeRealFormatsTest, ZERA)    { test_real_format<z::Zera>(); }
TEST(ZerializeRealFormatsTest, BSON)    { test_real_format<z::Bson>(); }
TEST(ZerializeRealFormatsTest, Ion)     { test_real_format<z::Ion>(); }

}  // namespace
