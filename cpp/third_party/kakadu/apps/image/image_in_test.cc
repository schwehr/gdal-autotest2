// Unit tests for third_party/kakadu/vendor/apps/image/image_in.cpp.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/apps/image/kdu_image.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_elementary.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"

namespace kdu_supp {
namespace {

using ::kdu_core::kdu_byte;
using ::kdu_core::kdu_codestream;
using ::kdu_core::kdu_exception;
using ::kdu_core::kdu_int32;
using ::kdu_core::kdu_int64;
using ::kdu_core::kdu_line_buf;
using ::kdu_core::kdu_long;
using ::kdu_core::kdu_sample_allocator;
using ::kdu_core::kdu_uint16;
using ::kdu_core::kdu_uint32;
using ::kdu_core::kdu_uint64;

constexpr kdu_uint16 kTiffByte = 1;
constexpr kdu_uint16 kTiffAscii = 2;
constexpr kdu_uint16 kTiffShort = 3;
constexpr kdu_uint16 kTiffLong = 4;
constexpr kdu_uint16 kTiffRational = 5;
constexpr kdu_uint16 kTiffUndefined = 7;
constexpr kdu_uint16 kTiffFloat = 11;
constexpr kdu_uint16 kTiffDouble = 12;
constexpr kdu_uint16 kTiffLong8 = 16;

class ThrowingErrorHandler final : public ::kdu_core::kdu_thread_safe_message {
 public:
  void put_text(const char* /*text*/) override {}
  void flush(bool end_of_message = false) override {
    ::kdu_core::kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw KDU_ERROR_EXCEPTION;
    }
  }
};

class SilentWarningHandler final : public ::kdu_core::kdu_thread_safe_message {
 public:
  void put_text(const char* /*text*/) override {}
};

class ImageInTestEnv final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const err_handler = new ThrowingErrorHandler();
    static auto* const warn_handler = new SilentWarningHandler();
    ::kdu_core::kdu_customize_errors(err_handler);
    ::kdu_core::kdu_customize_warnings(warn_handler);
  }
};

const ::testing::Environment* const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new ImageInTestEnv());

class TempFile {
 public:
  explicit TempFile(const std::string& filename)
      : path_(::testing::TempDir() + "/" + filename) {}

  ~TempFile() { std::remove(path_.c_str()); }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  void WriteBytes(const std::vector<kdu_byte>& data) const {
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "Failed to open " << path_;
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    ASSERT_TRUE(out.good());
  }

  void WriteString(const std::string& text) const {
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "Failed to open " << path_;
    out.write(text.data(), text.size());
    ASSERT_TRUE(out.good());
  }

  const char* c_str() const { return path_.c_str(); }

 private:
  std::string path_;
};

class ScopedImageIn {
 public:
  ScopedImageIn(const char* fname, kdu_image_dims& dims, int& next_comp,
                bool vflip = false, kdu_rgb8_palette* palette = nullptr,
                kdu_long skip_bytes = 0, bool quiet = true)
      : reader_(fname, dims, next_comp, vflip, palette, skip_bytes, quiet) {}
  ScopedImageIn(ScopedImageIn&& other) noexcept : reader_(other.reader_) {
    other.reader_ = kdu_image_in();
  }
  ScopedImageIn& operator=(ScopedImageIn&& other) noexcept {
    if (this != &other) {
      if (reader_.exists()) reader_.destroy();
      reader_ = other.reader_;
      other.reader_ = kdu_image_in();
    }
    return *this;
  }
  ~ScopedImageIn() {
    if (reader_.exists()) reader_.destroy();
  }
  ScopedImageIn(const ScopedImageIn&) = delete;
  ScopedImageIn& operator=(const ScopedImageIn&) = delete;
  kdu_image_in& get() { return reader_; }

 private:
  kdu_image_in reader_;
};

ScopedImageIn OpenReader(const char* fname, kdu_image_dims& dims,
                         int& next_comp, bool vflip = false,
                         kdu_rgb8_palette* palette = nullptr,
                         kdu_long skip_bytes = 0, bool quiet = true) {
  return ScopedImageIn(fname, dims, next_comp, vflip, palette, skip_bytes,
                       quiet);
}

bool ReadLineValues64(ScopedImageIn& scoped_reader, int comp_idx, int width,
                      kdu_byte flags, int x_tnum,
                      std::vector<kdu_int64>* ints_out,
                      std::vector<float>* floats_out) {
  kdu_image_in& reader = scoped_reader.get();
  kdu_sample_allocator alloc;
  kdu_line_buf line;
  if (width > 0) {
    line.pre_create(&alloc, width, flags, 0, 0);
    alloc.finalize(kdu_codestream());
    line.create();
  }
  bool ok = reader.get(comp_idx, line, x_tnum);
  if (ok) {
    if (ints_out != nullptr) {
      ints_out->clear();
      if (line.get_buf64() != nullptr) {
        for (int i = 0; i < width; ++i) {
          ints_out->push_back(line.get_buf64()[i].ival);
        }
      } else if (line.get_buf32() != nullptr && line.is_absolute()) {
        for (int i = 0; i < width; ++i) {
          ints_out->push_back(line.get_buf32()[i].ival);
        }
      } else if (line.get_buf16() != nullptr) {
        for (int i = 0; i < width; ++i) {
          ints_out->push_back(line.get_buf16()[i].ival);
        }
      }
    }
    if (floats_out != nullptr && line.get_buf32() != nullptr &&
        !line.is_absolute()) {
      floats_out->clear();
      for (int i = 0; i < width; ++i) {
        floats_out->push_back(line.get_buf32()[i].fval);
      }
    }
  }
  line.destroy();
  return ok;
}

std::vector<kdu_int64> ReadAbsRow(ScopedImageIn& reader, int comp_idx,
                                  int width, kdu_byte flags, int x_tnum = 0) {
  std::vector<kdu_int64> values;
  EXPECT_TRUE(ReadLineValues64(reader, comp_idx, width, flags, x_tnum, &values,
                               nullptr));
  return values;
}

std::vector<float> ReadFloatRow(ScopedImageIn& reader, int comp_idx, int width,
                                int x_tnum = 0) {
  std::vector<float> values;
  EXPECT_TRUE(
      ReadLineValues64(reader, comp_idx, width, 0, x_tnum, nullptr, &values));
  return values;
}

void AppendLe16(std::vector<kdu_byte>& out, kdu_uint16 v) {
  out.push_back(static_cast<kdu_byte>(v & 0xFF));
  out.push_back(static_cast<kdu_byte>((v >> 8) & 0xFF));
}

void AppendLe32(std::vector<kdu_byte>& out, kdu_uint32 v) {
  out.push_back(static_cast<kdu_byte>(v & 0xFF));
  out.push_back(static_cast<kdu_byte>((v >> 8) & 0xFF));
  out.push_back(static_cast<kdu_byte>((v >> 16) & 0xFF));
  out.push_back(static_cast<kdu_byte>((v >> 24) & 0xFF));
}

void AppendUint(std::vector<kdu_byte>& out, kdu_uint64 v, int bytes,
                bool little_endian) {
  if (little_endian) {
    for (int i = 0; i < bytes; ++i) {
      out.push_back(static_cast<kdu_byte>((v >> (8 * i)) & 0xFF));
    }
  } else {
    for (int i = bytes - 1; i >= 0; --i) {
      out.push_back(static_cast<kdu_byte>((v >> (8 * i)) & 0xFF));
    }
  }
}

void WriteUintAt(std::vector<kdu_byte>& out, size_t pos, kdu_uint64 v,
                 int bytes, bool little_endian) {
  if (little_endian) {
    for (int i = 0; i < bytes; ++i) {
      out[pos + i] = static_cast<kdu_byte>((v >> (8 * i)) & 0xFF);
    }
  } else {
    for (int i = 0; i < bytes; ++i) {
      out[pos + bytes - 1 - i] = static_cast<kdu_byte>((v >> (8 * i)) & 0xFF);
    }
  }
}

kdu_uint64 DoubleToBits(double d) {
  kdu_uint64 bits = 0;
  std::memcpy(&bits, &d, sizeof(bits));
  return bits;
}

kdu_uint32 FloatToBits(float f) {
  kdu_uint32 bits = 0;
  std::memcpy(&bits, &f, sizeof(bits));
  return bits;
}

class TiffBuilder {
 public:
  TiffBuilder(bool little_endian, bool bigtiff)
      : little_endian_(little_endian), bigtiff_(bigtiff) {}

  void AddShort(kdu_uint16 tag, const std::vector<kdu_uint16>& vals) {
    AddShortTo(entries_, tag, vals);
  }

  void AddLong(kdu_uint16 tag, const std::vector<kdu_uint32>& vals) {
    AddLongTo(entries_, tag, vals);
  }

  void AddLong8(kdu_uint16 tag, const std::vector<kdu_uint64>& vals) {
    AddLong8To(entries_, tag, vals);
  }

  void AddRational(kdu_uint16 tag, kdu_uint32 num, kdu_uint32 den) {
    std::vector<kdu_byte> data;
    AppendUint(data, num, 4, little_endian_);
    AppendUint(data, den, 4, little_endian_);
    entries_.push_back({tag, kTiffRational, 1, std::move(data)});
  }

  void AddDouble(kdu_uint16 tag, const std::vector<double>& vals) {
    std::vector<kdu_byte> data;
    for (double d : vals) {
      AppendUint(data, DoubleToBits(d), 8, little_endian_);
    }
    entries_.push_back({tag, kTiffDouble, vals.size(), std::move(data)});
  }

  void AddFloat(kdu_uint16 tag, const std::vector<float>& vals) {
    std::vector<kdu_byte> data;
    for (float f : vals) {
      AppendUint(data, FloatToBits(f), 4, little_endian_);
    }
    entries_.push_back({tag, kTiffFloat, vals.size(), std::move(data)});
  }

  void AddBytes(kdu_uint16 tag, kdu_uint16 type,
                const std::vector<kdu_byte>& vals) {
    entries_.push_back({tag, type, vals.size(), vals});
  }

  void AddAscii(kdu_uint16 tag, const std::string& text) {
    std::vector<kdu_byte> data(text.begin(), text.end());
    data.push_back('\0');
    entries_.push_back({tag, kTiffAscii, data.size(), std::move(data)});
  }

  std::vector<kdu_byte> BuildWithChunks(
      const std::vector<std::vector<kdu_byte>>& chunks, bool use_tiles,
      kdu_uint16 offsets_tag_type = 0) const {
    std::vector<kdu_byte> out;
    out.push_back(little_endian_ ? 'I' : 'M');
    out.push_back(little_endian_ ? 'I' : 'M');
    if (bigtiff_) {
      AppendUint(out, 43, 2, little_endian_);
      AppendUint(out, 8, 4, little_endian_);
      AppendUint(out, 16, 8, little_endian_);
    } else {
      AppendUint(out, 42, 2, little_endian_);
      AppendUint(out, 8, 4, little_endian_);
    }

    std::vector<Entry> all = entries_;
    size_t num_chunks = chunks.size();
    kdu_uint16 off_tag = use_tiles ? 324 : 273;
    kdu_uint16 cnt_tag = use_tiles ? 325 : 279;

    if (offsets_tag_type == kTiffShort && !bigtiff_) {
      AddShortTo(all, off_tag, std::vector<kdu_uint16>(num_chunks, 0));
      AddShortTo(all, cnt_tag, std::vector<kdu_uint16>(num_chunks, 0));
    } else if (bigtiff_ && offsets_tag_type == kTiffLong8) {
      AddLong8To(all, off_tag, std::vector<kdu_uint64>(num_chunks, 0));
      AddLong8To(all, cnt_tag, std::vector<kdu_uint64>(num_chunks, 0));
    } else {
      AddLongTo(all, off_tag, std::vector<kdu_uint32>(num_chunks, 0));
      AddLongTo(all, cnt_tag, std::vector<kdu_uint32>(num_chunks, 0));
    }
    std::sort(all.begin(), all.end(),
              [](const Entry& a, const Entry& b) { return a.tag < b.tag; });

    size_t count_bytes = bigtiff_ ? 8 : 2;
    size_t entry_bytes = bigtiff_ ? 20 : 12;
    size_t next_ifd_bytes = bigtiff_ ? 8 : 4;
    size_t inline_cap = bigtiff_ ? 8 : 4;

    size_t ifd_start = out.size();
    size_t ifd_size = count_bytes + all.size() * entry_bytes + next_ifd_bytes;
    out.resize(ifd_start + ifd_size, 0);
    WriteUintAt(out, ifd_start, all.size(), count_bytes, little_endian_);

    size_t off_patch_pos = 0;
    size_t cnt_patch_pos = 0;
    for (size_t i = 0; i < all.size(); ++i) {
      const Entry& e = all[i];
      size_t pos = ifd_start + count_bytes + i * entry_bytes;
      WriteUintAt(out, pos, e.tag, 2, little_endian_);
      WriteUintAt(out, pos + 2, e.type, 2, little_endian_);
      WriteUintAt(out, pos + 4, e.count, bigtiff_ ? 8 : 4, little_endian_);
      size_t val_pos = pos + (bigtiff_ ? 12 : 8);
      if (e.data.size() <= inline_cap) {
        std::memcpy(out.data() + val_pos, e.data.data(), e.data.size());
        if (e.tag == off_tag) off_patch_pos = val_pos;
        if (e.tag == cnt_tag) cnt_patch_pos = val_pos;
      } else {
        kdu_uint64 data_off = out.size();
        WriteUintAt(out, val_pos, data_off, inline_cap, little_endian_);
        if (e.tag == off_tag) off_patch_pos = data_off;
        if (e.tag == cnt_tag) cnt_patch_pos = data_off;
        out.insert(out.end(), e.data.begin(), e.data.end());
        if ((out.size() & 1) != 0) out.push_back(0);
      }
    }

    size_t elem_size =
        (offsets_tag_type == kTiffShort && !bigtiff_)
            ? 2
            : ((bigtiff_ && offsets_tag_type == kTiffLong8) ? 8 : 4);
    for (size_t i = 0; i < num_chunks; ++i) {
      kdu_uint64 chunk_off = out.size();
      WriteUintAt(out, off_patch_pos + i * elem_size, chunk_off, elem_size,
                  little_endian_);
      WriteUintAt(out, cnt_patch_pos + i * elem_size, chunks[i].size(),
                  elem_size, little_endian_);
      out.insert(out.end(), chunks[i].begin(), chunks[i].end());
    }
    return out;
  }

