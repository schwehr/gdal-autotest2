// Unit tests for third_party/kakadu/vendor/apps/image/kdu_tiff.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/apps/image/kdu_tiff.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_elementary.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"

namespace kdu_supp {
namespace {

using ::kdu_core::kdu_byte;
using ::kdu_core::kdu_compressed_source;
using ::kdu_core::kdu_compressed_target;
using ::kdu_core::kdu_exception;
using ::kdu_core::kdu_int16;
using ::kdu_core::kdu_int32;
using ::kdu_core::kdu_long;
using ::kdu_core::kdu_uint16;
using ::kdu_core::kdu_uint32;

constexpr kdu_uint32 MakeTagType(kdu_uint16 tag, kdu_uint16 type) {
  return (static_cast<kdu_uint32>(tag) << 16) | type;
}

class ThrowingErrorHandler final : public ::kdu_core::kdu_thread_safe_message {
 public:
  void put_text(const char *text) override { message_ += text; }
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      message_.clear();
      ::kdu_core::kdu_thread_safe_message::flush(end_of_message);
      throw KDU_ERROR_EXCEPTION;
    }
    ::kdu_core::kdu_thread_safe_message::flush(end_of_message);
  }

 private:
  std::string message_;
};

class TiffTestEnv final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto *const handler = new ThrowingErrorHandler();
    ::kdu_core::kdu_customize_errors(handler);
  }
};

const ::testing::Environment *const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new TiffTestEnv());

class MemoryTarget final : public kdu_compressed_target {
 public:
  explicit MemoryTarget(int fail_after_writes = -1)
      : fail_after_writes_(fail_after_writes) {}

  bool write(const kdu_byte *buf, int num_bytes) override {
    if (fail_after_writes_ >= 0 && write_count_ >= fail_after_writes_) {
      return false;
    }
    ++write_count_;
    data_.insert(data_.end(), buf, buf + num_bytes);
    return true;
  }

  const std::vector<kdu_byte> &data() const { return data_; }

 private:
  int fail_after_writes_;
  int write_count_ = 0;
  std::vector<kdu_byte> data_;
};

class MemorySource final : public kdu_compressed_source {
 public:
  explicit MemorySource(const std::vector<kdu_byte> &data,
                        int capabilities = KDU_SOURCE_CAP_SEQUENTIAL |
                                           KDU_SOURCE_CAP_SEEKABLE,
                        int fail_seek_at_call = -1)
      : data_(data),
        capabilities_(capabilities),
        fail_seek_at_call_(fail_seek_at_call) {}

  int get_capabilities() override { return capabilities_; }

  int read(kdu_byte *buf, int num_bytes) override {
    int avail = static_cast<int>(data_.size()) - pos_;
    int n = std::min(avail, num_bytes);
    if (n > 0) {
      std::memcpy(buf, data_.data() + pos_, n);
      pos_ += n;
    }
    return n;
  }

  bool seek(kdu_long offset) override {
    if (fail_seek_at_call_ >= 0 && seek_calls_ == fail_seek_at_call_) {
      ++seek_calls_;
      return false;
    }
    ++seek_calls_;
    if (offset < 0 || offset > static_cast<kdu_long>(data_.size())) {
      return false;
    }
    pos_ = static_cast<int>(offset);
    return true;
  }

  kdu_long get_pos() override { return pos_; }

 private:
  const std::vector<kdu_byte> &data_;
  int capabilities_;
  int fail_seek_at_call_;
  int seek_calls_ = 0;
  int pos_ = 0;
};

