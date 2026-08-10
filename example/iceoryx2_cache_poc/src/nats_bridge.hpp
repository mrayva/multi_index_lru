/// Bridge from synchronous/callback call sites to mrayva/nats_asio's
/// ASIO-coroutine-based NATS client.
///
/// nats_asio runs entirely as `asio::awaitable<...>` coroutines driven by an
/// `asio::io_context`. This class runs that io_context on a dedicated
/// background thread and offers two ways to consume it:
///
/// - `get_async()`/`put_async()`/`erase_async()`: fire the NATS operation and
///   return immediately; `on_done` is invoked on the NATS thread once it
///   completes. This is what server_readthrough.cpp uses, so a slow NATS
///   round trip for one client never blocks any other client's request --
///   see CompletionQueue in server_readthrough.cpp for how the response
///   actually gets sent once `on_done` fires.
/// - `get()`/`put()`/`erase()`: thin wrappers around the `_async` versions
///   that block the *caller's* thread (via std::promise/std::future) until
///   the callback fires. client_readthrough.cpp uses these for its own
///   one-shot, sequential NATS calls, where blocking is simplest and there's
///   no other client to stall.
///
/// Either way, only the NATS thread ever touches the nats_asio connection,
/// and callers are responsible for not touching a multi_index_lru::Container
/// from inside `on_done` directly -- see server_readthrough.cpp's
/// CompletionQueue, which defers the actual cache mutation to its own main
/// thread so the "single owner, no internal locking" contract holds.
///
/// A per-instance circuit breaker guards against a sustained NATS outage:
/// without it, every miss/write during an outage independently waits out the
/// full op_timeout (3s by default) before failing -- fine for one request,
/// wasteful and slow for every client hitting the daemon during that outage.
/// After `failure_threshold` consecutive failures the circuit opens and
/// every call fails instantly (no NATS round trip attempted at all) until
/// `open_duration` has passed, at which point the next call is let through
/// as a probe: success closes the circuit, failure reopens it. A NotFound
/// is a real answer from a healthy NATS, not a failure, and doesn't count
/// against the breaker.
///
/// `watch()` subscribes to every key change in a bucket (cross-daemon
/// coherence: see server_dispatch.hpp's InvalidationQueue/RevisionTracker
/// for how server_readthrough.cpp uses this to invalidate local cache
/// entries when another process writes the same NATS bucket).
#pragma once

#include <nats_asio/nats_asio.hpp>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace poc {

enum class NatsResult { Ok, NotFound, Error };

struct NatsGetResult {
    NatsResult result = NatsResult::Error;
    std::vector<std::uint8_t> value;  // valid iff result == Ok
    std::uint64_t revision = 0;       // valid iff result == Ok; NATS KV revision (sequence) for this key
    std::string error;                // valid iff result == Error
};

// Result of list_and_get_async(): every (full NATS key, value) pair matching
// a wildcard pattern, fetched as of a single point in time. `entries` uses
// the *full* NATS key (e.g. "alice.email"), not the prefix that was queried
// -- see server_dispatch.hpp's GetAll/prefix handling for why that matters
// (each entry becomes its own NameCache record, individually addressable by
// a later plain Get).
struct NatsListResult {
    NatsResult result = NatsResult::Error;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries;  // valid iff result == Ok
    bool truncated = false;  // valid iff result == Ok; true iff more than max_results keys matched
    std::string error;       // valid iff result == Error
};