 private:
  struct Entry {
    kdu_uint16 tag;
    kdu_uint16 type;
    kdu_uint64 count;
    std::vector<kdu_byte> data;
  };

  void AddShortTo(std::vector<Entry>& dst, kdu_uint16 tag,
                  const std::vector<kdu_uint16>& vals) const {
    std::vector<kdu_byte> data;
    for (kdu_uint16 v : vals) AppendUint(data, v, 2, little_endian_);
    dst.push_back({tag, kTiffShort, vals.size(), std::move(data)});
  }

  void AddLongTo(std::vector<Entry>& dst, kdu_uint16 tag,
                 const std::vector<kdu_uint32>& vals) const {
    std::vector<kdu_byte> data;
    for (kdu_uint32 v : vals) AppendUint(data, v, 4, little_endian_);
    dst.push_back({tag, kTiffLong, vals.size(), std::move(data)});
  }

  void AddLong8To(std::vector<Entry>& dst, kdu_uint16 tag,
                  const std::vector<kdu_uint64>& vals) const {
    std::vector<kdu_byte> data;
    for (kdu_uint64 v : vals) AppendUint(data, v, 8, little_endian_);
    dst.push_back({tag, kTiffLong8, vals.size(), std::move(data)});
  }

  bool little_endian_;
  bool bigtiff_;
  std::vector<Entry> entries_;
};

std::vector<kdu_byte> BuildBmp(int width, int height, bool top_down, int bpp,
                               int compression, kdu_uint32 colors_used,
                               const std::vector<kdu_byte>& palette_bgra,
                               const std::vector<kdu_byte>& pixel_bytes,
                               kdu_int32 xpels_per_meter = 2835) {
  std::vector<kdu_byte> out;
  kdu_uint32 offset = 54 + static_cast<kdu_uint32>(palette_bgra.size());
  kdu_uint32 file_size = offset + static_cast<kdu_uint32>(pixel_bytes.size());
  out.push_back('B');
  out.push_back('M');
  AppendLe32(out, file_size);
  AppendLe16(out, 0);
  AppendLe16(out, 0);
  AppendLe32(out, offset);

  AppendLe32(out, 40);
  AppendLe32(out, static_cast<kdu_uint32>(width));
  kdu_int32 signed_h = top_down ? -height : height;
  AppendLe32(out, static_cast<kdu_uint32>(signed_h));
  AppendLe16(out, 1);
  AppendLe16(out, static_cast<kdu_uint16>(bpp));
  AppendLe32(out, static_cast<kdu_uint32>(compression));
  AppendLe32(out, static_cast<kdu_uint32>(pixel_bytes.size()));
  AppendLe32(out, static_cast<kdu_uint32>(xpels_per_meter));
  AppendLe32(out, static_cast<kdu_uint32>(xpels_per_meter));
  AppendLe32(out, colors_used);
  AppendLe32(out, colors_used);
  out.insert(out.end(), palette_bgra.begin(), palette_bgra.end());
  out.insert(out.end(), pixel_bytes.begin(), pixel_bytes.end());
  return out;
}

TEST(ImageInDispatcherTest, LifecycleAndBadSuffixes) {
  kdu_image_in empty_reader;
  EXPECT_FALSE(empty_reader.exists());
  EXPECT_TRUE(!empty_reader);

  kdu_image_dims dims;
  int next_comp = 0;
  EXPECT_THROW(OpenReader("no_suffix_here", dims, next_comp), kdu_exception);
  EXPECT_THROW(OpenReader("unknown.xyz", dims, next_comp), kdu_exception);
  EXPECT_THROW(OpenReader("missing_file.pgm", dims, next_comp), kdu_exception);

  TempFile pgm("lifecycle.pgm");
  pgm.WriteBytes({'P', '5', '\n', '2', ' ', '2', '\n', '2', '5', '5', '\n', 10,
                  20, 30, 40});
  bool vflip = false;
  kdu_image_in r1(pgm.c_str(), dims, next_comp, vflip);
  EXPECT_TRUE(r1.exists());
  EXPECT_FALSE(!r1);
  kdu_image_in r2(r1);
  EXPECT_TRUE(r2.exists());
  kdu_image_in r3;
  r3 = r2;
  EXPECT_TRUE(r3.exists());
  // Calling destroy() before consuming all rows emits a warning.
  r3.destroy();
  EXPECT_FALSE(r3.exists());
}

TEST(ImageInPbmTest, ReadsP4AcrossBuffersTilesPaletteAndForcedPrecision) {
  TempFile pbm("sample.pbm");
  pbm.WriteBytes({'P', '4',  '\n', '#',  ' ',  'c', 'o', 'm',
                  'm', 'e',  'n',  't',  '\n', '1', '0', ' ',
                  '2', '\n', 0xAA, 0xC0, 0x55, 0x00});

  {
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pbm.c_str(), dims, next_comp);
    EXPECT_EQ(1, next_comp);
    EXPECT_EQ(2, dims.get_height(0));
    EXPECT_EQ(10, dims.get_width(0));
    EXPECT_EQ(1, dims.get_bit_depth(0));
    EXPECT_FALSE(dims.get_signed(0));

    auto t0 = ReadAbsRow(reader, 0, 6,
                         KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE, 0);
    auto t1 = ReadAbsRow(reader, 0, 4, KDU_LINE_BUF_SHORTS, 1);
    EXPECT_EQ(6u, t0.size());
    EXPECT_EQ(4u, t1.size());
    std::vector<kdu_int64> empty_tile;
    EXPECT_TRUE(ReadLineValues64(reader, 0, 0, KDU_LINE_BUF_SHORTS, 2,
                                 &empty_tile, nullptr));

    auto r1a = ReadAbsRow(reader, 0, 4,
                          KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE, 0);
    auto r1b = ReadFloatRow(reader, 0, 6, 1);
    EXPECT_EQ(4u, r1a.size());
    EXPECT_EQ(6u, r1b.size());

    std::vector<kdu_int64> past_eof;
    EXPECT_FALSE(ReadLineValues64(reader, 0, 4, KDU_LINE_BUF_ABSOLUTE, 0,
                                  &past_eof, nullptr));
  }

  for (bool monochrome : {false, true}) {
    kdu_image_dims dims;
    kdu_rgb8_palette pal;
    int next_comp = 0;
    ScopedImageIn reader =
        OpenReader(pbm.c_str(), dims, next_comp, false, &pal);
    if (monochrome) {
      pal.blue[0] = pal.green[0] = pal.red[0] = 0;
      pal.blue[1] = pal.green[1] = pal.red[1] = 255;
    }
    EXPECT_TRUE(pal.exists());
    EXPECT_EQ(10u, ReadAbsRow(reader, 0, 10, KDU_LINE_BUF_ABSOLUTE, 0).size());
    EXPECT_EQ(10u, ReadAbsRow(reader, 0, 10,
                              KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE, 0)
                       .size());
  }

  {
    kdu_image_dims dims;
    dims.set_forced_precision(0, 8, false, 3);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pbm.c_str(), dims, next_comp);
    EXPECT_EQ(8, dims.get_bit_depth(0));
    EXPECT_EQ(10u, ReadAbsRow(reader, 0, 10, KDU_LINE_BUF_ABSOLUTE, 0).size());
    EXPECT_EQ(10u, ReadAbsRow(reader, 0, 10, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }
  {
    kdu_image_dims dims;
    dims.set_forced_precision(0, 6, true, 0);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pbm.c_str(), dims, next_comp);
    EXPECT_EQ(6, dims.get_bit_depth(0));
    EXPECT_EQ(10u, ReadAbsRow(reader, 0, 10, KDU_LINE_BUF_ABSOLUTE, 0).size());
    EXPECT_EQ(10u, ReadAbsRow(reader, 0, 10, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }

  TempFile bad_pbm("bad.pbm");
  bad_pbm.WriteString("P1\n2 2\n");
  kdu_image_dims dims;
  int next_comp = 0;
  EXPECT_THROW(OpenReader(bad_pbm.c_str(), dims, next_comp), kdu_exception);

  TempFile trunc_pbm("trunc.pbm");
  trunc_pbm.WriteString("P4\n8 2\n");
  auto r_trunc = OpenReader(trunc_pbm.c_str(), dims, next_comp);
  EXPECT_THROW(ReadAbsRow(r_trunc, 0, 8, KDU_LINE_BUF_ABSOLUTE, 0),
               kdu_exception);
}

TEST(ImageInPgmTest, ReadsP5Across8And16BitAndErrorPaths) {
  TempFile pgm8("pgm8.pgm");
  pgm8.WriteBytes({'P',  '5', ' ', '#',  ' ', 'h', 'i', '\n', '3', ' ', '2',
                   '\n', '1', '5', '\n', 0,   5,   15,  1,    7,   14});
  {
    kdu_image_dims dims;
    dims.set_forced_precision(0, 12, false, 0);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pgm8.c_str(), dims, next_comp);
    EXPECT_EQ(12, dims.get_bit_depth(0));
    EXPECT_EQ(3u, ReadAbsRow(reader, 0, 3, KDU_LINE_BUF_ABSOLUTE, 0).size());
    EXPECT_EQ(3u, ReadAbsRow(reader, 0, 3,
                             KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE, 0)
                      .size());
  }

  TempFile pgm16("pgm16.pgm");
  pgm16.WriteBytes({'P', '5',  '\n', '2',  ' ',  '2',  '\n', '1',  '0',  '2',
                    '3', '\n', 0x00, 0x01, 0x03, 0xFF, 0x01, 0x00, 0x02, 0x00});
  {
    kdu_image_dims dims;
    dims.set_forced_precision(0, 16, false, 5);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pgm16.c_str(), dims, next_comp);
    EXPECT_EQ(16, dims.get_bit_depth(0));
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2,
                             KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE, 0)
                      .size());
  }

  {
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pgm8.c_str(), dims, next_comp);
    EXPECT_EQ(4, dims.get_bit_depth(0));
    EXPECT_EQ(2u, ReadFloatRow(reader, 0, 2, 0).size());
    EXPECT_EQ(1u, ReadAbsRow(reader, 0, 1,
                             KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE, 1)
                      .size());
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_SHORTS, 0).size());
    EXPECT_EQ(1u, ReadAbsRow(reader, 0, 1, KDU_LINE_BUF_SHORTS, 1).size());
    std::vector<kdu_int64> eof_vals;
    EXPECT_FALSE(ReadLineValues64(reader, 0, 3, KDU_LINE_BUF_ABSOLUTE, 0,
                                  &eof_vals, nullptr));
  }

  TempFile bad_pgm("bad.pgm");
  bad_pgm.WriteString("P2\n2 2\n255\n");
  kdu_image_dims dims;
  int next_comp = 0;
  EXPECT_THROW(OpenReader(bad_pgm.c_str(), dims, next_comp), kdu_exception);

  TempFile bad_max("badmax.pgm");
  bad_max.WriteString("P5\n0 2\n255\n");
  EXPECT_THROW(OpenReader(bad_max.c_str(), dims, next_comp), kdu_exception);
}

