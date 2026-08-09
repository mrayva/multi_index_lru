/// Tiny CLI-flag / env-var config resolution shared by all four POC
/// binaries. Not a general-purpose argument parser: each option is resolved
/// individually via resolve_*(), with precedence CLI flag > env var >
/// hardcoded default. `resolve_*()` mutates `args` in place, removing the
/// flag (and its value) it matched so callers can still parse whatever
/// positional arguments remain afterward -- see client.cpp, which resolves
/// --service-name first and then dispatches its existing `get`/`put`/`erase`
/// manual-command syntax from what's left.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace poc::config {

inline std::optional<std::string> getenv_str(const char* name) {
    if (const char* v = std::getenv(name)) {
        return std::string(v);
    }
    return std::nullopt;
}

// Scans `args` for "--flag value" or "--flag=value", removing whichever form
// matched (both the flag and its value, for the two-token form) and
// returning the value. Leaves everything else in `args` untouched and in
// order. Throws if "--flag" appears with no following value.
inline std::optional<std::string> take_flag(std::vector<std::string>& args, const std::string& flag) {
    const std::string prefix = flag + "=";
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == flag) {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("missing value for " + flag);
            }
            std::string value = args[i + 1];
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
            return value;
        }
        if (args[i].rfind(prefix, 0) == 0) {
            std::string value = args[i].substr(prefix.size());
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
            return value;
        }
    }
    return std::nullopt;
}

inline std::string resolve_str(std::vector<std::string>& args, const std::string& flag, const char* env_name,
                                const std::string& default_value) {
    if (auto v = take_flag(args, flag)) {
        return *v;
    }
    return getenv_str(env_name).value_or(default_value);
}

inline std::uint16_t resolve_u16(std::vector<std::string>& args, const std::string& flag, const char* env_name,
                                  std::uint16_t default_value) {
    if (auto v = take_flag(args, flag)) {
        return static_cast<std::uint16_t>(std::stoul(*v));
    }
    if (auto v = getenv_str(env_name)) {
        return static_cast<std::uint16_t>(std::stoul(*v));
    }
    return default_value;
}

inline std::size_t resolve_size(std::vector<std::string>& args, const std::string& flag, const char* env_name,
                                 std::size_t default_value) {
    if (auto v = take_flag(args, flag)) {
        return static_cast<std::size_t>(std::stoull(*v));
    }
    if (auto v = getenv_str(env_name)) {
        return static_cast<std::size_t>(std::stoull(*v));
    }
    return default_value;
}

inline int resolve_int(std::vector<std::string>& args, const std::string& flag, const char* env_name,
                        int default_value) {
    if (auto v = take_flag(args, flag)) {
        return std::stoi(*v);
    }
    if (auto v = getenv_str(env_name)) {
        return std::stoi(*v);
    }
    return default_value;
}

inline std::chrono::milliseconds resolve_millis(std::vector<std::string>& args, const std::string& flag,
                                                  const char* env_name, long default_ms) {
    if (auto v = take_flag(args, flag)) {
        return std::chrono::milliseconds(std::stol(*v));
    }
    if (auto v = getenv_str(env_name)) {
        return std::chrono::milliseconds(std::stol(*v));
    }
    return std::chrono::milliseconds(default_ms);
}

}  // namespace poc::config
