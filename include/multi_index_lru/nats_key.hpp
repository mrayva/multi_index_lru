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

/// @file multi_index_lru/nats_key.hpp
/// @brief Derives NATS JetStream KV keys/wildcard patterns from an ordered,
///        typed list of multi_index key fields, so a composite key and its
///        NATS-backed naming convention can't drift apart via hand-typed
///        string concatenation at each call site.
///
/// The core idea: a composite (or simple) multi_index key is an ORDERED
/// tuple of fields. If that order is encoded as dot-separated NATS subject
/// tokens, then:
///   - a *unique* index over the full field list maps to one NATS key
///     (nats_key() -- an exact kv_get/kv_put/kv_delete).
///   - a *non-unique* index over a strict PREFIX of that field list maps to
///     a NATS wildcard pattern over the remaining, unlisted fields
///     (nats_pattern() -- kv_keys/a fetch-by-pattern).
///
/// Field values must never contain '.', '*', or '>' -- NATS treats '.' as
/// an absolute subject-token separator and the other two as wildcards, so
/// any of them inside a value would silently corrupt the intended
/// hierarchy. validate_nats_key_field() rejects all three up front rather
/// than producing a key that looks fine but matches the wrong thing.
///
/// This header only derives key/pattern *strings* -- it doesn't touch NATS
/// itself. Callers still issue the actual kv_get/kv_put/kv_keys calls (see
/// example/iceoryx2_cache_poc's NatsBridge for one such caller, and
/// test/nats_key_test.cpp / the composite-key NATS integration test in that
/// example for it wired up end to end against a real composite multi_index
/// key and a real NATS server).

#include <cstddef>
#include <stdexcept>
#include <string>

namespace multi_index_lru {

/// Rejects a field value that would corrupt the NATS subject hierarchy if
/// used as (or inside) a key: '.' is the token separator, '*'/'>' are
/// wildcards. Throws std::invalid_argument; also rejects an empty value,
/// which would produce a malformed (empty) subject token.
inline void validate_nats_key_field(const std::string& value) {
    if (value.empty()) {
        throw std::invalid_argument("NATS key field is empty");
    }
    if (value.find('.') != std::string::npos) {
        throw std::invalid_argument("NATS key field contains '.': \"" + value + "\"");
    }
    if (value.find('*') != std::string::npos || value.find('>') != std::string::npos) {
        throw std::invalid_argument("NATS key field contains a NATS wildcard character: \"" + value + "\"");
    }
}

namespace detail {
template <typename... Fields>
inline void append_dot_joined(std::string& out, const Fields&... fields) {
    bool first = true;
    ((out += (first ? "" : "."), out += fields, first = false), ...);
}
}  // namespace detail

/// Exact NATS key for a *unique* index over these fields, in order --
/// e.g. nats_key("alice", "email") -> "alice.email". Every field is
/// validated first (see validate_nats_key_field()), so a bad field throws
/// before any part of the key is built.
template <typename... Fields>
std::string nats_key(const Fields&... fields) {
    (validate_nats_key_field(fields), ...);
    std::string out;
    detail::append_dot_joined(out, fields...);
    return out;
}

/// Which NATS subject wildcard a pattern's trailing token should use.
enum class NatsWildcard {
    SingleToken,  ///< '*' -- matches exactly one more token
    MultiToken,   ///< '>' -- matches one or more remaining tokens
};

/// NATS wildcard *pattern* for a *non-unique* index over a PREFIX of a
/// larger field list -- e.g. nats_pattern(SingleToken, "alice") ->
/// "alice.*" (exactly one more field, e.g. "alice.email"/"alice.phone"),
/// or nats_pattern(MultiToken, "alice") -> "alice.>" (arbitrary remaining
/// depth). Prefer NatsCompositeKey::prefix_pattern() below when the full
/// field count is known -- it picks SingleToken vs MultiToken for you
/// instead of you having to choose it by hand.
template <typename... Fields>
std::string nats_pattern(NatsWildcard tail, const Fields&... fields) {
    (validate_nats_key_field(fields), ...);
    std::string out;
    detail::append_dot_joined(out, fields...);
    out += '.';
    out += (tail == NatsWildcard::SingleToken) ? '*' : '>';
    return out;
}

/// Describes a composite key's ordered field list and lets a strict prefix
/// of it (a non-unique index over just the leading fields) derive its own
/// NATS pattern automatically -- the wildcard depth is exactly "however
/// many fields the full key has beyond this prefix," not a hand-picked
/// guess. `AllFields...` is the full, ordered field-type list of the
/// *unique* key this type describes (e.g. `NatsCompositeKey<std::string,
/// std::string>` for a two-field `(owner, attribute)` key).
template <typename... AllFields>
struct NatsCompositeKey {
    static constexpr std::size_t field_count = sizeof...(AllFields);

    /// Full key, e.g. (owner, attribute), as an exact NATS key.
    static std::string key(const AllFields&... fields) { return nats_key(fields...); }

    /// A leading prefix of the full key, e.g. just (owner,) out of
    /// (owner, attribute), as a NATS wildcard pattern. `PrefixFields...`
    /// must be a strict prefix of `AllFields...` in the same order -- the
    /// caller supplies only the prefix's own values; the wildcard tail is
    /// derived from how many fields remain.
    template <typename... PrefixFields>
    static std::string prefix_pattern(const PrefixFields&... prefix_fields) {
        static_assert(sizeof...(PrefixFields) < field_count,
                      "prefix_pattern's field count must be a strict prefix of the full key "
                      "(fewer fields than NatsCompositeKey's own field_count)");
        constexpr std::size_t remaining = field_count - sizeof...(PrefixFields);
        return nats_pattern(remaining == 1 ? NatsWildcard::SingleToken : NatsWildcard::MultiToken,
                             prefix_fields...);
    }
};

}  // namespace multi_index_lru