void WriteTagTypedAllVariants(kdu_tiffdir *dir, bool bigtiff) {
  // Unsorted tag codes exercise writedir's bubble sort (head & inner swaps).
  dir->create_tag(MakeTagType(0x0300, 3));  // Empty tag: skipped in writedir.

  const kdu_uint32 tag_byte = MakeTagType(0x0210, 1);
  const kdu_byte bytes_part1[3] = {10, 20, 30};
  const kdu_byte bytes_part2[7] = {40, 50, 60, 70, 80, 90, 100};
  dir->write_tag(tag_byte, 3, bytes_part1);
  dir->write_tag(tag_byte, 7, bytes_part2);  // Triggers buffer reallocation.

  const char ascii[] = "KakaduTIFF";
  dir->write_tag(KDU_TIFFTAG_Software, static_cast<int>(sizeof(ascii)),
                 reinterpret_cast<const kdu_byte *>(ascii));

  // Inline and out-of-line 16-bit unsigned/signed tags.
  dir->write_tag(KDU_TIFFTAG_SamplesPerPixel, static_cast<kdu_uint16>(3));
  const kdu_uint16 u16_arr[6] = {10, 200, 3000, 40000, 50000, 65535};
  dir->write_tag(MakeTagType(0x0211, 3), 6, u16_arr);

  const kdu_uint16 s16_inline = static_cast<kdu_uint16>(kdu_int16{-1234});
  dir->write_tag(MakeTagType(0x0212, 8), s16_inline);
  const kdu_long s16_long_vals[5] = {-30000, -1, 0, 123, 32767};
  dir->write_tag(MakeTagType(0x0213, 8), 5, s16_long_vals);

  // 32-bit unsigned/signed tags.
  dir->write_tag(KDU_TIFFTAG_ImageWidth32, static_cast<kdu_uint32>(1920));
  const kdu_uint32 u32_arr[3] = {0x12345678u, 0x9ABCDEF0u, 42u};
  dir->write_tag(MakeTagType(0x0214, 4), 3, u32_arr);

  const kdu_long s32_long_single = -987654;
  dir->write_tag(MakeTagType(0x0215, 9), s32_long_single);
  const kdu_long s32_long_arr[3] = {-1000000, 0, 1000000};
  dir->write_tag(MakeTagType(0x0216, 9), 3, s32_long_arr);

  // Float (11), double (12), unsigned rational (5), signed rational (10).
  const double floats[3] = {1.5, -2.25, 0.0};
  dir->write_tag(KDU_TIFFTAG_SminSampleValueF, 3, floats);
  const double float_single = 3.25;
  dir->write_tag(MakeTagType(0x0217, 11), 1, &float_single);

  const double dbl_single = -123.456789;
  dir->write_tag(KDU_TIFFTAG_SmaxSampleValueD, 1, &dbl_single);
  const double dbl_arr[2] = {0.125, 9876.54321};
  dir->write_tag(MakeTagType(0x0218, 12), 2, dbl_arr);

  const double urat_vals[4] = {-1.0, 0.0, 2.5, 1.0 / 3.0};
  dir->write_tag(KDU_TIFFTAG_XResolution, 4, urat_vals);

  const double srat_vals[4] = {-2.5, -1.0 / 3.0, 0.0, 3.75};
  dir->write_tag(MakeTagType(0x0219, 10), 4, srat_vals);

  if (bigtiff) {
    dir->write_tag(KDU_TIFFTAG_StripOffsets64,
                   static_cast<kdu_long>(0x0102030405060708LL));
    const kdu_long u64_arr[2] = {10000000000LL, 20000000000LL};
    dir->write_tag(KDU_TIFFTAG_StripByteCounts64, 2, u64_arr);
    const kdu_long s64_single = -0x0011223344556677LL;
    dir->write_tag(MakeTagType(0x0220, 17), 1, &s64_single);
  }
}

