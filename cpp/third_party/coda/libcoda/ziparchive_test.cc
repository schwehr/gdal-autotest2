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
// Tests ZIP archive parsing, entry lookup, decompression, and error handling
// (libcoda/ziparchive.c).
//
// google3 build caveats:
//   - ziparchive.c links against //third_party/rust/zlib_rs:zlib for raw
//     deflate decompression (inflateInit2 with windowBits = -15).
//   - WORDS_BIGENDIAN and WIN32 are undefined in config.h, so byte-swapping and
//     Windows-specific open flags are compiled out.
//   - Coverage ceiling on libcoda/ziparchive.c is 279/335 (83.3%) = 100% of
//     reachable non-OOM, non-syscall-failure lines: all 56 missed lines
//     (123-124, 129-130, 160-161, 171-172, 187-188, 284-285, 291-292,
//     310-311, 386, 388-390, 395-396, 398-400, 430-432, 514-515, 522-523,
//     631-632, 640-641, 658-659, 665-667, 686, 688-689, 692, 694-695,
//     713-716, 722, 724-725, 728, 730) are OOM (malloc/strdup == NULL),
//     read()/lseek() < 0 syscall error checks, or zlib init/end/mem errors.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/ziparchive.c
//   https://github.com/stcorp/coda/blob/master/libcoda/ziparchive.h

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>  // NOLINT
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include "third_party/stcorp_coda/libcoda/ziparchive.h"
}  // extern "C"

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/rust/zlib_rs/zlib.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

namespace stcorp_coda {
namespace {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

constexpr uint32_t kLocalHeaderSignature = 0x04034B50U;
constexpr uint32_t kCentralDirSignature = 0x02014B50U;
constexpr uint32_t kEocdSignature = 0x06054B50U;

thread_local std::vector<std::string>* g_captured_errors = nullptr;

void CaptureErrorHandler(const char* fmt, ...) {
  if (g_captured_errors == nullptr || fmt == nullptr) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  va_list ap_copy;
  va_copy(ap_copy, ap);
  const int needed = std::vsnprintf(nullptr, 0, fmt, ap_copy);
  va_end(ap_copy);
  if (needed >= 0) {
    std::string msg(static_cast<size_t>(needed) + 1, '\0');
    std::vsnprintf(msg.data(), msg.size(), fmt, ap);
    msg.resize(static_cast<size_t>(needed));
    g_captured_errors->push_back(std::move(msg));
  }
  va_end(ap);
}

class ScopedErrorCapture {
 public:
  ScopedErrorCapture() : previous_(g_captured_errors) {
    g_captured_errors = &messages_;
  }

  ~ScopedErrorCapture() { g_captured_errors = previous_; }

  ScopedErrorCapture(const ScopedErrorCapture&) = delete;
  ScopedErrorCapture& operator=(const ScopedErrorCapture&) = delete;

  const std::vector<std::string>& messages() const { return messages_; }

 private:
  std::vector<std::string> messages_;
  std::vector<std::string>* previous_;
};

void AppendLe16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFFU));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFFU));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFU));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFU));
}

void WriteLe16(std::span<uint8_t> data, size_t offset, uint16_t value) {
  data[offset] = static_cast<uint8_t>(value & 0xFFU);
  data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
}

