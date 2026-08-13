/// Shared service/type definitions for the iceoryx2 request-response POC.
///
/// Two independent multi_index_lru::ExpirableContainer instances -- NameCache
/// and IdCache -- served over one iceoryx2 request-response service. They're
/// intentionally unrelated *to each other* (not two indices over the same
/// entries): this exists to demo/test both a string-keyed and an
/// integral-keyed multi_index_lru::ExpirableContainer side by side, each
/// holding an opaque byte blob per entry (stand-in for a real
/// zerialize-produced flexbuffer/msgpack record -- see
/// example/zerialize_cache.cpp for how that serialization step actually
/// works) that expires after a configurable TTL on top of the usual
/// LRU-capacity eviction (see --ttl-ms/MIL_TTL_MS in
/// server.cpp/server_readthrough.cpp). NameCache does internally have two
/// indices over its own entries, though -- see "Non-unique key lookup
/// (GetAll)" below.
///
/// server.cpp answers requests purely from these in-memory caches. See
/// server_readthrough.cpp for the variant that falls through to a NATS
/// JetStream KV bucket per cache on a local miss.
///
/// --- Non-unique key lookup (GetAll) --------------------------------------
///
/// NameCache's primary key (`NameTag`, hashed_unique) addresses one
/// NameEntry at a time, same as IdCache's. `CategoryTag` is a second,
/// hashed_non_unique index over the same NameEntry data: multiple entries
/// can share a category, and GetAll (KeyKind::Category) returns every one of
/// them in a single response, not just the first (see wire::Op::GetAll's
/// request/response shape below, and handle_request_local()'s GetAll case
/// for how it's answered). boost::multi_index keeps both indices in sync
/// automatically on every insert/erase/upsert -- Put's existing
/// erase-then-emplace upsert (see handle_request_local()'s Put case) needs
/// no special handling to move an entry from one category to another;
/// erasing by the primary key removes it from the category index too. This
/// flavor is local-cache-only: server_readthrough.cpp doesn't implement it
/// (NATS KV has no secondary-index concept to fall through to on a miss).
///
/// --- Prefix lookup (GetAll, KeyKind::Name) --------------------------------
///
/// A second, unrelated GetAll flavor: KeyKind::Name treats the request
/// string as a NATS key *prefix* rather than a category, and IS NATS-backed
/// (server_readthrough.cpp only -- server.cpp rejects it, since a plain
/// in-memory cache has no prefix concept either). This exists for a primary
/// key that isn't really 1:1 with what's stored in NATS -- e.g. "alice"
/// addressing several NATS keys ("alice.email", "alice.phone", ...) rather
/// than one -- and is answered by asking NATS itself what exists under the
/// prefix (nats_asio::kv_keys with a wildcard pattern) rather than via any
/// local index, since only NATS can authoritatively answer "what keys exist
/// under this prefix right now." See NatsBridge::list_and_get_async() and
/// server_dispatch.hpp's GetAll/KeyKind::Name case.
#pragma once

#include "security_cache.hpp"
#include "wire.hpp"

#include <multi_index_lru/expirable_container.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace poc {

// `found`: true for a real cached record, false for a negative-cache entry
// (server_dispatch.hpp -- "this key is confirmed absent from NATS as of the
// last check", not "never looked up"). `record` is empty and meaningless
// when found is false. Defaults to true so every existing 2-arg
// NameEntry{key, record} construction (server.cpp's seed data,
// handle_request_local()'s plain in-memory upsert, which never negatively
// caches) is unaffected.
//
// `category`: a second, non-unique key alongside the (unique) primary `key`
// -- see "Non-unique key lookup (GetAll)" below. Defaults to "" (meaning
// "uncategorized", not "never set") so every existing NameEntry{...}
// construction that doesn't mention it is unaffected. Placed last so it
// doesn't disturb the positional meaning of any existing 2- or 3-arg
// construction.
struct NameEntry {
    std::string key;
    std::vector<std::uint8_t> record;
    bool found = true;
    std::string category;
};

