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

#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#ifdef SUCCEED
#undef SUCCEED
#endif
#include "third_party/hdf4/hdf/src/dfgr_priv.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/hdf_priv.h"
#include "third_party/hdf4/hdf/src/hproto.h"

namespace hdf4 {
namespace {

class DfgrTest : public ::testing::Test {
 protected:
  void SetUp() override {
    DFGRPshutdown();
    HEclear();
  }

  void TearDown() override {
    DFGRPshutdown();
    HEclear();
  }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }

  std::vector<uint8_t> MakePatternImage(int32_t width, int32_t height,
                                        int ncomps) {
    std::vector<uint8_t> data(width * height * ncomps);
    for (size_t i = 0; i < data.size(); ++i) {
      data[i] = static_cast<uint8_t>((i * 7 + 13) % 256);
    }
    return data;
  }

  std::vector<uint8_t> MakePalette() {
    std::vector<uint8_t> pal(256 * 3);
    for (size_t i = 0; i < pal.size(); ++i) {
      pal[i] = static_cast<uint8_t>(i % 256);
    }
    return pal;
  }
};

TEST_F(DfgrTest, SetcompressValidAndInvalid) {
  comp_info cinfo{};

  // COMP_NONE
  EXPECT_EQ(DFGRsetcompress(COMP_NONE, nullptr), SUCCEED);
  EXPECT_EQ(DFGRsetcompress(COMP_NONE, &cinfo), SUCCEED);

  // Various valid schemes
  EXPECT_EQ(DFGRsetcompress(COMP_RLE, &cinfo), SUCCEED);
  EXPECT_EQ(DFGRsetcompress(COMP_IMCOMP, &cinfo), SUCCEED);

  cinfo.jpeg.quality = 75;
  cinfo.jpeg.force_baseline = 1;
  EXPECT_EQ(DFGRsetcompress(COMP_JPEG, &cinfo), SUCCEED);

  // Invalid or unmapped schemes
  EXPECT_EQ(DFGRsetcompress(1, &cinfo), FAIL);
  EXPECT_EQ(DFGRsetcompress(3, &cinfo), FAIL);
  EXPECT_EQ(DFGRsetcompress(-1, &cinfo), FAIL);
  EXPECT_EQ(DFGRsetcompress(COMP_MAX_COMP + 1, &cinfo), FAIL);
  EXPECT_EQ(DFGRsetcompress(9999, &cinfo), FAIL);
}

TEST_F(DfgrTest, SetDimsAndIlErrors) {
  // Valid settings
  EXPECT_EQ(DFGRsetimdims(16, 16, 1, 0), SUCCEED);
  EXPECT_EQ(DFGRsetlutdims(256, 1, 3, 0), SUCCEED);

  // Invalid il
  EXPECT_EQ(DFGRsetimdims(16, 16, 1, -1), FAIL);
  EXPECT_EQ(DFGRsetlutdims(256, 1, 3, -1), FAIL);

  // Invalid dims
  EXPECT_EQ(DFGRsetimdims(0, 16, 1, 0), FAIL);
  EXPECT_EQ(DFGRsetimdims(16, 0, 1, 0), FAIL);
  EXPECT_EQ(DFGRsetimdims(-1, 16, 1, 0), FAIL);
  EXPECT_EQ(DFGRsetimdims(16, -1, 1, 0), FAIL);
  EXPECT_EQ(DFGRsetimdims(16, 16, FAIL, 0), FAIL);

  EXPECT_EQ(DFGRsetlutdims(0, 1, 3, 0), FAIL);
  EXPECT_EQ(DFGRsetlutdims(256, 0, 3, 0), FAIL);
  EXPECT_EQ(DFGRsetlutdims(-5, 1, 3, 0), FAIL);
  EXPECT_EQ(DFGRsetlutdims(256, -1, 3, 0), FAIL);
  EXPECT_EQ(DFGRsetlutdims(256, 1, FAIL, 0), FAIL);

  // DFGRIsetdims / DFGRIsetil directly
  EXPECT_EQ(DFGRIsetil(0, 0), SUCCEED);
  EXPECT_EQ(DFGRIsetil(0, 1), SUCCEED);
  EXPECT_EQ(DFGRIsetil(-1, 0), FAIL);
  EXPECT_EQ(DFGRIsetdims(10, 10, 1, 0), SUCCEED);
  EXPECT_EQ(DFGRIsetdims(0, 10, 1, 0), FAIL);
}

TEST_F(DfgrTest, WriteAndRead8BitImage) {
  const std::string path = GetTempFilePath("test_8bit.hdf");
  std::filesystem::remove(path);
  absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

  constexpr int32_t width = 16;
  constexpr int32_t height = 12;
  const auto img = MakePatternImage(width, height, 1);

  EXPECT_EQ(DFGRsetimdims(width, height, 1, 0), SUCCEED);
  EXPECT_EQ(DFGRaddimage(path.c_str(), const_cast<uint8_t*>(img.data()), width,
                         height),
            SUCCEED);

  const uint16_t last_ref = DFGRIlastref();
  EXPECT_GT(last_ref, 0);

  int32_t rwidth = 0;
  int32_t rheight = 0;
  int rncomps = 0;
  int ril = 0;
  EXPECT_EQ(DFGRgetimdims(path.c_str(), &rwidth, &rheight, &rncomps, &ril),
            SUCCEED);
  EXPECT_EQ(rwidth, width);
  EXPECT_EQ(rheight, height);
  EXPECT_EQ(rncomps, 1);
  EXPECT_EQ(ril, 0);

  std::vector<uint8_t> read_buf(width * height);
  EXPECT_EQ(DFGRgetimage(path.c_str(), read_buf.data(), width, height),
            SUCCEED);
  EXPECT_EQ(read_buf, img);
}

TEST_F(DfgrTest, PutImageRecreatesFile) {
  const std::string path = GetTempFilePath("test_put.hdf");
  std::filesystem::remove(path);
  absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

  constexpr int32_t width = 8;
  constexpr int32_t height = 8;
  const auto img1 = MakePatternImage(width, height, 1);

  EXPECT_EQ(DFGRputimage(path.c_str(), const_cast<uint8_t*>(img1.data()), width,
                         height),
            SUCCEED);

  // Overwrite with putimage
  std::vector<uint8_t> img2(width * height, 42);
  EXPECT_EQ(DFGRputimage(path.c_str(), img2.data(), width, height), SUCCEED);

  int32_t rw = 0;
  int32_t rh = 0;
  int rnc = 0;
  int ril = 0;
  EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), SUCCEED);
  EXPECT_EQ(rw, width);
  EXPECT_EQ(rh, height);

  std::vector<uint8_t> read_buf(width * height);
  EXPECT_EQ(DFGRgetimage(path.c_str(), read_buf.data(), width, height),
            SUCCEED);
  EXPECT_EQ(read_buf, img2);
}