class NatsBridge {
public:
    NatsBridge(const std::string& host, std::uint16_t port,
               std::chrono::milliseconds op_timeout = std::chrono::milliseconds(3000), int failure_threshold = 3,
               std::chrono::milliseconds open_duration = std::chrono::milliseconds(2000))
        : timeout_(op_timeout),
          failure_threshold_(failure_threshold),
          open_duration_(open_duration),
          work_guard_(asio::make_work_guard(ioc_)) {
        io_thread_ = std::thread([this] { ioc_.run(); });

        conn_ = nats_asio::connect(ioc_, host, port);
        for (int i = 0; i < 100 && !conn_->is_connected(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!conn_->is_connected()) {
            work_guard_.reset();
            ioc_.stop();
            io_thread_.join();
            throw std::runtime_error("NatsBridge: failed to connect to NATS at " + host + ":" + std::to_string(port));
        }
    }

    ~NatsBridge() {
        work_guard_.reset();
        ioc_.stop();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
    }

    NatsBridge(const NatsBridge&) = delete;
    NatsBridge& operator=(const NatsBridge&) = delete;

    // --- non-blocking: on_done runs on the NATS thread ----------------------
    //
    // Templated (rather than std::function<...>) specifically so on_done can
    // be move-only: server_readthrough.cpp's callbacks capture an iceoryx2
    // ActiveRequest, which is move-only by design (see its deleted copy
    // constructor in active_request.hpp), and std::function requires its
    // target to be copy-constructible.

    // on_done: void(NatsGetResult)
    template <typename Callback>
    void get_async(const std::string& bucket, const std::string& key, Callback on_done) {
        if (is_circuit_open()) {
            NatsGetResult result;
            result.result = NatsResult::Error;
            result.error = "circuit breaker open: NATS considered unavailable";
            on_done(std::move(result));
            return;
        }

        auto conn = conn_;
        auto timeout = timeout_;

        // asio::detached means an exception escaping this coroutine calls
        // std::terminate() -- catch everything and fold it into the same
        // Error result the caller already handles, rather than risk taking
        // the whole process down over one bad NATS response.
        asio::co_spawn(
            ioc_,
            [this, conn, bucket, key, timeout, on_done = std::move(on_done)]() mutable -> asio::awaitable<void> {
                // Fields left at their NatsGetResult defaults are simply
                // unused for that branch (documented per-field above) --
                // set explicitly by name rather than positionally, since a
                // 3-value aggregate-init silently shifts which field a
                // literal lands in whenever NatsGetResult gains a member.
                NatsGetResult result;
                try {
                    auto [entry, status] = co_await conn->kv_get(bucket, key, timeout);
                    if (status.ok()) {
                        record_success();
                        result.result = NatsResult::Ok;
                        result.value.assign(entry.value.begin(), entry.value.end());
                        result.revision = entry.revision;
                    } else if (status.code() == nats_asio::error_code::key_not_found) {
                        record_success();
                        result.result = NatsResult::NotFound;
                    } else {
                        record_failure();
                        result.result = NatsResult::Error;
                        result.error = status.error();
                    }
                } catch (const std::exception& e) {
                    record_failure();
                    result.result = NatsResult::Error;
                    result.error = std::string("exception: ") + e.what();
                } catch (...) {
                    record_failure();
                    result.result = NatsResult::Error;
                    result.error = "unknown exception";
                }
                on_done(std::move(result));
                co_return;
            },
            asio::detached);
    }

    // on_done: void(bool ok, std::uint64_t revision, std::string error)
    // `revision` (valid iff ok) is the NATS KV revision this write produced --
    // used by server_dispatch.hpp's RevisionTracker to recognize the daemon's
    // own writes echoed back through its own kv_watch subscription, so it
    // doesn't needlessly invalidate the entry it just wrote.
    template <typename Callback>
    void put_async(const std::string& bucket, const std::string& key, std::vector<std::uint8_t> value,
                    Callback on_done) {
        if (is_circuit_open()) {
            on_done(false, 0, "circuit breaker open: NATS considered unavailable");
            return;
        }

        auto conn = conn_;
        auto timeout = timeout_;

        asio::co_spawn(
            ioc_,
            [this, conn, bucket, key, value, timeout, on_done = std::move(on_done)]() mutable -> asio::awaitable<void> {
                try {
                    std::span<const char> span(reinterpret_cast<const char*>(value.data()), value.size());
                    auto [revision, status] = co_await conn->kv_put(bucket, key, span, timeout);
                    if (status.ok()) {
                        record_success();
                    } else {
                        record_failure();
                    }
                    on_done(status.ok(), revision, status.ok() ? std::string{} : status.error());
                } catch (const std::exception& e) {
                    record_failure();
                    on_done(false, 0, std::string("exception: ") + e.what());
                } catch (...) {
                    record_failure();
                    on_done(false, 0, "unknown exception");
                }
                co_return;
            },
            asio::detached);
    }

    // on_done: void(bool ok, std::uint64_t revision, std::string error)
    // `revision` (valid iff ok) is the NATS KV revision this delete produced --
    // same purpose as put_async()'s, so an erase's negative-cache entry
    // (server_dispatch.hpp) participates in RevisionTracker the same way a
    // positive PUT does.
    template <typename Callback>
    void erase_async(const std::string& bucket, const std::string& key, Callback on_done) {
        if (is_circuit_open()) {
            on_done(false, 0, "circuit breaker open: NATS considered unavailable");
            return;
        }

        auto conn = conn_;
        auto timeout = timeout_;

        asio::co_spawn(
            ioc_,
            [this, conn, bucket, key, timeout, on_done = std::move(on_done)]() mutable -> asio::awaitable<void> {
                try {
                    auto [revision, status] = co_await conn->kv_delete(bucket, key, timeout);
                    if (status.ok()) {
                        record_success();
                    } else {
                        record_failure();
                    }
                    on_done(status.ok(), revision, status.ok() ? std::string{} : status.error());
                } catch (const std::exception& e) {
                    record_failure();
                    on_done(false, 0, std::string("exception: ") + e.what());
                } catch (...) {
                    record_failure();
                    on_done(false, 0, "unknown exception");
                }
                co_return;
            },
            asio::detached);
    }

    // Lists every key matching `key_pattern` (a NATS subject pattern scoped
    // under `bucket` -- e.g. "alice.*" or "alice.>", NOT a plain key) and
    // fetches each one's current value, one at a time, in a single
    // coroutine. Sequential rather than fanned out in parallel: this is a
    // POC-scope tradeoff (see server_dispatch.hpp's GetAll/prefix handling)
    // -- fine for a prefix with a handful of matches, would want
    // parallelizing if some prefix's fan-out gets wide. `max_results` caps
    // how many matched keys are actually fetched (`truncated` reports
    // whether more existed than that).
    //
    // Listing and fetching aren't atomic: a key can be deleted between the
    // kv_keys() call and its kv_get() here, racing with a concurrent writer.
    // Such a key is silently dropped from `entries` rather than failing the
    // whole call -- same "best effort as of roughly now" contract GetAll
    // already has for the local category index (see cache_service.hpp).
    //
    // on_done: void(NatsListResult)
    template <typename Callback>
    void list_and_get_async(const std::string& bucket, const std::string& key_pattern, std::size_t max_results,
                             Callback on_done) {
        if (is_circuit_open()) {
            NatsListResult result;
            result.result = NatsResult::Error;
            result.error = "circuit breaker open: NATS considered unavailable";
            on_done(std::move(result));
            return;
        }

        auto conn = conn_;
        auto timeout = timeout_;

        asio::co_spawn(
            ioc_,
            [this, conn, bucket, key_pattern, timeout, max_results,
             on_done = std::move(on_done)]() mutable -> asio::awaitable<void> {
                NatsListResult result;
                try {
                    auto [keys, key_status] = co_await conn->kv_keys(bucket, key_pattern, timeout);
                    if (!key_status.ok()) {
                        record_failure();
                        result.result = NatsResult::Error;
                        result.error = key_status.error();
                        on_done(std::move(result));
                        co_return;
                    }

                    result.truncated = keys.size() > max_results;
                    if (result.truncated) {
                        keys.resize(max_results);
                    }

                    for (const auto& key : keys) {
                        auto [entry, get_status] = co_await conn->kv_get(bucket, key, timeout);
                        if (get_status.ok()) {
                            result.entries.emplace_back(
                                key, std::vector<std::uint8_t>(entry.value.begin(), entry.value.end()));
                        }
                        // A miss/error on one key (raced with a delete, or a
                        // transient hiccup) doesn't fail the whole batch --
                        // it's simply excluded, same as it would be if it
                        // hadn't matched the pattern at all.
                    }
                    record_success();
                    result.result = NatsResult::Ok;
                } catch (const std::exception& e) {
                    record_failure();
                    result.result = NatsResult::Error;
                    result.error = std::string("exception: ") + e.what();
                } catch (...) {
                    record_failure();
                    result.result = NatsResult::Error;
                    result.error = "unknown exception";
                }
                on_done(std::move(result));
                co_return;
            },
            asio::detached);
    }

    // --- blocking: for simple sequential callers (client_readthrough.cpp) --

    NatsGetResult get(const std::string& bucket, const std::string& key) {
        auto prom = std::make_shared<std::promise<NatsGetResult>>();
        auto fut = prom->get_future();
        get_async(bucket, key, [prom](NatsGetResult result) { prom->set_value(std::move(result)); });
        return fut.get();
    }

    // Returns {ok, error message (empty if ok)}. Discards the revision
    // put_async()'s callback carries -- callers who need it (server_dispatch.hpp)
    // use put_async() directly; the blocking callers of this wrapper
    // (client_readthrough.cpp, most nats_bridge_test.cpp cases) don't.
    std::pair<bool, std::string> put(const std::string& bucket, const std::string& key,
                                      const std::vector<std::uint8_t>& value) {
        auto prom = std::make_shared<std::promise<std::pair<bool, std::string>>>();
        auto fut = prom->get_future();
        put_async(bucket, key, value, [prom](bool ok, std::uint64_t /*revision*/, std::string err) {
            prom->set_value({ok, std::move(err)});
        });
        return fut.get();
    }

    // Returns {ok, error message (empty if ok)}. Discards the revision
    // erase_async()'s callback carries, same as put()'s wrapper above.
    std::pair<bool, std::string> erase(const std::string& bucket, const std::string& key) {
        auto prom = std::make_shared<std::promise<std::pair<bool, std::string>>>();
        auto fut = prom->get_future();
        erase_async(bucket, key, [prom](bool ok, std::uint64_t /*revision*/, std::string err) {
            prom->set_value({ok, std::move(err)});
        });
        return fut.get();
    }

    // Blocking wrapper around list_and_get_async(), same pattern as get()
    // above wraps get_async(). See list_and_get_async()'s own doc comment
    // for what it does and its known limitations.
    NatsListResult list_and_get(const std::string& bucket, const std::string& key_pattern,
                                 std::size_t max_results) {
        auto prom = std::make_shared<std::promise<NatsListResult>>();
        auto fut = prom->get_future();
        list_and_get_async(bucket, key_pattern, max_results,
                            [prom](NatsListResult result) { prom->set_value(std::move(result)); });
        return fut.get();
    }

    // --- cross-daemon coherence: watch every key change in a bucket --------

    // Subscribes to every key change (put/delete/purge) in `bucket` for the
    // lifetime of this NatsBridge; `on_entry` runs on the NATS thread once
    // per change event -- callers must not touch a multi_index_lru::Container
    // from inside it directly, same rule as get_async()/put_async()/
    // erase_async()'s on_done. Note: NATS delivers the *current* value of
    // every existing key in the bucket immediately upon subscribing (not
    // just future changes), so expect a burst of events right after this
    // call returns.
    //
    // Meant to be called once per bucket during daemon startup, not per
    // request -- blocks until the underlying subscription is confirmed
    // established, or throws if it couldn't be.
    template <typename Callback>
    void watch(const std::string& bucket, Callback on_entry) {
        auto conn = conn_;
        auto prom = std::make_shared<std::promise<bool>>();
        auto fut = prom->get_future();

        asio::co_spawn(
            ioc_,
            [this, conn, bucket, on_entry = std::move(on_entry), prom]() mutable -> asio::awaitable<void> {
                auto [watcher, status] = co_await conn->kv_watch(
                    bucket,
                    [on_entry](const nats_asio::kv_entry& entry) -> asio::awaitable<void> {
                        on_entry(entry);
                        co_return;
                    },
                    "");  // empty key = watch every key in the bucket
                if (status.ok()) {
                    watchers_.push_back(std::move(watcher));
                }
                prom->set_value(status.ok());
                co_return;
            },
            asio::detached);

        if (!fut.get()) {
            throw std::runtime_error("NatsBridge: failed to start watch on bucket " + bucket);
        }
    }

    // --- circuit breaker introspection (mainly for tests) -------------------

    [[nodiscard]] bool is_circuit_open() const {
        const auto opened_at = circuit_opened_at_.load();
        if (opened_at == std::chrono::steady_clock::time_point::min()) {
            return false;
        }
        return std::chrono::steady_clock::now() - opened_at < open_duration_;
    }

private:
    // NotFound is a real answer from a healthy NATS, not a failure.
    void record_success() {
        consecutive_failures_.store(0);
        circuit_opened_at_.store(std::chrono::steady_clock::time_point::min());
    }

    void record_failure() {
        if (consecutive_failures_.fetch_add(1) + 1 >= failure_threshold_) {
            circuit_opened_at_.store(std::chrono::steady_clock::now());
        }
    }

    asio::io_context ioc_;
    std::chrono::milliseconds timeout_;
    int failure_threshold_;
    std::chrono::milliseconds open_duration_;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
    std::thread io_thread_;
    nats_asio::iconnection_sptr conn_;
    std::atomic<int> consecutive_failures_{0};
    std::atomic<std::chrono::steady_clock::time_point> circuit_opened_at_{std::chrono::steady_clock::time_point::min()};
    // Keeps each watch() subscription alive; only ever touched from the NATS
    // thread (inside watch()'s own coroutine, at subscription-setup time).
    std::vector<nats_asio::ikv_watcher_sptr> watchers_;
};

}  // namespace poc
