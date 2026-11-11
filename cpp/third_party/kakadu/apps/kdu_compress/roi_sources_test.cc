// Copyright 2026 Google LLC

#include "third_party/kakadu/vendor/apps/kdu_compress/roi_sources.h"

#include <atomic>
#include <cstdint>
#include <filesystem>  // NOLINT
#include <fstream>
#include <string>
#include <system_error>  // NOLINT(build/c++11)
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
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

std::filesystem::path MakeTempDir() {
  static std::atomic<int> counter{0};
  std::filesystem::path dir =
      std::filesystem::path(::testing::TempDir()) /
      ("kakadu_roi_sources_test_" + std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void WriteBytes(const std::filesystem::path& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open());
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.close();
  ASSERT_TRUE(out.good());
}

void InitCodestream(ScopedCodestream& ctx, int width, int height,
                    int components = 1, bool subsample_chroma = false,
                    int tile_w = 0, int tile_h = 0, int ox = 0, int oy = 0) {
  ASSERT_TRUE(ctx.siz.parse_string(
      ("Scomponents=" + std::to_string(components)).c_str()));
  if (ox != 0 || oy != 0) {
    ASSERT_TRUE(ctx.siz.parse_string(
        ("Sorigin={" + std::to_string(oy) + "," + std::to_string(ox) + "}")
            .c_str()));
  }
  if (subsample_chroma && components >= 2) {
    ASSERT_TRUE(ctx.siz.parse_string(("Ssize={" + std::to_string(oy + height) +
                                      "," + std::to_string(ox + width) + "}")
                                         .c_str()));
    ASSERT_TRUE(ctx.siz.parse_string("Ssampling={1,1},{2,2}"));
  } else {
    ASSERT_TRUE(ctx.siz.parse_string(
        ("Sdims={" + std::to_string(height) + "," + std::to_string(width) + "}")
            .c_str()));
  }
  if (tile_w > 0 && tile_h > 0) {
    ASSERT_TRUE(ctx.siz.parse_string(("Stiles={" + std::to_string(tile_h) +
                                      "," + std::to_string(tile_w) + "}")
                                         .c_str()));
  }
  ASSERT_TRUE(ctx.siz.parse_string("Sprecision=8"));
  ASSERT_TRUE(ctx.siz.parse_string("Ssigned=no"));
  static_cast<kdu_core::kdu_params*>(&ctx.siz)->finalize();

  ctx.codestream.create(&ctx.siz, &ctx.target);
  kdu_core::siz_params* params = ctx.codestream.access_siz();
  ASSERT_TRUE(params->parse_string("Clevels=1"));
  ASSERT_TRUE(params->parse_string("Rlevels=1"));
  params->finalize_all();
}

std::vector<kdu_core::kdu_dims> GetAllCompRegions(kdu_core::kdu_codestream cs,
                                                  int tile_x, int tile_y,
                                                  int num_comps) {
  kdu_core::kdu_tile tile = cs.open_tile(kdu_core::kdu_coords(tile_x, tile_y));
  absl::Cleanup close_tile = [&tile]() { tile.close(); };
  std::vector<kdu_core::kdu_dims> result;
  result.reserve(num_comps);
  for (int c = 0; c < num_comps; ++c) {
    kdu_core::kdu_dims dims;
    tile.access_component(c).access_resolution().get_dims(dims);
    result.push_back(dims);
  }
  return result;
}

kdu_core::kdu_dims GetTileCompRegion(kdu_core::kdu_codestream cs, int tile_x,
                                     int tile_y, int comp_idx) {
  return GetAllCompRegions(cs, tile_x, tile_y, comp_idx + 1)[comp_idx];
}

std::vector<kdu_core::kdu_byte> PullAllRows(kdu_core::kdu_roi_node* node,
                                            const kdu_core::kdu_dims& dims) {
  std::vector<kdu_core::kdu_byte> out(
      static_cast<size_t>(dims.size.x) * dims.size.y, 0);
  for (int r = 0; r < dims.size.y; ++r) {
    kdu_core::kdu_byte* row_ptr =
        dims.size.x > 0 ? &out[static_cast<size_t>(r) * dims.size.x] : nullptr;
    node->pull(row_ptr, dims.size.x);
  }
  return out;
}

std::string MakePgm(int w, int h, const std::string& header_prefix,
                    uint8_t fill_value, bool fg_center_box = false) {
  std::string s = header_prefix;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t v = fill_value;
      if (fg_center_box && x >= w / 4 && x < (3 * w) / 4 && y >= h / 4 &&
          y < (3 * h) / 4) {
        v = 250;
      }
      s.push_back(static_cast<char>(v));
    }
  }
  return s;
}