struct IdEntry {
    std::int64_t key;
    std::vector<std::uint8_t> record;
    bool found = true;
};

struct NameTag {};
struct IdTag {};
struct CategoryTag {};

// ExpirableContainer, not plain Container: entries carry a TTL (see
// --ttl-ms/MIL_TTL_MS in server.cpp/server_readthrough.cpp) on top of the
// LRU-capacity eviction both caches already had. find() slides the
// expiration forward on every hit, same as it already refreshes LRU
// recency, so a key under active use never expires out from under it.
//
// Two indices over the same NameEntry data: `NameTag` (hashed_unique, the
// primary key -- get/put/erase all address a NameEntry by this) and
// `CategoryTag` (hashed_non_unique -- GetAll below returns every NameEntry
// sharing a category). This is boost::multi_index's actual point: one
// container, multiple simultaneous ways to look the same data up, kept in
// sync automatically by the library on every insert/erase/upsert. IdCache
// has no equivalent second index -- this demonstrates the capability once,
// on the cache where it reads most naturally, rather than duplicating it.
using NameCache = multi_index_lru::ExpirableContainer<
    NameEntry,
    boost::multi_index::indexed_by<
        boost::multi_index::hashed_unique<
            boost::multi_index::tag<NameTag>,
            boost::multi_index::member<NameEntry, std::string, &NameEntry::key>>,
        boost::multi_index::hashed_non_unique<
            boost::multi_index::tag<CategoryTag>,
            boost::multi_index::member<NameEntry, std::string, &NameEntry::category>>>>;

using IdCache = multi_index_lru::ExpirableContainer<
    IdEntry,
    boost::multi_index::indexed_by<
        boost::multi_index::hashed_unique<
            boost::multi_index::tag<IdTag>,
            boost::multi_index::member<IdEntry, std::int64_t, &IdEntry::key>>>>;

// An ExpirableContainer only evicts an expired entry when something touches
// it (find()/erase()) or when cleanup_expired() is called explicitly -- an
// entry that's written once and never read again would otherwise sit in the
// cache, occupying an LRU-capacity slot, until eviction pressure from other
// inserts got around to it. Both servers' main loops call this once per
// `interval` (not every single iteration -- cleanup_expired()'s cost is
// proportional to how many *consecutive* least-recently-used entries are
// actually expired, which is cheap, but there's no reason to pay it more
// than a few times a second).
inline void cleanup_expired_periodically(NameCache& name_cache, IdCache& id_cache, SecurityCache& security_cache,
                                          std::chrono::steady_clock::time_point& last_cleanup,
                                          std::chrono::steady_clock::duration interval) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_cleanup < interval) {
        return;
    }
    name_cache.cleanup_expired();
    id_cache.cleanup_expired();
    security_cache.cleanup_expired();
    last_cleanup = now;
}

// Every client and server must agree on this name to find each other.
inline constexpr auto kServiceName = "multi_index_lru/cache/rpc";

