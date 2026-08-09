/// The non-blocking request-dispatch machinery for server_readthrough.cpp,
/// pulled into its own header so it's includable from tests: dispatch_request()
/// needs a real iceoryx2 ActiveRequest, which can only come from an actual
/// Server that actually received a RequestMut over real IPC -- there's no
/// standalone/fake way to construct one -- so testing it means driving a real
/// (in-process) client/server pair, which test/dispatch_request_test.cpp does.
///
/// See server_readthrough.cpp's file comment and README.md's "the concurrency
/// model" for the design this implements, including KeyOperationQueue below,
/// which serializes operations on the same key so a GET can never race a
/// concurrent PUT/ERASE for that key and return an inconsistent result.
#pragma once

#include "cache_service.hpp"
#include "nats_bridge.hpp"

#include "iox2/iceoryx2.hpp"

#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace poc {

// Defaults only -- dispatch_request() takes the actual bucket names as
// parameters so server_readthrough.cpp can override them via
// --name-bucket/--id-bucket or MIL_NAME_BUCKET/MIL_ID_BUCKET (see
// config.hpp). Kept as named constants since tests and client_readthrough.cpp
// still want a sensible fixed default to talk to.
inline constexpr auto kNameBucket = "mil_by_name";
inline constexpr auto kIdBucket = "mil_by_id";

// request_response<Slice<u8>, Slice<u8>>() with no explicit header types
// resolves to `void` headers -- see ServiceBuilder::request_response() in
// iceoryx2-cxx/include/iox2/service_builder.hpp.
using ActiveRequestType =
    iox2::ActiveRequest<iox2::ServiceType::Ipc, iox2::bb::Slice<std::uint8_t>, void, iox2::bb::Slice<std::uint8_t>,
                         void>;

// ActiveRequestType is move-only by design. It now needs to be captured by
// closures stored in both CompletionQueue and KeyOperationQueue below, both
// of which need those closures to be copy-constructible (std::function's
// requirement) -- a shared_ptr is the simplest way to get that without
// hand-rolling a move-only type-erased callable.
using ActiveRequestPtr = std::shared_ptr<ActiveRequestType>;

// Runs on the main thread once a deferred request's NATS operation has
// completed: applies whatever cache mutation is needed (a Get populating the
// cache on a NATS hit, or a Put/Erase applying now that NATS confirmed it)
// and returns the response bytes to send.
using ApplyFn = std::function<std::vector<std::uint8_t>(NameCache&, IdCache&)>;

struct PendingCompletion {
    ActiveRequestPtr active_request;
    ApplyFn apply;
    std::string description;  // for logging only, e.g. "GET name=\"alice\"" -- built on whichever
                               // thread calls push(), but only ever printed on the main thread's
                               // drain loop, so it doesn't need to touch std::cout itself.
    std::string queue_key;     // KeyOperationQueue key this completion must release when applied.
    std::uint64_t revision = 0;  // valid iff has_revision -- see RevisionTracker
    bool has_revision = false;   // true for a successful GET-populate, PUT, or Erase; false for a
                                  // failed op, or a GET-populate that found nothing (a key that's
                                  // never existed has no revision to record)
};

// Shared between the main iceoryx2 thread and NatsBridge's background NATS
// I/O thread. NATS completions are pushed here (cheaply -- no cache access,
// no iceoryx2 API calls) from the NATS thread; only the main thread ever
// drains it, which is where the actual cache mutation and iceoryx2 response
// happen. That keeps both multi_index_lru::Container's single-owner/
// no-locking contract and iceoryx2's Server/ActiveRequest usage confined to
// one thread.
class CompletionQueue {
public:
    void push(ActiveRequestPtr active_request, ApplyFn apply, std::string description, std::string queue_key,
              std::uint64_t revision = 0, bool has_revision = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(PendingCompletion{std::move(active_request), std::move(apply), std::move(description),
                                            std::move(queue_key), revision, has_revision});
    }

    std::vector<PendingCompletion> drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PendingCompletion> out;
        out.reserve(items_.size());
        for (auto& item : items_) {
            out.push_back(std::move(item));
        }
        items_.clear();
        return out;
    }

    // Test-only: how many completions are currently queued, without draining
    // them. Not used by server_readthrough.cpp's main loop.
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<PendingCompletion> items_;
};

inline void respond(ActiveRequestType& active_request, const std::vector<std::uint8_t>& response_bytes) {
    auto response = active_request.loan_slice_uninit(response_bytes.size()).value();
    auto initialized = response.write_from_fn([&](auto byte_idx) { return response_bytes[byte_idx]; });
    send(std::move(initialized)).value();
}

