/// Blocking bridge from a synchronous call site to mrayva/nats_asio's
/// ASIO-coroutine-based NATS client.
///
/// nats_asio runs entirely as `asio::awaitable<...>` coroutines driven by an
/// `asio::io_context`; the iceoryx2 request loop in server_readthrough.cpp is
/// a plain synchronous poll loop with no event loop of its own. This class
/// runs the io_context on a dedicated background thread and turns each KV
/// operation into a blocking call: `get()`/`put()`/`erase()` spawn a
/// coroutine on that thread and block the *caller's* thread (via
/// std::promise/std::future) until it completes.
///
/// That keeps server_readthrough.cpp's caches single-owner/single-writer
/// (only its main thread ever touches them, so multi_index_lru's "not
/// thread-safe, no internal locking" contract is respected) at the cost of
/// stalling every other pending client request for the duration of a NATS
/// round trip. See README.md "What this doesn't answer yet" for the
/// non-blocking alternative this POC deliberately doesn't attempt.
#pragma once

#include <nats_asio/nats_asio.hpp>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

#include <chrono>
#include <cstdint>
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
               std::chrono::milliseconds op_timeout = std::chrono::milliseconds(3000))
        : timeout_(op_timeout), work_guard_(asio::make_work_guard(ioc_)) {
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

    NatsGetResult get(const std::string& bucket, const std::string& key) {
        auto prom = std::make_shared<std::promise<NatsGetResult>>();
        auto fut = prom->get_future();
        auto conn = conn_;
        auto timeout = timeout_;

        asio::co_spawn(
            ioc_,
            [conn, bucket, key, timeout, prom]() -> asio::awaitable<void> {
                auto [entry, status] = co_await conn->kv_get(bucket, key, timeout);
                if (status.ok()) {
                    prom->set_value(NatsGetResult{
                        NatsResult::Ok, std::vector<std::uint8_t>(entry.value.begin(), entry.value.end()), {}});
                } else if (status.code() == nats_asio::error_code::key_not_found) {
                    prom->set_value(NatsGetResult{NatsResult::NotFound, {}, {}});
                } else {
                    prom->set_value(NatsGetResult{NatsResult::Error, {}, status.error()});
                }
                co_return;
            },
            asio::detached);

        return fut.get();
    }

    // Returns {ok, error message (empty if ok)}.
    std::pair<bool, std::string> put(const std::string& bucket, const std::string& key,
                                      const std::vector<std::uint8_t>& value) {
        auto prom = std::make_shared<std::promise<std::pair<bool, std::string>>>();
        auto fut = prom->get_future();
        auto conn = conn_;
        auto timeout = timeout_;

        asio::co_spawn(
            ioc_,
            [conn, bucket, key, value, timeout, prom]() -> asio::awaitable<void> {
                std::span<const char> span(reinterpret_cast<const char*>(value.data()), value.size());
                auto [revision, status] = co_await conn->kv_put(bucket, key, span, timeout);
                (void)revision;
                prom->set_value({status.ok(), status.ok() ? std::string{} : status.error()});
                co_return;
            },
            asio::detached);

        return fut.get();
    }

    // Returns {ok, error message (empty if ok)}.
    std::pair<bool, std::string> erase(const std::string& bucket, const std::string& key) {
        auto prom = std::make_shared<std::promise<std::pair<bool, std::string>>>();
        auto fut = prom->get_future();
        auto conn = conn_;
        auto timeout = timeout_;

        asio::co_spawn(
            ioc_,
            [conn, bucket, key, timeout, prom]() -> asio::awaitable<void> {
                auto [revision, status] = co_await conn->kv_delete(bucket, key, timeout);
                (void)revision;
                prom->set_value({status.ok(), status.ok() ? std::string{} : status.error()});
                co_return;
            },
            asio::detached);

        return fut.get();
    }

private:
    asio::io_context ioc_;
    std::chrono::milliseconds timeout_;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
    std::thread io_thread_;
    nats_asio::iconnection_sptr conn_;
};

}  // namespace poc