TEST_F(DfgrTest, ReadImageWithoutGetimdims) {
  const std::string path = GetTempFilePath("test_direct_get.hdf");
  std::filesystem::remove(path);
  absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

  constexpr int32_t width = 10;
  constexpr int32_t height = 10;
  const auto img = MakePatternImage(width, height, 1);

  EXPECT_EQ(DFGRputimage(path.c_str(), const_cast<uint8_t*>(img.data()), width,
                         height),
            SUCCEED);

  DFGRIrestart();

  // Call DFGRIgetimlut directly with IMAGE without DFGRgetimdims
  std::vector<uint8_t> read_buf(width * height);
  int compressed = 0;
  uint16_t compr_type = 0;
  int has_pal = 0;
  EXPECT_EQ(DFGRIgetimlut(path.c_str(), read_buf.data(), width, height, 1, 0,
                          &compressed, &compr_type, &has_pal),
            SUCCEED);
  EXPECT_EQ(read_buf, img);
}

TEST_F(DfgrTest, AddAndGetLut) {
  const std::string path = GetTempFilePath("test_lut.hdf");
  std::filesystem::remove(path);
  absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

  const int32_t init_fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(init_fid, FAIL);
  ASSERT_NE(Hclose(init_fid), FAIL);

  const auto pal = MakePalette();
  EXPECT_EQ(DFGRsetlutdims(256, 1, 3, 0), SUCCEED);
  EXPECT_EQ(DFGRaddlut(path.c_str(), const_cast<uint8_t*>(pal.data()), 256, 1),
            SUCCEED);

  int32_t xdim = 0;
  int32_t ydim = 0;
  int ncomps = 0;
  int il = 0;
  EXPECT_EQ(DFGRgetlutdims(path.c_str(), &xdim, &ydim, &ncomps, &il), SUCCEED);
  EXPECT_EQ(xdim, 256);
  EXPECT_EQ(ydim, 1);
  EXPECT_EQ(ncomps, 3);
  EXPECT_EQ(il, 0);

  std::vector<uint8_t> read_pal(256 * 3);
  EXPECT_EQ(DFGRgetlut(path.c_str(), read_pal.data(), 256, 1), SUCCEED);
  EXPECT_EQ(read_pal, pal);

  // Test with nullptr out pointers in DFGRIgetdims
  EXPECT_EQ(DFGRIgetdims(path.c_str(), nullptr, nullptr, nullptr, nullptr, 0),
            SUCCEED);
}

TEST_F(DfgrTest, SetlutAssociatedWithImage) {
  const std::string path = GetTempFilePath("test_setlut.hdf");
  std::filesystem::remove(path);
  absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

  const auto pal = MakePalette();
  EXPECT_EQ(DFGRsetlutdims(256, 1, 3, 0), SUCCEED);
  EXPECT_EQ(DFGRsetlut(const_cast<uint8_t*>(pal.data()), 256, 1), SUCCEED);

  constexpr int32_t width = 8;
  constexpr int32_t height = 8;
  const auto img = MakePatternImage(width, height, 1);

  EXPECT_EQ(DFGRsetimdims(width, height, 1, 0), SUCCEED);
  EXPECT_EQ(DFGRaddimage(path.c_str(), const_cast<uint8_t*>(img.data()), width,
                         height),
            SUCCEED);

  // Read image and palette back
  int32_t rw = 0;
  int32_t rh = 0;
  int rnc = 0;
  int ril = 0;
  EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), SUCCEED);
  EXPECT_EQ(rw, width);
  EXPECT_EQ(rh, height);

  int32_t lx = 0;
  int32_t ly = 0;
  int lnc = 0;
  int lil = 0;
  EXPECT_EQ(DFGRgetlutdims(path.c_str(), &lx, &ly, &lnc, &lil), SUCCEED);
  EXPECT_EQ(lx, 256);
  EXPECT_EQ(ly, 1);
  EXPECT_EQ(lnc, 3);

  std::vector<uint8_t> read_pal(256 * 3);
  EXPECT_EQ(DFGRgetlut(path.c_str(), read_pal.data(), 256, 1), SUCCEED);
  EXPECT_EQ(read_pal, pal);

  std::vector<uint8_t> read_img(width * height);
  EXPECT_EQ(DFGRgetimage(path.c_str(), read_img.data(), width, height),
            SUCCEED);
  EXPECT_EQ(read_img, img);

  // Clear set LUT
  EXPECT_EQ(DFGRsetlut(nullptr, 0, 0), SUCCEED);
}

