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
    std::string error;                // valid iff result == Error
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
            on_done(NatsGetResult{NatsResult::Error, {}, "circuit breaker open: NATS considered unavailable"});
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
                try {
                    auto [entry, status] = co_await conn->kv_get(bucket, key, timeout);
                    if (status.ok()) {
                        record_success();
                        on_done(NatsGetResult{
                            NatsResult::Ok, std::vector<std::uint8_t>(entry.value.begin(), entry.value.end()), {}});
                    } else if (status.code() == nats_asio::error_code::key_not_found) {
                        record_success();
                        on_done(NatsGetResult{NatsResult::NotFound, {}, {}});
                    } else {
                        record_failure();
                        on_done(NatsGetResult{NatsResult::Error, {}, status.error()});
                    }
                } catch (const std::exception& e) {
                    record_failure();
                    on_done(NatsGetResult{NatsResult::Error, {}, std::string("exception: ") + e.what()});
                } catch (...) {
                    record_failure();
                    on_done(NatsGetResult{NatsResult::Error, {}, "unknown exception"});
                }
                co_return;
            },
            asio::detached);
    }

    // on_done: void(bool ok, std::string error)
    template <typename Callback>
    void put_async(const std::string& bucket, const std::string& key, std::vector<std::uint8_t> value,
                    Callback on_done) {
        if (is_circuit_open()) {
            on_done(false, "circuit breaker open: NATS considered unavailable");
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
                    (void)revision;
                    if (status.ok()) {
                        record_success();
                    } else {
                        record_failure();
                    }
                    on_done(status.ok(), status.ok() ? std::string{} : status.error());
                } catch (const std::exception& e) {
                    record_failure();
                    on_done(false, std::string("exception: ") + e.what());
                } catch (...) {
                    record_failure();
                    on_done(false, "unknown exception");
                }
                co_return;
            },
            asio::detached);
    }

    // on_done: void(bool ok, std::string error)
    template <typename Callback>
    void erase_async(const std::string& bucket, const std::string& key, Callback on_done) {
        if (is_circuit_open()) {
            on_done(false, "circuit breaker open: NATS considered unavailable");
            return;
        }

        auto conn = conn_;
        auto timeout = timeout_;

        asio::co_spawn(
            ioc_,
            [this, conn, bucket, key, timeout, on_done = std::move(on_done)]() mutable -> asio::awaitable<void> {
                try {
                    auto [revision, status] = co_await conn->kv_delete(bucket, key, timeout);
                    (void)revision;
                    if (status.ok()) {
                        record_success();
                    } else {
                        record_failure();
                    }
                    on_done(status.ok(), status.ok() ? std::string{} : status.error());
                } catch (const std::exception& e) {
                    record_failure();
                    on_done(false, std::string("exception: ") + e.what());
                } catch (...) {
                    record_failure();
                    on_done(false, "unknown exception");
                }
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

    // Returns {ok, error message (empty if ok)}.
    std::pair<bool, std::string> put(const std::string& bucket, const std::string& key,
                                      const std::vector<std::uint8_t>& value) {
        auto prom = std::make_shared<std::promise<std::pair<bool, std::string>>>();
        auto fut = prom->get_future();
        put_async(bucket, key, value,
                  [prom](bool ok, std::string err) { prom->set_value({ok, std::move(err)}); });
        return fut.get();
    }

    // Returns {ok, error message (empty if ok)}.
    std::pair<bool, std::string> erase(const std::string& bucket, const std::string& key) {
        auto prom = std::make_shared<std::promise<std::pair<bool, std::string>>>();
        auto fut = prom->get_future();
        erase_async(bucket, key, [prom](bool ok, std::string err) { prom->set_value({ok, std::move(err)}); });
        return fut.get();
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
};

}  // namespace poc
