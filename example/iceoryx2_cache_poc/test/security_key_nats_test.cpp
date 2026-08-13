// Worked example, run for real: multi_index_lru::NatsCompositeKey::pattern()
// (nats_key.hpp) wildcarding an INTERIOR field position, using
// security_cache.hpp's SecurityEntry -- ASSET_TYPE, CUSIP, ISIN, SEDOL, RIC.
// Verified against a real NATS server the same way
// composite_key_nats_test.cpp is. Excluded from the dependency-free
// poc-unit-tests CI job for the same reason -- see README.md "Read-through
// / write-through over NATS" > "Setup" for what needs to be running first:
//   ./nats-server -js -m 8222
//   nats --server localhost:4222 kv add mil_security_key_test
#include "../src/security_cache.hpp"
#include "../src/nats_bridge.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace poc {
namespace {

constexpr auto kBucket = "mil_security_key_test";
constexpr auto kHost = "127.0.0.1";
constexpr std::uint16_t kPort = 4222;
// SecurityCache is an ExpirableContainer (see security_cache.hpp) -- long
// enough that this test (which doesn't sleep) could never hit it.
constexpr auto kTtl = std::chrono::minutes(10);

std::vector<std::uint8_t> bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string str(const std::vector<std::uint8_t>& b) {
    return std::string(b.begin(), b.end());
}

class SecurityKeyNatsTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { nats_ = std::make_unique<NatsBridge>(kHost, kPort); }
    static void TearDownTestSuite() { nats_.reset(); }

    static std::unique_ptr<NatsBridge> nats_;
};

std::unique_ptr<NatsBridge> SecurityKeyNatsTest::nats_;

// --- unique lookup: SecurityKey::key(asset_type, cusip, isin, sedol, ric) -

TEST_F(SecurityKeyNatsTest, PutThenGetByFullKeyRoundTrips) {
    // Real RIC codes routinely carry a literal '.' exchange suffix (e.g.
    // "AAPL.OQ") -- that can't be used as-is as a NATS key field (see the
    // rejection test below), so a real caller substitutes NATS's separator
    // for something else (e.g. "AAPL_OQ") before deriving the key. This
    // test uses the substituted form; the rejection test demonstrates why.
    const auto nats_key_str = SecurityKey::key("EQUITY", "037833100", "US0378331005", "2046251", "AAPL_OQ");
    ASSERT_EQ(nats_key_str, "EQUITY.037833100.US0378331005.2046251.AAPL_OQ");

    nats_->erase(kBucket, nats_key_str);
    ASSERT_TRUE(nats_->put(kBucket, nats_key_str, bytes("Apple Inc.")).first);

    auto result = nats_->get(kBucket, nats_key_str);
    ASSERT_EQ(result.result, NatsResult::Ok);
    EXPECT_EQ(str(result.value), "Apple Inc.");

    // The exact same derived key addresses the local composite-key index
    // too -- boost::multi_index::composite_key's find() takes a tuple of
    // the component values, in field order.
    SecurityCache cache(100, kTtl);
    cache.emplace(SecurityEntry{"EQUITY", "037833100", "US0378331005", "2046251", "AAPL_OQ", result.value});
    auto it = cache.find<SecurityKeyTag>(boost::make_tuple(std::string("EQUITY"), std::string("037833100"),
                                                             std::string("US0378331005"), std::string("2046251"),
                                                             std::string("AAPL_OQ")));
    ASSERT_NE(it, cache.end<SecurityKeyTag>());
    EXPECT_EQ(str(it->record), "Apple Inc.");

    nats_->erase(kBucket, nats_key_str);
}

// --- interior wildcard: SecurityKey::pattern(asset_type, *, isin, *, *) --

TEST_F(SecurityKeyNatsTest, PatternFindsByAssetTypeAndIsinEvenThoughIsinIsNotALeadingField) {
    // Two EQUITY rows (different identifiers) plus one BOND row that
    // happens to reuse the same ISIN value under a different ASSET_TYPE --
    // the pattern must never cross that boundary.
    const auto apple = SecurityKey::key("EQUITY", "037833100", "US0378331005", "2046251", "AAPL_OQ");
    const auto msft = SecurityKey::key("EQUITY", "594918104", "US5949181045", "2588173", "MSFT_OQ");
    const auto bond_same_isin = SecurityKey::key("BOND", "000000000", "US0378331005", "0000000", "N/A");
    nats_->erase(kBucket, apple);
    nats_->erase(kBucket, msft);
    nats_->erase(kBucket, bond_same_isin);
    ASSERT_TRUE(nats_->put(kBucket, apple, bytes("Apple Inc.")).first);
    ASSERT_TRUE(nats_->put(kBucket, msft, bytes("Microsoft Corp.")).first);
    ASSERT_TRUE(nats_->put(kBucket, bond_same_isin, bytes("some bond, same ISIN token")).first);

    // ASSET_TYPE+ISIN known; CUSIP/SEDOL/RIC not -- ISIN sits between two
    // wildcarded positions (CUSIP before it, SEDOL/RIC after), which
    // prefix_pattern() has no way to express.
    const auto pattern =
        SecurityKey::pattern("EQUITY", multi_index_lru::nats_any, "US0378331005", multi_index_lru::nats_any,
                              multi_index_lru::nats_any);
    ASSERT_EQ(pattern, "EQUITY.*.US0378331005.*.*");

    auto result = nats_->list_and_get(kBucket, pattern, /*max_results=*/100);
    ASSERT_EQ(result.result, NatsResult::Ok);
    EXPECT_FALSE(result.truncated);
    ASSERT_EQ(result.entries.size(), 1u) << "must match Apple's EQUITY row, not MSFT's and not the BOND "
                                             "row that reuses the same ISIN under a different ASSET_TYPE";
    EXPECT_EQ(result.entries[0].first, apple);
    EXPECT_EQ(str(result.entries[0].second), "Apple Inc.");

    nats_->erase(kBucket, apple);
    nats_->erase(kBucket, msft);
    nats_->erase(kBucket, bond_same_isin);
}

// --- validation: a field value that would corrupt the hierarchy ----------

TEST_F(SecurityKeyNatsTest, KeyDerivationRejectsAFieldContainingADotBeforeTouchingNats) {
    // RIC values routinely contain '.' in real feeds (e.g. "AAPL.OQ" above
    // is already borderline) -- SecurityKey::key() must refuse to build a
    // key from a field containing one, not send something to NATS that
    // *looks* fine but silently adds an extra subject token.
    EXPECT_THROW(SecurityKey::key("EQUITY", "037833100", "US0378331005", "2046251", "AAPL.O"), std::invalid_argument);
}

}  // namespace
}  // namespace poc
