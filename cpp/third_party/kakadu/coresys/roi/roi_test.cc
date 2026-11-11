// Copyright 2026 Google LLC

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_roi_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace {

class ThrowingMessage final : public kdu_core::kdu_message {
 public:
  ~ThrowingMessage() noexcept override = default;
  void put_text(const char* /*string*/) override {}
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      throw static_cast<kdu_core::kdu_exception>(KDU_ERROR_EXCEPTION);
    }
  }
};

class SilentMessage final : public kdu_core::kdu_message {
 public:
  ~SilentMessage() noexcept override = default;
  void put_text(const char* /*string*/) override {}
};

class KakaduTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    kdu_core::kdu_customize_errors(&error_handler_);
    kdu_core::kdu_customize_warnings(&warning_handler_);
  }

 private:
  ThrowingMessage error_handler_;
  SilentMessage warning_handler_;
};

const ::testing::Environment* const kKakaduEnv =
    ::testing::AddGlobalTestEnvironment(new KakaduTestEnvironment());

class VectorTarget final : public kdu_core::kdu_compressed_target {
 public:
  bool write(const kdu_core::kdu_byte* buf, int num_bytes) override {
    bytes_.insert(bytes_.end(), buf, buf + num_bytes);
    return true;
  }

 private:
  std::vector<kdu_core::kdu_byte> bytes_;
};

struct ScopedCodestream {
  VectorTarget target;
  kdu_core::siz_params siz;
  kdu_core::kdu_codestream codestream;

  ~ScopedCodestream() {
    if (codestream.exists()) {
      codestream.destroy();
    }
  }
};

struct ScopedTile {
  kdu_core::kdu_tile tile;
  ~ScopedTile() {
    if (tile.exists()) {
      tile.close();
    }
  }
};

struct ScopedRoiLevel {
  kdu_core::kdu_roi_level level;
  ~ScopedRoiLevel() {
    if (level.exists()) {
      level.destroy();
    }
  }
};

struct ScopedRoiNode {
  kdu_core::kdu_roi_node* node = nullptr;
  ScopedRoiNode() = default;
  explicit ScopedRoiNode(kdu_core::kdu_roi_node* n) : node(n) {}
  ScopedRoiNode(const ScopedRoiNode&) = delete;
  ScopedRoiNode& operator=(const ScopedRoiNode&) = delete;
  ScopedRoiNode(ScopedRoiNode&& other) noexcept : node(other.node) {
    other.node = nullptr;
  }
  ScopedRoiNode& operator=(ScopedRoiNode&& other) noexcept {
    if (this != &other) {
      ReleaseNow();
      node = other.node;
      other.node = nullptr;
    }
    return *this;
  }
  ~ScopedRoiNode() { ReleaseNow(); }
  void ReleaseNow() {
    if (node != nullptr) {
      node->release();
      node = nullptr;
    }
  }
};

class FunctionalRoiNode final : public kdu_core::kdu_roi_node {
 public:
  FunctionalRoiNode(
      int width, int height,
      std::function<void(int row, kdu_core::kdu_byte* buf, int width)> fill_fn)
      : width_(width), height_(height), fill_fn_(std::move(fill_fn)) {}

  void pull(kdu_core::kdu_byte buf[], int width) override {
    EXPECT_EQ(width_, width);
    EXPECT_LT(rows_pulled_, height_);
    fill_fn_(rows_pulled_, buf, width);
    ++rows_pulled_;
  }

  void release() override {
    released_ = true;
    kdu_core::kdu_roi_node::release();
  }

  int rows_pulled() const { return rows_pulled_; }
  bool released() const { return released_; }

 private:
  int width_;
  int height_;
  std::function<void(int, kdu_core::kdu_byte*, int)> fill_fn_;
  int rows_pulled_ = 0;
  bool released_ = false;
};

class DummyRoiImage final : public kdu_core::kdu_roi_image {
 public:
  ~DummyRoiImage() override = default;
  kdu_core::kdu_roi_node* acquire_node(int /*component*/,
                                       kdu_core::kdu_dims region) override {
    last_region_ = region;
    return nullptr;
  }
  kdu_core::kdu_dims last_region() const { return last_region_; }

 private:
  kdu_core::kdu_dims last_region_;
};