// --- request encoding (client side) -------------------------------------
//
// Wire format:
//   Get/Erase:  [op:u8][key_kind:u8][ Name: u32-prefixed-str | Id: i64 ]
//   Put:        [op:u8][key_kind:u8][ Name: u32-prefixed-str + u32-prefixed
//                category-str | Id: i64 ][record_len:u32][record bytes]
//               -- the category field only appears for KeyKind::Name (see
//               "Non-unique key lookup (GetAll)" below); Id-keyed Puts are
//               unchanged.
//   GetAll (category):  [op:u8][key_kind:u8=Category][category:u32-prefixed-str]
//   GetAll (prefix):    [op:u8][key_kind:u8=Name][prefix:u32-prefixed-str]
//
// --- Security-keyed cache (security_cache.hpp's 5-field composite key) ---
//
// A third, independent cache alongside NameCache/IdCache -- see
// security_cache.hpp for SecurityEntry/SecurityKey (ASSET_TYPE, CUSIP,
// ISIN, SEDOL, RIC) and why a "wide" combined key needs
// NatsCompositeKey::pattern() rather than prefix_pattern() for its find.
//
//   Get/Erase (Security): [op:u8][key_kind:u8=Security][asset_type:str]
//             [cusip:str][isin:str][sedol:str][ric:str]
//   Put (Security):       [op:u8][key_kind:u8=Security][asset_type:str]
//             [cusip:str][isin:str][sedol:str][ric:str][record_len:u32]
//             [record bytes]
//   GetAll (Security, pattern find): [op:u8][key_kind:u8=Security]
//             ([is_wildcard:u8][value:str]) x 5, one pair per field in
//             SecurityKey's (asset_type, cusip, isin, sedol, ric) order --
//             is_wildcard=1 means that position is nats_any (value is
//             empty and ignored), is_wildcard=0 means value is that
//             position's literal. This is the wire-level mirror of
//             SecurityKey::pattern()'s std::variant<AllFields, NatsAny>
//             arguments -- see server_dispatch_read.hpp's Security GetAll
//             branch, which decodes exactly this shape to build the actual
//             pattern() call. NATS-backed only (server_readthrough.cpp/
//             server_readthrough_ecal.cpp); server.cpp (no NATS) rejects
//             it, same as GetAll(prefix) already does for KeyKind::Name.
//
// Response:
//   Get/GetAll miss, Erase, Put: [status:u8] -- Ok, NotFound, or Error.
//   Get+Ok:   [status:u8=Ok][record bytes]
//   GetAll(category)+Ok (at least one match): [status:u8=Ok][count:u32]
//             [truncated:u8][record_len:u32][record bytes] x count --
//             truncated is 1 iff more than --max-getall-results/
//             MIL_MAX_GETALL_RESULTS records matched and the rest were left
//             out, 0 otherwise.
//   GetAll(prefix)+Ok (at least one match): [status:u8=Ok][count:u32]
//             [truncated:u8]([key:u32-prefixed-str][record_len:u32][record
//             bytes]) x count -- same truncation meaning as above; carries
//             each match's full NATS key (e.g. "alice.email") alongside its
//             record, unlike the category form, since the whole point of a
//             prefix lookup is discovering what keys exist underneath it.

inline std::vector<std::uint8_t> encode_get(wire::KeyKind kind, const std::string& name, std::int64_t id) {
    wire::Writer w;
    w.u8(static_cast<std::uint8_t>(wire::Op::Get));
    w.u8(static_cast<std::uint8_t>(kind));
    if (kind == wire::KeyKind::Name) {
        w.str(name);
    } else {
        w.i64(id);
    }
    return w.buffer();
}

inline std::vector<std::uint8_t> encode_erase(wire::KeyKind kind, const std::string& name, std::int64_t id) {
    wire::Writer w;
    w.u8(static_cast<std::uint8_t>(wire::Op::Erase));
    w.u8(static_cast<std::uint8_t>(kind));
    if (kind == wire::KeyKind::Name) {
        w.str(name);
    } else {
        w.i64(id);
    }
    return w.buffer();
}

inline std::vector<std::uint8_t> encode_put(
    wire::KeyKind kind, const std::string& name, std::int64_t id, const std::vector<std::uint8_t>& record,
    const std::string& category = "") {
    wire::Writer w;
    w.u8(static_cast<std::uint8_t>(wire::Op::Put));
    w.u8(static_cast<std::uint8_t>(kind));
    if (kind == wire::KeyKind::Name) {
        w.str(name);
        w.str(category);
    } else {
        w.i64(id);
    }
    w.u32(static_cast<std::uint32_t>(record.size()));
    w.bytes(record);
    return w.buffer();
}