// Serializes operations on the same (cache, key) so at most one is ever "in
// flight" at a time -- without this, a GET-miss racing a concurrent PUT for
// the same key can return a stale value to the GET caller even though the
// cache itself converges correctly (the stale value's cache write is a safe
// no-op against the now-present key, but the *response* bytes were already
// built from the stale fetch before the race was noticed). See README.md
// "the stale GET response race" for the full scenario this closes.
//
// A side effect: concurrent GET-misses on the same never-cached key now also
// naturally coalesce -- the second GET queues behind the first instead of
// firing its own redundant NATS round trip, and finds a local hit once the
// first GET's completion has populated the cache. That's the thundering-herd
// protection the old blocking-bridge design gave for free and the move to
// non-blocking dispatch had otherwise dropped.
//
// Backpressure: `max_depth_per_key` caps how many operations (in flight plus
// queued) a single key can accumulate. Without a cap, a client (or several)
// hammering one hot key with PUTs -- each necessarily serialized behind the
// last since they share a key -- can queue arbitrarily many closures (each
// holding an ActiveRequestPtr and its own captured request bytes) with
// throughput bounded by NATS round-trip latency alone; README.md "Load
// testing" measured this directly (memory tracked queue depth and receded
// once it drained, so not a leak, but nothing was stopping that depth from
// growing without limit). Once a key is at capacity, enqueue() rejects
// instead of queuing -- dispatch_request() responds Error immediately,
// rather than making the caller wait behind an ever-growing backlog for a
// key that's clearly not keeping up. Other keys are entirely unaffected:
// this is a per-key limit, not a global one.
//
// enqueue() and complete() are only ever called from the main iceoryx2
// thread (dispatch_request()'s call site and CompletionQueue's drain loop in
// server_readthrough.cpp's main()), never from the NATS thread, so no
// locking is needed here.
class KeyOperationQueue {
public:
    explicit KeyOperationQueue(std::size_t max_depth_per_key) : max_depth_per_key_(max_depth_per_key) {
        if (max_depth_per_key == 0) {
            throw std::invalid_argument("KeyOperationQueue: max_depth_per_key must be positive");
        }
    }

    // If no operation is currently in flight for `key`, runs `op` immediately
    // (synchronously, before returning) and returns true. If one already is,
    // queues `op` to run automatically once the current holder calls
    // complete(key) and returns true -- unless `key`'s queue (in flight plus
    // already queued) is already at max_depth_per_key, in which case `op` is
    // left untouched and this returns false: the caller must respond to
    // that request itself instead (see dispatch_request()'s rejection path).
    bool enqueue(const std::string& key, std::function<void()> op) {
        auto it = queues_.find(key);
        if (it != queues_.end() && it->second.size() >= max_depth_per_key_) {
            return false;
        }
        auto& q = queues_[key];
        q.push_back(std::move(op));
        if (q.size() == 1) {
            // Run from a copy, not a reference into the deque: op's own body
            // may call complete(key) synchronously (the local-hit path
            // does), which would pop/erase this very deque entry while it's
            // still executing if we invoked it in place.
            std::function<void()> to_run = q.front();
            to_run();
        }
        return true;
    }

    // Must be called exactly once, when the operation currently at the front
    // of `key`'s queue has fully finished (cache mutated if needed, response
    // sent). Starts the next queued operation for `key`, if any.
    void complete(const std::string& key) {
        auto it = queues_.find(key);
        if (it == queues_.end()) {
            return;
        }
        it->second.pop_front();
        if (it->second.empty()) {
            queues_.erase(it);
            return;
        }
        std::function<void()> to_run = it->second.front();
        to_run();
    }

private:
    std::size_t max_depth_per_key_;
    std::unordered_map<std::string, std::deque<std::function<void()>>> queues_;
};

inline std::string name_queue_key(const std::string& name) { return "N:" + name; }
inline std::string id_queue_key(std::int64_t id) { return "I:" + std::to_string(id); }

// Cross-daemon coherence, part 1: tracks the highest NATS KV revision this
// daemon already knows about for each key. Used two ways:
//   - After applying a value from our own GET-populate or PUT completion,
//     observe() records that revision.
//   - When a kv_watch event arrives (server_dispatch.hpp's InvalidationQueue
//     below), observe() reports whether that revision is genuinely new
//     information. If it isn't -- because it's an echo of a write this same
//     daemon just made, arriving back through its own watch subscription --
//     the caller skips invalidating, avoiding the pointless
//     evict-then-immediately-refetch "flicker" a daemon would otherwise
//     inflict on every one of its own writes.
//
// Only ever touched from the main iceoryx2 thread (dispatch_request()'s
// apply closures and the invalidation-drain loop in server_readthrough.cpp's
// main()), so no locking is needed here.
class RevisionTracker {
public:
    // Returns true if `revision` is new information -- greater than whatever
    // was already recorded for `key` (or nothing was recorded yet) -- and
    // updates the record to `revision` either way (so a later duplicate
    // report of the same revision, from any source, is also recognized as
    // already-known).
    bool observe(const std::string& key, std::uint64_t revision) {
        auto [it, inserted] = known_.try_emplace(key, revision);
        if (inserted) {
            return true;
        }
        if (revision > it->second) {
            it->second = revision;
            return true;
        }
        return false;
    }

private:
    std::unordered_map<std::string, std::uint64_t> known_;
};