void WriteLe32(std::span<uint8_t> data, size_t offset, uint32_t value) {
  data[offset] = static_cast<uint8_t>(value & 0xFFU);
  data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
  data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
  data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

std::vector<uint8_t> ToBytes(std::string_view text) {
  std::vector<uint8_t> bytes;
  bytes.reserve(text.size());
  for (const char ch : text) {
    bytes.push_back(static_cast<uint8_t>(ch));
  }
  return bytes;
}

std::vector<uint8_t> DeflateRaw(std::span<const uint8_t> input) {
  z_stream zs{};
  const int init_rc = deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                                   -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
  EXPECT_EQ(Z_OK, init_rc);
  if (init_rc != Z_OK) {
    return {};
  }
  const absl::Cleanup end_stream = [&zs] { deflateEnd(&zs); };

  zs.next_in = input.empty() ? nullptr : const_cast<Bytef*>(input.data());
  zs.avail_in = static_cast<uInt>(input.size());

  std::vector<uint8_t> output(
      deflateBound(&zs, static_cast<uLong>(input.size())) + 16);
  zs.next_out = output.data();
  zs.avail_out = static_cast<uInt>(output.size());

  EXPECT_EQ(Z_STREAM_END, deflate(&zs, Z_FINISH));
  output.resize(zs.total_out);
  return output;
}

struct CustomZipEntry {
  std::string_view name;
  std::vector<uint8_t> payload;
  uint16_t local_compression = 0;
  uint16_t cd_compression = 0;
  uint16_t local_mod_time = 0;
  uint16_t cd_mod_time = 0;
  uint16_t local_mod_date = 0;
  uint16_t cd_mod_date = 0;
  uint32_t local_crc = 0;
  uint32_t cd_crc = 0;
  std::optional<uint32_t> local_compressed_size;
  std::optional<uint32_t> cd_compressed_size;
  std::optional<uint32_t> local_uncompressed_size;
  std::optional<uint32_t> cd_uncompressed_size;
  std::optional<uint16_t> local_name_len;
  std::optional<uint16_t> cd_name_len;
  std::vector<uint8_t> local_extra;
  std::vector<uint8_t> cd_extra;
  std::vector<uint8_t> cd_comment;
  uint16_t internal_attr = 0;
  uint32_t external_attr = 0;
  uint32_t local_signature = kLocalHeaderSignature;
  uint32_t cd_signature = kCentralDirSignature;
  std::optional<uint32_t> local_offset_override;
};

// Builds a PKZIP archive where local header and central directory fields can be
// independently controlled to exercise header checks, extra fields, and corrupt
// offsets/sizes.
std::vector<uint8_t> BuildCustomZipBytes(
    std::span<const CustomZipEntry> entries) {
  std::vector<uint8_t> archive;
  std::vector<uint32_t> local_offsets;
  local_offsets.reserve(entries.size());

  for (const CustomZipEntry& entry : entries) {
    local_offsets.push_back(static_cast<uint32_t>(archive.size()));
    const uint32_t payload_size = static_cast<uint32_t>(entry.payload.size());
    const uint32_t comp_size =
        entry.local_compressed_size.value_or(payload_size);
    const uint32_t uncomp_size =
        entry.local_uncompressed_size.value_or(payload_size);
    const uint16_t name_len =
        entry.local_name_len.value_or(static_cast<uint16_t>(entry.name.size()));

    AppendLe32(archive, entry.local_signature);
    AppendLe16(archive, 20);  // version needed to extract
    AppendLe16(archive, 0);   // general purpose bit flag
    AppendLe16(archive, entry.local_compression);
    AppendLe16(archive, entry.local_mod_time);
    AppendLe16(archive, entry.local_mod_date);
    AppendLe32(archive, entry.local_crc);
    AppendLe32(archive, comp_size);
    AppendLe32(archive, uncomp_size);
    AppendLe16(archive, name_len);
    AppendLe16(archive, static_cast<uint16_t>(entry.local_extra.size()));
    archive.insert(archive.end(), entry.name.begin(), entry.name.end());
    archive.insert(archive.end(), entry.local_extra.begin(),
                   entry.local_extra.end());
    archive.insert(archive.end(), entry.payload.begin(), entry.payload.end());
  }

  const uint32_t cd_offset = static_cast<uint32_t>(archive.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    const CustomZipEntry& entry = entries[i];
    const uint32_t payload_size = static_cast<uint32_t>(entry.payload.size());
    const uint32_t comp_size = entry.cd_compressed_size.value_or(payload_size);
    const uint32_t uncomp_size =
        entry.cd_uncompressed_size.value_or(payload_size);
    const uint16_t name_len =
        entry.cd_name_len.value_or(static_cast<uint16_t>(entry.name.size()));
    const uint32_t local_offset =
        entry.local_offset_override.value_or(local_offsets[i]);

    AppendLe32(archive, entry.cd_signature);
    AppendLe16(archive, 20);  // version made by
    AppendLe16(archive, 20);  // version needed
    AppendLe16(archive, 0);   // flags
    AppendLe16(archive, entry.cd_compression);
    AppendLe16(archive, entry.cd_mod_time);
    AppendLe16(archive, entry.cd_mod_date);
    AppendLe32(archive, entry.cd_crc);
    AppendLe32(archive, comp_size);
    AppendLe32(archive, uncomp_size);
    AppendLe16(archive, name_len);
    AppendLe16(archive, static_cast<uint16_t>(entry.cd_extra.size()));
    AppendLe16(archive, static_cast<uint16_t>(entry.cd_comment.size()));
    AppendLe16(archive, 0);  // disk number start
    AppendLe16(archive, entry.internal_attr);
    AppendLe32(archive, entry.external_attr);
    AppendLe32(archive, local_offset);
    archive.insert(archive.end(), entry.name.begin(), entry.name.end());
    archive.insert(archive.end(), entry.cd_extra.begin(), entry.cd_extra.end());
    archive.insert(archive.end(), entry.cd_comment.begin(),
                   entry.cd_comment.end());
  }
  const uint32_t cd_size = static_cast<uint32_t>(archive.size()) - cd_offset;

  AppendLe32(archive, kEocdSignature);
  AppendLe16(archive, 0);
  AppendLe16(archive, 0);
  AppendLe16(archive, static_cast<uint16_t>(entries.size()));
  AppendLe16(archive, static_cast<uint16_t>(entries.size()));
  AppendLe32(archive, cd_size);
  AppendLe32(archive, cd_offset);
  AppendLe16(archive, 0);  // comment length
  return archive;
}

std::vector<uint8_t> BuildCustomZipBytes(
    std::initializer_list<CustomZipEntry> entries) {
  return BuildCustomZipBytes(
      std::span<const CustomZipEntry>(entries.begin(), entries.size()));
}

TEST(ZiparchiveTest, OpenMissingFileFails) {
  TempFile missing("ziparchive_missing.zip");
  ScopedErrorCapture errors;

  EXPECT_EQ(nullptr, za_open(missing.c_str(), CaptureErrorHandler));
  EXPECT_THAT(errors.messages(),
              ElementsAre(AllOf(HasSubstr("could not find"),
                                HasSubstr(missing.string()))));
}

TEST(ZiparchiveTest, OpenPathThroughRegularFileFailsWithStatError) {
  TempFile regular_file("ziparchive_not_a_dir.dat");
  ASSERT_TRUE(regular_file.WriteText("x"));

  // Statting a child path under a regular file fails with ENOTDIR (!= ENOENT),
  // exercising the non-ENOENT stat() error branch.
  const std::string bad_path = regular_file.string() + "/child.zip";
  ScopedErrorCapture errors;

  EXPECT_EQ(nullptr, za_open(bad_path.c_str(), CaptureErrorHandler));
  EXPECT_THAT(errors.messages(), ElementsAre(AllOf(HasSubstr("could not open"),
                                                   HasSubstr(bad_path))));
}

TEST(ZiparchiveTest, OpenDirectoryFailsAsNotRegularFile) {
  TempDir dir("ziparchive_dir");
  ScopedErrorCapture errors;

  EXPECT_EQ(nullptr, za_open(dir.c_str(), CaptureErrorHandler));
  EXPECT_THAT(errors.messages(), ElementsAre(HasSubstr("not a regular file")));
}

TEST(ZiparchiveTest, OpenFileSmallerThanMinZipSizeFails) {
  TempFile empty_file("ziparchive_empty.zip");
  ASSERT_TRUE(empty_file.WriteText(""));

  TempFile short_pk("ziparchive_short_pk.zip");
  ASSERT_TRUE(short_pk.WriteText("PK1234567890123456789"));  // 21 bytes (< 22)

  for (const TempFile* file : {&empty_file, &short_pk}) {
    ScopedErrorCapture errors;
    EXPECT_EQ(nullptr, za_open(file->c_str(), CaptureErrorHandler));
    EXPECT_THAT(errors.messages(), ElementsAre(HasSubstr("not a zip file")));
  }
}

TEST(ZiparchiveTest, OpenUnreadableFileFailsWhenNotRoot) {
  if (::geteuid() == 0) {
    GTEST_SKIP() << "chmod(0000) does not deny open(O_RDONLY) for root";
  }
  TempFile file("ziparchive_unreadable.zip");
  ASSERT_TRUE(file.WriteBytes(BuildZipBytes({})));
  ASSERT_EQ(0, ::chmod(file.c_str(), 0000));
  const absl::Cleanup restore_perms = [&file] {
    static_cast<void>(::chmod(file.c_str(), 0600));
  };

  ScopedErrorCapture errors;
  EXPECT_EQ(nullptr, za_open(file.c_str(), CaptureErrorHandler));
  EXPECT_THAT(errors.messages(), ElementsAre(HasSubstr("could not open file")));
}

TEST(ZiparchiveTest, OpenFileWithoutPkMagicFails) {
  std::array<uint8_t, 24> bad_first_byte{};
  bad_first_byte[0] = 'X';
  bad_first_byte[1] = 'K';

  std::array<uint8_t, 24> bad_second_byte{};
  bad_second_byte[0] = 'P';
  bad_second_byte[1] = 'X';

  TempFile file("ziparchive_bad_magic.zip");
  for (const auto& bytes : {bad_first_byte, bad_second_byte}) {
    ASSERT_TRUE(file.WriteBytes(bytes));
    ScopedErrorCapture errors;
    EXPECT_EQ(nullptr, za_open(file.c_str(), CaptureErrorHandler));
    EXPECT_THAT(errors.messages(), ElementsAre(HasSubstr("not a zip file")));
  }
}

TEST(ZiparchiveTest, OpenWithNullErrorHandlerUsesDefaultHandlerOnCorruptZip) {
  // ziparchive.c only invokes zf->handle_error (default_error_handler when
  // NULL) after passing stat() and the 2-byte "PK" signature check.
  std::vector<uint8_t> corrupt(32, 0);
  corrupt[0] = 'P';
  corrupt[1] = 'K';
  TempFile file("ziparchive_null_handler_corrupt.zip");
  ASSERT_TRUE(file.WriteBytes(corrupt));

  EXPECT_EQ(nullptr, za_open(file.c_str(), nullptr));
}

TEST(ZiparchiveTest, OpenZipWithTrailingFileCommentFailsMissingEocd) {
  std::vector<uint8_t> bytes = BuildZipBytes({});
  // ziparchive.c seeks strictly -22 bytes from SEEK_END to read EOCD, so any
  // trailing archive comment hides the 0x06054B50 EOCD signature.
  WriteLe16(bytes, bytes.size() - 2, 4);
  bytes.insert(bytes.end(), {'n', 'o', 't', 'e'});

  TempFile file("ziparchive_comment.zip");
  ASSERT_TRUE(file.WriteBytes(bytes));

  ScopedErrorCapture errors;
  EXPECT_EQ(nullptr, za_open(file.c_str(), CaptureErrorHandler));
  EXPECT_THAT(errors.messages(),
              ElementsAre(HasSubstr("could not locate package index")));
}

TEST(ZiparchiveTest, OpenZipWithTruncatedCentralDirectoryFails) {
  // 1. Single entry whose EOCD cd_offset leaves fewer than 46 bytes before EOF.
  std::vector<uint8_t> single = BuildCustomZipBytes({{
      .name = "a.txt",
      .payload = ToBytes("hello"),
  }});
  const uint32_t truncated_cd_offset =
      static_cast<uint32_t>(single.size() - 30);
  WriteLe32(single, single.size() - 6, truncated_cd_offset);

  TempFile single_file("ziparchive_trunc_cd_single.zip");
  ASSERT_TRUE(single_file.WriteBytes(single));
  {
    ScopedErrorCapture errors;
    EXPECT_EQ(nullptr, za_open(single_file.c_str(), CaptureErrorHandler));
    EXPECT_THAT(errors.messages(),
                ElementsAre(HasSubstr("unexpected end in zip file")));
  }

  // 2. Two entries where entry 0 parses and allocates filename, then entry 1's
  // CD header is truncated (< 46 bytes) so za_close() frees mixed NULL/non-NULL
  // entry filenames.
  std::vector<uint8_t> two_entries = BuildCustomZipBytes({
      {.name = "first.txt", .payload = ToBytes("one")},
      {.name = "second.txt", .payload = ToBytes("two")},
  });
  // Entry 0 local = 30+9+3 = 42; entry 1 local = 30+10+3 = 43 -> CD starts
  // at 85. Entry 0 CD = 46+9 = 55 -> entry 1 CD starts at 140. Keep 10 bytes of
  // entry 1 CD plus a 22-byte EOCD (32 < 46 remaining bytes from offset 140).
  const size_t cut_at = 140 + 10;
  std::vector<uint8_t> partial_cd(two_entries.begin(),
                                  two_entries.begin() + cut_at);
  AppendLe32(partial_cd, kEocdSignature);
  AppendLe16(partial_cd, 0);
  AppendLe16(partial_cd, 0);
  AppendLe16(partial_cd, 2);  // num_entries
  AppendLe16(partial_cd, 2);
  AppendLe32(partial_cd, static_cast<uint32_t>(cut_at - 85));
  AppendLe32(partial_cd, 85);  // cd_offset
  AppendLe16(partial_cd, 0);

  TempFile two_file("ziparchive_trunc_cd_partial.zip");
  ASSERT_TRUE(two_file.WriteBytes(partial_cd));
  {
    ScopedErrorCapture errors;
    EXPECT_EQ(nullptr, za_open(two_file.c_str(), CaptureErrorHandler));
    EXPECT_THAT(errors.messages(),
                ElementsAre(HasSubstr("unexpected end in zip file")));
  }
}

TEST(ZiparchiveTest, OpenZipWithBadCentralDirectorySignatureFails) {
  const std::vector<uint8_t> bytes = BuildCustomZipBytes({{
      .name = "a.txt",
      .payload = ToBytes("hello"),
      .cd_signature = 0x02014B51U,
  }});
  TempFile file("ziparchive_bad_cd_sig.zip");
  ASSERT_TRUE(file.WriteBytes(bytes));

  ScopedErrorCapture errors;
  EXPECT_EQ(nullptr, za_open(file.c_str(), CaptureErrorHandler));
  EXPECT_THAT(errors.messages(),
              ElementsAre(HasSubstr("invalid file header signature")));
}

TEST(ZiparchiveTest, OpenZipWithUnsupportedCompressionMethodFails) {
  const std::vector<uint8_t> bytes = BuildCustomZipBytes({{
      .name = "a.txt",
      .payload = ToBytes("hello"),
      .cd_compression = 9,  // Deflate64 (unsupported; only 0 and 8 allowed)
  }});
  TempFile file("ziparchive_bad_compression.zip");
  ASSERT_TRUE(file.WriteBytes(bytes));

  ScopedErrorCapture errors;
  EXPECT_EQ(nullptr, za_open(file.c_str(), CaptureErrorHandler));
  EXPECT_THAT(errors.messages(),
              ElementsAre(HasSubstr("unsupported compression")));
}

TEST(ZiparchiveTest, OpenZipWithTruncatedCentralDirectoryFilenameFails) {
  std::vector<uint8_t> bytes = BuildCustomZipBytes({{
      .name = "a.txt",
      .payload = ToBytes("hello"),
      .cd_name_len = 100,
  }});
  // Layout: local (40) + 46-byte CD header (40..85) + 5-byte name + EOCD (22).
  // Keep local + 46-byte CD header, then append EOCD so only 22 < 100 bytes
  // remain when get_entries() reads the CD entry filename.
  std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 86);
  AppendLe32(truncated, kEocdSignature);
  AppendLe16(truncated, 0);
  AppendLe16(truncated, 0);
  AppendLe16(truncated, 1);
  AppendLe16(truncated, 1);
  AppendLe32(truncated, 46);
  AppendLe32(truncated, 40);
  AppendLe16(truncated, 0);

  TempFile file("ziparchive_trunc_cd_name.zip");
  ASSERT_TRUE(file.WriteBytes(truncated));

  ScopedErrorCapture errors;
  EXPECT_EQ(nullptr, za_open(file.c_str(), CaptureErrorHandler));
  EXPECT_THAT(errors.messages(),
              ElementsAre(HasSubstr("unexpected end in zip file")));
}

