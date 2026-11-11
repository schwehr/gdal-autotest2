// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Synthetic ClassicTIFF / BigTIFF binary image builder for crafting valid,
// edge-case, or malformed IFDs and headers purely in memory.

#ifndef THIRD_PARTY_TIFF_GOOGLE_UTIL_RAW_TIFF_BUILDER_H_
#define THIRD_PARTY_TIFF_GOOGLE_UTIL_RAW_TIFF_BUILDER_H_

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "third_party/absl/types/span.h"
#include "third_party/tiff/libtiff/tiff.h"

namespace libtiff {

struct RawIfdEntry {
  uint16_t tag = 0;
  uint16_t type = 0;
  uint64_t count = 0;
  // Exactly one of inline_bytes, out_of_line_bytes, or explicit_raw_slot is
  // used; if none is explicitly populated, an inline value slot (zeros or
  // packed bytes) is emitted.
  std::array<uint8_t, 8> inline_bytes{};
  bool has_out_of_line = false;
  std::vector<uint8_t> out_of_line_bytes;
};

// Builds endian-aware ClassicTIFF (magic 42) or BigTIFF (magic 43) byte buffers
// with exact control over IFD tags, types, counts, inline vs. out-of-line
// storage, offsets, and IFD chaining.
class RawTiffBuilder {
 public:
  explicit RawTiffBuilder(bool big_endian = false, bool big_tiff = false)
      : big_endian_(big_endian), big_tiff_(big_tiff) {
    EmitHeader();
  }

  bool big_endian() const { return big_endian_; }
  bool big_tiff() const { return big_tiff_; }
  size_t InlineSlotSize() const { return big_tiff_ ? 8 : 4; }

  // Appends an arbitrary byte blob (aligned to a 2-byte boundary) and returns
  // its offset in the file. Useful for strip/tile data referenced by IFDs.
  uint64_t AppendBlob(absl::Span<const uint8_t> blob) {
    AlignTo2Bytes();
    const uint64_t off = buf_.size();
    buf_.insert(buf_.end(), blob.begin(), blob.end());
    return off;
  }

  // Appends a complete IFD at the current 2-byte-aligned end of the buffer,
  // writes any out-of-line entry values first or after the IFD, and patches
  // the previous next-IFD pointer (or header first-IFD pointer for the first
  // IFD) to point to this IFD. Returns the offset of the new IFD.
  //
  // If explicit_next_ifd_offset is provided, sets this IFD's next-IFD field to
  // that value (useful for crafting IFD loops or bad next-IFD pointers).
  uint64_t AddIfd(
      std::vector<RawIfdEntry> entries,
      std::optional<uint64_t> explicit_next_ifd_offset = std::nullopt,
      bool sort_by_tag = true, bool link_from_previous = true) {
    if (sort_by_tag) {
      std::stable_sort(entries.begin(), entries.end(),
                       [](const RawIfdEntry& a, const RawIfdEntry& b) {
                         return a.tag < b.tag;
                       });
    }

    // Reserve space for out-of-line data after the IFD so offsets are valid and
    // aligned.
    AlignTo2Bytes();
    const uint64_t ifd_offset = buf_.size();
    if (link_from_previous) {
      PatchOffsetField(pending_next_ifd_patch_pos_, ifd_offset);
    }

    const size_t count_size = big_tiff_ ? 8 : 2;
    const size_t entry_size = big_tiff_ ? 20 : 12;
    const size_t next_size = big_tiff_ ? 8 : 4;
    const uint64_t ifd_byte_size =
        count_size + entries.size() * entry_size + next_size;

    // Emit out-of-line values starting immediately after the IFD (aligned to
    // 2).
    uint64_t data_cursor = ifd_offset + ifd_byte_size;
    if (data_cursor & 1U) ++data_cursor;

    std::vector<uint64_t> out_offsets(entries.size(), 0);
    for (size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].has_out_of_line) {
        if (data_cursor & 1U) ++data_cursor;
        out_offsets[i] = data_cursor;
        data_cursor += entries[i].out_of_line_bytes.size();
      }
    }

    // Write dircount.
    if (big_tiff_) {
      AppendUint64(entries.size());
    } else {
      AppendUint16(static_cast<uint16_t>(entries.size()));
    }

    // Write IFD entries.
    for (size_t i = 0; i < entries.size(); ++i) {
      const RawIfdEntry& e = entries[i];
      AppendUint16(e.tag);
      AppendUint16(e.type);
      if (big_tiff_) {
        AppendUint64(e.count);
        if (e.has_out_of_line) {
          AppendUint64(out_offsets[i]);
        } else {
          buf_.insert(buf_.end(), e.inline_bytes.begin(), e.inline_bytes.end());
        }
      } else {
        AppendUint32(static_cast<uint32_t>(e.count));
        if (e.has_out_of_line) {
          AppendUint32(static_cast<uint32_t>(out_offsets[i]));
        } else {
          buf_.insert(buf_.end(), e.inline_bytes.begin(),
                      e.inline_bytes.begin() + 4);
        }
      }
    }

