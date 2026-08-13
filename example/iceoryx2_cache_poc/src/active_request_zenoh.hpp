/// The zenoh-backed half of server_dispatch_common.hpp's transport seam --
/// see active_request_iceoryx2.hpp for the iceoryx2 counterpart and
/// active_request_ecal.hpp for the eCAL one.
///
/// Unlike eCAL's synchronous-only service callback (which forced
/// active_request_ecal.hpp to build its own IncomingRequestQueue +
/// std::promise/future hand-off), zenoh's `zenoh::Query` -- the type a
/// `Session::declare_queryable` callback/handler hands over per request --
/// is itself an owned, movable handle: nothing requires it to be replied to
/// before the request-producing code returns it. Verified live (not just
/// from the docs) with a spike server that pulled a Query off a
/// `zenoh::channels::FifoChannel` handler, moved it into a std::deque,
/// slept to simulate an in-flight async op, and replied to it from a later
/// loop iteration -- exactly server_readthrough_zenoh.cpp's real usage
/// below. So `ActiveRequestType` is just `zenoh::Query` directly: no
/// separate incoming-request queue type is needed here at all --
/// `Queryable<FifoHandler<Query>>::handler().try_recv()`
/// (server_readthrough_zenoh.cpp's main loop) already *is* that queue,
/// filled by zenoh's own internal I/O thread(s) and drained non-blockingly
/// by the single dispatch thread, the same role iceoryx2's `server.receive()`
/// plays for the iceoryx2 build.
#pragma once

#include <zenoh.hxx>

#include <cstdint>
#include <memory>
#include <vector>

namespace poc {

using ActiveRequestType = zenoh::Query;

// Move-only, same reasoning as active_request_iceoryx2.hpp's identical
// comment: CompletionQueue/KeyOperationQueue need their stored closures
// copy-constructible.
using ActiveRequestPtr = std::shared_ptr<ActiveRequestType>;

inline void respond(ActiveRequestType& active_request, const std::vector<std::uint8_t>& response_bytes) {
    active_request.reply(active_request.get_keyexpr(), zenoh::Bytes(response_bytes));
}

}  // namespace poc
