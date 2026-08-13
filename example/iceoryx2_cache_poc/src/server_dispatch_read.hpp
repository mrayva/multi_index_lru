/// The read side of request dispatch: Get (Name/Id) and GetAll (prefix).
/// Never touches --allow-writes -- there's no gate here at all, by
/// construction, not by a runtime check this file happens to always pass.
/// Deliberately has no direct access to anything write-specific (no
/// put_async/erase_async call is reachable from this file); see
/// server_dispatch_write.hpp for that side and server_dispatch.hpp for the
/// thin router that decodes op/kind and picks one or the other.
#pragma once

#include "cache_service.hpp"
#include "nats_bridge.hpp"
#include "server_dispatch_common.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <variant>

namespace poc {

// Handles op == Get or op == GetAll (kind and the rest of the request are
// still unread from `r` -- op/kind were already consumed by the router in
// server_dispatch.hpp before it decided this was a read). Either responds
// immediately (local cache hit, a rejected category GetAll, or lets a
// malformed request's exception propagate to the router's catch block) or
// hands `active_request` to `completions` -- to be responded to once the
// async NATS operation it kicks off completes. Every path goes through
// `key_queue` so operations on the same key never overlap in flight -- see
// KeyOperationQueue in server_dispatch_common.hpp.
//
// `max_getall_results` caps how many matched keys a GetAll(prefix) request
// actually fetches from NATS -- same purpose and default as server.cpp's
// --max-getall-results for the local category flavor (cache_service.hpp).
//
// Any exception this throws (malformed/truncated request -- see
// wire::Reader, or SecurityKey::key()/pattern() rejecting an invalid field
// -- see nats_key.hpp's validate_nats_key_field()) is expected to propagate
// to server_dispatch.hpp's single try/catch, not be caught here:
// `active_request` is passed by reference, still bound to the router's own
// local, still-intact object, so the router's catch block can always
// respond with it directly as long as parsing (which is all that can
// throw) happens before any std::make_shared/move of `active_request`
// below -- exactly the same invariant the pre-split single function relied
// on. This is why every Security branch below derives its NATS key/pattern
// -- which can throw -- before constructing `request`.
inline void dispatch_read_request(NameCache& name_cache, IdCache& id_cache, SecurityCache& security_cache,
                                   NatsBridge& nats, const std::string& name_bucket, const std::string& id_bucket,
                                   const std::string& security_bucket, CompletionQueue& completions,
                                   KeyOperationQueue& key_queue, std::size_t max_getall_results, wire::Op op,
                                   wire::KeyKind kind, wire::Reader& r, ActiveRequestType& active_request) {
    if (op == wire::Op::Get) {
        if (kind == wire::KeyKind::Security) {
            auto asset_type = r.str();
            auto cusip = r.str();
            auto isin = r.str();
            auto sedol = r.str();
            auto ric = r.str();
            // Validates + derives the NATS key before `active_request` is
            // touched -- see this function's own doc comment above.
            const auto nats_key = SecurityKey::key(asset_type, cusip, isin, sedol, ric);
            auto qkey = "S:" + nats_key;
            auto request = std::make_shared<ActiveRequestType>(std::move(active_request));
            if (!key_queue.enqueue(qkey, [&security_cache, &nats, &completions, &key_queue, security_bucket, request,
                                           asset_type, cusip, isin, sedol, ric, nats_key, qkey]() mutable {
                if (auto it = security_cache.find<SecurityKeyTag>(boost::make_tuple(asset_type, cusip, isin, sedol, ric));
                    it != security_cache.end<SecurityKeyTag>()) {
                    if (it->found) {
                        std::cout << "[server] GET security=\"" << nats_key << "\" -> local hit\n";
                        respond(*request, encode_found(it->record));
                    } else {
                        std::cout << "[server] GET security=\"" << nats_key << "\" -> local negative hit (cached miss)\n";
                        respond(*request, encode_status(wire::Status::NotFound));
                    }
                    key_queue.complete(qkey);
                    return;
                }
                std::cout << "[server] GET security=\"" << nats_key << "\" -> local miss, dispatched to NATS\n";
                nats.get_async(security_bucket, nats_key,
                                [&completions, request, asset_type, cusip, isin, sedol, ric, nats_key, qkey](
                                    NatsGetResult result) mutable {
                                    ApplyFn apply = [asset_type, cusip, isin, sedol, ric, result](
                                                         NameCache&, IdCache&,
                                                         SecurityCache& security_cache) -> std::vector<std::uint8_t> {
                                        if (result.result == NatsResult::Ok) {
                                            security_cache.emplace(
                                                SecurityEntry{asset_type, cusip, isin, sedol, ric, result.value});
                                            return encode_found(result.value);
                                        }
                                        if (result.result == NatsResult::NotFound) {
                                            security_cache.emplace(
                                                SecurityEntry{asset_type, cusip, isin, sedol, ric, {}, false});
                                            return encode_status(wire::Status::NotFound);
                                        }
                                        return encode_status(wire::Status::Error);
                                    };
                                    completions.push(std::move(request), std::move(apply),
                                                      "GET security=\"" + nats_key + "\"", qkey,
                                                      result.result == NatsResult::Ok ? result.revision : 0,
                                                      result.result == NatsResult::Ok);
                                });
            })) {
                std::cout << "[server] GET security=\"" << nats_key << "\" -> rejected (queue full for this key)\n";
                respond(*request, encode_status(wire::Status::Error));
            }
            return;
        }

        if (kind == wire::KeyKind::Name) {
            auto key = r.str();
            auto qkey = name_queue_key(key);
            auto request = std::make_shared<ActiveRequestType>(std::move(active_request));
            if (!key_queue.enqueue(qkey, [&name_cache, &nats, &completions, &key_queue, name_bucket, request, key,
                                           qkey]() mutable {
                if (auto it = name_cache.find<NameTag>(key); it != name_cache.end<NameTag>()) {
                    if (it->found) {
                        std::cout << "[server] GET name=\"" << key << "\" -> local hit\n";
                        respond(*request, encode_found(it->record));
                    } else {
                        std::cout << "[server] GET name=\"" << key << "\" -> local negative hit (cached miss)\n";
                        respond(*request, encode_status(wire::Status::NotFound));
                    }
                    key_queue.complete(qkey);
                    return;
                }
                std::cout << "[server] GET name=\"" << key << "\" -> local miss, dispatched to NATS\n";
                nats.get_async(name_bucket, key, [&completions, request, key, qkey](NatsGetResult result) mutable {
                    ApplyFn apply = [key, result](NameCache& name_cache, IdCache&,
                                                   SecurityCache&) -> std::vector<std::uint8_t> {
                        if (result.result == NatsResult::Ok) {
                            name_cache.emplace(NameEntry{key, result.value});
                            return encode_found(result.value);
                        }
                        if (result.result == NatsResult::NotFound) {
                            // Negative-cache the miss: the next GET for
                            // this key is a local (negative) hit instead
                            // of another NATS round trip, until kv_watch
                            // sees it written or the TTL lapses.
                            name_cache.emplace(NameEntry{key, {}, false});
                            return encode_status(wire::Status::NotFound);
                        }
                        return encode_status(wire::Status::Error);
                    };
                    completions.push(std::move(request), std::move(apply), "GET name=\"" + key + "\"", qkey,
                                      result.result == NatsResult::Ok ? result.revision : 0,
                                      result.result == NatsResult::Ok);
                });
            })) {
                std::cout << "[server] GET name=\"" << key << "\" -> rejected (queue full for this key)\n";
                respond(*request, encode_status(wire::Status::Error));
            }
            return;
        }

        auto key = r.i64();
        auto qkey = id_queue_key(key);
        auto request = std::make_shared<ActiveRequestType>(std::move(active_request));
        if (!key_queue.enqueue(
                qkey, [&id_cache, &nats, &completions, &key_queue, id_bucket, request, key, qkey]() mutable {
                               if (auto it = id_cache.find<IdTag>(key); it != id_cache.end<IdTag>()) {
                                   if (it->found) {
                                       std::cout << "[server] GET id=" << key << " -> local hit\n";
                                       respond(*request, encode_found(it->record));
                                   } else {
                                       std::cout << "[server] GET id=" << key
                                                 << " -> local negative hit (cached miss)\n";
                                       respond(*request, encode_status(wire::Status::NotFound));
                                   }
                                   key_queue.complete(qkey);
                                   return;
                               }
                               std::cout << "[server] GET id=" << key << " -> local miss, dispatched to NATS\n";
                               nats.get_async(id_bucket, std::to_string(key),
                                              [&completions, request, key, qkey](NatsGetResult result) mutable {
                                                  ApplyFn apply = [key, result](NameCache&,
                                                                                 IdCache& id_cache,
                                                                                 SecurityCache&)
                                                      -> std::vector<std::uint8_t> {
                                                      if (result.result == NatsResult::Ok) {
                                                          id_cache.emplace(IdEntry{key, result.value});
                                                          return encode_found(result.value);
                                                      }
                                                      if (result.result == NatsResult::NotFound) {
                                                          // See the Name branch's identical comment.
                                                          id_cache.emplace(IdEntry{key, {}, false});
                                                          return encode_status(wire::Status::NotFound);
                                                      }
                                                      return encode_status(wire::Status::Error);
                                                  };
                                                  completions.push(std::move(request), std::move(apply),
                                                                    "GET id=" + std::to_string(key), qkey,
                                                                    result.result == NatsResult::Ok
                                                                        ? result.revision
                                                                        : 0,
                                                                    result.result == NatsResult::Ok);
                                              });
                           })) {
            std::cout << "[server] GET id=" << key << " -> rejected (queue full for this key)\n";
            respond(*request, encode_status(wire::Status::Error));
        }
        return;
    }

    // op == GetAll
    if (kind == wire::KeyKind::Security) {
        // Decode the wire-level mirror of SecurityKey::pattern()'s 5
        // std::variant<std::string, NatsAny> arguments -- see
        // cache_service.hpp "GetAll (Security, pattern find)" for the wire
        // shape and encode_find_security() for the encoder this decodes.
        auto read_field = [&r]() -> std::variant<std::string, multi_index_lru::NatsAny> {
            const bool wildcard = r.u8() != 0;
            auto value = r.str();
            if (wildcard) {
                return multi_index_lru::nats_any;
            }
            return value;
        };
        auto f0 = read_field();
        auto f1 = read_field();
        auto f2 = read_field();
        auto f3 = read_field();
        auto f4 = read_field();
        // Validates every literal field and derives the pattern before
        // `active_request` is touched -- see this function's own doc
        // comment above.
        const auto pattern = SecurityKey::pattern(f0, f1, f2, f3, f4);
        // No single key to serialize on -- a pattern find's own queue
        // slot, same idea as GetAll(prefix)'s use of name_queue_key(prefix)
        // below.
        auto qkey = "SFind:" + pattern;
        auto request = std::make_shared<ActiveRequestType>(std::move(active_request));
        if (!key_queue.enqueue(qkey, [&nats, &completions, security_bucket, request, pattern, qkey,
                                       max_getall_results]() mutable {
            std::cout << "[server] FIND security pattern=\"" << pattern << "\" -> dispatched to NATS\n";
            nats.list_and_get_async(
                security_bucket, pattern, max_getall_results,
                [&completions, request, pattern, qkey](NatsListResult result) mutable {
                    ApplyFn apply = [pattern, result](NameCache&, IdCache&,
                                                       SecurityCache& security_cache) -> std::vector<std::uint8_t> {
                        if (result.result != NatsResult::Ok) {
                            return encode_status(wire::Status::Error);
                        }
                        // Same reasoning as GetAll(prefix)'s identical
                        // comment: populate the local cache with each
                        // match under its own full composite key, but
                        // never claim local completeness for the pattern
                        // itself -- a repeat find always re-asks NATS.
                        for (const auto& [full_key, value] : result.entries) {
                            const auto fields = split_security_key(full_key);
                            security_cache.erase<SecurityKeyTag>(
                                boost::make_tuple(fields[0], fields[1], fields[2], fields[3], fields[4]));
                            security_cache.emplace(
                                SecurityEntry{fields[0], fields[1], fields[2], fields[3], fields[4], value});
                        }
                        if (result.entries.empty()) {
                            return encode_status(wire::Status::NotFound);
                        }
                        return encode_found_all_with_keys(result.entries, result.truncated);
                    };
                    // No revision -- same reasoning as GetAll(prefix)'s
                    // identical comment: a read, and the pattern itself
                    // was never a real NATS key with a revision of its own.
                    completions.push(std::move(request), std::move(apply),
                                      "FIND security pattern=\"" + pattern + "\"", qkey, 0, false);
                });
        })) {
            std::cout << "[server] FIND security pattern=\"" << pattern << "\" -> rejected (queue full for this key)\n";
            respond(*request, encode_status(wire::Status::Error));
        }
        return;
    }
    if (kind != wire::KeyKind::Name) {
        // Category-based GetAll has no NATS equivalent -- see
        // cache_service.hpp "Non-unique key lookup (GetAll)".
        respond(active_request, encode_status(wire::Status::Error));
        return;
    }
    auto prefix = r.str();
    const auto pattern = prefix + ".*";
    // Its own queue slot, namespaced like a plain Get/Put/Erase on
    // "prefix" itself would be -- note this does NOT serialize
    // against a concurrent Get on one of the prefix's *matched*
    // sub-keys (e.g. "alice.email" while this fetches "alice.*"):
    // KeyOperationQueue's key namespace is per-exact-key, and which
    // sub-keys a prefix expands to isn't known until the NATS list
    // call returns. A GetAll racing a Get on one if its own matches
    // is a known, accepted gap -- see README.md.
    auto qkey = name_queue_key(prefix);
    auto request = std::make_shared<ActiveRequestType>(std::move(active_request));
    if (!key_queue.enqueue(qkey, [&nats, &completions, name_bucket, request, prefix, pattern, qkey,
                                   max_getall_results]() mutable {
        std::cout << "[server] GETALL name-prefix=\"" << prefix << "\" (pattern \"" << pattern
                  << "\") -> dispatched to NATS\n";
        nats.list_and_get_async(
            name_bucket, pattern, max_getall_results,
            [&completions, request, prefix, qkey](NatsListResult result) mutable {
                ApplyFn apply = [prefix, result](NameCache& name_cache, IdCache&,
                                                   SecurityCache&) -> std::vector<std::uint8_t> {
                    if (result.result != NatsResult::Ok) {
                        return encode_status(wire::Status::Error);
                    }
                    // Populate the cache with each match under its
                    // own full key -- a later plain Get for e.g.
                    // "alice.email" is a local hit for free. This
                    // GetAll never claims local completeness for
                    // the prefix itself, though (a key added under
                    // it later, that this call never saw, isn't
                    // known locally until asked for): a repeat
                    // GetAll for the same prefix always re-asks
                    // NATS rather than trusting the cache.
                    for (const auto& [full_key, value] : result.entries) {
                        name_cache.erase<NameTag>(full_key);
                        name_cache.emplace(NameEntry{full_key, value});
                    }
                    if (result.entries.empty()) {
                        return encode_status(wire::Status::NotFound);
                    }
                    return encode_found_all_with_keys(result.entries, result.truncated);
                };
                // No revision: this is a read, not a write -- unlike
                // Put/Erase there's no self-echo for RevisionTracker
                // to suppress, and the prefix itself was never a
                // real NATS key with a revision of its own.
                completions.push(std::move(request), std::move(apply),
                                  "GETALL name-prefix=\"" + prefix + "\"", qkey, 0, false);
            });
    })) {
        std::cout << "[server] GETALL name-prefix=\"" << prefix << "\" -> rejected (queue full for this key)\n";
        respond(*request, encode_status(wire::Status::Error));
    }
}

}  // namespace poc
