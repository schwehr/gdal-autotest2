// Copyright 2026 Google Inc. All Rights Reserved.
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

#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_virtualmem.h"

namespace {

std::unique_ptr<GDALDataset> CreateMemDS(int width, int height, int bands,
                                         GDALDataType dt = GDT_Byte) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  return std::unique_ptr<GDALDataset>(
      drv->Create("", width, height, bands, dt, nullptr));
}

TEST(GdalVirtualMemTest, ParameterValidationErrors) {
  GDALAllRegister();
  auto ds = CreateMemDS(64, 64, 3, GDT_Byte);
  ASSERT_NE(ds, nullptr);
  GDALDatasetH hds = GDALDataset::ToHandle(ds.get());
  GDALRasterBandH hband = GDALRasterBand::ToHandle(ds->GetRasterBand(1));

  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. nXSize != nBufXSize || nYSize != nBufYSize:
  EXPECT_EQ(
      GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 16, 16, GDT_Byte, 1,
                               nullptr, 0, 0, 0, 4096, 0, TRUE, nullptr),
      nullptr);

  // 2. Invalid window requests:
  EXPECT_EQ(GDALRasterBandGetVirtualMem(hband, GF_Read, -1, 0, 32, 32, 32, 32,
                                        GDT_Byte, 0, 0, 4096, 0, TRUE, nullptr),
            nullptr);
  EXPECT_EQ(GDALRasterBandGetVirtualMem(hband, GF_Read, 0, 0, 100, 32, 100, 32,
                                        GDT_Byte, 0, 0, 4096, 0, TRUE, nullptr),
            nullptr);

  // 3. Negative spacing:
  EXPECT_EQ(
      GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32, 32, GDT_Byte, 1,
                               nullptr, -1, 0, 0, 4096, 0, TRUE, nullptr),
      nullptr);

  // 4. GDALCheckBandParameters errors:
  EXPECT_EQ(
      GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32, 32, GDT_Byte, 0,
                               nullptr, 0, 0, 0, 4096, 0, TRUE, nullptr),
      nullptr);
  int bad_map[1] = {5};
  EXPECT_EQ(
      GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32, 32, GDT_Byte, 1,
                               bad_map, 0, 0, 0, 4096, 0, TRUE, nullptr),
      nullptr);
  EXPECT_EQ(
      GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32, 32, GDT_Byte, 5,
                               nullptr, 0, 0, 0, 4096, 0, TRUE, nullptr),
      nullptr);

  // 5. Unsupported interleaving:
  // nLineSpace < nBufXSize * nPixelSpace:
  EXPECT_EQ(
      GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32, 32, GDT_Byte, 2,
                               nullptr, 2, 10, 0, 4096, 0, TRUE, nullptr),
      nullptr);
  // nBandCount > 1 && nBandSpace == nPixelSpace:
  EXPECT_EQ(
      GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32, 32, GDT_Byte, 2,
                               nullptr, 2, 64, 2, 4096, 0, TRUE, nullptr),
      nullptr);
  // nBandSpace > nPixelSpace && nBandSpace < nBufYSize * nLineSpace:
  EXPECT_EQ(
      GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32, 32, GDT_Byte, 2,
                               nullptr, 1, 32, 100, 4096, 0, TRUE, nullptr),
      nullptr);

  // 6. Spacing not a multiple of data type size:
  EXPECT_EQ(
      GDALDatasetGetVirtualMem(hds, GF_Read, 0, 0, 32, 32, 32, 32, GDT_UInt16,
                               1, nullptr, 3, 96, 0, 4096, 0, TRUE, nullptr),
      nullptr);

  // 7. Tiled virtual mem errors:
  EXPECT_EQ(GDALDatasetGetTiledVirtualMem(hds, GF_Read, -1, 0, 64, 64, 64, 64,
                                          GDT_Byte, 1, nullptr, GTO_BSQ, 4096,
                                          TRUE, nullptr),
            nullptr);
  EXPECT_EQ(GDALDatasetGetTiledVirtualMem(hds, GF_Read, 0, 0, 64, 64, 64, 64,
                                          GDT_Byte, 0, nullptr, GTO_BSQ, 4096,
                                          TRUE, nullptr),
            nullptr);
  // Incompatible tile size with page size (10*10 = 100 % 4096 != 0):
  EXPECT_EQ(GDALDatasetGetTiledVirtualMem(hds, GF_Read, 0, 0, 64, 64, 10, 10,
                                          GDT_Byte, 1, nullptr, GTO_BSQ, 4096,
                                          TRUE, nullptr),
            nullptr);

  CPLPopErrorHandler();
}

