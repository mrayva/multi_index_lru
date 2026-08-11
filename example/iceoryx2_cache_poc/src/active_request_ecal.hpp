/// The eCAL-backed half of server_dispatch_common.hpp's transport seam --
/// see active_request_iceoryx2.hpp for the iceoryx2 counterpart and its doc
/// comment for why the split exists at all.
///
/// eCAL's CServiceServer method callback (eCAL::ServiceMethodCallbackT --
/// see /home/mrayva/ecal/install/include/ecal/service/types.h) must fill
/// its response and return synchronously, from whatever thread eCAL invokes
/// it on: `std::function<int(SServiceMethodInformation, const std::string&
/// request, std::string& response)>`. There is no defer-and-respond-later
/// handle the way iceoryx2's ActiveRequest gives one. A promise stands in
/// for that handle instead: the callback (server_readthrough_ecal.cpp)
/// pushes a job onto IncomingRequestQueue below and blocks on the paired
/// future; respond() here just unblocks it with whatever response bytes
/// dispatch_request() built, on whichever thread actually processed the
/// request (server_readthrough_ecal.cpp's single worker thread owns that,
/// same as every other cache mutation -- see its file comment).
///
/// This also means multi_index_lru::Container's single-owner/no-locking
/// contract, and KeyOperationQueue/RevisionTracker's own "only ever touched
/// from the main thread" comments in server_dispatch_common.hpp, still
/// hold exactly as they do for the iceoryx2 build: however many eCAL
/// callback threads are blocked waiting on their own promises, none of them
/// ever touches the caches directly -- they only push a job and wait.
#pragma once

#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

namespace poc {

using ActiveRequestType = std::promise<std::vector<std::uint8_t>>;

// Move-only, same reasoning as active_request_iceoryx2.hpp's identical
// comment: CompletionQueue/KeyOperationQueue need their stored closures
// copy-constructible.
using ActiveRequestPtr = std::shared_ptr<ActiveRequestType>;

inline void respond(ActiveRequestType& active_request, const std::vector<std::uint8_t>& response_bytes) {
    active_request.set_value(response_bytes);
}

// One raw request handed from an eCAL method-callback thread to the single
// worker thread that owns the caches (server_readthrough_ecal.cpp's main
// loop). `active_request` is a plain (not yet shared_ptr-wrapped)
// ActiveRequestType -- dispatch_request() takes it by value and wraps it
// itself, exactly as it does for the iceoryx2 build (see
// dispatch_read_request()/dispatch_write_request()'s
// `std::make_shared<ActiveRequestType>(std::move(active_request))`). Its
// future is fulfilled by respond() above, from the worker thread, once
// dispatch_request() has produced (or deferred, via CompletionQueue, and
// later produced) the response bytes; the eCAL callback thread that pushed
// this is blocked on that future the entire time.
struct PendingIncomingRequest {
    std::vector<std::uint8_t> request_bytes;
    ActiveRequestType active_request;
};

// Thread-safe hand-off, same shape as CompletionQueue/InvalidationQueue in
// server_dispatch_common.hpp: cheap push from many eCAL callback threads,
// drained only by the single worker thread.
class IncomingRequestQueue {
public:
    void push(std::vector<std::uint8_t> request_bytes, ActiveRequestType active_request) {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(PendingIncomingRequest{std::move(request_bytes), std::move(active_request)});
    }

    std::vector<PendingIncomingRequest> drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PendingIncomingRequest> out;
        out.reserve(items_.size());
        for (auto& item : items_) {
            out.push_back(std::move(item));
        }
        items_.clear();
        return out;
    }

private:
    std::mutex mutex_;
    std::deque<PendingIncomingRequest> items_;
};

}  // namespace poc
