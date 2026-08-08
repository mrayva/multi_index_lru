/// POC client for the cache_service daemon (server.cpp / server_readthrough.cpp)
/// over iceoryx2 request-response. Both servers speak the same wire protocol
/// (cache_service.hpp), so this client works against either.
///
/// With no arguments, runs a fixed demo script exercising get/put/erase
/// against both the string-keyed and int64-keyed cache. With arguments,
/// sends a single manual request:
///
///   cache_poc_client get   name <name>
///   cache_poc_client get   id   <id>
///   cache_poc_client erase name <name>
///   cache_poc_client erase id   <id>
///   cache_poc_client put   name <name> <record-text>
///   cache_poc_client put   id   <id>   <record-text>
///
/// Run server.cpp (or server_readthrough.cpp) first in one terminal, then
/// this in another.
#include "cache_service.hpp"

#include "iox2/iceoryx2.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using iox2::AllocationStrategy;
using iox2::bb::Slice;

constexpr iox2::bb::Duration kCycleTime = iox2::bb::Duration::from_millis(50);
constexpr std::uint64_t kInitialSliceLenHint = 256;

// Client owns one request/response round trip per call: send `request_bytes`,
// block (via the node's event loop) until a response arrives, return it.
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

void print_status_result(const std::vector<std::uint8_t>& response_bytes, const char* verb) {
    poc::wire::Reader r(response_bytes.data(), response_bytes.size());
    const auto status = static_cast<poc::wire::Status>(r.u8());
    std::cout << "  -> " << verb << ": " << status_name(status) << "\n";
}

template <typename Client>
void run_demo_script(Client& client, iox2::Node<iox2::ServiceType::Ipc>& node) {
    std::cout << "--- string-keyed cache ---\n";

    std::cout << "[client] GET name=\"alice\" (pre-seeded) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Name, "alice", 0)));

    std::cout << "[client] GET name=\"dave\" (does not exist yet) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Name, "dave", 0)));

    std::cout << "[client] PUT name=\"dave\" record={\"name\":\"Dave\"} ...\n";
    print_status_result(
        call(client, node,
             poc::encode_put(poc::wire::KeyKind::Name, "dave", 0,
                              {'{', '"', 'n', 'a', 'm', 'e', '"', ':', '"', 'D', 'a', 'v', 'e', '"', '}'})),
        "put");

    std::cout << "[client] GET name=\"dave\" (should be there now) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Name, "dave", 0)));

    std::cout << "[client] PUT name=\"dave\" record={\"name\":\"Dave\",\"v\":2} (update) ...\n";
    print_status_result(
        call(client, node,
             poc::encode_put(poc::wire::KeyKind::Name, "dave", 0,
                              {'{', '"', 'n', 'a', 'm', 'e', '"', ':', '"', 'D', 'a', 'v', 'e', '"', ',', '"', 'v',
                               '"', ':', '2', '}'})),
        "put");

    std::cout << "[client] GET name=\"dave\" (should show the update) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Name, "dave", 0)));

    std::cout << "[client] ERASE name=\"dave\" ...\n";
    print_status_result(call(client, node, poc::encode_erase(poc::wire::KeyKind::Name, "dave", 0)), "erase");

    std::cout << "[client] GET name=\"dave\" (erased) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Name, "dave", 0)));

    std::cout << "\n--- int64-keyed cache ---\n";

    std::cout << "[client] GET id=1 (pre-seeded) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Id, "", 1)));

    std::cout << "[client] GET id=42 (does not exist yet) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Id, "", 42)));

    std::cout << "[client] PUT id=42 record={\"id\":42} ...\n";
    print_status_result(
        call(client, node,
             poc::encode_put(poc::wire::KeyKind::Id, "", 42, {'{', '"', 'i', 'd', '"', ':', '4', '2', '}'})),
        "put");

    std::cout << "[client] GET id=42 (should be there now) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Id, "", 42)));

    std::cout << "[client] ERASE id=42 ...\n";
    print_status_result(call(client, node, poc::encode_erase(poc::wire::KeyKind::Id, "", 42)), "erase");

    std::cout << "[client] GET id=42 (erased) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Id, "", 42)));
}

template <typename Client>
void run_manual_command(Client& client, iox2::Node<iox2::ServiceType::Ipc>& node,
                         const std::vector<std::string>& args) {
    const auto& verb = args[0];

    auto parse_kind_and_key = [&](std::size_t idx, poc::wire::KeyKind& kind, std::string& name, std::int64_t& id) {
        kind = (args[idx] == "id") ? poc::wire::KeyKind::Id : poc::wire::KeyKind::Name;
        if (kind == poc::wire::KeyKind::Id) {
            id = std::stoll(args[idx + 1]);
        } else {
            name = args[idx + 1];
        }
    };

    if (verb == "get" && args.size() == 3) {
        poc::wire::KeyKind kind{};
        std::string name;
        std::int64_t id = 0;
        parse_kind_and_key(1, kind, name, id);
        print_get_result(call(client, node, poc::encode_get(kind, name, id)));
    } else if (verb == "erase" && args.size() == 3) {
        poc::wire::KeyKind kind{};
        std::string name;
        std::int64_t id = 0;
        parse_kind_and_key(1, kind, name, id);
        print_status_result(call(client, node, poc::encode_erase(kind, name, id)), "erase");
    } else if (verb == "put" && args.size() == 4) {
        poc::wire::KeyKind kind{};
        std::string name;
        std::int64_t id = 0;
        parse_kind_and_key(1, kind, name, id);
        const auto& text = args[3];
        print_status_result(
            call(client, node, poc::encode_put(kind, name, id, std::vector<std::uint8_t>(text.begin(), text.end()))),
            "put");
    } else {
        std::cerr << "usage:\n"
                  << "  cache_poc_client                              (run demo script)\n"
                  << "  cache_poc_client get   name <name>\n"
                  << "  cache_poc_client get   id   <id>\n"
                  << "  cache_poc_client erase name <name>\n"
                  << "  cache_poc_client erase id   <id>\n"
                  << "  cache_poc_client put   name <name> <record-text>\n"
                  << "  cache_poc_client put   id   <id>   <record-text>\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    using namespace iox2;

    set_log_level_from_env_or(LogLevel::Info);

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

    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        run_demo_script(client, node);
    } else {
        run_manual_command(client, node, args);
    }

    std::cout << "[client] exit\n";
    return 0;
}
