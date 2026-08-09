// Tests poc::NatsBridge against a real local NATS server -- there's no
// injectable fake for nats_asio::iconnection (NatsBridge always calls the
// real nats_asio::connect()), so this is an integration test, not a hermetic
// unit test. It's excluded from the dependency-free poc-unit-tests CI job
// for exactly that reason (this target only exists when
// POC_ENABLE_NATS_READTHROUGH=ON, which that CI job doesn't set) -- see
// README.md "Read-through / write-through over NATS" > "Setup" for what
// needs to be running first:
//   ./nats-server -js -m 8222
//   nats --server localhost:4222 kv add mil_bridge_test
#include "../src/nats_bridge.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace poc {
namespace {

constexpr auto kBucket = "mil_bridge_test";
constexpr auto kHost = "127.0.0.1";
constexpr std::uint16_t kPort = 4222;

std::vector<std::uint8_t> bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string str(const std::vector<std::uint8_t>& b) {
    return std::string(b.begin(), b.end());
}

// One NatsBridge (one TCP connection, one background thread) shared across
// every test in this file, rather than reconnecting per test -- connecting
// is the slow part, the KV operations themselves are fast.
class NatsBridgeTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { nats_ = std::make_unique<NatsBridge>(kHost, kPort); }
    static void TearDownTestSuite() { nats_.reset(); }

    static std::unique_ptr<NatsBridge> nats_;
};

std::unique_ptr<NatsBridge> NatsBridgeTest::nats_;

// --- blocking API ------------------------------------------------------

TEST_F(NatsBridgeTest, GetOfNeverWrittenKeyReturnsNotFound) {
    nats_->erase(kBucket, "get_missing");  // idempotent: clean slate regardless of prior runs
    auto result = nats_->get(kBucket, "get_missing");
    EXPECT_EQ(result.result, NatsResult::NotFound);
}

TEST_F(NatsBridgeTest, PutThenGetReturnsTheValue) {
    ASSERT_TRUE(nats_->put(kBucket, "put_get", bytes("hello")).first);
    auto result = nats_->get(kBucket, "put_get");
    ASSERT_EQ(result.result, NatsResult::Ok);
    EXPECT_EQ(str(result.value), "hello");
}

TEST_F(NatsBridgeTest, SecondPutOverwritesTheValue) {
    ASSERT_TRUE(nats_->put(kBucket, "overwrite", bytes("v1")).first);
    ASSERT_TRUE(nats_->put(kBucket, "overwrite", bytes("v2")).first);
    auto result = nats_->get(kBucket, "overwrite");
    ASSERT_EQ(result.result, NatsResult::Ok);
    EXPECT_EQ(str(result.value), "v2");
}

TEST_F(NatsBridgeTest, PutIsBinarySafe) {
    std::vector<std::uint8_t> value{'\x00', '\x01', 'a', '\x00', 'b'};
    ASSERT_TRUE(nats_->put(kBucket, "binary", value).first);
    auto result = nats_->get(kBucket, "binary");
    ASSERT_EQ(result.result, NatsResult::Ok);
    EXPECT_EQ(result.value, value);
}

TEST_F(NatsBridgeTest, EraseRemovesTheKey) {
    ASSERT_TRUE(nats_->put(kBucket, "erase_me", bytes("x")).first);
    ASSERT_EQ(nats_->get(kBucket, "erase_me").result, NatsResult::Ok);

    ASSERT_TRUE(nats_->erase(kBucket, "erase_me").first);
    EXPECT_EQ(nats_->get(kBucket, "erase_me").result, NatsResult::NotFound);
}

TEST_F(NatsBridgeTest, GetFromNonexistentBucketIsAnError) {
    // Distinct from NotFound: the bucket itself was never `nats kv add`-ed,
    // so there's no backing JetStream stream to answer at all -- exercises
    // the same failure mode server_readthrough.cpp would hit from a
    // misconfigured bucket name.
    auto result = nats_->get("mil_bridge_test_bucket_that_does_not_exist", "anything");
    EXPECT_EQ(result.result, NatsResult::Error);
    EXPECT_FALSE(result.error.empty());
}

// --- async API: on_done fires on the NATS thread, not the caller's -------

TEST_F(NatsBridgeTest, GetAsyncInvokesCallbackWithTheResult) {
    ASSERT_TRUE(nats_->put(kBucket, "async_get", bytes("async-value")).first);

    std::promise<NatsGetResult> prom;
    auto fut = prom.get_future();
    nats_->get_async(kBucket, "async_get", [&prom](NatsGetResult result) { prom.set_value(std::move(result)); });

    auto result = fut.get();
    ASSERT_EQ(result.result, NatsResult::Ok);
    EXPECT_EQ(str(result.value), "async-value");
}

TEST_F(NatsBridgeTest, PutAsyncThenGetAsyncRoundTrip) {
    std::promise<bool> put_done;
    nats_->put_async(kBucket, "async_put", bytes("via-async-put"),
                      [&put_done](bool ok, std::string) { put_done.set_value(ok); });
    ASSERT_TRUE(put_done.get_future().get());

    std::promise<NatsGetResult> get_done;
    nats_->get_async(kBucket, "async_put", [&get_done](NatsGetResult result) { get_done.set_value(std::move(result)); });
    auto result = get_done.get_future().get();
    ASSERT_EQ(result.result, NatsResult::Ok);
    EXPECT_EQ(str(result.value), "via-async-put");
}

