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

This is **not** wired into the top-level `CMakeLists.txt` and is not built by
CI. The base demo depends on `iceoryx2-cxx` (Rust toolchain to build); the
NATS-backed demo additionally depends on `nats_asio` and its own much
heavier dependency graph (asio, fmt, concurrentqueue, gtl, magic-enum,
nlohmann-json, openssl, simdjson, spdlog, stringzilla, zstd). Both are
dependencies the rest of this header-only, Boost-only project deliberately
does not take on. Treat this directory as a standalone experiment, not a
supported part of the library.

## What it demonstrates

- `cache_service.hpp` defines **two independent** `multi_index_lru::Container`
  instances — `NameCache` (hashed on a `std::string`) and `IdCache` (hashed
  on an `int64_t`) — each holding an opaque byte blob per entry (a stand-in
  for a real [zerialize](../zerialize_cache.cpp)-produced flexbuffer/msgpack
  record). They're unrelated caches, not two indices over the same entries:
  this exists to demo/test both a string-keyed and an integral-keyed
  `Container` side by side under one service.
- `wire.hpp` is a tiny little-endian binary encoder/decoder for the
  request/response messages (POC-scope only: assumes a little-endian host).
- `server.cpp` is the sole owner of both containers (respecting the "not
  thread-safe, no internal locking" constraint documented in the main
  README) and answers `request_response<Slice<u8>, Slice<u8>>` requests
  carrying **get / put / erase**, each naming which cache to hit.
- `client.cpp` runs a demo script exercising all three operations against
  both caches (see below), or a single manual command when given arguments.
- `server_readthrough.cpp` / `nats_bridge.hpp` / `client_readthrough.cpp`:
  the same wire protocol and caches, but backed by NATS JetStream KV — see
  "Read-through / write-through over NATS" below.

Everything else — LRU eviction, TTL, composite keys, node pooling — still
lives entirely in `multi_index_lru::Container` on the server side; iceoryx2
only carries bytes across the process boundary.

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

## Unit tests for wire.hpp

`test/wire_test.cpp` is a `ctest`-integrated GoogleTest suite covering the
`wire::Writer`/`wire::Reader` binary protocol in isolation: byte-level
encoding of each primitive, round-trips (including negative/extreme `i64`
values, empty strings, and binary-safe strings with embedded NUL bytes),
truncation raising `std::out_of_range` at every field type (including the
exact "huge length prefix, no data behind it" shape used to verify the
servers survive a malformed request), an off-by-one boundary check, and a
pinned regression test on the `Op`/`KeyKind`/`Status` enum values themselves
-- since those are the actual wire format, a silent renumbering would break
compatibility between a client and server built at different times.

`wire.hpp` has no dependency on iceoryx2, `nats_asio`, Boost, or
`multi_index_lru` -- it's a standalone header -- so `cache_poc_wire_test`
only links against GTest. Configuring this directory at all still requires
`iceoryx2-cxx` (see below), since the other targets in the same
`CMakeLists.txt` aren't optional; `POC_BUILD_TESTS` (default `ON`) only
controls whether the test target itself is added.

```bash
cmake -S example/iceoryx2_cache_poc -B build-poc \
      -DCMAKE_PREFIX_PATH=/path/to/iceoryx2/install
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

2. **The two KV buckets, created once** (`nats_asio` has no bucket-creation
   call — it assumes the backing JetStream stream already exists):
   ```bash
   nats --server localhost:4222 kv add mil_by_name
   nats --server localhost:4222 kv add mil_by_id
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
[server] GET name="seeded_directly" -> local miss, NATS hit (populating cache)
[server] GET name="seeded_directly" -> local hit
[server] PUT name="via_daemon" -> NATS ok, cache updated
[server] GET id=777 -> local miss, NATS hit (populating cache)
[server] GET id=777 -> local hit
[server] ERASE id=777 -> NATS ok, cache updated
```

### The concurrency bridge (and its cost)

`nats_asio` is built entirely on `asio::awaitable` coroutines driven by an
`asio::io_context`; the iceoryx2 request loop is a plain synchronous
`while (node.wait(...))` poll loop with no event loop of its own.
`nats_bridge.hpp` bridges the two the simplest way that keeps correctness
obvious: it runs the `io_context` on one dedicated background thread, and
every NATS call from the main thread spawns a coroutine on that thread and
**blocks the caller** (via `std::promise`/`std::future`) until it completes.

This keeps both containers single-owner/single-writer — only the main thread
ever touches `NameCache`/`IdCache`, so `multi_index_lru`'s "no internal
locking" contract holds exactly as documented — and is simple enough to trust
by inspection. The cost: a NATS round trip (a miss, or any write) stalls
*every other pending client request* to the daemon for its duration. Fine
for a POC proving the pattern works; not a load-bearing design for real
concurrent traffic. A non-blocking version would hold the iceoryx2
`ActiveRequest` open and keep servicing other requests while a NATS fetch
runs in the background, replying whenever it completes — meaningfully more
code (a pending-request table, and depends on whether iceoryx2's
`ActiveRequest` can be held/responded to outside the receive loop), and
deliberately out of scope here.

## What this doesn't answer yet

This proves the pattern works end-to-end and is easy to wire up, not that
it's the right architecture for a given workload. Open questions before this
becomes more than a POC:

- **TTL / expiration.** `ExpirableContainer` isn't wired into either server;
  the write path only covers `Container`'s plain get/put/erase, not TTL
  refresh or `cleanup_expired()`. NATS KV itself supports a max-age per
  bucket, which isn't used here either.
- **Upsert races.** The erase-then-emplace upsert (base server) and the
  NATS-first-then-cache write-through (NATS server) are correct because each
  server is single-threaded and processes one request at a time — either
  would need real transactional handling if the server ever became
  multi-threaded.
- **No invalidation broadcast.** Every daemon keeps its own local cache with
  no cross-daemon coordination; if you ran two `cache_poc_server_readthrough`
  processes against the same NATS buckets, a PUT on one wouldn't invalidate
  the other's stale local entry. NATS KV's `kv_watch` is sitting right there
  as the natural fix and isn't used here.
- **Negative caching.** A NATS miss is not cached, so a key that's genuinely
  absent from both layers gets a full NATS round trip on every GET.
- **Latency under real load / concurrency.** See "the concurrency bridge"
  above — this POC does one request at a time with no contention, and proves
  nothing about throughput with many concurrent clients.
- **Failure handling.** No reconnect/retry logic beyond what `nats_asio`
  does internally, no handling of the daemon process dying mid-request
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
- **Build footprint.** Two Rust-adjacent toolchains (Rust itself for
  iceoryx2-cxx, plus nats_asio's own sizeable vcpkg dependency tree) for a
  project that otherwise only needs Boost + a C++20 compiler; worth weighing
  against lighter alternatives before committing to either.
