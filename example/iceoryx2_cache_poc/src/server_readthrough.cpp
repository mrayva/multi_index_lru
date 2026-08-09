/// POC: multi_index_lru as a cache-aside / read-through + write-through
/// layer in front of NATS JetStream KV, served over the same iceoryx2
/// request-response protocol as server.cpp (cache_poc_client talks to
/// either).
///
/// Two independent Container instances (string-keyed / int64-keyed), each
/// backed by its own NATS KV bucket (default names below, see "Configurable
/// via..." further down).
///   - name cache -> bucket "mil_by_name"
///   - id cache   -> bucket "mil_by_id"
///
/// Both buckets must already exist -- nats_asio has no bucket-creation call,
/// see README.md for the one-time `nats kv add` setup commands.
///
/// GET: a local hit responds immediately. A local miss kicks off an async
/// NATS JetStream KV `kv_get` and returns without responding; the response
/// (and, on a NATS hit, populating the local cache -- read-through) happens
/// once that completes, via CompletionQueue (server_dispatch.hpp). PUT and
/// ERASE are write-through and always go to NATS: `kv_put`/`kv_delete` is
/// kicked off async, and the local cache is only touched -- and the response
/// only sent -- once that NATS write completes, so the cache never holds
/// something NATS doesn't durably have.
///
/// Read-only by default: this daemon expects NATS KV to be populated by
/// something else (a producer service, `nats kv put`, another system
/// entirely) and normally just reads through it -- so PUT/ERASE are
/// rejected with Status::ReadOnly unless started with
/// --allow-writes/MIL_ALLOW_WRITES. This isn't just a permission check:
/// with writes disabled, every kv_watch event this daemon sees is
/// necessarily someone else's change (see "Cross-daemon coherence" below),
/// which is exactly the invariant RevisionTracker's self-echo suppression
/// exists to protect when writes *are* allowed. Enabling --allow-writes
/// doesn't turn that suppression off -- it's still there -- but a read-only
/// deployment doesn't need to reason about it at all.
///
/// Nothing here blocks the main iceoryx2 request loop on a NATS round trip:
/// while one client's GET/PUT/ERASE is waiting on NATS, the loop keeps
/// receiving and dispatching every other client's requests. See
/// "the concurrency model" in README.md for how this differs from (and
/// improves on) the single-threaded blocking-bridge design this replaced.
///
/// Cross-daemon coherence: this daemon also subscribes (via NatsBridge::watch())
/// to every key change in both buckets. When another process -- another
/// daemon instance, a raw `nats kv put`, anything -- writes a key this
/// daemon has cached, the watch event invalidates the local entry so the
/// next GET for it reads the fresh value through from NATS. RevisionTracker
/// (server_dispatch.hpp) recognizes and skips watch echoes of this daemon's
/// own writes, so a PUT here doesn't immediately evict the value it just
/// wrote.
///
/// Prewarm: NATS delivers the *current* value of every existing key in a
/// bucket immediately on subscribing to it (kv_watch's "initial burst"),
/// not just future changes -- for --prewarm-window-ms/MIL_PREWARM_WINDOW_MS
/// after startup (default 2s), apply_pending_invalidations()
/// (server_dispatch.hpp) uses that burst to populate the cache directly
/// from each event's own value instead of merely evicting, so a freshly
/// (re)started daemon doesn't pay a separate NATS round trip for every key
/// its first wave of GETs happens to want. Scoped to startup only --
/// once the window closes, behavior is exactly the eviction-only one
/// described above, unchanged.
///
/// The actual request-dispatch logic lives in server_dispatch.hpp, pulled
/// out of this file so test/dispatch_request_test.cpp can drive it directly.
///
/// Configurable via CLI flag or env var (flag wins if both are given):
///   --service-name / MIL_SERVICE_NAME               (default poc::kServiceName)
///   --cache-capacity / MIL_CACHE_CAPACITY            (default 1000, applies to both caches)
///   --ttl-ms / MIL_TTL_MS                            (default 300000 = 5min, applies to both caches)
///   --nats-host / MIL_NATS_HOST                      (default 127.0.0.1)
///   --nats-port / MIL_NATS_PORT                      (default 4222)
///   --name-bucket / MIL_NAME_BUCKET                  (default poc::kNameBucket)
///   --id-bucket / MIL_ID_BUCKET                       (default poc::kIdBucket)
///   --nats-timeout-ms / MIL_NATS_TIMEOUT_MS          (default 3000)
///   --cb-failure-threshold / MIL_CB_FAILURE_THRESHOLD (default 3)
///   --cb-open-duration-ms / MIL_CB_OPEN_DURATION_MS   (default 2000)
///   --max-clients / MIL_MAX_CLIENTS                   (default 16 -- iceoryx2-cxx's own
///                                                        default is 8; see README.md "Load testing")
///   --max-active-requests-per-client / MIL_MAX_ACTIVE_REQUESTS_PER_CLIENT
///                                                      (default 32 -- iceoryx2-cxx's own default is 4)
///   --max-queue-depth-per-key / MIL_MAX_QUEUE_DEPTH_PER_KEY
///                                                      (default 64 -- backpressure: a key already
///                                                       this deep in KeyOperationQueue rejects any
///                                                       further request for it with Error instead of
///                                                       queuing indefinitely; see README.md "Backpressure")
///   --allow-writes / MIL_ALLOW_WRITES                 (default false -- Put/Erase are rejected with
///                                                       Status::ReadOnly unless set; see "Read-only
///                                                       by default" above)
///   --prewarm-window-ms / MIL_PREWARM_WINDOW_MS       (default 2000 -- how long after startup
///                                                       kv_watch events populate the cache directly
///                                                       from their own value instead of merely
///                                                       evicting; see "Prewarm" above)
///   --max-getall-results / MIL_MAX_GETALL_RESULTS     (default 100 -- caps how many matched keys a
///                                                       GetAll(prefix) request fetches from NATS;
///                                                       see cache_service.hpp "Prefix lookup (GetAll,
///                                                       KeyKind::Name)")
#include "server_dispatch.hpp"
#include "config.hpp"

