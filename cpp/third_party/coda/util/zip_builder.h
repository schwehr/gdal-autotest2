// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Minimal in-memory ZIP and .codadef archive synthesizer using zlib_rs so tests
// can construct valid and malformed archives without checked-in binary files.

#ifndef THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_ZIP_BUILDER_H_
#define THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_ZIP_BUILDER_H_

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "third_party/rust/zlib_rs/zlib.h"

namespace stcorp_coda {

enum class ZipMethod : uint16_t {
  kStored = 0,
  kDeflated = 8,
};

struct ZipEntrySpec {
  std::string_view name;
  std::span<const uint8_t> data;
  ZipMethod method = ZipMethod::kStored;
  uint16_t flags = 0;
  uint32_t external_attr = 0;
};

namespace zip_internal {

inline void AppendLe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

inline void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

inline bool RawDeflate(std::span<const uint8_t> input,
                       std::vector<uint8_t>& output) {
  z_stream zs{};
  if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    return false;
  }
  zs.next_in = input.empty() ? nullptr : const_cast<Bytef*>(input.data());
  zs.avail_in = static_cast<uInt>(input.size());

  output.resize(deflateBound(&zs, static_cast<uLong>(input.size())) + 16);
  zs.next_out = output.data();
  zs.avail_out = static_cast<uInt>(output.size());

  const int rc = deflate(&zs, Z_FINISH);
  const uLong total_out = zs.total_out;
  deflateEnd(&zs);
  if (rc != Z_STREAM_END) {
    return false;
  }
  output.resize(total_out);
  return true;
}

}  // namespace zip_internal

// Builds a standard PKZIP archive in memory from `entries`.
inline std::vector<uint8_t> BuildZipBytes(
    std::span<const ZipEntrySpec> entries) {
  std::vector<uint8_t> archive;
  struct CentralDirRecord {
    uint32_t offset;
    uint32_t crc;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint16_t method;
    uint16_t flags;
    uint32_t external_attr;
    std::string_view name;
  };
  std::vector<CentralDirRecord> cd_records;
  cd_records.reserve(entries.size());

  for (const ZipEntrySpec& entry : entries) {
    const uint32_t crc =
        entry.data.empty()
            ? 0
            : static_cast<uint32_t>(crc32(
                  0L, entry.data.data(), static_cast<uInt>(entry.data.size())));
    std::vector<uint8_t> compressed;
    std::span<const uint8_t> payload = entry.data;
    uint16_t method = static_cast<uint16_t>(entry.method);
    if (entry.method == ZipMethod::kDeflated) {
      if (zip_internal::RawDeflate(entry.data, compressed)) {
        payload = compressed;
      } else {
        method = static_cast<uint16_t>(ZipMethod::kStored);
      }
    }

    const uint32_t local_offset = static_cast<uint32_t>(archive.size());
    const uint16_t name_len = static_cast<uint16_t>(entry.name.size());
    const uint32_t comp_size = static_cast<uint32_t>(payload.size());
    const uint32_t uncomp_size = static_cast<uint32_t>(entry.data.size());

    // Local file header (PK\x03\x04).
    zip_internal::AppendLe32(archive, 0x04034B50U);
    zip_internal::AppendLe16(archive, 20);  // version needed to extract
    zip_internal::AppendLe16(archive, entry.flags);
    zip_internal::AppendLe16(archive, method);
    zip_internal::AppendLe16(archive, 0);  // mod time
    zip_internal::AppendLe16(archive, 0);  // mod date
    zip_internal::AppendLe32(archive, crc);
    zip_internal::AppendLe32(archive, comp_size);
    zip_internal::AppendLe32(archive, uncomp_size);
    zip_internal::AppendLe16(archive, name_len);
    zip_internal::AppendLe16(archive, 0);  // extra field length
    archive.insert(archive.end(), entry.name.begin(), entry.name.end());
    archive.insert(archive.end(), payload.begin(), payload.end());

    cd_records.push_back({
        .offset = local_offset,
        .crc = crc,
        .comp_size = comp_size,
        .uncomp_size = uncomp_size,
        .method = method,
        .flags = entry.flags,
        .external_attr = entry.external_attr,
        .name = entry.name,
    });
  }

  const uint32_t cd_offset = static_cast<uint32_t>(archive.size());
  for (const CentralDirRecord& rec : cd_records) {
    // Central directory file header (PK\x01\x02).
    zip_internal::AppendLe32(archive, 0x02014B50U);
    zip_internal::AppendLe16(archive, 20);  // version made by
    zip_internal::AppendLe16(archive, 20);  // version needed
    zip_internal::AppendLe16(archive, rec.flags);
    zip_internal::AppendLe16(archive, rec.method);
    zip_internal::AppendLe16(archive, 0);  // mod time
    zip_internal::AppendLe16(archive, 0);  // mod date
    zip_internal::AppendLe32(archive, rec.crc);
    zip_internal::AppendLe32(archive, rec.comp_size);
    zip_internal::AppendLe32(archive, rec.uncomp_size);
    zip_internal::AppendLe16(archive, static_cast<uint16_t>(rec.name.size()));
    zip_internal::AppendLe16(archive, 0);  // extra len
    zip_internal::AppendLe16(archive, 0);  // comment len
    zip_internal::AppendLe16(archive, 0);  // disk number start
    zip_internal::AppendLe16(archive, 0);  // internal file attrs
    zip_internal::AppendLe32(archive, rec.external_attr);
    zip_internal::AppendLe32(archive, rec.offset);
    archive.insert(archive.end(), rec.name.begin(), rec.name.end());
  }
  const uint32_t cd_size = static_cast<uint32_t>(archive.size()) - cd_offset;

  // End of central directory record (PK\x05\x06).
  zip_internal::AppendLe32(archive, 0x06054B50U);
  zip_internal::AppendLe16(archive, 0);
  zip_internal::AppendLe16(archive, 0);
  zip_internal::AppendLe16(archive, static_cast<uint16_t>(cd_records.size()));
  zip_internal::AppendLe16(archive, static_cast<uint16_t>(cd_records.size()));
  zip_internal::AppendLe32(archive, cd_size);
  zip_internal::AppendLe32(archive, cd_offset);
  zip_internal::AppendLe16(archive, 0);  // comment len
  return archive;
}

// Convenience wrapper that builds a `.codadef` ZIP archive containing
// `index.xml` and optional `types/*.xml` or `products/*.xml` entries.
inline std::vector<uint8_t> BuildCodadefBytes(
    std::string_view index_xml,
    std::span<const std::pair<std::string_view, std::string_view>>
        extra_entries = {},
    ZipMethod method = ZipMethod::kDeflated) {
  std::vector<ZipEntrySpec> specs;
  specs.reserve(1 + extra_entries.size());
  specs.push_back({
      .name = "index.xml",
      .data = std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(index_xml.data()), index_xml.size()),
      .method = method,
  });
  for (const auto& [name, content] : extra_entries) {
    specs.push_back({
        .name = name,
        .data = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(content.data()), content.size()),
        .method = method,
    });
  }
  return BuildZipBytes(specs);
}

}  // namespace stcorp_coda

#endif  // THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_ZIP_BUILDER_H_
