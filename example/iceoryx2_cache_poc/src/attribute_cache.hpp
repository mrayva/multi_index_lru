/// Worked example: a *genuine* composite multi_index key (not a single flat
/// string like NameCache's) mapped onto NATS JetStream KV via
/// multi_index_lru::NatsCompositeKey (nats_key.hpp) instead of hand-typed
/// string concatenation.
///
/// AttributeEntry models one attribute of one owner -- e.g. owner="alice",
/// attribute="email" -- addressed two ways:
///   - uniquely, by the full (owner, attribute) pair (OwnerAttributeTag,
///     hashed_unique, a boost::multi_index::composite_key over both
///     members) -- one entry, one exact NATS key.
///   - by owner alone, a strict PREFIX of that same field order -- not a
///     second local index (unlike NameCache's CategoryTag: there's no
///     "every attribute of this owner" secondary index here at all), but a
///     NATS-side wildcard pattern query, same as server_dispatch.hpp's
///     GetAll(prefix) -- NATS is the only thing that can authoritatively
///     answer "what attributes does this owner have," so there's nothing
///     for a local index to add over just asking it directly.
///
/// OwnerAttributeKey (a 2-field multi_index_lru::NatsCompositeKey) derives
/// both encodings from the *same* field order:
///   OwnerAttributeKey::key(owner, attribute)     -> "owner.attribute"
///   OwnerAttributeKey::prefix_pattern(owner)      -> "owner.*"
/// (SingleToken is picked automatically: 2 total fields - 1 given = exactly
/// 1 remaining -- see nats_key.hpp's NatsCompositeKey::prefix_pattern().)
///
/// See test/composite_key_nats_test.cpp for this wired up end to end
/// against a real NATS server: unique put/get by composite key, and a
/// prefix fetch (kv_keys with the derived pattern, then kv_get each match)
/// that returns exactly one owner's attributes, none of another's.
#pragma once

#include <multi_index_lru/container.hpp>
#include <multi_index_lru/nats_key.hpp>

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace poc {

struct AttributeEntry {
    std::string owner;
    std::string attribute;
    std::vector<std::uint8_t> record;
};

struct OwnerAttributeTag {};

using AttributeCache = multi_index_lru::Container<
    AttributeEntry,
    boost::multi_index::indexed_by<
        boost::multi_index::hashed_unique<
            boost::multi_index::tag<OwnerAttributeTag>,
            boost::multi_index::composite_key<
                AttributeEntry,
                boost::multi_index::member<AttributeEntry, std::string, &AttributeEntry::owner>,
                boost::multi_index::member<AttributeEntry, std::string, &AttributeEntry::attribute>>>>>;

// The single source of truth for how (owner, attribute) maps onto NATS --
// both the exact-key and prefix-pattern forms come from this one
// declaration, so they can't independently drift out of sync.
using OwnerAttributeKey = multi_index_lru::NatsCompositeKey<std::string, std::string>;

}  // namespace poc