void InitCodestream(ScopedCodestream& ctx, int width, int height, int ox,
                    int oy, const std::string& kernels, int levels,
                    const std::string& decomp = "") {
  ctx.siz.parse_string("Scomponents=1");
  const std::string sdims =
      "Sdims={" + std::to_string(height) + "," + std::to_string(width) + "}";
  ASSERT_TRUE(ctx.siz.parse_string(sdims.c_str()));
  if (ox != 0 || oy != 0) {
    const std::string sorigin =
        "Sorigin={" + std::to_string(oy) + "," + std::to_string(ox) + "}";
    ASSERT_TRUE(ctx.siz.parse_string(sorigin.c_str()));
  }
  ASSERT_TRUE(ctx.siz.parse_string("Sprecision=8"));
  ASSERT_TRUE(ctx.siz.parse_string("Ssigned=no"));
  static_cast<kdu_core::kdu_params*>(&ctx.siz)->finalize();

  ctx.codestream.create(&ctx.siz, &ctx.target);
  kdu_core::siz_params* params = ctx.codestream.access_siz();
  ASSERT_TRUE(
      params->parse_string(("Clevels=" + std::to_string(levels)).c_str()));
  ASSERT_TRUE(params->parse_string(("Ckernels=" + kernels).c_str()));
  if (kernels == "W9X7") {
    ASSERT_TRUE(params->parse_string("Creversible=no"));
  }
  if (!decomp.empty()) {
    ASSERT_TRUE(params->parse_string(("Cdecomp=" + decomp).c_str()));
  }
  ASSERT_TRUE(
      params->parse_string(("Rlevels=" + std::to_string(levels)).c_str()));
  params->finalize_all();
}

struct ChildOutput {
  int band_idx = -1;
  kdu_core::kdu_dims dims;
  ScopedRoiNode handle;
  std::vector<kdu_core::kdu_byte> data;
  int rows_pulled = 0;
};

void PullChildrenInLockstep(std::vector<ChildOutput>& children) {
  int max_rows = 0;
  for (const ChildOutput& ch : children) {
    max_rows = std::max(max_rows, ch.dims.size.y);
  }
  // Pull one row per active child in lockstep so sibling row queues stay at
  // depth <= 1 and do not trigger the upstream reallocation leak in
  // roi.cpp:119.
  for (int r = 0; r < max_rows; ++r) {
    for (ChildOutput& ch : children) {
      if (ch.handle.node != nullptr && r < ch.dims.size.y) {
        kdu_core::kdu_byte* dst =
            ch.data.empty() ? nullptr
                            : &ch.data[static_cast<size_t>(r) * ch.dims.size.x];
        ch.handle.node->pull(dst, ch.dims.size.x);
        ++ch.rows_pulled;
      }
    }
  }
}

void RunTwoDCase(int width, int height, int ox, int oy,
                 const std::string& kernels, bool use_node_create,
                 const std::function<void(int, kdu_core::kdu_byte*, int)>& fill,
                 std::vector<ChildOutput>* out_children = nullptr) {
  ScopedCodestream ctx;
  InitCodestream(ctx, width, height, ox, oy, kernels, /*levels=*/1);
  ScopedTile st{ctx.codestream.open_tile(kdu_core::kdu_coords(0, 0))};
  kdu_core::kdu_tile_comp tc = st.tile.access_component(0);
  kdu_core::kdu_resolution res = tc.access_resolution();
  EXPECT_TRUE(res.propagate_roi());
  kdu_core::kdu_dims res_dims;
  res.get_dims(res_dims);
  EXPECT_EQ(res_dims.size.x, width);
  EXPECT_EQ(res_dims.size.y, height);
  EXPECT_EQ(res_dims.pos.x, ox);
  EXPECT_EQ(res_dims.pos.y, oy);

  FunctionalRoiNode src(width, height, fill);
  ScopedRoiLevel level_guard;
  if (use_node_create) {
    level_guard.level.create(res.access_node(), &src);
  } else {
    level_guard.level.create(res, &src);
  }
  ASSERT_TRUE(level_guard.level.exists());
  EXPECT_FALSE(!level_guard.level);

  std::vector<ChildOutput> children;
  const kdu_core::kdu_node root = res.access_node();
  for (int band : {LL_BAND, HL_BAND, LH_BAND, HH_BAND}) {
    kdu_core::kdu_node cn = root.access_child(band);
    if (!cn.exists()) {
      continue;
    }
    ChildOutput ch;
    ch.band_idx = band;
    cn.get_dims(ch.dims);
    ch.handle.node = level_guard.level.acquire_node(band);
    ASSERT_NE(nullptr, ch.handle.node);
    ch.data.assign(static_cast<size_t>(ch.dims.size.x) * ch.dims.size.y, 0);
    children.push_back(std::move(ch));
  }

  PullChildrenInLockstep(children);
  for (ChildOutput& ch : children) {
    EXPECT_EQ(ch.rows_pulled, ch.dims.size.y);
    ch.handle.ReleaseNow();
  }
  level_guard.level.destroy();
  EXPECT_FALSE(level_guard.level.exists());
  EXPECT_TRUE(!level_guard.level);
  EXPECT_EQ(src.rows_pulled(), height);
  EXPECT_TRUE(src.released());
  if (out_children != nullptr) {
    *out_children = std::move(children);
  }
}

