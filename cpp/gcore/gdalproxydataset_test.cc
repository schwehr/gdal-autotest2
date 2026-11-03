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
// Tests GDALProxyDataset and GDALProxyRasterBand (gcore/gdalproxydataset.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalproxydataset.cpp

#include <cstddef>
#include <cstring>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_proxy.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_virtualmem.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using VirtualMemPtr =
    std::unique_ptr<CPLVirtualMem,
                    decltype([](CPLVirtualMem* p) { CPLVirtualMemFree(p); })>;

class NullBandDataset final : public GDALDataset {
 public:
  NullBandDataset() {
    nRasterXSize = 4;
    nRasterYSize = 4;
    nBands = 1;
    papoBands = static_cast<GDALRasterBand**>(CPLCalloc(1, sizeof(void*)));
  }
  ~NullBandDataset() override {
    CPLFree(papoBands);
    papoBands = nullptr;
    nBands = 0;
  }
};

class BlockReadWriteBand final : public GDALRasterBand {
 public:
  BlockReadWriteBand(GDALDataset* ds, int band) {
    poDS = ds;
    nBand = band;
    nRasterXSize = 4;
    nRasterYSize = 4;
    eDataType = GDT_Byte;
    nBlockXSize = 4;
    nBlockYSize = 4;
  }
  CPLErr IReadBlock(int, int, void* pImage) override {
    std::memset(pImage, 7, 16);
    return CE_None;
  }
  CPLErr IWriteBlock(int, int, void*) override { return CE_None; }
};

class BlockReadWriteDataset final : public GDALDataset {
 public:
  BlockReadWriteDataset() {
    nRasterXSize = 4;
    nRasterYSize = 4;
    eAccess = GA_Update;
    SetBand(1, new BlockReadWriteBand(this, 1));
  }
};

class FailInitBlockBand final : public GDALRasterBand {
 public:
  FailInitBlockBand() {
    nRasterXSize = 4;
    nRasterYSize = 4;
    eDataType = GDT_Byte;
    nBlockXSize = 0;  // Invalid block size triggers InitBlockInfo() failure
    nBlockYSize = 0;
  }
  CPLErr IReadBlock(int, int, void*) override { return CE_None; }
};

class TestProxyRasterBand final : public GDALProxyRasterBand {
 public:
  TestProxyRasterBand(GDALDataset* ds, int band, GDALRasterBand* underlying,
                      GDALDataType dt = GDT_Byte, int bx = 4, int by = 4)
      : m_underlying(underlying) {
    poDS = ds;
    nBand = band;
    nRasterXSize = 4;
    nRasterYSize = 4;
    eDataType = dt;
    nBlockXSize = bx;
    nBlockYSize = by;
  }

  void SetUnderlying(GDALRasterBand* band) { m_underlying = band; }

  using GDALProxyRasterBand::IGetDataCoverageStatus;
  using GDALProxyRasterBand::IRasterIO;
  using GDALProxyRasterBand::IReadBlock;
  using GDALProxyRasterBand::IWriteBlock;
  using GDALProxyRasterBand::UnrefUnderlyingRasterBand;

 protected:
  GDALRasterBand* RefUnderlyingRasterBand(bool /*bForceOpen*/) const override {
    return m_underlying;
  }

 private:
  GDALRasterBand* m_underlying = nullptr;
};

class TestProxyDataset final : public GDALProxyDataset {
 public:
  explicit TestProxyDataset(GDALDataset* underlying)
      : m_underlying(underlying) {
    nRasterXSize = 4;
    nRasterYSize = 4;
    if (underlying != nullptr && underlying->GetRasterCount() > 0) {
      SetBand(1,
              new TestProxyRasterBand(this, 1, underlying->GetRasterBand(1)));
    }
  }

  void SetUnderlying(GDALDataset* ds) { m_underlying = ds; }

  using GDALProxyDataset::BlockBasedRasterIO;
  using GDALProxyDataset::IBuildOverviews;
  using GDALProxyDataset::IRasterIO;
  using GDALProxyDataset::UnrefUnderlyingDataset;

 protected:
  GDALDataset* RefUnderlyingDataset() const override { return m_underlying; }

 private:
  GDALDataset* m_underlying = nullptr;
};

