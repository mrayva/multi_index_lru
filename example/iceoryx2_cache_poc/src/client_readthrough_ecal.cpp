/// eCAL port of client_readthrough.cpp -- same demo (seed a key directly
/// into NATS, ask the daemon for it through the wire protocol, confirm
/// read-through; write-through via PUT; erase propagation) against
/// server_readthrough_ecal.cpp instead of server_readthrough.cpp. Watch
/// server_readthrough_ecal.cpp's own console output alongside this to see
/// "local miss, NATS hit" for yourself, same as the iceoryx2 demo.
///
/// The eCAL client side is simpler than the iceoryx2 one: eCAL's
/// CClientInstance::CallWithResponse() is a single blocking RPC call, no
/// separate poll-for-response loop to write (see call() below).
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

#include <ecal/ecal.h>
#include <ecal/service/client.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// Same default bucket names server_readthrough_ecal.cpp uses -- duplicated
// as plain string literals rather than including server_dispatch_common.hpp,
// same reasoning as client_readthrough.cpp's identical comment.
constexpr auto kDefaultNameBucket = "mil_by_name";
constexpr auto kDefaultIdBucket = "mil_by_id";

std::vector<std::uint8_t> call(eCAL::CServiceClient& client, const std::vector<std::uint8_t>& request_bytes) {
    auto instances = client.GetClientInstances();
    if (instances.empty()) {
        throw std::runtime_error("no connected server instance");
    }
    const std::string request(request_bytes.begin(), request_bytes.end());
    auto [ok, response] = instances.front().CallWithResponse("dispatch", request);
    if (!ok || response.call_state != eCAL::eCallState::executed) {
        throw std::runtime_error("call failed: " + response.error_msg);
    }
    return std::vector<std::uint8_t>(response.response.begin(), response.response.end());
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

std::vector<std::uint8_t> to_bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    const std::string service_name = poc::config::resolve_str(args, "--service-name", "MIL_SERVICE_NAME", poc::kServiceName);
    const std::string nats_host = poc::config::resolve_str(args, "--nats-host", "MIL_NATS_HOST", "127.0.0.1");
    const std::uint16_t nats_port = poc::config::resolve_u16(args, "--nats-port", "MIL_NATS_PORT", 4222);
    const std::string name_bucket = poc::config::resolve_str(args, "--name-bucket", "MIL_NAME_BUCKET", kDefaultNameBucket);
    const std::string id_bucket = poc::config::resolve_str(args, "--id-bucket", "MIL_ID_BUCKET", kDefaultIdBucket);

    std::cout << "[client] connecting directly to NATS (bypassing the daemon) to seed test data ...\n";
    poc::NatsBridge nats(nats_host, nats_port);

    nats.erase(name_bucket, "seeded_directly_ecal");
    nats.erase(id_bucket, "778");

    auto [name_ok, name_err] = nats.put(name_bucket, "seeded_directly_ecal",
                                         to_bytes(R"({"note":"came from NATS, not a daemon PUT"})"));
    std::cout << "[client] seeded NATS bucket \"" << name_bucket << "\" key \"seeded_directly_ecal\": "
              << (name_ok ? "ok" : ("FAILED: " + name_err)) << "\n";

    auto [id_ok, id_err] = nats.put(id_bucket, "778", to_bytes(R"({"note":"came from NATS, not a daemon PUT"})"));
    std::cout << "[client] seeded NATS bucket \"" << id_bucket << "\" key \"778\": "
              << (id_ok ? "ok" : ("FAILED: " + id_err)) << "\n\n";

    eCAL::Initialize(service_name + "_client");
    eCAL::CServiceClient client(service_name);

    std::cout << "[client] waiting for the eCAL service \"" << service_name << "\" to be discovered ...\n";
    for (int attempt = 0; attempt < 50 && !client.IsConnected(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!client.IsConnected()) {
        std::cerr << "[client] never connected to service \"" << service_name
                  << "\" -- is server_readthrough_ecal running?\n";
        eCAL::Finalize();
        return 1;
    }

    std::cout << "--- read-through: string-keyed cache ---\n";

    std::cout << "[client] GET name=\"seeded_directly_ecal\" (never went through the daemon) ...\n";
    print_get_result(call(client, poc::encode_get(poc::wire::KeyKind::Name, "seeded_directly_ecal", 0)));

    std::cout << "[client] GET name=\"seeded_directly_ecal\" again (should now be a local cache hit) ...\n";
    print_get_result(call(client, poc::encode_get(poc::wire::KeyKind::Name, "seeded_directly_ecal", 0)));

    std::cout << "\n--- write-through: string-keyed cache ---\n";

    std::cout << "[client] PUT name=\"via_daemon_ecal\" (write-through to NATS) ...\n";
    print_put_result(call(client, poc::encode_put(poc::wire::KeyKind::Name, "via_daemon_ecal", 0,
                                                    to_bytes(R"({"note":"written through the daemon"})"))));

    auto direct = nats.get(name_bucket, "via_daemon_ecal");
    std::cout << "[client] direct NATS get of \"via_daemon_ecal\" (bypassing the daemon): "
              << (direct.result == poc::NatsResult::Ok ? std::string(direct.value.begin(), direct.value.end())
                                                         : "not found in NATS -- write-through failed!")
              << "\n\n";

    std::cout << "--- read-through: int64-keyed cache ---\n";

    std::cout << "[client] GET id=778 (never went through the daemon) ...\n";
    print_get_result(call(client, poc::encode_get(poc::wire::KeyKind::Id, "", 778)));

    std::cout << "[client] GET id=778 again (should now be a local cache hit) ...\n";
    print_get_result(call(client, poc::encode_get(poc::wire::KeyKind::Id, "", 778)));

    std::cout << "\n--- erase propagates to NATS ---\n";

    std::cout << "[client] ERASE id=778 via the daemon ...\n";
    {
        auto response = call(client, poc::encode_erase(poc::wire::KeyKind::Id, "", 778));
        poc::wire::Reader r(response.data(), response.size());
        std::cout << "  -> erase: " << status_name(static_cast<poc::wire::Status>(r.u8())) << "\n";
    }

    auto direct_after_erase = nats.get(id_bucket, "778");
    std::cout << "[client] direct NATS get of id=778 (bypassing the daemon): "
              << (direct_after_erase.result == poc::NatsResult::NotFound
                      ? "not found -- confirmed NATS was updated, not just the local cache"
                      : "still present -- erase did NOT propagate to NATS!")
              << "\n";

    std::cout << "\n[client] exit\n";
    eCAL::Finalize();
    return 0;
}
