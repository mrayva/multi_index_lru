/// zenoh port of server_readthrough.cpp -- same cache-aside read-through +
/// write-through logic in front of NATS JetStream KV (see that file's own
/// doc comment for the full behavior: read-through, write-through,
/// cross-daemon coherence via kv_watch, prewarm), served over a zenoh
/// Queryable instead of an iceoryx2 request-response service. Everything
/// except the transport is identical -- dispatch_request()
/// (server_dispatch.hpp) and everything it calls into is unmodified,
/// unaware this isn't iceoryx2 (see server_dispatch_common.hpp's doc
/// comment on the active_request_iceoryx2.hpp / active_request_ecal.hpp /
/// active_request_zenoh.hpp seam).
///
/// Unlike the eCAL port, no separate incoming-request hand-off queue is
/// needed: `session.declare_queryable<zenoh::channels::FifoChannel>(...)`
/// gives back a handler whose `try_recv()` is itself a non-blocking poll
/// for the next `zenoh::Query` -- filled by zenoh's own I/O thread(s),
/// drained here on the single dispatch thread, exactly the role iceoryx2's
/// `server.receive()` plays. A `zenoh::Query` is a movable, independently
/// reply-able handle (verified live with a spike before writing this file
/// -- see active_request_zenoh.hpp's doc comment), so it can be moved
/// straight into `dispatch_request()` and, if deferred, into
/// `CompletionQueue` via `std::make_shared<ActiveRequestType>` exactly like
/// the iceoryx2 build's `iox2::ActiveRequest`.
///
/// zenoh has no session-level equivalent of iceoryx2's signal-aware
/// `node.wait()` or eCAL's `eCAL::Ok()`, so this file installs its own
/// SIGINT/SIGTERM handler to flip `g_keep_running` for a clean shutdown.
///
/// Configuration flags/env vars are identical to server_readthrough.cpp
/// (see that file's doc comment for the full list), minus the
/// iceoryx2-specific ones (--max-clients, --max-active-requests-per-client,
/// no zenoh equivalent), plus:
///   --zenoh-port / MIL_ZENOH_PORT  (default 17447 -- zenoh listens on
///                                    tcp/127.0.0.1:<port>; client_readthrough
///                                    _zenoh.cpp must connect to the same
///                                    port. See its own doc comment for why
///                                    an explicit TCP endpoint is used
///                                    instead of zenoh's default multicast
///                                    scouting.)
#include "server_dispatch.hpp"
#include "config.hpp"

#include <zenoh.hxx>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr auto kCycleTime = std::chrono::milliseconds(5);
// How often the main loop reclaims capacity slots held by expired-but-
// unaccessed entries -- see cleanup_expired_periodically() in
// cache_service.hpp.
constexpr auto kCleanupInterval = std::chrono::seconds(1);
// How many queries the zenoh FIFO channel buffers before a new one would
// block its (zenoh-internal) producer thread -- generously sized, same
// spirit as the iceoryx2 build's --max-active-requests-per-client.
constexpr std::size_t kQueryableFifoCapacity = 256;