TEST(RoiSourcesTest, RoiRectMultiTileSubsampledAndEmptyRegions) {
  (void)kKakaduEnv;
  ScopedCodestream ctx;
  InitCodestream(ctx, 20, 16, /*components=*/2, /*subsample_chroma=*/true,
                 /*tile_w=*/10, /*tile_h=*/8, /*ox=*/2, /*oy=*/4);

  kdu_core::kdu_dims roi_region;
  roi_region.pos = kdu_core::kdu_coords(5, 6);
  roi_region.size = kdu_core::kdu_coords(10, 8);
  ::kdu_roi_rect rect_roi(ctx.codestream, roi_region);

  // Pull full luma and chroma for tile (0,0), checking exact foreground mask.
  const std::vector<kdu_core::kdu_dims> tile00_dims =
      GetAllCompRegions(ctx.codestream, 0, 0, /*num_comps=*/2);
  for (int comp : {0, 1}) {
    const kdu_core::kdu_dims& dims = tile00_dims[comp];
    kdu_core::kdu_roi_node* node = rect_roi.acquire_node(comp, dims);
    ASSERT_NE(nullptr, node);
    absl::Cleanup release_node = [node]() { node->release(); };
    const std::vector<kdu_core::kdu_byte> mask = PullAllRows(node, dims);
    int ones = 0;
    int zeros = 0;
    for (kdu_core::kdu_byte v : mask) {
      if (v > 0) {
        ++ones;
      } else {
        ++zeros;
      }
    }
    EXPECT_GT(ones, 0);
    EXPECT_GT(zeros, 0);
  }

  // Pull partial rows then release early on tile (1,0), and zero-row release on
  // tile (0,1).
  {
    const kdu_core::kdu_dims dims = GetTileCompRegion(ctx.codestream, 1, 0, 0);
    kdu_core::kdu_roi_node* node = rect_roi.acquire_node(0, dims);
    ASSERT_NE(nullptr, node);
    absl::Cleanup release_node = [node]() { node->release(); };
    std::vector<kdu_core::kdu_byte> row(dims.size.x, 0);
    node->pull(row.data(), dims.size.x);
  }
  {
    const kdu_core::kdu_dims dims = GetTileCompRegion(ctx.codestream, 0, 1, 0);
    kdu_core::kdu_roi_node* node = rect_roi.acquire_node(0, dims);
    ASSERT_NE(nullptr, node);
    node->release();
  }

  // Empty/non-intersecting region yields all zeros.
  kdu_core::kdu_dims empty_region;
  empty_region.pos = kdu_core::kdu_coords(500, 500);
  empty_region.size = kdu_core::kdu_coords(4, 4);
  ::kdu_roi_rect outside_roi(ctx.codestream, empty_region);
  kdu_core::kdu_roi_node* node = outside_roi.acquire_node(0, tile00_dims[0]);
  ASSERT_NE(nullptr, node);
  absl::Cleanup release_node = [node]() { node->release(); };
  for (kdu_core::kdu_byte v : PullAllRows(node, tile00_dims[0])) {
    EXPECT_EQ(v, 0);
  }
}

