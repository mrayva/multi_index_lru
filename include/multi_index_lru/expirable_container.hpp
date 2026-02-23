// Copyright 2024 Yandex LLC and userver contributors
// Copyright 2026 multi_index_lru contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

/// @file multi_index_lru/expirable_container.hpp
/// @brief TTL-based expirable LRU container based on boost::multi_index

#include "container.hpp"

#include <cassert>
#include <chrono>

namespace multi_index_lru {

/// @brief MultiIndex LRU container with TTL-based expiration
///
/// Extends Container with time-to-live (TTL) semantics. Items automatically
/// expire after a configurable duration. Access via find() refreshes the
/// expiration timer.
///
/// @tparam Value The value type stored in the container
/// @tparam IndexSpecifierList boost::multi_index::indexed_by<...> specifying indices
/// @tparam Allocator Allocator type (defaults to std::allocator<Value>)
///
/// Example usage:
/// @code
/// struct User {
///     int id;
///     std::string name;
/// };
///
/// struct IdTag {};
///
/// using UserCache = multi_index_lru::ExpirableContainer<
///     User,
///     boost::multi_index::indexed_by<
///         boost::multi_index::hashed_unique<
///             boost::multi_index::tag<IdTag>,
///             boost::multi_index::member<User, int, &User::id>>>>;
///
/// UserCache cache(1000, std::chrono::minutes(5));  // Capacity 1000, 5min TTL
/// cache.insert(User{1, "Alice"});
///
/// // Access refreshes TTL
/// auto it = cache.find<IdTag>(1);  
///
/// // Periodic cleanup of expired items
/// cache.cleanup_expired();
/// @endcode
template <typename Value, typename IndexSpecifierList, typename Allocator = std::allocator<Value>>
class ExpirableContainer {
public:
    using value_type = Value;
    using allocator_type = Allocator;
    using size_type = std::size_t;
    using clock_type = std::chrono::steady_clock;
    using duration_type = std::chrono::milliseconds;
    using time_point_type = clock_type::time_point;

    /// @brief Construct container with specified capacity and TTL
    /// @param max_size Maximum number of elements before LRU eviction
    /// @param ttl Time-to-live for each element
    explicit ExpirableContainer(size_type max_size, duration_type ttl)
        : container_(max_size), ttl_(ttl)
    {
        assert(ttl.count() > 0 && "TTL must be positive");
    }

    /// @brief Emplace a new element
    /// @param args Arguments forwarded to value constructor
    /// @return Pair of (wrapped iterator, bool indicating new insertion)
    ///
    /// If an element with matching key(s) exists, its timestamp is refreshed.
    template <typename... Args>
    auto emplace(Args&&... args) {
        auto result = container_.get_sequenced().emplace_front(
            CacheItem{Value{std::forward<Args>(args)...}});

        if (!result.second) {
            result.first->last_accessed = clock_type::now();
            container_.get_sequenced().relocate(
                container_.get_sequenced().begin(), result.first);
        } else if (container_.size() > container_.capacity()) {
            container_.get_sequenced().pop_back();
        }

        return std::pair{detail::TimestampedIteratorWrapper{result.first}, result.second};
    }

    /// @brief Insert a value (copy)
    /// @param value Value to insert
    /// @return true if newly inserted, false if existing element was refreshed
    bool insert(const Value& value) { return emplace(value).second; }

    /// @brief Insert a value (move)
    /// @param value Value to insert  
    /// @return true if newly inserted, false if existing element was refreshed
    bool insert(Value&& value) { return emplace(std::move(value)).second; }

    /// @brief Find element by key, checking TTL and refreshing timestamp
    /// @tparam Tag Index tag type
    /// @param key Key to search for
    /// @return Wrapped iterator to found element, or end() if not found or expired
    ///
    /// If the element is found but expired, it is removed and end() is returned.
    /// If found and not expired, the access timestamp is refreshed.
    template <typename Tag, typename Key = void>
    auto find(const auto& key) {
        auto now = clock_type::now();
        auto& index = container_.template get_index<Tag>();
        auto it = index.find(key);
        
        if (it != index.end()) {
            if (now > it->last_accessed + ttl_) {
                // Item expired - remove it
                index.erase(it);
                return detail::TimestampedIteratorWrapper{index.end()};
            } else {
                // Refresh timestamp and move to front
                it->last_accessed = now;
                container_.get_sequenced().relocate(
                    container_.get_sequenced().begin(),
                    container_.get_container().template project<0>(it));
            }
        }
        
        return detail::TimestampedIteratorWrapper{it};
    }

