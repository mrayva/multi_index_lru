#include "../src/wire.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace poc::wire {
namespace {

// --- Writer: each primitive appends the expected little-endian bytes ------

TEST(WireWriter, U8AppendsSingleByte) {
    Writer w;
    w.u8(0x42);
    EXPECT_EQ(w.buffer(), (std::vector<std::uint8_t>{0x42}));
}

TEST(WireWriter, U32AppendsLittleEndian) {
    Writer w;
    w.u32(0x01020304);
    EXPECT_EQ(w.buffer(), (std::vector<std::uint8_t>{0x04, 0x03, 0x02, 0x01}));
}

TEST(WireWriter, I64AppendsLittleEndian) {
    Writer w;
    w.i64(0x0102030405060708);
    EXPECT_EQ(w.buffer(), (std::vector<std::uint8_t>{0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}));
}

TEST(WireWriter, BytesAppendsRawContent) {
    Writer w;
    w.bytes({0xAA, 0xBB, 0xCC});
    EXPECT_EQ(w.buffer(), (std::vector<std::uint8_t>{0xAA, 0xBB, 0xCC}));
}

TEST(WireWriter, StrAppendsLengthPrefixThenContent) {
    Writer w;
    w.str("hi");
    // u32 length prefix (2, little-endian) followed by the two chars.
    EXPECT_EQ(w.buffer(), (std::vector<std::uint8_t>{0x02, 0x00, 0x00, 0x00, 'h', 'i'}));
}

TEST(WireWriter, StrHandlesEmptyString) {
    Writer w;
    w.str("");
    EXPECT_EQ(w.buffer(), (std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x00}));
}

TEST(WireWriter, MultipleWritesAppendInOrder) {
    Writer w;
    w.u8(1);
    w.u32(2);
    w.str("ab");
    EXPECT_EQ(w.buffer(), (std::vector<std::uint8_t>{1, 2, 0, 0, 0, 2, 0, 0, 0, 'a', 'b'}));
}

// --- Reader: round-trips through a Writer-built buffer ---------------------

TEST(WireRoundTrip, U8) {
    for (std::uint8_t v : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{200}, std::uint8_t{255}}) {
        Writer w;
        w.u8(v);
        Reader r(w.buffer().data(), w.buffer().size());
        EXPECT_EQ(r.u8(), v);
    }
}

TEST(WireRoundTrip, U32) {
    for (std::uint32_t v : {0u, 1u, 42u, std::numeric_limits<std::uint32_t>::max()}) {
        Writer w;
        w.u32(v);
        Reader r(w.buffer().data(), w.buffer().size());
        EXPECT_EQ(r.u32(), v);
    }
}

TEST(WireRoundTrip, I64PositiveNegativeAndExtremes) {
    for (std::int64_t v : {std::int64_t{0}, std::int64_t{42}, std::int64_t{-1}, std::int64_t{-42},
                            std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min()}) {
        Writer w;
        w.i64(v);
        Reader r(w.buffer().data(), w.buffer().size());
        EXPECT_EQ(r.i64(), v);
    }
}

TEST(WireRoundTrip, Bytes) {
    std::vector<std::uint8_t> data{1, 2, 3, 4, 5};
    Writer w;
    w.bytes(data);
    Reader r(w.buffer().data(), w.buffer().size());
    EXPECT_EQ(r.bytes(data.size()), data);
}

TEST(WireRoundTrip, StrIncludingEmpty) {
    for (const std::string& s : {std::string(""), std::string("a"), std::string("hello world"),
                                  std::string(1000, 'x')}) {
        Writer w;
        w.str(s);
        Reader r(w.buffer().data(), w.buffer().size());
        EXPECT_EQ(r.str(), s);
    }
}

TEST(WireRoundTrip, StrIsBinarySafe) {
    // A key/record could legitimately contain NUL bytes or arbitrary binary
    // data (e.g. a msgpack/flexbuffer blob) -- the length-prefixed encoding
    // must not truncate at an embedded '\0' the way a C-string would.
    std::string s{'\x00', '\x01', 'a', '\x00', 'b'};
    Writer w;
    w.str(s);
    Reader r(w.buffer().data(), w.buffer().size());
    auto decoded = r.str();
    EXPECT_EQ(decoded, s);
    EXPECT_EQ(decoded.size(), 5u);
}

