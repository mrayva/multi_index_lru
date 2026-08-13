/// POC: multi_index_lru served over iceoryx2 request-response, purely
/// in-memory (no backing store).
///
/// Owns three independent multi_index_lru caches -- one keyed by string, one
/// by int64, and one by security_cache.hpp's 5-field (asset_type, cusip,
/// isin, sedol, ric) composite key -- and answers get/put/erase requests
/// from other processes. Request/response bodies are the small binary
/// protocol defined in cache_service.hpp / wire.hpp.
///
/// This process is the only one that touches either container directly, so
/// the documented single-threaded/no-internal-locking constraints of
/// multi_index_lru (see README.md "Thread Safety") are respected: exactly
/// one owner, other processes only ever go through this request-response
/// service.
///
/// See server_readthrough.cpp for the variant that falls through to a NATS
/// JetStream KV bucket on a local miss.
///
/// Configurable via CLI flag or env var (flag wins if both are given):
///   --service-name / MIL_SERVICE_NAME    (default poc::kServiceName)
///   --cache-capacity / MIL_CACHE_CAPACITY (default 1000, applies to both caches)
///   --ttl-ms / MIL_TTL_MS                 (default 300000 = 5min, applies to both caches)
///   --max-clients / MIL_MAX_CLIENTS       (default 16 -- iceoryx2-cxx's own default is 8;
///                                           see README.md "Load testing" for why this is raised)
///   --max-active-requests-per-client / MIL_MAX_ACTIVE_REQUESTS_PER_CLIENT
///                                          (default 32 -- iceoryx2-cxx's own default is 4)
///   --max-getall-results / MIL_MAX_GETALL_RESULTS
///                                          (default 100 -- caps a single GetAll response;
///                                           see cache_service.hpp "Non-unique key lookup (GetAll)")
#include "cache_service.hpp"
#include "config.hpp"

#include "iox2/iceoryx2.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr iox2::bb::Duration kCycleTime = iox2::bb::Duration::from_millis(100);
constexpr std::uint64_t kInitialSliceLenHint = 256;
// How often the main loop reclaims capacity slots held by expired-but-
// unaccessed entries -- see cleanup_expired_periodically() in
// cache_service.hpp.
constexpr auto kCleanupInterval = std::chrono::seconds(1);

std::vector<std::uint8_t> to_bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

}  // namespace

int main(int argc, char** argv) {
    using namespace iox2;

    set_log_level_from_env_or(LogLevel::Info);
    std::cout << std::unitbuf;  // flush every line -- this process is normally run with stdout redirected to a log file

    std::vector<std::string> args(argv + 1, argv + argc);
    const std::string service_name = poc::config::resolve_str(args, "--service-name", "MIL_SERVICE_NAME", poc::kServiceName);
    const std::size_t capacity = poc::config::resolve_size(args, "--cache-capacity", "MIL_CACHE_CAPACITY", 1000);
    const auto ttl = poc::config::resolve_millis(args, "--ttl-ms", "MIL_TTL_MS", 300000);
    const std::size_t max_clients = poc::config::resolve_size(args, "--max-clients", "MIL_MAX_CLIENTS", 16);
    const std::size_t max_active_requests_per_client = poc::config::resolve_size(
        args, "--max-active-requests-per-client", "MIL_MAX_ACTIVE_REQUESTS_PER_CLIENT", 32);
    const std::size_t max_getall_results =
        poc::config::resolve_size(args, "--max-getall-results", "MIL_MAX_GETALL_RESULTS", 100);

    // --- seed the caches this process owns --------------------------------
    poc::NameCache name_cache(capacity, ttl);
    name_cache.emplace(poc::NameEntry{"alice", to_bytes(R"({"name":"Alice"})"), true, "friends"});
    name_cache.emplace(poc::NameEntry{"bob", to_bytes(R"({"name":"Bob"})"), true, "friends"});
    name_cache.emplace(poc::NameEntry{"carol", to_bytes(R"({"name":"Carol"})"), true, "coworkers"});

    poc::IdCache id_cache(capacity, ttl);
    id_cache.emplace(poc::IdEntry{1, to_bytes(R"({"id":1,"name":"Alice"})")});
    id_cache.emplace(poc::IdEntry{2, to_bytes(R"({"id":2,"name":"Bob"})")});

    poc::SecurityCache security_cache(capacity, ttl);
    security_cache.emplace(poc::SecurityEntry{"EQUITY", "037833100", "US0378331005", "2046251", "AAPL_OQ",
                                                to_bytes(R"({"name":"Apple Inc."})")});
    security_cache.emplace(poc::SecurityEntry{"EQUITY", "594918104", "US5949181045", "2588173", "MSFT_OQ",
                                                to_bytes(R"({"name":"Microsoft Corp."})")});
    auto last_cleanup = std::chrono::steady_clock::now();

    // --- set up the iceoryx2 side ----------------------------------------
    auto node = NodeBuilder().create<ServiceType::Ipc>().value();

    auto service = node.service_builder(ServiceName::create(service_name.c_str()).value())
                        .request_response<bb::Slice<std::uint8_t>, bb::Slice<std::uint8_t>>()
                        .max_clients(max_clients)
                        .max_active_requests_per_client(max_active_requests_per_client)
                        .open_or_create()
                        .value();

    auto server = service.server_builder()
                      .initial_max_slice_len(kInitialSliceLenHint)
                      .allocation_strategy(AllocationStrategy::PowerOfTwo)
                      .create()
                      .value();

    std::cout << "[server] cache daemon ready, service \"" << service_name << "\", "
              << name_cache.size() << " name-keyed + " << id_cache.size() << " id-keyed entries loaded. "
              << "Ctrl+C to stop.\n";

    while (node.wait(kCycleTime).has_value()) {
        poc::cleanup_expired_periodically(name_cache, id_cache, security_cache, last_cleanup, kCleanupInterval);

        while (true) {
            auto active_request = server.receive().value();
            if (!active_request.has_value()) {
                break;
            }

            const auto& request_payload = active_request->payload();
            auto response_bytes = poc::handle_request_local(
                name_cache, id_cache, security_cache, request_payload.data(), request_payload.number_of_bytes(),
                max_getall_results);
            std::cout << "[server] handled request (" << request_payload.number_of_bytes() << " bytes in, "
                      << response_bytes.size() << " bytes out), caches now have " << name_cache.size()
                      << " name-keyed + " << id_cache.size() << " id-keyed entries\n";

            auto response = active_request->loan_slice_uninit(response_bytes.size()).value();
            auto initialized = response.write_from_fn([&](auto byte_idx) { return response_bytes[byte_idx]; });
            send(std::move(initialized)).value();
        }
    }

    std::cout << "[server] exit\n";
    return 0;
}