TEST_F(NatsBridgeTest, EraseAsyncInvokesCallback) {
    nats_->put(kBucket, "async_erase", bytes("x"));

    std::promise<bool> erase_done;
    nats_->erase_async(kBucket, "async_erase", [&erase_done](bool ok, std::string) { erase_done.set_value(ok); });
    EXPECT_TRUE(erase_done.get_future().get());
    EXPECT_EQ(nats_->get(kBucket, "async_erase").result, NatsResult::NotFound);
}

// --- the actual point of get_async/put_async/erase_async: concurrency ----

TEST_F(NatsBridgeTest, ConcurrentGetAsyncCallsAllCompleteIndependently) {
    constexpr int kCount = 5;
    std::vector<std::string> keys;
    std::vector<std::promise<NatsGetResult>> proms(kCount);
    for (int i = 0; i < kCount; ++i) {
        keys.push_back("concurrent_" + std::to_string(i));
        ASSERT_TRUE(nats_->put(kBucket, keys.back(), bytes("value-" + std::to_string(i))).first);
    }

    // Fire all kCount requests without waiting between them -- get_async()
    // returning is not the same as the NATS round trip completing, so this
    // loop itself should be fast regardless of NATS latency.
    for (int i = 0; i < kCount; ++i) {
        nats_->get_async(kBucket, keys[static_cast<std::size_t>(i)],
                          [&proms, i](NatsGetResult result) { proms[static_cast<std::size_t>(i)].set_value(std::move(result)); });
    }

    for (int i = 0; i < kCount; ++i) {
        auto result = proms[static_cast<std::size_t>(i)].get_future().get();
        ASSERT_EQ(result.result, NatsResult::Ok) << "key index " << i;
        EXPECT_EQ(str(result.value), "value-" + std::to_string(i));
    }
}

// --- constructor: connection failure -------------------------------------

TEST(NatsBridgeConnectTest, UnreachableServerThrows) {
    // Port 1 is a reserved/privileged port nothing is listening on; the
    // constructor's connect-wait loop (up to 100 * 50ms) should time out and
    // throw rather than hang or silently produce an unusable bridge.
    EXPECT_THROW(NatsBridge(kHost, 1), std::runtime_error);
}

// --- circuit breaker -------------------------------------------------------
// A short op_timeout/open_duration here keeps these tests fast; the point
// being tested is the breaker's state machine, not the specific durations.

TEST(NatsBridgeCircuitBreakerTest, OpensAfterConsecutiveFailuresAndFastFails) {
    // op_timeout=200ms, failure_threshold=2, open_duration=10s (long enough
    // that this test can't accidentally observe it closing again).
    NatsBridge nats(kHost, kPort, std::chrono::milliseconds(200), 2, std::chrono::milliseconds(10000));
    ASSERT_FALSE(nats.is_circuit_open());

    constexpr auto kMissingBucket = "mil_bridge_test_bucket_that_does_not_exist";
    for (int i = 0; i < 2; ++i) {
        auto result = nats.get(kMissingBucket, "anything");
        EXPECT_EQ(result.result, NatsResult::Error) << "failure " << i;
    }
    EXPECT_TRUE(nats.is_circuit_open());

    // With the circuit open, a call must fail near-instantly (no NATS round
    // trip attempted) rather than waiting out another 200ms op_timeout.
    const auto start = std::chrono::steady_clock::now();
    auto result = nats.get(kBucket, "irrelevant, should short-circuit before touching NATS");
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(result.result, NatsResult::Error);
    EXPECT_LT(elapsed, std::chrono::milliseconds(50)) << "should fail fast without attempting a NATS round trip";
}

TEST(NatsBridgeCircuitBreakerTest, ClosesAfterCooldownOnSuccessfulProbe) {
    // failure_threshold=1 so a single failure opens it; open_duration=100ms
    // so the test doesn't have to wait long for the cooldown.
    NatsBridge nats(kHost, kPort, std::chrono::milliseconds(200), 1, std::chrono::milliseconds(100));

    constexpr auto kMissingBucket = "mil_bridge_test_bucket_that_does_not_exist";
    ASSERT_EQ(nats.get(kMissingBucket, "anything").result, NatsResult::Error);
    ASSERT_TRUE(nats.is_circuit_open());

    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let open_duration elapse
    EXPECT_FALSE(nats.is_circuit_open()) << "cooldown elapsed, should allow a probe through";

    // A successful call (against a real bucket/key) should close the circuit.
    ASSERT_TRUE(nats.put(kBucket, "breaker_probe", bytes("probe-value")).first);
    EXPECT_FALSE(nats.is_circuit_open());

    nats.erase(kBucket, "breaker_probe");
}

TEST(NatsBridgeCircuitBreakerTest, NotFoundDoesNotCountAsFailure) {
    NatsBridge nats(kHost, kPort, std::chrono::milliseconds(200), 1, std::chrono::milliseconds(10000));

    nats.erase(kBucket, "breaker_notfound_check");
    for (int i = 0; i < 5; ++i) {
        auto result = nats.get(kBucket, "breaker_notfound_check");
        ASSERT_EQ(result.result, NatsResult::NotFound) << "iteration " << i;
    }
    EXPECT_FALSE(nats.is_circuit_open()) << "repeated NotFound is NATS answering correctly, not a failure";
}

}  // namespace
}  // namespace poc