TEST(ZiparchiveTest, OpenZipWithDuplicateEntryNamesFails) {
  const std::vector<uint8_t> payload = ToBytes("data");
  const std::array<ZipEntrySpec, 2> specs = {{
      {.name = "dup.txt", .data = payload, .method = ZipMethod::kStored},
      {.name = "dup.txt", .data = payload, .method = ZipMethod::kStored},
  }};
  TempFile file("ziparchive_duplicate_name.zip");
  ASSERT_TRUE(file.WriteBytes(BuildZipBytes(specs)));

  ScopedErrorCapture errors;
  EXPECT_EQ(nullptr, za_open(file.c_str(), CaptureErrorHandler));
  EXPECT_THAT(
      errors.messages(),
      ElementsAre(AllOf(HasSubstr("contains two entries with the same name"),
                        HasSubstr("dup.txt"))));
}

TEST(ZiparchiveTest, OpenZipWithInvalidSizesAndOffsetsFails) {
  struct InvalidSizeCase {
    std::string_view label;
    uint16_t cd_compression;
    uint32_t cd_compressed_size;
    uint32_t cd_uncompressed_size;
    std::optional<uint32_t> local_offset_override;
    std::string_view expected_error;
  };
  constexpr InvalidSizeCase kCases[] = {
      {
          .label = "local_offset_past_eof",
          .cd_compression = 0,
          .cd_compressed_size = 5,
          .cd_uncompressed_size = 5,
          .local_offset_override = 1000,
          .expected_error = "local header offset exceeds file size",
      },
      {
          .label = "deflated_entry_past_eof",
          .cd_compression = 8,
          .cd_compressed_size = 100,
          .cd_uncompressed_size = 100,
          .local_offset_override = 80,
          .expected_error = "entry size exceeds file size",
      },
      {
          // ziparchive.c rejects uncompressed_size / 1032 > compressed_size + 1
          // (4128 / 1032 == 4 > 2 + 1).
          .label = "deflated_ratio_exceeds_1032",
          .cd_compression = 8,
          .cd_compressed_size = 2,
          .cd_uncompressed_size = 4128,
          .local_offset_override = std::nullopt,
          .expected_error = "invalid uncompressed size in zip file",
      },
      {
          .label = "stored_size_mismatch",
          .cd_compression = 0,
          .cd_compressed_size = 4,
          .cd_uncompressed_size = 5,
          .local_offset_override = std::nullopt,
          .expected_error = "compressed and uncompressed sizes should be equal",
      },
      {
          .label = "stored_entry_past_eof",
          .cd_compression = 0,
          .cd_compressed_size = 100,
          .cd_uncompressed_size = 100,
          .local_offset_override = 80,
          .expected_error = "entry size exceeds file size",
      },
  };

  TempFile file("ziparchive_invalid_sizes.zip");
  for (const InvalidSizeCase& tc : kCases) {
    SCOPED_TRACE(tc.label);
    const std::vector<uint8_t> bytes = BuildCustomZipBytes({{
        .name = "a.txt",
        .payload = ToBytes("hello"),
        .cd_compression = tc.cd_compression,
        .cd_compressed_size = tc.cd_compressed_size,
        .cd_uncompressed_size = tc.cd_uncompressed_size,
        .local_offset_override = tc.local_offset_override,
    }});
    ASSERT_TRUE(file.WriteBytes(bytes));

    ScopedErrorCapture errors;
    EXPECT_EQ(nullptr, za_open(file.c_str(), CaptureErrorHandler));
    EXPECT_THAT(errors.messages(), ElementsAre(HasSubstr(tc.expected_error)));
  }
}

