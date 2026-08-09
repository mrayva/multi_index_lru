/// Demo client for server_readthrough.cpp. Uses the exact same
/// cache_poc_client wire protocol/service (cache_service.hpp) to drive the
/// daemon, plus its own direct, daemon-bypassing NATS connection to prove
/// the read-through path is real: it seeds a key straight into a NATS KV
/// bucket, then asks the daemon for that key -- which the daemon has never
/// seen -- and shows it comes back correctly. Watch server_readthrough.cpp's
/// own console output alongside this to see "local miss, NATS hit" for
/// yourself.
///
/// Configurable via CLI flag or env var (flag wins if both are given), and
/// must match whatever the server was given:
///   --service-name / MIL_SERVICE_NAME    (default poc::kServiceName)
///   --nats-host / MIL_NATS_HOST          (default 127.0.0.1)
///   --nats-port / MIL_NATS_PORT          (default 4222)
///   --name-bucket / MIL_NAME_BUCKET      (default poc::kNameBucket)
///   --id-bucket / MIL_ID_BUCKET          (default poc::kIdBucket)
#include "cache_service.hpp"
#include "config.hpp"
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
// Same default bucket names server_readthrough.cpp uses (poc::kNameBucket/
// poc::kIdBucket, defined in server_dispatch.hpp) -- duplicated here as
// plain string literals rather than including that header, which would also
// pull in the (server-only) dispatch/CompletionQueue machinery this client
// has no use for.
constexpr auto kDefaultNameBucket = "mil_by_name";
constexpr auto kDefaultIdBucket = "mil_by_id";

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
        case poc::wire::Status::ReadOnly: return "read-only (server started without --allow-writes)";
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

// Decodes a GetAll(prefix) response -- see cache_service.hpp's
// "GetAll(prefix)+Ok" wire format: unlike category GetAll, each match
// carries its own full NATS key alongside the record.
void print_get_all_prefix_result(const std::vector<std::uint8_t>& response_bytes) {
    poc::wire::Reader r(response_bytes.data(), response_bytes.size());
    const auto status = static_cast<poc::wire::Status>(r.u8());
    if (status != poc::wire::Status::Ok) {
        std::cout << "  -> " << status_name(status) << "\n";
        return;
    }
    const auto count = r.u32();
    const bool truncated = r.u8() != 0;
    std::cout << "  -> " << count << " key(s)" << (truncated ? " (truncated -- more matched)" : "") << ":\n";
    for (std::uint32_t i = 0; i < count; ++i) {
        auto key = r.str();
        auto record = r.bytes(r.u32());
        std::cout << "     " << key << " = " << std::string(record.begin(), record.end()) << "\n";
    }
}

std::vector<std::uint8_t> to_bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

}  // namespace

int main(int argc, char** argv) {
    using namespace iox2;

    set_log_level_from_env_or(LogLevel::Info);

    std::vector<std::string> args(argv + 1, argv + argc);
    const std::string service_name = poc::config::resolve_str(args, "--service-name", "MIL_SERVICE_NAME", poc::kServiceName);
    const std::string nats_host = poc::config::resolve_str(args, "--nats-host", "MIL_NATS_HOST", "127.0.0.1");
    const std::uint16_t nats_port = poc::config::resolve_u16(args, "--nats-port", "MIL_NATS_PORT", 4222);
    const std::string name_bucket = poc::config::resolve_str(args, "--name-bucket", "MIL_NAME_BUCKET", kDefaultNameBucket);
    const std::string id_bucket = poc::config::resolve_str(args, "--id-bucket", "MIL_ID_BUCKET", kDefaultIdBucket);

    std::cout << "[client] connecting directly to NATS (bypassing the daemon) to seed test data ...\n";
    poc::NatsBridge nats(nats_host, nats_port);

    // Seed keys that server_readthrough.cpp's caches have never seen -- if
    // it hasn't been restarted since these ran before, delete first so the
    // demo is deterministic regardless of prior runs.
    nats.erase(name_bucket, "seeded_directly");
    nats.erase(id_bucket, "777");

    auto [name_ok, name_err] =
        nats.put(name_bucket, "seeded_directly", to_bytes(R"({"note":"came from NATS, not a daemon PUT"})"));
    std::cout << "[client] seeded NATS bucket \"" << name_bucket << "\" key \"seeded_directly\": "
              << (name_ok ? "ok" : ("FAILED: " + name_err)) << "\n";

    auto [id_ok, id_err] = nats.put(id_bucket, "777", to_bytes(R"({"note":"came from NATS, not a daemon PUT"})"));
    std::cout << "[client] seeded NATS bucket \"" << id_bucket << "\" key \"777\": "
              << (id_ok ? "ok" : ("FAILED: " + id_err)) << "\n\n";

    auto node = NodeBuilder().create<ServiceType::Ipc>().value();

    auto service = node.service_builder(ServiceName::create(service_name.c_str()).value())
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

    auto direct = nats.get(name_bucket, "via_daemon");
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

    auto direct_after_erase = nats.get(id_bucket, "777");
    std::cout << "[client] direct NATS get of id=777 (bypassing the daemon): "
              << (direct_after_erase.result == poc::NatsResult::NotFound
                      ? "not found -- confirmed NATS was updated, not just the local cache"
                      : "still present -- erase did NOT propagate to NATS!")
              << "\n";

    std::cout << "\n--- GetAll(prefix): one primary key, several NATS keys underneath it ---\n";

    // Seed directly into NATS, bypassing the daemon entirely -- same idea as
    // "seeded_directly" above, but three keys sharing a "." prefix rather
    // than one plain key. Delete first so repeat runs are deterministic.
    nats.erase(name_bucket, "prefix_demo.email");
    nats.erase(name_bucket, "prefix_demo.phone");
    nats.erase(name_bucket, "other_prefix.email");
    nats.put(name_bucket, "prefix_demo.email", to_bytes("prefix_demo@example.com"));
    nats.put(name_bucket, "prefix_demo.phone", to_bytes("555-0100"));
    nats.put(name_bucket, "other_prefix.email", to_bytes("someone-else@example.com"));
    std::cout << "[client] seeded \"prefix_demo.email\", \"prefix_demo.phone\", \"other_prefix.email\" directly in NATS\n";

    std::cout << "[client] GETALL name-prefix=\"prefix_demo\" (should return exactly the two prefix_demo.* keys) ...\n";
    print_get_all_prefix_result(call(client, node, poc::encode_get_all_by_prefix("prefix_demo")));

    std::cout << "[client] GETALL name-prefix=\"prefix_demo\" again (still asks NATS -- a repeat GetAll never trusts"
                 " the cache alone, since a new key added under the prefix since the last call wouldn't be known"
                 " locally; see cache_service.hpp) ...\n";
    print_get_all_prefix_result(call(client, node, poc::encode_get_all_by_prefix("prefix_demo")));

    std::cout << "[client] GET name=\"prefix_demo.email\" (a *plain* Get for one of GETALL's matches IS a local hit"
                 " now -- watch server_readthrough.cpp's own log) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Name, "prefix_demo.email", 0)));

    std::cout << "[client] GETALL name-prefix=\"nonexistent_prefix\" ...\n";
    print_get_all_prefix_result(call(client, node, poc::encode_get_all_by_prefix("nonexistent_prefix")));

    std::cout << "\n[client] exit\n";
    return 0;
}
