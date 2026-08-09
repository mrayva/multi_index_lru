// Tests dispatch_request() (server_dispatch.hpp) end to end: a real iceoryx2
// Client and Server pair, both created within this test process on a
// dedicated service name (distinct from poc::kServiceName, so this never
// collides with a running demo daemon), plus a real NatsBridge. There's no
// way to construct a standalone/fake iceoryx2 ActiveRequest -- it can only
// come from a Server that actually received a RequestMut over real IPC -- so
// this is an integration test, not a hermetic unit test.
//
// Excluded from the dependency-free poc-unit-tests CI job (this target only
// exists when POC_ENABLE_NATS_READTHROUGH=ON, which that job doesn't set).
// Needs the same local NATS server + buckets as the rest of the NATS-backed
// POC -- see README.md "Read-through / write-through over NATS" > "Setup".
#include "../src/server_dispatch.hpp"

#include "iox2/iceoryx2.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace poc {
namespace {

constexpr auto kTestServiceName = "multi_index_lru/cache/rpc/dispatch_request_test";

using NodeType = iox2::Node<iox2::ServiceType::Ipc>;
using ServerType = iox2::Server<iox2::ServiceType::Ipc, iox2::bb::Slice<std::uint8_t>, void, iox2::bb::Slice<std::uint8_t>,
                                 void>;
using ClientType = iox2::Client<iox2::ServiceType::Ipc, iox2::bb::Slice<std::uint8_t>, void, iox2::bb::Slice<std::uint8_t>,
                                 void>;

std::vector<std::uint8_t> bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string str(const std::vector<std::uint8_t>& b) {
    return std::string(b.begin(), b.end());
}

class DispatchRequestTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nats_ = std::make_unique<NatsBridge>("127.0.0.1", 4222);

        node_.emplace(iox2::NodeBuilder().create<iox2::ServiceType::Ipc>().value());

        auto service = node_->service_builder(iox2::ServiceName::create(kTestServiceName).value())
                           .request_response<iox2::bb::Slice<std::uint8_t>, iox2::bb::Slice<std::uint8_t>>()
                           .open_or_create()
                           .value();

        server_.emplace(service.server_builder()
                             .initial_max_slice_len(256)
                             .allocation_strategy(iox2::AllocationStrategy::PowerOfTwo)
                             .create()
                             .value());
        client_.emplace(service.client_builder()
                             .initial_max_slice_len(256)
                             .allocation_strategy(iox2::AllocationStrategy::PowerOfTwo)
                             .create()
                             .value());
    }

    static void TearDownTestSuite() {
        client_.reset();
        server_.reset();
        node_.reset();
        nats_.reset();
    }

    void SetUp() override {
        name_cache_.emplace(100);
        id_cache_.emplace(100);
    }

    // One iteration of what server_readthrough.cpp's main loop does: drain
    // completions, then dispatch every currently-pending new request.
    void pump_server() {
        for (auto& completion : completions_.drain()) {
            auto response_bytes = completion.apply(*name_cache_, *id_cache_);
            respond(completion.active_request, response_bytes);
        }
        while (true) {
            auto active_request_opt = server_->receive().value();
            if (!active_request_opt.has_value()) {
                break;
            }
            const auto& payload = active_request_opt->payload();
            std::vector<std::uint8_t> request_bytes(payload.data(), payload.data() + payload.number_of_bytes());
            dispatch_request(*name_cache_, *id_cache_, *nats_, kNameBucket, kIdBucket, completions_,
                              std::move(active_request_opt.value()), request_bytes.data(), request_bytes.size());
        }
    }

    auto send_request(const std::vector<std::uint8_t>& request_bytes) {
        auto request = client_->loan_slice_uninit(request_bytes.size()).value();
        auto initialized = request.write_from_fn([&](auto byte_idx) { return request_bytes[byte_idx]; });
        return send(std::move(initialized)).value();
    }

    // Sends a request and pumps the server (with a short sleep between
    // attempts) until a response arrives or 5s elapse.
    template <typename PendingResponse>
    std::vector<std::uint8_t> wait_for_response(PendingResponse& pending) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            pump_server();
            auto response = pending.receive().value();
            if (response.has_value()) {
                const auto& payload = response->payload();
                return std::vector<std::uint8_t>(payload.data(), payload.data() + payload.number_of_bytes());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("wait_for_response: no response within deadline");
    }

    std::vector<std::uint8_t> round_trip(const std::vector<std::uint8_t>& request_bytes) {
        auto pending = send_request(request_bytes);
        return wait_for_response(pending);
    }

    static std::unique_ptr<NatsBridge> nats_;
    static std::optional<NodeType> node_;
    static std::optional<ServerType> server_;
    static std::optional<ClientType> client_;

    std::optional<NameCache> name_cache_;
    std::optional<IdCache> id_cache_;
    CompletionQueue completions_;
};

std::unique_ptr<NatsBridge> DispatchRequestTest::nats_;
std::optional<NodeType> DispatchRequestTest::node_;
std::optional<ServerType> DispatchRequestTest::server_;
std::optional<ClientType> DispatchRequestTest::client_;

TEST_F(DispatchRequestTest, LocalHitRespondsWithoutTouchingNats) {
    name_cache_->emplace(NameEntry{"alice", bytes("A")});

    auto response_bytes = round_trip(encode_get(wire::KeyKind::Name, "alice", 0));
    wire::Reader r(response_bytes.data(), response_bytes.size());
    ASSERT_EQ(static_cast<wire::Status>(r.u8()), wire::Status::Ok);
    EXPECT_EQ(str(r.remaining()), "A");
    EXPECT_EQ(completions_.size(), 0u) << "a local hit must never touch the completion queue";
}

