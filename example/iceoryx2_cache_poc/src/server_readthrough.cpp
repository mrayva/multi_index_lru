/// POC: multi_index_lru as a cache-aside / read-through + write-through
/// layer in front of NATS JetStream KV, served over the same iceoryx2
/// request-response protocol as server.cpp (cache_poc_client talks to
/// either).
///
/// Two independent Container instances (string-keyed / int64-keyed), each
/// backed by its own NATS KV bucket:
///   - name cache -> bucket "mil_by_name"
///   - id cache   -> bucket "mil_by_id"
///
/// Both buckets must already exist -- nats_asio has no bucket-creation call,
/// see README.md for the one-time `nats kv add` setup commands.
///
/// GET: a local hit responds immediately. A local miss kicks off an async
/// NATS JetStream KV `kv_get` and returns without responding; the response
/// (and, on a NATS hit, populating the local cache -- read-through) happens
/// once that completes, via CompletionQueue (server_dispatch.hpp). PUT and
/// ERASE are write-through and always go to NATS: `kv_put`/`kv_delete` is
/// kicked off async, and the local cache is only touched -- and the response
/// only sent -- once that NATS write completes, so the cache never holds
/// something NATS doesn't durably have.
///
/// Nothing here blocks the main iceoryx2 request loop on a NATS round trip:
/// while one client's GET/PUT/ERASE is waiting on NATS, the loop keeps
/// receiving and dispatching every other client's requests. See
/// "the concurrency model" in README.md for how this differs from (and
/// improves on) the single-threaded blocking-bridge design this replaced.
///
/// The actual request-dispatch logic lives in server_dispatch.hpp, pulled
/// out of this file so test/dispatch_request_test.cpp can drive it directly.
#include "server_dispatch.hpp"

#include "iox2/iceoryx2.hpp"

#include <iostream>

namespace {
constexpr iox2::bb::Duration kCycleTime = iox2::bb::Duration::from_millis(5);
constexpr std::uint64_t kInitialSliceLenHint = 256;
}  // namespace

int main() {
    using namespace iox2;

    set_log_level_from_env_or(LogLevel::Info);
    std::cout << std::unitbuf;  // flush every line -- this process is normally run with stdout redirected to a log file

    std::cout << "[server] connecting to NATS at 127.0.0.1:4222 ...\n";
    poc::NatsBridge nats("127.0.0.1", 4222);
    std::cout << "[server] connected to NATS\n";

    poc::NameCache name_cache(1000);
    poc::IdCache id_cache(1000);
    poc::CompletionQueue completions;

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

    std::cout << "[server] read-through cache daemon ready (non-blocking), service \"" << poc::kServiceName
              << "\", buckets \"" << poc::kNameBucket << "\" / \"" << poc::kIdBucket << "\". Ctrl+C to stop.\n";

    while (node.wait(kCycleTime).has_value()) {
        // Respond to whatever NATS operations finished since the last cycle
        // before looking at new requests, so deferred requests don't wait
        // behind a burst of fresh local-hit ones.
        for (auto& completion : completions.drain()) {
            auto response_bytes = completion.apply(name_cache, id_cache);
            const auto status = static_cast<poc::wire::Status>(response_bytes[0]);
            std::cout << "[server] " << completion.description << " -> "
                      << (status == poc::wire::Status::Ok       ? "ok (NATS)"
                          : status == poc::wire::Status::NotFound ? "not found (local miss, NATS miss)"
                                                                    : "error (NATS)")
                      << "\n";
            poc::respond(completion.active_request, response_bytes);
        }

        while (true) {
            auto active_request_opt = server.receive().value();
            if (!active_request_opt.has_value()) {
                break;
            }

            const auto& payload = active_request_opt->payload();
            std::vector<std::uint8_t> request_bytes(payload.data(), payload.data() + payload.number_of_bytes());

            poc::dispatch_request(name_cache, id_cache, nats, completions, std::move(active_request_opt.value()),
                                   request_bytes.data(), request_bytes.size());
        }
    }

    std::cout << "[server] exit\n";
    return 0;
}
