/// eCAL port of server_readthrough.cpp -- same cache-aside read-through +
/// write-through logic in front of NATS JetStream KV (see that file's own
/// doc comment for the full behavior: read-through, write-through,
/// cross-daemon coherence via kv_watch, prewarm), served over an eCAL
/// Service instead of an iceoryx2 request-response service. Everything
/// except the transport is identical -- dispatch_request()
/// (server_dispatch.hpp) and everything it calls into is unmodified,
/// unaware this isn't iceoryx2 (see server_dispatch_common.hpp's doc
/// comment on the active_request_iceoryx2.hpp / active_request_ecal.hpp
/// seam).
///
/// The one real architectural difference: eCAL's CServiceServer method
/// callback (eCAL::ServiceMethodCallbackT) must fill its response and
/// return synchronously, from whatever thread eCAL invokes it on -- there's
/// no defer-and-respond-later handle the way iceoryx2's ActiveRequest gives
/// one. So the callback below does the minimum possible on its own thread:
/// push {request_bytes, promise} onto IncomingRequestQueue
/// (active_request_ecal.hpp) and block on the paired future. All of
/// dispatch_request(), CompletionQueue, KeyOperationQueue, RevisionTracker,
/// and every cache mutation still happen on exactly one thread -- this
/// file's main loop, below -- same single-owner/no-locking invariant
/// multi_index_lru::Container and server_dispatch_common.hpp already
/// documented for the iceoryx2 build. However many eCAL callback threads
/// are blocked waiting on their own promises at once, none of them ever
/// touches the caches.
///
/// Configuration flags/env vars are identical to server_readthrough.cpp,
/// minus the iceoryx2-specific ones (--max-clients,
/// --max-active-requests-per-client, which have no eCAL equivalent) --
/// see that file's doc comment for the full list.
#include "server_dispatch.hpp"
#include "config.hpp"

#include <ecal/ecal.h>
#include <ecal/service/server.h>

#include <chrono>
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
    poc::IncomingRequestQueue incoming;
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

    eCAL::Initialize(service_name);

    // One method, "dispatch" -- the entire wire::Reader-decoded Op/KeyKind
    // space (Get/GetAll/Put/Erase) is multiplexed over it, exactly as
    // iceoryx2's single request_response service is. The callback itself
    // does no cache or NATS work: it hands the raw bytes to the single
    // worker thread below and blocks until that thread has an answer.
    eCAL::CServiceServer server(service_name);
    server.SetMethodCallback(
        eCAL::SServiceMethodInformation{"dispatch", {}, {}},
        [&incoming](const eCAL::SServiceMethodInformation&, const std::string& request, std::string& response) -> int {
            poc::ActiveRequestType active_request;
            auto future = active_request.get_future();
            incoming.push(std::vector<std::uint8_t>(request.begin(), request.end()), std::move(active_request));
            auto response_bytes = future.get();  // blocks this eCAL callback thread only
            response.assign(response_bytes.begin(), response_bytes.end());
            return 0;
        });

    std::cout << "[server] read-through cache daemon ready (eCAL), service \"" << service_name << "\", buckets \""
              << name_bucket << "\" / \"" << id_bucket << "\" / \"" << security_bucket << "\", writes "
              << (allow_writes ? "enabled (--allow-writes)" : "disabled (read-only by default)")
              << ". Ctrl+C to stop.\n";

    while (eCAL::Ok()) {
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

        // New requests -- the eCAL equivalent of server_readthrough.cpp's
        // `while (true) { server.receive() ... }` loop, just fed by
        // IncomingRequestQueue instead of iceoryx2 polling directly.
        for (auto& incoming_request : incoming.drain()) {
            poc::dispatch_request(name_cache, id_cache, security_cache, nats, name_bucket, id_bucket, security_bucket,
                                   completions, key_queue, allow_writes, max_getall_results,
                                   std::move(incoming_request.active_request), incoming_request.request_bytes.data(),
                                   incoming_request.request_bytes.size());
        }

        std::this_thread::sleep_for(kCycleTime);
    }

    eCAL::Finalize();
    std::cout << "[server] exit\n";
    return 0;
}
