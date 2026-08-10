/// The write side of request dispatch: Put and Erase. Owns the
/// --allow-writes gate itself -- it's the only file that needs to know that
/// flag exists at all; server_dispatch_read.hpp has no equivalent parameter
/// and can't be gated by it even by mistake. See README.md "Read-only by
/// default" for why writes are rejected unless the daemon was explicitly
/// started with --allow-writes/MIL_ALLOW_WRITES.
///
/// Notably takes no NameCache&/IdCache& at all: unlike the read path, a
/// write's cache mutation happens later, inside the ApplyFn closure that
/// CompletionQueue carries -- server_readthrough.cpp's main loop is the
/// only place that ever touches the caches from a write, once NATS has
/// confirmed the write landed. This file only needs NatsBridge and the two
/// shared queues.
#pragma once

#include "cache_service.hpp"
#include "nats_bridge.hpp"
#include "server_dispatch_common.hpp"

#include <iostream>
#include <memory>
#include <string>

namespace poc {

// Handles op == Put or op == Erase (kind and the rest of the request are
// still unread from `r` -- op/kind were already consumed by the router in
// server_dispatch.hpp before it decided this was a write). Rejects
// immediately with Status::ReadOnly if `allow_writes` is false, before
// touching `key_queue` or NATS at all -- a write that's always going to be
// refused shouldn't cost a queue slot or count against another key's
// backpressure budget. Otherwise hands `active_request` to `completions`,
// to be responded to once the async NATS write it kicks off completes.
// Every path goes through `key_queue` so operations on the same key never
// overlap in flight -- see KeyOperationQueue in server_dispatch_common.hpp.
//
// Same exception-propagation contract as dispatch_read_request(): any
// exception this throws is expected to reach server_dispatch.hpp's single
// try/catch, not be caught here, relying on the same "parse before you
// move `active_request`" invariant.
inline void dispatch_write_request(NatsBridge& nats, const std::string& name_bucket, const std::string& id_bucket,
                                    CompletionQueue& completions, KeyOperationQueue& key_queue, bool allow_writes,
                                    wire::Op op, wire::KeyKind kind, wire::Reader& r,
                                    ActiveRequestType& active_request) {
    if (!allow_writes) {
        respond(active_request, encode_status(wire::Status::ReadOnly));
        return;
    }

    if (op == wire::Op::Put) {
        if (kind == wire::KeyKind::Name) {
            auto key = r.str();
            // Category is stored (NameCache's second, non-unique index
            // -- see cache_service.hpp "Non-unique key lookup
            // (GetAll)") but GetAll itself isn't implemented against
            // this NATS-backed server -- read through/write through
            // only cover the primary key. Still has to be parsed here
            // regardless, matching encode_put()'s wire format exactly:
            // this decode has no way to know a category was omitted
            // versus genuinely empty, and skipping it would misread
            // every byte after it as part of the record.
            auto category = r.str();
            auto record = r.bytes(r.u32());
            auto qkey = name_queue_key(key);
            auto request = std::make_shared<ActiveRequestType>(std::move(active_request));
            if (!key_queue.enqueue(qkey, [&nats, &completions, name_bucket, request, key, record, category,
                                           qkey]() mutable {
                std::cout << "[server] PUT name=\"" << key << "\" -> dispatched to NATS\n";
                nats.put_async(name_bucket, key, record,
                                [&completions, request, key, record, category, qkey](
                                    bool ok, std::uint64_t revision, std::string /*err*/) mutable {
                                    ApplyFn apply = [key, record, category, ok](
                                                         NameCache& name_cache,
                                                         IdCache&) -> std::vector<std::uint8_t> {
                                        if (!ok) {
                                            return encode_status(wire::Status::Error);
                                        }
                                        name_cache.erase<NameTag>(key);
                                        name_cache.emplace(NameEntry{key, record, true, category});
                                        return encode_status(wire::Status::Ok);
                                    };
                                    completions.push(std::move(request), std::move(apply),
                                                      "PUT name=\"" + key + "\"", qkey, revision, ok);
                                });
            })) {
                std::cout << "[server] PUT name=\"" << key << "\" -> rejected (queue full for this key)\n";
                respond(*request, encode_status(wire::Status::Error));
            }
            return;
        }

        auto key = r.i64();
        auto record = r.bytes(r.u32());
        auto qkey = id_queue_key(key);
        auto request = std::make_shared<ActiveRequestType>(std::move(active_request));
        if (!key_queue.enqueue(qkey, [&nats, &completions, id_bucket, request, key, record, qkey]() mutable {
            std::cout << "[server] PUT id=" << key << " -> dispatched to NATS\n";
            nats.put_async(id_bucket, std::to_string(key), record,
                            [&completions, request, key, record, qkey](bool ok, std::uint64_t revision,
                                                                        std::string /*err*/) mutable {
                                ApplyFn apply = [key, record, ok](NameCache&,
                                                                   IdCache& id_cache) -> std::vector<std::uint8_t> {
                                    if (!ok) {
                                        return encode_status(wire::Status::Error);
                                    }
                                    id_cache.erase<IdTag>(key);
                                    id_cache.emplace(IdEntry{key, record});
                                    return encode_status(wire::Status::Ok);
                                };
                                completions.push(std::move(request), std::move(apply),
                                                  "PUT id=" + std::to_string(key), qkey, revision, ok);
                            });
        })) {
            std::cout << "[server] PUT id=" << key << " -> rejected (queue full for this key)\n";
            respond(*request, encode_status(wire::Status::Error));
        }
        return;
    }

    // op == Erase
    if (kind == wire::KeyKind::Name) {
        auto key = r.str();
        auto qkey = name_queue_key(key);
        auto request = std::make_shared<ActiveRequestType>(std::move(active_request));
        if (!key_queue.enqueue(qkey, [&nats, &completions, name_bucket, request, key, qkey]() mutable {
            std::cout << "[server] ERASE name=\"" << key << "\" -> dispatched to NATS\n";
            nats.erase_async(name_bucket, key,
                              [&completions, request, key, qkey](bool ok, std::uint64_t revision,
                                                                  std::string /*err*/) mutable {
                                  ApplyFn apply = [key, ok](NameCache& name_cache,
                                                             IdCache&) -> std::vector<std::uint8_t> {
                                      if (!ok) {
                                          return encode_status(wire::Status::Error);
                                      }
                                      // Negative-cache the now-confirmed
                                      // absence, same as a NATS-confirmed
                                      // GET miss -- the next GET for this
                                      // key is a local hit, not another
                                      // NATS round trip.
                                      name_cache.erase<NameTag>(key);
                                      name_cache.emplace(NameEntry{key, {}, false});
                                      return encode_status(wire::Status::Ok);
                                  };
                                  completions.push(std::move(request), std::move(apply),
                                                    "ERASE name=\"" + key + "\"", qkey, revision, ok);
                              });
        })) {
            std::cout << "[server] ERASE name=\"" << key << "\" -> rejected (queue full for this key)\n";
            respond(*request, encode_status(wire::Status::Error));
        }
        return;
    }

    auto key = r.i64();
    auto qkey = id_queue_key(key);
    auto request = std::make_shared<ActiveRequestType>(std::move(active_request));
    if (!key_queue.enqueue(qkey, [&nats, &completions, id_bucket, request, key, qkey]() mutable {
        std::cout << "[server] ERASE id=" << key << " -> dispatched to NATS\n";
        nats.erase_async(id_bucket, std::to_string(key),
                          [&completions, request, key, qkey](bool ok, std::uint64_t revision,
                                                              std::string /*err*/) mutable {
                              ApplyFn apply = [key, ok](NameCache&, IdCache& id_cache) -> std::vector<std::uint8_t> {
                                  if (!ok) {
                                      return encode_status(wire::Status::Error);
                                  }
                                  // See the Name branch's identical comment.
                                  id_cache.erase<IdTag>(key);
                                  id_cache.emplace(IdEntry{key, {}, false});
                                  return encode_status(wire::Status::Ok);
                              };
                              completions.push(std::move(request), std::move(apply),
                                                "ERASE id=" + std::to_string(key), qkey, revision, ok);
                          });
    })) {
        std::cout << "[server] ERASE id=" << key << " -> rejected (queue full for this key)\n";
        respond(*request, encode_status(wire::Status::Error));
    }
}

}  // namespace poc
