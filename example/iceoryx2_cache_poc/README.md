# iceoryx2 request-response POC (experimental)

Prototype of `multi_index_lru` served as a shared, cross-process cache: one
process owns two `Container` instances (`server.cpp`), other processes look
up a key and get the whole matching record back as raw bytes (`client.cpp`),
over an [iceoryx2](https://github.com/eclipse-iceoryx/iceoryx2)
request-response service (zero-copy shared memory transport, no network
stack involved). `server_readthrough.cpp` / `client_readthrough.cpp` extend
this into a cache-aside / read-through + write-through layer in front of
[NATS](https://nats.io) JetStream KV, using
[mrayva/nats_asio](https://github.com/mrayva/nats_asio).

This is **not** wired into the top-level `CMakeLists.txt`, and the iceoryx2-
and NATS-backed servers/clients are not built by CI. The base demo depends
on `iceoryx2-cxx` (Rust toolchain to build); the NATS-backed demo
additionally depends on `nats_asio` and its own much heavier dependency
graph (asio, fmt, concurrentqueue, gtl, magic-enum, nlohmann-json, openssl,
simdjson, spdlog, stringzilla, zstd). Both are dependencies the rest of this
header-only, Boost-only project deliberately does not take on. Treat this
directory as a standalone experiment, not a supported part of the library --
the one exception is `test/wire_test.cpp` and
`test/handle_request_local_test.cpp`, which have no dependency on either
toolchain and *are* run by the main CI workflow (`poc-unit-tests` in
`.github/workflows/ci.yml`) on every push, since they only need Boost and a
C++20 compiler (see "Unit tests" below).

## What it demonstrates

- `cache_service.hpp` defines **two independent**
  `multi_index_lru::ExpirableContainer` instances — `NameCache` (hashed on a
  `std::string`) and `IdCache` (hashed on an `int64_t`) — each holding an
  opaque byte blob per entry (a stand-in for a real
  [zerialize](../zerialize_cache.cpp)-produced flexbuffer/msgpack record)
  that expires after a configurable TTL on top of the usual LRU-capacity
  eviction (`--ttl-ms`/`MIL_TTL_MS`, see "Configuration" below). They're
  unrelated caches, not two indices over the same entries: this exists to
  demo/test both a string-keyed and an integral-keyed `ExpirableContainer`
  side by side under one service.
- `wire.hpp` is a tiny little-endian binary encoder/decoder for the
  request/response messages (POC-scope only: assumes a little-endian host).
- `server.cpp` is the sole owner of both containers (respecting the "not
  thread-safe, no internal locking" constraint documented in the main
  README) and answers `request_response<Slice<u8>, Slice<u8>>` requests
  carrying **get / put / erase**, each naming which cache to hit.
- `client.cpp` runs a demo script exercising all three operations against
  both caches (see below), or a single manual command when given arguments.
- `server_readthrough.cpp` / `server_dispatch.hpp` / `nats_bridge.hpp` /
  `client_readthrough.cpp`: the same wire protocol and caches, but backed by
  NATS JetStream KV — see "Read-through / write-through over NATS" below.
  `server_dispatch.hpp` holds the actual non-blocking request-dispatch logic
  (`dispatch_request()`, `CompletionQueue`, `KeyOperationQueue`) so it's
  includable from `test/dispatch_request_test.cpp`, not just
  `server_readthrough.cpp`'s `main()`.
- `config.hpp` is the CLI-flag/env-var resolution all four binaries share —
  see "Configuration" below for the full flag/env-var list.

Everything else — LRU eviction, TTL, composite keys, node pooling — still
lives entirely in `multi_index_lru::ExpirableContainer` on the server side;
iceoryx2 only carries bytes across the process boundary.

### Wire protocol

Request: `[op:u8][key_kind:u8][...]`
- `Get`/`Erase`: `[name:u32-prefixed-str]` (key_kind=Name) or `[id:i64]` (key_kind=Id)
- `Put`: same key, then `[record_len:u32][record bytes]`

Response: `[status:u8][record bytes if Get+Ok]` — `status` is `Ok`, `NotFound`, or `Error`
(`Error` is only ever produced by the NATS-backed server, e.g. on a NATS timeout).

`Put` is an upsert against whichever cache `key_kind` selects: the server
erases any existing entry under that key first (a plain `Container::emplace()`
only inserts-if-absent, it won't overwrite an existing value on a duplicate
key) and then inserts fresh.

## Building iceoryx2-cxx

There's no released C++ package to fetch, so build it from source once and
point `CMAKE_PREFIX_PATH` at the install prefix. From the version of
`eclipse-iceoryx/iceoryx2` used for this POC (`main`, `v0.9.999`):

```bash
git clone --depth 1 https://github.com/eclipse-iceoryx/iceoryx2.git
cd iceoryx2

cargo build --release --package iceoryx2-ffi-c

cmake -S iceoryx2-cmake-modules -B target/ff/cmake-modules/build
cmake --install target/ff/cmake-modules/build --prefix target/ff/cc/install

cmake -S iceoryx2-c -B target/ff/c/build \
      -DRUST_BUILD_ARTIFACT_PATH="$(pwd)/target/release" \
      -DCMAKE_PREFIX_PATH="$(pwd)/target/ff/cc/install"
cmake --build target/ff/c/build
cmake --install target/ff/c/build --prefix target/ff/cc/install

cmake -S iceoryx2-bb/cxx -B target/ff/bb-cxx/build \
      -DCMAKE_PREFIX_PATH="$(pwd)/target/ff/cc/install"
cmake --build target/ff/bb-cxx/build
cmake --install target/ff/bb-cxx/build --prefix target/ff/cc/install

cmake -S iceoryx2-cxx -B target/ff/cxx/build \
      -DCMAKE_PREFIX_PATH="$(pwd)/target/ff/cc/install"
cmake --build target/ff/cxx/build
cmake --install target/ff/cxx/build --prefix target/ff/cc/install
```

(See `iceoryx2-cxx/README.md` in that repo for the up-to-date version of
these steps.)

## Building and running the base (in-memory) POC

```bash
cmake -S example/iceoryx2_cache_poc -B build-poc \
      -DCMAKE_PREFIX_PATH=/path/to/iceoryx2/target/ff/cc/install
cmake --build build-poc

# terminal 1
./build-poc/cache_poc_server

# terminal 2 — demo script (get/put/erase against both caches)
./build-poc/cache_poc_client

# or a single manual command:
./build-poc/cache_poc_client get name alice
./build-poc/cache_poc_client get id 2
./build-poc/cache_poc_client put name dave 'some record text'
./build-poc/cache_poc_client put id 42 'some record text'
./build-poc/cache_poc_client erase id 42
```

Expected demo-script output:

```
--- string-keyed cache ---
[client] GET name="alice" (pre-seeded) ...
  -> {"name":"Alice"}
[client] GET name="dave" (does not exist yet) ...
  -> not found
[client] PUT name="dave" record={"name":"Dave"} ...
  -> put: ok
[client] GET name="dave" (should be there now) ...
  -> {"name":"Dave"}
[client] PUT name="dave" record={"name":"Dave","v":2} (update) ...
  -> put: ok
[client] GET name="dave" (should show the update) ...
  -> {"name":"Dave","v":2}
[client] ERASE name="dave" ...
  -> erase: ok
[client] GET name="dave" (erased) ...
  -> not found

--- int64-keyed cache ---
[client] GET id=1 (pre-seeded) ...
  -> {"id":1,"name":"Alice"}
[client] GET id=42 (does not exist yet) ...
  -> not found
[client] PUT id=42 record={"id":42} ...
  -> put: ok
[client] GET id=42 (should be there now) ...
  -> {"id":42}
[client] ERASE id=42 ...
  -> erase: ok
[client] GET id=42 (erased) ...
  -> not found
[client] exit
```

This was run for real (client and server as separate OS processes,
communicating only via iceoryx2 shared memory) to produce the output above.

## Configuration

Everything that previously had to be recompiled to change is now a CLI flag
or env var, resolved by `config.hpp` (shared by all four binaries): **a CLI
flag always wins if both are given for the same setting.** Both forms are
accepted for flags with a value: `--flag value` or `--flag=value`.

| Flag | Env var | Default | Used by |
|---|---|---|---|
| `--service-name` | `MIL_SERVICE_NAME` | `poc::kServiceName` | all four -- client and server must agree |
| `--cache-capacity` | `MIL_CACHE_CAPACITY` | `1000` | `server.cpp`, `server_readthrough.cpp` (applies to both caches) |
| `--ttl-ms` | `MIL_TTL_MS` | `300000` (5min) | `server.cpp`, `server_readthrough.cpp` (applies to both caches) |
| `--nats-host` | `MIL_NATS_HOST` | `127.0.0.1` | `server_readthrough.cpp`, `client_readthrough.cpp` |
| `--nats-port` | `MIL_NATS_PORT` | `4222` | `server_readthrough.cpp`, `client_readthrough.cpp` |
| `--name-bucket` | `MIL_NAME_BUCKET` | `poc::kNameBucket` (`mil_by_name`) | `server_readthrough.cpp`, `client_readthrough.cpp` |
| `--id-bucket` | `MIL_ID_BUCKET` | `poc::kIdBucket` (`mil_by_id`) | `server_readthrough.cpp`, `client_readthrough.cpp` |
| `--nats-timeout-ms` | `MIL_NATS_TIMEOUT_MS` | `3000` | `server_readthrough.cpp` (`NatsBridge` op timeout) |
| `--cb-failure-threshold` | `MIL_CB_FAILURE_THRESHOLD` | `3` | `server_readthrough.cpp` (circuit breaker) |
| `--cb-open-duration-ms` | `MIL_CB_OPEN_DURATION_MS` | `2000` | `server_readthrough.cpp` (circuit breaker) |

`dispatch_request()` (`server_dispatch.hpp`) now takes the two bucket names
as parameters rather than reading the `poc::kNameBucket`/`poc::kIdBucket`
constants directly, so `server_readthrough.cpp` can override them per
instance -- those constants remain as the actual defaults, and
`client_readthrough.cpp`/tests still use them directly where a fixed default
is all that's needed.

Verified live, not just wired up: started `cache_poc_server` with
`--cache-capacity=3` and confirmed real LRU eviction at that capacity (the
third insert correctly evicted the least-recently-touched entry, not
whichever one happened to be oldest by insertion order); confirmed
`MIL_SERVICE_NAME` alone (no flag) lets a client find a server; confirmed a
`--service-name` flag overrides a conflicting `MIL_SERVICE_NAME` env var
(pointing the client at a nonexistent service name correctly failed to find
the running server); and started `server_readthrough` with
`--name-bucket=mil_bridge_test` and confirmed a `PUT` through it landed in
that bucket and *not* in `mil_by_name`.

```bash
# override the service name and point the read-through daemon at a
# non-default NATS instance and bucket set
./cache_poc_server_readthrough --service-name=my-cache --nats-host=nats.internal --nats-port=4222 \
    --name-bucket=my_name_bucket --id-bucket=my_id_bucket

# or via env vars (handy for containers)
MIL_SERVICE_NAME=my-cache MIL_NATS_HOST=nats.internal MIL_NAME_BUCKET=my_name_bucket \
    MIL_ID_BUCKET=my_id_bucket ./cache_poc_server_readthrough
```

`test/config_test.cpp` covers `config.hpp` itself -- see "Unit tests" below.

## TTL / expiration

Both `NameCache` and `IdCache` are `multi_index_lru::ExpirableContainer`, not
plain `Container`: on top of the LRU-capacity eviction both always had, every
entry now also expires `--ttl-ms`/`MIL_TTL_MS` after it was last written or
read (default 5 minutes, applies to both caches). `find()` slides the
expiration forward on every hit -- exactly like it already refreshes LRU
recency -- so a key under active use never expires out from under it; only a
key nobody has touched in `ttl-ms` actually goes away. A `GET` for an expired
key is a plain `NotFound` (from `server.cpp`) or a local miss that reads
through to NATS again (from `server_readthrough.cpp`) -- to that key's next
caller, it looks exactly like the entry was never cached, not like an error.

Expiration is normally lazy (checked the next time something touches that
key), which by itself would leave an entry that's written once and never
read again sitting in the cache, occupying an LRU-capacity slot, until
eviction pressure from other inserts got around to it. Both servers' main
loops also call `cleanup_expired()` on each cache once a second
(`cleanup_expired_periodically()`, `cache_service.hpp`) to reclaim those
slots proactively instead of waiting on that pressure.

Verified live: started `cache_poc_server --ttl-ms=500`, `PUT` a key, `GET` it
immediately (hit), waited 1.2s (past the 500ms TTL), `GET` it again --
`not found`. The server's own log over that run additionally showed the
2 seeded name-keyed + 2 seeded id-keyed entries (present at startup, never
touched again) drop to zero *before* the first `PUT` arrived -- proactive
`cleanup_expired()` reclaiming them on its own, not something waiting for a
`GET` to trip over them. `test/handle_request_local_test.cpp`'s
`HandleRequestLocalTtlTest` covers this at the unit level with a short TTL,
including that an access *before* expiry resets the clock rather than just
checking it (`GetBeforeExpiryRefreshesTtlSoEntrySurvivesPastOriginalDeadline`).
`ExpirableContainer`'s own TTL/eviction logic has its own generic coverage in
the main repo's `test/expirable_test.cpp` -- these POC tests are scoped to
confirming the wiring (a real, non-default TTL reaches the constructor and is
observable through `handle_request_local()`), not re-testing the library.

## Unit tests

Five `ctest`-integrated GoogleTest suites in total: three dependency-free,
and two integration suites for the NATS-backed path that need a real local
NATS server (there's no fake for `nats_asio::iconnection`, and no way to
construct a standalone/fake iceoryx2 `ActiveRequest` -- it can only come
from a real `Server` that actually received a `RequestMut` over real IPC).

The three dependency-free suites don't need iceoryx2, NATS, or a second
process to exercise:

- `test/wire_test.cpp` covers the `wire::Writer`/`wire::Reader` binary
  protocol in isolation: byte-level encoding of each primitive, round-trips
  (including negative/extreme `i64` values, empty strings, and binary-safe
  strings with embedded NUL bytes), truncation raising `std::out_of_range`
  at every field type, an off-by-one boundary check, and a pinned regression
  test on the `Op`/`KeyKind`/`Status` enum values themselves -- since those
  are the actual wire format, a silent renumbering would break compatibility
  between a client and server built at different times. `wire.hpp` has no
  dependency on iceoryx2, `nats_asio`, Boost, or `multi_index_lru` -- it's a
  standalone header -- so `cache_poc_wire_test` only links against GTest.

- `test/handle_request_local_test.cpp` covers `handle_request_local()`'s
  decode-operate-encode contract directly against a real `NameCache`/
  `IdCache` pair (no iceoryx2, no process boundary): get/put/erase for both
  key kinds, put-is-an-upsert (a second `Put` overwrites rather than
  duplicating, and the cache size stays at 1), the two caches being
  genuinely independent (a `Name` key that happens to be the digits `"42"`
  doesn't leak into the `Id` cache's key `42`), and the exception-hardening
  contract as a *repeatable* test -- an empty request, a truncated string
  length prefix (the same shape used to verify the real servers survive a
  malformed message live), and a truncated `Put` record all come back as
  `Status::Error` via `EXPECT_NO_THROW` rather than propagating, and a
  truncated `Put` is confirmed to leave the cache empty rather than
  half-applied. `multi_index_lru::Container`'s own behavior (LRU eviction,
  node pooling, etc.) is exercised in the main repo's
  `test/container_test.cpp`, not duplicated here -- these tests are scoped
  to the request-handling logic that's specific to this POC's server. This
  target links `multi_index_lru_headers` (so it needs Boost, like the rest
  of this directory) but nothing from iceoryx2 or `nats_asio`.

- `test/config_test.cpp` covers `config.hpp`'s CLI-flag/env-var resolution
  directly: both accepted flag forms (`--flag value` and `--flag=value`),
  `take_flag()` not false-matching one flag as a prefix of another (e.g.
  looking for `--id-bucket` must not match `--name-bucket`), a missing-value
  flag throwing, and flag-over-env-over-default precedence for every
  `resolve_*()` variant (`resolve_str`/`resolve_u16`/`resolve_size`/
  `resolve_int`/`resolve_millis`). Uses real `setenv`/`unsetenv` against a
  test-only env var name, cleaned up in `TearDown()` so it can't leak
  between tests. `config.hpp` is standard-library-only, same as `wire.hpp`.

`POC_BUILD_ICEORYX2_TARGETS` (default `ON`) gates `iceoryx2-cxx` itself and
every target that links it. Turned off, this directory configures and
builds with nothing but Boost and a C++20 compiler -- no Rust toolchain
needed -- which is exactly what the main repo's CI does to run these three
suites on every push (see `poc-unit-tests` in `.github/workflows/ci.yml`):

```bash
cmake -S example/iceoryx2_cache_poc -B build-poc-tests \
      -DPOC_BUILD_ICEORYX2_TARGETS=OFF -DPOC_BUILD_TESTS=ON
cmake --build build-poc-tests
ctest --test-dir build-poc-tests --output-on-failure
```

To build everything, including the iceoryx2-backed servers/clients, alongside
the tests:

```bash
cmake -S example/iceoryx2_cache_poc -B build-poc \
      -DCMAKE_PREFIX_PATH=/path/to/iceoryx2/install
cmake --build build-poc
ctest --test-dir build-poc --output-on-failure
```

### Integration tests (require a real NATS server)

Built only when `POC_ENABLE_NATS_READTHROUGH=ON` (see "Read-through /
write-through over NATS" below for the full setup, including `nats_asio`
itself); not part of the `poc-unit-tests` CI job for that reason.

- `test/nats_bridge_test.cpp` exercises `NatsBridge` directly against the
  `mil_bridge_test` bucket: blocking and async get/put/erase, overwrite,
  binary-safe values, a nonexistent-bucket error (distinct from a
  genuine key miss), several `get_async` calls fired without waiting between
  them all completing correctly (the actual point of the async API), the
  constructor throwing on a real connection failure (an unreachable port),
  and the circuit breaker (see "the circuit breaker" below) opening after
  consecutive real failures, fast-failing while open, closing again after a
  successful post-cooldown probe, and `NotFound` not counting as a failure.

- `test/dispatch_request_test.cpp` exercises `dispatch_request()`
  (`server_dispatch.hpp`, the logic `server_readthrough.cpp` runs in its
  main loop) end to end: a real iceoryx2 `Client`/`Server` pair created
  within the test process on a dedicated service name (`.../
  dispatch_request_test`, distinct from `poc::kServiceName` so it can't
  collide with a running demo daemon), driving `dispatch_request()` exactly
  as `server_readthrough.cpp`'s `main()` does. Covers: a local hit never
  touching `CompletionQueue`; a local miss reading through NATS and
  populating the cache; `Put`/`Erase` write-through, confirmed against NATS
  directly (not just the local cache); a malformed request responding with
  `Status::Error` within a single pump (not deferred); an in-flight
  NATS-bound miss provably not blocking a concurrent local hit, by pumping
  the server exactly once after dispatching the miss and asserting it has
  *no* response yet, then completing an unrelated request in full before
  letting the miss finish; and `KeyOperationQueue`'s two guarantees directly:
  a GET-miss racing a concurrent PUT for the *same* key serializes correctly
  (confirmed via a direct NATS read that the PUT hasn't touched NATS while
  the GET is still in flight, then that the GET legitimately sees the
  pre-PUT value while the cache and NATS both end up with the PUT's value,
  not a stale overwrite), and two concurrent GET-misses on the same key
  coalesce into one NATS fetch (the second resolves as a local hit in the
  very same completion that resolved the first).

```bash
cmake -S example/iceoryx2_cache_poc -B build-poc \
      -DPOC_ENABLE_NATS_READTHROUGH=ON \
      -DCMAKE_PREFIX_PATH="/path/to/iceoryx2/install;/path/to/nats_asio/install;/path/to/nats_asio/build/vcpkg_installed/x64-linux"
cmake --build build-poc
ctest --test-dir build-poc --output-on-failure
```

## Read-through / write-through over NATS

`server_readthrough.cpp` speaks the identical wire protocol as `server.cpp`
(so `cache_poc_client` works against it unchanged) but adds a backing store:

- **GET**: a local cache hit returns immediately. A local miss falls through
  to a NATS JetStream KV `kv_get` on the corresponding bucket; a NATS hit is
  written into the local cache before replying (classic cache-aside /
  read-through), so the next GET for that key is a local hit. A NATS miss
  too is a normal `NotFound`.
- **PUT** / **ERASE**: write-through. NATS is updated first (`kv_put` /
  `kv_delete`); the local cache is only touched once that succeeds, so the
  cache never holds something NATS doesn't durably have. A NATS failure is
  reported back as `Error` and the local cache is left untouched.

Each of the two caches gets its own NATS KV bucket:

| Cache      | Key type  | NATS bucket    |
|------------|-----------|----------------|
| `NameCache`| `string`  | `mil_by_name`  |
| `IdCache`  | `int64_t` | `mil_by_id` (int keys stored as decimal strings) |

### Setup

1. **A running `nats-server` with JetStream enabled** — e.g. the one this
   POC was built and tested against:
   ```bash
   ./nats-server -js -m 8222
   ```
   (default client port 4222; `-m 8222` is just the HTTP monitoring port).

2. **The KV buckets, created once** (`nats_asio` has no bucket-creation call
   — it assumes the backing JetStream stream already exists). `mil_by_name`
   / `mil_by_id` are what the servers use; `mil_bridge_test` is a third,
   separate bucket the `nats_bridge_test`/`dispatch_request_test` suites use
   so they never collide with a running demo daemon's data:
   ```bash
   nats --server localhost:4222 kv add mil_by_name
   nats --server localhost:4222 kv add mil_by_id
   nats --server localhost:4222 kv add mil_bridge_test
   ```

3. **A built `nats_asio`.** This POC was built and tested against
   [mrayva/nats_asio](https://github.com/mrayva/nats_asio) using its own
   vcpkg manifest (`cmake -S . -B build` from that repo bootstraps vcpkg and
   builds every dependency — this is the slowest step in the whole setup).
   Once built, `cmake --install <nats_asio build dir> --prefix <some prefix>`
   gives you a `nats_asio-config.cmake` you can point `CMAKE_PREFIX_PATH` at,
   alongside the vcpkg triplet dir the build produced
   (`<nats_asio build dir>/vcpkg_installed/x64-linux`) for its transitive
   dependencies.

### Building and running

```bash
cmake -S example/iceoryx2_cache_poc -B build-poc \
      -DPOC_ENABLE_NATS_READTHROUGH=ON \
      -DCMAKE_PREFIX_PATH="/path/to/iceoryx2/install;/path/to/nats_asio/install;/path/to/nats_asio/build/vcpkg_installed/x64-linux"
cmake --build build-poc

# terminal 1
./build-poc/cache_poc_server_readthrough

# terminal 2 — seeds NATS directly (bypassing the daemon) then proves
# read-through, write-through, and erase-propagation all work
./build-poc/cache_poc_client_readthrough

# cache_poc_client (the plain one) also works against this server unchanged,
# it just won't see anything seeded directly in NATS until a GET pulls it in
./build-poc/cache_poc_client get name seeded_directly
```

Don't run `cache_poc_server` and `cache_poc_server_readthrough` at the same
time — they share the same iceoryx2 service name and would both try to
answer the same requests.

`cache_poc_client_readthrough`'s output was captured from a real run (daemon
and client as separate processes, against a real local `nats-server -js`):

```
[client] connecting directly to NATS (bypassing the daemon) to seed test data ...
[client] seeded NATS bucket "mil_by_name" key "seeded_directly": ok
[client] seeded NATS bucket "mil_by_id" key "777": ok

--- read-through: string-keyed cache ---
[client] GET name="seeded_directly" (never went through the daemon) ...
  -> {"note":"came from NATS, not a daemon PUT"}
[client] GET name="seeded_directly" again (should now be a local cache hit) ...
  -> {"note":"came from NATS, not a daemon PUT"}

--- write-through: string-keyed cache ---
[client] PUT name="via_daemon" (write-through to NATS) ...
  -> put: ok
[client] direct NATS get of "via_daemon" (bypassing the daemon): {"note":"written through the daemon"}

--- read-through: int64-keyed cache ---
[client] GET id=777 (never went through the daemon) ...
  -> {"note":"came from NATS, not a daemon PUT"}
[client] GET id=777 again (should now be a local cache hit) ...
  -> {"note":"came from NATS, not a daemon PUT"}

--- erase propagates to NATS ---
[client] ERASE id=777 via the daemon ...
  -> erase: ok
[client] direct NATS get of id=777 (bypassing the daemon): not found -- confirmed NATS was updated, not just the local cache
```

And the daemon's own console log for that same run, showing the mechanism
directly:

```
[server] GET name="seeded_directly" -> local miss, dispatched to NATS
[server] GET name="seeded_directly" -> ok (NATS)
[server] GET name="seeded_directly" -> local hit
[server] PUT name="via_daemon" -> dispatched to NATS
[server] PUT name="via_daemon" -> ok (NATS)
[server] GET id=777 -> local miss, dispatched to NATS
[server] GET id=777 -> ok (NATS)
[server] GET id=777 -> local hit
[server] ERASE id=777 -> dispatched to NATS
[server] ERASE id=777 -> ok (NATS)
```

### The concurrency model

`nats_asio` is built entirely on `asio::awaitable` coroutines driven by an
`asio::io_context`; the iceoryx2 request loop is a plain synchronous
`while (node.wait(...))` poll loop with no event loop of its own.
`nats_bridge.hpp` bridges the two **without blocking the request loop**: it
runs the `io_context` on one dedicated background thread, and
`get_async()`/`put_async()`/`erase_async()` fire a coroutine on that thread
and return immediately -- the callback (`on_done`) runs on the NATS thread
once the operation completes.

`server_readthrough.cpp` uses this to keep the main loop free while a NATS
round trip is in flight: on a local miss (GET) or any write (PUT/ERASE), it
kicks off the async NATS call, moves the iceoryx2 `ActiveRequest` into the
callback's capture, and returns -- the main loop goes straight back to
`server.receive()` for the *next* request instead of waiting. When the NATS
callback fires (on the NATS thread), it doesn't touch the cache or iceoryx2
directly; it pushes an `ActiveRequest` + a small "apply this to the cache and
build the response" closure onto `CompletionQueue`, a mutex-guarded queue.
Only the main thread ever drains that queue -- doing the actual cache
mutation and sending the response -- so `multi_index_lru`'s "single owner, no
internal locking" contract and iceoryx2's "one thread touches the
`Server`/`ActiveRequest` API" both hold, exactly as before, just deferred by
one hop through the queue instead of enforced by blocking.

This was verified, not just designed: 8 keys were seeded directly into NATS
(bypassing the daemon, so each was guaranteed to be a local miss), then 7 of
them were requested via 7 separate `cache_poc_client` processes launched
nearly simultaneously. A single such request takes ~57ms wall time
(dominated by the client process's own startup/service-discovery, not NATS
latency); all 7 concurrent requests together also completed in ~58ms, and the
daemon's log showed all 7 `-> local miss, dispatched to NATS` lines before
any of their `-> ok (NATS)` completions, with completions arriving out of
request order -- both confirm the requests were genuinely handled
concurrently, not serialized behind one another the way the old
blocking-bridge design would have.

**The tradeoff this introduced, and how it's closed**: dropping the blocking
bridge also dropped the thundering-herd protection it incidentally
provided, and opened a real race -- a GET-miss racing a concurrent PUT for
the *same* key could return a stale value to the GET caller even though the
cache itself ended up correct: if client B's PUT completed and updated the
cache before client A's earlier GET-miss fetch (already in flight with the
pre-PUT value) completed, `apply()`'s `emplace()` on A's stale result was a
no-op against the now-present key (a plain `Container::emplace()` won't
overwrite an existing value), so the *cache* kept B's fresher value -- but
A's *response* still carried the stale value it fetched.

`KeyOperationQueue` (`server_dispatch.hpp`) closes both gaps with one
mechanism: at most one operation is ever "in flight" for a given
(cache, key) at a time. `dispatch_request()` funnels every Get/Put/Erase
through `key_queue.enqueue(composite_key, ...)`; if nothing's in flight for
that key it runs immediately (same as before), otherwise it queues behind
whatever is. The in-flight holder releases the slot -- via
`key_queue.complete(composite_key)` -- only once its cache mutation (if any)
and response are both done, which for a deferred (NATS) operation happens in
`server_readthrough.cpp`'s `main()` right after draining its completion.
That gives per-key linearizability: operations on the same key always
finish in the order they were dispatched, so a later write can never be
clobbered by an earlier read's now-stale completion. As a side effect,
concurrent GET-misses on the same never-cached key now coalesce too -- the
second one queues behind the first instead of firing its own redundant NATS
fetch, and finds a local hit once the first's completion has populated the
cache -- restoring the thundering-herd protection the blocking-bridge design
gave for free.

Different keys are completely unaffected -- `KeyOperationQueue` only
serializes *same*-key operations, so the concurrency demonstrated above
(7 concurrent requests for 7 distinct keys, ~58ms total) still holds
unchanged; re-verified after adding the queue with the same live multi-process
test. And this was verified for the race itself, not just designed: a test
dispatches a GET-miss for a key, confirms via a direct NATS read that a
concurrently-dispatched PUT for the *same* key has not yet touched NATS
(proving it queued rather than racing ahead), then lets both resolve and
confirms the GET legitimately received the pre-PUT value (correct, since it
ran first) while the cache and NATS both end up holding the PUT's value, not
a stale overwrite. A second test confirms two concurrent GET-misses on the
same key coalesce: the second resolves as a local hit in the very same
completion that resolved the first, with no second NATS round trip visible
in the daemon's log.

### The circuit breaker

Without it, every miss/write during a NATS outage independently waits out
the full `op_timeout` (3s by default) before failing -- correct, but slow
and wasteful for every client hitting the daemon during that outage, and
each in-flight timed-out coroutine ties up resources for the duration.

`NatsBridge` tracks consecutive failures (a real error -- timeout,
disconnected, etc. -- not a `NotFound`, which is NATS answering correctly,
just with no data). After `failure_threshold` consecutive failures (default
3) the circuit opens: every `get_async`/`put_async`/`erase_async` call fails
instantly with `Error`, without attempting a NATS round trip at all, for
`open_duration` (default 2s). The next call after that cooldown is let
through as a probe -- success closes the circuit and resets the failure
count, failure reopens it for another `open_duration`. Both are constructor
parameters, so `test/nats_bridge_test.cpp`'s `NatsBridgeCircuitBreakerTest`
suite uses short values (a couple hundred ms) to stay fast; verified live:
opens after `failure_threshold` real failures against a nonexistent bucket,
the very next call while open completes in under 50ms (vs. the 200ms
`op_timeout` a real attempt would take), and it closes again once a probe
after the cooldown succeeds.

`server_readthrough.cpp`/`server_dispatch.hpp` need no changes for this --
they already treat any NATS `Error` uniformly, so a circuit-open response
looks exactly like any other NATS failure to `dispatch_request()`.

### Cross-daemon coherence

Everything above assumes one daemon owns each cache. Run a second
`cache_poc_server_readthrough` against the same two buckets (a different
`--service-name` so they don't fight over the same iceoryx2 service, e.g. for
a horizontally-scaled deployment) and, without this, each keeps its own
local cache with no idea the other exists -- a PUT on one leaves the other
serving a now-stale value out of its local cache indefinitely.

`NatsBridge::watch(bucket, on_entry)` closes that gap: at startup,
`server_readthrough.cpp`'s `main()` subscribes to every key change (put,
delete, purge) in both buckets via NATS JetStream KV's `kv_watch`. When
*any* process writes a key -- another daemon instance, a raw `nats kv put`,
anything -- the watch event lands in `InvalidationQueue`
(`server_dispatch.hpp`); the main loop drains it once per cycle
(`apply_pending_invalidations()`) and evicts the corresponding entry from
whichever local cache holds it, so the next GET for that key is a real miss
and reads the fresh value through from NATS.

