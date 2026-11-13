#include "urt/net/bytes.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace urt::net {

void ByteWriter::u8(std::uint8_t value) { buffer_.push_back(value); }

void ByteWriter::u16(std::uint16_t value) {
    buffer_.push_back(static_cast<std::uint8_t>(value & 0xFF));
    buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void ByteWriter::u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

void ByteWriter::f32(float value) {
    // bit_cast copies the float's bits into an integer without changing them.
    u32(std::bit_cast<std::uint32_t>(value));
}

void ByteWriter::string(std::string_view text) {
    const std::size_t length = std::min<std::size_t>(text.size(), 255);
    u8(static_cast<std::uint8_t>(length));
    buffer_.insert(buffer_.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(length));
}

const std::uint8_t* ByteReader::take(std::size_t count) {
    if (!ok_ || count > remaining()) {
        ok_ = false;
        return nullptr;
    }
    const std::uint8_t* start = data_.data() + position_;
    position_ += count;
    return start;
}

std::uint8_t ByteReader::u8() {
    const std::uint8_t* bytes = take(1);
    return bytes != nullptr ? bytes[0] : std::uint8_t{0};
}

std::uint16_t ByteReader::u16() {
    const std::uint8_t* bytes = take(2);
    if (bytes == nullptr) {
        return 0;
    }
    return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

std::uint32_t ByteReader::u32() {
    const std::uint8_t* bytes = take(4);
    if (bytes == nullptr) {
        return 0;
    }
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

float ByteReader::f32() {
    const float value = std::bit_cast<float>(u32());
    if (!std::isfinite(value)) {
        ok_ = false;
        return 0.0f;
    }
    return value;
}

std::string ByteReader::string(std::size_t max_length) {
    const std::size_t length = u8();
    if (length > max_length) {
        ok_ = false;
        return {};
    }
    const std::uint8_t* bytes = take(length);
    if (bytes == nullptr) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(bytes), length);
}

} // namespace urt::net