TEST(WireRoundTrip, RemainingReturnsRestOfBuffer) {
    Writer w;
    w.u8(1);
    w.bytes({0xDE, 0xAD, 0xBE, 0xEF});
    Reader r(w.buffer().data(), w.buffer().size());
    r.u8();  // consume the leading byte
    EXPECT_EQ(r.remaining(), (std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}));
}

TEST(WireRoundTrip, SequentialMixedFieldsMatchesProtocolShape) {
    // Shape of a real Put request: [op:u8][key_kind:u8][name:str][record_len:u32][record bytes]
    Writer w;
    w.u8(static_cast<std::uint8_t>(Op::Put));
    w.u8(static_cast<std::uint8_t>(KeyKind::Name));
    w.str("dave");
    std::vector<std::uint8_t> record{'{', '}'};
    w.u32(static_cast<std::uint32_t>(record.size()));
    w.bytes(record);

    Reader r(w.buffer().data(), w.buffer().size());
    EXPECT_EQ(static_cast<Op>(r.u8()), Op::Put);
    EXPECT_EQ(static_cast<KeyKind>(r.u8()), KeyKind::Name);
    EXPECT_EQ(r.str(), "dave");
    EXPECT_EQ(r.bytes(r.u32()), record);
}

// --- Reader: truncation is reported as std::out_of_range, not UB or a crash

TEST(WireTruncation, U8OnEmptyBufferThrows) {
    Reader r(nullptr, 0);
    EXPECT_THROW(r.u8(), std::out_of_range);
}

TEST(WireTruncation, U32WithInsufficientBytesThrows) {
    std::vector<std::uint8_t> data{1, 2, 3};  // one short of a u32
    Reader r(data.data(), data.size());
    EXPECT_THROW(r.u32(), std::out_of_range);
}

TEST(WireTruncation, I64WithInsufficientBytesThrows) {
    std::vector<std::uint8_t> data{1, 2, 3, 4, 5, 6, 7};  // one short of an i64
    Reader r(data.data(), data.size());
    EXPECT_THROW(r.i64(), std::out_of_range);
}

TEST(WireTruncation, BytesRequestingMoreThanAvailableThrows) {
    std::vector<std::uint8_t> data{1, 2, 3};
    Reader r(data.data(), data.size());
    EXPECT_THROW(r.bytes(4), std::out_of_range);
}

TEST(WireTruncation, StrWithLengthPrefixExceedingBufferThrows) {
    // This is the exact shape of the malformed request used to verify
    // server.cpp / server_readthrough.cpp survive a bad message: a length
    // prefix claiming far more data than actually follows it.
    Writer w;
    w.u32(0x7FFFFFFF);  // claims ~2GB of string content
    Reader r(w.buffer().data(), w.buffer().size());
    EXPECT_THROW(r.str(), std::out_of_range);
}

TEST(WireTruncation, ReadingExactlyToTheBoundarySucceeds) {
    Writer w;
    w.u8(1);
    w.u32(2);
    Reader r(w.buffer().data(), w.buffer().size());
    EXPECT_NO_THROW(r.u8());
    EXPECT_NO_THROW(r.u32());
}

TEST(WireTruncation, ReadingOneBytePastTheBoundaryThrows) {
    Writer w;
    w.u8(1);
    Reader r(w.buffer().data(), w.buffer().size());
    r.u8();
    EXPECT_THROW(r.u8(), std::out_of_range);
}

// --- Enum values are the actual wire format: pin them as a regression test.
// Both client and server encode/decode these as raw bytes; a silent value
// change here would break wire compatibility between builds.

TEST(WireEnums, OpValuesArePinned) {
    EXPECT_EQ(static_cast<std::uint8_t>(Op::Get), 0);
    EXPECT_EQ(static_cast<std::uint8_t>(Op::Put), 1);
    EXPECT_EQ(static_cast<std::uint8_t>(Op::Erase), 2);
}

TEST(WireEnums, KeyKindValuesArePinned) {
    EXPECT_EQ(static_cast<std::uint8_t>(KeyKind::Name), 0);
    EXPECT_EQ(static_cast<std::uint8_t>(KeyKind::Id), 1);
}

TEST(WireEnums, StatusValuesArePinned) {
    EXPECT_EQ(static_cast<std::uint8_t>(Status::Ok), 0);
    EXPECT_EQ(static_cast<std::uint8_t>(Status::NotFound), 1);
    EXPECT_EQ(static_cast<std::uint8_t>(Status::Error), 2);
}

}  // namespace
}  // namespace poc::wire
