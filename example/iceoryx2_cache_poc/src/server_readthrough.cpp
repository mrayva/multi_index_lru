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
/// GET: a local hit returns immediately. A local miss falls through to
/// NATS; on a NATS hit the entry is written into the local cache before
/// replying (read-through), so the next GET for that key is a local hit.
/// PUT and ERASE are write-through: NATS is updated first, and the local
/// cache is only touched once that succeeds, so the cache never holds
/// something NATS doesn't durably have.
///
/// The bridge from iceoryx2's synchronous request loop to nats_asio's async
/// coroutines (nats_bridge.hpp) blocks the main thread for the duration of
/// each NATS round trip -- see README.md "What this doesn't answer yet" for
/// why that's an acceptable POC simplification and not a production design.
#include "cache_service.hpp"
#include "nats_bridge.hpp"

#include "iox2/iceoryx2.hpp"

#include <iostream>
#include <string>

namespace {

constexpr iox2::bb::Duration kCycleTime = iox2::bb::Duration::from_millis(100);
constexpr std::uint64_t kInitialSliceLenHint = 256;
constexpr auto kNameBucket = "mil_by_name";
constexpr auto kIdBucket = "mil_by_id";

std::vector<std::uint8_t> handle_request(poc::NameCache& name_cache, poc::IdCache& id_cache, poc::NatsBridge& nats,
                                          const std::uint8_t* data, std::size_t size) {
    poc::wire::Reader r(data, size);
    const auto op = static_cast<poc::wire::Op>(r.u8());
    const auto kind = static_cast<poc::wire::KeyKind>(r.u8());

    if (op == poc::wire::Op::Get) {
        if (kind == poc::wire::KeyKind::Name) {
            auto key = r.str();
            if (auto it = name_cache.find<poc::NameTag>(key); it != name_cache.end<poc::NameTag>()) {
                std::cout << "[server] GET name=\"" << key << "\" -> local hit\n";
                return poc::encode_found(it->record);
            }
            auto result = nats.get(kNameBucket, key);
            if (result.result == poc::NatsResult::Ok) {
                std::cout << "[server] GET name=\"" << key << "\" -> local miss, NATS hit (populating cache)\n";
                name_cache.emplace(poc::NameEntry{key, result.value});
                return poc::encode_found(result.value);
            }
            if (result.result == poc::NatsResult::NotFound) {
                std::cout << "[server] GET name=\"" << key << "\" -> local miss, NATS miss\n";
                return poc::encode_status(poc::wire::Status::NotFound);
            }
            std::cout << "[server] GET name=\"" << key << "\" -> NATS error: " << result.error << "\n";
            return poc::encode_status(poc::wire::Status::Error);
        }

        auto key = r.i64();
        if (auto it = id_cache.find<poc::IdTag>(key); it != id_cache.end<poc::IdTag>()) {
            std::cout << "[server] GET id=" << key << " -> local hit\n";
            return poc::encode_found(it->record);
        }
        auto result = nats.get(kIdBucket, std::to_string(key));
        if (result.result == poc::NatsResult::Ok) {
            std::cout << "[server] GET id=" << key << " -> local miss, NATS hit (populating cache)\n";
            id_cache.emplace(poc::IdEntry{key, result.value});
            return poc::encode_found(result.value);
        }
        if (result.result == poc::NatsResult::NotFound) {
            std::cout << "[server] GET id=" << key << " -> local miss, NATS miss\n";
            return poc::encode_status(poc::wire::Status::NotFound);
        }
        std::cout << "[server] GET id=" << key << " -> NATS error: " << result.error << "\n";
        return poc::encode_status(poc::wire::Status::Error);
    }

    if (op == poc::wire::Op::Put) {
        if (kind == poc::wire::KeyKind::Name) {
            auto key = r.str();
            auto record = r.bytes(r.u32());
            auto [ok, err] = nats.put(kNameBucket, key, record);
            if (!ok) {
                std::cout << "[server] PUT name=\"" << key << "\" -> NATS error: " << err << "\n";
                return poc::encode_status(poc::wire::Status::Error);
            }
            name_cache.erase<poc::NameTag>(key);
            name_cache.emplace(poc::NameEntry{key, record});
            std::cout << "[server] PUT name=\"" << key << "\" -> NATS ok, cache updated\n";
            return poc::encode_status(poc::wire::Status::Ok);
        }

        auto key = r.i64();
        auto record = r.bytes(r.u32());
        auto [ok, err] = nats.put(kIdBucket, std::to_string(key), record);
        if (!ok) {
            std::cout << "[server] PUT id=" << key << " -> NATS error: " << err << "\n";
            return poc::encode_status(poc::wire::Status::Error);
        }
        id_cache.erase<poc::IdTag>(key);
        id_cache.emplace(poc::IdEntry{key, record});
        std::cout << "[server] PUT id=" << key << " -> NATS ok, cache updated\n";
        return poc::encode_status(poc::wire::Status::Ok);
    }

    // Erase
    if (kind == poc::wire::KeyKind::Name) {
        auto key = r.str();
        auto [ok, err] = nats.erase(kNameBucket, key);
        if (!ok) {
            std::cout << "[server] ERASE name=\"" << key << "\" -> NATS error: " << err << "\n";
            return poc::encode_status(poc::wire::Status::Error);
        }
        name_cache.erase<poc::NameTag>(key);
        std::cout << "[server] ERASE name=\"" << key << "\" -> NATS ok, cache updated\n";
        return poc::encode_status(poc::wire::Status::Ok);
    }

    auto key = r.i64();
    auto [ok, err] = nats.erase(kIdBucket, std::to_string(key));
    if (!ok) {
        std::cout << "[server] ERASE id=" << key << " -> NATS error: " << err << "\n";
        return poc::encode_status(poc::wire::Status::Error);
    }
    id_cache.erase<poc::IdTag>(key);
    std::cout << "[server] ERASE id=" << key << " -> NATS ok, cache updated\n";
    return poc::encode_status(poc::wire::Status::Ok);
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

    std::cout << "[server] read-through cache daemon ready, service \"" << poc::kServiceName << "\", buckets \""
              << kNameBucket << "\" / \"" << kIdBucket << "\". Ctrl+C to stop.\n";

    while (node.wait(kCycleTime).has_value()) {
        while (true) {
            auto active_request = server.receive().value();
            if (!active_request.has_value()) {
                break;
            }

            const auto& payload = active_request->payload();
            auto response_bytes = handle_request(name_cache, id_cache, nats, payload.data(), payload.number_of_bytes());

            auto response = active_request->loan_slice_uninit(response_bytes.size()).value();
            auto initialized = response.write_from_fn([&](auto byte_idx) { return response_bytes[byte_idx]; });
            send(std::move(initialized)).value();
        }
    }

    std::cout << "[server] exit\n";
    return 0;
}
