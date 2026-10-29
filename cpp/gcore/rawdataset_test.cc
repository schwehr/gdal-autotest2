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

#include "third_party/gdal/gcore/rawdataset.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_virtualmem.h"
#include "third_party/gdal/port/cpl_vsi.h"

bool RAWDatasetCheckMemoryUsage(int nXSize, int nYSize, int nBands, int nDTSize,
                                int nPixelOffset, int nLineOffset,
                                vsi_l_offset nHeaderSize,
                                vsi_l_offset nBandOffset, VSILFILE* fp);

namespace {

class TestRawDataset : public RawDataset {
 public:
  TestRawDataset(int xsize, int ysize, GDALAccess access = GA_Update) {
    nRasterXSize = xsize;
    nRasterYSize = ysize;
    eAccess = access;
    SetPamFlags(GPF_NOSAVE);
  }
  ~TestRawDataset() override { Close(); }

  CPLErr Close() override { return FlushCache(true); }

  using RawDataset::SetBand;
};

VSILFILE* CreateZeroFile(const char* path, size_t size) {
  VSILFILE* fp = VSIFOpenL(path, "wb+");
  if (!fp) return nullptr;
  if (size > 0) {
    std::vector<GByte> zeros(size, 0);
    VSIFWriteL(zeros.data(), 1, size, fp);
    VSIFSeekL(fp, 0, SEEK_SET);
  }
  return fp;
}

TEST(RawDatasetTest, ConstructorsOffsetsByteOrdersAndVaxFloat) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  const char* path = "/vsimem/raw_basic.bin";
  VSILFILE* fp = CreateZeroFile(path, 4096);
  ASSERT_NE(fp, nullptr);

  // 1. Invalid offsets / dimensions in RawRasterBand::Create:
  TestRawDataset ds(8, 8);
  // Negative line offset larger than nImgOffset:
  EXPECT_EQ(
      RawRasterBand::Create(&ds, 1, fp, /*nImgOffset=*/10,
                            /*nPixelOffset=*/1, /*nLineOffset=*/-8, GDT_Byte,
                            RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                            RawRasterBand::OwnFP::NO),
      nullptr);
  // Negative pixel offset larger than nSmallestOffset:
  EXPECT_EQ(
      RawRasterBand::Create(&ds, 1, fp, /*nImgOffset=*/2,
                            /*nPixelOffset=*/-1, /*nLineOffset=*/8, GDT_Byte,
                            RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                            RawRasterBand::OwnFP::NO),
      nullptr);
  // Overflowing nImgOffset:
  EXPECT_EQ(RawRasterBand::Create(
                &ds, 1, fp, std::numeric_limits<vsi_l_offset>::max() - 2, 1, 8,
                GDT_Byte, RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                RawRasterBand::OwnFP::NO),
            nullptr);
  // Floating band with invalid dimensions (0x0):
  EXPECT_EQ(RawRasterBand::Create(fp, 0, 1, 1, GDT_Byte,
                                  RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                                  0, 0, RawRasterBand::OwnFP::NO),
            nullptr);

  // 2. Valid bottom-up and right-to-left scanlines (negative nLineOffset and
  //    negative nPixelOffset) + legacy bNativeOrder constructors (both DS and
  //    floating):
  {
    TestRawDataset legacy_ds(4, 4);
    auto legacy_band = std::make_unique<RawRasterBand>(
        &legacy_ds, 1, fp, 0, 1, 4, GDT_Byte, /*bNativeOrder=*/TRUE,
        RawRasterBand::OwnFP::NO);
    EXPECT_TRUE(legacy_band->IsValid());

    auto floating_band = std::make_unique<RawRasterBand>(
        fp, /*nImgOffset=*/500, /*nPixelOffset=*/-2, /*nLineOffset=*/-16,
        GDT_UInt16, /*bNativeOrder=*/FALSE, 4, 4, RawRasterBand::OwnFP::NO);
    ASSERT_TRUE(floating_band->IsValid());
    EXPECT_EQ(floating_band->GetImgOffset(), 500u);
    EXPECT_EQ(floating_band->GetPixelOffset(), -2);
    EXPECT_EQ(floating_band->GetLineOffset(), -16);
    EXPECT_EQ(floating_band->GetByteOrder(),
              RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN);
    EXPECT_EQ(floating_band->GetFPL(), fp);
    EXPECT_EQ(floating_band->GetOwnsFP(), FALSE);
    floating_band->SetAccess(GA_Update);
    floating_band->StoreNoDataValue(-999.0);

    const char* cats[] = {"A", "B", nullptr};
    EXPECT_EQ(floating_band->SetCategoryNames(const_cast<char**>(cats)),
              CE_None);
    EXPECT_NE(floating_band->GetCategoryNames(), nullptr);
    GDALColorTable ct;
    EXPECT_EQ(floating_band->SetColorTable(&ct), CE_None);
    EXPECT_NE(floating_band->GetColorTable(), nullptr);
    EXPECT_EQ(floating_band->SetColorTable(nullptr), CE_None);
    EXPECT_EQ(floating_band->SetColorInterpretation(GCI_RedBand), CE_None);
    EXPECT_EQ(floating_band->GetColorInterpretation(), GCI_RedBand);

    GUInt16 row[4] = {100, 200, 300, 400};
    EXPECT_EQ(floating_band->WriteBlock(0, 1, row), CE_None);
    EXPECT_EQ(floating_band->FlushCache(false), CE_None);
    GUInt16 read_row[4] = {0};
    EXPECT_EQ(floating_band->ReadBlock(0, 1, read_row), CE_None);
    EXPECT_EQ(read_row[0], 100);
    EXPECT_EQ(read_row[3], 400);
  }