TEST(ImageInPpmTest, ReadsP6Across8And16Bit) {
  TempFile ppm8("ppm8.ppm");
  ppm8.WriteBytes({'P', '6', '\n', '2', ' ', '2', '\n', '2', '5', '5', '\n', 10,
                   20,  30,  40,   50,  60,  70,  80,   90,  100, 110, 120});
  {
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(ppm8.c_str(), dims, next_comp);
    EXPECT_EQ(3, next_comp);
    EXPECT_EQ(8, dims.get_bit_depth(0));
    for (int c = 0; c < 3; ++c) {
      EXPECT_EQ(2u, ReadAbsRow(reader, c, 2,
                               KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE, 0)
                        .size());
    }
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
    EXPECT_EQ(2u, ReadFloatRow(reader, 1, 2, 0).size());
    EXPECT_EQ(2u, ReadAbsRow(reader, 2, 2,
                             KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE, 0)
                      .size());
  }

  TempFile ppm16("ppm16.ppm");
  std::vector<kdu_byte> ppm16_bytes = {'P',  '6', '\n', '2', ' ', '1',
                                       '\n', '4', '0',  '9', '5', '\n'};
  for (int i = 0; i < 6; ++i) {
    AppendUint(ppm16_bytes, 100 + i * 200, 2, false);
  }
  ppm16.WriteBytes(ppm16_bytes);
  {
    kdu_image_dims dims;
    dims.set_forced_precision(0, 40, false, 0);
    dims.set_forced_precision(1, 10, true, 0);
    dims.set_forced_precision(2, 16, false, 5);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(ppm16.c_str(), dims, next_comp);
    EXPECT_EQ(32, dims.get_bit_depth(0));
    EXPECT_EQ(10, dims.get_bit_depth(1));
    EXPECT_EQ(16, dims.get_bit_depth(2));
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
    EXPECT_EQ(2u, ReadAbsRow(reader, 1, 2, KDU_LINE_BUF_SHORTS, 0).size());
    EXPECT_EQ(2u, ReadAbsRow(reader, 2, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }

  TempFile bad_ppm("bad.ppm");
  bad_ppm.WriteString("P3\n1 1\n255\n");
  kdu_image_dims dims;
  int next_comp = 0;
  EXPECT_THROW(OpenReader(bad_ppm.c_str(), dims, next_comp), kdu_exception);
}

TEST(ImageInPfmTest, ReadsColorAndGreyscaleAcrossEndiansAndBuffers) {
  auto write_pfm = [](const TempFile& f, bool color, int w, int h, float scale,
                      const std::vector<float>& samples) {
    std::ostringstream hdr;
    hdr << (color ? "PF" : "Pf") << "\n"
        << w << " " << h << "\n"
        << scale << "\n";
    std::string hstr = hdr.str();
    std::vector<kdu_byte> bytes(hstr.begin(), hstr.end());
    bool le = (scale < 0.0f);
    for (float v : samples) {
      AppendUint(bytes, FloatToBits(v), 4, le);
    }
    f.WriteBytes(bytes);
  };

  TempFile pfm_le("le.pfm");
  write_pfm(pfm_le, true, 2, 2, -1.0f,
            {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.5f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f,
             0.6f});
  {
    kdu_image_dims dims;
    dims.set_forced_precision(0, 16, false, 5);
    dims.set_forced_precision(1, -1, false, 0);
    dims.set_forced_precision(2, -1, false, 0);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pfm_le.c_str(), dims, next_comp);
    EXPECT_EQ(3, next_comp);
    EXPECT_EQ(16, dims.get_bit_depth(0));
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
    EXPECT_EQ(2u, ReadFloatRow(reader, 1, 2, 0).size());
    EXPECT_EQ(2u, ReadAbsRow(reader, 2, 2,
                             KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE, 0)
                      .size());
    EXPECT_THROW(ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_SHORTS, 0),
                 kdu_exception);
  }

  TempFile pfm_be("be.pfm");
  write_pfm(pfm_be, false, 3, 1, 2.0f, {-0.5f, 0.5f, 1.5f});
  {
    kdu_image_dims dims;
    dims.set_forced_precision(0, 16, true, 0);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pfm_be.c_str(), dims, next_comp);
    EXPECT_EQ(1, next_comp);
    EXPECT_EQ(3u, ReadAbsRow(reader, 0, 3, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }

  TempFile bad_pfm("bad.pfm");
  bad_pfm.WriteString("PF\n2 2\n0.0\n");
  kdu_image_dims dims;
  int next_comp = 0;
  EXPECT_THROW(OpenReader(bad_pfm.c_str(), dims, next_comp), kdu_exception);
}

TEST(ImageInPgxTest, ReadsSignedUnsignedEndiansAndBitDepths) {
  auto write_pgx = [](const TempFile& f, const std::string& header,
                      int bytes_per_sample, bool le,
                      const std::vector<kdu_uint32>& vals) {
    std::vector<kdu_byte> bytes(header.begin(), header.end());
    for (kdu_uint32 v : vals) {
      AppendUint(bytes, v, bytes_per_sample, le);
    }
    f.WriteBytes(bytes);
  };

  TempFile pgx1("signed_ml_24.pgx");
  write_pgx(pgx1, "PG ML -24 2 1\n", 4, false, {0x001234, 0xFFEDC0});
  {
    SCOPED_TRACE("pgx1");
    kdu_image_dims dims;
    dims.set_forced_precision(0, 16, false, 5);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pgx1.c_str(), dims, next_comp);
    EXPECT_EQ(1, dims.get_height(0));
    EXPECT_EQ(2, dims.get_width(0));
    EXPECT_TRUE(dims.get_signed(0));
    EXPECT_EQ(16, dims.get_bit_depth(0));
    EXPECT_NO_THROW(EXPECT_EQ(
        2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size()));
  }

  TempFile pgx2("unsigned_lm_12.pgx");
  write_pgx(pgx2, "PG LM +12 2 2\n", 2, true, {100, 2000, 300, 4000});
  {
    SCOPED_TRACE("pgx2");
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pgx2.c_str(), dims, next_comp);
    EXPECT_EQ(2, dims.get_height(0));
    EXPECT_EQ(2, dims.get_width(0));
    EXPECT_FALSE(dims.get_signed(0));
    EXPECT_EQ(12, dims.get_bit_depth(0));
    EXPECT_NO_THROW(EXPECT_EQ(2u, ReadFloatRow(reader, 0, 2, 0).size()));
    EXPECT_NO_THROW(
        EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_SHORTS, 0).size()));
  }

  TempFile pgx3("bare_32_ml.pgx");
  write_pgx(pgx3, "PG ML 32 2 1\n", 4, false, {0x10000000U, 0x20000000U});
  {
    SCOPED_TRACE("pgx3");
    kdu_image_dims dims;
    dims.set_forced_precision(0, 8, false, 0);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pgx3.c_str(), dims, next_comp);
    EXPECT_EQ(1, dims.get_height(0));
    EXPECT_EQ(2, dims.get_width(0));
    EXPECT_NO_THROW(EXPECT_EQ(
        2u, ReadAbsRow(reader, 0, 2,
                       KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE, 0)
                .size()));
  }

  TempFile bad_pgx("bad.pgx");
  bad_pgx.WriteString("PG ML +40 2 2\n");
  kdu_image_dims dims;
  int next_comp = 0;
  EXPECT_THROW(OpenReader(bad_pgx.c_str(), dims, next_comp), kdu_exception);
}

TEST(ImageInRawTest, ReadsRawAndRawlAcrossPrecisionsAndFloatForcing) {
  TempFile raw_be("be.raw");
  std::vector<kdu_byte> raw_data = {'S', 'K', 'I', 'P'};
  AppendUint(raw_data, 0x123456, 3, false);
  AppendUint(raw_data, 0x654321, 3, false);
  AppendUint(raw_data, 0x112233, 3, false);
  AppendUint(raw_data, 0x445566, 3, false);
  raw_be.WriteBytes(raw_data);

  {
    kdu_image_dims dims;
    dims.add_component(2, 2, 24, false, 0);
    dims.set_forced_precision(0, 16, true, 0);
    int next_comp = 0;
    ScopedImageIn reader =
        OpenReader(raw_be.c_str(), dims, next_comp, false, nullptr, 4);
    EXPECT_EQ(16, dims.get_bit_depth(0));
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2,
                             KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE, 0)
                      .size());
  }

  TempFile raw_le("le.rawl");
  std::vector<kdu_byte> rawf;
  AppendUint(rawf, FloatToBits(-0.5f), 4, true);
  AppendUint(rawf, FloatToBits(0.75f), 4, true);
  raw_le.WriteBytes(rawf);
  {
    kdu_image_dims dims;
    dims.add_component(1, 2, 32, true, 0);
    dims.set_forced_precision(0, 16, false, 8);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(raw_le.c_str(), dims, next_comp);
    EXPECT_EQ(16, dims.get_bit_depth(0));
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }

  kdu_image_dims empty_dims;
  int next_comp = 0;
  EXPECT_THROW(OpenReader(raw_le.c_str(), empty_dims, next_comp),
               kdu_exception);
  empty_dims.add_component(1, 2, 40, false, 0);
  next_comp = 0;
  EXPECT_THROW(OpenReader(raw_le.c_str(), empty_dims, next_comp),
               kdu_exception);
}