**Not evicting a daemon's own writes.** Without more care, this would make
every daemon invalidate the value it *just wrote*, the instant its own write
echoes back through its own watch subscription -- a pointless
write-then-immediately-evict flicker on every PUT. `RevisionTracker`
(`server_dispatch.hpp`) closes this using NATS KV's per-key revision (a
monotonically increasing sequence number, returned by both `kv_put` and
`kv_get`/`kv_watch`): whenever this daemon's own GET-populate or PUT
completes, it records the revision that operation produced or observed. A
watch event is only treated as genuinely new information -- and only then
does it evict -- if its revision is *greater* than whatever was last
recorded for that key; an echo of this daemon's own write carries the exact
revision already recorded, so it's recognized and skipped.

This needs a `nats_asio` build where `kv_watch`'s delivered `kv_entry`
carries a real (non-zero, strictly increasing) `revision` --
[mrayva/nats_asio](https://github.com/mrayva/nats_asio) `parse_kv_entry_from_message`
originally only looked for a `Nats-Sequence` *header*, which nats-server
attaches to `kv_get`'s Direct Get responses but not to ordinary
push-consumer deliveries (`kv_watch`'s underlying mechanism); every watch
event's revision silently came back `0`, which made `RevisionTracker`
correctly recognize *nothing* as new after the first event per key --
external writes stopped invalidating anything after that. Fixed to parse
the stream sequence out of the JetStream ACK reply-to subject instead (the
same technique `parse_js_message_metadata` already used elsewhere in that
file), which is always present on a push-consumer delivery.

Verified live: two `cache_poc_server_readthrough` instances (`--service-name`
`.../e2e_a` and `.../e2e_b`) against the same `mil_by_name` bucket. A GET
through daemon A cached `{"v":1}`; a PUT through daemon B wrote `{"v":2}` for
the same key; daemon A's log showed `invalidated name="..." (external write,
revision N)` without ever receiving a request telling it to, and daemon A's
very next GET for that key returned `{"v":2}` -- read fresh through NATS, not
served stale from its own cache. A second run confirmed a daemon's own PUT
does *not* trigger this: no `invalidated` line ever appears for a key this
same daemon just wrote. Both behaviors also have `dispatch_request_test.cpp`
coverage (`ExternalWriteInvalidatesLocalCacheEntry`,
`OwnWriteDoesNotSelfInvalidate`) that runs on every `ctest` invocation
(`POC_ENABLE_NATS_READTHROUGH=ON` builds), not just this one manual check.

## What this doesn't answer yet

This proves the pattern works end-to-end and is easy to wire up, not that
it's the right architecture for a given workload. Open questions before this
becomes more than a POC:

- **NATS KV's own max-age is unused.** "TTL / expiration" above covers the
  local caches; NATS KV buckets also support a per-bucket max-age for the
  durable data itself, independent of either daemon's local TTL, which isn't
  configured here.
- **Upsert races.** `server.cpp`'s erase-then-emplace upsert is correct
  because it's single-threaded and processes one request at a time from
  start to finish. `server_readthrough.cpp`'s equivalent is now protected by
  `KeyOperationQueue` for same-key operations (see "the concurrency model"
  above) -- but `KeyOperationQueue`'s composite keys (`"N:" + name` /
  `"I:" + id`) could theoretically collide if a `Name` key were literally the
  string `"I:5"` while an `Id` key `5` was also in flight; harmless
  over-serialization in that vanishingly unlikely case, not a correctness
  bug, but worth knowing about.
- **Invalidation is eviction, not refresh.** "Cross-daemon coherence" above
  covers multiple daemons; a watched key is dropped from the local cache, not
  proactively re-fetched, so the cost of staying coherent is paid by the next
  GET for that key (a real miss), not by the watch event itself.
- **Negative caching.** A NATS miss is not cached, so a key that's genuinely
  absent from both layers gets a full NATS round trip on every GET.
- **Throughput under sustained load.** "The concurrency model" above verified
  that several concurrent requests each requiring their own NATS round trip
  complete concurrently rather than serialized, at small scale (7 requests).
  That's a correctness/architecture check, not a load test -- it says
  nothing about behavior under sustained high concurrency (how many
  in-flight NATS operations `nats_asio`/the NATS server can actually sustain,
  memory growth of `CompletionQueue`/`KeyOperationQueue` under backlog --
  especially a long queue of operations piled up behind one slow/stuck
  operation for a single hot key -- etc.).
- **Failure handling.** No handling of the daemon process dying mid-request
  beyond what iceoryx2 does by default. A malformed/truncated request (or an
  unexpected NATS-side exception) is now caught at the request-handling
  boundary and turned into an `Error` response instead of crashing the
  daemon — see `handle_request_local()` in `cache_service.hpp` and
  `handle_request()`/`nats_bridge.hpp` in the NATS-backed server — but there's
  still no rate limiting or backpressure if a client hammers the daemon with
  bad requests. Signal handling needed no fix: iceoryx2's `node.wait()`
  already returns an unhappy result on SIGINT/SIGTERM, so the existing
  `while (node.wait(...))` loop already exits gracefully and lets `main()`
  return normally, running `NatsBridge`'s destructor (clean NATS thread
  shutdown) via ordinary RAII — confirmed by sending SIGTERM to a running
  daemon and observing `[server] exit` followed by the process disappearing.
  `NatsBridge` now has a per-instance circuit breaker (see below) for
  *sustained* NATS unavailability; it still relies entirely on `nats_asio`'s
  own internal reconnect logic to actually recover the underlying
  connection — there's no additional retry/backoff layered on top of that
  from this POC's own code.
- **Build footprint.** Two Rust-adjacent toolchains (Rust itself for
  iceoryx2-cxx, plus nats_asio's own sizeable vcpkg dependency tree) for a
  project that otherwise only needs Boost + a C++20 compiler; worth weighing
  against lighter alternatives before committing to either.
