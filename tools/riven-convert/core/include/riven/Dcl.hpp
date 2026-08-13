#pragma once

// PKWARE Data Compression Library "implode", the compression inside an
// InstallShield v3 installer archive.
//
// Riven's 5-CD release does not ship riven.exe or extras.mhk as files. Both are
// inside program/arcriven.z, which is an InstallShield v3 container whose
// entries are DCL-compressed -- so the cursors and the inventory art are behind
// this decoder and nothing else. ScummVM reaches them the same way
// (riven.cpp:122-125 opens arcriven.z and adds it to its search path;
// common/compression/installshieldv3_archive.cpp:99 hands each entry to
// Common::decompressDCL).
//
// SPECIFICATION, NOT SOURCE. docs/licensing.md forbids copying ScummVM code;
// common/compression/dcl.cpp is cited here as the description of a published
// format and this implementation is written from that description. The format
// itself is PKWARE's and is documented in the appnote and in zlib's contrib
// blast.c; nothing here is anyone else's expression.
//
// The three fixed Huffman tables are the part worth knowing about. They are
// stored as run-length-encoded code LENGTHS -- each byte is
// (repeat - 1) << 4 | codeLength -- and decoded MSB-first with the code bits
// INVERTED. Getting the inversion wrong produces a stream that decodes without
// erroring and is complete garbage, which is why it is stated twice.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace riven
{

/// Decompress a DCL stream.
///
/// `expected` is the size the container claims the entry has. A stream that
/// decodes to a different length is reported as damaged rather than returned:
/// half a PE file parses just far enough to be confusing.
///
/// Never throws and never reads past `size`. Returns an empty vector with
/// `error` set on any malformed input.
std::vector<std::uint8_t> decompressDcl(const std::uint8_t *data, std::size_t size,
                                        std::size_t expected, std::string &error);

} // namespace riven