TEST(ZiparchiveTest, OpenEmptyZipArchiveHasZeroEntries) {
  TempFile file("ziparchive_empty_valid.zip");
  ASSERT_TRUE(file.WriteBytes(BuildZipBytes({})));

  ScopedErrorCapture errors;
  ScopedZaFile zf(za_open(file.c_str(), CaptureErrorHandler));
  ASSERT_NE(nullptr, zf.get());
  EXPECT_THAT(errors.messages(), IsEmpty());

  EXPECT_EQ(0, za_get_num_entries(zf.get()));
  EXPECT_STREQ(file.c_str(), za_get_filename(zf.get()));
  EXPECT_EQ(nullptr, za_get_entry_by_index(zf.get(), -1));
  EXPECT_EQ(nullptr, za_get_entry_by_index(zf.get(), 0));
  EXPECT_EQ(nullptr, za_get_entry_by_name(zf.get(), "anything"));
}

TEST(ZiparchiveTest, OpenAndReadStoredAndDeflatedEntries) {
  const std::vector<uint8_t> stored_text = ToBytes("Hello, CODA zip!");
  const std::vector<uint8_t> deflated_text = ToBytes(
      "Repeated text for zlib compression! Repeated text for zlib compression! "
      "1234567890");
  std::vector<uint8_t> large_payload(16384);
  for (size_t i = 0; i < large_payload.size(); ++i) {
    large_payload[i] = static_cast<uint8_t>(i % 251);
  }

  const std::array<ZipEntrySpec, 4> specs = {{
      {.name = "empty.bin", .data = {}, .method = ZipMethod::kStored},
      {.name = "dir/hello.txt",
       .data = stored_text,
       .method = ZipMethod::kStored},
      {.name = "nested/compressed.txt",
       .data = deflated_text,
       .method = ZipMethod::kDeflated},
      {.name = "large.bin",
       .data = large_payload,
       .method = ZipMethod::kDeflated},
  }};

  TempFile file("ziparchive_multi_valid.zip");
  ASSERT_TRUE(file.WriteBytes(BuildZipBytes(specs)));

  ScopedErrorCapture errors;
  ScopedZaFile zf(za_open(file.c_str(), CaptureErrorHandler));
  ASSERT_NE(nullptr, zf.get());

  EXPECT_EQ(4, za_get_num_entries(zf.get()));
  EXPECT_STREQ(file.c_str(), za_get_filename(zf.get()));
  EXPECT_EQ(nullptr, za_get_entry_by_index(zf.get(), -1));
  EXPECT_EQ(nullptr, za_get_entry_by_index(zf.get(), 4));
  EXPECT_EQ(nullptr, za_get_entry_by_name(zf.get(), "missing.txt"));
  // hashtable_new(1) in za_open is case-sensitive.
  EXPECT_EQ(nullptr, za_get_entry_by_name(zf.get(), "DIR/HELLO.TXT"));

  // Read entries in reverse order and then re-read entry 1 to confirm seeks
  // are independent of prior reads.
  for (long i = 3; i >= 0; --i) {
    const ZipEntrySpec& spec = specs[static_cast<size_t>(i)];
    za_entry* by_index = za_get_entry_by_index(zf.get(), i);
    ASSERT_NE(nullptr, by_index);
    const std::string name_copy(spec.name);
    za_entry* by_name = za_get_entry_by_name(zf.get(), name_copy.c_str());
    EXPECT_EQ(by_index, by_name);
    EXPECT_EQ(spec.name, std::string_view(za_get_entry_name(by_index)));
    EXPECT_EQ(static_cast<long>(spec.data.size()), za_get_entry_size(by_index));

    std::vector<char> actual(spec.data.empty() ? 1 : spec.data.size(), '\xAA');
    ASSERT_EQ(0, za_read_entry(by_index, actual.data()));
    actual.resize(spec.data.size());
    std::vector<uint8_t> actual_bytes;
    actual_bytes.reserve(actual.size());
    for (const char ch : actual) {
      actual_bytes.push_back(static_cast<uint8_t>(ch));
    }
    EXPECT_EQ(actual_bytes,
              std::vector<uint8_t>(spec.data.begin(), spec.data.end()));
  }

  za_entry* hello_entry = za_get_entry_by_name(zf.get(), "dir/hello.txt");
  ASSERT_NE(nullptr, hello_entry);
  std::vector<char> reread(stored_text.size(), '\0');
  EXPECT_EQ(0, za_read_entry(hello_entry, reread.data()));
  EXPECT_EQ("Hello, CODA zip!", std::string_view(reread.data(), reread.size()));
  EXPECT_THAT(errors.messages(), IsEmpty());
}

