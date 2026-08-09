/// Load-test driver for cache_poc_server / cache_poc_server_readthrough:
/// fires many concurrent requests at a running daemon, keeping roughly
/// --concurrency requests in flight at once, and reports throughput and
/// latency percentiles.
///
/// Not a unit test -- start a server first (in another terminal), then run
/// this against it. See README.md "Load testing" for methodology, the
/// scenarios this was actually run against, and what they found.
///
/// iceoryx2 caps how many requests a single Client can have in flight at
/// once (service.static_config().max_active_requests_per_client(), 4 by
/// default -- see README.md "Load testing" for how this was discovered).
/// To reach --concurrency anyway, this creates enough Client instances to
/// spread the load across, round-robining sends over whichever currently
/// has room -- which also happens to model the real world more faithfully
/// than one Client with an inflated cap would: production traffic comes
/// from many separate client processes/connections, not one client with
/// hundreds of requests open at once.
///
/// The interesting scenario this exists for: --op put --key-pool-size 1
/// forces every single request to serialize behind the previous one for
/// that same key (server_dispatch.hpp's KeyOperationQueue), each waiting
/// out a full NATS round trip -- deliberately reproducing the "long queue
/// piled up behind one slow/stuck key" backlog server_readthrough.cpp's
/// design had never been measured under (see README.md "What this doesn't
/// answer yet" > "Throughput under sustained load").
///
/// Configurable via CLI flag or env var (flag wins if both are given):
///   --service-name / MIL_SERVICE_NAME       (default poc::kServiceName)
///   --concurrency / MIL_LOAD_CONCURRENCY     (default 50 -- requests kept in flight at once,
///                                              spread across as many Clients as needed)
///   --total-requests / MIL_LOAD_TOTAL        (default 5000)
///   --key-pool-size / MIL_LOAD_KEY_POOL      (default 200 -- distinct keys round-robined
///                                              over; 1 forces every request onto the same
///                                              key, see above)
///   --op / MIL_LOAD_OP                       (get|put, default get)
#include "cache_service.hpp"
#include "config.hpp"

#include "iox2/iceoryx2.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace iox2;

// Small on purpose: node.wait() is a plain sleep-for-up-to-this-long poll
// gate, not an event-driven wait woken by response arrival (see Node::wait's
// doc comment), so a large cycle time here would cap this tool's own
// measured throughput/latency rather than the server's. server_readthrough.cpp
// itself still polls at its own (larger, 5ms) cycle time, which the results
// below are read against -- see README.md "Load testing" for that caveat.
constexpr bb::Duration kCycleTime = bb::Duration::from_micros(200);
constexpr std::uint64_t kInitialSliceLenHint = 256;

using ClientType = Client<ServiceType::Ipc, bb::Slice<std::uint8_t>, void, bb::Slice<std::uint8_t>, void>;
using PendingResponseType =
    PendingResponse<ServiceType::Ipc, bb::Slice<std::uint8_t>, void, bb::Slice<std::uint8_t>, void>;

struct InFlight {
    PendingResponseType pending;
    std::chrono::steady_clock::time_point sent_at;
    std::size_t client_idx;
};

// `sorted_us` must already be sorted ascending.
double percentile(const std::vector<double>& sorted_us, double p) {
    if (sorted_us.empty()) {
        return 0.0;
    }
    const auto idx = static_cast<std::size_t>(p * static_cast<double>(sorted_us.size() - 1));
    return sorted_us[idx];
}

}  // namespace

