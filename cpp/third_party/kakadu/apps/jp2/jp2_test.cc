// Tests for third_party/kakadu/vendor/apps/jp2/jp2.cpp.
// Exercises JP2 box I/O, textualization, dref permissions, data references,
// dimensions, resolution, palette, channel mappings, colour spaces, ICC profile
// synthesis/parsing, colour conversion, and jp2_source / jp2_target lifecycle.

#include "jp2.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "jpx.h"
#include "kdu_compressed.h"
#include "kdu_elementary.h"
#include "kdu_messaging.h"
#include "kdu_params.h"
#include "kdu_sample_processing.h"
#include "kdu_utils.h"
#include "testing/base/public/gunit.h"

namespace {

using kdu_core::kdu_byte;
using kdu_core::kdu_compressed_source;
using kdu_core::kdu_compressed_target;
using kdu_core::kdu_coords;
using kdu_core::kdu_dims;
using kdu_core::kdu_error;
using kdu_core::kdu_exception;
using kdu_core::kdu_int16;
using kdu_core::kdu_int32;
using kdu_core::kdu_int64;
using kdu_core::kdu_line_buf;
using kdu_core::kdu_long;
using kdu_core::kdu_membroker;
using kdu_core::kdu_message;
using kdu_core::kdu_sample16;
using kdu_core::kdu_sample32;
using kdu_core::kdu_sample_allocator;
using kdu_core::kdu_uint16;
using kdu_core::kdu_uint32;
using kdu_core::kdu_uint64;
using kdu_core::siz_params;

using kdu_supp::jp2_add_box_descriptions;
using kdu_supp::jp2_box_textualizer;
using kdu_supp::jp2_channels;
using kdu_supp::jp2_colour;
using kdu_supp::jp2_colour_converter;
using kdu_supp::jp2_colour_space;
using kdu_supp::jp2_data_references;
using kdu_supp::jp2_dimensions;
using kdu_supp::jp2_dref_permissions;
using kdu_supp::jp2_family_src;
using kdu_supp::jp2_family_tgt;
using kdu_supp::jp2_input_box;
using kdu_supp::jp2_locator;
using kdu_supp::jp2_output_box;
using kdu_supp::jp2_palette;
using kdu_supp::jp2_resolution;
using kdu_supp::jp2_source;
using kdu_supp::jp2_target;
using kdu_supp::jpx_source;
using kdu_supp::jpx_target;
using kdu_supp::kdu_cache;

// Installs Kakadu error/warning handlers that throw kdu_exception on error.
class ScopedMessageHandlers {
 public:
  ScopedMessageHandlers()
      : err_(*this, true),
        warn_(*this, false),
        prev_err_(nullptr),
        prev_warn_(nullptr) {
    kdu_core::kdu_customize_errors(&err_);
    kdu_core::kdu_customize_warnings(&warn_);
  }

  ~ScopedMessageHandlers() {
    // Re-registering empty defaults isn't exposed, so keep active until process
    // exit or replace cleanly with no-op static handlers on exit.
    kdu_core::kdu_customize_errors(&FallbackError());
    kdu_core::kdu_customize_warnings(&FallbackWarning());
  }

  std::string errors() const { return errors_; }
  std::string warnings() const { return warnings_; }

 private:
  class MsgSink : public kdu_message {
   public:
    MsgSink(ScopedMessageHandlers& owner, bool is_error)
        : owner_(owner), is_error_(is_error) {}
    void put_text(const char* string) override {
      if (is_error_) {
        owner_.errors_ += string;
      } else {
        owner_.warnings_ += string;
      }
    }
    void flush(bool end_of_message = false) override {
      if (end_of_message && is_error_) {
        throw KDU_ERROR_EXCEPTION;
      }
    }

   private:
    ScopedMessageHandlers& owner_;
    bool is_error_;
  };

  class StaticFallback : public kdu_message {
   public:
    explicit StaticFallback(bool is_error) : is_error_(is_error) {}
    void put_text(const char*) override {}
    void flush(bool end_of_message = false) override {
      if (end_of_message && is_error_) {
        throw KDU_ERROR_EXCEPTION;
      }
    }

   private:
    bool is_error_;
  };

  static StaticFallback& FallbackError() {
    static StaticFallback inst(true);
    return inst;
  }
  static StaticFallback& FallbackWarning() {
    static StaticFallback inst(false);
    return inst;
  }

  MsgSink err_;
  MsgSink warn_;
  kdu_message* prev_err_;
  kdu_message* prev_warn_;
  std::string errors_;
  std::string warnings_;
};

class StringMessage : public kdu_message {
 public:
  void put_text(const char* text) override { text_ += text; }
  const std::string& text() const { return text_; }
  void clear() { text_.clear(); }

 private:
  std::string text_;
};

class MemSource : public kdu_compressed_source {
 public:
  explicit MemSource(std::vector<kdu_byte> data, bool can_seek = true)
      : data_(std::move(data)), can_seek_(can_seek) {}

  int get_capabilities() override {
    int caps = KDU_SOURCE_CAP_SEQUENTIAL;
    if (can_seek_) caps |= KDU_SOURCE_CAP_SEEKABLE;
    return caps;
  }

  int read(kdu_byte* buf, int num_bytes) override {
    if (pos_ >= static_cast<kdu_long>(data_.size())) return 0;
    kdu_long avail = static_cast<kdu_long>(data_.size()) - pos_;
    int n = static_cast<int>(
        avail < static_cast<kdu_long>(num_bytes) ? avail : num_bytes);
    if (n > 0) {
      std::memcpy(buf, data_.data() + pos_, n);
      pos_ += n;
    }
    return n;
  }

  bool seek(kdu_long offset) override {
    if (!can_seek_ || offset < 0) return false;
    pos_ = offset;
    return true;
  }

  kdu_long get_pos() override { return can_seek_ ? pos_ : -1; }

 private:
  std::vector<kdu_byte> data_;
  kdu_long pos_ = 0;
  bool can_seek_;
};

class MemTarget : public kdu_compressed_target {
 public:
  int get_capabilities() override { return KDU_TARGET_CAP_SEQUENTIAL; }

  bool write(const kdu_byte* buf, int num_bytes) override {
    if (fail_writes_) return false;
    if (rewrite_active_) {
      if (rewrite_pos_ + num_bytes > static_cast<kdu_long>(data_.size())) {
        data_.resize(static_cast<size_t>(rewrite_pos_ + num_bytes), 0);
      }
      if (num_bytes > 0) {
        std::memcpy(data_.data() + rewrite_pos_, buf, num_bytes);
        rewrite_pos_ += num_bytes;
      }
      return true;
    }
    if (num_bytes > 0) {
      data_.insert(data_.end(), buf, buf + num_bytes);
    }
    return true;
  }

  bool start_rewrite(kdu_long backtrack) override {
    if (rewrite_active_ || backtrack < 0 ||
        backtrack > static_cast<kdu_long>(data_.size())) {
      return false;
    }
    rewrite_active_ = true;
    rewrite_pos_ = static_cast<kdu_long>(data_.size()) - backtrack;
    return true;
  }

  bool end_rewrite() override {
    if (!rewrite_active_) return false;
    rewrite_active_ = false;
    return true;
  }

  void set_fail_writes(bool v) { fail_writes_ = v; }
  const std::vector<kdu_byte>& data() const { return data_; }

 private:
  std::vector<kdu_byte> data_;
  bool rewrite_active_ = false;
  kdu_long rewrite_pos_ = 0;
  bool fail_writes_ = false;
};

void AppendBe16(std::vector<kdu_byte>& v, kdu_uint16 val) {
  v.push_back(static_cast<kdu_byte>((val >> 8) & 0xFF));
  v.push_back(static_cast<kdu_byte>(val & 0xFF));
}

void AppendBe32(std::vector<kdu_byte>& v, kdu_uint32 val) {
  v.push_back(static_cast<kdu_byte>((val >> 24) & 0xFF));
  v.push_back(static_cast<kdu_byte>((val >> 16) & 0xFF));
  v.push_back(static_cast<kdu_byte>((val >> 8) & 0xFF));
  v.push_back(static_cast<kdu_byte>(val & 0xFF));
}

void AppendBe64(std::vector<kdu_byte>& v, kdu_uint64 val) {
  AppendBe32(v, static_cast<kdu_uint32>(val >> 32));
  AppendBe32(v, static_cast<kdu_uint32>(val & 0xFFFFFFFFU));
}

void AppendBox(std::vector<kdu_byte>& out, kdu_uint32 type,
               const std::vector<kdu_byte>& payload, bool long_header = false) {
  if (long_header) {
    AppendBe32(out, 1);
    AppendBe32(out, type);
    AppendBe64(out, 16 + payload.size());
  } else {
    AppendBe32(out, static_cast<kdu_uint32>(8 + payload.size()));
    AppendBe32(out, type);
  }
  out.insert(out.end(), payload.begin(), payload.end());
}

void WriteBe16At(std::vector<kdu_byte>& v, size_t off, kdu_uint16 val) {
  v[off] = static_cast<kdu_byte>((val >> 8) & 0xFF);
  v[off + 1] = static_cast<kdu_byte>(val & 0xFF);
}

void WriteBe32At(std::vector<kdu_byte>& v, size_t off, kdu_uint32 val) {
  v[off] = static_cast<kdu_byte>((val >> 24) & 0xFF);
  v[off + 1] = static_cast<kdu_byte>((val >> 16) & 0xFF);
  v[off + 2] = static_cast<kdu_byte>((val >> 8) & 0xFF);
  v[off + 3] = static_cast<kdu_byte>(val & 0xFF);
}

// Builds a synthetic ICC profile in memory (v2.1). Supports TRC+XYZ,
// mft1 (8-bit LUT), mft2 (16-bit LUT), and monochrome gray TRC.
struct IccTag {
  kdu_uint32 sig;
  std::vector<kdu_byte> data;
};

std::vector<kdu_byte> BuildIccProfile(
    kdu_uint32 color_space, kdu_uint32 pcs, const std::vector<IccTag>& tags,
    kdu_uint32 dev_class = 0x6D6E7472 /*mntr*/, bool bad_sig = false) {
  std::vector<kdu_byte> p(128 + 4 + tags.size() * 12, 0);
  // Header fields.
  WriteBe32At(p, 4, 0x6B616B64);    // CMM
  WriteBe32At(p, 8, 0x02100000);    // Version 2.1
  WriteBe32At(p, 12, dev_class);    // Class
  WriteBe32At(p, 16, color_space);  // Data colour space
  WriteBe32At(p, 20, pcs);          // PCS
  if (!bad_sig) {
    WriteBe32At(p, 36, 0x61637370);  // 'acsp'
  }
  // PCS illuminant D50: (0.9642, 1.0, 0.8249) in s15Fixed16.
  WriteBe32At(p, 68, 0x0000F6D6);
  WriteBe32At(p, 72, 0x00010000);
  WriteBe32At(p, 76, 0x0000D32D);

  WriteBe32At(p, 128, static_cast<kdu_uint32>(tags.size()));
  for (size_t i = 0; i < tags.size(); ++i) {
    while (p.size() % 4 != 0) p.push_back(0);
    kdu_uint32 off = static_cast<kdu_uint32>(p.size());
    kdu_uint32 len = static_cast<kdu_uint32>(tags[i].data.size());
    WriteBe32At(p, 132 + i * 12, tags[i].sig);
    WriteBe32At(p, 132 + i * 12 + 4, off);
    WriteBe32At(p, 132 + i * 12 + 8, len);
    p.insert(p.end(), tags[i].data.begin(), tags[i].data.end());
  }
  WriteBe32At(p, 0, static_cast<kdu_uint32>(p.size()));
  return p;
}

IccTag MakeXyzTag(kdu_uint32 sig, double x, double y, double z) {
  std::vector<kdu_byte> d;
  AppendBe32(d, 0x58595A20);  // 'XYZ '
  AppendBe32(d, 0);
  AppendBe32(d, static_cast<kdu_uint32>(static_cast<kdu_int32>(x * 65536.0)));
  AppendBe32(d, static_cast<kdu_uint32>(static_cast<kdu_int32>(y * 65536.0)));
  AppendBe32(d, static_cast<kdu_uint32>(static_cast<kdu_int32>(z * 65536.0)));
  return {sig, std::move(d)};
}

IccTag MakeCurvGammaTag(kdu_uint32 sig, double gamma) {
  std::vector<kdu_byte> d;
  AppendBe32(d, 0x63757276);  // 'curv'
  AppendBe32(d, 0);
  AppendBe32(d, 1);
  AppendBe16(d, static_cast<kdu_uint16>(gamma * 256.0));
  return {sig, std::move(d)};
}

IccTag MakeCurvTableTag(kdu_uint32 sig, int n) {
  std::vector<kdu_byte> d;
  AppendBe32(d, 0x63757276);  // 'curv'
  AppendBe32(d, 0);
  AppendBe32(d, static_cast<kdu_uint32>(n));
  for (int i = 0; i < n; ++i) {
    double x = (n <= 1) ? 0.0 : static_cast<double>(i) / (n - 1);
    AppendBe16(d, static_cast<kdu_uint16>(std::pow(x, 2.2) * 65535.0));
  }
  return {sig, std::move(d)};
}

IccTag MakeParaCurveTag(kdu_uint32 sig, kdu_uint16 func_type) {
  std::vector<kdu_byte> d;
  AppendBe32(d, 0x70617261);  // 'para'
  AppendBe32(d, 0);
  AppendBe16(d, func_type);
  AppendBe16(d, 0);
  auto push_s15 = [&](double v) {
    AppendBe32(d, static_cast<kdu_uint32>(static_cast<kdu_int32>(v * 65536.0)));
  };
  if (func_type == 0) {
    push_s15(2.2);
  } else if (func_type == 1) {
    push_s15(2.2);
    push_s15(0.95);
    push_s15(0.05);
  } else if (func_type == 2) {
    push_s15(2.2);
    push_s15(0.95);
    push_s15(0.05);
    push_s15(0.01);
  } else if (func_type == 3) {
    push_s15(2.4);
    push_s15(1.0 / 1.055);
    push_s15(0.055 / 1.055);
    push_s15(1.0 / 12.92);
    push_s15(0.04045);
  } else {
    push_s15(2.4);
    push_s15(1.0 / 1.055);
    push_s15(0.055 / 1.055);
    push_s15(1.0 / 12.92);
    push_s15(0.04045);
    push_s15(0.0);
    push_s15(0.0);
  }
  return {sig, std::move(d)};
}

IccTag MakeIdentityLutTag(kdu_uint32 sig, bool is_16bit, int in_ch, int out_ch,
                          int grid_pts) {
  std::vector<kdu_byte> d;
  AppendBe32(d, is_16bit ? 0x6D667432 : 0x6D667431);  // 'mft2' or 'mft1'
  AppendBe32(d, 0);
  d.push_back(static_cast<kdu_byte>(in_ch));
  d.push_back(static_cast<kdu_byte>(out_ch));
  d.push_back(static_cast<kdu_byte>(grid_pts));
  d.push_back(0);
  // 3x3 identity matrix in s15Fixed16.
  const double mat[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  for (double v : mat) {
    AppendBe32(d, static_cast<kdu_uint32>(static_cast<kdu_int32>(v * 65536.0)));
  }
  int in_len = is_16bit ? 4 : 256;
  int out_len = is_16bit ? 4 : 256;
  if (is_16bit) {
    AppendBe16(d, static_cast<kdu_uint16>(in_len));
    AppendBe16(d, static_cast<kdu_uint16>(out_len));
  }
  for (int c = 0; c < in_ch; ++c) {
    for (int i = 0; i < in_len; ++i) {
      if (is_16bit) {
        AppendBe16(d, static_cast<kdu_uint16>(i * 65535 / (in_len - 1)));
      } else {
        d.push_back(static_cast<kdu_byte>(i));
      }
    }
  }
  int total_grid = 1;
  for (int c = 0; c < in_ch; ++c) total_grid *= grid_pts;
  for (int g = 0; g < total_grid; ++g) {
    for (int c = 0; c < out_ch; ++c) {
      if (is_16bit) {
        AppendBe16(d, 32768);
      } else {
        d.push_back(128);
      }
    }
  }
  for (int c = 0; c < out_ch; ++c) {
    for (int i = 0; i < out_len; ++i) {
      if (is_16bit) {
        AppendBe16(d, static_cast<kdu_uint16>(i * 65535 / (out_len - 1)));
      } else {
        d.push_back(static_cast<kdu_byte>(i));
      }
    }
  }
  return {sig, std::move(d)};
}

// Compresses a minimal valid JPEG2000 codestream into `tgt`.
void CompressMinimalCodestream(kdu_compressed_target& tgt, int width,
                               int height, int num_comps, int bit_depth = 8,
                               bool is_signed = false, bool reversible = true) {
  siz_params siz;
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Scomponents, 0, 0, num_comps);
  for (int c = 0; c < num_comps; ++c) {
    siz.set(Sprecision, c, 0, bit_depth);
    siz.set(Ssigned, c, 0, is_signed);
  }
  static_cast<kdu_core::kdu_params&>(siz).finalize();
  kdu_core::kdu_codestream cs;
  cs.create(&siz, &tgt);
  cs.access_siz()->parse_string(reversible ? "Creversible=yes"
                                           : "Creversible=no");
  cs.access_siz()->finalize_all();
  auto tile = cs.open_tile(kdu_coords(0, 0));
  kdu_sample_allocator alloc;
  kdu_core::kdu_push_pull_params pp_params;
  std::vector<kdu_core::kdu_push_ifc> engines(num_comps);
  for (int c = 0; c < num_comps; ++c) {
    auto res = tile.access_component(c).access_resolution();
    engines[c] = kdu_core::kdu_analysis(res, &alloc, pp_params,
                                        KDU_SAMPLE_PREC16, 1.0F, 0, nullptr);
  }
  kdu_line_buf line;
  line.pre_create(
      &alloc, width,
      static_cast<kdu_byte>(KDU_LINE_BUF_SHORTS |
                            (reversible ? KDU_LINE_BUF_ABSOLUTE : 0)),
      0, 0);
  alloc.finalize(cs);
  line.create();
  for (int c = 0; c < num_comps; ++c) {
    for (int r = 0; r < height; ++r) {
      for (int i = 0; i < width; ++i) {
        line.get_buf16()[i].ival =
            static_cast<kdu_int16>((r * 5 + i * 3 + c * 7) & 0x7F);
      }
      engines[c].push(line);
    }
    engines[c].destroy();
  }
  line.destroy();
  tile.close();
  kdu_long max_bytes = 65536;
  cs.flush(&max_bytes, 1);
  cs.destroy();
}

bool CustomTextualizerOk(jp2_input_box* box, kdu_message& msg,
                         bool /*xml_embedded*/, int /*max_len*/) {
  kdu_uint32 v = 0;
  if (!box->read(v)) return false;
  msg << "<custom>" << v << "</custom>";
  return true;
}

TEST(Jp2Test, UtilitiesAndBoxTextualizer) {
  ScopedMessageHandlers handlers;

  // jp2_4cc_to_string: spaces -> '_', non-printable / high-bit -> '.'.
  char buf[5] = {};
  EXPECT_STREQ(kdu_supp::jp2_4cc_to_string(jp2_brand, buf), "jp2_");
  EXPECT_STREQ(kdu_supp::jp2_4cc_to_string(0x011F80FFU, buf), "....");

  // jp2_is_superbox: true for known superboxes, false for leaf boxes.
  EXPECT_TRUE(kdu_supp::jp2_is_superbox(jp2_header_4cc));
  EXPECT_TRUE(kdu_supp::jp2_is_superbox(jp2_resolution_4cc));
  EXPECT_TRUE(kdu_supp::jp2_is_superbox(mj2_movie_4cc));
  EXPECT_FALSE(kdu_supp::jp2_is_superbox(jp2_image_header_4cc));
  EXPECT_FALSE(kdu_supp::jp2_is_superbox(jp2_codestream_4cc));

  jp2_box_textualizer tex;
  EXPECT_STREQ(tex.get_box_name(jp2_file_type_4cc), "file-type");
  EXPECT_EQ(tex.get_box_name(0x7A7A7A7AU), nullptr);
  EXPECT_FALSE(tex.check_textualizer_function(jp2_file_type_4cc));
  EXPECT_FALSE(tex.check_textualizer_function(0x7A7A7A7AU));

  // Invalid add_box_type arguments.
  EXPECT_FALSE(tex.add_box_type(0, "bad"));
  EXPECT_FALSE(tex.add_box_type(0x7A7A7A7AU, nullptr));
  EXPECT_FALSE(tex.add_box_type(0x7A7A7A7AU, ""));

  // Register and overwrite custom long name (> 80 chars).
  std::string long_name(100, 'a');
  EXPECT_TRUE(
      tex.add_box_type(0x7A7A7A7AU, long_name.c_str(), CustomTextualizerOk));
  EXPECT_TRUE(tex.check_textualizer_function(0x7A7A7A7AU));
  EXPECT_EQ(std::strlen(tex.get_box_name(0x7A7A7A7AU)), 80U);
  EXPECT_TRUE(tex.add_box_type(0x7A7A7A7AU, "short", nullptr));
  EXPECT_STREQ(tex.get_box_name(0x7A7A7A7AU), "short");
  EXPECT_TRUE(tex.check_textualizer_function(0x7A7A7A7AU));

  jp2_add_box_descriptions(tex);
  EXPECT_TRUE(tex.check_textualizer_function(jp2_file_type_4cc));
  EXPECT_TRUE(tex.check_textualizer_function(jp2_colour_4cc));

  // Build synthetic boxes and textualize all types.
  std::vector<kdu_byte> stream;
  // 1. Unknown box
  AppendBox(stream, 0x71717171U, {1, 2, 3, 4});
  // 2. Custom box (incomplete + valid)
  AppendBox(stream, 0x7A7A7A7AU, {1, 2});
  AppendBox(stream, 0x7A7A7A7AU, {0, 0, 0, 42});
  // 3. ftyp (empty -> false, full -> true)
  AppendBox(stream, jp2_file_type_4cc, {1, 2});
  {
    std::vector<kdu_byte> ftyp;
    AppendBe32(ftyp, jp2_brand);
    AppendBe32(ftyp, 0);
    AppendBe32(ftyp, jp2_brand);
    AppendBe32(ftyp, jpx_brand);
    AppendBox(stream, jp2_file_type_4cc, ftyp);
  }
  // 4. ihdr: multiple compression types + bpc=255, signed, unsigned, unk, ipr.
  AppendBox(stream, jp2_image_header_4cc, {1, 2});
  const int ctypes[] = {
      JP2_COMPRESSION_TYPE_NONE,  JP2_COMPRESSION_TYPE_MH,
      JP2_COMPRESSION_TYPE_MR,    JP2_COMPRESSION_TYPE_MMR,
      JP2_COMPRESSION_TYPE_JBIG,  JP2_COMPRESSION_TYPE_JPEG,
      JP2_COMPRESSION_TYPE_JLS,   JP2_COMPRESSION_TYPE_JPEG2000,
      JP2_COMPRESSION_TYPE_JBIG2, 99,
  };
  for (size_t i = 0; i < sizeof(ctypes) / sizeof(ctypes[0]); ++i) {
    std::vector<kdu_byte> ihdr;
    AppendBe32(ihdr, 64);
    AppendBe32(ihdr, 128);
    AppendBe16(ihdr, 3);
    kdu_byte bpc = (i == 0) ? 255 : (i == 1 ? 0x87 : 0x07);
    ihdr.push_back(bpc);
    ihdr.push_back(static_cast<kdu_byte>(ctypes[i]));
    ihdr.push_back(i % 2);
    ihdr.push_back((i + 1) % 2);
    AppendBox(stream, jp2_image_header_4cc, ihdr);
  }
  // 5. bpcc: unsigned, signed, variable.
  AppendBox(stream, jp2_bits_per_component_4cc, {0x07, 0x8F, 255});
  // 6. resn (truncated + valid)
  AppendBox(stream, jp2_capture_resolution_4cc, {1, 2});
  AppendBox(stream, jp2_display_resolution_4cc,
            {0, 10, 0, 1, 0, 20, 0, 2, 1, 2});
  // 7. literal boxes: xml, lbl, iprr (with \r, \0, >255 bytes, empty).
  AppendBox(stream, jp2_xml_4cc, {});
  {
    std::vector<kdu_byte> xml(300, 'x');
    xml[5] = '\r';
    xml[10] = '\0';
    AppendBox(stream, jp2_xml_4cc, xml);
  }
  AppendBox(stream, jp2_label_4cc, {'h', 'i'});
  // 8. pxfm (truncated + all formats)
  AppendBox(stream, jp2_pixel_format_4cc, {});
  AppendBox(stream, jp2_pixel_format_4cc, {0, 1, 0, 0});
  {
    std::vector<kdu_byte> pxfm;
    AppendBe16(pxfm, 5);
    AppendBe16(pxfm, 0);
    AppendBe16(pxfm, 0x0000);
    AppendBe16(pxfm, 1);
    AppendBe16(pxfm, 0x1000);
    AppendBe16(pxfm, 2);
    AppendBe16(pxfm, 0x2000);
    AppendBe16(pxfm, 3);
    AppendBe16(pxfm, 0x3004);
    AppendBe16(pxfm, 4);
    AppendBe16(pxfm, 0x400A);
    AppendBox(stream, jp2_pixel_format_4cc, pxfm);
  }
  // 9. colr: meth 1 all spaces + CIELab illuminants + meth 2/3/4/99.
  AppendBox(stream, jp2_colour_4cc, {1});
  AppendBox(stream, jp2_colour_4cc, {1, 0, 0});
  const kdu_uint32 enum_css[] = {0,  1,  3,  4,  9,  11, 12, 13, 15, 16,
                                 17, 18, 19, 20, 21, 22, 23, 24, 999};
  for (kdu_uint32 ecs : enum_css) {
    std::vector<kdu_byte> colr = {1, 0xFE, 2};
    AppendBe32(colr, ecs);
    if (ecs == 19) {
      for (int k = 0; k < 6; ++k) AppendBe32(colr, k + 1);
    }
    AppendBox(stream, jp2_colour_4cc, colr);
  }
  const kdu_uint32 ills[] = {
      JP2_CIE_D50, JP2_CIE_D65, JP2_CIE_D75, JP2_CIE_SA,          JP2_CIE_SC,
      JP2_CIE_F2,  JP2_CIE_F7,  JP2_CIE_F11, JP2_CIE_DAY | 5500U, 0x12345678U};
  for (kdu_uint32 ill : ills) {
    std::vector<kdu_byte> colr = {1, 0, 0};
    AppendBe32(colr, 14);
    for (int k = 0; k < 6; ++k) AppendBe32(colr, 10 + k);
    AppendBe32(colr, ill);
    AppendBox(stream, jp2_colour_4cc, colr);
  }
  AppendBox(stream, jp2_colour_4cc, {2, 0, 0});
  AppendBox(stream, jp2_colour_4cc, {3, 0, 0});
  AppendBox(stream, jp2_colour_4cc, {4, 0, 0, 1, 2});
  {
    std::vector<kdu_byte> v(19, 0xAB);
    v[0] = 4;
    v[1] = 0;
    v[2] = 0;
    AppendBox(stream, jp2_colour_4cc, v);
  }
  AppendBox(stream, jp2_colour_4cc, {9, 0, 0});

  MemSource msrc(stream);
  jp2_family_src fsrc;
  fsrc.open(&msrc);
  jp2_input_box box;
  ASSERT_TRUE(box.open(&fsrc));
  StringMessage msg;
  do {
    msg.clear();
    bool embedded = (box.get_box_type() == jp2_xml_4cc);
    tex.textualize_box(&box, msg, embedded, -1);
    box.close();
  } while (box.open_next());

  // Also test literal max_len > 0 path.
  ASSERT_TRUE(box.open(&fsrc));
  while (box.get_box_type() != jp2_label_4cc) {
    box.close();
    ASSERT_TRUE(box.open_next());
  }
  msg.clear();
  EXPECT_TRUE(tex.textualize_box(&box, msg, false, 1));
  EXPECT_EQ(msg.text(), "h");
  box.close();
  fsrc.close();
}

TEST(Jp2Test, DrefPermissionsAndDataReferences) {
  ScopedMessageHandlers handlers;

  jp2_dref_permissions perms;
  EXPECT_FALSE(perms.can_access("./a.jp2"));
  EXPECT_EQ(perms.get_access_path_len("./a.jp2"), 0U);
  char path_buf[256];
  EXPECT_EQ(perms.get_access_path(path_buf, sizeof(path_buf), "./a.jp2"),
            nullptr);

  // Relative access without container path fails.
  perms.init(nullptr, "**");
  EXPECT_FALSE(perms.can_access("./a.jp2"));
  EXPECT_FALSE(perms.can_access(nullptr));

  // Container with trailing directory.
  perms.init("/tmp/dir/main.jp2",
             "./*.{jp2,jpx}:./**/{sub,alt}/*.jp2:./**.jp2:./literal\\:file");
  EXPECT_TRUE(perms.can_access("./img.jp2"));
  EXPECT_TRUE(perms.can_access("./img.jpx"));
  EXPECT_FALSE(perms.can_access("./img.tif"));
  EXPECT_FALSE(perms.can_access("./a.b.jp2"));  // '*' does not match '.'
  EXPECT_TRUE(perms.can_access("./a/sub/c.jp2"));
  EXPECT_TRUE(perms.can_access("./x/alt/c.jp2"));
  EXPECT_TRUE(perms.can_access("./a/b/sub/c.jp2"));    // matched by ./**.jp2
  EXPECT_FALSE(perms.can_access("./a/b//sub/c.jp2"));  // '//' rejected by '**'
  EXPECT_EQ(perms.get_access_path(nullptr, sizeof(path_buf), "./img.jp2"),
            nullptr);
  EXPECT_EQ(perms.get_access_path(path_buf, 4, "./img.jp2"), nullptr);
  ASSERT_NE(perms.get_access_path(path_buf, sizeof(path_buf), "./img.jp2"),
            nullptr);
  EXPECT_STREQ(path_buf, "/tmp/dir/./img.jp2");

  // Wildcard & choice edge cases: empty choice branch, escapes, malformed stop.
  perms.init("file.jp2", "./a{.b,}.jp2:./x\\*y.jp2:malformed\\");
  EXPECT_TRUE(perms.can_access("./a.b.jp2"));
  EXPECT_TRUE(perms.can_access("./a.jp2"));
  EXPECT_TRUE(perms.can_access("./x*y.jp2"));
  perms.init("/abs/file.jp2", "/var/*/img.jp2:**");
  EXPECT_TRUE(perms.can_access("/var/123/img.jp2"));
  ASSERT_NE(
      perms.get_access_path(path_buf, sizeof(path_buf), "/var/123/img.jp2"),
      nullptr);
  EXPECT_STREQ(path_buf, "/var/123/img.jp2");
  perms.init("file.jp2", "/foo/**/bar:{open_unclosed");
  EXPECT_TRUE(perms.can_access("/foo/a/bar"));
  EXPECT_FALSE(perms.can_access("anything"));
  perms.reset();
  EXPECT_FALSE(perms.can_access("./img.jp2"));

  // jp2_data_references through jpx_target / jpx_source.
  MemTarget mtgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mtgt);
  {
    jpx_target jt;
    jt.open(&ftgt);
    jp2_data_references dref = jt.access_data_references();
    ASSERT_TRUE(dref.exists());
    EXPECT_EQ(dref.add_url(nullptr), 0);
    int i1 = dref.add_file_url("rel path/img 1.jp2");
    int i2 = dref.add_file_url("/abs/dir/img2.jp2");
    int i3 = dref.add_url("http:///example.com/res.jp2");
    EXPECT_EQ(dref.add_url("http:///example.com/res.jp2"), i3);  // dedup
    EXPECT_EQ(dref.add_url("/override.jp2", i2), i2);
    EXPECT_EQ(dref.get_num_urls(), 3);
    EXPECT_STREQ(dref.get_url(0), "");
    EXPECT_EQ(dref.get_url(-1), nullptr);
    EXPECT_EQ(dref.get_url(4), nullptr);
    EXPECT_GT(dref.find_url("/override.jp2"), 0);
    EXPECT_EQ(dref.find_url("missing"), 0);
    EXPECT_STREQ(dref.get_file_url(i1), "./rel path/img 1.jp2");
    EXPECT_STREQ(dref.get_file_url(i2), "/override.jp2");
    EXPECT_EQ(dref.get_file_url(i3), nullptr);  // http:/// not local file

    auto cs_tgt = jt.add_codestream();
    cs_tgt.access_dimensions().init(kdu_coords(8, 8), 1);
    cs_tgt.access_dimensions().set_precision(0, 8, false);
    auto layer = jt.add_layer();
    layer.access_channels().init(1);
    EXPECT_TRUE(layer.access_channels().set_colour_mapping(0, 0, -1, 0));
    layer.add_colour().init(kdu_supp::JP2_sLUM_SPACE);
    jt.write_headers();
    auto* cs_stream = cs_tgt.open_stream();
    CompressMinimalCodestream(*cs_stream, 8, 8, 1);
    cs_stream->close();
    jt.close();
  }
  ftgt.close();

  MemSource msrc(mtgt.data());
  jp2_family_src fsrc;
  fsrc.open(&msrc);
  fsrc.set_access_permissions("./**:/**");
  jpx_source jsrc;
  ASSERT_EQ(jsrc.open(&fsrc, false), 1);
  jp2_data_references rdref = jsrc.access_data_references();
  ASSERT_TRUE(rdref.exists());
  EXPECT_EQ(rdref.get_num_urls(), 3);
  EXPECT_STREQ(rdref.get_file_url(1), "./rel path/img 1.jp2");
  EXPECT_STREQ(rdref.get_file_url(2), "/override.jp2");
  jsrc.close();
  fsrc.close();
}

TEST(Jp2Test, FamilySrcInputBoxCacheAndOutputBox) {
  ScopedMessageHandlers handlers;

  // Test kdu_membroker attachment, limits, and failures via jp2_family_tgt.
  kdu_membroker broker(1 << 20);
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt, &broker);

    // pre_open + open_next + detached open(box_type) + get_contents.
    jp2_output_box pre;
    EXPECT_TRUE(pre.pre_open(&ftgt));
    pre.open_next(jp2_label_4cc);
    EXPECT_TRUE(pre.write(static_cast<kdu_uint32>(0x11223344U)));
    EXPECT_TRUE(pre.write(static_cast<kdu_uint16>(0x5566U)));
    EXPECT_GT(pre.get_box_length(), 8);
    pre.close();

    jp2_output_box detached;
    detached.open(jp2_label_4cc);
    detached.write(static_cast<kdu_uint32>(0xAABBCCDDU));
    kdu_long det_len = 0;
    EXPECT_NE(detached.get_contents(det_len), nullptr);
    EXPECT_EQ(det_len, 4);
    detached.close();

    // Standalone + set_target_size + write_header_last + free box.
    jp2_output_box standalone;
    standalone.open(&ftgt, jp2_label_4cc, false, false);
    standalone.set_target_size(8);
    standalone.write(static_cast<kdu_uint32>(0xCAFEBABEU));
    standalone.write(static_cast<kdu_uint32>(0x11223344U));
    EXPECT_TRUE(standalone.close());

    jp2_output_box last_hdr;
    last_hdr.open(&ftgt, jp2_uuid_4cc, false, false);
    last_hdr.write_header_last();
    last_hdr.write(static_cast<kdu_uint32>(1));
    last_hdr.close();

    jp2_output_box free_box;
    free_box.open(&ftgt, jp2_label_4cc);
    free_box.write(static_cast<kdu_uint32>(1234));
    EXPECT_TRUE(free_box.write_free_and_close(12));

    // Rubber-length super-box must be last box written to ftgt.
    jp2_output_box outer;
    outer.open(&ftgt, jp2_header_4cc, /*rubber_length=*/true);
    EXPECT_EQ(outer.get_header_length(), 8);
    EXPECT_GE(outer.get_start_pos(), 0);

    // Sub-box inside outer + use_long_header + rewrite.
    jp2_output_box sub;
    sub.open(&outer, jp2_label_4cc);
    EXPECT_GT(sub.use_long_header(), 8);
    kdu_byte payload[4] = {'t', 'e', 's', 't'};
    EXPECT_TRUE(sub.write(payload, 4));
    EXPECT_TRUE(sub.start_rewrite(4));
    EXPECT_TRUE(sub.write(reinterpret_cast<const kdu_byte*>("BEST"), 4));
    EXPECT_TRUE(sub.end_rewrite());
    sub.close();
    sub.open_next(jp2_xml_4cc);
    EXPECT_TRUE(sub.write(reinterpret_cast<const kdu_byte*>("<a/>"), 4));
    sub.close();
    outer.close();

    ftgt.close();
  }
  EXPECT_EQ(broker.get_allocated_bytes(false), 0);

  // Simulated start pos + write_box + reopen.
  {
    jp2_family_tgt sim_tgt;
    sim_tgt.open(static_cast<kdu_long>(100), &broker);
    jp2_output_box b;
    b.open(&sim_tgt, jp2_label_4cc);
    b.write(static_cast<kdu_uint32>(0x12345678U));
    EXPECT_GE(b.write_box(&sim_tgt, false), 12);
    b.close();
    sim_tgt.close();
  }

  // Real temp file open/close for jp2_family_tgt and jp2_family_src.
  const std::string tmp_path = testing::TempDir() + "/kakadu_jp2_test.jp2";
  {
    jp2_family_tgt ftgt;
    ftgt.open(tmp_path.c_str());
    jp2_target tgt;
    tgt.open(&ftgt);
    tgt.access_dimensions().init(kdu_coords(8, 8), 1);
    tgt.access_dimensions().set_precision(0, 8, false);
    tgt.access_colour().init(kdu_supp::JP2_sLUM_SPACE);
    tgt.write_header();
    tgt.open_codestream(false);
    CompressMinimalCodestream(tgt, 8, 8, 1);
    tgt.close();
    ftgt.close();

    jp2_family_src fsrc;
    fsrc.open(tmp_path.c_str(), /*allow_seeks=*/true);
    jp2_input_box box;
    ASSERT_TRUE(box.open(&fsrc));
    EXPECT_EQ(box.get_box_type(), jp2_signature_4cc);
    EXPECT_TRUE(box.is_complete());
    box.close();
    fsrc.close();
  }
  std::remove(tmp_path.c_str());

  // Non-existent file throws kdu_exception.
  {
    jp2_family_src bad_src;
    EXPECT_THROW(bad_src.open("/nonexistent_dir_kakadu/missing_file.jp2"),
                 kdu_exception);
    jp2_family_tgt bad_tgt;
    EXPECT_THROW(bad_tgt.open("/nonexistent_dir_kakadu/missing_out.jp2"),
                 kdu_exception);
  }

  // Build synthetic boxes to test jp2_input_box: long 64-bit box, size==0 box,
  // fork, transplant, load_in_memory (internal + external), access_memory,
  // seek.
  std::vector<kdu_byte> raw;
  AppendBox(raw, jp2_label_4cc, {'A', 'B', 'C', 'D'}, /*long_header=*/true);
  {
    std::vector<kdu_byte> sub;
    AppendBox(sub, jp2_label_4cc, {'1', '2', '3', '4'});
    AppendBox(raw, jp2_header_4cc, sub);
  }
  // Box with size == 0 extending to EOF.
  AppendBe32(raw, 0);
  AppendBe32(raw, jp2_xml_4cc);
  raw.push_back('Z');
  raw.push_back('!');

  // Test seekable & non-seekable MemSource + partial close.
  for (bool seekable : {true, false}) {
    MemSource msrc(raw, seekable);
    jp2_family_src fsrc;
    fsrc.open(&msrc, &broker);
    jp2_input_box box;
    ASSERT_TRUE(box.open(&fsrc));
    EXPECT_EQ(box.get_box_type(), jp2_label_4cc);
    EXPECT_EQ(box.get_box_bytes(), 20);
    EXPECT_EQ(box.get_remaining_bytes(), 4);
    EXPECT_TRUE(box.load_in_memory(10));
    kdu_long mpos = 0;
    kdu_byte* mlim = nullptr;
    kdu_byte* mptr = box.access_memory(mpos, mlim);
    ASSERT_NE(mptr, nullptr);
    EXPECT_EQ(mlim - mptr, 4);
    kdu_uint32 w = 0;
    EXPECT_TRUE(box.read(w));
    EXPECT_EQ(w, 0x41424344U);
    if (seekable) {
      EXPECT_TRUE(box.seek(1));
      kdu_uint16 w16 = 0;
      EXPECT_TRUE(box.read(w16));
      EXPECT_EQ(w16, 0x4243U);
    }
    box.close();

    // Sub-box open + fork + transplant.
    ASSERT_TRUE(box.open_next());
    EXPECT_EQ(box.get_box_type(), jp2_header_4cc);
    jp2_input_box sub, forked, trans;
    ASSERT_TRUE(sub.open(&box));
    forked.fork(sub);
    trans.transplant(forked);
    kdu_byte buf[4] = {};
    EXPECT_EQ(trans.read(buf, 4), 4);
    EXPECT_EQ(buf[0], '1');
    trans.close();
    sub.close();
    box.close();

    // External buffer load_in_memory + size==0 box (MemSource returns false
    // because FILE* is null).
    ASSERT_TRUE(box.open_next());
    EXPECT_EQ(box.get_box_type(), jp2_xml_4cc);
    kdu_byte ext_buf[16];
    EXPECT_FALSE(box.load_in_memory(16, ext_buf, sizeof(ext_buf)));
    box.close();

    if (seekable) {
      EXPECT_TRUE(fsrc.is_top_level_complete());
      EXPECT_TRUE(fsrc.is_codestream_main_header_complete(0));
      fsrc.close(/*partial=*/true);
      MemSource msrc2(raw, true);
      fsrc.open(&msrc2, &broker);
      EXPECT_TRUE(box.open(&fsrc));
      box.close();
      fsrc.close(/*partial=*/false);
    } else {
      fsrc.close(/*partial=*/false);
    }
  }
}

TEST(Jp2Test, DimensionsAndResolution) {
  ScopedMessageHandlers handlers;

  MemTarget mtgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mtgt);
  jp2_target tgt;
  tgt.open(&ftgt);

  // Mixed precisions + signedness triggers bpcc box.
  jp2_dimensions dims = tgt.access_dimensions();
  dims.init(kdu_coords(32, 24), 3, /*unknown_space=*/false,
            JP2_COMPRESSION_TYPE_JPEG2000);
  EXPECT_TRUE(dims.set_precision(0, 8, false));
  EXPECT_TRUE(dims.set_precision(1, 12, true));
  EXPECT_TRUE(dims.set_precision(2, 16, false));
  EXPECT_FALSE(dims.set_precision(-1, 8, false));
  EXPECT_FALSE(dims.set_precision(3, 8, false));
  dims.set_ipr_box_available();
  // Check JP2 vs Part-2 vs HTJ2K finalized compatibility.
  dims.finalize_compatibility(0, 0, 0, 0, 0, 0);
  EXPECT_TRUE(dims.is_jp2_compatible(false));
  EXPECT_FALSE(dims.is_jph_compatible(false));

  jp2_colour col = tgt.access_colour();
  col.init(kdu_supp::JP2_sRGB_SPACE);

  jp2_resolution res = tgt.access_resolution();
  EXPECT_TRUE(res.init(1.5F));
  EXPECT_TRUE(res.set_different_capture_aspect_ratio(0.75F));
  EXPECT_TRUE(res.set_resolution(3000.0F, true));
  EXPECT_TRUE(res.set_resolution(6000.0F, false));
  EXPECT_NEAR(res.get_aspect_ratio(true), 1.5F, 1e-3F);
  EXPECT_NEAR(res.get_aspect_ratio(false), 0.75F, 1e-3F);
  EXPECT_NEAR(res.get_resolution(true), 3000.0F, 5.0F);
  EXPECT_NEAR(res.get_resolution(false), 6000.0F, 10.0F);

  tgt.write_header();
  tgt.open_codestream();
  CompressMinimalCodestream(tgt, 32, 24, 3);
  tgt.close();
  ftgt.close();

  // Read back and verify copy / getters.
  MemSource msrc(mtgt.data());
  jp2_family_src fsrc;
  fsrc.open(&msrc);
  jp2_source src;
  ASSERT_TRUE(src.open(&fsrc));
  ASSERT_EQ(src.read_header(), 1);
  jp2_dimensions rdims = src.access_dimensions();
  EXPECT_EQ(rdims.get_size().x, 32);
  EXPECT_EQ(rdims.get_size().y, 24);
  EXPECT_EQ(rdims.get_num_components(), 3);
  EXPECT_FALSE(rdims.colour_space_known());
  EXPECT_EQ(rdims.get_bit_depth(0), 8);
  EXPECT_EQ(rdims.get_bit_depth(1), 12);
  EXPECT_TRUE(rdims.get_signed(1));
  EXPECT_EQ(rdims.get_compression_type(), JP2_COMPRESSION_TYPE_JPEG2000);
  EXPECT_TRUE(rdims.is_ipr_box_available());

  jp2_resolution rres = src.access_resolution();
  EXPECT_NEAR(rres.get_aspect_ratio(true), 1.5F, 0.01F);
  EXPECT_NEAR(rres.get_aspect_ratio(false), 0.75F, 0.01F);
  EXPECT_NEAR(rres.get_resolution(true), 3000.0F, 20.0F);
  EXPECT_NEAR(rres.get_resolution(false), 6000.0F, 40.0F);

  // Copy dimensions and resolution into another target.
  MemTarget mtgt2;
  jp2_family_tgt ftgt2;
  ftgt2.open(&mtgt2);
  jp2_target tgt2;
  tgt2.open(&ftgt2);
  tgt2.access_dimensions().copy(rdims);
  tgt2.access_dimensions().finalize_compatibility(rdims);
  tgt2.access_resolution().copy(rres);
  tgt2.access_colour().copy(src.access_colour());
  // Also test JPH part15 capability flag.
  {
    MemTarget mtgt_h;
    jp2_family_tgt ftgt_h;
    ftgt_h.open(&mtgt_h);
    jp2_target tgt_h;
    tgt_h.open(&ftgt_h);
    tgt_h.access_dimensions().init(kdu_coords(8, 8), 1);
    tgt_h.access_dimensions().set_precision(0, 8, false);
    tgt_h.access_dimensions().finalize_compatibility(0, Scap_P15, 0, 0, 0, 1);
    EXPECT_TRUE(tgt_h.access_dimensions().is_jph_compatible(false));
    EXPECT_EQ(tgt_h.get_brand(), jph_brand);
    tgt_h.close();
    ftgt_h.close();
  }
  tgt2.close();
  ftgt2.close();
  src.close();
  fsrc.close();
}

TEST(Jp2Test, PaletteAndChannelsFullMatrix) {
  ScopedMessageHandlers handlers;

  // Test palette with unsigned 8-bit, signed 16-bit, float, fixpoint LUTs.
  MemTarget mtgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mtgt);
  jpx_target jt;
  jt.open(&ftgt);

  auto cs0 = jt.add_codestream();
  cs0.access_dimensions().init(kdu_coords(8, 8), 3);
  cs0.access_dimensions().set_precision(0, 8, false);
  cs0.access_dimensions().set_precision(1, 16, true);
  cs0.access_dimensions().set_precision(2, 8, false);

  auto layer = jt.add_layer();
  jp2_palette pal = cs0.access_palette();
  pal.init(3, 4);
  kdu_int32 lut0[4] = {0, 85, 170, 255};
  kdu_int32 lut1[4] = {-32768, -100, 100, 32767};
  kdu_int32 lut2[4] = {0x3C00, 0x3E00, 0x4000, 0x4200};
  EXPECT_TRUE(pal.set_lut(0, lut0, 8, false));
  EXPECT_TRUE(pal.set_lut(1, lut1, 16, true));
  EXPECT_TRUE(pal.set_lut(2, lut2, 16, true));
  EXPECT_FALSE(pal.set_lut(3, lut0, 8, false));
  EXPECT_EQ(pal.get_num_entries(), 4);
  EXPECT_EQ(pal.get_num_luts(), 3);
  EXPECT_EQ(pal.get_bit_depth(1), 16);
  EXPECT_TRUE(pal.get_signed(1));

  float flt[4] = {};
  EXPECT_TRUE(pal.get_lut(0, flt));
  EXPECT_TRUE(pal.get_lut(1, flt, JP2_CHANNEL_FORMAT_FIXPOINT, 0));
  EXPECT_TRUE(pal.get_lut(2, flt, JP2_CHANNEL_FORMAT_FLOAT, 5));
  EXPECT_FALSE(pal.get_lut(5, flt));
  kdu_sample16 s16[4] = {};
  EXPECT_TRUE(pal.get_lut(0, s16));
  EXPECT_TRUE(pal.get_lut(1, s16, JP2_CHANNEL_FORMAT_FIXPOINT, 0));
  EXPECT_TRUE(pal.get_lut(2, s16, JP2_CHANNEL_FORMAT_FLOAT, 5));
  kdu_sample32 abs32[4] = {};
  kdu_sample16 abs16[4] = {};
  EXPECT_TRUE(pal.get_abs_lut(0, abs32));
  EXPECT_TRUE(pal.get_abs_lut(1, abs16));

  layer.add_colour().init(kdu_supp::JP2_sRGB_SPACE);
  jp2_channels ch = layer.access_channels();
  ch.init(3, 1);
  EXPECT_TRUE(ch.set_colour_mapping(0, 0, 0, 0));
  EXPECT_TRUE(ch.set_colour_mapping(1, 0, 1, 0));
  EXPECT_TRUE(ch.set_colour_mapping(2, 0, 2, 0));
  int fix_fmt[1] = {0};
  EXPECT_TRUE(ch.set_non_colour_mapping(0, 0x1234, 1, -1, 0,
                                        JP2_CHANNEL_FORMAT_FIXPOINT, fix_fmt));
  EXPECT_TRUE(ch.set_opacity_mapping(0, 2, -1, 0));

  jt.write_headers();
  auto* cs0_stream = cs0.open_stream();
  CompressMinimalCodestream(*cs0_stream, 8, 8, 3);
  cs0_stream->close();
  jt.close();
  ftgt.close();

  // Read back and check getters + premultiply + copy with stream_map.
  MemSource msrc(mtgt.data());
  jp2_family_src fsrc;
  fsrc.open(&msrc);
  jpx_source jsrc;
  ASSERT_EQ(jsrc.open(&fsrc, false), 1);
  auto rlayer = jsrc.access_layer(0);
  jp2_channels rch = rlayer.access_channels();
  EXPECT_EQ(rch.get_num_colours(), 3);
  EXPECT_EQ(rch.get_num_non_colours(), 1);
  int min_s = 0, max_s = 0, indices[4] = {};
  EXPECT_EQ(rch.get_codestreams_used(min_s, max_s, indices, 4), 1);
  int comp = 0, lut = -1, stream = 0, dfmt = 0, fparams[3] = {};
  EXPECT_TRUE(rch.get_colour_mapping(0, comp, lut, stream, dfmt, fparams));
  kdu_uint16 asoc = 0;
  EXPECT_TRUE(
      rch.get_non_colour_mapping(0, asoc, comp, lut, stream, dfmt, fparams));
  EXPECT_EQ(asoc, 0x1234U);
  EXPECT_TRUE(rch.get_opacity_mapping(0, comp, lut, stream, dfmt, fparams));

  // Verify copy with stream_map + premult mapping on a fresh target.
  MemTarget mtgt2;
  jp2_family_tgt ftgt2;
  ftgt2.open(&mtgt2);
  jpx_target jt2;
  jt2.open(&ftgt2);
  auto l2 = jt2.add_layer();
  auto cs2 = jt2.add_codestream();
  cs2.access_palette().copy(jsrc.access_codestream(0).access_palette());
  const int smap[1] = {0};
  l2.access_channels().copy(rch, smap, 1, 0);
  EXPECT_TRUE(l2.access_channels().set_premult_mapping(1, 2, -1, 0));
  EXPECT_TRUE(l2.access_channels().get_premult_mapping(1, comp, lut, stream,
                                                       dfmt, fparams));
  jt2.close();
  ftgt2.close();
  jsrc.close();
  fsrc.close();

  // Separate JPX container testing set_chroma_key / get_chroma_key.
  MemTarget cmtgt;
  jp2_family_tgt cftgt;
  cftgt.open(&cmtgt);
  jpx_target cjt;
  cjt.open(&cftgt);
  auto ccs = cjt.add_codestream();
  ccs.access_dimensions().init(kdu_coords(8, 8), 3);
  ccs.access_dimensions().set_precision(0, 8, false);
  ccs.access_dimensions().set_precision(1, 8, false);
  ccs.access_dimensions().set_precision(2, 8, false);
  auto clyr = cjt.add_layer();
  clyr.add_colour().init(kdu_supp::JP2_sRGB_SPACE);
  clyr.access_channels().init(3);
  EXPECT_TRUE(clyr.access_channels().set_colour_mapping(0, 0, -1, 0));
  EXPECT_TRUE(clyr.access_channels().set_colour_mapping(1, 1, -1, 0));
  EXPECT_TRUE(clyr.access_channels().set_colour_mapping(2, 2, -1, 0));
  EXPECT_TRUE(clyr.access_channels().set_chroma_key(0, 0));
  cjt.write_headers();
  auto* ccs_stream = ccs.open_stream();
  CompressMinimalCodestream(*ccs_stream, 8, 8, 3);
  ccs_stream->close();
  cjt.close();
  cftgt.close();

  MemSource cmsrc(cmtgt.data());
  jp2_family_src cfsrc;
  cfsrc.open(&cmsrc);
  jpx_source cjsrc;
  ASSERT_EQ(cjsrc.open(&cfsrc, false), 1);
  kdu_int32 key = -1;
  EXPECT_TRUE(cjsrc.access_layer(0).access_channels().get_chroma_key(0, key));
  EXPECT_EQ(key, 0);
  cjsrc.close();
  cfsrc.close();
}

TEST(Jp2Test, ColourSpacesIccProfilesAndConverter) {
  ScopedMessageHandlers handlers;

  kdu_sample_allocator alloc;
  kdu_line_buf lum_f, r_f, g_f, b_f, k_f;
  kdu_line_buf lum_i, r_i, g_i, b_i, k_i;
  lum_f.pre_create(&alloc, 16, 0, 0, 0);
  r_f.pre_create(&alloc, 16, 0, 0, 0);
  g_f.pre_create(&alloc, 16, 0, 0, 0);
  b_f.pre_create(&alloc, 16, 0, 0, 0);
  k_f.pre_create(&alloc, 16, 0, 0, 0);
  lum_i.pre_create(&alloc, 16, KDU_LINE_BUF_SHORTS, 0, 0);
  r_i.pre_create(&alloc, 16, KDU_LINE_BUF_SHORTS, 0, 0);
  g_i.pre_create(&alloc, 16, KDU_LINE_BUF_SHORTS, 0, 0);
  b_i.pre_create(&alloc, 16, KDU_LINE_BUF_SHORTS, 0, 0);
  k_i.pre_create(&alloc, 16, KDU_LINE_BUF_SHORTS, 0, 0);
  alloc.finalize(kdu_core::kdu_codestream());
  lum_f.create();
  r_f.create();
  g_f.create();
  b_f.create();
  k_f.create();
  lum_i.create();
  r_i.create();
  g_i.create();
  b_i.create();
  k_i.create();

  auto fill_lines = [&]() {
    for (int i = 0; i < 16; ++i) {
      float v = (static_cast<float>(i) / 15.0F) - 0.5F;
      lum_f.get_buf32()[i].fval = v;
      r_f.get_buf32()[i].fval = v;
      g_f.get_buf32()[i].fval = v * 0.5F;
      b_f.get_buf32()[i].fval = -v * 0.25F;
      k_f.get_buf32()[i].fval = 0.1F;
      kdu_int16 iv = static_cast<kdu_int16>(v * (1 << KDU_FIX_POINT));
      lum_i.get_buf16()[i].ival = iv;
      r_i.get_buf16()[i].ival = iv;
      g_i.get_buf16()[i].ival = iv / 2;
      b_i.get_buf16()[i].ival = -iv / 4;
      k_i.get_buf16()[i].ival =
          static_cast<kdu_int16>(0.1 * (1 << KDU_FIX_POINT));
    }
  };

  auto test_enum_space = [&](jp2_colour_space sp, bool wide, bool fast) {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    jpx_target jt;
    jt.open(&ftgt);
    auto layer = jt.add_layer();
    jp2_colour col = layer.add_colour();
    col.init(sp);
    int nch = col.get_num_colours();
    EXPECT_EQ(col.get_space(), sp);
    col.is_jp2_compatible();
    col.is_jph_compatible();
    col.is_opponent_space();
    col.get_natural_unsigned_zero_point(0);
    col.get_precedence();
    col.get_approximation_level();

    jp2_colour_converter conv;
    bool ok = conv.init(col, wide, fast);
    if (sp == kdu_supp::JP2_CIEJab_SPACE) {
      EXPECT_FALSE(ok);
    } else if (ok) {
      const bool expect_wide = wide && (sp != kdu_supp::JP2_bilevel1_SPACE) &&
                               (sp != kdu_supp::JP2_bilevel2_SPACE);
      EXPECT_EQ(conv.get_wide_gamut(), expect_wide);
      conv.is_approximate();
      conv.is_non_trivial();
      float sw = 0.0F;
      bool is_chroma = false;
      EXPECT_TRUE(conv.get_channel_info(0, sw, is_chroma));
      EXPECT_FALSE(conv.get_channel_info(nch + 1, sw, is_chroma));
      fill_lines();
      if (nch == 1) {
        EXPECT_TRUE(conv.convert_lum(lum_f));
        EXPECT_TRUE(conv.convert_lum(lum_i, 8));
      } else if (nch == 3) {
        EXPECT_TRUE(conv.convert_rgb(r_f, g_f, b_f));
        EXPECT_TRUE(conv.convert_rgb(r_i, g_i, b_i, 8));
      } else if (nch == 4) {
        EXPECT_TRUE(conv.convert_rgb4(r_f, g_f, b_f, k_f));
        EXPECT_TRUE(conv.convert_rgb4(r_i, g_i, b_i, k_i, 8));
        fill_lines();
        EXPECT_TRUE(conv.convert_rgb(r_f, g_f, b_f));
      }
      conv.clear();
      EXPECT_FALSE(conv.exists());
    }
    jt.close();
    ftgt.close();
  };

  const jp2_colour_space spaces[] = {
      kdu_supp::JP2_bilevel1_SPACE, kdu_supp::JP2_bilevel2_SPACE,
      kdu_supp::JP2_sLUM_SPACE,     kdu_supp::JP2_sRGB_SPACE,
      kdu_supp::JP2_esRGB_SPACE,    kdu_supp::JP2_ROMMRGB_SPACE,
      kdu_supp::JP2_sYCC_SPACE,     kdu_supp::JP2_esYCC_SPACE,
      kdu_supp::JP2_YCbCr1_SPACE,   kdu_supp::JP2_YCbCr2_SPACE,
      kdu_supp::JP2_YCbCr3_SPACE,   kdu_supp::JP2_PhotoYCC_SPACE,
      kdu_supp::JP2_YPbPr60_SPACE,  kdu_supp::JP2_YPbPr50_SPACE,
      kdu_supp::JP2_CMY_SPACE,      kdu_supp::JP2_CMYK_SPACE,
      kdu_supp::JP2_YCCK_SPACE,     kdu_supp::JP2_CIELab_SPACE,
      kdu_supp::JP2_CIEJab_SPACE,
  };
  for (jp2_colour_space sp : spaces) {
    test_enum_space(sp, false, false);
    test_enum_space(sp, true, true);
  }

  // Synthesized monochrome ICC, synthesized RGB ICC, CIELab/Jab custom, vendor.
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    jpx_target jt;
    jt.open(&ftgt);

    auto l_mono = jt.add_layer();
    jp2_colour col_mono = l_mono.add_colour();
    col_mono.init(2.2, 0.05, 32);
    jp2_colour_converter c_mono;
    // Patch 09 requires 3 TRCs in j2_icc_profile::init, so 1-TRC monochrome
    // synthesized profiles set uses_3d_luts=true and cannot init converter
    // LUTs.
    EXPECT_FALSE(c_mono.init(col_mono, true, false));

    auto l_rgb = jt.add_layer();
    const double rx[2] = {0.64, 0.33}, gx[2] = {0.30, 0.60},
                 bx[2] = {0.15, 0.06};
    jp2_colour col_rgb = l_rgb.add_colour();
    col_rgb.init(rx, gx, bx, 2.2, 0.05, 32, true);
    int icc_len = 0;
    EXPECT_NE(col_rgb.get_icc_profile(&icc_len), nullptr);
    EXPECT_GT(icc_len, 128);
    std::vector<kdu_byte> icc_copy(icc_len);
    EXPECT_EQ(col_rgb.get_icc_profile(icc_copy.data(), icc_len), icc_len);
    jp2_colour_converter c_rgb;
    EXPECT_TRUE(c_rgb.init(col_rgb, false, false));
    fill_lines();
    EXPECT_TRUE(c_rgb.convert_rgb(r_f, g_f, b_f));
    EXPECT_TRUE(c_rgb.convert_rgb(r_i, g_i, b_i));

    auto l_lab = jt.add_layer();
    jp2_colour col_lab = l_lab.add_colour();
    col_lab.init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 8, 255, 128, 8, 255, 128,
                 8, JP2_CIE_D65, 6500);
    int lr = 0, lo = 0, lb = 0, ar = 0, ao = 0, ab = 0, br = 0, bo = 0, bb = 0;
    kdu_uint32 ill = 0;
    kdu_uint16 temp = 0;
    EXPECT_TRUE(
        col_lab.get_lab_params(lr, lo, lb, ar, ao, ab, br, bo, bb, ill, temp));
    EXPECT_FALSE(col_lab.check_cie_default());
    jp2_colour_converter c_lab;
    EXPECT_TRUE(c_lab.init(col_lab, false, false));
    fill_lines();
    EXPECT_TRUE(c_lab.convert_rgb(r_f, g_f, b_f));

    auto l_jab = jt.add_layer();
    jp2_colour col_jab = l_jab.add_colour();
    col_jab.init(kdu_supp::JP2_CIEJab_SPACE, 100, 0, 8, 255, 128, 8, 255, 128,
                 8);
    EXPECT_TRUE(col_jab.get_jab_params(lr, lo, lb, ar, ao, ab, br, bo, bb));

    auto l_vnd = jt.add_layer();
    kdu_byte uuid[16] = {1, 2, 3, 4};
    kdu_byte vdata[4] = {9, 8, 7, 6};
    jp2_colour col_vnd = l_vnd.add_colour();
    col_vnd.init(uuid, 4, vdata);
    kdu_byte out_uuid[16] = {};
    int vlen = 0;
    EXPECT_TRUE(col_vnd.get_vendor_uuid(out_uuid));
    EXPECT_NE(col_vnd.get_vendor_data(&vlen), nullptr);
    EXPECT_EQ(vlen, 4);

    // Custom synthetic ICC profiles:
    // 1. Monochrome ICC with parametric curve (func types 0..4) & curve table.
    // Include gTRC/bTRC alongside kTRC so patch 09's 3-TRC check passes.
    for (int ftype = 0; ftype <= 4; ++ftype) {
      auto prof =
          BuildIccProfile(0x47524159 /*GRAY*/, 0x58595A20 /*XYZ*/,
                          {MakeParaCurveTag(0x6B545243 /*kTRC*/, ftype),
                           MakeParaCurveTag(0x67545243 /*gTRC*/, ftype),
                           MakeParaCurveTag(0x62545243 /*bTRC*/, ftype)});
      auto lc = jt.add_layer();
      jp2_colour col_c = lc.add_colour();
      col_c.init(prof.data());
      jp2_colour_converter conv;
      EXPECT_TRUE(conv.init(col_c));
      fill_lines();
      EXPECT_TRUE(conv.convert_lum(lum_f));
    }
    // 2. RGB ICC with matrix + curve table (256-entry).
    {
      auto prof = BuildIccProfile(
          0x52474220 /*RGB */, 0x58595A20 /*XYZ */,
          {MakeXyzTag(0x7258595A, 0.436, 0.222, 0.014),
           MakeXyzTag(0x6758595A, 0.385, 0.717, 0.097),
           MakeXyzTag(0x6258595A, 0.143, 0.061, 0.714),
           MakeCurvTableTag(0x72545243, 32), MakeCurvGammaTag(0x67545243, 2.2),
           MakeParaCurveTag(0x62545243, 3)});
      auto lc = jt.add_layer();
      jp2_colour col_c = lc.add_colour();
      col_c.init(prof.data());
      jp2_colour_converter conv;
      EXPECT_TRUE(conv.init(col_c, true, false));
      fill_lines();
      EXPECT_TRUE(conv.convert_rgb(r_f, g_f, b_f));
      EXPECT_TRUE(conv.convert_rgb(r_i, g_i, b_i));
    }
    // 3. Unrestricted ICC with mft1 / mft2 A2B0 1D/3D LUT.
    for (bool is16 : {false, true}) {
      auto prof = BuildIccProfile(
          0x52474220, 0x4C616220 /*Lab */,
          {MakeIdentityLutTag(0x41324230 /*A2B0*/, is16, 3, 3, 2)},
          0x73636E72 /*scnr*/);
      auto lc = jt.add_layer();
      jp2_colour col_c = lc.add_colour();
      col_c.init(prof.data());
      jp2_colour_converter conv;
      conv.init(col_c);
    }
    jt.close();
    ftgt.close();
  }
}

TEST(Jp2Test, SourceAndTargetHeaderValidationErrors) {
  ScopedMessageHandlers handlers;

  // Empty / non-JP2 / missing ftyp / wrong brand / missing jp2h / missing ihdr.
  auto try_open_read = [](const std::vector<kdu_byte>& bytes,
                          bool ret_incompat) {
    MemSource msrc(bytes);
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    jp2_source src;
    int res = 0;
    try {
      if (!src.open(&fsrc)) return -2;
      res = src.read_header(ret_incompat);
    } catch (...) {
      src.close();
      fsrc.close();
      throw;
    }
    src.close();
    fsrc.close();
    return res;
  };

  EXPECT_EQ(try_open_read({0, 1, 2, 3}, false), -2);
  {
    std::vector<kdu_byte> bad_sig = {0,   0,   0, 12, 'j', 'P',
                                     ' ', ' ', 1, 2,  3,   4};
    EXPECT_EQ(try_open_read(bad_sig, true), -1);
    EXPECT_THROW(try_open_read(bad_sig, false), kdu_exception);
  }
  {
    std::vector<kdu_byte> no_ftyp;
    AppendBox(no_ftyp, jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
    EXPECT_EQ(try_open_read(no_ftyp, true), -2);
    EXPECT_THROW(try_open_read(no_ftyp, false), kdu_exception);
  }
  {
    std::vector<kdu_byte> wrong_brand;
    AppendBox(wrong_brand, jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
    std::vector<kdu_byte> ftyp;
    AppendBe32(ftyp, mj2_brand);
    AppendBe32(ftyp, 0);
    AppendBe32(ftyp, mj2_brand);
    AppendBox(wrong_brand, jp2_file_type_4cc, ftyp);
    EXPECT_EQ(try_open_read(wrong_brand, true), -3);
    EXPECT_THROW(try_open_read(wrong_brand, false), kdu_exception);
  }
  {
    std::vector<kdu_byte> no_jp2h;
    AppendBox(no_jp2h, jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
    std::vector<kdu_byte> ftyp;
    AppendBe32(ftyp, jp2_brand);
    AppendBe32(ftyp, 0);
    AppendBe32(ftyp, jp2_brand);
    AppendBox(no_jp2h, jp2_file_type_4cc, ftyp);
    EXPECT_EQ(try_open_read(no_jp2h, true), -2);
    EXPECT_THROW(try_open_read(no_jp2h, false), kdu_exception);
  }
  {
    std::vector<kdu_byte> bad_jp2h;
    AppendBox(bad_jp2h, jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
    std::vector<kdu_byte> ftyp;
    AppendBe32(ftyp, jp2_brand);
    AppendBe32(ftyp, 0);
    AppendBe32(ftyp, jp2_brand);
    AppendBox(bad_jp2h, jp2_file_type_4cc, ftyp);
    AppendBox(bad_jp2h, jp2_header_4cc, {});
    EXPECT_THROW(try_open_read(bad_jp2h, false), kdu_exception);
  }

  // Round-trip via jp2_target -> jp2_source + get_metadata_memory checks.
  MemTarget mtgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mtgt);
  jp2_target tgt;
  tgt.open(&ftgt);
  const int initial_mem = tgt.get_metadata_memory();
  EXPECT_GT(initial_mem, 0);
  tgt.access_dimensions().init(kdu_coords(16, 16), 3);
  for (int c = 0; c < 3; ++c) {
    tgt.access_dimensions().set_precision(c, 8, false);
  }
  tgt.access_colour().init(kdu_supp::JP2_sRGB_SPACE);
  tgt.write_header();
  EXPECT_GT(tgt.get_metadata_memory(), initial_mem);
  tgt.open_codestream(true);
  CompressMinimalCodestream(tgt, 16, 16, 3);
  tgt.close();
  ftgt.close();

  MemSource msrc(mtgt.data());
  jp2_family_src fsrc;
  fsrc.open(&msrc);
  jp2_source src;
  ASSERT_TRUE(src.open(&fsrc));
  ASSERT_EQ(src.read_header(), 1);
  EXPECT_GT(src.get_metadata_memory(), 0);
  EXPECT_TRUE(src.access_dimensions().exists());
  EXPECT_TRUE(src.access_colour().exists());
  EXPECT_TRUE(src.access_palette().exists());
  EXPECT_TRUE(src.access_channels().exists());
  EXPECT_TRUE(src.access_resolution().exists());
  src.close();
  fsrc.close();
}

TEST(Jp2Test, BoxIOFileRewriteSimulationAndPlaceholders) {
  ScopedMessageHandlers handlers;
  auto write_str = [](jp2_output_box& box, const char* s) {
    return box.write(reinterpret_cast<const kdu_byte*>(s),
                     static_cast<int>(std::strlen(s)));
  };

  // 1. Real FILE* target & source: regular, long-header, reopen, rewrite,
  //    set_rubber_length, set_target_size, open_as, load_in_memory.
  const std::string path = ::testing::TempDir() + "/jp2_test_boxes.jp2";
  {
    jp2_family_tgt ftgt;
    ftgt.open(path.c_str());

    // Write a 12-byte box, close it, then reopen and rewrite box type + tail.
    jp2_output_box b1;
    b1.open(&ftgt, jp2_label_4cc);
    write_str(b1, "abcdefgh");
    EXPECT_TRUE(b1.close());

    EXPECT_GE(b1.reopen(jp2_xml_4cc, 4), 4);
    write_str(b1, "WXYZ");
    EXPECT_TRUE(b1.close());

    // Buffered rewrite inside open box (start_rewrite / end_rewrite).
    jp2_output_box b2;
    b2.open(&ftgt, jp2_uuid_4cc);
    kdu_byte u16[16] = {1, 2, 3, 4};
    b2.write(u16, 16);
    EXPECT_TRUE(b2.start_rewrite(4));
    write_str(b2, "TAIL");
    EXPECT_TRUE(b2.end_rewrite());
    b2.use_long_header();
    EXPECT_TRUE(b2.close());

    // Test set_target_size on FILE* after pre-buffering bytes.
    jp2_output_box b3;
    b3.open(&ftgt, jp2_label_4cc);
    write_str(b3, "1234");
    b3.set_target_size(8);
    write_str(b3, "5678");
    EXPECT_TRUE(b3.close());

    // Standalone buffered box written via write_box + force_headerless.
    jp2_output_box standalone;
    standalone.open(static_cast<jp2_family_tgt*>(nullptr), jp2_xml_4cc);
    write_str(standalone, "abcd");
    standalone.use_long_header();
    EXPECT_GT(standalone.write_box(&ftgt, false), 4);
    EXPECT_EQ(standalone.write_box(&ftgt, true), 4);
    standalone.set_target_size(4);
    EXPECT_TRUE(standalone.close());

    // Nested super-box with reopen, set_rubber_length (must be last in file).
    jp2_output_box super;
    super.open(&ftgt, jp2_header_4cc);
    jp2_output_box sub;
    sub.open(&super, jp2_label_4cc);
    write_str(sub, "sub12345");
    EXPECT_TRUE(sub.close());
    EXPECT_GE(sub.reopen(jp2_xml_4cc, 3), 5);
    write_str(sub, "NEW");
    EXPECT_TRUE(sub.close());
    EXPECT_TRUE(super.close());
    // Reopen child AFTER parent super-box was closed.
    EXPECT_GE(sub.reopen(jp2_label_4cc, 0), 8);
    write_str(sub, "OLD12345");
    EXPECT_TRUE(sub.close());

    // Final rubber-length box on FILE* with buffered data flushed at trigger.
    jp2_output_box tail;
    tail.open(&ftgt, jp2_label_4cc);
    write_str(tail, "head");
    tail.set_rubber_length();
    write_str(tail, "body");
    EXPECT_TRUE(tail.close());

    ftgt.close();
  }

  // Read back from FILE* + test load_in_memory on rubber_length box + open_as.
  {
    jp2_family_src fsrc;
    fsrc.open(path.c_str());

    jp2_input_box in;
    ASSERT_TRUE(in.open(&fsrc));
    EXPECT_EQ(in.get_box_type(), jp2_xml_4cc);
    char buf[16] = {};
    in.read(reinterpret_cast<kdu_byte*>(buf), 8);
    EXPECT_EQ(std::string(buf, 8), "abcdWXYZ");
    jp2_locator loc1 = in.get_locator();
    EXPECT_TRUE(in.close());

    // open_as on exact locator + content locator.
    jp2_locator cont_loc = loc1;
    cont_loc.set_file_pos(loc1.get_file_pos() + 8);
    ASSERT_TRUE(in.open_as(jp2_label_4cc, &fsrc, loc1, cont_loc, 8));
    EXPECT_EQ(in.get_box_type(), jp2_label_4cc);
    EXPECT_TRUE(in.load_in_memory(16));
    EXPECT_TRUE(in.is_complete());
    kdu_byte r8[8] = {};
    EXPECT_EQ(in.read(r8, 8), 8);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(r8), 8), "abcdWXYZ");
    EXPECT_TRUE(in.close());
    EXPECT_THROW(in.open_as(jp2_label_4cc, nullptr, loc1, cont_loc, 4),
                 kdu_exception);

    // Advance to the trailing rubber-length box and load_in_memory on FILE*.
    while (in.open_next()) {
      if (!in.exists()) break;
      kdu_uint32 btype = in.get_box_type();
      if (in.get_remaining_bytes() == std::numeric_limits<kdu_long>::max() ||
          btype == jp2_label_4cc) {
        if (in.get_box_bytes() == 0) {
          EXPECT_TRUE(in.load_in_memory(64));
          char tbuf[16] = {};
          EXPECT_EQ(in.read(reinterpret_cast<kdu_byte*>(tbuf), 8), 8);
          EXPECT_EQ(std::string(tbuf, 8), "headbody");
        }
      }
      in.close();
    }
    fsrc.close();
  }
  std::remove(path.c_str());

  // 2. Indirect (MemTarget) reopen, start_rewrite, end_rewrite,
  // set_target_size,
  //    set_rubber_length + error checks.
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);

    jp2_output_box bx;
    bx.open(&ftgt, jp2_label_4cc);
    write_str(bx, "12345678");
    EXPECT_TRUE(bx.start_rewrite(4));
    write_str(bx, "abcd");
    EXPECT_TRUE(bx.end_rewrite());
    EXPECT_TRUE(bx.close());
    EXPECT_GE(bx.reopen(jp2_xml_4cc, 2), 6);
    write_str(bx, "XY");
    EXPECT_TRUE(bx.close());

    jp2_output_box ts;
    ts.open(&ftgt, jp2_label_4cc);
    write_str(ts, "pre");
    ts.set_target_size(6);
    EXPECT_THROW(ts.set_target_size(8), kdu_exception);
    EXPECT_THROW(ts.set_rubber_length(), kdu_exception);
    write_str(ts, "suf");
    EXPECT_TRUE(ts.close());

    jp2_output_box rb;
    rb.open(&ftgt, jp2_label_4cc);
    write_str(rb, "buf");
    rb.set_rubber_length();
    EXPECT_THROW(rb.set_target_size(10), kdu_exception);
    write_str(rb, "tail");
    EXPECT_TRUE(rb.close());
    ftgt.close();
  }

  // 3. Simulation mode: jp2_family_tgt::open(simulated_start_pos).
  {
    jp2_family_tgt ftgt;
    ftgt.open(static_cast<kdu_long>(100));

    jp2_output_box bx;
    bx.open(&ftgt, jp2_label_4cc);
    write_str(bx, "12345678");
    EXPECT_TRUE(bx.close());
    EXPECT_GE(bx.reopen(jp2_xml_4cc, 4), 4);
    write_str(bx, "ABCD");
    EXPECT_TRUE(bx.close());

    jp2_output_box sb;
    sb.open(&ftgt, jp2_header_4cc);
    jp2_output_box sub;
    sub.open(&sb, jp2_label_4cc);
    write_str(sub, "sub");
    sub.set_target_size(5);
    write_str(sub, "12");
    EXPECT_TRUE(sub.close());
    sb.set_rubber_length();
    EXPECT_TRUE(sb.close());

    jp2_output_box wr;
    wr.open(static_cast<jp2_family_tgt*>(nullptr), jp2_label_4cc);
    write_str(wr, "hello");
    EXPECT_GT(wr.write_box(&ftgt, false), 5);
    wr.set_rubber_length();
    EXPECT_TRUE(wr.close());

    ftgt.close();
  }

  // 4. Non-seekable MemSource sequential skipping + Placeholder (phld) boxes.
  {
    std::vector<kdu_byte> raw;
    // Box 1: xml with 64 bytes payload (skipped partially on non-seekable
    // read).
    std::vector<kdu_byte> p1(64, 0x5A);
    AppendBox(raw, jp2_xml_4cc, p1);
    // Box 2: super-box containing two sub-boxes on non-seekable source.
    std::vector<kdu_byte> sub1, sub2, super_payload;
    AppendBox(super_payload, jp2_label_4cc, {'l', 'a', 'b', 'l'});
    AppendBox(super_payload, jp2_xml_4cc, {'x', 'm', 'l', '!'});
    AppendBox(raw, jp2_header_4cc, super_payload);

    MemSource nsrc(raw, /*can_seek=*/false);
    jp2_family_src nfsrc;
    nfsrc.open(&nsrc);
    jp2_input_box in;
    ASSERT_TRUE(in.open(&nfsrc));
    kdu_byte b4[4];
    EXPECT_EQ(in.read(b4, 4), 4);
    EXPECT_FALSE(in.close());  // Unread bytes remain on non-seekable source.
    ASSERT_TRUE(in.open_next());
    EXPECT_EQ(in.get_box_type(), jp2_header_4cc);
    jp2_input_box sub;
    ASSERT_TRUE(sub.open(&in));
    EXPECT_EQ(sub.get_box_type(), jp2_label_4cc);
    EXPECT_EQ(sub.read(b4, 4), 4);
    EXPECT_TRUE(sub.close());
    ASSERT_TRUE(sub.open_next());
    EXPECT_EQ(sub.get_box_type(), jp2_xml_4cc);
    EXPECT_EQ(sub.read(b4, 4), 4);
    EXPECT_TRUE(sub.close());
    EXPECT_TRUE(in.close());
    nfsrc.close();
  }

  // 5. Placeholder (phld) box formats parsed by read_box_header.
  {
    // flags=0 (no access to original)
    std::vector<kdu_byte> phld0(20, 0);
    AppendBe32(phld0, 12);           // Orig LBox
    AppendBe32(phld0, jp2_xml_4cc);  // Orig TBox
    // Wait: phld payload starts with: flags(4), OrigBinID(8), OrigLBox(4),
    // OrigTBox(4) = 20 bytes!
    phld0.clear();
    AppendBe32(phld0, 0);  // flags=0
    AppendBe32(phld0, 0);
    AppendBe32(phld0, 1);            // OrigBinID=1
    AppendBe32(phld0, 28);           // OrigLBox=28 (matches phld box size)
    AppendBe32(phld0, jp2_xml_4cc);  // OrigTBox

    // flags=1 (prefer_originals -> original visible, short length)
    std::vector<kdu_byte> phld1;
    AppendBe32(phld1, 1);
    AppendBe32(phld1, 0);
    AppendBe32(phld1, 2);
    AppendBe32(phld1, 28);
    AppendBe32(phld1, jp2_label_4cc);

    // flags=2 (stream equivalent with short equiv box length)
    std::vector<kdu_byte> phld2;
    AppendBe32(phld2, 2);
    AppendBe32(phld2, 0);
    AppendBe32(phld2, 3);
    AppendBe32(phld2, 44);  // OrigLBox=44 (matches 8+36 phld box)
    AppendBe32(phld2, jp2_label_4cc);
    AppendBe32(phld2, 0);
    AppendBe32(phld2, 4);            // EquivBinID=4
    AppendBe32(phld2, 20);           // EquivLBox=20
    AppendBe32(phld2, jp2_xml_4cc);  // EquivTBox

    // flags=2|4|8 (codestream equivalent + EquivLBox==1 64-bit length +
    // Ncodestream range)
    std::vector<kdu_byte> phld3;
    AppendBe32(phld3, 2 | 4 | 8);
    AppendBe32(phld3, 0);
    AppendBe32(phld3, 5);
    AppendBe32(phld3, 1);  // OrigLBox==1 extended
    AppendBe32(phld3, jp2_codestream_4cc);
    AppendBe32(phld3, 0);
    AppendBe32(phld3, 68);  // Orig XLBox=68
    AppendBe32(phld3, 0);
    AppendBe32(phld3, 6);  // EquivBinID=6
    AppendBe32(phld3, 1);  // EquivLBox==1 extended
    AppendBe32(phld3, jp2_codestream_4cc);
    AppendBe32(phld3, 0);
    AppendBe32(phld3, 40);  // Equiv XLBox=40
    AppendBe32(phld3, 0);
    AppendBe32(phld3, 0);  // codestream_min=0
    AppendBe32(phld3, 2);  // num_codestreams=2

    std::vector<kdu_byte> stream;
    AppendBox(stream, jp2_placeholder_4cc, phld1);
    AppendBox(stream, jp2_placeholder_4cc, phld0);
    AppendBox(stream, jp2_placeholder_4cc, phld2);
    AppendBox(stream, jp2_placeholder_4cc, phld3);

    MemSource msrc(stream);
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    jp2_input_box in;
    ASSERT_TRUE(in.open(&fsrc));
    EXPECT_EQ(in.get_box_type(), jp2_label_4cc);
    in.close();
    ASSERT_TRUE(
        in.open_next());  // Skips flags=0 phld0, opens phld2 stream equiv
    EXPECT_EQ(in.get_box_type(), jp2_xml_4cc);
    in.close();
    ASSERT_TRUE(in.open_next());  // Opens phld3 codestream equiv
    EXPECT_EQ(in.get_box_type(), jp2_codestream_4cc);
    in.close();
    EXPECT_FALSE(in.open_next());
    fsrc.close();

    // Direct open on flags=0 phld throws error 13 (box_type == 0).
    {
      std::vector<kdu_byte> deny_phld;
      AppendBox(deny_phld, jp2_placeholder_4cc, phld0);
      MemSource dsrc(deny_phld);
      jp2_family_src dfsrc;
      dfsrc.open(&dsrc);
      jp2_input_box din;
      EXPECT_THROW(din.open(&dfsrc), kdu_exception);
      dfsrc.close();
    }

    // Short phld LBox=12 (<28 bytes) throws error.
    std::vector<kdu_byte> short_phld;
    AppendBox(short_phld, jp2_placeholder_4cc, {0, 0, 0, 0});
    MemSource bsrc(short_phld);
    jp2_family_src bfsrc;
    bfsrc.open(&bsrc);
    jp2_input_box bin;
    EXPECT_THROW(bin.open(&bfsrc), kdu_exception);
    bfsrc.close();

    // Truncated EOF inside LBox=28 phld returns false from open().
    {
      std::vector<kdu_byte> eof_phld;
      AppendBe32(eof_phld, 28);
      AppendBe32(eof_phld, jp2_placeholder_4cc);
      AppendBe32(eof_phld, 1);
      MemSource esrc(eof_phld);
      jp2_family_src efsrc;
      efsrc.open(&esrc);
      jp2_input_box ein;
      EXPECT_FALSE(ein.open(&efsrc));
      efsrc.close();
    }

    // Illegal OrigLBox=4 inside phld throws error 7.
    {
      std::vector<kdu_byte> bad_len;
      AppendBe32(bad_len, 1);
      AppendBe32(bad_len, 0);
      AppendBe32(bad_len, 1);
      AppendBe32(bad_len, 4);
      AppendBe32(bad_len, jp2_xml_4cc);
      std::vector<kdu_byte> raw_bad;
      AppendBox(raw_bad, jp2_placeholder_4cc, bad_len);
      MemSource lsrc(raw_bad);
      jp2_family_src lfsrc;
      lfsrc.open(&lsrc);
      jp2_input_box lin;
      EXPECT_THROW(lin.open(&lfsrc), kdu_exception);
      lfsrc.close();
    }
  }
}

TEST(Jp2Test, ColourRoundTripAllSpacesAndIccVariants) {
  ScopedMessageHandlers handlers;

  // Serialize a JPX container containing layers for every enumerated space,
  // default/custom CIELab, default/custom CIEJab, vendor UUID, synthesized RGB
  // ICC, and synthetic A2B0/TRCs ICC profiles, then parse back and verify.
  const jp2_colour_space spaces[] = {
      kdu_supp::JP2_bilevel1_SPACE, kdu_supp::JP2_bilevel2_SPACE,
      kdu_supp::JP2_sLUM_SPACE,     kdu_supp::JP2_sRGB_SPACE,
      kdu_supp::JP2_esRGB_SPACE,    kdu_supp::JP2_ROMMRGB_SPACE,
      kdu_supp::JP2_sYCC_SPACE,     kdu_supp::JP2_esYCC_SPACE,
      kdu_supp::JP2_YCbCr1_SPACE,   kdu_supp::JP2_YCbCr2_SPACE,
      kdu_supp::JP2_YCbCr3_SPACE,   kdu_supp::JP2_PhotoYCC_SPACE,
      kdu_supp::JP2_YPbPr60_SPACE,  kdu_supp::JP2_YPbPr50_SPACE,
      kdu_supp::JP2_CMY_SPACE,      kdu_supp::JP2_CMYK_SPACE,
      kdu_supp::JP2_YCCK_SPACE,     kdu_supp::JP2_CIELab_SPACE,
      kdu_supp::JP2_CIEJab_SPACE,
  };

  MemTarget mtgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mtgt);
  jpx_target jt;
  jt.open(&ftgt);

  auto add_cs_for_nch = [&](int nch) {
    auto cs = jt.add_codestream();
    cs.access_dimensions().init(kdu_coords(8, 8), nch);
    for (int c = 0; c < nch; ++c) {
      cs.access_dimensions().set_precision(c, 8, false);
    }
    return cs;
  };
  auto cs1 = add_cs_for_nch(1);
  auto cs3 = add_cs_for_nch(3);
  auto cs4 = add_cs_for_nch(4);

  auto map_cs = [](kdu_supp::jpx_layer_target lyr, int cs_idx, int nch) {
    lyr.access_channels().init(nch);
    for (int c = 0; c < nch; ++c) {
      lyr.access_channels().set_colour_mapping(c, c, -1, cs_idx);
    }
  };

  for (jp2_colour_space sp : spaces) {
    auto lyr = jt.add_layer();
    jp2_colour col = lyr.add_colour(/*prec=*/-1, /*approx=*/2);
    col.init(sp);
    EXPECT_EQ(col.get_approximation_level(), 2);
    int nch = col.get_num_colours();
    map_cs(lyr, nch == 1 ? 0 : (nch == 4 ? 2 : 1), nch);
  }
  // Custom CIELab with daylight illuminant temperature.
  {
    auto lyr = jt.add_layer();
    jp2_colour col = lyr.add_colour();
    col.init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 8, 255, 128, 8, 255, 128, 8,
             JP2_CIE_DAY, 6500);
    map_cs(lyr, 1, 3);
  }
  // Custom CIEJab.
  {
    auto lyr = jt.add_layer();
    jp2_colour col = lyr.add_colour();
    col.init(kdu_supp::JP2_CIEJab_SPACE, 100, 0, 8, 255, 128, 8, 255, 128, 8);
    map_cs(lyr, 1, 3);
  }
  // Synthesized RGB ICC (method 2).
  {
    auto lyr = jt.add_layer();
    jp2_colour col = lyr.add_colour();
    const double rx[2] = {0.64, 0.33}, gx[2] = {0.30, 0.60},
                 bx[2] = {0.15, 0.06};
    col.init(rx, gx, bx, 2.2, 0.05, 32, false);
    map_cs(lyr, 1, 3);
  }
  // Unrestricted ICC with Lab PCS + A2B0 (method 3).
  {
    auto lyr = jt.add_layer();
    jp2_colour col = lyr.add_colour();
    auto prof = BuildIccProfile(0x52474220, 0x4C616220,
                                {MakeIdentityLutTag(0x41324230, true, 3, 3, 2)},
                                0x73636E72 /*scnr*/);
    col.init(prof.data());
    map_cs(lyr, 1, 3);
  }
  // Vendor UUID colour space (method 4, with copy & compare check).
  {
    auto lyr = jt.add_layer();
    jp2_colour col = lyr.add_colour();
    kdu_byte uuid[16] = {0xAA, 0xBB, 0xCC, 0xDD};
    kdu_byte data[4] = {1, 2, 3, 4};
    col.init(uuid, 4, data);
    // jp2_colour::init(vendor) leaves num_colours==0 unless copied/used
    // carefully.
    map_cs(lyr, 1, 3);
  }

  jt.write_headers();
  for (auto* cs_t : {&cs1, &cs3, &cs4}) {
    auto* stream = cs_t->open_stream();
    int nc = cs_t->access_dimensions().get_num_components();
    CompressMinimalCodestream(*stream, 8, 8, nc);
    stream->close();
  }
  jt.close();
  ftgt.close();

  // Read back all layers via jpx_source and exercise
  // compare/copy/check_cie_default.
  MemSource msrc(mtgt.data());
  jp2_family_src fsrc;
  fsrc.open(&msrc);
  jpx_source jsrc;
  ASSERT_EQ(jsrc.open(&fsrc, false), 1);
  int num_layers = 0;
  EXPECT_TRUE(jsrc.count_compositing_layers(num_layers));
  EXPECT_GE(num_layers, 22);
  jp2_colour c0 = jsrc.access_layer(0).access_colour(0);
  EXPECT_EQ(c0.get_space(), kdu_supp::JP2_bilevel1_SPACE);
  EXPECT_EQ(c0.get_precedence(), -1);
  EXPECT_LE(jsrc.access_layer(1).access_colour(0).get_approximation_level(), 4);
  // Layer 17 is default CIELab after finalization; Layer 19 is custom CIELab.
  EXPECT_TRUE(jsrc.access_layer(17).access_colour(0).check_cie_default());
  EXPECT_FALSE(jsrc.access_layer(19).access_colour(0).check_cie_default());

  // Copy & compare round-tripped colour objects.
  {
    MemTarget mt2;
    jp2_family_tgt ft2;
    ft2.open(&mt2);
    jpx_target jt2;
    jt2.open(&ft2);
    auto l0 = jt2.add_layer();
    jp2_colour dst0 = l0.add_colour();
    dst0.copy(jsrc.access_layer(3).access_colour(0));  // sRGB
    EXPECT_EQ(dst0.get_space(), kdu_supp::JP2_sRGB_SPACE);
    jt2.close();
    ft2.close();
  }
  jsrc.close();
  fsrc.close();

  // Additional ICC colour space signatures & curve tables & error paths.
  const kdu_uint32 clr_spaces[] = {
      0x58595A20,  // XYZ
      0x4C757620,  // Luv
      0x59436272,  // YCbCr ('YCbr')
      0x59787920,  // Yxy
      0x48535620,  // HSV
      0x484C5320,  // HLS
      0x434D594B,  // CMYK
      0x434D5920,  // CMY
      0x32434C52, 0x33434C52, 0x34434C52, 0x35434C52, 0x36434C52,
      0x37434C52, 0x38434C52, 0x39434C52, 0x41434C52, 0x42434C52,
      0x43434C52, 0x44434C52, 0x45434C52, 0x46434C52,
  };
  for (kdu_uint32 sp : clr_spaces) {
    auto prof = BuildIccProfile(
        sp, 0x58595A20,
        {MakeCurvTableTag(0x72545243, 0),  // Identity 0-entry curve
         MakeCurvGammaTag(0x67545243, 1.8), MakeCurvTableTag(0x62545243, 16)},
        0x73636E72 /*scnr*/);
    MemTarget t;
    jp2_family_tgt f;
    f.open(&t);
    jpx_target j;
    j.open(&f);
    jp2_colour c = j.add_layer().add_colour();
    c.init(prof.data());
    j.close();
    f.close();
  }
  // Bad ICC profiles (<132 bytes, unknown color space, unknown PCS).
  {
    MemTarget t;
    jp2_family_tgt f;
    f.open(&t);
    jpx_target j;
    j.open(&f);
    std::vector<kdu_byte> tiny_prof(132, 0);
    EXPECT_THROW(j.add_layer().add_colour().init(tiny_prof.data()),
                 kdu_exception);
    auto bad_cs = BuildIccProfile(0x3F3F3F3F, 0x58595A20, {}, 0x73636E72);
    EXPECT_THROW(j.add_layer().add_colour().init(bad_cs.data()), kdu_exception);
    auto bad_pcs = BuildIccProfile(0x52474220, 0x3F3F3F3F, {}, 0x73636E72);
    EXPECT_THROW(j.add_layer().add_colour().init(bad_pcs.data()),
                 kdu_exception);
    j.close();
    f.close();
  }
}

TEST(Jp2Test, ChannelsFormatsPaletteFloatAndHeaderValidation) {
  ScopedMessageHandlers handlers;

  // 1. Opacity / premult mapping: global + per-channel + SPLIT_EXP + FLOAT +
  //    FIXPOINT + cdef round-trip.
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    jpx_target jt;
    jt.open(&ftgt);
    auto cs = jt.add_codestream();
    cs.access_dimensions().init(kdu_coords(8, 8), 6);
    for (int c = 0; c < 6; ++c) {
      cs.access_dimensions().set_precision(c, 8, false);
    }

    // Layer 0: global opacity with SPLIT_EXP, overridden by per-channel FLOAT,
    // plus premult FIXPOINT on channel 1.
    auto l0 = jt.add_layer();
    l0.add_colour().init(kdu_supp::JP2_sRGB_SPACE);
    auto ch0 = l0.access_channels();
    ch0.init(3);
    int split_p[3] = {3, -1, 0};
    EXPECT_TRUE(ch0.set_colour_mapping(0, 0, -1, 0));
    EXPECT_TRUE(ch0.set_colour_mapping(1, 1, -1, 0));
    EXPECT_TRUE(ch0.set_colour_mapping(2, 2, -1, 0));
    EXPECT_TRUE(ch0.set_opacity_mapping(-1, 3, -1, 0,
                                        JP2_CHANNEL_FORMAT_SPLIT_EXP, split_p));
    int got_c = -1, got_lut = -1, got_cs = -1, got_fmt = -1, got_p[3] = {};
    EXPECT_TRUE(
        ch0.get_opacity_mapping(-1, got_c, got_lut, got_cs, got_fmt, got_p));
    EXPECT_EQ(got_fmt, JP2_CHANNEL_FORMAT_SPLIT_EXP);
    EXPECT_EQ(got_p[0], 3);
    // Cancel global and assign per-channel opacity + global premult SPLIT_EXP.
    int flt_p[1] = {3};
    int fix_p[1] = {4};
    EXPECT_TRUE(
        ch0.set_opacity_mapping(0, 3, -1, 0, JP2_CHANNEL_FORMAT_FLOAT, flt_p));
    EXPECT_TRUE(ch0.set_premult_mapping(-1, 4, -1, 0,
                                        JP2_CHANNEL_FORMAT_SPLIT_EXP, split_p));
    EXPECT_TRUE(
        ch0.get_premult_mapping(-1, got_c, got_lut, got_cs, got_fmt, got_p));
    EXPECT_EQ(got_fmt, JP2_CHANNEL_FORMAT_SPLIT_EXP);
    EXPECT_TRUE(ch0.set_premult_mapping(1, 5, -1, 0,
                                        JP2_CHANNEL_FORMAT_FIXPOINT, fix_p));
    EXPECT_TRUE(
        ch0.get_opacity_mapping(0, got_c, got_lut, got_cs, got_fmt, got_p));
    EXPECT_EQ(got_fmt, JP2_CHANNEL_FORMAT_FLOAT);
    EXPECT_TRUE(
        ch0.get_premult_mapping(1, got_c, got_lut, got_cs, got_fmt, got_p));
    EXPECT_EQ(got_fmt, JP2_CHANNEL_FORMAT_FIXPOINT);
    // Invalid params error check (use a separate target so ch0 isn't left
    // mutated).
    {
      MemTarget bmt;
      jp2_family_tgt bft;
      bft.open(&bmt);
      jpx_target bjt;
      bjt.open(&bft);
      auto bl = bjt.add_layer();
      auto bch = bl.access_channels();
      bch.init(3);
      EXPECT_THROW(bch.set_opacity_mapping(0, 3, -1, 0,
                                           JP2_CHANNEL_FORMAT_FLOAT, nullptr),
                   kdu_exception);
      EXPECT_THROW(bch.set_premult_mapping(
                       0, 3, -1, 0, JP2_CHANNEL_FORMAT_FIXPOINT, nullptr),
                   kdu_exception);
      bjt.close();
      bft.close();
    }

    // Layer 1: global opacity default mapped -> cdef / opct.
    auto l1 = jt.add_layer();
    l1.add_colour().init(kdu_supp::JP2_sRGB_SPACE);
    auto ch1 = l1.access_channels();
    ch1.init(3);
    EXPECT_TRUE(ch1.set_colour_mapping(0, 0, -1, 0));
    EXPECT_TRUE(ch1.set_colour_mapping(1, 1, -1, 0));
    EXPECT_TRUE(ch1.set_colour_mapping(2, 2, -1, 0));
    EXPECT_TRUE(ch1.set_opacity_mapping(-1, 3, -1, 0));

    // Copy channel map into layer 2.
    auto l2 = jt.add_layer();
    l2.add_colour().init(kdu_supp::JP2_sRGB_SPACE);
    l2.access_channels().copy(ch0);

    jt.write_headers();
    auto* stream = cs.open_stream();
    CompressMinimalCodestream(*stream, 8, 8, 6);
    stream->close();
    jt.close();
    ftgt.close();

    MemSource msrc(mtgt.data());
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    jpx_source jsrc;
    ASSERT_EQ(jsrc.open(&fsrc, false), 1);
    int c = -1, l = -1, csi = -1, fmt = -1, p[3] = {};
    EXPECT_TRUE(jsrc.access_layer(0).access_channels().get_opacity_mapping(
        0, c, l, csi, fmt, p));
    EXPECT_TRUE(jsrc.access_layer(0).access_channels().get_premult_mapping(
        1, c, l, csi, fmt, p));
    EXPECT_TRUE(jsrc.access_layer(1).access_channels().get_opacity_mapping(
        0, c, l, csi, fmt, p));
    jsrc.close();
    fsrc.close();
  }

  // 2. jp2_palette::get_lut with JP2_CHANNEL_FORMAT_FLOAT (unsigned & signed)
  //    for both kdu_sample32 and kdu_sample16 + 16-bit/32-bit pclr round-trip.
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    jp2_target tgt;
    tgt.open(&ftgt);
    tgt.access_dimensions().init(kdu_coords(8, 8), 1);
    tgt.access_dimensions().set_precision(0, 8, false);
    tgt.access_colour().init(kdu_supp::JP2_sRGB_SPACE);

    jp2_palette pal = tgt.access_palette();
    pal.init(3, 4);
    kdu_int32 lut_u16[4] = {0, 0x3C00, 0x4000,
                            0x7FFF};  // IEEE fp16-like unsigned
    kdu_int32 lut_s16[4] = {-100, 0, 100, 32767};
    kdu_int32 lut_u32[4] = {0x3F800000, 0x40000000, 0x40400000, 0x40800000};
    pal.set_lut(0, lut_u16, 16, /*is_signed=*/false);
    pal.set_lut(1, lut_s16, 16, /*is_signed=*/true);
    pal.set_lut(2, lut_u32, 32, /*is_signed=*/false);

    tgt.access_channels().init(3);
    tgt.access_channels().set_colour_mapping(0, 0, 0);
    tgt.access_channels().set_colour_mapping(1, 0, 1);
    tgt.access_channels().set_colour_mapping(2, 0, 2);

    float flt_out[4] = {};
    EXPECT_TRUE(pal.get_lut(0, flt_out, JP2_CHANNEL_FORMAT_FLOAT, 5));
    EXPECT_TRUE(pal.get_lut(1, flt_out, JP2_CHANNEL_FORMAT_FLOAT, 5));
    EXPECT_TRUE(pal.get_lut(2, flt_out, JP2_CHANNEL_FORMAT_FLOAT, 8));

    kdu_sample16 s16_out[4] = {};
    EXPECT_TRUE(pal.get_lut(0, s16_out, JP2_CHANNEL_FORMAT_FLOAT, 5));
    EXPECT_TRUE(pal.get_lut(1, s16_out, JP2_CHANNEL_FORMAT_FLOAT, 5));
    EXPECT_TRUE(pal.get_lut(2, s16_out, JP2_CHANNEL_FORMAT_FLOAT, 8));

    tgt.write_header();
    tgt.open_codestream(true);
    CompressMinimalCodestream(tgt, 8, 8, 1);
    tgt.close();
    ftgt.close();

    MemSource msrc(mtgt.data());
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    jp2_source src;
    ASSERT_TRUE(src.open(&fsrc));
    ASSERT_EQ(src.read_header(), 1);
    EXPECT_EQ(src.access_palette().get_num_entries(), 4);
    EXPECT_EQ(src.access_palette().get_num_luts(), 3);
    src.close();
    fsrc.close();
  }

  // 3. jp2_dimensions::init(siz_params*) + finalize_compatibility(kdu_params*)
  //    with Part-2 MCT / MCC / MCO clusters via kdu_codestream.
  {
    kdu_core::siz_params siz;
    siz.set(Sdims, 0, 0, 16);
    siz.set(Sdims, 0, 1, 16);
    siz.set(Scomponents, 0, 0, 3);
    for (int c = 0; c < 3; ++c) {
      siz.set(Sprecision, c, 0, 8);
      siz.set(Ssigned, c, 0, false);
    }
    EXPECT_TRUE(siz.parse_string("Sextensions=MCT"));
    EXPECT_TRUE(siz.parse_string("Mcomponents=3"));
    EXPECT_TRUE(siz.parse_string("Mprecision=8"));
    EXPECT_TRUE(siz.parse_string("Msigned=no"));
    static_cast<kdu_core::kdu_params&>(siz).finalize();

    MemTarget cs_tgt;
    kdu_core::kdu_codestream cs;
    cs.create(&siz, &cs_tgt);
    kdu_core::siz_params* root = cs.access_siz();
    for (const char* p : {
             "Mmatrix_size:I1=9",
             "Mmatrix_coeffs:I1=1.0,0.25,0.0,0.0,1.0,-0.25,0.25,0.0,1.0",
             "Mstage_inputs:I1={0,2}",
             "Mstage_outputs:I1={0,2}",
             "Mstage_collections:I1={3,3}",
             "Mstage_xforms:I1={MATRIX,0,1,0,0}",
             "Mnum_stages=1",
             "Mstages=1",
         }) {
      EXPECT_TRUE(root->parse_string(p));
    }
    root->finalize_all();

    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    jp2_target tgt;
    tgt.open(&ftgt);
    tgt.access_dimensions().init(root, false);
    tgt.access_dimensions().finalize_compatibility(root);
    EXPECT_EQ(tgt.access_dimensions().get_size().x, 16);
    EXPECT_FALSE(tgt.access_dimensions().is_jp2_compatible(false));
    tgt.close();
    ftgt.close();
    cs.destroy();
  }

  // 4. jp2_header::write validation failures in jp2_target (JP2 vs JPH).
  {
    auto try_jp2_header = [&](auto setup_fn, bool as_jph = false) {
      MemTarget mtgt;
      jp2_family_tgt ftgt;
      ftgt.open(&mtgt);
      jp2_target tgt;
      tgt.open(&ftgt);
      tgt.access_dimensions().init(kdu_coords(8, 8), 3);
      for (int c = 0; c < 3; ++c) {
        tgt.access_dimensions().set_precision(c, 8, false);
      }
      if (as_jph) {
        tgt.access_dimensions().finalize_compatibility(0, Scap_P15, 0, 0, 0, 1);
      }
      setup_fn(tgt);
      try {
        tgt.write_header();
      } catch (...) {
        tgt.close();
        ftgt.close();
        throw;
      }
      tgt.close();
      ftgt.close();
    };

    // chroma key -> needs opct -> disallowed in JP2/JPH.
    EXPECT_THROW(try_jp2_header([](jp2_target& t) {
                   t.access_colour().init(kdu_supp::JP2_sRGB_SPACE);
                   t.access_channels().init(3);
                   t.access_channels().set_chroma_key(0, 0);
                 }),
                 kdu_exception);

    // Non-default pixel format -> needs pxfm -> disallowed in JP2.
    EXPECT_THROW(try_jp2_header([](jp2_target& t) {
                   t.access_colour().init(kdu_supp::JP2_sRGB_SPACE);
                   t.access_channels().init(3);
                   int p = 2;
                   t.access_channels().set_colour_mapping(
                       0, 0, -1, 0, JP2_CHANNEL_FORMAT_FIXPOINT, &p);
                 }),
                 kdu_exception);

    // Complex per-channel opacity disallowed in JPH.
    EXPECT_THROW(try_jp2_header(
                     [](jp2_target& t) {
                       t.access_colour().init(kdu_supp::JP2_sRGB_SPACE);
                       t.access_channels().init(3);
                       t.access_channels().set_opacity_mapping(0, 2);
                     },
                     /*as_jph=*/true),
                 kdu_exception);

    // Non-colour channel disallowed in baseline JP2.
    EXPECT_THROW(try_jp2_header([](jp2_target& t) {
                   t.access_colour().init(kdu_supp::JP2_sRGB_SPACE);
                   t.access_channels().init(3, 1);
                   t.access_channels().set_non_colour_mapping(0, 0xFFFF, 1, 2);
                 }),
                 kdu_exception);

    // JPX-only colour space (CMYK) disallowed in JP2 and JPH.
    EXPECT_THROW(try_jp2_header(
                     [](jp2_target& t) {
                       t.access_colour().init(kdu_supp::JP2_CMYK_SPACE);
                     },
                     /*as_jph=*/true),
                 kdu_exception);

    // JPH-only colour space (esRGB) disallowed in baseline JP2.
    EXPECT_THROW(try_jp2_header(
                     [](jp2_target& t) {
                       t.access_colour().init(kdu_supp::JP2_esRGB_SPACE);
                     },
                     /*as_jph=*/false),
                 kdu_exception);
  }

  // 5. j2_memsafe broker overlimit + jp2_data_references grow (>9 URLs) + close
  //    leak check.
  {
    kdu_membroker broker(/*limit_bytes=*/128);
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt, &broker);
    EXPECT_THROW(
        [&]() {
          jp2_output_box big;
          big.open(&ftgt, jp2_xml_4cc);
          std::vector<kdu_byte> data(2048, 0x11);
          big.write(data.data(), static_cast<int>(data.size()));
          big.close();
        }(),
        kdu_exception);
    ftgt.close();
  }
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    jpx_target jt;
    jt.open(&ftgt);
    jp2_data_references dref = jt.access_data_references();
    for (int i = 1; i <= 12; ++i) {
      std::string url = "http://example.com/item" + std::to_string(i);
      EXPECT_EQ(dref.add_url(url.c_str()), i);
    }
    EXPECT_STREQ(dref.get_url(1), "http://example.com/item1");
    EXPECT_STREQ(dref.get_url(12), "http://example.com/item12");
    jt.close();
    ftgt.close();
  }
}

class ChunkedMemSource : public MemSource {
 public:
  ChunkedMemSource(std::vector<kdu_byte> data, bool can_seek, int max_chunk)
      : MemSource(std::move(data), can_seek), max_chunk_(max_chunk) {}

  int read(kdu_byte* buf, int num_bytes) override {
    int n = num_bytes < max_chunk_ ? num_bytes : max_chunk_;
    return MemSource::read(buf, n);
  }

 private:
  int max_chunk_;
};

class TrackingMemBroker : public kdu_core::kdu_membroker {
 public:
  explicit TrackingMemBroker(kdu_core::kdu_int64 limit_bytes)
      : kdu_core::kdu_membroker(limit_bytes) {}

  void note_allocation_failure(kdu_core::kdu_int64 deficit_bytes,
                               kdu_core::kdu_int64 allocated_bytes,
                               bool alloc_failed) override {
    failures_++;
    last_deficit_ = deficit_bytes;
    last_allocated_ = allocated_bytes;
    last_alloc_failed_ = alloc_failed;
    kdu_core::kdu_membroker::note_allocation_failure(
        deficit_bytes, allocated_bytes, alloc_failed);
  }

  int failures() const { return failures_; }

 private:
  int failures_ = 0;
  kdu_core::kdu_int64 last_deficit_ = 0;
  kdu_core::kdu_int64 last_allocated_ = 0;
  bool last_alloc_failed_ = false;
};

template <typename Fn>
bool ExpectKduThrows(Fn&& fn) {
  try {
    fn();
  } catch (kdu_exception) {
    return true;
  }
  return false;
}

TEST(Jp2Test, OutputAndInputBoxRewriteFileSimulationAndBrokerLimits) {
  ScopedMessageHandlers handlers;
  const std::string tmp_path = testing::TempDir() + "/jp2_rewrite_test.jp2";
  std::remove(tmp_path.c_str());

  // 1. FILE*-backed jp2_family_tgt covering set_target_size, write_header_last,
  // start_rewrite/end_rewrite, reopen (errors + success), super-boxes,
  // set_rubber_length, and write_free_and_close.
  {
    jp2_family_tgt ftgt;
    ftgt.open(tmp_path.c_str());
    EXPECT_TRUE(ftgt.exists());

    // Box 1: fixed target size + start_rewrite/end_rewrite on FILE*.
    jp2_output_box box1;
    box1.open(&ftgt, 0x74657331 /*'tes1'*/);
    box1.set_target_size(16);
    kdu_byte payload1[16] = {1, 2,  3,  4,  5,  6,  7,  8,
                             9, 10, 11, 12, 13, 14, 15, 16};
    EXPECT_TRUE(box1.write(payload1, 8));
    EXPECT_TRUE(box1.start_rewrite(4));
    kdu_byte patch4[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    EXPECT_TRUE(box1.write(patch4, 4));
    EXPECT_TRUE(box1.end_rewrite());
    EXPECT_TRUE(box1.write(payload1 + 8, 8));
    // Reopen before close must return < 0.
    EXPECT_LT(box1.reopen(0x74657331, 0), 0);
    EXPECT_TRUE(box1.close());
    // Invalid offset reopen (< 0).
    EXPECT_LT(box1.reopen(0x74657331, 100), 0);
    // Valid reopen renaming 'tes1' -> 'tesA' and rewriting from offset 4.
    EXPECT_EQ(box1.reopen(0x74657341 /*'tesA'*/, 4), 12);
    kdu_byte rewrite4[4] = {0x11, 0x22, 0x33, 0x44};
    EXPECT_TRUE(box1.write(rewrite4, 4));
    EXPECT_TRUE(box1.close());

    // Box 2: write_header_last + >256 bytes + start_rewrite.
    jp2_output_box box2;
    box2.open(&ftgt, 0x74657332 /*'tes2'*/);
    box2.write_header_last();
    std::vector<kdu_byte> big(300, 0x5A);
    EXPECT_TRUE(box2.write(big.data(), static_cast<int>(big.size())));
    EXPECT_TRUE(box2.start_rewrite(20));
    EXPECT_TRUE(box2.write(rewrite4, 4));
    EXPECT_TRUE(box2.end_rewrite());
    EXPECT_TRUE(box2.close());

    // Box 3: super-box with sub-box reopen + write_free_and_close.
    jp2_output_box sup;
    sup.open(&ftgt, 0x74737570 /*'tsup'*/);
    jp2_output_box sub1;
    sub1.open(&sup, 0x73756231 /*'sub1'*/);
    kdu_byte sdata[16] = {};
    EXPECT_TRUE(sub1.write(sdata, 16));
    EXPECT_TRUE(sub1.close());
    EXPECT_EQ(sub1.reopen(0x73756252 /*'subR'*/, 4), 12);
    EXPECT_TRUE(sub1.write(patch4, 4));
    EXPECT_TRUE(sub1.close());

    jp2_output_box sub2;
    sub2.open(&sup, 0x73756232 /*'sub2'*/);
    sub2.set_target_size(24);
    EXPECT_TRUE(sub2.write(sdata, 8));
    EXPECT_TRUE(sub2.write_free_and_close(16));

    // Closing super-box with reopen error check on invalid fixed size / <8
    // free.
    EXPECT_TRUE(sup.close());

    // Box 4: write_free_and_close(0) and invalid free_bytes error paths.
    jp2_output_box free0;
    free0.open(&ftgt, 0x66723030 /*'fr00'*/);
    EXPECT_TRUE(free0.write(sdata, 8));
    EXPECT_TRUE(free0.write_free_and_close(0));

    jp2_output_box bad_free;
    bad_free.open(&ftgt, 0x62616466 /*'badf'*/);
    bad_free.write(sdata, 4);
    EXPECT_TRUE(ExpectKduThrows([&]() { bad_free.write_free_and_close(4); }));
    bad_free.set_target_size(20);
    EXPECT_TRUE(ExpectKduThrows([&]() { bad_free.write_free_and_close(10); }));
    EXPECT_TRUE(bad_free.write_free_and_close(16));

    // Box 5: rubber-length box at the end.
    jp2_output_box rub;
    rub.open(&ftgt, 0x72756262 /*'rubb'*/);
    rub.set_rubber_length();
    EXPECT_TRUE(rub.write(payload1, 10));
    EXPECT_TRUE(rub.close());

    EXPECT_GT(ftgt.get_bytes_written(), 400);
    ftgt.close();
    EXPECT_FALSE(ftgt.exists());
  }

  // 2. Simulation mode on jp2_family_tgt (simulated_start_pos = 100).
  {
    jp2_family_tgt sim_tgt;
    sim_tgt.open(static_cast<kdu_core::kdu_long>(100));
    jp2_output_box sbox;
    sbox.open(&sim_tgt, 0x73696D31 /*'sim1'*/);
    sbox.set_target_size(16);
    kdu_byte buf[16] = {1, 2, 3, 4};
    EXPECT_TRUE(sbox.write(buf, 8));
    EXPECT_TRUE(sbox.start_rewrite(4));
    EXPECT_TRUE(sbox.write(buf, 4));
    EXPECT_TRUE(sbox.end_rewrite());
    EXPECT_TRUE(sbox.write(buf + 8, 8));
    EXPECT_TRUE(sbox.close());
    EXPECT_EQ(sbox.reopen(0x73696D32 /*'sim2'*/, 2), 14);
    EXPECT_TRUE(sbox.write(buf, 4));
    EXPECT_TRUE(sbox.close());
    EXPECT_EQ(sim_tgt.get_bytes_written(), 124);
    sim_tgt.close();
  }

  // 3. Indirect MemTarget write_header_last, reopen, start_rewrite, rubber,
  // and write-failure propagation.
  {
    MemTarget mtgt;
    jp2_family_tgt itgt;
    itgt.open(&mtgt);

    jp2_output_box ibox;
    ibox.open(&itgt, 0x696E6431 /*'ind1'*/);
    ibox.write_header_last();
    std::vector<kdu_byte> data(300, 0x3C);
    EXPECT_TRUE(ibox.write(data.data(), static_cast<int>(data.size())));
    EXPECT_TRUE(ibox.start_rewrite(16));
    kdu_byte mark[4] = {0x7E, 0x7F, 0x80, 0x81};
    EXPECT_TRUE(ibox.write(mark, 4));
    EXPECT_TRUE(ibox.end_rewrite());
    EXPECT_TRUE(ibox.close());
    EXPECT_EQ(ibox.reopen(0x696E6432 /*'ind2'*/, 8), 292);
    EXPECT_TRUE(ibox.write(mark, 4));
    EXPECT_TRUE(ibox.close());

    jp2_output_box fail_box;
    fail_box.open(&itgt, 0x6661696C /*'fail'*/);
    mtgt.set_fail_writes(true);
    fail_box.write(mark, 4);
    EXPECT_FALSE(fail_box.close());
    mtgt.set_fail_writes(false);
    itgt.close();
  }

  // 4. Read back FILE* in non-seekable and seekable modes, plus close(true).
  {
    jp2_family_src fsrc_seq;
    fsrc_seq.open(tmp_path.c_str(), false);
    jp2_input_box ibox;
    ASSERT_TRUE(ibox.open(&fsrc_seq));
    EXPECT_EQ(ibox.get_box_type(), 0x74657341U /*'tesA'*/);
    kdu_byte rbuf[16] = {};
    EXPECT_EQ(ibox.read(rbuf, 4), 4);
    EXPECT_FALSE(ibox.seek(8));
    EXPECT_EQ(ibox.read(rbuf + 4, 8), 8);
    ibox.close();
    EXPECT_TRUE(ibox.open_next());
    EXPECT_EQ(ibox.get_box_type(), 0x74657332U /*'tes2'*/);
    ibox.close();
    fsrc_seq.close();

    jp2_family_src fsrc_seek;
    fsrc_seek.open(tmp_path.c_str(), true);
    jp2_input_box box1;
    ASSERT_TRUE(box1.open(&fsrc_seek));
    box1.close();
    ASSERT_TRUE(box1.open_next());
    box1.close();
    ASSERT_TRUE(box1.open_next());  // 'tsup'
    EXPECT_EQ(box1.get_box_type(), 0x74737570U);
    jp2_input_box forked;
    forked.fork(box1);
    jp2_input_box sub;
    ASSERT_TRUE(sub.open(&forked));
    EXPECT_EQ(sub.get_box_type(), 0x73756252U /*'subR'*/);
    EXPECT_TRUE(sub.seek(-1));
    EXPECT_EQ(sub.get_pos(), 0);
    EXPECT_TRUE(sub.seek(1000));
    EXPECT_EQ(sub.get_pos(), 16);
    sub.load_in_memory(1024);
    EXPECT_TRUE(sub.seek(4));
    kdu_byte sread[4] = {};
    EXPECT_EQ(sub.read(sread, 4), 4);
    EXPECT_EQ(sread[0], 0xAA);
    sub.close();
    forked.close();
    box1.close();
    // Test partial close followed by full close.
    fsrc_seek.close(true);
    fsrc_seek.close(false);
  }
  std::remove(tmp_path.c_str());

  // 5. Non-seekable ChunkedMemSource partial read + forward seek discard loop
  // + malformed box headers and rubber subbox boundaries.
  {
    std::vector<kdu_byte> stream;
    AppendBox(stream, 0x626F7831 /*'box1'*/, std::vector<kdu_byte>(40, 0x1A));
    std::vector<kdu_byte> sub1;
    AppendBox(sub1, 0x73756231, std::vector<kdu_byte>(10, 0x2B));
    // Append a rubber subbox (LBox=0) inside superbox.
    AppendBe32(sub1, 0);
    AppendBe32(sub1, 0x72756232);
    sub1.insert(sub1.end(), 6, 0x3C);
    AppendBox(stream, 0x73757072 /*'supr'*/, sub1);

    MemSource csrc(stream, false);
    jp2_family_src fsrc;
    fsrc.open(&csrc);
    jp2_input_box b1;
    ASSERT_TRUE(b1.open(&fsrc));
    EXPECT_FALSE(b1.seek(20));
    kdu_byte tmp[12] = {};
    EXPECT_EQ(b1.read(tmp, 12), 12);
    EXPECT_EQ(tmp[0], 0x1A);
    b1.close();
    ASSERT_TRUE(b1.open_next());  // 'supr' discards remaining 28 bytes of box1
    jp2_input_box sub;
    ASSERT_TRUE(sub.open(&b1));
    sub.close();
    ASSERT_TRUE(sub.open_next());  // rubber subbox
    EXPECT_EQ(sub.get_box_type(), 0x72756232U);
    EXPECT_EQ(sub.read(tmp, 6), 6);
    // Once rubber subbox is reached, open_next() returns false.
    sub.close();
    EXPECT_FALSE(sub.open_next());
    b1.close();
    fsrc.close();

    // Separate ChunkedMemSource partial read (< requested bytes).
    ChunkedMemSource chunked(stream, true, 16);
    jp2_family_src fchunk;
    fchunk.open(&chunked);
    jp2_input_box bc;
    ASSERT_TRUE(bc.open(&fchunk));
    kdu_byte bigbuf[32] = {};
    EXPECT_EQ(bc.read(bigbuf, 32), 16);
    bc.close();
    fchunk.close();
  }
  {
    // 1) LBox=4 invalid, 2) LBox=1 with XLBox=10 (<16) throw; truncated XLBox
    // returns false.
    for (const std::vector<kdu_byte>& bad : {
             std::vector<kdu_byte>{0, 0, 0, 4, 'a', 'b', 'c', 'd'},
             std::vector<kdu_byte>{0, 0, 0, 1, 'a', 'b', 'c', 'd', 0, 0, 0, 0,
                                   0, 0, 0, 10},
         }) {
      MemSource bsrc(bad);
      jp2_family_src fsrc;
      fsrc.open(&bsrc);
      jp2_input_box box;
      EXPECT_TRUE(ExpectKduThrows([&]() { box.open(&fsrc); }));
      box.close();
      fsrc.close();
    }
    {
      MemSource bsrc(
          std::vector<kdu_byte>{0, 0, 0, 1, 'a', 'b', 'c', 'd', 0, 0});
      jp2_family_src fsrc;
      fsrc.open(&bsrc);
      jp2_input_box box;
      EXPECT_FALSE(box.open(&fsrc));
      box.close();
      fsrc.close();
    }
  }

  // 6. Custom kdu_membroker coverage on jp2_family_tgt, jp2_family_src, and
  // j2_memsafe alloc/overlimit/detach paths.
  {
    TrackingMemBroker small_ok(4096);
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt, &small_ok);
    EXPECT_EQ(ftgt.get_membroker(), &small_ok);
    // Small box (<256 bytes -> 1-byte alloc header) + larger box (>256 -> 4B).
    jp2_output_box bsmall;
    bsmall.open(&ftgt, 0x736D616C /*'smal'*/);
    std::vector<kdu_byte> dsmall(32, 0x01);
    bsmall.write(dsmall.data(), static_cast<int>(dsmall.size()));
    bsmall.close();
    jp2_output_box bmed;
    bmed.open(&ftgt, 0x6D656420 /*'med '*/);
    std::vector<kdu_byte> dmed(300, 0x02);
    bmed.write(dmed.data(), static_cast<int>(dmed.size()));
    bmed.close();
    ftgt.close();
    EXPECT_EQ(small_ok.failures(), 0);

    // Exhausted broker on jp2_family_tgt::alloc.
    TrackingMemBroker tight_tgt(48);
    jp2_family_tgt ftgt_fail;
    ftgt_fail.open(&mtgt, &tight_tgt);
    jp2_output_box bfail;
    bfail.open(&ftgt_fail, 0x6F766572 /*'over'*/);
    std::vector<kdu_byte> dbig(200, 0x03);
    EXPECT_TRUE(ExpectKduThrows(
        [&]() { bfail.write(dbig.data(), static_cast<int>(dbig.size())); }));
    bfail.close();
    ftgt_fail.close();
    EXPECT_GT(tight_tgt.failures(), 0);

    // Exhausted broker on jp2_family_src::load_in_memory (returns false) and
    // jp2_target / j2_memsafe overlimit (throws + records broker failure).
    TrackingMemBroker tight_src(16);
    MemSource msrc(mtgt.data());
    jp2_family_src fsrc_fail;
    fsrc_fail.open(&msrc, &tight_src);
    EXPECT_EQ(fsrc_fail.get_membroker(), &tight_src);
    jp2_input_box bin;
    ASSERT_TRUE(bin.open(&fsrc_fail));
    EXPECT_FALSE(bin.load_in_memory(1024));
    bin.close();
    fsrc_fail.close();

    TrackingMemBroker tight_memsafe(8);
    MemTarget mtgt_ms;
    jp2_family_tgt ftgt_ms;
    ftgt_ms.open(&mtgt_ms, &tight_memsafe);
    EXPECT_TRUE(ExpectKduThrows([&]() {
      jp2_target jpt;
      jpt.open(&ftgt_ms);
      jpt.access_palette().init(4, 16);
    }));
    ftgt_ms.close();
    EXPECT_GT(tight_memsafe.failures(), 0);
  }
}

TEST(Jp2Test, IccEdgeCurvesLabD50PermissionsDrefAndHeaderErrors) {
  ScopedMessageHandlers handlers;

  // 1. Build a multi-layer JPX with:
  //    - Layer 0: RGB ICC with 'para' func_type 4 curves
  //    - Layer 1: CIE Lab with non-default D50 illuminant
  //    - Layer 2: Monochrome ICC with 'para' func_type 4 curve
  //    - Layer 3: copied Lab & colour/palette/channels/dimensions/resolution
  //      overrides to hit j2_colour::copy, compare, res, bpcc, and ftyp wrap.
  std::vector<kdu_byte> jpx_bytes;
  std::vector<kdu_byte> rgb_icc_para4 = BuildIccProfile(
      0x52474220 /*'RGB '*/, 0x58595A20 /*'XYZ '*/,
      {
          MakeXyzTag(0x77747074 /*'wtpt'*/, 0.9642, 1.0, 0.8249),
          MakeXyzTag(0x7258595A /*'rXYZ'*/, 0.4360, 0.2225, 0.0139),
          MakeXyzTag(0x6758595A /*'gXYZ'*/, 0.3851, 0.7169, 0.0971),
          MakeXyzTag(0x6258595A /*'bXYZ'*/, 0.1431, 0.0606, 0.7141),
          MakeParaCurveTag(0x72545243 /*'rTRC'*/, 4),
          MakeParaCurveTag(0x67545243 /*'gTRC'*/, 4),
          MakeParaCurveTag(0x62545243 /*'bTRC'*/, 4),
      });
  std::vector<kdu_byte> gray_icc_para4 = BuildIccProfile(
      0x47524159 /*'GRAY'*/, 0x58595A20 /*'XYZ '*/,
      {
          MakeXyzTag(0x77747074 /*'wtpt'*/, 0.9642, 1.0, 0.8249),
          MakeParaCurveTag(0x6B545243 /*'kTRC'*/, 4),
          MakeParaCurveTag(0x67545243 /*'gTRC'*/, 4),
          MakeParaCurveTag(0x62545243 /*'bTRC'*/, 4),
      });

  const int d8x3[3] = {8, 8, 8};
  const int d8x1[1] = {8};
  const int d8x4[4] = {8, 8, 8, 8};

  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    jpx_target jt;
    jt.open(&ftgt);

    // Data references edge paths (Windows backslash, UNC, relative path).
    jp2_data_references dref = jt.access_data_references();
    EXPECT_GT(dref.add_file_url("C:\\images\\tile.jp2"), 0);
    EXPECT_GT(dref.add_file_url("\\\\fileserver\\share\\tile2.jp2"), 0);
    EXPECT_GT(dref.add_file_url("nested/tile3.jp2"), 0);
    EXPECT_EQ(dref.add_url(nullptr), 0);
    EXPECT_STREQ(dref.get_file_url(1), "C:/images/tile.jp2");
    EXPECT_EQ(dref.add_url("http://example.com/override.jp2", 1), 1);
    EXPECT_TRUE(ExpectKduThrows([&]() { dref.add_url("http://max", 65536); }));

    std::vector<kdu_supp::jpx_codestream_target> cs_tgts;
    auto add_layer_with_cs =
        [&](int num_comps, const int* depths) -> kdu_supp::jpx_layer_target {
      kdu_supp::jpx_codestream_target ct = jt.add_codestream();
      jp2_dimensions dims = ct.access_dimensions();
      dims.init(kdu_coords(8, 8), num_comps);
      for (int c = 0; c < num_comps; ++c) {
        dims.set_precision(c, depths[c], false);
      }
      cs_tgts.push_back(ct);
      return jt.add_layer();
    };

    // Layer 0: RGB ICC para4 + capture/display resolution.
    auto lyr0 = add_layer_with_cs(3, d8x3);
    jp2_colour col0_tgt = lyr0.add_colour();
    col0_tgt.init(rgb_icc_para4.data());
    jp2_channels ch0 = lyr0.access_channels();
    ch0.init(3);
    ch0.set_colour_mapping(0, 0, -1, 0);
    ch0.set_colour_mapping(1, 1, -1, 0);
    ch0.set_colour_mapping(2, 2, -1, 0);
    lyr0.access_resolution().init(1.25f);
    lyr0.access_resolution().set_resolution(300.0f, true);
    lyr0.access_resolution().set_resolution(150.0f, false);

    // Layer 1: CIE Lab D50.
    auto lyr1 = add_layer_with_cs(3, d8x3);
    jp2_colour col1_tgt = lyr1.add_colour();
    col1_tgt.init(jp2_colour_space::JP2_CIELab_SPACE, 100, 0, 8, 255, 128, 8,
                  255, 128, 8, JP2_CIE_D50, 5000);
    jp2_channels ch1 = lyr1.access_channels();
    ch1.init(3);
    ch1.set_colour_mapping(0, 0, -1, 1);
    ch1.set_colour_mapping(1, 1, -1, 1);
    ch1.set_colour_mapping(2, 2, -1, 1);

    // Layer 2: Monochrome ICC para4.
    auto lyr2 = add_layer_with_cs(1, d8x1);
    lyr2.add_colour().init(gray_icc_para4.data());
    jp2_channels ch2 = lyr2.access_channels();
    ch2.init(1);
    ch2.set_colour_mapping(0, 0, -1, 2);

    // Layer 3: Copy colour across Lab -> ICC -> vendor, plus non-colour
    // mapping.
    auto lyr3 = add_layer_with_cs(4, d8x4);
    jp2_colour col3 = lyr3.add_colour();
    col3.copy(col1_tgt);
    jp2_colour col3b = lyr3.add_colour(-1, 2);
    col3b.copy(col0_tgt);
    col3b.copy(col1_tgt);  // Overwrite ICC with Lab
    kdu_byte v_uuid[16] = {1, 2,  3,  4,  5,  6,  7,  8,
                           9, 10, 11, 12, 13, 14, 15, 16};
    kdu_byte v_data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    jp2_colour col3c = lyr3.add_colour(-1, 3);
    col3c.init(v_uuid, 4, v_data);
    col3b.copy(col3c);     // Overwrite Lab with vendor
    col3b.copy(col0_tgt);  // Overwrite vendor with ICC

    jp2_channels ch3 = lyr3.access_channels();
    ch3.init(3, 1);
    ch3.set_colour_mapping(0, 0, -1, 3);
    ch3.set_colour_mapping(1, 1, -1, 3);
    ch3.set_colour_mapping(2, 2, -1, 3);
    ch3.set_non_colour_mapping(0, 0xCAFE, 3, -1, 3);

    // Write minimal codestreams to finalize JPX boxes cleanly.
    jt.write_headers();
    for (int i = 0; i < 4; ++i) {
      int nc = (i == 2) ? 1 : (i == 3 ? 4 : 3);
      auto* cs_stream = cs_tgts[i].open_stream();
      CompressMinimalCodestream(*cs_stream, 8, 8, nc);
      cs_stream->close();
    }
    jt.close();
    ftgt.close();
    jpx_bytes = mtgt.data();
  }

  // 2. Read back JPX layers: textualize real + synthetic ftyp(6 brands)/bpcc,
  // check resolution, ICC profile bytes, non-colour mapping, CIE default=false
  // on D50 Lab, and colour conversions with out-of-range fixed16 & float data.
  {
    std::vector<kdu_byte> extra_ftyp_stream = jpx_bytes;
    std::vector<kdu_byte> ftyp6;
    AppendBe32(ftyp6, jp2_brand);
    AppendBe32(ftyp6, 0);
    AppendBe32(ftyp6, jp2_brand);
    AppendBe32(ftyp6, jpx_brand);
    AppendBe32(ftyp6, 0x62726E31);
    AppendBe32(ftyp6, 0x62726E32);
    AppendBe32(ftyp6, 0x62726E33);
    AppendBe32(ftyp6, 0x62726E34);
    AppendBox(extra_ftyp_stream, jp2_file_type_4cc, ftyp6);
    AppendBox(extra_ftyp_stream, 0x62706363 /*'bpcc'*/, {7, 0x89, 11});

    MemSource msrc_tex(extra_ftyp_stream);
    jp2_family_src fsrc_tex;
    fsrc_tex.open(&msrc_tex);
    StringMessage dump;
    jp2_box_textualizer tex;
    jp2_add_box_descriptions(tex);
    jpx_add_box_descriptions(tex);
    jp2_input_box box;
    if (box.open(&fsrc_tex)) {
      do {
        tex.textualize_box(&box, dump, false, 4096);
        box.close();
      } while (box.open_next());
    }
    fsrc_tex.close();
    EXPECT_FALSE(dump.text().empty());
  }

  {
    kdu_core::kdu_sample_allocator alloc;
    kdu_core::kdu_line_buf lum_f, r_f, g_f, b_f;
    kdu_core::kdu_line_buf lum_i, r_i, g_i, b_i;
    lum_f.pre_create(&alloc, 4, 0, 0, 0);
    r_f.pre_create(&alloc, 4, 0, 0, 0);
    g_f.pre_create(&alloc, 4, 0, 0, 0);
    b_f.pre_create(&alloc, 4, 0, 0, 0);
    lum_i.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
    r_i.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
    g_i.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
    b_i.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
    alloc.finalize(kdu_core::kdu_codestream());
    lum_f.create();
    r_f.create();
    g_f.create();
    b_f.create();
    lum_i.create();
    r_i.create();
    g_i.create();
    b_i.create();

    auto fill_extreme = [&]() {
      const float fvals[4] = {-0.85F, 0.0F, 0.45F, 0.85F};
      for (int i = 0; i < 4; ++i) {
        lum_f.get_buf32()[i].fval = fvals[i];
        r_f.get_buf32()[i].fval = fvals[i];
        g_f.get_buf32()[i].fval = fvals[(i + 1) % 4];
        b_f.get_buf32()[i].fval = fvals[(i + 2) % 4];
        const kdu_int16 iv =
            static_cast<kdu_int16>(fvals[i] * (1 << KDU_FIX_POINT));
        lum_i.get_buf16()[i].ival = iv;
        r_i.get_buf16()[i].ival = iv;
        g_i.get_buf16()[i].ival =
            static_cast<kdu_int16>(fvals[(i + 1) % 4] * (1 << KDU_FIX_POINT));
        b_i.get_buf16()[i].ival =
            static_cast<kdu_int16>(fvals[(i + 2) % 4] * (1 << KDU_FIX_POINT));
      }
    };

    MemSource msrc(jpx_bytes);
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    jpx_source jsrc;
    ASSERT_TRUE(jsrc.open(&fsrc, false));

    // Layer 0: RGB ICC para4 + capture/display resolution.
    auto ls0 = jsrc.access_layer(0);
    jp2_resolution res0 = ls0.access_resolution();
    EXPECT_TRUE(res0.exists());
    EXPECT_NEAR(res0.get_resolution(true), 300.0f, 1.0f);
    EXPECT_NEAR(res0.get_resolution(false), 150.0f, 1.0f);

    jp2_colour col0 = ls0.access_colour(0);
    int icc_bytes = 0;
    const kdu_byte* icc_ptr = col0.get_icc_profile(&icc_bytes);
    ASSERT_NE(icc_ptr, nullptr);
    EXPECT_EQ(icc_bytes, static_cast<int>(rgb_icc_para4.size()));

    jp2_colour_converter conv_rgb;
    ASSERT_TRUE(conv_rgb.init(col0));
    fill_extreme();
    EXPECT_TRUE(conv_rgb.convert_rgb(r_i, g_i, b_i, 4));
    EXPECT_TRUE(conv_rgb.convert_rgb(r_f, g_f, b_f, 4));

    // Layer 1: Lab D50 -> check_cie_default() == false + convert_rgb.
    auto ls1 = jsrc.access_layer(1);
    jp2_colour col1 = ls1.access_colour(0);
    EXPECT_FALSE(col1.check_cie_default());
    jp2_colour_converter conv_lab;
    ASSERT_TRUE(conv_lab.init(col1, true, false));
    fill_extreme();
    EXPECT_TRUE(conv_lab.convert_rgb(r_i, g_i, b_i, 4));
    EXPECT_TRUE(conv_lab.convert_rgb(r_f, g_f, b_f, 4));

    // Layer 2: Monochrome ICC para4 -> out-of-range fixed16 & float
    // convert_lum.
    auto ls2 = jsrc.access_layer(2);
    jp2_colour_converter conv_lum;
    ASSERT_TRUE(conv_lum.init(ls2.access_colour(0)));
    fill_extreme();
    EXPECT_TRUE(conv_lum.convert_lum(lum_i, 4));
    EXPECT_TRUE(conv_lum.convert_lum(lum_f, 4));

    // Layer 3: non-colour mapping verification.
    auto ls3 = jsrc.access_layer(3);
    jp2_channels ch3 = ls3.access_channels();
    kdu_core::kdu_uint16 asoc_key = 0;
    int comp_idx = -1, lut_idx = -1, cs_id = -1, data_fmt = -1;
    EXPECT_TRUE(ch3.get_non_colour_mapping(0, asoc_key, comp_idx, lut_idx,
                                           cs_id, data_fmt));
    EXPECT_EQ(asoc_key, 0xCAFE);
    EXPECT_EQ(comp_idx, 3);

    jsrc.close();
    fsrc.close();
  }

  // 3. Invalid ICC curve / tag profiles -> exercises ICC error branches.
  auto test_bad_icc = [&](std::vector<kdu_byte> prof) {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    jpx_target jt;
    jt.open(&ftgt);
    kdu_supp::jpx_codestream_target ct = jt.add_codestream();
    ct.access_dimensions().init(kdu_coords(8, 8), 1);
    ct.access_dimensions().set_precision(0, 8, false);
    auto lyr = jt.add_layer();
    lyr.access_channels().init(1);
    lyr.access_channels().set_colour_mapping(0, 0, -1, 0);
    EXPECT_TRUE(ExpectKduThrows([&]() {
      jp2_colour col = lyr.add_colour();
      col.init(prof.data());
      jp2_colour_converter conv;
      conv.init(col);
    }));
    jt.close();
    ftgt.close();
  };
  {
    // Unrecognized 'para' function type 5 (warns then fails length check).
    std::vector<kdu_byte> bad_para_data(12, 0);
    WriteBe32At(bad_para_data, 0, 0x70617261);  // 'para'
    WriteBe16At(bad_para_data, 8, 5);
    test_bad_icc(BuildIccProfile(0x47524159, 0x58595A20,
                                 {MakeXyzTag(0x77747074, 0.9642, 1.0, 0.8249),
                                  {0x6B545243, bad_para_data}}));

    // Truncated 'para' (< 12 bytes) and truncated 'para' func_type 4.
    std::vector<kdu_byte> short_para(8, 0);
    WriteBe32At(short_para, 0, 0x70617261);
    test_bad_icc(BuildIccProfile(0x47524159, 0x58595A20,
                                 {MakeXyzTag(0x77747074, 0.9642, 1.0, 0.8249),
                                  {0x6B545243, short_para}}));

    std::vector<kdu_byte> trunc_para4(14, 0);
    WriteBe32At(trunc_para4, 0, 0x70617261);
    WriteBe16At(trunc_para4, 8, 4);
    test_bad_icc(BuildIccProfile(0x47524159, 0x58595A20,
                                 {MakeXyzTag(0x77747074, 0.9642, 1.0, 0.8249),
                                  {0x6B545243, trunc_para4}}));

    // Truncated 'curv' (< 12 bytes).
    std::vector<kdu_byte> short_curv(8, 0);
    WriteBe32At(short_curv, 0, 0x63757276);
    test_bad_icc(BuildIccProfile(0x47524159, 0x58595A20,
                                 {MakeXyzTag(0x77747074, 0.9642, 1.0, 0.8249),
                                  {0x6B545243, short_curv}}));

    // Wrong curve tag signature 'badt'.
    std::vector<kdu_byte> wrong_type(16, 0);
    WriteBe32At(wrong_type, 0, 0x62616474);
    test_bad_icc(BuildIccProfile(0x47524159, 0x58595A20,
                                 {MakeXyzTag(0x77747074, 0.9642, 1.0, 0.8249),
                                  {0x6B545243, wrong_type}}));
  }

  // 4. jp2_dref_permissions wildcard & syntax parsing + jp2_family_src API.
  {
    jp2_dref_permissions perms;
    perms.init("/tmp/container.jpx",
               "/*/{foo,bar}/**.jp2:"
               "./**.jp2:"
               "/dir\\*/img.jp2");
    EXPECT_TRUE(perms.can_access("/root/foo/a/b/c.jp2"));
    EXPECT_TRUE(perms.can_access("/root/bar/x.jp2"));
    EXPECT_FALSE(perms.can_access("/root/baz/x.jp2"));
    EXPECT_TRUE(perms.can_access("./sub/tile.jp2"));
    EXPECT_TRUE(perms.can_access("/dir*/img.jp2"));
    EXPECT_FALSE(perms.can_access(nullptr));

    char apath[256] = {};
    EXPECT_NE(perms.get_access_path(apath, sizeof(apath), "./sub/tile.jp2"),
              nullptr);

    // Syntax errors in pattern -> can_access returns false.
    perms.init("/tmp/container.jpx", "/{a,b:/bad\\");
    EXPECT_FALSE(perms.can_access("/a"));

    MemSource msrc(jpx_bytes);
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    fsrc.set_access_permissions("/**.jp2");
    ASSERT_NE(fsrc.get_dref_permissions(), nullptr);
    EXPECT_TRUE(fsrc.get_dref_permissions()->can_access("/any/path.jp2"));
    fsrc.close();
  }

  // 5. jp2_source::read_header(false) errors on invalid/incomplete JP2 headers.
  {
    auto try_read_header = [&](const std::vector<kdu_byte>& raw) {
      MemSource msrc(raw);
      jp2_family_src fsrc;
      fsrc.open(&msrc);
      jp2_source jps;
      EXPECT_TRUE(ExpectKduThrows([&]() {
        jps.open(&fsrc);
        jps.read_header(false);
      }));
      jps.close();
      fsrc.close();
    };
    // Empty / truncated signature box.
    try_read_header(
        std::vector<kdu_byte>{0, 0, 0, 12, 'j', 'P', ' ', ' ', 0, 0, 0, 0});
    // Valid signature but missing ftyp.
    std::vector<kdu_byte> no_ftyp;
    AppendBox(no_ftyp, 0x6A502020, {0x0D, 0x0A, 0x87, 0x0A});
    AppendBox(no_ftyp, 0x6A703268, {});
    try_read_header(no_ftyp);
    // Valid sig + ftyp, missing jp2h.
    std::vector<kdu_byte> no_jp2h;
    AppendBox(no_jp2h, 0x6A502020, {0x0D, 0x0A, 0x87, 0x0A});
    std::vector<kdu_byte> ftyp_p;
    AppendBe32(ftyp_p, 0x6A703220);
    AppendBe32(ftyp_p, 0);
    AppendBe32(ftyp_p, 0x6A703220);
    AppendBox(no_jp2h, 0x66747970, ftyp_p);
    try_read_header(no_jp2h);
  }
}

TEST(Jp2Test, DeepEdgeCoverageSplitExpChromaKeySimAndErrors) {
  ScopedMessageHandlers handlers;

  // 1. jpx_target / jpx_source with SPLIT_EXP, FLOAT, FIXPOINT pixel formats,
  // chroma key opacity, channel copy, and same-space j2_colour::compare paths.
  {
    MemTarget mt_cs0, mt_cs1, mt_cs2;
    CompressMinimalCodestream(mt_cs0, 4, 4, 6, 8);
    CompressMinimalCodestream(mt_cs1, 4, 4, 2, 16);
    CompressMinimalCodestream(mt_cs2, 4, 4, 3, 8);
    const std::vector<kdu_byte>& cs0 = mt_cs0.data();
    const std::vector<kdu_byte>& cs1 = mt_cs1.data();
    const std::vector<kdu_byte>& cs2 = mt_cs2.data();

    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    jpx_target jt;
    jt.open(&ftgt);

    kdu_supp::jpx_codestream_target jcs_list[3];
    for (int cs_idx = 0; cs_idx < 3; ++cs_idx) {
      jcs_list[cs_idx] = jt.add_codestream();
      jp2_dimensions dims = jcs_list[cs_idx].access_dimensions();
      if (cs_idx == 0) {
        dims.init(kdu_coords(4, 4), 6, false);
        for (int c = 0; c < 6; ++c) {
          dims.set_precision(c, 8, false);
        }
        dims.finalize_compatibility(Sprofile_PART1, 0, 0, 0, 0, 0);
      } else if (cs_idx == 1) {
        dims.init(kdu_coords(4, 4), 2, true);
        for (int c = 0; c < 2; ++c) {
          dims.set_precision(c, 16, true);
        }
        // Exercise 6-arg finalize_compatibility with and without Scap_P15.
        dims.finalize_compatibility(Sprofile_PART2, Scap_P15, Sextensions_MCT,
                                    JP2_MCT_MULTI_STAGE | JP2_MCT_REV_MATRIX,
                                    0x1234, 0x0007);
        dims.finalize_compatibility(Sprofile_PART2, 0, 0, 0, 0x0001, 0x00FF);
      } else {
        dims.init(kdu_coords(4, 4), 3, false);
        for (int c = 0; c < 3; ++c) {
          dims.set_precision(c, 8, false);
        }
      }
      jcs_list[cs_idx].access_palette();
    }

    // Also test siz_params-based finalize_compatibility with PART2, SCP2_caps,
    // multi-stage MCT (MATRIX reversible, DEP, DWT), and multi-block MCT.
    {
      auto run_mct_compat = [&](const std::vector<const char*>& params) {
        siz_params siz;
        siz.set(Sdims, 0, 0, 4);
        siz.set(Sdims, 0, 1, 4);
        siz.set(Scomponents, 0, 0, 3);
        for (int c = 0; c < 3; ++c) {
          siz.set(Sprecision, c, 0, 8);
          siz.set(Ssigned, c, 0, false);
        }
        EXPECT_TRUE(siz.parse_string("Sprofile=PART2"));
        siz.set(SCP2_caps, 0, 0, SCP2_caps_EXTENDED_COD);
        EXPECT_TRUE(siz.parse_string("Sextensions=MCT"));
        EXPECT_TRUE(siz.parse_string("Mcomponents=3"));
        EXPECT_TRUE(siz.parse_string("Mprecision=8"));
        EXPECT_TRUE(siz.parse_string("Msigned=no"));
        static_cast<kdu_core::kdu_params&>(siz).finalize();
        MemTarget cs_tmp;
        kdu_core::kdu_codestream cs;
        cs.create(&siz, &cs_tmp);
        kdu_core::siz_params* root = cs.access_siz();
        for (const char* p : params) {
          EXPECT_TRUE(root->parse_string(p));
        }
        root->finalize_all();
        // Verify root-based finalize_compatibility on a standalone jp2_target
        // so cs0 keeps its 6-component metadata clean for layers below.
        {
          MemTarget mcompat;
          jp2_family_tgt fcompat;
          fcompat.open(&mcompat);
          jp2_target tcompat;
          tcompat.open(&fcompat);
          tcompat.access_dimensions().init(kdu_coords(4, 4), 3, false);
          for (int c = 0; c < 3; ++c) {
            tcompat.access_dimensions().set_precision(c, 8, false);
          }
          tcompat.access_dimensions().finalize_compatibility(root);
          tcompat.close();
          fcompat.close();
        }
        cs.destroy();
      };
      // DEP + MATRIX multi-stage (stages 1, 1) and multi-block.
      run_mct_compat({
          "Mmatrix_size:I1=4",
          "Mmatrix_coeffs:I1=1,0,0,1",
          "Mmatrix_size:I2=1",
          "Mmatrix_coeffs:I2=1",
          "Mstage_inputs:I1={0,1},{2,2}",
          "Mstage_outputs:I1={0,1},{2,2}",
          "Mstage_collections:I1={2,2},{1,1}",
          "Mstage_xforms:I1={MATRIX,0,1,1,0},{MATRIX,0,2,0,0}",
          "Mnum_stages=2",
          "Mstages=1,1",
      });
      // DEP stage.
      run_mct_compat({
          "Mtriang_size:I1=3",
          "Mtriang_coeffs:I1=0.25,-0.25,0.5",
          "Mvector_size:I1=3",
          "Mvector_coeffs:I1=3,-2,1",
          "Mstage_inputs:I1={0,2}",
          "Mstage_outputs:I1={0,2}",
          "Mstage_collections:I1={3,3}",
          "Mstage_xforms:I1={DEP,1,1,0,0}",
          "Mnum_stages=1",
          "Mstages=1",
      });
      // DWT stage.
      run_mct_compat({
          "Mvector_size:I1=3",
          "Mvector_coeffs:I1=3,-2,1",
          "Mstage_inputs:I1={0,2}",
          "Mstage_outputs:I1={0,2}",
          "Mstage_collections:I1={3,3}",
          "Mstage_xforms:I1={DWT,1,1,1,1}",
          "Mnum_stages=1",
          "Mstages=1",
      });
    }

    // Standalone jp2_channels setter error checks (avoid tainting jpx layers).
    {
      MemTarget tmp_mtgt;
      jp2_family_tgt tmp_ftgt;
      tmp_ftgt.open(&tmp_mtgt);
      jp2_target tmp_jp2;
      tmp_jp2.open(&tmp_ftgt);
      jp2_channels err_ch = tmp_jp2.access_channels();
      err_ch.init(3, 1);
      EXPECT_FALSE(err_ch.set_colour_mapping(9, 0));
      EXPECT_TRUE(ExpectKduThrows([&]() {
        err_ch.set_colour_mapping(0, 0, -1, 0, JP2_CHANNEL_FORMAT_SPLIT_EXP,
                                  nullptr);
      }));
      int zero_exp[1] = {0};
      EXPECT_TRUE(err_ch.set_colour_mapping(
          0, 0, -1, 0, JP2_CHANNEL_FORMAT_FLOAT, zero_exp));
      int neg_fix[1] = {-1};
      EXPECT_TRUE(err_ch.set_colour_mapping(
          0, 0, -1, 0, JP2_CHANNEL_FORMAT_FIXPOINT, neg_fix));
      EXPECT_FALSE(err_ch.set_chroma_key(9, 1));
      tmp_jp2.close();
      tmp_ftgt.close();
    }

    // Layer 0: RGB ICC with FLOAT, FLOAT, FIXPOINT.
    {
      kdu_supp::jpx_layer_target jl0 = jt.add_layer();
      jp2_channels ch0 = jl0.access_channels();
      ch0.init(3);
      int float_params[1] = {5};
      int fix_params[1] = {4};
      ch0.set_colour_mapping(0, 0, -1, 0, JP2_CHANNEL_FORMAT_FLOAT,
                             float_params);
      ch0.set_colour_mapping(1, 1, -1, 0, JP2_CHANNEL_FORMAT_FLOAT,
                             float_params);
      ch0.set_colour_mapping(2, 2, -1, 0, JP2_CHANNEL_FORMAT_FIXPOINT,
                             fix_params);
      jl0.add_colour().init(
          BuildIccProfile(0x52474220, 0x58595A20,
                          {
                              MakeXyzTag(0x7258595A, 0.436, 0.222, 0.014),
                              MakeXyzTag(0x6758595A, 0.385, 0.717, 0.097),
                              MakeXyzTag(0x6258595A, 0.143, 0.061, 0.714),
                              MakeParaCurveTag(0x72545243, 1),
                              MakeParaCurveTag(0x67545243, 1),
                              MakeParaCurveTag(0x62545243, 1),
                          })
              .data());
      jl0.access_resolution().init(0.5F);
      jl0.access_resolution().set_resolution(100.0F, true);
      jl0.access_resolution().set_resolution(200.0F, false);
    }

    // Layer 1 & Layer 2: same Lab D50 colour (exercises j2_colour::compare)
    // and Layer 1 chroma-key opacity + ch.copy in Layer 2.
    {
      kdu_supp::jpx_layer_target jl1 = jt.add_layer();
      jp2_channels ch1 = jl1.access_channels();
      ch1.init(3);
      ch1.set_colour_mapping(0, 0, -1, 2);
      ch1.set_colour_mapping(1, 1, -1, 2);
      ch1.set_colour_mapping(2, 2, -1, 2);
      ch1.set_chroma_key(0, 11);
      ch1.set_chroma_key(1, 22);
      ch1.set_chroma_key(2, 33);
      EXPECT_FALSE(ch1.set_chroma_key(9, 1));
      jl1.add_colour().init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 8, 255, 128, 8,
                            255, 128, 8, JP2_CIE_D50, 5000);

      kdu_supp::jpx_layer_target jl2 = jt.add_layer();
      int stream_map[3] = {0, 1, 2};
      jl2.access_channels().copy(ch1, stream_map, 3, 0);
      jl2.add_colour().init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 8, 255, 128, 8,
                            255, 128, 8, JP2_CIE_D50, 5000);
    }

    // Layer 3 & Layer 4: identical & differing Lab, vendor UUID colours.
    {
      kdu_byte uuid_a[16] = {1, 2,  3,  4,  5,  6,  7,  8,
                             9, 10, 11, 12, 13, 14, 15, 16};
      kdu_byte uuid_b[16] = {16, 15, 14, 13, 12, 11, 10, 9,
                             8,  7,  6,  5,  4,  3,  2,  1};
      kdu_byte vdata[2] = {0xAA, 0xBB};
      kdu_supp::jpx_layer_target jl3 = jt.add_layer();
      jl3.access_channels().init(3);
      jl3.access_channels().set_colour_mapping(0, 0, -1, 2);
      jl3.access_channels().set_colour_mapping(1, 1, -1, 2);
      jl3.access_channels().set_colour_mapping(2, 2, -1, 2);
      jl3.add_colour().init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 8, 255, 128, 8,
                            255, 128, 8, JP2_CIE_D50, 5000);
      jl3.add_colour().init(uuid_a, 2, vdata);

      kdu_supp::jpx_layer_target jl4 = jt.add_layer();
      jl4.access_channels().init(3);
      jl4.access_channels().set_colour_mapping(0, 0, -1, 2);
      jl4.access_channels().set_colour_mapping(1, 1, -1, 2);
      jl4.access_channels().set_colour_mapping(2, 2, -1, 2);
      jl4.add_colour().init(kdu_supp::JP2_CIELab_SPACE, 120, 0, 8, 255, 128, 8,
                            255, 128, 8, JP2_CIE_D50, 5000);
      jl4.add_colour().init(uuid_b, 2, vdata);
      jl4.add_colour().init(uuid_a, 2, vdata);

      // Layer 5: uses cs1 (16-bit signed, 2 comps).
      kdu_supp::jpx_layer_target jl5 = jt.add_layer();
      jl5.access_channels().init(1);
      jl5.access_channels().set_colour_mapping(0, 0, -1, 1);
      jl5.access_channels().set_opacity_mapping(0, 1, -1, 1);
      jl5.add_colour().init(kdu_supp::JP2_sLUM_SPACE);

      // Layer 6: SPLIT_EXP + opacity + non-colour mapping on cs0.
      kdu_supp::jpx_layer_target jl6 = jt.add_layer();
      int l6_split[3] = {3, -1, 0};
      int l6_float[1] = {5};
      int l6_fix[1] = {4};
      jl6.access_channels().init(1, 1);
      jl6.access_channels().set_colour_mapping(
          0, 0, -1, 0, JP2_CHANNEL_FORMAT_SPLIT_EXP, l6_split);
      jl6.access_channels().set_opacity_mapping(
          0, 4, -1, 0, JP2_CHANNEL_FORMAT_FIXPOINT, l6_fix);
      jl6.access_channels().set_non_colour_mapping(
          0, 0x5678, 5, -1, 0, JP2_CHANNEL_FORMAT_FLOAT, l6_float);
      jl6.add_colour().init(kdu_supp::JP2_sLUM_SPACE);
    }

    jt.write_headers();
    for (int idx = 0; idx < 3; ++idx) {
      jp2_output_box* cs_out = jcs_list[idx].open_stream();
      const auto& cs_bytes = (idx == 0) ? cs0 : ((idx == 1) ? cs1 : cs2);
      cs_out->write(cs_bytes.data(), static_cast<int>(cs_bytes.size()));
      cs_out->close();
    }
    jt.close();
    ftgt.close();

    MemSource msrc(mtgt.data());
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    EXPECT_TRUE(fsrc.is_top_level_complete());
    EXPECT_TRUE(fsrc.is_codestream_main_header_complete(0));
    jpx_source js;
    ASSERT_TRUE(js.open(&fsrc, false));

    {
      kdu_supp::jpx_codestream_source rcs0 = js.access_codestream(0);
      EXPECT_EQ(rcs0.access_dimensions().get_num_components(), 6);
    }
    kdu_supp::jpx_layer_source l0 = js.access_layer(0);
    jp2_channels rch0 = l0.access_channels();
    int cmp = -1, lut = -1, cs = -1, fmt = -1, fp[3] = {0, 0, 0};
    EXPECT_TRUE(rch0.get_colour_mapping(0, cmp, lut, cs, fmt, fp));
    EXPECT_EQ(fmt, JP2_CHANNEL_FORMAT_FLOAT);
    EXPECT_TRUE(rch0.get_colour_mapping(1, cmp, lut, cs, fmt, fp));
    EXPECT_EQ(fmt, JP2_CHANNEL_FORMAT_FLOAT);
    EXPECT_TRUE(rch0.get_colour_mapping(2, cmp, lut, cs, fmt, fp));
    EXPECT_EQ(fmt, JP2_CHANNEL_FORMAT_FIXPOINT);
    kdu_supp::jpx_layer_source l6 = js.access_layer(6);
    jp2_channels rch6 = l6.access_channels();
    EXPECT_TRUE(rch6.get_colour_mapping(0, cmp, lut, cs, fmt, fp));
    EXPECT_EQ(fmt, JP2_CHANNEL_FORMAT_SPLIT_EXP);
    EXPECT_TRUE(rch6.get_opacity_mapping(0, cmp, lut, cs, fmt, fp));
    kdu_uint16 asoc = 0;
    EXPECT_TRUE(rch6.get_non_colour_mapping(0, asoc, cmp, lut, cs, fmt, fp));
    EXPECT_EQ(asoc, 0x5678);

    kdu_supp::jpx_layer_source l1 = js.access_layer(1);
    jp2_channels rch1 = l1.access_channels();
    kdu_int32 key_val = 0;
    EXPECT_TRUE(rch1.get_chroma_key(0, key_val));
    EXPECT_EQ(key_val, 11);
    EXPECT_TRUE(rch1.get_chroma_key(1, key_val));
    EXPECT_EQ(key_val, 22);
    EXPECT_TRUE(rch1.get_chroma_key(2, key_val));
    EXPECT_EQ(key_val, 33);
    EXPECT_FALSE(rch1.get_chroma_key(9, key_val));
    js.close();
    fsrc.close();
  }

  // 2. jp2_family_src::mem_config, alloc/free failure limits, and jp2_input_box
  // open_as / open / fork error paths and long/UUID box parsing.
  {
    std::vector<kdu_byte> raw;
    AppendBox(raw, 0x6A502020, {0x0D, 0x0A, 0x87, 0x0A});
    std::vector<kdu_byte> ftyp_p;
    AppendBe32(ftyp_p, 0x6A703220);
    AppendBe32(ftyp_p, 0);
    AppendBe32(ftyp_p, 0x6A703220);
    AppendBox(raw, 0x66747970, ftyp_p);
    // Long-header UUID box (16-byte UUID + 8-byte payload).
    std::vector<kdu_byte> uuid_p(24, 0x42);
    AppendBox(raw, jp2_uuid_4cc, uuid_p, /*long_header=*/true);
    // Large buffered sub-box container to exercise heap/broker allocation.
    std::vector<kdu_byte> free_p(512, 0x5A);
    AppendBox(raw, jp2_free_4cc, free_p);

    MemSource msrc(raw);
    TrackingMemBroker tight_broker(128);
    jp2_family_src fsrc;
    fsrc.open(&msrc, &tight_broker);
    EXPECT_TRUE(ExpectKduThrows([&]() {
                  jp2_input_box box;
                  ASSERT_TRUE(box.open(&fsrc));
                  ASSERT_TRUE(box.open_next());  // ftyp
                  ASSERT_TRUE(box.open_next());  // uuid (long header)
                  kdu_byte uuid[16] = {};
                  EXPECT_EQ(box.read(uuid, 16), 16);
                  EXPECT_EQ(uuid[0], 0x42);
                  ASSERT_TRUE(box.open_next());  // free (512 bytes)
                  // Over 128-byte limit -> load_in_memory triggers broker
                  // limit.
                  EXPECT_FALSE(box.load_in_memory(1024));
                  jp2_input_box dup;
                  dup.fork(box);
                  dup.close();
                  box.close();
                }) ||
                tight_broker.failures() > 0);
    fsrc.close(/*allow_src_close=*/false);

    // open_as with valid box locator and fork(in_memory_box).
    MemSource msrc2(raw);
    jp2_family_src fsrc2;
    fsrc2.open(&msrc2);
    jp2_input_box b_root;
    ASSERT_TRUE(b_root.open(&fsrc2));
    jp2_locator box_loc = b_root.get_locator();
    b_root.close();
    jp2_locator cont_loc = box_loc;
    cont_loc.set_file_pos(box_loc.get_file_pos() + 8);
    jp2_input_box b_as;
    EXPECT_TRUE(b_as.open_as(0x6A502020, &fsrc2, box_loc, cont_loc, 4));
    EXPECT_EQ(b_as.get_box_type(), 0x6A502020U);
    EXPECT_EQ(b_as.get_remaining_bytes(), 4);
    EXPECT_TRUE(b_as.load_in_memory(64));
    EXPECT_TRUE(ExpectKduThrows([&]() { b_as.open(&fsrc2); }));
    jp2_input_box b_forked;
    b_forked.fork(b_as);
    EXPECT_TRUE(ExpectKduThrows([&]() { b_forked.fork(b_as); }));
    b_forked.close();
    b_as.close();
    EXPECT_TRUE(ExpectKduThrows([&]() { b_forked.fork(b_as); }));
    EXPECT_TRUE(ExpectKduThrows([&]() {
      jp2_input_box closed_sub;
      closed_sub.open(&b_as);
    }));
    fsrc2.close();
  }

  // 3. jp2_output_box: use_long_header, set_rubber_length errors, >256B resize,
  // over-target_size error, and TrackingMemBroker failure.
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);

    jp2_output_box lb;
    lb.open(&ftgt, 0x66726565);
    EXPECT_EQ(lb.use_long_header(), 16);
    EXPECT_TRUE(ExpectKduThrows([&]() { lb.set_rubber_length(); }));
    std::vector<kdu_byte> big(600, 0x77);
    EXPECT_TRUE(lb.write(big.data(), static_cast<int>(big.size())));
    EXPECT_TRUE(lb.close());

    jp2_output_box fixed;
    fixed.open(&ftgt, 0x66726565);
    fixed.set_target_size(4);
    EXPECT_TRUE(ExpectKduThrows([&]() {
      kdu_byte too_much[8] = {};
      fixed.write(too_much, 8);
    }));
    EXPECT_TRUE(ExpectKduThrows([&]() { fixed.close(); }));

    // Superbox forced rubber length via inner rubber-length child box, plus
    // use_long_header() error after set_rubber_length().
    jp2_output_box sup;
    sup.open(&ftgt, 0x6D6F6F76 /*'moov'*/);
    sup.write(big.data(), 300);  // buffered > 256B before rubber transition
    sup.set_rubber_length();
    EXPECT_TRUE(ExpectKduThrows([&]() { sup.use_long_header(); }));
    jp2_output_box child;
    child.open(&sup, 0x6D646174 /*'mdat'*/, /*rubber_length=*/true);
    kdu_byte tail[4] = {1, 2, 3, 4};
    EXPECT_TRUE(child.write(tail, 4));
    EXPECT_TRUE(child.close());
    EXPECT_TRUE(sup.close());
    ftgt.close();

    // Target broker limit test.
    TrackingMemBroker tgt_broker(64);
    MemTarget mtgt2;
    jp2_family_tgt ftgt2;
    ftgt2.open(&mtgt2, &tgt_broker);
    jp2_output_box over;
    over.open(&ftgt2, 0x66726565);
    EXPECT_TRUE(ExpectKduThrows([&]() {
      std::vector<kdu_byte> huge(2048, 0x11);
      over.write(huge.data(), static_cast<int>(huge.size()));
    }));
    over.close();
    ftgt2.close();
    EXPECT_GT(tgt_broker.failures(), 0);
  }

  // 4. Synthetic JP2 files exercising pclr (16-bit signed & 38-bit unsigned +
  // errors), cdef, cmap, bpcc, res (resc + resd + unknown + errors), unknown
  // jp2h subboxes, dtbl, and jp2_source::read_header(true/false) errors.
  {
    auto make_jp2_with_jp2h = [](const std::vector<kdu_byte>& jp2h_payload,
                                 const std::vector<kdu_byte>& extra_top = {}) {
      std::vector<kdu_byte> out;
      AppendBox(out, 0x6A502020, {0x0D, 0x0A, 0x87, 0x0A});
      std::vector<kdu_byte> ftyp;
      AppendBe32(ftyp, 0x6A703220);
      AppendBe32(ftyp, 0);
      AppendBe32(ftyp, 0x6A703220);
      AppendBox(out, 0x66747970, ftyp);
      AppendBox(out, 0x6A703268, jp2h_payload);
      out.insert(out.end(), extra_top.begin(), extra_top.end());
      return out;
    };
    auto make_ihdr = [](kdu_uint32 w, kdu_uint32 h, kdu_uint16 comps,
                        kdu_byte bpc, kdu_byte c_type, kdu_byte unkc,
                        kdu_byte ipr) {
      std::vector<kdu_byte> p;
      AppendBe32(p, h);
      AppendBe32(p, w);
      AppendBe16(p, comps);
      p.push_back(bpc);
      p.push_back(c_type);
      p.push_back(unkc);
      p.push_back(ipr);
      return p;
    };
    auto make_colr_srgb = []() {
      std::vector<kdu_byte> c = {1, 0, 0};
      AppendBe32(c, kdu_supp::JP2_sRGB_SPACE);
      return c;
    };

    // Valid complex jp2h: ihdr (bpc=255 -> bpcc), bpcc (signed + unsigned),
    // pclr (1 entry? 2 entries x 3 components with 16-bit signed + 38-bit
    // unsigned -> hits entry_bytes==5 shifts), cmap, cdef (incl asoc=0 +
    // 65535), res (resc + resd with neg/large exponents + unknown sub-box),
    // colr, unknown jp2h sub-box + codestream.
    std::vector<kdu_byte> jp2h_valid;
    AppendBox(jp2h_valid, 0x69686472 /*ihdr*/,
              make_ihdr(4, 4, 1, 255, 7, 0, 0));
    AppendBox(jp2h_valid, 0x62706363 /*bpcc*/, {7});  // 8-bit unsigned
    {
      // pclr: 2 entries, 3 components: 16-bit signed (0x8F), 38-bit unsigned
      // (37), 8-bit unsigned (7).
      std::vector<kdu_byte> pclr;
      AppendBe16(pclr, 2);
      pclr.push_back(3);
      pclr.push_back(0x8F);
      pclr.push_back(37);
      pclr.push_back(7);
      for (int e = 0; e < 2; ++e) {
        AppendBe16(pclr, static_cast<kdu_uint16>(e ? 0xFFF0 : 0x0010));
        // 5 bytes for 38-bit unsigned
        pclr.push_back(0x10);
        pclr.push_back(0x20);
        pclr.push_back(0x30);
        pclr.push_back(0x40);
        pclr.push_back(static_cast<kdu_byte>(e ? 0xC0 : 0x00));
        pclr.push_back(static_cast<kdu_byte>(e * 200));
      }
      AppendBox(jp2h_valid, 0x70636C72 /*pclr*/, pclr);
    }
    {
      // cmap: map 3 palette columns from component 0.
      std::vector<kdu_byte> cmap;
      for (kdu_byte col = 0; col < 3; ++col) {
        AppendBe16(cmap, 0);
        cmap.push_back(1);  // palette mapped
        cmap.push_back(col);
      }
      AppendBox(jp2h_valid, 0x636D6170 /*cmap*/, cmap);
    }
    {
      // cdef: 3 triplets (asoc=1, 2, 3 matching the 3 cmap channels).
      std::vector<kdu_byte> cdef;
      AppendBe16(cdef, 3);
      AppendBe16(cdef, 0);
      AppendBe16(cdef, 0);
      AppendBe16(cdef, 1);
      AppendBe16(cdef, 1);
      AppendBe16(cdef, 0);
      AppendBe16(cdef, 2);
      AppendBe16(cdef, 2);
      AppendBe16(cdef, 0);
      AppendBe16(cdef, 3);
      AppendBox(jp2h_valid, 0x63646566 /*cdef*/, cdef);
    }
    {
      // res superbox with resc (negative exponent -2), resd (large +50 exp),
      // and unknown subbox.
      std::vector<kdu_byte> res_pay;
      std::vector<kdu_byte> resc;
      AppendBe16(resc, 1);
      AppendBe16(resc, 1);
      AppendBe16(resc, 1);
      AppendBe16(resc, 1);
      resc.push_back(static_cast<kdu_byte>(-2));
      resc.push_back(static_cast<kdu_byte>(-2));
      AppendBox(res_pay, 0x72657363 /*resc*/, resc);
      std::vector<kdu_byte> resd;
      AppendBe16(resd, 2);
      AppendBe16(resd, 1);
      AppendBe16(resd, 3);
      AppendBe16(resd, 1);
      resd.push_back(10);
      resd.push_back(static_cast<kdu_byte>(-10));
      AppendBox(res_pay, 0x72657364 /*resd*/, resd);
      AppendBox(res_pay, 0x7A7A7A7A /*zzzz*/, {1, 2});
      AppendBox(jp2h_valid, 0x72657320 /*res */, res_pay);
    }
    AppendBox(jp2h_valid, 0x636F6C72 /*colr*/, make_colr_srgb());
    AppendBox(jp2h_valid, 0x78787878 /*xxxx*/, {0x01, 0x02});

    std::vector<kdu_byte> extra;
    MemTarget mt_cs;
    CompressMinimalCodestream(mt_cs, 4, 4, 1, 8);
    AppendBox(extra, 0x6A703263 /*jp2c*/, mt_cs.data());

    std::vector<kdu_byte> valid_file = make_jp2_with_jp2h(jp2h_valid, extra);
    {
      MemSource msrc(valid_file);
      jp2_family_src fsrc;
      fsrc.open(&msrc);
      jp2_source jps;
      ASSERT_TRUE(jps.open(&fsrc));
      EXPECT_TRUE(jps.read_header(false));
      // Exercise 38-bit signed/unsigned palette get_lut in both modes.
      jp2_palette pal = jps.access_palette();
      float flt_lut[2] = {};
      kdu_sample16 fix_lut[2] = {};
      pal.get_lut(0, flt_lut);
      pal.get_lut(1, flt_lut);
      pal.get_lut(0, fix_lut);
      pal.get_lut(1, fix_lut);
      EXPECT_GT(jps.access_resolution().get_resolution(true), 0.0F);
      jps.close();
      fsrc.close();
    }

    // Error cases via jp2_source::read_header(true) and read_header(false).
    // Note: read_header returns int (1=complete, 0=need more data,
    // -2=incompatible JP2 structure, -3=incompatible brand), or throws (-10).
    auto check_hdr = [](const std::vector<kdu_byte>& file, bool ret_incompat) {
      MemSource msrc(file);
      jp2_family_src fsrc;
      fsrc.open(&msrc);
      jp2_source jps;
      int res = 1;
      bool threw = ExpectKduThrows([&]() {
        jps.open(&fsrc);
        res = jps.read_header(ret_incompat);
      });
      jps.close();
      fsrc.close();
      return threw ? -10 : res;
    };

    // c_type == 1 (non-JPEG2000 compression) succeeds in read_header, while
    // c_type == 10 (> JP2_COMPRESSION_TYPE_JBIG) throws.
    {
      std::vector<kdu_byte> h;
      AppendBox(h, 0x69686472, make_ihdr(4, 4, 1, 7, 1, 0, 0));
      AppendBox(h, 0x636F6C72, make_colr_srgb());
      EXPECT_EQ(check_hdr(make_jp2_with_jp2h(h, extra), true), 1);
      EXPECT_EQ(check_hdr(make_jp2_with_jp2h(h, extra), false), 1);

      std::vector<kdu_byte> h_bad_ctype;
      AppendBox(h_bad_ctype, 0x69686472, make_ihdr(4, 4, 1, 7, 10, 0, 0));
      AppendBox(h_bad_ctype, 0x636F6C72, make_colr_srgb());
      EXPECT_EQ(check_hdr(make_jp2_with_jp2h(h_bad_ctype, extra), false), -10);
    }
    // Malformed ihdr: truncated, zero dims, bpc=38, trailing.
    {
      std::vector<kdu_byte> h_short;
      AppendBox(h_short, 0x69686472, {0, 0, 0, 4});
      EXPECT_EQ(check_hdr(make_jp2_with_jp2h(h_short, extra), false), -10);

      std::vector<kdu_byte> h_bad_dim;
      AppendBox(h_bad_dim, 0x69686472, make_ihdr(0, 4, 1, 7, 7, 0, 0));
      EXPECT_EQ(check_hdr(make_jp2_with_jp2h(h_bad_dim, extra), false), -10);

      std::vector<kdu_byte> h_bad_bpc;
      AppendBox(h_bad_bpc, 0x69686472, make_ihdr(4, 4, 1, 38, 7, 0, 0));
      EXPECT_EQ(check_hdr(make_jp2_with_jp2h(h_bad_bpc, extra), false), -10);

      std::vector<kdu_byte> ih = make_ihdr(4, 4, 1, 7, 7, 0, 0);
      ih.push_back(0x99);
      std::vector<kdu_byte> h_trail;
      AppendBox(h_trail, 0x69686472, ih);
      EXPECT_EQ(check_hdr(make_jp2_with_jp2h(h_trail, extra), false), -10);
    }
    // Incompatible ftyp brand (neither jp2 nor jpx) -> -3 or throw.
    {
      std::vector<kdu_byte> bad_brand;
      AppendBox(bad_brand, 0x6A502020, {0x0D, 0x0A, 0x87, 0x0A});
      std::vector<kdu_byte> ftyp;
      AppendBe32(ftyp, 0x666F6F20 /*foo */);
      AppendBe32(ftyp, 0);
      AppendBe32(ftyp, 0x62617220 /*bar */);
      AppendBox(bad_brand, 0x66747970, ftyp);
      std::vector<kdu_byte> h;
      AppendBox(h, 0x69686472, make_ihdr(4, 4, 1, 7, 7, 0, 0));
      AppendBox(h, 0x636F6C72, make_colr_srgb());
      AppendBox(bad_brand, 0x6A703268, h);
      bad_brand.insert(bad_brand.end(), extra.begin(), extra.end());
      EXPECT_EQ(check_hdr(bad_brand, true), -3);
      EXPECT_EQ(check_hdr(bad_brand, false), -10);
    }
    // Missing colr box throws in both modes.
    {
      std::vector<kdu_byte> h;
      AppendBox(h, 0x69686472, make_ihdr(4, 4, 1, 7, 7, 0, 0));
      EXPECT_EQ(check_hdr(make_jp2_with_jp2h(h, extra), true), -10);
      EXPECT_EQ(check_hdr(make_jp2_with_jp2h(h, extra), false), -10);
    }
    // pclr errors (ne=0, truncated bpc, bpc>37, truncated entries, trailing).
    for (const std::vector<kdu_byte>& bad_pclr :
         std::vector<std::vector<kdu_byte>>{
             {0, 0, 1},
             {0, 1, 2, 7},
             {0, 1, 1, 38},
             {0, 2, 1, 7, 10},
             {0, 1, 1, 7, 10, 99},
         }) {
      std::vector<kdu_byte> h;
      AppendBox(h, 0x69686472, make_ihdr(4, 4, 1, 7, 7, 0, 0));
      AppendBox(h, 0x70636C72, bad_pclr);
      AppendBox(h, 0x636F6C72, make_colr_srgb());
      EXPECT_EQ(check_hdr(make_jp2_with_jp2h(h, extra), false), -10);
    }
    // resc/resd errors (empty, v_exp=50 out-of-range, & trailing bytes).
    for (const std::vector<kdu_byte>& bad_resc :
         std::vector<std::vector<kdu_byte>>{
             {0, 1, 0, 1},
             {0, 2, 0, 1, 0, 3, 0, 1, 50, 0},
             {0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0x99},
         }) {
      std::vector<kdu_byte> h;
      AppendBox(h, 0x69686472, make_ihdr(4, 4, 1, 7, 7, 0, 0));
      std::vector<kdu_byte> rp;
      AppendBox(rp, 0x72657363, bad_resc);
      AppendBox(h, 0x72657320, rp);
      AppendBox(h, 0x636F6C72, make_colr_srgb());
      EXPECT_EQ(check_hdr(make_jp2_with_jp2h(h, extra), false), -10);
    }
    // cdef errors (num_descs=0, invalid typ=9, global+local collision, trail).
    for (const std::vector<kdu_byte>& bad_cdef :
         std::vector<std::vector<kdu_byte>>{
             {0, 0},
             {0, 1, 0, 0, 0, 9, 0, 1},
             {0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
             {0, 1, 0, 0, 0, 0, 0, 1, 0x55},
         }) {
      std::vector<kdu_byte> h;
      AppendBox(h, 0x69686472, make_ihdr(4, 4, 1, 7, 7, 0, 0));
      AppendBox(h, 0x63646566, bad_cdef);
      AppendBox(h, 0x636F6C72, make_colr_srgb());
      EXPECT_EQ(check_hdr(make_jp2_with_jp2h(h, extra), false), -10);
    }
  }
}

TEST(Jp2Test, MassiveUncoveredPathsMemBoxesIccPaletteColourAndDref) {
  using kdu_supp::jp2_channels;
  using kdu_supp::jp2_colour;
  using kdu_supp::jp2_colour_converter;
  using kdu_supp::jp2_colour_space;
  using kdu_supp::jp2_data_references;
  using kdu_supp::jp2_dimensions;
  using kdu_supp::jp2_dref_permissions;
  using kdu_supp::jp2_family_src;
  using kdu_supp::jp2_family_tgt;
  using kdu_supp::jp2_input_box;
  using kdu_supp::jp2_output_box;
  using kdu_supp::jp2_palette;
  using kdu_supp::jp2_source;
  using kdu_supp::jp2_target;
  using kdu_supp::jpx_codestream_target;
  using kdu_supp::jpx_layer_target;
  using kdu_supp::jpx_source;
  using kdu_supp::jpx_target;

  ScopedMessageHandlers handlers;

  // Part A: jp2_family_src::mem_config, alloc_uint8 (>=256B prefix),
  // free (align_probe 1/4/8), load_in_memory, read_box_header edge cases.
  {
    std::vector<kdu_byte> stream;
    AppendBox(stream, jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
    // Large xml box (320 bytes >= 256 -> prefix_bytes=4 in alloc_uint8).
    std::vector<kdu_byte> big_xml(320, 'A');
    AppendBox(stream, jp2_xml_4cc, big_xml);
    // Superbox containing two sub-boxes, the last one with rubber_length=0.
    std::vector<kdu_byte> res_sub;
    AppendBox(res_sub, jp2_capture_resolution_4cc,
              {0, 1, 0, 1, 0, 1, 0, 1, 0, 0});
    // Rubber-length sub-box (LBox=0).
    AppendBe32(res_sub, 0);
    AppendBe32(res_sub, jp2_display_resolution_4cc);
    for (int i = 0; i < 10; ++i) res_sub.push_back(static_cast<kdu_byte>(i));
    AppendBox(stream, jp2_resolution_4cc, res_sub);

    // mem_config with partial close(true), tiny broker OOM (allow_null=true),
    // and large broker switch + reset to nullptr.
    kdu_core::kdu_membroker tiny_broker(/*limit_bytes=*/8);
    kdu_core::kdu_membroker big_broker(/*limit_bytes=*/1024 * 1024);
    MemSource msrc(stream);
    jp2_family_src fsrc;
    fsrc.open(&msrc, &tiny_broker);
    EXPECT_EQ(fsrc.get_membroker(), &tiny_broker);

    jp2_input_box box;
    EXPECT_FALSE(box.load_in_memory(1024));  // Unopened box.
    EXPECT_TRUE(box.open(&fsrc));            // jP
    box.close();
    EXPECT_TRUE(box.open_next());          // xml (320 bytes)
    EXPECT_FALSE(box.load_in_memory(16));  // Exceeds max_bytes=16.
    std::array<kdu_byte, 8> small_ext{};
    EXPECT_FALSE(box.load_in_memory(16, small_ext.data(), 8));
    // tiny_broker rejects 320-byte internal allocation -> returns false.
    EXPECT_FALSE(box.load_in_memory(1024));
    box.close();
    fsrc.close(/*leave_indirect_open=*/true);
    msrc.seek(0);
    fsrc.open(&msrc, &big_broker);
    EXPECT_EQ(fsrc.get_membroker(), &big_broker);
    EXPECT_TRUE(box.open(&fsrc));
    box.close();
    EXPECT_TRUE(box.open_next());  // xml (320 bytes -> align_probe=4 alloc)
    std::array<kdu_byte, 400> big_ext{};
    EXPECT_TRUE(
        box.load_in_memory(1024, big_ext.data(), 400));  // Uses ext buf.
    EXPECT_TRUE(box.load_in_memory(1024));  // Already loaded in memory -> true.
    kdu_long cpos = 0;
    kdu_byte* clim = nullptr;
    EXPECT_EQ(box.access_memory(cpos, clim), big_ext.data());
    box.close();

    // Superbox loaded in memory -> open sub-box + rubber-length sub-box.
    EXPECT_TRUE(box.open_next());  // res_ (superbox)
    EXPECT_TRUE(box.load_in_memory(4096));
    jp2_input_box sub;
    EXPECT_TRUE(sub.open(&box));  // resc inside in-memory superbox
    EXPECT_TRUE(sub.load_in_memory(4096));
    EXPECT_EQ(sub.get_box_type(), jp2_capture_resolution_4cc);
    sub.close();
    EXPECT_TRUE(sub.open_next());  // resd with rubber_length=0
    EXPECT_EQ(sub.get_box_type(), jp2_display_resolution_4cc);
    EXPECT_EQ(sub.get_remaining_bytes(), 10);
    sub.close();
    box.close();
    fsrc.close(/*leave_indirect_open=*/true);
    msrc.seek(0);
    fsrc.open(&msrc, nullptr);
    EXPECT_TRUE(box.open(&fsrc));
    box.close();
    EXPECT_TRUE(box.open_next());
    EXPECT_TRUE(
        box.load_in_memory(4096));  // Internal alloc >=256 bytes + free.
    box.close();
    fsrc.close();

    // Premature EOF during load_in_memory adjusts contents length.
    std::vector<kdu_byte> trunc_stream;
    AppendBe32(trunc_stream, 128);
    AppendBe32(trunc_stream, jp2_xml_4cc);
    trunc_stream.insert(trunc_stream.end(), 12, 'Z');
    MemSource tsrc(trunc_stream);
    jp2_family_src tfsrc;
    tfsrc.open(&tsrc);
    jp2_input_box tbox;
    EXPECT_TRUE(tbox.open(&tfsrc));
    EXPECT_TRUE(tbox.load_in_memory(1024));
    EXPECT_LT(tbox.get_box_bytes(), 128);
    tbox.close();
    tfsrc.close();

    // read_box_header error & truncation cases.
    // 1. Truncated < 8 bytes & LBox=1 < 16 bytes -> open returns false.
    for (const std::vector<kdu_byte>& short_hdr :
         std::vector<std::vector<kdu_byte>>{
             {0, 0, 0, 12},
             {0, 0, 0, 1, 'x', 'm', 'l', ' ', 0, 0, 0, 0},
         }) {
      MemSource s(short_hdr);
      jp2_family_src f;
      f.open(&s);
      jp2_input_box b;
      EXPECT_FALSE(b.open(&f));
      f.close();
    }
    // 2. Illegal LBox=4 (<8), XLBox=12 (<16), negative XLBox (b/332080984).
    std::vector<std::vector<kdu_byte>> bad_hdrs;
    {
      std::vector<kdu_byte> b;
      AppendBe32(b, 4);
      AppendBe32(b, jp2_xml_4cc);
      bad_hdrs.push_back(b);
    }
    {
      std::vector<kdu_byte> b;
      AppendBe32(b, 1);
      AppendBe32(b, jp2_xml_4cc);
      AppendBe64(b, 12);
      bad_hdrs.push_back(b);
    }
    {
      std::vector<kdu_byte> b;
      AppendBe32(b, 1);
      AppendBe32(b, jp2_xml_4cc);
      AppendBe64(b, 0x8000000000000100ULL);
      bad_hdrs.push_back(b);
    }
    // 3. phld box shorter than 28 bytes (original_box_length < 28 -> err 5).
    {
      std::vector<kdu_byte> b;
      AppendBox(b, jp2_placeholder_4cc, std::vector<kdu_byte>(8, 0));
      bad_hdrs.push_back(b);
    }
    // 4. phld box with valid 28B header containing illegal LBox=4 or XLBox<0.
    {
      std::vector<kdu_byte> ph(20, 0);
      WriteBe32At(ph, 12, 4);
      WriteBe32At(ph, 16, jp2_xml_4cc);
      std::vector<kdu_byte> b;
      AppendBox(b, jp2_placeholder_4cc, ph);
      bad_hdrs.push_back(b);
    }
    {
      std::vector<kdu_byte> ph(28, 0);
      WriteBe32At(ph, 12, 1);
      WriteBe32At(ph, 16, jp2_xml_4cc);
      ph[20] = 0x80;
      std::vector<kdu_byte> b;
      AppendBox(b, jp2_placeholder_4cc, ph);
      bad_hdrs.push_back(b);
    }
    // 5. phld box nested inside superbox (jp2.cpp:1667 throws).
    {
      std::vector<kdu_byte> ph(20, 0);
      WriteBe32At(ph, 0, 1);  // flags & 1
      WriteBe32At(ph, 12, 12);
      WriteBe32At(ph, 16, jp2_xml_4cc);
      std::vector<kdu_byte> sup;
      AppendBox(sup, jp2_header_4cc, ph);
      bad_hdrs.push_back(sup);
    }
    for (size_t idx = 0; idx < bad_hdrs.size(); ++idx) {
      MemSource s(bad_hdrs[idx]);
      jp2_family_src f;
      f.open(&s);
      jp2_input_box b;
      EXPECT_THROW(
          {
            if (b.open(&f) && idx == bad_hdrs.size() - 1) {
              jp2_input_box inner;
              inner.open(&b);
              inner.close();
            }
            b.close();
          },
          kdu_core::kdu_exception);
      b.close();
      f.close();
    }
    // Valid simple placeholder box (flags=0 no access vs flags=1 with XLBox).
    for (kdu_uint32 flags : {0U, 1U}) {
      std::vector<kdu_byte> ph(28, 0);
      WriteBe32At(ph, 0, flags);
      WriteBe32At(ph, 12, 1);
      WriteBe32At(ph, 16, jp2_xml_4cc);
      WriteBe32At(ph, 24, 32);  // XLBox=32
      std::vector<kdu_byte> b;
      AppendBox(b, jp2_placeholder_4cc, ph);
      MemSource s(b);
      jp2_family_src f;
      f.open(&s);
      jp2_input_box box_ph;
      if (flags == 0) {
        EXPECT_THROW(box_ph.open(&f), kdu_core::kdu_exception);
      } else {
        EXPECT_TRUE(box_ph.open(&f));
        EXPECT_EQ(box_ph.get_box_type(), jp2_xml_4cc);
      }
      box_ph.close();
      f.close();
    }
  }

  // Part B: jp2_output_box & jp2_family_tgt (start_rewrite, end_rewrite,
  // reopen with type change, write_free_and_close >=1024B, set_target_size,
  // write failures, write_header_last).
  {
    MemTarget rtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&rtgt);

    jp2_output_box box;
    box.open(&ftgt, jp2_xml_4cc);
    box.use_long_header();
    std::vector<kdu_byte> payload(16, 0x33);
    box.write(payload.data(), 16);
    EXPECT_FALSE(box.start_rewrite(-1));
    EXPECT_FALSE(box.start_rewrite(100));
    EXPECT_TRUE(box.start_rewrite(8));
    EXPECT_FALSE(box.start_rewrite(4));  // Double start_rewrite -> false.
    std::array<kdu_byte, 4> patch = {1, 2, 3, 4};
    box.write(patch.data(), 4);
    EXPECT_TRUE(box.end_rewrite());
    EXPECT_FALSE(box.end_rewrite());              // Not rewriting -> false.
    EXPECT_TRUE(box.write_free_and_close(1100));  // >1024B zero fill loop.

    // Reopen closed box with a DIFFERENT box_type, exercising reopen type
    // rewrite on RewriteMemTarget, and immediate write beyond box_size throws.
    EXPECT_EQ(box.reopen(jp2_uuid_4cc, 999999), -1);
    EXPECT_GE(box.reopen(jp2_uuid_4cc, 0), 16);
    std::array<kdu_byte, 4> new_hdr = {'u', 'u', 'i', 'd'};
    box.write(new_hdr.data(), 4);
    // Write huge chunk exceeding box_size -> KDU_ERROR_DEV.
    std::vector<kdu_byte> too_much(2000, 0x55);
    EXPECT_THROW(box.write(too_much.data(), static_cast<int>(too_much.size())),
                 kdu_core::kdu_exception);
    EXPECT_TRUE(box.close());

    // Superbox + sub-box reopen with type change + write_header_last.
    jp2_output_box sup, sub;
    sup.open(&ftgt, jp2_header_4cc);
    sup.write_header_last();
    sub.open(&sup, jp2_xml_4cc);
    sub.write(payload.data(), 12);
    EXPECT_TRUE(sub.close());
    EXPECT_EQ(sub.reopen(jp2_uuid_4cc, 0), 12);
    sub.write(new_hdr.data(), 4);
    EXPECT_TRUE(sub.close());
    EXPECT_TRUE(sup.close());

    // set_target_size error (<8 write_free_and_close) & write over target_size.
    jp2_output_box fixed_box;
    fixed_box.open(&ftgt, jp2_xml_4cc);
    fixed_box.set_target_size(12);
    fixed_box.write(payload.data(), 4);
    EXPECT_THROW(fixed_box.write_free_and_close(4), kdu_core::kdu_exception);
    EXPECT_THROW(fixed_box.write_free_and_close(12), kdu_core::kdu_exception);
    EXPECT_THROW(fixed_box.set_rubber_length(), kdu_core::kdu_exception);
    EXPECT_TRUE(fixed_box.write_free_and_close(8));  // Exact 4 + 8 == 12.

    {
      jp2_output_box over_box;
      over_box.open(&ftgt, jp2_xml_4cc);
      over_box.set_target_size(12);
      over_box.write(payload.data(), 12);
      EXPECT_THROW(over_box.write(payload.data(), 4), kdu_core::kdu_exception);
      EXPECT_THROW(over_box.close(), kdu_core::kdu_exception);
    }

    // Indirect write_box failure with fail_writes=true.
    rtgt.set_fail_writes(true);
    jp2_output_box fail_box;
    fail_box.open(&ftgt, jp2_xml_4cc);
    fail_box.write(payload.data(), 16);
    EXPECT_FALSE(fail_box.close());
    rtgt.set_fail_writes(false);
    ftgt.close();
  }

  // Part C: jp2_palette::get_signed + get_lut(float) across DEFAULT,
  // FIXPOINT, FLOAT, 1-bit (<2), 16-bit, 32-bit, and invalid args.
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    jpx_target jt;
    jt.open(&ftgt);
    jpx_codestream_target cs = jt.add_codestream();
    jpx_layer_target lt = jt.add_layer();
    cs.access_dimensions().init(kdu_coords(4, 4), 1);
    cs.access_dimensions().set_precision(0, 8, false);
    jp2_palette pal = cs.access_palette();
    pal.init(5, 4);
    kdu_int32 lut1[4] = {0, 1, 0, 1};  // 1-bit unsigned (<2 bits)
    kdu_int32 lut16u[4] = {0, 0x3C00, 0x8001, 0xFFFF};
    kdu_int32 lut16s[4] = {-32768, -100, 100, 32767};
    kdu_int32 lut32u[4] = {0x00000001, 0x3F800000, 0x7F800000,
                           static_cast<kdu_int32>(0xBF800000U)};
    kdu_int32 lut32s[4] = {static_cast<kdu_int32>(0xC0000000U), 0, 0x3F800000,
                           0x47800000};
    EXPECT_TRUE(pal.set_lut(0, lut1, 1, false));
    EXPECT_TRUE(pal.set_lut(1, lut16u, 16, false));
    EXPECT_TRUE(pal.set_lut(2, lut16s, 16, true));
    EXPECT_TRUE(pal.set_lut(3, lut32u, 32, false));
    EXPECT_TRUE(pal.set_lut(4, lut32s, 32, true));
    lt.access_colour(0).init(kdu_supp::JP2_sRGB_SPACE);
    lt.access_channels().init(3);
    lt.access_channels().set_colour_mapping(0, 0, 1, 0);
    lt.access_channels().set_colour_mapping(1, 0, 3, 0);
    lt.access_channels().set_colour_mapping(2, 0, 4, 0);
    jt.write_headers();
    {
      auto* s = cs.open_stream();
      kdu_byte soc[2] = {0xFF, 0x4F};
      s->write(soc, 2);
      s->close();
    }
    jt.close();
    ftgt.close();

    MemSource msrc(mtgt.data());
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    jpx_source jsrc;
    EXPECT_EQ(jsrc.open(&fsrc, false), 1);
    jp2_palette rpal = jsrc.access_codestream(0).access_palette();
    EXPECT_FALSE(rpal.get_signed(-1));
    EXPECT_FALSE(rpal.get_signed(99));
    EXPECT_FALSE(rpal.get_signed(0));
    EXPECT_TRUE(rpal.get_signed(2));

    float out[4];
    EXPECT_TRUE(rpal.get_lut(0, out, JP2_CHANNEL_FORMAT_DEFAULT, 0));
    EXPECT_TRUE(rpal.get_lut(0, out, JP2_CHANNEL_FORMAT_FIXPOINT, 0));
    EXPECT_TRUE(rpal.get_lut(1, out, JP2_CHANNEL_FORMAT_FIXPOINT, 2));
    EXPECT_TRUE(rpal.get_lut(2, out, JP2_CHANNEL_FORMAT_FIXPOINT, 2));
    EXPECT_TRUE(rpal.get_lut(1, out, JP2_CHANNEL_FORMAT_FLOAT, 5));
    EXPECT_TRUE(rpal.get_lut(3, out, JP2_CHANNEL_FORMAT_DEFAULT, 0));
    EXPECT_TRUE(rpal.get_lut(4, out, JP2_CHANNEL_FORMAT_DEFAULT, 0));
    EXPECT_TRUE(rpal.get_lut(3, out, JP2_CHANNEL_FORMAT_FIXPOINT, 4));
    EXPECT_TRUE(rpal.get_lut(4, out, JP2_CHANNEL_FORMAT_FIXPOINT, 4));
    EXPECT_TRUE(rpal.get_lut(3, out, JP2_CHANNEL_FORMAT_FLOAT, 8));
    EXPECT_TRUE(rpal.get_lut(4, out, JP2_CHANNEL_FORMAT_FLOAT, 8));
    EXPECT_TRUE(rpal.get_lut(3, out, JP2_CHANNEL_FORMAT_FLOAT, 12));
    EXPECT_TRUE(rpal.get_lut(4, out, JP2_CHANNEL_FORMAT_FLOAT, 12));
    // Invalid get_lut inputs.
    EXPECT_FALSE(rpal.get_lut(-1, out, JP2_CHANNEL_FORMAT_DEFAULT, 0));
    EXPECT_FALSE(rpal.get_lut(99, out, JP2_CHANNEL_FORMAT_DEFAULT, 0));
    EXPECT_TRUE(rpal.get_lut(0, out, JP2_CHANNEL_FORMAT_FLOAT, 1));
    EXPECT_THROW(rpal.get_lut(1, out, JP2_CHANNEL_FORMAT_SPLIT_EXP, 0),
                 kdu_core::kdu_exception);
    jsrc.close();
    fsrc.close();
  }

  // Part D: jp2_colour::init(all spaces), copy/compare differences,
  // ICC para tag unknown linear fallback + val16=4, float convert_rgb.
  {
    const jp2_colour_space all_spaces[] = {
        kdu_supp::JP2_bilevel1_SPACE, kdu_supp::JP2_YCbCr1_SPACE,
        kdu_supp::JP2_YCbCr2_SPACE,   kdu_supp::JP2_YCbCr3_SPACE,
        kdu_supp::JP2_PhotoYCC_SPACE, kdu_supp::JP2_CMY_SPACE,
        kdu_supp::JP2_CMYK_SPACE,     kdu_supp::JP2_YCCK_SPACE,
        kdu_supp::JP2_bilevel2_SPACE, kdu_supp::JP2_sRGB_SPACE,
        kdu_supp::JP2_sLUM_SPACE,     kdu_supp::JP2_sYCC_SPACE,
        kdu_supp::JP2_esRGB_SPACE,    kdu_supp::JP2_ROMMRGB_SPACE,
        kdu_supp::JP2_YPbPr60_SPACE,  kdu_supp::JP2_YPbPr50_SPACE,
        kdu_supp::JP2_esYCC_SPACE,
    };
    for (jp2_colour_space sp : all_spaces) {
      MemTarget mtgt;
      jp2_family_tgt ftgt;
      ftgt.open(&mtgt);
      jpx_target jt;
      jt.open(&ftgt);
      jpx_codestream_target cs = jt.add_codestream();
      cs.access_dimensions().init(kdu_coords(4, 4), 4);
      for (int c = 0; c < 4; ++c) cs.access_dimensions().set_precision(c, 8);
      jpx_layer_target lt = jt.add_layer();
      lt.access_colour(0).init(sp);
      jpx_layer_target lt_copy = jt.add_layer();
      lt_copy.access_colour(0).copy(lt.access_colour(0));
      jt.write_headers();
      {
        auto* s = cs.open_stream();
        kdu_byte soc[2] = {0xFF, 0x4F};
        s->write(soc, 2);
        s->close();
      }
      jt.close();
      ftgt.close();
    }
    // Invalid colour space throws.
    {
      MemTarget mtgt;
      jp2_family_tgt ftgt;
      ftgt.open(&mtgt);
      jp2_target jt;
      jt.open(&ftgt);
      jt.access_dimensions().init(kdu_coords(4, 4), 3);
      EXPECT_THROW(jt.access_colour().init(kdu_supp::JP2_iccANY_SPACE),
                   kdu_core::kdu_exception);
      jt.close();
      ftgt.close();
    }
    // Multiple layers with differing Lab/Jab/ICC/Vendor colours force
    // j2_colour::compare non-matching branches (separate colr boxes).
    {
      MemTarget mtgt;
      jp2_family_tgt ftgt;
      ftgt.open(&mtgt);
      jpx_target jt;
      jt.open(&ftgt);
      jpx_codestream_target cs = jt.add_codestream();
      cs.access_dimensions().init(kdu_coords(4, 4), 3);
      for (int c = 0; c < 3; ++c) cs.access_dimensions().set_precision(c, 8);
      jpx_codestream_target cs10 = jt.add_codestream();
      cs10.access_dimensions().init(kdu_coords(4, 4), 3);
      for (int c = 0; c < 3; ++c) cs10.access_dimensions().set_precision(c, 10);
      auto init_ch = [](jpx_layer_target& l, int cs_idx) {
        l.access_channels().init(3);
        for (int c = 0; c < 3; ++c) {
          l.access_channels().set_colour_mapping(c, c, -1, cs_idx);
        }
      };
      auto l1 = jt.add_layer();
      init_ch(l1, 0);
      l1.access_colour(0).init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 8, 170, 128,
                               8, 200, 128, 8, JP2_CIE_D50);
      auto l2 = jt.add_layer();
      init_ch(l2, 1);
      l2.access_colour(0).init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 10, 170, 128,
                               10, 200, 128, 10, JP2_CIE_D50);
      auto l3 = jt.add_layer();
      init_ch(l3, 0);
      l3.access_colour(0).init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 8, 170, 128,
                               8, 200, 128, 8, JP2_CIE_D65);
      auto l4 = jt.add_layer();
      init_ch(l4, 0);
      l4.access_colour(0).init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 8, 170, 128,
                               8, 200, 128, 8, JP2_CIE_DAY, 6500);
      auto l5 = jt.add_layer();
      init_ch(l5, 0);
      l5.access_colour(0).init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 8, 170, 128,
                               8, 200, 128, 8, JP2_CIE_DAY, 5000);
      auto l6 = jt.add_layer();
      init_ch(l6, 0);
      l6.access_colour(0).init(kdu_supp::JP2_CIEJab_SPACE);
      // Vendor UUID differences
      kdu_byte uuid1[16] = {1, 2, 3};
      kdu_byte uuid2[16] = {9, 8, 7};
      kdu_byte vd1[4] = {1, 2, 3, 4};
      kdu_byte vd2[4] = {1, 2, 3, 5};
      kdu_byte vd3[6] = {1, 2, 3, 4, 5, 6};
      auto l7 = jt.add_layer();
      init_ch(l7, 0);
      l7.access_colour(0).init(uuid1, 4, vd1);
      auto l8 = jt.add_layer();
      init_ch(l8, 0);
      l8.access_colour(0).init(uuid2, 4, vd1);
      auto l9 = jt.add_layer();
      init_ch(l9, 0);
      l9.access_colour(0).init(uuid1, 4, vd2);
      auto l10 = jt.add_layer();
      init_ch(l10, 0);
      l10.access_colour(0).init(uuid1, 6, vd3);
      // Copy all variants to exercise jp2_colour::copy.
      jpx_layer_target orig_layers[] = {l1, l2, l3, l4, l5,
                                        l6, l7, l8, l9, l10};
      for (size_t idx = 0; idx < 10; ++idx) {
        auto lc = jt.add_layer();
        init_ch(lc, idx == 1 ? 1 : 0);
        lc.access_colour(0).copy(orig_layers[idx].access_colour(0));
      }
      jt.write_headers();
      {
        auto* s = cs.open_stream();
        kdu_byte soc[2] = {0xFF, 0x4F};
        s->write(soc, 2);
        s->close();
      }
      {
        auto* s10 = cs10.open_stream();
        kdu_byte soc[2] = {0xFF, 0x4F};
        s10->write(soc, 2);
        s10->close();
      }
      jt.close();
      ftgt.close();
    }

    // ICC para unknown type (warning + linear get_lut fallback) + para type 4
    // + float convert_rgb with Rec.709 sRGB primaries (skip_primary_matrix).
    {
      auto make_para_tag = [](kdu_uint32 sig, kdu_uint16 fn,
                              std::initializer_list<double> params) {
        std::vector<kdu_byte> d;
        AppendBe32(d, 0x70617261);  // 'para'
        AppendBe32(d, 0);
        AppendBe16(d, fn);
        AppendBe16(d, 0);
        for (double v : params) {
          AppendBe32(
              d, static_cast<kdu_uint32>(static_cast<kdu_int32>(v * 65536.0)));
        }
        return IccTag{sig, std::move(d)};
      };
      // sRGB Rec.709 primaries in XYZ (D50-adapted) ->
      // skip_primary_matrix=true.
      auto make_xyz = [](kdu_uint32 sig, double x, double y, double z) {
        std::vector<kdu_byte> d;
        AppendBe32(d, 0x58595A20);  // 'XYZ '
        AppendBe32(d, 0);
        AppendBe32(
            d, static_cast<kdu_uint32>(static_cast<kdu_int32>(x * 65536.0)));
        AppendBe32(
            d, static_cast<kdu_uint32>(static_cast<kdu_int32>(y * 65536.0)));
        AppendBe32(
            d, static_cast<kdu_uint32>(static_cast<kdu_int32>(z * 65536.0)));
        return IccTag{sig, std::move(d)};
      };
      // Unknown para fn=9 + type 4 (7 params) + float convert_rgb.
      std::vector<kdu_byte> icc1 = BuildIccProfile(
          0x52474220 /*RGB */, 0x58595A20 /*XYZ */,
          {
              make_xyz(0x7258595A, 0.43607, 0.22249, 0.01392),
              make_xyz(0x6758595A, 0.38515, 0.71687, 0.09708),
              make_xyz(0x6258595A, 0.14307, 0.06061, 0.71410),
              make_para_tag(0x72545243, 9, {}),
              make_para_tag(0x67545243, 4,
                            {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92,
                             0.04045, 0.0, 0.0}),
              make_para_tag(0x62545243, 4,
                            {2.2, 0.9, 0.1, 0.1, 0.05, 0.01, -0.01}),
          });
      MemTarget mtgt;
      jp2_family_tgt ftgt;
      ftgt.open(&mtgt);
      jp2_target jt;
      jt.open(&ftgt);
      jt.access_dimensions().init(kdu_coords(4, 4), 3);
      for (int c = 0; c < 3; ++c) jt.access_dimensions().set_precision(c, 8);
      jt.access_colour().init(icc1.data());
      jt.write_header();
      jt.open_codestream(true);
      jt.close();
      ftgt.close();

      MemSource msrc(mtgt.data());
      jp2_family_src fsrc;
      fsrc.open(&msrc);
      jp2_source jsrc;
      EXPECT_THROW(jsrc.read_header(false), kdu_core::kdu_exception);
      EXPECT_TRUE(jsrc.open(&fsrc));
      EXPECT_EQ(jsrc.read_header(false), 1);
      EXPECT_EQ(jsrc.read_header(false),
                1);  // Already ready -> early return 1.

      jp2_colour_converter conv;
      EXPECT_TRUE(conv.init(jsrc.access_colour()));
      kdu_sample_allocator alloc;
      kdu_line_buf r_f, g_f, b_f;
      kdu_line_buf r_i, g_i, b_i;
      r_f.pre_create(&alloc, 4, 0, 0, 0);
      g_f.pre_create(&alloc, 4, 0, 0, 0);
      b_f.pre_create(&alloc, 4, 0, 0, 0);
      r_i.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
      g_i.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
      b_i.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
      alloc.finalize(kdu_core::kdu_codestream());
      r_f.create();
      g_f.create();
      b_f.create();
      r_i.create();
      g_i.create();
      b_i.create();
      float vals[4] = {-0.8f, -0.1f, 0.25f, 0.9f};
      for (int i = 0; i < 4; ++i) {
        r_f.get_buf32()[i].fval = vals[i];
        g_f.get_buf32()[i].fval = vals[i];
        b_f.get_buf32()[i].fval = vals[i];
        r_i.get_buf16()[i].ival = (i == 0) ? -2500 : 100;
        g_i.get_buf16()[i].ival = (i == 0) ? -2500 : 100;
        b_i.get_buf16()[i].ival = (i == 0) ? -2500 : 100;
      }
      EXPECT_TRUE(conv.convert_rgb(r_f, g_f, b_f));
      EXPECT_TRUE(conv.convert_rgb(r_i, g_i, b_i));
      r_f.destroy();
      g_f.destroy();
      b_f.destroy();
      r_i.destroy();
      g_i.destroy();
      b_i.destroy();
      jsrc.close();
      fsrc.close();
    }
  }

  // Part E: jp2_data_references, jp2_dref_permissions extra patterns,
  // jp2_dimensions::init(siz_params) errors, jp2_source::read_header errors.
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    jpx_target jt;
    jt.open(&ftgt);
    jp2_data_references dref = jt.access_data_references();
    EXPECT_EQ(dref.add_url(nullptr), 0);
    EXPECT_EQ(dref.add_url("http://a.com/x.jp2"), 1);
    EXPECT_EQ(dref.add_url("http://a.com/x.jp2"), 1);  // Dedup existing URL.
    EXPECT_EQ(dref.add_url("http://b.com/y.jp2", 14), 14);   // Sparse grow.
    EXPECT_EQ(dref.add_url("http://b2.com/y.jp2", 14), 14);  // Replace slot.
    EXPECT_EQ(dref.add_file_url(nullptr), 0);
    EXPECT_GT(dref.add_file_url("dir/rel.jp2"), 0);
    EXPECT_GT(dref.add_file_url("C:\\dir\\win.jp2"), 0);
    EXPECT_GT(dref.add_file_url("/abs/path.jp2"), 0);
    EXPECT_THROW(dref.add_url("http://bad", 65535), kdu_core::kdu_exception);
    EXPECT_THROW(dref.add_file_url("bad", 65535), kdu_core::kdu_exception);
    EXPECT_GE(dref.get_num_urls(), 15);
    EXPECT_EQ(dref.find_url("http://a.com/x.jp2"), 1);
    EXPECT_EQ(dref.find_url("http://missing"), 0);
    jpx_codestream_target cs = jt.add_codestream();
    cs.access_dimensions().init(kdu_coords(4, 4), 1);
    cs.access_dimensions().set_precision(0, 8);
    auto lt = jt.add_layer();
    lt.access_colour(0).init(kdu_supp::JP2_sLUM_SPACE);
    jt.write_headers();
    {
      auto* s = cs.open_stream();
      kdu_byte soc[2] = {0xFF, 0x4F};
      s->write(soc, 2);
      s->close();
    }
    jt.close();
    ftgt.close();

    // jp2_dref_permissions choice `{a,b}` / escapes `\:` / double wildcards
    // and syntax errors.
    jp2_dref_permissions perms;
    perms.init("/root/file.jp2",
               "./{test,foo}.jp2:./a\\:b.jpx:./a\\*.jpx:**:a**b");
    EXPECT_TRUE(perms.can_access("./test.jp2"));
    EXPECT_TRUE(perms.can_access("./a:b.jpx"));
    EXPECT_TRUE(perms.can_access("./a*.jpx"));
    EXPECT_FALSE(perms.can_access("./a:b.jp3"));
    EXPECT_GT(perms.get_access_path_len("./test.jp2"), 0u);
    std::array<char, 64> pbuf{};
    EXPECT_EQ(perms.get_access_path(nullptr, 64, "./test.jp2"), nullptr);
    EXPECT_EQ(perms.get_access_path(pbuf.data(), 2, "./test.jp2"), nullptr);
    EXPECT_NE(perms.get_access_path(pbuf.data(), 64, "./test.jp2"), nullptr);
    for (const char* bad_pat : {"./a**", "./{a,b", "./a}b", "./\\", "./{{a}"}) {
      perms.init("/root/f.jp2", bad_pat);
      EXPECT_FALSE(perms.can_access("./test.jp2"));
    }

    // jp2_dimensions::init(siz_params) incomplete + already-initialized throws.
    {
      kdu_core::siz_params bad_siz;
      MemTarget mt2;
      jp2_family_tgt ft2;
      ft2.open(&mt2);
      jp2_target jt2;
      jt2.open(&ft2);
      EXPECT_THROW(jt2.access_dimensions().init(&bad_siz),
                   kdu_core::kdu_exception);
      jt2.access_dimensions().init(kdu_coords(4, 4), 1);
      EXPECT_THROW(jt2.access_dimensions().init(kdu_coords(4, 4), 1),
                   kdu_core::kdu_exception);
      jt2.close();
      ft2.close();
    }

    // jp2_source::read_header incompatible (non-jP box) & bad jP payload.
    for (bool ret_incompat : {true, false}) {
      std::vector<kdu_byte> bad_jp;
      AppendBox(bad_jp, jp2_xml_4cc, {'a', 'b', 'c', 'd'});
      MemSource ms(bad_jp);
      jp2_family_src fs;
      fs.open(&ms);
      jp2_source js;
      EXPECT_TRUE(js.open(&fs));
      if (ret_incompat) {
        EXPECT_EQ(js.read_header(true), -1);
      } else {
        EXPECT_THROW(js.read_header(false), kdu_core::kdu_exception);
      }
      js.close();
      fs.close();
    }
    for (bool ret_incompat : {true, false}) {
      std::vector<kdu_byte> bad_sig;
      AppendBox(bad_sig, jp2_signature_4cc, {1, 2, 3, 4});
      MemSource ms(bad_sig);
      jp2_family_src fs;
      fs.open(&ms);
      jp2_source js;
      EXPECT_TRUE(js.open(&fs));
      if (ret_incompat) {
        EXPECT_EQ(js.read_header(true), -1);
      } else {
        EXPECT_THROW(js.read_header(false), kdu_core::kdu_exception);
      }
      js.close();
      fs.close();
    }
  }
}

TEST(Jp2Test, PlaceholdersScopesAndMembroker) {
  ScopedMessageHandlers handlers;

  auto make_box = [](kdu_uint32 type, const std::vector<kdu_byte>& body) {
    std::vector<kdu_byte> out;
    AppendBe32(out, static_cast<kdu_uint32>(8 + body.size()));
    AppendBe32(out, type);
    out.insert(out.end(), body.begin(), body.end());
    return out;
  };
  auto append_vec = [](std::vector<kdu_byte>& dst,
                       const std::vector<kdu_byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
  };
  auto make_phld_body =
      [](kdu_uint32 flags, kdu_uint64 orig_bin_id, kdu_uint32 orig_len,
         kdu_uint32 orig_type, bool has_equiv, kdu_uint64 equiv_bin_id,
         kdu_uint32 equiv_len, kdu_uint32 equiv_type, bool equiv_64,
         kdu_uint64 equiv_len64, bool has_cs, kdu_uint64 cs_min,
         bool has_cs_range, kdu_uint32 cs_offset) {
        std::vector<kdu_byte> d;
        AppendBe32(d, flags);
        AppendBe64(d, orig_bin_id);
        AppendBe32(d, orig_len);
        AppendBe32(d, orig_type);
        if (has_equiv) {
          AppendBe64(d, equiv_bin_id);
          AppendBe32(d, equiv_len);
          AppendBe32(d, equiv_type);
          if (equiv_64) AppendBe64(d, equiv_len64);
        }
        if (has_cs) {
          AppendBe64(d, cs_min);
          if (has_cs_range) AppendBe32(d, cs_offset);
        }
        return d;
      };

  // 1. Placeholder modes and scopes on MemSource.
  {
    std::vector<kdu_byte> uuid_box =
        make_box(jp2_uuid_4cc, std::vector<kdu_byte>(16, 0x11));
    std::vector<kdu_byte> ph_no_acc =
        make_box(jp2_placeholder_4cc,
                 make_phld_body(0, 10, 28, jp2_uuid_4cc, false, 0, 0, 0, false,
                                0, false, 0, false, 0));
    std::vector<kdu_byte> ph_orig = make_box(
        jp2_placeholder_4cc, make_phld_body(1, 2, 28, jp2_uuid_4cc, false, 0, 0,
                                            0, false, 0, false, 0, false, 0));
    std::vector<kdu_byte> url_box =
        make_box(jp2_data_entry_url_4cc, {0, 0, 0, 0, 'a', 0});
    std::vector<kdu_byte> ph_64equiv =
        make_box(jp2_placeholder_4cc,
                 make_phld_body(2, 11, 44, jp2_data_entry_url_4cc, true, 3, 1,
                                jp2_data_entry_url_4cc, true,
                                8 + url_box.size() + 8, false, 0, false, 0));
    std::vector<kdu_byte> ph_csrange = make_box(
        jp2_placeholder_4cc,
        make_phld_body(6 | 8, 12, 48, jp2_codestream_4cc, true, 0, 48,
                       jp2_codestream_4cc, false, 0, true, 0, true, 3));

    std::vector<kdu_byte> bin0;
    append_vec(bin0, uuid_box);
    size_t off_no_acc = bin0.size();
    append_vec(bin0, ph_no_acc);
    size_t off_orig = bin0.size();
    append_vec(bin0, ph_orig);
    size_t off_64 = bin0.size();
    append_vec(bin0, ph_64equiv);
    size_t off_cs = bin0.size();
    append_vec(bin0, ph_csrange);

    MemSource ms2(bin0);
    kdu_supp::jp2_family_src fs2;
    fs2.open(&ms2);
    kdu_supp::jp2_input_box box;

    kdu_supp::jp2_locator loc;
    loc.set_file_pos(static_cast<kdu_long>(off_no_acc));
    EXPECT_THROW(box.open(&fs2, loc), kdu_core::kdu_exception);

    loc.set_file_pos(static_cast<kdu_long>(off_orig));
    EXPECT_TRUE(box.open(&fs2, loc));
    EXPECT_EQ(box.get_box_type(), jp2_uuid_4cc);
    box.close();

    loc.set_file_pos(static_cast<kdu_long>(off_64));
    EXPECT_TRUE(box.open_as(jp2_data_entry_url_4cc, &fs2, loc, loc, 4));
    EXPECT_TRUE(box.is_complete());
    box.close();

    loc.set_file_pos(static_cast<kdu_long>(off_cs));
    EXPECT_TRUE(box.open(&fs2, loc));
    EXPECT_EQ(box.get_codestream_scope(), 0);
    EXPECT_TRUE(box.set_codestream_scope(1, false));
    EXPECT_FALSE(box.set_codestream_scope(99, false));
    EXPECT_FALSE(box.set_tileheader_scope(0, 1));
    EXPECT_FALSE(box.set_precinct_scope(0));
    box.close();
    fs2.close();
  }

  // 2. Malformed phld boxes inside MemSource.
  {
    auto expect_phld_error = [&](const std::vector<kdu_byte>& body,
                                 bool expect_false_instead) {
      std::vector<kdu_byte> raw = make_box(jp2_placeholder_4cc, body);
      if (expect_false_instead) {
        // Declare large LBox (>= 28) while truncating underlying bytes.
        raw[0] = 0;
        raw[1] = 0;
        raw[2] = 0;
        raw[3] = 64;
      }
      MemSource ms(raw);
      kdu_supp::jp2_family_src fsrc;
      fsrc.open(&ms);
      kdu_supp::jp2_input_box b;
      if (expect_false_instead) {
        EXPECT_FALSE(b.open(&fsrc));
      } else {
        EXPECT_THROW(b.open(&fsrc), kdu_core::kdu_exception);
      }
      b.close();
      fsrc.close();
    };
    expect_phld_error({0, 0, 0, 1}, false);  // LBox < 28
    expect_phld_error({0, 0, 0, 1}, true);   // Truncated < 20 bytes
    expect_phld_error(make_phld_body(2, 1, 28, jp2_uuid_4cc, false, 0, 0, 0,
                                     false, 0, false, 0, false, 0),
                      true);  // equiv < 16
    {
      // 64-bit negative orig len (orig_len == 1 followed by
      // 0x8000000000000001).
      std::vector<kdu_byte> neg_orig;
      AppendBe32(neg_orig, 2);
      AppendBe64(neg_orig, 1);
      AppendBe32(neg_orig, 1);
      AppendBe32(neg_orig, jp2_uuid_4cc);
      AppendBe64(neg_orig, 0x8000000000000001ULL);
      expect_phld_error(neg_orig, false);
    }
    expect_phld_error(
        make_phld_body(2, 1, 28, jp2_uuid_4cc, true, 1, 1, jp2_uuid_4cc, true,
                       0x8000000000000001ULL, false, 0, false, 0),
        false);  // negative 64-bit equiv len
    expect_phld_error(
        make_phld_body(2, 1, 28, jp2_uuid_4cc, true, 1, 4, jp2_uuid_4cc, false,
                       0, false, 0, false, 0),
        false);  // equiv len < 8
    expect_phld_error(
        make_phld_body(6, 1, 28, jp2_uuid_4cc, true, 1, 16, jp2_uuid_4cc, false,
                       0, false, 0, false, 0),
        true);  // cs < 8 bytes
    expect_phld_error(
        make_phld_body(6, 1, 28, jp2_uuid_4cc, true, 1, 16, jp2_uuid_4cc, false,
                       0, true, 0x8000000000000000ULL, false, 0),
        false);  // negative cs_min
    expect_phld_error(make_phld_body(6 | 8, 1, 28, jp2_uuid_4cc, true, 1, 16,
                                     jp2_uuid_4cc, false, 0, true, 0, false, 0),
                      true);  // range < 4 bytes
    expect_phld_error(
        make_phld_body(6 | 8, 1, 28, jp2_uuid_4cc, true, 1, 16, jp2_uuid_4cc,
                       false, 0, true, 0x7FFFFFFFFFFFFFFFULL - 2, true, 10),
        false);  // cs_min + offset overflow
  }

  // 3. kdu_membroker limits, mem_config on jp2_family_src / j2_memsafe.
  {
    std::vector<kdu_byte> big_box =
        make_box(jp2_uuid_4cc, std::vector<kdu_byte>(400, 0x55));
    MemSource ms(big_box);
    kdu_core::kdu_membroker broker(100);
    kdu_supp::jp2_family_src fs;
    fs.open(&ms, &broker);

    EXPECT_THROW(fs.set_access_permissions(std::string(600, 'a').c_str()),
                 kdu_core::kdu_exception);

    kdu_supp::jp2_input_box box;
    EXPECT_TRUE(box.open(&fs));
    EXPECT_FALSE(box.load_in_memory(1024));  // Exceeds 100B broker limit
    box.close();
    fs.close();

    kdu_core::kdu_membroker broker2(1000);
    ms.seek(0);
    fs.open(&ms, &broker2);
    EXPECT_TRUE(box.open(&fs));
    EXPECT_TRUE(box.load_in_memory(1024));  // Allocates 404B (prefix=4)
    fs.close(true);  // Partial close preserves alloc_bytes
    kdu_core::kdu_membroker tight(10);
    ms.seek(0);
    EXPECT_THROW(fs.open(&ms, &tight), kdu_core::kdu_exception);
    box.close();
    fs.close();

    // j2_memsafe overlimit growth (succeeds via parent) and failure.
    kdu_core::kdu_membroker parent(1 << 16);
    kdu_core::kdu_membroker child;
    child.attach_to_parent(&parent, 32);
    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt, &child);
    kdu_supp::jp2_target jt;
    jt.open(&ft);
    jt.access_palette().init(
        3, 64);  // Exceeds 32B private limit -> borrows parent
    jt.close();
    ft.close();

    kdu_core::kdu_membroker hard_limit(16);
    MemTarget mt2;
    kdu_supp::jp2_family_tgt ft2;
    ft2.open(&mt2, &hard_limit);
    kdu_supp::jp2_target jt2;
    EXPECT_THROW(jt2.open(&ft2), kdu_core::kdu_exception);
    jt2.close();
    ft2.close();
  }
}

TEST(Jp2Test, BoxesChannelsColourPaletteDrefAndHeaderErrors) {
  ScopedMessageHandlers handlers;

  auto make_box = [](kdu_uint32 type, const std::vector<kdu_byte>& body) {
    std::vector<kdu_byte> out;
    AppendBe32(out, static_cast<kdu_uint32>(8 + body.size()));
    AppendBe32(out, type);
    out.insert(out.end(), body.begin(), body.end());
    return out;
  };
  auto append_vec = [](std::vector<kdu_byte>& dst,
                       const std::vector<kdu_byte>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
  };

  // 1. jp2_output_box: buffer growth (tgt & standalone), write_box, nested
  //    set_rubber_length + rewrite, simulated target, and failed rewrite
  //    target.
  {
    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt);
    {
      kdu_supp::jp2_output_box b;
      b.open(&ft, jp2_uuid_4cc);
      std::vector<kdu_byte> chunk1(32, 0xAA);
      std::vector<kdu_byte> chunk2(2048, 0xBB);
      b.write(chunk1.data(), static_cast<int>(chunk1.size()));
      b.write(chunk2.data(), static_cast<int>(chunk2.size()));
      b.write_box(&ft, false);
      b.write_box(&ft, true);
      b.close();

      // Standalone box (tgt == NULL) buffer growth + use_long_header() ->
      // close.
      kdu_supp::jp2_output_box standalone;
      standalone.open(jp2_uuid_4cc);
      standalone.write(chunk1.data(), static_cast<int>(chunk1.size()));
      standalone.write(chunk2.data(), static_cast<int>(chunk2.size()));
      standalone.use_long_header();
      standalone.set_rubber_length();
      standalone.write_box(&ft, false);
      mt.set_fail_writes(true);
      EXPECT_EQ(standalone.write_box(&ft, false), -1);
      mt.set_fail_writes(false);
      standalone.close();

      {
        MemTarget mt_rub;
        kdu_supp::jp2_family_tgt ft_rub;
        ft_rub.open(&mt_rub);
        kdu_supp::jp2_output_box rubber_long_err;
        rubber_long_err.open(&ft_rub, jp2_uuid_4cc);
        rubber_long_err.set_rubber_length();
        EXPECT_THROW(rubber_long_err.use_long_header(),
                     kdu_core::kdu_exception);
        rubber_long_err.close();
        ft_rub.close();
      }

      kdu_supp::jp2_output_box long_free;
      long_free.open(&ft, jp2_uuid_4cc);
      long_free.use_long_header();
      long_free.write(static_cast<kdu_uint32>(1));
      long_free.write_free_and_close(16);
    }

    // Nested superbox set_rubber_length (flushes child buffer) & rewrite.
    {
      kdu_supp::jp2_output_box sup, sub;
      sup.open(&ft, jp2_header_4cc);
      sup.set_rubber_length();
      sub.open(&sup, jp2_uuid_4cc);
      std::vector<kdu_byte> pad(32, 0xCC);
      sub.write(pad.data(), static_cast<int>(pad.size()));
      sub.set_rubber_length();
      EXPECT_TRUE(sub.start_rewrite(8));
      sub.write(pad.data(), 8);
      EXPECT_TRUE(sub.end_rewrite());
      sub.close();
      sup.close();
    }
    ft.close();

    // Simulated jp2_family_tgt + set_target_size + start_rewrite +
    // write_header_last.
    kdu_supp::jp2_family_tgt ft_sim;
    ft_sim.open(static_cast<kdu_long>(0));
    kdu_supp::jp2_output_box bsim;
    bsim.open(&ft_sim, jp2_uuid_4cc);
    bsim.set_target_size(32);
    bsim.write(static_cast<kdu_uint32>(0x12345678));
    std::vector<kdu_byte> pad28(28, 0);
    bsim.write(pad28.data(), 28);
    EXPECT_TRUE(bsim.start_rewrite(4));
    bsim.write(static_cast<kdu_uint32>(0x87654321));
    EXPECT_TRUE(bsim.end_rewrite());
    bsim.close();
    kdu_supp::jp2_output_box bsim2;
    bsim2.open(&ft_sim, jp2_uuid_4cc);
    bsim2.write_header_last();
    bsim2.write(static_cast<kdu_uint32>(0x11223344));
    bsim2.close();
    ft_sim.close();

    // Non-rewritable indirect target -> start_rewrite returns false cleanly.
    class NoRewriteTarget : public MemTarget {
     public:
      bool start_rewrite(kdu_core::kdu_long) override { return false; }
    };
    NoRewriteTarget nrt;
    kdu_supp::jp2_family_tgt ft_nr;
    ft_nr.open(&nrt);
    kdu_supp::jp2_output_box bnr;
    bnr.open(&ft_nr, jp2_uuid_4cc);
    bnr.set_rubber_length();
    bnr.write(static_cast<kdu_uint32>(1));
    EXPECT_FALSE(bnr.start_rewrite(4));
    bnr.close();
    ft_nr.close();
  }

  // 2. jp2_channels: SPLIT_EXP non-colour mappings, invalid init/format,
  //    get_codestreams_used overflow insertion, malformed
  //    cdef/cmap/pxfm/opct/res.
  {
    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt);
    kdu_supp::jpx_target xt;
    xt.open(&ft);

    // Build 4 codestreams so get_codestreams_used exercises insertion bump.
    std::vector<kdu_supp::jpx_codestream_target> cts;
    for (int i = 0; i < 4; ++i) {
      kdu_supp::jpx_codestream_target ct = xt.add_codestream();
      kdu_core::siz_params siz;
      siz.set(Sdims, 0, 0, 4);
      siz.set(Sdims, 0, 1, 4);
      siz.set(Scomponents, 0, 0, 2);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, false);
      kdu_core::kdu_params* root = &siz;
      root->finalize_all();
      ct.access_dimensions().init(&siz);
      ct.access_dimensions().finalize_compatibility(&siz);
      cts.push_back(ct);
    }

    // Layer 0 (RGB ICC): maps all 4 codestreams, FIXPOINT & FLOAT on
    // non-colour 0/1.
    {
      kdu_supp::jpx_layer_target lt = xt.add_layer();
      kdu_supp::jp2_channels ch = lt.access_channels();
      ch.init(3, 2);
      EXPECT_THROW(ch.init(1, 0), kdu_core::kdu_exception);  // Double init
      ch.set_colour_mapping(0, 0, -1, 0);
      ch.set_colour_mapping(1, 0, -1, 1);
      ch.set_colour_mapping(2, 0, -1, 2);
      EXPECT_THROW(ch.set_non_colour_mapping(0, 1, 0, -1, 0,
                                             JP2_CHANNEL_FORMAT_FLOAT, nullptr),
                   kdu_core::kdu_exception);
      int fp_bits = 4;
      EXPECT_TRUE(ch.set_non_colour_mapping(
          0, 1, 0, -1, 3, JP2_CHANNEL_FORMAT_FIXPOINT, &fp_bits));
      int float_bits = 3;
      EXPECT_TRUE(ch.set_non_colour_mapping(
          1, 2, 1, -1, 0, JP2_CHANNEL_FORMAT_FLOAT, &float_bits));
      EXPECT_FALSE(ch.set_non_colour_mapping(-1, 1, 0, -1, 0));
      EXPECT_FALSE(ch.set_non_colour_mapping(5, 1, 0, -1, 0));
      kdu_uint16 asoc = 0;
      int cc = -1, lut = -1, cs = -1, fmt = -1, got_p[3] = {};
      EXPECT_TRUE(ch.get_non_colour_mapping(0, asoc, cc, lut, cs, fmt, got_p));
      EXPECT_EQ(fmt, JP2_CHANNEL_FORMAT_FIXPOINT);
      EXPECT_EQ(got_p[0], 4);
      EXPECT_TRUE(ch.get_non_colour_mapping(1, asoc, cc, lut, cs, fmt, got_p));
      EXPECT_EQ(fmt, JP2_CHANNEL_FORMAT_FLOAT);
      EXPECT_EQ(got_p[0], 3);
      EXPECT_FALSE(ch.get_non_colour_mapping(9, asoc, cc, lut, cs, fmt, got_p));

      double red[2] = {0.64, 0.33};
      double green[2] = {0.30, 0.60};
      double blue[2] = {0.15, 0.06};
      kdu_supp::jp2_colour col = lt.add_colour();
      EXPECT_THROW(col.init(red, green, blue, 0.5, 0.1, 8, false),
                   kdu_core::kdu_exception);
      col.init(red, green, blue, 2.2, 0.05, 8, true);
      EXPECT_THROW(col.init(red, green, blue, 2.2, 0.05, 8, true),
                   kdu_core::kdu_exception);
    }

    // Separate small JPX file exercising Monochrome ICC ctor + SPLIT_EXP +
    // readback.
    {
      MemTarget mt_mono;
      kdu_supp::jp2_family_tgt ft_mono;
      ft_mono.open(&mt_mono);
      kdu_supp::jpx_target xt_mono;
      xt_mono.open(&ft_mono);
      kdu_supp::jpx_codestream_target ct_m = xt_mono.add_codestream();
      ct_m.access_dimensions().init(kdu_coords(4, 4), 2);
      for (int c = 0; c < 2; ++c) {
        ct_m.access_dimensions().set_precision(c, 8, false);
      }
      kdu_supp::jpx_layer_target lt_m = xt_mono.add_layer();
      lt_m.access_channels().init(1);
      int split_params[3] = {1, -1, 0};
      lt_m.access_channels().set_colour_mapping(
          0, 0, -1, 0, JP2_CHANNEL_FORMAT_SPLIT_EXP, split_params);
      int got_cc = -1, got_lut = -1, got_cs = -1, got_fmt = -1, got_p[3] = {};
      EXPECT_TRUE(lt_m.access_channels().get_colour_mapping(
          0, got_cc, got_lut, got_cs, got_fmt, got_p));
      EXPECT_EQ(got_fmt, JP2_CHANNEL_FORMAT_SPLIT_EXP);
      EXPECT_EQ(got_p[0], 1);
      kdu_supp::jp2_colour col_m = lt_m.add_colour();
      EXPECT_THROW(col_m.init(0.5, 0.1, 8), kdu_core::kdu_exception);
      col_m.init(1.0, 0.0, 8);
      EXPECT_THROW(col_m.init(2.2, 0.05, 8), kdu_core::kdu_exception);
      xt_mono.write_headers();
      auto* sm = ct_m.open_stream();
      CompressMinimalCodestream(*sm, 4, 4, 2);
      sm->close();
      xt_mono.close();
      ft_mono.close();
      MemSource ms_m(mt_mono.data());
      kdu_supp::jp2_family_src fs_m;
      fs_m.open(&ms_m);
      kdu_supp::jpx_source xs_m;
      EXPECT_TRUE(xs_m.open(&fs_m, true));
      xs_m.close();
      fs_m.close();
    }

    // Also add two layers sharing CIELab params to exercise j2_colour::compare
    // in a dedicated JPX target so Layer 0 of `xt' matches `default_channels'.
    {
      MemTarget mt_lab;
      kdu_supp::jp2_family_tgt ft_lab;
      ft_lab.open(&mt_lab);
      kdu_supp::jpx_target xt_lab;
      xt_lab.open(&ft_lab);
      kdu_supp::jpx_codestream_target ct_lab = xt_lab.add_codestream();
      ct_lab.access_dimensions().init(kdu_coords(4, 4), 3);
      for (int c = 0; c < 3; ++c) {
        ct_lab.access_dimensions().set_precision(c, 8, false);
      }
      auto add_lab_layer = [&](int Lbits) {
        kdu_supp::jpx_layer_target lt = xt_lab.add_layer();
        lt.access_channels().init(3);
        for (int c = 0; c < 3; ++c) {
          lt.access_channels().set_colour_mapping(c, c, -1, 0);
        }
        lt.add_colour().init(kdu_supp::JP2_CIELab_SPACE, 255, 0, Lbits, 255,
                             128, 8, 255, 128, 8, JP2_CIE_D65);
      };
      add_lab_layer(8);
      add_lab_layer(8);
      xt_lab.write_headers();
      auto* s_lab = ct_lab.open_stream();
      CompressMinimalCodestream(*s_lab, 4, 4, 3);
      s_lab->close();
      xt_lab.close();
      ft_lab.close();
    }

    // Data references -> dtbl box readback by jpx_source.
    kdu_supp::jp2_data_references xdref = xt.access_data_references();
    xdref.add_url("http://example.com/a.jp2");
    xdref.add_file_url("b.jp2", 2);
    xt.write_headers();
    for (size_t i = 0; i < cts.size(); ++i) {
      auto* s = cts[i].open_stream();
      CompressMinimalCodestream(*s, 4, 4, 2);
      s->close();
    }
    xt.close();
    ft.close();

    // Parse back via jpx_source -> hits j2_data_references::init, parse_cdef,
    // parse_pxfm, finalize, find_cmap_channels.
    MemSource ms(mt.data());
    kdu_supp::jp2_family_src fs;
    fs.open(&ms);
    fs.set_access_permissions("./**:/**");
    kdu_supp::jpx_source xs;
    try {
      EXPECT_TRUE(xs.open(&fs, true));
      EXPECT_EQ(xs.access_data_references().get_num_urls(), 2);
      int min_cs = 0, max_cs = 0;
      int cs_used[1] = {-1};
      int used = xs.access_layer(0).access_channels().get_codestreams_used(
          min_cs, max_cs, cs_used, 1);
      EXPECT_GE(used, 1);
    } catch (kdu_core::kdu_exception) {
      ADD_FAILURE() << "Unexpected jpx_source error: " << handlers.errors();
    }
    xs.close();
    fs.close();
  }

  // 3. Malformed cmap / pxfm / cdef / opct / res / dtbl boxes via JPX/JP2
  // headers.
  {
    auto try_jp2_with_jp2h = [&](const std::vector<kdu_byte>& extra_subboxes) {
      std::vector<kdu_byte> sig =
          make_box(jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
      std::vector<kdu_byte> ftyp =
          make_box(jp2_file_type_4cc,
                   {'j', 'p', '2', ' ', 0, 0, 0, 0, 'j', 'p', '2', ' '});
      std::vector<kdu_byte> ihdr = make_box(
          jp2_image_header_4cc, {0, 0, 0, 4, 0, 0, 0, 4, 0, 1, 7, 7, 0, 0});
      std::vector<kdu_byte> colr =
          make_box(jp2_colour_4cc, {1, 0, 0, 0, 0, 0, 17});
      std::vector<kdu_byte> jp2h_body;
      append_vec(jp2h_body, ihdr);
      append_vec(jp2h_body, colr);
      append_vec(jp2h_body, extra_subboxes);
      std::vector<kdu_byte> jp2h = make_box(jp2_header_4cc, jp2h_body);
      std::vector<kdu_byte> raw;
      append_vec(raw, sig);
      append_vec(raw, ftyp);
      append_vec(raw, jp2h);
      MemSource ms(raw);
      kdu_supp::jp2_family_src fs;
      fs.open(&ms);
      kdu_supp::jp2_source js;
      EXPECT_TRUE(js.open(&fs));
      EXPECT_THROW(js.read_header(), kdu_core::kdu_exception);
      js.close();
      fs.close();
    };
    try_jp2_with_jp2h(make_box(jp2_component_mapping_4cc, {}));
    try_jp2_with_jp2h(
        make_box(jp2_component_mapping_4cc, {0, 0, 2, 0}));  // mtyp=2
    try_jp2_with_jp2h(
        make_box(jp2_channel_definition_4cc, {0, 1, 0, 0, 0, 0, 0, 1, 99}));
    try_jp2_with_jp2h(
        make_box(jp2_channel_definition_4cc, {0, 1, 0, 0, 0, 1, 0, 1}));
    {
      std::vector<kdu_byte> res = make_box(jp2_resolution_4cc, {});
      try_jp2_with_jp2h(res);  // Empty res box
      std::vector<kdu_byte> two_res = make_box(
          jp2_resolution_4cc,
          make_box(jp2_capture_resolution_4cc, {0, 1, 0, 1, 0, 1, 0, 1, 0, 0}));
      append_vec(two_res, two_res);
      try_jp2_with_jp2h(two_res);  // Duplicate res box
    }
    // Malformed dtbl in JPX: truncated, non-url subbox, extra trailing bytes.
    auto try_jpx_dtbl = [&](const std::vector<kdu_byte>& dtbl_body) {
      std::vector<kdu_byte> sig =
          make_box(jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
      std::vector<kdu_byte> ftyp =
          make_box(jp2_file_type_4cc,
                   {'j', 'p', 'x', ' ', 0, 0, 0, 0, 'j', 'p', 'x', ' '});
      std::vector<kdu_byte> dtbl = make_box(jp2_dtbl_4cc, dtbl_body);
      std::vector<kdu_byte> raw;
      append_vec(raw, sig);
      append_vec(raw, ftyp);
      append_vec(raw, dtbl);
      MemSource ms(raw);
      kdu_supp::jp2_family_src fs;
      fs.open(&ms);
      kdu_supp::jpx_source xs;
      EXPECT_THROW(
          {
            xs.open(&fs, true);
            xs.access_layer(0);
          },
          kdu_core::kdu_exception);
      xs.close();
      fs.close();
    };
    try_jpx_dtbl({0});
    try_jpx_dtbl({0, 1, 0, 0, 0, 8, 'u', 'u', 'i', 'd'});
    {
      std::vector<kdu_byte> b = {0, 1};
      append_vec(b, make_box(jp2_data_entry_url_4cc, {0, 0, 0, 0, 0}));
      b.push_back(0xFF);
      try_jpx_dtbl(b);
    }

    // Malformed pxfm & opct in JPX jpxl->cdef/opct/pxfm.
    auto try_jpx_layer_sub = [&](const std::vector<kdu_byte>& jpxh_subs) {
      std::vector<kdu_byte> sig =
          make_box(jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
      std::vector<kdu_byte> ftyp =
          make_box(jp2_file_type_4cc,
                   {'j', 'p', 'x', ' ', 0, 0, 0, 0, 'j', 'p', 'x', ' '});
      std::vector<kdu_byte> ihdr = make_box(
          jp2_image_header_4cc, {0, 0, 0, 4, 0, 0, 0, 4, 0, 1, 7, 7, 0, 0});
      std::vector<kdu_byte> jp2h = make_box(jp2_header_4cc, ihdr);
      std::vector<kdu_byte> jplh_body;
      append_vec(jplh_body, jpxh_subs);
      std::vector<kdu_byte> jplh =
          make_box(static_cast<kdu_uint32>(0x6a706c68), jplh_body);  // 'jplh'
      std::vector<kdu_byte> raw;
      append_vec(raw, sig);
      append_vec(raw, ftyp);
      append_vec(raw, jp2h);
      append_vec(raw, jplh);
      MemSource ms(raw);
      kdu_supp::jp2_family_src fs;
      fs.open(&ms);
      kdu_supp::jpx_source xs;
      EXPECT_THROW(
          {
            xs.open(&fs, true);
            xs.access_layer(0);
          },
          kdu_core::kdu_exception);
      xs.close();
      fs.close();
    };
    try_jpx_layer_sub(make_box(jp2_pixel_format_4cc, {0, 0}));
    try_jpx_layer_sub(make_box(jp2_pixel_format_4cc, {0, 1, 0, 0, 0x50, 0}));
    try_jpx_layer_sub(make_box(jp2_opacity_4cc, {3}));
    try_jpx_layer_sub(make_box(jp2_opacity_4cc, {2}));
    try_jpx_layer_sub(make_box(jp2_opacity_4cc, {0, 99}));
  }

  // 4. jp2_target::write_header JPH & error paths, finalize_compatibility MCT.
  {
    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt);
    kdu_supp::jp2_target jt_unopen;
    EXPECT_THROW(jt_unopen.write_header(), kdu_core::kdu_exception);

    kdu_supp::jp2_output_box b;
    b.open(&ft, jp2_uuid_4cc);
    b.write(static_cast<kdu_uint32>(1));
    b.close();
    kdu_supp::jp2_target jt_nonempty;
    EXPECT_THROW(jt_nonempty.open(&ft), kdu_core::kdu_exception);
    jt_nonempty.close();
    ft.close();

    // Valid JPH file (HT cap -> jph_brand).
    MemTarget mt_jph, cs_jph_mt;
    ft.open(&mt_jph);
    kdu_supp::jp2_target jt_jph;
    jt_jph.open(&ft);
    kdu_core::siz_params siz;
    siz.set(Sdims, 0, 0, 8);
    siz.set(Sdims, 0, 1, 8);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    static_cast<kdu_core::kdu_params&>(siz).finalize();
    {
      kdu_core::kdu_codestream cs;
      cs.create(&siz, &cs_jph_mt);
      cs.access_siz()->parse_string("Cmodes=HT");
      cs.access_siz()->finalize_all();
      jt_jph.access_dimensions().init(cs.access_siz());
      jt_jph.access_dimensions().finalize_compatibility(cs.access_siz());
      cs.destroy();
    }
    jt_jph.access_colour().init(kdu_supp::JP2_sLUM_SPACE);
    jt_jph.write_header();
    EXPECT_EQ(jt_jph.get_brand(), jph_brand);
    EXPECT_THROW(jt_jph.write_header(), kdu_core::kdu_exception);  // Twice
    jt_jph.close();
    ft.close();

    // Incompatible Part-2 MCT -> write_header throws, finalize_compatibility
    // runs.
    MemTarget mt_mct, cs_mct_mt;
    ft.open(&mt_mct);
    kdu_supp::jp2_target jt_mct;
    jt_mct.open(&ft);
    kdu_core::siz_params siz2;
    siz2.set(Sdims, 0, 0, 8);
    siz2.set(Sdims, 0, 1, 8);
    siz2.set(Scomponents, 0, 0, 3);
    for (int c = 0; c < 3; ++c) {
      siz2.set(Sprecision, c, 0, 8);
      siz2.set(Ssigned, c, 0, false);
    }
    static_cast<kdu_core::kdu_params&>(siz2).finalize();
    {
      kdu_core::kdu_codestream cs2;
      cs2.create(&siz2, &cs_mct_mt);
      cs2.access_siz()->parse_string("Mcomponents=3");
      cs2.access_siz()->parse_string("Mmatrix_size:I1=9");
      cs2.access_siz()->parse_string("Mmatrix_coeffs:I1=1,0,0,0,1,0,0,0,1");
      cs2.access_siz()->parse_string("Mstage_inputs:I1={0,2}");
      cs2.access_siz()->parse_string("Mstage_outputs:I1={0,2}");
      cs2.access_siz()->parse_string("Mstage_collections:I1={3,3}");
      cs2.access_siz()->parse_string("Mstage_xforms:I1={MATRIX,1,0,0,0}");
      cs2.access_siz()->parse_string("Mnum_stages=1");
      cs2.access_siz()->parse_string("Mstages=1");
      cs2.access_siz()->finalize_all();
      jt_mct.access_dimensions().init(cs2.access_siz());
      jt_mct.access_dimensions().finalize_compatibility(cs2.access_siz());
      cs2.destroy();
    }
    jt_mct.access_colour().init(kdu_supp::JP2_sRGB_SPACE);
    EXPECT_THROW(jt_mct.write_header(), kdu_core::kdu_exception);
    jt_mct.close();
    ft.close();
  }
}

TEST(Jp2Test, IoStreamingSequentialErrorsAndReopen) {
  ScopedMessageHandlers handlers;

  auto make_box = [](kdu_uint32 type, const std::vector<kdu_byte>& body,
                     bool rubber = false) {
    std::vector<kdu_byte> out;
    AppendBe32(out, rubber ? 0U : static_cast<kdu_uint32>(8 + body.size()));
    AppendBe32(out, type);
    out.insert(out.end(), body.begin(), body.end());
    return out;
  };

  // 1. Non-seekable MemSource + rubber-length EOF + open_next + open_as.
  std::vector<kdu_byte> seq_buf;
  {
    auto b1 = make_box(jp2_uuid_4cc, std::vector<kdu_byte>(24, 0x11));
    std::vector<kdu_byte> sub1 =
        make_box(jp2_image_header_4cc, std::vector<kdu_byte>(14, 0x22));
    std::vector<kdu_byte> sub2 =
        make_box(jp2_colour_4cc, {1, 0, 0, 0, 0, 0, 16});
    std::vector<kdu_byte> super_body;
    super_body.insert(super_body.end(), sub1.begin(), sub1.end());
    super_body.insert(super_body.end(), sub2.begin(), sub2.end());
    auto b2 = make_box(jp2_header_4cc, super_body);
    auto b3 = make_box(jp2_uuid_4cc, std::vector<kdu_byte>(40, 0x33),
                       /*rubber=*/true);
    seq_buf.insert(seq_buf.end(), b1.begin(), b1.end());
    seq_buf.insert(seq_buf.end(), b2.begin(), b2.end());
    seq_buf.insert(seq_buf.end(), b3.begin(), b3.end());
  }

  {
    MemSource ms_seq(seq_buf, /*can_seek=*/false);
    kdu_supp::jp2_family_src fs;
    fs.open(&ms_seq);
    kdu_supp::jp2_input_box box, sub;
    EXPECT_THROW(box.open_next(), kdu_core::kdu_exception);  // Not open yet
    EXPECT_EQ(box.get_pos(), 0);
    EXPECT_TRUE(box.open(&fs));
    EXPECT_FALSE(box.seek(0));
    kdu_byte tmp[128];
    EXPECT_EQ(box.read(tmp, 4), 4);
    EXPECT_EQ(box.get_pos(), 4);
    // Double open / open_as errors while open.
    EXPECT_THROW(box.open(&box), kdu_core::kdu_exception);
    EXPECT_THROW(
        box.open_as(jp2_uuid_4cc, &fs, jp2_locator(), jp2_locator(), 4),
        kdu_core::kdu_exception);
    box.close();  // Sequential skip remaining 20 bytes

    EXPECT_TRUE(box.open_next());  // jp2h superbox
    EXPECT_TRUE(sub.open(&box));
    sub.close();
    EXPECT_TRUE(sub.open_next());
    sub.close();
    EXPECT_FALSE(sub.open_next());  // Past end of superbox
    box.close();

    EXPECT_TRUE(box.open_next());       // Trailing rubber-length uuid box
    EXPECT_EQ(box.read(tmp, 100), 40);  // Hits EOF on rubber-length box
    EXPECT_EQ(box.read(tmp, 10), 0);
    EXPECT_FALSE(box.load_in_memory(1024));  // Non-seekable after read
    box.close();
    EXPECT_THROW(box.open_next(), kdu_core::kdu_exception);
    EXPECT_THROW(box.read(tmp, 1), kdu_core::kdu_exception);  // Closed read
    fs.close();
  }

  // 2. Seekable two-box interleaved read (hits indirect->seek on pos mismatch),
  //    open_as, and truncated box premature EOF error.
  {
    MemSource ms_seek(seq_buf, /*can_seek=*/true);
    kdu_supp::jp2_family_src fs;
    fs.open(&ms_seek);
    kdu_supp::jp2_input_box b1, b2;
    EXPECT_TRUE(b1.open(&fs));
    kdu_byte tmp[8];
    EXPECT_EQ(b1.read(tmp, 4), 4);
    jp2_locator loc2;
    loc2.set_file_pos(8 + 24);  // Start of jp2h
    EXPECT_TRUE(b2.open(&fs, loc2));
    EXPECT_EQ(b2.read(tmp, 4), 4);  // Moves src->last_read_pos
    EXPECT_TRUE(b1.seek(0));
    EXPECT_EQ(b1.read(tmp, 4), 4);  // Forces indirect->seek back to b1
    b1.close();
    b2.close();

    EXPECT_THROW(
        b1.open_as(jp2_uuid_4cc, nullptr, jp2_locator(), jp2_locator(), 4),
        kdu_core::kdu_exception);
    EXPECT_FALSE(b1.open_as(0, &fs, jp2_locator(), jp2_locator(), 4));
    jp2_locator box_loc, cont_loc;
    box_loc.set_file_pos(0);
    cont_loc.set_file_pos(8);
    EXPECT_TRUE(b1.open_as(jp2_uuid_4cc, &fs, box_loc, cont_loc, 10));
    EXPECT_EQ(b1.read(tmp, 4), 4);
    b1.close();
    fs.close();

    // Box claiming 200 bytes in a 20-byte stream -> premature EOF error.
    std::vector<kdu_byte> trunc;
    AppendBe32(trunc, 200);
    AppendBe32(trunc, jp2_uuid_4cc);
    trunc.insert(trunc.end(), 12, 0x55);
    MemSource ms_trunc(trunc, /*can_seek=*/true);
    fs.open(&ms_trunc);
    EXPECT_TRUE(b1.open(&fs));
    kdu_byte buf[64];
    EXPECT_EQ(b1.read(buf, 64), 12);
    EXPECT_EQ(b1.read(buf, 64), 0);
    b1.close();
    fs.close();
  }

  // 3. Incremental TrickleSource into jp2_source::read_header() + incompatible
  //    brand read_header(true) == -2 / read_header(false) throw.
  {
    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt);
    kdu_supp::jp2_target jt;
    jt.open(&ft);
    kdu_core::siz_params siz;
    siz.set(Sdims, 0, 0, 8);
    siz.set(Sdims, 0, 1, 8);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    static_cast<kdu_core::kdu_params&>(siz).finalize();
    jt.access_dimensions().init(&siz);
    kdu_int32 pal_vals[4] = {0, 80, 160, 255};
    jt.access_palette().init(3, 4);
    for (int c = 0; c < 3; ++c) {
      jt.access_palette().set_lut(c, pal_vals, 8, false);
    }
    jt.access_channels().init(3);
    for (int c = 0; c < 3; ++c) {
      jt.access_channels().set_colour_mapping(c, 0, c);
    }
    jt.access_colour().init(kdu_supp::JP2_sRGB_SPACE);
    jt.access_resolution().init(1.0f);
    jt.write_header();
    jt.open_codestream(true);
    CompressMinimalCodestream(jt, 8, 8, 1);
    jt.close();
    ft.close();

    MemSource ms_full(mt.data());
    kdu_supp::jp2_family_src fs;
    fs.open(&ms_full);
    kdu_supp::jp2_source js;
    EXPECT_TRUE(js.open(&fs));
    EXPECT_GT(js.read_header(), 0);
    js.close();
    fs.close();

    // Missing jp2c -> read_header(true) == -2 / read_header(false) throws.
    std::vector<kdu_byte> no_jp2c;
    AppendBox(no_jp2c, jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
    AppendBox(no_jp2c, jp2_uuid_4cc, {1, 2, 3, 4});
    MemSource ms_no_cs(no_jp2c);
    fs.open(&ms_no_cs);
    EXPECT_TRUE(js.open(&fs));
    EXPECT_EQ(js.read_header(true), -2);
    js.close();
    fs.close();
    MemSource ms_no_cs2(no_jp2c);
    fs.open(&ms_no_cs2);
    EXPECT_TRUE(js.open(&fs));
    EXPECT_THROW(js.read_header(false), kdu_core::kdu_exception);
    js.close();
    fs.close();

    // jp2c encountered without ftyp/jp2h -> read_header(true) == -2 / throw.
    std::vector<kdu_byte> early_jp2c = no_jp2c;
    AppendBox(early_jp2c, jp2_codestream_4cc, {0xFF, 0x4F});
    MemSource ms_early1(early_jp2c);
    fs.open(&ms_early1);
    EXPECT_TRUE(js.open(&fs));
    EXPECT_EQ(js.read_header(true), -2);
    js.close();
    fs.close();
    MemSource ms_early2(early_jp2c);
    fs.open(&ms_early2);
    EXPECT_TRUE(js.open(&fs));
    EXPECT_THROW(js.read_header(false), kdu_core::kdu_exception);
    js.close();
    fs.close();

    // Patch ftyp brand + compat list to incompatible 'xxxx'.
    std::vector<kdu_byte> bad_brand = mt.data();
    if (bad_brand.size() >= 32) {
      bad_brand[20] = 'x';
      bad_brand[21] = 'x';
      bad_brand[22] = 'x';
      bad_brand[23] = 'x';
      bad_brand[28] = 'x';
      bad_brand[29] = 'x';
      bad_brand[30] = 'x';
      bad_brand[31] = 'x';
    }
    MemSource ms_bad1(bad_brand);
    fs.open(&ms_bad1);
    EXPECT_TRUE(js.open(&fs));
    EXPECT_EQ(js.read_header(true), -3);
    js.close();
    fs.close();

    MemSource ms_bad2(bad_brand);
    fs.open(&ms_bad2);
    EXPECT_TRUE(js.open(&fs));
    EXPECT_THROW(js.read_header(false), kdu_core::kdu_exception);
    js.close();
    fs.close();
  }

  // 4. jp2_output_box standalone (tgt==NULL), set_target_size,
  // write_header_last,
  //    and reopen errors.
  {
    kdu_supp::jp2_output_box b_null;
    EXPECT_THROW(b_null.write_header_last(),
                 kdu_core::kdu_exception);  // Closed
    EXPECT_THROW(b_null.open(static_cast<kdu_supp::jp2_family_tgt*>(nullptr),
                             jp2_uuid_4cc, /*rubber_length=*/true),
                 kdu_core::kdu_exception);
    b_null.open(static_cast<kdu_supp::jp2_family_tgt*>(nullptr), jp2_uuid_4cc);
    b_null.set_target_size(16);
    b_null.write(static_cast<kdu_uint32>(7));
    kdu_supp::jp2_output_box b_sub;
    b_sub.open(&b_null, jp2_uuid_4cc);
    b_sub.write(static_cast<kdu_uint32>(9));
    b_sub.close();
    EXPECT_LT(b_null.reopen(jp2_uuid_4cc, 0), 0);
    b_null.close();

    class LocalNoRewriteTarget : public MemTarget {
     public:
      bool start_rewrite(kdu_core::kdu_long) override { return false; }
    };
    LocalNoRewriteTarget nrt;
    kdu_supp::jp2_family_tgt ft_nr;
    ft_nr.open(&nrt);
    kdu_supp::jp2_output_box b_nr;
    b_nr.open(&ft_nr, jp2_uuid_4cc);
    EXPECT_THROW(b_nr.write_header_last(), kdu_core::kdu_exception);
    b_nr.set_target_size(8);
    b_nr.write(static_cast<kdu_uint32>(1));
    b_nr.write(static_cast<kdu_uint32>(2));
    b_nr.close();
    EXPECT_EQ(b_nr.reopen(jp2_free_4cc, 0), 0);  // start_rewrite false
    EXPECT_LT(b_nr.reopen(jp2_uuid_4cc, 0), 0);
    ft_nr.close();

    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt);
    kdu_supp::jp2_output_box sup, child, child_err;
    sup.open(&ft, jp2_header_4cc);
    child.open(&sup, jp2_uuid_4cc);
    child.write(static_cast<kdu_uint32>(1));
    child.start_rewrite(4);
    EXPECT_THROW(child.set_target_size(16), kdu_core::kdu_exception);
    child.end_rewrite();
    child.close();
    child_err.open(&sup, jp2_uuid_4cc);
    child_err.write(static_cast<kdu_uint32>(2));
    EXPECT_THROW(child_err.set_target_size(2), kdu_core::kdu_exception);
    EXPECT_THROW(child_err.close(), kdu_core::kdu_exception);
    sup.close();
    EXPECT_GE(sup.reopen(jp2_header_4cc, 0), 0);
    EXPECT_LT(child.reopen(jp2_uuid_4cc, 0), 0);  // super_box is reopened
    sup.close();
    ft.close();
  }
}

TEST(Jp2Test, PaletteChannelsColourConverterAndDrefCoverage) {
  ScopedMessageHandlers handlers;

  // 1. jp2_palette bounds, 32-bit float/fix16 LUT conversions & downshift.
  {
    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt);
    kdu_supp::jp2_target jt;
    jt.open(&ft);
    kdu_supp::jp2_palette pal = jt.access_palette();
    EXPECT_THROW(pal.init(0, 4), kdu_core::kdu_exception);
    EXPECT_THROW(pal.init(256, 4), kdu_core::kdu_exception);
    EXPECT_THROW(pal.init(1, 0), kdu_core::kdu_exception);
    EXPECT_THROW(pal.init(1, 1025), kdu_core::kdu_exception);
    pal.init(3, 4);
    EXPECT_THROW(pal.init(1, 4), kdu_core::kdu_exception);  // Double init

    kdu_int32 u32_vals[4] = {0, 0x3F800000, 0x40000000, 0x7FFFFFFF};
    kdu_int32 s32_vals[4] = {std::numeric_limits<kdu_int32>::min(), -1000000, 0,
                             std::numeric_limits<kdu_int32>::max()};
    kdu_int32 b1_vals[4] = {0, 1, 0, 1};
    EXPECT_TRUE(pal.set_lut(0, u32_vals, 32, false));
    EXPECT_TRUE(pal.set_lut(1, s32_vals, 32, true));
    EXPECT_TRUE(pal.set_lut(2, b1_vals, 1, false));
    EXPECT_FALSE(pal.set_lut(9, b1_vals, 8, false));

    float flt[4] = {};
    kdu_sample16 fx[4] = {};
    int exp_small = 2;  // mantissa_bits = 29 > 23 -> mantissa_downshift = 6
    int exp_large = 40;
    EXPECT_TRUE(pal.get_lut(0, flt, JP2_CHANNEL_FORMAT_FLOAT, exp_small));
    EXPECT_TRUE(pal.get_lut(1, flt, JP2_CHANNEL_FORMAT_FLOAT, exp_large));
    EXPECT_TRUE(pal.get_lut(0, fx, JP2_CHANNEL_FORMAT_FLOAT, exp_small));
    EXPECT_TRUE(pal.get_lut(1, fx, JP2_CHANNEL_FORMAT_FLOAT, exp_small));
    EXPECT_TRUE(pal.get_lut(2, fx, JP2_CHANNEL_FORMAT_FLOAT, exp_large));
    EXPECT_FALSE(pal.get_lut(9, flt));
    EXPECT_FALSE(pal.get_lut(9, fx));
    EXPECT_THROW(pal.get_lut(0, flt, 999), kdu_core::kdu_exception);
    EXPECT_THROW(pal.get_lut(0, fx, 999), kdu_core::kdu_exception);
    jt.close();
    ft.close();
  }

  // 2. jp2_channels opct type 1 (premult), type 2 (signed chroma key),
  //    premult+opacity cdef + chroma warning, dedup pxfm, out-of-order cs,
  //    malformed cdef/opct/pxfm/pclr errors.
  {
    kdu_core::siz_params siz;
    siz.set(Sdims, 0, 0, 4);
    siz.set(Sdims, 0, 1, 4);
    siz.set(Scomponents, 0, 0, 4);
    for (int c = 0; c < 4; ++c) {
      siz.set(Sprecision, c, 0, 8);
      siz.set(Ssigned, c, 0, c == 0);
    }
    static_cast<kdu_core::kdu_params&>(siz).finalize();

    // Non-colour channels in JP2 -> write_header throws (L11499), plus invalid
    // premult/opacity formats on dedicated target.
    {
      MemTarget mt_err;
      kdu_supp::jp2_family_tgt ft_err;
      ft_err.open(&mt_err);
      kdu_supp::jp2_target jt_err;
      jt_err.open(&ft_err);
      jt_err.access_dimensions().init(&siz);
      auto ch_err = jt_err.access_channels();
      ch_err.init(3, 1);
      for (int c = 0; c < 3; ++c) ch_err.set_colour_mapping(c, c);
      ch_err.set_non_colour_mapping(0, 0, 3);
      jt_err.access_colour().init(kdu_supp::JP2_sRGB_SPACE);
      EXPECT_THROW(jt_err.write_header(), kdu_core::kdu_exception);
      EXPECT_THROW(ch_err.set_premult_mapping(
                       0, 3, -1, 0, JP2_CHANNEL_FORMAT_FLOAT, nullptr),
                   kdu_core::kdu_exception);
      EXPECT_THROW(ch_err.set_opacity_mapping(
                       0, 3, -1, 0, JP2_CHANNEL_FORMAT_FLOAT, nullptr),
                   kdu_core::kdu_exception);
      jt_err.close();
      ft_err.close();
    }

    // Premult opct type 1 write (layer 1 => avoid_opct=false) & readback.
    MemTarget mt_p;
    kdu_supp::jp2_family_tgt ft_p;
    ft_p.open(&mt_p);
    kdu_supp::jpx_target xt_p;
    xt_p.open(&ft_p);
    auto ct_p = xt_p.add_codestream();
    ct_p.access_dimensions().init(&siz);
    auto l0_p = xt_p.add_layer();
    l0_p.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l0_p.access_channels().set_colour_mapping(c, c);
    l0_p.add_colour().init(kdu_supp::JP2_sRGB_SPACE);

    auto l1_p = xt_p.add_layer();
    kdu_supp::jp2_channels ch_p = l1_p.access_channels();
    ch_p.init(3);
    for (int c = 0; c < 3; ++c) ch_p.set_colour_mapping(c, c);
    EXPECT_FALSE(ch_p.set_premult_mapping(9, 3));
    EXPECT_TRUE(ch_p.set_premult_mapping(-1, 3));
    EXPECT_TRUE(ch_p.set_premult_mapping(0, 3));
    EXPECT_TRUE(ch_p.set_premult_mapping(-1, 3));
    l1_p.add_colour().init(kdu_supp::JP2_sRGB_SPACE);
    xt_p.write_headers();
    auto* sp = ct_p.open_stream();
    CompressMinimalCodestream(*sp, 4, 4, 4);
    sp->close();
    xt_p.close();
    ft_p.close();

    MemSource ms_p(mt_p.data());
    kdu_supp::jp2_family_src fs_p;
    fs_p.open(&ms_p);
    kdu_supp::jpx_source xs_p;
    EXPECT_TRUE(xs_p.open(&fs_p, true));
    int pcc = -1, plut = -1, pcs = -1, pfmt = -1;
    EXPECT_TRUE(xs_p.access_layer(1).access_channels().get_premult_mapping(
        0, pcc, plut, pcs, pfmt));
    xs_p.close();
    fs_p.close();

    // Opacity + premult + chroma key warning + signed chroma key in JPX.
    MemTarget mt_op;
    kdu_supp::jp2_family_tgt ft_op;
    ft_op.open(&mt_op);
    kdu_supp::jpx_target xt_op;
    xt_op.open(&ft_op);
    kdu_supp::jpx_codestream_target ct_op = xt_op.add_codestream();
    ct_op.access_dimensions().init(kdu_coords(4, 4), 3);
    for (int c = 0; c < 3; ++c) {
      ct_op.access_dimensions().set_precision(c, 8, true);
    }
    auto lt_ck = xt_op.add_layer();
    lt_ck.access_channels().init(1);
    lt_ck.access_channels().set_colour_mapping(0, 0, -1, 0);
    lt_ck.access_channels().set_chroma_key(0, -10);
    lt_ck.add_colour().init(kdu_supp::JP2_sLUM_SPACE);

    auto lt_both = xt_op.add_layer();
    lt_both.access_channels().init(1);
    lt_both.access_channels().set_colour_mapping(0, 0, -1, 0);
    lt_both.access_channels().set_opacity_mapping(0, 1, -1, 0);
    lt_both.access_channels().set_premult_mapping(0, 2, -1, 0);
    lt_both.add_colour().init(kdu_supp::JP2_sLUM_SPACE);
    xt_op.write_headers();
    auto* sop = ct_op.open_stream();
    CompressMinimalCodestream(*sop, 4, 4, 3);
    sop->close();
    xt_op.close();
    ft_op.close();

    MemSource ms_op(mt_op.data());
    kdu_supp::jp2_family_src fs_op;
    fs_op.open(&ms_op);
    kdu_supp::jpx_source xs_op;
    EXPECT_TRUE(xs_op.open(&fs_op, true));
    xs_op.close();
    fs_op.close();

    // Out-of-order codestream mapping -> insertion sort shift in
    // get_codestreams_used + SPLIT_EXP get_non_colour_mapping + dedup pxfm.
    MemTarget mt_ord;
    kdu_supp::jp2_family_tgt ft_ord;
    ft_ord.open(&mt_ord);
    kdu_supp::jpx_target xt_ord;
    xt_ord.open(&ft_ord);
    std::vector<kdu_supp::jpx_codestream_target> cts;
    for (int i = 0; i < 4; ++i) {
      auto ct = xt_ord.add_codestream();
      ct.access_dimensions().init(kdu_coords(4, 4), 2);
      for (int c = 0; c < 2; ++c) {
        ct.access_dimensions().set_precision(c, 8, false);
      }
      cts.push_back(ct);
    }
    auto lt_ord = xt_ord.add_layer();
    auto ch_ord = lt_ord.access_channels();
    ch_ord.init(3, 1);
    ch_ord.set_colour_mapping(0, 0, -1, 3);
    ch_ord.set_colour_mapping(1, 0, -1, 1);
    ch_ord.set_colour_mapping(2, 0, -1, 2);
    int sp_p[3] = {1, -1, 0};
    EXPECT_TRUE(ch_ord.set_non_colour_mapping(
        0, 0, 0, -1, 0, JP2_CHANNEL_FORMAT_SPLIT_EXP, sp_p));
    kdu_uint16 asoc = 0;
    int cc = -1, lut = -1, cs = -1, fmt = -1, gp[3] = {};
    EXPECT_TRUE(ch_ord.get_non_colour_mapping(0, asoc, cc, lut, cs, fmt, gp));
    EXPECT_EQ(fmt, JP2_CHANNEL_FORMAT_SPLIT_EXP);
    EXPECT_FALSE(ch_ord.get_non_colour_mapping(5, asoc, cc, lut, cs, fmt, gp));
    int min_cs = 0, max_cs = 0, used_arr[16] = {};
    EXPECT_GE(ch_ord.get_codestreams_used(min_cs, max_cs, used_arr, 16), 4);
    EXPECT_EQ(used_arr[0], 3);
    lt_ord.add_colour().init(kdu_supp::JP2_sRGB_SPACE);
    xt_ord.write_headers();
    for (auto& ct : cts) {
      auto* s = ct.open_stream();
      CompressMinimalCodestream(*s, 4, 4, 2);
      s->close();
    }
    xt_ord.close();
    ft_ord.close();

    // pxfm write errors: int_bits > precision, exp_bits < 1.
    for (int err_case = 0; err_case < 2; ++err_case) {
      MemTarget mt_pe;
      kdu_supp::jp2_family_tgt ft_pe;
      ft_pe.open(&mt_pe);
      kdu_supp::jpx_target xt_pe;
      xt_pe.open(&ft_pe);
      auto ct_pe = xt_pe.add_codestream();
      ct_pe.access_dimensions().init(kdu_coords(4, 4), 1);
      ct_pe.access_dimensions().set_precision(0, 8, false);
      auto lt_pe = xt_pe.add_layer();
      lt_pe.access_channels().init(1);
      int param = (err_case == 0) ? 12 /*>8*/ : 8 /*mant>=8 -> exp_bits<1*/;
      lt_pe.access_channels().set_colour_mapping(
          0, 0, -1, 0,
          (err_case == 0) ? JP2_CHANNEL_FORMAT_FIXPOINT
                          : JP2_CHANNEL_FORMAT_FLOAT,
          &param);
      lt_pe.add_colour().init(kdu_supp::JP2_sLUM_SPACE);
      EXPECT_THROW(xt_pe.write_headers(), kdu_core::kdu_exception);
      xt_pe.close();
      ft_pe.close();
    }
  }

  // 3. Malformed JP2 headers (1-col colr vs 3-col cdef warning, bad cdef order,
  //    opct chroma length mismatch, pxfm index out of bounds).
  {
    auto try_bad_jp2h = [&](const std::vector<kdu_byte>& extra_jp2h) {
      MemTarget cs_mt;
      CompressMinimalCodestream(cs_mt, 4, 4, 3);
      std::vector<kdu_byte> f;
      AppendBox(f, jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
      std::vector<kdu_byte> ftyp;
      AppendBe32(ftyp, jp2_brand);
      AppendBe32(ftyp, 0);
      AppendBe32(ftyp, jp2_brand);
      AppendBox(f, jp2_file_type_4cc, ftyp);

      std::vector<kdu_byte> ihdr;
      AppendBe32(ihdr, 4);
      AppendBe32(ihdr, 4);
      AppendBe16(ihdr, 3);
      ihdr.push_back(7);
      ihdr.push_back(0);
      ihdr.push_back(0);
      ihdr.push_back(0);
      std::vector<kdu_byte> jp2h;
      AppendBox(jp2h, jp2_image_header_4cc, ihdr);
      jp2h.insert(jp2h.end(), extra_jp2h.begin(), extra_jp2h.end());
      AppendBox(f, jp2_header_4cc, jp2h);
      AppendBox(f, jp2_codestream_4cc, cs_mt.data());

      MemSource ms(f);
      kdu_supp::jp2_family_src fs;
      fs.open(&ms);
      kdu_supp::jp2_source js;
      EXPECT_TRUE(js.open(&fs));
      try {
        js.read_header();
      } catch (kdu_core::kdu_exception) {
      }
      js.close();
      fs.close();
    };

    // sLUM (1 colour) + 3-colour cdef -> warning L6508.
    std::vector<kdu_byte> b_lum_3cdef;
    std::vector<kdu_byte> colr_lum = {1, 0, 0};
    AppendBe32(colr_lum, 17);  // sLUM
    AppendBox(b_lum_3cdef, jp2_colour_4cc, colr_lum);
    std::vector<kdu_byte> cdef3;
    AppendBe16(cdef3, 3);
    for (int i = 0; i < 3; ++i) {
      AppendBe16(cdef3, static_cast<kdu_uint16>(i));
      AppendBe16(cdef3, 0);
      AppendBe16(cdef3, static_cast<kdu_uint16>(i + 1));
    }
    AppendBox(b_lum_3cdef, jp2_channel_definition_4cc, cdef3);
    try_bad_jp2h(b_lum_3cdef);

    // cdef with non-colour channel mapped to index < num_colour_entries.
    std::vector<kdu_byte> b_bad_cdef;
    std::vector<kdu_byte> colr_rgb = {1, 0, 0};
    AppendBe32(colr_rgb, 16);  // sRGB
    AppendBox(b_bad_cdef, jp2_colour_4cc, colr_rgb);
    std::vector<kdu_byte> cdef_nc;
    AppendBe16(cdef_nc, 4);
    for (int i = 0; i < 3; ++i) {
      AppendBe16(cdef_nc, static_cast<kdu_uint16>(i));
      AppendBe16(cdef_nc, 0);
      AppendBe16(cdef_nc, static_cast<kdu_uint16>(i + 1));
    }
    AppendBe16(cdef_nc, 3);
    AppendBe16(cdef_nc, 3);
    AppendBe16(cdef_nc, 1);
    AppendBox(b_bad_cdef, jp2_channel_definition_4cc, cdef_nc);
    // pxfm with too few descriptors (1 < idx 2).
    std::vector<kdu_byte> pxfm1;
    AppendBe16(pxfm1, 1);
    AppendBe16(pxfm1, 0);
    AppendBe16(pxfm1, 0);
    AppendBox(b_bad_cdef, jp2_pixel_format_4cc, pxfm1);
    try_bad_jp2h(b_bad_cdef);

    // opct chroma key length mismatch + monochrome 3-entry mismatch.
    std::vector<kdu_byte> b_opct;
    AppendBox(b_opct, jp2_colour_4cc, colr_lum);
    AppendBox(b_opct, jp2_opacity_4cc, {2, 1, 0, 0, 0});  // 4B chroma != 1B
    try_bad_jp2h(b_opct);
  }

  // 4. jp2_colour_converter negative RGB samples, Lab/Jab illuminants,
  //    vendor colour compare, and ICC device classes/errors.
  {
    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt);
    kdu_supp::jpx_target xt;
    xt.open(&ft);

    auto ct = xt.add_codestream();
    ct.access_dimensions().init(kdu_coords(4, 4), 3);
    for (int c = 0; c < 3; ++c) {
      ct.access_dimensions().set_precision(c, 8, false);
    }

    auto l_srgb = xt.add_layer();
    l_srgb.access_channels().init(3);
    for (int c = 0; c < 3; ++c)
      l_srgb.access_channels().set_colour_mapping(c, c);
    kdu_supp::jp2_colour col_srgb = l_srgb.add_colour();
    col_srgb.init(kdu_supp::JP2_sRGB_SPACE);
    kdu_supp::jp2_colour_converter conv;
    EXPECT_TRUE(conv.init(col_srgb, false, false));
    kdu_core::kdu_sample_allocator alloc;
    kdu_core::kdu_line_buf rf, gf, bf, ri, gi, bi;
    rf.pre_create(&alloc, 4, 0, 0, 0);
    gf.pre_create(&alloc, 4, 0, 0, 0);
    bf.pre_create(&alloc, 4, 0, 0, 0);
    ri.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
    gi.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
    bi.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
    alloc.finalize(kdu_core::kdu_codestream());
    rf.create();
    gf.create();
    bf.create();
    ri.create();
    gi.create();
    bi.create();
    for (int i = 0; i < 4; ++i) {
      rf.get_buf32()[i].fval = -1.0f;
      gf.get_buf32()[i].fval = -1.0f;
      bf.get_buf32()[i].fval = -1.0f;
      ri.get_buf16()[i].ival = -20000;
      gi.get_buf16()[i].ival = -20000;
      bi.get_buf16()[i].ival = -20000;
    }
    EXPECT_TRUE(conv.convert_rgb(rf, gf, bf));
    EXPECT_TRUE(conv.convert_rgb(ri, gi, bi));

    // Lab/Jab errors + D75/0 illuminant + zeta clamping (<0 and >0.75).
    auto l_lab = xt.add_layer();
    l_lab.access_channels().init(3);
    for (int c = 0; c < 3; ++c)
      l_lab.access_channels().set_colour_mapping(c, c);
    kdu_supp::jp2_colour col_lab = l_lab.add_colour();
    EXPECT_THROW(
        col_lab.init(kdu_supp::JP2_sRGB_SPACE, 1, 0, 8, 1, 0, 8, 1, 0, 8),
        kdu_core::kdu_exception);
    col_lab.init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 8, 200, 220, 8, 200, 128,
                 8, JP2_CIE_D75);
    EXPECT_THROW(
        col_lab.init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 8, 1, 0, 8, 1, 0, 8),
        kdu_core::kdu_exception);

    auto l_jab = xt.add_layer();
    l_jab.access_channels().init(3);
    for (int c = 0; c < 3; ++c)
      l_jab.access_channels().set_colour_mapping(c, c);
    l_jab.add_colour().init(kdu_supp::JP2_CIEJab_SPACE, 100, 0, 8, 200, 220, 8,
                            200, 128, 8, 0, 5000);

    // Negative offset clamps zeta to 0 in jp2_colour::init (cannot be saved
    // because j2_colour::save_box asserts offset[c] >= 0).
    kdu_supp::jp2_target jt_neg_off;
    kdu_supp::jp2_family_tgt fam_neg_off;
    MemTarget mt_neg_off;
    fam_neg_off.open(&mt_neg_off);
    jt_neg_off.open(&fam_neg_off);
    jt_neg_off.access_colour().init(kdu_supp::JP2_CIELab_SPACE, 100, -5, 8, 200,
                                    220, 8, 200, 128, 8, JP2_CIE_D75);
    jt_neg_off.close();
    fam_neg_off.close();

    // Vendor colour compare branches (different UUID, same UUID diff buffer).
    kdu_byte u1[16] = {1};
    kdu_byte u2[16] = {2};
    kdu_byte d1[4] = {1, 2, 3, 4};
    kdu_byte d2[4] = {1, 2, 3, 5};
    auto l_v1 = xt.add_layer();
    l_v1.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l_v1.access_channels().set_colour_mapping(c, c);
    l_v1.add_colour().init(u1, 4, d1);
    auto l_v2 = xt.add_layer();
    l_v2.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l_v2.access_channels().set_colour_mapping(c, c);
    l_v2.add_colour().init(u2, 4, d1);
    auto l_v3 = xt.add_layer();
    l_v3.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l_v3.access_channels().set_colour_mapping(c, c);
    l_v3.add_colour().init(u1, 4, d2);

    // ICC output profile ('prtr') + invalid display channel count error.
    auto icc_prtr = BuildIccProfile(
        0x52474220 /*RGB */, 0x58595A20 /*XYZ */,
        {MakeXyzTag(0x7258595A, 0.436, 0.222, 0.014),
         MakeXyzTag(0x6758595A, 0.385, 0.717, 0.097),
         MakeXyzTag(0x6258595A, 0.143, 0.061, 0.714),
         MakeCurvGammaTag(0x72545243, 2.2), MakeCurvGammaTag(0x67545243, 2.2),
         MakeCurvGammaTag(0x62545243, 2.2)},
        0x70727472 /*prtr*/);
    auto l_prtr = xt.add_layer();
    l_prtr.access_channels().init(3);
    for (int c = 0; c < 3; ++c)
      l_prtr.access_channels().set_colour_mapping(c, c);
    l_prtr.add_colour().init(icc_prtr.data());

    auto icc_bad_cmyk = BuildIccProfile(0x434D594B /*CMYK*/, 0x58595A20,
                                        {MakeCurvGammaTag(0x6B545243, 2.2)},
                                        0x6D6E7472 /*mntr*/);
    {
      MemTarget mt_bi;
      kdu_supp::jp2_family_tgt ft_bi;
      ft_bi.open(&mt_bi);
      kdu_supp::jp2_target jt_bi;
      jt_bi.open(&ft_bi);
      EXPECT_THROW(jt_bi.access_colour().init(icc_bad_cmyk.data()),
                   kdu_core::kdu_exception);
      jt_bi.close();
      ft_bi.close();
    }

    // Data references: >4 URLs growth, nullptr url, invalid get_url, perms.
    kdu_supp::jp2_data_references dref = xt.access_data_references();
    dref.add_url(nullptr);
    for (int i = 0; i < 8; ++i) {
      std::string url = "http://example.com/file_" + std::to_string(i) + ".jp2";
      EXPECT_GT(dref.add_url(url.c_str()), 0);
    }
    EXPECT_EQ(dref.get_url(99), nullptr);

    xt.write_headers();
    auto* s = ct.open_stream();
    CompressMinimalCodestream(*s, 4, 4, 3);
    s->close();
    xt.close();
    ft.close();

    // Parse back and exercise jp2_dref_permissions error/mismatch branches.
    MemSource ms(mt.data());
    kdu_supp::jp2_family_src fs;
    fs.open(&ms);
    fs.set_access_permissions("{bad");
    fs.set_access_permissions("*a*");
    fs.set_access_permissions("a\\");
    fs.set_access_permissions("}");
    fs.set_access_permissions(
        "https://**:./**\\z:**/{foo,bar}:http://other.com/**");
    kdu_supp::jpx_source xs;
    EXPECT_TRUE(xs.open(&fs, true));
    xs.close();
    fs.close();

    // Direct jp2_dref_permissions pattern errors & wildcards/escapes/choices.
    kdu_supp::jp2_dref_permissions perms;
    perms.init("/tmp/c.jp2", "./**");
    EXPECT_TRUE(perms.can_access("./dir/file"));
    EXPECT_FALSE(perms.can_access("./dir/a.jp2"));
    perms.init("/tmp/c.jp2", "***");
    EXPECT_FALSE(perms.can_access("xabc"));
    perms.init("/tmp/c.jp2", "{abc");
    EXPECT_FALSE(perms.can_access("abc"));
    perms.init("/tmp/c.jp2", "}");
    EXPECT_FALSE(perms.can_access("a"));
    perms.init("/tmp/c.jp2", "a\\");
    EXPECT_FALSE(perms.can_access("a"));
    perms.init("/tmp/c.jp2", "./**\\:.jp2");
    EXPECT_FALSE(perms.can_access("./dir/:"));
    EXPECT_TRUE(perms.can_access("./dir/x:.jp2"));
    perms.init("/tmp/c.jp2", "./**{a,b}.jp2");
    EXPECT_FALSE(perms.can_access("./dir/a.jp2"));
    EXPECT_TRUE(perms.can_access("./dir/xa.jp2"));
    perms.init("/tmp/c.jp2", "./foo:./sub/{foo,bar}");
    EXPECT_FALSE(perms.can_access("./dir/a.jp2"));
    EXPECT_TRUE(perms.can_access("./sub/bar"));
    EXPECT_FALSE(perms.can_access("./sub/baz"));
    perms.init("/tmp/c.jp2", "./sub\\a");
    EXPECT_TRUE(perms.can_access("./suba"));
  }
}

TEST(Jp2Test, MemsafeDimensionsIccHeadersAndDrefCoverage) {
  auto append_box = [](std::vector<kdu_byte>& dst, kdu_uint32 fourcc,
                       const std::vector<kdu_byte>& body) {
    AppendBe32(dst, static_cast<kdu_uint32>(8 + body.size()));
    AppendBe32(dst, fourcc);
    dst.insert(dst.end(), body.begin(), body.end());
  };
  auto build_jp2_with_jp2h = [&](const std::vector<kdu_byte>& jp2h_body) {
    std::vector<kdu_byte> f;
    append_box(f, jp2_signature_4cc, {0x0d, 0x0a, 0x87, 0x0a});
    std::vector<kdu_byte> ftyp;
    AppendBe32(ftyp, 0x6A703220U);  // 'jp2 '
    AppendBe32(ftyp, 0);
    AppendBe32(ftyp, 0x6A703220U);
    append_box(f, jp2_file_type_4cc, ftyp);
    append_box(f, jp2_header_4cc, jp2h_body);
    MemTarget mt_cs;
    CompressMinimalCodestream(mt_cs, 4, 4, 3);
    mt_cs.close();
    append_box(f, jp2_codestream_4cc, mt_cs.data());
    return f;
  };
  auto make_ihdr = [](int num_comps, kdu_byte bpc) {
    std::vector<kdu_byte> b;
    AppendBe32(b, 4);
    AppendBe32(b, 4);
    AppendBe16(b, static_cast<kdu_uint16>(num_comps));
    b.push_back(bpc);
    b.push_back(7);
    b.push_back(0);
    b.push_back(0);
    return b;
  };
  auto make_icc_hdr = [](kdu_uint32 space_sig, kdu_uint32 pcs_sig,
                         kdu_uint32 declared_len, kdu_uint32 num_tags) {
    std::vector<kdu_byte> p(132, 0);
    auto put32 = [&](int off, kdu_uint32 v) {
      p[off] = static_cast<kdu_byte>((v >> 24) & 0xFF);
      p[off + 1] = static_cast<kdu_byte>((v >> 16) & 0xFF);
      p[off + 2] = static_cast<kdu_byte>((v >> 8) & 0xFF);
      p[off + 3] = static_cast<kdu_byte>(v & 0xFF);
    };
    put32(0, declared_len);
    put32(12, 0x6D6E7472U);  // 'mntr'
    put32(16, space_sig);
    put32(20, pcs_sig);
    put32(128, num_tags);
    return p;
  };

  // 1. kdu_membroker overlimit in jp2_family_src::alloc_uint8 & j2_memsafe.
  {
    std::vector<kdu_byte> jp2_bytes;
    {
      MemTarget mt;
      kdu_supp::jp2_family_tgt ft;
      ft.open(&mt);
      kdu_supp::jp2_target jt;
      jt.open(&ft);
      kdu_core::siz_params siz;
      siz.set(Sdims, 0, 0, 4);
      siz.set(Sdims, 0, 1, 4);
      siz.set(Scomponents, 0, 0, 1);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, false);
      static_cast<kdu_core::kdu_params&>(siz).finalize();
      jt.access_dimensions().init(&siz);
      jt.access_dimensions().finalize_compatibility(Sprofile_PART1, 0, 0, 0, 0,
                                                    0);
      auto pal = jt.access_palette();
      pal.init(3, 128);
      kdu_int32 lut[128] = {};
      for (int c = 0; c < 3; ++c) pal.set_lut(c, lut, 8, false);
      jt.access_channels().init(3);
      for (int c = 0; c < 3; ++c) {
        jt.access_channels().set_colour_mapping(c, 0, c);
      }
      jt.access_colour().init(kdu_supp::JP2_sRGB_SPACE);
      jt.write_header();
      jt.open_codestream(true);
      CompressMinimalCodestream(jt, 4, 4, 1);
      jt.close();
      ft.close();
      jp2_bytes = mt.data();
    }
    kdu_core::kdu_membroker zero_broker(0);
    MemSource ms(jp2_bytes);
    kdu_supp::jp2_family_src fs;
    fs.open(&ms, &zero_broker);
    kdu_supp::jp2_input_box box;
    EXPECT_TRUE(box.open(&fs));
    EXPECT_FALSE(box.load_in_memory(1024));
    box.close();
    kdu_supp::jp2_source js;
    EXPECT_TRUE(js.open(&fs));
    EXPECT_THROW(js.read_header(), kdu_core::kdu_exception);
    js.close();
    fs.close();
  }

  // 2. jp2_dimensions::finalize_compatibility & malformed bpcc/colr/res/jp2h.
  {
    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt);
    kdu_supp::jp2_target jt;
    jt.open(&ft);
    auto dims = jt.access_dimensions();
    dims.init(kdu_coords(4, 4), 3, false);
    dims.finalize_compatibility(nullptr);
    kdu_core::siz_params root;
    root.set(Sprofile, 0, 0, Sprofile_PART2);
    root.set(Scap, 0, 0, Scap_P15);
    root.set(SCP2_caps, 0, 0, 32768);
    root.set(SCP15_caps, 0, 0, 32);
    root.set(Sextensions, 0, 0, Sextensions_MCT);
    dims.finalize_compatibility(&root);
    jt.close();
    ft.close();

    auto try_parse_jp2 = [&](const std::vector<kdu_byte>& jp2h_body,
                             bool expect_throw) {
      std::vector<kdu_byte> f = build_jp2_with_jp2h(jp2h_body);
      MemSource ms(f);
      kdu_supp::jp2_family_src fs;
      fs.open(&ms);
      kdu_supp::jp2_source js;
      js.open(&fs);
      if (expect_throw) {
        EXPECT_THROW(js.read_header(), kdu_core::kdu_exception);
      } else {
        EXPECT_EQ(js.read_header(), 1);
      }
      js.close();
      fs.close();
    };

    // Malformed bpcc boxes (truncated, >38 bits, extra bytes).
    {
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 0xFF));
      append_box(h, jp2_bits_per_component_4cc, {7});
      try_parse_jp2(h, true);
    }
    {
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 0xFF));
      append_box(h, jp2_bits_per_component_4cc, {50, 7, 7});
      try_parse_jp2(h, true);
    }
    {
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 0xFF));
      append_box(h, jp2_bits_per_component_4cc, {7, 7, 7, 7});
      try_parse_jp2(h, true);
    }

    // Malformed colr boxes + meth=3 Lab/Lab + unknown jp2h subbox + bad res.
    {
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 7));
      append_box(h, jp2_colour_4cc, {1, 0});  // Truncated colr
      try_parse_jp2(h, true);
    }
    {
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 7));
      append_box(h, jp2_colour_4cc, {5, 0, 0, 0, 0, 0, 16});  // Bad meth
      try_parse_jp2(h, true);
    }
    {
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 7));
      append_box(h, jp2_colour_4cc, {1, 0, 0, 0, 16});  // Truncated meth=1
      try_parse_jp2(h, true);
    }
    {
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 7));
      std::vector<kdu_byte> colr = {1, 0, 0};
      AppendBe32(colr, 999);  // Unknown enum_cs -> returns uninitialized
      append_box(h, jp2_colour_4cc, colr);
      try_parse_jp2(h, true);
    }
    {
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 7));
      std::vector<kdu_byte> colr = {1, 0, 0};
      AppendBe32(colr, 14);
      AppendBe32(colr, 100);  // Truncated EP (< 7 fields)
      append_box(h, jp2_colour_4cc, colr);
      try_parse_jp2(h, true);
    }
    {
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 7));
      std::vector<kdu_byte> colr = {1, 0, 0};
      AppendBe32(colr, 16);
      colr.push_back(0xAA);  // Trailing byte -> !colr->close()
      append_box(h, jp2_colour_4cc, colr);
      try_parse_jp2(h, true);
    }
    {
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 7));
      std::vector<kdu_byte> colr = {2, 0, 0};
      auto icc = make_icc_hdr(0x52474220U, 0x58595A20U, 300, 0);  // Truncated
      colr.insert(colr.end(), icc.begin(), icc.end());
      append_box(h, jp2_colour_4cc, colr);
      try_parse_jp2(h, true);
    }
    {
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 7));
      std::vector<kdu_byte> colr = {2, 0, 0};
      auto icc = make_icc_hdr(0x52474220U, 0x42414420U, 132, 0);  // Bad PCS
      colr.insert(colr.end(), icc.begin(), icc.end());
      append_box(h, jp2_colour_4cc, colr);
      try_parse_jp2(h, true);
    }
    {
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 7));
      std::vector<kdu_byte> colr = {2, 0, 0};
      auto icc =
          make_icc_hdr(0x52474220U, 0x58595A20U, 132, 50);  // Too many tags
      colr.insert(colr.end(), icc.begin(), icc.end());
      append_box(h, jp2_colour_4cc, colr);
      try_parse_jp2(h, true);
    }
    {
      // meth=3 Lab + Lab PCS executes j2_icc_profile Lab paths before JP2
      // check.
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 7));
      std::vector<kdu_byte> colr = {3, 0, 0};
      auto icc = make_icc_hdr(0x4C616220U, 0x4C616220U, 132, 0);
      colr.insert(colr.end(), icc.begin(), icc.end());
      append_box(h, jp2_colour_4cc, colr);
      try_parse_jp2(h, true);
    }
    {
      // Valid sRGB + unknown box in jp2h (skipped cleanly).
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 7));
      std::vector<kdu_byte> colr = {1, 0, 0};
      AppendBe32(colr, 16);
      append_box(h, jp2_colour_4cc, colr);
      append_box(h, 0x78787878U, {1, 2, 3, 4});
      try_parse_jp2(h, false);
    }
    {
      // Empty res box -> throws.
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 7));
      std::vector<kdu_byte> colr = {1, 0, 0};
      AppendBe32(colr, 16);
      append_box(h, jp2_colour_4cc, colr);
      append_box(h, jp2_resolution_4cc, {});
      try_parse_jp2(h, true);
    }
    {
      // Duplicate res boxes -> throws.
      std::vector<kdu_byte> h, r;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 7));
      std::vector<kdu_byte> colr = {1, 0, 0};
      AppendBe32(colr, 16);
      append_box(h, jp2_colour_4cc, colr);
      append_box(r, jp2_capture_resolution_4cc, {0, 1, 0, 1, 0, 1, 0, 1, 0, 0});
      append_box(h, jp2_resolution_4cc, r);
      append_box(h, jp2_resolution_4cc, r);
      try_parse_jp2(h, true);
    }
    {
      // Trailing bytes inside jp2h -> !box->close() throws.
      std::vector<kdu_byte> h;
      append_box(h, jp2_image_header_4cc, make_ihdr(3, 7));
      std::vector<kdu_byte> colr = {1, 0, 0};
      AppendBe32(colr, 16);
      append_box(h, jp2_colour_4cc, colr);
      h.push_back(0);
      h.push_back(0);
      try_parse_jp2(h, true);
    }
  }

  // 3. jp2_colour::get_icc_profile, check_cie_default, j2_colour::compare,
  //    jp2_colour_converter skip_primary_matrix & vendor paths.
  {
    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt);
    kdu_supp::jpx_target xt;
    xt.open(&ft);
    auto ct = xt.add_codestream();
    ct.access_dimensions().init(kdu_coords(4, 4), 3, false);
    for (int c = 0; c < 3; ++c) {
      ct.access_dimensions().set_precision(c, 8, false);
    }

    // Layer 0: default CIELab -> false before finalize, true after
    // write_headers.
    auto l0 = xt.add_layer();
    l0.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l0.access_channels().set_colour_mapping(c, c);
    auto col0 = l0.add_colour();
    col0.init(kdu_supp::JP2_CIELab_SPACE);
    EXPECT_FALSE(col0.check_cie_default());
    EXPECT_EQ(col0.get_icc_profile(), nullptr);
    kdu_byte icc_tmp[16] = {};
    EXPECT_EQ(col0.get_icc_profile(icc_tmp, 16), 0);

    // Layer 1: default CIEJab (note upstream sets range[0]=0) & explicit CIEJab
    auto l1 = xt.add_layer();
    l1.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l1.access_channels().set_colour_mapping(c, c);
    auto col1 = l1.add_colour();
    col1.init(kdu_supp::JP2_CIEJab_SPACE);
    EXPECT_FALSE(col1.check_cie_default());
    auto col1_def = l1.add_colour();
    col1_def.init(kdu_supp::JP2_CIEJab_SPACE, 100, 0, 8, 255, 128, 8, 255, 128,
                  8);
    EXPECT_TRUE(col1_def.check_cie_default());
    auto col1_off = l1.add_colour();
    col1_off.init(kdu_supp::JP2_CIEJab_SPACE, 100, 1, 8, 255, 128, 8, 255, 128,
                  8);
    EXPECT_FALSE(col1_off.check_cie_default());

    // Layer 2: non-default CIELab (D65) -> check_cie_default == false +
    // compare.
    auto l2 = xt.add_layer();
    l2.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l2.access_channels().set_colour_mapping(c, c);
    auto col2 = l2.add_colour();
    col2.init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 8, 170, 128, 8, 200, 96, 8,
              JP2_CIE_D65);
    EXPECT_FALSE(col2.check_cie_default());
    auto col2_range = l2.add_colour();
    col2_range.init(kdu_supp::JP2_CIELab_SPACE, 99, 0, 8, 170, 128, 8, 200, 96,
                    8, JP2_CIE_D50);
    EXPECT_FALSE(col2_range.check_cie_default());
    auto col2_off = l2.add_colour();
    col2_off.init(kdu_supp::JP2_CIELab_SPACE, 100, 1, 8, 170, 128, 8, 200, 96,
                  8, JP2_CIE_D50);
    EXPECT_FALSE(col2_off.check_cie_default());

    // Layer 3 & 4: two different RGB ICC profiles -> get_icc_profile & compare.
    std::vector<IccTag> tags_a = {
        MakeXyzTag(0x7258595AU, 0.436, 0.222, 0.014),  // rXYZ
        MakeXyzTag(0x6758595AU, 0.385, 0.717, 0.097),  // gXYZ
        MakeXyzTag(0x6258595AU, 0.143, 0.061, 0.714),  // bXYZ
        MakeCurvGammaTag(0x72545243U, 2.2),            // rTRC
        MakeCurvGammaTag(0x67545243U, 2.2),            // gTRC
        MakeCurvGammaTag(0x62545243U, 2.2),            // bTRC
    };
    std::vector<IccTag> tags_b = tags_a;
    tags_b[3] = MakeCurvGammaTag(0x72545243U, 1.8);
    auto icc_a = BuildIccProfile(0x52474220U, 0x58595A20U, tags_a);
    auto icc_b = BuildIccProfile(0x52474220U, 0x58595A20U, tags_b);

    auto l3 = xt.add_layer();
    l3.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l3.access_channels().set_colour_mapping(c, c);
    auto col3 = l3.add_colour();
    col3.init(icc_a.data());
    int icc_len = 0;
    EXPECT_NE(col3.get_icc_profile(&icc_len), nullptr);
    EXPECT_EQ(icc_len, static_cast<int>(icc_a.size()));
    EXPECT_EQ(col3.get_icc_profile(icc_tmp, 16), icc_len);
    EXPECT_EQ(col3.get_icc_profile(nullptr, 0), icc_len);

    auto l4 = xt.add_layer();
    l4.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l4.access_channels().set_colour_mapping(c, c);
    l4.add_colour().init(icc_b.data());

    // Layer 5: esRGB + vendor colour for jp2_colour_converter.
    auto l5 = xt.add_layer();
    l5.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l5.access_channels().set_colour_mapping(c, c);
    auto col_es = l5.add_colour();
    col_es.init(kdu_supp::JP2_esRGB_SPACE);
    EXPECT_FALSE(col_es.check_cie_default());

    {
      kdu_supp::jp2_colour_converter conv;
      EXPECT_TRUE(conv.init(col_es, false));
      kdu_core::kdu_sample_allocator alloc;
      kdu_core::kdu_line_buf rf, gf, bf, ri, gi, bi;
      rf.pre_create(&alloc, 4, 0, 0, 0);
      gf.pre_create(&alloc, 4, 0, 0, 0);
      bf.pre_create(&alloc, 4, 0, 0, 0);
      ri.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
      gi.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
      bi.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
      alloc.finalize(kdu_core::kdu_codestream());
      rf.create();
      gf.create();
      bf.create();
      ri.create();
      gi.create();
      bi.create();
      for (int i = 0; i < 4; ++i) {
        rf.get_buf32()[i].fval = -1.0f;
        gf.get_buf32()[i].fval = -1.0f;
        bf.get_buf32()[i].fval = -1.0f;
        ri.get_buf16()[i].ival = -20000;
        gi.get_buf16()[i].ival = -20000;
        bi.get_buf16()[i].ival = -20000;
      }
      EXPECT_TRUE(conv.convert_rgb(rf, gf, bf));
      EXPECT_TRUE(conv.convert_rgb(ri, gi, bi));
    }

    auto l6 = xt.add_layer();
    l6.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l6.access_channels().set_colour_mapping(c, c);
    kdu_byte uuid[16] = {9};
    kdu_byte vbuf[4] = {1, 2, 3, 4};
    auto col_v = l6.add_colour();
    col_v.init(uuid, 4, vbuf);
    kdu_supp::jp2_colour_converter conv_v;
    EXPECT_FALSE(conv_v.init(col_v));

    xt.write_headers();
    EXPECT_TRUE(col0.check_cie_default());
    EXPECT_FALSE(col1.check_cie_default());
    auto* s = ct.open_stream();
    CompressMinimalCodestream(*s, 4, 4, 3);
    s->close();
    xt.close();
    ft.close();
  }

  // 4. jp2_palette::get_abs_lut (signed & 32-bit), jp2_channels::copy,
  //    jp2_output_box::write_free_and_close, jp2_data_references.
  {
    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt);
    kdu_supp::jpx_target xt;
    xt.open(&ft);
    auto ct = xt.add_codestream();
    ct.access_dimensions().init(kdu_coords(4, 4), 1, false, 8);
    auto l = xt.add_layer();

    auto pal = ct.access_palette();
    pal.init(1, 4);
    kdu_int32 lut_vals[4] = {0, 10, -5, 20};
    pal.set_lut(0, lut_vals, 32, true);
    kdu_core::kdu_sample32 abs_lut[4];
    EXPECT_FALSE(pal.get_abs_lut(-1, abs_lut));
    EXPECT_TRUE(pal.get_abs_lut(0, abs_lut));
    EXPECT_EQ(abs_lut[2].ival, -5);

    auto ch_src = l.access_channels();
    ch_src.init(3, 1);
    ch_src.set_colour_mapping(0, 0, 0, 2);
    ch_src.set_colour_mapping(1, 0, 0, 0);
    ch_src.set_colour_mapping(2, 0, 0, 0);
    ch_src.set_opacity_mapping(0, 0, -1, 0);
    ch_src.set_chroma_key(0, 10);
    int smap[2] = {0, 0};
    auto l2 = xt.add_layer();
    auto ch_dst = l2.access_channels();
    ch_dst.copy(ch_src, smap, 2,
                1);  // Exercises <0 & >=stream_map_elts clamping
    EXPECT_THROW(ch_dst.copy(ch_src), kdu_core::kdu_exception);

    // Data references: add_file_url, get_file_url, add_url invalidation/growth.
    auto dref = xt.access_data_references();
    EXPECT_EQ(dref.add_file_url("a.jp2"), 1);
    EXPECT_EQ(dref.add_file_url("/tmp/b.jp2"), 2);
    EXPECT_EQ(dref.add_file_url("C:/win/c.jp2"), 3);
    EXPECT_EQ(dref.add_file_url("./rel.jp2"), 4);
    EXPECT_EQ(dref.add_file_url(nullptr, 5), 5);
    EXPECT_EQ(dref.get_file_url(0), nullptr);
    EXPECT_NE(dref.get_file_url(1), nullptr);
    EXPECT_NE(dref.get_file_url(3), nullptr);
    EXPECT_EQ(dref.add_url("file:///updated.jp2", 1),
              1);  // Clears file_refs[0]
    EXPECT_EQ(dref.add_url("file:///grown.jp2", 20), 20);  // Grows file_refs
    EXPECT_EQ(dref.add_url(nullptr, -1), 0);
    EXPECT_THROW(dref.add_url("file:///too_big.jp2", 65535),
                 kdu_core::kdu_exception);

    xt.close();
    ft.close();

    // Uninitialized add_url / add_file_url / get_file_url return 0 / nullptr.
    kdu_supp::jp2_data_references dref_empty;
    EXPECT_EQ(dref_empty.add_url("http://x"), 0);
    EXPECT_EQ(dref_empty.add_file_url("x.jp2"), 0);
    EXPECT_EQ(dref_empty.get_file_url(1), nullptr);
  }

  // Extended 64-bit write_free_and_close on failing target.
  {
    struct FailingTarget : public kdu_core::kdu_compressed_target {
      int writes = 0;
      bool write(const kdu_byte* buf, int num_bytes) override {
        writes += num_bytes;
        return writes <= 24;
      }
      int get_capabilities() override { return 0; }
    } fail_tgt;
    kdu_supp::jp2_family_tgt ft_fail;
    ft_fail.open(&fail_tgt);
    kdu_supp::jp2_output_box out_fail;
    out_fail.open(&ft_fail, jp2_uuid_4cc, false);
    EXPECT_THROW(out_fail.write_free_and_close(kdu_long{1} << 33),
                 kdu_core::kdu_exception);
    ft_fail.close();
  }
}

TEST(Jp2Test, OutputBoxesColoursIccChannelsAndStreamingCoverage) {
  // 1. jp2_family_tgt simulation mode & jp2_output_box edge cases.
  {
    kdu_supp::jp2_output_box out_uninit;
    EXPECT_EQ(out_uninit.get_start_pos(), 0);
    EXPECT_FALSE(out_uninit.pre_open(nullptr));
    EXPECT_THROW(out_uninit.open_next(jp2_uuid_4cc), kdu_core::kdu_exception);
    EXPECT_THROW(out_uninit.use_long_header(), kdu_core::kdu_exception);
    out_uninit.close();

    // Standalone buffer-only box (tgt == nullptr).
    kdu_supp::jp2_output_box out_null_rub;
    EXPECT_THROW(
        out_null_rub.open(static_cast<kdu_supp::jp2_family_tgt*>(nullptr),
                          jp2_uuid_4cc, true),
        kdu_core::kdu_exception);

    kdu_supp::jp2_output_box out_buf;
    out_buf.open(static_cast<kdu_supp::jp2_family_tgt*>(nullptr), jp2_uuid_4cc,
                 false);
    kdu_byte payload[16] = {1, 2, 3, 4};
    EXPECT_TRUE(out_buf.write(payload, 16));
    out_buf.set_rubber_length();
    out_buf.close();
    EXPECT_EQ(out_buf.reopen(jp2_xml_4cc, 0), -1);
    EXPECT_EQ(out_buf.reopen(jp2_uuid_4cc, 0), -1);

    // Simulation family target.
    kdu_supp::jp2_family_tgt ft_sim;
    ft_sim.open(kdu_long{16});
    EXPECT_THROW(ft_sim.open(kdu_long{0}), kdu_core::kdu_exception);

    kdu_supp::jp2_output_box out_sim2;
    EXPECT_TRUE(out_sim2.pre_open(&ft_sim));
    out_sim2.open(&ft_sim, jp2_uuid_4cc, false);
    kdu_supp::jp2_family_tgt ft_other;
    EXPECT_THROW(out_sim2.open(&ft_sim, jp2_uuid_4cc), kdu_core::kdu_exception);
    EXPECT_THROW(out_sim2.pre_open(&ft_other), kdu_core::kdu_exception);
    EXPECT_TRUE(out_sim2.write(payload, 16));
    EXPECT_EQ(out_sim2.get_start_pos(), 16);
    out_sim2.close();
    EXPECT_GE(out_sim2.reopen(jp2_xml_4cc, 4), 0);
    EXPECT_TRUE(out_sim2.write(payload, 4));
    out_sim2.close();

    // Large payload (>256B) + nested sub-box + set_rubber_length in sim mode.
    kdu_supp::jp2_output_box out_sim;
    out_sim.open(&ft_sim, jp2_uuid_4cc, false);
    kdu_supp::jp2_output_box sub_sim;
    sub_sim.open(&out_sim, jp2_uuid_4cc, false);
    EXPECT_THROW(sub_sim.open(&out_sim, jp2_uuid_4cc), kdu_core::kdu_exception);
    EXPECT_GT(sub_sim.get_start_pos(), 0);
    std::vector<kdu_byte> big(300, 0x5A);
    EXPECT_TRUE(sub_sim.write(big.data(), static_cast<int>(big.size())));
    sub_sim.set_rubber_length();
    sub_sim.close();
    EXPECT_THROW(out_sim.write_free_and_close(32), kdu_core::kdu_exception);
    EXPECT_TRUE(out_sim.close());

    kdu_supp::jp2_output_box out_after_rub;
    EXPECT_THROW(out_after_rub.open(&ft_sim, jp2_uuid_4cc),
                 kdu_core::kdu_exception);
    ft_sim.close();
  }

  // 2. jp2_source / jp2_target pre-open accessors & error paths + baseline
  // JPH/JP2 rejection.
  {
    kdu_supp::jp2_source js_empty;
    EXPECT_FALSE(js_empty.access_dimensions().exists());
    EXPECT_FALSE(js_empty.access_colour().exists());
    EXPECT_FALSE(js_empty.access_palette().exists());
    EXPECT_FALSE(js_empty.access_channels().exists());
    EXPECT_FALSE(js_empty.access_resolution().exists());

    kdu_supp::jp2_target jt_empty;
    EXPECT_EQ(jt_empty.get_brand(), 0u);
    EXPECT_FALSE(jt_empty.access_dimensions().exists());
    EXPECT_FALSE(jt_empty.access_palette().exists());
    EXPECT_FALSE(jt_empty.access_channels().exists());
    EXPECT_FALSE(jt_empty.access_resolution().exists());
    EXPECT_THROW(jt_empty.open_codestream(), kdu_core::kdu_exception);

    // Baseline JP2 rejecting JP2_iccANY_SPACE (Lab scanner ICC profile).
    auto icc_lab = BuildIccProfile(0x4C616220U, 0x4C616220U, {}, 0x73636E72U);
    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt);
    kdu_supp::jp2_target jt;
    jt.open(&ft);
    EXPECT_EQ(jt.get_brand(), 0u);
    EXPECT_THROW(jt.open_codestream(), kdu_core::kdu_exception);
    kdu_core::siz_params siz;
    siz.set(Sdims, 0, 0, 4);
    siz.set(Sdims, 0, 1, 4);
    siz.set(Scomponents, 0, 0, 3);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    jt.access_dimensions().init(&siz);
    jt.access_colour().init(icc_lab.data());
    EXPECT_THROW(jt.write_header(), kdu_core::kdu_exception);
    jt.close();
    ft.close();

    // Valid jp2_target: double write_header throws.
    MemTarget mt_ok;
    kdu_supp::jp2_family_tgt ft_ok;
    ft_ok.open(&mt_ok);
    kdu_supp::jp2_target jt_ok;
    jt_ok.open(&ft_ok);
    jt_ok.access_dimensions().init(&siz);
    jt_ok.access_colour().init(kdu_supp::JP2_sRGB_SPACE);
    jt_ok.write_header();
    EXPECT_EQ(jt_ok.get_brand(), jp2_brand);
    EXPECT_THROW(jt_ok.write_header(), kdu_core::kdu_exception);
    jt_ok.close();
    ft_ok.close();
  }

  // 3. ICC curve error paths, gamma constructors, sRGB negative float/int
  // paths.
  {
    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt);
    kdu_supp::jpx_target xt;
    xt.open(&ft);
    auto ct = xt.add_codestream();
    ct.access_dimensions().init(kdu_coords(4, 4), 3, false);
    for (int c = 0; c < 3; ++c)
      ct.access_dimensions().set_precision(c, 8, false);

    // gamma < 1.0 with non-zero beta throws; beta=0 sets pure exponent;
    // num_points < 1 -> linear; gamma=1.0 -> linear.
    const double xy_r[2] = {0.64, 0.33};
    const double xy_g[2] = {0.30, 0.60};
    const double xy_b[2] = {0.15, 0.06};
    auto l0 = xt.add_layer();
    l0.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l0.access_channels().set_colour_mapping(c, c);
    auto c_pure = l0.add_colour();
    c_pure.init(xy_r, xy_g, xy_b, 2.2, 0.0, 8, false);
    auto c_npts0 = l0.add_colour();
    c_npts0.init(xy_r, xy_g, xy_b, 2.2, 0.05, 0, false);
    auto c_g1 = l0.add_colour();
    c_g1.init(xy_r, xy_g, xy_b, 1.0, 0.0, 8, false);

    // JP2_sRGB_SPACE negative inputs in convert_rgb (skip_primary_matrix ==
    // true).
    auto c_srgb = l0.add_colour();
    c_srgb.init(kdu_supp::JP2_sRGB_SPACE);
    {
      kdu_supp::jp2_colour_converter conv;
      EXPECT_TRUE(conv.init(c_srgb, false));
      kdu_core::kdu_sample_allocator alloc;
      kdu_core::kdu_line_buf rf, gf, bf, ri, gi, bi;
      rf.pre_create(&alloc, 4, 0, 0, 0);
      gf.pre_create(&alloc, 4, 0, 0, 0);
      bf.pre_create(&alloc, 4, 0, 0, 0);
      ri.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
      gi.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
      bi.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
      alloc.finalize(kdu_core::kdu_codestream());
      rf.create();
      gf.create();
      bf.create();
      ri.create();
      gi.create();
      bi.create();
      for (int i = 0; i < 4; ++i) {
        rf.get_buf32()[i].fval = -1.0f;
        gf.get_buf32()[i].fval = -1.0f;
        bf.get_buf32()[i].fval = -1.0f;
        ri.get_buf16()[i].ival = -20000;
        gi.get_buf16()[i].ival = -20000;
        bi.get_buf16()[i].ival = -20000;
      }
      EXPECT_TRUE(conv.convert_rgb(rf, gf, bf));
      EXPECT_TRUE(conv.convert_rgb(ri, gi, bi));
    }

    // Vendor UUID & data comparison across layers, plus D65 / D75 CIELab parse.
    kdu_byte uuid1[16] = {1};
    kdu_byte uuid2[16] = {2};
    kdu_byte vdata1[4] = {10, 20, 30, 40};
    kdu_byte vdata2[4] = {10, 20, 30, 99};
    auto lv1 = xt.add_layer();
    lv1.access_channels().init(3);
    for (int c = 0; c < 3; ++c) lv1.access_channels().set_colour_mapping(c, c);
    lv1.access_channels().set_chroma_key(0, 10);
    lv1.add_colour().init(kdu_supp::JP2_sRGB_SPACE);

    auto lv2 = xt.add_layer();
    lv2.access_channels().init(3);
    for (int c = 0; c < 3; ++c) lv2.access_channels().set_colour_mapping(c, c);
    lv2.access_channels().set_chroma_key(0, 11);  // chroma_key differs
    lv2.add_colour().init(kdu_supp::JP2_sRGB_SPACE);

    auto lv3 = xt.add_layer();
    lv3.access_channels().init(3);
    for (int c = 0; c < 3; ++c) lv3.access_channels().set_colour_mapping(c, c);
    lv3.add_colour().init(uuid1, 4, vdata2);

    auto l_d65 = xt.add_layer();
    l_d65.access_channels().init(3);
    for (int c = 0; c < 3; ++c)
      l_d65.access_channels().set_colour_mapping(c, c);
    l_d65.add_colour().init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 8, 170, 128, 8,
                            200, 96, 8, JP2_CIE_D65);

    auto l_d75 = xt.add_layer();
    l_d75.access_channels().init(3);
    for (int c = 0; c < 3; ++c)
      l_d75.access_channels().set_colour_mapping(c, c);
    l_d75.add_colour().init(kdu_supp::JP2_CIELab_SPACE, 100, 0, 8, 170, 128, 8,
                            200, 96, 8, JP2_CIE_D75);

    auto lv_u1 = xt.add_layer();
    lv_u1.access_channels().init(3);
    for (int c = 0; c < 3; ++c)
      lv_u1.access_channels().set_colour_mapping(c, c);
    lv_u1.add_colour().init(uuid1, 4, vdata1);

    auto lv_u2 = xt.add_layer();
    lv_u2.access_channels().init(3);
    for (int c = 0; c < 3; ++c)
      lv_u2.access_channels().set_colour_mapping(c, c);
    lv_u2.add_colour().init(uuid2, 4, vdata1);  // uuid differs from uuid1

    // Resolution save paths: aspect ratio without resolution, display + cap
    // ratio.
    auto lr1 = xt.add_layer();
    lr1.access_channels().init(3);
    for (int c = 0; c < 3; ++c) lr1.access_channels().set_colour_mapping(c, c);
    lr1.add_colour().init(kdu_supp::JP2_sRGB_SPACE);
    lr1.access_resolution().init(2.0f);
    auto lr2 = xt.add_layer();
    lr2.access_channels().init(3);
    for (int c = 0; c < 3; ++c) lr2.access_channels().set_colour_mapping(c, c);
    lr2.add_colour().init(kdu_supp::JP2_sRGB_SPACE);
    lr2.access_resolution().init(1.0f);
    lr2.access_resolution().set_resolution(100.0f, true);
    lr2.access_resolution().set_different_capture_aspect_ratio(1.5f);

    // Throwaway jpx_target for colour init validation errors (doesn't flush).
    {
      MemTarget mt_err;
      kdu_supp::jp2_family_tgt ft_err;
      ft_err.open(&mt_err);
      kdu_supp::jpx_target xt_err;
      xt_err.open(&ft_err);
      auto l_bad = xt_err.add_layer();
      EXPECT_THROW(
          l_bad.add_colour().init(xy_r, xy_g, xy_b, 0.5, 0.05, 8, false),
          kdu_core::kdu_exception);

      std::vector<IccTag> no_bxyz = {
          MakeXyzTag(0x7258595AU, 0.436, 0.222, 0.014),
          MakeXyzTag(0x6758595AU, 0.385, 0.717, 0.097),
          MakeCurvGammaTag(0x72545243U, 2.2),
          MakeCurvGammaTag(0x67545243U, 2.2),
          MakeCurvGammaTag(0x62545243U, 2.2),
      };
      auto icc_no_bxyz = BuildIccProfile(0x52474220U, 0x58595A20U, no_bxyz);
      EXPECT_THROW(l_bad.add_colour().init(icc_no_bxyz.data()),
                   kdu_core::kdu_exception);

      std::vector<IccTag> no_btrc = {
          MakeXyzTag(0x7258595AU, 0.436, 0.222, 0.014),
          MakeXyzTag(0x6758595AU, 0.385, 0.717, 0.097),
          MakeXyzTag(0x6258595AU, 0.143, 0.061, 0.714),
          MakeCurvGammaTag(0x72545243U, 2.2),
          MakeCurvGammaTag(0x67545243U, 2.2),
      };
      auto icc_no_btrc = BuildIccProfile(0x52474220U, 0x58595A20U, no_btrc);
      EXPECT_THROW(l_bad.add_colour().init(icc_no_btrc.data()),
                   kdu_core::kdu_exception);
      xt_err.close();
      ft_err.close();
    }

    // Unknown para func (type 5 -> warning + linear LUT fallback).
    std::vector<IccTag> para5 = {
        MakeXyzTag(0x7258595AU, 0.436, 0.222, 0.014),
        MakeXyzTag(0x6758595AU, 0.385, 0.717, 0.097),
        MakeXyzTag(0x6258595AU, 0.143, 0.061, 0.714),
        MakeParaCurveTag(0x72545243U, 5),
        MakeParaCurveTag(0x67545243U, 4),
        MakeParaCurveTag(0x62545243U, 3),
    };
    auto icc_p5 = BuildIccProfile(0x52474220U, 0x58595A20U, para5);
    auto lp5 = xt.add_layer();
    lp5.access_channels().init(3);
    for (int c = 0; c < 3; ++c) lp5.access_channels().set_colour_mapping(c, c);
    auto col_p5 = lp5.add_colour();
    col_p5.init(icc_p5.data());
    {
      kdu_supp::jp2_colour_converter conv_p5;
      EXPECT_TRUE(conv_p5.init(col_p5, false));
    }

    xt.write_headers();
    auto* s = ct.open_stream();
    CompressMinimalCodestream(*s, 4, 4, 3);
    s->close();
    xt.close();
    ft.close();

    // Parse back JPX: copy populated channels (cdef_triplets + chroma_key_buf)
    // with out-of-range stream_base to exercise clamping & deep copies.
    ScopedMessageHandlers msg;
    try {
      MemSource ms(mt.data());
      kdu_supp::jp2_family_src fs;
      fs.open(&ms);
      kdu_supp::jpx_source xs;
      ASSERT_TRUE(xs.open(&fs, false));
      kdu_supp::jpx_layer_source src_lv1 = xs.access_layer(1);
      ASSERT_TRUE(src_lv1.exists());
      int lr = 0, lo = 0, lb = 0, ar = 0, ao = 0, ab = 0, br = 0, bo = 0,
          bb = 0;
      kdu_uint32 illum_d65 = 0, illum_d75 = 0;
      kdu_uint16 temp_d65 = 0, temp_d75 = 0;
      EXPECT_TRUE(xs.access_layer(4).access_colour(0).get_lab_params(
          lr, lo, lb, ar, ao, ab, br, bo, bb, illum_d65, temp_d65));
      EXPECT_TRUE(xs.access_layer(5).access_colour(0).get_lab_params(
          lr, lo, lb, ar, ao, ab, br, bo, bb, illum_d75, temp_d75));
      EXPECT_EQ(illum_d65, JP2_CIE_D65);
      EXPECT_EQ(illum_d75, JP2_CIE_D75);

      MemTarget mt_copy;
      kdu_supp::jp2_family_tgt ft_copy;
      ft_copy.open(&mt_copy);
      kdu_supp::jpx_target xt_copy;
      xt_copy.open(&ft_copy);
      auto ct_copy = xt_copy.add_codestream();
      ct_copy.access_dimensions().init(kdu_coords(4, 4), 3, false);
      for (int c = 0; c < 3; ++c)
        ct_copy.access_dimensions().set_precision(c, 8, false);
      int smap[2] = {0, 0};
      auto lc1 = xt_copy.add_layer();
      lc1.access_channels().copy(src_lv1.access_channels(), smap, 2, -1);
      auto lc2 = xt_copy.add_layer();
      lc2.access_channels().copy(src_lv1.access_channels(), smap, 2, 5);
      xs.close();
      fs.close();
      xt_copy.close();
      ft_copy.close();
    } catch (kdu_core::kdu_exception exc) {
      ADD_FAILURE() << "kdu_exception=" << exc << " errors=" << msg.errors()
                    << " warnings=" << msg.warnings();
    }
  }
}

TEST(Jp2Test, FinalThresholdCoveragePush) {
  // 1. j2_colour::compare (vendor UUID/buf mismatch) & j2_channels::compare
  // (chroma_key),
  //    plus skip_primary_matrix == true negative sp1 float/int convert_rgb,
  //    plus jp2_channels::init(>J2_MAX_CHANNELS) and
  //    get_codestreams_used(nullptr, 0).
  {
    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt);
    kdu_supp::jpx_target xt;
    xt.open(&ft);
    auto ct = xt.add_codestream();
    ct.access_dimensions().init(kdu_coords(4, 4), 3, false);
    for (int c = 0; c < 3; ++c)
      ct.access_dimensions().set_precision(c, 8, false);

    kdu_byte uuid1[16] = {1};
    kdu_byte uuid2[16] = {2};
    kdu_byte vdata1[4] = {10, 20, 30, 40};
    kdu_byte vdata2[4] = {10, 20, 30, 99};

    auto l0 = xt.add_layer();
    l0.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l0.access_channels().set_colour_mapping(c, c);
    l0.access_channels().set_chroma_key(0, 10);
    l0.add_colour().init(uuid1, 4, vdata1);

    int min_s = 0, max_s = 0;
    EXPECT_EQ(
        l0.access_channels().get_codestreams_used(min_s, max_s, nullptr, 0), 1);

    auto l1 = xt.add_layer();
    l1.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l1.access_channels().set_colour_mapping(c, c);
    l1.access_channels().set_chroma_key(0, 11);
    l1.add_colour().init(uuid2, 4, vdata1);

    auto l2 = xt.add_layer();
    l2.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l2.access_channels().set_colour_mapping(c, c);
    l2.add_colour().init(uuid1, 4, vdata2);

    // ICC RGB profile with sRGB primaries so skip_primary_matrix == true.
    const double xy_r[2] = {0.64, 0.33};
    const double xy_g[2] = {0.30, 0.60};
    const double xy_b[2] = {0.15, 0.06};
    auto l3 = xt.add_layer();
    l3.access_channels().init(3);
    for (int c = 0; c < 3; ++c) l3.access_channels().set_colour_mapping(c, c);
    auto c_icc_srgb = l3.add_colour();
    c_icc_srgb.init(xy_r, xy_g, xy_b, 1.8, 0.0, 64, false);
    {
      kdu_supp::jp2_colour_converter conv;
      EXPECT_TRUE(conv.init(c_icc_srgb, false));
      kdu_core::kdu_sample_allocator alloc;
      kdu_core::kdu_line_buf rf, gf, bf, ri, gi, bi;
      rf.pre_create(&alloc, 4, 0, 0, 0);
      gf.pre_create(&alloc, 4, 0, 0, 0);
      bf.pre_create(&alloc, 4, 0, 0, 0);
      ri.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
      gi.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
      bi.pre_create(&alloc, 4, KDU_LINE_BUF_SHORTS, 0, 0);
      alloc.finalize(kdu_core::kdu_codestream());
      rf.create();
      gf.create();
      bf.create();
      ri.create();
      gi.create();
      bi.create();
      for (int i = 0; i < 4; ++i) {
        rf.get_buf32()[i].fval = -1.0f;
        gf.get_buf32()[i].fval = -1.0f;
        bf.get_buf32()[i].fval = -1.0f;
        ri.get_buf16()[i].ival = -20000;
        gi.get_buf16()[i].ival = -20000;
        bi.get_buf16()[i].ival = -20000;
      }
      EXPECT_TRUE(conv.convert_rgb(rf, gf, bf));
      EXPECT_TRUE(conv.convert_rgb(ri, gi, bi));
    }

    xt.write_headers();
    auto* s = ct.open_stream();
    CompressMinimalCodestream(*s, 4, 4, 3);
    s->close();
    xt.close();
    ft.close();

    MemTarget mt_err;
    kdu_supp::jp2_family_tgt ft_err;
    ft_err.open(&mt_err);
    kdu_supp::jpx_target xt_err;
    xt_err.open(&ft_err);
    auto l_err = xt_err.add_layer();
    EXPECT_THROW(l_err.access_channels().init(65537), kdu_core::kdu_exception);
    xt_err.close();
    ft_err.close();
  }

  // 2. Allocator overflow/small-free, open_as negative cont_length &
  // load_in_memory ext_buf,
  //    dref_permissions pattern errors, and palette float precision/exponent
  //    downshift.
  {
    MemTarget mt;
    kdu_supp::jp2_family_tgt ft;
    ft.open(&mt);
    // Write small JP2 with resolution + palette float branches.
    kdu_supp::jp2_target jt;
    jt.open(&ft);
    kdu_core::siz_params siz;
    siz.set(Sdims, 0, 0, 4);
    siz.set(Sdims, 0, 1, 4);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    jt.access_dimensions().init(&siz);
    jt.access_colour().init(kdu_supp::JP2_sLUM_SPACE);
    jt.access_resolution().init(1.0f);
    jt.access_resolution().set_resolution(72.0f, true);
    jt.write_header();
    jt.open_codestream();
    CompressMinimalCodestream(jt, 4, 4, 1);
    jt.close();
    ft.close();

    // Palette float precision/exponent downshift (independent target).
    MemTarget mt_pal;
    kdu_supp::jp2_family_tgt ft_pal;
    ft_pal.open(&mt_pal);
    kdu_supp::jp2_target jt_pal;
    jt_pal.open(&ft_pal);
    jt_pal.access_dimensions().init(&siz);
    jt_pal.access_colour().init(kdu_supp::JP2_sLUM_SPACE);
    auto pal = jt_pal.access_palette();
    pal.init(2, 4);
    kdu_int32 lut_u[4] = {0, 1, 100, 1000};
    pal.set_lut(0, lut_u, 32, false);
    pal.set_lut(1, lut_u, 32, true);
    float f_out[4] = {};
    EXPECT_TRUE(pal.get_lut(0, f_out, JP2_CHANNEL_FORMAT_FLOAT, 6));
    EXPECT_TRUE(pal.get_lut(1, f_out, JP2_CHANNEL_FORMAT_FLOAT, 9));
    jt_pal.close();
    ft_pal.close();

    // dref_permissions edge patterns ({bad and ./**a).
    kdu_supp::jp2_dref_permissions perms;
    perms.init("a.jp2", "{bad:./**a");
    EXPECT_FALSE(perms.can_access("./foo.jp2"));
    perms.init("a.jp2", "./**a");
    EXPECT_TRUE(perms.can_access("./xa"));

    // Non-seekable streaming source to exercise read_header paths.
    class ChunkedStreamSource : public kdu_core::kdu_compressed_source {
     public:
      ChunkedStreamSource(const std::vector<kdu_byte>& data, size_t limit)
          : data_(data), limit_(std::min(limit, data.size())) {}
      int get_capabilities() override { return KDU_SOURCE_CAP_SEQUENTIAL; }
      int read(kdu_byte* buf, int num_bytes) override {
        size_t avail = (pos_ < limit_) ? (limit_ - pos_) : 0;
        size_t n = std::min<size_t>(avail, static_cast<size_t>(num_bytes));
        if (n > 0) {
          std::memcpy(buf, data_.data() + pos_, n);
          pos_ += n;
        }
        return static_cast<int>(n);
      }

     private:
      const std::vector<kdu_byte>& data_;
      size_t pos_ = 0;
      size_t limit_ = 0;
    };

    ChunkedStreamSource csrc_trunc(mt.data(), 18);
    kdu_supp::jp2_family_src fs_trunc;
    fs_trunc.open(&csrc_trunc);
    kdu_supp::jp2_source js_trunc;
    EXPECT_TRUE(js_trunc.open(&fs_trunc));
    EXPECT_EQ(js_trunc.read_header(true), -2);
    js_trunc.close();
    fs_trunc.close();

    ChunkedStreamSource csrc_full(mt.data(), mt.data().size());
    kdu_supp::jp2_family_src fs_full;
    fs_full.open(&csrc_full);
    kdu_supp::jp2_source js_full;
    EXPECT_TRUE(js_full.open(&fs_full));
    EXPECT_EQ(js_full.read_header(true), 1);
    js_full.close();
    fs_full.close();

    // open_as negative cont_length + load_in_memory external_buffer.
    MemSource ms2(mt.data());
    kdu_supp::jp2_family_src fs2;
    fs2.open(&ms2);
    kdu_supp::jp2_input_box ibox;
    ASSERT_TRUE(ibox.open(&fs2));
    kdu_supp::jp2_locator loc = ibox.get_locator();
    kdu_supp::jp2_input_box box_oa;
    EXPECT_TRUE(box_oa.open_as(jp2_uuid_4cc, &fs2, loc, loc, -5));
    kdu_byte ext_buf[64] = {};
    EXPECT_TRUE(box_oa.load_in_memory(64, ext_buf, 64));
    box_oa.close();
    ibox.close();
    fs2.close();
  }

  // 3. Malformed cmap boxes (duplicate cmap & > J2_MAX_CHANNELS).
  {
    auto make_box = [](kdu_uint32 type, const std::vector<kdu_byte>& body) {
      std::vector<kdu_byte> out;
      AppendBe32(out, static_cast<kdu_uint32>(8 + body.size()));
      AppendBe32(out, type);
      out.insert(out.end(), body.begin(), body.end());
      return out;
    };
    auto build_jp2 = [&](const std::vector<kdu_byte>& jp2h_body) {
      std::vector<kdu_byte> f;
      auto sig = make_box(jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
      auto ftyp = make_box(jp2_file_type_4cc, {'j', 'p', '2', ' ', 0, 0, 0, 0,
                                               'j', 'p', '2', ' '});
      auto jp2h = make_box(jp2_header_4cc, jp2h_body);
      f.insert(f.end(), sig.begin(), sig.end());
      f.insert(f.end(), ftyp.begin(), ftyp.end());
      f.insert(f.end(), jp2h.begin(), jp2h.end());
      return f;
    };
    std::vector<kdu_byte> ihdr_body(14, 0);
    WriteBe32At(ihdr_body, 0, 4);
    WriteBe32At(ihdr_body, 4, 4);
    ihdr_body[9] = 1;
    ihdr_body[10] = 7;
    ihdr_body[11] = 7;
    auto ihdr = make_box(jp2_image_header_4cc, ihdr_body);
    auto colr = make_box(jp2_colour_4cc, {1, 0, 0, 0, 0, 0, 17});
    auto cmap_ok = make_box(jp2_component_mapping_4cc, {0, 0, 0, 0});

    // Duplicate cmap throws.
    std::vector<kdu_byte> dup_body;
    dup_body.insert(dup_body.end(), ihdr.begin(), ihdr.end());
    dup_body.insert(dup_body.end(), colr.begin(), colr.end());
    dup_body.insert(dup_body.end(), cmap_ok.begin(), cmap_ok.end());
    dup_body.insert(dup_body.end(), cmap_ok.begin(), cmap_ok.end());
    auto f_dup = build_jp2(dup_body);
    MemSource ms_dup(f_dup);
    kdu_supp::jp2_family_src fs_dup;
    fs_dup.open(&ms_dup);
    kdu_supp::jp2_source js_dup;
    EXPECT_TRUE(js_dup.open(&fs_dup));
    EXPECT_THROW(js_dup.read_header(), kdu_core::kdu_exception);
    js_dup.close();
    fs_dup.close();

    // Oversized cmap (> 4 * 65536 bytes) throws.
    std::vector<kdu_byte> huge_cmap_body(4 * 65537, 0);
    auto cmap_huge = make_box(jp2_component_mapping_4cc, huge_cmap_body);
    std::vector<kdu_byte> over_body;
    over_body.insert(over_body.end(), ihdr.begin(), ihdr.end());
    over_body.insert(over_body.end(), colr.begin(), colr.end());
    over_body.insert(over_body.end(), cmap_huge.begin(), cmap_huge.end());
    auto f_over = build_jp2(over_body);
    MemSource ms_over(f_over);
    kdu_supp::jp2_family_src fs_over;
    fs_over.open(&ms_over);
    kdu_supp::jp2_source js_over;
    EXPECT_TRUE(js_over.open(&fs_over));
    EXPECT_THROW(js_over.read_header(), kdu_core::kdu_exception);
    js_over.close();
    fs_over.close();

    auto expect_header_throws = [&](const std::vector<kdu_byte>& jp2h_body) {
      auto bytes = build_jp2(jp2h_body);
      MemSource ms_bad(bytes);
      kdu_supp::jp2_family_src fs_bad;
      fs_bad.open(&ms_bad);
      kdu_supp::jp2_source js_bad;
      EXPECT_TRUE(js_bad.open(&fs_bad));
      EXPECT_THROW(js_bad.read_header(), kdu_core::kdu_exception);
      js_bad.close();
      fs_bad.close();
    };

    // cmap without pclr throws (KDU_ERROR 69).
    expect_header_throws([&] {
      std::vector<kdu_byte> b;
      b.insert(b.end(), ihdr.begin(), ihdr.end());
      b.insert(b.end(), colr.begin(), colr.end());
      b.insert(b.end(), cmap_ok.begin(), cmap_ok.end());
      return b;
    }());

    // pclr + cmap with out-of-range component_idx throws (KDU_ERROR 70).
    auto pclr = make_box(jp2_palette_4cc, {0, 1, 1, 7, 42});
    auto cmap_oob = make_box(jp2_component_mapping_4cc, {0, 9, 0, 0});
    expect_header_throws([&] {
      std::vector<kdu_byte> b;
      b.insert(b.end(), ihdr.begin(), ihdr.end());
      b.insert(b.end(), colr.begin(), colr.end());
      b.insert(b.end(), pclr.begin(), pclr.end());
      b.insert(b.end(), cmap_oob.begin(), cmap_oob.end());
      return b;
    }());

    // Duplicate (typ, asoc) in cdef throws (KDU_ERROR 76).
    std::vector<kdu_byte> ihdr2_body = ihdr_body;
    ihdr2_body[9] = 2;
    auto ihdr2 = make_box(jp2_image_header_4cc, ihdr2_body);
    auto cdef_dup = make_box(jp2_channel_definition_4cc,
                             {0, 2, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1});
    expect_header_throws([&] {
      std::vector<kdu_byte> b;
      b.insert(b.end(), ihdr2.begin(), ihdr2.end());
      b.insert(b.end(), colr.begin(), colr.end());
      b.insert(b.end(), cdef_dup.begin(), cdef_dup.end());
      return b;
    }());

    // More colour channels in cdef than colour space supports throws (KDU_ERROR
    // 81).
    auto cdef_too_many = make_box(jp2_channel_definition_4cc,
                                  {0, 2, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 2});
    expect_header_throws([&] {
      std::vector<kdu_byte> b;
      b.insert(b.end(), ihdr2.begin(), ihdr2.end());
      b.insert(b.end(), colr.begin(), colr.end());
      b.insert(b.end(), cdef_too_many.begin(), cdef_too_many.end());
      return b;
    }());

    // Truncated ICC colr box, short vendor colr box, and truncated CIELab EP
    // params.
    auto colr_trunc_icc =
        make_box(jp2_colour_4cc, {2, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0});
    expect_header_throws([&] {
      std::vector<kdu_byte> b;
      b.insert(b.end(), ihdr.begin(), ihdr.end());
      b.insert(b.end(), colr_trunc_icc.begin(), colr_trunc_icc.end());
      return b;
    }());

    auto colr_short_vendor = make_box(jp2_colour_4cc, {4, 0, 0, 1, 2, 3, 4});
    expect_header_throws([&] {
      std::vector<kdu_byte> b;
      b.insert(b.end(), ihdr.begin(), ihdr.end());
      b.insert(b.end(), colr_short_vendor.begin(), colr_short_vendor.end());
      return b;
    }());

    auto colr_trunc_lab =
        make_box(jp2_colour_4cc, {1, 0, 0, 0, 0, 0, 14, 0, 0, 1, 0});
    expect_header_throws([&] {
      std::vector<kdu_byte> b;
      b.insert(b.end(), ihdr.begin(), ihdr.end());
      b.insert(b.end(), colr_trunc_lab.begin(), colr_trunc_lab.end());
      return b;
    }());

    // jp2_family_src::open double-open & zero-capability source errors.
    MemSource ms_reopen(f_over);
    kdu_supp::jp2_family_src fs_reopen;
    fs_reopen.open(&ms_reopen);
    EXPECT_THROW(fs_reopen.open(&ms_reopen), kdu_core::kdu_exception);
    fs_reopen.close();

    struct ZeroCapSource : public kdu_core::kdu_compressed_source {
      int get_capabilities() override { return 0; }
      int read(kdu_byte*, int) override { return 0; }
    } zero_cap;
    kdu_supp::jp2_family_src fs_zcap;
    EXPECT_THROW(fs_zcap.open(&zero_cap), kdu_core::kdu_exception);
    fs_zcap.close();

    // High resolution exponents (L354, L368) & dimensions unknown-compat / OOB.
    {
      MemTarget mt_res;
      kdu_supp::jp2_family_tgt ft_res;
      ft_res.open(&mt_res);
      kdu_supp::jp2_target jt_res;
      jt_res.open(&ft_res);
      auto dims = jt_res.access_dimensions();
      dims.init(kdu_coords(4, 4), 1, false);
      EXPECT_TRUE(dims.is_jp2_compatible(true));
      EXPECT_FALSE(dims.is_jph_compatible(false));
      EXPECT_EQ(dims.get_bit_depth(-1), 0);
      EXPECT_FALSE(dims.get_signed(-1));
      dims.set_precision(0, 8, false);
      jt_res.access_colour().init(kdu_supp::JP2_sLUM_SPACE);
      auto res = jt_res.access_resolution();
      res.init(1.0e8f);
      res.set_resolution(1.0e8f, true);
      jt_res.write_header();
      jt_res.open_codestream();
      CompressMinimalCodestream(jt_res, 4, 4, 1);
      jt_res.close();
      ft_res.close();
    }

    // jp2_dref_permissions trailing backslash & wildcard middle mismatch.
    {
      kdu_supp::jp2_dref_permissions perms;
      perms.init("a.jp2", "a\\");
      EXPECT_FALSE(perms.can_access("ab"));
      perms.init("a.jp2", "./**a");
      EXPECT_FALSE(perms.can_access("./x/a"));
    }

    // jp2_family_src missing file, set_access_permissions before open,
    // double-open file, and jp2_input_box closed/misaligned/open_as checks.
    {
      kdu_supp::jp2_family_src fs_pre;
      fs_pre.set_access_permissions("./*");
      EXPECT_THROW(fs_pre.open("/nonexistent_kakadu_dir_xyz/missing.jp2"),
                   kdu_core::kdu_exception);
      fs_pre.close();

      kdu_supp::jp2_input_box closed_box;
      EXPECT_EQ(closed_box.get_box_bytes(), 0);
      EXPECT_FALSE(closed_box.is_complete());
      EXPECT_THROW(closed_box.seek(0), kdu_core::kdu_exception);
      EXPECT_FALSE(closed_box.load_in_memory(16));
      kdu_byte dummy = 0;
      EXPECT_THROW(closed_box.read(&dummy, 1), kdu_core::kdu_exception);

      auto three_byte_box = make_box(jp2_uuid_4cc, {1, 2, 3});
      MemSource ms_3(three_byte_box);
      kdu_supp::jp2_family_src fs_3;
      fs_3.open(&ms_3);
      kdu_supp::jp2_input_box box_3;
      ASSERT_TRUE(box_3.open(&fs_3));
      EXPECT_EQ(box_3.read(&dummy, 0), 0);
      kdu_uint32 u32 = 0;
      EXPECT_FALSE(box_3.read(u32));  // Leaves partial_word_bytes == 3.
      kdu_uint16 u16 = 0;
      EXPECT_THROW(box_3.read(u16), kdu_core::kdu_exception);
      box_3.close();

      // open_as with cont_locator < box_locator clamps header length to 0.
      kdu_supp::jp2_locator loc_end, loc_start;
      loc_end.set_file_pos(8);
      loc_start.set_file_pos(0);
      kdu_supp::jp2_input_box box_neg;
      EXPECT_TRUE(box_neg.open_as(jp2_uuid_4cc, &fs_3, loc_end, loc_start, 4));
      box_neg.close();
      fs_3.close();
    }

    // jp2_family_tgt double-open, missing output file, simulation small flush,
    // and write_header_last edge cases.
    {
      kdu_supp::jp2_family_tgt ft_bad;
      EXPECT_THROW(ft_bad.open("/nonexistent_kakadu_dir_xyz/out.jp2"),
                   kdu_core::kdu_exception);
      ft_bad.close();

      MemTarget mt_double;
      kdu_supp::jp2_family_tgt ft_sim;
      ft_sim.open(kdu_long{0});
      EXPECT_THROW(ft_sim.open(&mt_double), kdu_core::kdu_exception);
      EXPECT_THROW(ft_sim.open("/nonexistent_kakadu_dir_xyz/out.jp2"),
                   kdu_core::kdu_exception);
      kdu_supp::jp2_output_box ob_sim;
      ob_sim.open(&ft_sim, jp2_uuid_4cc, false);
      ob_sim.write_header_last();  // Buffers small (< 256) write -> align_probe
                                   // == 1.
      kdu_byte payload[16] = {};
      ob_sim.write(payload, 16);
      ob_sim.close();
      ft_sim.close();

      MemTarget mt_hdr;
      kdu_supp::jp2_family_tgt ft_hdr;
      ft_hdr.open(&mt_hdr);
      kdu_supp::jp2_output_box ob_hdrless;
      ob_hdrless.open(&ft_hdr, jp2_uuid_4cc, false, true);
      ob_hdrless.write_header_last();  // headerless early return.
      ob_hdrless.close();
      ft_hdr.close();

      kdu_supp::jp2_output_box ob_nulltgt;
      ob_nulltgt.open(static_cast<kdu_supp::jp2_family_tgt*>(nullptr),
                      jp2_uuid_4cc, false);
      ob_nulltgt.write_header_last();  // No tgt/super_box early return.
      ob_nulltgt.close();
    }

    // jp2_channels::copy stream_idx >= stream_map_elts clamp &
    // double jp2_colour::init(uuid) + vendor converter init.
    {
      MemTarget mt1, mt2;
      kdu_supp::jp2_family_tgt ft1, ft2;
      ft1.open(&mt1);
      ft2.open(&mt2);
      kdu_supp::jpx_target xt1, xt2;
      xt1.open(&ft1);
      xt2.open(&ft2);
      auto l1 = xt1.add_layer();
      auto ch1 = l1.access_channels();
      ch1.init(1);
      ch1.set_colour_mapping(0, 0, -1, 5);
      auto col1 = l1.add_colour();
      kdu_byte u1[16] = {3};
      kdu_byte d1[4] = {1, 2, 3, 4};
      col1.init(u1, 4, d1);
      EXPECT_THROW(col1.init(u1, 4, d1), kdu_core::kdu_exception);
      kdu_supp::jp2_colour_converter conv_v;
      EXPECT_FALSE(conv_v.init(col1, false));

      auto l2 = xt2.add_layer();
      int smap[2] = {0, 0};
      l2.access_channels().copy(ch1, smap, 2, 0);
      xt2.close();
      ft2.close();
      xt1.close();
      ft1.close();
    }

    // Bad ICC XYZ tag offset / wrong tag type & truncated ICC colr box read.
    {
      std::vector<kdu_byte> bad_xyz(20, 0);
      bad_xyz[0] = 'b';
      bad_xyz[1] = 'a';
      bad_xyz[2] = 'd';
      bad_xyz[3] = ' ';
      auto p_bad_type =
          BuildIccProfile(0x52474220, 0x58595A20,
                          {{0x7258595A, bad_xyz},
                           MakeXyzTag(0x6758595A, 0.385, 0.717, 0.097),
                           MakeXyzTag(0x6258595A, 0.143, 0.061, 0.714),
                           MakeXyzTag(0x77747074, 0.964, 1.0, 0.825),
                           MakeCurvGammaTag(0x72545243, 2.2),
                           MakeCurvGammaTag(0x67545243, 2.2),
                           MakeCurvGammaTag(0x62545243, 2.2)});
      std::vector<kdu_byte> colr_body = {2, 0, 0};
      colr_body.insert(colr_body.end(), p_bad_type.begin(), p_bad_type.end());
      auto colr_bad_xyz = make_box(jp2_colour_4cc, colr_body);
      expect_header_throws([&] {
        std::vector<kdu_byte> b;
        b.insert(b.end(), ihdr.begin(), ihdr.end());
        b.insert(b.end(), colr_bad_xyz.begin(), colr_bad_xyz.end());
        return b;
      }());

      // Out-of-bounds tag offset in ICC header directory.
      auto p_oob = BuildIccProfile(
          0x52474220, 0x58595A20,
          {MakeXyzTag(0x7258595A, 0.436, 0.222, 0.014),
           MakeXyzTag(0x6758595A, 0.385, 0.717, 0.097),
           MakeXyzTag(0x6258595A, 0.143, 0.061, 0.714),
           MakeXyzTag(0x77747074, 0.964, 1.0, 0.825),
           MakeCurvGammaTag(0x72545243, 2.2), MakeCurvGammaTag(0x67545243, 2.2),
           MakeCurvGammaTag(0x62545243, 2.2)});
      WriteBe32At(p_oob, 132 + 4, 999999);  // rXYZ offset past profile end.
      std::vector<kdu_byte> colr_oob_body = {2, 0, 0};
      colr_oob_body.insert(colr_oob_body.end(), p_oob.begin(), p_oob.end());
      auto colr_oob_xyz = make_box(jp2_colour_4cc, colr_oob_body);
      expect_header_throws([&] {
        std::vector<kdu_byte> b;
        b.insert(b.end(), ihdr.begin(), ihdr.end());
        b.insert(b.end(), colr_oob_xyz.begin(), colr_oob_xyz.end());
        return b;
      }());

      // Truncated box at EOF inside meth=2 colr triggers short colr->read.
      std::vector<kdu_byte> trunc_colr_box = {0, 0, 0, 100, 'c', 'o', 'l', 'r',
                                              2, 0, 0, 0,   0,   0,   40};
      std::vector<kdu_byte> jp2h_trunc;
      jp2h_trunc.insert(jp2h_trunc.end(), ihdr.begin(), ihdr.end());
      jp2h_trunc.insert(jp2h_trunc.end(), trunc_colr_box.begin(),
                        trunc_colr_box.end());
      expect_header_throws(jp2h_trunc);
    }
  }
}

}  // namespace