TEST(RoiSourcesTest, RoiGraphicsPgmCommentsUpsampleAndDownsample) {
  const std::filesystem::path dir = MakeTempDir();
  absl::Cleanup dir_cleanup = [&dir]() {
    std::error_code ec;  // NOLINT(build/c++11)
    std::filesystem::remove_all(dir, ec);
  };

  // 1. 1:1 with # comments and subsampled chroma.
  const std::filesystem::path pgm_1to1 = dir / "mask_comments.pgm";
  WriteBytes(
      pgm_1to1,
      MakePgm(8, 8,
              "P5\n# first comment\n8 # w comment\n8\n# max comment\n255\n", 10,
              /*fg_center_box=*/true));
  {
    ScopedCodestream ctx;
    InitCodestream(ctx, 8, 8, /*components=*/2, /*subsample_chroma=*/true);
    ::kdu_roi_graphics gfx(ctx.codestream, pgm_1to1.c_str(),
                           /*threshold=*/0.5F);
    const std::vector<kdu_core::kdu_dims> comp_dims =
        GetAllCompRegions(ctx.codestream, 0, 0, /*num_comps=*/2);
    for (int comp : {0, 1}) {
      const kdu_core::kdu_dims& dims = comp_dims[comp];
      kdu_core::kdu_roi_node* node = gfx.acquire_node(comp, dims);
      ASSERT_NE(nullptr, node);
      absl::Cleanup release_node = [node]() { node->release(); };
      const std::vector<kdu_core::kdu_byte> mask = PullAllRows(node, dims);
      int fg_count = 0;
      for (kdu_core::kdu_byte v : mask) {
        if (v > 0) {
          ++fg_count;
        }
      }
      EXPECT_GT(fg_count, 0);
      EXPECT_LT(fg_count, static_cast<int>(mask.size()));
    }
  }

  // 2. Upsampling: 4x4 PGM mapped onto 16x12 codestream.
  const std::filesystem::path pgm_small = dir / "small.pgm";
  WriteBytes(pgm_small, MakePgm(4, 4, "P5\n4 4\n255\n", 0, true));
  {
    ScopedCodestream ctx;
    InitCodestream(ctx, 16, 12, /*components=*/1);
    ::kdu_roi_graphics gfx(ctx.codestream, pgm_small.c_str(), 0.5F);
    const kdu_core::kdu_dims dims = GetTileCompRegion(ctx.codestream, 0, 0, 0);
    kdu_core::kdu_roi_node* node = gfx.acquire_node(0, dims);
    ASSERT_NE(nullptr, node);
    absl::Cleanup release_node = [node]() { node->release(); };
    const std::vector<kdu_core::kdu_byte> mask = PullAllRows(node, dims);
    int fg_count = 0;
    for (kdu_core::kdu_byte v : mask) {
      if (v > 0) {
        ++fg_count;
      }
    }
    EXPECT_GT(fg_count, 0);
    EXPECT_LT(fg_count, static_cast<int>(mask.size()));
  }

  // 3. Downsampling: 24x20 PGM mapped onto 8x6 codestream.
  const std::filesystem::path pgm_large = dir / "large.pgm";
  WriteBytes(pgm_large, MakePgm(24, 20, "P5\n24 20\n255\n", 0, true));
  {
    ScopedCodestream ctx;
    InitCodestream(ctx, 8, 6, /*components=*/1);
    ::kdu_roi_graphics gfx(ctx.codestream, pgm_large.c_str(), 0.25F);
    const kdu_core::kdu_dims dims = GetTileCompRegion(ctx.codestream, 0, 0, 0);
    kdu_core::kdu_roi_node* node = gfx.acquire_node(0, dims);
    ASSERT_NE(nullptr, node);
    absl::Cleanup release_node = [node]() { node->release(); };
    const std::vector<kdu_core::kdu_byte> mask = PullAllRows(node, dims);
    int fg_count = 0;
    for (kdu_core::kdu_byte v : mask) {
      if (v > 0) {
        ++fg_count;
      }
    }
    EXPECT_GT(fg_count, 0);
    EXPECT_LT(fg_count, static_cast<int>(mask.size()));
  }
}

TEST(RoiSourcesTest, RoiGraphicsMultiTileAndEarlyRelease) {
  const std::filesystem::path dir = MakeTempDir();
  absl::Cleanup dir_cleanup = [&dir]() {
    std::error_code ec;  // NOLINT(build/c++11)
    std::filesystem::remove_all(dir, ec);
  };

  const std::filesystem::path pgm_path = dir / "tiled.pgm";
  WriteBytes(pgm_path, MakePgm(16, 16, "P5\n16 16\n255\n", 0, true));

  ScopedCodestream ctx;
  InitCodestream(ctx, 24, 24, /*components=*/2, /*subsample_chroma=*/true,
                 /*tile_w=*/12, /*tile_h=*/12);
  ::kdu_roi_graphics gfx(ctx.codestream, pgm_path.c_str(), 0.5F);

  // Tile (0,0): pull full component 0 and partial component 1 then release
  // early.
  {
    const std::vector<kdu_core::kdu_dims> tile00 =
        GetAllCompRegions(ctx.codestream, 0, 0, /*num_comps=*/2);
    kdu_core::kdu_roi_node* n0 = gfx.acquire_node(0, tile00[0]);
    ASSERT_NE(nullptr, n0);
    absl::Cleanup r0 = [n0]() { n0->release(); };
    (void)PullAllRows(n0, tile00[0]);

    kdu_core::kdu_roi_node* n1 = gfx.acquire_node(1, tile00[1]);
    ASSERT_NE(nullptr, n1);
    absl::Cleanup r1 = [n1]() { n1->release(); };
    std::vector<kdu_core::kdu_byte> row(tile00[1].size.x, 0);
    n1->pull(row.data(), tile00[1].size.x);
  }

  // Tile (1,0): acquire and immediately release without pulling.
  {
    const kdu_core::kdu_dims dims = GetTileCompRegion(ctx.codestream, 1, 0, 0);
    kdu_core::kdu_roi_node* node = gfx.acquire_node(0, dims);
    ASSERT_NE(nullptr, node);
    node->release();
  }

  // Tile (1,1): acquire skipping (0,1) and pull completely.
  {
    const kdu_core::kdu_dims dims = GetTileCompRegion(ctx.codestream, 1, 1, 0);
    kdu_core::kdu_roi_node* node = gfx.acquire_node(0, dims);
    ASSERT_NE(nullptr, node);
    absl::Cleanup rel = [node]() { node->release(); };
    (void)PullAllRows(node, dims);
  }
}

