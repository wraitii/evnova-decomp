#pragma once

// Shared big/little-endian scalar readers for the resource decoders and the
// game data loaders. Each reader is bounds-checked against the payload and
// returns 0 for a short read: the original game trusts the resource lengths,
// so returning 0 keeps a truncated or malformed file from reading past the
// buffer (the readers used to be duplicated per translation unit with
// inconsistent/no checking).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace evnova::util {

[[nodiscard]] inline std::uint16_t ReadBe16(std::span<const std::byte> bytes,
                                            std::size_t offset) {
  if (offset + 2 > bytes.size()) {
    return 0;
  }
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) << 8U |
      std::to_integer<std::uint8_t>(bytes[offset + 1]));
}

[[nodiscard]] inline std::int16_t ReadBeI16(std::span<const std::byte> bytes,
                                            std::size_t offset) {
  return static_cast<std::int16_t>(ReadBe16(bytes, offset));
}

[[nodiscard]] inline std::uint32_t ReadBe32(std::span<const std::byte> bytes,
                                            std::size_t offset) {
  if (offset + 4 > bytes.size()) {
    return 0;
  }
  return std::to_integer<std::uint32_t>(bytes[offset]) << 24U |
         std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16U |
         std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8U |
         std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] inline std::int32_t ReadBeI32(std::span<const std::byte> bytes,
                                            std::size_t offset) {
  return static_cast<std::int32_t>(ReadBe32(bytes, offset));
}

[[nodiscard]] inline std::uint16_t ReadLe16(std::span<const std::byte> bytes,
                                            std::size_t offset) {
  if (offset + 2 > bytes.size()) {
    return 0;
  }
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) |
      std::to_integer<std::uint8_t>(bytes[offset + 1]) << 8U);
}

[[nodiscard]] inline std::uint32_t ReadLe32(std::span<const std::byte> bytes,
                                            std::size_t offset) {
  if (offset + 4 > bytes.size()) {
    return 0;
  }
  return std::to_integer<std::uint32_t>(bytes[offset]) |
         std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U |
         std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U |
         std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U;
}

// NUL-terminated C string at `offset`, bounded by the payload. Returns an
// empty string when the offset is past the end or the string is unterminated.
[[nodiscard]] inline std::string ReadCString(std::span<const std::byte> bytes,
                                             std::size_t offset) {
  if (offset >= bytes.size()) {
    return {};
  }
  const auto *begin = reinterpret_cast<const char *>(bytes.data() + offset);
  const auto max_len = bytes.size() - offset;
  const void *nul = std::memchr(begin, '\0', max_len);
  const auto length =
      nul != nullptr
          ? static_cast<std::size_t>(static_cast<const char *>(nul) - begin)
          : max_len;
  return std::string{begin, length};
}

} // namespace evnova::util