TEST_F(DfgrTest, MultipleImagesAndReadRef) {
  const std::string path = GetTempFilePath("test_multi.hdf");
  std::filesystem::remove(path);
  absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

  constexpr int32_t width = 8;
  constexpr int32_t height = 8;
  std::vector<uint8_t> img1(width * height, 11);
  std::vector<uint8_t> img2(width * height, 22);
  std::vector<uint8_t> img3(width * height, 33);

  EXPECT_EQ(DFGRsetimdims(width, height, 1, 0), SUCCEED);
  EXPECT_EQ(DFGRaddimage(path.c_str(), img1.data(), width, height), SUCCEED);
  const uint16_t ref1 = DFGRIlastref();

  EXPECT_EQ(DFGRsetimdims(width, height, 1, 0), SUCCEED);
  EXPECT_EQ(DFGRaddimage(path.c_str(), img2.data(), width, height), SUCCEED);
  const uint16_t ref2 = DFGRIlastref();

  EXPECT_EQ(DFGRsetimdims(width, height, 1, 0), SUCCEED);
  EXPECT_EQ(DFGRaddimage(path.c_str(), img3.data(), width, height), SUCCEED);
  const uint16_t ref3 = DFGRIlastref();

  EXPECT_NE(ref1, ref2);
  EXPECT_NE(ref2, ref3);

  // Seek to image 2 with DFGRreadref
  EXPECT_EQ(DFGRreadref(path.c_str(), ref2), SUCCEED);

  int32_t rw = 0;
  int32_t rh = 0;
  int rnc = 0;
  int ril = 0;
  EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), SUCCEED);
  std::vector<uint8_t> rdata(width * height);
  EXPECT_EQ(DFGRgetimage(path.c_str(), rdata.data(), width, height), SUCCEED);
  EXPECT_EQ(rdata, img2);

  // Read next (image 3)
  EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), SUCCEED);
  EXPECT_EQ(DFGRgetimage(path.c_str(), rdata.data(), width, height), SUCCEED);
  EXPECT_EQ(rdata, img3);

  // Past the end should fail
  EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), FAIL);

  // Seek to ref1
  EXPECT_EQ(DFGRreadref(path.c_str(), ref1), SUCCEED);
  EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), SUCCEED);
  EXPECT_EQ(DFGRgetimage(path.c_str(), rdata.data(), width, height), SUCCEED);
  EXPECT_EQ(rdata, img1);

  // Non-existent ref
  EXPECT_EQ(DFGRreadref(path.c_str(), 9999), FAIL);
}

// Convert 3D coords between interlaces
static std::vector<uint8_t> ConvertInterlace(const std::vector<uint8_t>& src,
                                             int32_t width, int32_t height,
                                             int ncomps, int src_il,
                                             int dst_il) {
  std::vector<uint8_t> dst(src.size());
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      for (int c = 0; c < ncomps; ++c) {
        size_t src_idx = 0;
        if (src_il == 0) {  // Pixel: (y * width + x) * ncomps + c
          src_idx = (y * width + x) * ncomps + c;
        } else if (src_il == 1) {  // Line: (y * ncomps + c) * width + x
          src_idx = (y * ncomps + c) * width + x;
        } else {  // Plane: (c * height + y) * width + x
          src_idx = (c * height + y) * width + x;
        }

        size_t dst_idx = 0;
        if (dst_il == 0) {
          dst_idx = (y * width + x) * ncomps + c;
        } else if (dst_il == 1) {
          dst_idx = (y * ncomps + c) * width + x;
        } else {
          dst_idx = (c * height + y) * width + x;
        }
        dst[dst_idx] = src[src_idx];
      }
    }
  }
  return dst;
}

TEST_F(DfgrTest, InterlaceConversions24Bit) {
  constexpr int32_t width = 4;
  constexpr int32_t height = 3;
  constexpr int ncomps = 3;
  const auto src_pixel = MakePatternImage(width, height, ncomps);

  const int il_modes[3] = {0, 1, 2};

  for (int write_il : il_modes) {
    const std::string filename =
        "test_il_w" + std::to_string(write_il) + ".hdf";
    const std::string path = GetTempFilePath(filename);
    std::filesystem::remove(path);
    absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

    // Prepare data in write_il ordering
    const auto write_data =
        ConvertInterlace(src_pixel, width, height, ncomps, 0, write_il);

    for (int read_il : il_modes) {
      // Re-write file
      EXPECT_EQ(DFGRsetimdims(width, height, ncomps, write_il), SUCCEED);
      EXPECT_EQ(
          DFGRputimage(path.c_str(), const_cast<uint8_t*>(write_data.data()),
                       width, height),
          SUCCEED);

      // Set requested read interlace
      EXPECT_EQ(DFGRreqimil(read_il), SUCCEED);

      int32_t rw = 0;
      int32_t rh = 0;
      int rnc = 0;
      int ril = 0;
      EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), SUCCEED);
      EXPECT_EQ(rw, width);
      EXPECT_EQ(rh, height);
      EXPECT_EQ(rnc, ncomps);
      EXPECT_EQ(ril, write_il);

      std::vector<uint8_t> read_buf(width * height * ncomps);
      EXPECT_EQ(DFGRgetimage(path.c_str(), read_buf.data(), width, height),
                SUCCEED);

      const auto expected =
          ConvertInterlace(src_pixel, width, height, ncomps, 0, read_il);
      EXPECT_EQ(read_buf, expected) << "Mismatch with write_il=" << write_il
                                    << " and read_il=" << read_il;
    }
  }
}