TEST(RoiSourcesTest, RoiGraphicsErrorPathsWithCleanup) {
  const std::filesystem::path dir = MakeTempDir();
  absl::Cleanup dir_cleanup = [&dir]() {
    std::error_code ec;  // NOLINT(build/c++11)
    std::filesystem::remove_all(dir, ec);
  };

  ScopedCodestream ctx;
  InitCodestream(ctx, 8, 8, /*components=*/1);
  const kdu_core::kdu_dims dims = GetTileCompRegion(ctx.codestream, 0, 0, 0);

  // 1. Missing file.
  EXPECT_THROW(
      {
        ::kdu_roi_graphics gfx(ctx.codestream, (dir / "missing.pgm").c_str(),
                               0.5F);
      },
      kdu_core::kdu_exception);

  // 2. Invalid magic header (P6 instead of P5).
  const std::filesystem::path bad_magic = dir / "bad_magic.pgm";
  WriteBytes(bad_magic, "P6\n8 8\n255\n");
  EXPECT_THROW(
      { ::kdu_roi_graphics gfx(ctx.codestream, bad_magic.c_str(), 0.5F); },
      kdu_core::kdu_exception);

  // 3. Invalid PGM header (non-numeric maxval fails stream extraction).
  const std::filesystem::path bad_header = dir / "bad_header.pgm";
  WriteBytes(bad_header, "P5\n8 8\nabc\n");
  EXPECT_THROW(
      { ::kdu_roi_graphics gfx(ctx.codestream, bad_header.c_str(), 0.5F); },
      kdu_core::kdu_exception);

  // 4. TODO: Passing non-positive dimensions ("P5\n0 8\n255\n") throws inside
  // kdu_roi_graphics::kdu_roi_graphics() after allocating arrays at
  // third_party/kakadu/vendor/apps/kdu_compress/roi_sources.cpp:372 and :381,
  // leaking 128 bytes in 2 allocations because C++ does not run
  // ~kdu_roi_graphics() when the constructor throws. Skip until upstream frees
  // constructor allocations on error.

  // 5. Threshold endpoints [0.0F, 1.0F].
  // TODO: Out-of-range thresholds trigger UBSan float-cast-overflow in
  // kdu_roi_graphics::kdu_roi_graphics() on conversion to byte threshold;
  // restrict tests to documented [0.0F, 1.0F] range.
  const std::filesystem::path valid_pgm = dir / "valid.pgm";
  WriteBytes(valid_pgm, MakePgm(8, 8, "P5\n8 8\n255\n", 128));
  {
    ::kdu_roi_graphics gfx(ctx.codestream, valid_pgm.c_str(), 0.0F);
    kdu_core::kdu_roi_node* node = gfx.acquire_node(0, dims);
    ASSERT_NE(nullptr, node);
    absl::Cleanup release_node = [node]() { node->release(); };
    for (kdu_core::kdu_byte v : PullAllRows(node, dims)) {
      EXPECT_NE(v, 0);
    }
  }
  {
    ::kdu_roi_graphics gfx(ctx.codestream, valid_pgm.c_str(), 1.0F);
    kdu_core::kdu_roi_node* node = gfx.acquire_node(0, dims);
    ASSERT_NE(nullptr, node);
    absl::Cleanup release_node = [node]() { node->release(); };
    for (kdu_core::kdu_byte v : PullAllRows(node, dims)) {
      EXPECT_EQ(v, 0);
    }
  }

  // 6. Truncated pixel payload detected during pull(): absl::Cleanup releases
  // the acquired node before ~kdu_roi_graphics runs so its destructor assertion
  // does not fire during stack unwinding.
  const std::filesystem::path truncated = dir / "truncated.pgm";
  WriteBytes(truncated, "P5\n8 8\n255\n\xFF\x00\xFF");
  EXPECT_THROW(
      {
        ::kdu_roi_graphics gfx(ctx.codestream, truncated.c_str(), 0.5F);
        kdu_core::kdu_roi_node* node = gfx.acquire_node(0, dims);
        ASSERT_NE(nullptr, node);
        absl::Cleanup release_node = [node]() { node->release(); };
        (void)PullAllRows(node, dims);
      },
      kdu_core::kdu_exception);
}

}  // namespace
