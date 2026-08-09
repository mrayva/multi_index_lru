/// Minimal little-endian binary wire format for the cache POC's request and
/// response messages. POC scope only: assumes a little-endian host (fine for
/// the x86_64 machine this runs on) and throws on a truncated/malformed
/// message rather than doing anything more graceful.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace poc::wire {

// GetAll: looks up every NameEntry sharing a (non-unique) category rather
// than a single record by its (unique) primary key -- see cache_service.hpp
// "Non-unique key lookup (GetAll)" and README.md for the full design; it's
// the only Op that pairs with KeyKind::Category.
enum class Op : std::uint8_t { Get = 0, Put = 1, Erase = 2, GetAll = 3 };
enum class KeyKind : std::uint8_t { Name = 0, Id = 1, Category = 2 };
// ReadOnly: a Put/Erase reached a server_readthrough.cpp instance that's
// running without --allow-writes/MIL_ALLOW_WRITES -- distinct from Error so
// a caller can tell "this daemon deliberately doesn't accept writes" apart
// from "something actually went wrong." See README.md "Read-only by
// default".
enum class Status : std::uint8_t { Ok = 0, NotFound = 1, Error = 2, ReadOnly = 3 };

class Writer {
public:
    void u8(std::uint8_t v) { buf_.push_back(v); }

    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            buf_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }

    void i64(std::int64_t v) {
        const auto u = static_cast<std::uint64_t>(v);
        for (int i = 0; i < 8; ++i) {
            buf_.push_back(static_cast<std::uint8_t>(u >> (8 * i)));
        }
    }

    void bytes(const std::vector<std::uint8_t>& v) { buf_.insert(buf_.end(), v.begin(), v.end()); }

    void str(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        buf_.insert(buf_.end(), s.begin(), s.end());
    }

    const std::vector<std::uint8_t>& buffer() const { return buf_; }

private:
    std::vector<std::uint8_t> buf_;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    std::uint8_t u8() {
        need(1);
        return data_[pos_++];
    }

    std::uint32_t u32() {
        need(4);
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(data_[pos_ + i]) << (8 * i);
        }
        pos_ += 4;
        return v;
    }

    std::int64_t i64() {
        need(8);
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(data_[pos_ + i]) << (8 * i);
        }
        pos_ += 8;
        return static_cast<std::int64_t>(v);
    }

    std::vector<std::uint8_t> bytes(std::size_t n) {
        need(n);
        std::vector<std::uint8_t> out(data_ + pos_, data_ + pos_ + n);
        pos_ += n;
        return out;
    }

    std::string str() {
        const auto n = u32();
        need(n);
        std::string out(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return out;
    }

    std::vector<std::uint8_t> remaining() { return bytes(size_ - pos_); }

private:
    void need(std::size_t n) const {
        if (pos_ + n > size_) {
            throw std::out_of_range("wire::Reader: message truncated");
        }
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

}  // namespace poc::wire