TEST_F(DfgrTest, LutInterlaceConversion) {
  const std::string path = GetTempFilePath("test_lut_il.hdf");
  std::filesystem::remove(path);
  absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

  const int32_t init_fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(init_fid, FAIL);
  ASSERT_NE(Hclose(init_fid), FAIL);

  constexpr int32_t width = 4;
  constexpr int32_t height = 2;
  constexpr int ncomps = 3;
  const auto src_lut = MakePatternImage(width, height, ncomps);

  EXPECT_EQ(DFGRsetlutdims(width, height, ncomps, 0), SUCCEED);
  EXPECT_EQ(DFGRaddlut(path.c_str(), const_cast<uint8_t*>(src_lut.data()),
                       width, height),
            SUCCEED);

  // Request interlace mode 1 for LUT
  EXPECT_EQ(DFGRreqlutil(1), SUCCEED);

  int32_t lx = 0;
  int32_t ly = 0;
  int lnc = 0;
  int lil = 0;
  EXPECT_EQ(DFGRgetlutdims(path.c_str(), &lx, &ly, &lnc, &lil), SUCCEED);
  EXPECT_EQ(lx, width);
  EXPECT_EQ(ly, height);

  std::vector<uint8_t> read_lut(width * height * ncomps);
  EXPECT_EQ(DFGRgetlut(path.c_str(), read_lut.data(), width, height), SUCCEED);

  const auto expected = ConvertInterlace(src_lut, width, height, ncomps, 0, 1);
  EXPECT_EQ(read_lut, expected);
}

TEST_F(DfgrTest, CompressedImages) {
  constexpr int32_t width = 16;
  constexpr int32_t height = 16;
  const auto img8 = MakePatternImage(width, height, 1);

  // 1. RLE
  {
    const std::string path = GetTempFilePath("test_rle.hdf");
    std::filesystem::remove(path);
    absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

    comp_info cinfo{};
    EXPECT_EQ(DFGRsetcompress(COMP_RLE, &cinfo), SUCCEED);
    EXPECT_EQ(DFGRsetimdims(width, height, 1, 0), SUCCEED);
    EXPECT_EQ(DFGRputimage(path.c_str(), const_cast<uint8_t*>(img8.data()),
                           width, height),
              SUCCEED);

    std::vector<uint8_t> read_buf(width * height);
    EXPECT_EQ(DFGRgetimage(path.c_str(), read_buf.data(), width, height),
              SUCCEED);
    EXPECT_EQ(read_buf, img8);

    // Unsupported requested interlace for compressed image
    EXPECT_EQ(DFGRreqimil(1), SUCCEED);
    DFGRIrestart();
    EXPECT_EQ(DFGRgetimage(path.c_str(), read_buf.data(), width, height), FAIL);
    EXPECT_EQ(DFGRreqimil(0), SUCCEED);
  }

  // 3. IMCOMP (requires palette)
  {
    const std::string path = GetTempFilePath("test_imcomp.hdf");
    std::filesystem::remove(path);
    absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

    const auto pal = MakePalette();
    EXPECT_EQ(DFGRsetlutdims(256, 1, 3, 0), SUCCEED);
    EXPECT_EQ(DFGRsetlut(const_cast<uint8_t*>(pal.data()), 256, 1), SUCCEED);

    comp_info cinfo{};
    EXPECT_EQ(DFGRsetcompress(COMP_IMCOMP, &cinfo), SUCCEED);
    EXPECT_EQ(DFGRsetimdims(width, height, 1, 0), SUCCEED);
    EXPECT_EQ(DFGRputimage(path.c_str(), const_cast<uint8_t*>(img8.data()),
                           width, height),
              SUCCEED);

    std::vector<uint8_t> read_buf(width * height);
    EXPECT_EQ(DFGRgetimage(path.c_str(), read_buf.data(), width, height),
              SUCCEED);
    EXPECT_FALSE(read_buf.empty());

    EXPECT_EQ(DFGRsetlut(nullptr, 0, 0), SUCCEED);
  }

  // 4. JPEG (24-bit)
  {
    const std::string path = GetTempFilePath("test_jpeg.hdf");
    std::filesystem::remove(path);
    absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

    const auto img24 = MakePatternImage(width, height, 3);
    comp_info cinfo{};
    cinfo.jpeg.quality = 75;
    cinfo.jpeg.force_baseline = 1;
    EXPECT_EQ(DFGRsetcompress(COMP_JPEG, &cinfo), SUCCEED);
    EXPECT_EQ(DFGRsetimdims(width, height, 3, 0), SUCCEED);
    EXPECT_EQ(DFGRputimage(path.c_str(), const_cast<uint8_t*>(img24.data()),
                           width, height),
              SUCCEED);

    std::vector<uint8_t> read_buf(width * height * 3);
    EXPECT_EQ(DFGRgetimage(path.c_str(), read_buf.data(), width, height),
              SUCCEED);
    EXPECT_FALSE(read_buf.empty());
  }

  // Reset compression
  EXPECT_EQ(DFGRsetcompress(COMP_NONE, nullptr), SUCCEED);
}