TEST(RoiTest, EmptyLifecycleAndRoiImageInterface) {
  (void)kKakaduEnv;
  kdu_core::kdu_roi_level empty_level;
  EXPECT_FALSE(empty_level.exists());
  EXPECT_TRUE(!empty_level);
  empty_level.destroy();
  EXPECT_FALSE(empty_level.exists());

  DummyRoiImage roi_image;
  kdu_core::kdu_dims region;
  region.pos = kdu_core::kdu_coords(1, 2);
  region.size = kdu_core::kdu_coords(3, 4);
  EXPECT_EQ(nullptr, roi_image.acquire_node(0, region));
  EXPECT_EQ(roi_image.last_region().size.x, 3);
  EXPECT_EQ(roi_image.last_region().size.y, 4);
}

TEST(RoiTest, TwoDW5X3EvenOddOriginsAndTinySizes) {
  auto all_zero = [](int /*r*/, kdu_core::kdu_byte* buf, int w) {
    std::fill(buf, buf + w, 0);
  };
  auto all_ones = [](int /*r*/, kdu_core::kdu_byte* buf, int w) {
    std::fill(buf, buf + w, 255);
  };

  struct CaseSpec {
    int w;
    int h;
    int ox;
    int oy;
    bool use_node;
  };
  const std::array<CaseSpec, 8> cases = {{
      {16, 16, 0, 0, false},
      {17, 13, 1, 1, true},
      {1, 1, 0, 0, false},
      {1, 1, 1, 1, true},
      {2, 1, 1, 0, false},
      {1, 2, 0, 1, true},
      {3, 3, 1, 1, false},
      {10, 11, 1, 0, true},
  }};

  for (const CaseSpec& c : cases) {
    std::vector<ChildOutput> zero_out;
    RunTwoDCase(c.w, c.h, c.ox, c.oy, "W5X3", c.use_node, all_zero, &zero_out);
    for (const ChildOutput& ch : zero_out) {
      for (kdu_core::kdu_byte v : ch.data) {
        EXPECT_EQ(v, 0);
      }
    }

    std::vector<ChildOutput> ones_out;
    RunTwoDCase(c.w, c.h, c.ox, c.oy, "W5X3", !c.use_node, all_ones, &ones_out);
    for (const ChildOutput& ch : ones_out) {
      for (kdu_core::kdu_byte v : ch.data) {
        EXPECT_NE(v, 0);
      }
    }
  }
}

TEST(RoiTest, TwoDW9X7EvenOddOriginsAndSparseMask) {
  auto sparse_center = [](int r, kdu_core::kdu_byte* buf, int w) {
    std::fill(buf, buf + w, 0);
    if (r == 8 && w > 8) {
      buf[8] = 255;
    }
  };
  RunTwoDCase(20, 20, 0, 0, "W9X7", /*use_node_create=*/false, sparse_center);
  RunTwoDCase(17, 15, 1, 1, "W9X7", /*use_node_create=*/true, sparse_center);

  auto all_255 = [](int /*r*/, kdu_core::kdu_byte* buf, int w) {
    std::fill(buf, buf + w, 255);
  };
  RunTwoDCase(1, 1, 0, 0, "W9X7", false, all_255);
  RunTwoDCase(1, 1, 1, 1, "W9X7", true, all_255);
  RunTwoDCase(3, 3, 1, 1, "W9X7", false, all_255);
}

