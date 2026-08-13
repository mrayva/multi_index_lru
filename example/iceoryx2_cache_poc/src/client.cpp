/// POC client for the cache_service daemon (server.cpp / server_readthrough.cpp)
/// over iceoryx2 request-response. Both servers speak the same wire protocol
/// (cache_service.hpp), so this client works against either.
///
/// With no arguments, runs a fixed demo script exercising get/put/erase
/// against both the string-keyed and int64-keyed cache. With arguments,
/// sends a single manual request:
///
///   cache_poc_client get    name <name>
///   cache_poc_client get    id   <id>
///   cache_poc_client erase  name <name>
///   cache_poc_client erase  id   <id>
///   cache_poc_client put    name <name> <record-text> [category]
///   cache_poc_client put    id   <id>   <record-text>
///   cache_poc_client getall <category>
///   cache_poc_client getallprefix <name-prefix>
///   cache_poc_client get    security <asset_type> <cusip> <isin> <sedol> <ric>
///   cache_poc_client erase  security <asset_type> <cusip> <isin> <sedol> <ric>
///   cache_poc_client put    security <asset_type> <cusip> <isin> <sedol> <ric> <record-text>
///   cache_poc_client findsecurity <asset_type|*> <cusip|*> <isin|*> <sedol|*> <ric|*>
///
/// `getall` and `put name`'s optional trailing [category] exercise
/// NameCache's second (non-unique) index -- see cache_service.hpp
/// "Non-unique key lookup (GetAll)". Id-keyed entries have no category.
///
/// `getallprefix` and `findsecurity` are NATS-backed only
/// (server_readthrough.cpp/server_readthrough_ecal.cpp) -- see
/// cache_service.hpp "Prefix lookup (GetAll, KeyKind::Name)" and "GetAll
/// (Security, pattern find)". Against server.cpp (no NATS) they always
/// respond "error". `findsecurity` takes a literal value or "*" (wildcard,
/// SecurityKey::pattern()'s nats_any) per field, in security_cache.hpp's
/// (asset_type, cusip, isin, sedol, ric) order.
///
/// Run server.cpp (or server_readthrough.cpp) first in one terminal, then
/// this in another.
///
/// Configurable via CLI flag or env var (flag wins if both are given), and
/// must match whatever the server was given:
///   --service-name / MIL_SERVICE_NAME    (default poc::kServiceName)
#include "cache_service.hpp"
#include "config.hpp"

#include "iox2/iceoryx2.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
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

void print_status_result(const std::vector<std::uint8_t>& response_bytes, const char* verb) {
    poc::wire::Reader r(response_bytes.data(), response_bytes.size());
    const auto status = static_cast<poc::wire::Status>(r.u8());
    std::cout << "  -> " << verb << ": " << status_name(status) << "\n";
}