void VerifyTagValues(kdu_tiffdir *dir, bool bigtiff,
                     bool expect_empty_tag_present, bool include_sub_thresh) {
  if (expect_empty_tag_present) {
    EXPECT_EQ(dir->open_tag(MakeTagType(0x0300, 0)), MakeTagType(0x0300, 3));
  } else {
    EXPECT_EQ(dir->open_tag(MakeTagType(0x0300, 0)), 0u);  // Empty tag dropped.
  }

  // Wildcard type lookup and cumulative partial reads.
  EXPECT_EQ(dir->open_tag(MakeTagType(0x0210, 0)), MakeTagType(0x0210, 1));
  EXPECT_EQ(dir->get_taglength(MakeTagType(0x0210, 1)), 10);
  kdu_byte byte_part[6] = {};
  EXPECT_EQ(dir->read_tag(MakeTagType(0x0210, 1), 6, byte_part), 6);
  EXPECT_EQ(byte_part[0], 10);
  EXPECT_EQ(byte_part[5], 60);
  kdu_byte byte_rest[10] = {};
  EXPECT_EQ(dir->read_tag(MakeTagType(0x0210, 1), 10, byte_rest), 4);
  EXPECT_EQ(byte_rest[0], 70);
  EXPECT_EQ(byte_rest[3], 100);
  EXPECT_EQ(dir->read_tag(MakeTagType(0x0210, 1), 4, byte_rest), 0);

  // Re-open resets read pointer.
  EXPECT_EQ(dir->open_tag(KDU_TIFFTAG_Software), KDU_TIFFTAG_Software);
  char ascii[16] = {};
  EXPECT_EQ(dir->read_tag(KDU_TIFFTAG_Software, 16,
                          reinterpret_cast<kdu_byte *>(ascii)),
            11);
  EXPECT_STREQ(ascii, "KakaduTIFF");

  // 16-bit read via uint16, uint32, and kdu_long.
  if (include_sub_thresh) {
    kdu_uint16 spp = 0;
    EXPECT_EQ(dir->read_tag(KDU_TIFFTAG_SamplesPerPixel, 1, &spp), 1);
    EXPECT_EQ(spp, 3);
    EXPECT_EQ(dir->read_tag(KDU_TIFFTAG_SamplesPerPixel, 1, &spp), 0);
  }

  kdu_uint32 u16_as_u32[6] = {};
  EXPECT_EQ(dir->read_tag(MakeTagType(0x0211, 3), 6, u16_as_u32), 6);
  EXPECT_EQ(u16_as_u32[0], 10u);
  EXPECT_EQ(u16_as_u32[5], 65535u);

  EXPECT_EQ(dir->open_tag(MakeTagType(0x0211, 3)), MakeTagType(0x0211, 3));
  kdu_long u16_as_long[6] = {};
  EXPECT_EQ(dir->read_tag(MakeTagType(0x0211, 3), 6, u16_as_long), 6);
  EXPECT_EQ(u16_as_long[4], 50000);

  // Signed 16-bit expansion to int32 and int64.
  if (include_sub_thresh) {
    kdu_uint32 s16_as_u32 = 0;
    EXPECT_EQ(dir->read_tag(MakeTagType(0x0212, 8), 1, &s16_as_u32), 1);
    EXPECT_EQ(static_cast<kdu_int32>(s16_as_u32), -1234);
  }

  kdu_long s16_as_long[5] = {};
  EXPECT_EQ(dir->read_tag(MakeTagType(0x0213, 8), 5, s16_as_long), 5);
  EXPECT_EQ(s16_as_long[0], -30000);
  EXPECT_EQ(s16_as_long[1], -1);
  EXPECT_EQ(s16_as_long[4], 32767);

  // 32-bit reads and 64-bit expansion.
  if (include_sub_thresh || !bigtiff) {
    kdu_uint32 width = 0;
    EXPECT_EQ(dir->read_tag(KDU_TIFFTAG_ImageWidth32, 1, &width), 1);
    EXPECT_EQ(width, 1920u);
    EXPECT_EQ(dir->read_tag(KDU_TIFFTAG_ImageWidth32, 1, &width), 0);
  }

  kdu_long u32_as_long[3] = {};
  EXPECT_EQ(dir->read_tag(MakeTagType(0x0214, 4), 3, u32_as_long), 3);
  EXPECT_EQ(u32_as_long[0], 0x12345678LL);
  EXPECT_EQ(u32_as_long[2], 42);

  if (include_sub_thresh || !bigtiff) {
    kdu_long s32_single = 0;
    EXPECT_EQ(dir->read_tag(MakeTagType(0x0215, 9), 1, &s32_single), 1);
    EXPECT_EQ(s32_single, -987654);
  }

  kdu_long s32_arr[3] = {};
  EXPECT_EQ(dir->read_tag(MakeTagType(0x0216, 9), 3, s32_arr), 3);
  EXPECT_EQ(s32_arr[0], -1000000);
  EXPECT_EQ(s32_arr[2], 1000000);

  // Floating point and rationals.
  double fvals[3] = {};
  EXPECT_EQ(dir->read_tag(KDU_TIFFTAG_SminSampleValueF, 3, fvals), 3);
  EXPECT_DOUBLE_EQ(fvals[0], 1.5);
  EXPECT_DOUBLE_EQ(fvals[1], -2.25);
  EXPECT_DOUBLE_EQ(fvals[2], 0.0);

  if (include_sub_thresh || !bigtiff) {
    double fsingle = 0.0;
    EXPECT_EQ(dir->read_tag(MakeTagType(0x0217, 11), 1, &fsingle), 1);
    EXPECT_DOUBLE_EQ(fsingle, 3.25);
    EXPECT_EQ(dir->read_tag(MakeTagType(0x0217, 11), 1, &fsingle), 0);
  }

  double dsingle = 0.0;
  EXPECT_EQ(dir->read_tag(KDU_TIFFTAG_SmaxSampleValueD, 1, &dsingle), 1);
  EXPECT_DOUBLE_EQ(dsingle, -123.456789);

  double darr[2] = {};
  EXPECT_EQ(dir->read_tag(MakeTagType(0x0218, 12), 2, darr), 2);
  EXPECT_DOUBLE_EQ(darr[0], 0.125);
  EXPECT_DOUBLE_EQ(darr[1], 9876.54321);

  double urat[4] = {};
  EXPECT_EQ(dir->read_tag(KDU_TIFFTAG_XResolution, 4, urat), 4);
  EXPECT_DOUBLE_EQ(urat[0], 0.0);
  EXPECT_DOUBLE_EQ(urat[1], 0.0);
  EXPECT_DOUBLE_EQ(urat[2], 2.5);
  EXPECT_NEAR(urat[3], 1.0 / 3.0, 1e-8);

  double srat[4] = {};
  EXPECT_EQ(dir->read_tag(MakeTagType(0x0219, 10), 4, srat), 4);
  EXPECT_DOUBLE_EQ(srat[0], -2.5);
  EXPECT_NEAR(srat[1], -1.0 / 3.0, 1e-8);
  EXPECT_DOUBLE_EQ(srat[2], 0.0);
  EXPECT_DOUBLE_EQ(srat[3], 3.75);

  if (bigtiff) {
    kdu_long off64 = 0;
    EXPECT_EQ(dir->read_tag(KDU_TIFFTAG_StripOffsets64, 1, &off64), 1);
    EXPECT_EQ(off64, 0x0102030405060708LL);
    EXPECT_EQ(dir->read_tag(KDU_TIFFTAG_StripOffsets64, 1, &off64), 0);

    kdu_long bc64[2] = {};
    EXPECT_EQ(dir->read_tag(KDU_TIFFTAG_StripByteCounts64, 2, bc64), 2);
    EXPECT_EQ(bc64[0], 10000000000LL);
    EXPECT_EQ(bc64[1], 20000000000LL);

    kdu_long s64 = 0;
    EXPECT_EQ(dir->read_tag(MakeTagType(0x0220, 17), 1, &s64), 1);
    EXPECT_EQ(s64, -0x0011223344556677LL);
  }
}