std::atomic<bool> g_keep_running{true};
void handle_shutdown_signal(int) { g_keep_running.store(false); }
}  // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;  // flush every line -- this process is normally run with stdout redirected to a log file

    std::vector<std::string> args(argv + 1, argv + argc);
    const std::string service_name = poc::config::resolve_str(args, "--service-name", "MIL_SERVICE_NAME", poc::kServiceName);
    const std::size_t capacity = poc::config::resolve_size(args, "--cache-capacity", "MIL_CACHE_CAPACITY", 1000);
    const auto ttl = poc::config::resolve_millis(args, "--ttl-ms", "MIL_TTL_MS", 300000);
    const std::string nats_host = poc::config::resolve_str(args, "--nats-host", "MIL_NATS_HOST", "127.0.0.1");
    const std::uint16_t nats_port = poc::config::resolve_u16(args, "--nats-port", "MIL_NATS_PORT", 4222);
    const std::string name_bucket = poc::config::resolve_str(args, "--name-bucket", "MIL_NAME_BUCKET", poc::kNameBucket);
    const std::string id_bucket = poc::config::resolve_str(args, "--id-bucket", "MIL_ID_BUCKET", poc::kIdBucket);
    const std::string security_bucket =
        poc::config::resolve_str(args, "--security-bucket", "MIL_SECURITY_BUCKET", poc::kSecurityBucket);
    const auto nats_timeout = poc::config::resolve_millis(args, "--nats-timeout-ms", "MIL_NATS_TIMEOUT_MS", 3000);
    const int cb_failure_threshold =
        poc::config::resolve_int(args, "--cb-failure-threshold", "MIL_CB_FAILURE_THRESHOLD", 3);
    const auto cb_open_duration =
        poc::config::resolve_millis(args, "--cb-open-duration-ms", "MIL_CB_OPEN_DURATION_MS", 2000);
    const std::size_t max_queue_depth_per_key =
        poc::config::resolve_size(args, "--max-queue-depth-per-key", "MIL_MAX_QUEUE_DEPTH_PER_KEY", 64);
    const bool allow_writes = poc::config::resolve_bool(args, "--allow-writes", "MIL_ALLOW_WRITES", false);
    const auto prewarm_window =
        poc::config::resolve_millis(args, "--prewarm-window-ms", "MIL_PREWARM_WINDOW_MS", 2000);
    const std::size_t max_getall_results =
        poc::config::resolve_size(args, "--max-getall-results", "MIL_MAX_GETALL_RESULTS", 100);
    const std::uint16_t zenoh_port = poc::config::resolve_u16(args, "--zenoh-port", "MIL_ZENOH_PORT", 17447);

    // Same declaration-order reasoning as server_readthrough.cpp: `nats`
    // must be the last of this group to be destroyed, so its background
    // NATS thread is stopped and joined before anything it might still be
    // using is torn down.
    poc::NameCache name_cache(capacity, ttl);
    poc::IdCache id_cache(capacity, ttl);
    poc::SecurityCache security_cache(capacity, ttl);
    poc::CompletionQueue completions;
    poc::KeyOperationQueue key_queue(max_queue_depth_per_key);
    poc::RevisionTracker revisions;
    poc::InvalidationQueue invalidations;
    auto last_cleanup = std::chrono::steady_clock::now();

    std::cout << "[server] connecting to NATS at " << nats_host << ":" << nats_port << " ...\n";
    poc::NatsBridge nats(nats_host, nats_port, nats_timeout, cb_failure_threshold, cb_open_duration);
    std::cout << "[server] connected to NATS\n";

    // Cross-daemon coherence -- identical to server_readthrough.cpp, see
    // its file comment for the full rationale.
    auto on_kv_change = [&invalidations, name_bucket, id_bucket](const nats_asio::kv_entry& entry) {
        const auto cache_kind = (entry.bucket == name_bucket)  ? poc::CacheKind::Name
                                 : (entry.bucket == id_bucket) ? poc::CacheKind::Id
                                                                 : poc::CacheKind::Security;
        invalidations.push(cache_kind, entry.key, entry.revision,
                            std::vector<std::uint8_t>(entry.value.begin(), entry.value.end()),
                            entry.op == nats_asio::kv_entry::operation::put);
    };
    nats.watch(name_bucket, on_kv_change);
    nats.watch(id_bucket, on_kv_change);
    nats.watch(security_bucket, on_kv_change);
    std::cout << "[server] watching buckets \"" << name_bucket << "\" / \"" << id_bucket << "\" / \""
              << security_bucket << "\" for external changes, prewarming from their current contents for the next "
              << prewarm_window.count() << "ms\n";

    const auto prewarm_deadline = std::chrono::steady_clock::now() + prewarm_window;

    auto config = zenoh::Config::create_default();
    // Explicit TCP endpoint rather than zenoh's default multicast
    // scouting -- multicast proved unreliable in the environment this was
    // spiked in (WSL2), and this POC only needs same-host/same-subnet
    // discovery anyway. client_readthrough_zenoh.cpp connects to the same
    // port.
    config.insert_json5("listen/endpoints", "[\"tcp/127.0.0.1:" + std::to_string(zenoh_port) + "\"]");
    auto session = zenoh::Session::open(std::move(config));

    zenoh::KeyExpr keyexpr(service_name);
    // FifoChannel handler, not a callback: `try_recv()` below is a genuine
    // non-blocking poll, so the main loop can pull requests directly off
    // it -- no separate incoming-request queue needed (see
    // active_request_zenoh.hpp's doc comment).
    auto queryable = session.declare_queryable<zenoh::channels::FifoChannel>(
        keyexpr, zenoh::channels::FifoChannel(kQueryableFifoCapacity));

    std::signal(SIGINT, handle_shutdown_signal);
    std::signal(SIGTERM, handle_shutdown_signal);

    std::cout << "[server] read-through cache daemon ready (zenoh), key expression \"" << keyexpr.as_string_view()
              << "\" on tcp/127.0.0.1:" << zenoh_port << ", buckets \"" << name_bucket << "\" / \"" << id_bucket
              << "\" / \"" << security_bucket << "\", writes "
              << (allow_writes ? "enabled (--allow-writes)" : "disabled (read-only by default)")
              << ". Ctrl+C to stop.\n";

    while (g_keep_running.load()) {
        // Respond to whatever NATS operations finished since the last
        // cycle before looking at new requests -- same ordering rationale
        // as server_readthrough.cpp's identical comment.
        for (auto& completion : completions.drain()) {
            auto response_bytes = completion.apply(name_cache, id_cache, security_cache);
            const auto status = static_cast<poc::wire::Status>(response_bytes[0]);
            std::cout << "[server] " << completion.description << " -> "
                      << (status == poc::wire::Status::Ok       ? "ok (NATS)"
                          : status == poc::wire::Status::NotFound ? "not found (local miss, NATS miss)"
                                                                    : "error (NATS)")
                      << "\n";
            poc::respond(*completion.active_request, response_bytes);
            if (completion.has_revision) {
                revisions.observe(completion.queue_key, completion.revision);
            }
            key_queue.complete(completion.queue_key);
        }

        poc::apply_pending_invalidations(name_cache, id_cache, security_cache, revisions, invalidations,
                                          prewarm_deadline);
        poc::cleanup_expired_periodically(name_cache, id_cache, security_cache, last_cleanup, kCleanupInterval);

        // New requests -- pull directly off the zenoh FIFO channel until
        // it's empty, the same shape as server.cpp's/server_readthrough
        // .cpp's `while (true) { server.receive() ... }` loops.
        while (true) {
            auto recv_result = queryable.handler().try_recv();
            auto* query = std::get_if<zenoh::Query>(&recv_result);
            if (query == nullptr) {
                break;  // Z_NODATA (nothing waiting) or Z_DISCONNECTED
            }
            std::vector<std::uint8_t> request_bytes;
            if (auto payload = query->get_payload()) {
                request_bytes = payload->get().as_vector();
            }
            poc::dispatch_request(name_cache, id_cache, security_cache, nats, name_bucket, id_bucket,
                                   security_bucket, completions, key_queue, allow_writes, max_getall_results,
                                   std::move(*query), request_bytes.data(), request_bytes.size());
        }

        std::this_thread::sleep_for(kCycleTime);
    }

    std::cout << "[server] exit\n";
    return 0;
}