TEST(ZiparchiveTest, HandlesNonZeroMetadataExtraFieldsAndComments) {
  const std::vector<uint8_t> first_payload = ToBytes("first-payload");
  const std::vector<uint8_t> second_payload = ToBytes("second-payload");
  const std::array<CustomZipEntry, 2> entries = {{
      {
          .name = "first.txt",
          .payload = first_payload,
          .local_compression = 0,
          .cd_compression = 0,
          .local_mod_time = 0x1234,
          .cd_mod_time = 0x1234,
          .local_mod_date = 0x5678,
          .cd_mod_date = 0x5678,
          .local_crc = 0xAABBCCDDU,
          .cd_crc = 0xAABBCCDDU,
          // Local and CD extrafield lengths are explicitly permitted to differ.
          .local_extra = {0x01, 0x02, 0x03},
          .cd_extra = {0x10, 0x20, 0x30, 0x40, 0x50},
          .cd_comment = ToBytes("comment"),
          .internal_attr = 0x0001,  // ascii flag bit
          .external_attr = 0x81A40000U,
      },
      {
          .name = "second.txt",
          .payload = second_payload,
      },
  }};

  TempFile file("ziparchive_metadata_extra.zip");
  ASSERT_TRUE(file.WriteBytes(BuildCustomZipBytes(entries)));

  ScopedErrorCapture errors;
  ScopedZaFile zf(za_open(file.c_str(), CaptureErrorHandler));
  ASSERT_NE(nullptr, zf.get());
  ASSERT_EQ(2, za_get_num_entries(zf.get()));

  constexpr std::pair<const char*, std::string_view> kExpectedEntries[] = {
      {"first.txt", "first-payload"},
      {"second.txt", "second-payload"},
  };
  for (const auto& [name, expected] : kExpectedEntries) {
    za_entry* entry = za_get_entry_by_name(zf.get(), name);
    ASSERT_NE(nullptr, entry);
    std::vector<char> buf(expected.size(), '\0');
    ASSERT_EQ(0, za_read_entry(entry, buf.data()));
    EXPECT_EQ(expected, std::string_view(buf.data(), buf.size()));
  }
  EXPECT_THAT(errors.messages(), IsEmpty());
}