TEST_F(DfgrTest, LegacyRaster8Reading) {
  // Test reading DFTAG_RI8, DFTAG_CI8, DFTAG_II8 with DFTAG_ID8 and DFTAG_IP8
  // 1. DFTAG_RI8
  {
    const std::string path = GetTempFilePath("test_legacy_ri8.hdf");
    std::filesystem::remove(path);
    absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    constexpr uint16_t ref = 100;
    // DFTAG_ID8: uint16 xdim, uint16 ydim
    const uint8_t id8_buf[4] = {0, 8, 0, 8};
    ASSERT_NE(Hputelement(fid, DFTAG_ID8, ref, id8_buf, sizeof(id8_buf)), FAIL);

    const std::vector<uint8_t> img8(64, 77);
    ASSERT_NE(Hputelement(fid, DFTAG_RI8, ref, img8.data(), img8.size()), FAIL);

    // Optional palette DFTAG_IP8
    const std::vector<uint8_t> pal8(256 * 3, 99);
    ASSERT_NE(Hputelement(fid, DFTAG_IP8, ref, pal8.data(), pal8.size()), FAIL);

    ASSERT_NE(Hclose(fid), FAIL);

    // Read with DFGR
    int32_t rw = 0;
    int32_t rh = 0;
    int rnc = 0;
    int ril = 0;
    EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), SUCCEED);
    EXPECT_EQ(rw, 8);
    EXPECT_EQ(rh, 8);
    EXPECT_EQ(rnc, 1);

    int32_t lx = 0;
    int32_t ly = 0;
    int lnc = 0;
    int lil = 0;
    EXPECT_EQ(DFGRgetlutdims(path.c_str(), &lx, &ly, &lnc, &lil), SUCCEED);
    EXPECT_EQ(lx, 256);
    EXPECT_EQ(ly, 1);
    EXPECT_EQ(lnc, 3);

    std::vector<uint8_t> rpal(256 * 3);
    EXPECT_EQ(DFGRgetlut(path.c_str(), rpal.data(), 256, 1), SUCCEED);
    EXPECT_EQ(rpal, pal8);

    std::vector<uint8_t> rimg(64);
    EXPECT_EQ(DFGRgetimage(path.c_str(), rimg.data(), 8, 8), SUCCEED);
    EXPECT_EQ(rimg, img8);
  }

  // 2. DFTAG_CI8
  {
    const std::string path = GetTempFilePath("test_legacy_ci8.hdf");
    std::filesystem::remove(path);
    absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    constexpr uint16_t ref = 200;
    const uint8_t id8_buf[4] = {0, 4, 0, 4};
    ASSERT_NE(Hputelement(fid, DFTAG_ID8, ref, id8_buf, sizeof(id8_buf)), FAIL);

    // Write RLE-compressed 16-byte image
    std::vector<uint8_t> raw_img(16, 55);
    comp_info cinfo{};
    ASSERT_NE(DFputcomp(fid, DFTAG_CI8, ref, raw_img.data(), 4, 4, nullptr,
                        nullptr, DFTAG_RLE, &cinfo),
              FAIL);

    ASSERT_NE(Hclose(fid), FAIL);

    int32_t rw = 0;
    int32_t rh = 0;
    int rnc = 0;
    int ril = 0;
    EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), SUCCEED);
    EXPECT_EQ(rw, 4);
    EXPECT_EQ(rh, 4);

    std::vector<uint8_t> rimg(16);
    EXPECT_EQ(DFGRgetimage(path.c_str(), rimg.data(), 4, 4), SUCCEED);
    EXPECT_EQ(rimg, raw_img);
  }

  // 3. DFTAG_II8 (IMCOMP compressed 8-bit image)
  {
    const std::string path = GetTempFilePath("test_legacy_ii8.hdf");
    std::filesystem::remove(path);
    absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    constexpr uint16_t ref = 300;
    const uint8_t id8_buf[4] = {0, 4, 0, 4};
    ASSERT_NE(Hputelement(fid, DFTAG_ID8, ref, id8_buf, sizeof(id8_buf)), FAIL);

    std::vector<uint8_t> raw_img(16, 88);
    auto pal = MakePalette();
    comp_info cinfo{};
    std::vector<uint8_t> newpal(256 * 3);
    ASSERT_NE(DFputcomp(fid, DFTAG_II8, ref, raw_img.data(), 4, 4, pal.data(),
                        newpal.data(), DFTAG_IMC, &cinfo),
              FAIL);

    ASSERT_NE(Hclose(fid), FAIL);

    int32_t rw = 0;
    int32_t rh = 0;
    int rnc = 0;
    int ril = 0;
    EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), SUCCEED);
    EXPECT_EQ(rw, 4);
    EXPECT_EQ(rh, 4);
    EXPECT_EQ(rnc, 1);

    std::vector<uint8_t> rimg(16);
    EXPECT_EQ(DFGRgetimage(path.c_str(), rimg.data(), 4, 4), SUCCEED);
    EXPECT_FALSE(rimg.empty());
  }

  // 4. Missing DFTAG_ID8 for legacy raster
  {
    const std::string path = GetTempFilePath("test_legacy_noid8.hdf");
    std::filesystem::remove(path);
    absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    constexpr uint16_t ref = 400;
    const std::vector<uint8_t> img8(64, 1);
    ASSERT_NE(Hputelement(fid, DFTAG_RI8, ref, img8.data(), img8.size()), FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    int32_t rw = 0;
    int32_t rh = 0;
    int rnc = 0;
    int ril = 0;
    EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), FAIL);
  }
}

