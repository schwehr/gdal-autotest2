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
// Tests RawDataset and RawRasterBand (gcore/rawdataset.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/rawdataset.cpp

#include "third_party/gdal/gcore/rawdataset.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>  // NOLINT
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_pam.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_virtualmem.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kVsiDir[] = "/vsimem/rawdataset_test";

using VirtualMemPtr =
    std::unique_ptr<CPLVirtualMem,
                    decltype([](CPLVirtualMem* p) { CPLVirtualMemFree(p); })>;

class TestRawDataset final : public RawDataset {
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
  if (fp == nullptr) return nullptr;
  if (size > 0) {
    const std::vector<GByte> zeros(size, 0);
    VSIFWriteL(zeros.data(), 1, size, fp);
    VSIFSeekL(fp, 0, SEEK_SET);
  }
  return fp;
}

TEST(RawDatasetTest, ConstructorsOffsetsByteOrdersAndVaxFloat) {
  absl::Cleanup vsi_cleanup = [] { VSIRmdirRecursive(kVsiDir); };

  constexpr char kBasicPath[] = "/vsimem/rawdataset_test/raw_basic.bin";
  VSILFILE* fp = CreateZeroFile(kBasicPath, 4096);
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };

  // 1. Invalid offsets / dimensions in RawRasterBand::Create:
  TestRawDataset ds(8, 8);
  {
    WithQuietHandler quiet;
    // Negative line offset larger than nImgOffset:
    EXPECT_EQ(nullptr, RawRasterBand::Create(
                           &ds, 1, fp, /*nImgOffset=*/10,
                           /*nPixelOffset=*/1, /*nLineOffset=*/-8, GDT_Byte,
                           RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                           RawRasterBand::OwnFP::NO));
    // Negative pixel offset larger than nSmallestOffset:
    EXPECT_EQ(nullptr, RawRasterBand::Create(
                           &ds, 1, fp, /*nImgOffset=*/2,
                           /*nPixelOffset=*/-1, /*nLineOffset=*/8, GDT_Byte,
                           RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                           RawRasterBand::OwnFP::NO));
    // Overflowing nImgOffset:
    EXPECT_EQ(nullptr,
              RawRasterBand::Create(
                  &ds, 1, fp, std::numeric_limits<vsi_l_offset>::max() - 2, 1,
                  8, GDT_Byte, RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                  RawRasterBand::OwnFP::NO));
    // Floating band with invalid dimensions (0x0):
    EXPECT_EQ(nullptr, RawRasterBand::Create(
                           fp, 0, 1, 1, GDT_Byte,
                           RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN, 0, 0,
                           RawRasterBand::OwnFP::NO));
    CPLErrorReset();
  }

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
    EXPECT_EQ(500u, floating_band->GetImgOffset());
    EXPECT_EQ(-2, floating_band->GetPixelOffset());
    EXPECT_EQ(-16, floating_band->GetLineOffset());
    EXPECT_EQ(RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN,
              floating_band->GetByteOrder());
    EXPECT_EQ(fp, floating_band->GetFPL());
    EXPECT_EQ(FALSE, floating_band->GetOwnsFP());
    floating_band->SetAccess(GA_Update);
    floating_band->StoreNoDataValue(-999.0);

    CPLStringList cats;
    cats.AddString("A");
    cats.AddString("B");
    EXPECT_EQ(CE_None, floating_band->SetCategoryNames(cats.List()));
    EXPECT_NE(nullptr, floating_band->GetCategoryNames());
    GDALColorTable ct;
    EXPECT_EQ(CE_None, floating_band->SetColorTable(&ct));
    EXPECT_NE(nullptr, floating_band->GetColorTable());
    EXPECT_EQ(CE_None, floating_band->SetColorTable(nullptr));
    EXPECT_EQ(CE_None, floating_band->SetColorInterpretation(GCI_RedBand));
    EXPECT_EQ(GCI_RedBand, floating_band->GetColorInterpretation());

    GUInt16 row[4] = {100, 200, 300, 400};
    EXPECT_EQ(CE_None, floating_band->WriteBlock(0, 1, row));
    EXPECT_EQ(CE_None, floating_band->FlushCache(false));
    GUInt16 read_row[4] = {0};
    EXPECT_EQ(CE_None, floating_band->ReadBlock(0, 1, read_row));
    EXPECT_EQ(100, read_row[0]);
    EXPECT_EQ(400, read_row[3]);
  }

  // 3. ORDER_VAX byte-swapping for Float32, CFloat32, Float64, CFloat64:
  for (GDALDataType dt :
       {GDT_Float32, GDT_CFloat32, GDT_Float64, GDT_CFloat64}) {
    const int dt_size = GDALGetDataTypeSizeBytes(dt);
    auto vax_band = RawRasterBand::Create(fp, 1024, dt_size, dt_size * 4, dt,
                                          RawRasterBand::ByteOrder::ORDER_VAX,
                                          4, 2, RawRasterBand::OwnFP::NO);
    ASSERT_NE(nullptr, vax_band);
    vax_band->SetAccess(GA_Update);
    std::vector<double> write_vals(8, 1.5);
    EXPECT_EQ(CE_None, vax_band->WriteBlock(0, 0, write_vals.data()));
    EXPECT_EQ(CE_None, vax_band->FlushCache(false));
    std::vector<double> read_vals(8, 0.0);
    EXPECT_EQ(CE_None, vax_band->AccessLine(1));
    EXPECT_EQ(CE_None, vax_band->ReadBlock(0, 0, read_vals.data()));
  }
}