TEST(ZiparchiveTest, OpenValidZipWithNullErrorHandlerSucceeds) {
  const std::vector<uint8_t> payload = ToBytes("valid-with-null-handler");
  const std::array<ZipEntrySpec, 1> specs = {{
      {.name = "item.txt", .data = payload, .method = ZipMethod::kDeflated},
  }};
  TempFile file("ziparchive_valid_null_handler.zip");
  ASSERT_TRUE(file.WriteBytes(BuildZipBytes(specs)));

  ScopedZaFile zf(za_open(file.c_str(), nullptr));
  ASSERT_NE(nullptr, zf.get());
  za_entry* entry = za_get_entry_by_index(zf.get(), 0);
  ASSERT_NE(nullptr, entry);
  std::vector<char> buf(payload.size(), '\0');
  EXPECT_EQ(0, za_read_entry(entry, buf.data()));
  EXPECT_EQ("valid-with-null-handler",
            std::string_view(buf.data(), buf.size()));
}

TEST(ZiparchiveTest, ReadEntryTruncatedLocalHeaderFails) {
  // Set localheader_offset within 20 (< 30) bytes of EOF with size 0 so
  // get_entries() accepts localheader_offset + 0 <= file_size, and
  // za_read_entry() hits short read on the 30-byte local file header.
  std::vector<uint8_t> bytes = BuildCustomZipBytes({{
      .name = "a.txt",
      .payload = {},
  }});
  const uint32_t near_eof_offset = static_cast<uint32_t>(bytes.size() - 20);
  bytes = BuildCustomZipBytes({{
      .name = "a.txt",
      .payload = {},
      .local_offset_override = near_eof_offset,
  }});

  TempFile file("ziparchive_trunc_local_hdr.zip");
  ASSERT_TRUE(file.WriteBytes(bytes));

  ScopedErrorCapture errors;
  ScopedZaFile zf(za_open(file.c_str(), CaptureErrorHandler));
  ASSERT_NE(nullptr, zf.get());
  za_entry* entry = za_get_entry_by_index(zf.get(), 0);
  ASSERT_NE(nullptr, entry);

  char dummy[1]{};
  EXPECT_EQ(-1, za_read_entry(entry, dummy));
  EXPECT_THAT(errors.messages(),
              ElementsAre(HasSubstr("unexpected end in zip file")));
}

