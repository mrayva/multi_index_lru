/// The non-blocking request-dispatch entry point for server_readthrough.cpp,
/// pulled into its own header so it's includable from tests: dispatch_request()
/// needs a real iceoryx2 ActiveRequest, which can only come from an actual
/// Server that actually received a RequestMut over real IPC -- there's no
/// standalone/fake way to construct one -- so testing it means driving a real
/// (in-process) client/server pair, which test/dispatch_request_test.cpp does.
///
/// dispatch_request() itself is a thin router: decode op/kind, then hand off
/// to dispatch_read_request() (server_dispatch_read.hpp -- Get, GetAll) or
/// dispatch_write_request() (server_dispatch_write.hpp -- Put, Erase). The
/// two are kept in separate files with no read-side access to
/// put_async/erase_async and no write-side access to NameCache/IdCache, so
/// "is this a read or a write" is a question the file layout answers, not
/// just a runtime branch -- see server_dispatch_common.hpp for what both
/// sides still legitimately share (KeyOperationQueue, CompletionQueue,
/// cross-daemon coherence).
///
/// See server_readthrough.cpp's file comment and README.md's "the concurrency
/// model" for the design this implements, including KeyOperationQueue
/// (server_dispatch_common.hpp), which serializes operations on the same key
/// so a GET can never race a concurrent PUT/ERASE for that key and return an
/// inconsistent result.
#pragma once

#include "server_dispatch_common.hpp"
#include "server_dispatch_read.hpp"
#include "server_dispatch_write.hpp"

namespace poc {

// Parses one request and routes it to the read or write dispatcher by `op`
// (Get/GetAll -> dispatch_read_request(); Put/Erase -> dispatch_write_request(),
// which owns the `allow_writes` gate -- see its own doc comment for why
// Get/GetAll are never gated). Either responds immediately (a malformed
// request) or hands `active_request` off to be responded to once whatever
// async NATS operation the chosen dispatcher kicked off completes.
//
// `max_getall_results` caps how many matched keys a GetAll(prefix) request
// actually fetches from NATS -- see dispatch_read_request()'s doc comment.
//
// Any exception here (malformed/truncated request -- see wire::Reader) is
// caught and turned into an immediate Error response; the same hardening
// server.cpp's handle_request_local() has. Parsing (and any exception it can
// throw) always happens before `active_request` is converted to a
// KeyOperationQueue-compatible ActiveRequestPtr -- inside whichever
// dispatcher is called, not here -- so this catch block can always respond
// with the still-intact `active_request` parameter directly, whether the
// exception came from op/kind decoding here or from further parsing inside
// dispatch_read_request()/dispatch_write_request().
inline void dispatch_request(NameCache& name_cache, IdCache& id_cache, SecurityCache& security_cache, NatsBridge& nats,
                              const std::string& name_bucket, const std::string& id_bucket,
                              const std::string& security_bucket, CompletionQueue& completions,
                              KeyOperationQueue& key_queue, bool allow_writes, std::size_t max_getall_results,
                              ActiveRequestType active_request, const std::uint8_t* data, std::size_t size) {
    try {
        wire::Reader r(data, size);
        const auto op = static_cast<wire::Op>(r.u8());
        const auto kind = static_cast<wire::KeyKind>(r.u8());

        if (op == wire::Op::Get || op == wire::Op::GetAll) {
            dispatch_read_request(name_cache, id_cache, security_cache, nats, name_bucket, id_bucket, security_bucket,
                                   completions, key_queue, max_getall_results, op, kind, r, active_request);
        } else {
            dispatch_write_request(nats, name_bucket, id_bucket, security_bucket, completions, key_queue,
                                    allow_writes, op, kind, r, active_request);
        }
    } catch (const std::exception&) {
        respond(active_request, encode_status(wire::Status::Error));
    } catch (...) {
        respond(active_request, encode_status(wire::Status::Error));
    }
}

}  // namespace poc
