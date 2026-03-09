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

/// @file multi_index_lru/sbe_cache.hpp
/// @brief Adapters for caching SBE payloads with extracted keys

#include <multi_index_lru/container.hpp>
#include <multi_index_lru/expirable_container.hpp>

#include <concepts>
#include <cstdint>
#include <functional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace multi_index_lru {

/// @brief Entry that owns SBE bytes and stores extracted index keys
template <typename Keys>
struct SbeEntry {
    using keys_type = Keys;
    static constexpr std::size_t key_count = std::tuple_size_v<Keys>;

    Keys keys;
    std::vector<uint8_t> data;

    SbeEntry() = default;

    SbeEntry(Keys k, std::vector<uint8_t> d)
        : keys(std::move(k)), data(std::move(d)) {}

    SbeEntry(Keys k, std::span<const uint8_t> d)
        : keys(std::move(k)), data(d.begin(), d.end()) {}

    /// @brief Build an SBE view from owned bytes using a user-provided factory
    template <typename ViewFactory>
        requires std::invocable<ViewFactory, std::span<const uint8_t>>
    [[nodiscard]] auto view(ViewFactory&& factory) const {
        return std::invoke(std::forward<ViewFactory>(factory), raw_data());
    }

    /// @brief Get raw payload bytes
    [[nodiscard]] std::span<const uint8_t> raw_data() const noexcept {
        return std::span<const uint8_t>(data);
    }
};

/// @brief Helper alias to define SbeEntry with key types
template <typename... KeyTypes>
using SbeEntryWithKeys_t = SbeEntry<std::tuple<KeyTypes...>>;

/// @brief Key extractor for SbeEntry tuple keys
template <std::size_t N, typename Entry>
struct sbe_key {
    using result_type = std::tuple_element_t<N, typename Entry::keys_type>;

    result_type operator()(const Entry& entry) const {
        return std::get<N>(entry.keys);
    }
};

/// @brief Key extractor for TimestampedValue<SbeEntry> in ExpirableContainer
template <std::size_t N, typename Entry>
struct sbe_timestamped_key {
    using result_type = std::tuple_element_t<N, typename Entry::keys_type>;

    template <typename TimestampedEntry>
        requires requires(const TimestampedEntry& t) { t.value.keys; }
    result_type operator()(const TimestampedEntry& wrapped) const {
        return std::get<N>(wrapped.value.keys);
    }

    result_type operator()(const Entry& entry) const {
        return std::get<N>(entry.keys);
    }
};

/// @brief Builder that extracts index keys from an SBE view and stores owned bytes
template <typename Entry, typename ViewFactory, typename... Extractors>
class SbeEntryBuilder {
public:
    using keys_type = typename Entry::keys_type;

    explicit SbeEntryBuilder(ViewFactory view_factory, Extractors... extractors)
        : view_factory_(std::move(view_factory)), extractors_(std::move(extractors)...)
    {}

    /// @brief Build an entry from raw SBE bytes (bytes are copied into entry ownership)
    Entry build(std::span<const uint8_t> data) const {
        auto view = std::invoke(view_factory_, data);
        return build_impl(view, data, std::index_sequence_for<Extractors...>{});
    }

private:
    template <typename View, std::size_t... Is>
    Entry build_impl(const View& view, std::span<const uint8_t> data, std::index_sequence<Is...>) const {
        return Entry(keys_type(std::get<Is>(extractors_)(view)...), data);
    }

    ViewFactory view_factory_;
    std::tuple<Extractors...> extractors_;
};

/// @brief Create SbeEntryBuilder
template <typename Entry, typename ViewFactory, typename... Extractors>
auto make_sbe_entry_builder(ViewFactory view_factory, Extractors... extractors) {
    return SbeEntryBuilder<Entry, ViewFactory, Extractors...>(
        std::move(view_factory), std::move(extractors)...);
}

/// @brief Wrap any callable member/accessor as an extractor for builder keys
template <typename T, typename Accessor>
struct sbe_field {
    Accessor accessor;

    explicit sbe_field(Accessor a)
        : accessor(std::move(a))
    {}

    template <typename View>
    T operator()(const View& view) const {
        return static_cast<T>(std::invoke(accessor, view));
    }
};

template <typename T, typename Accessor>
auto make_sbe_field(Accessor accessor) {
    return sbe_field<T, Accessor>(std::move(accessor));
}

}  // namespace multi_index_lru
