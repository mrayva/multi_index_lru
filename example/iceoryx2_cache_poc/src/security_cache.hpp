/// Worked example: multi_index_lru::NatsCompositeKey::pattern() (nats_key.hpp)
/// wildcarding an INTERIOR field position -- attribute_cache.hpp's
/// OwnerAttributeKey only demonstrates prefix_pattern(), which can wildcard
/// a trailing run of fields but nothing else.
///
/// SecurityEntry models one row of a securities reference-data table:
/// ASSET_TYPE, CUSIP, ISIN, SEDOL, RIC (plus the rest of the record).
/// CUSIP/ISIN/SEDOL/RIC are each unique only in combination with
/// ASSET_TYPE, not on their own, and the common query supplies ASSET_TYPE
/// plus exactly ONE of the four identifiers -- leaving the other three
/// unknown, and not necessarily as a trailing run: querying by
/// ASSET_TYPE+ISIN with the field order below wildcards CUSIP (position 1)
/// *and* SEDOL/RIC (positions 3-4), with ISIN (the known field) sitting
/// between them.
///
/// The "wide"/combined single-key design this is a worked example of (as
/// opposed to one duplicate key per identifier, which trades write
/// amplification for a single-hop lookup on every identifier): one NATS
/// key per row, holding the full record, with a fixed field order:
///   SecurityKey::key(asset_type, cusip, isin, sedol, ric) -> exact NATS key
///
/// A query that knows ASSET_TYPE and ISIN, but not CUSIP/SEDOL/RIC, can't
/// use prefix_pattern() -- ISIN isn't a leading prefix of the field list --
/// so it needs pattern() to wildcard the positions in between and after:
///   SecurityKey::pattern(asset_type, nats_any, isin, nats_any, nats_any)
///   -> "asset_type.*.isin.*.*"
/// That's a kv_keys() list (one pruned lookup, not a scan of the whole
/// bucket -- the first TWO tokens being literal in the *common*
/// ASSET_TYPE+CUSIP case prunes straight to one match; ASSET_TYPE+ISIN
/// still prunes on ASSET_TYPE, just not down to a single token) followed by
/// a kv_get on whatever matched -- still two NATS round trips, same as any
/// other non-exact query; pattern() only saves you from hand-typing the
/// pattern string and getting the field order or wildcard count wrong.
///
/// See test/security_key_nats_test.cpp for this wired up end to end
/// against a real NATS server.
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

struct SecurityEntry {
    std::string asset_type;
    std::string cusip;
    std::string isin;
    std::string sedol;
    std::string ric;
    std::vector<std::uint8_t> record;
};

struct SecurityKeyTag {};

using SecurityCache = multi_index_lru::Container<
    SecurityEntry,
    boost::multi_index::indexed_by<boost::multi_index::hashed_unique<
        boost::multi_index::tag<SecurityKeyTag>,
        boost::multi_index::composite_key<
            SecurityEntry, boost::multi_index::member<SecurityEntry, std::string, &SecurityEntry::asset_type>,
            boost::multi_index::member<SecurityEntry, std::string, &SecurityEntry::cusip>,
            boost::multi_index::member<SecurityEntry, std::string, &SecurityEntry::isin>,
            boost::multi_index::member<SecurityEntry, std::string, &SecurityEntry::sedol>,
            boost::multi_index::member<SecurityEntry, std::string, &SecurityEntry::ric>>>>>;

// The single source of truth for how (asset_type, cusip, isin, sedol, ric)
// maps onto NATS -- the exact-key form and every pattern() query derive
// from this one field order, so they can't independently drift out of
// sync.
using SecurityKey =
    multi_index_lru::NatsCompositeKey<std::string, std::string, std::string, std::string, std::string>;

}  // namespace poc