TEST(KduTiffTest, UnopenedStateAndFieldLengths) {
  kdu_tiffdir dir;
  EXPECT_FALSE(dir.exists());
  EXPECT_TRUE(!dir);
  EXPECT_TRUE(dir.is_native_littlendian());
  EXPECT_EQ(dir.get_dirlength(), 0);
  MemoryTarget tgt;
  EXPECT_EQ(dir.write_header(&tgt, 8), 0);
  EXPECT_FALSE(dir.writedir(&tgt, 8));
  EXPECT_FALSE(dir.delete_tag(KDU_TIFFTAG_ImageWidth32));
  EXPECT_EQ(dir.open_tag(KDU_TIFFTAG_ImageWidth32), 0u);
  EXPECT_EQ(dir.get_taglength(KDU_TIFFTAG_ImageWidth32), 0);

  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 1)), 1);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 2)), 1);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 3)), 2);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 4)), 4);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 5)), 8);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 6)), 1);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 7)), 1);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 8)), 2);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 9)), 4);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 10)), 8);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 11)), 4);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 12)), 8);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 16)), 8);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 17)), 8);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 0)), 0);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 13)), 0);
  EXPECT_EQ(dir.get_fieldlength(MakeTagType(0x0100, 99)), 0);
}

TEST(KduTiffTest, RoundTripLittleAndBigEndianRegularAndBigTiff) {
  for (bool littlendian : {true, false}) {
    for (bool bigtiff : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "littlendian=" << littlendian << " bigtiff=" << bigtiff);
      kdu_tiffdir out_dir;
      out_dir.init(littlendian, bigtiff);
      ASSERT_TRUE(out_dir.exists());
      EXPECT_EQ(out_dir.is_littlendian(), littlendian);

      WriteTagTypedAllVariants(&out_dir, bigtiff);

      // Verify reads directly from locally written out_buf tags too.
      VerifyTagValues(&out_dir, bigtiff, /*expect_empty_tag_present=*/true,
                      /*include_sub_thresh=*/true);

      // Avoid writedir's unpadded out_buf copy below threshold (2B/4B on 8B
      // BigTIFF) so ASan/MSan remain clean across round-trip serialization.
      EXPECT_TRUE(out_dir.delete_tag(KDU_TIFFTAG_SamplesPerPixel));
      EXPECT_TRUE(out_dir.delete_tag(MakeTagType(0x0212, 8)));
      if (bigtiff) {
        EXPECT_TRUE(out_dir.delete_tag(KDU_TIFFTAG_ImageWidth32));
        EXPECT_TRUE(out_dir.delete_tag(MakeTagType(0x0215, 9)));
        EXPECT_TRUE(out_dir.delete_tag(MakeTagType(0x0217, 11)));
      }

      const int header_len = bigtiff ? 16 : 8;
      const kdu_long expected_dirlen = out_dir.get_dirlength();
      EXPECT_GT(expected_dirlen, header_len);

      MemoryTarget tgt;
      EXPECT_EQ(out_dir.write_header(&tgt, header_len), header_len);
      ASSERT_TRUE(out_dir.writedir(&tgt, header_len));
      EXPECT_EQ(static_cast<kdu_long>(tgt.data().size()),
                header_len + expected_dirlen);

      // Open back both at offset 0 and with a non-zero global stream offset.
      MemorySource src(tgt.data());
      kdu_tiffdir in_dir;
      ASSERT_TRUE(in_dir.opendir(&src));
      EXPECT_TRUE(in_dir.exists());
      EXPECT_EQ(in_dir.is_littlendian(), littlendian);
      VerifyTagValues(&in_dir, bigtiff, /*expect_empty_tag_present=*/false,
                      /*include_sub_thresh=*/false);

      std::vector<kdu_byte> prefixed(24, 0xAA);
      prefixed.insert(prefixed.end(), tgt.data().begin(), tgt.data().end());
      MemorySource prefixed_src(prefixed);
      ASSERT_TRUE(prefixed_src.seek(24));
      kdu_tiffdir offset_dir;
      ASSERT_TRUE(offset_dir.opendir(&prefixed_src));
      VerifyTagValues(&offset_dir, bigtiff, /*expect_empty_tag_present=*/false,
                      /*include_sub_thresh=*/false);
    }
  }
}