TEST_F(DispatchRequestTest, LocalMissReadsThroughToNatsAndPopulatesCache) {
    nats_->erase(kNameBucket, "readthrough_test");
    ASSERT_TRUE(nats_->put(kNameBucket, "readthrough_test", bytes("from-nats")).first);

    auto response_bytes = round_trip(encode_get(wire::KeyKind::Name, "readthrough_test", 0));
    wire::Reader r(response_bytes.data(), response_bytes.size());
    ASSERT_EQ(static_cast<wire::Status>(r.u8()), wire::Status::Ok);
    EXPECT_EQ(str(r.remaining()), "from-nats");

    auto it = name_cache_->find<NameTag>("readthrough_test");
    ASSERT_NE(it, name_cache_->end<NameTag>());
    EXPECT_EQ(str(it->record), "from-nats");

    nats_->erase(kNameBucket, "readthrough_test");
}

TEST_F(DispatchRequestTest, GetOfKeyMissingFromBothLayersReturnsNotFound) {
    nats_->erase(kIdBucket, "999999");
    auto response_bytes = round_trip(encode_get(wire::KeyKind::Id, "", 999999));
    wire::Reader r(response_bytes.data(), response_bytes.size());
    EXPECT_EQ(static_cast<wire::Status>(r.u8()), wire::Status::NotFound);
}

TEST_F(DispatchRequestTest, PutIsWriteThroughAndAppliesOnCompletion) {
    nats_->erase(kIdBucket, "42");

    auto response_bytes = round_trip(encode_put(wire::KeyKind::Id, "", 42, bytes("write-through-value")));
    wire::Reader r(response_bytes.data(), response_bytes.size());
    EXPECT_EQ(static_cast<wire::Status>(r.u8()), wire::Status::Ok);

    // Confirm it's really in NATS, not just the local cache.
    auto nats_result = nats_->get(kIdBucket, "42");
    ASSERT_EQ(nats_result.result, NatsResult::Ok);
    EXPECT_EQ(str(nats_result.value), "write-through-value");

    auto it = id_cache_->find<IdTag>(42);
    ASSERT_NE(it, id_cache_->end<IdTag>());
    EXPECT_EQ(str(it->record), "write-through-value");

    nats_->erase(kIdBucket, "42");
}

TEST_F(DispatchRequestTest, EraseIsWriteThroughAndAppliesOnCompletion) {
    ASSERT_TRUE(nats_->put(kNameBucket, "erase_test", bytes("x")).first);
    name_cache_->emplace(NameEntry{"erase_test", bytes("x")});

    auto response_bytes = round_trip(encode_erase(wire::KeyKind::Name, "erase_test", 0));
    wire::Reader r(response_bytes.data(), response_bytes.size());
    EXPECT_EQ(static_cast<wire::Status>(r.u8()), wire::Status::Ok);

    EXPECT_EQ(nats_->get(kNameBucket, "erase_test").result, NatsResult::NotFound);
    EXPECT_EQ(name_cache_->find<NameTag>("erase_test"), name_cache_->end<NameTag>());
}

TEST_F(DispatchRequestTest, MalformedRequestRespondsWithErrorWithinOnePump) {
    // Op::Get, KeyKind::Name, then a length prefix claiming ~2GB with no
    // actual bytes behind it -- same shape used to verify the real servers
    // survive a malformed request.
    std::vector<std::uint8_t> garbage{static_cast<std::uint8_t>(wire::Op::Get),
                                       static_cast<std::uint8_t>(wire::KeyKind::Name), 0xFF, 0xFF, 0xFF, 0x7F};
    auto pending = send_request(garbage);

    pump_server();  // a malformed request must respond synchronously within this one pump
    EXPECT_EQ(completions_.size(), 0u);

    auto response = pending.receive().value();
    ASSERT_TRUE(response.has_value()) << "malformed request should have responded within a single pump";
    wire::Reader r(response->payload().data(), response->payload().number_of_bytes());
    EXPECT_EQ(static_cast<wire::Status>(r.u8()), wire::Status::Error);
}

TEST_F(DispatchRequestTest, InFlightMissDoesNotBlockAConcurrentLocalHit) {
    name_cache_->emplace(NameEntry{"already_cached", bytes("cached-value")});

    nats_->erase(kNameBucket, "blocking_check");
    ASSERT_TRUE(nats_->put(kNameBucket, "blocking_check", bytes("from-nats")).first);

    // Dispatch the miss and pump exactly once -- it must be deferred, not
    // yet answered, proving dispatch_request() itself didn't block on NATS.
    auto miss_pending = send_request(encode_get(wire::KeyKind::Name, "blocking_check", 0));
    pump_server();
    EXPECT_FALSE(miss_pending.receive().value().has_value())
        << "the miss should still be in flight to NATS, not answered yet";

    // A completely unrelated local hit must still work immediately, proving
    // the in-flight miss above isn't holding up other requests.
    auto hit_bytes = round_trip(encode_get(wire::KeyKind::Name, "already_cached", 0));
    wire::Reader hit_r(hit_bytes.data(), hit_bytes.size());
    ASSERT_EQ(static_cast<wire::Status>(hit_r.u8()), wire::Status::Ok);
    EXPECT_EQ(str(hit_r.remaining()), "cached-value");

    // Let the original miss finish too, so it doesn't leak into other tests.
    auto miss_bytes = wait_for_response(miss_pending);
    wire::Reader miss_r(miss_bytes.data(), miss_bytes.size());
    EXPECT_EQ(static_cast<wire::Status>(miss_r.u8()), wire::Status::Ok);

    nats_->erase(kNameBucket, "blocking_check");
}

}  // namespace
}  // namespace poc
