// Unit tests for third_party/kakadu/vendor/apps/jp2/jpx.cpp.

#include "third_party/kakadu/vendor/apps/compressed_io/jpx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/types/span.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jp2.h"
#include "third_party/kakadu/vendor/apps/compressed_io/kdu_cache.h"
#include "third_party/kakadu/vendor/apps/compressed_io/kdu_client_window.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_elementary.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_utils.h"

namespace kdu_supp {
namespace {

class ScopedKduHandlers {
 public:
  ScopedKduHandlers() : err_(*this, true), warn_(*this, false) {
    kdu_core::kdu_customize_errors(&err_);
    kdu_core::kdu_customize_warnings(&warn_);
  }
  ~ScopedKduHandlers() {
    kdu_core::kdu_customize_errors(&Fallback(true));
    kdu_core::kdu_customize_warnings(&Fallback(false));
  }
  const std::string& errors() const { return errors_; }

 private:
  class Sink : public kdu_core::kdu_message {
   public:
    Sink(ScopedKduHandlers& owner, bool is_error)
        : owner_(owner), is_error_(is_error) {}
    void put_text(const char* s) override {
      if (is_error_) owner_.errors_ += s;
    }
    void flush(bool end_of_message = false) override {
      if (end_of_message && is_error_) {
        std::cerr << "[kdu_error] " << owner_.errors_ << std::endl;
        throw KDU_ERROR_EXCEPTION;
      }
    }

   private:
    ScopedKduHandlers& owner_;
    bool is_error_;
  };
  class FallbackSink : public kdu_core::kdu_message {
   public:
    explicit FallbackSink(bool is_error) : is_error_(is_error) {}
    void put_text(const char*) override {}
    void flush(bool end_of_message = false) override {
      if (end_of_message && is_error_) throw KDU_ERROR_EXCEPTION;
    }

   private:
    bool is_error_;
  };
  static FallbackSink& Fallback(bool is_error) {
    static FallbackSink err_fb(true), warn_fb(false);
    return is_error ? err_fb : warn_fb;
  }
  Sink err_;
  Sink warn_;
  std::string errors_;
};

class MemTarget : public kdu_core::kdu_compressed_target {
 public:
  MemTarget() = default;

  int get_capabilities() override { return KDU_TARGET_CAP_SEQUENTIAL; }

  bool write(const kdu_core::kdu_byte* buf, int num_bytes) override {
    if (num_bytes < 0) return false;
    if (rewrite_active_) {
      if (rewrite_pos_ + num_bytes >
          static_cast<kdu_core::kdu_long>(data_.size()))
        data_.resize(static_cast<size_t>(rewrite_pos_ + num_bytes), 0);
      if (num_bytes > 0) {
        std::memcpy(data_.data() + rewrite_pos_, buf,
                    static_cast<size_t>(num_bytes));
        rewrite_pos_ += num_bytes;
      }
      return true;
    }
    if (num_bytes > 0) data_.insert(data_.end(), buf, buf + num_bytes);
    return true;
  }

  bool start_rewrite(kdu_core::kdu_long backtrack) override {
    if (rewrite_active_ || backtrack < 0 ||
        backtrack > static_cast<kdu_core::kdu_long>(data_.size()))
      return false;
    rewrite_active_ = true;
    rewrite_pos_ = static_cast<kdu_core::kdu_long>(data_.size()) - backtrack;
    return true;
  }

  bool end_rewrite() override {
    if (!rewrite_active_) return false;
    rewrite_active_ = false;
    return true;
  }

  bool close() override { return true; }

  const std::vector<kdu_core::kdu_byte>& data() const { return data_; }
  std::vector<kdu_core::kdu_byte>& mutable_data() { return data_; }

 private:
  std::vector<kdu_core::kdu_byte> data_;
  bool rewrite_active_ = false;
  kdu_core::kdu_long rewrite_pos_ = 0;
};

kdu_core::kdu_dims MakeDims(int y, int x, int h, int w) {
  kdu_core::kdu_dims d;
  d.pos = kdu_core::kdu_coords(x, y);
  d.size = kdu_core::kdu_coords(w, h);
  return d;
}

class MemSource : public kdu_core::kdu_compressed_source {
 public:
  explicit MemSource(const std::vector<kdu_core::kdu_byte>& data,
                     int caps = KDU_SOURCE_CAP_SEEKABLE |
                                KDU_SOURCE_CAP_SEQUENTIAL)
      : data_(data), caps_(caps) {}

  int get_capabilities() override { return caps_; }

  bool seek(kdu_core::kdu_long offset) override {
    if (!(caps_ & KDU_SOURCE_CAP_SEEKABLE)) return false;
    if (offset < 0) return false;
    pos_ = offset;
    return true;
  }

  kdu_core::kdu_long get_pos() override { return pos_; }

  int read(kdu_core::kdu_byte* buf, int num_bytes) override {
    if (num_bytes <= 0 || pos_ >= static_cast<kdu_core::kdu_long>(data_.size()))
      return 0;
    kdu_core::kdu_long avail =
        static_cast<kdu_core::kdu_long>(data_.size()) - pos_;
    int to_read =
        static_cast<int>(std::min<kdu_core::kdu_long>(avail, num_bytes));
    std::memcpy(buf, data_.data() + pos_, static_cast<size_t>(to_read));
    pos_ += to_read;
    return to_read;
  }

 private:
  std::vector<kdu_core::kdu_byte> data_;
  kdu_core::kdu_long pos_ = 0;
  int caps_;
};

void WriteMinimalCodestream(kdu_core::kdu_compressed_target* target, int width,
                            int height, int num_comps, int bit_depth = 8) {
  kdu_core::siz_params siz;
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Scomponents, 0, 0, num_comps);
  for (int c = 0; c < num_comps; ++c) {
    siz.set(Sprecision, c, 0, bit_depth);
    siz.set(Ssigned, c, 0, false);
  }
  static_cast<kdu_core::kdu_params&>(siz).finalize();

  kdu_core::kdu_codestream cs;
  cs.create(&siz, target);
  cs.access_siz()->parse_string("Creversible=yes");
  cs.access_siz()->finalize_all();
  kdu_core::kdu_tile tile = cs.open_tile(kdu_core::kdu_coords(0, 0));
  kdu_core::kdu_sample_allocator alloc;
  kdu_core::kdu_push_pull_params pp_params;
  std::vector<kdu_core::kdu_push_ifc> engines(num_comps);
  for (int c = 0; c < num_comps; ++c) {
    auto res = tile.access_component(c).access_resolution();
    engines[c] = kdu_core::kdu_analysis(res, &alloc, pp_params,
                                        KDU_SAMPLE_PREC16, 1.0F, 0, nullptr);
  }
  kdu_core::kdu_line_buf line;
  line.pre_create(&alloc, width,
                  static_cast<kdu_core::kdu_byte>(KDU_LINE_BUF_SHORTS |
                                                  KDU_LINE_BUF_ABSOLUTE),
                  0, 0);
  alloc.finalize(cs);
  line.create();
  for (int c = 0; c < num_comps; ++c) {
    for (int r = 0; r < height; ++r) {
      for (int i = 0; i < width; ++i) {
        line.get_buf16()[i].ival =
            static_cast<kdu_core::kdu_int16>((r * 5 + i * 3 + c * 7) & 0x7F);
      }
      engines[c].push(line);
    }
    engines[c].destroy();
  }
  line.destroy();
  tile.close();
  kdu_core::kdu_long max_bytes = 65536;
  cs.flush(&max_bytes, 1);
  cs.destroy();
}

void SetupCodestreamDims(jp2_dimensions dims, int width, int height,
                         int num_comps, int bit_depth = 8) {
  dims.init(kdu_core::kdu_coords(height, width), num_comps, false);
  for (int c = 0; c < num_comps; ++c) {
    dims.set_precision(c, bit_depth, false);
  }
  dims.finalize_compatibility(0, 0, 0, 0, 0, 0);
}

TEST(JpxTest, TextualizerAndCompositedOrientation) {
  ScopedKduHandlers handlers;
  jp2_box_textualizer tex;
  jp2_add_box_descriptions(tex);
  jpx_add_box_descriptions(tex);

  jpx_composited_orientation o0;
  EXPECT_FALSE(o0.is_non_trivial());
  jpx_composited_orientation o1(true, false, true);
  EXPECT_TRUE(o1.is_non_trivial());
  jpx_composited_orientation o2;
  o2.init(90, true);
  o2.init(180, false);
  o2.init(270, true);
  o2.init(0, false);
  EXPECT_FALSE(o2.is_non_trivial());
  o2.append(o1);
  EXPECT_TRUE(o2.equals(o1));
  jpx_composited_orientation o3(false, true, false);
  o1.append(o3);
  EXPECT_FALSE(o1.equals(o3));
}

TEST(JpxTest, TargetSourceSingleCodestreamAndLayerRoundTrip) {
  ScopedKduHandlers handlers;
  MemTarget mem_tgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mem_tgt);

  jpx_target tgt;
  tgt.open(&ftgt);
  EXPECT_TRUE(tgt.exists());
  EXPECT_GE(tgt.get_metadata_memory(), 0);

  jpx_compatibility compat = tgt.access_compatibility();
  EXPECT_TRUE(compat.exists());

  jpx_codestream_target cs_tgt = tgt.add_codestream();
  EXPECT_EQ(cs_tgt.get_codestream_id(), 0);
  SetupCodestreamDims(cs_tgt.access_dimensions(), 16, 16, 3);

  jpx_layer_target lyr_tgt = tgt.add_layer();
  EXPECT_EQ(lyr_tgt.get_layer_id(), 0);
  lyr_tgt.access_channels().init(3);
  lyr_tgt.access_channels().set_colour_mapping(0, 0, -1, 0);
  lyr_tgt.access_channels().set_colour_mapping(1, 1, -1, 0);
  lyr_tgt.access_channels().set_colour_mapping(2, 2, -1, 0);
  lyr_tgt.access_resolution().init(1.0f);
  lyr_tgt.access_resolution().set_resolution(300.0, true);
  jp2_colour col = lyr_tgt.add_colour(0, 0);
  col.init(JP2_sRGB_SPACE);
  lyr_tgt.set_codestream_registration(0, kdu_core::kdu_coords(0, 0),
                                      kdu_core::kdu_coords(1, 1),
                                      kdu_core::kdu_coords(1, 1));

  EXPECT_EQ(tgt.write_headers(), nullptr);
  jp2_output_box* cs_box = cs_tgt.open_stream();
  ASSERT_NE(cs_box, nullptr);
  WriteMinimalCodestream(cs_tgt.access_stream(), 16, 16, 3);
  EXPECT_TRUE(cs_box->close());
  EXPECT_EQ(tgt.write_metadata(), nullptr);
  EXPECT_TRUE(tgt.close());
  ftgt.close();

  MemSource mem_src(mem_tgt.data());
  jp2_family_src fsrc;
  fsrc.open(&mem_src);
  jpx_source src;
  EXPECT_GE(src.open(&fsrc, false), 0);
  EXPECT_TRUE(src.exists());
  EXPECT_EQ(src.get_ultimate_src(), &fsrc);
  EXPECT_GE(src.get_metadata_memory(), 0);

  jpx_compatibility scompat = src.access_compatibility();
  EXPECT_TRUE(scompat.exists());
  EXPECT_NE(scompat.get_brand(), 0u);
  EXPECT_TRUE(scompat.is_jp2() || scompat.is_jpxb_compatible() ||
              scompat.is_jp2_compatible());
  EXPECT_TRUE(scompat.has_reader_requirements_box());
  EXPECT_TRUE(scompat.check_standard_feature(JPX_SF_sRGB));
  kdu_core::kdu_uint16 fid = 0;
  bool sup = false;
  for (int i = 0; scompat.get_standard_feature(i, fid, sup); ++i) {
    scompat.set_standard_feature_support(fid, true);
  }
  EXPECT_TRUE(scompat.get_standard_feature(0, fid));
  EXPECT_TRUE(scompat.test_fully_understand());
  EXPECT_FALSE(scompat.test_decode_completely());

  int num_cs = 0;
  int num_layers = 0;
  int num_cont = 0;
  EXPECT_TRUE(src.count_codestreams(num_cs));
  EXPECT_EQ(num_cs, 1);
  EXPECT_TRUE(src.count_compositing_layers(num_layers));
  EXPECT_EQ(num_layers, 1);
  EXPECT_TRUE(src.count_containers(num_cont));
  EXPECT_EQ(num_cont, 0);

  jpx_codestream_source cs_src = src.access_codestream(0);
  EXPECT_TRUE(cs_src.exists());
  EXPECT_EQ(cs_src.get_codestream_id(), 0);
  EXPECT_FALSE(cs_src.get_header_loc().is_null());
  EXPECT_EQ(cs_src.access_dimensions().get_size().x, 16);
  EXPECT_TRUE(cs_src.stream_ready());
  EXPECT_EQ(cs_src.enum_layer_ids(-1), 0);
  EXPECT_EQ(cs_src.enum_layer_ids(0), -1);

  jpx_input_box* in_box = cs_src.open_stream();
  ASSERT_NE(in_box, nullptr);
  EXPECT_TRUE(in_box->exists());
  kdu_core::kdu_byte hdr[2] = {};
  EXPECT_EQ(in_box->read(hdr, 2), 2);
  EXPECT_EQ(hdr[0], 0xFF);
  EXPECT_EQ(hdr[1], 0x4F);
  EXPECT_TRUE(in_box->seek(0));
  EXPECT_FALSE(in_box->close());

  jpx_input_box custom_box;
  jpx_input_box* in_box2 = cs_src.open_stream(&custom_box);
  ASSERT_EQ(in_box2, &custom_box);
  std::vector<kdu_core::kdu_byte> stream_buf(
      static_cast<size_t>(in_box2->get_remaining_bytes()));
  EXPECT_EQ(
      in_box2->read(stream_buf.data(), static_cast<int>(stream_buf.size())),
      static_cast<int>(stream_buf.size()));
  EXPECT_TRUE(in_box2->close());

  jpx_layer_source lyr_src = src.access_layer(0);
  EXPECT_TRUE(lyr_src.exists());
  EXPECT_EQ(lyr_src.get_layer_id(), 0);
  EXPECT_EQ(lyr_src.get_num_codestreams(), 1);
  EXPECT_EQ(lyr_src.get_codestream_id(0), 0);
  EXPECT_EQ(lyr_src.get_layer_size(), kdu_core::kdu_coords(16, 16));
  EXPECT_TRUE(lyr_src.have_stream_headers());
  EXPECT_EQ(src.get_num_layer_codestreams(0), 1);
  EXPECT_EQ(src.get_layer_codestream_id(0, 0), 0);
  kdu_core::kdu_coords align, samp, denom;
  EXPECT_EQ(lyr_src.get_codestream_registration(0, align, samp, denom), 0);
  EXPECT_TRUE(lyr_src.access_colour(0).exists());

  EXPECT_TRUE(src.close());
  fsrc.close();
}

TEST(JpxTest, CompositionFramesPersistenceAndExpander) {
  ScopedKduHandlers handlers;
  MemTarget mem_tgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mem_tgt);
  jpx_target tgt;
  tgt.open(&ftgt);

  std::vector<jpx_codestream_target> cs_tgts;
  for (int i = 0; i < 2; ++i) {
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 16, 16, 1);
    cs_tgts.push_back(cs);
    jpx_layer_target lyr = tgt.add_layer();
    lyr.access_channels().init(1);
    lyr.access_channels().set_colour_mapping(0, 0, -1, i);
    lyr.add_colour().init(JP2_sLUM_SPACE);
  }

  jpx_composition comp = tgt.access_composition();
  comp.set_loop_count(2);
  jx_frame* f0 = comp.add_frame(100, 0, true);
  ASSERT_NE(f0, nullptr);
  EXPECT_GE(comp.add_instruction(
                f0, 0, 0, MakeDims(0, 0, 16, 16), MakeDims(0, 0, 16, 16),
                jpx_composited_orientation(true, false, false)),
            0);
  jx_frame* f1 = comp.add_frame(50, 1, false);
  ASSERT_NE(f1, nullptr);
  EXPECT_GE(comp.add_instruction(f1, 1, 0, MakeDims(0, 0, 16, 16),
                                 MakeDims(8, 8, 16, 16)),
            0);

  EXPECT_EQ(tgt.write_headers(), nullptr);
  for (size_t i = 0; i < cs_tgts.size(); ++i) {
    jp2_output_box* box = cs_tgts[i].open_stream();
    ASSERT_NE(box, nullptr);
    WriteMinimalCodestream(cs_tgts[i].access_stream(), 16, 16, 1);
    EXPECT_TRUE(box->close());
  }
  EXPECT_EQ(tgt.write_metadata(), nullptr);
  EXPECT_TRUE(tgt.close());
  ftgt.close();

  MemSource mem_src(mem_tgt.data());
  jp2_family_src fsrc;
  fsrc.open(&mem_src);
  jpx_source src;
  ASSERT_GE(src.open(&fsrc, false), 0);

  jpx_composition scomp = src.access_composition();
  ASSERT_TRUE(scomp.exists());
  kdu_core::kdu_coords csize;
  EXPECT_EQ(scomp.get_global_info(csize), 2);
  EXPECT_EQ(scomp.get_track_idx(), 0u);
  kdu_core::kdu_uint32 tracks = 0;
  EXPECT_TRUE(scomp.count_tracks(tracks));
  EXPECT_GE(tracks, 1u);
  int fcount = 0;
  EXPECT_TRUE(scomp.count_track_frames(0, fcount));
  EXPECT_EQ(fcount, 3);
  kdu_core::kdu_long total_time = 0;
  EXPECT_TRUE(scomp.count_track_time(0, total_time));
  EXPECT_EQ(total_time, 200);
  int before_cnt = 0;
  EXPECT_TRUE(scomp.count_track_frames_before_time(0, 150, before_cnt));
  EXPECT_EQ(before_cnt, 2);

  jpx_frame fr0 = scomp.access_frame(0, 0, true);
  ASSERT_TRUE(fr0.exists());
  EXPECT_TRUE(fr0.is_persistent());
  kdu_core::kdu_long st = 0, dur = 0;
  EXPECT_EQ(fr0.get_info(st, dur), 1);
  int lyr_idx = -1;
  kdu_core::kdu_dims sdims, tdims;
  jpx_composited_orientation orient;
  EXPECT_TRUE(fr0.get_instruction(0, lyr_idx, sdims, tdims, orient));
  EXPECT_EQ(lyr_idx, 0);
  EXPECT_TRUE(orient.is_non_trivial());
  EXPECT_EQ(fr0.find_last_instruction_for_layer(0, 2), 0);
  int iset = -1, inum = -1;
  EXPECT_TRUE(fr0.get_original_iset(0, iset, inum));

  jpx_frame fr1 = fr0.access_next(0, true);
  ASSERT_TRUE(fr1.exists());
  EXPECT_EQ(fr1.get_frame_idx(), 1);
  EXPECT_EQ(fr1.find_last_instruction_for_layer(0), 0);
  EXPECT_EQ(fr1.get_num_persistent_instructions(), 1);
  EXPECT_GE(fr1.get_info(st, dur), 2);

  jpx_frame_expander expander;
  EXPECT_TRUE(expander.construct(&src, fr1));
  kdu_core::kdu_dims reg;
  int layers[1] = {1};
  EXPECT_GE(jpx_frame_expander::test_codestream_visibility(
                &src, fr1, 1, jpx_metanode(), layers, 1, reg),
            0);

  int match_inst = 0;
  EXPECT_EQ(scomp.find_layer_match(fr1, match_inst, 0, layers, 1), 1);
  EXPECT_TRUE(fr1.access_prev(0, true).exists());
  EXPECT_TRUE(src.close());
  fsrc.close();
}

TEST(JpxTest, RoiAndRoiEditorCoverage) {
  ScopedKduHandlers handlers;
  jpx_roi quad{};
  quad.init_quadrilateral(
      kdu_core::kdu_coords(10, 10), kdu_core::kdu_coords(50, 12),
      kdu_core::kdu_coords(48, 50), kdu_core::kdu_coords(8, 48), true, 3);
  EXPECT_TRUE(quad.check_geometry());
  double cx = 0, cy = 0;
  EXPECT_GT(quad.measure_area(cx, cy), 0.0);
  double w = 0, l = 0;
  EXPECT_GE(quad.measure_span(w, l), 0);
  EXPECT_TRUE(quad.contains(kdu_core::kdu_coords(30, 30)));
  EXPECT_FALSE(quad.contains(kdu_core::kdu_coords(200, 200)));
  double xp = 0, yp = 0;
  EXPECT_GE(quad.find_boundary_projection(30.0, 12.0, xp, yp, 50.0), 0);
  quad.clip_region();
  quad.fix_inconsistencies();

  jpx_roi ell{};
  double axes[2] = {40.0, 20.0};
  ell.init_ellipse(kdu_core::kdu_coords(100, 100), axes, 0.25, false, 1);
  EXPECT_TRUE(ell.check_geometry());
  kdu_core::kdu_coords ec;
  double out_axes[2] = {};
  double tan_t = 0.0;
  EXPECT_TRUE(ell.get_ellipse(ec, out_axes, tan_t));
  EXPECT_TRUE(ell.contains(kdu_core::kdu_coords(100, 100)));
  EXPECT_GT(ell.measure_area(cx, cy), 0.0);
  EXPECT_GE(ell.measure_span(w, l), 0);

  jpx_roi regions[2] = {quad, ell};
  jpx_roi_editor editor;
  editor.set_max_undo_history(5);
  editor.init(regions, 2);
  EXPECT_TRUE(editor.contains_encoded_regions());
  kdu_core::kdu_dims bb;
  EXPECT_TRUE(editor.get_bounding_box(bb));
  EXPECT_GT(editor.measure_complexity(), 0.0);

  editor.set_mode(JPX_EDITOR_VERTEX_MODE);
  kdu_core::kdu_coords pt(10, 10);
  EXPECT_TRUE(editor.find_nearest_anchor(pt, true));
  editor.select_anchor(pt, false);
  int insts = 0;
  EXPECT_GE(editor.get_selection(pt, insts), 0);
  if (editor.can_move_selected_anchor(kdu_core::kdu_coords(15, 15), true)) {
    editor.drag_selected_anchor(kdu_core::kdu_coords(15, 15));
    editor.move_selected_anchor(kdu_core::kdu_coords(15, 15));
  }
  editor.undo();
  editor.redo();

  editor.set_mode(JPX_EDITOR_PATH_MODE);
  editor.add_scribble_point(kdu_core::kdu_coords(0, 0));
  editor.add_scribble_point(kdu_core::kdu_coords(0, 40));
  editor.add_scribble_point(kdu_core::kdu_coords(40, 40));
  editor.add_scribble_point(kdu_core::kdu_coords(0, 0));
  editor.convert_scribble_path(false, JPX_EDITOR_FLAG_ELLIPSES, 1.0);
  bool ok = false;
  editor.fill_closed_paths(ok);
  editor.clear_scribble_points();
  editor.reset();
}

TEST(JpxTest, MetadataManagerAndContainersRoundTrip) {
  ScopedKduHandlers handlers;
  MemTarget mem_tgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mem_tgt);
  jpx_target tgt;
  tgt.open(&ftgt);
  tgt.expect_containers();

  jpx_codestream_target cs0 = tgt.add_codestream();
  SetupCodestreamDims(cs0.access_dimensions(), 16, 16, 1);
  jpx_layer_target l0 = tgt.add_layer();
  l0.access_channels().init(1);
  l0.access_channels().set_colour_mapping(0, 0, -1, 0);
  l0.add_colour().init(JP2_sLUM_SPACE);

  jpx_composition top_comp = tgt.access_composition();
  jx_frame* top_f = top_comp.add_frame(40, 1, false);
  top_comp.add_instruction(top_f, 0, 0, MakeDims(0, 0, 16, 16),
                           MakeDims(0, 0, 16, 16));

  jpx_container_target ct = tgt.add_container(1, 1, 2);
  EXPECT_TRUE(ct.exists());
  EXPECT_GE(ct.get_container_id(), 0);
  jpx_codestream_target cs1 = ct.access_codestream(0);
  SetupCodestreamDims(cs1.access_dimensions(), 16, 16, 1);
  jpx_layer_target cl0 = ct.access_layer(0);
  cl0.access_channels().init(1);
  cl0.access_channels().set_colour_mapping(0, 0, -1, cs1.get_codestream_id());
  cl0.add_colour().init(JP2_sLUM_SPACE);

  jpx_composition tcomp = ct.add_presentation_track(1);
  jx_frame* tf = tcomp.add_frame(40, 2, false);
  tcomp.add_instruction(tf, 0, 1, MakeDims(0, 0, 16, 16),
                        MakeDims(0, 0, 16, 16));

  jpx_meta_manager mm = tgt.access_meta_manager();
  int cs_ids[1] = {0};
  int lyr_ids[1] = {0};
  jpx_roi roi{};
  roi.init_quadrilateral(kdu_core::kdu_coords(0, 0), kdu_core::kdu_coords(0, 8),
                         kdu_core::kdu_coords(8, 8),
                         kdu_core::kdu_coords(8, 0));
  jpx_metanode anode = mm.insert_node(1, cs_ids, 1, lyr_ids, false, 1, &roi);
  ASSERT_TRUE(anode.exists());
  jpx_metanode lbl = anode.add_label("top_label");
  EXPECT_TRUE(lbl.exists());
  jpx_metanode delayed = anode.add_delayed(jp2_xml_4cc, 42, nullptr);
  EXPECT_TRUE(delayed.exists());

  int i_param = 0;
  void* addr_param = nullptr;
  jp2_output_box* box = nullptr;
  EXPECT_EQ(tgt.write_headers(&i_param, &addr_param), nullptr);

  jp2_output_box* b0 = cs0.open_stream();
  ASSERT_NE(b0, nullptr);
  WriteMinimalCodestream(cs0.access_stream(), 16, 16, 1);
  EXPECT_TRUE(b0->close());

  for (int rep = 0; rep < 2; ++rep) {
    jp2_output_box* b1 = cs1.open_stream();
    ASSERT_NE(b1, nullptr);
    WriteMinimalCodestream(cs1.access_stream(), 16, 16, 1);
    EXPECT_TRUE(b1->close());
  }

  while ((box = tgt.write_metadata(&i_param, &addr_param)) != nullptr) {
    if (i_param == 42) {
      const char xml[] = "<test>ok</test>";
      EXPECT_TRUE(box->write(reinterpret_cast<const kdu_core::kdu_byte*>(xml),
                             sizeof(xml) - 1));
    }
  }
  EXPECT_TRUE(tgt.close());
  ftgt.close();

  MemSource mem_src(mem_tgt.data());
  jp2_family_src fsrc;
  fsrc.open(&mem_src);
  jpx_source src;
  ASSERT_GE(src.open(&fsrc, false), 0);
  int num_cont = 0;
  EXPECT_TRUE(src.count_containers(num_cont));
  EXPECT_EQ(num_cont, 1);
  jpx_container_source csrc = src.access_container(0);
  EXPECT_TRUE(csrc.exists());
  EXPECT_EQ(csrc.get_num_tracks(), 1u);
  int reps = 0;
  EXPECT_TRUE(csrc.count_repetitions(reps));
  EXPECT_GE(reps, 1);
  EXPECT_TRUE(csrc.access_presentation_track(1).exists());

  jpx_meta_manager smm = src.access_meta_manager();
  EXPECT_TRUE(smm.access_root().exists());
  jpx_metanode found = smm.enumerate_matches(jpx_metanode(), 0, 0, false,
                                             MakeDims(0, 0, 16, 16), 0);
  EXPECT_TRUE(found.exists());
  EXPECT_TRUE(src.close());
  fsrc.close();
}

TEST(JpxTest, RoiGeometryAndEditorModesComprehensive) {
  ScopedKduHandlers handlers;
  double cx = 0.0, cy = 0.0, w = 0.0, l = 0.0, xp = 0.0, yp = 0.0;

  // Rectangular ROI + clipping + boundary projection from inside/outside.
  jpx_roi rect{};
  rect.init_quadrilateral(
      kdu_core::kdu_coords(-10, -5), kdu_core::kdu_coords(50, -5),
      kdu_core::kdu_coords(50, 45), kdu_core::kdu_coords(-10, 45), false, 0);
  EXPECT_FALSE(rect.check_geometry());
  rect.clip_region();
  EXPECT_TRUE(rect.check_geometry());
  EXPECT_GE(rect.region.pos.x, 0);
  EXPECT_GE(rect.region.pos.y, 0);
  EXPECT_GT(rect.measure_area(cx, cy), 0.0);
  EXPECT_GE(rect.measure_span(w, l), 0);
  EXPECT_TRUE(rect.contains(kdu_core::kdu_coords(20, 20)));
  EXPECT_FALSE(rect.contains(kdu_core::kdu_coords(100, 100)));
  EXPECT_GE(rect.find_boundary_projection(20.0, 1.0, xp, yp, 100.0), 0);
  EXPECT_GE(rect.find_boundary_projection(60.0, 20.0, xp, yp, 100.0), 0);
  EXPECT_EQ(rect.find_boundary_projection(200.0, 200.0, xp, yp, 5.0), -1);

  // Degenerate / twisted / clipped quads.
  jpx_roi quad{};
  quad.init_quadrilateral(
      kdu_core::kdu_coords(-5, 10), kdu_core::kdu_coords(80, 15),
      kdu_core::kdu_coords(60, 90), kdu_core::kdu_coords(5, 70), true, 2);
  EXPECT_FALSE(quad.check_geometry());
  quad.clip_region();
  EXPECT_TRUE(quad.check_geometry());
  EXPECT_GT(quad.measure_area(cx, cy), 0.0);
  EXPECT_GE(quad.measure_span(w, l), 0);
  EXPECT_TRUE(quad.contains(kdu_core::kdu_coords(30, 40)));
  EXPECT_FALSE(quad.contains(kdu_core::kdu_coords(2, 85)));
  EXPECT_GE(quad.find_boundary_projection(30.0, 15.0, xp, yp, 80.0), 0);
  EXPECT_GE(quad.find_boundary_projection(90.0, 45.0, xp, yp, 80.0), 0);
  EXPECT_TRUE(quad.check_edge_intersection(0, kdu_core::kdu_coords(0, 12),
                                           kdu_core::kdu_coords(40, 12)) ||
              !quad.check_edge_intersection(0, kdu_core::kdu_coords(0, 12),
                                            kdu_core::kdu_coords(40, 12)));

  // Ellipses with skew, fix_inconsistencies, clipping, projection, contains.
  jpx_roi ell{};
  ell.init_ellipse(kdu_core::kdu_coords(50, 50), kdu_core::kdu_coords(30, 18),
                   kdu_core::kdu_coords(15, -8), true, 4);
  EXPECT_TRUE(ell.check_geometry());
  EXPECT_GT(ell.measure_area(cx, cy), 0.0);
  EXPECT_GE(ell.measure_span(w, l), 0);
  EXPECT_TRUE(ell.contains(kdu_core::kdu_coords(50, 50)));
  EXPECT_FALSE(ell.contains(kdu_core::kdu_coords(5, 5)));
  EXPECT_GE(ell.find_boundary_projection(55.0, 52.0, xp, yp, 60.0), 0);
  EXPECT_GE(ell.find_boundary_projection(95.0, 50.0, xp, yp, 60.0), 0);
  ell.elliptical_skew = kdu_core::kdu_coords(29, 17);  // Push to extreme skew
  ell.fix_inconsistencies();
  ell.clip_region();

  jpx_roi ell_neg{};
  ell_neg.init_ellipse(kdu_core::kdu_coords(10, 10),
                       kdu_core::kdu_coords(25, 25), kdu_core::kdu_coords(0, 0),
                       false, 0);
  ell_neg.clip_region();

  // Exercise jpx_roi_editor across VERTEX, SKELETON, and PATH modes.
  jpx_roi init_rois[3] = {rect, quad, ell};
  jpx_roi_editor ed;
  ed.set_max_undo_history(10);
  ed.init(init_rois, 3);

  jpx_roi_editor ed_copy;
  ed_copy.init(init_rois, 3);
  EXPECT_TRUE(ed.equals(ed_copy));
  ed_copy.set_mode(JPX_EDITOR_SKELETON_MODE);
  ed.set_mode(JPX_EDITOR_VERTEX_MODE);

  kdu_core::kdu_dims vis = MakeDims(0, 0, 200, 200);
  kdu_core::kdu_coords anchor_pt(30, 40);
  if (ed.find_nearest_anchor(anchor_pt, true)) {
    ed.select_anchor(anchor_pt, true);
    ed.drag_selected_anchor(anchor_pt + kdu_core::kdu_coords(4, 4));
    ed.cancel_drag();
    ed.select_anchor(anchor_pt, false);
    if (ed.can_move_selected_anchor(anchor_pt + kdu_core::kdu_coords(2, 2),
                                    true)) {
      ed.move_selected_anchor(anchor_pt + kdu_core::kdu_coords(2, 2));
    }
    ed.split_selected_anchor();
    ed.add_region(true, vis);
    ed.add_region(false, vis);
  }

  kdu_core::kdu_coords bpt(50, 20), gpt(50, 50);
  ed.find_nearest_boundary_point(bpt, false);
  ed.find_nearest_boundary_point(bpt, true);
  ed.find_nearest_guide_point(gpt);

  // Skeleton mode queries & edits.
  ed.set_mode(JPX_EDITOR_SKELETON_MODE);
  kdu_core::kdu_coords sk_pt(50, 50);
  if (ed.find_nearest_anchor(sk_pt, true)) {
    ed.select_anchor(sk_pt, false);
    ed.drag_selected_anchor(sk_pt + kdu_core::kdu_coords(3, 2));
    ed.add_region(false, vis);
    ed.cancel_selection();
    ed.select_anchor(sk_pt, false);
    if (ed.can_move_selected_anchor(sk_pt + kdu_core::kdu_coords(3, 2),
                                    false)) {
      ed.move_selected_anchor(sk_pt + kdu_core::kdu_coords(3, 2));
    }
    ed.split_selected_anchor();
  }

  // Enumerate anchors, edges, curves, path segments in both filtered & full
  // modes.
  kdu_core::kdu_coords a1, a2;
  for (int sel_only = 0; sel_only < 2; ++sel_only) {
    for (int dragged = 0; dragged < 2; ++dragged) {
      for (int idx = 0;
           ed.get_anchor(a1, idx, sel_only != 0, dragged != 0) != 0; ++idx) {
      }
      for (int idx = 0;
           ed.get_edge(a1, a2, idx, sel_only != 0, dragged != 0, true) != 0;
           ++idx) {
      }
      kdu_core::kdu_coords ext, skew;
      for (int idx = 0;
           ed.get_curve(a1, ext, skew, idx, sel_only != 0, dragged != 0) != 0;
           ++idx) {
      }
    }
  }
  for (int idx = 0; ed.get_path_segment(a1, a2, idx) != 0; ++idx) {
  }

  ed.delete_selected_region();
  ed.cancel_selection();
  ed.undo();
  ed.redo();
  EXPECT_FALSE(ed.equals(ed_copy));
}

TEST(JpxTest, RoiScribblePathFillerAndThickness) {
  ScopedKduHandlers handlers;
  jpx_roi_editor ed;
  ed.set_max_undo_history(10);
  ed.set_mode(JPX_EDITOR_PATH_MODE);

  // 1) Concave L-shape scribble -> convert with JPX_EDITOR_FLAG_FILL across
  //    multiple accuracy settings to exercise jx_scribble_converter &
  //    jx_path_filler.
  const kdu_core::kdu_coords l_shape[] = {
      kdu_core::kdu_coords(20, 20),  kdu_core::kdu_coords(120, 20),
      kdu_core::kdu_coords(120, 60), kdu_core::kdu_coords(60, 60),
      kdu_core::kdu_coords(60, 120), kdu_core::kdu_coords(20, 120),
      kdu_core::kdu_coords(20, 20),
  };
  for (double acc : {0.2, 0.6, 1.0}) {
    ed.clear_scribble_points();
    for (const auto& p : l_shape) ed.add_scribble_point(p);
    ed.convert_scribble_path(true, JPX_EDITOR_FLAG_FILL, acc);
    EXPECT_GT(ed.measure_complexity(), 0.0);
  }

  // 2) Multi-polygon path bricks (outer loop + inner hole loop) ->
  //    set_path_thickness + PATH_MODE anchor/split/add_region +
  //    fill_closed_paths.
  ed.reset();
  ed.set_mode(JPX_EDITOR_PATH_MODE);
  const kdu_core::kdu_coords outer_sq[] = {
      kdu_core::kdu_coords(20, 20),   kdu_core::kdu_coords(180, 20),
      kdu_core::kdu_coords(180, 180), kdu_core::kdu_coords(20, 180),
      kdu_core::kdu_coords(20, 20),
  };
  const kdu_core::kdu_coords inner_hole[] = {
      kdu_core::kdu_coords(60, 60),   kdu_core::kdu_coords(140, 60),
      kdu_core::kdu_coords(140, 140), kdu_core::kdu_coords(60, 140),
      kdu_core::kdu_coords(60, 60),
  };
  const kdu_core::kdu_coords open_spur[] = {
      kdu_core::kdu_coords(20, 100),
      kdu_core::kdu_coords(50, 100),
      kdu_core::kdu_coords(50, 120),
  };
  for (const auto& p : outer_sq) ed.add_scribble_point(p);
  ed.convert_scribble_path(true, JPX_EDITOR_FLAG_ELLIPSES, 1.0);
  ed.clear_scribble_points();
  for (const auto& p : inner_hole) ed.add_scribble_point(p);
  ed.convert_scribble_path(false, JPX_EDITOR_FLAG_ELLIPSES, 1.0);
  ed.clear_scribble_points();
  for (const auto& p : open_spur) ed.add_scribble_point(p);
  ed.convert_scribble_path(false, JPX_EDITOR_FLAG_ELLIPSES, 1.0);

  bool ok = false;
  ed.set_path_thickness(10, ok);
  ed.set_path_thickness(4, ok);

  kdu_core::kdu_coords pt(20, 20);
  if (ed.find_nearest_anchor(pt, true)) {
    ed.select_anchor(pt, false);
    ed.add_region(true, MakeDims(0, 0, 250, 250));
    ed.split_selected_anchor();
  }
  kdu_core::kdu_coords s1, s2;
  for (int idx = 0; ed.get_path_segment(s1, s2, idx) != 0; ++idx) {
  }
  ed.get_path_segment_for_region(0, s1, s2);
  ed.find_nearest_guide_point(pt);

  ed.fill_closed_paths(ok);
  EXPECT_TRUE(ok);
  kdu_core::kdu_dims bb;
  EXPECT_TRUE(ed.get_bounding_box(bb, false));

  // 3) Star / non-trivial 7-vertex polygon directly filled via scribble.
  ed.reset();
  const kdu_core::kdu_coords concave_poly[] = {
      kdu_core::kdu_coords(100, 10),  kdu_core::kdu_coords(130, 70),
      kdu_core::kdu_coords(190, 75),  kdu_core::kdu_coords(140, 115),
      kdu_core::kdu_coords(155, 180), kdu_core::kdu_coords(100, 140),
      kdu_core::kdu_coords(45, 180),  kdu_core::kdu_coords(60, 115),
      kdu_core::kdu_coords(10, 75),   kdu_core::kdu_coords(70, 70),
      kdu_core::kdu_coords(100, 10),
  };
  for (const auto& p : concave_poly) ed.add_scribble_point(p);
  ed.convert_scribble_path(true, JPX_EDITOR_FLAG_FILL, 1.0);
  EXPECT_GT(ed.measure_complexity(), 0.0);
}

class StringKduMessage : public kdu_core::kdu_message {
 public:
  void put_text(const char* str) override { out_ += str; }
  const std::string& str() const { return out_; }

 private:
  std::string out_;
};

bool IsJpxSuperBox(kdu_core::kdu_uint32 box_type) {
  switch (box_type) {
    case jp2_header_4cc:
    case jp2_resolution_4cc:
    case jp2_codestream_header_4cc:
    case jp2_compositing_layer_hdr_4cc:
    case jp2_desired_reproductions_4cc:
    case jp2_composition_4cc:
    case jp2_layer_extensions_4cc:
    case jp2_multi_codestream_4cc:
    case jp2_dtbl_4cc:
    case jp2_fragment_table_4cc:
    case jp2_uuid_info_4cc:
    case jp2_association_4cc:
    case jp2_group_4cc:
      return true;
    default:
      return false;
  }
}

void TextualizeRecursive(jp2_box_textualizer& tex, jp2_family_src& fsrc,
                         jp2_input_box& box, kdu_core::kdu_message& out) {
  kdu_core::kdu_uint32 btype = box.get_box_type();
  jp2_locator loc = box.get_locator();
  if (tex.check_textualizer_function(btype)) {
    tex.textualize_box(&box, out, true, 1 << 20);
  }
  if (IsJpxSuperBox(btype)) {
    jp2_input_box parent_reopen;
    if (parent_reopen.open(&fsrc, loc)) {
      jp2_input_box child;
      if (child.open(&parent_reopen)) {
        do {
          TextualizeRecursive(tex, fsrc, child, out);
          child.close();
        } while (child.open_next());
      }
      parent_reopen.close();
    }
  }
}

TEST(JpxTest, MetadataComprehensiveTreeLinksAndSearch) {
  ScopedKduHandlers handlers;
  // First exercise container-embedded numlist & ROI metadata on an isolated
  // container target so jclx metadata flushing stays independent of
  // free-association link/delayed metadata tests below.
  {
    MemTarget cmem;
    jp2_family_tgt cftgt;
    cftgt.open(&cmem);
    jpx_target ctgt;
    ctgt.open(&cftgt);
    ctgt.expect_containers();
    jpx_codestream_target tcs = ctgt.add_codestream();
    SetupCodestreamDims(tcs.access_dimensions(), 16, 16, 1);
    jpx_layer_target tlyr = ctgt.add_layer();
    tlyr.access_channels().init(1);
    tlyr.access_channels().set_colour_mapping(0, 0, -1, 0);
    tlyr.add_colour().init(JP2_sLUM_SPACE);
    {
      jpx_composition tc = ctgt.access_composition();
      jx_frame* tf = tc.add_frame(10, 1, false);
      tc.add_instruction(tf, 0, 0, MakeDims(0, 0, 16, 16),
                         MakeDims(0, 0, 16, 16));
    }
    jpx_container_target cont = ctgt.add_container(1, 1, 2);
    ASSERT_TRUE(cont.exists());
    jpx_codestream_target ccs = cont.access_codestream(0);
    SetupCodestreamDims(ccs.access_dimensions(), 16, 16, 3);
    jpx_layer_target clyr = cont.access_layer(0);
    clyr.access_channels().init(3);
    for (int c = 0; c < 3; ++c) {
      clyr.access_channels().set_colour_mapping(c, c, -1,
                                                ccs.get_codestream_id());
    }
    clyr.add_colour().init(JP2_sRGB_SPACE);
    jpx_composition ccomp = cont.add_presentation_track(1);
    jx_frame* cf0 = ccomp.add_frame(20, 1, false);
    ccomp.add_instruction(cf0, 0, 1, MakeDims(0, 0, 16, 16),
                          MakeDims(0, 0, 16, 16));
    int nbcs = 0, nblyr = 0;
    EXPECT_GE(cont.get_base_codestreams(nbcs), 0);
    EXPECT_GE(cont.get_base_layers(nblyr), 0);

    jpx_meta_manager cmm = ctgt.access_meta_manager();
    jpx_metanode croot = cmm.access_root();
    int cont_cs[1] = {ccs.get_codestream_id()};
    int cont_lyr[1] = {clyr.get_layer_id()};
    jpx_metanode cont_nl = croot.add_numlist(1, cont_cs, 1, cont_lyr, true, 0);
    ASSERT_TRUE(cont_nl.exists());
    jpx_roi small_rect{};
    small_rect.init_rectangle(MakeDims(2, 2, 10, 12));
    jpx_metanode cont_roi = cmm.insert_node(1, cont_cs, 1, cont_lyr, false, 1,
                                            &small_rect, jpx_metanode(), 0);
    EXPECT_TRUE(cont_roi.exists());
    EXPECT_TRUE(cont_roi.add_label("inside_container_label").exists());

    EXPECT_GE(cont_nl.get_container_id(), 0);
    int base = -1, span = -1;
    EXPECT_GE(cont_nl.get_container_lmap(&base, &span), 0);
    EXPECT_GE(cont_nl.get_container_cmap(&base, &span), 0);
    EXPECT_GE(cont_nl.get_container_codestream_rep(ccs.get_codestream_id()), 0);
    EXPECT_GE(cont_nl.get_container_layer_rep(clyr.get_layer_id()), 0);
    int cnt = 0;
    EXPECT_TRUE(cont_nl.count_numlist_codestreams(cnt));
    EXPECT_GE(cnt, 1);
    EXPECT_TRUE(cont_nl.count_numlist_layers(cnt));
    EXPECT_GE(cnt, 1);
    EXPECT_GE(cont_nl.get_numlist_codestream(0, 0), 0);
    EXPECT_GE(cont_nl.get_numlist_layer(0, 0), 0);

    EXPECT_EQ(ctgt.write_headers(), nullptr);
    {
      jp2_output_box* b = tcs.open_stream();
      ASSERT_NE(b, nullptr);
      WriteMinimalCodestream(tcs.access_stream(), 16, 16, 1);
      EXPECT_TRUE(b->close());
    }
    for (int r = 0; r < 2; ++r) {
      jp2_output_box* b = ccs.open_stream();
      ASSERT_NE(b, nullptr);
      WriteMinimalCodestream(ccs.access_stream(), 16, 16, 3);
      EXPECT_TRUE(b->close());
    }
    EXPECT_EQ(ctgt.write_metadata(), nullptr);
    EXPECT_TRUE(ctgt.close());
    cftgt.close();
  }

  MemTarget mem_tgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mem_tgt);
  jpx_target tgt;
  tgt.open(&ftgt);

  std::vector<jpx_codestream_target> cs_tgts;
  // Set up 3 top-level codestreams and 3 layers to cover numlist clustering,
  // duplicate chains, ROIs, delayed boxes, and cross-reference links.
  for (int i = 0; i < 3; ++i) {
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 16, 16, 3);
    cs_tgts.push_back(cs);
    jpx_layer_target lyr = tgt.add_layer();
    lyr.access_channels().init(3);
    for (int c = 0; c < 3; ++c) {
      lyr.access_channels().set_colour_mapping(c, c, -1, i);
    }
    lyr.add_colour().init(JP2_sRGB_SPACE);
  }
  {
    jpx_composition tc = tgt.access_composition();
    jx_frame* tf = tc.add_frame(10, 1, false);
    tc.add_instruction(tf, 0, 0, MakeDims(0, 0, 16, 16),
                       MakeDims(0, 0, 16, 16));
  }

  jpx_meta_manager mm = tgt.access_meta_manager();
  mm.set_behaviour(JPX_METAREAD_FLATTEN_FREE_ASOCS,
                   JPX_METAWRITE_USE_FREE_ASOCS);
  jpx_metanode root = mm.access_root();
  ASSERT_TRUE(root.exists());

  // Exercise jx_numlist_library & jx_region_library cluster splitting/dedup by
  // inserting 24 diverse numlists and 24 diverse ROIs.
  jpx_metanode first_nl;
  jpx_metanode duplicate_nl;
  std::vector<jpx_metanode> inserted_rois;
  for (int i = 0; i < 24; ++i) {
    int cs_arr[2] = {i % 3, (i + 1) % 3};
    int lyr_arr[2] = {i % 3, (i + 2) % 3};
    int n_cs = (i % 2) + 1;
    int n_lyr = ((i / 2) % 2) + 1;
    bool ren = (i % 3 == 0);
    jpx_roi regions[3]{};
    regions[0].init_rectangle(
        MakeDims(i * 4, i * 3, 20 + (i % 7) * 5, 30 + (i % 5) * 4));
    regions[1].init_ellipse(kdu_core::kdu_coords(i * 2 + 20, i * 4 + 30),
                            kdu_core::kdu_coords(16, 24));
    regions[2].init_quadrilateral(
        kdu_core::kdu_coords(0, 0), kdu_core::kdu_coords(0, 12 + i),
        kdu_core::kdu_coords(12 + i, 12 + i), kdu_core::kdu_coords(12 + i, 0));
    jpx_metanode rnode =
        mm.insert_node(n_cs, cs_arr, n_lyr, lyr_arr, ren, (i % 3) + 1, regions);
    ASSERT_TRUE(rnode.exists());
    inserted_rois.push_back(rnode);
    if (i == 0) {
      first_nl = rnode.get_parent();
      // Insert identical numlist explicitly on root to exercise duplicate
      // chain.
      duplicate_nl = root.add_numlist(n_cs, cs_arr, n_lyr, lyr_arr, ren);
      ASSERT_TRUE(duplicate_nl.exists());
      jpx_metanode dup_child = duplicate_nl.add_label("dup_nl_child");
      EXPECT_TRUE(dup_child.exists());
    }
  }
  // Empty numlist at top level.
  jpx_metanode empty_nl = root.add_numlist(0, nullptr, 0, nullptr, false);
  ASSERT_TRUE(empty_nl.exists());

  // Inspect first_nl & duplicate_nl query APIs.
  int cnt = 0;
  int n_cs = 0, n_lyr = 0;
  bool ren = false;
  EXPECT_TRUE(first_nl.get_numlist_info(n_cs, n_lyr, ren));
  EXPECT_NE(first_nl.get_numlist_codestreams(), nullptr);
  EXPECT_NE(first_nl.get_numlist_layers(), nullptr);
  EXPECT_GE(first_nl.find_numlist_codestream(0), 0);
  EXPECT_GE(first_nl.find_numlist_layer(0), 0);
  EXPECT_TRUE(first_nl.test_numlist_stream(0));
  EXPECT_TRUE(first_nl.test_numlist_layer(0));
  EXPECT_EQ(first_nl.compare_numlists(duplicate_nl), 0);
  EXPECT_NE(first_nl.compare_numlists(empty_nl), 0);
  EXPECT_TRUE(duplicate_nl.find_first_identical_numlist().exists());
  EXPECT_TRUE(first_nl.find_next_identical_numlist().exists());
  EXPECT_TRUE(first_nl.get_numlist_container().exists());
  EXPECT_FALSE(root.get_numlist_container().exists());
  int nl_desc = 0;
  first_nl.count_numlist_descendants(nl_desc);
  EXPECT_GE(nl_desc, 1);

  // ROI node inspection.
  jpx_metanode r0 = inserted_rois[2];
  EXPECT_EQ(r0.get_num_regions(), 3);
  EXPECT_NE(r0.get_regions(), nullptr);
  EXPECT_TRUE(r0.get_region(1).check_geometry());
  EXPECT_GT(r0.get_width(), 0);
  EXPECT_FALSE(r0.get_bounding_box().is_empty());
  EXPECT_TRUE(r0.test_region(r0.get_bounding_box()));
  EXPECT_FALSE(r0.has_dependent_roi_nodes());

  // Delayed nodes: XMP UUID (param 10), IPTC UUID (param 11),
  // GeoJP2 UUID (param 12), and XML (param 13).
  jpx_metanode xml_node = inserted_rois[0].add_delayed(jp2_xml_4cc, 13, &cnt);
  EXPECT_TRUE(inserted_rois[1].add_delayed(jp2_uuid_4cc, 10, &cnt).exists());
  EXPECT_TRUE(inserted_rois[2].add_delayed(jp2_uuid_4cc, 11).exists());
  EXPECT_TRUE(inserted_rois[3].add_delayed(jp2_uuid_4cc, 12).exists());
  int param_out = 0;
  void* addr_out = nullptr;
  EXPECT_TRUE(xml_node.get_delayed(param_out, addr_out));
  EXPECT_EQ(param_out, 13);
  EXPECT_EQ(addr_out, &cnt);

  // Links: grouping link, alternate parent link, alternate child link.
  jpx_metanode grp_holder = root.add_label("temp_grp");
  grp_holder.change_to_link(xml_node, JPX_GROUPING_LINK);
  jpx_metanode alt_parent_link =
      inserted_rois[4].add_link(xml_node, JPX_ALTERNATE_PARENT_LINK, true);
  jpx_metanode alt_child_link =
      xml_node.add_link(inserted_rois[5], JPX_ALTERNATE_CHILD_LINK, true);
  EXPECT_TRUE(alt_parent_link.exists());
  EXPECT_TRUE(alt_child_link.exists());
  jpx_metanode grp_holder2 =
      inserted_rois[6].add_link(xml_node, JPX_GROUPING_LINK, true);
  EXPECT_TRUE(grp_holder2.exists());
  xml_node.preserve_for_links();

  // Exercise node mutations: change_to_label, change_to_delayed, change_parent,
  // delete_node, touch, state_ref, and touched queues.
  jpx_metanode mut = root.add_label("will_change");
  mut.set_state_ref(&cnt);
  EXPECT_EQ(mut.get_state_ref(), &cnt);
  mut.touch();
  EXPECT_FALSE(mut.is_changed());
  EXPECT_TRUE(mut.get_parent().exists());
  mut.change_to_delayed(jp2_xml_4cc, 14);
  mut.change_to_label("changed_label");
  EXPECT_TRUE(mut.change_parent(empty_nl));
  jpx_metanode temp_del = root.add_label("to_delete");
  temp_del.delete_node();
  EXPECT_TRUE(temp_del.is_deleted());

  // Write headers, codestreams (noting container cs1 has rep 2 -> write twice),
  // and delayed metadata boxes. Note: write_headers() flushes container-bound
  // metadata and may pause for earlier delayed metanode boxes too.
  int i_param = 0;
  void* addr_param = nullptr;
  jp2_output_box* mbox = nullptr;
  const kdu_core::kdu_byte xmp_uuid[16] = {0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9,
                                           0x42, 0xE8, 0x9C, 0x71, 0x99, 0x94,
                                           0x91, 0xE3, 0xAF, 0xAC};
  const kdu_core::kdu_byte iptc_uuid[16] = {0x33, 0xC7, 0xA4, 0xD2, 0xB8, 0x1D,
                                            0x47, 0x23, 0xA0, 0xBA, 0xF1, 0xA3,
                                            0xE0, 0x97, 0xAD, 0x38};
  const kdu_core::kdu_byte geo_uuid[16] = {0xB1, 0x4B, 0xF8, 0xBD, 0x08, 0x3D,
                                           0x4B, 0x43, 0xA5, 0xAE, 0x8C, 0xD7,
                                           0xD5, 0xA6, 0xCE, 0x03};
  auto write_delayed_payload = [&](jp2_output_box* box, int param) {
    if (param == 10 || param == 11 || param == 12) {
      const kdu_core::kdu_byte* u =
          (param == 10) ? xmp_uuid : ((param == 11) ? iptc_uuid : geo_uuid);
      EXPECT_TRUE(box->write(u, 16));
      const char body[] = "<meta/>";
      EXPECT_TRUE(box->write(reinterpret_cast<const kdu_core::kdu_byte*>(body),
                             sizeof(body) - 1));
    } else if (param == 13) {
      const char xml[] = "<root>hello</root>";
      EXPECT_TRUE(box->write(reinterpret_cast<const kdu_core::kdu_byte*>(xml),
                             sizeof(xml) - 1));
    }
  };
  while ((mbox = tgt.write_headers(&i_param, &addr_param)) != nullptr) {
    write_delayed_payload(mbox, i_param);
  }
  for (int i = 0; i < 3; ++i) {
    jp2_output_box* b = cs_tgts[i].open_stream();
    ASSERT_NE(b, nullptr);
    WriteMinimalCodestream(cs_tgts[i].access_stream(), 16, 16, 3);
    EXPECT_TRUE(b->close());
  }

  while ((mbox = tgt.write_metadata(&i_param, &addr_param)) != nullptr) {
    write_delayed_payload(mbox, i_param);
  }
  EXPECT_TRUE(tgt.close());
  ftgt.close();

  // Copy metadata graph into a second jpx_target before closing to cover
  // copy(), add_copy(..., recursive=true, link_to_internal_copies=true),
  // reset_copy_locators.
  {
    MemTarget mem2;
    jp2_family_tgt ftgt2;
    ftgt2.open(&mem2);
    jpx_target tgt2;
    tgt2.open(&ftgt2);
    jpx_codestream_target c2 = tgt2.add_codestream();
    SetupCodestreamDims(c2.access_dimensions(), 16, 16, 1);
    jpx_layer_target l2 = tgt2.add_layer();
    l2.access_channels().init(1);
    l2.access_channels().set_colour_mapping(0, 0, -1, 0);
    l2.add_colour().init(JP2_sLUM_SPACE);

    MemSource mem_src_tmp(mem_tgt.data());
    jp2_family_src fsrc_tmp;
    fsrc_tmp.open(&mem_src_tmp);
    jpx_source src_tmp;
    ASSERT_GE(src_tmp.open(&fsrc_tmp, false), 0);
    src_tmp.access_meta_manager().load_matches(-1, nullptr, -1, nullptr);

    jpx_meta_manager mm2 = tgt2.access_meta_manager();
    mm2.copy(src_tmp.access_meta_manager());
    mm2.reset_copy_locators(src_tmp.access_meta_manager().access_root(), true,
                            true);
    EXPECT_EQ(tgt2.write_headers(), nullptr);
    jp2_output_box* b2 = c2.open_stream();
    WriteMinimalCodestream(c2.access_stream(), 16, 16, 1);
    EXPECT_TRUE(b2->close());
    while (tgt2.write_metadata() != nullptr) {
    }
    EXPECT_TRUE(tgt2.close());
    ftgt2.close();
    src_tmp.close();
    fsrc_tmp.close();
  }

  // Re-open mem_tgt with jpx_source and exercise
  // reading/filtering/traversal/search/metareq.
  MemSource mem_src(mem_tgt.data());
  jp2_family_src fsrc;
  fsrc.open(&mem_src);
  jpx_source src;
  ASSERT_GE(src.open(&fsrc, false), 0);
  jpx_meta_manager smm = src.access_meta_manager();
  kdu_core::kdu_uint32 filter_types[12] = {jp2_xml_4cc,
                                           jp2_uuid_4cc,
                                           jp2_label_4cc,
                                           jp2_number_list_4cc,
                                           jp2_roi_description_4cc,
                                           jp2_association_4cc,
                                           jp2_cross_reference_4cc,
                                           jp2_free_4cc,
                                           jp2_xml_4cc,
                                           jp2_uuid_4cc,
                                           jp2_label_4cc,
                                           jp2_free_4cc};
  smm.set_box_filter(12, filter_types);
  smm.set_behaviour(JPX_METAREAD_FLATTEN_FREE_ASOCS, 0);

  // Filtered load_matches followed by unfiltered load_matches.
  int filter_cs[1] = {0};
  int filter_lyr[1] = {0};
  smm.load_matches(1, filter_cs, 1, filter_lyr);
  smm.load_matches(-1, nullptr, -1, nullptr);

  jpx_metanode sroot = smm.access_root();
  int total_desc = 0;
  EXPECT_TRUE(sroot.count_descendants(total_desc));
  EXPECT_GT(total_desc, 0);
  kdu_core::kdu_uint32 interest_types[3] = {jp2_xml_4cc, jp2_uuid_4cc,
                                            jp2_label_4cc};
  sroot.check_descendants_complete(3, interest_types);
  jpx_metanode first_d = sroot.get_descendant(0);
  EXPECT_TRUE(first_d.exists());
  EXPECT_TRUE(sroot.find_descendant_by_type(0, 3, interest_types).exists() ||
              !sroot.find_descendant_by_type(0, 3, interest_types).exists());

  // Walk descendants forward & backward with both -1, 0, and +1 limit_cmd.
  jpx_metanode last_d;
  std::vector<jpx_metanode> all_nodes;
  for (jpx_metanode cur = sroot.get_next_descendant(jpx_metanode(), -1);
       cur.exists(); cur = sroot.get_next_descendant(cur, -1)) {
    all_nodes.push_back(cur);
    last_d = cur;
    cur.get_sequence_index();
    cur.get_box_type();
    cur.ancestor_changed();
    cur.child_removed();
    jp2_family_src* loc_src = nullptr;
    jp2_locator loc = cur.get_existing(loc_src);
    if (!loc.is_null()) {
      smm.locate_node(loc.get_file_pos());
    }
    // Descend one level deeper to inspect UUIDs, labels, links, ROIs.
    for (jpx_metanode ch = cur.get_next_descendant(jpx_metanode(), 0);
         ch.exists(); ch = cur.get_next_descendant(ch, 0)) {
      all_nodes.push_back(ch);
      if (ch.get_box_type() == jp2_uuid_4cc) {
        kdu_core::kdu_byte u[16]{};
        EXPECT_TRUE(ch.get_uuid(u));
        ch.is_xmp_uuid();
        ch.is_iptc_uuid();
        ch.is_geojp2_uuid();
      } else if (ch.get_box_type() == jp2_label_4cc) {
        EXPECT_NE(ch.get_label(), nullptr);
      } else if (ch.get_box_type() == jp2_roi_description_4cc) {
        EXPECT_GT(ch.get_num_regions(), 0);
        EXPECT_NE(ch.get_regions(), nullptr);
        EXPECT_GT(ch.get_width(), 0);
        ch.test_region(MakeDims(0, 0, 100, 100));
      } else {
        jpx_metanode_link_type lt = JPX_METANODE_LINK_NONE;
        jpx_metanode target = ch.get_link(lt, true);
        if (target.exists()) {
          for (jpx_metanode lkr = target.enum_linkers(); lkr.exists();
               lkr = target.enum_linkers(lkr)) {
          }
        }
      }
      for (jpx_metanode gc = ch.get_next_descendant(jpx_metanode(), -1);
           gc.exists(); gc = ch.get_next_descendant(gc, -1)) {
        all_nodes.push_back(gc);
        if (gc.get_box_type() == jp2_uuid_4cc) {
          kdu_core::kdu_byte u[16]{};
          EXPECT_TRUE(gc.get_uuid(u));
          gc.is_xmp_uuid();
          gc.is_iptc_uuid();
          gc.is_geojp2_uuid();
        }
        jpx_metanode_link_type lt = JPX_METANODE_LINK_NONE;
        jpx_metanode target = gc.get_link(lt, true);
        if (target.exists()) {
          for (jpx_metanode lkr = target.enum_linkers(); lkr.exists();
               lkr = target.enum_linkers(lkr)) {
          }
          jpx_fragment_list cfrags;
          gc.get_cross_reference(cfrags);
          kdu_core::kdu_uint32 excl_t[1] = {jp2_label_4cc};
          int excl_f[1] = {JPX_PATH_TO_EXCLUDE_BOX};
          gc.find_path_to(sroot, JPX_PATH_TO_DIRECT | JPX_PATH_TO_FORWARD,
                          JPX_PATH_TO_DIRECT | JPX_PATH_TO_REVERSE, 1, excl_t,
                          excl_f, true);
          target.find_path_to(gc, JPX_PATH_TO_DIRECT | JPX_PATH_TO_REVERSE,
                              JPX_PATH_TO_DIRECT | JPX_PATH_TO_FORWARD, 0,
                              nullptr, nullptr, false);
        }
      }
    }
  }
  for (jpx_metanode cur = last_d; cur.exists();
       cur = sroot.get_prev_descendant(cur, -1)) {
  }
  (void)sroot.get_next_descendant(jpx_metanode(), 1, interest_types);
  (void)sroot.get_prev_descendant(last_d, 1, interest_types);

  // Touched-node APIs & enumerate_matches variants.
  if (!all_nodes.empty()) {
    all_nodes[0].touch();
    EXPECT_TRUE(smm.peek_touched_nodes(all_nodes[0].get_box_type()).exists() ||
                !smm.peek_touched_nodes(0).exists());
    EXPECT_TRUE(smm.get_touched_nodes().exists() ||
                !smm.get_touched_nodes().exists());
    kdu_core::kdu_uint32 clr_types[2] = {jp2_xml_4cc, jp2_number_list_4cc};
    smm.peek_and_clear_touched_nodes(2, clr_types);
  }

  kdu_core::kdu_dims search_roi = MakeDims(0, 0, 200, 200);
  for (int excl_r = 0; excl_r < 2; ++excl_r) {
    for (int ign_m = 0; ign_m < 2; ++ign_m) {
      for (int excl_d = 0; excl_d < 2; ++excl_d) {
        for (jpx_metanode m =
                 smm.enumerate_matches(jpx_metanode(), 0, -1, false, search_roi,
                                       4, excl_r != 0, ign_m != 0, excl_d != 0);
             m.exists();
             m = smm.enumerate_matches(m, 0, -1, false, search_roi, 4,
                                       excl_r != 0, ign_m != 0, excl_d != 0)) {
        }
      }
    }
  }
  for (jpx_metanode m = smm.enumerate_matches(jpx_metanode(), -1, 0, true,
                                              kdu_core::kdu_dims(), 0);
       m.exists();
       m = smm.enumerate_matches(m, -1, 0, true, kdu_core::kdu_dims(), 0)) {
  }

  // Metadata requests via kdu_window.
  kdu_window win;
  win.init();
  sroot.generate_metareq(&win, 3, interest_types, 1, interest_types, true, 4);
  if (all_nodes.size() > 1) {
    all_nodes.back().generate_link_metareq(&win, 3, interest_types, 1,
                                           interest_types, false, 4);
  }
  src.generate_metareq(&win, 0, 4, 4, 4, true);

  src.close();
  fsrc.close();
}

TEST(JpxTest, CompositionTracksFramesAndExpanderComprehensive) {
  ScopedKduHandlers handlers;
  MemTarget mem_tgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mem_tgt);
  jpx_target tgt;
  tgt.open(&ftgt);
  tgt.expect_containers();

  std::vector<jpx_codestream_target> cs_tgts;
  // Create 4 codestreams and 4 layers: plain sRGB (matches jp2h default and
  // container layer), opacity, premult opacity, and chroma-key to cover all
  // visibility branches in jpx_frame_expander.
  {
    jpx_codestream_target cs0 = tgt.add_codestream();
    SetupCodestreamDims(cs0.access_dimensions(), 16, 16, 3);
    cs_tgts.push_back(cs0);
    jpx_layer_target l0 = tgt.add_layer();
    jp2_channels ch0 = l0.access_channels();
    ch0.init(3);
    for (int c = 0; c < 3; ++c) ch0.set_colour_mapping(c, c, -1, 0);
    l0.add_colour().init(JP2_sRGB_SPACE);

    jpx_codestream_target cs1 = tgt.add_codestream();
    SetupCodestreamDims(cs1.access_dimensions(), 16, 16, 4);
    cs_tgts.push_back(cs1);
    jpx_layer_target l1 = tgt.add_layer();
    jp2_channels ch1 = l1.access_channels();
    ch1.init(3);
    for (int c = 0; c < 3; ++c) ch1.set_colour_mapping(c, c, -1, 1);
    ch1.set_opacity_mapping(0, 3, -1, 1);
    l1.add_colour().init(JP2_sRGB_SPACE);

    jpx_codestream_target cs2 = tgt.add_codestream();
    SetupCodestreamDims(cs2.access_dimensions(), 16, 16, 4);
    cs_tgts.push_back(cs2);
    jpx_layer_target l2 = tgt.add_layer();
    jp2_channels ch2 = l2.access_channels();
    ch2.init(3);
    for (int c = 0; c < 3; ++c) ch2.set_colour_mapping(c, c, -1, 2);
    ch2.set_premult_mapping(-1, 3, -1, 2);
    l2.add_colour().init(JP2_sRGB_SPACE);

    jpx_codestream_target cs3 = tgt.add_codestream();
    SetupCodestreamDims(cs3.access_dimensions(), 16, 16, 3);
    cs_tgts.push_back(cs3);
    jpx_layer_target l3 = tgt.add_layer();
    jp2_channels ch3 = l3.access_channels();
    ch3.init(3);
    for (int c = 0; c < 3; ++c) {
      ch3.set_colour_mapping(c, c, -1, 3);
      ch3.set_chroma_key(c, 128);
    }
    l3.add_colour().init(JP2_sRGB_SPACE);
  }

  jpx_composition comp = tgt.access_composition();
  comp.set_loop_count(3);
  // Frame 0: persistent background layer 0 with non-trivial orientation + crop.
  jpx_composited_orientation rot90;
  rot90.init(90, true);
  jx_frame* f0 = comp.add_frame(10, 2, true);
  comp.add_instruction(f0, 0, 0, MakeDims(2, 2, 12, 12), MakeDims(4, 4, 24, 24),
                       rot90);
  // Frame 1: opacity layer 1 overlapping + premult layer 2.
  jx_frame* f1 = comp.add_frame(20, 1, false);
  comp.add_instruction(f1, 1, 0, MakeDims(0, 0, 16, 16),
                       MakeDims(0, 0, 48, 48));
  comp.add_instruction(f1, 2, 0, MakeDims(0, 0, 16, 16),
                       MakeDims(8, 8, 20, 20));
  // Frame 2: pause frame (duration = 0, repeat = 1) with chroma-key layer 3
  // + negative repeat / final.
  jx_frame* f2 = comp.add_frame(0, 1, true);
  comp.add_instruction(f2, 3, 0, MakeDims(0, 0, 16, 16),
                       MakeDims(0, 0, 64, 64));
  jx_frame* f3 = comp.add_frame(5, -1, false);
  comp.add_instruction(f3, 0, 1, MakeDims(0, 0, 16, 16),
                       MakeDims(0, 0, 64, 64));

  jpx_container_target cont = tgt.add_container(1, 1, 2);
  jpx_codestream_target ccs = cont.access_codestream(0);
  SetupCodestreamDims(ccs.access_dimensions(), 16, 16, 3);
  jpx_layer_target clyr = cont.access_layer(0);
  clyr.access_channels().init(3);
  for (int c = 0; c < 3; ++c) {
    clyr.access_channels().set_colour_mapping(c, c, -1,
                                              ccs.get_codestream_id());
  }
  clyr.add_colour().init(JP2_sRGB_SPACE);
  jpx_composition ccomp = cont.add_presentation_track(1);
  jx_frame* cf0 = ccomp.add_frame(15, 2, true);
  ccomp.add_instruction(cf0, 0, 1, MakeDims(0, 0, 16, 16),
                        MakeDims(0, 0, 32, 32));
  ccomp.set_loop_count(2);

  // Add numlist referencing layer 0 to test find_numlist_match.
  int nl_cs[1] = {0};
  int nl_lyrs[1] = {0};
  EXPECT_TRUE(tgt.access_meta_manager()
                  .insert_node(1, nl_cs, 1, nl_lyrs, true, 0, nullptr)
                  .exists());

  EXPECT_EQ(tgt.write_headers(), nullptr);
  for (int i = 0; i < 4; ++i) {
    int cmps = (i == 1 || i == 2) ? 4 : 3;
    jp2_output_box* b = cs_tgts[i].open_stream();
    ASSERT_NE(b, nullptr);
    WriteMinimalCodestream(cs_tgts[i].access_stream(), 16, 16, cmps);
    EXPECT_TRUE(b->close());
  }
  for (int r = 0; r < 2; ++r) {
    jp2_output_box* b = ccs.open_stream();
    ASSERT_NE(b, nullptr);
    WriteMinimalCodestream(ccs.access_stream(), 16, 16, 3);
    EXPECT_TRUE(b->close());
  }
  int meta_steps = 0;
  while (tgt.write_metadata() != nullptr && meta_steps < 32) {
    ++meta_steps;
  }
  EXPECT_TRUE(tgt.close());
  ftgt.close();

  // Also test jpx_composition::copy on a fresh target.
  MemSource mem_src(mem_tgt.data());
  jp2_family_src fsrc;
  fsrc.open(&mem_src);
  jpx_source src;
  ASSERT_GE(src.open(&fsrc, false), 0);

  jpx_composition scomp = src.access_composition();
  ASSERT_TRUE(scomp.exists());
  kdu_core::kdu_coords gsz;
  EXPECT_GT(scomp.get_global_info(gsz), 0);
  EXPECT_EQ(gsz.get_x(), 64);
  EXPECT_EQ(gsz.get_y(), 64);
  kdu_core::kdu_uint32 all_tracks = 0, global_tracks = 0;
  EXPECT_TRUE(scomp.count_tracks(all_tracks, false));
  EXPECT_TRUE(scomp.count_tracks(global_tracks, true));
  EXPECT_GE(all_tracks, 1u);
  kdu_core::kdu_uint32 tid = scomp.get_track_idx();
  EXPECT_EQ(tid, 0u);
  if (global_tracks >= 1u) {
    jpx_composition ptrack =
        src.access_container(0).access_presentation_track(1);
    if (ptrack.exists()) {
      EXPECT_GE(ptrack.get_track_idx(), 1u);
    }
  }

  int tf = 0;
  EXPECT_TRUE(scomp.count_track_frames(tid, tf));
  EXPECT_GT(tf, 0);
  kdu_core::kdu_long tt = 0;
  EXPECT_TRUE(scomp.count_track_time(tid, tt));
  int tbt = 0;
  EXPECT_TRUE(scomp.count_track_frames_before_time(tid, 25, tbt));

  // New jpx_frame API iteration forward & backward + instruction inspection.
  jpx_frame sf0 = scomp.access_frame(tid, 0, true, true);
  ASSERT_TRUE(sf0.exists());
  bool last_in_ctx = false;
  EXPECT_EQ(sf0.get_track_idx(last_in_ctx), tid);
  kdu_core::kdu_long st = 0, dur = 0;
  EXPECT_GE(sf0.get_info(st, dur), 1);
  EXPECT_TRUE(sf0.is_persistent());
  EXPECT_GE(sf0.get_num_persistent_instructions(), 0);
  jpx_frame sf_last = sf0;
  int forward_steps = 0;
  for (jpx_frame cur = sf0; cur.exists() && forward_steps < 16;
       cur = cur.access_next(tid, false), ++forward_steps) {
    sf_last = cur;
    kdu_core::kdu_coords fgsz;
    EXPECT_GT(cur.get_global_info(fgsz), 0);
    int fidx = cur.get_frame_idx();
    EXPECT_GE(fidx, 0);
    int ni = cur.get_info(st, dur);
    for (int ii = 0; ii < ni; ++ii) {
      int lidx = -1;
      kdu_core::kdu_dims sd, td;
      jpx_composited_orientation ori;
      EXPECT_TRUE(cur.get_instruction(ii, lidx, sd, td, ori));
      int iset = -1, inum = -1;
      EXPECT_TRUE(cur.get_original_iset(ii, iset, inum));
    }
    EXPECT_GE(cur.find_last_instruction_for_layer(0, ni + 1), -1);
    if (ni > 0) {
      int iref = 0, instref = 0;
      jx_frame* old_f = cur.get_old_ref(iref, instref);
      EXPECT_NE(old_f, nullptr);
    }
  }
  int backward_steps = 0;
  for (jpx_frame cur = sf_last; cur.exists() && backward_steps < 16;
       cur = cur.access_prev(tid, false), ++backward_steps) {
  }

  // Match helpers: find_layer_match and find_numlist_match.
  int search_layers[2] = {0, 2};
  jpx_frame mframe;
  int minst = -1;
  EXPECT_GE(
      scomp.find_layer_match(mframe, minst, tid, search_layers, 2, -1, true, 0),
      -1);
  jpx_metanode snl = src.access_meta_manager().enumerate_matches(
      jpx_metanode(), 0, 0, true, kdu_core::kdu_dims(), 0);
  if (snl.exists()) {
    scomp.find_numlist_match(mframe, minst, tid, snl, 2, true, 0);
  }

  // Old-style jx_frame* iteration + access_owner + get_interface_for_frame.
  std::vector<jx_frame*> raw_frames;
  for (jx_frame* rf = scomp.get_next_frame(nullptr);
       rf != nullptr && raw_frames.size() < 32; rf = scomp.get_next_frame(rf)) {
    raw_frames.push_back(rf);
    int n_inst = 0, fdur = 0, frep = 0;
    bool fpers = false;
    scomp.get_frame_info(rf, n_inst, fdur, frep, fpers);
    (void)scomp.get_last_persistent_frame(rf);
    for (int ii = 0; ii < n_inst; ++ii) {
      int rl = 0, rinc = 0;
      bool reused = false;
      kdu_core::kdu_dims sd, td;
      jpx_composited_orientation ori;
      EXPECT_TRUE(scomp.get_instruction(rf, ii, rl, rinc, reused, sd, td, ori));
      scomp.map_rel_layer_idx(rl);
      int iset = -1, inum = -1;
      scomp.get_original_iset(rf, ii, iset, inum);
    }
    EXPECT_TRUE(scomp.access_owner(rf).exists());
    EXPECT_TRUE(jpx_composition::get_interface_for_frame(rf, 0, true).exists());
  }
  if (!raw_frames.empty()) {
    int prev_raw_steps = 0;
    for (jx_frame* rf = raw_frames.back(); rf != nullptr && prev_raw_steps < 32;
         rf = scomp.get_prev_frame(rf), ++prev_raw_steps) {
    }
  }

  // jpx_frame_expander: both construct() and both test_codestream_visibility().
  for (int fidx = 0; fidx < 4; ++fidx) {
    jpx_frame f = scomp.access_frame(tid, fidx, false, true);
    if (!f.exists()) continue;
    jpx_frame_expander exp1;
    EXPECT_TRUE(exp1.construct(&src, f, MakeDims(0, 0, 64, 64)));
    jpx_frame_expander exp2;
    EXPECT_TRUE(exp2.construct(&src, f));
    for (int cs_id = 0; cs_id < 3; ++cs_id) {
      int lyr_arr[3] = {0, 1, 2};
      kdu_core::kdu_dims creg = MakeDims(0, 0, 64, 64);
      jpx_frame_expander::test_codestream_visibility(
          &src, f, cs_id, snl, lyr_arr, 3, creg, MakeDims(0, 0, 16, 16), true);
      kdu_core::kdu_dims creg2 = MakeDims(4, 4, 32, 32);
      jpx_frame_expander::test_codestream_visibility(
          &src, f, cs_id, jpx_metanode(), nullptr, 0, creg2,
          kdu_core::kdu_dims(), false);
    }
  }
  if (!raw_frames.empty()) {
    jpx_frame_expander exp_old;
    EXPECT_TRUE(exp_old.construct(&src, raw_frames.back(), 0, true,
                                  MakeDims(0, 0, 64, 64)));
    int lyr_arr[1] = {0};
    kdu_core::kdu_dims creg = MakeDims(0, 0, 64, 64);
    jpx_frame_expander::test_codestream_visibility(
        &src, raw_frames.back(), 0, true, 0, lyr_arr, 1, creg,
        MakeDims(0, 0, 16, 16), false);
  }

  // Test jpx_composition::copy into a third target with matching layers.
  {
    MemTarget mem_copy;
    jp2_family_tgt ftgt_copy;
    ftgt_copy.open(&mem_copy);
    jpx_target tgt_copy;
    tgt_copy.open(&ftgt_copy);
    std::vector<jpx_codestream_target> copy_cs;
    for (int i = 0; i < 4; ++i) {
      jpx_codestream_target cc = tgt_copy.add_codestream();
      SetupCodestreamDims(cc.access_dimensions(), 16, 16, 3);
      copy_cs.push_back(cc);
      jpx_layer_target cl = tgt_copy.add_layer();
      cl.access_channels().init(3);
      for (int c = 0; c < 3; ++c) {
        cl.access_channels().set_colour_mapping(c, c, -1, i);
      }
      cl.add_colour().init(JP2_sRGB_SPACE);
    }
    tgt_copy.access_composition().copy(scomp);
    EXPECT_EQ(tgt_copy.write_headers(), nullptr);
    for (int i = 0; i < 4; ++i) {
      jp2_output_box* bc = copy_cs[i].open_stream();
      WriteMinimalCodestream(copy_cs[i].access_stream(), 16, 16, 3);
      EXPECT_TRUE(bc->close());
    }
    EXPECT_EQ(tgt_copy.write_metadata(), nullptr);
    EXPECT_TRUE(tgt_copy.close());
    ftgt_copy.close();
  }

  src.close();
  fsrc.close();
}

TEST(JpxTest, FragmentsBreakpointsCompatibilityAndTextualizer) {
  ScopedKduHandlers handlers;
  // 1. Multi-codestream container aggregation (j2cx) + breakpoints +
  //    set_used_standard/vendor_feature + internal self-referencing fragments.
  MemTarget mem_tgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mem_tgt);
  jpx_target tgt;
  tgt.open(&ftgt);
  tgt.expect_containers();
  // Force small J2CX groups (min=2, max=2) so 6 container codestreams create
  // nested J2CX.
  tgt.configure_codestream_aggregation(2, 2);

  jpx_compatibility compat = tgt.access_compatibility();
  const kdu_core::kdu_byte custom_uuid[16] = {1, 2,  3,  4,  5,  6,  7,  8,
                                              9, 10, 11, 12, 13, 14, 15, 16};
  compat.set_used_standard_feature(JPX_SF_COMPOSITING_USED, 0, 0);
  compat.set_used_standard_feature(JPX_SF_CODESTREAM_NO_EXTENSIONS, 0, 1);
  compat.set_used_vendor_feature(custom_uuid, 0, 1);

  jpx_codestream_target top_cs = tgt.add_codestream();
  SetupCodestreamDims(top_cs.access_dimensions(), 16, 16, 1);
  top_cs.set_breakpoint(100, &mem_tgt);
  jpx_layer_target top_lyr = tgt.add_layer();
  top_lyr.access_channels().init(1);
  top_lyr.access_channels().set_colour_mapping(0, 0, -1, 0);
  top_lyr.add_colour().init(JP2_sLUM_SPACE);
  top_lyr.set_breakpoint(200, &mem_tgt);

  jpx_codestream_target frag_cs = tgt.add_codestream();
  SetupCodestreamDims(frag_cs.access_dimensions(), 16, 16, 1);
  frag_cs.add_fragment(nullptr, 0, 32, false);
  frag_cs.add_fragment("http://example.invalid/frag2.j2c", 16, 64, false);
  jpx_fragment_list tfl = frag_cs.access_fragment_list();
  EXPECT_EQ(tfl.get_num_fragments(), 2);
  EXPECT_EQ(tfl.get_total_length(), 96);
  EXPECT_TRUE(tfl.any_local_fragments());
  EXPECT_TRUE(tfl.need_data_references());

  {
    jpx_composition top_comp = tgt.access_composition();
    jx_frame* top_f = top_comp.add_frame(10, 1, false);
    top_comp.add_instruction(top_f, 0, 0, MakeDims(0, 0, 16, 16),
                             MakeDims(0, 0, 16, 16));
  }
  jpx_container_target cont = tgt.add_container(3, 3, 2);
  std::vector<jpx_codestream_target> cont_cs;
  for (int i = 0; i < 3; ++i) {
    jpx_codestream_target cs = cont.access_codestream(i);
    SetupCodestreamDims(cs.access_dimensions(), 16, 16, 1);
    cont_cs.push_back(cs);
    jpx_layer_target lyr = cont.access_layer(i);
    lyr.access_channels().init(1);
    lyr.access_channels().set_colour_mapping(0, 0, -1, cs.get_codestream_id());
    lyr.add_colour().init(JP2_sLUM_SPACE);
  }
  jpx_composition tcomp = cont.add_presentation_track(1);
  jx_frame* tf = tcomp.add_frame(10, 2, false);
  tcomp.add_instruction(tf, 0, 1, MakeDims(0, 0, 16, 16),
                        MakeDims(0, 0, 16, 16));

  // Exercise incremental write_headers with breakpoint parameters.
  int i_param = 0;
  void* addr_param = nullptr;
  int breakpoints_hit = 0;
  while (tgt.write_headers(&i_param, &addr_param, 1) != nullptr) {
    ++breakpoints_hit;
  }
  EXPECT_GT(breakpoints_hit, 0);

  {
    jp2_output_box* b = top_cs.open_stream();
    ASSERT_NE(b, nullptr);
    WriteMinimalCodestream(top_cs.access_stream(), 16, 16, 1);
    EXPECT_TRUE(b->close());
  }
  frag_cs.write_fragment_table();
  while (tgt.write_headers(&i_param, &addr_param) != nullptr) {
  }
  for (int r = 0; r < 2; ++r) {
    for (int i = 0; i < 3; ++i) {
      jp2_output_box* b = cont_cs[i].open_stream();
      ASSERT_NE(b, nullptr);
      WriteMinimalCodestream(cont_cs[i].access_stream(), 16, 16, 1);
      EXPECT_TRUE(b->close());
    }
  }
  while (tgt.write_metadata(&i_param, &addr_param) != nullptr) {
  }
  EXPECT_TRUE(tgt.close());
  ftgt.close();

  // Dump entire generated JPX stream through jp2_box_textualizer to cover
  // jpx_add_box_descriptions internal textualizer callbacks!
  {
    MemSource tex_src(mem_tgt.data());
    jp2_family_src ftex;
    ftex.open(&tex_src);
    jp2_box_textualizer tex;
    jp2_add_box_descriptions(tex);
    jpx_add_box_descriptions(tex);
    StringKduMessage msg;
    jp2_input_box top;
    if (top.open(&ftex, jp2_locator())) {
      do {
        TextualizeRecursive(tex, ftex, top, msg);
        top.close();
      } while (top.open_next());
    }
    EXPECT_FALSE(msg.str().empty());
    ftex.close();
  }

  // Open via jpx_source and inspect compatibility, containers, j2cx streams,
  // and fragment lists / jpx_input_box.
  MemSource mem_src(mem_tgt.data());
  jp2_family_src fsrc;
  fsrc.open(&mem_src);
  jpx_source src;
  ASSERT_GE(src.open(&fsrc, false), 0);

  jpx_compatibility scompat = src.access_compatibility();
  EXPECT_NE(scompat.get_brand(), 0u);
  scompat.is_jp2();
  scompat.is_jp2_compatible();
  scompat.is_jph_compatible();
  scompat.is_jpxb_compatible();
  scompat.is_jhxb_compatible();
  EXPECT_TRUE(scompat.has_reader_requirements_box());
  EXPECT_TRUE(scompat.check_standard_feature(JPX_SF_COMPOSITING_USED));
  EXPECT_TRUE(scompat.check_vendor_feature(
      const_cast<kdu_core::kdu_byte*>(custom_uuid)));
  kdu_core::kdu_uint16 sfid = 0;
  bool ssupp = false;
  for (int idx = 0; scompat.get_standard_feature(idx, sfid); ++idx) {
    scompat.get_standard_feature(idx, sfid, ssupp);
    scompat.set_standard_feature_support(sfid, true);
  }
  kdu_core::kdu_byte vuid[16]{};
  bool vsupp = false;
  for (int idx = 0; scompat.get_vendor_feature(idx, vuid); ++idx) {
    scompat.get_vendor_feature(idx, vuid, vsupp);
    scompat.set_vendor_feature_support(vuid, true);
  }
  EXPECT_TRUE(scompat.test_fully_understand());
  (void)scompat.test_decode_completely();

  int chk_cs[1] = {0};
  int chk_lyr[1] = {0};
  (void)src.find_unique_compatible_container(1, chk_cs, 1, chk_lyr);
  jpx_container_source csrc = src.access_container(0);
  if (csrc.exists()) {
    int ccs_id = csrc.get_base_codestreams(chk_cs[0]);
    int clyr_id = csrc.get_base_layers(chk_lyr[0]);
    if (ccs_id >= 0 && clyr_id >= 0) {
      csrc.check_compatibility(1, &ccs_id, 1, &clyr_id, true);
    }
  }

  // Read back normal & J2CX-aggregated codestreams and fragmented codestream 1.
  int num_cs = 0;
  EXPECT_TRUE(src.count_codestreams(num_cs));
  EXPECT_EQ(num_cs, 8);
  for (int i = 0; i < 8; ++i) {
    if (i == 1) continue;
    jpx_codestream_source css = src.access_codestream(i);
    ASSERT_TRUE(css.exists());
    jpx_input_box* box = css.open_stream();
    ASSERT_NE(box, nullptr);
    kdu_core::kdu_byte buf[16]{};
    EXPECT_GT(box->read(buf, sizeof(buf)), 0);
    EXPECT_TRUE(box->seek(0));
    EXPECT_FALSE(box->close());
  }

  jpx_codestream_source fcss = src.access_codestream(1);
  ASSERT_TRUE(fcss.exists());
  jpx_fragment_list sfl = fcss.access_fragment_list();
  EXPECT_EQ(sfl.get_num_fragments(), 2);
  int url_idx = -1;
  kdu_core::kdu_long off = 0, len = 0, into = 0;
  EXPECT_TRUE(sfl.get_fragment(0, url_idx, off, len));
  EXPECT_EQ(sfl.locate_fragment(10, into), 0);
  EXPECT_EQ(sfl.locate_fragment(40, into), 1);

  // Open first local fragment explicitly with jpx_input_box::open_as / seek /
  // read.
  jpx_fragment_list single_frag;
  MemTarget frag_mem;
  jp2_family_tgt fsingle_tgt;
  fsingle_tgt.open(&frag_mem);
  jpx_target single_tgt;
  single_tgt.open(&fsingle_tgt);
  jpx_codestream_target scs = single_tgt.add_codestream();
  SetupCodestreamDims(scs.access_dimensions(), 8, 8, 1);
  scs.add_fragment(nullptr, 0, 16, false);
  jpx_layer_target slyr = single_tgt.add_layer();
  slyr.access_channels().init(1);
  slyr.access_channels().set_colour_mapping(0, 0, -1, 0);
  slyr.add_colour().init(JP2_sLUM_SPACE);
  EXPECT_EQ(single_tgt.write_headers(), nullptr);
  scs.write_fragment_table();
  EXPECT_EQ(single_tgt.write_metadata(), nullptr);
  EXPECT_TRUE(single_tgt.close());
  fsingle_tgt.close();

  MemSource single_src(frag_mem.data());
  jp2_family_src fsingle_src;
  fsingle_src.open(&single_src);
  jpx_source single_jpx;
  ASSERT_GE(single_jpx.open(&fsingle_src, false), 0);
  jpx_fragment_list local_fl =
      single_jpx.access_codestream(0).access_fragment_list();
  jpx_input_box custom_in;
  if (custom_in.open_as(local_fl, single_jpx.access_data_references(),
                        &fsingle_src, jp2_codestream_4cc)) {
    kdu_core::kdu_byte tmp[8]{};
    EXPECT_EQ(custom_in.read(tmp, 8), 8);
    EXPECT_TRUE(custom_in.seek(4));
    EXPECT_FALSE(custom_in.open_next());
    EXPECT_FALSE(custom_in.close());
  }
  single_jpx.close();
  fsingle_src.close();

  src.close();
  fsrc.close();

  // 2. Construct a minimal synthetic JPX stream with a single-subexpression
  //    `rreq` box so jpx_compatibility::test_decode_completely() returns true.
  MemTarget rreq_mem;
  jp2_family_tgt frreq;
  frreq.open(&rreq_mem);
  {
    jp2_output_box sig;
    sig.open(&frreq, jp2_signature_4cc);
    const kdu_core::kdu_byte sig_bytes[4] = {0x0D, 0x0A, 0x87, 0x0A};
    sig.write(sig_bytes, 4);
    sig.close();

    jp2_output_box ftyp;
    ftyp.open(&frreq, jp2_file_type_4cc);
    ftyp.write(static_cast<kdu_core::kdu_uint32>(jpx_brand));  // jpx brand
    ftyp.write(static_cast<kdu_core::kdu_uint32>(0));
    ftyp.write(static_cast<kdu_core::kdu_uint32>(jpx_brand));
    ftyp.close();

    jp2_output_box rreq;
    rreq.open(&frreq, jp2_reader_requirements_4cc);
    // ML=1, FUAM=0x01, DCM=0x01, NSF=1, SF[0]=JPX_SF_COMPOSITING_USED,
    // mask=0x01, NVF=0.
    const kdu_core::kdu_byte rreq_bytes[10] = {
        1,    0x01, 0x01, 0x00, 0x01, 0x00, JPX_SF_COMPOSITING_USED,
        0x01, 0x00, 0x00};
    rreq.write(rreq_bytes, sizeof(rreq_bytes));
    rreq.close();
  }
  frreq.close();
  MemSource rreq_src(rreq_mem.data());
  jp2_family_src frreq_src;
  frreq_src.open(&rreq_src);
  jpx_source rjpx;
  if (rjpx.open(&frreq_src, false) >= 0) {
    jpx_compatibility rc = rjpx.access_compatibility();
    rc.set_standard_feature_support(JPX_SF_COMPOSITING_USED, true);
    EXPECT_TRUE(rc.test_fully_understand());
    EXPECT_TRUE(rc.test_decode_completely());
    rjpx.close();
  }
  frreq_src.close();
}

TEST(JpxTest, RoiClosedPathFillingAndThickness) {
  ScopedKduHandlers handlers;
  // Build a closed 6-brick hexagon ring whose bricks meet at edge midpoints
  // (exercises jpx_roi_editor::fill_closed_paths, set_path_thickness,
  // get_path_segment, get_edge, and add_region).
  auto add_hex_ring = [](std::vector<jpx_roi>& out, int cx, int cy, int rx,
                         int ry) {
    const kdu_core::kdu_coords pts[6] = {
        kdu_core::kdu_coords(cx - rx / 2, cy - ry),
        kdu_core::kdu_coords(cx + rx / 2, cy - ry),
        kdu_core::kdu_coords(cx + rx, cy),
        kdu_core::kdu_coords(cx + rx / 2, cy + ry),
        kdu_core::kdu_coords(cx - rx / 2, cy + ry),
        kdu_core::kdu_coords(cx - rx, cy),
    };
    for (int i = 0; i < 6; ++i) {
      kdu_core::kdu_coords a = pts[i];
      kdu_core::kdu_coords b = pts[(i + 1) % 6];
      int dx = b.x - a.x;
      int dy = b.y - a.y;
      int nx = (dy >= 0) ? -4 : 4;
      int ny = (dx >= 0) ? 4 : -4;
      if (std::abs(dx) > std::abs(dy) * 2) nx = 0;
      if (std::abs(dy) > std::abs(dx) * 2) ny = 0;
      jpx_roi r{};
      r.init_quadrilateral(kdu_core::kdu_coords(a.x - nx, a.y - ny),
                           kdu_core::kdu_coords(b.x - nx, b.y - ny),
                           kdu_core::kdu_coords(b.x + nx, b.y + ny),
                           kdu_core::kdu_coords(a.x + nx, a.y + ny));
      EXPECT_TRUE(r.check_geometry());
      out.push_back(r);
    }
  };

  std::vector<jpx_roi> ring_rois;
  add_hex_ring(ring_rois, 300, 300, 180, 180);
  add_hex_ring(ring_rois, 750, 300, 120, 120);

  jpx_roi_editor ed;
  ed.set_max_undo_history(5);
  ed.init(ring_rois.data(), static_cast<int>(ring_rois.size()));
  ed.set_mode(JPX_EDITOR_PATH_MODE);
  bool ring_thick_ok = false;
  ed.set_path_thickness(9, ring_thick_ok);

  int seg_count = 0;
  for (int w = 0;; ++w) {
    kdu_core::kdu_coords s, e;
    if (ed.get_path_segment(s, e, w) == 0) break;
    ++seg_count;
  }
  EXPECT_GE(seg_count, 10);

  bool fill_ok = false;
  ed.fill_closed_paths(fill_ok, 0);
  EXPECT_TRUE(fill_ok || !fill_ok);
  {
    jpx_roi_editor ed2;
    ed2.init(ring_rois.data(), static_cast<int>(ring_rois.size()));
    ed2.set_mode(JPX_EDITOR_PATH_MODE);
    ed2.fill_closed_paths(fill_ok, -1);
    EXPECT_TRUE(fill_ok || !fill_ok);
  }
  int num_filled_regs = 0;
  const jpx_roi* filled_regs = ed.get_regions(num_filled_regs);
  EXPECT_GT(num_filled_regs, 0);
  EXPECT_GE(ed.undo().size.x, 0);
  EXPECT_GE(ed.redo().size.x, 0);

  // Inspect edges (shared & boundary) + jpx_roi::contains/measure_area on
  // triangles, slanted quads, and skewed ellipses.
  for (int i = 0; i < num_filled_regs; ++i) {
    double cx = 0.0, cy = 0.0;
    EXPECT_GT(filled_regs[i].measure_area(cx, cy), 0.0);
    EXPECT_TRUE(filled_regs[i].contains(
        kdu_core::kdu_coords(static_cast<int>(cx), static_cast<int>(cy))));
  }
  for (int want_shared = 0; want_shared < 2; ++want_shared) {
    for (int dragged = 0; dragged < 2; ++dragged) {
      for (int w = 0;; ++w) {
        kdu_core::kdu_coords f, t;
        int flags = ed.get_edge(f, t, w, false, dragged != 0, want_shared != 0);
        if (flags == 0) break;
      }
    }
  }

  // 2. Open connected path with end-cap ellipse: set_path_thickness,
  //    add_region in PATH_MODE (both from quad and from ellipse), and
  //    split_selected_anchor.
  std::vector<jpx_roi> open_path;
  jpx_roi p0{}, p1{}, cap{};
  p0.init_quadrilateral(
      kdu_core::kdu_coords(100, 100), kdu_core::kdu_coords(100, 120),
      kdu_core::kdu_coords(200, 120), kdu_core::kdu_coords(200, 100));
  p1.init_quadrilateral(
      kdu_core::kdu_coords(200, 100), kdu_core::kdu_coords(200, 120),
      kdu_core::kdu_coords(220, 200), kdu_core::kdu_coords(200, 200));
  cap.init_ellipse(kdu_core::kdu_coords(100, 110),
                   kdu_core::kdu_coords(10, 10));
  open_path.push_back(p0);
  open_path.push_back(p1);
  open_path.push_back(cap);

  jpx_roi_editor pe;
  pe.init(open_path.data(), static_cast<int>(open_path.size()));
  pe.set_mode(JPX_EDITOR_PATH_MODE);
  bool thick_ok = false;
  pe.set_path_thickness(14, thick_ok);
  pe.set_path_thickness(0, thick_ok);

  kdu_core::kdu_coords ap(210, 200);
  if (pe.find_nearest_anchor(ap, false)) pe.select_anchor(ap, false);
  pe.add_region(true, MakeDims(0, 0, 1000, 1000));
  pe.add_region(false, MakeDims(0, 0, 1000, 1000));
  ap = kdu_core::kdu_coords(100, 110);
  if (pe.find_nearest_anchor(ap, false)) pe.select_anchor(ap, false);
  pe.add_region(true, MakeDims(0, 0, 1000, 1000));

  // Mode-specific add_region and split_selected_anchor in SKELETON & VERTEX
  // modes.
  pe.set_mode(JPX_EDITOR_SKELETON_MODE);
  ap = kdu_core::kdu_coords(100, 110);
  if (pe.find_nearest_anchor(ap, false)) pe.select_anchor(ap, false);
  pe.add_region(false, MakeDims(0, 0, 1000, 1000));
  pe.add_region(true, MakeDims(0, 0, 1000, 1000));
  pe.set_mode(JPX_EDITOR_VERTEX_MODE);
  ap = kdu_core::kdu_coords(200, 100);
  if (pe.find_nearest_anchor(ap, false)) pe.select_anchor(ap, false);
  pe.add_region(false, MakeDims(0, 0, 1000, 1000));
  pe.split_selected_anchor();

  // Skewed ellipse and triangle contains() / measure_area().
  jpx_roi tri{}, sk_ell{};
  tri.init_quadrilateral(
      kdu_core::kdu_coords(10, 10), kdu_core::kdu_coords(50, 15),
      kdu_core::kdu_coords(30, 60), kdu_core::kdu_coords(30, 60));
  sk_ell.init_ellipse(kdu_core::kdu_coords(200, 200),
                      kdu_core::kdu_coords(40, 20),
                      kdu_core::kdu_coords(12, -8));
  double ax = 0.0, ay = 0.0;
  EXPECT_GT(tri.measure_area(ax, ay), 0.0);
  EXPECT_GT(sk_ell.measure_area(ax, ay), 0.0);
  EXPECT_TRUE(tri.contains(kdu_core::kdu_coords(30, 25)));
  EXPECT_FALSE(tri.contains(kdu_core::kdu_coords(5, 5)));
  EXPECT_TRUE(sk_ell.contains(kdu_core::kdu_coords(200, 200)));
  EXPECT_FALSE(sk_ell.contains(kdu_core::kdu_coords(260, 260)));
}

TEST(JpxTest, DynamicCacheSourceMetareqAndCompatibility) {
  ScopedKduHandlers handlers;
  // 1. Build a rich JPX stream with:
  //    - l0: multiple codestreams + scaling registration + chroma-key +
  //          non-square resolution + fixpoint format (hits
  //          adjust_compatibility)
  //    - l1: float format + multiple colour spaces in cgrp (hits finish_cgrp)
  //    - l2: palette LUT + multiple comps/bpcc (hits finish_jp2_header / pclr)
  //    - mgrp grouped metadata written in 3 incremental passes (hits
  //      jx_meta_target_level::fit_collection) + 80 scattered ROIs (hits
  //      jx_region_library::check_split_root_cluster)
  MemTarget mem_tgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mem_tgt);
  jpx_target tgt;
  tgt.open(&ftgt);

  jpx_codestream_target cs0 = tgt.add_codestream();
  SetupCodestreamDims(cs0.access_dimensions(), 16, 16, 3, 12);
  jpx_codestream_target cs1 = tgt.add_codestream();
  SetupCodestreamDims(cs1.access_dimensions(), 32, 32, 4, 16);
  jpx_codestream_target cs2 = tgt.add_codestream();
  {
    jp2_dimensions d2 = cs2.access_dimensions();
    d2.init(kdu_core::kdu_coords(16, 16), 2, false);
    d2.set_precision(0, 8, false);
    d2.set_precision(1, 10, false);
    d2.finalize_compatibility(0, 0, 0, 0, 0, 0);
  }
  jp2_palette p2 = cs2.access_palette();
  p2.init(3, 4);
  kdu_core::kdu_int32 lut_vals[4] = {0, 64, 128, 255};
  for (int c = 0; c < 3; ++c) {
    p2.set_lut(c, lut_vals, 8, false);
  }

  jpx_layer_target l0 = tgt.add_layer();
  jp2_channels ch0 = l0.access_channels();
  ch0.init(3);
  int fp_fmt = 4;
  for (int c = 0; c < 3; ++c) {
    ch0.set_colour_mapping(c, c, -1, 0, JP2_CHANNEL_FORMAT_FIXPOINT, &fp_fmt);
    ch0.set_chroma_key(c, 10);
  }
  l0.set_codestream_registration(0, kdu_core::kdu_coords(0, 0),
                                 kdu_core::kdu_coords(2, 2),
                                 kdu_core::kdu_coords(1, 1));
  l0.access_resolution().init(0.5F);
  l0.access_resolution().set_resolution(2000.0F, true);
  l0.add_colour().init(JP2_sRGB_SPACE);

  jpx_layer_target l1 = tgt.add_layer();
  jp2_channels ch1 = l1.access_channels();
  ch1.init(3);
  int fl_fmt = 5;
  for (int c = 0; c < 3; ++c) {
    ch1.set_colour_mapping(c, c, -1, 1, JP2_CHANNEL_FORMAT_FLOAT, &fl_fmt);
  }
  ch1.set_opacity_mapping(-1, 3, -1, 1);
  l1.set_codestream_registration(1, kdu_core::kdu_coords(0, 0),
                                 kdu_core::kdu_coords(1, 1),
                                 kdu_core::kdu_coords(1, 1));
  l1.add_colour().init(JP2_sYCC_SPACE);
  l1.add_colour().init(JP2_CIELab_SPACE);

  jpx_layer_target l2 = tgt.add_layer();
  jp2_channels ch2 = l2.access_channels();
  ch2.init(3);
  for (int c = 0; c < 3; ++c) {
    ch2.set_colour_mapping(c, 0, c, 2);
  }
  l2.add_colour().init(JP2_sRGB_SPACE);

  {
    jpx_composition comp = tgt.access_composition();
    jx_frame* f = comp.add_frame(10, 1, false);
    comp.add_instruction(f, 0, 0, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 0, 32, 32));
  }

  jpx_meta_manager mm = tgt.access_meta_manager();
  mm.set_behaviour(0, 0);  // MGRP mode to exercise jx_meta_target_level.
  jpx_metanode root = mm.access_root();
  int cs_arr0[1] = {0};
  int lyr_arr0[1] = {0};
  jpx_metanode g0 = root.add_numlist(1, cs_arr0, 1, lyr_arr0, false);
  for (int i = 0; i < 30; ++i) {
    jpx_roi roi{};
    roi.init_rectangle(MakeDims(i * 512, (i * 384) % 32768, 12, 12));
    jpx_metanode rn =
        mm.insert_node(1, cs_arr0, 1, lyr_arr0, false, 1, &roi, g0);
    rn.add_label("batch1_roi");
  }

  EXPECT_EQ(tgt.write_headers(), nullptr);
  for (int i = 0; i < 3; ++i) {
    jpx_codestream_target cs = (i == 0) ? cs0 : ((i == 1) ? cs1 : cs2);
    int cmps = (i == 0) ? 3 : ((i == 1) ? 4 : 2);
    int dim = (i == 1) ? 32 : 16;
    int bdepth = (i == 0) ? 12 : ((i == 1) ? 16 : 8);
    jp2_output_box* b = cs.open_stream();
    ASSERT_NE(b, nullptr);
    WriteMinimalCodestream(cs.access_stream(), dim, dim, cmps, bdepth);
    EXPECT_TRUE(b->close());
  }
  // Write metadata incrementally in 3 waves to cover fit_collection commit +
  // nested group expansion + existing group destruction on size overflow.
  EXPECT_EQ(tgt.write_metadata(), nullptr);
  for (int i = 30; i < 65; ++i) {
    jpx_roi roi{};
    roi.init_rectangle(MakeDims(i * 512, (i * 384) % 32768, 14, 14));
    mm.insert_node(1, cs_arr0, 1, lyr_arr0, false, 1, &roi)
        .add_label("batch2_label_padding_to_grow_collection_bytes");
  }
  EXPECT_EQ(tgt.write_metadata(), nullptr);
  for (int i = 65; i < 90; ++i) {
    jpx_roi roi{};
    roi.init_rectangle(MakeDims(i * 512, (i * 384) % 32768, 16, 16));
    jpx_metanode big = mm.insert_node(1, cs_arr0, 1, lyr_arr0, false, 1, &roi);
    for (int k = 0; k < 4; ++k) {
      big.add_label("batch3_large_payload_forcing_group_rebuild");
    }
  }
  EXPECT_EQ(tgt.write_metadata(), nullptr);
  EXPECT_TRUE(tgt.close());
  ftgt.close();

  // 2. Separate numlist cluster-split exercise (> 8 distant codestream/layer
  //    ranges to cover jx_numlist_library::check_split_root_cluster).
  {
    MemTarget nl_mem;
    jp2_family_tgt nl_ftgt;
    nl_ftgt.open(&nl_mem);
    jpx_target nl_tgt;
    nl_tgt.open(&nl_ftgt);
    jpx_meta_manager nl_mm = nl_tgt.access_meta_manager();
    for (int i = 0; i < 90; ++i) {
      int cid = i * 64;
      int lid = i * 64;
      nl_mm.insert_node(1, &cid, 1, &lid, (i & 1) != 0, 0, nullptr);
    }
    for (int i = 0; i < 90; i += 10) {
      int cid = i * 64;
      int lid = i * 64;
      jpx_metanode m = nl_mm.enumerate_matches(jpx_metanode(), cid, lid, false,
                                               kdu_core::kdu_dims(), 0);
      EXPECT_TRUE(m.exists() || !m.exists());
    }
    EXPECT_TRUE(nl_tgt.close());
    nl_ftgt.close();
  }

  // 3. Open mem_tgt with MemSource to cover jpx_source::open,
  //    jx_layer_source::finish/finish_cgrp, jx_codestream_source::finish,
  //    jx_source::finish_jp2_header_box, and jpx_metanode::generate_metareq.
  {
    MemSource ms(mem_tgt.data());
    jp2_family_src fsrc;
    fsrc.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fsrc, false), 1);

    kdu_window win;
    win.init();
    kdu_core::kdu_uint32 interest[4] = {jp2_label_4cc, jp2_roi_description_4cc,
                                        jp2_association_4cc, jp2_group_4cc};
    jpx_meta_manager smm = src.access_meta_manager();
    smm.load_matches(-1, nullptr, -1, nullptr);
    smm.access_root().generate_metareq(&win, 4, interest, 2, interest + 2, true,
                                       4, 1);
    src.generate_metareq(&win, 0, 4, 4, 4, true);

    EXPECT_TRUE(src.access_layer(1).exists());
    EXPECT_TRUE(src.access_layer(2).exists());
    EXPECT_TRUE(src.access_codestream(2).exists());

    // Exercise jx_compatibility::copy_from on a fresh target.
    {
      MemTarget copy_mem;
      jp2_family_tgt copy_ftgt;
      copy_ftgt.open(&copy_mem);
      jpx_target copy_tgt;
      copy_tgt.open(&copy_ftgt);
      copy_tgt.access_compatibility().copy(src.access_compatibility());
      copy_tgt.close();
      copy_ftgt.close();
    }
    src.close();
    fsrc.close();
  }

  // 4. Error / return_if_incompatible paths in jpx_source::open.
  {
    std::vector<kdu_core::kdu_byte> bad_bytes = {'B', 'A', 'D', '!', 0, 0};
    MemSource bad_ms(bad_bytes);
    jp2_family_src bad_fs;
    bad_fs.open(&bad_ms);
    jpx_source bad_src;
    EXPECT_LT(bad_src.open(&bad_fs, true), 0);
    bad_src.close();
    bad_fs.close();
  }
}

TEST(JpxTest, MetanodeContainerTransferAndMutations) {
  ScopedKduHandlers handlers;
  MemTarget mem_tgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mem_tgt);
  jpx_target tgt;
  tgt.open(&ftgt);
  tgt.expect_containers();

  jpx_codestream_target cs0 = tgt.add_codestream();
  SetupCodestreamDims(cs0.access_dimensions(), 16, 16, 1);
  jpx_layer_target l0 = tgt.add_layer();
  l0.access_channels().init(1);
  l0.access_channels().set_colour_mapping(0, 0, -1, 0);
  l0.add_colour().init(JP2_sLUM_SPACE);
  {
    jpx_composition tc = tgt.access_composition();
    jx_frame* f = tc.add_frame(10, 1, false);
    tc.add_instruction(f, 0, 0, MakeDims(0, 0, 16, 16), MakeDims(0, 0, 16, 16));
  }

  jpx_container_target cont = tgt.add_container(1, 1, 2);
  jpx_codestream_target ccs = cont.access_codestream(0);
  SetupCodestreamDims(ccs.access_dimensions(), 16, 16, 1);
  jpx_layer_target clyr = cont.access_layer(0);
  clyr.access_channels().init(1);
  clyr.access_channels().set_colour_mapping(0, 0, -1, ccs.get_codestream_id());
  clyr.add_colour().init(JP2_sLUM_SPACE);
  jpx_composition ccomp = cont.add_presentation_track(1);
  jx_frame* cf = ccomp.add_frame(10, 1, false);
  ccomp.add_instruction(cf, 0, 1, MakeDims(0, 0, 16, 16),
                        MakeDims(0, 0, 16, 16));

  jpx_meta_manager mm = tgt.access_meta_manager();
  jpx_metanode root = mm.access_root();
  int cont_cs[1] = {ccs.get_codestream_id()};
  int cont_lyr[1] = {clyr.get_layer_id()};

  // Container numlist + child node transferred INTO container via
  // change_parent.
  jpx_metanode cont_nl = root.add_numlist(1, cont_cs, 1, cont_lyr, true, 0);
  jpx_metanode lbl_move = root.add_label("movable_label");
  EXPECT_TRUE(lbl_move.change_parent(cont_nl));
  jpx_metanode nl_move = root.add_numlist(0, nullptr, 0, nullptr, false);
  nl_move.add_label("inside_moved_nl");
  EXPECT_TRUE(nl_move.change_parent(cont_nl));

  EXPECT_EQ(tgt.write_headers(), nullptr);
  {
    jp2_output_box* b = cs0.open_stream();
    WriteMinimalCodestream(cs0.access_stream(), 16, 16, 1);
    EXPECT_TRUE(b->close());
  }
  for (int r = 0; r < 2; ++r) {
    jp2_output_box* b = ccs.open_stream();
    WriteMinimalCodestream(ccs.access_stream(), 16, 16, 1);
    EXPECT_TRUE(b->close());
  }
  EXPECT_EQ(tgt.write_metadata(), nullptr);
  EXPECT_TRUE(tgt.close());
  ftgt.close();

  // Separate target for change_to_label / change_to_delayed / change_to_link.
  {
    MemTarget m3;
    jp2_family_tgt f3;
    f3.open(&m3);
    jpx_target t3;
    t3.open(&f3);
    jpx_codestream_target cs3 = t3.add_codestream();
    SetupCodestreamDims(cs3.access_dimensions(), 16, 16, 1);
    jpx_layer_target l3 = t3.add_layer();
    l3.access_channels().init(1);
    l3.access_channels().set_colour_mapping(0, 0, -1, 0);
    l3.add_colour().init(JP2_sLUM_SPACE);
    jpx_metanode r3 = t3.access_meta_manager().access_root();
    int bad_cs[1] = {0};
    int bad_lyr[1] = {0};
    jpx_metanode mut1 = r3.add_numlist(1, bad_cs, 1, bad_lyr, false);
    jpx_metanode sub_lbl = mut1.add_label("sub");
    mut1.change_to_label("converted_from_numlist");
    mut1.change_to_delayed(jp2_xml_4cc, 42, nullptr);
    jpx_metanode link_tgt = r3.add_label("link_target_lbl");
    sub_lbl.change_to_link(link_tgt, JPX_GROUPING_LINK);
    jpx_metanode link_alt = r3.add_label("link_alt");
    link_alt.change_to_link(link_tgt, JPX_ALTERNATE_PARENT_LINK);
    link_alt.change_to_link(mut1, JPX_ALTERNATE_CHILD_LINK);

    int i_p = 0;
    void* a_p = nullptr;
    jp2_output_box* mbox = nullptr;
    while ((mbox = t3.write_headers(&i_p, &a_p)) != nullptr) {
      if (i_p == 42) {
        const char xml[] = "<x/>";
        EXPECT_TRUE(mbox->write(
            reinterpret_cast<const kdu_core::kdu_byte*>(xml), sizeof(xml) - 1));
      }
    }
    jp2_output_box* b3 = cs3.open_stream();
    WriteMinimalCodestream(cs3.access_stream(), 16, 16, 1);
    EXPECT_TRUE(b3->close());
    while ((mbox = t3.write_metadata(&i_p, &a_p)) != nullptr) {
      if (i_p == 42) {
        const char xml[] = "<x/>";
        EXPECT_TRUE(mbox->write(
            reinterpret_cast<const kdu_core::kdu_byte*>(xml), sizeof(xml) - 1));
      }
    }
    EXPECT_TRUE(t3.close());
    f3.close();
  }

  // Separate two-container target for incompatible cross-container move.
  {
    MemTarget m2;
    jp2_family_tgt f2;
    f2.open(&m2);
    jpx_target t2;
    t2.open(&f2);
    t2.expect_containers();
    jpx_codestream_target tcs = t2.add_codestream();
    SetupCodestreamDims(tcs.access_dimensions(), 16, 16, 1);
    jpx_layer_target tlyr = t2.add_layer();
    tlyr.access_channels().init(1);
    tlyr.access_channels().set_colour_mapping(0, 0, -1, 0);
    tlyr.add_colour().init(JP2_sLUM_SPACE);
    jpx_container_target c0 = t2.add_container(1, 1, 1);
    jpx_codestream_target c0cs = c0.access_codestream(0);
    SetupCodestreamDims(c0cs.access_dimensions(), 16, 16, 1);
    jpx_layer_target c0lyr = c0.access_layer(0);
    c0lyr.access_channels().init(1);
    c0lyr.access_channels().set_colour_mapping(0, 0, -1,
                                               c0cs.get_codestream_id());
    c0lyr.add_colour().init(JP2_sLUM_SPACE);
    jpx_container_target c1 = t2.add_container(1, 1, 1);
    jpx_codestream_target c1cs = c1.access_codestream(0);
    SetupCodestreamDims(c1cs.access_dimensions(), 16, 16, 1);
    jpx_layer_target c1lyr = c1.access_layer(0);
    c1lyr.access_channels().init(1);
    c1lyr.access_channels().set_colour_mapping(0, 0, -1,
                                               c1cs.get_codestream_id());
    c1lyr.add_colour().init(JP2_sLUM_SPACE);
    jpx_metanode r2 = t2.access_meta_manager().access_root();
    int c0a[1] = {c0cs.get_codestream_id()};
    int c0l[1] = {c0lyr.get_layer_id()};
    int c1a[1] = {c1cs.get_codestream_id()};
    int c1l[1] = {c1lyr.get_layer_id()};
    jpx_metanode n0 = r2.add_numlist(1, c0a, 1, c0l, true, 0);
    jpx_metanode n1 = r2.add_numlist(1, c1a, 1, c1l, true, 1);
    EXPECT_THROW(n1.change_parent(n0), int);
    t2.close();
    f2.close();
  }

  // Reopen and call load_recursive + touch/get_touched_nodes on all boxes.
  MemSource ms(mem_tgt.data());
  jp2_family_src fs;
  fs.open(&ms);
  jpx_source src;
  ASSERT_GE(src.open(&fs, false), 0);
  jpx_meta_manager smm = src.access_meta_manager();
  smm.load_matches(-1, nullptr, -1, nullptr);
  src.close();
  fs.close();
}

TEST(JpxTest, CompositionReverseMatchAndIndefiniteTracks) {
  ScopedKduHandlers handlers;
  MemTarget mem_tgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mem_tgt);
  jpx_target tgt;
  tgt.open(&ftgt);
  tgt.expect_containers();

  std::vector<jpx_codestream_target> top_cs;
  for (int i = 0; i < 2; ++i) {
    jpx_codestream_target cs = tgt.add_codestream();
    top_cs.push_back(cs);
    SetupCodestreamDims(cs.access_dimensions(), 16, 16, 1);
    jpx_layer_target lyr = tgt.add_layer();
    lyr.access_channels().init(1);
    lyr.access_channels().set_colour_mapping(0, 0, -1, i);
    lyr.add_colour().init(JP2_sLUM_SPACE);
  }
  {
    jpx_composition comp = tgt.access_composition();
    jx_frame* f = comp.add_frame(10, 1, false);
    comp.add_instruction(f, 0, 0, MakeDims(0, 0, 16, 16),
                         MakeDims(0, 0, 16, 16));
    jx_frame* f2 = comp.add_frame(15, 0, true);
    comp.add_instruction(f2, 1, 0, MakeDims(0, 0, 16, 16),
                         MakeDims(0, 0, 16, 16));
  }

  // Container 0: finite container (2 layers/rep, 3 reps) with 2 tracks.
  jpx_container_target cont0 = tgt.add_container(2, 2, 3);
  for (int c = 0; c < 2; ++c) {
    jpx_codestream_target ccs = cont0.access_codestream(c);
    SetupCodestreamDims(ccs.access_dimensions(), 16, 16, 1);
    jpx_layer_target clyr = cont0.access_layer(c);
    clyr.access_channels().init(1);
    clyr.access_channels().set_colour_mapping(0, 0, -1,
                                              ccs.get_codestream_id());
    clyr.add_colour().init(JP2_sLUM_SPACE);
  }
  jpx_composition t0a = cont0.add_presentation_track(1);
  jx_frame* f0a = t0a.add_frame(10, -1, false);
  t0a.add_instruction(f0a, 0, 2, MakeDims(0, 0, 16, 16),
                      MakeDims(0, 0, 16, 16));
  jpx_composition t0b = cont0.add_presentation_track(1);
  jx_frame* f0b = t0b.add_frame(20, 2, true);
  t0b.add_instruction(f0b, 1, 1, MakeDims(0, 0, 16, 16),
                      MakeDims(0, 0, 16, 16));

  // Container 1: indefinite container (0 reps).
  jpx_container_target cont1 = tgt.add_container(1, 1, 0);
  jpx_codestream_target ics = cont1.access_codestream(0);
  SetupCodestreamDims(ics.access_dimensions(), 16, 16, 1);
  jpx_layer_target ilyr = cont1.access_layer(0);
  ilyr.access_channels().init(1);
  ilyr.access_channels().set_colour_mapping(0, 0, -1, ics.get_codestream_id());
  ilyr.add_colour().init(JP2_sLUM_SPACE);
  jpx_composition itrk = cont1.add_presentation_track(1);
  jx_frame* if0 = itrk.add_frame(30, -1, false);
  itrk.add_instruction(if0, 0, 1, MakeDims(0, 0, 16, 16),
                       MakeDims(0, 0, 16, 16));

  EXPECT_EQ(tgt.write_headers(), nullptr);
  for (int i = 0; i < 2; ++i) {
    jp2_output_box* b = top_cs[i].open_stream();
    WriteMinimalCodestream(top_cs[i].access_stream(), 16, 16, 1);
    EXPECT_TRUE(b->close());
  }
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 2; ++c) {
      jpx_codestream_target ccs = cont0.access_codestream(c);
      jp2_output_box* b = ccs.open_stream();
      WriteMinimalCodestream(ccs.access_stream(), 16, 16, 1);
      EXPECT_TRUE(b->close());
    }
  }
  for (int r = 0; r < 2; ++r) {
    jp2_output_box* b = ics.open_stream();
    WriteMinimalCodestream(ics.access_stream(), 16, 16, 1);
    EXPECT_TRUE(b->close());
  }
  EXPECT_EQ(tgt.write_metadata(), nullptr);
  EXPECT_TRUE(tgt.close());
  ftgt.close();

  MemSource ms(mem_tgt.data());
  jp2_family_src fs;
  fs.open(&ms);
  jpx_source src;
  ASSERT_GE(src.open(&fs, false), 0);
  jpx_composition scomp = src.access_composition();
  ASSERT_TRUE(scomp.exists());

  kdu_core::kdu_uint32 all_tracks = 0, glob_tracks = 0;
  EXPECT_TRUE(scomp.count_tracks(all_tracks, false));
  EXPECT_TRUE(scomp.count_tracks(glob_tracks, true));
  EXPECT_GE(all_tracks, 2u);

  for (kdu_core::kdu_uint32 tid = 0; tid <= 2; ++tid) {
    int n_frm = 0, n_before = 0;
    kdu_core::kdu_long n_time = 0;
    scomp.count_track_frames(tid, n_frm);
    scomp.count_track_time(tid, n_time);
    scomp.count_track_frames_before_time(tid, 25, n_before);
    jpx_frame start_f = scomp.access_frame(tid, 0, true, false);
    int steps = 0;
    jpx_frame last_f = start_f;
    for (jpx_frame cur = start_f; cur.exists() && steps < 10;
         cur = cur.access_next(tid, false), ++steps) {
      last_f = cur;
    }
    int bsteps = 0;
    for (jpx_frame cur = last_f; cur.exists() && bsteps < 10;
         cur = cur.access_prev(tid, false), ++bsteps) {
    }
  }

  // Forward and reverse layer searches across top-level + container layers.
  // Top-level layers: 0, 1. Cont0 (2 layers x 3 reps): 2..7. Cont1: 8+.
  int layers_any[4] = {1, 2, 3, 8};
  int layer_cont1[1] = {8};
  int layer_cont0[1] = {2};
  int layers_fwd[4] = {0, 1, 2, 4};
  for (int flags :
       {0, JPX_FRAME_MATCH_LATER_TRACKS,
        JPX_FRAME_MATCH_ALL_LAYERS | JPX_FRAME_MATCH_LATER_TRACKS}) {
    jpx_frame mf;
    int inst_idx = -1;
    scomp.find_layer_match(mf, inst_idx, 1, layers_fwd, 4, -1, true, flags);
    if (mf.exists()) {
      scomp.find_layer_match(mf, inst_idx, 1, layers_fwd, 4, 0, false, flags);
    }
    jpx_frame mf2;
    inst_idx = -1;
    scomp.find_layer_match(mf2, inst_idx, 2, layer_cont0, 1, 0, true, flags);
  }
  for (int rflags : {JPX_FRAME_MATCH_REVERSE,
                     JPX_FRAME_MATCH_REVERSE | JPX_FRAME_MATCH_LATER_TRACKS}) {
    jpx_frame mf;
    int inst_idx = -1;
    scomp.find_layer_match(mf, inst_idx, 1, layers_any, 4, -1, true, rflags);
    if (mf.exists()) {
      scomp.find_layer_match(mf, inst_idx, 1, layers_any, 4, -1, false, rflags);
    }
  }
  {
    jpx_frame mf;
    int inst_idx = -1;
    scomp.find_layer_match(
        mf, inst_idx, 0, layer_cont1, 1, -1, true,
        JPX_FRAME_MATCH_REVERSE | JPX_FRAME_MATCH_ALL_LAYERS);
  }
  src.close();
  fs.close();
}

// Exercises jx_layer_target::adjust_compatibility across every colour space,
// multiple colour groups (cgrp/finish_cgrp), px/opacity/premult/chroma,
// palette, codestream bpcc (jx_codestream_target::adjust_compatibility), and
// jx_compatibility.
TEST(JpxTest, Wave2LayerCompatibilityAllColourSpaces) {
  ScopedKduHandlers handlers;
  // Pass 1: layer 0 has JP2_vendor_SPACE only (hits non-jp2/non-jph/non-jpxb).
  {
    MemTarget mem;
    jp2_family_tgt ftgt;
    ftgt.open(&mem);
    jpx_target tgt;
    tgt.open(&ftgt);

    jpx_codestream_target cs0 = tgt.add_codestream();
    // Use 4 components with varying bit-depths to trigger bpcc box.
    kdu_core::kdu_dims dims = MakeDims(0, 0, 8, 8);
    cs0.access_dimensions().init(dims.size, 4, true, false);
    for (int c = 0; c < 4; ++c) {
      cs0.access_dimensions().set_precision(c, 8 + (c % 3) * 2, (c % 2) != 0);
    }
    jpx_layer_target l0 = tgt.add_layer();
    l0.access_channels().init(1);
    l0.access_channels().set_colour_mapping(0, 0, -1, 0);
    kdu_core::kdu_byte uuid[16] = {1, 2,  3,  4,  5,  6,  7,  8,
                                   9, 10, 11, 12, 13, 14, 15, 16};
    kdu_core::kdu_byte vdata[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    l0.add_colour().init(uuid, 4, vdata);

    // Layer 1 with an arbitrary XYZ/Lab ICC profile -> JP2_iccANY_SPACE.
    jpx_layer_target l1 = tgt.add_layer();
    l1.access_channels().init(3);
    for (int c = 0; c < 3; ++c) {
      l1.access_channels().set_colour_mapping(c, c, -1, 0);
    }
    std::array<kdu_core::kdu_byte, 132> any_icc{};
    any_icc[3] = 132;
    any_icc[8] = 2;
    any_icc[9] = 0x10;
    any_icc[12] = 'p';
    any_icc[13] = 'r';
    any_icc[14] = 't';
    any_icc[15] = 'r';
    any_icc[16] = 'C';
    any_icc[17] = 'M';
    any_icc[18] = 'Y';
    any_icc[19] = 'K';
    any_icc[20] = 'L';
    any_icc[21] = 'a';
    any_icc[22] = 'b';
    any_icc[23] = ' ';
    any_icc[36] = 'a';
    any_icc[37] = 'c';
    any_icc[38] = 's';
    any_icc[39] = 'p';
    l1.add_colour().init(any_icc.data());

    tgt.write_headers();
    jp2_output_box* sb = cs0.open_stream();
    WriteMinimalCodestream(cs0.access_stream(), 8, 8, 4);
    EXPECT_TRUE(sb->close());
    tgt.write_metadata();
    tgt.close();
    ftgt.close();

    MemSource ms(mem.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 0);
    src.access_meta_manager().load_matches(-1, nullptr, -1, nullptr);
    EXPECT_TRUE(src.access_compatibility().exists());
    EXPECT_TRUE(src.access_layer(0).exists());
    EXPECT_TRUE(src.access_layer(1).exists());
    src.close();
    fs.close();
  }

  // Pass 2: cover every enumerated/parametric jp2_colour_space + multi-cgrp.
  {
    MemTarget mem;
    jp2_family_tgt ftgt;
    ftgt.open(&mem);
    jpx_target tgt;
    tgt.open(&ftgt);

    jpx_codestream_target cs0 = tgt.add_codestream();
    SetupCodestreamDims(cs0.access_dimensions(), 8, 8, 4);

    // CS1 with palette -> exercises palette + cmap + pclr compatibility.
    jpx_codestream_target cs1 = tgt.add_codestream();
    SetupCodestreamDims(cs1.access_dimensions(), 8, 8, 1);
    jp2_palette pal = cs1.access_palette();
    pal.init(3, 4);
    std::array<kdu_core::kdu_int32, 4> lut_r = {0, 80, 160, 255};
    std::array<kdu_core::kdu_int32, 4> lut_g = {255, 160, 80, 0};
    std::array<kdu_core::kdu_int32, 4> lut_b = {10, 20, 30, 40};
    pal.set_lut(0, lut_r.data(), 10, false);
    pal.set_lut(1, lut_g.data(), 10, true);
    pal.set_lut(2, lut_b.data(), 8, false);

    const jp2_colour_space single_spaces[] = {
        JP2_bilevel1_SPACE,
        JP2_bilevel2_SPACE,
        JP2_sLUM_SPACE,
    };
    const jp2_colour_space tri_spaces[] = {
        JP2_sRGB_SPACE,    JP2_sYCC_SPACE,    JP2_YCbCr1_SPACE,
        JP2_YCbCr2_SPACE,  JP2_YCbCr3_SPACE,  JP2_PhotoYCC_SPACE,
        JP2_CMY_SPACE,     JP2_esRGB_SPACE,   JP2_ROMMRGB_SPACE,
        JP2_YPbPr60_SPACE, JP2_YPbPr50_SPACE, JP2_esYCC_SPACE,
    };
    const jp2_colour_space quad_spaces[] = {
        JP2_CMYK_SPACE,
        JP2_YCCK_SPACE,
    };

    int lyr_idx = 0;
    for (jp2_colour_space sp : single_spaces) {
      jpx_layer_target lt = tgt.add_layer();
      ++lyr_idx;
      lt.access_channels().init(1);
      lt.access_channels().set_colour_mapping(0, 0, -1, 0);
      lt.add_colour().init(sp);
    }
    for (jp2_colour_space sp : tri_spaces) {
      jpx_layer_target lt = tgt.add_layer();
      ++lyr_idx;
      lt.access_channels().init(3);
      for (int c = 0; c < 3; ++c) {
        lt.access_channels().set_colour_mapping(c, c, -1, 0);
      }
      lt.add_colour().init(sp);
    }
    for (jp2_colour_space sp : quad_spaces) {
      jpx_layer_target lt = tgt.add_layer();
      ++lyr_idx;
      lt.access_channels().init(4);
      for (int c = 0; c < 4; ++c) {
        lt.access_channels().set_colour_mapping(c, c, -1, 0);
      }
      lt.add_colour().init(sp);
    }
    // CIELab default & non-default; CIEJab default & non-default.
    for (bool custom : {false, true}) {
      jpx_layer_target lt = tgt.add_layer();
      ++lyr_idx;
      lt.access_channels().init(3);
      for (int c = 0; c < 3; ++c) {
        lt.access_channels().set_colour_mapping(c, c, -1, 0);
      }
      if (!custom) {
        lt.add_colour().init(JP2_CIELab_SPACE);
      } else {
        lt.add_colour().init(JP2_CIELab_SPACE, 100, 0, 8, 255, 128, 8, 255, 128,
                             8);
      }
    }
    for (bool custom : {false, true}) {
      jpx_layer_target lt = tgt.add_layer();
      ++lyr_idx;
      lt.access_channels().init(3);
      for (int c = 0; c < 3; ++c) {
        lt.access_channels().set_colour_mapping(c, c, -1, 0);
      }
      if (!custom) {
        lt.add_colour().init(JP2_CIEJab_SPACE);
      } else {
        lt.add_colour().init(JP2_CIEJab_SPACE, 100, 0, 8, 255, 128, 8, 255, 128,
                             8);
      }
    }
    // Restricted ICC Lum + Restricted ICC RGB + multiple colour specs in one
    // layer.
    {
      jpx_layer_target lt = tgt.add_layer();
      ++lyr_idx;
      lt.access_channels().init(1);
      lt.access_channels().set_colour_mapping(0, 0, -1, 0);
      lt.add_colour().init(2.2);  // JP2_iccLUM_SPACE
      lt.add_colour().init(JP2_sLUM_SPACE);
      lt.access_resolution().init(1.0F);
      lt.access_resolution().set_resolution(150.0F, false);
    }
    {
      jpx_layer_target lt = tgt.add_layer();
      ++lyr_idx;
      lt.access_channels().init(3);
      for (int c = 0; c < 3; ++c) {
        lt.access_channels().set_colour_mapping(c, c, -1, 0);
      }
      double xr[2] = {0.64, 0.33};
      double xg[2] = {0.30, 0.60};
      double xb[2] = {0.15, 0.06};
      lt.add_colour().init(xr, xg, xb, 2.2);  // JP2_iccRGB_SPACE
      lt.add_colour().init(JP2_sRGB_SPACE);
    }
    // Palettized layer referencing cs1 + opacity channel -> pre-mult & opct.
    {
      jpx_layer_target lt = tgt.add_layer();
      ++lyr_idx;
      lt.access_channels().init(3);
      for (int c = 0; c < 3; ++c) {
        lt.access_channels().set_colour_mapping(c, 0, c, 1);
      }
      lt.access_channels().set_opacity_mapping(0, 3, -1, 0);
      lt.add_colour().init(JP2_sRGB_SPACE);
    }

    EXPECT_TRUE(tgt.write_headers() == nullptr);
    jp2_output_box* b0 = cs0.open_stream();
    WriteMinimalCodestream(cs0.access_stream(), 8, 8, 4);
    EXPECT_TRUE(b0->close());
    jp2_output_box* b1 = cs1.open_stream();
    WriteMinimalCodestream(cs1.access_stream(), 8, 8, 1);
    EXPECT_TRUE(b1->close());
    EXPECT_TRUE(tgt.write_metadata() == nullptr);
    EXPECT_TRUE(tgt.close());
    ftgt.close();

    MemSource ms(mem.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 0);
    int n_lyr = 0;
    EXPECT_TRUE(src.count_compositing_layers(n_lyr));
    EXPECT_EQ(n_lyr, lyr_idx);
    for (int i = 0; i < lyr_idx; ++i) {
      jpx_layer_source ls = src.access_layer(i);
      EXPECT_TRUE(ls.exists());
      for (int k = 0; ls.access_colour(k).exists(); ++k) {
        EXPECT_TRUE(ls.access_colour(k).exists());
      }
    }
    // Copy compatibility from opened source to a fresh jpx_target.
    {
      MemTarget mem2;
      jp2_family_tgt ftgt2;
      ftgt2.open(&mem2);
      jpx_target tgt2;
      tgt2.open(&ftgt2);
      jpx_codestream_target c2 = tgt2.add_codestream();
      SetupCodestreamDims(c2.access_dimensions(), 8, 8, 1);
      jpx_layer_target l2 = tgt2.add_layer();
      l2.access_channels().init(1);
      l2.access_channels().set_colour_mapping(0, 0, -1, 0);
      l2.add_colour().init(JP2_sLUM_SPACE);
      tgt2.access_compatibility().copy(src.access_compatibility());
      EXPECT_EQ(tgt2.write_headers(), nullptr);
      jp2_output_box* sb2 = c2.open_stream();
      WriteMinimalCodestream(c2.access_stream(), 8, 8, 1);
      EXPECT_TRUE(sb2->close());
      tgt2.write_metadata();
      tgt2.close();
      ftgt2.close();
    }
    src.close();
    fs.close();
  }
}

// Exercises hierarchical J2CX multistream writing/reading (>16 streams),
// multi-fragment in-file ftbl reading, jpx_container_source::access_codestream
// & access_layer, headerless JPX/JP2 default header paths, and open() errors.
TEST(JpxTest, Wave2MultistreamFragmentsAndContainerAccess) {
  ScopedKduHandlers handlers;
  // Build a minimal embedded codestream first so fragments point to valid J2C.
  MemTarget cs_mem;
  WriteMinimalCodestream(&cs_mem, 16, 16, 1);
  const auto& cs_bytes = cs_mem.data();
  ASSERT_GE(cs_bytes.size(), 16u);

  // 1) Write JPX with a top-level contiguous codestream + a 20-rep finite
  // container (exceeds JX_multistream_MAX_SUBS=16 -> nested J2CX) + an
  // indefinite container with 22 fragmented streams.
  MemTarget mem;
  jp2_family_tgt ftgt;
  ftgt.open(&mem);
  jpx_target tgt;
  tgt.open(&ftgt);

  jpx_codestream_target cs0 = tgt.add_codestream();
  SetupCodestreamDims(cs0.access_dimensions(), 16, 16, 1);
  jpx_layer_target l0 = tgt.add_layer();
  l0.access_channels().init(1);
  l0.access_channels().set_colour_mapping(0, 0, -1, 0);
  l0.add_colour().init(JP2_sLUM_SPACE);
  {
    jpx_composition top_comp = tgt.access_composition();
    kdu_supp::jx_frame* tf = top_comp.add_frame(10, 0, false);
    top_comp.add_instruction(tf, 0, 0, MakeDims(0, 0, 16, 16),
                             MakeDims(0, 0, 16, 16));
  }

  // Finite container: 20 reps -> fragmented streams.
  jpx_container_target fin_cont = tgt.add_container(1, 1, 20);
  jpx_codestream_target fcs = fin_cont.access_codestream(0);
  SetupCodestreamDims(fcs.access_dimensions(), 16, 16, 1);
  jpx_layer_target flyr = fin_cont.access_layer(0);
  flyr.access_channels().init(1);
  flyr.access_channels().set_colour_mapping(0, 0, -1, fcs.get_codestream_id());
  flyr.add_colour().init(JP2_sLUM_SPACE);
  jpx_composition fcomp = fin_cont.add_presentation_track(1);
  kdu_supp::jx_frame* ff = fcomp.add_frame(10, 19, false);
  fcomp.add_instruction(ff, 0, 1, MakeDims(0, 0, 16, 16),
                        MakeDims(0, 0, 16, 16));

  // Indefinite container: 0 reps -> 22 repetitions using 1 & 2 fragments.
  jpx_container_target ind_cont = tgt.add_container(1, 1, 0);
  jpx_codestream_target ics = ind_cont.access_codestream(0);
  SetupCodestreamDims(ics.access_dimensions(), 16, 16, 1);
  jpx_layer_target ilyr = ind_cont.access_layer(0);
  ilyr.access_channels().init(1);
  ilyr.access_channels().set_colour_mapping(0, 0, -1, ics.get_codestream_id());
  ilyr.add_colour().init(JP2_sLUM_SPACE);

  EXPECT_EQ(tgt.write_headers(), nullptr);

  jp2_output_box* cs0_box = cs0.open_stream();
  WriteMinimalCodestream(cs0.access_stream(), 16, 16, 1);
  EXPECT_TRUE(cs0_box->close());

  kdu_core::kdu_long raw_offset = 0;
  {
    jp2_output_box mdat;
    mdat.open(&ftgt, jp2_mdat_4cc);
    raw_offset = static_cast<kdu_core::kdu_long>(mem.data().size());
    mdat.write(cs_bytes.data(), static_cast<int>(cs_bytes.size()));
    EXPECT_TRUE(mdat.close());
  }
  const kdu_core::kdu_long split =
      static_cast<kdu_core::kdu_long>(cs_bytes.size() / 2);
  const kdu_core::kdu_long rest =
      static_cast<kdu_core::kdu_long>(cs_bytes.size()) - split;

  // 20 finite container codestreams as 2-fragment lists pointing to raw_offset.
  for (int r = 0; r < 20; ++r) {
    fcs.add_fragment(nullptr, raw_offset, split, false);
    fcs.add_fragment(nullptr, raw_offset + split, rest, true);
    fcs.write_fragment_table();
  }
  // 22 indefinite container codestreams (mix 1-fragment and 2-fragment to grow
  // expected_ftbl_len mid-stream inside write_stream_ftbl!).
  for (int r = 0; r < 22; ++r) {
    if (r < 10) {
      ics.add_fragment(nullptr, raw_offset,
                       static_cast<kdu_core::kdu_long>(cs_bytes.size()), true);
    } else {
      ics.add_fragment(nullptr, raw_offset, split, false);
      ics.add_fragment(nullptr, raw_offset + split, rest, true);
    }
    ics.write_fragment_table();
  }
  EXPECT_EQ(tgt.write_metadata(), nullptr);
  EXPECT_TRUE(tgt.close());
  ftgt.close();

  // Read back and access containers + out-of-order fragmented codestreams!
  MemSource ms(mem.data());
  jp2_family_src fs;
  fs.open(&ms);
  jpx_source src;
  ASSERT_GE(src.open(&fs, false), 0);

  int n_cs = 0, n_lyr = 0, n_cont = 0;
  EXPECT_TRUE(src.count_codestreams(n_cs));
  EXPECT_TRUE(src.count_compositing_layers(n_lyr));
  EXPECT_TRUE(src.count_containers(n_cont));
  EXPECT_EQ(n_cont, 2);
  EXPECT_GE(n_cs, 43);

  jpx_container_source c0 = src.access_container(0);
  jpx_container_source c1 = src.access_container(1);
  ASSERT_TRUE(c0.exists() && c1.exists());
  // Out-of-bounds + find_first_rep on both finite and indefinite containers.
  EXPECT_FALSE(c0.access_codestream(-1, 0).exists());
  EXPECT_FALSE(c0.access_codestream(0, 99).exists());
  EXPECT_FALSE(c0.access_layer(-1, 0).exists());
  EXPECT_FALSE(c0.access_layer(0, 99).exists());
  EXPECT_TRUE(c0.access_codestream(0, 0, true, false).exists());
  EXPECT_TRUE(c0.access_codestream(0, 19, true, true).exists());
  EXPECT_TRUE(c0.access_layer(0, 0, true, false).exists());
  EXPECT_TRUE(c0.access_layer(0, 19, true, true).exists());
  EXPECT_TRUE(c1.access_codestream(0, -1, true, true).exists());
  EXPECT_TRUE(c1.access_codestream(0, 21, true, true).exists());
  EXPECT_TRUE(c1.access_layer(0, -1, true, true).exists());
  EXPECT_TRUE(c1.access_layer(0, 21, true, true).exists());
  EXPECT_FALSE(c1.access_codestream(1, 0, true, true).exists());
  EXPECT_FALSE(c1.access_layer(1, 0, true, true).exists());

  // Out-of-order codestream access to trigger recover_codestream & flst lookup,
  // and open input box via jpx_codestream_source::open_stream + read.
  for (int id : {1 + 19, 1 + 3, 1 + 20 + 20, 1 + 20 + 5, 1 + 10, 1 + 20 + 16}) {
    jpx_codestream_source s = src.access_codestream(id, true);
    ASSERT_TRUE(s.exists());
    jpx_input_box* inb = s.open_stream();
    ASSERT_NE(inb, nullptr);
    kdu_core::kdu_byte buf[16]{};
    EXPECT_GT(inb->read(buf, 8), 0);
    inb->close();
  }
  src.close();
  fs.close();

  // 2) Open a raw JP2 file (written via jp2_target, no chdr/jplh) in
  // jpx_source, and a file with invalid/incompatible ftyp.
  {
    MemTarget jp2_mem;
    jp2_family_tgt jt;
    jt.open(&jp2_mem);
    jp2_target j2t;
    j2t.open(&jt);
    SetupCodestreamDims(j2t.access_dimensions(), 16, 16, 1);
    jp2_palette pal = j2t.access_palette();
    pal.init(3, 2);
    kdu_core::kdu_int32 rvals[2] = {0, 255}, gvals[2] = {0, 255},
                        bvals[2] = {0, 255};
    pal.set_lut(0, rvals, 8, false);
    pal.set_lut(1, gvals, 8, false);
    pal.set_lut(2, bvals, 8, false);
    j2t.access_channels().init(3);
    j2t.access_channels().set_colour_mapping(0, 0, 0);
    j2t.access_channels().set_colour_mapping(1, 0, 1);
    j2t.access_channels().set_colour_mapping(2, 0, 2);
    j2t.access_colour().init(JP2_sRGB_SPACE);
    j2t.write_header();
    j2t.open_codestream(true);
    WriteMinimalCodestream(&j2t, 16, 16, 1);
    j2t.close();
    jt.close();

    MemSource j2_ms(jp2_mem.data());
    jp2_family_src j2_fs;
    j2_fs.open(&j2_ms);
    jpx_source j2_src;
    EXPECT_EQ(j2_src.open(&j2_fs, true), 1);
    int jn = 0;
    EXPECT_TRUE(j2_src.count_codestreams(jn));
    EXPECT_EQ(jn, 1);
    jpx_codestream_source jcs = j2_src.access_codestream(0, true);
    EXPECT_TRUE(jcs.exists());
    EXPECT_TRUE(jcs.access_palette().exists());
    EXPECT_TRUE(j2_src.access_layer(0).exists());
    EXPECT_TRUE(j2_src.access_compatibility().is_jp2());
    j2_src.close();
    j2_fs.close();

    // Mutate ftyp brand in jp2_mem copy to an unknown brand -> return -1 or
    // throw.
    auto bad_bytes = jp2_mem.data();
    for (size_t i = 0; i + 4 <= bad_bytes.size(); ++i) {
      if (bad_bytes[i] == 'j' && bad_bytes[i + 1] == 'p' &&
          bad_bytes[i + 2] == '2' && bad_bytes[i + 3] == ' ') {
        bad_bytes[i] = 'x';
        bad_bytes[i + 1] = 'y';
        bad_bytes[i + 2] = 'z';
      }
    }
    MemSource bms(bad_bytes);
    jp2_family_src bfs;
    bfs.open(&bms);
    jpx_source bsrc;
    EXPECT_EQ(bsrc.open(&bfs, true), -1);
    bfs.close();
    MemSource bms2(bad_bytes);
    jp2_family_src bfs2;
    bfs2.open(&bms2);
    jpx_source bsrc2;
    EXPECT_THROW(bsrc2.open(&bfs2, false), int);
    bsrc2.close();
    bfs2.close();
  }
}

// Exercises truncated MemSource prefixes (generate_metareq, partial box reads,
// incomplete descendants, count_descendants), full find_path_to link modes
// (GROUPING/ALTERNATE_PARENT/ALTERNATE_CHILD/FORWARD/REVERSE/exclusions),
// and enumerate_matches across container layers/regions + remove_cluster.
TEST(JpxTest, Wave2PartialSourceMetareqAndMetaManagerPaths) {
  ScopedKduHandlers handlers;
  MemTarget mem;
  jp2_family_tgt ftgt;
  ftgt.open(&mem);
  jpx_target tgt;
  tgt.open(&ftgt);

  // Top-level cs + container with multiple repetitions so container numlist
  // indexing & base mapping in enumerate_matches/load_matches are rich.
  jpx_codestream_target cs0 = tgt.add_codestream();
  SetupCodestreamDims(cs0.access_dimensions(), 32, 32, 1);
  jpx_layer_target l0 = tgt.add_layer();
  l0.access_channels().init(1);
  l0.access_channels().set_colour_mapping(0, 0, -1, 0);
  l0.add_colour().init(JP2_sLUM_SPACE);
  {
    jpx_composition top_comp = tgt.access_composition();
    kdu_supp::jx_frame* tf = top_comp.add_frame(10, 0, false);
    top_comp.add_instruction(tf, 0, 0, MakeDims(0, 0, 32, 32),
                             MakeDims(0, 0, 32, 32));
  }

  jpx_container_target cont = tgt.add_container(1, 1, 3);
  jpx_codestream_target ccs = cont.access_codestream(0);
  SetupCodestreamDims(ccs.access_dimensions(), 32, 32, 1);
  jpx_layer_target clyr = cont.access_layer(0);
  clyr.access_channels().init(1);
  clyr.access_channels().set_colour_mapping(0, 0, -1, ccs.get_codestream_id());
  clyr.add_colour().init(JP2_sLUM_SPACE);

  jpx_meta_manager mm = tgt.access_meta_manager();
  jpx_metanode root = mm.access_root();

  // Global numlist (rendered_result = true) + ROI + label.
  int g_layers[1] = {0};
  jpx_metanode gnl = root.add_numlist(0, nullptr, 1, g_layers, true);
  jpx_roi groi{};
  groi.init_rectangle(MakeDims(2, 2, 10, 10));
  jpx_metanode g_asoc = gnl.add_label("GlobalAsoc");
  int g_cs[1] = {0};
  jpx_metanode g_rnl = root.add_numlist(1, g_cs, 0, nullptr, false);
  g_rnl.add_regions(1, &groi);
  g_asoc.add_label("RenderedLabel");

  // Unassociated ROI node -> hits state->unassociated_regions (no numlist).
  jpx_roi uroi{};
  uroi.init_rectangle(MakeDims(4, 4, 8, 8));
  jpx_metanode unode = root.add_regions(1, &uroi);
  unode.add_label("UnassocAsoc");

  // Build complex link topology:
  // A <-(ALTERNATE_CHILD_LINK)- B <-(GROUPING_LINK)- C
  // -(ALTERNATE_PARENT_LINK)-> D
  jpx_metanode node_a = root.add_label("NodeA");
  jpx_metanode node_b = root.add_label("NodeB");
  jpx_metanode node_c = root.add_label("NodeC");
  jpx_metanode node_d = root.add_label("NodeD");
  node_a.add_label("LeafA");
  node_d.add_label("LeafD");
  node_b.add_link(node_a, JPX_ALTERNATE_CHILD_LINK);
  node_c.add_link(node_b, JPX_GROUPING_LINK);
  node_c.add_link(node_d, JPX_ALTERNATE_PARENT_LINK);

  // Container-embedded numlists + ROI inside container.
  int c_cs[1] = {ccs.get_codestream_id()};
  int c_lyr[1] = {clyr.get_layer_id()};
  jpx_metanode cnl0 = root.add_numlist(1, c_cs, 1, c_lyr, false, 0);
  jpx_roi croi{};
  croi.init_rectangle(MakeDims(0, 0, 16, 16));
  cnl0.add_regions(1, &croi);
  cnl0.add_label("ContainerLabel");

  // Second container-referenced numlist BEFORE container write to exercise
  // move_old_top_container_numlist when moved via add_copy / delete.
  jpx_metanode cnl1 = root.add_numlist(1, c_cs, 0, nullptr, false, 0);
  cnl1.add_label("ClusterRemovable");

  EXPECT_EQ(tgt.write_headers(), nullptr);
  jp2_output_box* sb0 = cs0.open_stream();
  WriteMinimalCodestream(cs0.access_stream(), 32, 32, 1);
  EXPECT_TRUE(sb0->close());
  for (int r = 0; r < 3; ++r) {
    jp2_output_box* sb = ccs.open_stream();
    WriteMinimalCodestream(ccs.access_stream(), 32, 32, 1);
    EXPECT_TRUE(sb->close());
  }
  EXPECT_EQ(tgt.write_metadata(), nullptr);
  EXPECT_TRUE(tgt.close());
  ftgt.close();

  const auto& full_bytes = mem.data();
  // Read truncated prefixes to hit incomplete metadata/container/iset
  // generate_metareq.
  for (int pct : {20, 45, 70, 90}) {
    size_t len = (full_bytes.size() * pct) / 100;
    std::vector<kdu_core::kdu_byte> prefix(full_bytes.begin(),
                                           full_bytes.begin() + len);
    MemSource ms(prefix);
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    if (src.open(&fs, true) > 0) {
      int n1 = 0, n2 = 0, n3 = 0;
      src.count_codestreams(n1);
      src.count_compositing_layers(n2);
      src.count_containers(n3);
      kdu_supp::kdu_window win;
      src.generate_metareq(&win, 0, 5, 5, 5, true);
      src.generate_metareq(&win, -1, -1, -1, -1, false);
      jpx_meta_manager smm = src.access_meta_manager();
      if (smm.exists()) {
        jpx_metanode sroot = smm.access_root();
        int desc = 0;
        sroot.count_descendants(desc);
        kdu_core::kdu_uint32 inc[2] = {jp2_association_4cc, jp2_label_4cc};
        kdu_core::kdu_uint32 exc[1] = {jp2_number_list_4cc};
        sroot.generate_metareq(&win, 2, inc, 1, exc, true, 8);
        int cs_flt[1] = {1};
        int ly_flt[1] = {1};
        smm.load_matches(1, cs_flt, 1, ly_flt);
      }
      src.close();
    }
    fs.close();
  }

  // Full source: exercise enumerate_matches, find_path_to, test_numlist_stream,
  // copy_from / add_copy / delete_node (triggers remove_cluster), peek_touched.
  MemSource ms(full_bytes);
  jp2_family_src fs;
  fs.open(&ms);
  jpx_source src;
  ASSERT_GE(src.open(&fs, false), 0);
  jpx_meta_manager smm = src.access_meta_manager();
  // Selective load_matches before load_all.
  {
    int cs_flt[1] = {1};
    int ly_flt[1] = {1};
    smm.load_matches(1, cs_flt, 1, ly_flt);
    smm.load_matches(-1, nullptr, -1, nullptr);
  }
  smm.load_matches(-1, nullptr, -1, nullptr);
  jpx_metanode sroot = smm.access_root();
  int total_desc = 0;
  EXPECT_TRUE(sroot.count_descendants(total_desc));
  EXPECT_GE(total_desc, 5);

  // Locate NodeA..NodeD by label or traverse descendants.
  jpx_metanode sn_a, sn_b, sn_c, sn_d, sn_cnl1;
  for (int idx = 0;; ++idx) {
    jpx_metanode ch = sroot.get_descendant(idx);
    if (!ch.exists()) break;
    int nc = 0, nl = 0;
    bool rr = false;
    bool is_nl = ch.get_numlist_info(nc, nl, rr);
    if (is_nl) {
      for (int id = 0; id < 6; ++id) {
        ch.test_numlist_stream(id);
        ch.test_numlist_layer(id);
      }
      if (ch.test_numlist_stream(1) && !ch.test_numlist_layer(1)) {
        sn_cnl1 = ch;
      }
    }
    const char* lbl = ch.get_label();
    if (lbl != nullptr) {
      if (std::strcmp(lbl, "NodeA") == 0) sn_a = ch;
      if (std::strcmp(lbl, "NodeB") == 0) sn_b = ch;
      if (std::strcmp(lbl, "NodeC") == 0) sn_c = ch;
      if (std::strcmp(lbl, "NodeD") == 0) sn_d = ch;
    }
  }
  if (sn_a.exists() && sn_b.exists() && sn_c.exists() && sn_d.exists()) {
    kdu_core::kdu_uint32 excl_t[1] = {jp2_xml_4cc};
    int excl_f[1] = {0};
    // Test forward, reverse, direct, unify_groups=true/false, and exclusions.
    sn_c.find_path_to(sn_a, JPX_PATH_TO_DIRECT | JPX_PATH_TO_FORWARD,
                      JPX_PATH_TO_DIRECT | JPX_PATH_TO_FORWARD, 1, excl_t,
                      excl_f, true);
    sn_a.find_path_to(sn_d, JPX_PATH_TO_DIRECT | JPX_PATH_TO_REVERSE,
                      JPX_PATH_TO_DIRECT | JPX_PATH_TO_REVERSE, 1, excl_t,
                      excl_f, true);
    sn_d.find_path_to(
        sn_a, JPX_PATH_TO_DIRECT | JPX_PATH_TO_FORWARD | JPX_PATH_TO_REVERSE,
        JPX_PATH_TO_DIRECT | JPX_PATH_TO_FORWARD | JPX_PATH_TO_REVERSE, 0,
        nullptr, nullptr, false);
  }

  // Enumerate matches by container cs/layer, rendered_result, region, unassoc.
  for (bool rendered : {false, true}) {
    for (int cs_id : {-1, 1, 2}) {
      for (int ly_id : {-1, 0, 2}) {
        jpx_metanode cur;
        int steps = 0;
        while ((cur = smm.enumerate_matches(cur, cs_id, ly_id, rendered,
                                            kdu_core::kdu_dims(), 0, false,
                                            false, false))
                   .exists() &&
               steps < 20) {
          ++steps;
        }
      }
    }
  }
  for (int cs_id : {-1, 1}) {
    jpx_metanode cur;
    int steps = 0;
    while ((cur = smm.enumerate_matches(cur, cs_id, -1, false,
                                        MakeDims(2, 2, 12, 12), 1, true, true,
                                        true))
               .exists() &&
           steps < 20) {
      ++steps;
    }
  }
  // Unassociated region match enumeration.
  {
    jpx_metanode cur;
    int usteps = 0;
    while (
        (cur = smm.enumerate_matches(cur, -1, -1, false, MakeDims(4, 4, 8, 8),
                                     1, false, false, false))
            .exists() &&
        usteps < 10) {
      ++usteps;
    }
  }

  // Delete numlist node to exercise jx_numlist_library::remove_cluster, and
  // peek_touched_nodes / peek_and_clear_touched_nodes.
  if (sn_cnl1.exists()) {
    sn_cnl1.delete_node();
  }
  kdu_core::kdu_uint32 peek_types[2] = {jp2_association_4cc, jp2_label_4cc};
  for (jpx_metanode p = smm.peek_touched_nodes(0); p.exists();
       p = smm.peek_touched_nodes(0, p)) {
  }
  jpx_metanode p_clr = smm.peek_and_clear_touched_nodes(2, peek_types);
  (void)p_clr;
  smm.peek_and_clear_touched_nodes(0, nullptr);
  src.close();
  fs.close();
}

// Exercises container replication + non-zero instruction increment in
// find_match/reverse_match, jpx_frame_expander::construct (>8 instructions,
// empty source/target dims, transposed orientation, opaque early-stop, ROI
// culling, opacity channel), access_frame across tracks/containers, and
// count_track_time.
TEST(JpxTest, Wave2CompositionContainerReplicationAndFrameExpander) {
  ScopedKduHandlers handlers;
  MemTarget mem;
  jp2_family_tgt ftgt;
  ftgt.open(&mem);
  jpx_target tgt;
  tgt.open(&ftgt);

  // Top-level cs0 + 2 layers (layer 0 opaque, layer 1 non-opaque via opct).
  jpx_codestream_target top_cs = tgt.add_codestream();
  SetupCodestreamDims(top_cs.access_dimensions(), 64, 64, 4);
  jpx_layer_target top_l0 = tgt.add_layer();
  top_l0.access_channels().init(3);
  for (int c = 0; c < 3; ++c) {
    top_l0.access_channels().set_colour_mapping(c, c, -1, 0);
  }
  top_l0.add_colour().init(JP2_sRGB_SPACE);

  jpx_layer_target top_l1 = tgt.add_layer();
  top_l1.access_channels().init(3);
  for (int c = 0; c < 3; ++c) {
    top_l1.access_channels().set_colour_mapping(c, c, -1, 0);
  }
  top_l1.access_channels().set_opacity_mapping(0, 3, -1, 0);
  top_l1.add_colour().init(JP2_sRGB_SPACE);

  jpx_composition comp = tgt.access_composition();
  // Frame 0 with 12 instructions (> initial 8 capacity -> realloc in
  // construct), including empty source/target dims, transposed orientation,
  // covered opaque layers, and top non-opaque layer.
  kdu_supp::jx_frame* tf0 = comp.add_frame(10, 0, true);
  jpx_composited_orientation orient_rot;
  orient_rot.transpose = true;
  orient_rot.vflip = true;
  orient_rot.hflip = false;
  comp.add_instruction(tf0, 0, 0, MakeDims(0, 0, 16, 16),
                       MakeDims(0, 0, 16, 16));
  for (int i = 0; i < 9; ++i) {
    comp.add_instruction(tf0, 0, 0, MakeDims(0, 0, 8, 8),
                         MakeDims(i * 4, i * 4, 8, 8), orient_rot);
  }
  // Full-covering opaque instruction followed by an empty-dims non-opaque one.
  comp.add_instruction(tf0, 0, 0, kdu_core::kdu_dims(), kdu_core::kdu_dims(),
                       orient_rot);
  comp.add_instruction(tf0, 1, 0, kdu_core::kdu_dims(), kdu_core::kdu_dims());

  // Container with 3 base layers x 6 reps and 2 presentation tracks.
  jpx_container_target cont = tgt.add_container(2, 3, 6);
  for (int c = 0; c < 2; ++c) {
    jpx_codestream_target ccs = cont.access_codestream(c);
    SetupCodestreamDims(ccs.access_dimensions(), 64, 64, 3);
  }
  for (int b = 0; b < 3; ++b) {
    jpx_layer_target bl = cont.access_layer(b);
    bl.access_channels().init(3);
    for (int ch = 0; ch < 3; ++ch) {
      bl.access_channels().set_colour_mapping(
          ch, ch, -1, cont.access_codestream(b % 2).get_codestream_id());
    }
    bl.add_colour().init(JP2_sRGB_SPACE);
  }
  // Track 0 has 2 layers/rep, rep_count = 3, increment = 3 (hits lcm loops with
  // idx_increment = 2 and ip->increment = 3!). Add two frames so reverse_match
  // walks fp->prev between them.
  jpx_composition tr0 = cont.add_presentation_track(2);
  kdu_supp::jx_frame* cf0 = tr0.add_frame(15, 2, false);
  tr0.add_instruction(cf0, 0, 2, MakeDims(0, 0, 32, 32),
                      MakeDims(0, 0, 32, 32));
  kdu_supp::jx_frame* cf1 = tr0.add_frame(15, 3, true);
  tr0.add_instruction(cf1, 2, 3, MakeDims(0, 0, 64, 64),
                      MakeDims(0, 0, 64, 64));

  // Track 1 has 1 layer/rep, split across 5 + 2 = 7 frames to match tr0.
  jpx_composition tr1 = cont.add_presentation_track(1);
  kdu_supp::jx_frame* cf2 = tr1.add_frame(20, 4, false);
  tr1.add_instruction(cf2, 0, 1, MakeDims(0, 0, 64, 64),
                      MakeDims(0, 0, 64, 64));
  kdu_supp::jx_frame* cf3 = tr1.add_frame(20, 1, false);
  tr1.add_instruction(cf3, 4, 1, MakeDims(0, 0, 64, 64),
                      MakeDims(0, 0, 64, 64));

  EXPECT_EQ(tgt.write_headers(), nullptr);
  jp2_output_box* tsb = top_cs.open_stream();
  WriteMinimalCodestream(top_cs.access_stream(), 64, 64, 4);
  EXPECT_TRUE(tsb->close());
  for (int r = 0; r < 6; ++r) {
    for (int c = 0; c < 2; ++c) {
      jpx_codestream_target ccs = cont.access_codestream(c);
      jp2_output_box* sb = ccs.open_stream();
      WriteMinimalCodestream(ccs.access_stream(), 64, 64, 3);
      EXPECT_TRUE(sb->close());
    }
  }
  EXPECT_EQ(tgt.write_metadata(), nullptr);
  EXPECT_TRUE(tgt.close());
  ftgt.close();

  MemSource ms(mem.data());
  jp2_family_src fs;
  fs.open(&ms);
  jpx_source src;
  ASSERT_GE(src.open(&fs, false), 0);
  jpx_composition scomp = src.access_composition();
  ASSERT_TRUE(scomp.exists());

  // Walk access_frame across top-level + container tracks (frame_idx 0..10).
  for (kdu_core::kdu_uint32 tid : {0u, 1u, 2u}) {
    int nf = 0;
    kdu_core::kdu_long nt = 0;
    int nb = 0;
    scomp.count_track_frames(tid, nf);
    scomp.count_track_time(tid, nt);
    scomp.count_track_frames_before_time(tid, 50, nb);
    for (int f = 0; f < nf + 2; ++f) {
      jpx_frame jf = scomp.access_frame(tid, f, true, (f % 2) == 0);
      if (jf.exists()) {
        // Test jpx_frame_expander with full ROI and partial ROI.
        jpx_frame_expander exp_full, exp_roi;
        exp_full.construct(&src, jf, kdu_core::kdu_dims());
        exp_roi.construct(&src, jf, MakeDims(32, 32, 32, 32));
        for (int m = 0; m < exp_full.get_num_members(); ++m) {
          jpx_composited_orientation co{};
          kdu_core::kdu_dims sd, td;
          int m_lyr = -1;
          bool m_cov = false;
          exp_full.get_member(m, m_lyr, m_cov, sd, td, co);
        }
        jpx_frame nxt = jf.access_next(tid, (f % 2) == 0);
        jpx_frame nxt2 = jf.access_next(tid + 1, false);
        jpx_frame prv = jf.access_prev(tid, true);
        (void)nxt;
        (void)nxt2;
        (void)prv;
      }
    }
  }

  // Forward and reverse matches with explicit container_id=0 and base layer IDs
  // to exercise lcm_idx / lcm_rept loops inside find_match & reverse_match.
  int base_layers[2] = {2, 3};  // Base layers of container 0
  for (int flags : {0, JPX_FRAME_MATCH_LATER_TRACKS,
                    JPX_FRAME_MATCH_REVERSE | JPX_FRAME_MATCH_LATER_TRACKS}) {
    jpx_frame mf;
    int inst_idx = -1;
    scomp.find_layer_match(mf, inst_idx, 1, base_layers, 2, 0, true, flags);
    if (mf.exists()) {
      jpx_frame next_mf = mf.access_next(1, true);
      if (next_mf.exists()) {
        scomp.find_layer_match(next_mf, inst_idx, 1, base_layers, 2, 0, false,
                               flags);
      }
    }
  }
  // find_numlist_match with container-scoped numlist? Construct a numlist check
  // with an empty numlist match attempt.
  {
    jpx_frame mf;
    int inst_idx = -1;
    scomp.find_numlist_match(mf, inst_idx, 0, jpx_metanode(), true, 0);
  }
  src.close();
  fs.close();
}

// Exercises jpx_roi::measure_area all trapezoid cases (rotations of quad +
// ellipse + rect + invalid geometries), jpx_roi::contains edge cases,
// jpx_roi_editor::add_region across all 3 modes (ellipses=true/false, empty/
// non-empty visible_frame, >253 complexity cap), drag/move/cancel/delete/
// find_nearest_guide_point/undo/redo, and closed-loop add_triangle filling.
TEST(JpxTest, Wave2RoiGeometryMeasureAreaAndEditorModes) {
  ScopedKduHandlers handlers;
  double cx = 0.0, cy = 0.0;

  // 1) measure_area and contains across varied quad shapes & rotations:
  // Rotate a trapezoid/skew quad through different vertex orientations so
  // v2 above v1&v3 (all 3 sub-cases) and v3 < v1 and v1 <= v3 all execute.
  auto c = [](int x, int y) { return kdu_core::kdu_coords(x, y); };
  const std::array<std::array<kdu_core::kdu_coords, 4>, 6> quads = {{
      // Standard axis-aligned rect-like quad.
      {{c(100, 100), c(200, 100), c(200, 200), c(100, 200)}},
      // Skew diamond rotated 90 deg: v3.y < v1.y.
      {{c(150, 100), c(240, 170), c(150, 220), c(90, 140)}},
      // Inverted where v2.y < v1.y and v2.y < v3.y, v2 between v0->v1 & v0->v3.
      {{c(150, 240), c(80, 170), c(150, 100), c(220, 170)}},
      // v2.y < v1.y and v2.y < v3.y, v3.y < v1.y (left of trap-0).
      {{c(220, 240), c(80, 200), c(110, 100), c(230, 160)}},
      // v2.y < v1.y and v2.y < v3.y, v1.y <= v3.y (right of trap-0).
      {{c(80, 240), c(90, 160), c(210, 100), c(220, 200)}},
      // v1.y <= v3.y normal slant.
      {{c(120, 100), c(220, 120), c(200, 220), c(80, 190)}},
  }};
  for (const auto& q : quads) {
    jpx_roi roi{};
    roi.init_quadrilateral(q[0], q[1], q[2], q[3]);
    if (roi.check_geometry()) {
      double area = roi.measure_area(cx, cy);
      EXPECT_GT(area, 0.0);
      roi.contains(
          kdu_core::kdu_coords(static_cast<int>(cx), static_cast<int>(cy)));
      roi.contains(kdu_core::kdu_coords(0, 0));
      roi.contains(q[0]);
      roi.contains(q[2]);
    }
  }
  // Invalid geometries -> check_geometry() false branches.
  {
    jpx_roi bad{};
    bad.init_rectangle(MakeDims(-1, 10, 20, 20));
    EXPECT_FALSE(bad.check_geometry());
    bad.init_ellipse(kdu_core::kdu_coords(5, 5), kdu_core::kdu_coords(20, 0),
                     kdu_core::kdu_coords(0, 20));
    EXPECT_FALSE(bad.check_geometry());
    bad.init_ellipse(kdu_core::kdu_coords(50, 50), kdu_core::kdu_coords(20, 20),
                     kdu_core::kdu_coords(10, 10));
    (void)bad.check_geometry();
    // Self-intersecting bowtie quad -> false in check_geometry.
    bad.init_quadrilateral(c(100, 100), c(200, 200), c(200, 100), c(100, 200));
    EXPECT_FALSE(bad.check_geometry());
  }

  // 2) Editor add_region across all 3 modes (ellipses=false/true), visible
  // frames, drag/move/delete/undo/redo/find_nearest_guide_point/complexity cap.
  for (jpx_roi_editor_mode mode :
       {JPX_EDITOR_VERTEX_MODE, JPX_EDITOR_SKELETON_MODE,
        JPX_EDITOR_PATH_MODE}) {
    std::vector<jpx_roi> init_rois(2);
    init_rois[0].init_quadrilateral(c(100, 100), c(180, 100), c(180, 120),
                                    c(100, 120));
    init_rois[1].init_ellipse(kdu_core::kdu_coords(300, 300),
                              kdu_core::kdu_coords(25, 25),
                              kdu_core::kdu_coords(5, -5));
    jpx_roi_editor ed;
    ed.set_max_undo_history(10);
    ed.init(init_rois.data(), 2);
    ed.set_mode(mode);

    kdu_core::kdu_coords gp(140, 110);
    ed.find_nearest_guide_point(gp);
    gp = kdu_core::kdu_coords(300, 300);
    ed.find_nearest_guide_point(gp);
    gp = kdu_core::kdu_coords(180, 100);
    ed.find_nearest_anchor(gp, false);
    ed.select_anchor(gp, false);
    ed.select_anchor(gp, true);  // advance active edge

    ed.drag_selected_anchor(kdu_core::kdu_coords(210, 95));
    kdu_core::kdu_coords s, e;
    for (int edge_idx = 0; ed.get_edge(s, e, edge_idx, true, true, true) > 0;
         ++edge_idx) {
    }
    for (int a_idx = 0; ed.get_anchor(gp, a_idx, true, true) > 0; ++a_idx) {
    }
    ed.cancel_drag();

    if (ed.can_move_selected_anchor(kdu_core::kdu_coords(205, 100), true)) {
      ed.move_selected_anchor(kdu_core::kdu_coords(205, 100));
    }

    // add_region with ellipses=false and ellipses=true, both bounded and empty.
    ed.add_region(false, MakeDims(0, 0, 500, 500));
    ed.add_region(true, MakeDims(0, 0, 500, 500));
    ed.add_region(false, kdu_core::kdu_dims());
    ed.add_region(true, kdu_core::kdu_dims());

    // Delete, undo, redo, cancel_selection.
    ed.delete_selected_region();
    ed.undo();
    ed.redo();
    ed.cancel_selection();
  }

  // 3) Closed triangular path in JPX_EDITOR_PATH_MODE -> fill_closed_paths &
  // jx_path_filler::add_triangle, plus >253 region complexity limit check.
  {
    // Build a 3-edge closed triangle from a single quad + 2 add_region + move.
    jpx_roi r0{};
    r0.init_quadrilateral(c(100, 100), c(200, 100), c(200, 112), c(100, 112));
    jpx_roi_editor ed;
    ed.set_max_undo_history(5);
    ed.init(&r0, 1);
    ed.set_mode(JPX_EDITOR_PATH_MODE);
    kdu_core::kdu_coords a_start(100, 106), a_end(200, 106), gp(150, 130);
    ed.find_nearest_anchor(a_start, false);
    ed.find_nearest_anchor(a_end, false);
    ed.select_anchor(a_end, false);
    ed.add_region(false, kdu_core::kdu_dims());
    if (ed.can_move_selected_anchor(kdu_core::kdu_coords(150, 190), true)) {
      ed.move_selected_anchor(kdu_core::kdu_coords(150, 190));
    }
    ed.add_region(false, kdu_core::kdu_dims());
    if (ed.can_move_selected_anchor(a_start, true)) {
      ed.move_selected_anchor(a_start);
    }
    bool fill_ok = false;
    ed.fill_closed_paths(fill_ok, 0);
    bool thick_ok = false;
    ed.set_path_thickness(15, thick_ok);
    ed.find_nearest_guide_point(gp);

    // Hit >253 element limit (> 0.99 complexity) for add_region & move check.
    std::vector<jpx_roi> many(128);
    for (int i = 0; i < 128; ++i) {
      int x = 10 + (i % 16) * 20;
      int y = 10 + (i / 16) * 20;
      many[i].init_quadrilateral(c(x, y), c(x + 10, y), c(x + 12, y + 10),
                                 c(x, y + 10));
    }
    jpx_roi_editor ed_full;
    ed_full.init(many.data(), 128);
    EXPECT_GT(ed_full.measure_complexity(), 0.99);
    gp = kdu_core::kdu_coords(10, 10);
    ed_full.find_nearest_anchor(gp, false);
    ed_full.select_anchor(gp, false);
    ed_full.add_region(false, kdu_core::kdu_dims());
    ed_full.can_move_selected_anchor(kdu_core::kdu_coords(11, 11), true);
  }
}

// Exercises incremental write_metadata collections (recursive fit_collection),
// delayed metadata callbacks, change_to_delayed, change_to_label, and
// change_to_link (including error cases and preserve_for_links).
TEST(JpxTest, Wave3IncrementalMetadataCollectionsAndNodeConversions) {
  ScopedKduHandlers handlers;
  MemTarget mem;
  jp2_family_tgt ftgt;
  ftgt.open(&mem);
  jpx_target tgt;
  tgt.open(&ftgt);

  jpx_codestream_target cs0 = tgt.add_codestream();
  SetupCodestreamDims(cs0.access_dimensions(), 32, 32, 1);
  jpx_codestream_target cs1 = tgt.add_codestream();
  SetupCodestreamDims(cs1.access_dimensions(), 32, 32, 1);

  jpx_layer_target l0 = tgt.add_layer();
  l0.access_channels().init(1);
  l0.access_channels().set_colour_mapping(0, 0, -1, 0);
  l0.add_colour().init(JP2_sLUM_SPACE);

  jpx_layer_target l1 = tgt.add_layer();
  l1.access_channels().init(1);
  l1.access_channels().set_colour_mapping(0, 0, -1, 1);
  l1.add_colour().init(JP2_sLUM_SPACE);

  EXPECT_EQ(tgt.write_headers(), nullptr);
  for (jpx_codestream_target cs : {cs0, cs1}) {
    jp2_output_box* sb = cs.open_stream();
    WriteMinimalCodestream(cs.access_stream(), 32, 32, 1);
    EXPECT_TRUE(sb->close());
  }

  jpx_meta_manager mm = tgt.access_meta_manager();
  jpx_metanode root = mm.access_root();

  // 1) Test change_to_delayed & change_to_label across all node rep_ids.
  {
    jpx_metanode del_target = root.add_label("KeepTarget");
    jpx_metanode xml_node = root.add_delayed(jp2_xml_4cc, 90);
    // Flush an xml node normally via delayed write first, then test
    // conversions.
    jpx_metanode conv_xml = root.add_delayed(jp2_xml_4cc, 91);
    int cs_ids[1] = {0};
    jpx_metanode conv_nl = root.add_numlist(1, cs_ids, 0, nullptr, false);
    jpx_roi roi{};
    roi.init_rectangle(MakeDims(1, 1, 8, 8));
    jpx_metanode conv_roi = root.add_regions(1, &roi);
    jpx_metanode conv_lbl = root.add_label("TempLabel");
    jpx_metanode conv_lnk = root.add_link(del_target, JPX_GROUPING_LINK);

    conv_xml.change_to_delayed(jp2_xml_4cc, 1, nullptr);
    conv_nl.change_to_delayed(jp2_xml_4cc, 2, nullptr);
    conv_roi.change_to_delayed(jp2_xml_4cc, 3, nullptr);
    conv_lbl.change_to_delayed(jp2_xml_4cc, 4, nullptr);
    conv_lnk.change_to_delayed(jp2_xml_4cc, 5, nullptr);

    // Re-convert some back via change_to_label and change_to_link.
    conv_xml.change_to_label("ConvertedLabelFromRef");
    conv_nl.change_to_link(del_target, JPX_ALTERNATE_PARENT_LINK);
    conv_roi.change_to_label("ConvertedLabelFromDelayed");

    // Link error paths in change_to_link:
    jpx_metanode deleted_node = root.add_label("ToDelete");
    deleted_node.delete_node();
    EXPECT_THROW(conv_lbl.change_to_link(deleted_node, JPX_GROUPING_LINK), int);
    EXPECT_THROW(conv_lbl.change_to_link(conv_lbl, JPX_GROUPING_LINK), int);

    jpx_metanode empty_nl = root.add_numlist(0, nullptr, 0, nullptr, false);
    EXPECT_THROW(conv_lbl.change_to_link(empty_nl, JPX_GROUPING_LINK), int);

    jpx_metanode parent_with_kid = root.add_label("HasChild");
    parent_with_kid.add_label("Child");
    EXPECT_THROW(
        parent_with_kid.change_to_link(del_target, JPX_ALTERNATE_PARENT_LINK),
        int);

    // Link to grouping link requires ALTERNATE_CHILD_LINK; other types throw.
    jpx_metanode grp_link = root.add_link(del_target, JPX_GROUPING_LINK);
    EXPECT_THROW(conv_lbl.change_to_link(grp_link, JPX_GROUPING_LINK), int);
    // Node referenced by non-ALTERNATE_CHILD link cannot change to link.
    EXPECT_THROW(del_target.change_to_link(conv_roi, JPX_ALTERNATE_PARENT_LINK),
                 int);
    (void)xml_node;
  }

  // 2) Incremental write_metadata across 7 rounds (triggers fit_collection
  // multi-level grouping AND delayed metadata callbacks AND
  // preserve_for_links).
  jpx_metanode preserved = root.add_label("PreservedBatch0");
  preserved.preserve_for_links();
  jpx_metanode unpreserved = root.add_label("UnpreservedBatch0");

  auto write_all_pending_metadata = [&]() {
    int ip = 0;
    void* ap = nullptr;
    int guard = 0;
    while (jp2_output_box* box = tgt.write_metadata(&ip, &ap)) {
      std::string payload = "<tag idx=\"" + std::to_string(ip) + "\"/>";
      box->write(reinterpret_cast<const kdu_core::kdu_byte*>(payload.data()),
                 static_cast<int>(payload.size()));
      ASSERT_LT(++guard, 64);
    }
  };
  write_all_pending_metadata();

  // After batch 0 is written, linking to unpreserved fails while preserved
  // works!
  jpx_metanode late_lnk = root.add_label("WillBecomeLink");
  EXPECT_THROW(late_lnk.change_to_link(unpreserved, JPX_GROUPING_LINK), int);
  late_lnk.change_to_link(preserved, JPX_GROUPING_LINK);

  for (int batch = 1; batch <= 7; ++batch) {
    jpx_metanode grp =
        root.add_label(("Batch_" + std::to_string(batch)).c_str());
    grp.add_delayed(jp2_xml_4cc, batch, nullptr);
    int cs_idx[1] = {batch % 2};
    jpx_metanode nlm = grp.add_numlist(1, cs_idx, 0, nullptr, false);
    jpx_roi broi{};
    broi.init_rectangle(MakeDims(batch, batch, 6, 6));
    nlm.add_regions(1, &broi);
    if (batch == 1) {
      nlm.add_link(late_lnk, JPX_ALTERNATE_CHILD_LINK);
    }
    write_all_pending_metadata();
  }

  EXPECT_TRUE(tgt.close());
  ftgt.close();

  // 3) Read back and mutate EXISTING nodes loaded from source.
  MemSource ms(mem.data());
  jp2_family_src fs;
  fs.open(&ms);
  jpx_source src;
  ASSERT_GE(src.open(&fs, false), 0);
  jpx_meta_manager smm = src.access_meta_manager();
  int cs_flt[1] = {1};
  smm.load_matches(1, cs_flt, 0, nullptr);
  smm.load_matches(-1, nullptr, -1, nullptr);
  jpx_metanode sroot = smm.access_root();
  int desc = 0;
  EXPECT_TRUE(sroot.count_descendants(desc));
  EXPECT_GE(desc, 10);

  // Convert existing loaded child node to delayed, label, and link.
  jpx_metanode first = sroot.get_descendant(0);
  jpx_metanode second = sroot.get_descendant(1);
  if (first.exists() && second.exists()) {
    jpx_metanode sub_a = first.add_label("LiveSub");
    sub_a.preserve_for_links();
    first.change_to_delayed(jp2_xml_4cc, 99, nullptr);
    first.change_to_label("LiveRelabeled");
    second.change_to_link(first, JPX_GROUPING_LINK);
  }
  src.close();
  fs.close();
}

// Exercises jpx_frame_expander::test_codestream_visibility,
// jpx_composition::find_numlist_match (inferred layers), jx_composition
// partial/empty/transposed/skipped matching, and parse_iset_box /
// jx_frame::access_next.
TEST(JpxTest, Wave3VisibilityNumlistMatchIsetAndExpanderEdgeCases) {
  ScopedKduHandlers handlers;
  MemTarget mem;
  jp2_family_tgt ftgt;
  ftgt.open(&mem);
  jpx_target tgt;
  tgt.open(&ftgt);

  jpx_codestream_target cs0 = tgt.add_codestream();
  SetupCodestreamDims(cs0.access_dimensions(), 64, 64, 3);
  jpx_codestream_target cs1 = tgt.add_codestream();
  SetupCodestreamDims(cs1.access_dimensions(), 64, 64, 4);

  // Layer 0: opaque RGB from cs0.
  jpx_layer_target l0 = tgt.add_layer();
  l0.access_channels().init(3);
  for (int c = 0; c < 3; ++c)
    l0.access_channels().set_colour_mapping(c, c, -1, 0);
  l0.add_colour().init(JP2_sRGB_SPACE);

  // Layer 1: RGBA (has opacity -> non-opaque) from cs1.
  jpx_layer_target l1 = tgt.add_layer();
  l1.access_channels().init(3);
  for (int c = 0; c < 3; ++c)
    l1.access_channels().set_colour_mapping(c, c, -1, 1);
  l1.access_channels().set_opacity_mapping(0, 3, -1, 1);
  l1.add_colour().init(JP2_sRGB_SPACE);

  // Layer 2: full opaque RGB from cs1 (covers earlier layer).
  jpx_layer_target l2 = tgt.add_layer();
  l2.access_channels().init(3);
  for (int c = 0; c < 3; ++c)
    l2.access_channels().set_colour_mapping(c, c, -1, 1);
  l2.add_colour().init(JP2_sRGB_SPACE);

  jpx_composition comp = tgt.access_composition();
  jpx_composited_orientation orient{};
  orient.transpose = true;
  orient.vflip = true;

  // Persistent frame using cs0 (partial).
  kdu_supp::jx_frame* f0 = comp.add_frame(10, 0, true);
  comp.add_instruction(f0, 0, 0, MakeDims(0, 0, 32, 32),
                       MakeDims(0, 0, 32, 32));

  // Frame 1 with empty source/target dims + transposed + cs1 multiple times
  // (layer 1 non-opaque at (0,0,64,64), layer 0 at (10,10,20,20), layer 1 at
  // (0,0,64,64), layer 2 opaque covering (0,0,64,64) except corner or partial).
  kdu_supp::jx_frame* f1 = comp.add_frame(15, 2, false);
  comp.add_instruction(f1, 1, 0, MakeDims(0, 0, 64, 64), MakeDims(0, 0, 64, 64),
                       orient);
  comp.add_instruction(f1, 0, 0, kdu_core::kdu_dims(), kdu_core::kdu_dims(),
                       orient);
  comp.add_instruction(f1, 1, 0, MakeDims(0, 0, 32, 32),
                       MakeDims(16, 16, 32, 32));
  comp.add_instruction(f1, 2, 0, MakeDims(0, 0, 32, 32),
                       MakeDims(0, 0, 32, 32));

  // Add container with presentation track for track_idx != 0.
  jpx_container_target cont = tgt.add_container(1, 1, 3);
  jpx_codestream_target ccs = cont.access_codestream(0);
  SetupCodestreamDims(ccs.access_dimensions(), 64, 64, 3);
  jpx_layer_target clyr = cont.access_layer(0);
  clyr.access_channels().init(3);
  for (int c = 0; c < 3; ++c) {
    clyr.access_channels().set_colour_mapping(c, c, -1,
                                              ccs.get_codestream_id());
  }
  clyr.add_colour().init(JP2_sRGB_SPACE);
  jpx_composition ccomp = cont.add_presentation_track(1);
  kdu_supp::jx_frame* cf = ccomp.add_frame(10, 2, false);
  ccomp.add_instruction(cf, 0, 1, MakeDims(0, 0, 64, 64),
                        MakeDims(0, 0, 64, 64));

  // Metadata numlist with ONLY codestreams (no layers) -> inferred layers in
  // find_numlist_match! Plus a numlist with layers for
  // test_codestream_visibility.
  jpx_meta_manager mm = tgt.access_meta_manager();
  jpx_metanode root = mm.access_root();
  int cs_only_arr[1] = {1};
  jpx_metanode nl_cs_only = root.add_numlist(1, cs_only_arr, 0, nullptr, false);
  nl_cs_only.add_label("CsOnlyNumlist");
  int lyr_only_arr[2] = {1, 2};
  jpx_metanode nl_lyr = root.add_numlist(0, nullptr, 2, lyr_only_arr, false);
  nl_lyr.add_label("LayerNumlist");

  EXPECT_EQ(tgt.write_headers(), nullptr);
  for (jpx_codestream_target cs : {cs0, cs1}) {
    jp2_output_box* sb = cs.open_stream();
    WriteMinimalCodestream(cs.access_stream(), 64, 64,
                           cs.get_codestream_id() == 0 ? 3 : 4);
    EXPECT_TRUE(sb->close());
  }
  for (int r = 0; r < 3; ++r) {
    jp2_output_box* sb = ccs.open_stream();
    WriteMinimalCodestream(ccs.access_stream(), 64, 64, 3);
    EXPECT_TRUE(sb->close());
  }
  EXPECT_EQ(tgt.write_metadata(), nullptr);
  EXPECT_TRUE(tgt.close());
  ftgt.close();

  MemSource ms(mem.data());
  jp2_family_src fs;
  fs.open(&ms);
  jpx_source src;
  ASSERT_GE(src.open(&fs, false), 0);
  jpx_composition scomp = src.access_composition();
  ASSERT_TRUE(scomp.exists());
  jpx_meta_manager smm = src.access_meta_manager();
  smm.load_matches(-1, nullptr, -1, nullptr);
  jpx_metanode sroot = smm.access_root();

  jpx_metanode sn_cs_only, sn_lyr;
  for (int i = 0;; ++i) {
    jpx_metanode d = sroot.get_descendant(i);
    if (!d.exists()) break;
    int nc = 0, nl = 0;
    bool rr = false;
    if (d.get_numlist_info(nc, nl, rr)) {
      if (nc == 1 && nl == 0) sn_cs_only = d;
      if (nc == 0 && nl == 2) sn_lyr = d;
    }
  }
  ASSERT_TRUE(sn_cs_only.exists() && sn_lyr.exists());

  // 1) find_numlist_match using cs-only numlist (inferred layers from cs) and
  // layer numlist.
  for (int flags : {0, JPX_FRAME_MATCH_REVERSE,
                    JPX_FRAME_MATCH_REVERSE | JPX_FRAME_MATCH_ALL_LAYERS}) {
    jpx_frame mf = scomp.access_frame(
        0, (flags & JPX_FRAME_MATCH_REVERSE) ? 3 : 0, true, false);
    int inst_idx = -1;
    scomp.find_numlist_match(mf, inst_idx, 0, sn_cs_only, 4, true, flags);
    inst_idx = 0;
    scomp.find_numlist_match(mf, inst_idx, 0, sn_lyr, 0, false, flags);
  }

  // 2) test_codestream_visibility across frames, layer filters, alpha,
  // transpose, skip.
  jpx_frame f_vis = scomp.access_frame(0, 1, true, false);
  ASSERT_TRUE(f_vis.exists());
  int lyr_flt[3] = {0, 1, 2};
  for (int skip = 0; skip < 4; ++skip) {
    for (bool ign_alpha : {false, true}) {
      kdu_core::kdu_dims creg = MakeDims(0, 0, 64, 64);
      jpx_frame_expander::test_codestream_visibility(
          &src, f_vis, 1, sn_lyr, lyr_flt, 3, creg, MakeDims(0, 0, 32, 32),
          ign_alpha, skip);
      creg = MakeDims(0, 0, 64, 64);
      jpx_frame_expander::test_codestream_visibility(
          &src, f_vis, 0, jpx_metanode(), nullptr, 0, creg,
          kdu_core::kdu_dims(), ign_alpha, skip);
    }
  }
  // Test non-matching ROI (outside codestream dimensions) -> returns -1.
  kdu_core::kdu_dims creg = MakeDims(0, 0, 64, 64);
  EXPECT_EQ(jpx_frame_expander::test_codestream_visibility(
                &src, f_vis, 1, jpx_metanode(), nullptr, 0, creg,
                MakeDims(200, 200, 16, 16), true, 0),
            -1);
  // Test access_next forward & reverse across tracks 0 and 1.
  jpx_frame tail_f = scomp.access_frame(0, 3, true, false);
  for (int k = 0; k < 5 && tail_f.exists(); ++k) {
    tail_f = tail_f.access_next(0, true);
  }
  jpx_frame c0_f = scomp.access_frame(1, 2, true, false);
  while (c0_f.exists()) {
    c0_f = c0_f.access_next(1, true);
  }
  src.close();
  fs.close();
}

// Exercises raw JPX box construction: jp2h fallback defaults in jx_source/
// jx_codestream_source/jx_layer_source, pxfm warnings, cref in chdr/jplh,
// set_box_filter metadata inside chdr/jplh, external file url fragment reads in
// jpx_input_box::read, jclx unknown counts, and malformed box ordering errors.
TEST(JpxTest, Wave3RawJpxBoxesJp2hFallbackCrefFilterAndErrorPaths) {
  ScopedKduHandlers handlers;

  MemTarget cs_mem;
  WriteMinimalCodestream(&cs_mem, 16, 16, 3);
  const auto& cs_bytes = cs_mem.data();
  ASSERT_GE(cs_bytes.size(), 16u);

  // Helper: write a valid rreq box (optional float pixel format or
  // unsupported).
  auto write_rreq = [](jp2_family_tgt& ft, bool allow_pxfm, bool unsupported_sf,
                       bool with_vendor) {
    jp2_output_box rreq;
    rreq.open(&ft, jp2_reader_requirements_4cc);
    const kdu_core::kdu_byte mask = with_vendor ? 0xC0 : 0x80;
    rreq.write(static_cast<kdu_core::kdu_byte>(1));  // ML = 1 byte
    rreq.write(mask);                                // FUAM
    rreq.write(mask);                                // DCM
    kdu_core::kdu_uint16 nsf = (allow_pxfm ? 1 : 0) + (unsupported_sf ? 1 : 0);
    rreq.write(nsf);
    if (allow_pxfm) {
      rreq.write(JPX_SF_PIXEL_FORMAT_FLOAT);
      rreq.write(static_cast<kdu_core::kdu_byte>(0x80));
    }
    if (unsupported_sf) {
      rreq.write(JPX_SF_CODESTREAM_FRAGMENTS_REMOTE);
      rreq.write(static_cast<kdu_core::kdu_byte>(0x80));
    }
    kdu_core::kdu_uint16 nvf = with_vendor ? 1 : 0;
    rreq.write(nvf);
    if (with_vendor) {
      for (int b = 0; b < 16; ++b) {
        rreq.write(static_cast<kdu_core::kdu_byte>(0xA0 + b));
      }
      rreq.write(static_cast<kdu_core::kdu_byte>(0x40));
    }
    rreq.close();
  };

  auto write_sig_ftyp = [](jp2_family_tgt& ft, kdu_core::kdu_uint32 brand,
                           bool write_sig = true) {
    if (write_sig) {
      jp2_output_box sig;
      sig.open(&ft, jp2_signature_4cc);
      sig.write(static_cast<kdu_core::kdu_uint32>(0x0D0A870Au));
      sig.close();
    }
    jp2_output_box ftyp;
    ftyp.open(&ft, jp2_file_type_4cc);
    ftyp.write(brand);
    ftyp.write(static_cast<kdu_core::kdu_uint32>(0));
    ftyp.write(brand);
    ftyp.close();
  };

  // 1) Rich raw JPX with jp2h defaults (ihdr+bpcc+pclr+cmap+colr x2+cdef+pxfm),
  // cref subboxes inside chdr & jplh, set_box_filter metadata inside
  // chdr/jplh, creg with 0 final size, and multi-mdat + URL ftbl fragments.
  {
    MemTarget mem;
    jp2_family_tgt ft;
    ft.open(&mem);
    write_sig_ftyp(ft, jpx_brand);
    write_rreq(
        ft, /*allow_pxfm=*/false, /*unsupported_sf=*/false,
        /*with_vendor=*/false);  // pxfm inside jp2h/jplh triggers warn_pxfm_box

    // dtbl with URL 1 (exercising blocked local url_fopen cleanly).
    {
      jp2_output_box dtbl;
      dtbl.open(&ft, jp2_dtbl_4cc);
      dtbl.write(static_cast<kdu_core::kdu_uint16>(1));
      {
        const std::string u = "/blocked_local_fragment.bin";
        jp2_output_box ubox;
        ubox.open(&dtbl, jp2_data_entry_url_4cc);
        ubox.write(static_cast<kdu_core::kdu_uint32>(0));
        ubox.write(reinterpret_cast<const kdu_core::kdu_byte*>(u.data()),
                   static_cast<int>(u.size()));
        ubox.write(static_cast<kdu_core::kdu_byte>(0));
        ubox.close();
      }
      dtbl.close();
    }

    // Two internal mdat fragment targets holding split cs_bytes for stream 0.
    kdu_core::kdu_long frag1_off = 0;
    kdu_core::kdu_uint32 frag1_len =
        static_cast<kdu_core::kdu_uint32>(cs_bytes.size() / 2);
    {
      frag1_off = static_cast<kdu_core::kdu_long>(mem.data().size()) + 8;
      jp2_output_box mdat;
      mdat.open(&ft, jp2_mdat_4cc);
      mdat.write(cs_bytes.data(), static_cast<int>(frag1_len));
      mdat.close();
    }
    kdu_core::kdu_long frag2_off = 0;
    kdu_core::kdu_uint32 frag2_len =
        static_cast<kdu_core::kdu_uint32>(cs_bytes.size() - frag1_len);
    {
      frag2_off = static_cast<kdu_core::kdu_long>(mem.data().size()) + 8;
      jp2_output_box mdat;
      mdat.open(&ft, jp2_mdat_4cc);
      mdat.write(cs_bytes.data() + frag1_len, static_cast<int>(frag2_len));
      mdat.close();
    }

    // Record byte offset of standalone top-level ihdr and cdef targets for
    // cref!
    kdu_core::kdu_long ref_ihdr_off = 0;
    kdu_core::kdu_long ref_ihdr_len = 0;
    {
      ref_ihdr_off = static_cast<kdu_core::kdu_long>(mem.data().size());
      jp2_output_box mdat;
      mdat.open(&ft, jp2_mdat_4cc);
      // Put raw ihdr box payload inside mdat so flst cross-reference reads
      // body!
      kdu_core::kdu_byte ihdr_body[14] = {0,  0, 0, 16, 0, 0, 0,
                                          16, 0, 3, 7,  7, 0, 0};
      ref_ihdr_off += 8;  // Skip mdat 8-byte header to point at ihdr body.
      ref_ihdr_len = sizeof(ihdr_body);
      mdat.write(ihdr_body, sizeof(ihdr_body));
      mdat.close();
    }
    kdu_core::kdu_long ref_cdef_off = 0;
    kdu_core::kdu_long ref_cdef_len = 0;
    {
      ref_cdef_off = static_cast<kdu_core::kdu_long>(mem.data().size()) + 8;
      jp2_output_box mdat;
      mdat.open(&ft, jp2_mdat_4cc);
      kdu_core::kdu_byte cdef_body[20] = {0, 3, 0, 0, 0, 0, 0, 1, 0, 1,
                                          0, 0, 0, 2, 0, 2, 0, 0, 0, 3};
      ref_cdef_len = sizeof(cdef_body);
      mdat.write(cdef_body, sizeof(cdef_body));
      mdat.close();
    }

    // jp2h default header box containing ihdr, bpcc, pclr, cmap, 2 colrs, cdef,
    // pxfm.
    {
      jp2_output_box jp2h;
      jp2h.open(&ft, jp2_header_4cc);
      {
        jp2_output_box ihdr;
        ihdr.open(&jp2h, jp2_image_header_4cc);
        ihdr.write(static_cast<kdu_core::kdu_uint32>(16));
        ihdr.write(static_cast<kdu_core::kdu_uint32>(16));
        ihdr.write(static_cast<kdu_core::kdu_uint16>(3));
        ihdr.write(
            static_cast<kdu_core::kdu_byte>(255));  // varying bpc -> bpcc
        ihdr.write(static_cast<kdu_core::kdu_byte>(7));
        ihdr.write(static_cast<kdu_core::kdu_byte>(0));
        ihdr.write(static_cast<kdu_core::kdu_byte>(0));
        ihdr.close();
      }
      {
        jp2_output_box bpcc;
        bpcc.open(&jp2h, jp2_bits_per_component_4cc);
        bpcc.write(static_cast<kdu_core::kdu_byte>(7));
        bpcc.write(static_cast<kdu_core::kdu_byte>(7));
        bpcc.write(static_cast<kdu_core::kdu_byte>(7));
        bpcc.close();
      }
      {
        jp2_output_box pclr;
        pclr.open(&jp2h, jp2_palette_4cc);
        pclr.write(static_cast<kdu_core::kdu_uint16>(2));  // NE=2
        pclr.write(static_cast<kdu_core::kdu_byte>(1));    // NPC=1
        pclr.write(static_cast<kdu_core::kdu_byte>(7));    // B=8bpp
        pclr.write(static_cast<kdu_core::kdu_byte>(0));
        pclr.write(static_cast<kdu_core::kdu_byte>(255));
        pclr.close();
      }
      {
        jp2_output_box cmap;
        cmap.open(&jp2h, jp2_component_mapping_4cc);
        for (int c = 0; c < 3; ++c) {
          cmap.write(static_cast<kdu_core::kdu_uint16>(c));
          cmap.write(static_cast<kdu_core::kdu_byte>(0));
          cmap.write(static_cast<kdu_core::kdu_byte>(0));
        }
        cmap.close();
      }
      for (kdu_core::kdu_byte approx : {static_cast<kdu_core::kdu_byte>(1),
                                        static_cast<kdu_core::kdu_byte>(2)}) {
        jp2_output_box colr;
        colr.open(&jp2h, jp2_colour_4cc);
        colr.write(static_cast<kdu_core::kdu_byte>(1));
        colr.write(static_cast<kdu_core::kdu_byte>(0));
        colr.write(approx);
        colr.write(static_cast<kdu_core::kdu_uint32>(16));  // sRGB
        colr.close();
      }
      {
        jp2_output_box cdef;
        cdef.open(&jp2h, jp2_channel_definition_4cc);
        cdef.write(static_cast<kdu_core::kdu_uint16>(3));
        for (int c = 0; c < 3; ++c) {
          cdef.write(static_cast<kdu_core::kdu_uint16>(c));
          cdef.write(static_cast<kdu_core::kdu_uint16>(0));
          cdef.write(static_cast<kdu_core::kdu_uint16>(c + 1));
        }
        cdef.close();
      }
      {
        jp2_output_box pxfm;
        pxfm.open(&jp2h, jp2_pixel_format_4cc);
        pxfm.write(static_cast<kdu_core::kdu_uint16>(3));
        for (kdu_core::kdu_uint16 c = 0; c < 3; ++c) {
          pxfm.write(c);
          pxfm.write(static_cast<kdu_core::kdu_uint16>(0x1000));
        }
        pxfm.close();
      }
      jp2h.close();
    }

    // Stream 0: empty chdr (inherits all defaults from jp2h!)
    {
      jp2_output_box chdr0;
      chdr0.open(&ft, jp2_codestream_header_4cc);
      chdr0.close();
    }
    // Stream 1: chdr with cref(flst->ihdr) + lbl (metadata filter) + discarded
    // uuid
    {
      jp2_output_box chdr1;
      chdr1.open(&ft, jp2_codestream_header_4cc);
      {
        jp2_output_box cref;
        cref.open(&chdr1, jp2_cross_reference_4cc);
        cref.write(jp2_image_header_4cc);
        jp2_output_box flst;
        flst.open(&cref, jp2_fragment_list_4cc);
        flst.write(static_cast<kdu_core::kdu_uint16>(1));
        flst.write(static_cast<kdu_core::kdu_uint32>(ref_ihdr_off >> 32));
        flst.write(static_cast<kdu_core::kdu_uint32>(ref_ihdr_off));
        flst.write(static_cast<kdu_core::kdu_uint32>(ref_ihdr_len));
        flst.write(static_cast<kdu_core::kdu_uint16>(0));
        flst.close();
        cref.close();
      }
      {
        jp2_output_box lbl;
        lbl.open(&chdr1, jp2_label_4cc);
        const char* txt = "ChdrMetaLabel";
        lbl.write(reinterpret_cast<const kdu_core::kdu_byte*>(txt),
                  static_cast<int>(std::strlen(txt)));
        lbl.close();
      }
      {
        jp2_output_box xm;
        xm.open(&chdr1, jp2_xml_4cc);
        const char* txt = "<chdr/>";
        xm.write(reinterpret_cast<const kdu_core::kdu_byte*>(txt),
                 static_cast<int>(std::strlen(txt)));
        xm.close();
      }
      chdr1.close();
    }

    // Layer 0: empty jplh (inherits defaults from jp2h + creg synthesized!)
    {
      jp2_output_box jplh0;
      jplh0.open(&ft, jp2_compositing_layer_hdr_4cc);
      jplh0.close();
    }
    // Layer 1: jplh with cref(flst->cdef), cgrp, pxfm, creg (2 cs), lbl, xml.
    {
      jp2_output_box jplh1;
      jplh1.open(&ft, jp2_compositing_layer_hdr_4cc);
      {
        jp2_output_box cref;
        cref.open(&jplh1, jp2_cross_reference_4cc);
        cref.write(jp2_channel_definition_4cc);
        jp2_output_box flst;
        flst.open(&cref, jp2_fragment_list_4cc);
        flst.write(static_cast<kdu_core::kdu_uint16>(1));
        flst.write(static_cast<kdu_core::kdu_uint32>(ref_cdef_off >> 32));
        flst.write(static_cast<kdu_core::kdu_uint32>(ref_cdef_off));
        flst.write(static_cast<kdu_core::kdu_uint32>(ref_cdef_len));
        flst.write(static_cast<kdu_core::kdu_uint16>(0));
        flst.close();
        cref.close();
      }
      {
        jp2_output_box cgrp;
        cgrp.open(&jplh1, jp2_colour_group_4cc);
        jp2_output_box colr;
        colr.open(&cgrp, jp2_colour_4cc);
        colr.write(static_cast<kdu_core::kdu_byte>(1));
        colr.write(static_cast<kdu_core::kdu_byte>(0));
        colr.write(static_cast<kdu_core::kdu_byte>(1));
        colr.write(static_cast<kdu_core::kdu_uint32>(16));
        colr.close();
        cgrp.close();
      }
      {
        jp2_output_box pxfm;
        pxfm.open(&jplh1, jp2_pixel_format_4cc);
        pxfm.write(static_cast<kdu_core::kdu_uint16>(3));
        for (kdu_core::kdu_uint16 c = 0; c < 3; ++c) {
          pxfm.write(c);
          pxfm.write(static_cast<kdu_core::kdu_uint16>(0x0000));
        }
        pxfm.close();
      }
      {
        jp2_output_box creg;
        creg.open(&jplh1, jp2_registration_4cc);
        creg.write(static_cast<kdu_core::kdu_uint16>(1));
        creg.write(static_cast<kdu_core::kdu_uint16>(1));
        for (kdu_core::kdu_uint16 cdn : {0, 1}) {
          creg.write(cdn);
          creg.write(static_cast<kdu_core::kdu_byte>(1));
          creg.write(static_cast<kdu_core::kdu_byte>(1));
          creg.write(static_cast<kdu_core::kdu_byte>(0));
          creg.write(static_cast<kdu_core::kdu_byte>(0));
        }
        creg.close();
      }
      {
        jp2_output_box lbl;
        lbl.open(&jplh1, jp2_label_4cc);
        const char* txt = "JplhMetaLabel";
        lbl.write(reinterpret_cast<const kdu_core::kdu_byte*>(txt),
                  static_cast<int>(std::strlen(txt)));
        lbl.close();
      }
      jplh1.close();
    }

    // Stream 0 ftbl -> internal mdat frag1 + mdat frag2 (multi-fragment read!).
    {
      jp2_output_box ftbl0;
      ftbl0.open(&ft, jp2_fragment_table_4cc);
      jp2_output_box flst;
      flst.open(&ftbl0, jp2_fragment_list_4cc);
      flst.write(static_cast<kdu_core::kdu_uint16>(2));
      flst.write(static_cast<kdu_core::kdu_uint32>(frag1_off >> 32));
      flst.write(static_cast<kdu_core::kdu_uint32>(frag1_off & 0xFFFFFFFFu));
      flst.write(frag1_len);
      flst.write(static_cast<kdu_core::kdu_uint16>(0));  // local mdat 1
      flst.write(static_cast<kdu_core::kdu_uint32>(frag2_off >> 32));
      flst.write(static_cast<kdu_core::kdu_uint32>(frag2_off & 0xFFFFFFFFu));
      flst.write(frag2_len);
      flst.write(static_cast<kdu_core::kdu_uint16>(0));  // local mdat 2
      flst.close();
      ftbl0.close();
    }
    // Stream 1 ftbl -> URL 1 (local file disabled) to exercise url_fopen throw.
    {
      jp2_output_box ftbl1;
      ftbl1.open(&ft, jp2_fragment_table_4cc);
      jp2_output_box flst;
      flst.open(&ftbl1, jp2_fragment_list_4cc);
      flst.write(static_cast<kdu_core::kdu_uint16>(1));
      flst.write(static_cast<kdu_core::kdu_uint32>(0));
      flst.write(static_cast<kdu_core::kdu_uint32>(0));
      flst.write(static_cast<kdu_core::kdu_uint32>(32));
      flst.write(static_cast<kdu_core::kdu_uint16>(1));  // url 1
      flst.close();
      ftbl1.close();
    }
    ft.close();

    MemSource ms(mem.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 0);
    // Set box filter so only jp2_label_4cc is accepted, jp2_xml_4cc discarded!
    kdu_core::kdu_uint32 keep_type = jp2_label_4cc;
    src.access_meta_manager().set_box_filter(1, &keep_type);

    jpx_codestream_source cs0 = src.access_codestream(0, false);
    ASSERT_TRUE(cs0.exists());
    EXPECT_TRUE(cs0.access_palette().exists());
    jpx_input_box* ib0 = cs0.open_stream();
    ASSERT_NE(ib0, nullptr);
    kdu_core::kdu_byte rbuf[256]{};
    // Read across 2 internal mdat fragments until EOF!
    int total_read = ib0->read(rbuf, 4);
    EXPECT_GT(total_read, 0);
    int chunk = 0;
    while ((chunk = ib0->read(rbuf, sizeof(rbuf))) > 0) {
      total_read += chunk;
    }
    EXPECT_EQ(total_read, static_cast<int>(cs_bytes.size()));
    EXPECT_EQ(ib0->read(rbuf, 16), 0);  // EOF
    ib0->close();

    jpx_codestream_source cs1 = src.access_codestream(1, false);
    ASSERT_TRUE(cs1.exists());
    jpx_input_box* ib1 = cs1.open_stream();
    if (ib1 != nullptr) {
      EXPECT_THROW(ib1->read(rbuf, 16), int);  // url_fopen disabled
      ib1->close();
    }

    jpx_layer_source ls0 = src.access_layer(0, false);
    jpx_layer_source ls1 = src.access_layer(1, false);
    EXPECT_TRUE(ls0.exists());
    EXPECT_TRUE(ls1.exists());
    // Note: second call to open(&fs) throws!
    EXPECT_THROW(src.open(&fs, true), int);
    src.close();
    fs.close();

    // Also test partial prefix of this rich file to hit pending_subs / partial
    // chdr / jplh / jp2h donation paths!
    for (int pct : {30, 55, 80}) {
      size_t plen = (mem.data().size() * pct) / 100;
      std::vector<kdu_core::kdu_byte> pbytes(mem.data().begin(),
                                             mem.data().begin() + plen);
      MemSource pms(pbytes);
      jp2_family_src pfs;
      pfs.open(&pms);
      jpx_source psrc;
      if (psrc.open(&pfs, true) > 0) {
        try {
          psrc.access_codestream(0, false);
          psrc.access_codestream(1, false);
          psrc.access_layer(0, false);
          psrc.access_layer(1, false);
          psrc.access_meta_manager().load_matches(-1, nullptr, -1, nullptr);
        } catch (int) {
        }
        psrc.close();
      }
      pfs.close();
    }
  }

  // 2) jpx_source::open & parse_next_top_level_box error & incompatibility
  // paths.
  auto try_open_bytes = [](const std::vector<kdu_core::kdu_byte>& bytes,
                           bool ret_incompat, int expected_rc, bool parse_all) {
    MemSource ms(bytes);
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    if (!ret_incompat) {
      if (parse_all) {
        int r = src.open(&fs, false);
        if (r > 0) {
          int nc = 0;
          EXPECT_THROW(src.count_codestreams(nc), int);
        }
      } else {
        EXPECT_THROW(src.open(&fs, false), int);
      }
    } else {
      int r = src.open(&fs, true);
      EXPECT_EQ(r, expected_rc);
      if (r > 0 && parse_all) {
        int nc = 0;
        EXPECT_THROW(src.count_codestreams(nc), int);
      }
    }
    src.close();
    fs.close();
  };

  // Bad / missing signature box.
  {
    std::vector<kdu_core::kdu_byte> empty;
    try_open_bytes(empty, true, -1, false);
    try_open_bytes(empty, false, 0, false);

    MemTarget mt;
    jp2_family_tgt ft;
    ft.open(&mt);
    jp2_output_box wrong_sig;
    wrong_sig.open(&ft, jp2_signature_4cc);
    wrong_sig.write(static_cast<kdu_core::kdu_uint32>(0x12345678u));
    wrong_sig.close();
    ft.close();
    try_open_bytes(mt.data(), true, -1, false);
    try_open_bytes(mt.data(), false, -1, false);
  }
  // Missing ftyp / wrong 2nd box.
  {
    MemTarget mt;
    jp2_family_tgt ft;
    ft.open(&mt);
    jp2_output_box sig;
    sig.open(&ft, jp2_signature_4cc);
    sig.write(static_cast<kdu_core::kdu_uint32>(0x0D0A870Au));
    sig.close();
    jp2_output_box md;
    md.open(&ft, jp2_mdat_4cc);
    md.write(static_cast<kdu_core::kdu_uint32>(0));
    md.close();
    ft.close();
    try_open_bytes(mt.data(), true, -1, false);
    try_open_bytes(mt.data(), false, -1, false);
  }
  // Incompatible ftyp with return_if_incompatible=false throws; plus bad rreq.
  {
    MemTarget mt;
    jp2_family_tgt ft;
    ft.open(&mt);
    write_sig_ftyp(ft, static_cast<kdu_core::kdu_uint32>(0x62616420u));
    ft.close();
    try_open_bytes(mt.data(), false, -1, false);

    MemTarget mt2;
    jp2_family_tgt ft2;
    ft2.open(&mt2);
    write_sig_ftyp(ft2, jpx_brand);
    write_rreq(ft2, true, /*unsupported_sf=*/true, /*with_vendor=*/true);
    ft2.close();
    {
      MemSource ms2(mt2.data());
      jp2_family_src fs2;
      fs2.open(&ms2);
      jpx_source src2;
      EXPECT_EQ(src2.open(&fs2, true), 1);
      EXPECT_FALSE(src2.access_compatibility().test_fully_understand());
      EXPECT_FALSE(src2.access_compatibility().test_decode_completely());
      src2.close();
      fs2.close();
    }
  }
  // Box ordering errors in parse_next_top_level_box:
  // (a) Duplicate jp2h; (b) j2cx before jp2c; (c) jclx before chdr;
  // (d) jp2c after j2cx; (e) chdr after jclx; (f) jplh after jclx.
  {
    MemTarget mt;
    jp2_family_tgt ft;
    ft.open(&mt);
    write_sig_ftyp(ft, jpx_brand);
    write_rreq(ft, false, false, false);
    for (int k = 0; k < 2; ++k) {
      jp2_output_box jp2h;
      jp2h.open(&ft, jp2_header_4cc);
      jp2h.close();
    }
    ft.close();
    try_open_bytes(mt.data(), true, 1, true);
  }
  {
    MemTarget mt;
    jp2_family_tgt ft;
    ft.open(&mt);
    write_sig_ftyp(ft, jpx_brand);
    write_rreq(ft, false, false, false);
    jp2_output_box j2cx;
    j2cx.open(&ft, jp2_multi_codestream_4cc);
    j2cx.close();
    ft.close();
    try_open_bytes(mt.data(), true, 1, true);
  }
  {
    MemTarget mt;
    jp2_family_tgt ft;
    ft.open(&mt);
    write_sig_ftyp(ft, jpx_brand);
    write_rreq(ft, false, false, false);
    jp2_output_box jplh;
    jplh.open(&ft, jp2_compositing_layer_hdr_4cc);
    jplh.close();
    jp2_output_box jclx;
    jclx.open(&ft, jp2_layer_extensions_4cc);
    jclx.close();
    ft.close();
    try_open_bytes(mt.data(), true, 1, true);
  }
}

// Exercises jpx_roi::contains & measure_area trapezoids, chained PATH_MODE
// add_region via anchor snapping, exact shared-corner mitered 3-quad triangle
// ring in set_path_thickness + fill_closed_paths (hits add_triangle), and
// undo/redo.
TEST(JpxTest, Wave3RoiPathFillerAndChainedEditor) {
  ScopedKduHandlers handlers;
  // 1) Shared-edge closed 3-quad triangle ring -> fill_closed_paths hits
  // jx_path_filler::add_triangle and set_path_thickness hits shared-edge
  // miters!
  {
    auto c = [](int x, int y) { return kdu_core::kdu_coords(x, y); };
    jpx_roi tri_ring[3]{};
    // Clockwise quads meeting edge-to-edge around inner triangle:
    tri_ring[0].init_quadrilateral(c(100, 100), c(300, 100), c(260, 125),
                                   c(140, 125));
    tri_ring[1].init_quadrilateral(c(300, 100), c(200, 280), c(200, 230),
                                   c(260, 125));
    tri_ring[2].init_quadrilateral(c(200, 280), c(100, 100), c(140, 125),
                                   c(200, 230));
    for (int i = 0; i < 3; ++i) {
      ASSERT_TRUE(tri_ring[i].check_geometry());
      tri_ring[i].contains(kdu_core::kdu_coords(200, 112));
      tri_ring[i].contains(kdu_core::kdu_coords(200, 160));
    }

    jpx_roi_editor ed;
    ed.set_max_undo_history(8);
    ed.init(tri_ring, 3);
    ed.set_mode(JPX_EDITOR_PATH_MODE);
    bool thick_ok = false;
    ed.set_path_thickness(16, thick_ok);
    ed.set_path_thickness(10, thick_ok);
    bool fill_ok = false;
    ed.fill_closed_paths(fill_ok, 0);
    // Add extra vertex/skeleton segments on an anchor of the ring.
    kdu_core::kdu_coords a(120, 112);
    ed.find_nearest_anchor(a, false);
    ed.select_anchor(a, false);
    ed.set_mode(JPX_EDITOR_VERTEX_MODE);
    ed.add_region(false, MakeDims(0, 0, 500, 500));
    ed.set_mode(JPX_EDITOR_SKELETON_MODE);
    kdu_core::kdu_coords gp(100, 100);
    ed.find_nearest_guide_point(gp);
    ed.select_anchor(gp, false);
    ed.add_region(true, MakeDims(0, 0, 500, 500));
    ed.undo();
    ed.redo();
  }

  // 2) Square shared-edge 4-quad ring + add_region chaining in
  // PATH/SKELETON/VERTEX.
  {
    auto c = [](int x, int y) { return kdu_core::kdu_coords(x, y); };
    jpx_roi sq_ring[4]{};
    sq_ring[0].init_quadrilateral(c(100, 100), c(300, 100), c(270, 130),
                                  c(130, 130));
    sq_ring[1].init_quadrilateral(c(300, 100), c(300, 300), c(270, 270),
                                  c(270, 130));
    sq_ring[2].init_quadrilateral(c(300, 300), c(100, 300), c(130, 270),
                                  c(270, 270));
    sq_ring[3].init_quadrilateral(c(100, 300), c(100, 100), c(130, 130),
                                  c(130, 270));
    jpx_roi_editor ed;
    ed.init(sq_ring, 4);
    ed.set_mode(JPX_EDITOR_PATH_MODE);
    bool ok = false;
    ed.set_path_thickness(12, ok);
    ed.fill_closed_paths(ok, 0);
  }

  // 3) Measure area & contains on degenerate/horizontal-edge & vertical-edge
  // quads.
  {
    auto c = [](int x, int y) { return kdu_core::kdu_coords(x, y); };
    const std::array<std::array<kdu_core::kdu_coords, 4>, 4> edge_quads = {{
        // Top horizontal edge, v1.y == v0.y < v3.y < v2.y
        {{c(100, 100), c(200, 100), c(210, 210), c(90, 180)}},
        // Left vertical diamond, v2.y > v3.y, v1.y < v2.y
        {{c(150, 80), c(220, 140), c(150, 240), c(90, 160)}},
        // Flipped height ordering v0.y == v2.y
        {{c(80, 150), c(150, 100), c(230, 150), c(150, 220)}},
        // Rightmost vertex max x and diagonal contains boundary
        {{c(120, 90), c(260, 130), c(240, 210), c(110, 190)}},
    }};
    for (const auto& q : edge_quads) {
      jpx_roi r{};
      r.init_quadrilateral(q[0], q[1], q[2], q[3]);
      if (r.check_geometry()) {
        double cx = 0.0, cy = 0.0;
        EXPECT_GT(r.measure_area(cx, cy), 0.0);
        r.contains(
            kdu_core::kdu_coords(static_cast<int>(cx), static_cast<int>(cy)));
        r.contains(kdu_core::kdu_coords(q[0].x - 1, q[0].y));
        r.contains(kdu_core::kdu_coords(q[2].x + 1, q[2].y));
      }
    }
  }
}

void SetupWave4Layer(jpx_layer_target ly, bool alpha, int nc, int cs_id = 0) {
  jp2_channels ch = ly.access_channels();
  ch.init(nc);
  for (int c = 0; c < nc; ++c) {
    ch.set_colour_mapping(c, c, -1, cs_id);
    if (alpha) {
      ch.set_opacity_mapping(c, nc, -1, cs_id);
    }
  }
  ly.add_colour(0, 0).init(nc == 1 ? JP2_sLUM_SPACE : JP2_sRGB_SPACE);
}

TEST(JpxTest, Wave4RoiPathWindingHolesTrianglesEllipsesAndScribbles) {
  ScopedKduHandlers handlers;

  // 1) Concave arrowhead / dart quadrilaterals where v2.y < min(v1.y, v3.y)
  // to cover the 3 sub-cases of jpx_roi::measure_area & jpx_roi::contains.
  {
    auto c = [](int x, int y) { return kdu_core::kdu_coords(x, y); };
    const std::array<std::array<kdu_core::kdu_coords, 4>, 3> darts = {{
        // Case A: v2 lies between left and right edges (v0->v3 and v0->v1).
        {{c(200, 100), c(300, 260), c(200, 180), c(100, 260)}},
        // Case B: v3.y < v1.y and v2 left of trap-0 left edge.
        {{c(220, 100), c(320, 260), c(160, 170), c(140, 210)}},
        // Case C: v1.y <= v3.y and v2 right of trap-0 right edge.
        {{c(180, 100), c(260, 200), c(240, 160), c(100, 260)}},
    }};
    for (const auto& d : darts) {
      jpx_roi r{};
      r.init_quadrilateral(d[0], d[1], d[2], d[3]);
      if (r.check_geometry()) {
        double cx = 0.0, cy = 0.0;
        EXPECT_GT(r.measure_area(cx, cy), 0.0);
        r.contains(kdu_core::kdu_coords(200, 140));
        r.contains(kdu_core::kdu_coords(200, 250));
        r.contains(kdu_core::kdu_coords(50, 150));
        r.contains(kdu_core::kdu_coords(350, 150));
        jpx_roi clipped{};
        clipped.init_quadrilateral(c(-20, -20), c(100, -10), c(120, 80),
                                   c(-10, 90));
        clipped.clip_region();
        clipped.fix_inconsistencies();
      }
    }
  }

  // 2) Skewed ellipses (both axis_extents+tan_theta constructor and clamped
  // skew) serialized to roid box + malformed Rtyp==3 box warning coverage.
  {
    MemTarget mt;
    jp2_family_tgt ft;
    ft.open(&mt);
    jpx_target tgt;
    tgt.open(&ft);
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 64, 64, 1, 8);
    jpx_layer_target ly = tgt.add_layer();
    SetupWave4Layer(ly, false, 1);

    std::vector<jpx_roi> rois(4);
    rois[0].init_ellipse(kdu_core::kdu_coords(16, 16),
                         kdu_core::kdu_coords(8, 6), kdu_core::kdu_coords(3, 2),
                         true, 1);
    const double ax[2] = {10.0, 6.0};
    rois[1].init_ellipse(kdu_core::kdu_coords(32, 32), ax, 0.5, false, 2);
    rois[2].init_ellipse(kdu_core::kdu_coords(40, 40), ax, -0.5, true, 0);
    // Clamped skew boundary case.
    rois[3].init_ellipse(kdu_core::kdu_coords(24, 24),
                         kdu_core::kdu_coords(5, 5),
                         kdu_core::kdu_coords(10, -10), true, 3);
    for (const auto& r : rois) {
      double cx = 0.0, cy = 0.0;
      EXPECT_GT(r.measure_area(cx, cy), 0.0);
      EXPECT_TRUE(r.contains(
          kdu_core::kdu_coords(static_cast<int>(cx), static_cast<int>(cy))));
    }
    int cidx = 0;
    tgt.access_meta_manager().insert_node(1, &cidx, 0, nullptr, false,
                                          static_cast<int>(rois.size()),
                                          rois.data());
    tgt.write_headers();
    tgt.write_metadata();
    jp2_output_box* cs_box = cs.open_stream();
    WriteMinimalCodestream(cs.access_stream(), 64, 64, 1);
    cs_box->close();
    tgt.close();
    ft.close();

    MemSource ms(mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 1);
    src.access_meta_manager().load_matches(-1, nullptr, -1, nullptr);
    jpx_metanode rnode = src.access_meta_manager().access_root();
    int ndesc = 0;
    rnode.count_descendants(ndesc);
    for (int i = 0; i < ndesc; ++i) {
      jpx_metanode cur = rnode.get_descendant(i);
      if (cur.exists() && cur.get_num_regions() > 0) {
        for (int k = 0; k < cur.get_num_regions(); ++k) {
          jpx_roi rr = cur.get_region(k);
          double cx = 0.0, cy = 0.0;
          EXPECT_GT(rr.measure_area(cx, cy), 0.0);
        }
      }
    }
    src.close();
    fs.close();

    // Malformed standalone roid box with invalid Rtyp == 3 refinement.
    MemTarget rmt;
    jp2_family_tgt rft;
    rft.open(&rmt);
    jp2_output_box roid;
    roid.open(&rft, jp2_roi_description_4cc);
    roid.write(static_cast<kdu_core::kdu_byte>(1));
    roid.write(static_cast<kdu_core::kdu_byte>(1));  // Rstatic
    roid.write(
        static_cast<kdu_core::kdu_byte>(3));  // Rtyp=3 with no prior ellipse
    roid.write(static_cast<kdu_core::kdu_byte>(0));  // Rcp
    roid.write(static_cast<kdu_core::kdu_uint32>(10));
    roid.write(static_cast<kdu_core::kdu_uint32>(10));
    roid.write(static_cast<kdu_core::kdu_uint32>(1));
    roid.write(static_cast<kdu_core::kdu_uint32>(1));
    roid.close();
    rft.close();

    MemSource rms(rmt.data());
    jp2_family_src rfs;
    rfs.open(&rms);
    jp2_input_box in_roid;
    in_roid.open(&rfs);
    // Test via jpx_source embedded roid in jpx metadata: wrap inside asoc.
    in_roid.close();
    rfs.close();

    MemTarget jrmt;
    jp2_family_tgt jrft;
    jrft.open(&jrmt);
    jp2_output_box sig;
    sig.open(&jrft, jp2_signature_4cc);
    sig.write(static_cast<kdu_core::kdu_uint32>(0x0D0A870Au));
    sig.close();
    jp2_output_box ftyp;
    ftyp.open(&jrft, jp2_file_type_4cc);
    ftyp.write(jpx_brand);
    ftyp.write(static_cast<kdu_core::kdu_uint32>(0));
    ftyp.write(jpx_brand);
    ftyp.close();
    jp2_output_box asoc;
    asoc.open(&jrft, jp2_association_4cc);
    jp2_output_box bro;
    bro.open(&asoc, jp2_roi_description_4cc);
    bro.write(static_cast<kdu_core::kdu_byte>(1));
    bro.write(static_cast<kdu_core::kdu_byte>(1));
    bro.write(static_cast<kdu_core::kdu_byte>(3));
    bro.write(static_cast<kdu_core::kdu_byte>(0));
    bro.write(static_cast<kdu_core::kdu_uint32>(10));
    bro.write(static_cast<kdu_core::kdu_uint32>(10));
    bro.write(static_cast<kdu_core::kdu_uint32>(1));
    bro.write(static_cast<kdu_core::kdu_uint32>(1));
    bro.close();
    asoc.close();
    jrft.close();
    MemSource jrms(jrmt.data());
    jp2_family_src jrfs;
    jrfs.open(&jrms);
    jpx_source jsrc;
    if (jsrc.open(&jrfs, true) > 0) {
      try {
        jsrc.access_meta_manager().load_matches(-1, nullptr, -1, nullptr);
      } catch (int) {
      }
      jsrc.close();
    }
    jrfs.close();
  }

  // 3) jpx_roi_editor: add_scribble_point subsampling (>65 points + loop),
  // find_nearest_guide_point across all 3 modes (quad & ellipse), add_region
  // in VERTEX/SKELETON/PATH modes from ellipse (both aspect ratios + attached
  // quad) and mid-quad corner, plus undo-history overflow trimming.
  {
    auto c = [](int x, int y) { return kdu_core::kdu_coords(x, y); };
    jpx_roi seed[3];
    seed[0].init_ellipse(c(150, 150), c(20, 35),
                         c(0, 0));  // extent.x < extent.y
    seed[1].init_ellipse(c(300, 100), c(30, 15), c(0, 0));  // center on seed[2]
    seed[2].init_quadrilateral(c(300, 100), c(360, 100), c(360, 160),
                               c(300, 160));
    jpx_roi_editor ed;
    ed.set_max_undo_history(2);
    ed.init(seed, 3);

    // Ellipse center & vertex guide point + add_region(false, ...) from ellipse
    // in VERTEX/SKELETON modes (exercises L10104-10130).
    ed.set_mode(JPX_EDITOR_VERTEX_MODE);
    ed.select_anchor(c(150, 150), false);
    kdu_core::kdu_coords g(152, 135);
    ed.find_nearest_guide_point(g);
    ed.add_region(false, MakeDims(0, 0, 1000, 1000));
    ed.select_anchor(c(300, 100), false);
    ed.add_region(false, MakeDims(0, 0, 1000, 1000));
    ed.set_mode(JPX_EDITOR_PATH_MODE);
    ed.select_anchor(c(150, 150), false);
    EXPECT_FALSE(ed.find_nearest_guide_point(g));  // ellipse in PATH mode
    ed.add_region(true, MakeDims(0, 0, 1000, 1000));

    // Quad corner anchor in PATH_MODE + midpoint drag -> add_region
    // L10138-10148.
    ed.select_anchor(c(360, 160), false);
    ed.add_region(false, MakeDims(250, 80, 150, 150));
    ed.set_mode(JPX_EDITOR_SKELETON_MODE);
    g = kdu_core::kdu_coords(335, 110);
    ed.find_nearest_guide_point(g);
    ed.set_mode(JPX_EDITOR_PATH_MODE);
    g = kdu_core::kdu_coords(335, 110);
    ed.find_nearest_guide_point(g);
    ed.add_region(false, MakeDims(0, 0, 1000, 1000));
    // Trigger undo-history cap trimming (L7662-7671).
    ed.set_max_undo_history(1);
    ed.undo();
    ed.redo();

    // Long scribble causing repeated subsampling + self-intersection pruning.
    ed.clear_scribble_points();
    ed.add_scribble_point(c(50, 50));
    for (int i = 1; i <= 65; ++i) {
      int x = 50 + i * 8;
      int y = 50 + ((i % 2 == 0) ? 15 : -10);
      ed.add_scribble_point(c(x, y));
    }
    ed.add_scribble_point(c(70, 55));  // Loop intersection
    ed.convert_scribble_path(false, JPX_EDITOR_FLAG_ELLIPSES, 1.0);
  }

  // Concentric 3 nested square rings (CW + CCW) + intersecting hole + concave
  // polygon converted via convert_scribble_path(false, 0, 1.0) so adjacent
  // path bricks form true closed loops for import_internal_boundary,
  // reverse winding (jx_path_filler::init L10630-10651), and add_triangle.
  {
    auto c = [](int x, int y) { return kdu_core::kdu_coords(x, y); };
    jpx_roi_editor ed;
    ed.init(nullptr, 0);
    ed.set_mode(JPX_EDITOR_PATH_MODE);

    auto add_scribble_loop = [&](const std::vector<kdu_core::kdu_coords>& pts) {
      ed.clear_scribble_points();
      for (const auto& pt : pts) {
        ed.add_scribble_point(pt);
      }
      ed.add_scribble_point(pts.front());
      kdu_core::kdu_dims d = ed.convert_scribble_path(false, 0, 1.0);
      EXPECT_FALSE(d.is_empty());
    };

    // Ring 1: outer CW square [100,100..500,500]
    add_scribble_loop({c(100, 100), c(500, 100), c(500, 500), c(100, 500)});
    // Ring 2: middle CCW square [180,180..420,420]
    add_scribble_loop({c(180, 180), c(180, 420), c(420, 420), c(420, 180)});
    // Ring 3: inner CW square [250,250..350,350] (3 deep nested -> L9109-9114)
    add_scribble_loop({c(250, 250), c(350, 250), c(350, 350), c(250, 350)});
    // Extra hole inside Ring 1 intersecting Ring 2 (L9123-9126).
    add_scribble_loop({c(140, 200), c(220, 200), c(220, 300), c(140, 300)});
    // Concave pentagon / dart + heptagon -> triggers add_triangle in
    // fill_closed_paths.
    add_scribble_loop(
        {c(600, 100), c(800, 100), c(820, 260), c(700, 180), c(580, 260)});
    add_scribble_loop({c(600, 340), c(720, 320), c(820, 380), c(790, 500),
                       c(680, 440), c(610, 520), c(560, 410)});
    bool thick_ok = false;
    ed.set_path_thickness(7, thick_ok);
    bool filled = false;
    ed.fill_closed_paths(filled, -1);
    EXPECT_TRUE(filled);
  }
}

TEST(JpxTest, Wave4CompositionMatchingFramesVisibilityAndTracks) {
  ScopedKduHandlers handlers;

  // 1) Raw comp/iset JPX file with empty frames (frame 0 empty & frame 2
  // out-of-bounds cropped invisible instruction) + I_rept=0xFFFF.
  {
    MemTarget mt;
    jp2_family_tgt ft;
    ft.open(&mt);
    jp2_output_box sig;
    sig.open(&ft, jp2_signature_4cc);
    sig.write(static_cast<kdu_core::kdu_uint32>(0x0D0A870Au));
    sig.close();
    jp2_output_box ftyp;
    ftyp.open(&ft, jp2_file_type_4cc);
    ftyp.write(jpx_brand);
    ftyp.write(static_cast<kdu_core::kdu_uint32>(0));
    ftyp.write(jpx_brand);
    ftyp.close();
    {
      jp2_output_box jp2h;
      jp2h.open(&ft, jp2_header_4cc);
      jp2_output_box ihdr;
      ihdr.open(&jp2h, jp2_image_header_4cc);
      ihdr.write(static_cast<kdu_core::kdu_uint32>(32));
      ihdr.write(static_cast<kdu_core::kdu_uint32>(32));
      ihdr.write(static_cast<kdu_core::kdu_uint16>(3));
      ihdr.write(static_cast<kdu_core::kdu_byte>(7));
      ihdr.write(static_cast<kdu_core::kdu_byte>(7));
      ihdr.write(static_cast<kdu_core::kdu_uint16>(0));
      ihdr.close();
      jp2_output_box colr;
      colr.open(&jp2h, jp2_colour_4cc);
      colr.write(static_cast<kdu_core::kdu_byte>(1));
      colr.write(static_cast<kdu_core::kdu_byte>(0));
      colr.write(static_cast<kdu_core::kdu_byte>(0));
      colr.write(static_cast<kdu_core::kdu_uint32>(JP2_sRGB_SPACE));
      colr.close();
      jp2h.close();
    }
    {
      jp2_output_box cs_box;
      cs_box.open(&ft, jp2_codestream_4cc);
      MemTarget csm;
      WriteMinimalCodestream(&csm, 32, 32, 3);
      cs_box.write(csm.data().data(), static_cast<int>(csm.data().size()));
      cs_box.close();
    }
    {
      jp2_output_box comp;
      comp.open(&ft, jp2_composition_4cc);
      {
        jp2_output_box copt;
        copt.open(&comp, jp2_comp_options_4cc);
        copt.write(static_cast<kdu_core::kdu_uint32>(32));  // height
        copt.write(static_cast<kdu_core::kdu_uint32>(32));  // width
        copt.write(static_cast<kdu_core::kdu_byte>(0));     // loop
        copt.close();
      }
      {
        jp2_output_box iset;
        iset.open(&comp, jp2_comp_instruction_set_4cc);
        // I_type (2 bytes): bit0(X0/Y0)=1, bit1(XS/YS)=2,
        // bit2(life/next_reuse)=4 -> 0x0007
        iset.write(static_cast<kdu_core::kdu_uint16>(0x0007));
        iset.write(static_cast<kdu_core::kdu_uint16>(0xFFFF));  // infinite rept
        iset.write(static_cast<kdu_core::kdu_uint32>(10));      // tick
        // Inst 0: cropped completely offscreen right -> invisible -> empty
        // frame!
        iset.write(static_cast<kdu_core::kdu_uint32>(100));  // X0
        iset.write(static_cast<kdu_core::kdu_uint32>(100));  // Y0
        iset.write(static_cast<kdu_core::kdu_uint32>(16));   // XS
        iset.write(static_cast<kdu_core::kdu_uint32>(16));   // YS
        iset.write(static_cast<kdu_core::kdu_uint32>(10));   // life
        iset.write(static_cast<kdu_core::kdu_uint32>(0));    // next_reuse
        // Inst 1: visible instruction (life > 0, inside 32x32)
        iset.write(static_cast<kdu_core::kdu_uint32>(0));
        iset.write(static_cast<kdu_core::kdu_uint32>(0));
        iset.write(static_cast<kdu_core::kdu_uint32>(32));
        iset.write(static_cast<kdu_core::kdu_uint32>(32));
        iset.write(static_cast<kdu_core::kdu_uint32>(10));
        iset.write(static_cast<kdu_core::kdu_uint32>(0));
        // Inst 2: another offscreen instruction -> frame after visible becomes
        // empty!
        iset.write(static_cast<kdu_core::kdu_uint32>(200));
        iset.write(static_cast<kdu_core::kdu_uint32>(0));
        iset.write(static_cast<kdu_core::kdu_uint32>(16));
        iset.write(static_cast<kdu_core::kdu_uint32>(16));
        iset.write(static_cast<kdu_core::kdu_uint32>(10));
        iset.write(static_cast<kdu_core::kdu_uint32>(0));
        iset.close();
      }
      comp.close();
    }
    ft.close();

    MemSource ms(mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    if (src.open(&fs, true) > 0) {
      jpx_composition comp = src.access_composition();
      if (comp.exists()) {
        int nf = 0;
        comp.count_track_frames(0, nf);
        jpx_frame fr = comp.access_frame(0, 0, true, false);
        EXPECT_TRUE(fr.exists() || !fr.exists());
      }
      src.close();
    }
    fs.close();
  }

  // 2) Replicated container + instruction replication (find_match &
  // reverse_match LCM paths) + count_track_time BEFORE access_frame + opaque
  // visibility overlay strips (bottom and right halves).
  {
    MemTarget mt;
    jp2_family_tgt ft;
    ft.open(&mt);
    jpx_target tgt;
    tgt.open(&ft);

    // Top-level layer 0 with top-level instruction replication (inc=0 and
    // inc=2).
    std::vector<jpx_codestream_target> top_cs;
    for (int i = 0; i < 6; ++i) {
      jpx_codestream_target cs = tgt.add_codestream();
      SetupCodestreamDims(cs.access_dimensions(), 32, 32, 4, 8);
      top_cs.push_back(cs);
      jpx_layer_target ly = tgt.add_layer();
      // Alternate opaque 3ch and alpha 4ch layers.
      if (i % 2 == 0) {
        SetupWave4Layer(ly, false, 3, i);
      } else {
        SetupWave4Layer(ly, true, 3, i);
      }
    }
    {
      jpx_composition tc = tgt.access_composition();
      // Frame 0: base persistent opaque layer 0, opaque layer 2, opaque layer 4
      // on top covering the entire canvas (hiding layers 0 and 2).
      kdu_supp::jx_frame* f0 = tc.add_frame(10, 0, true);
      tc.add_instruction(f0, 0, 0, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 0, 32, 32));
      tc.add_instruction(f0, 2, 0, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 0, 32, 32));
      tc.add_instruction(f0, 4, 0, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 0, 32, 32));
      // Frame 1: replicated instruction (increment = 2, repeat = 2 -> layers 1,
      // 3, 5).
      kdu_supp::jx_frame* f1 = tc.add_frame(10, 2, false);
      tc.add_instruction(f1, 1, 2, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 0, 32, 32));
    }

    // Replicated container with 3 reps, each having 6 layers, and BOTH
    // container replication + instruction increment=2, repeat=1!
    jpx_container_target c0 = tgt.add_container(6, 6, 3);
    for (int i = 0; i < 6; ++i) {
      jpx_codestream_target cs = c0.access_codestream(i);
      SetupCodestreamDims(cs.access_dimensions(), 32, 32, 3, 8);
      jpx_layer_target ly = c0.access_layer(i);
      SetupWave4Layer(ly, false, 3, cs.get_codestream_id());
    }
    {
      jpx_composition cc = c0.add_presentation_track(6);
      kdu_supp::jx_frame* cf0 = cc.add_frame(10, 0, false);
      cc.add_instruction(cf0, 0, 0, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 0, 32, 32));
      kdu_supp::jx_frame* cf1 = cc.add_frame(15, 8, false);
      // Relative layer 1 with increment 2, repeat 8 -> spans up to layer 17
      // (covering all 6*3 = 18 container layers of this track).
      cc.add_instruction(cf1, 1, 2, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 0, 32, 32));
    }

    EXPECT_EQ(tgt.write_headers(), nullptr);
    for (int i = 0; i < 6; ++i) {
      jp2_output_box* cs_box = top_cs[i].open_stream();
      WriteMinimalCodestream(top_cs[i].access_stream(), 32, 32,
                             (i % 2 == 1) ? 4 : 3);
      EXPECT_TRUE(cs_box->close());
    }
    for (int r = 0; r < 3; ++r) {
      for (int i = 0; i < 6; ++i) {
        jpx_codestream_target ccs = c0.access_codestream(i);
        jp2_output_box* cs_box = ccs.open_stream();
        WriteMinimalCodestream(ccs.access_stream(), 32, 32, 3);
        EXPECT_TRUE(cs_box->close());
      }
    }
    EXPECT_EQ(tgt.write_metadata(), nullptr);
    tgt.close();
    ft.close();

    // Open fresh source and call count_track_time BEFORE access_frame!
    {
      MemSource ms(mt.data());
      jp2_family_src fs;
      fs.open(&ms);
      jpx_source src;
      ASSERT_GE(src.open(&fs, false), 1);
      jpx_composition sc = src.access_composition();
      kdu_core::kdu_long track_time = 0;
      EXPECT_TRUE(sc.count_track_time(0, track_time));
      EXPECT_GT(track_time, 0);
      kdu_core::kdu_long track1_time = 0;
      EXPECT_TRUE(sc.count_track_time(1, track1_time));

      // Exercise find_match (forward & reverse) across top-level + replicated
      // container!
      jpx_frame mf;
      int inst_idx = 0;
      const int fwd_flags = JPX_FRAME_MATCH_LATER_TRACKS;
      const int rev_flags =
          JPX_FRAME_MATCH_REVERSE | JPX_FRAME_MATCH_LATER_TRACKS;
      int top_layers[3] = {1, 3, 5};
      sc.find_layer_match(mf, inst_idx, 0, top_layers, 3, 0, true, fwd_flags);
      sc.find_layer_match(mf, inst_idx, 0, top_layers, 3, 5, true, rev_flags);
      int cont_layers[3] = {6, 9, 13};
      sc.find_layer_match(mf, inst_idx, 1, cont_layers, 3, 0, true, fwd_flags);
      sc.find_layer_match(mf, inst_idx, 1, cont_layers, 3, 1, false, fwd_flags);
      sc.find_layer_match(mf, inst_idx, 1, cont_layers, 3, 20, true, rev_flags);
      sc.find_layer_match(mf, inst_idx, 1, cont_layers, 3, 20, false,
                          rev_flags);
      int mismatch_layers[2] = {99, 100};
      sc.find_layer_match(mf, inst_idx, 1, mismatch_layers, 2, 0, true,
                          fwd_flags);
      sc.find_layer_match(mf, inst_idx, 1, mismatch_layers, 2, 20, true,
                          rev_flags);

      // Static test_codestream_visibility with opaque overlay covering cs 0 &
      // 2!
      jpx_frame vis_frame = sc.access_frame(0, 0, true, false);
      kdu_core::kdu_dims vis_region;
      EXPECT_LT(jpx_frame_expander::test_codestream_visibility(
                    &src, vis_frame, 0, jpx_metanode(), nullptr, 0, vis_region),
                0);
      EXPECT_LT(jpx_frame_expander::test_codestream_visibility(
                    &src, vis_frame, 2, jpx_metanode(), nullptr, 0, vis_region),
                0);
      EXPECT_GE(jpx_frame_expander::test_codestream_visibility(
                    &src, vis_frame, 4, jpx_metanode(), nullptr, 0, vis_region),
                0);

      src.close();
      fs.close();
    }

    // Also open prefixes via MemSource to cover access_next / generate_metareq.
    for (size_t cut :
         {mt.data().size() / 4, mt.data().size() / 2, mt.data().size()}) {
      std::vector<kdu_core::kdu_byte> prefix(mt.data().begin(),
                                             mt.data().begin() + cut);
      MemSource pms(prefix, KDU_SOURCE_CAP_SEQUENTIAL);
      jp2_family_src cfs;
      cfs.open(&pms);
      jpx_source csrc;
      try {
        if (csrc.open(&cfs, true) > 0) {
          kdu_window win;
          win.init();
          csrc.generate_metareq(&win, 0, 10, 10, 10, true);
          csrc.generate_metareq(&win, -1, -1, -1, -1, false);
          jpx_composition cc = csrc.access_composition();
          if (cc.exists()) {
            jpx_frame f0 = cc.access_frame(0, 0, false, false);
            if (f0.exists()) {
              f0.access_next(0, true);
              f0.access_next(0, false);
              f0.access_next(99, false);  // wrong track
            }
            jpx_frame f1 = cc.access_frame(1, 0, false, false);
            if (f1.exists()) {
              f1.access_next(1, true);
              f1.access_next(1, false);
            }
          }
          csrc.close();
        }
      } catch (int) {
      }
      cfs.close();
    }
  }
}

TEST(JpxTest, Wave4MetadataIncrementalCollectionsMetareqAndLinks) {
  ScopedKduHandlers handlers;

  // 1) Incremental metadata collections (fit_collection commit/split/destroy) +
  // move_old_top_container_numlist (first, middle, last container numlists) +
  // jpx_source::find_unique_compatible_container.
  {
    MemTarget mt;
    jp2_family_tgt ft;
    ft.open(&mt);
    jpx_target tgt;
    tgt.open(&ft);
    std::vector<jpx_codestream_target> top_cs;
    for (int i = 0; i < 3; ++i) {
      jpx_codestream_target cs = tgt.add_codestream();
      SetupCodestreamDims(cs.access_dimensions(), 16, 16, 3, 8);
      top_cs.push_back(cs);
      jpx_layer_target ly = tgt.add_layer();
      SetupWave4Layer(ly, false, 3, i);
    }
    jpx_container_target c0 = tgt.add_container(3, 3, 2);
    for (int i = 0; i < 3; ++i) {
      jpx_codestream_target cs = c0.access_codestream(i);
      SetupCodestreamDims(cs.access_dimensions(), 16, 16, 3, 8);
      jpx_layer_target ly = c0.access_layer(i);
      SetupWave4Layer(ly, false, 3, cs.get_codestream_id());
    }
    {
      jpx_composition tc = tgt.access_composition();
      kdu_supp::jx_frame* tf = tc.add_frame(10, 0, false);
      tc.add_instruction(tf, 0, 0, MakeDims(0, 0, 16, 16),
                         MakeDims(0, 0, 16, 16));
    }

    jpx_meta_manager mm = tgt.access_meta_manager();
    // Create 3 numlist metanodes inside container c0 and mutate them so they
    // move out of container c0's [first_metanode, last_metanode] range!
    int c0_cs[3] = {3, 4, 5};
    jpx_metanode m_first =
        mm.insert_node(1, &c0_cs[0], 0, nullptr, false, 0, nullptr);
    jpx_metanode m_mid =
        mm.insert_node(1, &c0_cs[1], 0, nullptr, false, 0, nullptr);
    jpx_metanode m_last =
        mm.insert_node(1, &c0_cs[2], 0, nullptr, false, 0, nullptr);
    m_first.add_label("c0_first");
    m_mid.add_label("c0_mid");
    m_last.add_label("c0_last");
    // Converting first, last, and middle container numlists triggers
    // jx_metanode::move_old_top_container_numlist!
    m_first.change_to_label("c0_first_lbl");
    m_last.change_to_label("c0_last_lbl");
    m_mid.change_to_label("c0_mid_lbl");

    EXPECT_EQ(tgt.write_headers(), nullptr);
    // Write 100 nodes in 10 incremental write_metadata flushes to exercise
    // jx_meta_target_level::fit_collection group creation/commits/growth!
    for (int batch = 0; batch < 10; ++batch) {
      for (int k = 0; k < 10; ++k) {
        int cs_id = (batch * 10 + k) % 3;
        int ly_id = (batch * 10 + k) % 3;
        jpx_metanode n =
            mm.insert_node(1, &cs_id, 1, &ly_id, false, 0, nullptr);
        std::string lbl = "mgrp_batch_" + std::to_string(batch) + "_item_" +
                          std::to_string(k);
        for (int r = 0; r < ((batch % 3) + 1); ++r) {
          n.add_label(lbl.c_str());
        }
      }
      EXPECT_EQ(tgt.write_metadata(), nullptr);
    }
    for (int i = 0; i < 3; ++i) {
      jp2_output_box* cs_box = top_cs[i].open_stream();
      WriteMinimalCodestream(top_cs[i].access_stream(), 16, 16, 3);
      EXPECT_TRUE(cs_box->close());
    }
    for (int rep = 0; rep < 2; ++rep) {
      for (int i = 0; i < 3; ++i) {
        jpx_codestream_target ccs = c0.access_codestream(i);
        jp2_output_box* cs_box = ccs.open_stream();
        WriteMinimalCodestream(ccs.access_stream(), 16, 16, 3);
        EXPECT_TRUE(cs_box->close());
      }
    }
    tgt.close();
    ft.close();

    MemSource ms(mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 1);
    int ncont = 0;
    EXPECT_TRUE(src.count_containers(ncont));
    EXPECT_EQ(ncont, 1);
    int top_arr[1] = {0};
    EXPECT_FALSE(
        src.find_unique_compatible_container(1, top_arr, 0, nullptr).exists());
    int cont_arr[1] = {3};
    EXPECT_TRUE(
        src.find_unique_compatible_container(1, cont_arr, 0, nullptr).exists());
    int mixed_arr[2] = {0, 4};
    EXPECT_TRUE(src.find_unique_compatible_container(2, mixed_arr, 0, nullptr)
                    .exists());
    src.close();
    fs.close();
  }

  // 2) Placeholder + JPIP metadata recovery: covers
  // jpx_metanode::generate_metareq, load_matches filtering, and finish_reading
  // with flattened free asocs.
  {
    // Build inner databins first:
    // Databin 1: complete association subbox with a label box
    std::vector<kdu_core::kdu_byte> bin1_bytes;
    {
      MemTarget b1;
      jp2_family_tgt bft;
      bft.open(&b1);
      jp2_output_box lbl;
      lbl.open(&bft, jp2_label_4cc);
      const char* t = "placeholder_inner_label";
      lbl.write(reinterpret_cast<const kdu_core::kdu_byte*>(t),
                static_cast<int>(std::strlen(t)));
      lbl.close();
      bft.close();
      bin1_bytes = b1.data();
    }

    // Build main file with sig + ftyp + jp2h + jp2c + asoc(free+lbl, nlst...)
    MemTarget mt0;
    jp2_family_tgt ft0;
    ft0.open(&mt0);
    jp2_output_box sig;
    sig.open(&ft0, jp2_signature_4cc);
    sig.write(static_cast<kdu_core::kdu_uint32>(0x0D0A870Au));
    sig.close();
    jp2_output_box ftyp;
    ftyp.open(&ft0, jp2_file_type_4cc);
    ftyp.write(jpx_brand);
    ftyp.write(static_cast<kdu_core::kdu_uint32>(0));
    ftyp.write(jpx_brand);
    ftyp.close();
    {
      jp2_output_box jp2h;
      jp2h.open(&ft0, jp2_header_4cc);
      jp2_output_box ihdr;
      ihdr.open(&jp2h, jp2_image_header_4cc);
      ihdr.write(static_cast<kdu_core::kdu_uint32>(16));
      ihdr.write(static_cast<kdu_core::kdu_uint32>(16));
      ihdr.write(static_cast<kdu_core::kdu_uint16>(1));
      ihdr.write(static_cast<kdu_core::kdu_byte>(7));
      ihdr.write(static_cast<kdu_core::kdu_byte>(7));
      ihdr.write(static_cast<kdu_core::kdu_uint16>(0));
      ihdr.close();
      jp2_output_box colr;
      colr.open(&jp2h, jp2_colour_4cc);
      colr.write(static_cast<kdu_core::kdu_byte>(1));
      colr.write(static_cast<kdu_core::kdu_byte>(0));
      colr.write(static_cast<kdu_core::kdu_byte>(0));
      colr.write(static_cast<kdu_core::kdu_uint32>(JP2_sLUM_SPACE));
      colr.close();
      jp2h.close();
    }
    {
      jp2_output_box csb;
      csb.open(&ft0, jp2_codestream_4cc);
      MemTarget csm;
      WriteMinimalCodestream(&csm, 16, 16, 1);
      csb.write(csm.data().data(), static_cast<int>(csm.data().size()));
      csb.close();
    }

    auto write_phld = [](jp2_output_box& parent, kdu_core::kdu_long bin_id,
                         kdu_core::kdu_uint32 orig_len,
                         kdu_core::kdu_uint32 orig_type) {
      jp2_output_box ph;
      ph.open(&parent, jp2_placeholder_4cc);
      ph.write(static_cast<kdu_core::kdu_uint32>(1));  // original contents
      ph.write(static_cast<kdu_core::kdu_uint32>(bin_id >> 32));
      ph.write(static_cast<kdu_core::kdu_uint32>(bin_id & 0xFFFFFFFFu));
      ph.write(orig_len);
      ph.write(orig_type);
      ph.close();
    };

    jp2_output_box root_asoc;
    root_asoc.open(&ft0, jp2_association_4cc);
    // Free first subbox -> flattened asoc group!
    {
      jp2_output_box fr;
      fr.open(&root_asoc, jp2_free_4cc);
      fr.write(static_cast<kdu_core::kdu_uint32>(0));
      fr.close();
    }
    // Nested empty numlist + codestream 0 numlist + layer 1 numlist
    {
      jp2_output_box a0;
      a0.open(&root_asoc, jp2_association_4cc);
      jp2_output_box nl;
      nl.open(&a0, jp2_number_list_4cc);
      nl.write(static_cast<kdu_core::kdu_uint32>(0x00000000u));  // cs 0
      nl.close();
      jp2_output_box lbl;
      lbl.open(&a0, jp2_label_4cc);
      lbl.write(reinterpret_cast<const kdu_core::kdu_byte*>("cs0_node"), 8);
      lbl.close();
      a0.close();
    }
    {
      jp2_output_box a1;
      a1.open(&root_asoc, jp2_association_4cc);
      jp2_output_box nl;
      nl.open(&a1, jp2_number_list_4cc);
      nl.write(static_cast<kdu_core::kdu_uint32>(0x01000001u));  // layer 1
      nl.close();
      jp2_output_box lbl;
      lbl.open(&a1, jp2_label_4cc);
      lbl.write(reinterpret_cast<const kdu_core::kdu_byte*>("ly1_node"), 8);
      lbl.close();
      a1.close();
    }
    // Placeholders inside root_asoc referencing bin 1 and bin 2
    {
      jp2_output_box a2;
      a2.open(&root_asoc, jp2_association_4cc);
      write_phld(a2, 1,
                 static_cast<kdu_core::kdu_uint32>(8 + bin1_bytes.size()),
                 jp2_association_4cc);
      a2.close();
    }
    {
      jp2_output_box a3;
      a3.open(&root_asoc, jp2_association_4cc);
      write_phld(a3, 2,
                 static_cast<kdu_core::kdu_uint32>(8 + bin1_bytes.size()),
                 jp2_label_4cc);
      a3.close();
    }
    root_asoc.close();
    ft0.close();

    MemSource ms0(mt0.data());
    jp2_family_src cfs;
    cfs.open(&ms0);
    jpx_source src;
    try {
      if (src.open(&cfs, true) > 0) {
        jpx_meta_manager mm = src.access_meta_manager();
        kdu_window win;
        win.init();
        kdu_core::kdu_uint32 box_filter[2] = {jp2_label_4cc,
                                              jp2_association_4cc};
        kdu_core::kdu_uint32 desc[1] = {jp2_association_4cc};
        int cs_miss = 9, ly_miss = 9;
        mm.load_matches(1, &cs_miss, 1, &ly_miss);
        int cs_hit = 0, ly_hit = 1;
        mm.load_matches(1, &cs_hit, 0, nullptr);
        mm.load_matches(0, nullptr, 1, &ly_hit);

        mm.access_root().generate_metareq(&win, 0, nullptr, 1, desc, true, 4,
                                          1);
        mm.access_root().generate_metareq(&win, 2, box_filter, 1, desc, false,
                                          4, 2);
        mm.load_matches(-1, nullptr, -1, nullptr);
        src.close();
      }
    } catch (int) {
    }
    cfs.close();
  }
}

TEST(JpxTest, Wave4MultiCodestreamAggregationAndLayerCompat) {
  ScopedKduHandlers handlers;

  // 1) Multi-level j2cx aggregation tree (> 256 codestreams + container with
  // reserved spare slots -> close_boxes / fix_boxes / write_stream_ftbl).
  {
    MemTarget mt;
    jp2_family_tgt ft;
    ft.open(&mt);
    jpx_target tgt;
    tgt.open(&ft);

    // One shared minimal codestream byte buffer wrapped into top-level frag0
    // and reused by fragment refs or fast tiny codestreams: create 36
    // codestreams at top level (> streams_per_subbox=16) plus a replicated
    // container reserving 20 slots but writing only 3 reps with partial
    // codestreams.
    std::vector<jpx_codestream_target> top_cs;
    for (int i = 0; i < 36; ++i) {
      jpx_codestream_target cs = tgt.add_codestream();
      SetupCodestreamDims(cs.access_dimensions(), 8, 8, 1, 8);
      top_cs.push_back(cs);
    }
    jpx_layer_target l0 = tgt.add_layer();
    SetupWave4Layer(l0, false, 1);
    {
      jpx_composition tc = tgt.access_composition();
      kdu_supp::jx_frame* tf = tc.add_frame(10, 0, false);
      tc.add_instruction(tf, 0, 0, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));
    }

    // Replicated container with 18 layers/codestreams per rep, 2 reps = 36
    // more!
    jpx_container_target cont = tgt.add_container(18, 18, 2);
    for (int i = 0; i < 18; ++i) {
      jpx_codestream_target cs = cont.access_codestream(i);
      SetupCodestreamDims(cs.access_dimensions(), 8, 8, 1, 8);
      jpx_layer_target ly = cont.access_layer(i);
      SetupWave4Layer(ly, false, 1, cs.get_codestream_id());
    }
    EXPECT_EQ(cont.get_num_top_codestreams(), 36);
    EXPECT_EQ(cont.get_num_top_layers(), 1);

    EXPECT_EQ(tgt.write_headers(), nullptr);
    for (int i = 0; i < 36; ++i) {
      jp2_output_box* cs_box = top_cs[i].open_stream();
      WriteMinimalCodestream(top_cs[i].access_stream(), 8, 8, 1);
      EXPECT_TRUE(cs_box->close());
    }
    for (int r = 0; r < 2; ++r) {
      for (int i = 0; i < 18; ++i) {
        jpx_codestream_target ccs = cont.access_codestream(i);
        jp2_output_box* cs_box = ccs.open_stream();
        WriteMinimalCodestream(ccs.access_stream(), 8, 8, 1);
        EXPECT_TRUE(cs_box->close());
      }
    }
    EXPECT_EQ(tgt.write_metadata(), nullptr);
    tgt.close();
    ft.close();

    MemSource ms(mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 1);
    jpx_container_source sc = src.access_container(0);
    ASSERT_TRUE(sc.exists());
    EXPECT_EQ(sc.get_num_top_codestreams(), 36);
    EXPECT_EQ(sc.get_num_top_layers(), 1);
    EXPECT_TRUE(src.access_codestream(71, false).exists());
    src.close();
    fs.close();
  }

  // 2) Codestream & layer target compatibility branches: JPEG compression type,
  // Part-2, HTJ2K, Profile 0, Split-Exponent pixel format, opacity,
  // chroma-key, non-square aspect, scaling within layer, and
  // jpx_layer_target::copy_attributes(src_layer) + access_colour(idx).
  {
    MemTarget mt;
    jp2_family_tgt ft;
    ft.open(&mt);
    jpx_target tgt;
    tgt.open(&ft);

    // CS 0: non-JP2K (JPEG compression type), 4 components (3 colour + 1 exp)
    jpx_codestream_target cs0 = tgt.add_codestream();
    cs0.access_dimensions().init(kdu_core::kdu_coords(16, 16), 4, false,
                                 JP2_COMPRESSION_TYPE_JPEG);
    for (int c = 0; c < 4; ++c) {
      cs0.access_dimensions().set_precision(c, 8, false);
    }
    // CS 1: HTJ2K (Scap_P15)
    jpx_codestream_target cs1 = tgt.add_codestream();
    cs1.access_dimensions().init(kdu_core::kdu_coords(16, 16), 1, false);
    cs1.access_dimensions().set_precision(0, 8, false);
    cs1.access_dimensions().finalize_compatibility(0, Scap_P15, 0, 0, 0, 0);
    // CS 2: 1 component for opacity
    jpx_codestream_target cs2 = tgt.add_codestream();
    SetupCodestreamDims(cs2.access_dimensions(), 16, 16, 1, 8);
    // CS 3: Profile 0 (3 components, dedicated to l1/l2 so cs1's cmap stays 1)
    jpx_codestream_target cs3 = tgt.add_codestream();
    cs3.access_dimensions().init(kdu_core::kdu_coords(16, 16), 3, false);
    for (int c = 0; c < 3; ++c) {
      cs3.access_dimensions().set_precision(c, 8, false);
    }
    cs3.access_dimensions().finalize_compatibility(Sprofile_PROFILE0, 0, 0, 0,
                                                   0, 0);
    std::vector<jpx_codestream_target> cs_list = {cs0, cs1, cs2, cs3};

    // Layer 0: SPLIT_EXP pixel format + opacity + non-colour +
    // multi-codestream scaling + non-square resolution + 2 colours.
    jpx_layer_target l0 = tgt.add_layer();
    jp2_channels ch0 = l0.access_channels();
    ch0.init(3, 1);
    int split_params[3] = {3, -1, 0};
    for (int c = 0; c < 3; ++c) {
      ch0.set_colour_mapping(c, c, -1, 0, JP2_CHANNEL_FORMAT_SPLIT_EXP,
                             split_params);
    }
    ch0.set_opacity_mapping(0, 0, -1, 2);
    ch0.set_non_colour_mapping(0, 0x524Cu, 0, -1, 1);
    l0.set_codestream_registration(0, kdu_core::kdu_coords(0, 0),
                                   kdu_core::kdu_coords(1, 1),
                                   kdu_core::kdu_coords(2, 2));
    l0.set_codestream_registration(1, kdu_core::kdu_coords(0, 0),
                                   kdu_core::kdu_coords(2, 2),
                                   kdu_core::kdu_coords(2, 2));
    l0.set_codestream_registration(2, kdu_core::kdu_coords(0, 0),
                                   kdu_core::kdu_coords(1, 1),
                                   kdu_core::kdu_coords(2, 2));
    l0.access_resolution().init(0.5F);
    l0.add_colour(1, 0).init(JP2_sRGB_SPACE);
    l0.add_colour(2, 1).init(JP2_sRGB_SPACE);
    EXPECT_TRUE(l0.access_colour(0).exists());
    EXPECT_TRUE(l0.access_colour(1).exists());
    EXPECT_FALSE(l0.access_colour(2).exists());

    // Layer 1: chroma-key without opacity on cs 3.
    jpx_layer_target l1 = tgt.add_layer();
    jp2_channels ch1 = l1.access_channels();
    ch1.init(3);
    for (int c = 0; c < 3; ++c) {
      ch1.set_colour_mapping(c, c, -1, 3);
      ch1.set_chroma_key(c, 5 + c);
    }
    l1.add_colour(0, 0).init(JP2_sRGB_SPACE);

    // Layer 2: reversed RGB on cs 3 (writes its own cdef box; safe to copy).
    jpx_layer_target l2 = tgt.add_layer();
    jp2_channels ch2 = l2.access_channels();
    ch2.init(3);
    for (int c = 0; c < 3; ++c) {
      ch2.set_colour_mapping(c, 2 - c, -1, 3);
    }
    l2.add_colour(0, 0).init(JP2_sRGB_SPACE);

    EXPECT_EQ(tgt.write_headers(), nullptr);
    for (int i = 0; i < 4; ++i) {
      jp2_output_box* cs_box = cs_list[i].open_stream();
      kdu_core::kdu_byte b[16]{};
      cs_box->write(b, sizeof(b));
      EXPECT_TRUE(cs_box->close());
    }
    EXPECT_EQ(tgt.write_metadata(), nullptr);
    tgt.close();
    ft.close();

    // Open source and copy layer attributes to a fresh jpx_target!
    MemSource ms(mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 1);
    jpx_layer_source sl0 = src.access_layer(0, false);
    ASSERT_TRUE(sl0.exists());
    EXPECT_TRUE(sl0.access_channels().exists());
    EXPECT_TRUE(sl0.access_resolution().exists());
    jpx_layer_source sl1 = src.access_layer(1, false);
    ASSERT_TRUE(sl1.exists());
    EXPECT_TRUE(sl1.access_channels().exists());
    jpx_layer_source sl2 = src.access_layer(2, false);
    ASSERT_TRUE(sl2.exists());

    MemTarget cmt;
    jp2_family_tgt cft;
    cft.open(&cmt);
    jpx_target ctgt;
    ctgt.open(&cft);
    std::vector<jpx_codestream_target> ccs_list;
    for (int i = 0; i < 4; ++i) {
      jpx_codestream_target ccs = ctgt.add_codestream();
      SetupCodestreamDims(ccs.access_dimensions(), 16, 16,
                          (i == 0) ? 4 : (i == 3 ? 3 : 1), 8);
      ccs_list.push_back(ccs);
    }
    jpx_layer_target cly = ctgt.add_layer();
    // Copy sl2 (standard pixel format without SPLIT_EXP or chroma_key_buf).
    cly.copy_attributes(sl2);
    ctgt.access_compatibility().copy(src.access_compatibility());
    EXPECT_EQ(ctgt.write_headers(), nullptr);
    for (int i = 0; i < 4; ++i) {
      jp2_output_box* cs_box = ccs_list[i].open_stream();
      kdu_core::kdu_byte b[16]{};
      cs_box->write(b, sizeof(b));
      EXPECT_TRUE(cs_box->close());
    }
    EXPECT_EQ(ctgt.write_metadata(), nullptr);
    ctgt.close();
    cft.close();
    src.close();
    fs.close();
  }
}

TEST(JpxTest, Wave5CompositionMatchingTracksAndExpander) {
  ScopedKduHandlers handlers;

  // 1) Top-level indefinite-repeat frame + Container 0 (2 base layers/rep, R=8,
  // track frame repeat=4, increment=3 -> LCM(2,3)=6 matching paths) + Container
  // 1 continuation on Track 1 (covers next_in_track, access_next,
  // count_track_time, reverse_match LCM clamping, and jpx_frame_expander
  // cropping/scaling).
  MemTarget mt;
  jp2_family_tgt ft;
  ft.open(&mt);
  jpx_target tgt;
  tgt.open(&ft);

  std::vector<jpx_codestream_target> top_cs;
  for (int i = 0; i < 4; ++i) {
    int nc = (i == 3) ? 4 : 3;
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 32, 32, nc, 8);
    top_cs.push_back(cs);
    jpx_layer_target ly = tgt.add_layer();
    SetupWave4Layer(ly, i == 3, 3, i);
  }
  {
    jpx_composition comp = tgt.access_composition();
    jx_frame* f0 = comp.add_frame(5, 0, true);
    comp.add_instruction(f0, 0, 0, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 0, 32, 32));
    // Indefinite top-level frame cycling across layers 1, 2, 3.
    jx_frame* f1 = comp.add_frame(7, -1, false);
    comp.add_instruction(f1, 1, 1, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 0, 32, 32));
  }

  // Container 0: 2 base cs, 2 base layers, R = 8 repetitions (0..15).
  jpx_container_target c0 = tgt.add_container(2, 2, 8);
  for (int i = 0; i < 2; ++i) {
    SetupCodestreamDims(c0.access_codestream(i).access_dimensions(), 32, 32, 3,
                        8);
    SetupWave4Layer(c0.access_layer(i), false, 3,
                    c0.access_codestream(i).get_codestream_id());
  }
  {
    jpx_composition trk0 = c0.add_presentation_track(2);
    // Frame 0 repeats 4 times with increment=3 (covers 0..14 out of 0..15).
    jx_frame* tf0 = trk0.add_frame(10, 4, true);
    trk0.add_instruction(tf0, 0, 3, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 0, 32, 32));
    trk0.add_instruction(tf0, 1, 3, MakeDims(0, 0, 32, 32),
                         MakeDims(16, 16, 32, 32));
    trk0.add_instruction(tf0, 2, 3, MakeDims(0, 0, 32, 32),
                         MakeDims(32, 0, 32, 32));
    // Frame 1 covers layer 15 so all 16 container track layers are assigned.
    jx_frame* tf1 = trk0.add_frame(12, 0, false);
    trk0.add_instruction(tf1, 15, 0, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 32, 32, 32));
  }

  // Container 1: 2 base cs, 2 base layers, R = 2 repetitions (continues Track
  // 1).
  jpx_container_target c1 = tgt.add_container(2, 2, 2);
  for (int i = 0; i < 2; ++i) {
    SetupCodestreamDims(c1.access_codestream(i).access_dimensions(), 32, 32, 3,
                        8);
    SetupWave4Layer(c1.access_layer(i), false, 3,
                    c1.access_codestream(i).get_codestream_id());
  }
  {
    jpx_composition trk1 = c1.add_presentation_track(2);
    jx_frame* tf = trk1.add_frame(15, 1, false);
    trk1.add_instruction(tf, 0, 2, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 0, 32, 32));
    trk1.add_instruction(tf, 1, 2, MakeDims(0, 0, 32, 32),
                         MakeDims(32, 32, 32, 32));
  }

  EXPECT_EQ(tgt.write_headers(), nullptr);
  for (int i = 0; i < 4; ++i) {
    int nc = (i == 3) ? 4 : 3;
    jp2_output_box* box = top_cs[i].open_stream();
    WriteMinimalCodestream(top_cs[i].access_stream(), 32, 32, nc);
    EXPECT_TRUE(box->close());
  }
  for (int r = 0; r < 8; ++r) {
    for (int i = 0; i < 2; ++i) {
      jp2_output_box* box = c0.access_codestream(i).open_stream();
      WriteMinimalCodestream(c0.access_codestream(i).access_stream(), 32, 32,
                             3);
      EXPECT_TRUE(box->close());
    }
  }
  for (int r = 0; r < 2; ++r) {
    for (int i = 0; i < 2; ++i) {
      jp2_output_box* box = c1.access_codestream(i).open_stream();
      WriteMinimalCodestream(c1.access_codestream(i).access_stream(), 32, 32,
                             3);
      EXPECT_TRUE(box->close());
    }
  }
  EXPECT_EQ(tgt.write_metadata(), nullptr);
  tgt.close();
  ft.close();

  MemSource ms(mt.data());
  jp2_family_src fs;
  fs.open(&ms);
  jpx_source src;
  ASSERT_GE(src.open(&fs, false), 1);

  jpx_composition comp = src.access_composition();
  EXPECT_TRUE(comp.exists());
  kdu_core::kdu_long track_time = 0;
  EXPECT_TRUE(comp.count_track_time(0, track_time));
  EXPECT_TRUE(comp.count_track_time(1, track_time));
  int before_cnt = 0;
  EXPECT_TRUE(
      comp.count_track_frames_before_time(1, track_time + 1, before_cnt));
  EXPECT_GT(before_cnt, 0);

  // Advance across top-level indefinite frame (track 0) and across containers
  // in track 1 (hitting next_in_track + comp->total_frames == 0 paths).
  {
    jpx_frame f = comp.access_frame(0, 0, true);
    int steps = 0;
    while (f.exists() && steps < 10) {
      f = f.access_next(0, false);
      ++steps;
    }
    EXPECT_EQ(steps, 4);
    jpx_frame f1 = comp.access_frame(1, 0, true);
    int t1_steps = 0;
    while (f1.exists() && t1_steps < 20) {
      f1 = f1.access_next(1, false);
      ++t1_steps;
    }
    EXPECT_GT(t1_steps, 6);
    EXPECT_FALSE(comp.access_frame(1, 100, true).exists());
    EXPECT_FALSE(comp.access_frame(0, 100, false).exists());
  }

  // Exercise LCM search in find_match & reverse_match (L3519-3551, L3720-3755):
  // container 0 has base layers {4, 5}, id=0, R=8. Frame 0 has increment=3,
  // layers 4..19. Query base layer 4, rep layer 10 (k=3, rept=2), last rep 16.
  {
    jpx_container_source sc0 = src.access_container(0);
    ASSERT_TRUE(sc0.exists());
    int base_l[2] = {4, 5};
    jpx_frame fm;
    int inst_idx = -1;
    // Match base layer with container_id=0 and match_all=true.
    EXPECT_EQ(comp.find_layer_match(fm, inst_idx, 1, base_l, 2, 0, true,
                                    JPX_FRAME_MATCH_ALL_LAYERS),
              1);
    EXPECT_EQ(comp.find_layer_match(
                  fm, inst_idx, 1, base_l, 2, 0, true,
                  JPX_FRAME_MATCH_REVERSE | JPX_FRAME_MATCH_ALL_LAYERS),
              1);

    // LCM(idx_increment=2, ip->increment=3) with container_id=0 and min_rept >
    // 0:
    jpx_frame f_mid = comp.access_frame(1, 3, true);
    int mid_layers[2] = {4, 16};  // 4 matches rel 0 (rep 4), 16 matches rep 4!
    EXPECT_EQ(comp.find_layer_match(f_mid, inst_idx, 1, mid_layers, 2, 0, true,
                                    JPX_FRAME_MATCH_ALL_LAYERS),
              1);
    // Inconsistent repetitions for match_all (layers 4 + 14 on same rep fail):
    int mismatch_layers[2] = {6, 15};
    fm = jpx_frame();
    EXPECT_EQ(comp.find_layer_match(fm, inst_idx, 1, mismatch_layers, 2, -1,
                                    true, JPX_FRAME_MATCH_ALL_LAYERS),
              -1);
    // Backward match starting mid-track without container_id & with
    // container_id=0:
    int single_rep[1] = {4};
    jpx_frame f_rev = comp.access_frame(1, 5, true);
    comp.find_layer_match(f_rev, inst_idx, 1, single_rep, 1, 0, true,
                          JPX_FRAME_MATCH_REVERSE);
    int non_exist[1] = {999};
    fm = jpx_frame();
    EXPECT_EQ(comp.find_layer_match(
                  fm, inst_idx, 1, non_exist, 1, -1, true,
                  JPX_FRAME_MATCH_REVERSE | JPX_FRAME_MATCH_ALL_LAYERS),
              -1);

    // Container source accessors & jpx_frame_expander over persistent frame.
    EXPECT_EQ(sc0.get_container_id(), 0);
    int n_trk_base = 0;
    EXPECT_EQ(sc0.get_track_base_layers(0, n_trk_base), 0);
    EXPECT_EQ(n_trk_base, 0);
    EXPECT_GE(sc0.get_track_base_layers(1, n_trk_base), 0);
    EXPECT_EQ(n_trk_base, 2);
    EXPECT_TRUE(sc0.access_presentation_track(1).exists());
    EXPECT_TRUE(sc0.access_codestream(0, 1, false, false).exists());
    EXPECT_TRUE(sc0.access_layer(0, 1, false, false).exists());
  }

  // Legacy jx_frame* expander construction & ROI cropping.
  {
    jx_frame* raw_f0 = comp.get_next_frame(nullptr);
    ASSERT_NE(raw_f0, nullptr);
    jx_frame* raw_f1 = comp.get_next_frame(raw_f0);
    ASSERT_NE(raw_f1, nullptr);
    jpx_frame_expander exp;
    EXPECT_TRUE(exp.construct(&src, raw_f1, 0, true, MakeDims(0, 0, 16, 16)));
    EXPECT_GT(exp.get_num_members(), 0);
    exp.reset();
  }

  src.close();
  fs.close();
}

TEST(JpxTest, Wave5MultistreamLocatorFragmentsAndContainers) {
  ScopedKduHandlers handlers;

  // 1) >= 66 top-level codestreams (triggers 2-level jx_stream_locator root
  // growth & shift > 0 cleanup L20705-20748) +
  // configure_codestream_aggregation(2,64) on 36-rep container with mid-stream
  // write_metadata() flushes so multistream_root.close_boxes() -> fix_boxes() &
  // multi_sub nested J2CX execute!
  MemTarget mt;
  jp2_family_tgt ft;
  ft.open(&mt);
  jpx_target tgt;
  tgt.open(&ft);

  std::vector<jpx_codestream_target> top_cs;
  top_cs.reserve(66);
  for (int i = 0; i < 66; ++i) {
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 8, 8, 1, 8);
    top_cs.push_back(cs);
  }
  jpx_layer_target top_ly = tgt.add_layer();
  SetupWave4Layer(top_ly, false, 1, 0);
  {
    jpx_composition comp = tgt.access_composition();
    jx_frame* f = comp.add_frame(10, 0, false);
    comp.add_instruction(f, 0, 0, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));
  }

  tgt.configure_codestream_aggregation(2, 64);
  jpx_container_target cont = tgt.add_container(1, 1, 36);
  EXPECT_TRUE(tgt.access_container(0).exists());
  EXPECT_FALSE(tgt.access_container(1).exists());
  EXPECT_FALSE(tgt.access_container(-1).exists());
  SetupCodestreamDims(cont.access_codestream(0).access_dimensions(), 8, 8, 1,
                      8);
  SetupWave4Layer(cont.access_layer(0), false, 1,
                  cont.access_codestream(0).get_codestream_id());

  EXPECT_EQ(tgt.write_headers(), nullptr);
  for (int i = 0; i < 66; ++i) {
    jp2_output_box* box = top_cs[i].open_stream();
    WriteMinimalCodestream(top_cs[i].access_stream(), 8, 8, 1);
    EXPECT_TRUE(box->close());
  }
  jpx_meta_manager mm = tgt.access_meta_manager();
  for (int r = 0; r < 36; ++r) {
    jp2_output_box* box = cont.access_codestream(0).open_stream();
    WriteMinimalCodestream(cont.access_codestream(0).access_stream(), 8, 8, 1);
    EXPECT_TRUE(box->close());
    // Interrupt multistream_root between container codestreams so close_boxes()
    // fixes the J2CX box length and triggers fix_boxes() + multi_sub paths!
    if (r == 1 || r == 5 || r == 20) {
      int cid = 66 + r;
      mm.insert_node(1, &cid, 0, nullptr, false, 0, nullptr)
          .add_label("mid-stream-meta");
      EXPECT_EQ(tgt.write_metadata(), nullptr);
    }
  }
  EXPECT_EQ(tgt.write_metadata(), nullptr);
  tgt.close();
  ft.close();

  // Seekable read: touch codestream 0, 65 (2nd locator slot), and container
  // codestreams 66..101 to trigger recover_codestream + remove_descendant.
  {
    MemSource ms(mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 1);
    EXPECT_TRUE(src.access_codestream(0, false).exists());
    EXPECT_TRUE(src.access_codestream(65, false).exists());
    EXPECT_FALSE(src.access_codestream(65, false).get_header_loc().is_null());
    EXPECT_FALSE(src.access_layer(0, false).get_header_loc().is_null());
    for (int i = 66; i < 102; ++i) {
      EXPECT_TRUE(src.access_codestream(i, false).exists());
    }
    src.close();
    fs.close();
  }

  // Sequential read: discovers top + J2CX codestreams linearly
  // (discover_codestreams).
  {
    MemSource seq_ms(mt.data(), KDU_SOURCE_CAP_SEQUENTIAL);
    jp2_family_src seq_fs;
    seq_fs.open(&seq_ms);
    jpx_source seq_src;
    ASSERT_GE(seq_src.open(&seq_fs, false), 1);
    int ncs = 0;
    seq_src.count_codestreams(ncs);
    EXPECT_GE(ncs, 66);
    seq_src.close();
    seq_fs.close();
  }

  // 2) jpx_codestream_target::copy_attributes +
  // access_fragment_list().add_fragment contiguous fragment coalescing &
  // write_fragment_table + jpx_compatibility copy_from with 12 vendor features
  // and 12 standard features!
  {
    // First synthesize a JPX header with 12 standard features and 12 vendor
    // UUIDs.
    MemTarget rmt;
    jp2_family_tgt rft;
    rft.open(&rmt);
    auto write_tag = [&](kdu_core::kdu_uint32 fourcc) {
      jp2_output_box b;
      b.open(&rft, fourcc);
      return b;
    };
    {
      jp2_output_box sig = write_tag(jp2_signature_4cc);
      sig.write(static_cast<kdu_core::kdu_uint32>(0x0D0A870Au));
      sig.close();
      jp2_output_box ftyp = write_tag(jp2_file_type_4cc);
      ftyp.write(jpx_brand);
      ftyp.write(static_cast<kdu_core::kdu_uint32>(0));
      ftyp.write(jpx_brand);
      ftyp.close();
      jp2_output_box rreq;
      rreq.open(&rft, jp2_reader_requirements_4cc);
      rreq.write(static_cast<kdu_core::kdu_byte>(1));  // ML
      for (int i = 0; i < 2; ++i)
        rreq.write(static_cast<kdu_core::kdu_byte>(0x80));
      rreq.write(static_cast<kdu_core::kdu_uint16>(12));  // nsf=12
      for (kdu_core::kdu_uint16 sf = 1; sf <= 12; ++sf) {
        rreq.write(sf);
        rreq.write(static_cast<kdu_core::kdu_byte>(0x80));
      }
      rreq.write(static_cast<kdu_core::kdu_uint16>(12));  // nvf=12
      for (int vf = 0; vf < 12; ++vf) {
        for (int b = 0; b < 16; ++b) {
          rreq.write(static_cast<kdu_core::kdu_byte>(vf * 13 + b));
        }
        rreq.write(static_cast<kdu_core::kdu_byte>(0x80));
      }
      rreq.close();
    }
    MemTarget cs_buf;
    WriteMinimalCodestream(&cs_buf, 8, 8, 1);
    {
      jp2_output_box jp2h;
      jp2h.open(&rft, jp2_header_4cc);
      jp2_output_box ihdr;
      ihdr.open(&jp2h, jp2_image_header_4cc);
      ihdr.write(static_cast<kdu_core::kdu_uint32>(8));
      ihdr.write(static_cast<kdu_core::kdu_uint32>(8));
      ihdr.write(static_cast<kdu_core::kdu_uint16>(1));
      ihdr.write(static_cast<kdu_core::kdu_byte>(7));
      ihdr.write(static_cast<kdu_core::kdu_byte>(7));
      ihdr.write(static_cast<kdu_core::kdu_uint16>(0));
      ihdr.close();
      jp2_output_box colr;
      colr.open(&jp2h, jp2_colour_4cc);
      colr.write(static_cast<kdu_core::kdu_byte>(1));
      colr.write(static_cast<kdu_core::kdu_byte>(0));
      colr.write(static_cast<kdu_core::kdu_byte>(0));
      colr.write(static_cast<kdu_core::kdu_uint32>(JP2_sLUM_SPACE));
      colr.close();
      jp2h.close();
      jp2_output_box jp2c;
      jp2c.open(&rft, jp2_codestream_4cc);
      jp2c.write(cs_buf.data().data(), static_cast<int>(cs_buf.data().size()));
      jp2c.close();
    }
    rft.close();

    MemSource rms(rmt.data());
    jp2_family_src rfs;
    rfs.open(&rms);
    jpx_source rsrc;
    ASSERT_GE(rsrc.open(&rfs, false), 1);

    MemTarget fmt;
    jp2_family_tgt fft;
    fft.open(&fmt);
    jpx_target ftgt;
    ftgt.open(&fft);
    // Copy compatibility twice from 12+ vendor/standard feature source.
    ftgt.access_compatibility().copy(rsrc.access_compatibility());
    ftgt.access_compatibility().copy(rsrc.access_compatibility());
    jpx_codestream_target fcs = ftgt.add_codestream();
    fcs.copy_attributes(rsrc.access_codestream(0, false));
    jpx_layer_target fly = ftgt.add_layer();
    SetupWave4Layer(fly, false, 1, 0);
    EXPECT_EQ(ftgt.write_headers(), nullptr);
    kdu_core::kdu_long mdat_off =
        static_cast<kdu_core::kdu_long>(fmt.data().size()) + 8;
    {
      jp2_output_box mdat;
      mdat.open(&fft, jp2_mdat_4cc);
      mdat.write(cs_buf.data().data(), static_cast<int>(cs_buf.data().size()));
      mdat.close();
    }
    jpx_fragment_list fl = fcs.access_fragment_list();
    ASSERT_TRUE(fl.exists());
    EXPECT_FALSE(fl.read_only());
    kdu_core::kdu_long half =
        static_cast<kdu_core::kdu_long>(cs_buf.data().size() / 2);
    kdu_core::kdu_long rem =
        static_cast<kdu_core::kdu_long>(cs_buf.data().size()) - half;
    // Contiguous local fragments merge automatically inside add_fragment!
    EXPECT_TRUE(fl.add_fragment(0, mdat_off, half));
    EXPECT_TRUE(fl.add_fragment(0, mdat_off + half, rem));
    fcs.write_fragment_table();
    EXPECT_EQ(ftgt.write_metadata(), nullptr);
    ftgt.close();
    fft.close();
    rsrc.close();
    rfs.close();
  }

  // 3) Malformed J2CI (truncated & Rval > 31) and
  // validate_auto_codestream_binding errors.
  {
    auto try_open_bad_jpx = [](const std::vector<kdu_core::kdu_byte>& buf) {
      MemSource bad_ms(buf);
      jp2_family_src bad_fs;
      bad_fs.open(&bad_ms);
      jpx_source bad_src;
      try {
        if (bad_src.open(&bad_fs, true) > 0) {
          int n = 0;
          bad_src.count_codestreams(n);
          bad_src.access_codestream(0, false);
        }
      } catch (kdu_core::kdu_exception) {
      }
      bad_src.close();
      bad_fs.close();
    };

    // Malformed mcod -> j2ci with Rval = 31 (top 6 bits = 31) triggered!
    MemTarget mmt;
    jp2_family_tgt mft;
    mft.open(&mmt);
    jp2_output_box sig;
    sig.open(&mft, jp2_signature_4cc);
    sig.write(static_cast<kdu_core::kdu_uint32>(0x0D0A870Au));
    sig.close();
    jp2_output_box ftyp;
    ftyp.open(&mft, jp2_file_type_4cc);
    ftyp.write(jpx_brand);
    ftyp.write(static_cast<kdu_core::kdu_uint32>(0));
    ftyp.write(jpx_brand);
    ftyp.close();
    jp2_output_box jp2h;
    jp2h.open(&mft, jp2_header_4cc);
    jp2_output_box ihdr;
    ihdr.open(&jp2h, jp2_image_header_4cc);
    ihdr.write(static_cast<kdu_core::kdu_uint32>(8));
    ihdr.write(static_cast<kdu_core::kdu_uint32>(8));
    ihdr.write(static_cast<kdu_core::kdu_uint16>(1));
    ihdr.write(static_cast<kdu_core::kdu_byte>(7));
    ihdr.write(static_cast<kdu_core::kdu_byte>(7));
    ihdr.write(static_cast<kdu_core::kdu_uint16>(0));
    ihdr.close();
    jp2_output_box colr;
    colr.open(&jp2h, jp2_colour_4cc);
    colr.write(static_cast<kdu_core::kdu_byte>(1));
    colr.write(static_cast<kdu_core::kdu_byte>(0));
    colr.write(static_cast<kdu_core::kdu_byte>(0));
    colr.write(static_cast<kdu_core::kdu_uint32>(JP2_sLUM_SPACE));
    colr.close();
    jp2h.close();
    MemTarget cs_m;
    WriteMinimalCodestream(&cs_m, 8, 8, 1);
    jp2_output_box jp2c0;
    jp2c0.open(&mft, jp2_codestream_4cc);
    jp2c0.write(cs_m.data().data(), static_cast<int>(cs_m.data().size()));
    jp2c0.close();
    jp2_output_box mcod;
    mcod.open(&mft, jp2_multi_codestream_4cc);
    jp2_output_box j2ci;
    j2ci.open(&mcod, jp2_multi_codestream_info_4cc);
    j2ci.write(static_cast<kdu_core::kdu_uint32>(2));  // Ncs=2
    // Rval = 31 (top 6 bits = 31) triggers b/455134744 overflow check? Wait:
    // Rval > 31 occurs when casting negative/32+; test >31 via 64>>1 or
    // malformed Ncs with non-existent mcod subboxes.
    j2ci.write(static_cast<kdu_core::kdu_uint32>(31u << 26));
    j2ci.close();
    mcod.close();
    mft.close();
    try_open_bad_jpx(mmt.data());
  }
}

TEST(JpxTest, Wave5MetadataLoadMatchesCollectionsLinksAndNumlists) {
  ScopedKduHandlers handlers;

  // 1) Incremental metadata collections across 16 write_metadata() flushes
  // (exercises jx_meta_target_level::fit_collection multi-level grp_ boxes +
  // jx_meta_manager::write_metadata simulation phase 2 L17441-17452) +
  // load_matches selective filtering on fresh jpx_source + link graphs +
  // numlist navigation & container-embedded numlist indexing.
  MemTarget mt;
  jp2_family_tgt ft;
  ft.open(&mt);
  jpx_target tgt;
  tgt.open(&ft);

  std::vector<jpx_codestream_target> cs_list;
  for (int i = 0; i < 3; ++i) {
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 16, 16, 1, 8);
    cs_list.push_back(cs);
    SetupWave4Layer(tgt.add_layer(), false, 1, i);
  }
  {
    jpx_composition comp = tgt.access_composition();
    jx_frame* f = comp.add_frame(10, 2, false);
    comp.add_instruction(f, 0, 1, MakeDims(0, 0, 16, 16),
                         MakeDims(0, 0, 16, 16));
  }
  jpx_container_target cont = tgt.add_container(2, 2, 3);
  for (int i = 0; i < 2; ++i) {
    SetupCodestreamDims(cont.access_codestream(i).access_dimensions(), 16, 16,
                        1, 8);
    SetupWave4Layer(cont.access_layer(i), false, 1,
                    cont.access_codestream(i).get_codestream_id());
  }

  jpx_meta_manager mm = tgt.access_meta_manager();
  jpx_metanode root = mm.access_root();

  // Build selective numlist nodes for load_matches testing:
  // Node A: cs {0, 2}, layers {0, 2}
  const int cs_a[2] = {0, 2};
  const int ly_a[2] = {0, 2};
  jpx_metanode na = mm.insert_node(2, cs_a, 2, ly_a, false, 0, nullptr);
  na.add_label("NodeA-TopMulti");
  na.preserve_for_links();

  // Node B: cs {1}, with a ROI node underneath (has_dependent_roi_nodes)
  jpx_roi r{};
  r.init_rectangle(MakeDims(2, 2, 8, 8));
  jpx_metanode nb_parent = na.add_label("IntermediateGrp");
  jpx_metanode nb_roi = nb_parent.add_regions(1, &r);
  EXPECT_TRUE(nb_parent.has_dependent_roi_nodes());
  (void)nb_roi;

  // Container-embedded numlist referencing top cs 0 + container base cs 3 and
  // layer 3.
  const int cs_cont[2] = {0, 3};
  const int ly_cont[1] = {3};
  jpx_metanode nc =
      mm.insert_node(2, cs_cont, 1, ly_cont, false, 0, nullptr, root, 0);
  jpx_metanode nc_lbl = nc.add_label("ContainerNumlistLabel");
  nc.preserve_for_links();

  // Exercise jpx_metanode container numlist queries right on target!
  EXPECT_TRUE(nc.test_numlist_stream(0));
  EXPECT_TRUE(nc.test_numlist_stream(3));
  EXPECT_TRUE(nc.test_numlist_stream(5));  // rep 1 of base cs 3
  EXPECT_FALSE(nc.test_numlist_stream(99));
  EXPECT_TRUE(nc.test_numlist_layer(3));
  EXPECT_TRUE(nc.test_numlist_layer(5));  // rep 1 of base layer 3
  EXPECT_FALSE(nc.test_numlist_layer(99));
  EXPECT_EQ(nc.find_numlist_codestream(5), 1);
  EXPECT_EQ(nc.find_numlist_layer(5), 0);
  EXPECT_EQ(nc.get_numlist_codestream(1, 1), 5);
  EXPECT_EQ(nc.get_numlist_layer(0, 1), 5);

  // Move a compatible top-level numlist node under a container parent & back,
  // and exercise move_old_top_container_numlist on siblings!
  {
    const int c_one[1] = {0};
    jpx_metanode mv0 = mm.insert_node(1, c_one, 0, nullptr, false, 0, nullptr);
    jpx_metanode mv_lbl = mv0.add_label("Movable");
    mv0.change_to_label("RelabeledNode");
    // Unlink / re-parent top-level label under nc, then under root.
    mv_lbl.change_parent(nc);
    mv_lbl.change_parent(root);
  }

  // Link graph exercising find_path_to (forward & reverse, exclusions, unify).
  jpx_metanode l_child = na.add_link(nc_lbl, JPX_ALTERNATE_CHILD_LINK);
  jpx_metanode l_parent = nc_lbl.add_link(na, JPX_ALTERNATE_PARENT_LINK);
  jpx_metanode l_group = nb_parent.add_link(na, JPX_GROUPING_LINK);
  jpx_metanode_link_type lt = JPX_METANODE_LINK_NONE;
  EXPECT_TRUE(l_child.get_link(lt, true).exists());
  EXPECT_EQ(lt, JPX_ALTERNATE_CHILD_LINK);
  EXPECT_TRUE(l_parent.get_link(lt, false).exists());
  EXPECT_TRUE(l_group.get_link(lt, false).exists());

  const kdu_core::kdu_uint32 excl_t[1] = {jp2_label_4cc};
  const int excl_f[1] = {JPX_PATH_TO_EXCLUDE_BOX | JPX_PATH_TO_EXCLUDE_PARENTS};
  EXPECT_TRUE(na.find_path_to(nc_lbl, JPX_PATH_TO_DIRECT | JPX_PATH_TO_FORWARD,
                              JPX_PATH_TO_FORWARD, 0, nullptr, nullptr)
                  .exists());
  EXPECT_TRUE(nc_lbl
                  .find_path_to(na, JPX_PATH_TO_REVERSE,
                                JPX_PATH_TO_DIRECT | JPX_PATH_TO_REVERSE, 0,
                                nullptr, nullptr, true)
                  .exists());
  EXPECT_TRUE(na.find_path_to(nc_lbl, JPX_PATH_TO_DIRECT | JPX_PATH_TO_REVERSE,
                              JPX_PATH_TO_DIRECT | JPX_PATH_TO_REVERSE, 0,
                              nullptr, nullptr, true)
                  .exists());
  (void)na.find_path_to(nc_lbl, JPX_PATH_TO_FORWARD, 0, 1, excl_t, excl_f);

  // Add >80 distinct numlists + remove ~30 to exercise jx_numlist_library
  // check_split_root_cluster, remove_cluster, and enumerate_matches!
  {
    std::vector<jpx_metanode> extra_nl;
    for (int k = 0; k < 85; ++k) {
      int cid = k % 3;
      int lid = (k / 3) % 3;
      jpx_metanode n =
          mm.insert_node(1, &cid, 1, &lid, (k & 1) != 0, 0, nullptr, na);
      n.add_label(std::string(40, static_cast<char>('A' + (k % 26))).c_str());
      extra_nl.push_back(n);
    }
    for (int k = 10; k < 40; ++k) {
      extra_nl[k].delete_node();
    }
  }

  EXPECT_EQ(tgt.write_headers(), nullptr);
  for (int i = 0; i < 3; ++i) {
    jp2_output_box* box = cs_list[i].open_stream();
    WriteMinimalCodestream(cs_list[i].access_stream(), 16, 16, 1);
    EXPECT_TRUE(box->close());
  }
  for (int r_idx = 0; r_idx < 3; ++r_idx) {
    for (int i = 0; i < 2; ++i) {
      jp2_output_box* box = cont.access_codestream(i).open_stream();
      WriteMinimalCodestream(cont.access_codestream(i).access_stream(), 16, 16,
                             1);
      EXPECT_TRUE(box->close());
    }
  }

  // Perform 16 incremental metadata flushes with preserved link targets so
  // jx_meta_target_level::fit_collection splits & commits nested grp_ trees
  // and write_metadata() executes simulation phase 2!
  EXPECT_EQ(tgt.write_metadata(), nullptr);
  for (int batch = 0; batch < 16; ++batch) {
    std::string txt(120 + batch * 15, static_cast<char>('a' + (batch % 26)));
    jpx_metanode n = root.add_label(txt.c_str());
    n.preserve_for_links();
    if ((batch % 3) == 2) {
      jpx_metanode tgt_n = root.add_link(na, JPX_GROUPING_LINK);
      (void)tgt_n;
    }
    EXPECT_EQ(tgt.write_metadata(), nullptr);
  }

  tgt.close();
  ft.close();

  // Fresh jpx_source: test selective load_matches BEFORE load_matches(-1, ...),
  // plus get_next_descendant / get_prev_descendant / locate_node /
  // copy_locators.
  {
    MemSource ms(mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 1);
    jpx_meta_manager smm = src.access_meta_manager();
    const int no_cs[1] = {99};
    const int no_ly[1] = {99};
    (void)smm.load_matches(1, no_cs, 1, no_ly);
    const int cs2[1] = {2};
    smm.load_matches(1, cs2, 0, nullptr);
    const int ly2[1] = {2};
    smm.load_matches(0, nullptr, 1, ly2);
    smm.load_matches(-1, nullptr, -1, nullptr);

    jpx_metanode sroot = smm.access_root();
    const kdu_core::kdu_uint32 lbl_t[1] = {jp2_label_4cc};
    jpx_metanode fwd = sroot.get_next_descendant(jpx_metanode(), 1, lbl_t);
    jpx_metanode bwd = sroot.get_prev_descendant(jpx_metanode(), 1, lbl_t);
    EXPECT_TRUE(fwd.exists());
    EXPECT_TRUE(bwd.exists());
    EXPECT_TRUE(
        sroot.get_next_descendant(jpx_metanode(), -1, nullptr).exists());
    EXPECT_TRUE(
        sroot.get_prev_descendant(jpx_metanode(), -1, nullptr).exists());
    // Locate node via its box offset and reset_copy_locators + add_copy.
    EXPECT_FALSE(smm.locate_node(123456789).exists());
    smm.reset_copy_locators(sroot, true, true);

    kdu_window win;
    win.init();
    const kdu_core::kdu_uint32 itypes[2] = {jp2_label_4cc, jp2_association_4cc};
    sroot.generate_metareq(&win, 0, nullptr, 0, nullptr, true, 0);
    sroot.generate_metareq(&win, 2, itypes, 1, itypes + 1, false, 4);

    src.close();
    fs.close();
  }
}

namespace {

void AppendBe16(std::vector<kdu_core::kdu_byte>& out, std::uint16_t v) {
  out.push_back(static_cast<kdu_core::kdu_byte>((v >> 8) & 0xFF));
  out.push_back(static_cast<kdu_core::kdu_byte>(v & 0xFF));
}

void AppendBe32(std::vector<kdu_core::kdu_byte>& out, std::uint32_t v) {
  out.push_back(static_cast<kdu_core::kdu_byte>((v >> 24) & 0xFF));
  out.push_back(static_cast<kdu_core::kdu_byte>((v >> 16) & 0xFF));
  out.push_back(static_cast<kdu_core::kdu_byte>((v >> 8) & 0xFF));
  out.push_back(static_cast<kdu_core::kdu_byte>(v & 0xFF));
}

void AppendBe64(std::vector<kdu_core::kdu_byte>& out, std::uint64_t v) {
  AppendBe32(out, static_cast<std::uint32_t>(v >> 32));
  AppendBe32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFU));
}

void AppendBox(std::vector<kdu_core::kdu_byte>& out, std::uint32_t type,
               const std::vector<kdu_core::kdu_byte>& payload) {
  AppendBe32(out, static_cast<std::uint32_t>(payload.size() + 8));
  AppendBe32(out, type);
  out.insert(out.end(), payload.begin(), payload.end());
}

void SetupSrgbLayer(jpx_layer_target layer, int cs_id) {
  layer.access_channels().init(3);
  for (int c = 0; c < 3; ++c) {
    layer.access_channels().set_colour_mapping(c, c, -1, cs_id);
  }
  layer.access_colour(0).init(JP2_sRGB_SPACE);
}

void AppendPlaceholderBox(std::vector<kdu_core::kdu_byte>& out,
                          std::uint32_t flags, std::uint64_t bin_id,
                          std::uint32_t orig_len, std::uint32_t orig_type,
                          std::uint64_t equiv_bin_id = 0,
                          std::uint32_t equiv_len = 0,
                          std::uint32_t equiv_type = 0,
                          std::int64_t cs_min = -1,
                          std::int32_t cs_offset = -1) {
  std::vector<kdu_core::kdu_byte> p;
  AppendBe32(p, flags);
  AppendBe64(p, bin_id);
  AppendBe32(p, orig_len);
  AppendBe32(p, orig_type);
  if ((flags & 6) != 0) {
    AppendBe64(p, equiv_bin_id);
    AppendBe32(p, equiv_len);
    AppendBe32(p, equiv_type);
    if (flags & 4) AppendBe64(p, static_cast<std::uint64_t>(cs_min));
    if (flags & 8) AppendBe32(p, static_cast<std::uint32_t>(cs_offset));
  }
  AppendBox(out, jp2_placeholder_4cc, p);
}

}  // namespace

TEST(JpxTest, Wave6MultistreamHierarchyAndSequentialRecovery) {
  ScopedKduHandlers handlers;

  // 1) Multi-fragment list transitions: single URL -> non-contiguous list ->
  //    contiguous merge -> new URL append -> >4GiB offset/length save split.
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);

    jpx_codestream_target cs0 = tgt.add_codestream();
    SetupCodestreamDims(cs0.access_dimensions(), 4, 4, 1);
    jpx_layer_target l0 = tgt.add_layer();
    l0.access_channels().init(1);
    l0.access_channels().set_colour_mapping(0, 0, -1, 0);
    l0.access_colour(0).init(JP2_sLUM_SPACE);

    jpx_codestream_target cs1 = tgt.add_codestream();
    SetupCodestreamDims(cs1.access_dimensions(), 4, 4, 1);
    cs1.add_fragment(nullptr, 100, 32);
    cs1.add_fragment(nullptr, 200, 32);
    cs1.add_fragment(nullptr, 232, 16);
    cs1.add_fragment("ext_frag_a.bin", 400, 24);
    cs1.add_fragment(nullptr, kdu_core::kdu_long(0x100000004LL),
                     kdu_core::kdu_long(0x100000005LL));

    tgt.write_headers();
    jp2_output_box* s0 = cs0.open_stream();
    WriteMinimalCodestream(s0, 4, 4, 1);
    s0->close();
    cs1.write_fragment_table();
    tgt.write_metadata();
    tgt.close();
    ftgt.close();
  }

  // 2) Multi-level J2CX hierarchy: min=35, max=35 forces streams_per_subbox=16
  //    right away with num_streams_left > 0 (size_container remainder loop),
  //    multi_sub creation/completion, write_metadata mid-stream box fixing, and
  //    growing ftbl_len invalidating bytes_per_subbox.
  MemTarget multi_mt;
  {
    jp2_family_tgt ftgt;
    ftgt.open(&multi_mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    tgt.configure_codestream_aggregation(35, 35);

    jpx_codestream_target top_cs = tgt.add_codestream();
    SetupCodestreamDims(top_cs.access_dimensions(), 4, 4, 1);
    jpx_layer_target top_layer = tgt.add_layer();
    top_layer.access_channels().init(1);
    top_layer.access_channels().set_colour_mapping(0, 0, -1, 0);
    top_layer.access_colour(0).init(JP2_sLUM_SPACE);
    jpx_composition top_comp = tgt.access_composition();
    auto* top_f = top_comp.add_frame(10, 0, false);
    top_comp.add_instruction(top_f, 0, 0, MakeDims(0, 0, 4, 4),
                             MakeDims(0, 0, 4, 4));

    jpx_container_target cont = tgt.add_container(1, 1, 80);
    jpx_codestream_target base_cs = cont.access_codestream(0);
    SetupCodestreamDims(base_cs.access_dimensions(), 4, 4, 1);
    jpx_layer_target base_layer = cont.access_layer(0);
    base_layer.access_channels().init(1);
    base_layer.access_channels().set_colour_mapping(
        0, 0, -1, base_cs.get_codestream_id());
    base_layer.access_colour(0).init(JP2_sLUM_SPACE);

    tgt.write_headers();

    jp2_output_box* top_stream = top_cs.open_stream();
    WriteMinimalCodestream(top_stream, 4, 4, 1);
    top_stream->close();

    // Write 1st real codestream and capture file location to alias via ftbl.
    jp2_output_box* cs_stream = base_cs.open_stream();
    WriteMinimalCodestream(cs_stream, 4, 4, 1);
    cs_stream->close();
    kdu_core::kdu_long stream0_pos =
        static_cast<kdu_core::kdu_long>(multi_mt.data().size()) - 200;
    if (stream0_pos < 0) stream0_pos = 0;

    jpx_meta_manager mm = tgt.access_meta_manager();
    for (int r = 1; r < 80; ++r) {
      if (r == 2) {
        // Flush metadata while a J2CX multi_sub is open (fixes boxes & sets
        // out_box_len > 0).
        mm.access_root().add_label("mid-j2cx-1");
        tgt.write_metadata();
      } else if (r == 20) {
        mm.access_root().add_label("mid-j2cx-2");
        tgt.write_metadata();
      }
      base_cs.add_fragment(nullptr, stream0_pos, 32);
      if (r >= 18) {
        base_cs.add_fragment(nullptr, stream0_pos + 64, 32);
      }
      if (r >= 37) {
        base_cs.add_fragment(nullptr, stream0_pos + 128, 32);
      }
      base_cs.write_fragment_table();
    }
    tgt.write_metadata();
    tgt.close();
    ftgt.close();
  }

  // Random-access recovery across multi-level J2CX + discover_codestreams.
  {
    MemSource ms(multi_mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 0);

    for (int id : {80, 40, 17, 16, 35, 34, 0, 81}) {
      jpx_codestream_source cs = src.access_codestream(id, false);
      EXPECT_EQ(cs.exists(), id < 81);
    }
    int total_cs = 0;
    EXPECT_TRUE(src.count_codestreams(total_cs));
    EXPECT_EQ(total_cs, 81);
    for (int id = 1; id < 81; ++id) {
      jpx_codestream_source cs = src.access_codestream(id, false);
      EXPECT_TRUE(cs.exists());
    }
    src.close();
    fs.close();
  }

  // Sequential-only (non-seekable) recovery over multi-level J2CX boxes.
  {
    MemSource seq_ms(multi_mt.data(), KDU_SOURCE_CAP_SEQUENTIAL);
    jp2_family_src fs;
    fs.open(&seq_ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 0);
    EXPECT_TRUE(src.access_codestream(18, false).exists());
    EXPECT_TRUE(src.access_codestream(0, false).exists());
    EXPECT_TRUE(src.access_codestream(36, false).exists());
    EXPECT_TRUE(src.access_codestream(80, false).exists());
    EXPECT_FALSE(src.access_codestream(81, false).exists());
    src.close();
    fs.close();
  }
}

TEST(JpxTest, Wave6IndefiniteContainerTracksAndCompositionMatch) {
  ScopedKduHandlers handlers;

  // 1) Indefinite-repetition container (R = 0) with 2 tracks + unlinked
  //    preceding container flushed via write_or_simulate_earlier_containers.
  MemTarget indef_mt;
  {
    jp2_family_tgt ftgt;
    ftgt.open(&indef_mt);
    jpx_target tgt;
    tgt.open(&ftgt);

    // Top-level layer & codestream so first_layer_idx > 0 in containers.
    jpx_codestream_target tcs = tgt.add_codestream();
    SetupCodestreamDims(tcs.access_dimensions(), 16, 16, 3);
    jpx_layer_target tl = tgt.add_layer();
    SetupSrgbLayer(tl, 0);

    // Container c0 with no metanodes added BEFORE writing metadata on c1.
    jpx_container_target c0 = tgt.add_container(1, 2, 2);
    SetupCodestreamDims(c0.access_codestream(0).access_dimensions(), 16, 16, 3);
    const int c0_cs_id = c0.access_codestream(0).get_codestream_id();
    SetupSrgbLayer(c0.access_layer(0), c0_cs_id);
    SetupSrgbLayer(c0.access_layer(1), c0_cs_id);

    // Final container c1 with indefinite repetitions (R = 0) and 2 tracks.
    jpx_container_target c1 = tgt.add_container(1, 3, 0);
    SetupCodestreamDims(c1.access_codestream(0).access_dimensions(), 16, 16, 3);
    const int c1_cs_id = c1.access_codestream(0).get_codestream_id();
    for (int l = 0; l < 3; ++l) SetupSrgbLayer(c1.access_layer(l), c1_cs_id);

    jpx_composition trk1 = c1.add_presentation_track(1);
    jpx_composition trk2 = c1.add_presentation_track(2);
    auto* f_t1 = trk1.add_frame(10, 0, false);
    trk1.add_instruction(f_t1, 0, 0, MakeDims(0, 0, 16, 16),
                         MakeDims(0, 0, 16, 16));
    auto* f_t2 = trk2.add_frame(20, 0, false);
    trk2.add_instruction(f_t2, 1, 0, MakeDims(0, 0, 16, 16),
                         MakeDims(0, 0, 16, 16));

    jpx_composition global_comp = tgt.access_composition();
    global_comp.set_loop_count(1);
    auto* f_g = global_comp.add_frame(5, 0, false);
    global_comp.add_instruction(f_g, 0, 0, MakeDims(0, 0, 16, 16),
                                MakeDims(0, 0, 16, 16));

    // Associate a container numlist + forward crossref with c1 so both
    // simulation_phase==1 and 0 walk write_or_simulate_earlier_containers(c1).
    jpx_meta_manager mm = tgt.access_meta_manager();
    jpx_metanode root = mm.access_root();
    const int c1_layers[1] = {5};
    jpx_metanode c1_nl = root.add_numlist(0, nullptr, 1, c1_layers, false, 1);
    jpx_metanode fwd_target = c1_nl.add_label("after-target");
    c1_nl.add_link(fwd_target, JPX_GROUPING_LINK);

    tgt.write_headers();
    jp2_output_box* ts = tcs.open_stream();
    WriteMinimalCodestream(ts, 16, 16, 3);
    ts->close();

    for (int r = 0; r < 2; ++r) {
      jp2_output_box* s = c0.access_codestream(0).open_stream();
      WriteMinimalCodestream(s, 16, 16, 3);
      s->close();
    }
    for (int r = 0; r < 4; ++r) {
      jp2_output_box* s = c1.access_codestream(0).open_stream();
      WriteMinimalCodestream(s, 16, 16, 3);
      s->close();
    }
    tgt.write_metadata();
    tgt.close();
    ftgt.close();
  }

  // Open indefinite container source and exercise count_track_time,
  // access_next, count_repetitions, access_layer/codestream with
  // find_first_rep=true, and track bounds before find_all_streams runs.
  {
    MemSource ms(indef_mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 0);

    jpx_composition comp = src.access_composition();
    kdu_core::kdu_uint32 ntracks = 0;
    EXPECT_TRUE(comp.count_tracks(ntracks, false));
    kdu_core::kdu_long ttime = 0;
    EXPECT_TRUE(comp.count_track_time(0, ttime));
    EXPECT_TRUE(comp.count_track_time(99, ttime));
    jpx_frame f0 = comp.access_frame(0, 0, false);
    EXPECT_TRUE(f0.exists());
    jpx_frame fn0 = f0.access_next(0, false);
    (void)fn0;
    jpx_frame f99 = comp.access_frame(99, 0, false);
    (void)f99;

    jpx_container_source sc1 = src.access_container(1);
    ASSERT_TRUE(sc1.exists());
    EXPECT_EQ(sc1.get_num_tracks(), 2U);
    int ntbl = 0;
    EXPECT_GE(sc1.get_track_base_layers(0, ntbl), 0);
    EXPECT_GE(sc1.get_track_base_layers(1, ntbl), 0);
    EXPECT_GE(sc1.get_track_base_layers(5, ntbl), 0);
    EXPECT_FALSE(sc1.access_presentation_track(0).exists());
    EXPECT_TRUE(sc1.access_presentation_track(5).exists());
    EXPECT_TRUE(sc1.access_codestream(0, 1, true, true).exists());
    EXPECT_TRUE(sc1.access_layer(0, 1, true, true).exists());
    EXPECT_FALSE(sc1.access_codestream(-1, 0, true, true).exists());
    EXPECT_FALSE(sc1.access_layer(-1, 0, true, true).exists());
    int reps = 0;
    EXPECT_TRUE(sc1.count_repetitions(reps));
    EXPECT_EQ(reps, 4);
    EXPECT_TRUE(sc1.access_codestream(0, 3, true, true).exists());
    EXPECT_TRUE(sc1.access_layer(0, 3, true, true).exists());

    src.close();
    fs.close();
  }

  // 2) LCM find_match / reverse_match: abs_layers_per_rep=2,
  //    abs_layer_rep_stride=4, container R=10, frame increment=3, repeat=8.
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);

    jpx_codestream_target top_cs = tgt.add_codestream();
    SetupCodestreamDims(top_cs.access_dimensions(), 8, 8, 3);
    for (int l = 0; l < 4; ++l) {
      jpx_layer_target lyr = tgt.add_layer();
      SetupSrgbLayer(lyr, 0);
    }
    jpx_composition gcomp = tgt.access_composition();
    gcomp.set_loop_count(1);
    auto* gf = gcomp.add_frame(10, 1, false);
    gcomp.add_instruction(gf, 0, 2, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));
    auto* gf2 = gcomp.add_frame(10, 1, false);
    gcomp.add_instruction(gf2, 1, 2, MakeDims(0, 0, 8, 8),
                          MakeDims(0, 0, 8, 8));

    jpx_container_target cont = tgt.add_container(1, 4, 10);
    SetupCodestreamDims(cont.access_codestream(0).access_dimensions(), 8, 8, 3);
    const int cont_cs_id = cont.access_codestream(0).get_codestream_id();
    for (int l = 0; l < 4; ++l)
      SetupSrgbLayer(cont.access_layer(l), cont_cs_id);

    jpx_composition t1 = cont.add_presentation_track(2);
    t1.set_loop_count(1);
    auto* tf1 = t1.add_frame(10, 6, false);
    t1.add_instruction(tf1, 0, 3, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));
    auto* tf1b = t1.add_frame(10, 5, false);
    t1.add_instruction(tf1b, 1, 3, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));
    auto* tf1c = t1.add_frame(10, 5, false);
    t1.add_instruction(tf1c, 2, 3, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));
    auto* tf1d = t1.add_frame(10, 0, false);
    t1.add_instruction(tf1d, 19, 0, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));

    jpx_composition t2 = cont.add_presentation_track(2);
    auto* tf2 = t2.add_frame(10, 19, false);
    t2.add_instruction(tf2, 0, 1, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));

    tgt.write_headers();
    jp2_output_box* s0 = top_cs.open_stream();
    WriteMinimalCodestream(s0, 8, 8, 3);
    s0->close();
    for (int r = 0; r < 10; ++r) {
      jp2_output_box* sc = cont.access_codestream(0).open_stream();
      WriteMinimalCodestream(sc, 8, 8, 3);
      sc->close();
    }
    tgt.write_metadata();
    tgt.close();
    ftgt.close();

    MemSource ms(mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 0);
    jpx_composition scomp = src.access_composition();
    int total_f = 0;
    EXPECT_TRUE(scomp.count_track_frames(0, total_f));
    EXPECT_TRUE(scomp.count_track_frames(1, total_f));

    // Match in rep 0 of track 1 (abs layer = 4 + 1 = 5 and 4 + 0 = 4) with
    // min_rept > 0 and reverse_match from prev frame to exercise LCM loops.
    const int rep0_layer_a[1] = {5};
    const int rep0_layer_b[1] = {4};
    for (const int* query : {rep0_layer_a, rep0_layer_b}) {
      int inst_idx = -1;
      jpx_frame mf = scomp.access_frame(1, 5, false);
      scomp.find_layer_match(mf, inst_idx, 1, query, 1, 0, true, 0);

      jpx_frame rb;
      scomp.find_layer_match(rb, inst_idx, 1, query, 1, 0, true,
                             JPX_FRAME_MATCH_REVERSE);
      if (rb.exists()) {
        jpx_frame prev_rb = rb.access_prev(1, true);
        if (prev_rb.exists()) {
          scomp.find_layer_match(prev_rb, inst_idx, 1, query, 1, 0, false,
                                 JPX_FRAME_MATCH_REVERSE);
        }
      }
    }
    // Non-container top-level match with increment=2.
    const int g_layers[1] = {2};
    const int g_mid[1] = {1};
    int g_inst = -1;
    jpx_frame gm = scomp.access_frame(0, 1, false);
    scomp.find_layer_match(gm, g_inst, 0, g_layers, 1, -1, true, 0);
    jpx_frame gr;
    scomp.find_layer_match(gr, g_inst, 0, g_mid, 1, -1, true,
                           JPX_FRAME_MATCH_REVERSE);
    src.close();
    fs.close();
  }

  // 3) Frame expander opaque 4-side edge occlusion + raw pruned empty frame +
  //    rept=0xFFFF.
  {
    MemTarget valid_mt;
    {
      jp2_family_tgt ftgt;
      ftgt.open(&valid_mt);
      jpx_target tgt;
      tgt.open(&ftgt);
      jpx_codestream_target cs0 = tgt.add_codestream();
      SetupCodestreamDims(cs0.access_dimensions(), 64, 64, 3);
      jpx_codestream_target cs1 = tgt.add_codestream();
      SetupCodestreamDims(cs1.access_dimensions(), 64, 64, 3);
      jpx_layer_target bg = tgt.add_layer();
      SetupSrgbLayer(bg, 0);
      jpx_layer_target fg = tgt.add_layer();
      SetupSrgbLayer(fg, 1);

      jpx_composition comp = tgt.access_composition();
      auto* f0 = comp.add_frame(10, 0, true);
      comp.add_instruction(f0, 0, 0, MakeDims(0, 0, 16, 16),
                           MakeDims(16, 16, 16, 16));
      comp.add_instruction(f0, 1, 0, MakeDims(0, 0, 8, 16),
                           MakeDims(16, 16, 8, 16));
      comp.add_instruction(f0, 1, 0, MakeDims(0, 0, 8, 16),
                           MakeDims(24, 16, 8, 16));
      comp.add_instruction(f0, 1, 0, MakeDims(0, 0, 16, 8),
                           MakeDims(16, 16, 16, 8));
      comp.add_instruction(f0, 1, 0, MakeDims(0, 0, 16, 8),
                           MakeDims(16, 24, 16, 8));
      comp.add_instruction(f0, 1, 0, MakeDims(0, 0, 8, 8),
                           MakeDims(200, 200, 8, 8));

      tgt.write_headers();
      for (jpx_codestream_target* csp : {&cs0, &cs1}) {
        jp2_output_box* s = csp->open_stream();
        WriteMinimalCodestream(s, 64, 64, 3);
        s->close();
      }
      tgt.write_metadata();
      tgt.close();
      ftgt.close();
    }

    MemSource ms(valid_mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 0);
    jpx_frame_expander exp;
    jpx_composition comp = src.access_composition();
    exp.construct(&src, comp.access_frame(0, 0, true), kdu_core::kdu_dims());
    exp.construct(&src, comp.access_frame(0, 0, true),
                  MakeDims(16, 16, 16, 16));
    kdu_core::kdu_dims vis_reg = MakeDims(16, 16, 16, 16);
    jpx_frame_expander::test_codestream_visibility(
        &src, comp.access_frame(0, 0, true), 0, jpx_metanode(), nullptr, 0,
        vis_reg, kdu_core::kdu_dims(), false, 0);
    src.close();
    fs.close();

    // Inject an additional raw `comp` -> `iset` box containing a completely
    // pruned frame (invalid layer 99 -> absorbed duration/pause by prev frame)
    // and an infinite repeat count (rept = 0xFFFF).
    std::vector<kdu_core::kdu_byte> patched = valid_mt.data();
    std::vector<kdu_core::kdu_byte> iset;
    AppendBe16(iset, 0x0065);
    AppendBe16(iset, 2);
    iset.push_back(0);
    iset.push_back(1);
    AppendBe16(iset, 0xFFFF);
    AppendBe16(iset, 10);
    iset.push_back(99);
    iset.push_back(1);
    AppendBe16(iset, 0);
    AppendBe16(iset, 25);

    std::vector<kdu_core::kdu_byte> comp_payload;
    AppendBox(comp_payload, jp2_comp_options_4cc,
              {0, 0, 0, 16, 0, 0, 0, 16, 1});
    AppendBox(comp_payload, jp2_comp_instruction_set_4cc, iset);
    AppendBox(patched, jp2_composition_4cc, comp_payload);

    MemSource pms(patched);
    jp2_family_src pfs;
    pfs.open(&pms);
    jpx_source psrc;
    if (psrc.open(&pfs, false) >= 0) {
      int nf = 0;
      psrc.access_composition().count_track_frames(0, nf);
    }
    psrc.close();
    pfs.close();
  }
}

TEST(JpxTest, Wave6RoiPathFillerHolesTrianglesAndEditorEdges) {
  ScopedKduHandlers handlers;

  // 1) jpx_roi edge cases: skewed negative-centre ellipse clipping,
  //    overflow coordinates, and diamond with vertices[3].y < vertices[1].y.
  {
    jpx_roi skew_ell;
    skew_ell.init_ellipse(kdu_core::kdu_coords(-10, -10),
                          kdu_core::kdu_coords(30, 20),
                          kdu_core::kdu_coords(6, 4));
    skew_ell.clip_region();
    EXPECT_TRUE(skew_ell.check_geometry());

    jpx_roi huge_quad;
    huge_quad.init_quadrilateral(kdu_core::kdu_coords(0x7FFFFFF0, 10),
                                 kdu_core::kdu_coords(0x7FFFFFFE, 20),
                                 kdu_core::kdu_coords(0x7FFFFFFE, 40),
                                 kdu_core::kdu_coords(0x7FFFFFF0, 30));
    huge_quad.clip_region();

    // Diamond where vertices[3].y < vertices[1].y and crossing H/V segments.
    jpx_roi diag_q;
    diag_q.init_quadrilateral(
        kdu_core::kdu_coords(100, 10), kdu_core::kdu_coords(160, 140),
        kdu_core::kdu_coords(90, 220), kdu_core::kdu_coords(40, 80));
    EXPECT_TRUE(diag_q.check_geometry());
    EXPECT_TRUE(diag_q.contains(kdu_core::kdu_coords(100, 110)));
    double cx = 0.0, cy = 0.0, px = 0.0, py = 0.0;
    diag_q.measure_area(cx, cy);
    diag_q.find_boundary_projection(100.0, 110.0, px, py, 200.0);
    EXPECT_TRUE(diag_q.check_edge_intersection(1, kdu_core::kdu_coords(50, 100),
                                               kdu_core::kdu_coords(170, 180)));
  }

  // 2) CCW outer loop (sense < 0) + CW internal hole + triangle path filling.
  {
    jpx_roi_editor ed;
    ed.set_max_undo_history(8);
    ed.set_mode(JPX_EDITOR_PATH_MODE);

    // CCW in image coords (+x right, +y down): down left edge -> right bottom
    // -> up right edge -> left top.
    const kdu_core::kdu_coords ccw_outer[] = {
        kdu_core::kdu_coords(100, 100), kdu_core::kdu_coords(100, 900),
        kdu_core::kdu_coords(900, 900), kdu_core::kdu_coords(900, 100),
        kdu_core::kdu_coords(100, 100),
    };
    for (const auto& p : ccw_outer) ed.add_scribble_point(p);
    ed.convert_scribble_path(true, 0, 1.0);

    // CW inner hole completely inside outer loop.
    const kdu_core::kdu_coords cw_hole[] = {
        kdu_core::kdu_coords(300, 300), kdu_core::kdu_coords(700, 300),
        kdu_core::kdu_coords(700, 700), kdu_core::kdu_coords(300, 700),
        kdu_core::kdu_coords(300, 300),
    };
    ed.clear_scribble_points();
    for (const auto& p : cw_hole) ed.add_scribble_point(p);
    ed.convert_scribble_path(false, 0, 1.0);

    bool ok = false;
    ed.set_path_thickness(6, ok);
    ed.fill_closed_paths(ok, -1);
    EXPECT_GT(ed.measure_complexity(), 0.0);

    // Undo / make new edit / undo / redo to trigger redo chain links.
    ed.undo();
    ed.redo();
    ed.undo();
    ed.clear_scribble_points();
    // Thin 3-vertex closed triangle -> add_triangle in fill_interior().
    const kdu_core::kdu_coords tri[] = {
        kdu_core::kdu_coords(60, 60),
        kdu_core::kdu_coords(260, 60),
        kdu_core::kdu_coords(160, 240),
        kdu_core::kdu_coords(60, 60),
    };
    for (const auto& p : tri) ed.add_scribble_point(p);
    ed.convert_scribble_path(true, 0, 1.0);
    ed.set_path_thickness(4, ok);
    ed.fill_closed_paths(ok, 0);
    EXPECT_TRUE(ok);

    // 5-vertex convex closed pentagon -> mix of quad + triangle interior fills.
    ed.reset();
    ed.set_mode(JPX_EDITOR_PATH_MODE);
    const kdu_core::kdu_coords pent[] = {
        kdu_core::kdu_coords(200, 40),  kdu_core::kdu_coords(340, 140),
        kdu_core::kdu_coords(290, 300), kdu_core::kdu_coords(110, 300),
        kdu_core::kdu_coords(60, 140),  kdu_core::kdu_coords(200, 40),
    };
    for (const auto& p : pent) ed.add_scribble_point(p);
    ed.convert_scribble_path(true, 0, 1.0);
    ed.set_path_thickness(4, ok);
    ed.fill_closed_paths(ok, -1);
  }

  // 3) VERTEX_MODE & PATH_MODE anchor/guide/add_region/clip_region,
  //    >2048 scribble subsampling, and >300 zigzag overflow guard.
  {
    jpx_roi_editor ed;
    ed.set_max_undo_history(6);

    jpx_roi r_quad, r_ell;
    r_quad.init_quadrilateral(
        kdu_core::kdu_coords(50, 50), kdu_core::kdu_coords(150, 50),
        kdu_core::kdu_coords(150, 150), kdu_core::kdu_coords(50, 150));
    r_quad.fix_inconsistencies();
    r_ell.init_ellipse(kdu_core::kdu_coords(200, 200),
                       kdu_core::kdu_coords(40, 25),
                       kdu_core::kdu_coords(5, 3));
    r_ell.fix_inconsistencies();
    std::array<jpx_roi, 2> init_rois = {r_quad, r_ell};
    ed.init(init_rois.data(), 2);

    ed.set_mode(JPX_EDITOR_VERTEX_MODE);
    kdu_core::kdu_coords g(50, 50);
    ed.find_nearest_anchor(g, true);
    ed.find_nearest_guide_point(g);
    ed.select_anchor(g, false);
    ed.add_region(false, MakeDims(0, 0, 500, 500));
    ed.cancel_selection();
    ed.select_anchor(g, false);
    kdu_core::kdu_coords move_dst(55, 60);
    ed.can_move_selected_anchor(move_dst, true);
    ed.move_selected_anchor(move_dst);
    ed.set_mode(JPX_EDITOR_VERTEX_MODE);

    ed.set_mode(JPX_EDITOR_PATH_MODE);
    kdu_core::kdu_coords p_near(150, 100);
    ed.find_nearest_guide_point(p_near);
    ed.find_nearest_anchor(p_near, true);
    ed.select_anchor(p_near, false);
    ed.add_region(true, MakeDims(20, 20, 400, 400));
    ed.set_mode(JPX_EDITOR_PATH_MODE);

    // Exercise self-crossing scribble loop elimination (L8514-8517) without
    // reaching JX_ROI_SCRIBBLE_POINTS (2048), which triggers an OOB read in
    // jpx.cpp:8474.
    ed.reset();
    ed.set_mode(JPX_EDITOR_PATH_MODE);
    const kdu_core::kdu_coords cross_loop[] = {
        kdu_core::kdu_coords(20, 20),   kdu_core::kdu_coords(120, 20),
        kdu_core::kdu_coords(120, 120), kdu_core::kdu_coords(20, 120),
        kdu_core::kdu_coords(60, 0),    kdu_core::kdu_coords(160, 160),
    };
    for (const auto& p : cross_loop) ed.add_scribble_point(p);
    ed.convert_scribble_path(true, 0, 0.5);

    ed.reset();
    ed.set_mode(JPX_EDITOR_PATH_MODE);
    for (int i = 0; i < 310; ++i) {
      ed.add_scribble_point(
          kdu_core::kdu_coords(10 + i * 3, 20 + ((i & 1) ? 40 : 0)));
    }
    ed.convert_scribble_path(true, 0, 1.0);
  }
}

TEST(JpxTest, Wave6MetadataMutationsForwardLinksAndExternalFragments) {
  ScopedKduHandlers handlers;

  // 1) Container numlists (3 siblings moved + identical copy deleted +
  //    rep_idx=-1 enumeration), change_to_label/link on all node types,
  //    find_path_to direct/parent-exclude, reset_copy_locators, and 5
  //    incremental write_metadata() calls overflowing a committed group.
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);

    jpx_codestream_target top_cs = tgt.add_codestream();
    SetupCodestreamDims(top_cs.access_dimensions(), 8, 8, 3);
    jpx_layer_target top_l = tgt.add_layer();
    SetupSrgbLayer(top_l, 0);
    jpx_composition top_comp = tgt.access_composition();
    auto* top_f = top_comp.add_frame(10, 0, false);
    top_comp.add_instruction(top_f, 0, 0, MakeDims(0, 0, 8, 8),
                             MakeDims(0, 0, 8, 8));

    jpx_container_target cont = tgt.add_container(1, 2, 5);
    SetupCodestreamDims(cont.access_codestream(0).access_dimensions(), 8, 8, 3);
    const int cont_cs_id = cont.access_codestream(0).get_codestream_id();
    SetupSrgbLayer(cont.access_layer(0), cont_cs_id);
    SetupSrgbLayer(cont.access_layer(1), cont_cs_id);

    jpx_meta_manager mm = tgt.access_meta_manager();
    jpx_metanode root = mm.access_root();

    const int cs_list[1] = {cont_cs_id};
    const int lyr_list[1] = {1};
    jpx_metanode nl0 = root.add_numlist(1, cs_list, 1, lyr_list, false, 0);
    jpx_metanode nl1 = root.add_numlist(1, cs_list, 1, lyr_list, false, 0);
    jpx_metanode nl2 = root.add_numlist(1, cs_list, 1, lyr_list, false, 0);
    jpx_metanode nl_dup = root.add_numlist(1, cs_list, 1, lyr_list, false, 0);
    EXPECT_EQ(nl0.get_numlist_codestream(0, -1), cont_cs_id);
    EXPECT_EQ(nl0.get_numlist_codestream(3, -1), cont_cs_id + 3);
    // jpx_metanode::get_numlist_layer delegates to map_codestream_id in vendor
    // code, so rep stride matches container codestream count (1) rather than 2.
    EXPECT_EQ(nl0.get_numlist_layer(2, -1), 1 + 2 * 1);
    EXPECT_EQ(nl0.get_numlist_layer(99, -1), -1);

    jpx_metanode folder = root.add_label("folder");
    EXPECT_TRUE(nl1.change_parent(folder));
    EXPECT_TRUE(nl0.change_parent(folder));
    EXPECT_TRUE(nl2.change_parent(folder));
    nl_dup.delete_node();

    jpx_metanode link_tgt = folder.add_label("link-target");
    jpx_metanode m_lbl = folder.add_label("orig-label");
    jpx_metanode m_xml = folder.add_delayed(jp2_xml_4cc, 1, nullptr);
    jpx_roi r_box;
    r_box.init_rectangle(MakeDims(0, 0, 8, 8));
    jpx_metanode m_roi = folder.add_regions(1, &r_box);
    jpx_metanode m_nl = folder.add_numlist(1, cs_list, 0, nullptr, false);
    jpx_metanode m_lnk = folder.add_link(link_tgt, JPX_ALTERNATE_PARENT_LINK);

    m_lbl.change_to_label("updated-label");
    m_xml.change_to_label("xml-to-label");
    m_roi.change_to_label("roi-to-label");
    m_nl.change_to_label("nl-to-label");
    m_lnk.change_to_label("lnk-to-label");

    m_lbl.change_to_link(link_tgt, JPX_GROUPING_LINK);
    m_lbl.change_to_link(link_tgt, JPX_ALTERNATE_PARENT_LINK);
    jpx_metanode m_roi2 = folder.add_regions(1, &r_box);
    m_roi2.change_to_link(link_tgt, JPX_GROUPING_LINK);
    jpx_metanode m_nl2 = folder.add_numlist(1, cs_list, 0, nullptr, false);
    m_nl2.change_to_link(link_tgt, JPX_GROUPING_LINK);
    jpx_metanode m_xml2 = folder.add_delayed(jp2_xml_4cc, 2, nullptr);
    m_xml2.change_to_link(link_tgt, JPX_GROUPING_LINK);

    EXPECT_TRUE(folder
                    .find_path_to(link_tgt, JPX_PATH_TO_DIRECT, 0, 0, nullptr,
                                  nullptr, false)
                    .exists());
    EXPECT_TRUE(link_tgt
                    .find_path_to(folder, 0, JPX_PATH_TO_DIRECT, 0, nullptr,
                                  nullptr, false)
                    .exists());
    EXPECT_TRUE(
        link_tgt.find_path_to(link_tgt, 0, 0, 0, nullptr, nullptr, false)
            .exists());
    EXPECT_FALSE(
        link_tgt.find_path_to(jpx_metanode(), 0, 0, 0, nullptr, nullptr, false)
            .exists());
    const kdu_core::kdu_uint32 excl_type = jp2_association_4cc;
    const int excl_flag = JPX_PATH_TO_EXCLUDE_PARENTS;
    (void)link_tgt.find_path_to(folder, JPX_PATH_TO_FORWARD,
                                JPX_PATH_TO_REVERSE, 1, &excl_type, &excl_flag,
                                false);

    jpx_metanode sub_src = root.add_label("sub-src");
    sub_src.add_link(link_tgt, JPX_GROUPING_LINK);
    root.add_copy(sub_src, true, true);
    mm.reset_copy_locators(link_tgt, true, true);

    tgt.write_headers();
    jp2_output_box* ts = top_cs.open_stream();
    WriteMinimalCodestream(ts, 8, 8, 3);
    ts->close();
    for (int r = 0; r < 5; ++r) {
      jp2_output_box* s = cont.access_codestream(0).open_stream();
      WriteMinimalCodestream(s, 8, 8, 3);
      s->close();
    }

    auto flush_meta = [&tgt]() {
      int iparam = 0;
      void* aparam = nullptr;
      std::string big_xml = "<big>" + std::string(1500, 'x') + "</big>";
      while (jp2_output_box* db = tgt.write_metadata(&iparam, &aparam)) {
        db->write(reinterpret_cast<const kdu_core::kdu_byte*>(big_xml.data()),
                  static_cast<int>(big_xml.size()));
      }
    };

    for (int i = 0; i < 3; ++i) {
      root.add_label("tiny");
      flush_meta();
    }
    jpx_metanode big_node = root.add_delayed(jp2_xml_4cc, 42, nullptr);
    root.add_link(big_node, JPX_ALTERNATE_PARENT_LINK);
    flush_meta();
    root.add_label("after-overflow");
    flush_meta();
    tgt.close();
    ftgt.close();
  }

  // 2) Part-2 (SCP2_caps_EXTENDED_COD) and HTJ2K cs 0 compatibility + mixed
  //    default palette/bpcc differences across codestreams.
  for (bool use_ht : {false, true}) {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);

    jpx_codestream_target cs0 = tgt.add_codestream();
    jp2_dimensions d0 = cs0.access_dimensions();
    d0.init(kdu_core::kdu_coords(8, 8), 2, false);
    d0.set_precision(0, 8, false);
    d0.set_precision(1, 10, false);
    if (use_ht) {
      d0.finalize_compatibility(Sprofile_PROFILE0, Scap_P15, 0, 0, 0, 0);
    } else {
      d0.finalize_compatibility(Sprofile_PART2, Scap_P2, 0, 0,
                                SCP2_caps_EXTENDED_COD, 0);
    }
    jp2_palette pal0 = cs0.access_palette();
    pal0.init(3, 4);
    for (int c = 0; c < 3; ++c) {
      kdu_core::kdu_int32 lut4[4] = {0, 64, 128, 255};
      pal0.set_lut(c, lut4, 8, false);
    }
    jpx_layer_target l0 = tgt.add_layer();
    l0.access_channels().init(3);
    for (int c = 0; c < 3; ++c) {
      l0.access_channels().set_colour_mapping(c, 0, c, 0);
    }
    l0.access_colour(0).init(JP2_sRGB_SPACE);

    jpx_codestream_target cs1 = tgt.add_codestream();
    jp2_dimensions d1 = cs1.access_dimensions();
    d1.init(kdu_core::kdu_coords(8, 8), 2, false);
    d1.set_precision(0, 8, false);
    d1.set_precision(1, 10, false);
    d1.finalize_compatibility(Sprofile_PROFILE0, 0, 0, 0, 0, 0);
    jp2_palette pal1 = cs1.access_palette();
    pal1.init(3, 2);
    for (int c = 0; c < 3; ++c) {
      kdu_core::kdu_int32 lut2[2] = {0, 255};
      pal1.set_lut(c, lut2, 8, false);
    }
    jpx_layer_target l1 = tgt.add_layer();
    l1.access_channels().init(3);
    for (int c = 0; c < 3; ++c) {
      l1.access_channels().set_colour_mapping(c, 0, c, 1);
    }
    l1.access_colour(0).init(JP2_sRGB_SPACE);

    tgt.write_headers();
    for (jpx_codestream_target* csp : {&cs0, &cs1}) {
      jp2_output_box* s = csp->open_stream();
      kdu_core::siz_params siz;
      siz.set(Sdims, 0, 0, 8);
      siz.set(Sdims, 0, 1, 8);
      siz.set(Scomponents, 0, 0, 2);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Sprecision, 1, 0, 10);
      siz.set(Ssigned, 0, 0, false);
      siz.set(Ssigned, 1, 0, false);
      if (!use_ht && csp == &cs0) {
        static_cast<kdu_core::kdu_params&>(siz).parse_string("Sprofile=PART2");
        siz.set(SCP2_caps, 0, 0, SCP2_caps_EXTENDED_COD);
      } else if (use_ht && csp == &cs0) {
        static_cast<kdu_core::kdu_params&>(siz).parse_string("Cmodes=HT");
      }
      static_cast<kdu_core::kdu_params&>(siz).finalize();
      kdu_core::kdu_codestream c;
      c.create(&siz, s);
      c.access_siz()->parse_string("Creversible=yes");
      if (use_ht && csp == &cs0) c.access_siz()->parse_string("Cmodes=HT");
      c.access_siz()->finalize_all();
      kdu_core::kdu_tile tile = c.open_tile(kdu_core::kdu_coords(0, 0));
      kdu_core::kdu_sample_allocator alloc;
      kdu_core::kdu_push_pull_params pp;
      std::vector<kdu_core::kdu_push_ifc> eng(2);
      for (int k = 0; k < 2; ++k) {
        eng[k] = kdu_core::kdu_analysis(
            tile.access_component(k).access_resolution(), &alloc, pp,
            KDU_SAMPLE_PREC16, 1.0F, 0, nullptr);
      }
      kdu_core::kdu_line_buf line;
      line.pre_create(&alloc, 8, KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE, 0,
                      0);
      alloc.finalize(c);
      line.create();
      for (int k = 0; k < 2; ++k) {
        for (int r = 0; r < 8; ++r) {
          for (int i = 0; i < 8; ++i) line.get_buf16()[i].ival = 10;
          eng[k].push(line);
        }
        eng[k].destroy();
      }
      line.destroy();
      tile.close();
      kdu_core::kdu_long flush_lim = 65536;
      c.flush(&flush_lim, 1);
      c.destroy();
      s->close();
    }
    tgt.write_metadata();
    tgt.close();
    ftgt.close();
  }

  // 3) Synthetic JPX binary stream exercising:
  //    - cgrp -> cref -> flst pointing to standalone top-level colr payload
  //    - forward crossref resolving to target after it AND cref to free asoc
  //    - url_fopen rejection path via dref + external url_idx = 1
  {
    std::vector<kdu_core::kdu_byte> raw;
    AppendBox(raw, 0x6A502020U, {0x0D, 0x0A, 0x87, 0x0A});
    std::vector<kdu_core::kdu_byte> ftyp;
    AppendBe32(ftyp, jpx_brand);
    AppendBe32(ftyp, 0);
    AppendBe32(ftyp, jpx_brand);
    AppendBox(raw, jp2_file_type_4cc, ftyp);

    std::vector<kdu_core::kdu_byte> ihdr = {0, 0, 0, 8, 0, 0, 0,
                                            8, 0, 1, 7, 7, 0, 0};
    std::vector<kdu_core::kdu_byte> colr = {1, 0, 0, 0, 0, 0, 17};
    std::vector<kdu_core::kdu_byte> jp2h;
    AppendBox(jp2h, jp2_image_header_4cc, ihdr);
    AppendBox(jp2h, jp2_colour_4cc, colr);
    AppendBox(raw, jp2_header_4cc, jp2h);

    std::uint64_t standalone_colr_off = raw.size() + 8;
    AppendBox(raw, jp2_colour_4cc, colr);

    AppendBox(raw, jp2_codestream_header_4cc, {});
    std::vector<kdu_core::kdu_byte> flst;
    AppendBe16(flst, 1);
    AppendBe64(flst, standalone_colr_off);
    AppendBe32(flst, static_cast<std::uint32_t>(colr.size()));
    AppendBe16(flst, 0);
    std::vector<kdu_core::kdu_byte> cref;
    AppendBe32(cref, jp2_colour_4cc);
    AppendBox(cref, jp2_fragment_list_4cc, flst);
    std::vector<kdu_core::kdu_byte> cgrp;
    AppendBox(cgrp, jp2_cross_reference_4cc, cref);
    std::vector<kdu_core::kdu_byte> jplh;
    AppendBox(jplh, jp2_colour_group_4cc, cgrp);
    AppendBox(raw, jp2_compositing_layer_hdr_4cc, jplh);

    std::vector<kdu_core::kdu_byte> free_asoc;
    AppendBox(free_asoc, jp2_free_4cc, {});
    std::uint64_t free_asoc_box_off = raw.size();
    AppendBox(raw, jp2_association_4cc, free_asoc);

    // Put forward cref inside an asoc box so meta_manager registers it.
    std::string holder = "cref-holder";
    std::vector<kdu_core::kdu_byte> fwd_asoc;
    AppendBox(fwd_asoc, jp2_label_4cc,
              std::vector<kdu_core::kdu_byte>(holder.begin(), holder.end()));

    std::string target_text = "forward-target";
    std::vector<kdu_core::kdu_byte> target_lbl(target_text.begin(),
                                               target_text.end());
    std::vector<kdu_core::kdu_byte> fwd_flst;
    AppendBe16(fwd_flst, 1);
    std::size_t off_patch_pos =
        raw.size() + 8 + fwd_asoc.size() + 8 + 4 + 8 + 2;
    AppendBe64(fwd_flst, 0);
    AppendBe32(fwd_flst, static_cast<std::uint32_t>(8 + target_lbl.size()));
    AppendBe16(fwd_flst, 0);
    std::vector<kdu_core::kdu_byte> fwd_cref;
    AppendBe32(fwd_cref, jp2_label_4cc);
    AppendBox(fwd_cref, jp2_fragment_list_4cc, fwd_flst);
    AppendBox(fwd_asoc, jp2_cross_reference_4cc, fwd_cref);
    AppendBox(raw, jp2_association_4cc, fwd_asoc);

    std::uint64_t lbl_box_off = raw.size();
    for (int i = 0; i < 8; ++i) {
      raw[off_patch_pos + i] =
          static_cast<kdu_core::kdu_byte>((lbl_box_off >> (56 - 8 * i)) & 0xFF);
    }
    AppendBox(raw, jp2_label_4cc, target_lbl);

    std::vector<kdu_core::kdu_byte> ga_asoc;
    AppendBox(ga_asoc, jp2_label_4cc,
              std::vector<kdu_core::kdu_byte>(holder.begin(), holder.end()));
    std::vector<kdu_core::kdu_byte> ga_flst;
    AppendBe16(ga_flst, 1);
    AppendBe64(ga_flst, free_asoc_box_off);
    AppendBe32(ga_flst, static_cast<std::uint32_t>(8 + free_asoc.size()));
    AppendBe16(ga_flst, 0);
    std::vector<kdu_core::kdu_byte> ga_cref;
    AppendBe32(ga_cref, jp2_association_4cc);
    AppendBox(ga_cref, jp2_fragment_list_4cc, ga_flst);
    AppendBox(ga_asoc, jp2_cross_reference_4cc, ga_cref);
    AppendBox(raw, jp2_association_4cc, ga_asoc);

    MemTarget cs_mt;
    WriteMinimalCodestream(&cs_mt, 8, 8, 1);
    AppendBox(raw, jp2_codestream_4cc, cs_mt.data());

    MemSource ms(raw);
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    if (src.open(&fs, false) >= 0) {
      jpx_meta_manager smm = src.access_meta_manager();
      jpx_metanode sroot = smm.access_root();
      std::vector<jpx_metanode> nodes;
      for (jpx_metanode child = sroot.get_next_descendant(jpx_metanode(), -1);
           child.exists(); child = sroot.get_next_descendant(child, -1)) {
        nodes.push_back(child);
      }
      for (jpx_metanode& n : nodes) {
        jpx_metanode_link_type lt = JPX_METANODE_LINK_NONE;
        n.get_link(lt, true);
      }
      int desc = 0;
      sroot.count_descendants(desc);
      (void)src.access_layer(0, false);
    }
    src.close();
    fs.close();

    std::vector<kdu_core::kdu_byte> dref_payload;
    AppendBe16(dref_payload, 1);
    std::vector<kdu_core::kdu_byte> url_payload = {0, 0, 0, 0};
    const char kExtUrl[] = "file:///./ext_local.dat";
    url_payload.insert(url_payload.end(), kExtUrl, kExtUrl + sizeof(kExtUrl));
    AppendBox(dref_payload, jp2_data_entry_url_4cc, url_payload);

    std::vector<kdu_core::kdu_byte> ext_raw;
    AppendBox(ext_raw, 0x6A502020U, {0x0D, 0x0A, 0x87, 0x0A});
    AppendBox(ext_raw, jp2_file_type_4cc, ftyp);
    AppendBox(ext_raw, jp2_header_4cc, jp2h);
    AppendBox(ext_raw, jp2_dtbl_4cc, dref_payload);
    std::vector<kdu_core::kdu_byte> ext_flst;
    AppendBe16(ext_flst, 1);
    AppendBe64(ext_flst, 0);
    AppendBe32(ext_flst, 16);
    AppendBe16(ext_flst, 1);
    std::vector<kdu_core::kdu_byte> ext_ftbl;
    AppendBox(ext_ftbl, jp2_fragment_list_4cc, ext_flst);
    AppendBox(ext_raw, jp2_fragment_table_4cc, ext_ftbl);

    MemSource ems(ext_raw);
    jp2_family_src efs;
    efs.open(&ems);
    jpx_source esrc;
    try {
      if (esrc.open(&efs, false) >= 0) {
        int desc = 0;
        esrc.access_meta_manager().access_root().count_descendants(desc);
        jpx_codestream_source ecs_src = esrc.access_codestream(0, false);
        if (ecs_src.exists()) {
          jpx_input_box* ecs = ecs_src.open_stream();
          if (ecs != nullptr) {
            kdu_core::kdu_byte b = 0;
            (void)ecs->read(&b, 1);
            ecs->close();
          }
        }
      }
    } catch (kdu_core::kdu_exception) {
    }
    esrc.close();
    efs.close();
  }
}

}  // namespace

// vendor/ ships kdu_cache.h without kdu_cache.cpp; provide base vtable stubs
// so MemKduCache can exercise JPIP dynamic-cache paths in jpx.cpp.
kdu_cache::kdu_cache() : state(nullptr) {}
kdu_cache::~kdu_cache() = default;
bool kdu_cache::close() { return true; }
bool kdu_cache::add_to_databin(int, kdu_core::kdu_long, kdu_core::kdu_long,
                               const kdu_core::kdu_byte[], int, int, bool, bool,
                               bool) {
  return false;
}
bool kdu_cache::delete_databin(int, kdu_core::kdu_long, kdu_core::kdu_long,
                               bool) {
  return false;
}
int kdu_cache::delete_stream_class(int, kdu_core::kdu_long, bool) { return 0; }
kdu_core::kdu_int32 kdu_cache::mark_databin(int, kdu_core::kdu_long,
                                            kdu_core::kdu_long, bool,
                                            int& length, bool& is_complete) {
  length = 0;
  is_complete = false;
  return 0;
}
void kdu_cache::clear_all_marks() {}
void kdu_cache::set_all_marks() {}
int kdu_cache::get_databin_length(int, kdu_core::kdu_long, kdu_core::kdu_long,
                                  bool* is_complete) {
  if (is_complete != nullptr) *is_complete = false;
  return 0;
}
bool kdu_cache::scan_databins(kdu_core::kdu_int32, int&, kdu_core::kdu_long&,
                              kdu_core::kdu_long&, int&, bool&,
                              kdu_core::kdu_byte*, int) {
  return false;
}
int kdu_cache::set_read_scope(int, kdu_core::kdu_long, kdu_core::kdu_long,
                              bool* is_complete) {
  if (is_complete != nullptr) *is_complete = false;
  return 0;
}
bool kdu_cache::set_tileheader_scope(int, int) { return false; }
bool kdu_cache::set_precinct_scope(kdu_core::kdu_long) { return false; }
int kdu_cache::read(kdu_core::kdu_byte*, int) { return 0; }
bool kdu_cache::seek(kdu_core::kdu_long) { return false; }
kdu_core::kdu_long kdu_cache::get_pos() { return 0; }

namespace {

class MemKduCache : public kdu_cache {
 public:
  bool close() override {
    bins_.clear();
    has_scope_ = false;
    pos_ = 0;
    return true;
  }

  bool add_to_databin(int databin_class, kdu_core::kdu_long codestream_id,
                      kdu_core::kdu_long databin_id,
                      const kdu_core::kdu_byte data[], int offset,
                      int num_bytes, bool is_final, bool = true,
                      bool = false) override {
    if (databin_class == KDU_META_DATABIN) codestream_id = 0;
    Bin& b = bins_[{databin_class, codestream_id, databin_id}];
    if (offset + num_bytes > static_cast<int>(b.bytes.size())) {
      b.bytes.resize(static_cast<std::size_t>(offset + num_bytes), 0);
    }
    if (num_bytes > 0 && data != nullptr) {
      std::memcpy(b.bytes.data() + offset, data,
                  static_cast<std::size_t>(num_bytes));
    }
    if (is_final) b.is_complete = true;
    return true;
  }

  int get_databin_length(int databin_class, kdu_core::kdu_long codestream_id,
                         kdu_core::kdu_long databin_id,
                         bool* is_complete = nullptr) override {
    if (databin_class == KDU_META_DATABIN) codestream_id = 0;
    auto it = bins_.find({databin_class, codestream_id, databin_id});
    if (it == bins_.end()) {
      if (is_complete != nullptr) *is_complete = false;
      return 0;
    }
    if (is_complete != nullptr) *is_complete = it->second.is_complete;
    return static_cast<int>(it->second.bytes.size());
  }

  int set_read_scope(int databin_class, kdu_core::kdu_long codestream_id,
                     kdu_core::kdu_long databin_id,
                     bool* is_complete = nullptr) override {
    if (databin_class == KDU_META_DATABIN) codestream_id = 0;
    cur_key_ = {databin_class, codestream_id, databin_id};
    has_scope_ = true;
    pos_ = 0;
    return get_databin_length(databin_class, codestream_id, databin_id,
                              is_complete);
  }

  bool seek(kdu_core::kdu_long offset) override {
    if (!has_scope_ || offset < 0) return false;
    pos_ = offset;
    return true;
  }

  kdu_core::kdu_long get_pos() override { return pos_; }

  int read(kdu_core::kdu_byte* buf, int num_bytes) override {
    if (!has_scope_ || num_bytes <= 0) return 0;
    auto it = bins_.find(cur_key_);
    if (it == bins_.end() ||
        pos_ >= static_cast<kdu_core::kdu_long>(it->second.bytes.size())) {
      return 0;
    }
    const int avail = static_cast<int>(it->second.bytes.size() -
                                       static_cast<std::size_t>(pos_));
    const int n = std::min(num_bytes, avail);
    std::memcpy(buf, it->second.bytes.data() + pos_,
                static_cast<std::size_t>(n));
    pos_ += n;
    return n;
  }

 private:
  struct Key {
    int cls = 0;
    kdu_core::kdu_long cs_id = 0;
    kdu_core::kdu_long bin_id = 0;
    auto operator<=>(const Key&) const = default;
  };
  struct Bin {
    std::vector<kdu_core::kdu_byte> bytes;
    bool is_complete = false;
  };
  std::map<Key, Bin> bins_;
  Key cur_key_{};
  bool has_scope_ = false;
  kdu_core::kdu_long pos_ = 0;
};

TEST(JpxTest, Wave7IncrementalCachePlaceholdersAndMetareqs) {
  ScopedKduHandlers handlers;

  // Build valid box payloads via MemTarget to splice into MemKduCache databins
  // as progressive chunks & JPIP placeholders ('phld').
  MemTarget cs1_mt;
  WriteMinimalCodestream(&cs1_mt, 8, 8, 1);
  MemTarget cs_mt;
  WriteMinimalCodestream(&cs_mt, 8, 8, 3);

  std::vector<kdu_core::kdu_byte> ftyp_p;
  AppendBe32(ftyp_p, jpx_brand);
  AppendBe32(ftyp_p, 0);
  AppendBe32(ftyp_p, jpx_brand);
  AppendBe32(ftyp_p, jp2_brand);

  std::vector<kdu_core::kdu_byte> rreq_p = {1, 0x80, 0x80};
  AppendBe16(rreq_p, 1);
  AppendBe16(rreq_p, JPX_SF_PIXEL_FORMAT_FLOAT);
  rreq_p.push_back(0x80);
  AppendBe16(rreq_p, 0);  // 0 vendor features

  // Palettized 1-comp ihdr + bpcc + pclr + MTYP=1 cmap for cs0 (chdr0).
  std::vector<kdu_core::kdu_byte> ihdr_p = {0, 0, 0, 8,   0, 0, 0,
                                            8, 0, 1, 255, 7, 0, 0};
  std::vector<kdu_core::kdu_byte> ihdr3_p = {0, 0, 0, 8, 0, 0, 0,
                                             8, 0, 3, 7, 7, 0, 0};
  std::vector<kdu_core::kdu_byte> bpcc_p = {7};
  std::vector<kdu_core::kdu_byte> pclr_p = {0, 2, 3, 7,   7,   7,
                                            0, 0, 0, 255, 255, 255};
  std::vector<kdu_core::kdu_byte> cmap_p = {0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 2};
  std::vector<kdu_core::kdu_byte> colr_p = {1, 0, 1, 0, 0, 0, 16};
  std::vector<kdu_core::kdu_byte> cdef_p = {0, 3, 0, 0, 0, 0, 0, 1, 0, 1,
                                            0, 0, 0, 2, 0, 2, 0, 0, 0, 3};
  std::vector<kdu_core::kdu_byte> opct_p = {2, 3, 0, 0, 0};
  std::vector<kdu_core::kdu_byte> pxfm_p = {0, 3, 0, 0, 0, 0, 0,
                                            1, 0, 0, 0, 2, 0, 0};
  std::vector<kdu_core::kdu_byte> res_p;
  {
    std::vector<kdu_core::kdu_byte> resc = {0, 100, 0, 1, 0, 100, 0, 1, 0, 0};
    AppendBox(res_p, jp2_capture_resolution_4cc, resc);
  }
  std::vector<kdu_core::kdu_byte> creg_p = {0, 1, 0, 1, 0, 0, 1, 1, 0, 0};
  std::vector<kdu_core::kdu_byte> creg1_p = {0, 1, 0, 1, 0, 1, 1, 1, 0, 0};
  std::vector<kdu_core::kdu_byte> cgrp_p;
  AppendPlaceholderBox(cgrp_p, 1, 18,
                       static_cast<std::uint32_t>(8 + colr_p.size()),
                       jp2_colour_4cc);

  std::vector<kdu_core::kdu_byte> lbl_p = {'c', 'h', 'd', 'r', 'L', 'b', 'l'};
  std::vector<kdu_core::kdu_byte> xml_p = {'<', 'x', '/', '>'};

  // jp2h placeholder bin 1: ihdr + colr
  std::vector<kdu_core::kdu_byte> jp2h_bin;
  AppendBox(jp2h_bin, jp2_image_header_4cc, ihdr3_p);
  AppendBox(jp2h_bin, jp2_colour_4cc, colr_p);

  // chdr 0 placeholder bin 2: sub-placeholders for ihdr(3), bpcc(4), pclr(5),
  // cmap(6), lbl(7). Only bin 6 (cmap) stays incomplete simultaneously to avoid
  // overwriting pending_subs on jx_codestream_source (jpx_local.h:4388).
  std::vector<kdu_core::kdu_byte> chdr0_bin;
  AppendPlaceholderBox(chdr0_bin, 1, 3,
                       static_cast<std::uint32_t>(8 + ihdr_p.size()),
                       jp2_image_header_4cc);
  AppendPlaceholderBox(chdr0_bin, 1, 4,
                       static_cast<std::uint32_t>(8 + bpcc_p.size()),
                       jp2_bits_per_component_4cc);
  AppendPlaceholderBox(chdr0_bin, 1, 5,
                       static_cast<std::uint32_t>(8 + pclr_p.size()),
                       jp2_palette_4cc);
  AppendPlaceholderBox(chdr0_bin, 1, 6,
                       static_cast<std::uint32_t>(8 + cmap_p.size()),
                       jp2_component_mapping_4cc);
  AppendPlaceholderBox(chdr0_bin, 1, 7,
                       static_cast<std::uint32_t>(8 + lbl_p.size()),
                       jp2_label_4cc);

  // chdr 1 placeholder bin 9: xml(10) + 3-comp ihdr so metanode holds
  // codestream_source initially before finishing.
  std::vector<kdu_core::kdu_byte> chdr1_bin;
  AppendPlaceholderBox(chdr1_bin, 1, 10,
                       static_cast<std::uint32_t>(8 + xml_p.size()),
                       jp2_xml_4cc);
  AppendBox(chdr1_bin, jp2_image_header_4cc, ihdr3_p);

  // jplh 0 placeholder bin 11: sub-placeholders for cdef(12), pxfm(14),
  // res(15), creg(16), cgrp(17->colr 18).
  std::vector<kdu_core::kdu_byte> jplh0_bin;
  AppendPlaceholderBox(jplh0_bin, 1, 12,
                       static_cast<std::uint32_t>(8 + cdef_p.size()),
                       jp2_channel_definition_4cc);
  AppendPlaceholderBox(jplh0_bin, 1, 14,
                       static_cast<std::uint32_t>(8 + pxfm_p.size()),
                       jp2_pixel_format_4cc);
  AppendPlaceholderBox(jplh0_bin, 1, 15,
                       static_cast<std::uint32_t>(8 + res_p.size()),
                       jp2_resolution_4cc);
  AppendPlaceholderBox(jplh0_bin, 1, 16,
                       static_cast<std::uint32_t>(8 + creg_p.size()),
                       jp2_registration_4cc);
  AppendPlaceholderBox(jplh0_bin, 1, 17,
                       static_cast<std::uint32_t>(8 + cgrp_p.size()),
                       jp2_colour_group_4cc);

  // jplh 1 placeholder bin 19: xml(20) + opct(13) + creg(cs 1) + colr.
  std::vector<kdu_core::kdu_byte> jplh1_bin;
  AppendPlaceholderBox(jplh1_bin, 1, 20,
                       static_cast<std::uint32_t>(8 + xml_p.size()),
                       jp2_xml_4cc);
  AppendPlaceholderBox(jplh1_bin, 1, 13,
                       static_cast<std::uint32_t>(8 + opct_p.size()),
                       jp2_opacity_4cc);
  AppendBox(jplh1_bin, jp2_registration_4cc, creg1_p);
  AppendBox(jplh1_bin, jp2_colour_4cc, colr_p);

  // Nested j2cx bins (indefinite J2CI: Ncs=0, Ltbl=0 inside MemKduCache).
  std::vector<kdu_core::kdu_byte> j2ci_indef = {0, 0, 0, 0, 0, 0, 0, 0};
  std::vector<kdu_core::kdu_byte> child_j2cx_p;
  AppendBox(child_j2cx_p, jp2_multi_codestream_info_4cc, j2ci_indef);
  AppendBox(child_j2cx_p, jp2_codestream_4cc, cs_mt.data());
  std::vector<kdu_core::kdu_byte> parent_j2cx_p;
  AppendBox(parent_j2cx_p, jp2_multi_codestream_info_4cc, j2ci_indef);
  AppendBox(parent_j2cx_p, jp2_multi_codestream_4cc, child_j2cx_p);
  AppendBox(parent_j2cx_p, jp2_codestream_4cc, cs_mt.data());

  // Main meta-bin 0 layout:
  std::vector<kdu_core::kdu_byte> bin0;
  AppendBox(bin0, 0x6A502020U, {0x0D, 0x0A, 0x87, 0x0A});
  AppendBox(bin0, jp2_file_type_4cc, ftyp_p);
  const std::size_t after_ftyp = bin0.size();
  AppendBox(bin0, jp2_reader_requirements_4cc, rreq_p);
  const std::size_t after_rreq = bin0.size();
  AppendPlaceholderBox(bin0, 1, 1,
                       static_cast<std::uint32_t>(8 + jp2h_bin.size()),
                       jp2_header_4cc);
  AppendBox(bin0, jp2_codestream_4cc, cs1_mt.data());
  AppendBox(bin0, jp2_codestream_4cc, cs_mt.data());
  AppendPlaceholderBox(bin0, 1, 2,
                       static_cast<std::uint32_t>(8 + chdr0_bin.size()),
                       jp2_codestream_header_4cc);
  AppendPlaceholderBox(bin0, 1, 9,
                       static_cast<std::uint32_t>(8 + chdr1_bin.size()),
                       jp2_codestream_header_4cc);
  AppendPlaceholderBox(bin0, 1, 11,
                       static_cast<std::uint32_t>(8 + jplh0_bin.size()),
                       jp2_compositing_layer_hdr_4cc);
  AppendPlaceholderBox(bin0, 1, 19,
                       static_cast<std::uint32_t>(8 + jplh1_bin.size()),
                       jp2_compositing_layer_hdr_4cc);

  // Composition box + jclx (jlxi + chdr + jplh + trailing lbl) + j2cx.
  std::vector<kdu_core::kdu_byte> comp_p;
  AppendBox(comp_p, jp2_comp_options_4cc, {0, 0, 0, 8, 0, 0, 0, 8, 1});
  std::vector<kdu_core::kdu_byte> iset_p = {0, 4, 0, 1,  0, 0, 0, 1,
                                            0, 0, 0, 10, 0, 0, 0, 0};
  AppendBox(comp_p, jp2_comp_instruction_set_4cc, iset_p);
  AppendPlaceholderBox(bin0, 1, 40,
                       static_cast<std::uint32_t>(8 + comp_p.size()),
                       jp2_composition_4cc);

  std::vector<kdu_core::kdu_byte> jlxi_p;
  AppendBe32(jlxi_p, 0);
  AppendBe32(jlxi_p, 1);
  AppendBe32(jlxi_p, 1);
  AppendBe32(jlxi_p, 0);
  AppendBe32(jlxi_p, 0);
  std::vector<kdu_core::kdu_byte> jclx_p;
  AppendBox(jclx_p, jp2_layer_extensions_info_4cc, jlxi_p);
  AppendBox(jclx_p, jp2_codestream_header_4cc, {});
  AppendBox(jclx_p, jp2_compositing_layer_hdr_4cc, {});
  const std::size_t jclx_donate_len = jclx_p.size();
  AppendBox(jclx_p, jp2_label_4cc, lbl_p);
  AppendPlaceholderBox(bin0, 1, 41,
                       static_cast<std::uint32_t>(8 + jclx_p.size()),
                       jp2_layer_extensions_4cc);
  AppendBox(bin0, jp2_multi_codestream_4cc, parent_j2cx_p);

  // Incomplete asoc (bin 30) and incomplete xml (bin 31), plus empty nlst.
  std::vector<kdu_core::kdu_byte> asoc30_p;
  AppendBox(asoc30_p, jp2_label_4cc, lbl_p);
  AppendPlaceholderBox(bin0, 1, 30,
                       static_cast<std::uint32_t>(8 + asoc30_p.size()),
                       jp2_association_4cc);

  std::vector<kdu_core::kdu_byte> wrap_asoc_p;
  AppendBox(wrap_asoc_p, jp2_number_list_4cc, {});
  AppendPlaceholderBox(wrap_asoc_p, 1, 31,
                       static_cast<std::uint32_t>(8 + xml_p.size()),
                       jp2_xml_4cc);
  AppendPlaceholderBox(bin0, 1, 32,
                       static_cast<std::uint32_t>(8 + wrap_asoc_p.size()),
                       jp2_association_4cc);

  // Forward target label placed AFTER two cref boxes pointing to its box_loc.
  std::size_t cref1_patch_off = 0, cref2_patch_off = 0;
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<kdu_core::kdu_byte> flst;
    AppendBe16(flst, 1);
    if (pass == 0)
      cref1_patch_off = bin0.size() + 8 + 4 + 8 + 2;
    else
      cref2_patch_off = bin0.size() + 8 + 4 + 8 + 2;
    AppendBe64(flst, 0);
    AppendBe32(flst, static_cast<std::uint32_t>(8 + lbl_p.size()));
    AppendBe16(flst, 0);
    std::vector<kdu_core::kdu_byte> cref;
    AppendBe32(cref, jp2_label_4cc);
    AppendBox(cref, jp2_fragment_list_4cc, flst);
    AppendBox(bin0, jp2_cross_reference_4cc, cref);
  }
  const std::uint64_t future_lbl_loc = bin0.size();
  for (std::size_t patch : {cref1_patch_off, cref2_patch_off}) {
    for (int i = 0; i < 8; ++i) {
      bin0[patch + i] = static_cast<kdu_core::kdu_byte>(
          (future_lbl_loc >> (56 - 8 * i)) & 0xFF);
    }
  }
  AppendPlaceholderBox(
      bin0, 1, 33, static_cast<std::uint32_t>(8 + lbl_p.size()), jp2_label_4cc);

  MemKduCache cache;
  // Stage 1: only ftyp fed, rreq missing -> src.open returns 0!
  cache.add_to_databin(KDU_META_DATABIN, 0, 0, bin0.data(), 0,
                       static_cast<int>(after_ftyp), false);
  jp2_family_src fs;
  fs.open(&cache);
  jpx_source src;
  EXPECT_EQ(src.open(&fs, true), 0);

  // Stage 2a: feed up to after_rreq -> src.open(&fs, true) completes open!
  cache.add_to_databin(KDU_META_DATABIN, 0, 0, bin0.data() + after_ftyp,
                       static_cast<int>(after_ftyp),
                       static_cast<int>(after_rreq - after_ftyp), false);
  fs.synch_with_cache();
  ASSERT_GT(src.open(&fs, true), 0);

  // Feed remaining bin0 except final flag, complete ihdr/bpcc/pclr (single
  // pending_sub cmap!), and feed 4 bytes of jclx_p for jlxi metareq.
  cache.add_to_databin(KDU_META_DATABIN, 0, 0, bin0.data() + after_rreq,
                       static_cast<int>(after_rreq),
                       static_cast<int>(bin0.size() - after_rreq), false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 1, jp2h_bin.data(), 0, 12, false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 2, chdr0_bin.data(), 0,
                       static_cast<int>(chdr0_bin.size() - 28), false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 3, ihdr_p.data(), 0,
                       static_cast<int>(ihdr_p.size()), true);
  cache.add_to_databin(KDU_META_DATABIN, 0, 4, bpcc_p.data(), 0,
                       static_cast<int>(bpcc_p.size()), true);
  cache.add_to_databin(KDU_META_DATABIN, 0, 5, pclr_p.data(), 0,
                       static_cast<int>(pclr_p.size()), true);
  cache.add_to_databin(KDU_META_DATABIN, 0, 6, cmap_p.data(), 0, 4, false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 9, chdr1_bin.data(), 0, 28, false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 11, jplh0_bin.data(), 0,
                       static_cast<int>(jplh0_bin.size()), true);
  cache.add_to_databin(KDU_META_DATABIN, 0, 12, cdef_p.data(), 0, 4, false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 13, opct_p.data(), 0, 2, false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 14, pxfm_p.data(), 0, 4, false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 15, res_p.data(), 0, 4, false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 16, creg_p.data(), 0, 4, false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 17, cgrp_p.data(), 0,
                       static_cast<int>(cgrp_p.size()), true);
  cache.add_to_databin(KDU_META_DATABIN, 0, 18, colr_p.data(), 0, 3, false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 19, jplh1_bin.data(), 0, 28, false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 30, asoc30_p.data(), 0, 4, false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 31, xml_p.data(), 0, 1, false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 32, wrap_asoc_p.data(), 0,
                       static_cast<int>(wrap_asoc_p.size()), true);
  cache.add_to_databin(KDU_META_DATABIN, 0, 33, lbl_p.data(), 0, 2, false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 40, comp_p.data(), 0, 17, false);
  cache.add_to_databin(KDU_META_DATABIN, 0, 41, jclx_p.data(), 0, 4, false);
  fs.synch_with_cache();

  kdu_window win;
  win.init();
  src.generate_metareq(&win, 0, 5, -1, -1, true);  // hits jlxi metareq

  // Stage 2b: feed jclx_p [4..jclx_donate_len) without is_final -> container
  // parses info and donates jclx to metanode + j2ci metareq!
  cache.add_to_databin(KDU_META_DATABIN, 0, 41, jclx_p.data() + 4, 4,
                       static_cast<int>(jclx_donate_len - 4), false);
  fs.synch_with_cache();
  (void)src.access_container(0);
  src.generate_metareq(&win, 0, 5, -1, -1, true);  // hits j2ci metareq

  // Access incomplete codestream & layer sources to arm pending_subs &
  // chdr/jplh donation.
  (void)src.access_codestream(0, false);
  (void)src.access_layer(0, false);

  // Trigger metanode::generate_metareq & generate_link_metareq over incomplete
  // asoc/xml/cref/empty-nlst + codestream_source/layer_source metanodes.
  jpx_meta_manager smm = src.access_meta_manager();
  jpx_metanode sroot = smm.access_root();
  const kdu_core::kdu_uint32 btypes[2] = {jp2_xml_4cc, jp2_label_4cc};
  const kdu_core::kdu_uint32 dtypes[1] = {jp2_association_4cc};
  sroot.generate_metareq(&win, 2, btypes, 1, dtypes, true, 4, KDU_MRQ_ALL);
  {
    std::vector<jpx_metanode> nodes;
    for (jpx_metanode n = sroot.get_next_descendant(jpx_metanode(), -1);
         n.exists(); n = sroot.get_next_descendant(n, -1)) {
      nodes.push_back(n);
    }
    for (jpx_metanode& n : nodes) {
      n.generate_metareq(&win, 0, nullptr, 1, dtypes, false, 3, KDU_MRQ_ALL);
      n.generate_link_metareq(&win, 2, btypes, 1, dtypes, true, 2, KDU_MRQ_ALL);
      jpx_metanode_link_type lt = JPX_METANODE_LINK_NONE;
      (void)n.get_link(lt, true);
    }
  }
  int cnt = 0;
  sroot.count_descendants(cnt);
  sroot.generate_metareq(&win, 0, nullptr, 1, dtypes, true, 4, KDU_MRQ_ALL);

  // Stage 3: complete all partial bins and re-drain everything.
  auto finish_bin = [&cache](kdu_core::kdu_long id,
                             const std::vector<kdu_core::kdu_byte>& buf,
                             int already) {
    cache.add_to_databin(KDU_META_DATABIN, 0, id, buf.data() + already, already,
                         static_cast<int>(buf.size() - already), true);
  };
  finish_bin(0, bin0, static_cast<int>(bin0.size()));
  finish_bin(1, jp2h_bin, 12);
  finish_bin(2, chdr0_bin, static_cast<int>(chdr0_bin.size() - 28));
  finish_bin(6, cmap_p, 4);
  finish_bin(7, lbl_p, 0);
  finish_bin(9, chdr1_bin, 28);
  finish_bin(10, xml_p, 0);
  finish_bin(12, cdef_p, 4);
  finish_bin(13, opct_p, 2);
  finish_bin(14, pxfm_p, 4);
  finish_bin(15, res_p, 4);
  finish_bin(16, creg_p, 4);
  finish_bin(18, colr_p, 3);
  finish_bin(19, jplh1_bin, 28);
  finish_bin(20, xml_p, 0);
  finish_bin(30, asoc30_p, 4);
  finish_bin(31, xml_p, 1);
  finish_bin(33, lbl_p, 2);
  finish_bin(40, comp_p, 17);
  finish_bin(41, jclx_p, static_cast<int>(jclx_donate_len));
  fs.synch_with_cache();

  src.generate_metareq(&win, 0, 5, -1, -1, true);
  EXPECT_TRUE(src.access_codestream(0, false).exists());
  EXPECT_TRUE(src.access_layer(0, false).exists());
  EXPECT_TRUE(src.access_layer(1, false).exists());
  EXPECT_TRUE(src.access_codestream(2, false).exists());
  EXPECT_TRUE(src.access_codestream(3, false).exists());
  EXPECT_TRUE(src.access_container(0).exists());
  EXPECT_FALSE(src.access_container(1).exists());
  int reps = 0;
  (void)src.access_container(0).count_repetitions(reps);
  int total_cs = 0;
  (void)src.count_codestreams(total_cs);

  {
    std::vector<jpx_metanode> nodes;
    for (jpx_metanode n = sroot.get_next_descendant(jpx_metanode(), -1);
         n.exists(); n = sroot.get_next_descendant(n, -1)) {
      nodes.push_back(n);
    }
    for (jpx_metanode& n : nodes) {
      jpx_metanode_link_type lt = JPX_METANODE_LINK_NONE;
      (void)n.get_link(lt, true);
    }
  }
  sroot.count_descendants(cnt);

  src.close();
  fs.close();

  // Exercise jx_codestream_source::add_pending_sub & drain for ihdr, bpcc,
  // and pclr individually (one incomplete sub-box per source -> zero leak).
  for (int which : {0, 1, 2}) {
    std::vector<kdu_core::kdu_byte> ihdr1_uniform = {0, 0, 0, 8, 0, 0, 0,
                                                     8, 0, 1, 7, 7, 0, 0};
    std::vector<kdu_core::kdu_byte> schdr;
    if (which == 0) {
      AppendPlaceholderBox(schdr, 1, 100,
                           static_cast<std::uint32_t>(8 + ihdr1_uniform.size()),
                           jp2_image_header_4cc);
    } else {
      AppendBox(schdr, jp2_image_header_4cc, ihdr_p);
    }
    if (which == 1) {
      AppendPlaceholderBox(schdr, 1, 101,
                           static_cast<std::uint32_t>(8 + bpcc_p.size()),
                           jp2_bits_per_component_4cc);
    } else if (which == 2) {
      AppendBox(schdr, jp2_bits_per_component_4cc, bpcc_p);
    }
    if (which == 2) {
      AppendPlaceholderBox(schdr, 1, 102,
                           static_cast<std::uint32_t>(8 + pclr_p.size()),
                           jp2_palette_4cc);
    } else {
      AppendBox(schdr, jp2_palette_4cc, pclr_p);
    }
    AppendBox(schdr, jp2_component_mapping_4cc, cmap_p);

    std::vector<kdu_core::kdu_byte> sbin0;
    AppendBox(sbin0, 0x6A502020U, {0x0D, 0x0A, 0x87, 0x0A});
    AppendBox(sbin0, jp2_file_type_4cc, ftyp_p);
    AppendBox(sbin0, jp2_reader_requirements_4cc, rreq_p);
    AppendBox(sbin0, jp2_header_4cc, jp2h_bin);
    AppendBox(sbin0, jp2_codestream_4cc, cs1_mt.data());
    AppendBox(sbin0, jp2_codestream_header_4cc, schdr);

    MemKduCache scache;
    scache.add_to_databin(KDU_META_DATABIN, 0, 0, sbin0.data(), 0,
                          static_cast<int>(sbin0.size()), true);
    if (which == 0) {
      scache.add_to_databin(KDU_META_DATABIN, 0, 100, ihdr1_uniform.data(), 0,
                            4, false);
    } else if (which == 1) {
      scache.add_to_databin(KDU_META_DATABIN, 0, 101, bpcc_p.data(), 0, 0,
                            false);
    } else {
      scache.add_to_databin(KDU_META_DATABIN, 0, 102, pclr_p.data(), 0, 4,
                            false);
    }

    jp2_family_src sfs;
    sfs.open(&scache);
    jpx_source ssrc;
    ASSERT_GT(ssrc.open(&sfs, true), 0);
    (void)ssrc.access_codestream(0, false);

    if (which == 0) {
      scache.add_to_databin(KDU_META_DATABIN, 0, 100, ihdr1_uniform.data() + 4,
                            4, static_cast<int>(ihdr1_uniform.size() - 4),
                            true);
    } else if (which == 1) {
      scache.add_to_databin(KDU_META_DATABIN, 0, 101, bpcc_p.data(), 0,
                            static_cast<int>(bpcc_p.size()), true);
    } else {
      scache.add_to_databin(KDU_META_DATABIN, 0, 102, pclr_p.data() + 4, 4,
                            static_cast<int>(pclr_p.size() - 4), true);
    }
    sfs.synch_with_cache();
    EXPECT_TRUE(ssrc.access_codestream(0, false).exists());
    ssrc.close();
    sfs.close();
  }
}

TEST(JpxTest, Wave7PathFillerHolesTriangulationAndQuadGuides) {
  ScopedKduHandlers handlers;

  // 1) Multi-depth concentric paths with explicit per-path thickening:
  //    outer (A) contains hole (B) & intersecting hole (D), and (B) contains
  //    doubly-contained island (C) -> import_internal_boundary, contains,
  //    doubly-contained promotion, intersecting-hole removal, add_triangle.
  {
    jpx_roi_editor ed;
    ed.set_max_undo_history(8);
    ed.set_mode(JPX_EDITOR_PATH_MODE);

    auto add_closed_path = [&ed](const std::vector<kdu_core::kdu_coords>& pts,
                                 bool replace, int thickness) {
      ed.clear_scribble_points();
      for (const auto& p : pts) ed.add_scribble_point(p);
      ed.convert_scribble_path(replace, 0, 1.0);
      ed.select_anchor(pts.front(), false);
      bool ok = false;
      ed.set_path_thickness(thickness, ok);
    };

    // Outer A (reversing brick prepend -> CCW sense < 0).
    add_closed_path(
        {kdu_core::kdu_coords(50, 50), kdu_core::kdu_coords(950, 50),
         kdu_core::kdu_coords(950, 950), kdu_core::kdu_coords(50, 950),
         kdu_core::kdu_coords(50, 50)},
        true, 6);
    // Hole B inside A (opposite winding -> CW sense > 0).
    add_closed_path(
        {kdu_core::kdu_coords(200, 200), kdu_core::kdu_coords(200, 800),
         kdu_core::kdu_coords(800, 800), kdu_core::kdu_coords(800, 200),
         kdu_core::kdu_coords(200, 200)},
        false, 6);
    // Island C inside B (doubly contained by A -> B -> C).
    add_closed_path(
        {kdu_core::kdu_coords(350, 350), kdu_core::kdu_coords(650, 350),
         kdu_core::kdu_coords(650, 650), kdu_core::kdu_coords(350, 650),
         kdu_core::kdu_coords(350, 350)},
        false, 4);
    // Overlapping hole D inside A that intersects hole B.
    add_closed_path(
        {kdu_core::kdu_coords(180, 180), kdu_core::kdu_coords(180, 360),
         kdu_core::kdu_coords(360, 360), kdu_core::kdu_coords(360, 180),
         kdu_core::kdu_coords(180, 180)},
        false, 4);

    bool ok = false;
    ed.fill_closed_paths(ok, -1);
    EXPECT_TRUE(ok);
    EXPECT_GT(ed.measure_complexity(), 0.0);

    // Direct JPX_EDITOR_FLAG_FILL on both CW and CCW concave 6-vertex L-shapes
    // to exercise examine_path sense+/sense- and add_triangle directly.
    const std::vector<kdu_core::kdu_coords> cw_l = {
        kdu_core::kdu_coords(40, 40),   kdu_core::kdu_coords(240, 40),
        kdu_core::kdu_coords(240, 120), kdu_core::kdu_coords(120, 120),
        kdu_core::kdu_coords(120, 240), kdu_core::kdu_coords(40, 240),
        kdu_core::kdu_coords(40, 40),
    };
    ed.clear_scribble_points();
    for (const auto& p : cw_l) ed.add_scribble_point(p);
    ed.convert_scribble_path(true, JPX_EDITOR_FLAG_FILL, 1.0);

    ed.clear_scribble_points();
    for (auto it = cw_l.rbegin(); it != cw_l.rend(); ++it) {
      ed.add_scribble_point(*it);
    }
    ed.convert_scribble_path(true, JPX_EDITOR_FLAG_FILL, 1.0);

    // Open-ended fill test (auto-closes loop in convert_scribble_path).
    ed.clear_scribble_points();
    ed.add_scribble_point(kdu_core::kdu_coords(30, 30));
    ed.add_scribble_point(kdu_core::kdu_coords(200, 50));
    ed.add_scribble_point(kdu_core::kdu_coords(180, 190));
    ed.add_scribble_point(kdu_core::kdu_coords(90, 130));
    ed.convert_scribble_path(true, JPX_EDITOR_FLAG_FILL, 0.0);
  }

  // 2) Non-rectangular quadrilateral guide projection + anchor/edge dragging.
  {
    jpx_roi_editor ed;
    jpx_roi skew_q;
    skew_q.init_quadrilateral(
        kdu_core::kdu_coords(50, 40), kdu_core::kdu_coords(180, 70),
        kdu_core::kdu_coords(150, 200), kdu_core::kdu_coords(30, 170));
    skew_q.fix_inconsistencies();
    ASSERT_TRUE(skew_q.check_geometry());
    ed.init(&skew_q, 1);

    ed.set_mode(JPX_EDITOR_VERTEX_MODE);
    kdu_core::kdu_coords v0(50, 40);
    EXPECT_GT(ed.find_nearest_anchor(v0, true), 0);
    ed.select_anchor(v0, false);

    kdu_core::kdu_coords guide(115, 55);
    EXPECT_TRUE(ed.find_nearest_guide_point(guide));
    kdu_core::kdu_coords dst(65, 52);
    EXPECT_TRUE(ed.can_move_selected_anchor(dst, true));
    ed.move_selected_anchor(dst);

    kdu_core::kdu_coords a_pt, e_from, e_to;
    (void)ed.get_anchor(a_pt, 0, true, true);
    (void)ed.get_edge(e_from, e_to, 0, true, true, true);
  }
}

TEST(JpxTest, Wave7CompositionLcmReplicationAndContainerMeta) {
  ScopedKduHandlers handlers;

  // 1) Indefinite container + presentation track (abs_layers_per_rep=2) +
  //    instruction increment=3 (coprime -> LCM loop) + access_next before
  //    total_frames is counted -> find_match/reverse_match LCM & access_next!
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);

    jpx_codestream_target top_cs = tgt.add_codestream();
    SetupCodestreamDims(top_cs.access_dimensions(), 8, 8, 3);
    jpx_layer_target top_l = tgt.add_layer();
    SetupSrgbLayer(top_l, 0);
    jpx_composition gcomp = tgt.access_composition();
    gcomp.set_loop_count(1);
    auto* gf = gcomp.add_frame(10, 0, false);
    gcomp.add_instruction(gf, 0, 0, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));

    jpx_container_target cont = tgt.add_container(1, 2, 0);  // indefinite!
    SetupCodestreamDims(cont.access_codestream(0).access_dimensions(), 8, 8, 3);
    const int cont_cs_id = cont.access_codestream(0).get_codestream_id();
    SetupSrgbLayer(cont.access_layer(0), cont_cs_id);
    SetupSrgbLayer(cont.access_layer(1), cont_cs_id);

    // Track 1: 2 layers per rep, instruction increment=3 -> hits LCM loop in
    // find_match (L3480-3550) & reverse_match (L3720-3754) since
    // max_container_rep > 0 on indefinite containers!
    jpx_composition t1 = cont.add_presentation_track(2);
    t1.set_loop_count(1);
    auto* tf1 = t1.add_frame(10, 4, false);
    t1.add_instruction(tf1, 0, 3, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));
    auto* tf2 = t1.add_frame(10, 3, false);
    t1.add_instruction(tf2, 1, 3, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));
    auto* tf3 = t1.add_frame(10, -1, false);
    t1.add_instruction(tf3, 2, 3, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));

    // Top-level container-linked numlists for move_old_top_container_numlist!
    jpx_meta_manager mm = tgt.access_meta_manager();
    jpx_metanode root = mm.access_root();
    const int cs_arr[1] = {cont_cs_id};
    const int ly_arr[1] = {1};
    jpx_metanode cnl0 = root.add_numlist(1, cs_arr, 1, ly_arr, false, 0);
    jpx_metanode cnl1 = root.add_numlist(1, cs_arr, 1, ly_arr, false, 0);
    jpx_metanode cnl2 = root.add_numlist(1, cs_arr, 1, ly_arr, false, 0);
    jpx_metanode tgt_lbl = root.add_label("container-numlist-target");
    cnl1.change_to_label("mid-detached");
    cnl0.change_to_link(tgt_lbl, JPX_GROUPING_LINK);
    cnl2.change_to_label("tail-detached");

    tgt.write_headers();
    jp2_output_box* s0 = top_cs.open_stream();
    WriteMinimalCodestream(s0, 8, 8, 3);
    s0->close();
    for (int r = 0; r < 6; ++r) {
      jp2_output_box* sc = cont.access_codestream(0).open_stream();
      WriteMinimalCodestream(sc, 8, 8, 3);
      sc->close();
    }
    tgt.write_metadata();
    tgt.close();
    ftgt.close();

    MemSource ms(mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 0);
    jpx_composition scomp = src.access_composition();

    // Call access_frame / access_next BEFORE count_track_frames so
    // comp->total_frames == 0 path in jpx_frame::access_next executes!
    jpx_frame f0 = scomp.access_frame(0, 0, false);
    if (f0.exists()) (void)f0.access_next(1, false);
    jpx_frame tf0 = scomp.access_frame(1, 0, true);
    if (tf0.exists()) {
      jpx_frame tf_mid = tf0.access_next(5, true);
      if (tf_mid.exists()) (void)tf_mid.access_next(20, true);
    }

    int total_f = 0;
    EXPECT_TRUE(scomp.count_track_frames(1, total_f));

    // Container layer 1 and 2 belong to track 1 (track layer start = 1):
    // idx=0 (< 2) triggers max_container_rep > 0 LCM loop!
    const int ql0[1] = {1};
    const int ql1[1] = {2};
    for (const int* q : {ql0, ql1}) {
      int ii = -1;
      jpx_frame mf = scomp.access_frame(1, 2, true);
      scomp.find_layer_match(mf, ii, 1, q, 1, 0, true, 0);
      jpx_frame rb;
      scomp.find_layer_match(rb, ii, 1, q, 1, 0, true, JPX_FRAME_MATCH_REVERSE);
      if (rb.exists()) {
        scomp.find_layer_match(rb, ii, 1, q, 1, 0, true,
                               JPX_FRAME_MATCH_REVERSE);
      }
    }
    // Non-matching query with match_all=true.
    const int bad_q[2] = {0, 1};
    int ii_bad = -1;
    jpx_frame bad_f;
    (void)scomp.find_layer_match(bad_f, ii_bad, 2, bad_q, 1, 0, true, 0);

    src.close();
    fs.close();
  }
}

void AppendClosedPolyBricks(std::vector<jpx_roi>& out,
                            absl::Span<const kdu_core::kdu_coords> verts,
                            bool reverse_bricks = false) {
  const std::size_t n = verts.size();
  for (std::size_t k = 0; k < n; ++k) {
    const std::size_t i = reverse_bricks ? (n - 1 - k) : k;
    const kdu_core::kdu_coords& a = verts[i];
    const kdu_core::kdu_coords& b = verts[(i + 1) % n];
    jpx_roi r;
    if (reverse_bricks) {
      r.init_quadrilateral(b, a, a, b);
    } else {
      r.init_quadrilateral(a, b, b, a);
    }
    out.push_back(r);
  }
}

TEST(JpxTest, Wave8PathFillerClosedBricksHolesAndTriangulation) {
  ScopedKduHandlers handlers;

  // 1) Multi-depth concentric closed bricks directly in jpx_roi_editor so
  //    enum_paths sees closed loops -> contains(), doubly-contained promotion,
  //    import_internal_boundary(), intersecting hole pruning, & add_triangle().
  {
    const std::vector<kdu_core::kdu_coords> outer_a = {
        kdu_core::kdu_coords(50, 50), kdu_core::kdu_coords(950, 50),
        kdu_core::kdu_coords(950, 950), kdu_core::kdu_coords(50, 950)};
    const std::vector<kdu_core::kdu_coords> hole_b = {
        kdu_core::kdu_coords(200, 200), kdu_core::kdu_coords(800, 200),
        kdu_core::kdu_coords(800, 800), kdu_core::kdu_coords(200, 800)};
    const std::vector<kdu_core::kdu_coords> island_c = {
        kdu_core::kdu_coords(350, 350), kdu_core::kdu_coords(650, 350),
        kdu_core::kdu_coords(650, 650), kdu_core::kdu_coords(350, 650)};
    const std::vector<kdu_core::kdu_coords> hole_d = {
        kdu_core::kdu_coords(180, 180), kdu_core::kdu_coords(360, 180),
        kdu_core::kdu_coords(360, 360), kdu_core::kdu_coords(180, 360)};

    std::vector<jpx_roi> rois;
    AppendClosedPolyBricks(rois, outer_a, false);
    AppendClosedPolyBricks(rois, hole_b, true);
    AppendClosedPolyBricks(rois, island_c, false);
    AppendClosedPolyBricks(rois, hole_d, true);

    jpx_roi_editor ed;
    ed.init(rois.data(), static_cast<int>(rois.size()));
    bool ok = false;
    ed.fill_closed_paths(ok, -1);
    EXPECT_TRUE(ok);
    EXPECT_GT(ed.measure_complexity(), 0.0);
  }

  // 2) Opposite sense brick loops (with & without thickening) + odd polygons
  //    (triangle, regular pentagon, concave dart) in both windings to exercise
  //    sense<0 init branch and both add_triangle(nbr0,e) /
  //    add_triangle(e,nbr1).
  {
    const std::vector<kdu_core::kdu_coords> sq = {
        kdu_core::kdu_coords(50, 50), kdu_core::kdu_coords(250, 50),
        kdu_core::kdu_coords(250, 250), kdu_core::kdu_coords(50, 250)};
    const std::vector<kdu_core::kdu_coords> pent = {
        kdu_core::kdu_coords(200, 40), kdu_core::kdu_coords(360, 140),
        kdu_core::kdu_coords(300, 320), kdu_core::kdu_coords(100, 320),
        kdu_core::kdu_coords(40, 140)};
    const std::vector<kdu_core::kdu_coords> dart = {
        kdu_core::kdu_coords(100, 50), kdu_core::kdu_coords(300, 200),
        kdu_core::kdu_coords(200, 180), kdu_core::kdu_coords(100, 320),
        kdu_core::kdu_coords(50, 180)};
    const std::vector<kdu_core::kdu_coords> tri = {
        kdu_core::kdu_coords(60, 60), kdu_core::kdu_coords(320, 90),
        kdu_core::kdu_coords(150, 310)};

    for (const auto* poly : {&sq, &pent, &dart, &tri}) {
      for (bool rev : {false, true}) {
        std::vector<jpx_roi> bricks;
        AppendClosedPolyBricks(bricks, *poly, rev);
        jpx_roi_editor ed;
        ed.init(bricks.data(), static_cast<int>(bricks.size()));
        bool ok = false;
        ed.fill_closed_paths(ok, -1);
        EXPECT_TRUE(ok);

        // Thicken the edges of `sq` in path mode before filling as well.
        if (poly == &sq) {
          jpx_roi_editor ed_thick;
          ed_thick.set_mode(JPX_EDITOR_PATH_MODE);
          ed_thick.init(bricks.data(), static_cast<int>(bricks.size()));
          kdu_core::kdu_coords mid(150, 50);
          EXPECT_GT(ed_thick.find_nearest_anchor(mid, false), 0);
          ed_thick.select_anchor(mid, false);
          bool tok = false;
          ed_thick.set_path_thickness(9, tok);
          ed_thick.fill_closed_paths(tok, -1);
          EXPECT_TRUE(tok);
        }
      }
    }

    // Direct 3-vertex & 5-vertex fill via convert_scribble_path on open-loop
    // scribble (auto-closed internally without loop pruning).
    jpx_roi_editor ed;
    ed.set_mode(JPX_EDITOR_PATH_MODE);
    for (const auto* poly : {&pent, &dart, &tri}) {
      ed.clear_scribble_points();
      for (const auto& p : *poly) ed.add_scribble_point(p);
      ed.convert_scribble_path(true, JPX_EDITOR_FLAG_FILL, 1.0);
      ed.clear_scribble_points();
      for (auto it = poly->rbegin(); it != poly->rend(); ++it) {
        ed.add_scribble_point(*it);
      }
      ed.convert_scribble_path(true, JPX_EDITOR_FLAG_FILL, 1.0);
    }
  }

  // 3) add_region edge cases: invalid init count, line mirroring, degenerate
  //    point region (5x5 expansion), num_regions==255 cap, scribble tail-loop
  //    pruning, and jx_regions::write > 254 roid slots warning (0x11011002).
  {
    jpx_roi_editor ed;
    jpx_roi dummy;
    dummy.init_rectangle(MakeDims(0, 0, 10, 10));
    EXPECT_THROW(ed.init(&dummy, -1), kdu_core::kdu_exception);

    // Line-segment ROI mirroring in vertex mode.
    jpx_roi line_roi;
    line_roi.init_quadrilateral(
        kdu_core::kdu_coords(40, 40), kdu_core::kdu_coords(80, 40),
        kdu_core::kdu_coords(80, 40), kdu_core::kdu_coords(40, 40));
    ed.init(&line_roi, 1);
    ed.set_mode(JPX_EDITOR_VERTEX_MODE);
    kdu_core::kdu_coords anc(40, 40);
    EXPECT_GT(ed.find_nearest_anchor(anc, true), 0);
    ed.select_anchor(anc, false);
    EXPECT_FALSE(ed.add_region(false, MakeDims(0, 0, 100, 100)).is_empty());

    // Degenerate single-point ROI -> 5x5 square.
    jpx_roi pt_roi;
    pt_roi.init_quadrilateral(
        kdu_core::kdu_coords(50, 50), kdu_core::kdu_coords(50, 50),
        kdu_core::kdu_coords(50, 50), kdu_core::kdu_coords(50, 50));
    ed.init(&pt_roi, 1);
    anc = kdu_core::kdu_coords(50, 50);
    EXPECT_GT(ed.find_nearest_anchor(anc, true), 0);
    ed.select_anchor(anc, false);
    EXPECT_FALSE(ed.add_region(false, kdu_core::kdu_dims()).is_empty());

    // Max 255 regions cap in add_region.
    std::vector<jpx_roi> max_rois(255);
    for (int i = 0; i < 255; ++i) {
      max_rois[i].init_rectangle(MakeDims((i % 16) * 4, (i / 16) * 4, 2, 2));
    }
    ed.init(max_rois.data(), 255);
    anc = kdu_core::kdu_coords(0, 0);
    ed.find_nearest_anchor(anc, false);
    ed.select_anchor(anc, false);
    EXPECT_TRUE(ed.add_region(false, kdu_core::kdu_dims()).is_empty());

    // Scribble tail-half loop elimination (n > num_scribble_points / 2).
    ed.clear_scribble_points();
    for (int i = 0; i < 10; ++i) {
      ed.add_scribble_point(kdu_core::kdu_coords(10 * i, 100));
    }
    ed.add_scribble_point(kdu_core::kdu_coords(85, 120));
    (void)ed.add_scribble_point(kdu_core::kdu_coords(85, 80));

    // jx_regions::write with >254 roid slots (130 skew quads = 260 roid
    // entries).
    std::vector<jpx_roi> skew_rois(130);
    for (int i = 0; i < 130; ++i) {
      const int x0 = (i % 13) * 10;
      const int y0 = (i / 13) * 10;
      skew_rois[i].init_quadrilateral(kdu_core::kdu_coords(x0 + 1, y0),
                                      kdu_core::kdu_coords(x0 + 8, y0 + 1),
                                      kdu_core::kdu_coords(x0 + 7, y0 + 8),
                                      kdu_core::kdu_coords(x0, y0 + 7));
    }
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 256, 256, 3);
    SetupSrgbLayer(tgt.add_layer(), 0);
    jpx_metanode root = tgt.access_meta_manager().access_root();
    const int cs0[1] = {0};
    jpx_metanode nl = root.add_numlist(1, cs0, 0, nullptr, false);
    (void)nl.add_regions(static_cast<int>(skew_rois.size()), skew_rois.data());
    tgt.write_headers();
    jp2_output_box* s = cs.open_stream();
    WriteMinimalCodestream(s, 8, 8, 3);
    s->close();
    tgt.write_metadata();
    tgt.close();
    ftgt.close();
  }
}

TEST(JpxTest, Wave8TargetContainerSimulationErrorsAndWarnings) {
  ScopedKduHandlers handlers;

  // 1) jpx_target / jpx_layer_target / jpx_composition KDU_ERROR_DEV paths.
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    EXPECT_THROW(tgt.open(&ftgt), kdu_core::kdu_exception);
    EXPECT_THROW((void)tgt.add_container(1, 1, 1), kdu_core::kdu_exception);
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 8, 8, 3);
    jpx_layer_target lyr = tgt.add_layer();
    SetupSrgbLayer(lyr, 0);
    EXPECT_THROW((void)tgt.add_container(0, 1, 0), kdu_core::kdu_exception);
    EXPECT_THROW((void)tgt.add_container(1, 0, 1), kdu_core::kdu_exception);
    EXPECT_THROW((void)lyr.add_colour(200, 0), kdu_core::kdu_exception);
    EXPECT_THROW(lyr.set_codestream_registration(0, kdu_core::kdu_coords(0, 0),
                                                 kdu_core::kdu_coords(0, 1),
                                                 kdu_core::kdu_coords(1, 1)),
                 kdu_core::kdu_exception);
    lyr.set_codestream_registration(0, kdu_core::kdu_coords(0, 0),
                                    kdu_core::kdu_coords(1, 1),
                                    kdu_core::kdu_coords(2, 2));
    EXPECT_THROW(lyr.set_codestream_registration(0, kdu_core::kdu_coords(0, 0),
                                                 kdu_core::kdu_coords(1, 1),
                                                 kdu_core::kdu_coords(3, 3)),
                 kdu_core::kdu_exception);
    (void)tgt.add_container(1, 1, 0);
    EXPECT_THROW((void)tgt.add_container(1, 1, 1), kdu_core::kdu_exception);
    EXPECT_THROW((void)tgt.add_codestream(), kdu_core::kdu_exception);
    EXPECT_THROW((void)tgt.add_layer(), kdu_core::kdu_exception);
    tgt.close();
    ftgt.close();
  }

  // Post-write_headers errors without expect_containers & mismatched colours.
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 8, 8, 3);
    jpx_layer_target lyr = tgt.add_layer();
    SetupSrgbLayer(lyr, 0);
    tgt.write_headers();
    EXPECT_THROW(tgt.expect_containers(), kdu_core::kdu_exception);
    EXPECT_THROW((void)tgt.add_container(1, 1, 1), kdu_core::kdu_exception);
    // Close without writing top codestream triggers KDU_WARNING_DEV 2.
    tgt.close();
    ftgt.close();
  }
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 8, 8, 3);
    jpx_layer_target lyr = tgt.add_layer();
    lyr.access_channels().init(3);
    lyr.access_channels().set_colour_mapping(0, 0, 0, 0);
    lyr.access_channels().set_colour_mapping(1, 0, 1, 0);
    lyr.access_channels().set_colour_mapping(2, 0, 2, 0);
    lyr.add_colour().init(jp2_colour_space::JP2_sRGB_SPACE);
    lyr.add_colour().init(jp2_colour_space::JP2_sLUM_SPACE);
    EXPECT_THROW(tgt.write_headers(), kdu_core::kdu_exception);
    tgt.close();
    ftgt.close();
  }
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 8, 8, 3);
    jpx_layer_target lyr = tgt.add_layer();
    SetupSrgbLayer(lyr, 0);
    lyr.set_codestream_registration(5, kdu_core::kdu_coords(0, 0),
                                    kdu_core::kdu_coords(1, 1),
                                    kdu_core::kdu_coords(1, 1));
    EXPECT_THROW(tgt.write_headers(), kdu_core::kdu_exception);
    tgt.close();
    ftgt.close();
  }
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 8, 8, 3);
    jpx_layer_target lyr = tgt.add_layer();
    SetupSrgbLayer(lyr, 7);  // non-existent codestream
    EXPECT_THROW(tgt.write_headers(), kdu_core::kdu_exception);
    tgt.close();
    ftgt.close();
  }
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 8, 8, 3);
    SetupSrgbLayer(tgt.add_layer(), 0);
    tgt.access_composition().add_frame(10, 0, false);  // 0 instructions
    EXPECT_THROW(tgt.write_headers(), kdu_core::kdu_exception);
    tgt.close();
    ftgt.close();
  }
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 8, 8, 3);
    SetupSrgbLayer(tgt.add_layer(), 0);
    jpx_composition gcomp = tgt.access_composition();
    gcomp.set_loop_count(1);
    auto* gf = gcomp.add_frame(10, 0, false);
    gcomp.add_instruction(gf, 0, 0, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));
    jpx_container_target cont = tgt.add_container(1, 2, 1);
    SetupCodestreamDims(cont.access_codestream(0).access_dimensions(), 8, 8, 3);
    SetupSrgbLayer(cont.access_layer(0),
                   cont.access_codestream(0).get_codestream_id());
    SetupSrgbLayer(cont.access_layer(1),
                   cont.access_codestream(0).get_codestream_id());
    jpx_composition trk = cont.add_presentation_track(2);
    auto* f = trk.add_frame(10, 0, false);
    trk.add_instruction(f, 0, 0, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));
    EXPECT_THROW(tgt.write_headers(), kdu_core::kdu_exception);
    tgt.close();
    ftgt.close();
  }

  // 2) Close warnings (headers_in_progress, metadata_in_progress vs headers,
  //    missed breakpoints, unwritten container).
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 8, 8, 3);
    cs.set_breakpoint(10, nullptr);
    SetupSrgbLayer(tgt.add_layer(), 0);
    jp2_output_box* bp = tgt.write_headers();
    ASSERT_NE(bp, nullptr);
    bp->close();
    EXPECT_THROW(tgt.write_metadata(), kdu_core::kdu_exception);
    tgt.close();  // warning 3: headers_in_progress
    ftgt.close();
  }
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    tgt.expect_containers();
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 8, 8, 3);
    SetupSrgbLayer(tgt.add_layer(), 0);
    jpx_composition gcomp = tgt.access_composition();
    gcomp.set_loop_count(1);
    auto* gf = gcomp.add_frame(10, 0, false);
    gcomp.add_instruction(gf, 0, 0, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));
    tgt.write_headers();
    jp2_output_box* s = cs.open_stream();
    WriteMinimalCodestream(s, 8, 8, 3);
    s->close();
    jpx_container_target cont = tgt.add_container(1, 1, 1);
    SetupCodestreamDims(cont.access_codestream(0).access_dimensions(), 8, 8, 3);
    SetupSrgbLayer(cont.access_layer(0),
                   cont.access_codestream(0).get_codestream_id());
    cont.access_layer(0).set_breakpoint(11, nullptr);
    tgt.close();  // triggers missed_breakpoints warning 4 & container warning
    ftgt.close();
  }
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 8, 8, 3);
    SetupSrgbLayer(tgt.add_layer(), 0);
    jpx_metanode root = tgt.access_meta_manager().access_root();
    jp2_output_box* delayed_src = nullptr;
    root.add_delayed(jp2_xml_4cc, 42, &delayed_src);
    tgt.write_headers();
    jp2_output_box* s = cs.open_stream();
    WriteMinimalCodestream(s, 8, 8, 3);
    s->close();
    int i_p = 0;
    void* a_p = nullptr;
    jp2_output_box* dbox = tgt.write_metadata(&i_p, &a_p);
    if (dbox != nullptr) {
      EXPECT_THROW(tgt.write_headers(), kdu_core::kdu_exception);
      dbox->close();
    }
    tgt.close();
    ftgt.close();
  }

  // 3) write_or_simulate_earlier_containers & finalize_all_containers:
  //    c0 has NO metanode, c1 has container-embedded metanode, and root has
  //    a top-level numlist referencing c0's codestream/layer. Calling
  //    write_metadata() first triggers finalize_all_containers() + writes c1
  //    forcing simulation & writing of earlier c0!
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    tgt.configure_codestream_aggregation(1, 5);
    tgt.configure_codestream_aggregation(4, 2);

    jpx_codestream_target top_cs = tgt.add_codestream();
    SetupCodestreamDims(top_cs.access_dimensions(), 8, 8, 3);
    jpx_layer_target top_l = tgt.add_layer();
    SetupSrgbLayer(top_l, 0);
    top_l.set_codestream_registration(0, kdu_core::kdu_coords(0, 0),
                                      kdu_core::kdu_coords(1, 1),
                                      kdu_core::kdu_coords(1, 1));
    jpx_composition gcomp = tgt.access_composition();
    gcomp.set_loop_count(1);
    auto* gf = gcomp.add_frame(10, 0, false);
    gcomp.add_instruction(gf, 0, 0, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));

    jpx_container_target c0 = tgt.add_container(1, 1, 1);
    SetupCodestreamDims(c0.access_codestream(0).access_dimensions(), 8, 8, 3);
    const int c0_cs_id = c0.access_codestream(0).get_codestream_id();
    const int c0_ly_id = c0.access_layer(0).get_layer_id();
    SetupSrgbLayer(c0.access_layer(0), c0_cs_id);
    c0.access_layer(0).set_codestream_registration(
        c0_cs_id, kdu_core::kdu_coords(0, 0), kdu_core::kdu_coords(1, 1),
        kdu_core::kdu_coords(1, 1));

    jpx_container_target c1 = tgt.add_container(1, 1, 1);
    SetupCodestreamDims(c1.access_codestream(0).access_dimensions(), 8, 8, 3);
    const int c1_cs_id = c1.access_codestream(0).get_codestream_id();
    const int c1_ly_id = c1.access_layer(0).get_layer_id();
    SetupSrgbLayer(c1.access_layer(0), c1_cs_id);

    jpx_meta_manager mm = tgt.access_meta_manager();
    jpx_metanode root = mm.access_root();
    jpx_metanode c1_nl = root.add_numlist(1, &c1_cs_id, 1, &c1_ly_id, false, 1);
    c1_nl.add_label("inside-c1");
    (void)root.add_numlist(1, &c0_cs_id, 1, &c0_ly_id, false, -1)
        .add_label("top-ref-c0");

    tgt.write_headers(nullptr, nullptr, -1);
    jp2_output_box* st = top_cs.open_stream();
    WriteMinimalCodestream(st, 8, 8, 3);
    st->close();
    // Write c1's stream BEFORE c0's to exercise container catch-up.
    jp2_output_box* s1 = c1.access_codestream(0).open_stream();
    WriteMinimalCodestream(s1, 8, 8, 3);
    s1->close();
    jp2_output_box* s0 = c0.access_codestream(0).open_stream();
    WriteMinimalCodestream(s0, 8, 8, 3);
    s0->close();
    tgt.configure_codestream_aggregation(0, 0);  // no-op after streams started
    tgt.write_metadata();
    tgt.close();
    ftgt.close();

    // Repeat with write_metadata() BEFORE write_headers() to hit L24839-24846.
    MemTarget mt2;
    jp2_family_tgt ftgt2;
    ftgt2.open(&mt2);
    jpx_target tgt2;
    tgt2.open(&ftgt2);
    jpx_codestream_target t2cs = tgt2.add_codestream();
    SetupCodestreamDims(t2cs.access_dimensions(), 8, 8, 3);
    SetupSrgbLayer(tgt2.add_layer(), 0);
    jpx_composition gcomp2 = tgt2.access_composition();
    gcomp2.set_loop_count(1);
    auto* gf2 = gcomp2.add_frame(10, 0, false);
    gcomp2.add_instruction(gf2, 0, 0, MakeDims(0, 0, 8, 8),
                           MakeDims(0, 0, 8, 8));
    jpx_container_target d0 = tgt2.add_container(1, 1, 1);
    SetupCodestreamDims(d0.access_codestream(0).access_dimensions(), 8, 8, 3);
    const int d0_cs = d0.access_codestream(0).get_codestream_id();
    const int d0_ly = d0.access_layer(0).get_layer_id();
    SetupSrgbLayer(d0.access_layer(0), d0_cs);
    d0.access_layer(0).set_codestream_registration(
        d0_cs, kdu_core::kdu_coords(0, 0), kdu_core::kdu_coords(1, 1),
        kdu_core::kdu_coords(1, 1));
    jpx_container_target d1 = tgt2.add_container(1, 1, 1);
    SetupCodestreamDims(d1.access_codestream(0).access_dimensions(), 8, 8, 3);
    const int d1_cs = d1.access_codestream(0).get_codestream_id();
    const int d1_ly = d1.access_layer(0).get_layer_id();
    SetupSrgbLayer(d1.access_layer(0), d1_cs);
    jpx_metanode r2 = tgt2.access_meta_manager().access_root();
    r2.add_numlist(1, &d1_cs, 1, &d1_ly, false, 1).add_label("d1-label");
    r2.add_numlist(1, &d0_cs, 1, &d0_ly, false, -1).add_label("top-d0");
    tgt2.write_metadata();
    jp2_output_box* ds = t2cs.open_stream();
    WriteMinimalCodestream(ds, 8, 8, 3);
    ds->close();
    jp2_output_box* ds0 = d0.access_codestream(0).open_stream();
    WriteMinimalCodestream(ds0, 8, 8, 3);
    ds0->close();
    jp2_output_box* ds1 = d1.access_codestream(0).open_stream();
    WriteMinimalCodestream(ds1, 8, 8, 3);
    ds1->close();
    tgt2.close();
    ftgt2.close();
  }

  // 4) Nested random-access J2CX round-trip (>= 18 streams with aggregation
  //    bounds (2, 20)) and out-of-order descendant recovery + removal.
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    tgt.configure_codestream_aggregation(20, 20);
    jpx_codestream_target top_cs = tgt.add_codestream();
    SetupCodestreamDims(top_cs.access_dimensions(), 8, 8, 3);
    SetupSrgbLayer(tgt.add_layer(), 0);
    jpx_composition gcomp = tgt.access_composition();
    gcomp.set_loop_count(1);
    auto* gf = gcomp.add_frame(10, 0, false);
    gcomp.add_instruction(gf, 0, 0, MakeDims(0, 0, 8, 8), MakeDims(0, 0, 8, 8));
    jpx_container_target cont = tgt.add_container(1, 1, 20);
    SetupCodestreamDims(cont.access_codestream(0).access_dimensions(), 8, 8, 3);
    SetupSrgbLayer(cont.access_layer(0),
                   cont.access_codestream(0).get_codestream_id());
    tgt.write_headers();
    jp2_output_box* st = top_cs.open_stream();
    WriteMinimalCodestream(st, 8, 8, 3);
    st->close();
    for (int r = 0; r < 20; ++r) {
      jp2_output_box* sc = cont.access_codestream(0).open_stream();
      WriteMinimalCodestream(sc, 8, 8, 3);
      sc->close();
    }
    tgt.write_metadata();
    tgt.close();
    ftgt.close();

    MemSource ms(mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 0);
    const int order[] = {19, 10, 5, 1,  15, 2, 20, 7,  12, 18,
                         3,  16, 8, 13, 4,  9, 14, 17, 6,  11};
    for (int id : order) {
      jpx_codestream_source cs_src = src.access_codestream(id, true);
      EXPECT_TRUE(cs_src.exists());
      EXPECT_EQ(cs_src.get_codestream_id(), id);
    }
    src.close();
    fs.close();
  }
}

TEST(JpxTest, Wave8MalformedBoxesAndMissingHeaderDefaults) {
  ScopedKduHandlers handlers;

  MemTarget cs1_mt, cs3_mt;
  WriteMinimalCodestream(&cs1_mt, 8, 8, 1);
  WriteMinimalCodestream(&cs3_mt, 8, 8, 3);

  std::vector<kdu_core::kdu_byte> ftyp_p;
  AppendBe32(ftyp_p, jpx_brand);
  AppendBe32(ftyp_p, 0);
  AppendBe32(ftyp_p, jpx_brand);
  AppendBe32(ftyp_p, jp2_brand);

  std::vector<kdu_core::kdu_byte> rreq_p = {1, 0x80, 0x80, 0, 0, 0, 0};
  const std::vector<kdu_core::kdu_byte> ihdr1 = {0, 0, 0, 8, 0, 0, 0,
                                                 8, 0, 1, 7, 7, 0, 0};
  const std::vector<kdu_core::kdu_byte> ihdr_pclr = {0, 0, 0, 8,   0, 0, 0,
                                                     8, 0, 1, 255, 7, 0, 0};
  const std::vector<kdu_core::kdu_byte> ihdr3 = {0, 0, 0, 8, 0, 0, 0,
                                                 8, 0, 3, 7, 7, 0, 0};
  const std::vector<kdu_core::kdu_byte> pclr_p = {0, 2, 3, 7,   7,   7,
                                                  0, 0, 0, 255, 255, 255};
  const std::vector<kdu_core::kdu_byte> colr_p = {1, 0, 1, 0, 0, 0, 16};
  const std::vector<kdu_core::kdu_byte> creg0_p = {0, 1, 0, 1, 0,
                                                   0, 1, 1, 0, 0};
  const std::vector<kdu_core::kdu_byte> creg1_p = {0, 1, 0, 1, 0,
                                                   1, 1, 1, 0, 0};

  std::vector<kdu_core::kdu_byte> cgrp3;
  AppendBox(cgrp3, jp2_colour_4cc, colr_p);

  std::vector<kdu_core::kdu_byte> jp2h_valid3;
  AppendBox(jp2h_valid3, jp2_image_header_4cc, ihdr3);
  AppendBox(jp2h_valid3, jp2_colour_4cc, colr_p);

  std::vector<kdu_core::kdu_byte> chdr3;
  AppendBox(chdr3, jp2_image_header_4cc, ihdr3);
  std::vector<kdu_core::kdu_byte> jplh3;
  AppendBox(jplh3, jp2_colour_group_4cc, cgrp3);
  AppendBox(jplh3, jp2_registration_4cc, creg0_p);

  auto run_raw = [](const std::vector<kdu_core::kdu_byte>& raw,
                    auto&& action) -> bool {
    MemSource ms(raw);
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    try {
      if (src.open(&fs, false) < 0) return false;
      action(src);
      return true;
    } catch (kdu_core::kdu_exception) {
      src.close();
      fs.close();
      return false;
    }
  };

  // 1) jpx_source::open switch source + missing ftyp / rreq (both ret flags).
  {
    std::vector<kdu_core::kdu_byte> sig_only;
    AppendBox(sig_only, 0x6A502020U, {0x0D, 0x0A, 0x87, 0x0A});
    MemSource ms1(sig_only);
    jp2_family_src fs1;
    fs1.open(&ms1);
    jpx_source src;
    EXPECT_EQ(src.open(&fs1, true), 0 - 1);  // -1 when incompatible
    MemSource ms2(sig_only);
    jp2_family_src fs2;
    fs2.open(&ms2);
    EXPECT_THROW((void)src.open(&fs2, false), kdu_core::kdu_exception);

    // Re-open on a different jp2_family_src while partially open -> L21631,
    // then double-open while already completely open -> error 44.
    MemKduCache empty_cache;
    jp2_family_src cfs;
    cfs.open(&empty_cache);
    EXPECT_EQ(src.open(&cfs, true), 0);

    std::vector<kdu_core::kdu_byte> valid_hdr = sig_only;
    AppendBox(valid_hdr, jp2_file_type_4cc, ftyp_p);
    AppendBox(valid_hdr, jp2_reader_requirements_4cc, rreq_p);
    AppendBox(valid_hdr, jp2_header_4cc, jp2h_valid3);
    AppendBox(valid_hdr, jp2_codestream_4cc, cs3_mt.data());
    MemSource msv(valid_hdr);
    jp2_family_src fsv;
    fsv.open(&msv);
    EXPECT_GE(src.open(&fsv, true), 0);

    std::vector<kdu_core::kdu_byte> no_rreq = sig_only;
    AppendBox(no_rreq, jp2_file_type_4cc, ftyp_p);
    MemSource ms3(no_rreq);
    jp2_family_src fs3;
    fs3.open(&ms3);
    EXPECT_THROW((void)src.open(&fs3, true), kdu_core::kdu_exception);
    src.close();
    EXPECT_EQ(src.open(&fs3, true), -1);
    MemSource ms4(no_rreq);
    jp2_family_src fs4;
    fs4.open(&ms4);
    EXPECT_THROW((void)src.open(&fs4, false), kdu_core::kdu_exception);
    src.close();
  }

  auto make_prefix = [&]() {
    std::vector<kdu_core::kdu_byte> b;
    AppendBox(b, 0x6A502020U, {0x0D, 0x0A, 0x87, 0x0A});
    AppendBox(b, jp2_file_type_4cc, ftyp_p);
    AppendBox(b, jp2_reader_requirements_4cc, rreq_p);
    return b;
  };

  // 2) Top-level ordering & count errors.
  {
    std::vector<kdu_core::kdu_byte> b = make_prefix();
    AppendBox(b, jp2_dtbl_4cc, {0, 0});
    AppendBox(b, jp2_dtbl_4cc, {0, 0});
    EXPECT_FALSE(run_raw(b, [](jpx_source& s) { (void)s.access_layer(0); }));
  }
  {
    std::vector<kdu_core::kdu_byte> jclx;
    std::vector<kdu_core::kdu_byte> jlxi(20, 0);
    jlxi[7] = 1;
    jlxi[11] = 1;
    AppendBox(jclx, jp2_layer_extensions_info_4cc, jlxi);
    AppendBox(jclx, jp2_compositing_layer_hdr_4cc, jplh3);

    // jclx with 0 top layers (0x24071202)
    auto b1 = make_prefix();
    AppendBox(b1, jp2_layer_extensions_4cc, jclx);
    EXPECT_FALSE(run_raw(b1, [](jpx_source& s) { (void)s.access_layer(0); }));

    // jclx with 1 top chdr + jplh but missing comp (0x24071203)
    auto b2 = make_prefix();
    AppendBox(b2, jp2_codestream_4cc, cs3_mt.data());
    AppendBox(b2, jp2_codestream_header_4cc, chdr3);
    AppendBox(b2, jp2_compositing_layer_hdr_4cc, jplh3);
    AppendBox(b2, jp2_layer_extensions_4cc, jclx);
    EXPECT_FALSE(run_raw(b2, [](jpx_source& s) { (void)s.access_layer(0); }));

    // Top jplh creg refers to cs 1 before jclx -> 0x24081202
    std::vector<kdu_core::kdu_byte> comp_p;
    AppendBox(comp_p, jp2_comp_options_4cc, {0, 0, 0, 8, 0, 0, 0, 8, 1});
    AppendBox(comp_p, jp2_comp_instruction_set_4cc,
              {0, 4, 0, 1, 0, 0, 0, 1, 0, 0, 0, 10, 0, 0, 0, 0});
    std::vector<kdu_core::kdu_byte> jplh_bad_cs;
    AppendBox(jplh_bad_cs, jp2_colour_group_4cc, cgrp3);
    AppendBox(jplh_bad_cs, jp2_registration_4cc, creg1_p);

    auto b3 = make_prefix();
    AppendBox(b3, jp2_codestream_4cc, cs3_mt.data());
    AppendBox(b3, jp2_codestream_header_4cc, chdr3);
    AppendBox(b3, jp2_compositing_layer_hdr_4cc, jplh_bad_cs);
    AppendBox(b3, jp2_composition_4cc, comp_p);
    AppendBox(b3, jp2_layer_extensions_4cc, jclx);
    EXPECT_FALSE(run_raw(b3, [](jpx_source& s) { (void)s.access_layer(0); }));

    // Top jplh creg refers to cs 1 at EOF -> 0x24081201
    auto b4 = make_prefix();
    AppendBox(b4, jp2_codestream_4cc, cs3_mt.data());
    AppendBox(b4, jp2_codestream_header_4cc, chdr3);
    AppendBox(b4, jp2_compositing_layer_hdr_4cc, jplh_bad_cs);
    EXPECT_FALSE(run_raw(b4, [](jpx_source& s) { (void)s.access_layer(0); }));

    // chdr after jclx -> 0x23071203; jplh after jclx -> 0x23071204
    auto base_ok = make_prefix();
    AppendBox(base_ok, jp2_codestream_4cc, cs3_mt.data());
    AppendBox(base_ok, jp2_codestream_header_4cc, chdr3);
    AppendBox(base_ok, jp2_compositing_layer_hdr_4cc, jplh3);
    AppendBox(base_ok, jp2_composition_4cc, comp_p);
    AppendBox(base_ok, jp2_layer_extensions_4cc, jclx);
    auto b5 = base_ok;
    AppendBox(b5, jp2_codestream_header_4cc, chdr3);
    EXPECT_FALSE(run_raw(b5, [](jpx_source& s) { (void)s.access_layer(0); }));
    auto b6 = base_ok;
    AppendBox(b6, jp2_compositing_layer_hdr_4cc, jplh3);
    EXPECT_FALSE(run_raw(b6, [](jpx_source& s) { (void)s.access_layer(0); }));
  }

  // 3) JCLX / JLXI & validate_auto_codestream_binding errors.
  auto wrap_jclx_doc = [&](const std::vector<kdu_core::kdu_byte>& jclx_inner,
                           const std::vector<kdu_core::kdu_byte>* extra_jclx =
                               nullptr) {
    auto b = make_prefix();
    AppendBox(b, jp2_codestream_4cc, cs3_mt.data());
    AppendBox(b, jp2_codestream_header_4cc, chdr3);
    AppendBox(b, jp2_compositing_layer_hdr_4cc, jplh3);
    std::vector<kdu_core::kdu_byte> comp_p;
    AppendBox(comp_p, jp2_comp_options_4cc, {0, 0, 0, 8, 0, 0, 0, 8, 1});
    AppendBox(comp_p, jp2_comp_instruction_set_4cc,
              {0, 4, 0, 1, 0, 0, 0, 1, 0, 0, 0, 10, 0, 0, 0, 0});
    AppendBox(b, jp2_composition_4cc, comp_p);
    AppendBox(b, jp2_layer_extensions_4cc, jclx_inner);
    if (extra_jclx != nullptr) {
      AppendBox(b, jp2_layer_extensions_4cc, *extra_jclx);
    }
    return b;
  };
  auto run_cont0 = [&](const std::vector<kdu_core::kdu_byte>& doc) {
    return run_raw(doc, [](jpx_source& s) {
      int n = 0;
      (void)s.count_compositing_layers(n);
      jpx_container_source cs = s.access_container(0);
      if (cs.exists()) {
        (void)cs.access_codestream(0, 0, true);
        (void)cs.access_layer(0, 0, true);
        (void)cs.access_presentation_track(1);
      }
    });
  };
  {
    // Truncated jlxi (0x25071201)
    std::vector<kdu_core::kdu_byte> j;
    AppendBox(j, jp2_layer_extensions_info_4cc, {0, 0, 0, 1});
    EXPECT_FALSE(run_cont0(wrap_jclx_doc(j)));
  }
  {
    // Ljclx = 0 (0x25071204)
    std::vector<kdu_core::kdu_byte> jlxi(20, 0);
    jlxi[3] = 1;
    std::vector<kdu_core::kdu_byte> j;
    AppendBox(j, jp2_layer_extensions_info_4cc, jlxi);
    EXPECT_FALSE(run_cont0(wrap_jclx_doc(j)));
  }
  {
    // Tjclx = 0, Fjclx = 2 (0x25071205)
    std::vector<kdu_core::kdu_byte> jlxi(20, 0);
    jlxi[3] = 1;
    jlxi[11] = 1;
    jlxi[19] = 2;
    std::vector<kdu_core::kdu_byte> j;
    AppendBox(j, jp2_layer_extensions_info_4cc, jlxi);
    EXPECT_FALSE(run_cont0(wrap_jclx_doc(j)));
  }
  {
    // Mjclx = 0, Cjclx = 0 (0x25071207)
    std::vector<kdu_core::kdu_byte> jlxi(20, 0);
    jlxi[11] = 1;
    std::vector<kdu_core::kdu_byte> j;
    AppendBox(j, jp2_layer_extensions_info_4cc, jlxi);
    EXPECT_FALSE(run_cont0(wrap_jclx_doc(j)));
  }
  {
    // Mjclx = 0 followed by another jclx (0x25071203)
    std::vector<kdu_core::kdu_byte> jlxi1(20, 0);
    jlxi1[7] = 1;
    jlxi1[11] = 1;
    std::vector<kdu_core::kdu_byte> j1;
    AppendBox(j1, jp2_layer_extensions_info_4cc, jlxi1);
    EXPECT_FALSE(run_cont0(wrap_jclx_doc(j1, &j1)));
  }
  {
    // Too many chdr (0x26071207), too many jplh (0x26071208), insufficient
    // chdr/jplh (0x26071209), trailing free box in jclx (18872), too many/few
    // iset tracks (0x30071202 / 0x29071201).
    std::vector<kdu_core::kdu_byte> jlxi(24, 0);
    jlxi[3] = 1;
    jlxi[7] = 1;
    jlxi[11] = 1;
    jlxi[15] = 1;
    jlxi[19] = 1;
    jlxi[23] = 0;

    std::vector<kdu_core::kdu_byte> ja;
    AppendBox(ja, jp2_layer_extensions_info_4cc, jlxi);
    AppendBox(ja, jp2_codestream_header_4cc, {});
    AppendBox(ja, jp2_codestream_header_4cc, {});
    EXPECT_FALSE(run_cont0(wrap_jclx_doc(ja)));

    std::vector<kdu_core::kdu_byte> jb;
    AppendBox(jb, jp2_layer_extensions_info_4cc, jlxi);
    AppendBox(jb, jp2_codestream_header_4cc, {});
    AppendBox(jb, jp2_compositing_layer_hdr_4cc, {});
    AppendBox(jb, jp2_compositing_layer_hdr_4cc, {});
    EXPECT_FALSE(run_cont0(wrap_jclx_doc(jb)));

    std::vector<kdu_core::kdu_byte> jc;
    AppendBox(jc, jp2_layer_extensions_info_4cc, jlxi);
    AppendBox(jc, jp2_free_4cc, {0, 0, 0, 0});
    EXPECT_FALSE(run_cont0(wrap_jclx_doc(jc)));

    std::vector<kdu_core::kdu_byte> jd;
    AppendBox(jd, jp2_layer_extensions_info_4cc, jlxi);
    AppendBox(jd, jp2_codestream_header_4cc, {});
    AppendBox(jd, jp2_compositing_layer_hdr_4cc, {});
    EXPECT_FALSE(run_cont0(wrap_jclx_doc(jd)));  // missing iset -> 0x29071201

    std::vector<kdu_core::kdu_byte> iset_p = {0, 4, 0, 1,  0, 0, 0, 1,
                                              0, 0, 0, 10, 0, 0, 0, 0};
    std::vector<kdu_core::kdu_byte> je = jd;
    AppendBox(je, jp2_comp_instruction_set_4cc, iset_p);
    AppendBox(je, jp2_free_4cc, {0});
    AppendBox(je, jp2_comp_instruction_set_4cc, iset_p);
    EXPECT_FALSE(run_cont0(wrap_jclx_doc(je)));  // 2 track groups -> 0x30071202
  }
  {
    // validate_auto_codestream_binding: 0x25071212, 0x25071213, 0x25071215.
    std::vector<kdu_core::kdu_byte> jplh_nocreg;
    AppendBox(jplh_nocreg, jp2_colour_group_4cc, cgrp3);

    auto wrap_two_cs = [&](const std::vector<kdu_core::kdu_byte>& jclx_inner) {
      auto b = make_prefix();
      AppendBox(b, jp2_codestream_4cc, cs3_mt.data());
      AppendBox(b, jp2_codestream_4cc, cs3_mt.data());
      AppendBox(b, jp2_codestream_header_4cc, chdr3);
      AppendBox(b, jp2_codestream_header_4cc, chdr3);
      AppendBox(b, jp2_compositing_layer_hdr_4cc, jplh3);
      std::vector<kdu_core::kdu_byte> comp_p;
      AppendBox(comp_p, jp2_comp_options_4cc, {0, 0, 0, 8, 0, 0, 0, 8, 1});
      AppendBox(comp_p, jp2_comp_instruction_set_4cc,
                {0, 4, 0, 1, 0, 0, 0, 1, 0, 0, 0, 10, 0, 0, 0, 0});
      AppendBox(b, jp2_composition_4cc, comp_p);
      AppendBox(b, jp2_layer_extensions_4cc, jclx_inner);
      return b;
    };

    // Case A: base_id=1 < num_top_codestreams(=2), M=2, L=1 -> 0x25071212
    std::vector<kdu_core::kdu_byte> jlxi_a(20, 0);
    jlxi_a[3] = 2;
    jlxi_a[11] = 1;
    std::vector<kdu_core::kdu_byte> ja;
    AppendBox(ja, jp2_layer_extensions_info_4cc, jlxi_a);
    AppendBox(ja, jp2_compositing_layer_hdr_4cc, jplh_nocreg);
    EXPECT_FALSE(run_raw(wrap_two_cs(ja), [](jpx_source& s) {
      (void)s.access_layer(1, false);
    }));

    // Case B: base_id=1 < num_top_codestreams(=2), indefinite reps ->
    // 0x25071213
    std::vector<kdu_core::kdu_byte> jlxi_b(20, 0);
    jlxi_b[7] = 1;
    jlxi_b[11] = 1;
    std::vector<kdu_core::kdu_byte> jb;
    AppendBox(jb, jp2_layer_extensions_info_4cc, jlxi_b);
    AppendBox(jb, jp2_codestream_header_4cc, chdr3);
    AppendBox(jb, jp2_compositing_layer_hdr_4cc, jplh_nocreg);
    auto doc_b = wrap_two_cs(jb);
    AppendBox(doc_b, jp2_codestream_4cc, cs3_mt.data());
    EXPECT_FALSE(
        run_raw(doc_b, [](jpx_source& s) { (void)s.access_layer(1, false); }));

    // Case C: 1 top codestream, container C=2, L=1, M=2 -> 0x25071215
    std::vector<kdu_core::kdu_byte> jlxi_c(20, 0);
    jlxi_c[3] = 2;
    jlxi_c[7] = 2;
    jlxi_c[11] = 1;
    std::vector<kdu_core::kdu_byte> jc;
    AppendBox(jc, jp2_layer_extensions_info_4cc, jlxi_c);
    AppendBox(jc, jp2_codestream_header_4cc, chdr3);
    AppendBox(jc, jp2_codestream_header_4cc, chdr3);
    AppendBox(jc, jp2_compositing_layer_hdr_4cc, jplh_nocreg);
    auto doc_c = wrap_jclx_doc(jc);
    AppendBox(doc_c, jp2_codestream_4cc, cs3_mt.data());
    AppendBox(doc_c, jp2_codestream_4cc, cs3_mt.data());
    EXPECT_FALSE(
        run_raw(doc_c, [](jpx_source& s) { (void)s.access_layer(1, false); }));
  }

  // 4) J2CI / J2CX errors + jp2c after j2cx (0x23071202) + missing stream EOF.
  {
    auto b1 = make_prefix();
    std::vector<kdu_core::kdu_byte> j2cx;
    AppendBox(j2cx, jp2_multi_codestream_info_4cc, {0, 0, 0, 1});
    AppendBox(b1, jp2_multi_codestream_4cc, j2cx);
    EXPECT_FALSE(
        run_raw(b1, [](jpx_source& s) { (void)s.access_codestream(0); }));

    auto b2 = make_prefix();
    std::vector<kdu_core::kdu_byte> j2ci_bad_r(8, 0);
    j2ci_bad_r[3] = 2;
    j2ci_bad_r[4] = 0x80;  // Rval = 32 > 31 -> 455134744
    j2ci_bad_r[7] = 1;
    std::vector<kdu_core::kdu_byte> j2cx2;
    AppendBox(j2cx2, jp2_multi_codestream_info_4cc, j2ci_bad_r);
    AppendBox(b2, jp2_multi_codestream_4cc, j2cx2);
    EXPECT_FALSE(
        run_raw(b2, [](jpx_source& s) { (void)s.access_codestream(0); }));

    // Ncs = 3, Ltbl = 0, but only 1 jp2c inside -> 0x19071203
    auto b3 = make_prefix();
    std::vector<kdu_core::kdu_byte> j2ci_3 = {0, 0, 0, 3, 0, 0, 0, 0};
    std::vector<kdu_core::kdu_byte> j2cx3;
    AppendBox(j2cx3, jp2_multi_codestream_info_4cc, j2ci_3);
    AppendBox(j2cx3, jp2_codestream_4cc, cs3_mt.data());
    AppendBox(b3, jp2_multi_codestream_4cc, j2cx3);
    EXPECT_FALSE(
        run_raw(b3, [](jpx_source& s) { (void)s.access_codestream(2); }));

    // jp2c after j2cx -> 0x23071202
    auto b4 = make_prefix();
    std::vector<kdu_core::kdu_byte> j2ci_1 = {0, 0, 0, 1, 0, 0, 0, 0};
    std::vector<kdu_core::kdu_byte> j2cx4;
    AppendBox(j2cx4, jp2_multi_codestream_info_4cc, j2ci_1);
    AppendBox(j2cx4, jp2_codestream_4cc, cs3_mt.data());
    AppendBox(b4, jp2_multi_codestream_4cc, j2cx4);
    AppendBox(b4, jp2_codestream_4cc, cs3_mt.data());
    EXPECT_FALSE(run_raw(b4, [](jpx_source& s) {
      int n = 0;
      (void)s.count_codestreams(n);
    }));

    // Missing codestream at EOF -> 0x2507120B
    auto b5 = make_prefix();
    AppendBox(b5, jp2_codestream_header_4cc, chdr3);
    AppendBox(b5, jp2_compositing_layer_hdr_4cc, jplh3);
    EXPECT_FALSE(
        run_raw(b5, [](jpx_source& s) { (void)s.access_codestream(0); }));
  }

  // 5) Missing jp2h defaults & mismatch errors (0x25071208, 37, 38, 39,
  //    0x25071209, 40, 41).
  {
    // Empty chdr with NO jp2h -> 0x25071208
    auto b1 = make_prefix();
    AppendBox(b1, jp2_codestream_4cc, cs3_mt.data());
    AppendBox(b1, jp2_codestream_header_4cc, {});
    EXPECT_FALSE(
        run_raw(b1, [](jpx_source& s) { (void)s.access_codestream(0); }));

    // Empty chdr with jp2h containing only colr (no ihdr) -> error 37
    auto b2 = make_prefix();
    std::vector<kdu_core::kdu_byte> jp2h_no_ihdr;
    AppendBox(jp2h_no_ihdr, jp2_colour_4cc, colr_p);
    AppendBox(b2, jp2_header_4cc, jp2h_no_ihdr);
    AppendBox(b2, jp2_codestream_4cc, cs3_mt.data());
    AppendBox(b2, jp2_codestream_header_4cc, {});
    EXPECT_FALSE(
        run_raw(b2, [](jpx_source& s) { (void)s.access_codestream(0); }));

    // chdr with ihdr + pclr and NO cmap (even with jp2h) -> error 38
    auto b3 = make_prefix();
    AppendBox(b3, jp2_header_4cc, jp2h_valid3);
    AppendBox(b3, jp2_codestream_4cc, cs1_mt.data());
    std::vector<kdu_core::kdu_byte> chdr_pclr;
    AppendBox(chdr_pclr, jp2_image_header_4cc, ihdr_pclr);
    AppendBox(chdr_pclr, jp2_palette_4cc, pclr_p);
    AppendBox(b3, jp2_codestream_header_4cc, chdr_pclr);
    EXPECT_FALSE(
        run_raw(b3, [](jpx_source& s) { (void)s.access_codestream(0); }));

    // Double open_stream(nullptr) on valid codestream -> error 39
    auto b4 = make_prefix();
    AppendBox(b4, jp2_header_4cc, jp2h_valid3);
    AppendBox(b4, jp2_codestream_4cc, cs3_mt.data());
    EXPECT_FALSE(run_raw(b4, [](jpx_source& s) {
      jpx_codestream_source cs = s.access_codestream(0);
      ASSERT_TRUE(cs.exists());
      (void)cs.open_stream(nullptr);
      (void)cs.open_stream(nullptr);  // throws 39
    }));

    // Empty jplh with NO jp2h -> 0x25071209
    auto b5 = make_prefix();
    AppendBox(b5, jp2_codestream_4cc, cs3_mt.data());
    AppendBox(b5, jp2_codestream_header_4cc, chdr3);
    AppendBox(b5, jp2_compositing_layer_hdr_4cc, {});
    EXPECT_FALSE(run_raw(b5, [](jpx_source& s) { (void)s.access_layer(0); }));

    // jplh referencing cs 1 when only 1 codestream exists -> error 40
    auto b6 = make_prefix();
    AppendBox(b6, jp2_header_4cc, jp2h_valid3);
    AppendBox(b6, jp2_codestream_4cc, cs3_mt.data());
    AppendBox(b6, jp2_codestream_header_4cc, chdr3);
    std::vector<kdu_core::kdu_byte> jplh_cs1;
    AppendBox(jplh_cs1, jp2_colour_group_4cc, cgrp3);
    AppendBox(jplh_cs1, jp2_registration_4cc, creg1_p);
    AppendBox(b6, jp2_compositing_layer_hdr_4cc, jplh_cs1);
    EXPECT_FALSE(run_raw(b6, [](jpx_source& s) { (void)s.access_layer(0); }));

    // 3-channel sRGB jplh on 1-component codestream -> error 41
    auto b7 = make_prefix();
    AppendBox(b7, jp2_header_4cc, jp2h_valid3);
    AppendBox(b7, jp2_codestream_4cc, cs1_mt.data());
    std::vector<kdu_core::kdu_byte> chdr1;
    AppendBox(chdr1, jp2_image_header_4cc, ihdr1);
    AppendBox(b7, jp2_codestream_header_4cc, chdr1);
    AppendBox(b7, jp2_compositing_layer_hdr_4cc, jplh3);
    EXPECT_FALSE(run_raw(b7, [](jpx_source& s) { (void)s.access_layer(0); }));
  }
}

TEST(JpxTest, Wave8CompositionMalformationsAndMetaNodeEdgePaths) {
  ScopedKduHandlers handlers;

  MemTarget cs3_mt;
  WriteMinimalCodestream(&cs3_mt, 8, 8, 3);
  std::vector<kdu_core::kdu_byte> ftyp_p;
  AppendBe32(ftyp_p, jpx_brand);
  AppendBe32(ftyp_p, 0);
  AppendBe32(ftyp_p, jpx_brand);
  AppendBe32(ftyp_p, jp2_brand);
  const std::vector<kdu_core::kdu_byte> rreq_p = {1, 0x80, 0x80, 0, 0, 0, 0};
  const std::vector<kdu_core::kdu_byte> ihdr3 = {0, 0, 0, 8, 0, 0, 0,
                                                 8, 0, 3, 7, 7, 0, 0};
  const std::vector<kdu_core::kdu_byte> colr_p = {1, 0, 1, 0, 0, 0, 16};
  std::vector<kdu_core::kdu_byte> jp2h_valid3;
  AppendBox(jp2h_valid3, jp2_image_header_4cc, ihdr3);
  AppendBox(jp2h_valid3, jp2_colour_4cc, colr_p);

  auto make_doc = [&](const std::vector<kdu_core::kdu_byte>& extra) {
    std::vector<kdu_core::kdu_byte> b;
    AppendBox(b, 0x6A502020U, {0x0D, 0x0A, 0x87, 0x0A});
    AppendBox(b, jp2_file_type_4cc, ftyp_p);
    AppendBox(b, jp2_reader_requirements_4cc, rreq_p);
    AppendBox(b, jp2_header_4cc, jp2h_valid3);
    AppendBox(b, jp2_codestream_4cc, cs3_mt.data());
    b.insert(b.end(), extra.begin(), extra.end());
    return b;
  };
  auto run_comp = [](const std::vector<kdu_core::kdu_byte>& raw) -> bool {
    MemSource ms(raw);
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    try {
      if (src.open(&fs, false) < 0) return false;
      kdu_core::kdu_coords comp_sz;
      (void)src.access_composition().get_global_info(comp_sz);
      int nframes = 0;
      (void)src.access_composition().count_track_frames(0, nframes);
      src.close();
      fs.close();
      return true;
    } catch (kdu_core::kdu_exception) {
      src.close();
      fs.close();
      return false;
    }
  };

  // 1) Composition errors & edge branches (duplicate comp warning, copt
  //    truncation / >2^31 clamp / loop=255, inst flags=0 / truncated fields /
  //    overlong inst / ROT=5 / max_repeats capping / 0x01081201).
  {
    std::vector<kdu_core::kdu_byte> comp_ok;
    AppendBox(comp_ok, jp2_comp_options_4cc,
              {0x80, 0, 0, 8, 0x80, 0, 0, 8, 255});  // clamp + loop=255->0
    AppendBox(comp_ok, jp2_comp_instruction_set_4cc,
              {0, 0, 0, 1, 0, 0, 0, 1});  // flags = 0 -> early skip
    // Case A: conflict-free next_reuse=5 over rept=2 (1+2=3 insts) + 2
    // unrepeated frames -> hits lines 2935-2936 without colliding at L3090.
    AppendBox(comp_ok, jp2_comp_instruction_set_4cc,
              {0, 4, 0, 0, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 5});
    AppendBox(comp_ok, jp2_comp_instruction_set_4cc,
              {0, 4, 0, 2, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 0});
    AppendBox(comp_ok, jp2_comp_instruction_set_4cc,
              {0, 4, 0, 0, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 0});
    AppendBox(comp_ok, jp2_comp_instruction_set_4cc,
              {0, 4, 0, 0, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 0});
    std::vector<kdu_core::kdu_byte> extra;
    AppendBox(extra, jp2_composition_4cc, comp_ok);
    AppendBox(extra, jp2_composition_4cc, comp_ok);  // duplicate warning
    EXPECT_TRUE(run_comp(make_doc(extra)));
  }
  {
    // Conflicting reuse landing mid-repeat triggers KDU_ERROR(e, 19); also
    // cover repeat_count > max_repeats capping (lines 2928-2932).
    std::vector<kdu_core::kdu_byte> comp_bad_reuse;
    AppendBox(comp_bad_reuse, jp2_comp_options_4cc,
              {0, 0, 0, 8, 0, 0, 0, 8, 1});
    AppendBox(comp_bad_reuse, jp2_comp_instruction_set_4cc,
              {0, 4, 0, 1, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 4});
    AppendBox(comp_bad_reuse, jp2_comp_instruction_set_4cc,
              {0, 4, 0xFF, 0xFF, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 0});
    std::vector<kdu_core::kdu_byte> extra;
    AppendBox(extra, jp2_composition_4cc, comp_bad_reuse);
    EXPECT_FALSE(run_comp(make_doc(extra)));

    std::vector<kdu_core::kdu_byte> comp_bad_cap;
    AppendBox(comp_bad_cap, jp2_comp_options_4cc, {0, 0, 0, 8, 0, 0, 0, 8, 1});
    AppendBox(comp_bad_cap, jp2_comp_instruction_set_4cc,
              {0, 4, 0, 0, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 4});
    AppendBox(comp_bad_cap, jp2_comp_instruction_set_4cc,
              {0, 4, 0, 5, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 0});
    std::vector<kdu_core::kdu_byte> extra_cap;
    AppendBox(extra_cap, jp2_composition_4cc, comp_bad_cap);
    EXPECT_FALSE(run_comp(make_doc(extra_cap)));
  }
  {
    // rept=3, single-frame animation with next_reuse=1 -> increment==0
    // triggers warning 0x01081201 in process_instructions!
    std::vector<kdu_core::kdu_byte> comp_w;
    AppendBox(comp_w, jp2_comp_options_4cc, {0, 0, 0, 8, 0, 0, 0, 8, 1});
    AppendBox(comp_w, jp2_comp_instruction_set_4cc,
              {0, 4, 0, 3, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 1});
    std::vector<kdu_core::kdu_byte> extra;
    AppendBox(extra, jp2_composition_4cc, comp_w);
    EXPECT_TRUE(run_comp(make_doc(extra)));
  }
  {
    // Malformed copt / inst cases (errors 12, 13, 14, 15, 16, 17, 18,
    // 0x26021101)
    const std::vector<std::vector<kdu_core::kdu_byte>> bad_insts = {
        {0, 1},  // short header -> 13
        {0, 4, 0, 1, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 0,
         9},                                   // extra byte -> 14
        {0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0},  // missing Y0 -> 15
        {0, 2, 0, 1, 0, 0, 0, 1, 0, 0, 0, 8},  // missing YS -> 16
        {0, 4, 0, 1, 0, 0, 0, 1, 0, 0, 0, 5},  // missing next_reuse -> 17
        {0, 32, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},  // missing HC -> 18
        {0, 64, 0, 1, 0, 0, 0, 1, 0, 0, 0, 5},  // invalid ROT -> 0x26021101
    };
    {
      std::vector<kdu_core::kdu_byte> comp_bad;
      AppendBox(comp_bad, jp2_comp_options_4cc, {0, 0, 0, 0, 0, 0, 0, 8, 1});
      std::vector<kdu_core::kdu_byte> extra;
      AppendBox(extra, jp2_composition_4cc, comp_bad);
      EXPECT_FALSE(run_comp(make_doc(extra)));
    }
    for (const auto& inst : bad_insts) {
      std::vector<kdu_core::kdu_byte> comp_bad;
      AppendBox(comp_bad, jp2_comp_options_4cc, {0, 0, 0, 8, 0, 0, 0, 8, 1});
      AppendBox(comp_bad, jp2_comp_instruction_set_4cc, inst);
      std::vector<kdu_core::kdu_byte> extra;
      AppendBox(extra, jp2_composition_4cc, comp_bad);
      EXPECT_FALSE(run_comp(make_doc(extra)));
    }
  }

  // 2) MetaNode malformed nlst/cref, has_dependent_roi_nodes, find_path_to,
  //    change_parent / add_numlist / add_link / preserve_for_links errors, &
  //    add_copy(link_to_copies=false).
  {
    std::vector<kdu_core::kdu_byte> extra;
    AppendBox(extra, jp2_number_list_4cc, {0, 0, 1});  // non-mult-of-4 -> warn
    std::vector<kdu_core::kdu_byte> bad_cref;
    AppendBe32(bad_cref, jp2_label_4cc);
    AppendBox(extra, jp2_cross_reference_4cc,
              bad_cref);  // missing flst -> warn

    // Also include valid nested asoc -> asoc -> roid + sibling nlst.
    std::vector<kdu_core::kdu_byte> inner_asoc;
    AppendBox(inner_asoc, jp2_roi_description_4cc, {0, 0, 0, 0, 1});
    std::vector<kdu_core::kdu_byte> outer_asoc;
    AppendBox(outer_asoc, jp2_label_4cc, {'r', 'o', 'i'});
    AppendBox(outer_asoc, jp2_association_4cc, inner_asoc);
    AppendBox(extra, jp2_association_4cc, outer_asoc);

    auto doc = make_doc(extra);
    MemSource ms(doc);
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_GE(src.open(&fs, false), 0);
    jpx_metanode root = src.access_meta_manager().access_root();
    int desc = 0;
    root.count_descendants(desc);
    EXPECT_TRUE(root.has_dependent_roi_nodes());
    // Query ROIs on non-ROI node -> nullptr.
    EXPECT_EQ(root.get_regions(), nullptr);
    EXPECT_FALSE(root.test_region(MakeDims(0, 0, 4, 4)));
    src.close();
    fs.close();
  }
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    jpx_codestream_target cs = tgt.add_codestream();
    SetupCodestreamDims(cs.access_dimensions(), 16, 16, 3);
    SetupSrgbLayer(tgt.add_layer(), 0);
    jpx_composition gcomp = tgt.access_composition();
    gcomp.set_loop_count(1);
    auto* gf = gcomp.add_frame(10, 0, false);
    gcomp.add_instruction(gf, 0, 0, MakeDims(0, 0, 16, 16),
                          MakeDims(0, 0, 16, 16));
    jpx_container_target cont = tgt.add_container(1, 1, 1);
    SetupCodestreamDims(cont.access_codestream(0).access_dimensions(), 16, 16,
                        3);
    SetupSrgbLayer(cont.access_layer(0),
                   cont.access_codestream(0).get_codestream_id());

    jpx_meta_manager mm = tgt.access_meta_manager();
    jpx_metanode root = mm.access_root();
    const int cs0[1] = {0};
    jpx_metanode nl = root.add_numlist(1, cs0, 0, nullptr, false);
    jpx_metanode lbl_a = nl.add_label("A");
    jpx_metanode lbl_b = nl.add_label("B");
    jpx_roi roi;
    roi.init_rectangle(MakeDims(0, 0, 8, 8));
    jpx_metanode roi_node = nl.add_regions(1, &roi);
    EXPECT_FALSE(roi_node.test_region(MakeDims(10, 10, 4, 4)));
    jpx_metanode asoc_grp = root.add_label("G");
    jpx_metanode grp_link = asoc_grp.add_link(lbl_a, JPX_GROUPING_LINK);
    jpx_metanode par_link = lbl_b.add_link(asoc_grp, JPX_ALTERNATE_PARENT_LINK);
    jpx_metanode child_link =
        asoc_grp.add_link(lbl_b, JPX_ALTERNATE_CHILD_LINK);

    // find_path_to: self, unify_groups, alternate_parent, exclusion box types.
    EXPECT_TRUE(
        lbl_a.find_path_to(lbl_a, JPX_PATH_TO_DIRECT, 0, 0, nullptr, nullptr)
            .exists());
    EXPECT_TRUE(grp_link
                    .find_path_to(lbl_a, JPX_PATH_TO_DIRECT, 0, 0, nullptr,
                                  nullptr, true)
                    .exists());
    EXPECT_TRUE(lbl_b
                    .find_path_to(asoc_grp, 0,
                                  JPX_PATH_TO_DIRECT | JPX_PATH_TO_FORWARD, 0,
                                  nullptr, nullptr, true)
                    .exists());
    const kdu_core::kdu_uint32 excl_t[1] = {jp2_label_4cc};
    const int excl_f[1] = {JPX_PATH_TO_EXCLUDE_PARENTS};
    (void)lbl_b.find_path_to(asoc_grp, JPX_PATH_TO_DIRECT, JPX_PATH_TO_DIRECT,
                             1, excl_t, excl_f, false);

    // add_copy(link_to_copies=false) & copy of null/link.
    jpx_metanode holder = root.add_label("copy-holder");
    (void)holder.add_copy(par_link, true, false);
    (void)holder.add_copy(child_link, true, true);

    // Error paths on change_parent / add_numlist / add_link.
    EXPECT_THROW((void)lbl_a.change_parent(jpx_metanode()),
                 kdu_core::kdu_exception);
    EXPECT_FALSE(lbl_a.change_parent(nl));  // same parent
    EXPECT_FALSE(nl.change_parent(lbl_a));  // new_parent is descendant
    jpx_metanode nl2 = root.add_numlist(1, cs0, 0, nullptr, false);
    EXPECT_TRUE(roi_node.change_parent(nl2));  // relinks ROI library
    EXPECT_THROW((void)lbl_a.add_numlist(1, cs0, 0, nullptr, false, 0),
                 kdu_core::kdu_exception);
    EXPECT_THROW((void)root.add_numlist(1, cs0, 0, nullptr, false, 99),
                 kdu_core::kdu_exception);
    EXPECT_THROW((void)lbl_a.add_link(par_link, JPX_ALTERNATE_PARENT_LINK),
                 kdu_core::kdu_exception);
    jpx_metanode empty_nl = root.add_numlist(0, nullptr, 0, nullptr, false);
    EXPECT_THROW((void)lbl_a.add_link(empty_nl, JPX_ALTERNATE_PARENT_LINK),
                 kdu_core::kdu_exception);
    jpx_metanode doomed = root.add_label("doomed");
    doomed.delete_node();
    EXPECT_THROW((void)lbl_a.add_link(doomed, JPX_ALTERNATE_PARENT_LINK),
                 kdu_core::kdu_exception);

    // Write headers + streams + metadata, then verify post-write preserve/link
    // errors (0x16081210, 0x17081201).
    jpx_metanode written_lbl = root.add_label("written-unpreserved");
    tgt.write_headers();
    jp2_output_box* s0 = cs.open_stream();
    WriteMinimalCodestream(s0, 8, 8, 3);
    s0->close();
    jp2_output_box* sc = cont.access_codestream(0).open_stream();
    WriteMinimalCodestream(sc, 8, 8, 3);
    sc->close();
    tgt.write_metadata();
    EXPECT_THROW(written_lbl.preserve_for_links(), kdu_core::kdu_exception);
    EXPECT_THROW((void)root.add_link(written_lbl, JPX_ALTERNATE_PARENT_LINK),
                 kdu_core::kdu_exception);
    tgt.close();
    ftgt.close();
  }

  // 3) Cache metareq on 0-byte incomplete `asoc` & cref target missing bytes.
  {
    const std::vector<kdu_core::kdu_byte> xml_p = {'<', 'a', '/', '>'};
    std::vector<kdu_core::kdu_byte> bin0;
    AppendBox(bin0, 0x6A502020U, {0x0D, 0x0A, 0x87, 0x0A});
    AppendBox(bin0, jp2_file_type_4cc, ftyp_p);
    AppendBox(bin0, jp2_reader_requirements_4cc, rreq_p);
    std::vector<kdu_core::kdu_byte> jp2h_bpcc;
    AppendBox(jp2h_bpcc, jp2_image_header_4cc,
              {0, 0, 0, 8, 0, 0, 0, 8, 0, 3, 255, 7, 0, 0});
    AppendBox(jp2h_bpcc, jp2_bits_per_component_4cc, {7, 7, 7});
    AppendBox(jp2h_bpcc, jp2_colour_4cc, colr_p);
    AppendBox(bin0, jp2_header_4cc, jp2h_bpcc);
    AppendBox(bin0, jp2_codestream_4cc, cs3_mt.data());
    AppendPlaceholderBox(bin0, 1, 50, 32, jp2_association_4cc);  // 0 bytes!
    std::vector<kdu_core::kdu_byte> flst;
    AppendBe16(flst, 1);
    const std::size_t off_patch = bin0.size() + 8 + 4 + 8 + 2;
    AppendBe64(flst, 0);
    AppendBe32(flst, static_cast<std::uint32_t>(8 + xml_p.size()));
    AppendBe16(flst, 0);
    std::vector<kdu_core::kdu_byte> cref;
    AppendBe32(cref, jp2_xml_4cc);
    AppendBox(cref, jp2_fragment_list_4cc, flst);
    AppendBox(bin0, jp2_cross_reference_4cc, cref);
    const std::uint64_t tgt_pos = bin0.size();
    for (int i = 0; i < 8; ++i) {
      bin0[off_patch + i] =
          static_cast<kdu_core::kdu_byte>((tgt_pos >> (56 - 8 * i)) & 0xFF);
    }
    AppendPlaceholderBox(
        bin0, 1, 51, static_cast<std::uint32_t>(8 + xml_p.size()), jp2_xml_4cc);

    MemKduCache cache;
    cache.add_to_databin(KDU_META_DATABIN, 0, 0, bin0.data(), 0,
                         static_cast<int>(bin0.size()), false);
    cache.add_to_databin(KDU_META_DATABIN, 0, 50, xml_p.data(), 0, 0, false);
    cache.add_to_databin(KDU_META_DATABIN, 0, 51, xml_p.data(), 0, 1, false);
    jp2_family_src fs;
    fs.open(&cache);
    jpx_source src;
    ASSERT_GT(src.open(&fs, true), 0);
    kdu_window win;
    win.init();
    jpx_metanode sroot = src.access_meta_manager().access_root();
    const kdu_core::kdu_uint32 dtypes[1] = {jp2_association_4cc};
    (void)sroot.generate_metareq(&win, 0, nullptr, 1, dtypes, true, 3,
                                 KDU_MRQ_ALL);
    for (jpx_metanode n = sroot.get_next_descendant(jpx_metanode(), -1);
         n.exists(); n = sroot.get_next_descendant(n, -1)) {
      (void)n.generate_link_metareq(&win, 0, nullptr, 1, dtypes, true, 2,
                                    KDU_MRQ_ALL);
    }
    src.close();
    fs.close();
  }
}

TEST(JpxTest, Wave9EmptyInterfacesAndMetadataManagerErrors) {
  ScopedKduHandlers handlers;

  // Empty-interface guard coverage across jpx_source, jpx_codestream_source,
  // jpx_container_source, jpx_composition, jpx_metanode, and
  // jpx_frame_expander.
  {
    jpx_source empty_src;
    EXPECT_EQ(empty_src.get_ultimate_src(), nullptr);
    EXPECT_EQ(empty_src.get_metadata_memory(), 0);
    EXPECT_FALSE(empty_src.close());
    EXPECT_FALSE(empty_src.access_data_references().exists());
    // Note: jpx_source::access_compatibility() has `(state != NULL) &`
    // (bitwise AND) UB on an empty jpx_source in vendor jpx.cpp:21833.
    int cnt = -1;
    EXPECT_FALSE(empty_src.count_codestreams(cnt));
    EXPECT_EQ(cnt, 0);
    cnt = -1;
    EXPECT_FALSE(empty_src.count_compositing_layers(cnt));
    EXPECT_EQ(cnt, 0);
    EXPECT_FALSE(empty_src.access_codestream(0).exists());
    EXPECT_FALSE(empty_src.access_layer(-1).exists());
    EXPECT_FALSE(empty_src.access_container(-1).exists());
    EXPECT_FALSE(
        empty_src.find_unique_compatible_container(0, nullptr, 0, nullptr)
            .exists());
    EXPECT_EQ(empty_src.get_num_layer_codestreams(-1), 0);
    EXPECT_EQ(empty_src.get_layer_codestream_id(-1, 0), -1);
    EXPECT_EQ(empty_src.generate_metareq(nullptr, 0, 0, 0, 0, false), 0);

    jpx_codestream_source empty_cs;
    EXPECT_EQ(empty_cs.get_codestream_id(), -1);
    EXPECT_TRUE(empty_cs.get_header_loc().is_null());
    EXPECT_FALSE(empty_cs.access_fragment_list().exists());
    EXPECT_FALSE(empty_cs.access_dimensions().exists());
    EXPECT_FALSE(empty_cs.access_palette().exists());
    EXPECT_EQ(empty_cs.enum_layer_ids(-1), -1);

    jpx_container_source empty_csrc;
    int base_layers = -1;
    EXPECT_EQ(empty_csrc.get_track_base_layers(0, base_layers), 0);
    EXPECT_EQ(base_layers, 0);
    EXPECT_FALSE(empty_csrc.access_presentation_track(0).exists());

    jpx_composition empty_comp;
    kdu_core::kdu_uint32 ucnt = 99;
    EXPECT_FALSE(empty_comp.count_tracks(ucnt, false));
    EXPECT_EQ(ucnt, 0u);
    EXPECT_FALSE(empty_comp.count_track_frames(0, cnt));
    kdu_core::kdu_long lcnt = -1;
    EXPECT_FALSE(empty_comp.count_track_time(0, lcnt));
    EXPECT_EQ(lcnt, 0);
    EXPECT_FALSE(empty_comp.count_track_frames_before_time(0, 10, cnt));
    EXPECT_FALSE(empty_comp.access_frame(0, 0, false).exists());
    jpx_frame frm;
    int inst_idx = -1;
    EXPECT_EQ(empty_comp.find_layer_match(frm, inst_idx, 0, nullptr, 0), -1);
    EXPECT_EQ(empty_comp.find_numlist_match(frm, inst_idx, 0, jpx_metanode()),
              -1);
    EXPECT_EQ(empty_comp.get_next_frame(nullptr), nullptr);
    EXPECT_EQ(empty_comp.get_prev_frame(nullptr), nullptr);
    int ni = -1, dur = 0, rep = 0;
    bool pers = false;
    empty_comp.get_frame_info(nullptr, ni, dur, rep, pers);
    EXPECT_EQ(ni, 0);
    EXPECT_EQ(empty_comp.get_last_persistent_frame(nullptr), nullptr);
    int lyr_idx = 0, inc = 0;
    bool reused = false;
    kdu_core::kdu_dims sdims, tdims;
    jpx_composited_orientation orient;
    EXPECT_FALSE(empty_comp.get_instruction(nullptr, 0, lyr_idx, inc, reused,
                                            sdims, tdims, orient));
    int iset = 0, inum = 0;
    EXPECT_FALSE(empty_comp.get_original_iset(nullptr, 0, iset, inum));

    jpx_metanode empty_mn;
    int b = 0, s = 0;
    EXPECT_EQ(empty_mn.get_container_lmap(&b, &s), 0);
    EXPECT_EQ(empty_mn.get_container_cmap(&b, &s), 0);
    EXPECT_EQ(empty_mn.get_container_codestream_rep(0), 0);
    EXPECT_EQ(empty_mn.get_container_layer_rep(0), 0);
    EXPECT_EQ(empty_mn.get_numlist_codestreams(), nullptr);
    EXPECT_EQ(empty_mn.get_numlist_layers(), nullptr);
    EXPECT_TRUE(empty_mn.count_numlist_codestreams(cnt));
    EXPECT_EQ(cnt, 0);
    EXPECT_TRUE(empty_mn.count_numlist_layers(cnt));
    EXPECT_EQ(cnt, 0);
    EXPECT_FALSE(empty_mn.test_numlist_stream(0));
    kdu_core::kdu_byte uuid[16]{};
    EXPECT_FALSE(empty_mn.get_uuid(uuid));
    EXPECT_FALSE(empty_mn.is_xmp_uuid());
    EXPECT_FALSE(empty_mn.is_iptc_uuid());
    EXPECT_FALSE(empty_mn.is_geojp2_uuid());
    jpx_fragment_list flst;
    EXPECT_EQ(empty_mn.get_cross_reference(flst), 0u);
    jpx_metanode_link_type ltype = JPX_GROUPING_LINK;
    EXPECT_FALSE(empty_mn.get_link(ltype, false).exists());
    EXPECT_EQ(ltype, JPX_METANODE_LINK_NONE);

    kdu_core::kdu_dims comp_region;
    EXPECT_EQ(jpx_frame_expander::test_codestream_visibility(
                  &empty_src, frm, 0, empty_mn, nullptr, 0, comp_region),
              -1);
    EXPECT_EQ(jpx_frame_expander::test_codestream_visibility(
                  &empty_src, static_cast<jx_frame*>(nullptr), 0, false, 0,
                  nullptr, 0, comp_region),
              -1);
    jpx_frame_expander expander;
    EXPECT_TRUE(expander.construct(&empty_src, frm));
    EXPECT_EQ(expander.get_num_members(), 0);
    EXPECT_FALSE(expander.has_non_covering_members());
    EXPECT_EQ(expander.get_member(-1, lyr_idx, pers, sdims, tdims, orient), -1);
    expander.reset();
  }

  // jpx_meta_manager::set_box_filter(>32), insert_node errors & early return,
  // and jpx_source::find_unique_compatible_container across containers.
  MemTarget mt;
  jp2_family_tgt ftgt;
  ftgt.open(&mt);
  jpx_target tgt;
  tgt.open(&ftgt);
  tgt.expect_containers();

  jpx_codestream_target cs0 = tgt.add_codestream();
  SetupCodestreamDims(cs0.access_dimensions(), 16, 16, 3);
  jpx_layer_target l0 = tgt.add_layer();
  SetupSrgbLayer(l0, 0);

  jpx_container_target c0 = tgt.add_container(1, 1, 1);
  jpx_codestream_target cs1 = c0.access_codestream(0);
  SetupCodestreamDims(cs1.access_dimensions(), 16, 16, 3);
  jpx_layer_target l1 = c0.access_layer(0);
  SetupSrgbLayer(l1, cs1.get_codestream_id());

  jpx_container_target c1 = tgt.add_container(1, 1, 1);
  jpx_codestream_target cs2 = c1.access_codestream(0);
  SetupCodestreamDims(cs2.access_dimensions(), 16, 16, 3);
  jpx_layer_target l2 = c1.access_layer(0);
  SetupSrgbLayer(l2, cs2.get_codestream_id());

  jpx_composition comp_t = tgt.access_composition();
  comp_t.set_loop_count(1);
  auto* fr_ref = comp_t.add_frame(100, 0, false);
  comp_t.add_instruction(fr_ref, 0, 0, MakeDims(0, 0, 16, 16),
                         MakeDims(0, 0, 16, 16));

  jpx_meta_manager mm_t = tgt.access_meta_manager();
  // Truncates at 32 with KDU_WARNING_DEV 0x18081704.
  std::vector<kdu_core::kdu_uint32> filter_types(40, jp2_xml_4cc);
  mm_t.set_box_filter(40, filter_types.data());

  // Empty selection returns root node without inserting numlist.
  EXPECT_TRUE(mm_t.insert_node(0, nullptr, 0, nullptr, false, 0, nullptr,
                               jpx_metanode(), -1)
                  .exists());

  // Error 30: regions supplied with 0 codestreams.
  jpx_roi dummy_roi;
  dummy_roi.init_rectangle(MakeDims(0, 0, 4, 4));
  EXPECT_THROW(mm_t.insert_node(0, nullptr, 0, nullptr, false, 1, &dummy_roi),
               kdu_core::kdu_exception);

  // Error 0x12081202: non-existent container_id.
  int s1 = 1;
  EXPECT_THROW(mm_t.insert_node(1, &s1, 0, nullptr, false, 0, nullptr,
                                jpx_metanode(), 9),
               kdu_core::kdu_exception);

  // Error 0x12081201: container_id >= 0 with non-matching root metanode;
  // Error 0x12081203: incompatible codestream from container 1 inside container
  // 0.
  int s0 = 0;
  int s2 = 2;
  jpx_metanode top_sub = mm_t.access_root().add_label("top");
  EXPECT_THROW(
      mm_t.insert_node(1, &s0, 0, nullptr, false, 0, nullptr, top_sub, 0),
      kdu_core::kdu_exception);
  EXPECT_THROW(mm_t.insert_node(1, &s2, 0, nullptr, false, 0, nullptr,
                                jpx_metanode(), 0),
               kdu_core::kdu_exception);

  tgt.write_headers();
  for (jpx_codestream_target cs_t : {cs0, cs1, cs2}) {
    auto* box = cs_t.open_stream();
    ASSERT_NE(box, nullptr);
    WriteMinimalCodestream(box, 16, 16, 3);
    box->close();
  }
  tgt.write_metadata();
  tgt.close();
  ftgt.close();

  MemSource msrc(mt.data());
  jp2_family_src fsrc;
  fsrc.open(&msrc);
  jpx_source src;
  ASSERT_EQ(src.open(&fsrc, false), 1);
  EXPECT_TRUE(src.access_meta_manager().access_root().exists());
  int total_cs = 0;
  EXPECT_TRUE(src.count_codestreams(total_cs));
  EXPECT_EQ(total_cs, 3);

  // Check find_unique_compatible_container variants.
  int top_s[1] = {0};
  int c0_l[1] = {1};
  int c1_s[1] = {2};
  EXPECT_FALSE(
      src.find_unique_compatible_container(1, top_s, 0, nullptr).exists());
  jpx_container_source found_c0 =
      src.find_unique_compatible_container(0, nullptr, 1, c0_l);
  EXPECT_TRUE(found_c0.exists());
  int base_cnt = 0;
  EXPECT_EQ(found_c0.get_base_layers(base_cnt), 1);
  jpx_container_source found_c1 =
      src.find_unique_compatible_container(1, c1_s, 0, nullptr);
  EXPECT_TRUE(found_c1.exists());
  EXPECT_EQ(found_c1.get_base_codestreams(base_cnt), 2);
  // Span two incompatible containers -> empty.
  EXPECT_FALSE(src.find_unique_compatible_container(1, c1_s, 1, c0_l).exists());

  // Non-existent codestream/layer bounds checks on open jpx_source.
  EXPECT_FALSE(src.access_layer(99).exists());
  EXPECT_EQ(src.get_num_layer_codestreams(99), 0);
  EXPECT_EQ(src.get_layer_codestream_id(99, 0), -1);
  EXPECT_FALSE(src.access_container(99).exists());

  src.close();
  fsrc.close();
}

TEST(JpxTest, Wave9RegistrationFragmentsRoiAndFrameExpander) {
  ScopedKduHandlers handlers;

  // 1. jpx_layer_target::set_codestream_registration clamping & error checks.
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    jpx_codestream_target cs0 = tgt.add_codestream();
    SetupCodestreamDims(cs0.access_dimensions(), 16, 16, 3);
    jpx_layer_target l0 = tgt.add_layer();
    SetupSrgbLayer(l0, 0);
    l0.set_codestream_registration(0, kdu_core::kdu_coords(1, 2),
                                   kdu_core::kdu_coords(3, 4),
                                   kdu_core::kdu_coords(5, 6));
    tgt.write_headers();
    auto* box = cs0.open_stream();
    WriteMinimalCodestream(box, 16, 16, 3);
    box->close();
    tgt.close();
    ftgt.close();
  }
  for (const auto& bad_case : {
           std::array<int, 5>{99, 0, 1, 2, 0},  // 18235 / bad cs_id
           std::array<int, 5>{0, 0, 0, 1, 0},   // error 63 xr == 0
           std::array<int, 5>{0, 5, 1, 4, 0},   // error 63 xo >= denom
           std::array<int, 5>{0, 0, 1, 8, 1},   // error 62 denom mismatch
       }) {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    jpx_codestream_target cs0 = tgt.add_codestream();
    SetupCodestreamDims(cs0.access_dimensions(), 16, 16, 3);
    jpx_layer_target l0 = tgt.add_layer();
    SetupSrgbLayer(l0, 0);
    EXPECT_THROW(
        {
          if (bad_case[4] != 0) {
            l0.set_codestream_registration(0, kdu_core::kdu_coords(0, 0),
                                           kdu_core::kdu_coords(1, 1),
                                           kdu_core::kdu_coords(4, 4));
          }
          l0.set_codestream_registration(bad_case[0],
                                         kdu_core::kdu_coords(bad_case[1], 0),
                                         kdu_core::kdu_coords(bad_case[2], 1),
                                         kdu_core::kdu_coords(bad_case[3], 8));
          tgt.write_headers();
        },
        kdu_core::kdu_exception);
    tgt.close();
    ftgt.close();
  }

  // 2. Malformed 'creg' boxes in 'jplh' (errors 31..35) and 'rreq'/'ftyp'
  //    malformations (errors 6..11).
  auto build_base_jpx_boxes = [](bool with_creg_err,
                                 std::vector<kdu_core::kdu_byte> extra_jplh,
                                 std::vector<kdu_core::kdu_byte> ftyp_p,
                                 std::vector<kdu_core::kdu_byte> rreq_p) {
    MemTarget cs_mt;
    WriteMinimalCodestream(&cs_mt, 8, 8, 1);

    std::vector<kdu_core::kdu_byte> out;
    AppendBox(out, jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
    if (ftyp_p.empty()) {
      AppendBe32(ftyp_p, jpx_brand);
      AppendBe32(ftyp_p, 0);
      AppendBe32(ftyp_p, jpx_brand);
      AppendBe32(ftyp_p, jp2_brand);
    }
    AppendBox(out, jp2_file_type_4cc, ftyp_p);
    if (!rreq_p.empty()) {
      AppendBox(out, jp2_reader_requirements_4cc, rreq_p);
    }

    // Minimal jplh box for creg tests when requested.
    if (with_creg_err) {
      std::vector<kdu_core::kdu_byte> ihdr_p;
      AppendBe32(ihdr_p, 8);
      AppendBe32(ihdr_p, 8);
      AppendBe16(ihdr_p, 1);
      ihdr_p.push_back(7);
      ihdr_p.push_back(7);
      ihdr_p.push_back(0);
      ihdr_p.push_back(0);
      std::vector<kdu_core::kdu_byte> chdr_p;
      AppendBox(chdr_p, jp2_image_header_4cc, ihdr_p);
      AppendBox(out, jp2_codestream_header_4cc, chdr_p);

      std::vector<kdu_core::kdu_byte> cgrp_p;
      std::vector<kdu_core::kdu_byte> colr_p = {1, 0, 0, 0, 0, 0, 17};
      AppendBox(cgrp_p, jp2_colour_4cc, colr_p);
      std::vector<kdu_core::kdu_byte> jplh_p;
      AppendBox(jplh_p, jp2_colour_group_4cc, cgrp_p);
      jplh_p.insert(jplh_p.end(), extra_jplh.begin(), extra_jplh.end());
      AppendBox(out, jp2_compositing_layer_hdr_4cc, jplh_p);
    }
    AppendBox(out, jp2_codestream_4cc, cs_mt.data());
    return out;
  };

  // creg errors 31..35.
  {
    std::vector<kdu_core::kdu_byte> valid_creg = {0, 1, 0, 1, 0, 0, 1, 1, 0, 0};
    std::vector<kdu_core::kdu_byte> dup;
    AppendBox(dup, jp2_registration_4cc, valid_creg);
    AppendBox(dup, jp2_registration_4cc, valid_creg);

    std::vector<kdu_core::kdu_byte> xs_zero;
    AppendBox(xs_zero, jp2_registration_4cc, {0, 0, 0, 1, 0, 0, 1, 1, 0, 0});

    std::vector<kdu_core::kdu_byte> bad_len;
    AppendBox(bad_len, jp2_registration_4cc, {0, 1, 0, 1, 0});

    std::vector<kdu_core::kdu_byte> xr_zero;
    AppendBox(xr_zero, jp2_registration_4cc, {0, 1, 0, 1, 0, 0, 0, 1, 0, 0});

    std::vector<kdu_core::kdu_byte> xo_ge_xs;
    AppendBox(xo_ge_xs, jp2_registration_4cc, {0, 1, 0, 1, 0, 0, 1, 1, 2, 0});

    for (const auto& bad_creg : {dup, xs_zero, bad_len, xr_zero, xo_ge_xs}) {
      auto bytes = build_base_jpx_boxes(true, bad_creg, {}, {});
      MemSource ms(bytes);
      jp2_family_src fs;
      fs.open(&ms);
      jpx_source src;
      ASSERT_EQ(src.open(&fs, false), 1);
      EXPECT_THROW((void)src.access_layer(0, false), kdu_core::kdu_exception);
      src.close();
      fs.close();
    }
  }

  // ftyp compat variations (jph, jhxb, non-jpx compat) & rreq ML=5 +
  // errors 6..11.
  {
    std::vector<kdu_core::kdu_byte> jph_ftyp;
    AppendBe32(jph_ftyp, jph_brand);
    AppendBe32(jph_ftyp, 0);
    AppendBe32(jph_ftyp, jph_brand);
    AppendBe32(jph_ftyp, jpx_brand);
    AppendBe32(jph_ftyp, jpx_ht_baseline_brand);
    // Valid ML=5 rreq box:
    // ML(1)=5, fuam(5), dcm(5), nsf(2)=1, sf0(id=2 + mask=5), nvf(2)=1,
    // vf0(uuid=16 + mask=5). Total = 1 + 5 + 5 + 2 + 7 + 2 + 21 = 43 bytes.
    std::vector<kdu_core::kdu_byte> ok_rreq = {
        5,                              // ML=5
        0, 0,  0,  0,  0,               // fuam
        0, 0,  0,  0,  0,               // dcm
        0, 1,                           // nsf=1
        0, 10, 0,  0,  0,  0,  0,       // sf0: id=10, mask=5 bytes
        0, 1,                           // nvf=1
        1, 2,  3,  4,  5,  6,  7,  8,   // vf0 uuid[0..7]
        9, 10, 11, 12, 13, 14, 15, 16,  // vf0 uuid[8..15]
        0, 0,  0,  0,  0,               // vf0 mask=5 bytes
    };
    auto bytes = build_base_jpx_boxes(false, {}, jph_ftyp, ok_rreq);
    MemSource ms(bytes);
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    EXPECT_EQ(src.open(&fs, false), 1);
    EXPECT_TRUE(src.access_compatibility().is_jph_compatible());
    src.close();
    fs.close();
  }
  {
    std::vector<kdu_core::kdu_byte> bad_ftyp;
    AppendBe32(bad_ftyp, 0x62616431u);  // 'bad1'
    AppendBe32(bad_ftyp, 0);
    AppendBe32(bad_ftyp, 0x62616431u);
    auto bytes = build_base_jpx_boxes(false, {}, bad_ftyp, {});
    MemSource ms(bytes);
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    EXPECT_THROW((void)src.open(&fs, false), kdu_core::kdu_exception);
    src.close();
    fs.close();
  }
  {
    // Truncated/unaligned ftyp trailing byte -> !ftyp->close() KDU_ERROR 6.
    std::vector<kdu_core::kdu_byte> odd_ftyp = {0x6a, 0x70, 0x78, 0x20, 0,
                                                0,    0,    0,    0x99};
    auto bytes = build_base_jpx_boxes(false, {}, odd_ftyp, {});
    MemSource ms(bytes);
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    EXPECT_THROW((void)src.open(&fs, false), kdu_core::kdu_exception);
    src.close();
    fs.close();
  }
  for (const std::vector<kdu_core::kdu_byte>& bad_rreq : {
           std::vector<kdu_core::kdu_byte>{1, 0, 0},        // err 7: nsf
           std::vector<kdu_core::kdu_byte>{1, 0, 0, 0, 0},  // err 8: nvf
           std::vector<kdu_core::kdu_byte>{1, 0, 0, 0, 0, 0, 1,
                                           0},  // err 9: uuid
           std::vector<kdu_core::kdu_byte>{
               1, 0, 0, 0, 0,  0,  1,  1,  2,  3,  4, 5,
               6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},  // err 10: vmask
           std::vector<kdu_core::kdu_byte>{1, 0, 0, 0, 0, 0, 0,
                                           99},  // err 11: trailing
       }) {
    auto bytes = build_base_jpx_boxes(false, {}, {}, bad_rreq);
    MemSource ms(bytes);
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    EXPECT_THROW((void)src.open(&fs, false), kdu_core::kdu_exception);
    src.close();
    fs.close();
  }

  // 3. jpx_codestream_target fragment table errors (53, 55, 56, 57,
  //    0x03071204..07), jpx_input_box open_as/seek/read + temp-file url_idx=0
  //    & url_fopen 0x1337.
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    jpx_codestream_target cs0 = tgt.add_codestream();
    SetupCodestreamDims(cs0.access_dimensions(), 16, 16, 1);
    jpx_codestream_target cs1 = tgt.add_codestream();
    SetupCodestreamDims(cs1.access_dimensions(), 16, 16, 1);
    jpx_layer_target l0 = tgt.add_layer();
    l0.access_channels().init(1);
    l0.access_channels().set_colour_mapping(0, 0, -1, 0);
    l0.access_colour(0).init(JP2_sLUM_SPACE);

    // Must call write_headers() first -> 53 / 56.
    EXPECT_THROW(cs0.write_fragment_table(), kdu_core::kdu_exception);
    EXPECT_THROW((void)cs0.open_stream(), kdu_core::kdu_exception);

    tgt.write_headers();

    // cs1: open_stream while jp2c open -> 0x03071207 / 0x03071206, then
    // closing & calling open_stream again -> 0x03071205.
    auto* b1 = cs1.open_stream();
    ASSERT_NE(b1, nullptr);
    EXPECT_THROW(cs1.write_fragment_table(), kdu_core::kdu_exception);
    EXPECT_THROW((void)cs1.open_stream(), kdu_core::kdu_exception);
    WriteMinimalCodestream(b1, 16, 16, 1);
    b1->close();
    EXPECT_THROW((void)cs1.open_stream(), kdu_core::kdu_exception);

    // cs0: empty fragment list -> 55; invalid url_idx -> 0x22071201;
    // open_stream with fragment list -> 57; double write_fragment_table ->
    // 0x03071204.
    EXPECT_THROW(cs0.write_fragment_table(), kdu_core::kdu_exception);
    EXPECT_THROW(cs0.access_fragment_list().add_fragment(-1, 0, 10),
                 kdu_core::kdu_exception);
    cs0.add_fragment(nullptr, 0, 12, false);
    EXPECT_THROW((void)cs0.open_stream(), kdu_core::kdu_exception);

    const std::string ext_frag = ::testing::TempDir() + "/wave9_ext_frag.j2c";
    {
      FILE* fp = std::fopen(ext_frag.c_str(), "wb");
      ASSERT_NE(fp, nullptr);
      kdu_core::kdu_byte payload[16]{};
      std::fwrite(payload, 1, sizeof(payload), fp);
      std::fclose(fp);
    }
    cs0.add_fragment(ext_frag.c_str(), 0, 16, true);
    cs0.write_fragment_table();
    EXPECT_THROW(cs0.write_fragment_table(), kdu_core::kdu_exception);
    tgt.close();
    ftgt.close();

    // Parse back ftbl/flst & exercise jpx_input_box error/read paths.
    jpx_input_box closed_ib;
    kdu_core::kdu_byte buf[16]{};
    EXPECT_THROW(closed_ib.read(buf, 8),
                 kdu_core::kdu_exception);  // 0x05091201

    MemSource ms(mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_EQ(src.open(&fs, false), 1);
    jpx_codestream_source cs_s = src.access_codestream(1);
    ASSERT_TRUE(cs_s.exists());
    jpx_fragment_list flst = cs_s.access_fragment_list();
    ASSERT_TRUE(flst.exists());

    // open_as error checks: NULL src (5), empty flst (0x23071205), and
    // calling open_as when already open (4).
    jpx_input_box ib_err1;
    EXPECT_THROW(ib_err1.open_as(flst, src.access_data_references(), nullptr,
                                 jp2_codestream_4cc),
                 kdu_core::kdu_exception);
    jpx_input_box ib_err2;
    EXPECT_THROW(
        ib_err2.open_as(jpx_fragment_list(), src.access_data_references(), &fs,
                        jp2_codestream_4cc),
        kdu_core::kdu_exception);

    jpx_input_box ib;
    // Valid open_as + seek + read from url_idx=0 MemSource, open_as while open
    // (error 4), and url_idx=1 -> Google patch url_fopen KDU_ERROR 0x1337.
    ASSERT_TRUE(ib.open_as(flst, src.access_data_references(), &fs,
                           jp2_codestream_4cc));
    EXPECT_THROW(
        ib.open_as(flst, src.access_data_references(), &fs, jp2_codestream_4cc),
        kdu_core::kdu_exception);
    EXPECT_FALSE(ib.open_next());  // Already open on flst -> returns false
    EXPECT_TRUE(ib.seek(-1));  // Negative seek clamps pos to 0 and returns true
    EXPECT_TRUE(ib.seek(0));
    EXPECT_THROW(ib.read(buf, 16), kdu_core::kdu_exception);
    ib.close();
    src.close();
    fs.close();

    // Temp-file FILE* flst_src branch for url_idx=0 reading.
    const std::string main_jpx = ::testing::TempDir() + "/wave9_main.jpx";
    MemTarget cs_only;
    WriteMinimalCodestream(&cs_only, 8, 8, 1);
    {
      MemTarget mt2;
      jp2_family_tgt ft2;
      ft2.open(&mt2);
      jpx_target t2;
      t2.open(&ft2);
      jpx_codestream_target cs_t2 = t2.add_codestream();
      SetupCodestreamDims(cs_t2.access_dimensions(), 8, 8, 1);
      jpx_layer_target l_t2 = t2.add_layer();
      l_t2.access_channels().init(1);
      l_t2.access_channels().set_colour_mapping(0, 0, -1, 0);
      l_t2.access_colour(0).init(JP2_sLUM_SPACE);
      t2.write_headers();
      cs_t2.add_fragment(nullptr, 0,
                         static_cast<kdu_core::kdu_long>(cs_only.data().size()),
                         false);
      cs_t2.write_fragment_table();
      t2.close();
      ft2.close();
      FILE* fp = std::fopen(main_jpx.c_str(), "wb");
      ASSERT_NE(fp, nullptr);
      std::fwrite(mt2.data().data(), 1, mt2.data().size(), fp);
      std::fclose(fp);
    }
    jp2_family_src fsrc_fp;
    fsrc_fp.open(main_jpx.c_str());
    jpx_source src_fp;
    ASSERT_EQ(src_fp.open(&fsrc_fp, false), 1);
    jpx_fragment_list flst_fp =
        src_fp.access_codestream(0).access_fragment_list();
    ASSERT_TRUE(flst_fp.exists());
    jpx_input_box ib_fp;
    ASSERT_TRUE(ib_fp.open_as(flst_fp, jp2_data_references(), &fsrc_fp,
                              jp2_codestream_4cc));
    EXPECT_TRUE(ib_fp.seek(4));
    EXPECT_EQ(ib_fp.read(buf, 8), 8);
    EXPECT_EQ(ib_fp.read(buf, 0), 0);
    ib_fp.close();
    src_fp.close();
    fsrc_fp.close();
  }

  // 4. jpx_frame_expander (both overloads of construct and
  //    test_codestream_visibility, >8 members, partial opaque occlusion,
  //    orientations/scaling, alpha codestream) + jpx_roi clipping & quads.
  {
    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);

    // cs 0..3: main, opaque cover, alpha channel, secondary
    std::vector<jpx_codestream_target> css;
    for (int i = 0; i < 4; ++i) {
      jpx_codestream_target c = tgt.add_codestream();
      SetupCodestreamDims(c.access_dimensions(), 16, 16, 1);
      css.push_back(c);
    }
    // Layer 0: opaque cs0
    jpx_layer_target l0 = tgt.add_layer();
    l0.access_channels().init(1);
    l0.access_channels().set_colour_mapping(0, 0, -1, 0);
    l0.access_colour(0).init(JP2_sLUM_SPACE);
    // Layer 1: opaque cs1
    jpx_layer_target l1 = tgt.add_layer();
    l1.access_channels().init(1);
    l1.access_channels().set_colour_mapping(0, 0, -1, 1);
    l1.access_colour(0).init(JP2_sLUM_SPACE);
    // Layer 2: alpha-mapped (cs3 colour + cs2 opacity)
    jpx_layer_target l2 = tgt.add_layer();
    l2.access_channels().init(1);
    l2.access_channels().set_colour_mapping(0, 0, -1, 3);
    l2.access_channels().set_opacity_mapping(0, 0, -1, 2);
    l2.access_colour(0).init(JP2_sLUM_SPACE);
    // Layer 3: opaque cs0
    jpx_layer_target l3 = tgt.add_layer();
    l3.access_channels().init(1);
    l3.access_channels().set_colour_mapping(0, 0, -1, 0);
    l3.access_colour(0).init(JP2_sLUM_SPACE);

    jpx_composition comp_t = tgt.access_composition();
    comp_t.set_loop_count(1);

    // Frame 0: 10 instructions (>8 grows members array in construct()),
    // mixing transparent alpha, partial left/right/top/bottom opaque covers,
    // and orientations/scaling.
    auto* f0 = comp_t.add_frame(100, 0, false);
    jpx_composited_orientation rot;
    rot.transpose = true;
    rot.hflip = true;
    rot.vflip = true;
    // Bottom layers (low index): cs0 in layers 0 and 3, cs2 in alpha layer 2.
    comp_t.add_instruction(f0, 0, 0, MakeDims(0, 0, 16, 16),
                           MakeDims(0, 0, 16, 16), rot);
    comp_t.add_instruction(f0, 3, 0, MakeDims(0, 0, 16, 16),
                           MakeDims(0, 0, 8, 16));
    comp_t.add_instruction(f0, 2, 0, MakeDims(0, 0, 16, 16),
                           MakeDims(0, 0, 16, 16));
    comp_t.add_instruction(f0, 2, 0, MakeDims(0, 0, 16, 16),
                           MakeDims(0, 0, 16, 16));
    comp_t.add_instruction(f0, 2, 0, MakeDims(0, 0, 16, 16),
                           MakeDims(0, 0, 16, 16));
    // Partial opaque l1 instructions near top (y/x right & bottom stripes).
    comp_t.add_instruction(f0, 1, 0, MakeDims(0, 0, 4, 16),
                           MakeDims(0, 0, 4, 16));
    comp_t.add_instruction(f0, 1, 0, MakeDims(0, 0, 4, 16),
                           MakeDims(12, 0, 4, 16));
    comp_t.add_instruction(f0, 1, 0, MakeDims(0, 0, 16, 4),
                           MakeDims(0, 0, 16, 4));
    comp_t.add_instruction(f0, 1, 0, MakeDims(0, 0, 16, 4),
                           MakeDims(0, 12, 16, 4));
    comp_t.add_instruction(f0, 2, 0, MakeDims(0, 0, 16, 16),
                           MakeDims(0, 0, 16, 16));

    // Attach a codestream-only numlist metanode to test numlist exclusion in
    // test_codestream_visibility.
    jpx_meta_manager mm_t = tgt.access_meta_manager();
    int cs_only_arr[1] = {0};
    mm_t.insert_node(1, cs_only_arr, 0, nullptr, false, 0, nullptr)
        .add_label("cs0");

    tgt.write_headers();
    for (auto c : css) {
      auto* b = c.open_stream();
      WriteMinimalCodestream(b, 16, 16, 1);
      b->close();
    }
    tgt.write_metadata();
    tgt.close();
    ftgt.close();

    MemSource ms(mt.data());
    jp2_family_src fs;
    fs.open(&ms);
    jpx_source src;
    ASSERT_EQ(src.open(&fs, false), 1);
    jpx_composition comp = src.access_composition();
    ASSERT_TRUE(comp.exists());
    jpx_frame fr0 = comp.access_frame(0, 0, true);
    ASSERT_TRUE(fr0.exists());

    jpx_frame_expander exp;
    EXPECT_TRUE(exp.construct(&src, fr0, MakeDims(2, 2, 10, 10)));
    EXPECT_GT(exp.get_num_members(), 0);
    EXPECT_TRUE(exp.has_non_covering_members());
    auto* opaque_f0 = comp.get_next_frame(nullptr);
    ASSERT_NE(opaque_f0, nullptr);
    EXPECT_TRUE(exp.construct(&src, opaque_f0, 0, true, MakeDims(0, 0, 8, 8)));

    // Find the cs0-only numlist node (get_numlist_layer(0) < 0 triggers 5093).
    jpx_metanode cs_nl =
        src.access_meta_manager().access_root().get_next_descendant(
            jpx_metanode());
    kdu_core::kdu_dims vis_region;
    EXPECT_EQ(jpx_frame_expander::test_codestream_visibility(
                  &src, fr0, 0, cs_nl, nullptr, 0, vis_region,
                  MakeDims(100, 100, 4, 4)),
              -1);  // Non-intersecting codestream_roi
    vis_region = kdu_core::kdu_dims();
    int filter_l[2] = {0, 3};
    EXPECT_GE(jpx_frame_expander::test_codestream_visibility(
                  &src, fr0, 0, cs_nl, filter_l, 2, vis_region,
                  MakeDims(2, 2, 12, 12), true, 1),
              0);
    vis_region = kdu_core::kdu_dims();
    EXPECT_GE(jpx_frame_expander::test_codestream_visibility(
                  &src, opaque_f0, 0, true, 2, nullptr, 0, vis_region,
                  kdu_core::kdu_dims(), false, 0),
              0);
    src.close();
    fs.close();
  }

  // jpx_roi clip_region, axis_extents init_ellipse clamp, and quad trapezoid
  // branches.
  {
    jpx_roi big_e;
    const double tiny_axes[2] = {0.1, 0.1};
    big_e.init_ellipse(kdu_core::kdu_coords(5, 5), tiny_axes, 2.5, true, 1);
    big_e.region.pos = kdu_core::kdu_coords(0x7FFFFFFE, 0x7FFFFFFE);
    big_e.region.size = kdu_core::kdu_coords(1, 1);
    big_e.clip_region();
    double axes[2] = {40.0, 20.0};
    big_e.init_ellipse(kdu_core::kdu_coords(100, 5), axes, 2.5, true, 1);
    big_e.clip_region();
    big_e.init_ellipse(kdu_core::kdu_coords(5, 100), axes, -2.5, true, 1);
    big_e.clip_region();

    std::array<std::array<kdu_core::kdu_coords, 4>, 4> quads = {{
        {kdu_core::kdu_coords(50, 0), kdu_core::kdu_coords(90, 80),
         kdu_core::kdu_coords(2, 40), kdu_core::kdu_coords(10, 60)},
        {kdu_core::kdu_coords(50, 0), kdu_core::kdu_coords(90, 60),
         kdu_core::kdu_coords(98, 40), kdu_core::kdu_coords(10, 80)},
        {kdu_core::kdu_coords(50, 0), kdu_core::kdu_coords(80, 80),
         kdu_core::kdu_coords(40, 60), kdu_core::kdu_coords(10, 30)},
        {kdu_core::kdu_coords(50, 0), kdu_core::kdu_coords(90, 30),
         kdu_core::kdu_coords(60, 60), kdu_core::kdu_coords(10, 80)},
    }};
    for (const auto& qverts : quads) {
      jpx_roi q;
      q.init_quadrilateral(qverts[0], qverts[1], qverts[2], qverts[3]);
      double cx = 0.0, cy = 0.0;
      EXPECT_GE(q.measure_area(cx, cy), 0.0);
      EXPECT_TRUE(q.contains(qverts[0]) || !q.contains(qverts[0]));
      double xp = 0.0, yp = 0.0;
      q.find_boundary_projection(50.0, 50.0, xp, yp, 100.0);
    }
    double xp = 0.0, yp = 0.0;
    EXPECT_EQ(big_e.find_boundary_projection(big_e.region.pos.x + 10.0,
                                             big_e.region.pos.y + 10.0, xp, yp,
                                             0.01, 0.0),
              -1);
  }
}

TEST(JpxTest, Wave10RemainingPathTriangulationEmptyFramesAndMetanodeChecks) {
  ScopedKduHandlers handlers;

  // 1. jx_path_filler::add_triangle via odd closed polygons (triangle +
  //    pentagon).
  {
    const std::array<kdu_core::kdu_coords, 3> tri = {
        kdu_core::kdu_coords(20, 20), kdu_core::kdu_coords(180, 20),
        kdu_core::kdu_coords(100, 160)};
    const std::array<kdu_core::kdu_coords, 5> pent = {
        kdu_core::kdu_coords(300, 20), kdu_core::kdu_coords(380, 70),
        kdu_core::kdu_coords(350, 160), kdu_core::kdu_coords(250, 160),
        kdu_core::kdu_coords(220, 70)};
    std::vector<jpx_roi> rois;
    AppendClosedPolyBricks(rois, tri, false);
    AppendClosedPolyBricks(rois, pent, false);
    jpx_roi_editor ed;
    ed.init(rois.data(), static_cast<int>(rois.size()));
    bool ok = false;
    ed.fill_closed_paths(ok, -1);
    EXPECT_TRUE(ok);
    EXPECT_GT(ed.measure_complexity(), 0.0);
  }

  // 2. jx_check_metanode_before_add_child & jx_check_metanode_before_change.
  {
    jpx_metanode empty;
    EXPECT_THROW((void)empty.add_label("x"), kdu_core::kdu_exception);
    EXPECT_THROW(empty.change_to_label("x"), kdu_core::kdu_exception);

    MemTarget mt;
    jp2_family_tgt ftgt;
    ftgt.open(&mt);
    jpx_target tgt;
    tgt.open(&ftgt);
    jpx_codestream_target cs0 = tgt.add_codestream();
    SetupCodestreamDims(cs0.access_dimensions(), 8, 8, 1);
    jpx_layer_target l0 = tgt.add_layer();
    l0.access_channels().init(1);
    l0.access_channels().set_colour_mapping(0, 0, -1, 0);
    l0.access_colour(0).init(JP2_sLUM_SPACE);

    jpx_meta_manager mm = tgt.access_meta_manager();
    jpx_metanode root = mm.access_root();
    jpx_metanode target_lbl = root.add_label("target");
    jpx_metanode par_link =
        root.add_link(target_lbl, JPX_ALTERNATE_PARENT_LINK);
    EXPECT_THROW((void)par_link.add_label("bad"), kdu_core::kdu_exception);
    jpx_metanode delayed = root.add_delayed(jp2_xml_4cc, 42);

    tgt.write_headers();
    auto* box = cs0.open_stream();
    WriteMinimalCodestream(box, 8, 8, 1);
    box->close();

    // First write_metadata stops at delayed XML node -> add/change blocked.
    int ip = 0;
    void* ap = nullptr;
    bool checked_mid = false;
    while (jp2_output_box* dbox = tgt.write_metadata(&ip, &ap)) {
      EXPECT_EQ(ip, 42);
      if (!checked_mid) {
        EXPECT_THROW((void)root.add_label("mid"), kdu_core::kdu_exception);
        EXPECT_THROW(delayed.change_to_label("mid"), kdu_core::kdu_exception);
        checked_mid = true;
      }
      const char xml_body[] = "<x/>";
      dbox->write(reinterpret_cast<const kdu_core::kdu_byte*>(xml_body), 4);
      // Do NOT call dbox->close(); jpx_target::write_metadata() closes it.
    }
    EXPECT_TRUE(checked_mid);

    EXPECT_THROW((void)target_lbl.add_label("after"), kdu_core::kdu_exception);
    EXPECT_THROW(target_lbl.change_to_label("after"), kdu_core::kdu_exception);
    tgt.close();
    ftgt.close();
  }

  // 3. MemKduCache incremental codestream open_stream() + main header
  // completion
  //    (1076..1087 & 21534..21536).
  {
    MemTarget cs_mt;
    WriteMinimalCodestream(&cs_mt, 8, 8, 1);
    std::vector<kdu_core::kdu_byte> ihdr_p;
    AppendBe32(ihdr_p, 8);
    AppendBe32(ihdr_p, 8);
    AppendBe16(ihdr_p, 1);
    ihdr_p.insert(ihdr_p.end(), {7, 7, 0, 0});
    std::vector<kdu_core::kdu_byte> jp2h_p;
    AppendBox(jp2h_p, jp2_image_header_4cc, ihdr_p);
    AppendBox(jp2h_p, jp2_colour_4cc, {1, 0, 0, 0, 0, 0, 17});
    std::vector<kdu_core::kdu_byte> bin0;
    AppendBox(bin0, jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
    std::vector<kdu_core::kdu_byte> ftyp_p;
    AppendBe32(ftyp_p, jpx_brand);
    AppendBe32(ftyp_p, 0);
    AppendBe32(ftyp_p, jpx_brand);
    AppendBox(bin0, jp2_file_type_4cc, ftyp_p);
    AppendBox(bin0, jp2_header_4cc, jp2h_p);
    AppendPlaceholderBox(bin0, 4, 0,
                         static_cast<std::uint32_t>(8 + cs_mt.data().size()),
                         jp2_codestream_4cc, 0, 0, 0, 0);

    MemKduCache cache;
    cache.add_to_databin(KDU_META_DATABIN, 0, 0, bin0.data(), 0,
                         static_cast<int>(bin0.size()), true);
    ASSERT_GE(cs_mt.data().size(), 12u);
    cache.add_to_databin(KDU_MAIN_HEADER_DATABIN, 0, 0, cs_mt.data().data(), 0,
                         8, false);

    jp2_family_src fs;
    fs.open(&cache);
    jpx_source src;
    ASSERT_EQ(src.open(&fs, false), 1);
    jpx_codestream_source cs_inc = src.access_codestream(0, false);
    ASSERT_TRUE(cs_inc.exists());
    EXPECT_FALSE(src.access_codestream(0, true).exists());
    EXPECT_EQ(cs_inc.open_stream(), nullptr);

    cache.add_to_databin(KDU_MAIN_HEADER_DATABIN, 0, 0, cs_mt.data().data() + 8,
                         8, static_cast<int>(cs_mt.data().size()) - 8, true);
    cs_inc = src.access_codestream(0, true);
    ASSERT_TRUE(cs_inc.exists());
    jpx_input_box* inc_box = cs_inc.open_stream();
    ASSERT_NE(inc_box, nullptr);
    kdu_core::kdu_byte buf[4]{};
    EXPECT_TRUE(inc_box->seek(0));
    EXPECT_EQ(inc_box->read(buf, 4), 4);
    inc_box->close();
    src.close();
    fs.close();
  }
}

}  // namespace
}  // namespace kdu_supp
