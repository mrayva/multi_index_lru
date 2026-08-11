/// Tests for multi_index_lru::nats_key/nats_pattern/NatsCompositeKey
/// (nats_key.hpp) -- pure string derivation, no NATS connection needed.

#include <multi_index_lru/nats_key.hpp>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace multi_index_lru {
namespace {

// --- nats_key -----------------------------------------------------------

TEST(NatsKey, SingleFieldIsUsedVerbatim) {
    EXPECT_EQ(nats_key("alice"), "alice");
}

TEST(NatsKey, MultipleFieldsAreDotJoinedInOrder) {
    EXPECT_EQ(nats_key("alice", "email"), "alice.email");
    EXPECT_EQ(nats_key("alice", "friends", "email"), "alice.friends.email");
}

TEST(NatsKey, ThrowsOnEmptyField) {
    EXPECT_THROW(nats_key("alice", ""), std::invalid_argument);
}

TEST(NatsKey, ThrowsOnFieldContainingDot) {
    EXPECT_THROW(nats_key("al.ice", "email"), std::invalid_argument);
}

TEST(NatsKey, ThrowsOnFieldContainingWildcardCharacters) {
    EXPECT_THROW(nats_key("alice*", "email"), std::invalid_argument);
    EXPECT_THROW(nats_key("alice>", "email"), std::invalid_argument);
}

// --- nats_pattern ---------------------------------------------------------

TEST(NatsPattern, SingleTokenAppendsStarSuffix) {
    EXPECT_EQ(nats_pattern(NatsWildcard::SingleToken, "alice"), "alice.*");
}

TEST(NatsPattern, MultiTokenAppendsGreaterThanSuffix) {
    EXPECT_EQ(nats_pattern(NatsWildcard::MultiToken, "alice"), "alice.>");
}

TEST(NatsPattern, MultipleFieldsBeforeWildcardAreDotJoined) {
    EXPECT_EQ(nats_pattern(NatsWildcard::SingleToken, "alice", "friends"), "alice.friends.*");
}

TEST(NatsPattern, ThrowsOnInvalidFieldSameAsNatsKey) {
    EXPECT_THROW(nats_pattern(NatsWildcard::SingleToken, "al.ice"), std::invalid_argument);
}

// --- NatsCompositeKey -------------------------------------------------------

TEST(NatsCompositeKey, TwoFieldKeyExactLookup) {
    using OwnerAttributeKey = NatsCompositeKey<std::string, std::string>;
    EXPECT_EQ(OwnerAttributeKey::field_count, 2u);
    EXPECT_EQ(OwnerAttributeKey::key("alice", "email"), "alice.email");
}

TEST(NatsCompositeKey, TwoFieldKeyOneFieldPrefixPicksSingleTokenWildcard) {
    // 2 total fields, 1 given -> exactly 1 remaining -> '*'.
    using OwnerAttributeKey = NatsCompositeKey<std::string, std::string>;
    EXPECT_EQ(OwnerAttributeKey::prefix_pattern("alice"), "alice.*");
}

TEST(NatsCompositeKey, ThreeFieldKeyExactLookup) {
    using OwnerCategoryAttributeKey = NatsCompositeKey<std::string, std::string, std::string>;
    EXPECT_EQ(OwnerCategoryAttributeKey::key("alice", "friends", "email"), "alice.friends.email");
}

TEST(NatsCompositeKey, ThreeFieldKeyOneFieldPrefixPicksMultiTokenWildcard) {
    // 3 total fields, 1 given -> 2 remaining (more than one) -> '>'.
    using OwnerCategoryAttributeKey = NatsCompositeKey<std::string, std::string, std::string>;
    EXPECT_EQ(OwnerCategoryAttributeKey::prefix_pattern("alice"), "alice.>");
}

TEST(NatsCompositeKey, ThreeFieldKeyTwoFieldPrefixPicksSingleTokenWildcard) {
    // 3 total fields, 2 given -> exactly 1 remaining -> '*'.
    using OwnerCategoryAttributeKey = NatsCompositeKey<std::string, std::string, std::string>;
    EXPECT_EQ(OwnerCategoryAttributeKey::prefix_pattern("alice", "friends"), "alice.friends.*");
}

TEST(NatsCompositeKey, PatternWithAllFieldsLiteralMatchesKey) {
    using ThreeFieldKey = NatsCompositeKey<std::string, std::string, std::string>;
    EXPECT_EQ(ThreeFieldKey::pattern("alice", "friends", "email"), "alice.friends.email");
}

TEST(NatsCompositeKey, PatternWildcardsATrailingRunOneStarPerPosition) {
    using ThreeFieldKey = NatsCompositeKey<std::string, std::string, std::string>;
    EXPECT_EQ(ThreeFieldKey::pattern("alice", nats_any, nats_any), "alice.*.*");
}

TEST(NatsCompositeKey, PatternWildcardsALeadingPosition) {
    using ThreeFieldKey = NatsCompositeKey<std::string, std::string, std::string>;
    EXPECT_EQ(ThreeFieldKey::pattern(nats_any, "friends", "email"), "*.friends.email");
}

TEST(NatsCompositeKey, PatternWildcardsAnInteriorPositionNotAdjacentToTheLeadingField) {
    // The case prefix_pattern() can't express: the known fields (position 0
    // and 2) aren't a leading prefix -- position 1 is wildcarded in between.
    using FiveFieldKey = NatsCompositeKey<std::string, std::string, std::string, std::string, std::string>;
    EXPECT_EQ(FiveFieldKey::pattern("EQUITY", nats_any, "US1234567", nats_any, nats_any), "EQUITY.*.US1234567.*.*");
}

TEST(NatsCompositeKey, PatternThrowsOnInvalidLiteralFieldSameAsNatsKey) {
    using ThreeFieldKey = NatsCompositeKey<std::string, std::string, std::string>;
    EXPECT_THROW(ThreeFieldKey::pattern("al.ice", nats_any, nats_any), std::invalid_argument);
}

}  // namespace
}  // namespace multi_index_lru
