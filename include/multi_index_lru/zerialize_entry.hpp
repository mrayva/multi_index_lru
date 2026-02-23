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

/// @file multi_index_lru/zerialize_entry.hpp
/// @brief Entry wrapper for zerialize data with extracted keys

#include <cstdint>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace multi_index_lru {

/// @brief Entry that stores raw serialized bytes alongside extracted index keys
/// @tparam Keys Tuple of key types extracted from zerialize data
///
/// Example:
/// @code
/// // Entry with composite key: (tenant_id, user_id) and secondary key: email
/// using MyKeys = std::tuple<int64_t, int64_t, std::string>;
/// using MyEntry = ZerializeEntry<MyKeys>;
/// @endcode
template <typename Keys>
struct ZerializeEntry {
    using keys_type = Keys;
    static constexpr std::size_t key_count = std::tuple_size_v<Keys>;

    Keys keys;
    std::vector<uint8_t> data;

    ZerializeEntry() = default;

    ZerializeEntry(Keys k, std::vector<uint8_t> d)
        : keys(std::move(k)), data(std::move(d)) {}

    ZerializeEntry(Keys k, std::span<const uint8_t> d)
        : keys(std::move(k)), data(d.begin(), d.end()) {}

    /// @brief Deserialize the stored data using specified format
    /// @tparam Deserializer zerialize deserializer type (e.g., MsgPack::Deserializer)
    template <typename Deserializer>
    [[nodiscard]] Deserializer deserialize() const {
        return Deserializer(std::span<const uint8_t>(data));
    }

    /// @brief Get raw data span
    [[nodiscard]] std::span<const uint8_t> raw_data() const noexcept {
        return std::span<const uint8_t>(data);
    }
};

/// @brief Helper to define ZerializeEntry with explicit key types
/// @tparam KeyTypes Types of the extracted keys
template <typename... KeyTypes>
using EntryWithKeys_t = ZerializeEntry<std::tuple<KeyTypes...>>;

/// @brief Traits for ZerializeEntry
template <typename T>
struct is_zerialize_entry : std::false_type {};

template <typename Keys>
struct is_zerialize_entry<ZerializeEntry<Keys>> : std::true_type {};

template <typename T>
inline constexpr bool is_zerialize_entry_v = is_zerialize_entry<T>::value;

}  // namespace multi_index_lru