void print_get_all_result(const std::vector<std::uint8_t>& response_bytes) {
    poc::wire::Reader r(response_bytes.data(), response_bytes.size());
    const auto status = static_cast<poc::wire::Status>(r.u8());
    if (status != poc::wire::Status::Ok) {
        std::cout << "  -> " << status_name(status) << "\n";
        return;
    }
    const auto count = r.u32();
    const bool truncated = r.u8() != 0;
    std::cout << "  -> " << count << " record(s)" << (truncated ? " (truncated -- more matched)" : "") << ":\n";
    for (std::uint32_t i = 0; i < count; ++i) {
        auto record = r.bytes(r.u32());
        std::cout << "     " << std::string(record.begin(), record.end()) << "\n";
    }
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

template <typename Client>
void run_demo_script(Client& client, iox2::Node<iox2::ServiceType::Ipc>& node) {
    std::cout << "--- string-keyed cache ---\n";

    std::cout << "[client] GET name=\"alice\" (pre-seeded) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Name, "alice", 0)));

    std::cout << "[client] GET name=\"dave\" (does not exist yet) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Name, "dave", 0)));

    std::cout << "[client] PUT name=\"dave\" category=\"friends\" record={\"name\":\"Dave\"} ...\n";
    print_status_result(
        call(client, node,
             poc::encode_put(poc::wire::KeyKind::Name, "dave", 0,
                              {'{', '"', 'n', 'a', 'm', 'e', '"', ':', '"', 'D', 'a', 'v', 'e', '"', '}'}, "friends")),
        "put");

    std::cout << "[client] GET name=\"dave\" (should be there now) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Name, "dave", 0)));

    std::cout << "\n--- non-unique key lookup (GetAll) ---\n";

    std::cout << "[client] GETALL category=\"friends\" (alice, bob pre-seeded + dave just added) ...\n";
    print_get_all_result(call(client, node, poc::encode_get_all("friends")));

    std::cout << "[client] PUT name=\"dave\" category=\"friends\" record={\"name\":\"Dave\",\"v\":2} (update) ...\n";
    print_status_result(
        call(client, node,
             poc::encode_put(poc::wire::KeyKind::Name, "dave", 0,
                              {'{', '"', 'n', 'a', 'm', 'e', '"', ':', '"', 'D', 'a', 'v', 'e', '"', ',', '"', 'v',
                               '"', ':', '2', '}'}, "friends")),
        "put");

    std::cout << "[client] GET name=\"dave\" (should show the update) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Name, "dave", 0)));

    std::cout << "[client] ERASE name=\"dave\" ...\n";
    print_status_result(call(client, node, poc::encode_erase(poc::wire::KeyKind::Name, "dave", 0)), "erase");

    std::cout << "[client] GET name=\"dave\" (erased) ...\n";
    print_get_result(call(client, node, poc::encode_get(poc::wire::KeyKind::Name, "dave", 0)));

    std::cout << "[client] GETALL category=\"friends\" (dave's erase should have removed him from here too) ...\n";
    print_get_all_result(call(client, node, poc::encode_get_all("friends")));

    std::cout << "[client] GETALL category=\"coworkers\" (carol, pre-seeded) ...\n";
    print_get_all_result(call(client, node, poc::encode_get_all("coworkers")));

    std::cout << "[client] GETALL category=\"nonexistent\" ...\n";
    print_get_all_result(call(client, node, poc::encode_get_all("nonexistent")));

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

    std::cout << "\n--- security-keyed cache (5-field composite key) ---\n";

    std::cout << "[client] GET security AAPL (pre-seeded) ...\n";
    print_get_result(call(client, node,
                           poc::encode_get_security("EQUITY", "037833100", "US0378331005", "2046251", "AAPL_OQ")));

    std::cout << "[client] GET security for a CUSIP that does not exist yet ...\n";
    print_get_result(call(client, node,
                           poc::encode_get_security("EQUITY", "000000000", "US0000000000", "0000000", "NONE")));

    std::cout << "[client] PUT security (new row) ...\n";
    print_status_result(
        call(client, node,
             poc::encode_put_security("BOND", "912828YK0", "US912828YK00", "BQZTSK1", "T_4.5_2044",
                                       to_bytes(R"({"name":"US Treasury 4.5% 2044"})"))),
        "put");

    std::cout << "[client] GET security for the just-PUT row ...\n";
    print_get_result(
        call(client, node, poc::encode_get_security("BOND", "912828YK0", "US912828YK00", "BQZTSK1", "T_4.5_2044")));

    std::cout << "[client] ERASE security for the just-PUT row ...\n";
    print_status_result(
        call(client, node,
             poc::encode_erase_security("BOND", "912828YK0", "US912828YK00", "BQZTSK1", "T_4.5_2044")),
        "erase");

    std::cout << "[client] GET security for the erased row ...\n";
    print_get_result(
        call(client, node, poc::encode_get_security("BOND", "912828YK0", "US912828YK00", "BQZTSK1", "T_4.5_2044")));

    std::cout << "[client] FINDSECURITY pattern find (NATS-backed only -- against server.cpp this always errors) ...\n";
    print_get_all_prefix_result(call(
        client, node,
        poc::encode_find_security("EQUITY", std::nullopt, "US0378331005", std::nullopt, std::nullopt)));
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

    if (verb == "get" && args.size() == 7 && args[1] == "security") {
        print_get_result(call(client, node, poc::encode_get_security(args[2], args[3], args[4], args[5], args[6])));
    } else if (verb == "erase" && args.size() == 7 && args[1] == "security") {
        print_status_result(
            call(client, node, poc::encode_erase_security(args[2], args[3], args[4], args[5], args[6])), "erase");
    } else if (verb == "put" && args.size() == 8 && args[1] == "security") {
        const auto& text = args[7];
        print_status_result(
            call(client, node,
                 poc::encode_put_security(args[2], args[3], args[4], args[5], args[6],
                                           std::vector<std::uint8_t>(text.begin(), text.end()))),
            "put");
    } else if (verb == "findsecurity" && args.size() == 6) {
        auto field = [](const std::string& s) -> std::optional<std::string> {
            return (s == "*") ? std::nullopt : std::optional<std::string>(s);
        };
        print_get_all_prefix_result(call(
            client, node,
            poc::encode_find_security(field(args[1]), field(args[2]), field(args[3]), field(args[4]), field(args[5]))));
    } else if (verb == "get" && args.size() == 3) {
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
    } else if (verb == "put" && (args.size() == 4 || args.size() == 5)) {
        poc::wire::KeyKind kind{};
        std::string name;
        std::int64_t id = 0;
        parse_kind_and_key(1, kind, name, id);
        const auto& text = args[3];
        // args[4], if given, is the category -- only meaningful for
        // KeyKind::Name (encode_put ignores it for Id, same as Id-keyed
        // Puts have no category field on the wire at all).
        const std::string category = (args.size() == 5) ? args[4] : "";
        print_status_result(
            call(client, node,
                 poc::encode_put(kind, name, id, std::vector<std::uint8_t>(text.begin(), text.end()), category)),
            "put");
    } else if (verb == "getall" && args.size() == 2) {
        print_get_all_result(call(client, node, poc::encode_get_all(args[1])));
    } else if (verb == "getallprefix" && args.size() == 2) {
        print_get_all_prefix_result(call(client, node, poc::encode_get_all_by_prefix(args[1])));
    } else {
        std::cerr << "usage:\n"
                  << "  cache_poc_client                                    (run demo script)\n"
                  << "  cache_poc_client get    name <name>\n"
                  << "  cache_poc_client get    id   <id>\n"
                  << "  cache_poc_client erase  name <name>\n"
                  << "  cache_poc_client erase  id   <id>\n"
                  << "  cache_poc_client put    name <name> <record-text> [category]\n"
                  << "  cache_poc_client put    id   <id>   <record-text>\n"
                  << "  cache_poc_client getall <category>\n"
                  << "  cache_poc_client getallprefix <name-prefix>          (NATS-backed only)\n"
                  << "  cache_poc_client get    security <asset_type> <cusip> <isin> <sedol> <ric>\n"
                  << "  cache_poc_client erase  security <asset_type> <cusip> <isin> <sedol> <ric>\n"
                  << "  cache_poc_client put    security <asset_type> <cusip> <isin> <sedol> <ric> <record-text>\n"
                  << "  cache_poc_client findsecurity <asset_type|*> <cusip|*> <isin|*> <sedol|*> <ric|*>"
                     "  (NATS-backed only)\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    using namespace iox2;

    set_log_level_from_env_or(LogLevel::Info);

    std::vector<std::string> args(argv + 1, argv + argc);
    const std::string service_name = poc::config::resolve_str(args, "--service-name", "MIL_SERVICE_NAME", poc::kServiceName);

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

    if (args.empty()) {
        run_demo_script(client, node);
    } else {
        run_manual_command(client, node, args);
    }

    std::cout << "[client] exit\n";
    return 0;
}
