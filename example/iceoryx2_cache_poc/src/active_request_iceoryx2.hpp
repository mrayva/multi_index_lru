/// The iceoryx2-backed half of server_dispatch_common.hpp's transport
/// seam: `ActiveRequestType`/`respond()` for the iceoryx2 request-response
/// service. See active_request_ecal.hpp for the eCAL counterpart --
/// dispatch_request()/dispatch_read_request()/dispatch_write_request()
/// (and everything else in server_dispatch_common.hpp) don't know or care
/// which of the two is in play; they only ever see the `ActiveRequestType`/
/// `ActiveRequestPtr`/`respond()` names.
#pragma once

#include "iox2/iceoryx2.hpp"

#include <memory>
#include <vector>

namespace poc {

// request_response<Slice<u8>, Slice<u8>>() with no explicit header types
// resolves to `void` headers -- see ServiceBuilder::request_response() in
// iceoryx2-cxx/include/iox2/service_builder.hpp.
using ActiveRequestType =
    iox2::ActiveRequest<iox2::ServiceType::Ipc, iox2::bb::Slice<std::uint8_t>, void, iox2::bb::Slice<std::uint8_t>,
                         void>;

// ActiveRequestType is move-only by design. It needs to be captured by
// closures stored in both CompletionQueue and KeyOperationQueue
// (server_dispatch_common.hpp), both of which need those closures to be
// copy-constructible (std::function's requirement) -- a shared_ptr is the
// simplest way to get that without hand-rolling a move-only type-erased
// callable.
using ActiveRequestPtr = std::shared_ptr<ActiveRequestType>;

inline void respond(ActiveRequestType& active_request, const std::vector<std::uint8_t>& response_bytes) {
    auto response = active_request.loan_slice_uninit(response_bytes.size()).value();
    auto initialized = response.write_from_fn([&](auto byte_idx) { return response_bytes[byte_idx]; });
    send(std::move(initialized)).value();
}

}  // namespace poc
