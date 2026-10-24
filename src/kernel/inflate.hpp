#ifndef KERNEL_INFLATE_HPP
#define KERNEL_INFLATE_HPP

#include "types.hpp"

// Minimal DEFLATE (RFC 1951) decompressor with gzip (RFC 1952) and zlib
// (RFC 1950) container support. No dynamic allocation: the caller supplies the
// output buffer. Decoding stops gracefully if the output fills up.

namespace inflate {

// Decompresses `in_len` bytes from `in` into `out` (capacity `out_cap`).
// Container is auto-detected: gzip magic 1f 8b, zlib 0x78 header, otherwise a
// raw DEFLATE stream is assumed.
// Returns the number of bytes written to `out` (0 on failure/empty input).
size_t decompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap);

// True if the buffer begins with a container/method we recognise as compressed
// (gzip magic or a plausible zlib header).
bool looks_compressed(const uint8_t* in, size_t in_len);

} // namespace inflate

#endif // KERNEL_INFLATE_HPP
