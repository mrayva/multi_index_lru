#pragma once

/// @file multi_index_lru/container.hpp
/// @brief LRU container based on boost::multi_index

#include <boost/multi_index/identity.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace multi_index_lru {

namespace detail {

/// Check if type is boost::mpl::na (placeholder type)
template <typename T, typename = void>
inline constexpr bool is_mpl_na = false;

template <typename T>
inline constexpr bool is_mpl_na<T, std::void_t<decltype(std::declval<T>().~na())>> = true;

/// Helper to prepend sequenced index to existing indices
template <typename... Indices>
struct lazy_add_seq {
    using type = boost::multi_index::indexed_by<boost::multi_index::sequenced<>, Indices...>;
};

/// Helper to prepend sequenced index but skip last na index
template <typename... Indices>
struct lazy_add_seq_no_last {
private:
    template <std::size_t... I>
    static auto makeWithoutLast(std::index_sequence<I...>) {
        using Tuple = std::tuple<Indices...>;
        return boost::multi_index::indexed_by<
            boost::multi_index::sequenced<>,
            std::tuple_element_t<I, Tuple>...>{};
    }

public:
    using type = decltype(makeWithoutLast(std::make_index_sequence<sizeof...(Indices) - 1>{}));
};

/// Prepend a sequenced index to an indexed_by list for LRU tracking
template <typename IndexList>
struct add_seq_index {};

template <typename... Indices>
struct add_seq_index<boost::multi_index::indexed_by<Indices...>> {
    using LastType = decltype((Indices{}, ...));

    using type = typename std::conditional_t<
        is_mpl_na<LastType>,
        lazy_add_seq_no_last<Indices...>,
        lazy_add_seq<Indices...>>::type;
};

template <typename IndexList>
using add_seq_index_t = typename add_seq_index<IndexList>::type;

}  // namespace detail

/// @brief MultiIndex LRU container
///
/// A container that provides LRU (Least Recently Used) eviction semantics
/// on top of boost::multi_index_container. The container automatically
/// manages a sequenced index to track access order.
///
/// @tparam Value The value type stored in the container
/// @tparam IndexSpecifierList boost::multi_index::indexed_by<...> specifying indices
/// @tparam Allocator Allocator type (defaults to std::allocator<Value>)
///
/// Example usage:
/// @code
/// struct MyValue {
///     std::string key;
///     int value;
/// };
///
/// struct KeyTag {};
///
/// using MyCache = multi_index_lru::Container<
///     MyValue,
///     boost::multi_index::indexed_by<
///         boost::multi_index::hashed_unique<
///             boost::multi_index::tag<KeyTag>,
///             boost::multi_index::member<MyValue, std::string, &MyValue::key>>>>;
///
/// MyCache cache(1000);  // Capacity of 1000 items
/// cache.emplace(MyValue{"key1", 42});
/// auto it = cache.find<KeyTag>("key1");
/// @endcode
template <typename Value, typename IndexSpecifierList, typename Allocator = std::allocator<Value>>
class Container {
public:
    using value_type = Value;
    using allocator_type = Allocator;
    using size_type = std::size_t;

    /// @brief Construct container with specified capacity
    /// @param max_size Maximum number of elements before LRU eviction
    explicit Container(size_type max_size)
        : max_size_(max_size)
    {}

    /// @brief Emplace a new element
    /// @param args Arguments forwarded to value constructor
    /// @return true if element was newly inserted, false if existing element was updated
    ///
    /// If an element with matching key(s) exists, it's moved to front (most recently used).
    /// If insertion would exceed capacity, the least recently used element is evicted.
    template <typename... Args>
    bool emplace(Args&&... args) {
        auto& seq_index = container_.template get<0>();
        auto result = seq_index.emplace_front(std::forward<Args>(args)...);

        if (!result.second) {
            seq_index.relocate(seq_index.begin(), result.first);
        } else if (seq_index.size() > max_size_) {
            seq_index.pop_back();
        }
        return result.second;
    }

    /// @brief Insert a value (copy)
    /// @param value Value to insert
    /// @return true if newly inserted, false if existing element was refreshed
    bool insert(const Value& value) { return emplace(value); }