TEST(KduTiffTest, EditDeleteOverwriteCopyAndLargeTagPassthrough) {
  // Build source TIFF with > 64 KiB tag, plus various tags for overwrite/copy.
  kdu_tiffdir src_out;
  src_out.init(/*littlendian=*/true, /*bigtiff=*/false);
  const kdu_uint32 tag_large = MakeTagType(0x0400, 7);
  std::vector<kdu_byte> large_data(70000);
  for (size_t i = 0; i < large_data.size(); ++i) {
    large_data[i] = static_cast<kdu_byte>((i * 13 + 7) & 0xFF);
  }
  src_out.write_tag(tag_large, static_cast<int>(large_data.size()),
                    large_data.data());
  const kdu_uint32 tag_sub = MakeTagType(0x0277, 4);
  src_out.write_tag(tag_sub, static_cast<kdu_uint32>(4));
  src_out.write_tag(KDU_TIFFTAG_ImageWidth32, static_cast<kdu_uint32>(640));
  src_out.write_tag(KDU_TIFFTAG_ImageHeight32, static_cast<kdu_uint32>(480));
  const kdu_uint32 untouched_inline_tag = MakeTagType(0x0403, 4);
  src_out.write_tag(untouched_inline_tag, static_cast<kdu_uint32>(99));
  const kdu_uint16 u16_vals[4] = {100, 200, 300, 400};
  src_out.write_tag(KDU_TIFFTAG_BitsPerSample, 4, u16_vals);
  const double dbl_vals[2] = {1.25, -3.5};
  src_out.write_tag(KDU_TIFFTAG_SmaxSampleValueD, 2, dbl_vals);
  const double urat = 72.0;
  src_out.write_tag(KDU_TIFFTAG_XResolution, 1, &urat);
  const double srat = -12.5;
  src_out.write_tag(MakeTagType(0x0401, 10), 1, &srat);
  std::vector<kdu_byte> big_bytes(350, 0x5C);
  src_out.write_tag(MakeTagType(0x0402, 1), static_cast<int>(big_bytes.size()),
                    big_bytes.data());

  MemoryTarget src_tgt;
  ASSERT_EQ(src_out.write_header(&src_tgt, 8), 8);
  ASSERT_TRUE(src_out.writedir(&src_tgt, 8));

  // Open via opendir, then overwrite one tag, delete two (head + inner), and
  // pass through the 70000-byte tag and untouched inline tag.
  MemorySource src_in(src_tgt.data());
  kdu_tiffdir edit_dir;
  ASSERT_TRUE(edit_dir.opendir(&src_in));
  EXPECT_TRUE(edit_dir.delete_tag(tag_sub));
  EXPECT_FALSE(edit_dir.delete_tag(tag_sub));
  EXPECT_TRUE(edit_dir.delete_tag(KDU_TIFFTAG_ImageHeight32));
  edit_dir.write_tag(KDU_TIFFTAG_ImageWidth32, static_cast<kdu_uint32>(800));

  MemoryTarget edited_tgt;
  ASSERT_EQ(edit_dir.write_header(&edited_tgt, 8), 8);
  ASSERT_TRUE(edit_dir.writedir(&edited_tgt, 8));

  MemorySource edited_src(edited_tgt.data());
  kdu_tiffdir verify_edit;
  ASSERT_TRUE(verify_edit.opendir(&edited_src));
  EXPECT_EQ(verify_edit.open_tag(tag_sub), 0u);
  EXPECT_EQ(verify_edit.open_tag(KDU_TIFFTAG_ImageHeight32), 0u);
  kdu_uint32 kept_val = 0;
  EXPECT_EQ(verify_edit.read_tag(untouched_inline_tag, 1, &kept_val), 1);
  EXPECT_EQ(kept_val, 99u);
  kdu_uint32 new_width = 0;
  EXPECT_EQ(verify_edit.read_tag(KDU_TIFFTAG_ImageWidth32, 1, &new_width), 1);
  EXPECT_EQ(new_width, 800u);
  std::vector<kdu_byte> read_large(large_data.size());
  EXPECT_EQ(verify_edit.read_tag(tag_large, large_data.size(),
                                 read_large.data()),
            static_cast<kdu_long>(large_data.size()));
  EXPECT_EQ(read_large, large_data);

  // Copy tags across opposite endianness (little -> big).
  kdu_tiffdir be_copy;
  be_copy.init(/*littlendian=*/false, /*bigtiff=*/false);
  be_copy.copy_tag(verify_edit, MakeTagType(0x0999, 1));  // Missing tag: no-op.
  be_copy.copy_tag(verify_edit, MakeTagType(0x0402, 1));  // Multi-chunk bytes.
  be_copy.copy_tag(verify_edit, KDU_TIFFTAG_BitsPerSample);
  be_copy.copy_tag(verify_edit, KDU_TIFFTAG_ImageWidth32);
  be_copy.copy_tag(verify_edit, KDU_TIFFTAG_SmaxSampleValueD);
  be_copy.copy_tag(verify_edit, KDU_TIFFTAG_XResolution);
  be_copy.copy_tag(verify_edit, MakeTagType(0x0401, 10));

  MemoryTarget be_tgt;
  ASSERT_EQ(be_copy.write_header(&be_tgt, 8), 8);
  ASSERT_TRUE(be_copy.writedir(&be_tgt, 8));
  MemorySource be_src(be_tgt.data());
  kdu_tiffdir be_in;
  ASSERT_TRUE(be_in.opendir(&be_src));
  EXPECT_FALSE(be_in.is_littlendian());

  kdu_uint16 bps[4] = {};
  EXPECT_EQ(be_in.read_tag(KDU_TIFFTAG_BitsPerSample, 4, bps), 4);
  EXPECT_EQ(bps[0], 100);
  EXPECT_EQ(bps[3], 400);
  double rdbl[2] = {};
  EXPECT_EQ(be_in.read_tag(KDU_TIFFTAG_SmaxSampleValueD, 2, rdbl), 2);
  EXPECT_DOUBLE_EQ(rdbl[0], 1.25);
  EXPECT_DOUBLE_EQ(rdbl[1], -3.5);
  double rurat = 0.0, rsrat = 0.0;
  EXPECT_EQ(be_in.read_tag(KDU_TIFFTAG_XResolution, 1, &rurat), 1);
  EXPECT_DOUBLE_EQ(rurat, 72.0);
  EXPECT_EQ(be_in.read_tag(MakeTagType(0x0401, 10), 1, &rsrat), 1);
  EXPECT_DOUBLE_EQ(rsrat, -12.5);
  std::vector<kdu_byte> copied_bytes(350);
  EXPECT_EQ(be_in.read_tag(MakeTagType(0x0402, 1), 350, copied_bytes.data()),
            350);
  EXPECT_EQ(copied_bytes, big_bytes);
}

