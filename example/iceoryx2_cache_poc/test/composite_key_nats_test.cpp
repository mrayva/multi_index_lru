// Worked example, run for real: multi_index_lru::NatsCompositeKey
// (nats_key.hpp) deriving NATS keys/patterns for a *genuine* composite
// multi_index key (attribute_cache.hpp's AttributeEntry/AttributeCache --
// unlike NameCache's single flat string, owner and attribute are separate
// typed fields joined by a boost::multi_index::composite_key), verified
// against a real NATS server the same way test/nats_bridge_test.cpp and
// test/dispatch_request_test.cpp are. Excluded from the dependency-free
// poc-unit-tests CI job for the same reason those are (this target only
// exists when POC_ENABLE_NATS_READTHROUGH=ON) -- see README.md
// "Read-through / write-through over NATS" > "Setup" for what needs to be
// running first:
//   ./nats-server -js -m 8222
//   nats --server localhost:4222 kv add mil_composite_key_test
#include "../src/attribute_cache.hpp"
#include "../src/nats_bridge.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace poc {
namespace {

constexpr auto kBucket = "mil_composite_key_test";
constexpr auto kHost = "127.0.0.1";
constexpr std::uint16_t kPort = 4222;

std::vector<std::uint8_t> bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string str(const std::vector<std::uint8_t>& b) {
    return std::string(b.begin(), b.end());
}

// Recovers (owner, attribute) from a NATS key built by
// OwnerAttributeKey::key(owner, attribute) -- unambiguous only because
// nats_key.hpp's validate_nats_key_field() already guarantees neither field
// ever contains '.', so the first dot is genuinely the field boundary, not
// something a real parser would need to guess at.
std::pair<std::string, std::string> split_owner_attribute(const std::string& nats_key_str) {
    const auto dot = nats_key_str.find('.');
    if (dot == std::string::npos) {
        throw std::invalid_argument("not an owner.attribute key: \"" + nats_key_str + "\"");
    }
    return {nats_key_str.substr(0, dot), nats_key_str.substr(dot + 1)};
}

class CompositeKeyNatsTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { nats_ = std::make_unique<NatsBridge>(kHost, kPort); }
    static void TearDownTestSuite() { nats_.reset(); }

    static std::unique_ptr<NatsBridge> nats_;
};

std::unique_ptr<NatsBridge> CompositeKeyNatsTest::nats_;

// --- unique lookup: OwnerAttributeKey::key(owner, attribute) --------------

TEST_F(CompositeKeyNatsTest, PutThenGetByCompositeKeyRoundTrips) {
    const auto nats_key_str = OwnerAttributeKey::key("alice", "email");
    ASSERT_EQ(nats_key_str, "alice.email");

    nats_->erase(kBucket, nats_key_str);
    ASSERT_TRUE(nats_->put(kBucket, nats_key_str, bytes("alice@example.com")).first);

    auto result = nats_->get(kBucket, nats_key_str);
    ASSERT_EQ(result.result, NatsResult::Ok);
    EXPECT_EQ(str(result.value), "alice@example.com");

    // The exact same derived key addresses the local composite-key index
    // too -- boost::multi_index::composite_key's find() takes a tuple of
    // the component values, in field order.
    AttributeCache cache(100);
    cache.emplace(AttributeEntry{"alice", "email", result.value});
    auto it = cache.find<OwnerAttributeTag>(boost::make_tuple(std::string("alice"), std::string("email")));
    ASSERT_NE(it, cache.end<OwnerAttributeTag>());
    EXPECT_EQ(str(it->record), "alice@example.com");

    nats_->erase(kBucket, nats_key_str);
}

// --- prefix lookup: OwnerAttributeKey::prefix_pattern(owner) -------------

TEST_F(CompositeKeyNatsTest, PrefixPatternFetchesExactlyTheMatchingOwnersAttributes) {
    const auto alice_email = OwnerAttributeKey::key("composite_alice", "email");
    const auto alice_phone = OwnerAttributeKey::key("composite_alice", "phone");
    const auto bob_email = OwnerAttributeKey::key("composite_bob", "email");
    nats_->erase(kBucket, alice_email);
    nats_->erase(kBucket, alice_phone);
    nats_->erase(kBucket, bob_email);
    ASSERT_TRUE(nats_->put(kBucket, alice_email, bytes("alice@example.com")).first);
    ASSERT_TRUE(nats_->put(kBucket, alice_phone, bytes("555-0100")).first);
    ASSERT_TRUE(nats_->put(kBucket, bob_email, bytes("bob@example.com")).first);

    const auto pattern = OwnerAttributeKey::prefix_pattern("composite_alice");
    ASSERT_EQ(pattern, "composite_alice.*")
        << "2-field composite key, 1 field given -> exactly 1 remaining -> SingleToken ('*'), "
           "picked automatically by NatsCompositeKey::prefix_pattern()";

    auto result = nats_->list_and_get(kBucket, pattern, /*max_results=*/100);
    ASSERT_EQ(result.result, NatsResult::Ok);
    EXPECT_FALSE(result.truncated);
    ASSERT_EQ(result.entries.size(), 2u) << "must match alice's two attributes, not bob's one";

    AttributeCache cache(100);
    for (const auto& [nats_key_str, value] : result.entries) {
        auto [owner, attribute] = split_owner_attribute(nats_key_str);
        EXPECT_EQ(owner, "composite_alice") << "the pattern must never match a different owner";
        cache.emplace(AttributeEntry{owner, attribute, value});
    }

    auto email_it =
        cache.find<OwnerAttributeTag>(boost::make_tuple(std::string("composite_alice"), std::string("email")));
    ASSERT_NE(email_it, cache.end<OwnerAttributeTag>());
    EXPECT_EQ(str(email_it->record), "alice@example.com");

    auto phone_it =
        cache.find<OwnerAttributeTag>(boost::make_tuple(std::string("composite_alice"), std::string("phone")));
    ASSERT_NE(phone_it, cache.end<OwnerAttributeTag>());
    EXPECT_EQ(str(phone_it->record), "555-0100");

    // bob's attribute must never have been fetched, let alone cached.
    auto bob_it =
        cache.find<OwnerAttributeTag>(boost::make_tuple(std::string("composite_bob"), std::string("email")));
    EXPECT_EQ(bob_it, cache.end<OwnerAttributeTag>());

    nats_->erase(kBucket, alice_email);
    nats_->erase(kBucket, alice_phone);
    nats_->erase(kBucket, bob_email);
}

// --- validation: a field value that would corrupt the hierarchy ----------

TEST_F(CompositeKeyNatsTest, KeyDerivationRejectsAFieldContainingADotBeforeTouchingNats) {
    // A literal '.' inside a field value would silently misparse as an
    // extra subject token -- OwnerAttributeKey::key() must refuse to build
    // a key like this at all, not send something to NATS that *looks*
    // fine but matches the wrong things later.
    EXPECT_THROW(OwnerAttributeKey::key("alice.smith", "email"), std::invalid_argument);
}

}  // namespace
}  // namespace poc