#include "iox2/iceoryx2.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr iox2::bb::Duration kCycleTime = iox2::bb::Duration::from_millis(5);
constexpr std::uint64_t kInitialSliceLenHint = 256;
// How often the main loop reclaims capacity slots held by expired-but-
// unaccessed entries -- see cleanup_expired_periodically() in
// cache_service.hpp.
constexpr auto kCleanupInterval = std::chrono::seconds(1);
}  // namespace

int main(int argc, char** argv) {
    using namespace iox2;

    set_log_level_from_env_or(LogLevel::Info);
    std::cout << std::unitbuf;  // flush every line -- this process is normally run with stdout redirected to a log file

    std::vector<std::string> args(argv + 1, argv + argc);
    const std::string service_name = poc::config::resolve_str(args, "--service-name", "MIL_SERVICE_NAME", poc::kServiceName);
    const std::size_t capacity = poc::config::resolve_size(args, "--cache-capacity", "MIL_CACHE_CAPACITY", 1000);
    const auto ttl = poc::config::resolve_millis(args, "--ttl-ms", "MIL_TTL_MS", 300000);
    const std::string nats_host = poc::config::resolve_str(args, "--nats-host", "MIL_NATS_HOST", "127.0.0.1");
    const std::uint16_t nats_port = poc::config::resolve_u16(args, "--nats-port", "MIL_NATS_PORT", 4222);
    const std::string name_bucket = poc::config::resolve_str(args, "--name-bucket", "MIL_NAME_BUCKET", poc::kNameBucket);
    const std::string id_bucket = poc::config::resolve_str(args, "--id-bucket", "MIL_ID_BUCKET", poc::kIdBucket);
    const auto nats_timeout = poc::config::resolve_millis(args, "--nats-timeout-ms", "MIL_NATS_TIMEOUT_MS", 3000);
    const int cb_failure_threshold =
        poc::config::resolve_int(args, "--cb-failure-threshold", "MIL_CB_FAILURE_THRESHOLD", 3);
    const auto cb_open_duration =
        poc::config::resolve_millis(args, "--cb-open-duration-ms", "MIL_CB_OPEN_DURATION_MS", 2000);
    const std::size_t max_clients = poc::config::resolve_size(args, "--max-clients", "MIL_MAX_CLIENTS", 16);
    const std::size_t max_active_requests_per_client = poc::config::resolve_size(
        args, "--max-active-requests-per-client", "MIL_MAX_ACTIVE_REQUESTS_PER_CLIENT", 32);
    const std::size_t max_queue_depth_per_key =
        poc::config::resolve_size(args, "--max-queue-depth-per-key", "MIL_MAX_QUEUE_DEPTH_PER_KEY", 64);
    const bool allow_writes = poc::config::resolve_bool(args, "--allow-writes", "MIL_ALLOW_WRITES", false);
    const auto prewarm_window =
        poc::config::resolve_millis(args, "--prewarm-window-ms", "MIL_PREWARM_WINDOW_MS", 2000);
    const std::size_t max_getall_results =
        poc::config::resolve_size(args, "--max-getall-results", "MIL_MAX_GETALL_RESULTS", 100);

    // Declared before `nats` on purpose: local variables are destroyed in
    // reverse declaration order, and NatsBridge's own coroutines (get_async/
    // put_async/erase_async/watch callbacks) capture references to some of
    // these. `nats` must be the *last* of this group to be destroyed --
    // hence declared last -- so its destructor (which stops and joins the
    // NATS thread) always runs before anything it might still be using is
    // torn down, even if a NATS operation is still in flight when main()'s
    // loop exits (e.g. on SIGTERM).
    poc::NameCache name_cache(capacity, ttl);
    poc::IdCache id_cache(capacity, ttl);
    poc::CompletionQueue completions;
    poc::KeyOperationQueue key_queue(max_queue_depth_per_key);
    poc::RevisionTracker revisions;
    poc::InvalidationQueue invalidations;
    auto last_cleanup = std::chrono::steady_clock::now();

    std::cout << "[server] connecting to NATS at " << nats_host << ":" << nats_port << " ...\n";
    poc::NatsBridge nats(nats_host, nats_port, nats_timeout, cb_failure_threshold, cb_open_duration);
    std::cout << "[server] connected to NATS\n";

    // Cross-daemon coherence: watch every key in both buckets. `entry.bucket`
    // tells the shared callback which cache the change applies to. Runs on
    // the NATS thread -- just hands off to InvalidationQueue, no cache or
    // iceoryx2 access here. `entry.value`/`entry.op` are forwarded through
    // too, unused outside the startup prewarm window below (see
    // apply_pending_invalidations()) but harmless (and cheap) to always
    // carry -- keeps the decision of whether they matter entirely on the
    // main thread, not duplicated here.
    auto on_kv_change = [&invalidations, name_bucket](const nats_asio::kv_entry& entry) {
        invalidations.push(entry.bucket == name_bucket, entry.key, entry.revision,
                            std::vector<std::uint8_t>(entry.value.begin(), entry.value.end()),
                            entry.op == nats_asio::kv_entry::operation::put);
    };
    nats.watch(name_bucket, on_kv_change);
    nats.watch(id_bucket, on_kv_change);
    std::cout << "[server] watching buckets \"" << name_bucket << "\" / \"" << id_bucket
              << "\" for external changes, prewarming from their current contents for the next "
              << prewarm_window.count() << "ms\n";

    // Prewarm: for prewarm_window after the watch subscriptions above are
    // established, apply_pending_invalidations() populates the cache
    // directly from each watch event's own value (including kv_watch's
    // initial burst -- NATS delivering the *current* value of every
    // existing key immediately on subscribe) instead of merely evicting --
    // see its doc comment for the full rationale and why this is scoped to
    // startup only.
    const auto prewarm_deadline = std::chrono::steady_clock::now() + prewarm_window;

    auto node = NodeBuilder().create<ServiceType::Ipc>().value();

    auto service = node.service_builder(ServiceName::create(service_name.c_str()).value())
                        .request_response<bb::Slice<std::uint8_t>, bb::Slice<std::uint8_t>>()
                        .max_clients(max_clients)
                        .max_active_requests_per_client(max_active_requests_per_client)
                        .open_or_create()
                        .value();

    auto server = service.server_builder()
                      .initial_max_slice_len(kInitialSliceLenHint)
                      .allocation_strategy(AllocationStrategy::PowerOfTwo)
                      .create()
                      .value();

    std::cout << "[server] read-through cache daemon ready (non-blocking), service \"" << service_name
              << "\", buckets \"" << name_bucket << "\" / \"" << id_bucket << "\", writes "
              << (allow_writes ? "enabled (--allow-writes)" : "disabled (read-only by default)")
              << ". Ctrl+C to stop.\n";

    while (node.wait(kCycleTime).has_value()) {
        // Respond to whatever NATS operations finished since the last cycle
        // before looking at new requests, so deferred requests don't wait
        // behind a burst of fresh local-hit ones.
        for (auto& completion : completions.drain()) {
            auto response_bytes = completion.apply(name_cache, id_cache);
            const auto status = static_cast<poc::wire::Status>(response_bytes[0]);
            std::cout << "[server] " << completion.description << " -> "
                      << (status == poc::wire::Status::Ok       ? "ok (NATS)"
                          : status == poc::wire::Status::NotFound ? "not found (local miss, NATS miss)"
                                                                    : "error (NATS)")
                      << "\n";
            poc::respond(*completion.active_request, response_bytes);
            if (completion.has_revision) {
                // So the watch echo of this exact write/read doesn't also
                // trigger a redundant invalidation -- see RevisionTracker.
                revisions.observe(completion.queue_key, completion.revision);
            }
            // Releases the per-key slot so the next queued operation (if
            // any) for the same key -- e.g. a GET that arrived while this
            // PUT was still in flight -- only starts now, after this
            // completion's cache mutation and response are both done.
            key_queue.complete(completion.queue_key);
        }

        poc::apply_pending_invalidations(name_cache, id_cache, revisions, invalidations, prewarm_deadline);
        poc::cleanup_expired_periodically(name_cache, id_cache, last_cleanup, kCleanupInterval);

        while (true) {
            auto active_request_opt = server.receive().value();
            if (!active_request_opt.has_value()) {
                break;
            }

            const auto& payload = active_request_opt->payload();
            std::vector<std::uint8_t> request_bytes(payload.data(), payload.data() + payload.number_of_bytes());

            poc::dispatch_request(name_cache, id_cache, nats, name_bucket, id_bucket, completions, key_queue,
                                   allow_writes, max_getall_results, std::move(active_request_opt.value()),
                                   request_bytes.data(), request_bytes.size());
        }
    }

    std::cout << "[server] exit\n";
    return 0;
}