TEST(KduTiffTest, ZeroDenominatorRationalAndOpendirEdgeCases) {
  // Craft a little-endian regular TIFF with:
  //  - type 5 rational with den=0
  //  - type 10 signed rational with den=0
  //  - an unknown tag data_type (99) which opendir ignores
  std::vector<kdu_byte> data = {
      0x49, 0x49, 42,   0,    8,    0,    0,    0,     // Header -> offset 8
      3,    0,                                         // 3 entries
      0x00, 0x05, 5,    0,    1,    0,    0,    0,    50,   0, 0, 0,  // urat
      0x01, 0x05, 10,   0,    1,    0,    0,    0,    58,   0, 0, 0,  // srat
      0x02, 0x05, 99,   0,    1,    0,    0,    0,    0,    0, 0, 0,  // unknown
      0,    0,    0,    0,                                             // term
      123,  0,    0,    0,    0,    0,    0,    0,  // urat: 123 / 0
      200,  0,    0,    0,    0,    0,    0,    0   // srat: 200 / 0
  };
  MemorySource src(data);
  kdu_tiffdir dir;
  ASSERT_TRUE(dir.opendir(&src));
  EXPECT_EQ(dir.open_tag(MakeTagType(0x0502, 99)), 0u);
  double uval = 99.0, sval = 99.0;
  EXPECT_EQ(dir.read_tag(MakeTagType(0x0500, 5), 1, &uval), 1);
  EXPECT_EQ(uval, 0.0);
  EXPECT_EQ(dir.read_tag(MakeTagType(0x0501, 10), 1, &sval), 1);
  EXPECT_EQ(sval, 0.0);
}