TEST(RoiTest, SubsetChildrenAndEarlyRelease) {
  auto fill_half = [](int r, kdu_core::kdu_byte* buf, int w) {
    std::fill(buf, buf + w, (r >= 4 && r <= 7) ? 255 : 0);
  };

  // Case 1: Only acquire HL_BAND and HH_BAND (LL and LH never acquired).
  {
    ScopedCodestream ctx;
    InitCodestream(ctx, 16, 16, 0, 0, "W5X3", /*levels=*/1);
    ScopedTile st{ctx.codestream.open_tile(kdu_core::kdu_coords(0, 0))};
    kdu_core::kdu_resolution res =
        st.tile.access_component(0).access_resolution();
    FunctionalRoiNode src(16, 16, fill_half);
    ScopedRoiLevel level_guard;
    level_guard.level.create(res, &src);

    std::vector<ChildOutput> children;
    const kdu_core::kdu_node root = res.access_node();
    for (int band : {HL_BAND, HH_BAND}) {
      ChildOutput ch;
      ch.band_idx = band;
      root.access_child(band).get_dims(ch.dims);
      ch.handle.node = level_guard.level.acquire_node(band);
      ch.data.assign(static_cast<size_t>(ch.dims.size.x) * ch.dims.size.y, 0);
      children.push_back(std::move(ch));
    }
    PullChildrenInLockstep(children);
    for (ChildOutput& ch : children) {
      ch.handle.ReleaseNow();
    }
    level_guard.level.destroy();
    EXPECT_TRUE(src.released());
  }

  // Case 2: Only acquire LL_BAND.
  {
    ScopedCodestream ctx;
    InitCodestream(ctx, 16, 16, 1, 1, "W9X7", /*levels=*/1);
    ScopedTile st{ctx.codestream.open_tile(kdu_core::kdu_coords(0, 0))};
    kdu_core::kdu_resolution res =
        st.tile.access_component(0).access_resolution();
    FunctionalRoiNode src(16, 16, fill_half);
    ScopedRoiLevel level_guard;
    level_guard.level.create(res, &src);

    ChildOutput ch;
    ch.band_idx = LL_BAND;
    res.access_node().access_child(LL_BAND).get_dims(ch.dims);
    ch.handle.node = level_guard.level.acquire_node(LL_BAND);
    ch.data.assign(static_cast<size_t>(ch.dims.size.x) * ch.dims.size.y, 0);
    for (int r = 0; r < ch.dims.size.y; ++r) {
      ch.handle.node->pull(&ch.data[r * ch.dims.size.x], ch.dims.size.x);
    }
    ch.handle.ReleaseNow();
    level_guard.level.destroy();
  }

  // Case 3: Acquire all 4, release HL & HH early (before pulls & mid-stream).
  {
    ScopedCodestream ctx;
    InitCodestream(ctx, 16, 16, 0, 0, "W5X3", /*levels=*/1);
    ScopedTile st{ctx.codestream.open_tile(kdu_core::kdu_coords(0, 0))};
    kdu_core::kdu_resolution res =
        st.tile.access_component(0).access_resolution();
    FunctionalRoiNode src(16, 16, fill_half);
    ScopedRoiLevel level_guard;
    level_guard.level.create(res, &src);

    std::vector<ChildOutput> children;
    const kdu_core::kdu_node root = res.access_node();
    for (int band : {LL_BAND, HL_BAND, LH_BAND, HH_BAND}) {
      ChildOutput ch;
      ch.band_idx = band;
      root.access_child(band).get_dims(ch.dims);
      ch.handle.node = level_guard.level.acquire_node(band);
      ch.data.assign(static_cast<size_t>(ch.dims.size.x) * ch.dims.size.y, 0);
      children.push_back(std::move(ch));
    }
    // Release HL immediately before pulling any rows.
    children[1].handle.ReleaseNow();

    int max_rows = 8;
    for (int r = 0; r < max_rows; ++r) {
      for (ChildOutput& ch : children) {
        if (ch.handle.node != nullptr && r < ch.dims.size.y) {
          ch.handle.node->pull(&ch.data[r * ch.dims.size.x], ch.dims.size.x);
        }
      }
      if (r == 2) {
        // Release HH mid-stream while continuing LL and LH.
        children[3].handle.ReleaseNow();
      }
    }
    for (ChildOutput& ch : children) {
      ch.handle.ReleaseNow();
    }
    level_guard.level.destroy();
    EXPECT_TRUE(src.released());
  }
}