inline std::vector<std::uint8_t> encode_get_all(const std::string& category) {
    wire::Writer w;
    w.u8(static_cast<std::uint8_t>(wire::Op::GetAll));
    w.u8(static_cast<std::uint8_t>(wire::KeyKind::Category));
    w.str(category);
    return w.buffer();
}

// See "Prefix lookup (GetAll, KeyKind::Name)" above -- server.cpp (local,
// no NATS) rejects this; only server_readthrough.cpp answers it.
inline std::vector<std::uint8_t> encode_get_all_by_prefix(const std::string& name_prefix) {
    wire::Writer w;
    w.u8(static_cast<std::uint8_t>(wire::Op::GetAll));
    w.u8(static_cast<std::uint8_t>(wire::KeyKind::Name));
    w.str(name_prefix);
    return w.buffer();
}

// --- Security-keyed cache request encoding --------------------------------

inline std::vector<std::uint8_t> encode_get_security(const std::string& asset_type, const std::string& cusip,
                                                       const std::string& isin, const std::string& sedol,
                                                       const std::string& ric) {
    wire::Writer w;
    w.u8(static_cast<std::uint8_t>(wire::Op::Get));
    w.u8(static_cast<std::uint8_t>(wire::KeyKind::Security));
    w.str(asset_type);
    w.str(cusip);
    w.str(isin);
    w.str(sedol);
    w.str(ric);
    return w.buffer();
}

inline std::vector<std::uint8_t> encode_erase_security(const std::string& asset_type, const std::string& cusip,
                                                         const std::string& isin, const std::string& sedol,
                                                         const std::string& ric) {
    wire::Writer w;
    w.u8(static_cast<std::uint8_t>(wire::Op::Erase));
    w.u8(static_cast<std::uint8_t>(wire::KeyKind::Security));
    w.str(asset_type);
    w.str(cusip);
    w.str(isin);
    w.str(sedol);
    w.str(ric);
    return w.buffer();
}

inline std::vector<std::uint8_t> encode_put_security(const std::string& asset_type, const std::string& cusip,
                                                       const std::string& isin, const std::string& sedol,
                                                       const std::string& ric,
                                                       const std::vector<std::uint8_t>& record) {
    wire::Writer w;
    w.u8(static_cast<std::uint8_t>(wire::Op::Put));
    w.u8(static_cast<std::uint8_t>(wire::KeyKind::Security));
    w.str(asset_type);
    w.str(cusip);
    w.str(isin);
    w.str(sedol);
    w.str(ric);
    w.u32(static_cast<std::uint32_t>(record.size()));
    w.bytes(record);
    return w.buffer();
}

// A field left as std::nullopt is wildcarded (SecurityKey::pattern()'s
// nats_any at that position); a field with a value is that position's
// literal -- see "GetAll (Security, pattern find)" above for the wire
// shape this produces.
inline std::vector<std::uint8_t> encode_find_security(const std::optional<std::string>& asset_type,
                                                        const std::optional<std::string>& cusip,
                                                        const std::optional<std::string>& isin,
                                                        const std::optional<std::string>& sedol,
                                                        const std::optional<std::string>& ric) {
    wire::Writer w;
    w.u8(static_cast<std::uint8_t>(wire::Op::GetAll));
    w.u8(static_cast<std::uint8_t>(wire::KeyKind::Security));
    auto write_field = [&w](const std::optional<std::string>& field) {
        w.u8(field.has_value() ? 0 : 1);
        w.str(field.value_or(""));
    };
    write_field(asset_type);
    write_field(cusip);
    write_field(isin);
    write_field(sedol);
    write_field(ric);
    return w.buffer();
}

// --- response encoding (server side) -------------------------------------

inline std::vector<std::uint8_t> encode_status(wire::Status status) {
    wire::Writer w;
    w.u8(static_cast<std::uint8_t>(status));
    return w.buffer();
}

