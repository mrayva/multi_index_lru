/// POC: multi_index_lru served over iceoryx2 request-response, purely
/// in-memory (no backing store).
///
/// Owns two independent multi_index_lru::Container instances -- one keyed by
/// string, one keyed by int64 -- and answers get/put/erase requests from
/// other processes. Request/response bodies are the small binary protocol
/// defined in cache_service.hpp / wire.hpp.
///
/// This process is the only one that touches either container directly, so
/// the documented single-threaded/no-internal-locking constraints of
/// multi_index_lru (see README.md "Thread Safety") are respected: exactly
/// one owner, other processes only ever go through this request-response
/// service.
///
/// See server_readthrough.cpp for the variant that falls through to a NATS
/// JetStream KV bucket on a local miss.
#include "cache_service.hpp"

#include "iox2/iceoryx2.hpp"

#include <iostream>

namespace {

constexpr iox2::bb::Duration kCycleTime = iox2::bb::Duration::from_millis(100);
constexpr std::uint64_t kInitialSliceLenHint = 256;

std::vector<std::uint8_t> to_bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

}  // namespace

int main() {
    using namespace iox2;

    set_log_level_from_env_or(LogLevel::Info);
    std::cout << std::unitbuf;  // flush every line -- this process is normally run with stdout redirected to a log file

    // --- seed the caches this process owns --------------------------------
    poc::NameCache name_cache(1000);
    name_cache.emplace(poc::NameEntry{"alice", to_bytes(R"({"name":"Alice"})")});
    name_cache.emplace(poc::NameEntry{"bob", to_bytes(R"({"name":"Bob"})")});

    poc::IdCache id_cache(1000);
    id_cache.emplace(poc::IdEntry{1, to_bytes(R"({"id":1,"name":"Alice"})")});
    id_cache.emplace(poc::IdEntry{2, to_bytes(R"({"id":2,"name":"Bob"})")});

    // --- set up the iceoryx2 side ----------------------------------------
    auto node = NodeBuilder().create<ServiceType::Ipc>().value();

    auto service = node.service_builder(ServiceName::create(poc::kServiceName).value())
                        .request_response<bb::Slice<std::uint8_t>, bb::Slice<std::uint8_t>>()
                        .open_or_create()
                        .value();

    auto server = service.server_builder()
                      .initial_max_slice_len(kInitialSliceLenHint)
                      .allocation_strategy(AllocationStrategy::PowerOfTwo)
                      .create()
                      .value();

    std::cout << "[server] cache daemon ready, service \"" << poc::kServiceName << "\", "
              << name_cache.size() << " name-keyed + " << id_cache.size() << " id-keyed entries loaded. "
              << "Ctrl+C to stop.\n";

    while (node.wait(kCycleTime).has_value()) {
        while (true) {
            auto active_request = server.receive().value();
            if (!active_request.has_value()) {
                break;
            }

            const auto& request_payload = active_request->payload();
            auto response_bytes = poc::handle_request_local(
                name_cache, id_cache, request_payload.data(), request_payload.number_of_bytes());
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