  // 3. ORDER_VAX byte-swapping for Float32, CFloat32, Float64, CFloat64:
  for (GDALDataType dt :
       {GDT_Float32, GDT_CFloat32, GDT_Float64, GDT_CFloat64}) {
    const int dt_size = GDALGetDataTypeSizeBytes(dt);
    auto vax_band = RawRasterBand::Create(fp, 1024, dt_size, dt_size * 4, dt,
                                          RawRasterBand::ByteOrder::ORDER_VAX,
                                          4, 2, RawRasterBand::OwnFP::NO);
    ASSERT_NE(vax_band, nullptr);
    vax_band->SetAccess(GA_Update);
    std::vector<double> write_vals(8, 1.5);
    EXPECT_EQ(vax_band->WriteBlock(0, 0, write_vals.data()), CE_None);
    EXPECT_EQ(vax_band->FlushCache(false), CE_None);
    std::vector<double> read_vals(8, 0.0);
    EXPECT_EQ(vax_band->AccessLine(1), CE_None);
    EXPECT_EQ(vax_band->ReadBlock(0, 0, read_vals.data()), CE_None);
  }

  VSIFCloseL(fp);
  VSIUnlink(path);
  CPLPopErrorHandler();
}

TEST(RawDatasetTest, BIPBlockAndDirectIOAndLayoutAndMemoryUsage) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Multi-band BIP RawDataset with big-endian CFloat32 (tests IsBIP,
  //    BIPWriteBlock with all blocks dirty vs single band dirty, IReadBlock
  //    pre-caching other bands, RawDataset::IRasterIO direct BIP access and
  //    per-band direct IO, and GetRawBinaryLayout):
  const char* bip_path = "/vsimem/raw_bip.bin";
  VSILFILE* fp = CreateZeroFile(bip_path, 16384);
  ASSERT_NE(fp, nullptr);
  {
    auto ds = std::make_unique<TestRawDataset>(8, 4, GA_Update);
    ds->SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
    const int dt_size = GDALGetDataTypeSizeBytes(GDT_CFloat32);  // 8 bytes
    const int pixel_off = 2 * dt_size;                           // 16 bytes
    const int line_off = pixel_off * 8;                          // 128 bytes
    ds->SetBand(1, new RawRasterBand(ds.get(), 1, fp, 0, pixel_off, line_off,
                                     GDT_CFloat32,
                                     RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN,
                                     RawRasterBand::OwnFP::NO));
    ds->SetBand(2, new RawRasterBand(ds.get(), 2, fp, dt_size, pixel_off,
                                     line_off, GDT_CFloat32,
                                     RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN,
                                     RawRasterBand::OwnFP::YES));

    GDALDataset::RawBinaryLayout layout;
    EXPECT_TRUE(ds->GetRawBinaryLayout(layout));
    EXPECT_EQ(layout.eInterleaving,
              GDALDataset::RawBinaryLayout::Interleaving::BIP);
    EXPECT_FALSE(layout.bLittleEndianOrder);

    // Write via block cache on band 2 only (bAllBlocksDirty == false):
    std::vector<float> buf(16, 2.0f);
    EXPECT_EQ(ds->GetRasterBand(2)->WriteBlock(0, 0, buf.data()), CE_None);
    EXPECT_EQ(ds->FlushCache(false), CE_None);

    // Dirty both band 1 and band 2 blocks in cache then flush (bAllBlocksDirty
    // == true):
    GDALRasterBlock* blk1 = ds->GetRasterBand(1)->GetLockedBlockRef(0, 1, TRUE);
    GDALRasterBlock* blk2 = ds->GetRasterBand(2)->GetLockedBlockRef(0, 1, TRUE);
    ASSERT_NE(blk1, nullptr);
    ASSERT_NE(blk2, nullptr);
    blk1->MarkDirty();
    blk2->MarkDirty();
    blk1->DropLock();
    blk2->DropLock();
    EXPECT_EQ(ds->FlushCache(false), CE_None);

    // ReadBlock on band 1 (pre-caches band 2 block):
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 2, buf.data()), CE_None);
    // AccessLine on band 2 (forwards to band 1):
    EXPECT_EQ(static_cast<RawRasterBand*>(ds->GetRasterBand(2))->AccessLine(2),
              CE_None);
    // Clean cached block on band 1 while writing band 2 (covers
    // !apoBlocks->GetDirty()):
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 3, buf.data()), CE_None);
    EXPECT_EQ(ds->GetRasterBand(2)->WriteBlock(0, 3, buf.data()), CE_None);
    EXPECT_EQ(ds->FlushCache(false), CE_None);

    // Direct IO on band 1 (deinterleaved & subsampled read/write with
    // big-endian CFloat32 and full-res non-subsampled read/write):
    {
      CPLConfigOptionSetter one_big("GDAL_ONE_BIG_READ", "YES", false);
      std::vector<float> full_buf(8 * 4 * 2, 1.25f);
      EXPECT_EQ(
          ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 8, 4, full_buf.data(),
                                         8, 4, GDT_CFloat32, 0, 0, nullptr),
          CE_None);
      EXPECT_EQ(
          ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 8, 4, full_buf.data(),
                                         8, 4, GDT_CFloat32, 0, 0, nullptr),
          CE_None);
      std::vector<float> sub_buf(4 * 2 * 2, 3.5f);
      EXPECT_EQ(
          ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 8, 4, sub_buf.data(),
                                         4, 2, GDT_CFloat32, 0, 0, nullptr),
          CE_None);
      EXPECT_EQ(
          ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 8, 4, sub_buf.data(), 4,
                                         2, GDT_CFloat32, 0, 0, nullptr),
          CE_None);

      // Progress callback returning FALSE aborts DirectIO read:
      GDALRasterIOExtraArg extra;
      INIT_RASTERIO_EXTRA_ARG(extra);
      extra.pfnProgress = [](double, const char*, void*) -> int {
        return FALSE;
      };
      EXPECT_EQ(
          ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 8, 4, sub_buf.data(), 4,
                                         2, GDT_CFloat32, 0, 0, &extra),
          CE_Failure);

      // RawDataset::IRasterIO with bCanDirectAccessToBIPDataset == true:
      int band_map[2] = {1, 2};
      std::vector<float> bip_out(8 * 4 * 2 * 2, 0.0f);
      EXPECT_EQ(
          ds->RasterIO(GF_Read, 0, 0, 8, 4, bip_out.data(), 8, 4, GDT_CFloat32,
                       2, band_map, pixel_off, line_off, dt_size, nullptr),
          CE_None);

      // RawDataset::IRasterIO with bCanUseDirectIO == true,
      // bCanDirectAccessToBIPDataset == false:
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 8, 4, bip_out.data(), 8, 4,
                             GDT_CFloat32, 2, band_map, 0, 0, 0, nullptr),
                CE_None);
    }

    // RawDataset::IRasterIO with GDAL_ONE_BIG_READ=NO (falls back to
    // GDALDataset::IRasterIO):
    {
      CPLConfigOptionSetter no_big("GDAL_ONE_BIG_READ", "NO", false);
      int band_map[2] = {1, 2};
      std::vector<float> bip_out(8 * 4 * 2 * 2, 0.0f);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 8, 4, bip_out.data(), 8, 4,
                             GDT_CFloat32, 2, band_map, 0, 0, 0, nullptr),
                CE_None);
    }
  }
  VSIUnlink(bip_path);

  // 2. Contiguous BSQ DirectIO Read & Write (with byte-swapping), BIL layout,
  //    and wide scanline (>50000 bytes) CanUseDirectIO heuristic:
  const char* bsq_path = "/vsimem/raw_bsq.bin";
  VSILFILE* fp_bsq = CreateZeroFile(bsq_path, 250000);
  ASSERT_NE(fp_bsq, nullptr);
  {
    TestRawDataset bsq_ds(8, 4, GA_Update);
    bsq_ds.SetBand(1,
                   new RawRasterBand(&bsq_ds, 1, fp_bsq, 0, 2, 16, GDT_UInt16,
                                     RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN,
                                     RawRasterBand::OwnFP::NO));
    bsq_ds.SetBand(2,
                   new RawRasterBand(&bsq_ds, 2, fp_bsq, 64, 2, 16, GDT_UInt16,
                                     RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN,
                                     RawRasterBand::OwnFP::NO));
    GDALDataset::RawBinaryLayout layout;
    EXPECT_TRUE(bsq_ds.GetRawBinaryLayout(layout));
    EXPECT_EQ(layout.eInterleaving,
              GDALDataset::RawBinaryLayout::Interleaving::BSQ);

    // Contiguous DirectIO Write & Read on Band 1:
    std::vector<GUInt16> data(32, 0x1234);
    EXPECT_EQ(
        bsq_ds.GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 8, 4, data.data(), 8,
                                          4, GDT_UInt16, 2, 16, nullptr),
        CE_None);
    std::vector<GUInt16> read_data(32, 0);
    EXPECT_EQ(
        bsq_ds.GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 8, 4, read_data.data(),
                                          8, 4, GDT_UInt16, 2, 16, nullptr),
        CE_None);
    EXPECT_EQ(read_data[0], 0x1234);

    // BIL layout check + 3rd band with inconsistent offset:
    TestRawDataset bil_ds(8, 4, GA_Update);
    bil_ds.SetBand(
        1, new RawRasterBand(&bil_ds, 1, fp_bsq, 0, 2, 32, GDT_UInt16,
                             RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                             RawRasterBand::OwnFP::NO));
    bil_ds.SetBand(
        2, new RawRasterBand(&bil_ds, 2, fp_bsq, 16, 2, 32, GDT_UInt16,
                             RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                             RawRasterBand::OwnFP::NO));
    EXPECT_TRUE(bil_ds.GetRawBinaryLayout(layout));
    EXPECT_EQ(layout.eInterleaving,
              GDALDataset::RawBinaryLayout::Interleaving::BIL);
    bil_ds.SetBand(
        3, new RawRasterBand(&bil_ds, 3, fp_bsq, 999, 2, 32, GDT_UInt16,
                             RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                             RawRasterBand::OwnFP::NO));
    EXPECT_FALSE(bil_ds.GetRawBinaryLayout(layout));

    // ReadOnly short read on truncated file (without and with ENVI metadata):
    const char* short_path = "/vsimem/raw_short.bin";
    VSILFILE* fp_short = CreateZeroFile(short_path, 4);
    ASSERT_NE(fp_short, nullptr);
    {
      TestRawDataset ro_ds(8, 4, GA_ReadOnly);
      ro_ds.SetBand(
          1, new RawRasterBand(&ro_ds, 1, fp_short, 0, 1, 8, GDT_Byte,
                               RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                               RawRasterBand::OwnFP::NO));
      GByte line_buf[8];
      EXPECT_EQ(ro_ds.GetRasterBand(1)->ReadBlock(0, 0, line_buf), CE_Failure);
      ro_ds.SetMetadataItem("INTERLEAVE", "bsq", "ENVI");
      EXPECT_EQ(ro_ds.GetRasterBand(1)->ReadBlock(0, 0, line_buf), CE_None);
    }
    VSIFCloseL(fp_short);
    VSIUnlink(short_path);

    // Wide scanline (>50000 bytes) heuristic in CanUseDirectIO &
    // IsSignificantNumberOfLinesLoaded:
    TestRawDataset wide_ds(60000, 2, GA_Update);
    wide_ds.SetBand(
        1, new RawRasterBand(&wide_ds, 1, fp_bsq, 0, 1, 60000, GDT_Byte,
                             RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                             RawRasterBand::OwnFP::NO));
    GByte small_chunk[20];
    EXPECT_EQ(
        wide_ds.GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 10, 2, small_chunk,
                                           10, 2, GDT_Byte, 1, 10, nullptr),
        CE_None);
    // Load line 0 into block cache so IsSignificantNumberOfLinesLoaded returns
    // TRUE:
    std::vector<GByte> full_line(60000);
    EXPECT_EQ(wide_ds.GetRasterBand(1)->ReadBlock(0, 0, full_line.data()),
              CE_None);
    wide_ds.ClearCachedConfigOption();
    EXPECT_EQ(
        wide_ds.GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 10, 2, small_chunk,
                                           10, 2, GDT_Byte, 1, 10, nullptr),
        CE_None);

    // RAWDatasetCheckMemoryUsage:
    EXPECT_TRUE(RAWDatasetCheckMemoryUsage(8, 8, 1, 1, 1, 8, 0, 0, fp_bsq));
    EXPECT_FALSE(RAWDatasetCheckMemoryUsage(1000, 1000, 20, 1, 1, 1000, 0,
                                            1000000, fp_bsq));
    {
      CPLConfigOptionSetter mem_lim("RAW_MEM_ALLOC_LIMIT_MB", "0", false);
      EXPECT_FALSE(
          RAWDatasetCheckMemoryUsage(100, 100, 1, 1, 1, 100, 0, 0, fp_bsq));
    }
  }
  VSIFCloseL(fp_bsq);
  VSIUnlink(bsq_path);

  // 3. GetVirtualMemAuto on real file in TempDir():
  std::string real_path = testing::TempDir() + "/raw_vmem.bin";
  VSILFILE* fp_real = CreateZeroFile(real_path.c_str(), 4096);
  ASSERT_NE(fp_real, nullptr);
  VSIFCloseL(fp_real);
  FILE* fp_stdio = fopen(real_path.c_str(), "rb+");
  ASSERT_NE(fp_stdio, nullptr);
  VSILFILE* fp_vmem =
      reinterpret_cast<VSILFILE*>(VSIFOpenExL(real_path.c_str(), "rb+", true));
  ASSERT_NE(fp_vmem, nullptr);
  {
    TestRawDataset real_ds(16, 16, GA_Update);
    real_ds.SetBand(
        1, new RawRasterBand(&real_ds, 1, fp_vmem, 0, 1, 16, GDT_Byte,
                             RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                             RawRasterBand::OwnFP::YES));
    int pixel_space = 0;
    GIntBig line_space = 0;
    const char* def_opts[] = {"USE_DEFAULT_IMPLEMENTATION=YES", nullptr};
    CPLVirtualMem* vmem = real_ds.GetRasterBand(1)->GetVirtualMemAuto(
        GF_Read, &pixel_space, &line_space, const_cast<char**>(def_opts));
    if (vmem != nullptr) {
      CPLVirtualMemFree(vmem);
    }
    const char* no_def_opts[] = {"USE_DEFAULT_IMPLEMENTATION=NO", nullptr};
    CPLVirtualMem* vmem2 = real_ds.GetRasterBand(1)->GetVirtualMemAuto(
        GF_Read, &pixel_space, &line_space, const_cast<char**>(no_def_opts));
    if (vmem2 != nullptr) {
      CPLVirtualMemFree(vmem2);
    }
  }
  fclose(fp_stdio);
  VSIUnlink(real_path.c_str());

  // 4. Custom RawRasterBand with injectable fpRawL swap to cover I/O error
  //    handling in AccessLine, FlushCurrentLine (via FlushCache), AccessBlock,
  //    and IRasterIO:
  {
    class FailingRawBand : public RawRasterBand {
     public:
      FailingRawBand(GDALDataset* ds, int band, VSILFILE* fp, int pixel_off,
                     int line_off, GDALDataType dt, ByteOrder bo)
          : RawRasterBand(ds, band, fp, 0, pixel_off, line_off, dt, bo,
                          OwnFP::NO) {}
      void SetFP(VSILFILE* fp) { fpRawL = fp; }
      using RawRasterBand::AccessBlock;
    };

    const char* fail_path = "/vsimem/raw_fail_io.bin";
    VSILFILE* fp_fail = CreateZeroFile(fail_path, 4096);
    ASSERT_NE(fp_fail, nullptr);
    const char* ro_handle_path = "/vsimem/raw_ro_handle.bin";
    VSILFILE* fp_ro_init = CreateZeroFile(ro_handle_path, 4096);
    ASSERT_NE(fp_ro_init, nullptr);
    VSIFCloseL(fp_ro_init);
    VSILFILE* fp_ro = VSIFOpenL(ro_handle_path, "rb");
    ASSERT_NE(fp_ro, nullptr);

    {
      TestRawDataset fds(8, 4, GA_Update);
      // Band 1: contiguous pixel_offset=2 (UInt16 swapped)
      auto* b1 = new FailingRawBand(&fds, 1, fp_fail, 2, 16, GDT_UInt16,
                                    RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN);
      // Band 2: non-contiguous pixel_offset=4 (UInt16 swapped)
      auto* b2 = new FailingRawBand(&fds, 2, fp_fail, 4, 32, GDT_UInt16,
                                    RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN);
      fds.SetBand(1, b1);
      fds.SetBand(2, b2);

      // AccessLine short read on GA_Update (zero-pads buffer in memory):
      const char* tiny_path = "/vsimem/raw_tiny_upd.bin";
      VSILFILE* fp_tiny = CreateZeroFile(tiny_path, 2);
      ASSERT_NE(fp_tiny, nullptr);
      {
        TestRawDataset tds(8, 4, GA_Update);
        auto* tb =
            new FailingRawBand(&tds, 1, fp_tiny, 1, 8, GDT_Byte,
                               RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN);
        tds.SetBand(1, tb);
        GByte blk[8] = {0};
        EXPECT_EQ(tb->ReadBlock(0, 1, blk), CE_None);
      }
      VSIFCloseL(fp_tiny);
      VSIUnlink(tiny_path);

      // FlushCurrentLine write failures inside IWriteBlock (contiguous b1 and
      // non-contiguous b2):
      GByte line_data[16] = {1, 2, 3, 4};
      b1->SetFP(fp_ro);
      EXPECT_EQ(b1->WriteBlock(0, 0, line_data), CE_Failure);
      b1->SetFP(fp_fail);

      b2->SetFP(fp_ro);
      EXPECT_EQ(b2->WriteBlock(0, 0, line_data), CE_Failure);
      b2->SetFP(fp_fail);

      // Direct IRasterIO GF_Write write failures (Case 1, Case 2, Case 3) using
      // fp_ro:
      CPLConfigOptionSetter direct_io("GDAL_ONE_BIG_READ", "YES", false);
      fds.ClearCachedConfigOption();
      GUInt16 io_buf[32] = {0};

      // Case 1 (contiguous full-width write):
      b1->SetFP(fp_ro);
      EXPECT_NE(b1->RasterIO(GF_Write, 0, 0, 8, 2, io_buf, 8, 2, GDT_UInt16, 2,
                             16, nullptr),
                CE_None);

      // Case 2 (contiguous sub-window write):
      EXPECT_NE(b1->RasterIO(GF_Write, 1, 0, 4, 2, io_buf, 4, 2, GDT_UInt16, 2,
                             8, nullptr),
                CE_None);
      b1->SetFP(fp_fail);

      // Case 3 (non-contiguous pixel_offset=4 write on b2):
      b2->SetFP(fp_ro);
      EXPECT_NE(b2->RasterIO(GF_Write, 0, 0, 4, 2, io_buf, 4, 2, GDT_UInt16, 2,
                             8, nullptr),
                CE_None);
      b2->SetFP(fp_fail);

      // Additional RawDataset::IRasterIO and BIPWriteBlock branches:
      // 1) BIP dataset where all bands have dirty cached blocks on line 0:
      TestRawDataset bip2(4, 4, GA_Update);
      bip2.SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
      auto* bip_b1 =
          new FailingRawBand(&bip2, 1, fp_fail, 2, 8, GDT_Byte,
                             RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN);
      auto* bip_b2 =
          new RawRasterBand(&bip2, 2, fp_fail, 1, 2, 8, GDT_Byte,
                            RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                            RawRasterBand::OwnFP::NO);
      bip2.SetBand(1, bip_b1);
      bip2.SetBand(2, bip_b2);
      GDALRasterBlock* blk2 = bip_b2->GetLockedBlockRef(0, 0, true);
      ASSERT_NE(blk2, nullptr);
      blk2->MarkDirty();
      blk2->DropLock();
      GByte b1_row[4] = {9, 9, 9, 9};
      EXPECT_EQ(bip_b1->WriteBlock(0, 0, b1_row), CE_None);
      // Read block 0 of band 2 while it is already cached (covers poBlock !=
      // nullptr in IReadBlock):
      EXPECT_EQ(bip_b2->ReadBlock(0, 0, b1_row), CE_None);
      EXPECT_EQ(bip_b1->ReadBlock(0, 0, b1_row), CE_None);

      // Direct access to BIP dataset when file is shorter than expected
      // (VSIFReadL != 1 -> line 1678):
      const char* empty_bip_path = "/vsimem/raw_empty_bip.bin";
      VSILFILE* fp_empty_bip = CreateZeroFile(empty_bip_path, 0);
      ASSERT_NE(fp_empty_bip, nullptr);
      TestRawDataset empty_bip_ds(4, 4, GA_ReadOnly);
      empty_bip_ds.SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
      empty_bip_ds.SetBand(
          1,
          new RawRasterBand(&empty_bip_ds, 1, fp_empty_bip, 0, 2, 8, GDT_Byte,
                            RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                            RawRasterBand::OwnFP::NO));
      empty_bip_ds.SetBand(
          2,
          new RawRasterBand(&empty_bip_ds, 2, fp_empty_bip, 1, 2, 8, GDT_Byte,
                            RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                            RawRasterBand::OwnFP::NO));
      int bmap[2] = {1, 2};
      GByte bip_buf[32] = {0};
      EXPECT_EQ(empty_bip_ds.RasterIO(GF_Read, 0, 0, 4, 4, bip_buf, 4, 4,
                                      GDT_Byte, 2, bmap, 2, 8, 1, nullptr),
                CE_Failure);
      VSIFCloseL(fp_empty_bip);
      VSIUnlink(empty_bip_path);

      // RawDataset::IRasterIO with a non-RawRasterBand band (lines 1609-1612):
      class NonRawBand : public GDALPamRasterBand {
       public:
        NonRawBand(GDALDataset* ds, int b) {
          poDS = ds;
          nBand = b;
          eDataType = GDT_Byte;
          nRasterXSize = ds->GetRasterXSize();
          nRasterYSize = ds->GetRasterYSize();
          nBlockXSize = nRasterXSize;
          nBlockYSize = 1;
        }
        CPLErr IReadBlock(int, int, void* p) override {
          memset(p, 0, nBlockXSize);
          return CE_None;
        }
      };
      TestRawDataset mixed_ds(4, 4, GA_ReadOnly);
      mixed_ds.SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
      mixed_ds.SetBand(1, new NonRawBand(&mixed_ds, 1));
      mixed_ds.SetBand(2, new NonRawBand(&mixed_ds, 2));
      EXPECT_EQ(mixed_ds.RasterIO(GF_Read, 0, 0, 4, 4, bip_buf, 4, 4, GDT_Byte,
                                  2, bmap, 2, 8, 1, nullptr),
                CE_None);

      // RAWDatasetCheckMemoryUsage overflow catch block (lines 1780-1783):
      EXPECT_FALSE(RAWDatasetCheckMemoryUsage(
          100, 100, 20, 1, 1, 100, std::numeric_limits<vsi_l_offset>::max() - 5,
          1000, fp_fail));

      // Overflowing nPixelOffset, nLargestOffset > GINTBIG_MAX, and >INT_MAX
      // line buffer:
      EXPECT_EQ(RawRasterBand::Create(
                    &fds, 1, fp_fail, 10, std::numeric_limits<int>::max(), 8,
                    GDT_Byte, RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                    RawRasterBand::OwnFP::NO),
                nullptr);
      EXPECT_EQ(RawRasterBand::Create(
                    &fds, 1, fp_fail, static_cast<vsi_l_offset>(GINTBIG_MAX), 1,
                    8, GDT_Byte, RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                    RawRasterBand::OwnFP::NO),
                nullptr);
      TestRawDataset huge_x_ds(100000, 2, GA_ReadOnly);
      EXPECT_EQ(RawRasterBand::Create(
                    &huge_x_ds, 1, fp_fail, 0, 100000, 100000, GDT_Byte,
                    RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                    RawRasterBand::OwnFP::NO),
                nullptr);
    }
    VSIFCloseL(fp_ro);
    VSIUnlink(ro_handle_path);
    VSIFCloseL(fp_fail);
    VSIUnlink(fail_path);
  }

  CPLPopErrorHandler();
}

}  // namespace