TEST(ImageInBmpTest, Reads24Bit32BitPaletteNibbleAndBitModes) {
  TempFile bmp24("test24.bmp");
  std::vector<kdu_byte> px24 = {
      10, 20, 30, 40, 50, 60, 70, 80, 90, 0, 0, 0,
      11, 21, 31, 41, 51, 61, 71, 81, 91, 0, 0, 0,
  };
  bmp24.WriteBytes(BuildBmp(3, 2, false, 24, 0, 0, {}, px24));
  {
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(bmp24.c_str(), dims, next_comp, true);
    EXPECT_EQ(3, next_comp);
    bool units_known = false;
    double xres = 0.0;
    double yres = 0.0;
    EXPECT_TRUE(dims.get_resolution(0, units_known, xres, yres));
    EXPECT_TRUE(units_known);
    EXPECT_GT(xres, 2800.0);
    for (int r = 0; r < 2; ++r) {
      for (int c = 0; c < 3; ++c) {
        EXPECT_EQ(3u,
                  ReadAbsRow(reader, c, 3, KDU_LINE_BUF_ABSOLUTE, 0).size());
      }
    }
  }

  TempFile bmp32("test32.bmp");
  bmp32.WriteBytes(
      BuildBmp(2, 1, true, 32, 0, 0, {}, {1, 2, 3, 255, 4, 5, 6, 128}));
  {
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(bmp32.c_str(), dims, next_comp);
    EXPECT_GE(next_comp, 3);
    for (int c = 0; c < next_comp; ++c) {
      EXPECT_EQ(2u, ReadFloatRow(reader, c, 2, 0).size());
    }
  }

  TempFile bmp8("test8.bmp");
  std::vector<kdu_byte> pal8(256 * 4, 0);
  for (int i = 0; i < 4; ++i) {
    pal8[i * 4 + 0] = static_cast<kdu_byte>(i * 20);
    pal8[i * 4 + 1] = static_cast<kdu_byte>(i * 40);
    pal8[i * 4 + 2] = static_cast<kdu_byte>(i * 60);
  }
  bmp8.WriteBytes(BuildBmp(2, 1, true, 8, 0, 4, pal8, {0, 1, 0, 0}));
  {
    kdu_image_dims dims;
    kdu_rgb8_palette pal;
    int next_comp = 0;
    ScopedImageIn reader =
        OpenReader(bmp8.c_str(), dims, next_comp, false, &pal);
    EXPECT_EQ(1, next_comp);
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }
  {
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(bmp8.c_str(), dims, next_comp);
    EXPECT_EQ(3, next_comp);
    for (int c = 0; c < 3; ++c) {
      EXPECT_EQ(2u, ReadAbsRow(reader, c, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
    }
  }

  TempFile bmp4("test4.bmp");
  std::vector<kdu_byte> pal4(16 * 4, 0);
  pal4[0] = 10;
  pal4[1] = 20;
  pal4[2] = 30;
  pal4[4] = 40;
  pal4[5] = 50;
  pal4[6] = 60;
  bmp4.WriteBytes(BuildBmp(3, 1, true, 4, 0, 2, pal4, {0x01, 0x10, 0, 0}));
  {
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(bmp4.c_str(), dims, next_comp);
    for (int c = 0; c < 3; ++c) {
      EXPECT_EQ(3u, ReadAbsRow(reader, c, 3, KDU_LINE_BUF_ABSOLUTE, 0).size());
    }
  }
  {
    kdu_image_dims dims;
    kdu_rgb8_palette pal;
    int next_comp = 0;
    ScopedImageIn reader =
        OpenReader(bmp4.c_str(), dims, next_comp, false, &pal);
    EXPECT_EQ(3u, ReadAbsRow(reader, 0, 3, KDU_LINE_BUF_SHORTS, 0).size());
  }

  TempFile bmp1("test1.bmp");
  std::vector<kdu_byte> pal1 = {0, 0, 0, 0, 255, 0, 0, 0};
  bmp1.WriteBytes(BuildBmp(5, 1, true, 1, 0, 2, pal1, {0xA8, 0, 0, 0}));
  {
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(bmp1.c_str(), dims, next_comp);
    for (int c = 0; c < 3; ++c) {
      EXPECT_EQ(5u, ReadAbsRow(reader, c, 5, KDU_LINE_BUF_ABSOLUTE, 0).size());
    }
  }
  {
    kdu_image_dims dims;
    kdu_rgb8_palette pal;
    int next_comp = 0;
    ScopedImageIn reader =
        OpenReader(bmp1.c_str(), dims, next_comp, false, &pal);
    EXPECT_EQ(5u, ReadAbsRow(reader, 0, 5,
                             KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE, 0)
                      .size());
  }

  TempFile bmp_comp("comp.bmp");
  bmp_comp.WriteBytes(BuildBmp(2, 2, false, 8, 1, 0, pal8, {0, 0, 0, 0}));
  kdu_image_dims dims;
  int next_comp = 0;
  EXPECT_THROW(OpenReader(bmp_comp.c_str(), dims, next_comp), kdu_exception);
}

TEST(ImageInTiffTest, StripsTilesEndiansPhotometricBitDepthsAndMetadata) {
  {
    TiffBuilder tb(/*little_endian=*/true, /*bigtiff=*/false);
    tb.AddShort(256, {3});
    tb.AddShort(257, {2});
    tb.AddShort(258, {8, 8, 8});
    tb.AddShort(259, {1});
    tb.AddShort(262, {2});
    tb.AddShort(277, {3});
    tb.AddShort(278, {2});
    tb.AddShort(284, {1});
    tb.AddShort(296, {2});
    tb.AddRational(282, 72, 1);
    tb.AddRational(283, 72, 1);
    std::vector<kdu_byte> icc = {'i', 'c', 'c', '_', 'd', 'a', 't', 'a'};
    tb.AddBytes(34675, kTiffUndefined, icc);
    std::vector<kdu_byte> xmp = {'<', 'x', '/', '>', ' ', 'x'};
    tb.AddBytes(700, kTiffByte, xmp);
    std::vector<kdu_byte> iptc = {0x00, 0x00, 0x01, 0x1C,
                                  0xAA, 0xBB, 0xCC, 0xDD};
    tb.AddBytes(33723, kTiffLong, iptc);
    tb.AddDouble(33550, {1.0, 1.0, 0.0});
    tb.AddAscii(34737, "WGS84");

    std::vector<kdu_byte> chunk = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
    };
    TempFile tif("rgb_meta.tif");
    tif.WriteBytes(tb.BuildWithChunks({chunk}, false, kTiffShort));

    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp);
    EXPECT_EQ(3, next_comp);
    EXPECT_NE(nullptr, dims.get_source_metadata(0));
    for (int r = 0; r < 2; ++r) {
      for (int c = 0; c < 3; ++c) {
        EXPECT_EQ(3u,
                  ReadAbsRow(reader, c, 3, KDU_LINE_BUF_ABSOLUTE, 0).size());
      }
    }
  }

  {
    TiffBuilder tb(/*little_endian=*/false, /*bigtiff=*/true);
    tb.AddLong(256, {10});
    tb.AddLong(257, {2});
    tb.AddShort(258, {12, 12});
    tb.AddShort(259, {1});
    tb.AddShort(262, {0});
    tb.AddShort(266, {2});
    tb.AddShort(277, {2});
    tb.AddShort(284, {2});
    tb.AddShort(338, {2});
    tb.AddShort(322, {4});
    tb.AddShort(323, {2});

    std::vector<std::vector<kdu_byte>> chunks(6,
                                              std::vector<kdu_byte>(12, 0x5A));
    TempFile tif("bigtiff_tiled_12b.tif");
    tif.WriteBytes(tb.BuildWithChunks(chunks, true, kTiffLong8));

    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp);
    EXPECT_EQ(2, next_comp);
    EXPECT_EQ(12, dims.get_bit_depth(0));
    for (int r = 0; r < 2; ++r) {
      for (int c = 0; c < 2; ++c) {
        EXPECT_EQ(4u,
                  ReadAbsRow(reader, c, 4, KDU_LINE_BUF_ABSOLUTE, 0).size());
      }
      for (int c = 0; c < 2; ++c) {
        EXPECT_EQ(4u,
                  ReadAbsRow(reader, c, 4, KDU_LINE_BUF_ABSOLUTE, 1).size());
      }
      for (int c = 0; c < 2; ++c) {
        EXPECT_EQ(2u,
                  ReadAbsRow(reader, c, 2, KDU_LINE_BUF_ABSOLUTE, 2).size());
      }
    }
  }

  {
    TiffBuilder tb(/*little_endian=*/true, /*bigtiff=*/false);
    tb.AddLong(256, {3});
    tb.AddLong(257, {1});
    tb.AddShort(258, {32});
    tb.AddShort(259, {1});
    tb.AddShort(262, {1});
    tb.AddShort(277, {1});
    tb.AddShort(278, {1});
    tb.AddShort(284, {1});
    tb.AddShort(339, {3});
    tb.AddFloat(340, {-1.0f});
    tb.AddFloat(341, {1.0f});
    std::vector<kdu_byte> fchunk;
    AppendUint(fchunk, FloatToBits(-0.5f), 4, true);
    AppendUint(fchunk, FloatToBits(0.0f), 4, true);
    AppendUint(fchunk, FloatToBits(0.75f), 4, true);
    TempFile tif_f("float32.tif");
    tif_f.WriteBytes(tb.BuildWithChunks({fchunk}, false));

    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(tif_f.c_str(), dims, next_comp);
    EXPECT_EQ(3u, ReadFloatRow(reader, 0, 3, 0).size());
  }

  {
    TiffBuilder tb(/*little_endian=*/true, /*bigtiff=*/false);
    tb.AddShort(256, {4});
    tb.AddShort(257, {1});
    tb.AddShort(258, {8});
    tb.AddShort(259, {1});
    tb.AddShort(262, {3});
    tb.AddShort(277, {1});
    tb.AddShort(278, {1});
    tb.AddShort(284, {1});
    std::vector<kdu_uint16> colormap(256 * 3, 0);
    for (int i = 0; i < 4; ++i) {
      colormap[i] = static_cast<kdu_uint16>(i * 10000);
      colormap[256 + i] = static_cast<kdu_uint16>(i * 15000);
      colormap[512 + i] = static_cast<kdu_uint16>(i * 20000);
    }
    tb.AddShort(320, colormap);
    TempFile tif_p("pal.tif");
    tif_p.WriteBytes(tb.BuildWithChunks({{0, 1, 2, 3}}, false));

    {
      kdu_image_dims dims;
      kdu_rgb8_palette pal;
      int next_comp = 0;
      ScopedImageIn reader =
          OpenReader(tif_p.c_str(), dims, next_comp, false, &pal);
      EXPECT_EQ(1, next_comp);
      EXPECT_EQ(4u, ReadAbsRow(reader, 0, 4, KDU_LINE_BUF_ABSOLUTE, 0).size());
    }
    {
      kdu_image_dims dims;
      int next_comp = 0;
      ScopedImageIn reader = OpenReader(tif_p.c_str(), dims, next_comp);
      EXPECT_EQ(3, next_comp);
      for (int c = 0; c < 3; ++c) {
        EXPECT_EQ(4u,
                  ReadAbsRow(reader, c, 4, KDU_LINE_BUF_ABSOLUTE, 0).size());
      }
    }
  }

  {
    TiffBuilder tb(/*little_endian=*/true, /*bigtiff=*/false);
    tb.AddShort(256, {2});
    tb.AddShort(257, {2});
    tb.AddShort(258, {8});
    tb.AddShort(259, {5});
    tb.AddShort(262, {1});
    tb.AddShort(277, {1});
    tb.AddShort(278, {2});
    tb.AddShort(284, {1});
    TempFile tif_lzw("lzw.tif");
    tif_lzw.WriteBytes(tb.BuildWithChunks({{0, 1, 2, 3}}, false));

    kdu_image_dims dims;
    int next_comp = 0;
    EXPECT_THROW(
        OpenReader(tif_lzw.c_str(), dims, next_comp, false, nullptr, 4),
        kdu_exception);
  }
}

void ExerciseRawPrecisionModes(const char* path, int height, int width,
                               int precision, bool is_signed, bool is_raw_be,
                               int forced_prec, bool align_lsbs, int forced_exp,
                               bool expect_short_throw = false) {
  std::vector<kdu_byte> modes = {
      KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE, KDU_LINE_BUF_ABSOLUTE, 0,
      KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE,   KDU_LINE_BUF_SHORTS,
  };
  for (kdu_byte flags : modes) {
    if (forced_exp > 0 && flags == 0) {
      kdu_image_dims dims;
      dims.add_component(height, width, precision, is_signed, 0);
      dims.set_forced_precision(0, forced_prec, align_lsbs, forced_exp);
      int next_comp = 0;
      ScopedImageIn reader = OpenReader(path, dims, next_comp);
      EXPECT_THROW(ReadFloatRow(reader, 0, width, 0), kdu_exception);
      continue;
    }
    if (forced_exp > 0 && (flags & KDU_LINE_BUF_ABSOLUTE) == 0) {
      continue;
    }
    kdu_image_dims dims;
    dims.add_component(height, width, precision, is_signed, 0);
    if (forced_prec != 0) {
      dims.set_forced_precision(0, forced_prec, align_lsbs, forced_exp);
    }
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(path, dims, next_comp);
    if (expect_short_throw && (flags & KDU_LINE_BUF_SHORTS) != 0) {
      std::vector<kdu_int64> out;
      EXPECT_THROW(ReadLineValues64(reader, 0, width, flags, 0, &out, nullptr),
                   kdu_exception);
      continue;
    }
    for (int r = 0; r < height; ++r) {
      if (flags == 0) {
        EXPECT_EQ(static_cast<size_t>(width),
                  ReadFloatRow(reader, 0, width, 0).size());
      } else {
        EXPECT_EQ(static_cast<size_t>(width),
                  ReadAbsRow(reader, 0, width, flags, 0).size());
      }
    }
    (void)is_raw_be;
  }
}

