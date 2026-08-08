/// Demo client for server_readthrough.cpp. Uses the exact same
/// cache_poc_client wire protocol/service (cache_service.hpp) to drive the
/// daemon, plus its own direct, daemon-bypassing NATS connection to prove
/// the read-through path is real: it seeds a key straight into a NATS KV
/// bucket, then asks the daemon for that key -- which the daemon has never
/// seen -- and shows it comes back correctly. Watch server_readthrough.cpp's
/// own console output alongside this to see "local miss, NATS hit" for
/// yourself.
#include "cache_service.hpp"
#include "nats_bridge.hpp"

#include "iox2/iceoryx2.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using iox2::AllocationStrategy;
using iox2::bb::Slice;

constexpr iox2::bb::Duration kCycleTime = iox2::bb::Duration::from_millis(50);
constexpr std::uint64_t kInitialSliceLenHint = 256;
constexpr auto kNameBucket = "mil_by_name";
constexpr auto kIdBucket = "mil_by_id";

template <typename Client>
std::vector<std::uint8_t> call(Client& client, iox2::Node<iox2::ServiceType::Ipc>& node,
                                const std::vector<std::uint8_t>& request_bytes) {
    auto request = client.loan_slice_uninit(request_bytes.size()).value();
    auto initialized_request = request.write_from_fn([&](auto byte_idx) { return request_bytes[byte_idx]; });
    auto pending_response = send(std::move(initialized_request)).value();

    while (node.wait(kCycleTime).has_value()) {
        auto response = pending_response.receive().value();
        if (!response.has_value()) {
            continue;
        }
        const auto& payload = response->payload();
        return std::vector<std::uint8_t>(payload.data(), payload.data() + payload.number_of_bytes());
    }
    throw std::runtime_error("no response received");
}

const char* status_name(poc::wire::Status status) {
    switch (status) {
        case poc::wire::Status::Ok: return "ok";
        case poc::wire::Status::NotFound: return "not found";
        case poc::wire::Status::Error: return "error";
    }
    return "?";
}

void print_get_result(const std::vector<std::uint8_t>& response_bytes) {
    poc::wire::Reader r(response_bytes.data(), response_bytes.size());
    const auto status = static_cast<poc::wire::Status>(r.u8());
    if (status == poc::wire::Status::Ok) {
        auto record = r.remaining();
        std::cout << "  -> " << std::string(record.begin(), record.end()) << "\n";
    } else {
        std::cout << "  -> " << status_name(status) << "\n";
    }
}

void print_put_result(const std::vector<std::uint8_t>& response_bytes) {
    poc::wire::Reader r(response_bytes.data(), response_bytes.size());
    const auto status = static_cast<poc::wire::Status>(r.u8());
    std::cout << "  -> put: " << status_name(status) << "\n";
}

std::vector<std::uint8_t> to_bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

}  // namespace

int main() {
    using namespace iox2;

    set_log_level_from_env_or(LogLevel::Info);

    std::cout << "[client] connecting directly to NATS (bypassing the daemon) to seed test data ...\n";
    poc::NatsBridge nats("127.0.0.1", 4222);

    // Seed keys that server_readthrough.cpp's caches have never seen -- if
    // it hasn't been restarted since these ran before, delete first so the
    // demo is deterministic regardless of prior runs.
    nats.erase(kNameBucket, "seeded_directly");
    nats.erase(kIdBucket, "777");

    auto [name_ok, name_err] =
        nats.put(kNameBucket, "seeded_directly", to_bytes(R"({"note":"came from NATS, not a daemon PUT"})"));
    std::cout << "[client] seeded NATS bucket \"" << kNameBucket << "\" key \"seeded_directly\": "
              << (name_ok ? "ok" : ("FAILED: " + name_err)) << "\n";

    auto [id_ok, id_err] = nats.put(kIdBucket, "777", to_bytes(R"({"note":"came from NATS, not a daemon PUT"})"));
    std::cout << "[client] seeded NATS bucket \"" << kIdBucket << "\" key \"777\": "
              << (id_ok ? "ok" : ("FAILED: " + id_err)) << "\n\n";

    auto node = NodeBuilder().create<ServiceType::Ipc>().value();

    auto service = node.service_builder(ServiceName::create(poc::kServiceName).value())
                        .request_response<bb::Slice<std::uint8_t>, bb::Slice<std::uint8_t>>()
                        .open_or_create()
                        .value();

    auto client = service.client_builder()
                      .initial_max_slice_len(kInitialSliceLenHint)
                      .allocation_strategy(AllocationStrategy::PowerOfTwo)
                      .create()
                      .value();

    std::cout << "--- read-through: string-keyed cache ---\n";

    std::cout << "[client] GET name=\"seeded_directly\" (never went through the daemon) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Name, "seeded_directly", 0)));

    std::cout << "[client] GET name=\"seeded_directly\" again (should now be a local cache hit) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Name, "seeded_directly", 0)));

    std::cout << "\n--- write-through: string-keyed cache ---\n";

    std::cout << "[client] PUT name=\"via_daemon\" (write-through to NATS) ...\n";
    print_put_result(call(client, node,
                           poc::encode_put(poc::wire::KeyKind::Name, "via_daemon", 0,
                                            to_bytes(R"({"note":"written through the daemon"})"))));

    auto direct = nats.get(kNameBucket, "via_daemon");
    std::cout << "[client] direct NATS get of \"via_daemon\" (bypassing the daemon): "
              << (direct.result == poc::NatsResult::Ok ? std::string(direct.value.begin(), direct.value.end())
                                                         : "not found in NATS -- write-through failed!")
              << "\n\n";

    std::cout << "--- read-through: int64-keyed cache ---\n";

    std::cout << "[client] GET id=777 (never went through the daemon) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Id, "", 777)));

    std::cout << "[client] GET id=777 again (should now be a local cache hit) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Id, "", 777)));

    std::cout << "\n--- erase propagates to NATS ---\n";

    std::cout << "[client] ERASE id=777 via the daemon ...\n";
    {
        auto response = call(client, node, poc::encode_erase(poc::wire::KeyKind::Id, "", 777));
        poc::wire::Reader r(response.data(), response.size());
        std::cout << "  -> erase: " << status_name(static_cast<poc::wire::Status>(r.u8())) << "\n";
    }

    auto direct_after_erase = nats.get(kIdBucket, "777");
    std::cout << "[client] direct NATS get of id=777 (bypassing the daemon): "
              << (direct_after_erase.result == poc::NatsResult::NotFound
                      ? "not found -- confirmed NATS was updated, not just the local cache"
                      : "still present -- erase did NOT propagate to NATS!")
              << "\n";

    std::cout << "\n[client] exit\n";
    return 0;
}
