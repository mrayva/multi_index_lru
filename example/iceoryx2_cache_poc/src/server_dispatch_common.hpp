/// Machinery shared by both the read path (server_dispatch_read.hpp) and
/// the write path (server_dispatch_write.hpp): the request-handle type,
/// the deferred-completion queue NATS results flow back through,
/// per-key operation serialization, and cross-daemon coherence (kv_watch
/// invalidation). Nothing in this file is read-only or write-only --
/// CompletionQueue and KeyOperationQueue in particular are shared *because*
/// a Get and a Put on the same key must still serialize against each other,
/// even though the two operations now live in separate files.
///
/// `ActiveRequestType`/`ActiveRequestPtr`/`respond()` -- the only
/// transport-specific names this file (or dispatch_read/write.hpp) ever
/// touches -- come from whichever of active_request_iceoryx2.hpp /
/// active_request_ecal.hpp is selected below, by whether POC_USE_ECAL is
/// defined (see CMakeLists.txt: cache_poc_server_readthrough_ecal defines
/// it, cache_poc_server_readthrough does not). Everything else in this
/// file, and all of dispatch_request()/dispatch_read_request()/
/// dispatch_write_request(), is identical either way.
///
/// See server_readthrough.cpp's file comment and README.md's "the
/// concurrency model" for the design this implements, including
/// KeyOperationQueue below, which serializes operations on the same key so
/// a GET can never race a concurrent PUT/ERASE for that key and return an
/// inconsistent result.
#pragma once

#include "cache_service.hpp"

#ifdef POC_USE_ECAL
#include "active_request_ecal.hpp"
#else
#include "active_request_iceoryx2.hpp"
#endif

#include <chrono>
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
// one thread. Read and write completions share one queue and are drained
// together, in arrival order -- there's no reason for the main loop to treat
// them differently once NATS has already answered.
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

// Serializes operations on the same (cache, key) so at most one is ever "in
// flight" at a time -- without this, a GET-miss racing a concurrent PUT for
// the same key can return a stale value to the GET caller even though the
// cache itself converges correctly (the stale value's cache write is a safe
// no-op against the now-present key, but the *response* bytes were already
// built from the stale fetch before the race was noticed). See README.md
// "the stale GET response race" for the full scenario this closes. This is
// exactly why KeyOperationQueue lives here rather than in the read- or
// write-specific file: a Get and a Put on the same key must serialize
// against *each other*, so both dispatch_read_request() and
// dispatch_write_request() need the same instance.
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
// instead of queuing -- the caller responds Error immediately, rather than
// making the caller wait behind an ever-growing backlog for a key that's
// clearly not keeping up. Other keys are entirely unaffected: this is a
// per-key limit, not a global one.
//
// enqueue() and complete() are only ever called from the main iceoryx2
// thread (dispatch_read_request()/dispatch_write_request()'s call sites and
// CompletionQueue's drain loop in server_readthrough.cpp's main()), never
// from the NATS thread, so no locking is needed here.
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
    // that request itself instead (see dispatch_read_request()'s/
    // dispatch_write_request()'s rejection paths).
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
//   - When a kv_watch event arrives (InvalidationQueue below), observe()
//     reports whether that revision is genuinely new information. If it
//     isn't -- because it's an echo of a write this same daemon just made,
//     arriving back through its own watch subscription -- the caller skips
//     invalidating, avoiding the pointless evict-then-immediately-refetch
//     "flicker" a daemon would otherwise inflict on every one of its own
//     writes.
//
// Only ever touched from the main iceoryx2 thread (the read/write dispatch
// functions' apply closures and the invalidation-drain loop in
// server_readthrough.cpp's main()), so no locking is needed here.
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
//
// `value`/`is_put` carry the watch event's own payload -- during the
// startup prewarm window (see apply_pending_invalidations()) that's used to
// populate the cache directly instead of merely evicting, since kv_watch's
// initial burst (NATS delivering the *current* value of every existing key
// immediately on subscribe) already hands over the data; no reason to throw
// it away and pay a separate kv_get for it. Outside the prewarm window
// they're unused -- ordinary invalidation still just evicts, unchanged.
struct PendingInvalidation {
    bool is_name_cache;             // true -> NameCache, false -> IdCache
    std::string key;                // NameCache key verbatim, or IdCache key as its decimal string form
    std::uint64_t revision;
    std::vector<std::uint8_t> value;  // valid iff is_put
    bool is_put;                      // true for a put, false for a del/purge
};

// Thread-safe hand-off for PendingInvalidation, same shape as CompletionQueue.
class InvalidationQueue {
public:
    void push(bool is_name_cache, std::string key, std::uint64_t revision, std::vector<std::uint8_t> value,
              bool is_put) {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(
            PendingInvalidation{is_name_cache, std::move(key), revision, std::move(value), is_put});
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
// write), either prewarms or invalidates the corresponding local entry --
// see `prewarm_deadline` below. Meant to be called once per
// server_readthrough.cpp main-loop iteration, alongside draining
// CompletionQueue. Neither read- nor write-specific: an invalidation can
// originate from another daemon's write, a raw `nats kv put`, or (before
// RevisionTracker suppresses the echo) this daemon's own write.
//
// `prewarm_deadline`: while `steady_clock::now()` is still before it (see
// --prewarm-window-ms/MIL_PREWARM_WINDOW_MS, computed once in
// server_readthrough.cpp's main() right after the watch subscriptions are
// established), every event -- burst or, if any slip in before the window
// closes, genuinely live -- populates the cache directly from the event's
// own value (or, for a del/purge, negative-caches the absence) instead of
// evicting. This is deliberately scoped to *startup only*: once the
// deadline passes, behavior reverts to plain eviction exactly as before,
// unchanged -- ongoing operation doesn't turn kv_watch into a general
// refresh mechanism, only the cold-start burst benefits from it. A
// deadline already in the past (the default in tests that don't care about
// prewarming) means this is a no-op and every event is handled the old way.
inline void apply_pending_invalidations(NameCache& name_cache, IdCache& id_cache, RevisionTracker& revisions,
                                         InvalidationQueue& invalidations,
                                         std::chrono::steady_clock::time_point prewarm_deadline) {
    const bool prewarming = std::chrono::steady_clock::now() < prewarm_deadline;
    for (auto& invalidation : invalidations.drain()) {
        const auto qkey = invalidation.is_name_cache ? name_queue_key(invalidation.key)
                                                       : id_queue_key(std::stoll(invalidation.key));
        if (!revisions.observe(qkey, invalidation.revision)) {
            continue;  // echo of our own recent write (or an already-processed event) -- nothing to do
        }
        if (prewarming) {
            if (invalidation.is_name_cache) {
                name_cache.erase<NameTag>(invalidation.key);
                if (invalidation.is_put) {
                    name_cache.emplace(NameEntry{invalidation.key, invalidation.value});
                    std::cout << "[server] prewarmed name=\"" << invalidation.key << "\" from NATS\n";
                } else {
                    name_cache.emplace(NameEntry{invalidation.key, {}, false});
                }
            } else {
                const auto id = std::stoll(invalidation.key);
                id_cache.erase<IdTag>(id);
                if (invalidation.is_put) {
                    id_cache.emplace(IdEntry{id, invalidation.value});
                    std::cout << "[server] prewarmed id=" << id << " from NATS\n";
                } else {
                    id_cache.emplace(IdEntry{id, {}, false});
                }
            }
            continue;
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

}  // namespace poc