inline std::vector<std::uint8_t> encode_found(const std::vector<std::uint8_t>& record) {
    wire::Writer w;
    w.u8(static_cast<std::uint8_t>(wire::Status::Ok));
    w.bytes(record);
    return w.buffer();
}

// `truncated`: true iff `records` is only the first --max-getall-results of
// a larger match set (see handle_request_local()'s GetAll case) -- callers
// that care whether they got everything check this rather than assuming
// `records.size()` is the true total.
inline std::vector<std::uint8_t> encode_found_all(const std::vector<std::vector<std::uint8_t>>& records,
                                                    bool truncated) {
    wire::Writer w;
    w.u8(static_cast<std::uint8_t>(wire::Status::Ok));
    w.u32(static_cast<std::uint32_t>(records.size()));
    w.u8(truncated ? 1 : 0);
    for (const auto& record : records) {
        w.u32(static_cast<std::uint32_t>(record.size()));
        w.bytes(record);
    }
    return w.buffer();
}

// Same shape as encode_found_all(), plus each match's own full NATS key --
// see "GetAll(prefix)+Ok" in the wire format comment above for why the
// prefix flavor needs that and the category flavor doesn't.
inline std::vector<std::uint8_t> encode_found_all_with_keys(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries, bool truncated) {
    wire::Writer w;
    w.u8(static_cast<std::uint8_t>(wire::Status::Ok));
    w.u32(static_cast<std::uint32_t>(entries.size()));
    w.u8(truncated ? 1 : 0);
    for (const auto& [key, record] : entries) {
        w.str(key);
        w.u32(static_cast<std::uint32_t>(record.size()));
        w.bytes(record);
    }
    return w.buffer();
}

// --- purely local (no backing store) request handling ---------------------