TEST(KduTiffTest, OpendirFailureAndCorruptionPaths) {
  // Too short & invalid magic.
  {
    std::vector<kdu_byte> tiny = {0x49, 0x49, 42};
    MemorySource src(tiny);
    kdu_tiffdir dir;
    EXPECT_FALSE(dir.opendir(&src));
    EXPECT_FALSE(dir.exists());
  }
  {
    std::vector<kdu_byte> bad_magic = {0x41, 0x42, 42, 0, 8, 0, 0, 0};
    MemorySource src(bad_magic);
    kdu_tiffdir dir;
    EXPECT_FALSE(dir.opendir(&src));
  }
  // BigTIFF invalid offset32 != 8.
  {
    std::vector<kdu_byte> bad_bt = {0x49, 0x49, 43, 0, 4, 0, 0, 0,
                                    16,   0,    0,  0, 0, 0, 0, 0};
    MemorySource src(bad_bt);
    kdu_tiffdir dir;
    EXPECT_THROW(dir.opendir(&src), kdu_exception);
  }
  // BigTIFF truncated after first 8 bytes.
  {
    std::vector<kdu_byte> short_bt = {0x49, 0x49, 43, 0, 8, 0, 0, 0, 16, 0};
    MemorySource src(short_bt);
    kdu_tiffdir dir;
    EXPECT_FALSE(dir.opendir(&src));
  }
  // Seek to directory offset past EOF fails.
  {
    std::vector<kdu_byte> oob = {0x49, 0x49, 42, 0, 99, 0, 0, 0};
    MemorySource src(oob);
    kdu_tiffdir dir;
    EXPECT_FALSE(dir.opendir(&src));
  }
  // Truncated directory count (regular & BigTIFF).
  {
    std::vector<kdu_byte> trunc_cnt = {0x49, 0x49, 42, 0, 8, 0, 0, 0, 1};
    MemorySource src(trunc_cnt);
    kdu_tiffdir dir;
    EXPECT_THROW(dir.opendir(&src), kdu_exception);
  }
  {
    std::vector<kdu_byte> trunc_bt_cnt = {0x49, 0x49, 43, 0, 8, 0, 0, 0,
                                          16,   0,    0,  0, 0, 0, 0, 0,
                                          1,    0};
    MemorySource src(trunc_bt_cnt);
    kdu_tiffdir dir;
    EXPECT_THROW(dir.opendir(&src), kdu_exception);
  }
  // Truncated directory entries (regular & BigTIFF).
  {
    std::vector<kdu_byte> trunc_ent = {0x49, 0x49, 42, 0, 8, 0, 0, 0,
                                       1,    0,    1,  2, 3};
    MemorySource src(trunc_ent);
    kdu_tiffdir dir;
    EXPECT_THROW(dir.opendir(&src), kdu_exception);
  }
  {
    std::vector<kdu_byte> trunc_bt_ent = {
        0x49, 0x49, 43, 0, 8, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0,
        1,    0,    0,  0, 0, 0, 0, 0, 1,  2, 3, 4};
    MemorySource src(trunc_bt_ent);
    kdu_tiffdir dir;
    EXPECT_THROW(dir.opendir(&src), kdu_exception);
  }
  // Final seek(0) fails -> returns false (and exercises unknown tag skip).
  {
    std::vector<kdu_byte> valid_unknown_tag = {
        0x49, 0x49, 42, 0, 8, 0, 0, 0,
        1,    0,                          // entry_count = 1
        0x10, 0x01, 99, 0, 1, 0, 0, 0, 0, 0, 0, 0  // tag 0x0110, type 99
    };
    // Call 0 is seek(8); call 1 is seek(0).
    MemorySource src(valid_unknown_tag,
                     KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE,
                     /*fail_seek_at_call=*/1);
    kdu_tiffdir dir;
    EXPECT_FALSE(dir.opendir(&src));
    EXPECT_FALSE(dir.exists());
  }
  // Truncated external tag data triggers read_bytes error.
  {
    std::vector<kdu_byte> trunc_ext = {
        0x49, 0x49, 42, 0, 8, 0, 0, 0, 1,  0,
        0x11, 0x01, 4,  0, 4, 0, 0, 0, 26, 0, 0, 0,
        0,    0,    0,  0, 1, 2, 3, 4  // Only 4 of 16 bytes present.
    };
    MemorySource src(trunc_ext);
    kdu_tiffdir dir;
    ASSERT_TRUE(dir.opendir(&src));
    kdu_uint32 out[4] = {};
    EXPECT_THROW(dir.read_tag(KDU_TIFFTAG_StripOffsets32, 4, out),
                 kdu_exception);
  }
}

