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
/// once that completes, via CompletionQueue below. PUT and ERASE are
/// write-through and always go to NATS: `kv_put`/`kv_delete` is kicked off
/// async, and the local cache is only touched -- and the response only sent
/// -- once that NATS write completes, so the cache never holds something
/// NATS doesn't durably have.
///
/// Nothing here blocks the main iceoryx2 request loop on a NATS round trip:
/// while one client's GET/PUT/ERASE is waiting on NATS, the loop keeps
/// receiving and dispatching every other client's requests. See
/// "the concurrency model" in README.md for how this differs from (and
/// improves on) the single-threaded blocking-bridge design this replaced.
#include "cache_service.hpp"
#include "nats_bridge.hpp"

#include "iox2/iceoryx2.hpp"

#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

// Poll frequently: unlike the blocking-bridge design this replaced, request
// handling itself never blocks on NATS, so a short cycle just keeps
// completion-to-response latency low without wasting CPU on a busy loop.
constexpr iox2::bb::Duration kCycleTime = iox2::bb::Duration::from_millis(5);
constexpr std::uint64_t kInitialSliceLenHint = 256;
constexpr auto kNameBucket = "mil_by_name";
constexpr auto kIdBucket = "mil_by_id";

// request_response<Slice<u8>, Slice<u8>>() with no explicit header types
// resolves to `void` headers -- see ServiceBuilder::request_response() in
// iceoryx2-cxx/include/iox2/service_builder.hpp.
using ActiveRequestType =
    iox2::ActiveRequest<iox2::ServiceType::Ipc, iox2::bb::Slice<std::uint8_t>, void, iox2::bb::Slice<std::uint8_t>,
                         void>;

// Runs on the main thread once a deferred request's NATS operation has
// completed: applies whatever cache mutation is needed (a Get populating the
// cache on a NATS hit, or a Put/Erase applying now that NATS confirmed it)
// and returns the response bytes to send.
using ApplyFn = std::function<std::vector<std::uint8_t>(poc::NameCache&, poc::IdCache&)>;