    // Write next_ifd_offset slot.
    pending_next_ifd_patch_pos_ = buf_.size();
    const uint64_t next_val = explicit_next_ifd_offset.value_or(0);
    if (big_tiff_) {
      AppendUint64(next_val);
    } else {
      AppendUint32(static_cast<uint32_t>(next_val));
    }

    // Append out-of-line entry payloads matching calculated offsets.
    for (size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].has_out_of_line) {
        AlignTo2Bytes();
        buf_.insert(buf_.end(), entries[i].out_of_line_bytes.begin(),
                    entries[i].out_of_line_bytes.end());
      }
    }
    return ifd_offset;
  }

  void PatchNextIfdOffset(uint64_t target_offset) {
    PatchOffsetField(pending_next_ifd_patch_pos_, target_offset);
  }

  void PatchHeaderFirstIfdOffset(uint64_t target_offset) {
    PatchOffsetField(big_tiff_ ? 8 : 4, target_offset);
  }

  const std::vector<uint8_t>& bytes() const { return buf_; }
  std::vector<uint8_t>& mutable_bytes() { return buf_; }
  std::vector<uint8_t> Take() { return std::move(buf_); }

  // Entry construction helpers matching this builder's endianness & bitness.
  RawIfdEntry MakeByte(uint16_t tag, uint8_t value,
                       uint16_t type = TIFF_BYTE) const {
    RawIfdEntry e{.tag = tag, .type = type, .count = 1};
    e.inline_bytes[0] = value;
    return e;
  }

  RawIfdEntry MakeShort(uint16_t tag, uint16_t value,
                        uint16_t type = TIFF_SHORT) const {
    RawIfdEntry e{.tag = tag, .type = type, .count = 1};
    EncodeUint16(value, e.inline_bytes.data());
    return e;
  }

  RawIfdEntry MakeShortArray(uint16_t tag, absl::Span<const uint16_t> values,
                             uint16_t type = TIFF_SHORT) const {
    std::vector<uint8_t> raw(values.size() * 2);
    for (size_t i = 0; i < values.size(); ++i) {
      EncodeUint16(values[i], raw.data() + i * 2);
    }
    return MakeBytesOrOutOfLine(tag, type, values.size(), std::move(raw));
  }

  RawIfdEntry MakeLong(uint16_t tag, uint32_t value,
                       uint16_t type = TIFF_LONG) const {
    RawIfdEntry e{.tag = tag, .type = type, .count = 1};
    EncodeUint32(value, e.inline_bytes.data());
    return e;
  }

  RawIfdEntry MakeSlong(uint16_t tag, int32_t value) const {
    return MakeLong(tag, std::bit_cast<uint32_t>(value), TIFF_SLONG);
  }

  RawIfdEntry MakeLongArray(uint16_t tag, absl::Span<const uint32_t> values,
                            uint16_t type = TIFF_LONG) const {
    std::vector<uint8_t> raw(values.size() * 4);
    for (size_t i = 0; i < values.size(); ++i) {
      EncodeUint32(values[i], raw.data() + i * 4);
    }
    return MakeBytesOrOutOfLine(tag, type, values.size(), std::move(raw));
  }

  RawIfdEntry MakeLong8(uint16_t tag, uint64_t value,
                        uint16_t type = TIFF_LONG8) const {
    std::vector<uint8_t> raw(8);
    EncodeUint64(value, raw.data());
    return MakeBytesOrOutOfLine(tag, type, 1, std::move(raw));
  }

  RawIfdEntry MakeLong8Array(uint16_t tag, absl::Span<const uint64_t> values,
                             uint16_t type = TIFF_LONG8) const {
    std::vector<uint8_t> raw(values.size() * 8);
    for (size_t i = 0; i < values.size(); ++i) {
      EncodeUint64(values[i], raw.data() + i * 8);
    }
    return MakeBytesOrOutOfLine(tag, type, values.size(), std::move(raw));
  }

  RawIfdEntry MakeRational(uint16_t tag, uint32_t num, uint32_t den) const {
    std::vector<uint8_t> raw(8);
    EncodeUint32(num, raw.data());
    EncodeUint32(den, raw.data() + 4);
    return MakeBytesOrOutOfLine(tag, TIFF_RATIONAL, 1, std::move(raw));
  }

  RawIfdEntry MakeSrational(uint16_t tag, int32_t num, int32_t den) const {
    std::vector<uint8_t> raw(8);
    EncodeUint32(std::bit_cast<uint32_t>(num), raw.data());
    EncodeUint32(std::bit_cast<uint32_t>(den), raw.data() + 4);
    return MakeBytesOrOutOfLine(tag, TIFF_SRATIONAL, 1, std::move(raw));
  }

  RawIfdEntry MakeFloat(uint16_t tag, float value) const {
    return MakeLong(tag, std::bit_cast<uint32_t>(value), TIFF_FLOAT);
  }

  RawIfdEntry MakeDouble(uint16_t tag, double value) const {
    std::vector<uint8_t> raw(8);
    EncodeUint64(std::bit_cast<uint64_t>(value), raw.data());
    return MakeBytesOrOutOfLine(tag, TIFF_DOUBLE, 1, std::move(raw));
  }

  RawIfdEntry MakeAscii(uint16_t tag, std::string_view text,
                        bool append_nul = true) const {
    std::vector<uint8_t> raw(text.begin(), text.end());
    if (append_nul) raw.push_back(0);
    return MakeBytesOrOutOfLine(tag, TIFF_ASCII, raw.size(), std::move(raw));
  }

  RawIfdEntry MakeRawInline(uint16_t tag, uint16_t type, uint64_t count,
                            uint64_t value_or_offset) const {
    RawIfdEntry e{.tag = tag, .type = type, .count = count};
    if (big_tiff_) {
      EncodeUint64(value_or_offset, e.inline_bytes.data());
    } else {
      EncodeUint32(static_cast<uint32_t>(value_or_offset),
                   e.inline_bytes.data());
    }
    return e;
  }

  RawIfdEntry MakeOutOfLine(uint16_t tag, uint16_t type, uint64_t count,
                            std::vector<uint8_t> payload) const {
    return RawIfdEntry{
        .tag = tag,
        .type = type,
        .count = count,
        .has_out_of_line = true,
        .out_of_line_bytes = std::move(payload),
    };
  }

 private:
  RawIfdEntry MakeBytesOrOutOfLine(uint16_t tag, uint16_t type, uint64_t count,
                                   std::vector<uint8_t> raw) const {
    if (raw.size() <= InlineSlotSize()) {
      RawIfdEntry e{.tag = tag, .type = type, .count = count};
      std::copy(raw.begin(), raw.end(), e.inline_bytes.begin());
      return e;
    }
    return MakeOutOfLine(tag, type, count, std::move(raw));
  }

  void EmitHeader() {
    buf_.clear();
    const uint8_t endian_byte = big_endian_ ? 'M' : 'I';
    buf_.push_back(endian_byte);
    buf_.push_back(endian_byte);
    if (big_tiff_) {
      AppendUint16(TIFF_VERSION_BIG);
      AppendUint16(8);  // Offset byte size.
      AppendUint16(0);  // Unused zero word.
      pending_next_ifd_patch_pos_ = buf_.size();
      AppendUint64(0);
    } else {
      AppendUint16(TIFF_VERSION_CLASSIC);
      pending_next_ifd_patch_pos_ = buf_.size();
      AppendUint32(0);
    }
  }

  void AlignTo2Bytes() {
    if (buf_.size() & 1U) buf_.push_back(0);
  }

  void PatchOffsetField(size_t pos, uint64_t value) {
    if (big_tiff_) {
      EncodeUint64(value, buf_.data() + pos);
    } else {
      EncodeUint32(static_cast<uint32_t>(value), buf_.data() + pos);
    }
  }

  void AppendUint16(uint16_t v) {
    std::array<uint8_t, 2> tmp{};
    EncodeUint16(v, tmp.data());
    buf_.insert(buf_.end(), tmp.begin(), tmp.end());
  }

  void AppendUint32(uint32_t v) {
    std::array<uint8_t, 4> tmp{};
    EncodeUint32(v, tmp.data());
    buf_.insert(buf_.end(), tmp.begin(), tmp.end());
  }

  void AppendUint64(uint64_t v) {
    std::array<uint8_t, 8> tmp{};
    EncodeUint64(v, tmp.data());
    buf_.insert(buf_.end(), tmp.begin(), tmp.end());
  }

  void EncodeUint16(uint16_t v, uint8_t* dst) const {
    if (big_endian_) {
      dst[0] = static_cast<uint8_t>((v >> 8) & 0xff);
      dst[1] = static_cast<uint8_t>(v & 0xff);
    } else {
      dst[0] = static_cast<uint8_t>(v & 0xff);
      dst[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    }
  }

  void EncodeUint32(uint32_t v, uint8_t* dst) const {
    for (int i = 0; i < 4; ++i) {
      const int shift = big_endian_ ? (24 - 8 * i) : (8 * i);
      dst[i] = static_cast<uint8_t>((v >> shift) & 0xffU);
    }
  }

  void EncodeUint64(uint64_t v, uint8_t* dst) const {
    for (int i = 0; i < 8; ++i) {
      const int shift = big_endian_ ? (56 - 8 * i) : (8 * i);
      dst[i] = static_cast<uint8_t>((v >> shift) & 0xffULL);
    }
  }

  bool big_endian_;
  bool big_tiff_;
  size_t pending_next_ifd_patch_pos_ = 0;
  std::vector<uint8_t> buf_;
};

}  // namespace libtiff

#endif  // THIRD_PARTY_TIFF_GOOGLE_UTIL_RAW_TIFF_BUILDER_H_