TEST(GdalProxyDatasetTest, DatasetMethodsWithAndWithoutUnderlying) {
  GDALRegister_MEM();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr mem_ds(drv->Create("mem", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, mem_ds);

  TestProxyDataset proxy(mem_ds.get());
  proxy.UnrefUnderlyingDataset(mem_ds.get());

  GDALRasterIOExtraArg extra;
  INIT_RASTERIO_EXTRA_ARG(extra);
  GByte buf[16] = {0};
  int band_map[1] = {1};

  // Valid IRasterIO
  EXPECT_EQ(CE_None, proxy.IRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte,
                                     1, band_map, 1, 4, 16, &extra));

  {
    WithQuietHandler quiet;
    // Out of range window
    EXPECT_EQ(CE_Failure,
              proxy.IRasterIO(GF_Read, 1, 0, 4, 4, buf, 4, 4, GDT_Byte, 1,
                              band_map, 1, 4, 16, &extra));
    EXPECT_EQ(CE_Failure,
              proxy.IRasterIO(GF_Read, 0, 1, 4, 4, buf, 4, 4, GDT_Byte, 1,
                              band_map, 1, 4, 16, &extra));
    // panBandMap == nullptr and nBandCount > GetRasterCount()
    EXPECT_EQ(CE_Failure,
              proxy.IRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte, 2,
                              nullptr, 1, 4, 16, &extra));
    // Invalid band in panBandMap
    int bad_band_map[1] = {2};
    EXPECT_EQ(CE_Failure,
              proxy.IRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte, 1,
                              bad_band_map, 1, 4, 16, &extra));
    int zero_band_map[1] = {0};
    EXPECT_EQ(CE_Failure,
              proxy.IRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte, 1,
                              zero_band_map, 1, 4, 16, &extra));

    // Underlying dataset with null band
    NullBandDataset null_band_ds;
    proxy.SetUnderlying(&null_band_ds);
    EXPECT_EQ(CE_Failure,
              proxy.IRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte, 1,
                              band_map, 1, 4, 16, &extra));
    CPLErrorReset();
  }

  proxy.SetUnderlying(mem_ds.get());
  EXPECT_EQ(CE_None,
            proxy.BlockBasedRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte,
                                     1, band_map, 1, 4, 16, &extra));
  int ovr = 2;
  EXPECT_EQ(CE_None, proxy.IBuildOverviews("NEAREST", 1, &ovr, 1, band_map,
                                           nullptr, nullptr, nullptr));
  EXPECT_TRUE(proxy.GetCompressionFormats(0, 0, 4, 4, 1, band_map).empty());
  void* comp_buf = nullptr;
  size_t comp_size = 0;
  {
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure,
              proxy.ReadCompressedData("NONE", 0, 0, 4, 4, 1, band_map,
                                       &comp_buf, &comp_size, nullptr));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, proxy.FlushCache(false));

  EXPECT_EQ(CE_None, proxy.SetMetadataItem("K", "V", ""));
  EXPECT_STREQ("V", proxy.GetMetadataItem("K", ""));
  char** md = proxy.GetMetadata("");
  EXPECT_NE(nullptr, md);
  EXPECT_EQ(CE_None, proxy.SetMetadata(md, "SUB"));
  StringListPtr doms(proxy.GetMetadataDomainList(), CSLDestroy);

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  EXPECT_EQ(CE_None, proxy.SetSpatialRef(&srs));
  EXPECT_NE(nullptr, proxy.GetSpatialRef());

  GDALGeoTransform gt(1.0, 2.0, 0.0, 3.0, 0.0, -2.0);
  EXPECT_EQ(CE_None, proxy.SetGeoTransform(gt));
  GDALGeoTransform gt_out;
  EXPECT_EQ(CE_None, proxy.GetGeoTransform(gt_out));
  EXPECT_DOUBLE_EQ(1.0, gt_out[0]);

  EXPECT_EQ(nullptr, proxy.GetInternalHandle("test"));
  EXPECT_EQ(drv, proxy.GetDriver());
  StringListPtr files(proxy.GetFileList(), CSLDestroy);

  GDAL_GCP gcp;
  GDALInitGCPs(1, &gcp);
  absl::Cleanup gcp_cleaner = [&gcp] { GDALDeinitGCPs(1, &gcp); };
  gcp.dfGCPPixel = 1.0;
  gcp.dfGCPLine = 2.0;
  EXPECT_EQ(CE_None, proxy.SetGCPs(1, &gcp, &srs));
  EXPECT_EQ(1, proxy.GetGCPCount());
  EXPECT_NE(nullptr, proxy.GetGCPSpatialRef());
  EXPECT_NE(nullptr, proxy.GetGCPs());

  EXPECT_EQ(CE_None,
            proxy.AdviseRead(0, 0, 4, 4, 4, 4, GDT_Byte, 1, band_map, nullptr));
  EXPECT_EQ(CE_None, proxy.CreateMaskBand(GMF_PER_DATASET));

  // Now test with nullptr underlying dataset
  proxy.SetUnderlying(nullptr);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure,
              proxy.IRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte, 1,
                              band_map, 1, 4, 16, &extra));
    EXPECT_EQ(CE_Failure,
              proxy.BlockBasedRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte,
                                       1, band_map, 1, 4, 16, &extra));
    EXPECT_EQ(CE_Failure, proxy.IBuildOverviews("NEAREST", 1, &ovr, 1, band_map,
                                                nullptr, nullptr, nullptr));
    EXPECT_TRUE(proxy.GetCompressionFormats(0, 0, 4, 4, 1, band_map).empty());
    EXPECT_EQ(CE_Failure,
              proxy.ReadCompressedData("NONE", 0, 0, 4, 4, 1, band_map,
                                       &comp_buf, &comp_size, nullptr));
    EXPECT_EQ(CE_None, proxy.FlushCache(false));
    EXPECT_EQ(nullptr, proxy.GetMetadataDomainList());
    EXPECT_EQ(nullptr, proxy.GetMetadata(""));
    EXPECT_EQ(CE_Failure, proxy.SetMetadata(nullptr, ""));
    EXPECT_EQ(nullptr, proxy.GetMetadataItem("K", ""));
    EXPECT_EQ(CE_Failure, proxy.SetMetadataItem("K", "V", ""));
    EXPECT_EQ(nullptr, proxy.GetSpatialRef());
    EXPECT_EQ(CE_Failure, proxy.SetSpatialRef(&srs));
    EXPECT_EQ(CE_Failure, proxy.GetGeoTransform(gt_out));
    EXPECT_EQ(CE_Failure, proxy.SetGeoTransform(gt));
    EXPECT_EQ(nullptr, proxy.GetInternalHandle("test"));
    EXPECT_EQ(nullptr, proxy.GetDriver());
    EXPECT_EQ(nullptr, proxy.GetFileList());
    EXPECT_EQ(0, proxy.GetGCPCount());
    EXPECT_EQ(nullptr, proxy.GetGCPSpatialRef());
    EXPECT_EQ(nullptr, proxy.GetGCPs());
    EXPECT_EQ(CE_Failure, proxy.SetGCPs(0, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, proxy.AdviseRead(0, 0, 4, 4, 4, 4, GDT_Byte, 1,
                                           band_map, nullptr));
    EXPECT_EQ(CE_Failure, proxy.CreateMaskBand(GMF_PER_DATASET));
    CPLErrorReset();
  }
}

