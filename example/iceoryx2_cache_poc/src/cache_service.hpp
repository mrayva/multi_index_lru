/// Shared service/type definitions for the iceoryx2 request-response POC.
///
/// Two independent multi_index_lru::ExpirableContainer instances, each
/// single-keyed -- NameCache by a string, IdCache by an int64_t -- served
/// over one iceoryx2 request-response service. They're intentionally
/// unrelated caches (not two indices over the same entries): this exists to
/// demo/test both a string-keyed and an integral-keyed
/// multi_index_lru::ExpirableContainer side by side, each holding an opaque
/// byte blob per entry (stand-in for a real zerialize-produced
/// flexbuffer/msgpack record -- see example/zerialize_cache.cpp for how
/// that serialization step actually works) that expires after a
/// configurable TTL on top of the usual LRU-capacity eviction (see
/// --ttl-ms/MIL_TTL_MS in server.cpp/server_readthrough.cpp).
///
/// server.cpp answers requests purely from these in-memory caches. See
/// server_readthrough.cpp for the variant that falls through to a NATS
/// JetStream KV bucket per cache on a local miss.
#pragma once

#include "wire.hpp"

#include <multi_index_lru/expirable_container.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace poc {

struct NameEntry {
    std::string key;
    std::vector<std::uint8_t> record;
};

struct IdEntry {
    std::int64_t key;
    std::vector<std::uint8_t> record;
};

struct NameTag {};
struct IdTag {};

// ExpirableContainer, not plain Container: entries carry a TTL (see
// --ttl-ms/MIL_TTL_MS in server.cpp/server_readthrough.cpp) on top of the
// LRU-capacity eviction both caches already had. find() slides the
// expiration forward on every hit, same as it already refreshes LRU
// recency, so a key under active use never expires out from under it.
using NameCache = multi_index_lru::ExpirableContainer<
    NameEntry,
    boost::multi_index::indexed_by<
        boost::multi_index::hashed_unique<
            boost::multi_index::tag<NameTag>,
            boost::multi_index::member<NameEntry, std::string, &NameEntry::key>>>>;

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
inline void cleanup_expired_periodically(NameCache& name_cache, IdCache& id_cache,
                                          std::chrono::steady_clock::time_point& last_cleanup,
                                          std::chrono::steady_clock::duration interval) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_cleanup < interval) {
        return;
    }
    name_cache.cleanup_expired();
    id_cache.cleanup_expired();
    last_cleanup = now;
}

// Every client and server must agree on this name to find each other.
inline constexpr auto kServiceName = "multi_index_lru/cache/rpc";

// --- request encoding (client side) -------------------------------------
//
// Wire format:
//   Get/Erase:  [op:u8][key_kind:u8][ Name: u32-prefixed-str | Id: i64 ]
//   Put:        [op:u8][key_kind:u8][ Name: u32-prefixed-str | Id: i64 ][record_len:u32][record bytes]
//
// Response:
//   [status:u8][record bytes if Get+Ok] -- status is Ok, NotFound, or Error.

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
    wire::KeyKind kind, const std::string& name, std::int64_t id, const std::vector<std::uint8_t>& record) {
    wire::Writer w;
    w.u8(static_cast<std::uint8_t>(wire::Op::Put));
    w.u8(static_cast<std::uint8_t>(kind));
    if (kind == wire::KeyKind::Name) {
        w.str(name);
    } else {
        w.i64(id);
    }
    w.u32(static_cast<std::uint32_t>(record.size()));
    w.bytes(record);
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

// --- purely local (no backing store) request handling ---------------------

/// Applies one decoded request to the two in-memory caches and returns the
/// encoded response. Used by server.cpp (see server_readthrough.cpp for the
/// NATS-backed variant, which reimplements this with a fetch-on-miss /
/// write-through step around the same caches).
inline std::vector<std::uint8_t> handle_request_local(
    NameCache& name_cache, IdCache& id_cache, const std::uint8_t* data, std::size_t size) {
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
                const bool erased = (kind == wire::KeyKind::Name) ? name_cache.erase<NameTag>(r.str())
                                                                    : id_cache.erase<IdTag>(r.i64());
                return encode_status(erased ? wire::Status::Ok : wire::Status::NotFound);
            }
            case wire::Op::Put: {
                if (kind == wire::KeyKind::Name) {
                    auto name = r.str();
                    auto record = r.bytes(r.u32());
                    // Upsert: a plain emplace() only inserts-if-absent and
                    // won't overwrite an existing value on a duplicate key,
                    // so drop whatever's there first.
                    name_cache.erase<NameTag>(name);
                    name_cache.emplace(NameEntry{std::move(name), std::move(record)});
                } else {
                    auto id = r.i64();
                    auto record = r.bytes(r.u32());
                    id_cache.erase<IdTag>(id);
                    id_cache.emplace(IdEntry{id, std::move(record)});
                }
                return encode_status(wire::Status::Ok);
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