// Cross-daemon coherence, part 2: a NATS kv_watch change event, in the form
// the main thread needs to act on it (see apply_pending_invalidations()
// below). Built on the NATS thread (inside NatsBridge::watch()'s callback,
// wired up in server_readthrough.cpp's main()); only ever applied on the
// main thread.
struct PendingInvalidation {
    bool is_name_cache;  // true -> NameCache, false -> IdCache
    std::string key;     // NameCache key verbatim, or IdCache key as its decimal string form
    std::uint64_t revision;
};

// Thread-safe hand-off for PendingInvalidation, same shape as CompletionQueue.
class InvalidationQueue {
public:
    void push(bool is_name_cache, std::string key, std::uint64_t revision) {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(PendingInvalidation{is_name_cache, std::move(key), revision});
    }

    std::vector<PendingInvalidation> drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PendingInvalidation> out;
        out.reserve(items_.size());
        for (auto& item : items_) {
            out.push_back(std::move(item));
        }
        items_.clear();
        return out;
    }

private:
    std::mutex mutex_;
    std::deque<PendingInvalidation> items_;
};

// Applies every currently-queued invalidation: for each one that's genuinely
// new information (per `revisions`, not an echo of this daemon's own recent
// write), erases the corresponding entry from the local cache so the next
// GET for that key is a real miss and reads the fresh value through from
// NATS. Meant to be called once per server_readthrough.cpp main-loop
// iteration, alongside draining CompletionQueue.
inline void apply_pending_invalidations(NameCache& name_cache, IdCache& id_cache, RevisionTracker& revisions,
                                         InvalidationQueue& invalidations) {
    for (auto& invalidation : invalidations.drain()) {
        const auto qkey = invalidation.is_name_cache ? name_queue_key(invalidation.key)
                                                       : id_queue_key(std::stoll(invalidation.key));
        if (!revisions.observe(qkey, invalidation.revision)) {
            continue;  // echo of our own recent write (or an already-processed event) -- nothing to do
        }
        if (invalidation.is_name_cache) {
            if (name_cache.erase<NameTag>(invalidation.key)) {
                std::cout << "[server] invalidated name=\"" << invalidation.key
                          << "\" (external write, revision " << invalidation.revision << ")\n";
            }
        } else {
            const auto id = std::stoll(invalidation.key);
            if (id_cache.erase<IdTag>(id)) {
                std::cout << "[server] invalidated id=" << id << " (external write, revision "
                          << invalidation.revision << ")\n";
            }
        }
    }
}

// Parses one request and either responds immediately (local cache hit, or a
// malformed request) or hands `active_request` to `completions` -- to be
// responded to once the async NATS operation it kicks off completes. Every
// path goes through `key_queue` so operations on the same key never overlap
// in flight -- see KeyOperationQueue above.
//
// Any exception here (malformed/truncated request -- see wire::Reader) is
// caught and turned into an immediate Error response; the same hardening
// server.cpp's handle_request_local() has. Parsing (and any exception it can
// throw) always happens before `active_request` is converted to a
// KeyOperationQueue-compatible ActiveRequestPtr, so the catch block can
// always respond with the still-intact `active_request` parameter directly.
inline void dispatch_request(NameCache& name_cache, IdCache& id_cache, NatsBridge& nats, const std::string& name_bucket,
                              const std::string& id_bucket, CompletionQueue& completions, KeyOperationQueue& key_queue,
                              ActiveRequestType active_request, const std::uint8_t* data, std::size_t size) {
    try {
        wire::Reader r(data, size);
        const auto op = static_cast<wire::Op>(r.u8());
        const auto kind = static_cast<wire::KeyKind>(r.u8());

        if (op == wire::Op::Get) {
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
                        ApplyFn apply = [key, result](NameCache& name_cache, IdCache&) -> std::vector<std::uint8_t> {
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
                                                                                     IdCache& id_cache)
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

        if (op == wire::Op::Put) {
            if (kind == wire::KeyKind::Name) {
                auto key = r.str();
                auto record = r.bytes(r.u32());
                auto qkey = name_queue_key(key);
                auto request = std::make_shared<ActiveRequestType>(std::move(active_request));
                if (!key_queue.enqueue(qkey, [&nats, &completions, name_bucket, request, key, record, qkey]() mutable {
                    std::cout << "[server] PUT name=\"" << key << "\" -> dispatched to NATS\n";
                    nats.put_async(name_bucket, key, record,
                                    [&completions, request, key, record, qkey](bool ok, std::uint64_t revision,
                                                                                std::string /*err*/) mutable {
                                        ApplyFn apply = [key, record, ok](NameCache& name_cache,
                                                                           IdCache&) -> std::vector<std::uint8_t> {
                                            if (!ok) {
                                                return encode_status(wire::Status::Error);
                                            }
                                            name_cache.erase<NameTag>(key);
                                            name_cache.emplace(NameEntry{key, record});
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

        // Erase
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
    } catch (const std::exception&) {
        respond(active_request, encode_status(wire::Status::Error));
    } catch (...) {
        respond(active_request, encode_status(wire::Status::Error));
    }
}

}  // namespace poc