TEST(RoiTest, OneDimensionalVerticalAndHorizontalDecompositions) {
  auto pattern = [](int r, kdu_core::kdu_byte* buf, int w) {
    for (int x = 0; x < w; ++x) {
      buf[x] = ((x + r) % 5 == 0) ? 255 : 0;
    }
  };
  struct OneDSpec {
    int w;
    int h;
    int ox;
    int oy;
    const char* kernels;
    const char* decomp;
  };
  const std::array<OneDSpec, 8> specs = {{
      {12, 13, 0, 0, "W5X3", "V(-)"},
      {12, 13, 0, 1, "W9X7", "V(-)"},
      {1, 5, 0, 1, "W5X3", "V(-)"},
      {5, 1, 0, 0, "W5X3", "V(-)"},
      {13, 12, 0, 0, "W5X3", "H(-)"},
      {13, 12, 1, 0, "W9X7", "H(-)"},
      {5, 1, 1, 0, "W5X3", "H(-)"},
      {1, 5, 0, 0, "W5X3", "H(-)"},
  }};

  for (const OneDSpec& s : specs) {
    ScopedCodestream ctx;
    InitCodestream(ctx, s.w, s.h, s.ox, s.oy, s.kernels, /*levels=*/1,
                   s.decomp);
    ScopedTile st{ctx.codestream.open_tile(kdu_core::kdu_coords(0, 0))};
    kdu_core::kdu_resolution res =
        st.tile.access_component(0).access_resolution();
    FunctionalRoiNode src(s.w, s.h, pattern);
    ScopedRoiLevel level_guard;
    level_guard.level.create(res, &src);

    std::vector<ChildOutput> children;
    const kdu_core::kdu_node root = res.access_node();
    for (int band : {LL_BAND, HL_BAND, LH_BAND, HH_BAND}) {
      kdu_core::kdu_node cn = root.access_child(band);
      if (!cn.exists()) {
        continue;
      }
      ChildOutput ch;
      ch.band_idx = band;
      cn.get_dims(ch.dims);
      ch.handle.node = level_guard.level.acquire_node(band);
      ch.data.assign(static_cast<size_t>(ch.dims.size.x) * ch.dims.size.y, 0);
      children.push_back(std::move(ch));
    }
    EXPECT_EQ(children.size(), 2u);
    PullChildrenInLockstep(children);
    for (ChildOutput& ch : children) {
      ch.handle.ReleaseNow();
    }
    level_guard.level.destroy();
    EXPECT_TRUE(src.released());
  }
}

TEST(RoiTest, MultiLevelCascadeAndCreateReinit) {
  auto fill = [](int r, kdu_core::kdu_byte* buf, int w) {
    std::fill(buf, buf + w, 0);
    if (r >= 8 && r <= 10 && w > 12) {
      buf[9] = 255;
    }
  };
  ScopedCodestream ctx;
  InitCodestream(ctx, 21, 23, 1, 1, "W5X3", /*levels=*/2);
  ScopedTile st{ctx.codestream.open_tile(kdu_core::kdu_coords(0, 0))};
  kdu_core::kdu_tile_comp tc = st.tile.access_component(0);
  kdu_core::kdu_resolution res_top = tc.access_resolution(2);
  kdu_core::kdu_resolution res_low = tc.access_resolution(1);

  // Exercise destroy() + re-create with fresh sources.
  FunctionalRoiNode reinit_src1(21, 23, fill);
  FunctionalRoiNode reinit_src2(21, 23, fill);
  {
    ScopedRoiLevel reinit_level;
    reinit_level.level.create(res_top, &reinit_src1);
    EXPECT_TRUE(reinit_level.level.exists());
    reinit_level.level.destroy();
    EXPECT_TRUE(reinit_src1.released());
    reinit_level.level.create(res_top.access_node(), &reinit_src2);
    EXPECT_TRUE(reinit_level.level.exists());
    reinit_level.level.destroy();
    EXPECT_TRUE(reinit_src2.released());
  }

  FunctionalRoiNode src(21, 23, fill);
  ScopedRoiLevel level_top;
  level_top.level.create(res_top, &src);
  kdu_core::kdu_roi_node* ll_link = level_top.level.acquire_node(LL_BAND);
  ASSERT_NE(nullptr, ll_link);

  ScopedRoiLevel level_low;
  level_low.level.create(res_low, ll_link);

  std::vector<ChildOutput> children;
  const kdu_core::kdu_node root_low = res_low.access_node();
  for (int band : {LL_BAND, HL_BAND, LH_BAND, HH_BAND}) {
    kdu_core::kdu_node cn = root_low.access_child(band);
    ASSERT_TRUE(cn.exists());
    ChildOutput ch;
    ch.band_idx = band;
    cn.get_dims(ch.dims);
    ch.handle.node = level_low.level.acquire_node(band);
    ch.data.assign(static_cast<size_t>(ch.dims.size.x) * ch.dims.size.y, 0);
    children.push_back(std::move(ch));
  }
  PullChildrenInLockstep(children);
  for (ChildOutput& ch : children) {
    ch.handle.ReleaseNow();
  }
  // level_low.destroy() releases its source (ll_link), so do not call
  // ll_link->release() a second time afterwards.
  level_low.level.destroy();
  level_top.level.destroy();
  EXPECT_TRUE(src.released());
}

}  // namespace