/// Applies one decoded request to the two in-memory caches and returns the
/// encoded response. Used by server.cpp (see server_readthrough.cpp for the
/// NATS-backed variant, which reimplements this with a fetch-on-miss /
/// write-through step around the same caches -- GetAll is not implemented
/// there; see README.md "What this doesn't answer yet" for why).
///
/// `max_getall_results` caps how many records a single GetAll returns (see
/// --max-getall-results/MIL_MAX_GETALL_RESULTS in server.cpp) -- without a
/// cap, a popular category could make the response arbitrarily large, unlike
/// every other response this function returns, which is bounded by one
/// record.
inline std::vector<std::uint8_t> handle_request_local(
    NameCache& name_cache, IdCache& id_cache, SecurityCache& security_cache, const std::uint8_t* data,
    std::size_t size, std::size_t max_getall_results = 100) {
    // A malformed/truncated request (wire::Reader throws std::out_of_range on
    // that) must never escape this function: this is called from the
    // server's single request loop, and an unhandled exception there would
    // take the whole daemon down for every other client over one bad
    // message.
    try {
        wire::Reader r(data, size);
        const auto op = static_cast<wire::Op>(r.u8());
        const auto kind = static_cast<wire::KeyKind>(r.u8());

        switch (op) {
            case wire::Op::Get: {
                if (kind == wire::KeyKind::Security) {
                    auto asset_type = r.str();
                    auto cusip = r.str();
                    auto isin = r.str();
                    auto sedol = r.str();
                    auto ric = r.str();
                    auto it = security_cache.find<SecurityKeyTag>(
                        boost::make_tuple(asset_type, cusip, isin, sedol, ric));
                    return (it != security_cache.end<SecurityKeyTag>()) ? encode_found(it->record)
                                                                          : encode_status(wire::Status::NotFound);
                }
                if (kind == wire::KeyKind::Name) {
                    auto it = name_cache.find<NameTag>(r.str());
                    return (it != name_cache.end<NameTag>()) ? encode_found(it->record)
                                                               : encode_status(wire::Status::NotFound);
                }
                auto it = id_cache.find<IdTag>(r.i64());
                return (it != id_cache.end<IdTag>()) ? encode_found(it->record)
                                                       : encode_status(wire::Status::NotFound);
            }
            case wire::Op::Erase: {
                if (kind == wire::KeyKind::Security) {
                    auto asset_type = r.str();
                    auto cusip = r.str();
                    auto isin = r.str();
                    auto sedol = r.str();
                    auto ric = r.str();
                    const bool erased = security_cache.erase<SecurityKeyTag>(
                        boost::make_tuple(asset_type, cusip, isin, sedol, ric));
                    return encode_status(erased ? wire::Status::Ok : wire::Status::NotFound);
                }
                const bool erased = (kind == wire::KeyKind::Name) ? name_cache.erase<NameTag>(r.str())
                                                                    : id_cache.erase<IdTag>(r.i64());
                return encode_status(erased ? wire::Status::Ok : wire::Status::NotFound);
            }
            case wire::Op::Put: {
                if (kind == wire::KeyKind::Security) {
                    auto asset_type = r.str();
                    auto cusip = r.str();
                    auto isin = r.str();
                    auto sedol = r.str();
                    auto ric = r.str();
                    auto record = r.bytes(r.u32());
                    // Upsert -- same reasoning as the Name branch below.
                    security_cache.erase<SecurityKeyTag>(boost::make_tuple(asset_type, cusip, isin, sedol, ric));
                    security_cache.emplace(SecurityEntry{std::move(asset_type), std::move(cusip), std::move(isin),
                                                           std::move(sedol), std::move(ric), std::move(record)});
                } else if (kind == wire::KeyKind::Name) {
                    auto name = r.str();
                    auto category = r.str();
                    auto record = r.bytes(r.u32());
                    // Upsert: a plain emplace() only inserts-if-absent and
                    // won't overwrite an existing value on a duplicate key,
                    // so drop whatever's there first -- erasing by NameTag
                    // (the primary key) also removes the entry from the
                    // CategoryTag index automatically, so a re-Put under a
                    // *different* category can never leave a stale entry
                    // behind in the old one.
                    name_cache.erase<NameTag>(name);
                    name_cache.emplace(NameEntry{std::move(name), std::move(record), true, std::move(category)});
                } else {
                    auto id = r.i64();
                    auto record = r.bytes(r.u32());
                    id_cache.erase<IdTag>(id);
                    id_cache.emplace(IdEntry{id, std::move(record)});
                }
                return encode_status(wire::Status::Ok);
            }
            case wire::Op::GetAll: {
                if (kind == wire::KeyKind::Security) {
                    // A pattern() find needs NATS's kv_keys() to discover
                    // what matches -- this purely in-memory handler has no
                    // NATS to ask, same as the Name-prefix flavor below.
                    return encode_status(wire::Status::Error);
                }
                if (kind != wire::KeyKind::Category) {
                    // KeyKind::Name (prefix lookup) is NATS-backed only --
                    // see "Prefix lookup (GetAll, KeyKind::Name)" above.
                    // This purely in-memory handler has no NATS to ask.
                    return encode_status(wire::Status::Error);
                }
                auto category = r.str();
                auto [begin, end] = name_cache.equal_range<CategoryTag>(category);
                std::vector<std::vector<std::uint8_t>> records;
                bool truncated = false;
                for (auto it = begin; it != end; ++it) {
                    if (records.size() >= max_getall_results) {
                        truncated = true;
                        break;
                    }
                    records.push_back(it->record);
                }
                return records.empty() ? encode_status(wire::Status::NotFound)
                                        : encode_found_all(records, truncated);
            }
        }
        return encode_status(wire::Status::NotFound);
    } catch (const std::exception&) {
        return encode_status(wire::Status::Error);
    } catch (...) {
        return encode_status(wire::Status::Error);
    }
}

}  // namespace poc