TEST(GdalVirtualMemTest, BandSequentialReadAndWrite) {
  GDALAllRegister();
  if (CPLGetPageSize() == 0) return;

  // Case A: Scanline wider than 1 page (nBufXSize = 5000 > 4096) and multiple
  // lines to test partial-line, end-of-line, and multi-line I/O in
  // DoIOBandSequential.
  auto ds_wide = CreateMemDS(5000, 2, 2, GDT_Byte);
  ASSERT_NE(ds_wide, nullptr);
  ds_wide->GetRasterBand(1)->Fill(42);
  ds_wide->GetRasterBand(2)->Fill(84);

  int band_map[2] = {1, 2};
  CPLVirtualMem* vmem = GDALDatasetGetVirtualMem(
      GDALDataset::ToHandle(ds_wide.get()), GF_Write, 0, 0, 5000, 2, 5000, 2,
      GDT_Byte, 2, band_map, 0, 0, 0, 4096, 4096, TRUE, nullptr);
  ASSERT_NE(vmem, nullptr);
  auto* ptr = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem));
  ASSERT_NE(ptr, nullptr);

  // Pin and modify pages across band 1 and band 2; with cache size = 4096,
  // pinning subsequent pages evicts dirty pages and calls
  // SaveFromCacheBandSequential.
  for (size_t offset = 0; offset < 20000; offset += 4096) {
    CPLVirtualMemPin(vmem, ptr + offset, 4096, TRUE);
    ptr[offset] = 99;
  }
  CPLVirtualMemFree(vmem);

  // Case B: Non-compact band-sequential with line padding and band padding,
  // and single-band GDALRasterBandGetVirtualMem.
  auto ds_pad = CreateMemDS(100, 20, 2, GDT_Byte);
  ASSERT_NE(ds_pad, nullptr);
  ds_pad->GetRasterBand(1)->Fill(11);
  ds_pad->GetRasterBand(2)->Fill(22);

  // nPixelSpace = 1, nLineSpace = 128 (28 bytes padding per line),
  // nBandSpace = 20 * 128 + 5000 (padding at end of band).
  CPLVirtualMem* vmem_pad = GDALDatasetGetVirtualMem(
      GDALDataset::ToHandle(ds_pad.get()), GF_Read, 0, 0, 100, 20, 100, 20,
      GDT_Byte, 2, nullptr, 1, 128, 20 * 128 + 5000, 8192, 4096, TRUE, nullptr);
  ASSERT_NE(vmem_pad, nullptr);
  auto* ptr_pad = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem_pad));
  for (size_t offset = 0; offset < 2 * (20 * 128 + 5000); offset += 4096) {
    CPLVirtualMemPin(vmem_pad, ptr_pad + offset, 1, FALSE);
  }
  EXPECT_EQ(ptr_pad[0], 11);
  CPLVirtualMemFree(vmem_pad);

  // Single-band GDALRasterBandGetVirtualMem with line padding such that a page
  // starts in the x >= nBufXSize padding area (nBufXSize = 100, nLineSpace =
  // 4200):
  CPLVirtualMem* vmem_band = GDALRasterBandGetVirtualMem(
      GDALRasterBand::ToHandle(ds_pad->GetRasterBand(1)), GF_Read, 0, 0, 100, 3,
      100, 3, GDT_Byte, 1, 4200, 8192, 4096, TRUE, nullptr);
  ASSERT_NE(vmem_band, nullptr);
  auto* ptr_band = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem_band));
  CPLVirtualMemPin(vmem_band, ptr_band, 4096, FALSE);
  CPLVirtualMemPin(vmem_band, ptr_band + 4096, 4096, FALSE);
  CPLVirtualMemPin(vmem_band, ptr_band + 8192, 4096, FALSE);
  CPLVirtualMemFree(vmem_band);
}