TEST(ImageInRawTest, AllWordSizesEndiansAndFloatForcingBranches) {
  struct WordCase {
    int prec;
    int bytes;
    bool is_signed;
    std::vector<kdu_uint32> vals;
  };
  const std::vector<WordCase> cases = {
      {6, 1, false, {0, 15, 63}},
      {8, 1, true, {0xF0, 0x00, 0x10}},
      {15, 2, false, {10, 1000, 32767}},
      {16, 2, true, {0xFF00, 0x0000, 0x0100}},
      {24, 3, false, {1, 0x123456, 0xFFFFFF}},
      {30, 4, true, {0x00000010U, 0x00100000U, 0x3FFFFFFFU}},
  };
  for (bool little_endian : {false, true}) {
    for (size_t i = 0; i < cases.size(); ++i) {
      const auto& c = cases[i];
      std::string fname =
          "word_" + std::to_string(i) + (little_endian ? ".rawl" : ".raw");
      TempFile f(fname);
      std::vector<kdu_byte> data;
      for (kdu_uint32 v : c.vals) {
        AppendUint(data, v, c.bytes, little_endian);
      }
      f.WriteBytes(data);
      ExerciseRawPrecisionModes(f.c_str(), 1, 3, c.prec, c.is_signed,
                                !little_endian, 0, false, 0, c.bytes > 2);
    }
  }

  for (bool little_endian : {false, true}) {
    std::string f32_name = little_endian ? "f32_force.rawl" : "f32_force.raw";
    TempFile f32(f32_name);
    std::vector<kdu_byte> d32;
    AppendUint(d32, FloatToBits(-1.5f), 4, little_endian);
    AppendUint(d32, FloatToBits(0.0f), 4, little_endian);
    AppendUint(d32, FloatToBits(2.25f), 4, little_endian);
    f32.WriteBytes(d32);

    std::string f16_name = little_endian ? "f16_force.rawl" : "f16_force.raw";
    TempFile f16(f16_name);
    std::vector<kdu_byte> d16;
    AppendUint(d16, 0x8010, 2, little_endian);
    AppendUint(d16, 0x0000, 2, little_endian);
    AppendUint(d16, 0x0010, 2, little_endian);
    f16.WriteBytes(d16);

    std::string f12_name = little_endian ? "f12_force.rawl" : "f12_force.raw";
    TempFile f12(f12_name);
    std::vector<kdu_byte> d12;
    AppendUint(d12, 0x001, 2, little_endian);
    AppendUint(d12, 0x010, 2, little_endian);
    AppendUint(d12, 0x020, 2, little_endian);
    f12.WriteBytes(d12);

    for (bool is_signed : {false, true}) {
      ExerciseRawPrecisionModes(f32.c_str(), 1, 3, 32, is_signed,
                                !little_endian, 16, false, 8, true);
      ExerciseRawPrecisionModes(f32.c_str(), 1, 3, 32, is_signed,
                                !little_endian, 32, false, 8, true);
      ExerciseRawPrecisionModes(f16.c_str(), 1, 3, 16, is_signed,
                                !little_endian, 24, false, 5);
      ExerciseRawPrecisionModes(f16.c_str(), 1, 3, 16, is_signed,
                                !little_endian, 16, false, 5);
      ExerciseRawPrecisionModes(f16.c_str(), 1, 3, 16, is_signed,
                                !little_endian, 12, false, 5);
      ExerciseRawPrecisionModes(f12.c_str(), 1, 3, 12, is_signed,
                                !little_endian, 16, false, 4);
    }
  }
}

TEST(ImageInForcePrecisionTest, CoversAllBufferModesAndReinterpretBranches) {
  auto write_pgx = [](const TempFile& f, const std::string& header, int bps,
                      const std::vector<kdu_uint32>& vals) {
    std::vector<kdu_byte> bytes(header.begin(), header.end());
    for (kdu_uint32 v : vals) {
      AppendUint(bytes, v, bps, false);
    }
    f.WriteBytes(bytes);
  };

  TempFile pgx_u12("u12_force.pgx");
  write_pgx(pgx_u12, "PG ML +12 4 1\n", 2, {0, 100, 2000, 4095});
  TempFile pgx_s12("s12_force.pgx");
  write_pgx(pgx_s12, "PG ML -12 4 1\n", 2,
            {static_cast<kdu_uint32>(-2048 & 0xFFFF),
             static_cast<kdu_uint32>(-100 & 0xFFFF), 100, 2047});

  const std::vector<kdu_byte> all_modes = {
      KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE, KDU_LINE_BUF_ABSOLUTE, 0,
      KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE,   KDU_LINE_BUF_SHORTS,
  };

  struct ForceConfig {
    const char* path;
    int forced_prec;
    bool align_lsbs;
  };
  const std::vector<ForceConfig> configs = {
      {pgx_u12.c_str(), 16, true},  {pgx_u12.c_str(), 16, false},
      {pgx_u12.c_str(), 8, true},   {pgx_u12.c_str(), 8, false},
      {pgx_u12.c_str(), 12, false}, {pgx_s12.c_str(), 16, true},
      {pgx_s12.c_str(), 16, false}, {pgx_s12.c_str(), 8, true},
      {pgx_s12.c_str(), 8, false},
  };
  for (const auto& cfg : configs) {
    for (kdu_byte flags : all_modes) {
      kdu_image_dims dims;
      dims.set_forced_precision(0, cfg.forced_prec, cfg.align_lsbs, 0);
      int next_comp = 0;
      ScopedImageIn reader = OpenReader(cfg.path, dims, next_comp);
      if (flags == 0) {
        EXPECT_EQ(4u, ReadFloatRow(reader, 0, 4, 0).size());
      } else {
        EXPECT_EQ(4u, ReadAbsRow(reader, 0, 4, flags, 0).size());
      }
    }
  }

  for (const char* path : {pgx_u12.c_str(), pgx_s12.c_str()}) {
    for (int fexp : {5, 10}) {
      for (int fprec : {16, 28, 32}) {
        if (fprec <= fexp) continue;
        std::vector<kdu_byte> modes = {KDU_LINE_BUF_ABSOLUTE};
        if (fprec <= 16) {
          modes.push_back(KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE);
        }
        for (kdu_byte flags : modes) {
          kdu_image_dims dims;
          dims.set_forced_precision(0, fprec, false, fexp);
          int next_comp = 0;
          ScopedImageIn reader = OpenReader(path, dims, next_comp);
          EXPECT_EQ(4u, ReadAbsRow(reader, 0, 4, flags, 0).size());
        }
      }
    }
  }
}

TEST(ImageInPbmPgmPpmBufferTest, CoversUnrolledAndFixedPointPaths) {
  const std::vector<kdu_byte> all_modes = {
      KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE, KDU_LINE_BUF_ABSOLUTE, 0,
      KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE,   KDU_LINE_BUF_SHORTS,
  };

  TempFile pbm12("pbm12.pbm");
  pbm12.WriteBytes({'P', '4', '\n', '1', '2', ' ', '1', '\n', 0xAA, 0x50});
  for (kdu_byte flags : all_modes) {
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pbm12.c_str(), dims, next_comp);
    if (flags == 0) {
      EXPECT_EQ(12u, ReadFloatRow(reader, 0, 12, 0).size());
    } else {
      EXPECT_EQ(12u, ReadAbsRow(reader, 0, 12, flags, 0).size());
    }
  }

  TempFile pgm_6b("pgm_6b.pgm");
  pgm_6b.WriteBytes(
      {'P', '5', '\n', '5', ' ', '1', '\n', '6', '3', '\n', 0, 10, 20, 40, 63});
  TempFile pgm_8b("pgm_8b.pgm");
  pgm_8b.WriteBytes({'P', '5', '\n', '5', ' ', '1', '\n', '2', '5', '5', '\n',
                     0, 64, 128, 192, 255});
  TempFile pgm_16b("pgm_16b.pgm");
  std::vector<kdu_byte> pgm16_data = {'P', '5', '\n', '5', ' ', '1', '\n',
                                      '6', '5', '5',  '3', '5', '\n'};
  for (int i = 0; i < 5; ++i) {
    AppendUint(pgm16_data, i * 10000, 2, false);
  }
  pgm_16b.WriteBytes(pgm16_data);

  for (const char* path : {pgm_6b.c_str(), pgm_8b.c_str(), pgm_16b.c_str()}) {
    for (kdu_byte flags : all_modes) {
      kdu_image_dims dims;
      int next_comp = 0;
      ScopedImageIn reader = OpenReader(path, dims, next_comp);
      if (flags == 0) {
        EXPECT_EQ(5u, ReadFloatRow(reader, 0, 5, 0).size());
      } else {
        EXPECT_EQ(5u, ReadAbsRow(reader, 0, 5, flags, 0).size());
      }
    }
  }

  TempFile ppm_6b("ppm_6b.ppm");
  std::vector<kdu_byte> ppm6_data = {'P', '6',  '\n', '5', ' ',
                                     '1', '\n', '6',  '3', '\n'};
  for (int i = 0; i < 15; ++i) ppm6_data.push_back((i * 4) & 63);
  ppm_6b.WriteBytes(ppm6_data);

  TempFile ppm_8b("ppm_8b.ppm");
  std::vector<kdu_byte> ppm8_data = {'P',  '6', '\n', '5', ' ', '1',
                                     '\n', '2', '5',  '5', '\n'};
  for (int i = 0; i < 15; ++i) ppm8_data.push_back(i * 15);
  ppm_8b.WriteBytes(ppm8_data);

  TempFile ppm_16b("ppm_16b.ppm");
  std::vector<kdu_byte> ppm16_data = {'P', '6', '\n', '5', ' ', '1', '\n',
                                      '6', '5', '5',  '3', '5', '\n'};
  for (int i = 0; i < 15; ++i) AppendUint(ppm16_data, i * 4000, 2, false);
  ppm_16b.WriteBytes(ppm16_data);

  for (const char* path : {ppm_6b.c_str(), ppm_8b.c_str(), ppm_16b.c_str()}) {
    for (kdu_byte flags : all_modes) {
      kdu_image_dims dims;
      int next_comp = 0;
      ScopedImageIn reader = OpenReader(path, dims, next_comp);
      for (int c = 0; c < 3; ++c) {
        if (flags == 0) {
          EXPECT_EQ(5u, ReadFloatRow(reader, c, 5, 0).size());
        } else {
          EXPECT_EQ(5u, ReadAbsRow(reader, c, 5, flags, 0).size());
        }
      }
    }
  }
}

TEST(ImageInFloatTiffAndPfmTest, CoversFloatConversionsAndReinterpret64) {
  for (bool little_endian : {false, true}) {
    for (bool is_signed : {false, true}) {
      TiffBuilder tb(little_endian, /*bigtiff=*/false);
      tb.AddLong(256, {3});
      tb.AddLong(257, {1});
      tb.AddShort(258, {32});
      tb.AddShort(259, {1});
      tb.AddShort(262, {1});
      tb.AddShort(277, {1});
      tb.AddShort(278, {1});
      tb.AddShort(284, {1});
      tb.AddShort(339, {3});
      if (is_signed) {
        tb.AddDouble(340, {-1.0});
        tb.AddDouble(341, {1.0});
      } else {
        tb.AddDouble(341, {1.0});
      }
      // Exercise SMin > SMax to ensure default range handling without crash
      std::vector<kdu_byte> fchunk;
      AppendUint(fchunk, FloatToBits(-2.0f), 4, little_endian);
      AppendUint(fchunk, FloatToBits(0.5f), 4, little_endian);
      AppendUint(fchunk, FloatToBits(1.0e20f), 4, little_endian);

      TempFile tif(std::string("flt_") + (little_endian ? "le_" : "be_") +
                   (is_signed ? "s.tif" : "u.tif"));
      tif.WriteBytes(tb.BuildWithChunks({fchunk}, false));

      for (kdu_byte flags :
           {kdu_byte{KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE},
            kdu_byte{KDU_LINE_BUF_ABSOLUTE}, kdu_byte{0}}) {
        kdu_image_dims dims;
        int next_comp = 0;
        ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp);
        if (flags == 0) {
          EXPECT_EQ(3u, ReadFloatRow(reader, 0, 3, 0).size());
        } else {
          EXPECT_EQ(3u, ReadAbsRow(reader, 0, 3, flags, 0).size());
        }
      }

      {
        kdu_image_dims dims;
        int next_comp = 0;
        ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp);
        std::vector<kdu_int64> out;
        EXPECT_THROW(
            ReadLineValues64(reader, 0, 3,
                             KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE, 0,
                             &out, nullptr),
            kdu_exception);
      }

      for (int fprec : {16, 32}) {
        int fexp = (fprec == 16) ? 5 : 5;
        for (kdu_byte flags :
             {kdu_byte{KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE},
              kdu_byte{KDU_LINE_BUF_ABSOLUTE}}) {
          kdu_image_dims dims;
          dims.set_forced_precision(0, fprec, false, fexp);
          int next_comp = 0;
          ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp);
          EXPECT_EQ(3u, ReadAbsRow(reader, 0, 3, flags, 0).size());
        }
      }
      {
        kdu_image_dims dims;
        dims.set_forced_precision(0, 32, false, 10);
        int next_comp = 0;
        ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp);
        EXPECT_EQ(3u,
                  ReadAbsRow(reader, 0, 3,
                             KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE, 0)
                      .size());
      }
    }
  }

  // Float TIFF missing both SminSampleValue and SmaxSampleValue triggers
  // warning fallback and allows precision forcing without exponent bits.
  {
    TiffBuilder tb(/*little_endian=*/true, /*bigtiff=*/false);
    tb.AddLong(256, {2});
    tb.AddLong(257, {1});
    tb.AddShort(258, {32});
    tb.AddShort(259, {1});
    tb.AddShort(262, {1});
    tb.AddShort(277, {1});
    tb.AddShort(278, {1});
    tb.AddShort(284, {1});
    tb.AddShort(339, {3});
    std::vector<kdu_byte> fchunk;
    AppendUint(fchunk, FloatToBits(-0.25f), 4, true);
    AppendUint(fchunk, FloatToBits(0.25f), 4, true);
    TempFile tif("flt_norange.tif");
    tif.WriteBytes(tb.BuildWithChunks({fchunk}, false));

    kdu_image_dims dims;
    dims.set_forced_precision(0, 12, true, 0);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp);
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }

  // PFM with VPRECISE + 32-bit float forcing and buf16 error path.
  TempFile pfm("vprec.pfm");
  std::string hdr = "Pf\n2 1\n1.0\n";
  std::vector<kdu_byte> pbytes(hdr.begin(), hdr.end());
  AppendUint(pbytes, FloatToBits(-1.0f), 4, false);
  AppendUint(pbytes, FloatToBits(0.5f), 4, false);
  pfm.WriteBytes(pbytes);
  {
    kdu_image_dims dims;
    dims.set_forced_precision(0, 32, false, 5);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pfm.c_str(), dims, next_comp);
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2,
                             KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE, 0)
                      .size());
  }
  {
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pfm.c_str(), dims, next_comp);
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2,
                             KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE, 0)
                      .size());
  }
  {
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pfm.c_str(), dims, next_comp);
    std::vector<kdu_int64> out;
    EXPECT_THROW(ReadLineValues64(reader, 0, 2,
                                  KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE,
                                  0, &out, nullptr),
                 kdu_exception);
  }
}