TEST_F(DfgrTest, ErrorPaths) {
  // Bad filename / nonexistent file
  int32_t rw = 0;
  int32_t rh = 0;
  int rnc = 0;
  int ril = 0;
  EXPECT_EQ(DFGRgetimdims("nonexistent_file.hdf", &rw, &rh, &rnc, &ril), FAIL);
  EXPECT_EQ(DFGRgetlutdims("nonexistent_file.hdf", &rw, &rh, &rnc, &ril), FAIL);

  // File with no images or RIGs
  const std::string empty_path = GetTempFilePath("empty.hdf");
  std::filesystem::remove(empty_path);
  absl::Cleanup cleanup = [&] { std::filesystem::remove(empty_path); };

  const int32_t fid = Hopen(empty_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  ASSERT_NE(Hclose(fid), FAIL);

  EXPECT_EQ(DFGRgetimdims(empty_path.c_str(), &rw, &rh, &rnc, &ril), FAIL);
  EXPECT_EQ(DFGRgetlutdims(empty_path.c_str(), &rw, &rh, &rnc, &ril), FAIL);

  // Write an image without a palette, then query palette
  const std::string no_pal_path = GetTempFilePath("no_pal.hdf");
  std::filesystem::remove(no_pal_path);
  absl::Cleanup cleanup2 = [&] { std::filesystem::remove(no_pal_path); };

  const auto img = MakePatternImage(8, 8, 1);
  EXPECT_EQ(
      DFGRputimage(no_pal_path.c_str(), const_cast<uint8_t*>(img.data()), 8, 8),
      SUCCEED);

  EXPECT_EQ(DFGRgetimdims(no_pal_path.c_str(), &rw, &rh, &rnc, &ril), SUCCEED);
  // Expect getlutdims to fail with DFE_NOMATCH since no palette exists
  EXPECT_EQ(DFGRgetlutdims(no_pal_path.c_str(), &rw, &rh, &rnc, &ril), FAIL);

  // Dimension mismatch on DFGRgetimage
  std::vector<uint8_t> bad_buf(64);
  EXPECT_EQ(DFGRgetimage(no_pal_path.c_str(), bad_buf.data(), 10, 10), FAIL);

  // Addimage with null data
  EXPECT_EQ(DFGRaddimage(no_pal_path.c_str(), nullptr, 8, 8), FAIL);

  // Addimage with dimension mismatch
  EXPECT_EQ(DFGRsetimdims(8, 8, 1, 0), SUCCEED);
  EXPECT_EQ(DFGRaddimage(no_pal_path.c_str(), bad_buf.data(), 16, 16), FAIL);

  // Requested il >= ncomps
  EXPECT_EQ(DFGRreqimil(5), SUCCEED);
  DFGRIrestart();
  EXPECT_EQ(DFGRgetimage(no_pal_path.c_str(), bad_buf.data(), 8, 8), FAIL);
  EXPECT_EQ(DFGRreqimil(0), SUCCEED);

  // Low-level DFGRIopen bad call
  EXPECT_EQ(DFGRIopen(nullptr, DFACC_READ), FAIL);

  // DFGRIgetimlut bad open
  int c = 0;
  uint16_t ct = 0;
  int hp = 0;
  EXPECT_EQ(DFGRIgetimlut("nonexistent.hdf", bad_buf.data(), 8, 8, 1, 0, &c,
                          &ct, &hp),
            FAIL);

  // DFGRIgetimlut on LUT when Grnewdata == 0
  EXPECT_EQ(DFGRIgetimlut(no_pal_path.c_str(), bad_buf.data(), 8, 8, 0, 0, &c,
                          &ct, &hp),
            FAIL);

  // DFGRreadref on nonexistent file
  EXPECT_EQ(DFGRreadref("nonexistent.hdf", 1), FAIL);

  // COMP_IMCOMP without setting a palette
  comp_info cinfo_imc{};
  EXPECT_EQ(DFGRsetcompress(COMP_IMCOMP, &cinfo_imc), SUCCEED);
  EXPECT_EQ(DFGRaddimage(no_pal_path.c_str(), bad_buf.data(), 8, 8), FAIL);
  EXPECT_EQ(DFGRsetcompress(COMP_NONE, nullptr), SUCCEED);

  // Unwritable path for addimage
  EXPECT_EQ(
      DFGRaddimage("/nonexistent_dir/cannot_write.hdf", bad_buf.data(), 8, 8),
      FAIL);

  // Open file with DFACC_CREATE after writing image with LUT to exercise line
  // 701
  {
    const std::string pal_file = GetTempFilePath("pal_file.hdf");
    std::filesystem::remove(pal_file);
    absl::Cleanup cleanup3 = [&] { std::filesystem::remove(pal_file); };
    auto pal = MakePalette();
    EXPECT_EQ(DFGRsetlutdims(256, 1, 3, 0), SUCCEED);
    EXPECT_EQ(DFGRsetlut(pal.data(), 256, 1), SUCCEED);
    EXPECT_EQ(DFGRsetimdims(8, 8, 1, 0), SUCCEED);
    EXPECT_EQ(DFGRaddimage(pal_file.c_str(), bad_buf.data(), 8, 8), SUCCEED);
    // Opening new file with DFACC_CREATE triggers Ref.lut = 0 reset
    int32_t fid2 = DFGRIopen(no_pal_path.c_str(), DFACC_CREATE);
    if (fid2 != FAIL) Hclose(fid2);
    EXPECT_EQ(DFGRsetlut(nullptr, 0, 0), SUCCEED);
  }
}

TEST_F(DfgrTest, CorruptedRigScenarios) {
  const std::string path = GetTempFilePath("corrupt_rig.hdf");
  std::filesystem::remove(path);
  absl::Cleanup cleanup = [&] { std::filesystem::remove(path); };

  // 1. Invalid NT element in RIG
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    constexpr uint16_t ref = 50;

    // Bad NT string: width != 8 or type != DFNT_UCHAR
    const uint8_t bad_nt[4] = {DFNT_VERSION, DFNT_FLOAT, 32, DFNTC_BYTE};
    ASSERT_NE(Hputelement(fid, DFTAG_NT, ref, bad_nt, sizeof(bad_nt)), FAIL);

    // ID buffer pointing to bad NT
    uint8_t id_buf[64] = {};
    uint8_t* p = id_buf;
    INT32ENCODE(p, 8);          // xdim
    INT32ENCODE(p, 8);          // ydim
    UINT16ENCODE(p, DFTAG_NT);  // nt.tag
    UINT16ENCODE(p, ref);       // nt.ref
    INT16ENCODE(p, 1);          // ncomps
    INT16ENCODE(p, 0);          // interlace
    UINT16ENCODE(p, 0);         // compr.tag
    UINT16ENCODE(p, 0);         // compr.ref
    ASSERT_NE(Hputelement(fid, DFTAG_ID, ref, id_buf, p - id_buf), FAIL);

    const std::vector<uint8_t> dummy(64, 0);
    ASSERT_NE(Hputelement(fid, DFTAG_RI, ref, dummy.data(), dummy.size()),
              FAIL);

    const int32_t gid = DFdisetup(3);
    ASSERT_NE(gid, FAIL);
    ASSERT_EQ(DFdiput(gid, DFTAG_ID, ref), SUCCEED);
    ASSERT_EQ(DFdiput(gid, DFTAG_RI, ref), SUCCEED);
    ASSERT_GT(DFdiwrite(fid, gid, DFTAG_RIG, ref), 0);

    ASSERT_NE(Hclose(fid), FAIL);

    int32_t rw = 0;
    int32_t rh = 0;
    int rnc = 0;
    int ril = 0;
    EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), FAIL);
  }

  // 2. Missing DFTAG_ID element
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    constexpr uint16_t ref = 60;
    const int32_t gid = DFdisetup(2);
    ASSERT_NE(gid, FAIL);
    ASSERT_EQ(DFdiput(gid, DFTAG_ID, 999), SUCCEED);  // 999 does not exist
    ASSERT_GT(DFdiwrite(fid, gid, DFTAG_RIG, ref), 0);
    ASSERT_NE(Hclose(fid), FAIL);

    int32_t rw = 0;
    int32_t rh = 0;
    int rnc = 0;
    int ril = 0;
    EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), FAIL);
  }

  // 3. Missing DFTAG_NT element pointed to by DFTAG_ID
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    constexpr uint16_t ref = 70;
    uint8_t id_buf[64] = {};
    uint8_t* p = id_buf;
    INT32ENCODE(p, 8);
    INT32ENCODE(p, 8);
    UINT16ENCODE(p, DFTAG_NT);
    UINT16ENCODE(p, 999);  // NT 999 does not exist
    INT16ENCODE(p, 1);
    INT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    ASSERT_NE(Hputelement(fid, DFTAG_ID, ref, id_buf, p - id_buf), FAIL);

    const int32_t gid = DFdisetup(2);
    ASSERT_NE(gid, FAIL);
    ASSERT_EQ(DFdiput(gid, DFTAG_ID, ref), SUCCEED);
    ASSERT_GT(DFdiwrite(fid, gid, DFTAG_RIG, ref), 0);
    ASSERT_NE(Hclose(fid), FAIL);

    int32_t rw = 0;
    int32_t rh = 0;
    int rnc = 0;
    int ril = 0;
    EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), FAIL);
  }

  // 4. Missing DFTAG_ID element in RIG (Hgetelement for ID fails)
  DFGRIrestart();
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    constexpr uint16_t ref = 80;
    uint8_t bad_group[4] = {};
    uint8_t* p = bad_group;
    UINT16ENCODE(p, DFTAG_ID);
    UINT16ENCODE(p, ref);
    ASSERT_NE(Hputelement(fid, DFTAG_RIG, ref, bad_group, sizeof(bad_group)),
              FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    int32_t rw = 0;
    int32_t rh = 0;
    int rnc = 0;
    int ril = 0;
    EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), FAIL);
  }

  // 5. RIG with missing DFTAG_RI element when reading image
  DFGRIrestart();
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    constexpr uint16_t ref = 90;
    const uint8_t nt[4] = {DFNT_VERSION, DFNT_UCHAR, 8, DFNTC_BYTE};
    ASSERT_NE(Hputelement(fid, DFTAG_NT, ref, nt, sizeof(nt)), FAIL);

    uint8_t id_buf[64] = {};
    uint8_t* p = id_buf;
    INT32ENCODE(p, 4);
    INT32ENCODE(p, 4);
    UINT16ENCODE(p, DFTAG_NT);
    UINT16ENCODE(p, ref);
    INT16ENCODE(p, 1);
    INT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    ASSERT_NE(Hputelement(fid, DFTAG_ID, ref, id_buf, p - id_buf), FAIL);

    // Put image element then delete its DD
    const std::vector<uint8_t> dummy(16, 0);
    ASSERT_NE(Hputelement(fid, DFTAG_RI, ref, dummy.data(), dummy.size()),
              FAIL);
    ASSERT_NE(Hdeldd(fid, DFTAG_RI, ref), FAIL);

    const int32_t gid = DFdisetup(3);
    ASSERT_NE(gid, FAIL);
    ASSERT_EQ(DFdiput(gid, DFTAG_ID, ref), SUCCEED);
    ASSERT_EQ(DFdiput(gid, DFTAG_RI, ref), SUCCEED);
    ASSERT_GT(DFdiwrite(fid, gid, DFTAG_RIG, ref), 0);
    ASSERT_NE(Hclose(fid), FAIL);

    int32_t rw = 0;
    int32_t rh = 0;
    int rnc = 0;
    int ril = 0;
    EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), SUCCEED);
    std::vector<uint8_t> rdata(16);
    EXPECT_EQ(DFGRgetimage(path.c_str(), rdata.data(), 4, 4), FAIL);

    // Also test with interlace conversion (DFGRreqimil(1)) where Hstartread
    // fails
    EXPECT_EQ(DFGRreqimil(1), SUCCEED);
    DFGRIrestart();
    EXPECT_EQ(DFGRgetimage(path.c_str(), rdata.data(), 4, 4), FAIL);
    EXPECT_EQ(DFGRreqimil(0), SUCCEED);
  }

  // 6. Truncated image element during interlace conversion (Hread failure)
  DFGRIrestart();
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);

    constexpr uint16_t ref = 95;
    const uint8_t nt[4] = {DFNT_VERSION, DFNT_UCHAR, 8, DFNTC_BYTE};
    ASSERT_NE(Hputelement(fid, DFTAG_NT, ref, nt, sizeof(nt)), FAIL);

    uint8_t id_buf[64] = {};
    uint8_t* p = id_buf;
    INT32ENCODE(p, 4);
    INT32ENCODE(p, 4);
    UINT16ENCODE(p, DFTAG_NT);
    UINT16ENCODE(p, ref);
    INT16ENCODE(p, 3);
    INT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    UINT16ENCODE(p, 0);
    ASSERT_NE(Hputelement(fid, DFTAG_ID, ref, id_buf, p - id_buf), FAIL);

    // Only 2 bytes instead of 4 * 4 * 3 = 48 bytes
    const uint8_t short_data[2] = {1, 2};
    ASSERT_NE(Hputelement(fid, DFTAG_RI, ref, short_data, sizeof(short_data)),
              FAIL);

    const int32_t gid = DFdisetup(3);
    ASSERT_NE(gid, FAIL);
    ASSERT_EQ(DFdiput(gid, DFTAG_ID, ref), SUCCEED);
    ASSERT_EQ(DFdiput(gid, DFTAG_RI, ref), SUCCEED);
    ASSERT_GT(DFdiwrite(fid, gid, DFTAG_RIG, ref), 0);
    ASSERT_NE(Hclose(fid), FAIL);

    int32_t rw = 0;
    int32_t rh = 0;
    int rnc = 0;
    int ril = 0;
    EXPECT_EQ(DFGRgetimdims(path.c_str(), &rw, &rh, &rnc, &ril), SUCCEED);
    // Truncate file so Hread fails during reading
    {
      FILE* f = fopen(path.c_str(), "r+b");
      ASSERT_NE(f, nullptr);
      int fd = fileno(f);
      ASSERT_EQ(ftruncate(fd, 32), 0);  // truncate before data offset
      fclose(f);
    }
    DFGRIrestart();
    std::vector<uint8_t> rdata(48);
    EXPECT_EQ(DFGRgetimage(path.c_str(), rdata.data(), 4, 4), FAIL);
    EXPECT_EQ(DFGRreqimil(0), SUCCEED);
  }
}

