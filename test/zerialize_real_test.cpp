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
/// BEVE is covered separately, in its own zerialize_beve_test.cpp / own
/// zerialize_beve_test executable target, gated behind its own
/// MULTI_INDEX_LRU_BUILD_ZERIALIZE_BEVE_TESTS option: enabling BEVE
/// requires C++23 (glaze's own CMakeLists.txt hard-requires it), and it
/// does so by making the shared `zerialize` FetchContent target itself
/// require C++23 -- which this file's own executable target then also
/// inherits, since it links `zerialize` too, so this file additionally
/// moves to C++23 whenever BEVE is turned on (see test/CMakeLists.txt).
/// The thing that's actually guaranteed to stay C++20 unconditionally is
/// multi_index_lru_test (container_test.cpp, expirable_test.cpp, and this
/// file's own mock counterpart zerialize_test.cpp) -- it never links
/// `zerialize` at all, which is exactly why this file lives in its own
/// target rather than being folded into that one.

#include "zerialize_test_utils.hpp"

#include <zerialize/protocols/json.hpp>
#include <zerialize/protocols/msgpack.hpp>
#include <zerialize/protocols/cbor.hpp>
#include <zerialize/protocols/flex.hpp>
#include <zerialize/protocols/zera.hpp>
#include <zerialize/protocols/bson.hpp>
#include <zerialize/protocols/ion.hpp>

namespace {

namespace z = zerialize;
using zerialize_test_utils::test_real_format;

TEST(ZerializeRealFormatsTest, JSON)    { test_real_format<z::JSON>(); }
TEST(ZerializeRealFormatsTest, MsgPack) { test_real_format<z::MsgPack>(); }
TEST(ZerializeRealFormatsTest, CBOR)    { test_real_format<z::CBOR>(); }
TEST(ZerializeRealFormatsTest, Flex)    { test_real_format<z::Flex>(); }
TEST(ZerializeRealFormatsTest, ZERA)    { test_real_format<z::Zera>(); }
TEST(ZerializeRealFormatsTest, BSON)    { test_real_format<z::Bson>(); }
TEST(ZerializeRealFormatsTest, Ion)     { test_real_format<z::Ion>(); }

}  // namespace
