#pragma once

// Turning values into bytes and back.
//
// Everything on the wire is little-endian and written byte by byte, so the
// format does not depend on the CPU or the compiler.
//
// ByteReader never trusts its input: packets come from the network and may be
// cut short or forged. A read past the end does not crash; it returns zero and
// marks the reader as failed. Callers read all fields first and check ok()
// once at the end.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace urt::net {

class ByteWriter {
public:
    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void f32(float value);

    // One length byte followed by the characters. Longer text is cut to 255.
    void string(std::string_view text);

    std::span<const std::uint8_t> bytes() const { return buffer_; }
    std::size_t size() const { return buffer_.size(); }
    void clear() { buffer_.clear(); }

private:
    std::vector<std::uint8_t> buffer_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) : data_(data) {}

    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();

    // Fails on NaN and infinity: one such value would poison every position
    // computed from it.
    float f32();

    // Fails if the text is longer than max_length.
    std::string string(std::size_t max_length);

    // False once any read failed. Stays false.
    bool ok() const { return ok_; }

    std::size_t remaining() const { return data_.size() - position_; }

    // True if everything was read successfully and nothing is left over.
    bool finished() const { return ok_ && remaining() == 0; }

private:
    // Returns a pointer to `count` bytes and moves past them, or nullptr.
    const std::uint8_t* take(std::size_t count);

    std::span<const std::uint8_t> data_;
    std::size_t position_ = 0;
    bool ok_ = true;
};

} // namespace urt::net
