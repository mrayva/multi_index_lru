/// The non-blocking request-dispatch machinery for server_readthrough.cpp,
/// pulled into its own header so it's includable from tests: dispatch_request()
/// needs a real iceoryx2 ActiveRequest, which can only come from an actual
/// Server that actually received a RequestMut over real IPC -- there's no
/// standalone/fake way to construct one -- so testing it means driving a real
/// (in-process) client/server pair, which test/dispatch_request_test.cpp does.
///
/// See server_readthrough.cpp's file comment and README.md's "the concurrency
/// model" for the design this implements.
#pragma once

#include "cache_service.hpp"
#include "nats_bridge.hpp"

#include "iox2/iceoryx2.hpp"

#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace poc {

// Defaults only -- dispatch_request() takes the actual bucket names as
// parameters so server_readthrough.cpp can override them via
// --name-bucket/--id-bucket or MIL_NAME_BUCKET/MIL_ID_BUCKET (see
// config.hpp). Kept as named constants since tests and client_readthrough.cpp
// still want a sensible fixed default to talk to.
inline constexpr auto kNameBucket = "mil_by_name";
inline constexpr auto kIdBucket = "mil_by_id";

// request_response<Slice<u8>, Slice<u8>>() with no explicit header types
// resolves to `void` headers -- see ServiceBuilder::request_response() in
// iceoryx2-cxx/include/iox2/service_builder.hpp.
using ActiveRequestType =
    iox2::ActiveRequest<iox2::ServiceType::Ipc, iox2::bb::Slice<std::uint8_t>, void, iox2::bb::Slice<std::uint8_t>,
                         void>;

// Runs on the main thread once a deferred request's NATS operation has
// completed: applies whatever cache mutation is needed (a Get populating the
// cache on a NATS hit, or a Put/Erase applying now that NATS confirmed it)
// and returns the response bytes to send.
using ApplyFn = std::function<std::vector<std::uint8_t>(NameCache&, IdCache&)>;

struct PendingCompletion {
    ActiveRequestType active_request;
    ApplyFn apply;
    std::string description;  // for logging only, e.g. "GET name=\"alice\"" -- built on whichever
                               // thread calls push(), but only ever printed on the main thread's
                               // drain loop, so it doesn't need to touch std::cout itself.
};

// Shared between the main iceoryx2 thread and NatsBridge's background NATS
// I/O thread. NATS completions are pushed here (cheaply -- no cache access,
// no iceoryx2 API calls) from the NATS thread; only the main thread ever
// drains it, which is where the actual cache mutation and iceoryx2 response
// happen. That keeps both multi_index_lru::Container's single-owner/
// no-locking contract and iceoryx2's Server/ActiveRequest usage confined to
// one thread.
class CompletionQueue {
public:
    void push(ActiveRequestType active_request, ApplyFn apply, std::string description) {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(PendingCompletion{std::move(active_request), std::move(apply), std::move(description)});
    }

    std::vector<PendingCompletion> drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PendingCompletion> out;
        out.reserve(items_.size());
        for (auto& item : items_) {
            out.push_back(std::move(item));
        }
        items_.clear();
        return out;
    }

    // Test-only: how many completions are currently queued, without draining
    // them. Not used by server_readthrough.cpp's main loop.
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<PendingCompletion> items_;
};

inline void respond(ActiveRequestType& active_request, const std::vector<std::uint8_t>& response_bytes) {
    auto response = active_request.loan_slice_uninit(response_bytes.size()).value();
    auto initialized = response.write_from_fn([&](auto byte_idx) { return response_bytes[byte_idx]; });
    send(std::move(initialized)).value();
}