TEST_F(DfgrTest, AdditionalCoverageScenarios) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfgr_additional.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  DFGRIrestart();

  // 1. Direct DFGRgetimage on an empty HDF file (hits line 974)
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    std::vector<uint8_t> buf(16);
    EXPECT_EQ(DFGRgetimage(path.c_str(), buf.data(), 4, 4), FAIL);
  }

  // 2. Request interlace >= ncomponents on 1-component image (hits lines
  // 1000-1001)
  {
    std::vector<uint8_t> img(16, 42);
    EXPECT_EQ(DFGRsetimdims(4, 4, 1, 0), SUCCEED);
    EXPECT_EQ(DFGRaddimage(path.c_str(), img.data(), 4, 4), SUCCEED);

    DFGRIrestart();
    int32_t w = 0, h = 0;
    int nc = 0, il = 0;
    EXPECT_EQ(DFGRgetimdims(path.c_str(), &w, &h, &nc, &il), SUCCEED);
    EXPECT_EQ(nc, 1);

    EXPECT_EQ(DFGRreqimil(2), SUCCEED);
    std::vector<uint8_t> rbuf(16);
    EXPECT_EQ(DFGRgetimage(path.c_str(), rbuf.data(), 4, 4), FAIL);
    EXPECT_EQ(DFGRreqimil(0), SUCCEED);
  }

  // 3. Compressed image where decompression fails (hits line 994)
  {
    const std::string comp_path = dir + "/dfgr_comp_corrupt.hdf";
    (void)remove(comp_path.c_str());
    const absl::Cleanup comp_cleanup = [&comp_path] {
      (void)remove(comp_path.c_str());
    };

    DFGRIrestart();
    std::vector<uint8_t> img(16, 55);
    comp_info cinfo = {};
    EXPECT_EQ(DFGRsetcompress(COMP_RLE, &cinfo), SUCCEED);
    EXPECT_EQ(DFGRsetimdims(4, 4, 1, 0), SUCCEED);
    EXPECT_EQ(DFGRaddimage(comp_path.c_str(), img.data(), 4, 4), SUCCEED);
    EXPECT_EQ(DFGRsetcompress(COMP_NONE, nullptr), SUCCEED);

    DFGRIrestart();
    int32_t w = 0, h = 0;
    int nc = 0, il = 0;
    EXPECT_EQ(DFGRgetimdims(comp_path.c_str(), &w, &h, &nc, &il), SUCCEED);

    // Delete the compressed image DD (DFTAG_CI)
    const int32_t fid = Hopen(comp_path.c_str(), DFACC_RDWR, 0);
    ASSERT_NE(fid, FAIL);
    const uint16_t ref = DFGRIlastref();
    ASSERT_NE(Hdeldd(fid, DFTAG_CI, ref), FAIL);
    ASSERT_NE(Hclose(fid), FAIL);

    DFGRIrestart();
    // Re-read dims to populate Grread with the RIG
    EXPECT_EQ(DFGRgetimdims(comp_path.c_str(), &w, &h, &nc, &il), SUCCEED);
    std::vector<uint8_t> rbuf(16);
    EXPECT_EQ(DFGRgetimage(comp_path.c_str(), rbuf.data(), 4, 4), FAIL);
  }
}

}  // namespace
}  // namespace hdf4
