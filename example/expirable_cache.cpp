// Copyright 2026 multi_index_lru contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Example: ExpirableContainer with TTL-based expiration

#include <multi_index_lru/expirable_container.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace std::chrono_literals;

// Define our cached data structure
struct Session {
    std::string session_id;
    int user_id;
    std::string username;
    std::chrono::system_clock::time_point created;
};

// Index tags
struct SessionIdTag {};
struct UserIdTag {};

// Key extractor for session_id (works through TimestampedValue wrapper)
struct SessionIdExtractor {
    using result_type = std::string;
    template <typename T>
    result_type operator()(const T& wrapped) const { 
        return wrapped.value.session_id; 
    }
};

// Key extractor for user_id
struct UserIdExtractor {
    using result_type = int;
    template <typename T>
    result_type operator()(const T& wrapped) const { 
        return wrapped.value.user_id; 
    }
};

// Define the expirable session cache
using SessionCache = multi_index_lru::ExpirableContainer<
    Session,
    boost::multi_index::indexed_by<
        // Primary index: unique by session_id
        boost::multi_index::hashed_unique<
            boost::multi_index::tag<SessionIdTag>,
            SessionIdExtractor>,
        // Secondary index: non-unique by user_id (user may have multiple sessions)
        boost::multi_index::ordered_non_unique<
            boost::multi_index::tag<UserIdTag>,
            UserIdExtractor>>>;

int main() {
    // Create session cache with capacity=100 and 30-minute session timeout
    // For demo we use 200ms
    SessionCache cache(100, 200ms);
    
    std::cout << "=== Session Cache with TTL Demo ===" << std::endl;
    
    // Create some sessions
    cache.insert(Session{"sess-001", 1, "alice", std::chrono::system_clock::now()});
    cache.insert(Session{"sess-002", 1, "alice", std::chrono::system_clock::now()});  // Alice has 2 sessions
    cache.insert(Session{"sess-003", 2, "bob", std::chrono::system_clock::now()});
    
    std::cout << "Created 3 sessions (2 for alice, 1 for bob)" << std::endl;
    std::cout << "Cache size: " << cache.size() << std::endl;
    
    // Look up session by ID
    auto it = cache.find<SessionIdTag>(std::string("sess-001"));
    if (it != cache.end<SessionIdTag>()) {
        std::cout << "Found session sess-001 for user: " << it->username << std::endl;
    }
    
    // Find all sessions for a user using equal_range
    std::cout << "\nAll sessions for user_id=1:" << std::endl;
    auto [begin, end] = cache.equal_range<UserIdTag>(1);
    for (auto iter = begin; iter != end; ++iter) {
        std::cout << "  - " << iter->session_id << " (" << iter->username << ")" << std::endl;
    }
    
    // Wait for TTL to expire
    std::cout << "\nWaiting 250ms for sessions to expire..." << std::endl;
    std::this_thread::sleep_for(250ms);
    
    // Try to find expired session - will return end() and remove it
    it = cache.find<SessionIdTag>(std::string("sess-001"));
    if (it == cache.end<SessionIdTag>()) {
        std::cout << "Session sess-001 has expired and been removed" << std::endl;
    }
    
    // Cleanup any remaining expired sessions
    cache.cleanup_expired();
    std::cout << "After cleanup, cache size: " << cache.size() << std::endl;
    
    // Add a new session and keep it alive
    std::cout << "\n=== Keep-alive Demo ===" << std::endl;
    cache.insert(Session{"sess-004", 3, "charlie", std::chrono::system_clock::now()});
    std::cout << "Created session for charlie" << std::endl;
    
    // Access it before TTL expires to refresh
    std::this_thread::sleep_for(100ms);
    std::cout << "After 100ms, accessing session to refresh TTL..." << std::endl;
    cache.find<SessionIdTag>(std::string("sess-004"));  // This refreshes TTL
    
    std::this_thread::sleep_for(100ms);
    std::cout << "After another 100ms (200ms total), session should still exist..." << std::endl;
    
    it = cache.find<SessionIdTag>(std::string("sess-004"));
    if (it != cache.end<SessionIdTag>()) {
        std::cout << "Session sess-004 still alive for: " << it->username << std::endl;
    }
    
    std::cout << "\n=== Dynamic TTL Demo ===" << std::endl;
    std::cout << "Current TTL: " << cache.ttl().count() << "ms" << std::endl;
    cache.set_ttl(500ms);
    std::cout << "Changed TTL to: " << cache.ttl().count() << "ms" << std::endl;
    
    std::cout << "\nDone!" << std::endl;
    return 0;
}