// Parses one request and either responds immediately (local cache hit, or a
// malformed request) or hands `active_request` to `completions` -- to be
// responded to once the async NATS operation it kicks off completes.
//
// Any exception here (malformed/truncated request -- see wire::Reader) is
// caught and turned into an immediate Error response; the same hardening
// server.cpp's handle_request_local() has, but active_request is always
// still valid at that point since parsing (and any exception from it)
// happens before active_request is ever moved into a deferred callback.
inline void dispatch_request(NameCache& name_cache, IdCache& id_cache, NatsBridge& nats, const std::string& name_bucket,
                              const std::string& id_bucket, CompletionQueue& completions,
                              ActiveRequestType active_request, const std::uint8_t* data, std::size_t size) {
    try {
        wire::Reader r(data, size);
        const auto op = static_cast<wire::Op>(r.u8());
        const auto kind = static_cast<wire::KeyKind>(r.u8());

        if (op == wire::Op::Get) {
            if (kind == wire::KeyKind::Name) {
                auto key = r.str();
                if (auto it = name_cache.find<NameTag>(key); it != name_cache.end<NameTag>()) {
                    std::cout << "[server] GET name=\"" << key << "\" -> local hit\n";
                    respond(active_request, encode_found(it->record));
                    return;
                }
                std::cout << "[server] GET name=\"" << key << "\" -> local miss, dispatched to NATS\n";
                nats.get_async(name_bucket, key,
                                [&completions, active_request = std::move(active_request),
                                 key](NatsGetResult result) mutable {
                                    ApplyFn apply = [key, result](NameCache& name_cache,
                                                                   IdCache&) -> std::vector<std::uint8_t> {
                                        if (result.result == NatsResult::Ok) {
                                            name_cache.emplace(NameEntry{key, result.value});
                                            return encode_found(result.value);
                                        }
                                        if (result.result == NatsResult::NotFound) {
                                            return encode_status(wire::Status::NotFound);
                                        }
                                        return encode_status(wire::Status::Error);
                                    };
                                    completions.push(std::move(active_request), std::move(apply),
                                                      "GET name=\"" + key + "\"");
                                });
                return;
            }

            auto key = r.i64();
            if (auto it = id_cache.find<IdTag>(key); it != id_cache.end<IdTag>()) {
                std::cout << "[server] GET id=" << key << " -> local hit\n";
                respond(active_request, encode_found(it->record));
                return;
            }
            std::cout << "[server] GET id=" << key << " -> local miss, dispatched to NATS\n";
            nats.get_async(
                id_bucket, std::to_string(key),
                [&completions, active_request = std::move(active_request), key](NatsGetResult result) mutable {
                    ApplyFn apply = [key, result](NameCache&, IdCache& id_cache) -> std::vector<std::uint8_t> {
                        if (result.result == NatsResult::Ok) {
                            id_cache.emplace(IdEntry{key, result.value});
                            return encode_found(result.value);
                        }
                        if (result.result == NatsResult::NotFound) {
                            return encode_status(wire::Status::NotFound);
                        }
                        return encode_status(wire::Status::Error);
                    };
                    completions.push(std::move(active_request), std::move(apply), "GET id=" + std::to_string(key));
                });
            return;
        }

        if (op == wire::Op::Put) {
            if (kind == wire::KeyKind::Name) {
                auto key = r.str();
                auto record = r.bytes(r.u32());
                std::cout << "[server] PUT name=\"" << key << "\" -> dispatched to NATS\n";
                nats.put_async(name_bucket, key, record,
                                [&completions, active_request = std::move(active_request), key,
                                 record](bool ok, std::string /*err*/) mutable {
                                    ApplyFn apply = [key, record, ok](NameCache& name_cache,
                                                                       IdCache&) -> std::vector<std::uint8_t> {
                                        if (!ok) {
                                            return encode_status(wire::Status::Error);
                                        }
                                        name_cache.erase<NameTag>(key);
                                        name_cache.emplace(NameEntry{key, record});
                                        return encode_status(wire::Status::Ok);
                                    };
                                    completions.push(std::move(active_request), std::move(apply),
                                                      "PUT name=\"" + key + "\"");
                                });
                return;
            }

            auto key = r.i64();
            auto record = r.bytes(r.u32());
            std::cout << "[server] PUT id=" << key << " -> dispatched to NATS\n";
            nats.put_async(id_bucket, std::to_string(key), record,
                            [&completions, active_request = std::move(active_request), key,
                             record](bool ok, std::string /*err*/) mutable {
                                ApplyFn apply = [key, record, ok](NameCache&, IdCache& id_cache) -> std::vector<std::uint8_t> {
                                    if (!ok) {
                                        return encode_status(wire::Status::Error);
                                    }
                                    id_cache.erase<IdTag>(key);
                                    id_cache.emplace(IdEntry{key, record});
                                    return encode_status(wire::Status::Ok);
                                };
                                completions.push(std::move(active_request), std::move(apply),
                                                  "PUT id=" + std::to_string(key));
                            });
            return;
        }

        // Erase
        if (kind == wire::KeyKind::Name) {
            auto key = r.str();
            std::cout << "[server] ERASE name=\"" << key << "\" -> dispatched to NATS\n";
            nats.erase_async(name_bucket, key,
                              [&completions, active_request = std::move(active_request),
                               key](bool ok, std::string /*err*/) mutable {
                                  ApplyFn apply = [key, ok](NameCache& name_cache,
                                                             IdCache&) -> std::vector<std::uint8_t> {
                                      if (!ok) {
                                          return encode_status(wire::Status::Error);
                                      }
                                      name_cache.erase<NameTag>(key);
                                      return encode_status(wire::Status::Ok);
                                  };
                                  completions.push(std::move(active_request), std::move(apply),
                                                    "ERASE name=\"" + key + "\"");
                              });
            return;
        }

        auto key = r.i64();
        std::cout << "[server] ERASE id=" << key << " -> dispatched to NATS\n";
        nats.erase_async(
            id_bucket, std::to_string(key),
            [&completions, active_request = std::move(active_request), key](bool ok, std::string /*err*/) mutable {
                ApplyFn apply = [key, ok](NameCache&, IdCache& id_cache) -> std::vector<std::uint8_t> {
                    if (!ok) {
                        return encode_status(wire::Status::Error);
                    }
                    id_cache.erase<IdTag>(key);
                    return encode_status(wire::Status::Ok);
                };
                completions.push(std::move(active_request), std::move(apply), "ERASE id=" + std::to_string(key));
            });
    } catch (const std::exception&) {
        respond(active_request, encode_status(wire::Status::Error));
    } catch (...) {
        respond(active_request, encode_status(wire::Status::Error));
    }
}

}  // namespace poc