TEST(RawDatasetTest, BIPBlockAndDirectIOAndLayoutAndMemoryUsage) {
  absl::Cleanup vsi_cleanup = [] { VSIRmdirRecursive(kVsiDir); };

  // 1. Multi-band BIP RawDataset with big-endian CFloat32 (tests IsBIP,
  //    BIPWriteBlock with all blocks dirty vs single band dirty, IReadBlock
  //    pre-caching other bands, RawDataset::IRasterIO direct BIP access and
  //    per-band direct IO, and GetRawBinaryLayout):
  {
    constexpr char kBipPath[] = "/vsimem/rawdataset_test/raw_bip.bin";
    VSILFILE* fp = CreateZeroFile(kBipPath, 16384);
    ASSERT_NE(nullptr, fp);

    TestRawDataset ds(8, 4, GA_Update);
    ds.SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
    const int dt_size = GDALGetDataTypeSizeBytes(GDT_CFloat32);  // 8 bytes
    const int pixel_off = 2 * dt_size;                           // 16 bytes
    const int line_off = pixel_off * 8;                          // 128 bytes
    ds.SetBand(
        1, new RawRasterBand(&ds, 1, fp, 0, pixel_off, line_off, GDT_CFloat32,
                             RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN,
                             RawRasterBand::OwnFP::NO));
    ds.SetBand(2, new RawRasterBand(&ds, 2, fp, dt_size, pixel_off, line_off,
                                    GDT_CFloat32,
                                    RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN,
                                    RawRasterBand::OwnFP::YES));

    GDALDataset::RawBinaryLayout layout;
    EXPECT_TRUE(ds.GetRawBinaryLayout(layout));
    EXPECT_EQ(GDALDataset::RawBinaryLayout::Interleaving::BIP,
              layout.eInterleaving);
    EXPECT_FALSE(layout.bLittleEndianOrder);

    // Write via block cache on band 2 only (bAllBlocksDirty == false):
    std::vector<float> buf(16, 2.0f);
    EXPECT_EQ(CE_None, ds.GetRasterBand(2)->WriteBlock(0, 0, buf.data()));
    EXPECT_EQ(CE_None, ds.FlushCache(false));

    // Dirty both band 1 and band 2 blocks in cache then flush (bAllBlocksDirty
    // == true):
    GDALRasterBlock* blk1 = ds.GetRasterBand(1)->GetLockedBlockRef(0, 1, TRUE);
    GDALRasterBlock* blk2 = ds.GetRasterBand(2)->GetLockedBlockRef(0, 1, TRUE);
    ASSERT_NE(nullptr, blk1);
    ASSERT_NE(nullptr, blk2);
    blk1->MarkDirty();
    blk2->MarkDirty();
    blk1->DropLock();
    blk2->DropLock();
    EXPECT_EQ(CE_None, ds.FlushCache(false));

    // ReadBlock on band 1 (pre-caches band 2 block):
    EXPECT_EQ(CE_None, ds.GetRasterBand(1)->ReadBlock(0, 2, buf.data()));
    // AccessLine on band 2 (forwards to band 1):
    EXPECT_EQ(
        CE_None,
        cpl::down_cast<RawRasterBand*>(ds.GetRasterBand(2))->AccessLine(2));
    // Clean cached block on band 1 while writing band 2 (covers
    // !apoBlocks->GetDirty()):
    EXPECT_EQ(CE_None, ds.GetRasterBand(1)->ReadBlock(0, 3, buf.data()));
    EXPECT_EQ(CE_None, ds.GetRasterBand(2)->WriteBlock(0, 3, buf.data()));
    EXPECT_EQ(CE_None, ds.FlushCache(false));

    // Direct IO on band 1 (deinterleaved & subsampled read/write with
    // big-endian CFloat32 and full-res non-subsampled read/write):
    {
      CPLConfigOptionSetter one_big("GDAL_ONE_BIG_READ", "YES", false);
      std::vector<float> full_buf(8 * 4 * 2, 1.25f);
      EXPECT_EQ(CE_None, ds.GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 8, 4, full_buf.data(), 8, 4,
                             GDT_CFloat32, 0, 0, nullptr));
      EXPECT_EQ(CE_None, ds.GetRasterBand(1)->RasterIO(
                             GF_Read, 0, 0, 8, 4, full_buf.data(), 8, 4,
                             GDT_CFloat32, 0, 0, nullptr));
      std::vector<float> sub_buf(4 * 2 * 2, 3.5f);
      EXPECT_EQ(CE_None, ds.GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 8, 4, sub_buf.data(), 4, 2,
                             GDT_CFloat32, 0, 0, nullptr));
      EXPECT_EQ(CE_None, ds.GetRasterBand(1)->RasterIO(
                             GF_Read, 0, 0, 8, 4, sub_buf.data(), 4, 2,
                             GDT_CFloat32, 0, 0, nullptr));

      // Progress callback returning FALSE aborts DirectIO read:
      GDALRasterIOExtraArg extra;
      INIT_RASTERIO_EXTRA_ARG(extra);
      extra.pfnProgress = [](double, const char*, void*) -> int {
        return FALSE;
      };
      {
        WithQuietHandler quiet;
        EXPECT_EQ(CE_Failure, ds.GetRasterBand(1)->RasterIO(
                                  GF_Read, 0, 0, 8, 4, sub_buf.data(), 4, 2,
                                  GDT_CFloat32, 0, 0, &extra));
        CPLErrorReset();
      }

      // RawDataset::IRasterIO with bCanDirectAccessToBIPDataset == true:
      int band_map[2] = {1, 2};
      std::vector<float> bip_out(8 * 4 * 2 * 2, 0.0f);
      EXPECT_EQ(CE_None, ds.RasterIO(GF_Read, 0, 0, 8, 4, bip_out.data(), 8, 4,
                                     GDT_CFloat32, 2, band_map, pixel_off,
                                     line_off, dt_size, nullptr));

      // RawDataset::IRasterIO with bCanUseDirectIO == true,
      // bCanDirectAccessToBIPDataset == false:
      EXPECT_EQ(CE_None,
                ds.RasterIO(GF_Read, 0, 0, 8, 4, bip_out.data(), 8, 4,
                            GDT_CFloat32, 2, band_map, 0, 0, 0, nullptr));
    }

    // RawDataset::IRasterIO with GDAL_ONE_BIG_READ=NO (falls back to
    // GDALDataset::IRasterIO):
    {
      CPLConfigOptionSetter no_big("GDAL_ONE_BIG_READ", "NO", false);
      int band_map[2] = {1, 2};
      std::vector<float> bip_out(8 * 4 * 2 * 2, 0.0f);
      EXPECT_EQ(CE_None,
                ds.RasterIO(GF_Read, 0, 0, 8, 4, bip_out.data(), 8, 4,
                            GDT_CFloat32, 2, band_map, 0, 0, 0, nullptr));
    }
  }

  // 2. Contiguous BSQ DirectIO Read & Write (with byte-swapping), BIL layout,
  //    and wide scanline (>50000 bytes) CanUseDirectIO heuristic:
  {
    constexpr char kBsqPath[] = "/vsimem/rawdataset_test/raw_bsq.bin";
    VSILFILE* fp_bsq = CreateZeroFile(kBsqPath, 250000);
    ASSERT_NE(nullptr, fp_bsq);
    absl::Cleanup fp_bsq_closer = [fp_bsq] {
      EXPECT_EQ(0, VSIFCloseL(fp_bsq));
    };

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
    EXPECT_EQ(GDALDataset::RawBinaryLayout::Interleaving::BSQ,
              layout.eInterleaving);

    // Contiguous DirectIO Write & Read on Band 1:
    std::vector<GUInt16> data(32, 0x1234);
    EXPECT_EQ(CE_None, bsq_ds.GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 8, 4, data.data(), 8, 4, GDT_UInt16,
                           2, 16, nullptr));
    std::vector<GUInt16> read_data(32, 0);
    EXPECT_EQ(CE_None, bsq_ds.GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 8, 4, read_data.data(), 8, 4,
                           GDT_UInt16, 2, 16, nullptr));
    EXPECT_EQ(0x1234, read_data[0]);

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
    EXPECT_EQ(GDALDataset::RawBinaryLayout::Interleaving::BIL,
              layout.eInterleaving);
    bil_ds.SetBand(
        3, new RawRasterBand(&bil_ds, 3, fp_bsq, 999, 2, 32, GDT_UInt16,
                             RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                             RawRasterBand::OwnFP::NO));
    EXPECT_FALSE(bil_ds.GetRawBinaryLayout(layout));

    // ReadOnly short read on truncated file (without and with ENVI metadata):
    {
      constexpr char kShortPath[] = "/vsimem/rawdataset_test/raw_short.bin";
      VSILFILE* fp_short = CreateZeroFile(kShortPath, 4);
      ASSERT_NE(nullptr, fp_short);
      absl::Cleanup fp_short_closer = [fp_short] {
        EXPECT_EQ(0, VSIFCloseL(fp_short));
      };

      TestRawDataset ro_ds(8, 4, GA_ReadOnly);
      ro_ds.SetBand(
          1, new RawRasterBand(&ro_ds, 1, fp_short, 0, 1, 8, GDT_Byte,
                               RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                               RawRasterBand::OwnFP::NO));
      GByte line_buf[8];
      {
        WithQuietHandler quiet;
        EXPECT_EQ(CE_Failure,
                  ro_ds.GetRasterBand(1)->ReadBlock(0, 0, line_buf));
        CPLErrorReset();
      }
      ro_ds.SetMetadataItem("INTERLEAVE", "bsq", "ENVI");
      EXPECT_EQ(CE_None, ro_ds.GetRasterBand(1)->ReadBlock(0, 0, line_buf));
    }

    // Wide scanline (>50000 bytes) heuristic in CanUseDirectIO &
    // IsSignificantNumberOfLinesLoaded:
    TestRawDataset wide_ds(60000, 2, GA_Update);
    wide_ds.SetBand(
        1, new RawRasterBand(&wide_ds, 1, fp_bsq, 0, 1, 60000, GDT_Byte,
                             RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                             RawRasterBand::OwnFP::NO));
    GByte small_chunk[20];
    EXPECT_EQ(CE_None, wide_ds.GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 10, 2, small_chunk, 10, 2, GDT_Byte,
                           1, 10, nullptr));
    // Load line 0 into block cache so IsSignificantNumberOfLinesLoaded returns
    // TRUE:
    std::vector<GByte> full_line(60000);
    EXPECT_EQ(CE_None,
              wide_ds.GetRasterBand(1)->ReadBlock(0, 0, full_line.data()));
    wide_ds.ClearCachedConfigOption();
    EXPECT_EQ(CE_None, wide_ds.GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 10, 2, small_chunk, 10, 2, GDT_Byte,
                           1, 10, nullptr));

    // RAWDatasetCheckMemoryUsage:
    EXPECT_TRUE(RAWDatasetCheckMemoryUsage(8, 8, 1, 1, 1, 8, 0, 0, fp_bsq));
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(RAWDatasetCheckMemoryUsage(1000, 1000, 20, 1, 1, 1000, 0,
                                              1000000, fp_bsq));
      CPLConfigOptionSetter mem_lim("RAW_MEM_ALLOC_LIMIT_MB", "0", false);
      EXPECT_FALSE(
          RAWDatasetCheckMemoryUsage(100, 100, 1, 1, 1, 100, 0, 0, fp_bsq));
      CPLErrorReset();
    }
  }

  // 3. GetVirtualMemAuto on real file in TempDir():
  {
    const std::string real_path =
        (std::filesystem::path(::testing::TempDir()) / "raw_vmem.bin").string();
    absl::Cleanup real_path_cleaner = [&real_path] {
      VSIUnlink(real_path.c_str());
    };
    {
      VSILFILE* fp_real = CreateZeroFile(real_path.c_str(), 4096);
      ASSERT_NE(nullptr, fp_real);
      EXPECT_EQ(0, VSIFCloseL(fp_real));
    }
    std::unique_ptr<FILE,
                    decltype([](FILE* f) { EXPECT_EQ(0, std::fclose(f)); })>
        fp_stdio(std::fopen(real_path.c_str(), "rb+"));
    ASSERT_NE(nullptr, fp_stdio);
    VSILFILE* fp_vmem = VSIFOpenExL(real_path.c_str(), "rb+", true);
    ASSERT_NE(nullptr, fp_vmem);

    TestRawDataset real_ds(16, 16, GA_Update);
    real_ds.SetBand(
        1, new RawRasterBand(&real_ds, 1, fp_vmem, 0, 1, 16, GDT_Byte,
                             RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                             RawRasterBand::OwnFP::YES));
    int pixel_space = 0;
    GIntBig line_space = 0;
    CPLStringList def_opts;
    def_opts.AddString("USE_DEFAULT_IMPLEMENTATION=YES");
    VirtualMemPtr vmem(real_ds.GetRasterBand(1)->GetVirtualMemAuto(
        GF_Read, &pixel_space, &line_space, def_opts.List()));
    CPLStringList no_def_opts;
    no_def_opts.AddString("USE_DEFAULT_IMPLEMENTATION=NO");
    VirtualMemPtr vmem2(real_ds.GetRasterBand(1)->GetVirtualMemAuto(
        GF_Read, &pixel_space, &line_space, no_def_opts.List()));
  }

  // 4. Custom RawRasterBand with injectable fpRawL swap to cover I/O error
  //    handling in AccessLine, FlushCurrentLine (via FlushCache), AccessBlock,
  //    and IRasterIO:
  {
    class FailingRawBand final : public RawRasterBand {
     public:
      FailingRawBand(GDALDataset* ds, int band, VSILFILE* fp, int pixel_off,
                     int line_off, GDALDataType dt, ByteOrder bo)
          : RawRasterBand(ds, band, fp, 0, pixel_off, line_off, dt, bo,
                          OwnFP::NO) {}
      void SetFP(VSILFILE* fp) { fpRawL = fp; }
      using RawRasterBand::AccessBlock;
    };

    constexpr char kFailPath[] = "/vsimem/rawdataset_test/raw_fail_io.bin";
    VSILFILE* fp_fail = CreateZeroFile(kFailPath, 4096);
    ASSERT_NE(nullptr, fp_fail);
    absl::Cleanup fp_fail_closer = [fp_fail] {
      EXPECT_EQ(0, VSIFCloseL(fp_fail));
    };

    constexpr char kRoHandlePath[] =
        "/vsimem/rawdataset_test/raw_ro_handle.bin";
    {
      VSILFILE* fp_ro_init = CreateZeroFile(kRoHandlePath, 4096);
      ASSERT_NE(nullptr, fp_ro_init);
      EXPECT_EQ(0, VSIFCloseL(fp_ro_init));
    }
    VSILFILE* fp_ro = VSIFOpenL(kRoHandlePath, "rb");
    ASSERT_NE(nullptr, fp_ro);
    absl::Cleanup fp_ro_closer = [fp_ro] { EXPECT_EQ(0, VSIFCloseL(fp_ro)); };

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
    {
      constexpr char kTinyPath[] = "/vsimem/rawdataset_test/raw_tiny_upd.bin";
      VSILFILE* fp_tiny = CreateZeroFile(kTinyPath, 2);
      ASSERT_NE(nullptr, fp_tiny);
      absl::Cleanup fp_tiny_closer = [fp_tiny] {
        EXPECT_EQ(0, VSIFCloseL(fp_tiny));
      };

      TestRawDataset tds(8, 4, GA_Update);
      auto* tb =
          new FailingRawBand(&tds, 1, fp_tiny, 1, 8, GDT_Byte,
                             RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN);
      tds.SetBand(1, tb);
      GByte blk[8] = {0};
      EXPECT_EQ(CE_None, tb->ReadBlock(0, 1, blk));
    }

    // FlushCurrentLine write failures inside IWriteBlock (contiguous b1 and
    // non-contiguous b2):
    {
      WithQuietHandler quiet;
      GByte line_data[16] = {1, 2, 3, 4};
      b1->SetFP(fp_ro);
      EXPECT_EQ(CE_Failure, b1->WriteBlock(0, 0, line_data));
      b1->SetFP(fp_fail);

      b2->SetFP(fp_ro);
      EXPECT_EQ(CE_Failure, b2->WriteBlock(0, 0, line_data));
      b2->SetFP(fp_fail);

      // Direct IRasterIO GF_Write write failures (Case 1, Case 2, Case 3) using
      // fp_ro:
      CPLConfigOptionSetter direct_io("GDAL_ONE_BIG_READ", "YES", false);
      fds.ClearCachedConfigOption();
      GUInt16 io_buf[32] = {0};

      // Case 1 (contiguous full-width write):
      b1->SetFP(fp_ro);
      EXPECT_NE(CE_None, b1->RasterIO(GF_Write, 0, 0, 8, 2, io_buf, 8, 2,
                                      GDT_UInt16, 2, 16, nullptr));

      // Case 2 (contiguous sub-window write):
      EXPECT_NE(CE_None, b1->RasterIO(GF_Write, 1, 0, 4, 2, io_buf, 4, 2,
                                      GDT_UInt16, 2, 8, nullptr));
      b1->SetFP(fp_fail);

      // Case 3 (non-contiguous pixel_offset=4 write on b2):
      b2->SetFP(fp_ro);
      EXPECT_NE(CE_None, b2->RasterIO(GF_Write, 0, 0, 4, 2, io_buf, 4, 2,
                                      GDT_UInt16, 2, 8, nullptr));
      b2->SetFP(fp_fail);
      CPLErrorReset();
    }

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
    ASSERT_NE(nullptr, blk2);
    blk2->MarkDirty();
    blk2->DropLock();
    GByte b1_row[4] = {9, 9, 9, 9};
    EXPECT_EQ(CE_None, bip_b1->WriteBlock(0, 0, b1_row));
    // Read block 0 of band 2 while it is already cached (covers poBlock !=
    // nullptr in IReadBlock):
    EXPECT_EQ(CE_None, bip_b2->ReadBlock(0, 0, b1_row));
    EXPECT_EQ(CE_None, bip_b1->ReadBlock(0, 0, b1_row));

    // Direct access to BIP dataset when file is shorter than expected
    // (VSIFReadL != 1 -> line 1678):
    int bmap[2] = {1, 2};
    GByte bip_buf[32] = {0};
    {
      constexpr char kEmptyBipPath[] =
          "/vsimem/rawdataset_test/raw_empty_bip.bin";
      VSILFILE* fp_empty_bip = CreateZeroFile(kEmptyBipPath, 0);
      ASSERT_NE(nullptr, fp_empty_bip);
      absl::Cleanup fp_empty_bip_closer = [fp_empty_bip] {
        EXPECT_EQ(0, VSIFCloseL(fp_empty_bip));
      };

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
      WithQuietHandler quiet;
      EXPECT_EQ(CE_Failure,
                empty_bip_ds.RasterIO(GF_Read, 0, 0, 4, 4, bip_buf, 4, 4,
                                      GDT_Byte, 2, bmap, 2, 8, 1, nullptr));
      CPLErrorReset();
    }

    // RawDataset::IRasterIO with a non-RawRasterBand band (lines 1609-1612):
    class NonRawBand final : public GDALPamRasterBand {
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
        std::memset(p, 0, nBlockXSize);
        return CE_None;
      }
    };
    TestRawDataset mixed_ds(4, 4, GA_ReadOnly);
    mixed_ds.SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
    mixed_ds.SetBand(1, new NonRawBand(&mixed_ds, 1));
    mixed_ds.SetBand(2, new NonRawBand(&mixed_ds, 2));
    EXPECT_EQ(CE_None, mixed_ds.RasterIO(GF_Read, 0, 0, 4, 4, bip_buf, 4, 4,
                                         GDT_Byte, 2, bmap, 2, 8, 1, nullptr));

    // RAWDatasetCheckMemoryUsage overflow catch block (lines 1780-1783) and
    // overflowing nPixelOffset, nLargestOffset > GINTBIG_MAX, and >INT_MAX
    // line buffer:
    {
      WithQuietHandler quiet;
      EXPECT_FALSE(RAWDatasetCheckMemoryUsage(
          100, 100, 20, 1, 1, 100, std::numeric_limits<vsi_l_offset>::max() - 5,
          1000, fp_fail));

      EXPECT_EQ(nullptr,
                RawRasterBand::Create(
                    &fds, 1, fp_fail, 10, std::numeric_limits<int>::max(), 8,
                    GDT_Byte, RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                    RawRasterBand::OwnFP::NO));
      EXPECT_EQ(nullptr,
                RawRasterBand::Create(
                    &fds, 1, fp_fail, static_cast<vsi_l_offset>(GINTBIG_MAX), 1,
                    8, GDT_Byte, RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                    RawRasterBand::OwnFP::NO));
      TestRawDataset huge_x_ds(100000, 2, GA_ReadOnly);
      EXPECT_EQ(nullptr,
                RawRasterBand::Create(
                    &huge_x_ds, 1, fp_fail, 0, 100000, 100000, GDT_Byte,
                    RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                    RawRasterBand::OwnFP::NO));
      CPLErrorReset();
    }
  }
}

}  // namespace
}  // namespace autotest2