TEST(ImageInBmpModesTest, CoversMonoPalettesAllBuffersAndTopDown) {
  const std::vector<kdu_byte> all_modes = {
      KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE, KDU_LINE_BUF_ABSOLUTE, 0,
      KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE,   KDU_LINE_BUF_SHORTS,
  };

  TempFile bmp_td("topdown24.bmp");
  bmp_td.WriteBytes(
      BuildBmp(2, 1, true, 24, 0, 0, {}, {10, 20, 30, 40, 50, 60, 0, 0}));
  for (kdu_byte flags : all_modes) {
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(bmp_td.c_str(), dims, next_comp);
    for (int c = 0; c < 3; ++c) {
      if (flags == 0) {
        EXPECT_EQ(2u, ReadFloatRow(reader, c, 2, 0).size());
      } else {
        EXPECT_EQ(2u, ReadAbsRow(reader, c, 2, flags, 0).size());
      }
    }
  }

  std::vector<kdu_byte> gray4(16 * 4, 0);
  for (int i = 0; i < 4; ++i) {
    gray4[i * 4 + 0] = gray4[i * 4 + 1] = gray4[i * 4 + 2] =
        static_cast<kdu_byte>(i * 50);
  }
  TempFile bmp4g("gray4.bmp");
  bmp4g.WriteBytes(BuildBmp(3, 1, true, 4, 0, 4, gray4, {0x01, 0x20, 0, 0}));
  for (bool use_pal : {false, true}) {
    for (kdu_byte flags : all_modes) {
      kdu_image_dims dims;
      kdu_rgb8_palette pal;
      int next_comp = 0;
      ScopedImageIn reader = OpenReader(bmp4g.c_str(), dims, next_comp, false,
                                        use_pal ? &pal : nullptr);
      if (flags == 0) {
        EXPECT_EQ(3u, ReadFloatRow(reader, 0, 3, 0).size());
      } else {
        EXPECT_EQ(3u, ReadAbsRow(reader, 0, 3, flags, 0).size());
      }
    }
  }

  std::vector<kdu_byte> gray1 = {0, 0, 0, 0, 255, 255, 255, 0};
  TempFile bmp1g("gray1.bmp");
  bmp1g.WriteBytes(BuildBmp(5, 1, true, 1, 0, 2, gray1, {0xA8, 0, 0, 0}));
  for (bool use_pal : {false, true}) {
    for (kdu_byte flags : all_modes) {
      kdu_image_dims dims;
      kdu_rgb8_palette pal;
      int next_comp = 0;
      ScopedImageIn reader = OpenReader(bmp1g.c_str(), dims, next_comp, false,
                                        use_pal ? &pal : nullptr);
      if (flags == 0) {
        EXPECT_EQ(5u, ReadFloatRow(reader, 0, 5, 0).size());
      } else {
        EXPECT_EQ(5u, ReadAbsRow(reader, 0, 5, flags, 0).size());
      }
    }
  }
}

TEST(ImageInTiffAdvancedTest, CoversInvertCropUnpackPaletteAndSeparated) {
  const std::vector<kdu_byte> all_modes = {
      KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE, KDU_LINE_BUF_ABSOLUTE, 0,
      KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE,   KDU_LINE_BUF_SHORTS,
  };

  // WHITEISZERO across all 5 buffers to cover invert_line.
  {
    TiffBuilder tb(/*little_endian=*/true, /*bigtiff=*/false);
    tb.AddShort(256, {4});
    tb.AddShort(257, {1});
    tb.AddShort(258, {8});
    tb.AddShort(259, {1});
    tb.AddShort(262, {0});
    tb.AddShort(277, {1});
    tb.AddShort(278, {1});
    tb.AddShort(284, {1});
    TempFile tif("white_zero.tif");
    tif.WriteBytes(tb.BuildWithChunks({{10, 50, 100, 200}}, false));
    for (kdu_byte flags : all_modes) {
      kdu_image_dims dims;
      int next_comp = 0;
      ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp);
      if (flags == 0) {
        EXPECT_EQ(4u, ReadFloatRow(reader, 0, 4, 0).size());
      } else {
        EXPECT_EQ(4u, ReadAbsRow(reader, 0, 4, flags, 0).size());
      }
    }
  }

  // 1-bit GRAYSCALE & WHITEISZERO with palette.
  for (kdu_uint16 photo : {kdu_uint16{0}, kdu_uint16{1}}) {
    TiffBuilder tb(true, false);
    tb.AddShort(256, {5});
    tb.AddShort(257, {1});
    tb.AddShort(258, {1});
    tb.AddShort(259, {1});
    tb.AddShort(262, {photo});
    tb.AddShort(277, {1});
    tb.AddShort(278, {1});
    tb.AddShort(284, {1});
    TempFile tif(photo == 0 ? "bilevel_w.tif" : "bilevel_b.tif");
    tif.WriteBytes(tb.BuildWithChunks({{0xA8}}, false));
    kdu_image_dims dims;
    kdu_rgb8_palette pal;
    int next_comp = 0;
    ScopedImageIn reader =
        OpenReader(tif.c_str(), dims, next_comp, false, &pal);
    EXPECT_TRUE(pal.exists());
    EXPECT_EQ(5u, ReadAbsRow(reader, 0, 5, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }

  // 4-bit tiled palette TIFF with cropping across multiple tiles.
  {
    TiffBuilder tb(true, false);
    tb.AddLong(256, {10});
    tb.AddLong(257, {4});
    tb.AddShort(258, {4});
    tb.AddShort(259, {1});
    tb.AddShort(262, {3});
    tb.AddShort(277, {1});
    tb.AddShort(284, {1});
    tb.AddShort(322, {4});
    tb.AddShort(323, {2});
    std::vector<kdu_uint16> cmap(16 * 3, 0);
    for (int i = 0; i < 16; ++i) {
      cmap[i] = static_cast<kdu_uint16>(i * 4000);
      cmap[16 + i] = static_cast<kdu_uint16>(i * 3000);
      cmap[32 + i] = static_cast<kdu_uint16>(i * 2000);
    }
    tb.AddShort(320, cmap);
    std::vector<std::vector<kdu_byte>> tiles(6, std::vector<kdu_byte>(4, 0x12));
    TempFile tif("pal4_crop.tif");
    tif.WriteBytes(tb.BuildWithChunks(tiles, true));

    for (bool with_pal : {true, false}) {
      kdu_image_dims dims;
      dims.set_cropping(1, 6, 2, 4, 0);
      kdu_rgb8_palette pal;
      int next_comp = 0;
      ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp, false,
                                        with_pal ? &pal : nullptr);
      int ncomps = with_pal ? 1 : 3;
      EXPECT_EQ(ncomps, next_comp);
      EXPECT_EQ(4, dims.get_width(0));
      EXPECT_EQ(2, dims.get_height(0));
      for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < ncomps; ++c) {
          EXPECT_EQ(4u,
                    ReadAbsRow(reader, c, 4, KDU_LINE_BUF_ABSOLUTE, 0).size());
        }
      }
    }
  }

  // 24-bit TIFF to exercise sample_bytes == 4 unpack path.
  {
    TiffBuilder tb(true, false);
    tb.AddShort(256, {2});
    tb.AddShort(257, {1});
    tb.AddShort(258, {24});
    tb.AddShort(259, {1});
    tb.AddShort(262, {1});
    tb.AddShort(277, {1});
    tb.AddShort(278, {1});
    tb.AddShort(284, {1});
    tb.AddShort(339, {2});
    TempFile tif("signed_24b.tif");
    tif.WriteBytes(
        tb.BuildWithChunks({{0x12, 0x34, 0x56, 0xAB, 0xCD, 0xEF}}, false));
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp);
    EXPECT_TRUE(dims.get_signed(0));
    EXPECT_EQ(24, dims.get_bit_depth(0));
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }

  // Palette + alpha (num_components == 4) in contiguous & planar layouts.
  for (kdu_uint16 planar : {kdu_uint16{1}, kdu_uint16{2}}) {
    TiffBuilder tb(true, false);
    tb.AddShort(256, {3});
    tb.AddShort(257, {1});
    tb.AddShort(258, {8, 8});
    tb.AddShort(259, {1});
    tb.AddShort(262, {3});
    tb.AddShort(277, {2});
    tb.AddShort(278, {1});
    tb.AddShort(284, {planar});
    tb.AddShort(338, {1});
    std::vector<kdu_uint16> cmap(256 * 3, 0);
    for (int i = 0; i < 4; ++i) {
      cmap[i] = static_cast<kdu_uint16>(i * 10000);
      cmap[256 + i] = static_cast<kdu_uint16>(i * 12000);
      cmap[512 + i] = static_cast<kdu_uint16>(i * 14000);
    }
    tb.AddShort(320, cmap);
    TempFile tif(planar == 1 ? "pal_alpha_c.tif" : "pal_alpha_p.tif");
    if (planar == 1) {
      tif.WriteBytes(tb.BuildWithChunks({{0, 255, 1, 200, 2, 128}}, false));
    } else {
      tif.WriteBytes(tb.BuildWithChunks({{0, 1, 2}, {255, 200, 128}}, false));
    }
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp);
    EXPECT_EQ(4, next_comp);
    for (int c = 0; c < 4; ++c) {
      EXPECT_EQ(3u, ReadAbsRow(reader, c, 3, KDU_LINE_BUF_ABSOLUTE, 0).size());
    }
  }

  // SEPARATED CMYK TIFF.
  {
    TiffBuilder tb(true, false);
    tb.AddShort(256, {2});
    tb.AddShort(257, {1});
    tb.AddShort(258, {8, 8, 8, 8});
    tb.AddShort(259, {1});
    tb.AddShort(262, {5});
    tb.AddShort(277, {4});
    tb.AddShort(278, {1});
    tb.AddShort(284, {1});
    tb.AddShort(332, {1});
    tb.AddShort(334, {4});
    TempFile tif("cmyk.tif");
    tif.WriteBytes(
        tb.BuildWithChunks({{10, 20, 30, 40, 50, 60, 70, 80}}, false));
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp);
    EXPECT_EQ(4, next_comp);
    for (int c = 0; c < 4; ++c) {
      EXPECT_EQ(2u, ReadAbsRow(reader, c, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
    }
  }
}

