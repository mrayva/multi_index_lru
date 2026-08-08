// Tests handle_request_local()'s contract directly: encoded wire request in,
// encoded wire response out, against a real NameCache/IdCache pair -- no
// iceoryx2, no NATS, no process boundary. multi_index_lru::Container's own
// behavior (LRU eviction, node pooling, etc.) is exercised in the main
// repo's test/container_test.cpp; these tests are scoped to the
// decode-operate-encode logic that's specific to this POC's server.
#include "../src/cache_service.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace poc {
namespace {

constexpr std::size_t kCapacity = 100;

class HandleRequestLocalTest : public ::testing::Test {
protected:
    NameCache name_cache{kCapacity};
    IdCache id_cache{kCapacity};

    std::vector<std::uint8_t> handle(const std::vector<std::uint8_t>& request) {
        return handle_request_local(name_cache, id_cache, request.data(), request.size());
    }
};

wire::Status decode_status(const std::vector<std::uint8_t>& response) {
    wire::Reader r(response.data(), response.size());
    return static_cast<wire::Status>(r.u8());
}

// For a Get response: nullopt means NotFound/Error, otherwise the record bytes.
std::optional<std::vector<std::uint8_t>> decode_get(const std::vector<std::uint8_t>& response) {
    wire::Reader r(response.data(), response.size());
    if (static_cast<wire::Status>(r.u8()) != wire::Status::Ok) {
        return std::nullopt;
    }
    return r.remaining();
}

std::vector<std::uint8_t> text(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

// --- Get on empty caches ----------------------------------------------------

TEST_F(HandleRequestLocalTest, GetByNameOnEmptyCacheReturnsNotFound) {
    auto response = handle(encode_get(wire::KeyKind::Name, "alice", 0));
    EXPECT_EQ(decode_status(response), wire::Status::NotFound);
    EXPECT_EQ(decode_get(response), std::nullopt);
}

TEST_F(HandleRequestLocalTest, GetByIdOnEmptyCacheReturnsNotFound) {
    auto response = handle(encode_get(wire::KeyKind::Id, "", 1));
    EXPECT_EQ(decode_status(response), wire::Status::NotFound);
    EXPECT_EQ(decode_get(response), std::nullopt);
}

// --- Put then Get ------------------------------------------------------------

TEST_F(HandleRequestLocalTest, PutByNameThenGetReturnsTheRecord) {
    EXPECT_EQ(decode_status(handle(encode_put(wire::KeyKind::Name, "alice", 0, text("A")))), wire::Status::Ok);

    auto response = handle(encode_get(wire::KeyKind::Name, "alice", 0));
    EXPECT_EQ(decode_status(response), wire::Status::Ok);
    EXPECT_EQ(decode_get(response), text("A"));
}

TEST_F(HandleRequestLocalTest, PutByIdThenGetReturnsTheRecord) {
    EXPECT_EQ(decode_status(handle(encode_put(wire::KeyKind::Id, "", 42, text("fortytwo")))), wire::Status::Ok);

    auto response = handle(encode_get(wire::KeyKind::Id, "", 42));
    EXPECT_EQ(decode_status(response), wire::Status::Ok);
    EXPECT_EQ(decode_get(response), text("fortytwo"));
}

// --- Put is an upsert, not insert-only --------------------------------------

TEST_F(HandleRequestLocalTest, SecondPutByNameOverwritesTheRecord) {
    handle(encode_put(wire::KeyKind::Name, "dave", 0, text("v1")));
    handle(encode_put(wire::KeyKind::Name, "dave", 0, text("v2")));

    auto response = handle(encode_get(wire::KeyKind::Name, "dave", 0));
    EXPECT_EQ(decode_get(response), text("v2"));
    EXPECT_EQ(name_cache.size(), 1u) << "upsert must not leave a duplicate entry behind";
}

TEST_F(HandleRequestLocalTest, SecondPutByIdOverwritesTheRecord) {
    handle(encode_put(wire::KeyKind::Id, "", 7, text("v1")));
    handle(encode_put(wire::KeyKind::Id, "", 7, text("v2")));

    auto response = handle(encode_get(wire::KeyKind::Id, "", 7));
    EXPECT_EQ(decode_get(response), text("v2"));
    EXPECT_EQ(id_cache.size(), 1u) << "upsert must not leave a duplicate entry behind";
}

// --- Erase --------------------------------------------------------------------

TEST_F(HandleRequestLocalTest, EraseExistingNameEntryRemovesItAndReportsOk) {
    handle(encode_put(wire::KeyKind::Name, "alice", 0, text("A")));

    EXPECT_EQ(decode_status(handle(encode_erase(wire::KeyKind::Name, "alice", 0))), wire::Status::Ok);
    EXPECT_EQ(decode_status(handle(encode_get(wire::KeyKind::Name, "alice", 0))), wire::Status::NotFound);
}

TEST_F(HandleRequestLocalTest, EraseExistingIdEntryRemovesItAndReportsOk) {
    handle(encode_put(wire::KeyKind::Id, "", 42, text("A")));

    EXPECT_EQ(decode_status(handle(encode_erase(wire::KeyKind::Id, "", 42))), wire::Status::Ok);
    EXPECT_EQ(decode_status(handle(encode_get(wire::KeyKind::Id, "", 42))), wire::Status::NotFound);
}

TEST_F(HandleRequestLocalTest, EraseOfMissingNameKeyReportsNotFound) {
    EXPECT_EQ(decode_status(handle(encode_erase(wire::KeyKind::Name, "nobody", 0))), wire::Status::NotFound);
}

TEST_F(HandleRequestLocalTest, EraseOfMissingIdKeyReportsNotFound) {
    EXPECT_EQ(decode_status(handle(encode_erase(wire::KeyKind::Id, "", 999))), wire::Status::NotFound);
}

// --- The two caches are genuinely independent --------------------------------

TEST_F(HandleRequestLocalTest, NameCacheAndIdCacheDoNotCrossPollinate) {
    // Put under the *name* cache using the digits "42" as the key ...
    handle(encode_put(wire::KeyKind::Name, "42", 0, text("name-42")));

    // ... must not be visible through the *id* cache's key 42.
    EXPECT_EQ(decode_status(handle(encode_get(wire::KeyKind::Id, "", 42))), wire::Status::NotFound);

    // And erasing id=42 (never inserted there) must not touch name="42".
    handle(encode_erase(wire::KeyKind::Id, "", 42));
    auto response = handle(encode_get(wire::KeyKind::Name, "42", 0));
    EXPECT_EQ(decode_get(response), text("name-42"));
}

// --- Exception hardening: a malformed request must not crash/throw ----------
// (This is what previously would have propagated std::out_of_range out of
// the request loop -- see cache_service.hpp's comment on handle_request_local.)

TEST_F(HandleRequestLocalTest, EmptyRequestReturnsErrorInsteadOfThrowing) {
    std::vector<std::uint8_t> empty;
    EXPECT_NO_THROW({
        auto response = handle(empty);
        EXPECT_EQ(decode_status(response), wire::Status::Error);
    });
}

TEST_F(HandleRequestLocalTest, TruncatedStringLengthPrefixReturnsErrorInsteadOfThrowing) {
    // Op::Get, KeyKind::Name, then a length prefix claiming ~2GB with no
    // actual bytes behind it -- the exact shape used to verify the real
    // servers survive a malformed request live.
    std::vector<std::uint8_t> garbage{static_cast<std::uint8_t>(wire::Op::Get),
                                       static_cast<std::uint8_t>(wire::KeyKind::Name), 0xFF, 0xFF, 0xFF, 0x7F};
    EXPECT_NO_THROW({
        auto response = handle(garbage);
        EXPECT_EQ(decode_status(response), wire::Status::Error);
    });
}

TEST_F(HandleRequestLocalTest, TruncatedPutRecordLengthReturnsErrorInsteadOfThrowing) {
    // A Put whose declared record_len exceeds what actually follows it.
    auto request = encode_put(wire::KeyKind::Name, "alice", 0, text("short"));
    request.resize(request.size() - 2);  // chop off the tail of the record bytes
    EXPECT_NO_THROW({
        auto response = handle(request);
        EXPECT_EQ(decode_status(response), wire::Status::Error);
    });
    // And the cache must not have been left with a half-applied entry.
    EXPECT_EQ(name_cache.size(), 0u);
}

TEST_F(HandleRequestLocalTest, UnknownOpByteReturnsNotFoundInsteadOfCrashing) {
    std::vector<std::uint8_t> request{0x99, static_cast<std::uint8_t>(wire::KeyKind::Name)};
    EXPECT_NO_THROW({
        auto response = handle(request);
        EXPECT_EQ(decode_status(response), wire::Status::NotFound);
    });
}

}  // namespace
}  // namespace poc