int main(int argc, char** argv) {
    set_log_level_from_env_or(LogLevel::Warn);
    std::cout << std::unitbuf;

    std::vector<std::string> args(argv + 1, argv + argc);
    const std::string service_name = poc::config::resolve_str(args, "--service-name", "MIL_SERVICE_NAME", poc::kServiceName);
    const std::size_t concurrency = poc::config::resolve_size(args, "--concurrency", "MIL_LOAD_CONCURRENCY", 50);
    const std::size_t total_requests = poc::config::resolve_size(args, "--total-requests", "MIL_LOAD_TOTAL", 5000);
    const std::size_t key_pool_size = poc::config::resolve_size(args, "--key-pool-size", "MIL_LOAD_KEY_POOL", 200);
    const std::string op = poc::config::resolve_str(args, "--op", "MIL_LOAD_OP", "get");

    if (op != "get" && op != "put") {
        std::cerr << "--op must be \"get\" or \"put\"\n";
        return 1;
    }
    if (key_pool_size == 0 || concurrency == 0 || total_requests == 0) {
        std::cerr << "--key-pool-size, --concurrency, and --total-requests must all be > 0\n";
        return 1;
    }

    auto node = NodeBuilder().create<ServiceType::Ipc>().value();
    auto service = node.service_builder(ServiceName::create(service_name.c_str()).value())
                        .request_response<bb::Slice<std::uint8_t>, bb::Slice<std::uint8_t>>()
                        .open_or_create()
                        .value();

    // iceoryx2 caps both how many requests one Client can have in flight
    // (max_active_requests_per_client) and how many Clients can connect to
    // one Service at all (max_clients) -- both are properties of the
    // *service*, fixed at whichever daemon first created it (server.cpp /
    // server_readthrough.cpp use the iceoryx2-cxx defaults, 4 and 10 as of
    // this writing -- see README.md "Load testing"). The achievable
    // concurrency this tool can actually reach against an unmodified daemon
    // is therefore capped at their product, regardless of --concurrency.
    const auto max_active_per_client = service.static_config().max_active_requests_per_client();
    const auto max_clients = service.static_config().max_clients();
    const auto max_reachable_concurrency = max_active_per_client * max_clients;
    const auto effective_concurrency = std::min(concurrency, max_reachable_concurrency);
    const auto num_clients = (effective_concurrency + max_active_per_client - 1) / max_active_per_client;

    std::vector<ClientType> clients;
    clients.reserve(num_clients);
    for (std::size_t i = 0; i < num_clients; ++i) {
        clients.push_back(service.client_builder()
                               .initial_max_slice_len(kInitialSliceLenHint)
                               .allocation_strategy(AllocationStrategy::PowerOfTwo)
                               .create()
                               .value());
    }
    std::vector<std::size_t> client_in_flight(num_clients, 0);

    std::cout << "[load] service=\"" << service_name << "\" op=" << op << " concurrency=" << concurrency
              << " total_requests=" << total_requests << " key_pool_size=" << key_pool_size << "\n"
              << "[load] max_active_requests_per_client=" << max_active_per_client << " max_clients=" << max_clients
              << " -> max reachable concurrency=" << max_reachable_concurrency << ", using "
              << effective_concurrency << " across " << num_clients << " client(s)"
              << (effective_concurrency < concurrency ? " (clamped down from --concurrency)" : "") << "\n";

    const std::vector<std::uint8_t> put_record(64, 'x');  // fixed-size filler payload for PUT load
    std::size_t sent = 0;
    std::size_t send_failures = 0;

    // Round-robins over clients that still have room under
    // max_active_requests_per_client; returns false (and counts a failure)
    // if every client is currently full, which shouldn't happen given
    // `concurrency` is spread across enough clients to cover it, but a
    // load-test tool crashing under load defeats its own purpose, so this
    // stays a soft failure rather than an unwrap.
    auto send_one = [&](std::vector<InFlight>& in_flight) -> bool {
        const auto key = "loadtest_" + std::to_string(sent % key_pool_size);
        const std::vector<std::uint8_t> request_bytes = (op == "get")
                                                              ? poc::encode_get(poc::wire::KeyKind::Name, key, 0)
                                                              : poc::encode_put(poc::wire::KeyKind::Name, key, 0, put_record);

        for (std::size_t c = 0; c < num_clients; ++c) {
            if (client_in_flight[c] >= max_active_per_client) {
                continue;
            }
            auto request = clients[c].loan_slice_uninit(request_bytes.size()).value();
            auto initialized = request.write_from_fn([&](auto byte_idx) { return request_bytes[byte_idx]; });
            auto pending = send(std::move(initialized));
            if (!pending.has_value()) {
                ++send_failures;
                return false;
            }
            in_flight.push_back(InFlight{std::move(pending).value(), std::chrono::steady_clock::now(), c});
            ++client_in_flight[c];
            ++sent;
            return true;
        }
        ++send_failures;
        return false;
    };

    std::vector<InFlight> in_flight;
    in_flight.reserve(effective_concurrency);
    while (sent < total_requests && in_flight.size() < effective_concurrency) {
        if (!send_one(in_flight)) {
            break;
        }
    }

    std::size_t completed = 0;
    std::size_t ok_count = 0;
    std::size_t not_found_count = 0;
    std::size_t error_count = 0;
    std::vector<double> latencies_us;
    latencies_us.reserve(total_requests);

    const auto start = std::chrono::steady_clock::now();
    while (completed < total_requests && (!in_flight.empty() || sent < total_requests)) {
        if (!node.wait(kCycleTime).has_value()) {
            std::cerr << "[load] node.wait failed / interrupted after " << completed << "/" << total_requests
                      << " completed\n";
            break;
        }
        for (std::size_t i = 0; i < in_flight.size();) {
            auto response = in_flight[i].pending.receive().value();
            if (!response.has_value()) {
                ++i;
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            latencies_us.push_back(std::chrono::duration<double, std::micro>(now - in_flight[i].sent_at).count());

            const auto& payload = response->payload();
            const auto status = static_cast<poc::wire::Status>(payload.data()[0]);
            if (status == poc::wire::Status::Ok) {
                ++ok_count;
            } else if (status == poc::wire::Status::NotFound) {
                ++not_found_count;
            } else {
                ++error_count;
            }

            --client_in_flight[in_flight[i].client_idx];
            ++completed;
            in_flight[i] = std::move(in_flight.back());
            in_flight.pop_back();
            if (sent < total_requests) {
                send_one(in_flight);
            }
            // Doesn't advance `i`: the swapped-in element at this index
            // still needs to be checked.
        }
    }
    const auto end = std::chrono::steady_clock::now();

    const auto wall_seconds = std::chrono::duration<double>(end - start).count();
    std::sort(latencies_us.begin(), latencies_us.end());

    std::cout << "\n[load] === results ===\n"
              << "[load] completed: " << completed << " (ok=" << ok_count << " not_found=" << not_found_count
              << " error=" << error_count << ", send_failures=" << send_failures << ")\n"
              << "[load] wall time: " << wall_seconds << "s\n"
              << "[load] throughput: " << (wall_seconds > 0.0 ? static_cast<double>(completed) / wall_seconds : 0.0)
              << " req/s\n"
              << "[load] latency (us): min=" << (latencies_us.empty() ? 0.0 : latencies_us.front())
              << " p50=" << percentile(latencies_us, 0.50) << " p95=" << percentile(latencies_us, 0.95)
              << " p99=" << percentile(latencies_us, 0.99)
              << " max=" << (latencies_us.empty() ? 0.0 : latencies_us.back()) << "\n";

    return (error_count > 0 || send_failures > 0) ? 1 : 0;
}
