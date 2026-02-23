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

/// @file multi_index_lru/zerialize_cache.hpp
/// @brief LRU cache for zerialize data with compile-time index configuration

#include <multi_index_lru/container.hpp>
#include <multi_index_lru/zerialize_entry.hpp>

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <concepts>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace multi_index_lru {

/// @brief Concept for zerialize deserializer
template <typename D>
concept ZerializeDeserializer = requires(D d, std::span<const uint8_t> data) {
    D(data);
    { d["field"] } -> std::convertible_to<D>;
    { d.isMap() } -> std::same_as<bool>;
};

/// @brief Key extractor functor for use with boost::multi_index
/// @tparam N Index of the key in the tuple
template <std::size_t N, typename Entry>
struct key {
    using result_type = std::tuple_element_t<N, typename Entry::keys_type>;

    result_type operator()(const Entry& entry) const {
        return std::get<N>(entry.keys);
    }
};

/// @brief Builder for creating ZerializeEntry from deserializer
/// @tparam Entry The ZerializeEntry type
/// @tparam Extractors Pack of field extractor functions
template <typename Entry, typename... Extractors>
class EntryBuilder {
public:
    using keys_type = typename Entry::keys_type;

    explicit EntryBuilder(Extractors... extractors)
        : extractors_(std::move(extractors)...) {}

    /// @brief Build entry from raw data using deserializer
    /// @tparam Deserializer zerialize deserializer type
    template <ZerializeDeserializer Deserializer>
    Entry build(std::span<const uint8_t> data) const {
        Deserializer reader(data);
        return build_impl(reader, data, std::index_sequence_for<Extractors...>{});
    }

    /// @brief Build entry from existing deserializer + data
    template <ZerializeDeserializer Deserializer>
    Entry build(const Deserializer& reader, std::span<const uint8_t> data) const {
        return build_impl(reader, data, std::index_sequence_for<Extractors...>{});
    }

private:
    template <typename Deserializer, std::size_t... Is>
    Entry build_impl(const Deserializer& reader, std::span<const uint8_t> data,
                     std::index_sequence<Is...>) const {
        return Entry(
            keys_type(std::get<Is>(extractors_)(reader)...),
            data
        );
    }

    std::tuple<Extractors...> extractors_;
};

/// @brief Create an entry builder with field extractors
/// @tparam Entry ZerializeEntry type
/// @param extractors Lambda/function objects that extract each key from deserializer
template <typename Entry, typename... Extractors>
auto make_entry_builder(Extractors... extractors) {
    return EntryBuilder<Entry, Extractors...>(std::move(extractors)...);
}

/// @brief Field extractor helper - extracts a field by name and converts to type T
/// @tparam T Target type
template <typename T>
struct field {
    std::string name;

    explicit field(std::string field_name) : name(std::move(field_name)) {}

    template <typename Deserializer>
    T operator()(const Deserializer& reader) const {
        if constexpr (std::is_same_v<T, int64_t>) {
            return reader[name].asInt64();
        } else if constexpr (std::is_same_v<T, int32_t>) {
            return reader[name].asInt32();
        } else if constexpr (std::is_same_v<T, uint64_t>) {
            return reader[name].asUInt64();
        } else if constexpr (std::is_same_v<T, uint32_t>) {
            return reader[name].asUInt32();
        } else if constexpr (std::is_same_v<T, std::string>) {
            return reader[name].asString();
        } else if constexpr (std::is_same_v<T, double>) {
            return reader[name].asDouble();
        } else if constexpr (std::is_same_v<T, bool>) {
            return reader[name].asBool();
        } else {
            static_assert(sizeof(T) == 0, "Unsupported field type");
        }
    }
};

/// @brief Helper to create field extractors
template <typename T>
field<T> make_field(std::string name) {
    return field<T>(std::move(name));
}

// Convenience aliases for common field types
inline auto int64_field(std::string name) { return field<int64_t>(std::move(name)); }
inline auto int32_field(std::string name) { return field<int32_t>(std::move(name)); }
inline auto uint64_field(std::string name) { return field<uint64_t>(std::move(name)); }
inline auto string_field(std::string name) { return field<std::string>(std::move(name)); }
inline auto double_field(std::string name) { return field<double>(std::move(name)); }
inline auto bool_field(std::string name) { return field<bool>(std::move(name)); }

/// @brief Nested field extractor - accesses nested fields like reader["a"]["b"]["c"]
template <typename T>
struct nested_field {
    std::vector<std::string> path;

    explicit nested_field(std::initializer_list<std::string> field_path)
        : path(field_path) {}

    template <typename... Names>
    explicit nested_field(Names... names) : path{std::string(names)...} {}

    template <typename Deserializer>
    T operator()(const Deserializer& reader) const {
        auto current = reader;
        for (const auto& name : path) {
            current = current[name];
        }

        if constexpr (std::is_same_v<T, int64_t>) {
            return current.asInt64();
        } else if constexpr (std::is_same_v<T, std::string>) {
            return current.asString();
        } else if constexpr (std::is_same_v<T, double>) {
            return current.asDouble();
        } else {
            static_assert(sizeof(T) == 0, "Unsupported nested field type");
        }
    }
};

}  // namespace multi_index_lru