TEST(GdalProxyDatasetTest, RasterBandMethodsWithAndWithoutUnderlying) {
  GDALRegister_MEM();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr mem_ds(drv->Create("mem", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, mem_ds);
  GDALRasterBand* src_band = mem_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, src_band);

  TestProxyDataset proxy_ds(mem_ds.get());
  auto* band = cpl::down_cast<TestProxyRasterBand*>(proxy_ds.GetRasterBand(1));
  band->UnrefUnderlyingRasterBand(src_band);

  GByte block[16] = {0};
  BlockReadWriteDataset brw_ds;
  TestProxyRasterBand brw_proxy(&proxy_ds, 1, brw_ds.GetRasterBand(1));
  EXPECT_EQ(CE_None, brw_proxy.IWriteBlock(0, 0, block));
  EXPECT_EQ(CE_None, brw_proxy.IReadBlock(0, 0, block));

  {
    WithQuietHandler quiet;
    // Mismatched data type
    TestProxyRasterBand bad_dt_band(&proxy_ds, 1, src_band, GDT_Int16, 4, 4);
    EXPECT_EQ(CE_Failure, bad_dt_band.IReadBlock(0, 0, block));
    EXPECT_EQ(CE_Failure, bad_dt_band.IWriteBlock(0, 0, block));

    // Mismatched block dimensions
    TestProxyRasterBand bad_blk_band(&proxy_ds, 1, src_band, GDT_Byte, 2, 2);
    EXPECT_EQ(CE_Failure, bad_blk_band.IReadBlock(0, 0, block));
    EXPECT_EQ(CE_Failure, bad_blk_band.IWriteBlock(0, 0, block));

    // Failing InitBlockInfo on source band
    FailInitBlockBand fail_init_band;
    TestProxyRasterBand fail_init_proxy(&proxy_ds, 1, &fail_init_band);
    EXPECT_EQ(CE_Failure, fail_init_proxy.IReadBlock(0, 0, block));
    EXPECT_EQ(CE_Failure, fail_init_proxy.IWriteBlock(0, 0, block));
    CPLErrorReset();
  }

  // IRasterIO
  GDALRasterIOExtraArg extra;
  INIT_RASTERIO_EXTRA_ARG(extra);
  EXPECT_EQ(CE_None, band->IRasterIO(GF_Read, 0, 0, 4, 4, block, 4, 4, GDT_Byte,
                                     1, 4, &extra));
  {
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, band->IRasterIO(GF_Read, 1, 0, 4, 4, block, 4, 4,
                                          GDT_Byte, 1, 4, &extra));
    EXPECT_EQ(CE_Failure, band->IRasterIO(GF_Read, 0, 1, 4, 4, block, 4, 4,
                                          GDT_Byte, 1, 4, &extra));
    CPLErrorReset();
  }

  double pct = -1.0;
  EXPECT_GE(band->IGetDataCoverageStatus(0, 0, 4, 4, 0, &pct), 0);

  // Metadata
  band->EnablePixelTypeSignedByteWarning(false);
  EXPECT_EQ(CE_None, band->SetMetadataItem("BK", "BV", ""));
  EXPECT_STREQ("BV", band->GetMetadataItem("BK", ""));
  band->EnablePixelTypeSignedByteWarning(true);
  char** md = band->GetMetadata("");
  EXPECT_EQ(CE_None, band->SetMetadata(md, "BSUB"));
  StringListPtr doms(band->GetMetadataDomainList(), CSLDestroy);

  // Blocks & cache
  GDALRasterBlock* rb = band->GetLockedBlockRef(0, 0, FALSE);
  if (rb != nullptr) rb->DropLock();
  rb = band->TryGetLockedBlockRef(0, 0);
  if (rb != nullptr) rb->DropLock();
  EXPECT_EQ(CE_None, band->FlushBlock(0, 0, TRUE));
  EXPECT_EQ(CE_None, band->FlushCache(false));

  // Band properties
  CPLStringList cats;
  cats.AddString("cat1");
  EXPECT_EQ(CE_None, band->SetCategoryNames(cats.List()));
  EXPECT_NE(nullptr, band->GetCategoryNames());
  EXPECT_EQ(CE_None, band->SetNoDataValue(99.0));
  int success = 0;
  EXPECT_DOUBLE_EQ(99.0, band->GetNoDataValue(&success));
  EXPECT_EQ(CE_None, band->DeleteNoDataValue());
  band->GetMinimum(&success);
  band->GetMaximum(&success);
  EXPECT_EQ(CE_None, band->SetOffset(1.0));
  EXPECT_DOUBLE_EQ(1.0, band->GetOffset(&success));
  EXPECT_EQ(CE_None, band->SetScale(2.0));
  EXPECT_DOUBLE_EQ(2.0, band->GetScale(&success));
  EXPECT_EQ(CE_None, band->SetUnitType("m"));
  EXPECT_STREQ("m", band->GetUnitType());
  EXPECT_EQ(CE_None, band->SetColorInterpretation(GCI_GrayIndex));
  EXPECT_EQ(GCI_GrayIndex, band->GetColorInterpretation());
  GDALColorTable ct;
  EXPECT_EQ(CE_None, band->SetColorTable(&ct));
  EXPECT_NE(nullptr, band->GetColorTable());
  EXPECT_EQ(CE_None, band->Fill(5.0, 0.0));

  // Statistics & Histograms
  double min_v = 0.0;
  double max_v = 0.0;
  double mean_v = 0.0;
  double std_v = 0.0;
  EXPECT_EQ(CE_None, band->SetStatistics(1.0, 5.0, 3.0, 1.0));
  EXPECT_EQ(CE_None,
            band->GetStatistics(TRUE, TRUE, &min_v, &max_v, &mean_v, &std_v));
  EXPECT_EQ(CE_None, band->ComputeStatistics(FALSE, &min_v, &max_v, &mean_v,
                                             &std_v, nullptr, nullptr));
  double minmax[2] = {0.0, 0.0};
  EXPECT_EQ(CE_None, band->ComputeRasterMinMax(FALSE, minmax));

  EXPECT_EQ(0, band->HasArbitraryOverviews());
  int ovr = 2;
  {
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, band->BuildOverviews("NEAREST", 1, &ovr, nullptr,
                                               nullptr, nullptr));
    CPLErrorReset();
  }
  EXPECT_GE(band->GetOverviewCount(), 0);
  EXPECT_EQ(nullptr, band->GetOverview(0));
  EXPECT_NE(nullptr, band->GetRasterSampleOverview(1));
  EXPECT_EQ(CE_None, band->AdviseRead(0, 0, 4, 4, 4, 4, GDT_Byte, nullptr));

  GUIntBig hist[4] = {0};
  EXPECT_EQ(CE_None, band->GetHistogram(0.0, 10.0, 4, hist, TRUE, FALSE,
                                        nullptr, nullptr));
  EXPECT_EQ(CE_None, band->SetDefaultHistogram(0.0, 10.0, 4, hist));
  int n_buckets = 0;
  GUIntBig* def_hist = nullptr;
  absl::Cleanup hist_cleaner = [&def_hist] { VSIFree(def_hist); };
  EXPECT_EQ(CE_None,
            band->GetDefaultHistogram(&min_v, &max_v, &n_buckets, &def_hist,
                                      TRUE, nullptr, nullptr));

  GDALDefaultRasterAttributeTable rat;
  EXPECT_EQ(CE_None, band->SetDefaultRAT(&rat));
  EXPECT_NE(nullptr, band->GetDefaultRAT());

  EXPECT_EQ(CE_None, band->CreateMaskBand(GMF_PER_DATASET));
  EXPECT_NE(nullptr, band->GetMaskBand());
  EXPECT_NE(0, band->GetMaskFlags());
  EXPECT_FALSE(band->IsMaskBand());
  EXPECT_EQ(GMVR_UNKNOWN, band->GetMaskValueRange());

  int pspace = 0;
  GIntBig lspace = 0;
  VirtualMemPtr vmem(
      band->GetVirtualMemAuto(GF_Read, &pspace, &lspace, nullptr));

  double real_val = 0.0;
  EXPECT_EQ(CE_None, band->InterpolateAtPoint(1.0, 1.0, GRIORA_NearestNeighbour,
                                              &real_val, nullptr));

  // Now test with nullptr underlying band
  band->SetUnderlying(nullptr);
  band->EnablePixelTypeSignedByteWarning(false);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, band->IReadBlock(0, 0, block));
    EXPECT_EQ(CE_Failure, band->IWriteBlock(0, 0, block));
    EXPECT_EQ(CE_Failure, band->IRasterIO(GF_Read, 0, 0, 4, 4, block, 4, 4,
                                          GDT_Byte, 1, 4, &extra));
    EXPECT_NE(0, band->IGetDataCoverageStatus(0, 0, 4, 4, 0, &pct));
    EXPECT_EQ(nullptr, band->GetMetadataDomainList());
    EXPECT_EQ(nullptr, band->GetMetadata(""));
    EXPECT_EQ(CE_Failure, band->SetMetadata(nullptr, ""));
    EXPECT_EQ(nullptr, band->GetMetadataItem("BK", ""));
    EXPECT_EQ(CE_Failure, band->SetMetadataItem("BK", "BV", ""));
    EXPECT_EQ(nullptr, band->GetLockedBlockRef(0, 0, FALSE));
    EXPECT_EQ(nullptr, band->TryGetLockedBlockRef(0, 0));
    EXPECT_EQ(CE_Failure, band->FlushBlock(0, 0, TRUE));
    EXPECT_EQ(CE_Failure, band->FlushCache(false));
    EXPECT_EQ(nullptr, band->GetCategoryNames());
    EXPECT_DOUBLE_EQ(0.0, band->GetNoDataValue(&success));
    EXPECT_DOUBLE_EQ(0.0, band->GetMinimum(&success));
    EXPECT_DOUBLE_EQ(0.0, band->GetMaximum(&success));
    EXPECT_DOUBLE_EQ(0.0, band->GetOffset(&success));
    EXPECT_DOUBLE_EQ(0.0, band->GetScale(&success));
    EXPECT_EQ(nullptr, band->GetUnitType());
    EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());
    EXPECT_EQ(nullptr, band->GetColorTable());
    EXPECT_EQ(CE_Failure, band->Fill(0.0, 0.0));
    EXPECT_EQ(CE_Failure, band->SetCategoryNames(nullptr));
    EXPECT_EQ(CE_Failure, band->SetNoDataValue(0.0));
    EXPECT_EQ(CE_Failure, band->DeleteNoDataValue());
    EXPECT_EQ(CE_Failure, band->SetColorTable(nullptr));
    EXPECT_EQ(CE_Failure, band->SetColorInterpretation(GCI_Undefined));
    EXPECT_EQ(CE_Failure, band->SetOffset(0.0));
    EXPECT_EQ(CE_Failure, band->SetScale(0.0));
    EXPECT_EQ(CE_Failure, band->SetUnitType(""));
    EXPECT_EQ(CE_Failure,
              band->GetStatistics(TRUE, TRUE, &min_v, &max_v, &mean_v, &std_v));
    EXPECT_EQ(CE_Failure,
              band->ComputeStatistics(FALSE, &min_v, &max_v, &mean_v, &std_v,
                                      nullptr, nullptr));
    EXPECT_EQ(CE_Failure, band->SetStatistics(0, 0, 0, 0));
    EXPECT_EQ(CE_Failure, band->ComputeRasterMinMax(FALSE, minmax));
    EXPECT_EQ(0, band->HasArbitraryOverviews());
    EXPECT_EQ(0, band->GetOverviewCount());
    EXPECT_EQ(nullptr, band->GetOverview(0));
    EXPECT_EQ(nullptr, band->GetRasterSampleOverview(1));
    EXPECT_EQ(CE_Failure, band->BuildOverviews("NEAREST", 1, &ovr, nullptr,
                                               nullptr, nullptr));
    EXPECT_EQ(CE_Failure,
              band->AdviseRead(0, 0, 4, 4, 4, 4, GDT_Byte, nullptr));
    EXPECT_EQ(CE_Failure, band->GetHistogram(0, 10, 4, hist, TRUE, FALSE,
                                             nullptr, nullptr));
    GUIntBig* null_def_hist = nullptr;
    EXPECT_EQ(CE_Failure, band->GetDefaultHistogram(&min_v, &max_v, &n_buckets,
                                                    &null_def_hist, TRUE,
                                                    nullptr, nullptr));
    EXPECT_EQ(CE_Failure, band->SetDefaultHistogram(0, 10, 4, hist));
    EXPECT_EQ(nullptr, band->GetDefaultRAT());
    EXPECT_EQ(CE_Failure, band->SetDefaultRAT(nullptr));
    EXPECT_EQ(nullptr, band->GetMaskBand());
    EXPECT_EQ(0, band->GetMaskFlags());
    EXPECT_EQ(CE_Failure, band->CreateMaskBand(0));
    EXPECT_FALSE(band->IsMaskBand());
    EXPECT_EQ(GMVR_UNKNOWN, band->GetMaskValueRange());
    EXPECT_EQ(nullptr,
              band->GetVirtualMemAuto(GF_Read, &pspace, &lspace, nullptr));
    EXPECT_EQ(CE_Failure,
              band->InterpolateAtPoint(0, 0, GRIORA_NearestNeighbour, &real_val,
                                       nullptr));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