void ExerciseForcedParamClampingAndPartialDestroy(const char* path,
                                                  int expected_comps,
                                                  bool is_raw,
                                                  kdu_long skip_bytes) {
  struct ParamCase {
    int prec;
    bool align;
    int exp;
  };
  const std::vector<ParamCase> cases = {
      {40, false, 0}, {20, false, 18}, {-1, false, 5},
      {3, false, 5},  {-1, false, 0},
  };
  for (const auto& pc : cases) {
    kdu_image_dims dims;
    if (is_raw) {
      dims.add_component(2, 4, 8, false, 0);
    }
    dims.set_forced_precision(0, pc.prec, pc.align, pc.exp);
    int next_comp = 0;
    ScopedImageIn reader =
        OpenReader(path, dims, next_comp, false, nullptr, skip_bytes);
    EXPECT_EQ(expected_comps, next_comp);
    EXPECT_EQ(1u, ReadAbsRow(reader, 0, 1, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }
}

void ExerciseMultiTileAndTruncatedEof(const char* path, int num_comps) {
  kdu_image_dims dims;
  int next_comp = 0;
  ScopedImageIn reader = OpenReader(path, dims, next_comp);
  for (int r = 0; r < 2; ++r) {
    for (int c = 0; c < num_comps; ++c) {
      EXPECT_EQ(2u, ReadAbsRow(reader, c, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
    }
    for (int c = 0; c < num_comps; ++c) {
      EXPECT_EQ(2u, ReadAbsRow(reader, c, 2, KDU_LINE_BUF_ABSOLUTE, 1).size());
    }
    for (int c = 0; c < num_comps; ++c) {
      std::vector<kdu_int64> empty_out;
      EXPECT_TRUE(ReadLineValues64(reader, c, 0, KDU_LINE_BUF_ABSOLUTE, 2,
                                   &empty_out, nullptr));
    }
  }
  std::vector<kdu_int64> past_eof;
  EXPECT_FALSE(ReadLineValues64(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0,
                                &past_eof, nullptr));
}

TEST(ImageInLifecycleWarningsAndErrorsTest,
     CoversAllFormatsSkipBytesDestructorsEmptyTilesAndErrors) {
  TempFile pbm("skip_pbm.pbm");
  pbm.WriteBytes(
      {'S', 'K', 'I', 'P', 'P', '4', '\n', '4', ' ', '2', '\n', 0xF0, 0x0F});
  TempFile pgm("skip_pgm.pgm");
  pgm.WriteBytes({'S', 'K', 'I',  'P', 'P', '5', '\n', '4', ' ', '2', '\n', '2',
                  '5', '5', '\n', 1,   2,   3,   4,    5,   6,   7,   8});
  TempFile ppm("skip_ppm.ppm");
  std::vector<kdu_byte> ppm_bytes = {'S', 'K', 'I',  'P', 'P', '6', '\n', '4',
                                     ' ', '2', '\n', '2', '5', '5', '\n'};
  ppm_bytes.insert(ppm_bytes.end(), 24, 42);
  ppm.WriteBytes(ppm_bytes);

  TempFile pfm("skip_pfm.pfm");
  std::string pfm_hdr = "SKIPPf\n4 2\n-1.0\n";
  std::vector<kdu_byte> pfm_bytes(pfm_hdr.begin(), pfm_hdr.end());
  for (int i = 0; i < 8; ++i) AppendUint(pfm_bytes, FloatToBits(0.5f), 4, true);
  pfm.WriteBytes(pfm_bytes);

  TempFile pgx("skip_pgx.pgx");
  std::string pgx_hdr = "SKIPPG ML +8 4 2\n";
  std::vector<kdu_byte> pgx_bytes(pgx_hdr.begin(), pgx_hdr.end());
  pgx_bytes.insert(pgx_bytes.end(), 8, 77);
  pgx.WriteBytes(pgx_bytes);

  TempFile raw("plain.raw");
  raw.WriteBytes({1, 2, 3, 4, 5, 6, 7, 8});

  std::vector<kdu_byte> bmp_raw =
      BuildBmp(4, 2, false, 8, 0, 2,
               [] {
                 std::vector<kdu_byte> pal(256 * 4, 0);
                 pal[4] = pal[5] = pal[6] = 200;
                 return pal;
               }(),
               {0, 1, 0, 1, 1, 0, 1, 0});
  std::vector<kdu_byte> bmp_skip = {'S', 'K', 'I', 'P'};
  bmp_skip.insert(bmp_skip.end(), bmp_raw.begin(), bmp_raw.end());
  TempFile bmp("skip_bmp.bmp");
  bmp.WriteBytes(bmp_skip);

  TiffBuilder tb(true, false);
  tb.AddShort(256, {4});
  tb.AddShort(257, {2});
  tb.AddShort(258, {8});
  tb.AddShort(259, {1});
  tb.AddShort(262, {1});
  tb.AddShort(277, {1});
  tb.AddShort(278, {2});
  tb.AddShort(284, {1});
  auto tif_raw = tb.BuildWithChunks({{1, 2, 3, 4, 5, 6, 7, 8}}, false);
  TempFile tif_noskip("noskip_tif.tif");
  tif_noskip.WriteBytes(tif_raw);

  ExerciseForcedParamClampingAndPartialDestroy(pbm.c_str(), 1, false, 4);
  ExerciseForcedParamClampingAndPartialDestroy(pgm.c_str(), 1, false, 4);
  ExerciseForcedParamClampingAndPartialDestroy(ppm.c_str(), 3, false, 4);
  ExerciseForcedParamClampingAndPartialDestroy(pfm.c_str(), 1, false, 4);
  ExerciseForcedParamClampingAndPartialDestroy(pgx.c_str(), 1, false, 4);
  ExerciseForcedParamClampingAndPartialDestroy(raw.c_str(), 1, true, 0);
  ExerciseForcedParamClampingAndPartialDestroy(bmp.c_str(), 1, false, 4);
  ExerciseForcedParamClampingAndPartialDestroy(tif_noskip.c_str(), 1, false, 0);
  {
    kdu_image_dims skip_dims;
    int skip_next = 0;
    EXPECT_THROW(
        OpenReader(tif_noskip.c_str(), skip_dims, skip_next, false, nullptr, 4),
        kdu_exception);
  }

  // Full unskipped copies to test 2x4 multi-tile trailing empty tiles and EOF.
  TempFile pbm_u("u_pbm.pbm");
  pbm_u.WriteBytes({'P', '4', '\n', '4', ' ', '2', '\n', 0xF0, 0x0F});
  TempFile pgm_u("u_pgm.pgm");
  pgm_u.WriteBytes({'P', '5', '\n', '4', ' ', '2', '\n', '2', '5', '5', '\n', 1,
                    2, 3, 4, 5, 6, 7, 8});
  TempFile ppm_u("u_ppm.ppm");
  std::vector<kdu_byte> ppm_u_bytes = {'P',  '6', '\n', '4', ' ', '2',
                                       '\n', '2', '5',  '5', '\n'};
  ppm_u_bytes.insert(ppm_u_bytes.end(), 24, 42);
  ppm_u.WriteBytes(ppm_u_bytes);
  TempFile pfm_u("u_pfm.pfm");
  std::string pfm_u_hdr = "PF\n4 2\n-1.0\n";
  std::vector<kdu_byte> pfm_u_bytes(pfm_u_hdr.begin(), pfm_u_hdr.end());
  for (int i = 0; i < 24; ++i) {
    AppendUint(pfm_u_bytes, FloatToBits(0.5f), 4, true);
  }
  pfm_u.WriteBytes(pfm_u_bytes);
  TempFile pgx_u("u_pgx.pgx");
  std::string pgx_u_hdr = "PG ML +8 4 2\n";
  std::vector<kdu_byte> pgx_u_bytes(pgx_u_hdr.begin(), pgx_u_hdr.end());
  pgx_u_bytes.insert(pgx_u_bytes.end(), 8, 77);
  pgx_u.WriteBytes(pgx_u_bytes);
  TempFile bmp_u("u_bmp.bmp");
  bmp_u.WriteBytes(bmp_raw);
  TempFile tif_u("u_tif.tif");
  tif_u.WriteBytes(tif_raw);

  ExerciseMultiTileAndTruncatedEof(pbm_u.c_str(), 1);
  ExerciseMultiTileAndTruncatedEof(pgm_u.c_str(), 1);
  ExerciseMultiTileAndTruncatedEof(ppm_u.c_str(), 3);
  ExerciseMultiTileAndTruncatedEof(pfm_u.c_str(), 3);
  ExerciseMultiTileAndTruncatedEof(pgx_u.c_str(), 1);
  ExerciseMultiTileAndTruncatedEof(bmp_u.c_str(), 1);
  ExerciseMultiTileAndTruncatedEof(tif_u.c_str(), 1);

  {
    kdu_image_dims dims;
    dims.add_component(2, 4, 8, false, 0);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(raw.c_str(), dims, next_comp);
    for (int r = 0; r < 2; ++r) {
      EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
      EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 1).size());
      std::vector<kdu_int64> empty_out;
      EXPECT_TRUE(ReadLineValues64(reader, 0, 0, KDU_LINE_BUF_ABSOLUTE, 2,
                                   &empty_out, nullptr));
    }
    std::vector<kdu_int64> past_eof;
    EXPECT_FALSE(ReadLineValues64(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0,
                                  &past_eof, nullptr));
  }

  // BMP 8-bit monochrome expand_palette (num_components == 1) + forced prec.
  for (int fprec : {6, 16}) {
    kdu_image_dims dims;
    dims.set_forced_precision(0, fprec, fprec == 6, fprec == 16 ? 5 : 0);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(bmp_u.c_str(), dims, next_comp);
    EXPECT_EQ(4u, ReadAbsRow(reader, 0, 4, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }

  // Bad headers & truncated pixel data error checks (no heap leaks).
  const std::vector<std::pair<std::string, std::string>> bad_headers = {
      {"bad_h1.pbm", "P4\n0 2\n"},       {"bad_h2.pbm", "P4\nabc\n"},
      {"bad_h1.pgm", "P5\nabc\n"},       {"bad_h1.ppm", "P6\n0 1\n255\n"},
      {"bad_h2.ppm", "P6\nabc\n"},       {"bad_h1.pfm", "PX\n2 2\n-1.0\n"},
      {"bad_h2.pfm", "Pf\n0 2\n-1.0\n"}, {"bad_h1.pgx", "XX ML +8 2 2\n"},
      {"bad_h2.pgx", "PG XX +8 2 2\n"},  {"bad_h3.pgx", "PG ML +8 0 2\n"},
      {"bad_h1.bmp", "NOTBMP"},
  };
  for (const auto& [name, content] : bad_headers) {
    TempFile tf(name);
    tf.WriteString(content);
    kdu_image_dims dims;
    int next_comp = 0;
    EXPECT_THROW(OpenReader(tf.c_str(), dims, next_comp), kdu_exception);
  }

  TempFile bmp_bad_bpp("bad_bpp.bmp");
  bmp_bad_bpp.WriteBytes(BuildBmp(2, 2, false, 16, 0, 0, {}, {0, 0, 0, 0}));
  {
    kdu_image_dims dims;
    int next_comp = 0;
    EXPECT_THROW(OpenReader(bmp_bad_bpp.c_str(), dims, next_comp),
                 kdu_exception);
  }

  const std::vector<std::pair<std::string, std::string>> trunc_data = {
      {"trunc.pgm", "P5\n2 2\n255\n"},
      {"trunc.ppm", "P6\n2 2\n255\n"},
      {"trunc.pfm", "Pf\n2 2\n-1.0\n"},
      {"trunc.pgx", "PG ML +8 2 2\n"},
  };
  for (const auto& [name, content] : trunc_data) {
    TempFile tf(name);
    tf.WriteString(content);
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(tf.c_str(), dims, next_comp);
    std::vector<kdu_int64> out;
    EXPECT_THROW(
        ReadLineValues64(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0, &out, nullptr),
        kdu_exception);
  }

  TempFile trunc_raw("trunc.raw");
  trunc_raw.WriteBytes({1});
  {
    kdu_image_dims dims;
    dims.add_component(1, 2, 8, false, 0);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(trunc_raw.c_str(), dims, next_comp);
    std::vector<kdu_int64> out;
    EXPECT_THROW(
        ReadLineValues64(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0, &out, nullptr),
        kdu_exception);
  }

  TempFile trunc_bmp("trunc.bmp");
  trunc_bmp.WriteBytes(BuildBmp(2, 2, false, 24, 0, 0, {}, {}));
  {
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(trunc_bmp.c_str(), dims, next_comp);
    std::vector<kdu_int64> out;
    EXPECT_THROW(
        ReadLineValues64(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0, &out, nullptr),
        kdu_exception);
  }
}

TEST(ImageInTiffSignedAndForceFloatTest,
     CoversSignedForceToFloatReinterpretQuietAndTruncatedTiff) {
  // Signed 12-bit & 16-bit TIFF with force_to_floats_and_reinterpret.
  TiffBuilder tb_s(true, false);
  tb_s.AddShort(256, {3});
  tb_s.AddShort(257, {1});
  tb_s.AddShort(258, {12});
  tb_s.AddShort(259, {1});
  tb_s.AddShort(262, {1});
  tb_s.AddShort(277, {1});
  tb_s.AddShort(278, {1});
  tb_s.AddShort(284, {1});
  tb_s.AddShort(339, {2});
  std::vector<kdu_byte> s12_bytes = {0x80, 0x00, 0x00, 0x7F, 0xF0};
  TempFile tif_s("s12.tif");
  tif_s.WriteBytes(tb_s.BuildWithChunks({s12_bytes}, false));

  for (int fprec : {16, 32}) {
    for (int fexp : {5, 10}) {
      std::vector<kdu_byte> modes = {KDU_LINE_BUF_ABSOLUTE};
      if (fprec <= 16) {
        modes.push_back(KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE);
      }
      for (kdu_byte flags : modes) {
        kdu_image_dims dims;
        dims.set_forced_precision(0, fprec, false, fexp);
        int next_comp = 0;
        ScopedImageIn reader = OpenReader(tif_s.c_str(), dims, next_comp);
        EXPECT_EQ(3u, ReadAbsRow(reader, 0, 3, flags, 0).size());
      }
    }
  }
  {
    kdu_image_dims dims;
    dims.set_forced_precision(0, 8, false, 0);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(tif_s.c_str(), dims, next_comp);
    EXPECT_EQ(3u, ReadAbsRow(reader, 0, 3, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }

  // Unsigned 12-bit TIFF with force_to_floats_and_reinterpret (upshift + exp).
  TiffBuilder tb_u(true, false);
  tb_u.AddShort(256, {3});
  tb_u.AddShort(257, {1});
  tb_u.AddShort(258, {12});
  tb_u.AddShort(259, {1});
  tb_u.AddShort(262, {1});
  tb_u.AddShort(277, {1});
  tb_u.AddShort(278, {1});
  tb_u.AddShort(284, {1});
  TempFile tif_u("u12_f.tif");
  tif_u.WriteBytes(
      tb_u.BuildWithChunks({{0x00, 0x08, 0x00, 0xFF, 0xF0}}, false));
  for (int fprec : {16, 32}) {
    std::vector<kdu_byte> modes = {KDU_LINE_BUF_ABSOLUTE};
    if (fprec <= 16) {
      modes.push_back(KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE);
    }
    for (kdu_byte flags : modes) {
      kdu_image_dims dims;
      dims.set_forced_precision(0, fprec, false, 5);
      int next_comp = 0;
      ScopedImageIn reader = OpenReader(tif_u.c_str(), dims, next_comp);
      EXPECT_EQ(3u, ReadAbsRow(reader, 0, 3, flags, 0).size());
    }
  }

  // quiet=false with XMP, IPTC, ResolutionUnit=NONE, XRes=0 -> hits metadata
  // log and fallback resolution.
  {
    TiffBuilder tb(true, false);
    tb.AddShort(256, {2});
    tb.AddShort(257, {1});
    tb.AddShort(258, {8});
    tb.AddShort(259, {1});
    tb.AddShort(262, {1});
    tb.AddShort(277, {1});
    tb.AddShort(278, {1});
    tb.AddShort(284, {1});
    tb.AddShort(296, {1});
    tb.AddRational(282, 0, 1);
    tb.AddRational(283, 0, 1);
    tb.AddBytes(700, kTiffByte, {'<', 'x', '/', '>'});
    tb.AddBytes(33723, kTiffUndefined, {1, 2, 3, 4});
    TempFile tif("quiet_false.tif");
    tif.WriteBytes(tb.BuildWithChunks({{10, 20}}, false));
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp, false,
                                      nullptr, 0, /*quiet=*/false);
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }

  // Truncated strip payload throws in tif_in::get (no ctor leak).
  {
    TiffBuilder tb(true, false);
    tb.AddShort(256, {10});
    tb.AddShort(257, {1});
    tb.AddShort(258, {8});
    tb.AddShort(259, {1});
    tb.AddShort(262, {1});
    tb.AddShort(277, {1});
    tb.AddShort(278, {1});
    tb.AddShort(284, {1});
    std::vector<kdu_byte> bytes =
        tb.BuildWithChunks({{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}}, false);
    bytes.resize(bytes.size() - 5);
    TempFile tif("trunc_strip.tif");
    tif.WriteBytes(bytes);
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp);
    std::vector<kdu_int64> out;
    EXPECT_THROW(ReadLineValues64(reader, 0, 10, KDU_LINE_BUF_ABSOLUTE, 0, &out,
                                  nullptr),
                 kdu_exception);
  }
}

TEST(ImageInRemainingTest, CoversSafeBranchesAndMissingFiles) {
  // Missing files hit fopen==nullptr paths before any allocations (no leaks).
  const char* missing_paths[] = {"does_not_exist.pbm", "does_not_exist.ppm",
                                 "does_not_exist.pfm", "does_not_exist.pgx",
                                 "does_not_exist.raw", "does_not_exist.tif"};
  for (const char* name : missing_paths) {
    kdu_image_dims dims;
    int next_comp = 0;
    if (std::string(name).find(".raw") != std::string::npos) {
      dims.add_component(1, 1, 8, false, 0);
    }
    EXPECT_THROW(OpenReader(name, dims, next_comp), kdu_exception);
  }

  // 12-bit PGM read into normalized KDU_LINE_BUF_SHORTS (precision<=13).
  {
    TempFile pgm("norm12_shorts.pgm");
    std::vector<kdu_byte> data;
    std::string hdr = "P5\n2 1\n4095\n";
    data.insert(data.end(), hdr.begin(), hdr.end());
    data.push_back(0x00);
    data.push_back(0x10);
    data.push_back(0x0F);
    data.push_back(0xFF);
    pgm.WriteBytes(data);
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(pgm.c_str(), dims, next_comp);
    std::vector<kdu_int64> out;
    EXPECT_TRUE(
        ReadLineValues64(reader, 0, 2, KDU_LINE_BUF_SHORTS, 0, &out, nullptr));
    EXPECT_EQ(out.size(), 2u);
  }

  // tif_in with skip_bytes != 0 throws before opening or allocating arrays.
  {
    kdu_image_dims dims;
    int next_comp = 0;
    EXPECT_THROW(OpenReader("ignored.tif", dims, next_comp, false, nullptr, 1),
                 kdu_exception);
  }

  // Strip TIFF without RowsPerStrip, and Strip TIFF with RowsPerStrip > height.
  {
    TiffBuilder tb_no_rps(true, false);
    tb_no_rps.AddShort(256, {2});
    tb_no_rps.AddShort(257, {2});
    tb_no_rps.AddShort(258, {8});
    tb_no_rps.AddShort(259, {1});
    tb_no_rps.AddShort(262, {1});
    tb_no_rps.AddShort(277, {1});
    tb_no_rps.AddShort(284, {1});
    TempFile tif1("no_rps.tif");
    tif1.WriteBytes(tb_no_rps.BuildWithChunks({{1, 2, 3, 4}}, false));
    kdu_image_dims dims1;
    int next_comp1 = 0;
    ScopedImageIn r1 = OpenReader(tif1.c_str(), dims1, next_comp1);
    EXPECT_EQ(2u, ReadAbsRow(r1, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
    EXPECT_EQ(2u, ReadAbsRow(r1, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());

    TiffBuilder tb_big_rps(true, false);
    tb_big_rps.AddShort(256, {2});
    tb_big_rps.AddShort(257, {2});
    tb_big_rps.AddShort(258, {8});
    tb_big_rps.AddShort(259, {1});
    tb_big_rps.AddShort(262, {1});
    tb_big_rps.AddShort(277, {1});
    tb_big_rps.AddShort(278, {10});
    tb_big_rps.AddShort(284, {1});
    TempFile tif2("big_rps.tif");
    tif2.WriteBytes(tb_big_rps.BuildWithChunks({{5, 6, 7, 8}}, false));
    kdu_image_dims dims2;
    int next_comp2 = 0;
    ScopedImageIn r2 = OpenReader(tif2.c_str(), dims2, next_comp2);
    EXPECT_EQ(2u, ReadAbsRow(r2, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
    EXPECT_EQ(2u, ReadAbsRow(r2, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }

  // Multi-tile TIFF with tile_height > rows, and cropped first_tile_width >
  // cols.
  {
    TiffBuilder tb(true, false);
    tb.AddShort(256, {8});
    tb.AddShort(257, {2});
    tb.AddShort(258, {8});
    tb.AddShort(259, {1});
    tb.AddShort(262, {1});
    tb.AddShort(277, {1});
    tb.AddShort(284, {1});
    tb.AddShort(322, {4});
    tb.AddShort(323, {4});
    std::vector<kdu_byte> tile(16, 42);
    TempFile tif("tall_tile.tif");
    tif.WriteBytes(tb.BuildWithChunks({tile, tile}, true));
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp);
    EXPECT_EQ(4u, ReadAbsRow(reader, 0, 4, KDU_LINE_BUF_ABSOLUTE, 0).size());
    EXPECT_EQ(4u, ReadAbsRow(reader, 0, 4, KDU_LINE_BUF_ABSOLUTE, 1).size());
    EXPECT_EQ(4u, ReadAbsRow(reader, 0, 4, KDU_LINE_BUF_ABSOLUTE, 0).size());
    EXPECT_EQ(4u, ReadAbsRow(reader, 0, 4, KDU_LINE_BUF_ABSOLUTE, 1).size());
  }
  {
    TiffBuilder tb(true, false);
    tb.AddShort(256, {8});
    tb.AddShort(257, {2});
    tb.AddShort(258, {8});
    tb.AddShort(259, {1});
    tb.AddShort(262, {1});
    tb.AddShort(277, {1});
    tb.AddShort(284, {1});
    tb.AddShort(322, {8});
    tb.AddShort(323, {2});
    TempFile tif("crop_tile.tif");
    tif.WriteBytes(tb.BuildWithChunks({std::vector<kdu_byte>(16, 7)}, true));
    kdu_image_dims dims;
    dims.set_cropping(0, 0, 2, 4, 0);
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp);
    EXPECT_EQ(4u, ReadAbsRow(reader, 0, 4, KDU_LINE_BUF_ABSOLUTE, 0).size());
    EXPECT_EQ(4u, ReadAbsRow(reader, 0, 4, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }

  // All 6 GeoTIFF tags with quiet=false cover copy_tag & cout summary.
  {
    TiffBuilder tb(true, false);
    tb.AddShort(256, {2});
    tb.AddShort(257, {1});
    tb.AddShort(258, {8});
    tb.AddShort(259, {1});
    tb.AddShort(262, {1});
    tb.AddShort(277, {1});
    tb.AddShort(278, {1});
    tb.AddShort(284, {1});
    tb.AddDouble(33550, {1.0, 1.0, 0.0});
    tb.AddDouble(33922, {0.0, 0.0, 0.0, 1.0, 2.0, 0.0});
    tb.AddDouble(34264, std::vector<double>(16, 0.0));
    tb.AddShort(34735, {1, 1, 0, 0});
    tb.AddDouble(34736, {1.0, 2.0});
    tb.AddAscii(34737, "WGS84|");
    TempFile tif("all_geotiff.tif");
    tif.WriteBytes(tb.BuildWithChunks({{10, 20}}, false));
    kdu_image_dims dims;
    int next_comp = 0;
    ScopedImageIn reader = OpenReader(tif.c_str(), dims, next_comp, false,
                                      nullptr, 0, /*quiet=*/false);
    EXPECT_EQ(2u, ReadAbsRow(reader, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }

  // force_to_floats_and_reinterpret overflow clamping with small
  // forced_exponent.
  {
    TempFile pgx_u("clamp_u16.pgx");
    std::vector<kdu_byte> udata;
    std::string uhdr = "PG LM +16 2 1\n";
    udata.insert(udata.end(), uhdr.begin(), uhdr.end());
    udata.push_back(0x00);
    udata.push_back(0x00);
    udata.push_back(0xFF);
    udata.push_back(0xFF);
    pgx_u.WriteBytes(udata);
    kdu_image_dims dims_u;
    dims_u.set_forced_precision(0, 8, false, 2);
    int nc_u = 0;
    ScopedImageIn ru = OpenReader(pgx_u.c_str(), dims_u, nc_u);
    EXPECT_EQ(
        2u, ReadAbsRow(ru, 0, 2, KDU_LINE_BUF_ABSOLUTE | KDU_LINE_BUF_SHORTS, 0)
                .size());

    TempFile pgx_s("clamp_s16.pgx");
    std::vector<kdu_byte> sdata;
    std::string shdr = "PG LM -16 2 1\n";
    sdata.insert(sdata.end(), shdr.begin(), shdr.end());
    sdata.push_back(0x00);
    sdata.push_back(0x80);
    sdata.push_back(0xFF);
    sdata.push_back(0x7F);
    pgx_s.WriteBytes(sdata);
    kdu_image_dims dims_s;
    dims_s.set_forced_precision(0, 8, false, 2);
    int nc_s = 0;
    ScopedImageIn rs = OpenReader(pgx_s.c_str(), dims_s, nc_s);
    EXPECT_EQ(2u, ReadAbsRow(rs, 0, 2, KDU_LINE_BUF_ABSOLUTE, 0).size());
  }
}

}  // namespace
}  // namespace kdu_supp