    /// @brief Find element without updating timestamp or checking TTL
    /// @tparam Tag Index tag type
    /// @param key Key to search for
    /// @return Wrapped iterator to found element, or end() if not found
    ///
    /// Note: May return expired items. Use for read-only inspection.
    template <typename Tag, typename Key = void>
    auto find_no_update(const auto& key) {
        auto it = container_.template find_no_update<Tag>(key);
        return detail::TimestampedIteratorWrapper{it};
    }

    /// @brief Find range of elements, checking TTL and refreshing timestamps
    /// @tparam Tag Index tag type
    /// @param key Key to search for
    /// @return Pair of wrapped iterators defining the range
    ///
    /// Expired elements within the range are removed. Non-expired elements
    /// have their timestamps refreshed.
    template <typename Tag, typename Key = void>
    auto equal_range(const auto& key) {
        auto now = clock_type::now();
        auto& index = container_.template get_index<Tag>();
        auto range = index.equal_range(key);
        
        auto it = range.first;
        bool changed = false;
        
        while (it != range.second) {
            if (now > it->last_accessed + ttl_) {
                it = index.erase(it);
                changed = true;
            } else {
                it->last_accessed = now;
                container_.get_sequenced().relocate(
                    container_.get_sequenced().begin(),
                    container_.get_container().template project<0>(it));
                ++it;
            }
        }
        
        if (changed) {
            range = index.equal_range(key);
        }
        
        return std::pair{
            detail::TimestampedIteratorWrapper{range.first},
            detail::TimestampedIteratorWrapper{range.second}};
    }

    /// @brief Find range of elements without updating timestamps
    /// @tparam Tag Index tag type
    /// @param key Key to search for
    /// @return Pair of wrapped iterators defining the range
    template <typename Tag, typename Key = void>
    auto equal_range_no_update(const auto& key) {
        auto [begin, end] = container_.template equal_range_no_update<Tag>(key);
        return std::pair{
            detail::TimestampedIteratorWrapper{begin},
            detail::TimestampedIteratorWrapper{end}};
    }

    /// @brief Check if element exists (checking TTL)
    /// @tparam Tag Index tag type
    /// @param key Key to search for
    /// @return true if element exists and is not expired
    template <typename Tag, typename Key = void>
    bool contains(const auto& key) {
        return this->template find<Tag, Key>(key) != this->template end<Tag>();
    }

    /// @brief Erase element by key
    /// @tparam Tag Index tag type
    /// @param key Key of element to erase
    /// @return true if element was erased, false if not found
    template <typename Tag, typename Key = void>
    bool erase(const auto& key) {
        return container_.template erase<Tag>(key);
    }

    /// @brief Get current number of elements (including expired)
    [[nodiscard]] size_type size() const noexcept { return container_.size(); }

    /// @brief Check if container is empty
    [[nodiscard]] bool empty() const noexcept { return container_.empty(); }

    /// @brief Get current capacity
    [[nodiscard]] size_type capacity() const noexcept { return container_.capacity(); }

    /// @brief Set new capacity
    /// @param new_capacity New maximum size
    void set_capacity(size_type new_capacity) {
        container_.set_capacity(new_capacity);
    }

    /// @brief Remove all elements
    void clear() noexcept { container_.clear(); }

    /// @brief Get end iterator for specified index
    /// @tparam Tag Index tag type
    template <typename Tag>
    [[nodiscard]] auto end() {
        return detail::TimestampedIteratorWrapper{
            container_.template get_index<Tag>().end()};
    }

    /// @brief Get end iterator for specified index (const)
    template <typename Tag>
    [[nodiscard]] auto end() const {
        return detail::TimestampedIteratorWrapper{
            container_.template get_index<Tag>().end()};
    }

    /// @brief Remove all expired elements
    ///
    /// Scans from the back (oldest) and removes consecutive expired items.
    /// Call periodically to prevent memory bloat from expired entries.
    void cleanup_expired() {
        auto now = clock_type::now();
        auto& seq_index = container_.get_sequenced();

        while (!seq_index.empty()) {
            auto it = seq_index.rbegin();
            if (now > it->last_accessed + ttl_) {
                seq_index.pop_back();
            } else {
                break;
            }
        }
    }

    /// @brief Get current TTL setting
    [[nodiscard]] duration_type ttl() const noexcept { return ttl_; }

    /// @brief Set new TTL (affects future accesses, not existing timestamps)
    void set_ttl(duration_type new_ttl) {
        assert(new_ttl.count() > 0 && "TTL must be positive");
        ttl_ = new_ttl;
    }

private:
    using CacheItem = detail::TimestampedValue<Value>;
    using CacheContainer = Container<CacheItem, IndexSpecifierList, 
        typename std::allocator_traits<Allocator>::template rebind_alloc<CacheItem>>;

    CacheContainer container_;
    duration_type ttl_;
};

}  // namespace multi_index_lru