TEST(ZiparchiveTest, ReadEntryDetectsLocalHeaderInconsistencies) {
  struct MismatchCase {
    std::string_view label;
    CustomZipEntry entry;
    std::string_view expected_error;
  };
  const std::vector<uint8_t> payload = ToBytes("hello");
  const std::vector<MismatchCase> cases = {
      {
          .label = "local_signature",
          .entry =
              {
                  .name = "a.txt",
                  .payload = payload,
                  // Keep leading "PK" bytes so za_open() still accepts magic.
                  .local_signature = 0x99994B50U,
              },
          .expected_error = "local header has incorrect signature",
      },
      {
          .label = "compression",
          .entry =
              {
                  .name = "a.txt",
                  .payload = payload,
                  .local_compression = 8,
                  .cd_compression = 0,
              },
          .expected_error = "(compression)",
      },
      {
          .label = "modification_time",
          .entry =
              {
                  .name = "a.txt",
                  .payload = payload,
                  .local_mod_time = 0x1111,
                  .cd_mod_time = 0x2222,
              },
          .expected_error = "(modification_time)",
      },
      {
          .label = "modification_date",
          .entry =
              {
                  .name = "a.txt",
                  .payload = payload,
                  .local_mod_date = 0x1111,
                  .cd_mod_date = 0x2222,
              },
          .expected_error = "(modification_date)",
      },
      {
          .label = "crc",
          .entry =
              {
                  .name = "a.txt",
                  .payload = payload,
                  .local_crc = 0x11111111U,
                  .cd_crc = 0x22222222U,
              },
          .expected_error = "(crc)",
      },
      {
          .label = "compressed_size",
          .entry =
              {
                  .name = "a.txt",
                  .payload = payload,
                  .local_compressed_size = 4,
                  .cd_compressed_size = 5,
              },
          .expected_error = "(compressed_size)",
      },
      {
          .label = "uncompressed_size",
          .entry =
              {
                  .name = "a.txt",
                  .payload = payload,
                  .local_uncompressed_size = 4,
                  .cd_uncompressed_size = 5,
              },
          .expected_error = "(uncompressed_size)",
      },
      {
          .label = "filename_length",
          .entry =
              {
                  .name = "a.txt",
                  .payload = payload,
                  .local_name_len = 6,
                  .cd_name_len = 5,
              },
          .expected_error = "(filename_length)",
      },
  };

  TempFile file("ziparchive_local_cd_mismatch.zip");
  for (const MismatchCase& tc : cases) {
    SCOPED_TRACE(tc.label);
    ASSERT_TRUE(file.WriteBytes(BuildCustomZipBytes({tc.entry})));

    ScopedErrorCapture errors;
    ScopedZaFile zf(za_open(file.c_str(), CaptureErrorHandler));
    ASSERT_NE(nullptr, zf.get());
    za_entry* entry = za_get_entry_by_index(zf.get(), 0);
    ASSERT_NE(nullptr, entry);

    std::vector<char> buf(payload.size(), '\0');
    EXPECT_EQ(-1, za_read_entry(entry, buf.data()));
    EXPECT_THAT(errors.messages(), ElementsAre(HasSubstr(tc.expected_error)));
  }
}

TEST(ZiparchiveTest, ReadStoredAndDeflatedEntriesWithTruncatedPayloadFail) {
  for (const uint16_t method : {uint16_t{0}, uint16_t{8}}) {
    SCOPED_TRACE(method);
    std::vector<uint8_t> bytes = BuildCustomZipBytes({{
        .name = "a.txt",
        .payload = ToBytes("0123456789"),
        .local_compression = method,
        .cd_compression = method,
    }});
    // Inflate ONLY the local header's extrafield_length (offset 28) beyond
    // file_size so get_entries() passes (0 + 10 <= file_size) while
    // za_read_entry() seeks past EOF before reading the payload.
    WriteLe16(bytes, 28, 200);

    TempFile file("ziparchive_trunc_payload.zip");
    ASSERT_TRUE(file.WriteBytes(bytes));

    ScopedErrorCapture errors;
    ScopedZaFile zf(za_open(file.c_str(), CaptureErrorHandler));
    ASSERT_NE(nullptr, zf.get());
    za_entry* entry = za_get_entry_by_index(zf.get(), 0);
    ASSERT_NE(nullptr, entry);

    std::vector<char> buf(10, '\0');
    EXPECT_EQ(-1, za_read_entry(entry, buf.data()));
    EXPECT_THAT(errors.messages(),
                ElementsAre(HasSubstr("unexpected end in zip file")));
  }
}

