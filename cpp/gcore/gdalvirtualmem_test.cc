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
// Tests dataset and raster band virtual memory mappings
// (gdalvirtualmem.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalvirtualmem.cpp

#include <cstddef>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_virtualmem.h"

namespace autotest2 {
namespace {

using VirtualMemPtr =
    std::unique_ptr<CPLVirtualMem,
                    decltype([](CPLVirtualMem* p) { CPLVirtualMemFree(p); })>;

class GdalVirtualMemTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }

  static GDALDatasetUniquePtr CreateMemDS(int width, int height, int bands,
                                          GDALDataType dt = GDT_Byte) {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
    if (drv == nullptr) {
      return nullptr;
    }
    return GDALDatasetUniquePtr(
        drv->Create("", width, height, bands, dt, nullptr));
  }
};

TEST_F(GdalVirtualMemTest, ParameterValidationErrors) {
  GDALDatasetUniquePtr ds = CreateMemDS(64, 64, 3, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  GDALDatasetH hds = GDALDataset::ToHandle(ds.get());
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  GDALRasterBandH hband = GDALRasterBand::ToHandle(band);

  WithQuietHandler quiet;
  CPLErrorReset();

  // 1. nXSize != nBufXSize || nYSize != nBufYSize:
  EXPECT_EQ(nullptr, GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 16,
                                              16, GDT_Byte, 1, nullptr, 0, 0, 0,
                                              4096, 0, TRUE, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // 2. Invalid window requests:
  EXPECT_EQ(nullptr, GDALRasterBandGetVirtualMem(hband, GF_Read, -1, 0, 32, 32,
                                                 32, 32, GDT_Byte, 0, 0, 4096,
                                                 0, TRUE, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  EXPECT_EQ(nullptr, GDALRasterBandGetVirtualMem(hband, GF_Read, 0, 0, 100, 32,
                                                 100, 32, GDT_Byte, 0, 0, 4096,
                                                 0, TRUE, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // 3. Negative spacing:
  EXPECT_EQ(nullptr, GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32,
                                              32, GDT_Byte, 1, nullptr, -1, 0,
                                              0, 4096, 0, TRUE, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // 4. GDALCheckBandParameters errors:
  EXPECT_EQ(nullptr, GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32,
                                              32, GDT_Byte, 0, nullptr, 0, 0, 0,
                                              4096, 0, TRUE, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  int bad_map[1] = {5};
  EXPECT_EQ(nullptr, GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32,
                                              32, GDT_Byte, 1, bad_map, 0, 0, 0,
                                              4096, 0, TRUE, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  EXPECT_EQ(nullptr, GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32,
                                              32, GDT_Byte, 5, nullptr, 0, 0, 0,
                                              4096, 0, TRUE, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // 5. Unsupported interleaving:
  // nLineSpace < nBufXSize * nPixelSpace:
  EXPECT_EQ(nullptr, GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32,
                                              32, GDT_Byte, 2, nullptr, 2, 10,
                                              0, 4096, 0, TRUE, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // nBandCount > 1 && nBandSpace == nPixelSpace:
  EXPECT_EQ(nullptr, GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32,
                                              32, GDT_Byte, 2, nullptr, 2, 64,
                                              2, 4096, 0, TRUE, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // nBandSpace > nPixelSpace && nBandSpace < nBufYSize * nLineSpace:
  EXPECT_EQ(nullptr, GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32,
                                              32, GDT_Byte, 2, nullptr, 1, 32,
                                              100, 4096, 0, TRUE, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // 6. Spacing not a multiple of data type size:
  EXPECT_EQ(nullptr, GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32,
                                              32, GDT_UInt16, 1, nullptr, 3, 96,
                                              0, 4096, 0, TRUE, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // 7. Tiled virtual mem errors:
  EXPECT_EQ(nullptr, GDALDatasetGetTiledVirtualMem(
                         hds, GF_Read, -1, 0, 64, 64, 64, 64, GDT_Byte, 1,
                         nullptr, GTO_BSQ, 4096, TRUE, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  EXPECT_EQ(nullptr, GDALDatasetGetTiledVirtualMem(
                         hds, GF_Read, 0, 0, 64, 64, 64, 64, GDT_Byte, 0,
                         nullptr, GTO_BSQ, 4096, TRUE, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // Incompatible tile size with page size (10*10 = 100 % 4096 != 0):
  EXPECT_EQ(nullptr, GDALDatasetGetTiledVirtualMem(
                         hds, GF_Read, 0, 0, 64, 64, 10, 10, GDT_Byte, 1,
                         nullptr, GTO_BSQ, 4096, TRUE, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(GdalVirtualMemTest, BandSequentialReadAndWrite) {
  const size_t page = CPLGetPageSize();
  ASSERT_NE(0u, page);

  // Case A: Scanline wider than 1 page (nBufXSize = page + 904) and multiple
  // lines to test partial-line, end-of-line, and multi-line I/O in
  // DoIOBandSequential.
  const int wide_x = static_cast<int>(page + 904);
  GDALDatasetUniquePtr ds_wide = CreateMemDS(wide_x, 2, 2, GDT_Byte);
  ASSERT_NE(nullptr, ds_wide);
  EXPECT_EQ(CE_None, ds_wide->GetRasterBand(1)->Fill(42));
  EXPECT_EQ(CE_None, ds_wide->GetRasterBand(2)->Fill(84));

  int band_map[2] = {1, 2};
  VirtualMemPtr vmem(GDALDatasetGetVirtualMem(
      GDALDataset::ToHandle(ds_wide.get()), GF_Write, 0, 0, wide_x, 2, wide_x,
      2, GDT_Byte, 2, band_map, 0, 0, 0, page, page, TRUE, nullptr));
  ASSERT_NE(nullptr, vmem);
  auto* ptr = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem.get()));
  ASSERT_NE(nullptr, ptr);

  // Pin and modify pages across band 1 and band 2; with cache size = page,
  // pinning subsequent pages evicts dirty pages and calls
  // SaveFromCacheBandSequential.
  const size_t total_bsq_bytes = static_cast<size_t>(wide_x) * 4;
  for (size_t offset = 0; offset < total_bsq_bytes; offset += page) {
    CPLVirtualMemPin(vmem.get(), ptr + offset, page, TRUE);
    ptr[offset] = 99;
  }
  vmem.reset();

  GByte written_val = 0;
  ASSERT_EQ(CE_None, ds_wide->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, 1, 1, &written_val, 1, 1, GDT_Byte, 0,
                         0, nullptr));
  EXPECT_EQ(99, written_val);

  // Case B: Non-compact band-sequential with line padding and band padding,
  // and single-band GDALRasterBandGetVirtualMem.
  GDALDatasetUniquePtr ds_pad = CreateMemDS(100, 20, 2, GDT_Byte);
  ASSERT_NE(nullptr, ds_pad);
  EXPECT_EQ(CE_None, ds_pad->GetRasterBand(1)->Fill(11));
  EXPECT_EQ(CE_None, ds_pad->GetRasterBand(2)->Fill(22));

  // nPixelSpace = 1, nLineSpace = 128 (28 bytes padding per line),
  // nBandSpace = 20 * 128 + page (padding at end of band).
  const GIntBig band_space = static_cast<GIntBig>(20 * 128 + page);
  VirtualMemPtr vmem_pad(GDALDatasetGetVirtualMem(
      GDALDataset::ToHandle(ds_pad.get()), GF_Read, 0, 0, 100, 20, 100, 20,
      GDT_Byte, 2, nullptr, 1, 128, band_space, 2 * page, page, TRUE, nullptr));
  ASSERT_NE(nullptr, vmem_pad);
  auto* ptr_pad = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem_pad.get()));
  ASSERT_NE(nullptr, ptr_pad);
  for (size_t offset = 0; offset < static_cast<size_t>(2 * band_space);
       offset += page) {
    CPLVirtualMemPin(vmem_pad.get(), ptr_pad + offset, 1, FALSE);
  }
  EXPECT_EQ(11, ptr_pad[0]);
  vmem_pad.reset();

  // Single-band GDALRasterBandGetVirtualMem with line padding such that a page
  // starts in the x >= nBufXSize padding area (nBufXSize = 100, nLineSpace =
  // page + 104):
  const GIntBig padded_line = static_cast<GIntBig>(page + 104);
  VirtualMemPtr vmem_band(GDALRasterBandGetVirtualMem(
      GDALRasterBand::ToHandle(ds_pad->GetRasterBand(1)), GF_Read, 0, 0, 100, 3,
      100, 3, GDT_Byte, 1, padded_line, 2 * page, page, TRUE, nullptr));
  ASSERT_NE(nullptr, vmem_band);
  auto* ptr_band = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem_band.get()));
  ASSERT_NE(nullptr, ptr_band);
  CPLVirtualMemPin(vmem_band.get(), ptr_band, page, FALSE);
  CPLVirtualMemPin(vmem_band.get(), ptr_band + page, page, FALSE);
  CPLVirtualMemPin(vmem_band.get(), ptr_band + 2 * page, page, FALSE);
  EXPECT_EQ(11, ptr_band[0]);
  vmem_band.reset();
}

TEST_F(GdalVirtualMemTest, PixelInterleavedReadAndWrite) {
  const size_t page = CPLGetPageSize();
  ASSERT_NE(0u, page);

  // Case A: Compact pixel-interleaved with wide scanline (nBufXSize = page / 2,
  // nBandCount = 3 -> nLineSpace = 1.5 * page > page and not divisible by 3).
  // Page 0 ends mid-pixel; page 1 finishes that pixel, finishes line 0, and
  // recurses into line 1.
  const int inter_x = static_cast<int>(page / 2);
  const GIntBig inter_line_space = static_cast<GIntBig>(inter_x) * 3;
  GDALDatasetUniquePtr ds_wide = CreateMemDS(inter_x, 2, 3, GDT_Byte);
  ASSERT_NE(nullptr, ds_wide);
  EXPECT_EQ(CE_None, ds_wide->GetRasterBand(1)->Fill(10));
  EXPECT_EQ(CE_None, ds_wide->GetRasterBand(2)->Fill(20));
  EXPECT_EQ(CE_None, ds_wide->GetRasterBand(3)->Fill(30));

  VirtualMemPtr vmem(GDALDatasetGetVirtualMem(
      GDALDataset::ToHandle(ds_wide.get()), GF_Write, 0, 0, inter_x, 2, inter_x,
      2, GDT_Byte, 3, nullptr, 3, inter_line_space, 1, page, page, TRUE,
      nullptr));
  ASSERT_NE(nullptr, vmem);
  auto* ptr = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem.get()));
  ASSERT_NE(nullptr, ptr);
  const size_t total_inter_bytes = static_cast<size_t>(inter_line_space) * 2;
  for (size_t offset = 0; offset < total_inter_bytes; offset += page) {
    CPLVirtualMemPin(vmem.get(), ptr + offset, 1, TRUE);
    ptr[offset] = 77;
  }
  vmem.reset();

  GByte written_val = 0;
  ASSERT_EQ(CE_None, ds_wide->GetRasterBand(1)->RasterIO(
                         GF_Read, 0, 0, 1, 1, &written_val, 1, 1, GDT_Byte, 0,
                         0, nullptr));
  EXPECT_EQ(77, written_val);

  // Case B: Non-compact pixel-interleaved with narrow scanlines (nBufXSize =
  // 100, nPixelSpace = 4, nLineSpace = 512 < page, total = 2 * page) so page 0
  // reads multiple whole lines and page 1 crosses mid-scanline with band
  // padding.
  const int narrow_y = static_cast<int>((2 * page) / 512);
  GDALDatasetUniquePtr ds_narrow = CreateMemDS(100, narrow_y, 3, GDT_Byte);
  ASSERT_NE(nullptr, ds_narrow);
  VirtualMemPtr vmem2(GDALDatasetGetVirtualMem(
      GDALDataset::ToHandle(ds_narrow.get()), GF_Read, 0, 0, 100, narrow_y, 100,
      narrow_y, GDT_Byte, 3, nullptr, 4, 512, 1, 2 * page, page, TRUE,
      nullptr));
  ASSERT_NE(nullptr, vmem2);
  auto* ptr2 = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem2.get()));
  ASSERT_NE(nullptr, ptr2);
  CPLVirtualMemPin(vmem2.get(), ptr2, page, FALSE);
  CPLVirtualMemPin(vmem2.get(), ptr2 + page, page, FALSE);
  vmem2.reset();

  // Case C: Non-compact pixel-interleaved where offset lands in x >= nBufXSize
  // line padding (nBufXSize = 10, nBandCount = 3, nPixelSpace = 4,
  // nLineSpace = page + 104):
  VirtualMemPtr vmem3(GDALDatasetGetVirtualMem(
      GDALDataset::ToHandle(ds_narrow.get()), GF_Read, 0, 0, 10, 3, 10, 3,
      GDT_Byte, 3, nullptr, 4, static_cast<GIntBig>(page + 104), 1, 2 * page,
      page, TRUE, nullptr));
  ASSERT_NE(nullptr, vmem3);
  auto* ptr3 = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem3.get()));
  ASSERT_NE(nullptr, ptr3);
  CPLVirtualMemPin(vmem3.get(), ptr3 + page, page, FALSE);
  vmem3.reset();
}

TEST_F(GdalVirtualMemTest, TiledVirtualMemReadAndWrite) {
  const size_t page = CPLGetPageSize();
  ASSERT_NE(0u, page);

  // Derive tile dimensions (64 x (page / 64) == page bytes per single-band
  // tile) from CPLGetPageSize() so tile sizes are multiples of the host OS page
  // size (4K on x86_64, 16K/64K on arm64) and cover both full and partial edge
  // tiles.
  const int tile_x = 64;
  const int tile_y = static_cast<int>(page / 64);
  ASSERT_EQ(page, static_cast<size_t>(tile_x) * tile_y);
  const int width = tile_x + 6;
  const int height = tile_y + 6;
  const size_t cache_size = 2 * page;

  GDALDatasetUniquePtr ds = CreateMemDS(width, height, 2, GDT_Byte);
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(CE_None, ds->GetRasterBand(1)->Fill(15));
  EXPECT_EQ(CE_None, ds->GetRasterBand(2)->Fill(25));

  int band_map[2] = {1, 2};
  for (GDALTileOrganization org : {GTO_TIP, GTO_BIT, GTO_BSQ}) {
    VirtualMemPtr vmem(GDALDatasetGetTiledVirtualMem(
        GDALDataset::ToHandle(ds.get()), GF_Write, 0, 0, width, height, tile_x,
        tile_y, GDT_Byte, 2, band_map, org, cache_size, TRUE, nullptr));
    ASSERT_NE(nullptr, vmem);
    auto* ptr = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem.get()));
    ASSERT_NE(nullptr, ptr);
    size_t page_size = CPLVirtualMemGetPageSize(vmem.get());
    // Pin all 4 tiles (and bands for BSQ) to trigger FillCache and
    // SaveFromCache.
    for (size_t i = 0; i < 4; ++i) {
      CPLVirtualMemPin(vmem.get(), ptr + i * page_size, page_size, TRUE);
      ptr[i * page_size] = 55;
    }
    vmem.reset();

    GByte written_val = 0;
    ASSERT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &written_val,
                                             1, 1, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(55, written_val);
  }

  // Also test GDALRasterBandGetTiledVirtualMem and panBandMap == nullptr:
  VirtualMemPtr vmem_ds_nullmap(GDALDatasetGetTiledVirtualMem(
      GDALDataset::ToHandle(ds.get()), GF_Read, 0, 0, tile_x, tile_y, tile_x,
      tile_y, GDT_Byte, 2, nullptr, GTO_BSQ, cache_size, TRUE, nullptr));
  ASSERT_NE(nullptr, vmem_ds_nullmap);
  vmem_ds_nullmap.reset();

  VirtualMemPtr vmem_band(GDALRasterBandGetTiledVirtualMem(
      GDALRasterBand::ToHandle(ds->GetRasterBand(1)), GF_Read, 0, 0, width,
      height, tile_x, tile_y, GDT_Byte, cache_size, TRUE, nullptr));
  ASSERT_NE(nullptr, vmem_band);
  auto* ptr_band = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem_band.get()));
  ASSERT_NE(nullptr, ptr_band);
  CPLVirtualMemPin(vmem_band.get(), ptr_band, page, FALSE);
  CPLVirtualMemPin(vmem_band.get(), ptr_band + page, page, FALSE);
  vmem_band.reset();
}

}  // namespace
}  // namespace autotest2