TEST(GdalVirtualMemTest, PixelInterleavedReadAndWrite) {
  GDALAllRegister();
  if (CPLGetPageSize() == 0) return;

  // Case A: Compact pixel-interleaved with wide scanline (nBufXSize = 2000,
  // nBandCount = 3 -> nLineSpace = 6000 > 4096).
  // Page 0 (0..4095) ends mid-pixel (pixel 1365, band 1).
  // Page 1 (4096..8191) starts at band = 1 > 0, finishes pixel 1365, finishes
  // line 0, and recurses into line 1!
  auto ds_wide = CreateMemDS(2000, 2, 3, GDT_Byte);
  ASSERT_NE(ds_wide, nullptr);
  ds_wide->GetRasterBand(1)->Fill(10);
  ds_wide->GetRasterBand(2)->Fill(20);
  ds_wide->GetRasterBand(3)->Fill(30);

  CPLVirtualMem* vmem = GDALDatasetGetVirtualMem(
      GDALDataset::ToHandle(ds_wide.get()), GF_Write, 0, 0, 2000, 2, 2000, 2,
      GDT_Byte, 3, nullptr, 3, 6000, 1, 4096, 4096, TRUE, nullptr);
  ASSERT_NE(vmem, nullptr);
  auto* ptr = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem));
  for (size_t offset = 0; offset < 12000; offset += 4096) {
    CPLVirtualMemPin(vmem, ptr + offset, 1, TRUE);
    ptr[offset] = 77;
  }
  CPLVirtualMemFree(vmem);

  // Case B: Non-compact pixel-interleaved with narrow scanlines (nBufXSize =
  // 100, nBufYSize = 20, nBandCount = 3, nBandSpace = 1, nPixelSpace = 4,
  // nLineSpace = 400 < 4096) so page 0 reads multiple whole lines, and
  // page 1 (offset 4096 -> y=10, x=24, band=0) or padded pixel (band=3 >=
  // nBandCount) is exercised.
  auto ds_narrow = CreateMemDS(100, 20, 3, GDT_Byte);
  ASSERT_NE(ds_narrow, nullptr);
  CPLVirtualMem* vmem2 = GDALDatasetGetVirtualMem(
      GDALDataset::ToHandle(ds_narrow.get()), GF_Read, 0, 0, 100, 20, 100, 20,
      GDT_Byte, 3, nullptr, 4, 400, 1, 8192, 4096, TRUE, nullptr);
  ASSERT_NE(vmem2, nullptr);
  auto* ptr2 = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem2));
  CPLVirtualMemPin(vmem2, ptr2, 4096, FALSE);
  CPLVirtualMemPin(vmem2, ptr2 + 4096, 4096, FALSE);
  CPLVirtualMemFree(vmem2);

  // Case C: Non-compact pixel-interleaved where offset lands in x >= nBufXSize
  // line padding (nBufXSize = 10, nBandCount = 3, nPixelSpace = 4,
  // nLineSpace = 4200):
  CPLVirtualMem* vmem3 = GDALDatasetGetVirtualMem(
      GDALDataset::ToHandle(ds_narrow.get()), GF_Read, 0, 0, 10, 3, 10, 3,
      GDT_Byte, 3, nullptr, 4, 4200, 1, 8192, 4096, TRUE, nullptr);
  ASSERT_NE(vmem3, nullptr);
  auto* ptr3 = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem3));
  CPLVirtualMemPin(vmem3, ptr3 + 4096, 4096, FALSE);
  CPLVirtualMemFree(vmem3);
}

TEST(GdalVirtualMemTest, TiledVirtualMemReadAndWrite) {
  GDALAllRegister();
  if (CPLGetPageSize() == 0) return;

  // Use 70x70 dataset with 64x64 tiles (64*64 = 4096 = 1 page) so we have
  // both full tiles and partial edge tiles (nReqXSize < nTileXSize).
  auto ds = CreateMemDS(70, 70, 2, GDT_Byte);
  ASSERT_NE(ds, nullptr);
  ds->GetRasterBand(1)->Fill(15);
  ds->GetRasterBand(2)->Fill(25);

  int band_map[2] = {1, 2};
  for (GDALTileOrganization org : {GTO_TIP, GTO_BIT, GTO_BSQ}) {
    CPLVirtualMem* vmem = GDALDatasetGetTiledVirtualMem(
        GDALDataset::ToHandle(ds.get()), GF_Write, 0, 0, 70, 70, 64, 64,
        GDT_Byte, 2, band_map, org, 8192, TRUE, nullptr);
    ASSERT_NE(vmem, nullptr);
    auto* ptr = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem));
    size_t page_size = CPLVirtualMemGetPageSize(vmem);
    // Pin all 4 tiles (and bands for BSQ) to trigger FillCache and
    // SaveFromCache.
    for (size_t i = 0; i < 4; ++i) {
      CPLVirtualMemPin(vmem, ptr + i * page_size, page_size, TRUE);
      ptr[i * page_size] = 55;
    }
    CPLVirtualMemFree(vmem);
  }

  // Also test GDALRasterBandGetTiledVirtualMem and panBandMap == nullptr:
  CPLVirtualMem* vmem_ds_nullmap = GDALDatasetGetTiledVirtualMem(
      GDALDataset::ToHandle(ds.get()), GF_Read, 0, 0, 64, 64, 64, 64, GDT_Byte,
      2, nullptr, GTO_BSQ, 8192, TRUE, nullptr);
  ASSERT_NE(vmem_ds_nullmap, nullptr);
  CPLVirtualMemFree(vmem_ds_nullmap);

  CPLVirtualMem* vmem_band = GDALRasterBandGetTiledVirtualMem(
      GDALRasterBand::ToHandle(ds->GetRasterBand(1)), GF_Read, 0, 0, 70, 70, 64,
      64, GDT_Byte, 8192, TRUE, nullptr);
  ASSERT_NE(vmem_band, nullptr);
  auto* ptr_band = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem_band));
  CPLVirtualMemPin(vmem_band, ptr_band, 4096, FALSE);
  CPLVirtualMemPin(vmem_band, ptr_band + 4096, 4096, FALSE);
  CPLVirtualMemFree(vmem_band);
}

}  // namespace