TEST(ZiparchiveTest, ReadDeflatedEntryCorruptStreamFailsWithDataError) {
  const std::vector<uint8_t> bytes = BuildCustomZipBytes({{
      .name = "corrupt.bin",
      .payload = {0xFF, 0xFF, 0xFF, 0xFF},
      .local_compression = 8,
      .cd_compression = 8,
      .local_compressed_size = 4,
      .cd_compressed_size = 4,
      .local_uncompressed_size = 10,
      .cd_uncompressed_size = 10,
  }});
  TempFile file("ziparchive_corrupt_deflate.zip");
  ASSERT_TRUE(file.WriteBytes(bytes));

  ScopedErrorCapture errors;
  ScopedZaFile zf(za_open(file.c_str(), CaptureErrorHandler));
  ASSERT_NE(nullptr, zf.get());
  za_entry* entry = za_get_entry_by_index(zf.get(), 0);
  ASSERT_NE(nullptr, entry);

  std::vector<char> buf(10, '\0');
  EXPECT_EQ(-1, za_read_entry(entry, buf.data()));
  EXPECT_THAT(errors.messages(),
              ElementsAre(HasSubstr(
                  "invalid or incomplete deflate data for zip entry")));
}

TEST(ZiparchiveTest, ReadDeflatedEntrySizeMismatchFails) {
  const std::vector<uint8_t> raw_hello = ToBytes("hello");
  const std::vector<uint8_t> deflated_hello = DeflateRaw(raw_hello);
  ASSERT_FALSE(deflated_hello.empty());

  std::vector<uint8_t> deflated_with_trailing = deflated_hello;
  deflated_with_trailing.push_back(0x00);
  deflated_with_trailing.push_back(0x00);

  // Use an incompressible 2048-byte stream for the truncated-output subcase so
  // inflate() cannot consume the entire compressed stream while filling a
  // 5-byte out_buffer (guaranteeing zs.avail_in > 0).
  std::vector<uint8_t> raw_incompressible(2048);
  uint32_t lcg = 0x12345678U;
  for (uint8_t& b : raw_incompressible) {
    lcg = lcg * 1664525U + 1013904223U;
    b = static_cast<uint8_t>(lcg >> 24);
  }
  const std::vector<uint8_t> deflated_long = DeflateRaw(raw_incompressible);
  ASSERT_GT(deflated_long.size(), 512U);

  struct SizeMismatchCase {
    std::string_view label;
    std::vector<uint8_t> payload;
    uint32_t declared_uncompressed;
  };
  const std::vector<SizeMismatchCase> cases = {
      // inflate() reaches Z_STREAM_END with avail_out > 0.
      {.label = "avail_out_nonzero",
       .payload = deflated_hello,
       .declared_uncompressed = 10},
      // inflate() reaches Z_STREAM_END with trailing compressed bytes left.
      {.label = "avail_in_nonzero_trailing_bytes",
       .payload = deflated_with_trailing,
       .declared_uncompressed = static_cast<uint32_t>(raw_hello.size())},
      // inflate() fills out_buffer before reaching end of deflate stream.
      {.label = "avail_in_nonzero_truncated_output",
       .payload = deflated_long,
       .declared_uncompressed = 5},
  };

  TempFile file("ziparchive_deflate_size_mismatch.zip");
  for (const SizeMismatchCase& tc : cases) {
    SCOPED_TRACE(tc.label);
    const std::vector<uint8_t> bytes = BuildCustomZipBytes({{
        .name = "badsize.bin",
        .payload = tc.payload,
        .local_compression = 8,
        .cd_compression = 8,
        .local_compressed_size = static_cast<uint32_t>(tc.payload.size()),
        .cd_compressed_size = static_cast<uint32_t>(tc.payload.size()),
        .local_uncompressed_size = tc.declared_uncompressed,
        .cd_uncompressed_size = tc.declared_uncompressed,
    }});
    ASSERT_TRUE(file.WriteBytes(bytes));

    ScopedErrorCapture errors;
    ScopedZaFile zf(za_open(file.c_str(), CaptureErrorHandler));
    ASSERT_NE(nullptr, zf.get());
    za_entry* entry = za_get_entry_by_index(zf.get(), 0);
    ASSERT_NE(nullptr, entry);

    std::vector<char> buf(tc.declared_uncompressed, '\0');
    EXPECT_EQ(-1, za_read_entry(entry, buf.data()));
    EXPECT_THAT(errors.messages(),
                ElementsAre(HasSubstr("invalid compressed/uncompressed size")));
  }
}

// TODO(schwehr): 83.3% reachable ceiling in libcoda/ziparchive.c (279/335);
// all functions are covered and remaining uncovered lines
// are defensive OOM (malloc/strdup/Z_MEM_ERROR) and POSIX read()/lseek() I/O
// failure paths that require fault injection on regular files.

}  // namespace
}  // namespace stcorp_coda