    /// @brief Insert a value (move)
    /// @param value Value to insert
    /// @return true if newly inserted, false if existing element was refreshed
    bool insert(Value&& value) { return emplace(std::move(value)); }

    /// @brief Find element by key using specified index
    /// @tparam Tag Index tag type
    /// @tparam Key Key type (can often be deduced)
    /// @param key Key to search for
    /// @return Iterator to found element, or end() if not found
    ///
    /// Finding an element moves it to the front (most recently used).
    template <typename Tag, typename Key = void>
    auto find(const auto& key) {
        auto& primary_index = container_.template get<Tag>();
        auto it = primary_index.find(key);

        if (it != primary_index.end()) {
            auto& seq_index = container_.template get<0>();
            auto seq_it = container_.template project<0>(it);
            seq_index.relocate(seq_index.begin(), seq_it);
        }

        return it;
    }

    /// @brief Check if element exists by key
    /// @tparam Tag Index tag type
    /// @tparam Key Key type (can often be deduced)
    /// @param key Key to search for
    /// @return true if element exists
    ///
    /// This also refreshes the element's LRU position.
    template <typename Tag, typename Key = void>
    bool contains(const auto& key) {
        return this->template find<Tag, Key>(key) != container_.template get<Tag>().end();
    }

    /// @brief Erase element by key
    /// @tparam Tag Index tag type
    /// @tparam Key Key type (can often be deduced)
    /// @param key Key of element to erase
    /// @return true if element was erased, false if not found
    template <typename Tag, typename Key = void>
    bool erase(const auto& key) {
        return container_.template get<Tag>().erase(key) > 0;
    }

    /// @brief Get current number of elements
    [[nodiscard]] size_type size() const noexcept { return container_.size(); }

    /// @brief Check if container is empty
    [[nodiscard]] bool empty() const noexcept { return container_.empty(); }

    /// @brief Get current capacity
    [[nodiscard]] size_type capacity() const noexcept { return max_size_; }

    /// @brief Set new capacity
    /// @param new_capacity New maximum size
    ///
    /// If new capacity is smaller than current size, LRU elements are evicted.
    void set_capacity(size_type new_capacity) {
        max_size_ = new_capacity;
        auto& seq_index = container_.template get<0>();
        while (container_.size() > max_size_) {
            seq_index.pop_back();
        }
    }

    /// @brief Remove all elements
    void clear() noexcept { container_.clear(); }

    /// @brief Get end iterator for specified index
    /// @tparam Tag Index tag type
    template <typename Tag>
    [[nodiscard]] auto end() const {
        return container_.template get<Tag>().end();
    }

    /// @brief Get end iterator for specified index (non-const)
    template <typename Tag>
    [[nodiscard]] auto end() {
        return container_.template get<Tag>().end();
    }

    /// @brief Get begin iterator for sequenced (LRU) index
    /// @return Iterator to most recently used element
    [[nodiscard]] auto begin() const {
        return container_.template get<0>().begin();
    }

    /// @brief Get begin iterator for sequenced (LRU) index (non-const)
    [[nodiscard]] auto begin() {
        return container_.template get<0>().begin();
    }

    /// @brief Get end iterator for sequenced (LRU) index
    [[nodiscard]] auto end() const {
        return container_.template get<0>().end();
    }

    /// @brief Get end iterator for sequenced (LRU) index (non-const)
    [[nodiscard]] auto end() {
        return container_.template get<0>().end();
    }

    /// @brief Access underlying boost::multi_index_container
    /// @return Reference to internal container
    [[nodiscard]] auto& get_container() noexcept { return container_; }

    /// @brief Access underlying boost::multi_index_container (const)
    [[nodiscard]] const auto& get_container() const noexcept { return container_; }

private:
    using ExtendedIndexSpecifierList = detail::add_seq_index_t<IndexSpecifierList>;

    using BoostContainer = boost::multi_index::multi_index_container<
        Value,
        ExtendedIndexSpecifierList,
        Allocator>;

    BoostContainer container_;
    size_type max_size_;
};

}  // namespace multi_index_lru