TEST(KduTiffTest, WriteAndReadValidationErrors) {
  // Failing target writes during header, terminator, and out-of-line bodies.
  for (bool bigtiff : {false, true}) {
    kdu_tiffdir dir;
    dir.init(/*littlendian=*/true, bigtiff);
    const kdu_uint32 ext_u32[4] = {1, 2, 3, 4};
    dir.write_tag(KDU_TIFFTAG_StripOffsets32, 4, ext_u32);

    MemoryTarget fail_hdr(0);
    EXPECT_EQ(dir.write_header(&fail_hdr, 16), 0);

    // Write 0: entry count, Write 1: tag entry, Write 2: terminator.
    MemoryTarget fail_term(2);
    EXPECT_FALSE(dir.writedir(&fail_term, 16));

    // Write 3: extra tag body -> fail at 3.
    MemoryTarget fail_body(3);
    EXPECT_FALSE(dir.writedir(&fail_body, 16));
  }

  // Regular TIFF dir_offset > 32-bit throws during writedir.
  {
    kdu_tiffdir dir;
    dir.init(/*littlendian=*/true, /*bigtiff=*/false);
    dir.write_tag(KDU_TIFFTAG_ImageWidth32, static_cast<kdu_uint32>(100));
    MemoryTarget dummy;
    EXPECT_THROW(dir.writedir(&dummy, 0x100000000LL), kdu_exception);
  }

  // Unaligned tag access rejected by typed write_tag and typed read_tag.
  {
    kdu_tiffdir dir;
    dir.init(/*littlendian=*/true, /*bigtiff=*/false);
    const kdu_byte one_byte = 0x42;
    const kdu_byte three_bytes[3] = {1, 2, 3};
    const kdu_byte five_bytes[5] = {1, 2, 3, 4, 5};
    const kdu_uint32 u16_tag = MakeTagType(0x0600, 3);
    const kdu_uint32 u32_tag = MakeTagType(0x0601, 4);
    const kdu_uint32 f_tag = MakeTagType(0x0602, 11);
    const kdu_uint32 u64_tag = MakeTagType(0x0603, 16);

    dir.write_tag(u16_tag, 3, three_bytes);
    kdu_uint16 w16 = 1;
    EXPECT_THROW(dir.write_tag(u16_tag, 1, &w16), kdu_exception);
    kdu_byte discard = 0;
    EXPECT_EQ(dir.read_tag(u16_tag, 1, &discard), 1);  // Advance read_ptr to 1.
    kdu_uint16 r16 = 0;
    EXPECT_THROW(dir.read_tag(u16_tag, 1, &r16), kdu_exception);

    dir.write_tag(u32_tag, 5, five_bytes);
    kdu_uint32 w32 = 1;
    EXPECT_THROW(dir.write_tag(u32_tag, 1, &w32), kdu_exception);
    EXPECT_EQ(dir.read_tag(u32_tag, 1, &discard), 1);  // Advance read_ptr to 1.
    kdu_uint32 r32 = 0;
    EXPECT_THROW(dir.read_tag(u32_tag, 1, &r32), kdu_exception);

    dir.write_tag(f_tag, 5, five_bytes);
    double wd = 1.0, rd = 0.0;
    EXPECT_THROW(dir.write_tag(f_tag, 1, &wd), kdu_exception);
    EXPECT_EQ(dir.read_tag(f_tag, 1, &discard), 1);  // Advance read_ptr to 1.
    EXPECT_THROW(dir.read_tag(f_tag, 1, &rd), kdu_exception);

    kdu_tiffdir bt_dir;
    bt_dir.init(/*littlendian=*/true, /*bigtiff=*/true);
    bt_dir.write_tag(u64_tag, 1, &one_byte);
    kdu_long w64 = 1, r64 = 0;
    EXPECT_THROW(bt_dir.write_tag(u64_tag, 1, &w64), kdu_exception);
    const kdu_byte nine_bytes[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    const kdu_uint32 u64_tag2 = MakeTagType(0x0604, 16);
    bt_dir.write_tag(u64_tag2, 9, nine_bytes);
    EXPECT_EQ(bt_dir.read_tag(u64_tag2, 1, &discard), 1);
    EXPECT_THROW(bt_dir.read_tag(u64_tag2, 1, &r64), kdu_exception);
  }

  // create_tag, write_tag, and read_tag type / length validation errors.
  {
    kdu_tiffdir dir;
    dir.init(/*littlendian=*/true, /*bigtiff=*/false);
    EXPECT_THROW(dir.create_tag(MakeTagType(0x0100, 99)), kdu_exception);
    EXPECT_THROW(dir.create_tag(MakeTagType(0x0000, 3)), kdu_exception);
    EXPECT_THROW(dir.create_tag(KDU_TIFFTAG_StripOffsets64),
                 kdu_exception);

    dir.create_tag(MakeTagType(0x0700, 3));
    EXPECT_THROW(dir.create_tag(MakeTagType(0x0700, 4)), kdu_exception);

    const kdu_byte byte_val = 1;
    kdu_uint16 val16 = 1;
    kdu_uint32 val32 = 1;
    double val_dbl = 1.0;
    kdu_long val64 = 1;

    EXPECT_THROW(dir.write_tag(MakeTagType(0x0701, 4), 1, &val16),
                 kdu_exception);
    EXPECT_THROW(dir.write_tag(MakeTagType(0x0701, 3), 1, &val32),
                 kdu_exception);
    EXPECT_THROW(dir.write_tag(MakeTagType(0x0701, 4), 1, &val_dbl),
                 kdu_exception);
    EXPECT_THROW(dir.write_tag(MakeTagType(0x0701, 11), 1, &val64),
                 kdu_exception);

    EXPECT_THROW(dir.write_tag(MakeTagType(0x0702, 1), -1, &byte_val),
                 kdu_exception);
    EXPECT_THROW(dir.write_tag(MakeTagType(0x0703, 3), -1, &val16),
                 kdu_exception);
    EXPECT_THROW(dir.write_tag(MakeTagType(0x0704, 4), -1, &val32),
                 kdu_exception);
    EXPECT_THROW(dir.write_tag(MakeTagType(0x0705, 11), -1, &val_dbl),
                 kdu_exception);
    EXPECT_THROW(dir.write_tag(MakeTagType(0x0706, 4), -1, &val64),
                 kdu_exception);

    EXPECT_THROW(dir.read_tag(MakeTagType(0x0700, 4), 1, &val16),
                 kdu_exception);
    EXPECT_THROW(dir.read_tag(MakeTagType(0x0700, 11), 1, &val32),
                 kdu_exception);
    EXPECT_THROW(dir.read_tag(MakeTagType(0x0700, 4), 1, &val_dbl),
                 kdu_exception);
    EXPECT_THROW(dir.read_tag(MakeTagType(0x0700, 11), 1, &val64),
                 kdu_exception);

    // Missing tags return 0 on every read_tag overload.
    EXPECT_EQ(dir.read_tag(MakeTagType(0x0800, 1), 1,
                           const_cast<kdu_byte *>(&byte_val)),
              0);
    EXPECT_EQ(dir.read_tag(MakeTagType(0x0800, 3), 1, &val16), 0);
    EXPECT_EQ(dir.read_tag(MakeTagType(0x0800, 4), 1, &val32), 0);
    EXPECT_EQ(dir.read_tag(MakeTagType(0x0800, 11), 1, &val_dbl), 0);
    EXPECT_EQ(dir.read_tag(MakeTagType(0x0800, 16), 1, &val64), 0);
  }
}

}  // namespace
}  // namespace kdu_supp