struct PendingCompletion {
    ActiveRequestType active_request;
    ApplyFn apply;
    std::string description;  // for logging only, e.g. "GET name=\"alice\"" -- built on whichever
                               // thread calls push(), but only ever printed on the main thread's
                               // drain loop, so it doesn't need to touch std::cout itself.
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
    void push(ActiveRequestType active_request, ApplyFn apply, std::string description) {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(PendingCompletion{std::move(active_request), std::move(apply), std::move(description)});
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

private:
    std::mutex mutex_;
    std::deque<PendingCompletion> items_;
};

void respond(ActiveRequestType& active_request, const std::vector<std::uint8_t>& response_bytes) {
    auto response = active_request.loan_slice_uninit(response_bytes.size()).value();
    auto initialized = response.write_from_fn([&](auto byte_idx) { return response_bytes[byte_idx]; });
    send(std::move(initialized)).value();
}

// Parses one request and either responds immediately (local cache hit, or a
// malformed request) or hands `active_request` to `completions` -- to be
// responded to once the async NATS operation it kicks off completes.
//
// Any exception here (malformed/truncated request -- see wire::Reader) is
// caught and turned into an immediate Error response; the same hardening
// server.cpp's handle_request_local() has, but active_request is always
// still valid at that point since parsing (and any exception from it)
// happens before active_request is ever moved into a deferred callback.
void dispatch_request(poc::NameCache& name_cache, poc::IdCache& id_cache, poc::NatsBridge& nats,
                       CompletionQueue& completions, ActiveRequestType active_request, const std::uint8_t* data,
                       std::size_t size) {
    try {
        poc::wire::Reader r(data, size);
        const auto op = static_cast<poc::wire::Op>(r.u8());
        const auto kind = static_cast<poc::wire::KeyKind>(r.u8());

        if (op == poc::wire::Op::Get) {
            if (kind == poc::wire::KeyKind::Name) {
                auto key = r.str();
                if (auto it = name_cache.find<poc::NameTag>(key); it != name_cache.end<poc::NameTag>()) {
                    std::cout << "[server] GET name=\"" << key << "\" -> local hit\n";
                    respond(active_request, poc::encode_found(it->record));
                    return;
                }
                std::cout << "[server] GET name=\"" << key << "\" -> local miss, dispatched to NATS\n";
                nats.get_async(kNameBucket, key,
                                [&completions, active_request = std::move(active_request),
                                 key](poc::NatsGetResult result) mutable {
                                    ApplyFn apply = [key, result](poc::NameCache& name_cache,
                                                                   poc::IdCache&) -> std::vector<std::uint8_t> {
                                        if (result.result == poc::NatsResult::Ok) {
                                            name_cache.emplace(poc::NameEntry{key, result.value});
                                            return poc::encode_found(result.value);
                                        }
                                        if (result.result == poc::NatsResult::NotFound) {
                                            return poc::encode_status(poc::wire::Status::NotFound);
                                        }
                                        return poc::encode_status(poc::wire::Status::Error);
                                    };
                                    completions.push(std::move(active_request), std::move(apply),
                                                      "GET name=\"" + key + "\"");
                                });
                return;
            }

            auto key = r.i64();
            if (auto it = id_cache.find<poc::IdTag>(key); it != id_cache.end<poc::IdTag>()) {
                std::cout << "[server] GET id=" << key << " -> local hit\n";
                respond(active_request, poc::encode_found(it->record));
                return;
            }
            std::cout << "[server] GET id=" << key << " -> local miss, dispatched to NATS\n";
            nats.get_async(
                kIdBucket, std::to_string(key),
                [&completions, active_request = std::move(active_request), key](poc::NatsGetResult result) mutable {
                    ApplyFn apply = [key, result](poc::NameCache&, poc::IdCache& id_cache) -> std::vector<std::uint8_t> {
                        if (result.result == poc::NatsResult::Ok) {
                            id_cache.emplace(poc::IdEntry{key, result.value});
                            return poc::encode_found(result.value);
                        }
                        if (result.result == poc::NatsResult::NotFound) {
                            return poc::encode_status(poc::wire::Status::NotFound);
                        }
                        return poc::encode_status(poc::wire::Status::Error);
                    };
                    completions.push(std::move(active_request), std::move(apply), "GET id=" + std::to_string(key));
                });
            return;
        }

        if (op == poc::wire::Op::Put) {
            if (kind == poc::wire::KeyKind::Name) {
                auto key = r.str();
                auto record = r.bytes(r.u32());
                std::cout << "[server] PUT name=\"" << key << "\" -> dispatched to NATS\n";
                nats.put_async(kNameBucket, key, record,
                                [&completions, active_request = std::move(active_request), key,
                                 record](bool ok, std::string /*err*/) mutable {
                                    ApplyFn apply = [key, record, ok](poc::NameCache& name_cache,
                                                                       poc::IdCache&) -> std::vector<std::uint8_t> {
                                        if (!ok) {
                                            return poc::encode_status(poc::wire::Status::Error);
                                        }
                                        name_cache.erase<poc::NameTag>(key);
                                        name_cache.emplace(poc::NameEntry{key, record});
                                        return poc::encode_status(poc::wire::Status::Ok);
                                    };
                                    completions.push(std::move(active_request), std::move(apply),
                                                      "PUT name=\"" + key + "\"");
                                });
                return;
            }

            auto key = r.i64();
            auto record = r.bytes(r.u32());
            std::cout << "[server] PUT id=" << key << " -> dispatched to NATS\n";
            nats.put_async(kIdBucket, std::to_string(key), record,
                            [&completions, active_request = std::move(active_request), key,
                             record](bool ok, std::string /*err*/) mutable {
                                ApplyFn apply = [key, record, ok](poc::NameCache&,
                                                                   poc::IdCache& id_cache) -> std::vector<std::uint8_t> {
                                    if (!ok) {
                                        return poc::encode_status(poc::wire::Status::Error);
                                    }
                                    id_cache.erase<poc::IdTag>(key);
                                    id_cache.emplace(poc::IdEntry{key, record});
                                    return poc::encode_status(poc::wire::Status::Ok);
                                };
                                completions.push(std::move(active_request), std::move(apply),
                                                  "PUT id=" + std::to_string(key));
                            });
            return;
        }

        // Erase
        if (kind == poc::wire::KeyKind::Name) {
            auto key = r.str();
            std::cout << "[server] ERASE name=\"" << key << "\" -> dispatched to NATS\n";
            nats.erase_async(kNameBucket, key,
                              [&completions, active_request = std::move(active_request),
                               key](bool ok, std::string /*err*/) mutable {
                                  ApplyFn apply = [key, ok](poc::NameCache& name_cache,
                                                             poc::IdCache&) -> std::vector<std::uint8_t> {
                                      if (!ok) {
                                          return poc::encode_status(poc::wire::Status::Error);
                                      }
                                      name_cache.erase<poc::NameTag>(key);
                                      return poc::encode_status(poc::wire::Status::Ok);
                                  };
                                  completions.push(std::move(active_request), std::move(apply),
                                                    "ERASE name=\"" + key + "\"");
                              });
            return;
        }

        auto key = r.i64();
        std::cout << "[server] ERASE id=" << key << " -> dispatched to NATS\n";
        nats.erase_async(
            kIdBucket, std::to_string(key),
            [&completions, active_request = std::move(active_request), key](bool ok, std::string /*err*/) mutable {
                ApplyFn apply = [key, ok](poc::NameCache&, poc::IdCache& id_cache) -> std::vector<std::uint8_t> {
                    if (!ok) {
                        return poc::encode_status(poc::wire::Status::Error);
                    }
                    id_cache.erase<poc::IdTag>(key);
                    return poc::encode_status(poc::wire::Status::Ok);
                };
                completions.push(std::move(active_request), std::move(apply), "ERASE id=" + std::to_string(key));
            });
    } catch (const std::exception&) {
        respond(active_request, poc::encode_status(poc::wire::Status::Error));
    } catch (...) {
        respond(active_request, poc::encode_status(poc::wire::Status::Error));
    }
}

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
    CompletionQueue completions;

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
              << "\", buckets \"" << kNameBucket << "\" / \"" << kIdBucket << "\". Ctrl+C to stop.\n";

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
            respond(completion.active_request, response_bytes);
        }

        while (true) {
            auto active_request_opt = server.receive().value();
            if (!active_request_opt.has_value()) {
                break;
            }

            const auto& payload = active_request_opt->payload();
            std::vector<std::uint8_t> request_bytes(payload.data(), payload.data() + payload.number_of_bytes());

            dispatch_request(name_cache, id_cache, nats, completions, std::move(active_request_opt.value()),
                              request_bytes.data(), request_bytes.size());
        }
    }

    std::cout << "[server] exit\n";
    return 0;
}
