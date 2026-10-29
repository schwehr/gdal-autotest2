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

#include <cstddef>
#include <memory>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_proxy.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_virtualmem.h"

namespace gdal {
namespace {

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
    memset(pImage, 7, 16);
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
    if (underlying && underlying->GetRasterCount() > 0) {
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
  GDALAllRegister();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(drv, nullptr);
  std::unique_ptr<GDALDataset> mem_ds(
      drv->Create("mem", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(mem_ds, nullptr);

  TestProxyDataset proxy(mem_ds.get());
  proxy.UnrefUnderlyingDataset(mem_ds.get());

  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  GDALRasterIOExtraArg extra;
  INIT_RASTERIO_EXTRA_ARG(extra);
  GByte buf[16] = {0};
  int band_map[1] = {1};

  // Valid IRasterIO
  EXPECT_EQ(proxy.IRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte, 1,
                            band_map, 1, 4, 16, &extra),
            CE_None);
  // Out of range window
  EXPECT_EQ(proxy.IRasterIO(GF_Read, 1, 0, 4, 4, buf, 4, 4, GDT_Byte, 1,
                            band_map, 1, 4, 16, &extra),
            CE_Failure);
  EXPECT_EQ(proxy.IRasterIO(GF_Read, 0, 1, 4, 4, buf, 4, 4, GDT_Byte, 1,
                            band_map, 1, 4, 16, &extra),
            CE_Failure);
  // panBandMap == nullptr and nBandCount > GetRasterCount()
  EXPECT_EQ(proxy.IRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte, 2,
                            nullptr, 1, 4, 16, &extra),
            CE_Failure);
  // Invalid band in panBandMap
  int bad_band_map[1] = {2};
  EXPECT_EQ(proxy.IRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte, 1,
                            bad_band_map, 1, 4, 16, &extra),
            CE_Failure);
  int zero_band_map[1] = {0};
  EXPECT_EQ(proxy.IRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte, 1,
                            zero_band_map, 1, 4, 16, &extra),
            CE_Failure);

  // Underlying dataset with null band
  NullBandDataset null_band_ds;
  proxy.SetUnderlying(&null_band_ds);
  EXPECT_EQ(proxy.IRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte, 1,
                            band_map, 1, 4, 16, &extra),
            CE_Failure);

  proxy.SetUnderlying(mem_ds.get());
  EXPECT_EQ(proxy.BlockBasedRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte,
                                     1, band_map, 1, 4, 16, &extra),
            CE_None);
  int ovr = 2;
  EXPECT_EQ(proxy.IBuildOverviews("NEAREST", 1, &ovr, 1, band_map, nullptr,
                                  nullptr, nullptr),
            CE_None);
  EXPECT_TRUE(proxy.GetCompressionFormats(0, 0, 4, 4, 1, band_map).empty());
  void* comp_buf = nullptr;
  size_t comp_size = 0;
  EXPECT_EQ(proxy.ReadCompressedData("NONE", 0, 0, 4, 4, 1, band_map, &comp_buf,
                                     &comp_size, nullptr),
            CE_Failure);
  EXPECT_EQ(proxy.FlushCache(false), CE_None);

  EXPECT_EQ(proxy.SetMetadataItem("K", "V", ""), CE_None);
  EXPECT_STREQ(proxy.GetMetadataItem("K", ""), "V");
  char** md = proxy.GetMetadata("");
  EXPECT_NE(md, nullptr);
  EXPECT_EQ(proxy.SetMetadata(md, "SUB"), CE_None);
  char** doms = proxy.GetMetadataDomainList();
  CSLDestroy(doms);

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  EXPECT_EQ(proxy.SetSpatialRef(&srs), CE_None);
  EXPECT_NE(proxy.GetSpatialRef(), nullptr);

  GDALGeoTransform gt(1.0, 2.0, 0.0, 3.0, 0.0, -2.0);
  EXPECT_EQ(proxy.SetGeoTransform(gt), CE_None);
  GDALGeoTransform gt_out;
  EXPECT_EQ(proxy.GetGeoTransform(gt_out), CE_None);
  EXPECT_DOUBLE_EQ(gt_out[0], 1.0);

  EXPECT_EQ(proxy.GetInternalHandle("test"), nullptr);
  EXPECT_EQ(proxy.GetDriver(), drv);
  char** files = proxy.GetFileList();
  CSLDestroy(files);

  GDAL_GCP gcp;
  GDALInitGCPs(1, &gcp);
  gcp.dfGCPPixel = 1.0;
  gcp.dfGCPLine = 2.0;
  EXPECT_EQ(proxy.SetGCPs(1, &gcp, &srs), CE_None);
  GDALDeinitGCPs(1, &gcp);
  EXPECT_EQ(proxy.GetGCPCount(), 1);
  EXPECT_NE(proxy.GetGCPSpatialRef(), nullptr);
  EXPECT_NE(proxy.GetGCPs(), nullptr);

  EXPECT_EQ(proxy.AdviseRead(0, 0, 4, 4, 4, 4, GDT_Byte, 1, band_map, nullptr),
            CE_None);
  EXPECT_EQ(proxy.CreateMaskBand(GMF_PER_DATASET), CE_None);

  // Now test with nullptr underlying dataset
  proxy.SetUnderlying(nullptr);
  EXPECT_EQ(proxy.IRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte, 1,
                            band_map, 1, 4, 16, &extra),
            CE_Failure);
  EXPECT_EQ(proxy.BlockBasedRasterIO(GF_Read, 0, 0, 4, 4, buf, 4, 4, GDT_Byte,
                                     1, band_map, 1, 4, 16, &extra),
            CE_Failure);
  EXPECT_EQ(proxy.IBuildOverviews("NEAREST", 1, &ovr, 1, band_map, nullptr,
                                  nullptr, nullptr),
            CE_Failure);
  EXPECT_TRUE(proxy.GetCompressionFormats(0, 0, 4, 4, 1, band_map).empty());
  EXPECT_EQ(proxy.ReadCompressedData("NONE", 0, 0, 4, 4, 1, band_map, &comp_buf,
                                     &comp_size, nullptr),
            CE_Failure);
  EXPECT_EQ(proxy.FlushCache(false), CE_None);
  EXPECT_EQ(proxy.GetMetadataDomainList(), nullptr);
  EXPECT_EQ(proxy.GetMetadata(""), nullptr);
  EXPECT_EQ(proxy.SetMetadata(nullptr, ""), CE_Failure);
  EXPECT_EQ(proxy.GetMetadataItem("K", ""), nullptr);
  EXPECT_EQ(proxy.SetMetadataItem("K", "V", ""), CE_Failure);
  EXPECT_EQ(proxy.GetSpatialRef(), nullptr);
  EXPECT_EQ(proxy.SetSpatialRef(&srs), CE_Failure);
  EXPECT_EQ(proxy.GetGeoTransform(gt_out), CE_Failure);
  EXPECT_EQ(proxy.SetGeoTransform(gt), CE_Failure);
  EXPECT_EQ(proxy.GetInternalHandle("test"), nullptr);
  EXPECT_EQ(proxy.GetDriver(), nullptr);
  EXPECT_EQ(proxy.GetFileList(), nullptr);
  EXPECT_EQ(proxy.GetGCPCount(), 0);
  EXPECT_EQ(proxy.GetGCPSpatialRef(), nullptr);
  EXPECT_EQ(proxy.GetGCPs(), nullptr);
  EXPECT_EQ(proxy.SetGCPs(0, nullptr, nullptr), CE_Failure);
  EXPECT_EQ(proxy.AdviseRead(0, 0, 4, 4, 4, 4, GDT_Byte, 1, band_map, nullptr),
            CE_Failure);
  EXPECT_EQ(proxy.CreateMaskBand(GMF_PER_DATASET), CE_Failure);
}

TEST(GdalProxyDatasetTest, RasterBandMethodsWithAndWithoutUnderlying) {
  GDALAllRegister();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(drv, nullptr);
  std::unique_ptr<GDALDataset> mem_ds(
      drv->Create("mem", 4, 4, 1, GDT_Byte, nullptr));
  GDALRasterBand* src_band = mem_ds->GetRasterBand(1);

  TestProxyDataset proxy_ds(mem_ds.get());
  auto* band = static_cast<TestProxyRasterBand*>(proxy_ds.GetRasterBand(1));
  band->UnrefUnderlyingRasterBand(src_band);

  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  GByte block[16] = {0};
  BlockReadWriteDataset brw_ds;
  TestProxyRasterBand brw_proxy(&proxy_ds, 1, brw_ds.GetRasterBand(1));
  EXPECT_EQ(brw_proxy.IWriteBlock(0, 0, block), CE_None);
  EXPECT_EQ(brw_proxy.IReadBlock(0, 0, block), CE_None);

  // Mismatched data type
  TestProxyRasterBand bad_dt_band(&proxy_ds, 1, src_band, GDT_Int16, 4, 4);
  EXPECT_EQ(bad_dt_band.IReadBlock(0, 0, block), CE_Failure);
  EXPECT_EQ(bad_dt_band.IWriteBlock(0, 0, block), CE_Failure);

  // Mismatched block dimensions
  TestProxyRasterBand bad_blk_band(&proxy_ds, 1, src_band, GDT_Byte, 2, 2);
  EXPECT_EQ(bad_blk_band.IReadBlock(0, 0, block), CE_Failure);
  EXPECT_EQ(bad_blk_band.IWriteBlock(0, 0, block), CE_Failure);

  // Failing InitBlockInfo on source band
  FailInitBlockBand fail_init_band;
  TestProxyRasterBand fail_init_proxy(&proxy_ds, 1, &fail_init_band);
  EXPECT_EQ(fail_init_proxy.IReadBlock(0, 0, block), CE_Failure);
  EXPECT_EQ(fail_init_proxy.IWriteBlock(0, 0, block), CE_Failure);

  // IRasterIO
  GDALRasterIOExtraArg extra;
  INIT_RASTERIO_EXTRA_ARG(extra);
  EXPECT_EQ(
      band->IRasterIO(GF_Read, 0, 0, 4, 4, block, 4, 4, GDT_Byte, 1, 4, &extra),
      CE_None);
  EXPECT_EQ(
      band->IRasterIO(GF_Read, 1, 0, 4, 4, block, 4, 4, GDT_Byte, 1, 4, &extra),
      CE_Failure);
  EXPECT_EQ(
      band->IRasterIO(GF_Read, 0, 1, 4, 4, block, 4, 4, GDT_Byte, 1, 4, &extra),
      CE_Failure);

  double pct = -1.0;
  EXPECT_GE(band->IGetDataCoverageStatus(0, 0, 4, 4, 0, &pct), 0);

  // Metadata
  band->EnablePixelTypeSignedByteWarning(false);
  EXPECT_EQ(band->SetMetadataItem("BK", "BV", ""), CE_None);
  EXPECT_STREQ(band->GetMetadataItem("BK", ""), "BV");
  band->EnablePixelTypeSignedByteWarning(true);
  char** md = band->GetMetadata("");
  EXPECT_EQ(band->SetMetadata(md, "BSUB"), CE_None);
  char** doms = band->GetMetadataDomainList();
  CSLDestroy(doms);

  // Blocks & cache
  GDALRasterBlock* rb = band->GetLockedBlockRef(0, 0, FALSE);
  if (rb) rb->DropLock();
  rb = band->TryGetLockedBlockRef(0, 0);
  if (rb) rb->DropLock();
  EXPECT_EQ(band->FlushBlock(0, 0, TRUE), CE_None);
  EXPECT_EQ(band->FlushCache(false), CE_None);

  // Band properties
  const char* cats[2] = {"cat1", nullptr};
  EXPECT_EQ(band->SetCategoryNames(const_cast<char**>(cats)), CE_None);
  EXPECT_NE(band->GetCategoryNames(), nullptr);
  EXPECT_EQ(band->SetNoDataValue(99.0), CE_None);
  int success = 0;
  EXPECT_DOUBLE_EQ(band->GetNoDataValue(&success), 99.0);
  EXPECT_EQ(band->DeleteNoDataValue(), CE_None);
  band->GetMinimum(&success);
  band->GetMaximum(&success);
  EXPECT_EQ(band->SetOffset(1.0), CE_None);
  EXPECT_DOUBLE_EQ(band->GetOffset(&success), 1.0);
  EXPECT_EQ(band->SetScale(2.0), CE_None);
  EXPECT_DOUBLE_EQ(band->GetScale(&success), 2.0);
  EXPECT_EQ(band->SetUnitType("m"), CE_None);
  EXPECT_STREQ(band->GetUnitType(), "m");
  EXPECT_EQ(band->SetColorInterpretation(GCI_GrayIndex), CE_None);
  EXPECT_EQ(band->GetColorInterpretation(), GCI_GrayIndex);
  GDALColorTable ct;
  EXPECT_EQ(band->SetColorTable(&ct), CE_None);
  EXPECT_NE(band->GetColorTable(), nullptr);
  EXPECT_EQ(band->Fill(5.0, 0.0), CE_None);

  // Statistics & Histograms
  double min_v, max_v, mean_v, std_v;
  EXPECT_EQ(band->SetStatistics(1.0, 5.0, 3.0, 1.0), CE_None);
  EXPECT_EQ(band->GetStatistics(TRUE, TRUE, &min_v, &max_v, &mean_v, &std_v),
            CE_None);
  EXPECT_EQ(band->ComputeStatistics(FALSE, &min_v, &max_v, &mean_v, &std_v,
                                    nullptr, nullptr),
            CE_None);
  double minmax[2];
  EXPECT_EQ(band->ComputeRasterMinMax(FALSE, minmax), CE_None);

  EXPECT_EQ(band->HasArbitraryOverviews(), 0);
  int ovr = 2;
  EXPECT_EQ(band->BuildOverviews("NEAREST", 1, &ovr, nullptr, nullptr, nullptr),
            CE_Failure);
  EXPECT_GE(band->GetOverviewCount(), 0);
  EXPECT_EQ(band->GetOverview(0), nullptr);
  EXPECT_NE(band->GetRasterSampleOverview(1), nullptr);
  EXPECT_EQ(band->AdviseRead(0, 0, 4, 4, 4, 4, GDT_Byte, nullptr), CE_None);

  GUIntBig hist[4] = {0};
  EXPECT_EQ(
      band->GetHistogram(0.0, 10.0, 4, hist, TRUE, FALSE, nullptr, nullptr),
      CE_None);
  EXPECT_EQ(band->SetDefaultHistogram(0.0, 10.0, 4, hist), CE_None);
  int n_buckets = 0;
  GUIntBig* def_hist = nullptr;
  if (band->GetDefaultHistogram(&min_v, &max_v, &n_buckets, &def_hist, TRUE,
                                nullptr, nullptr) == CE_None) {
    VSIFree(def_hist);
  }

  GDALDefaultRasterAttributeTable rat;
  EXPECT_EQ(band->SetDefaultRAT(&rat), CE_None);
  EXPECT_NE(band->GetDefaultRAT(), nullptr);

  EXPECT_EQ(band->CreateMaskBand(GMF_PER_DATASET), CE_None);
  EXPECT_NE(band->GetMaskBand(), nullptr);
  EXPECT_NE(band->GetMaskFlags(), 0);
  EXPECT_FALSE(band->IsMaskBand());
  EXPECT_EQ(band->GetMaskValueRange(), GMVR_UNKNOWN);

  int pspace = 0;
  GIntBig lspace = 0;
  CPLVirtualMem* vmem =
      band->GetVirtualMemAuto(GF_Read, &pspace, &lspace, nullptr);
  if (vmem) CPLVirtualMemFree(vmem);

  double real_val = 0.0;
  EXPECT_EQ(band->InterpolateAtPoint(1.0, 1.0, GRIORA_NearestNeighbour,
                                     &real_val, nullptr),
            CE_None);

  // Now test with nullptr underlying band
  band->SetUnderlying(nullptr);
  band->EnablePixelTypeSignedByteWarning(false);
  EXPECT_EQ(band->IReadBlock(0, 0, block), CE_Failure);
  EXPECT_EQ(band->IWriteBlock(0, 0, block), CE_Failure);
  EXPECT_EQ(
      band->IRasterIO(GF_Read, 0, 0, 4, 4, block, 4, 4, GDT_Byte, 1, 4, &extra),
      CE_Failure);
  EXPECT_NE(band->IGetDataCoverageStatus(0, 0, 4, 4, 0, &pct), 0);
  EXPECT_EQ(band->GetMetadataDomainList(), nullptr);
  EXPECT_EQ(band->GetMetadata(""), nullptr);
  EXPECT_EQ(band->SetMetadata(nullptr, ""), CE_Failure);
  EXPECT_EQ(band->GetMetadataItem("BK", ""), nullptr);
  EXPECT_EQ(band->SetMetadataItem("BK", "BV", ""), CE_Failure);
  EXPECT_EQ(band->GetLockedBlockRef(0, 0, FALSE), nullptr);
  EXPECT_EQ(band->TryGetLockedBlockRef(0, 0), nullptr);
  EXPECT_EQ(band->FlushBlock(0, 0, TRUE), CE_Failure);
  EXPECT_EQ(band->FlushCache(false), CE_Failure);
  EXPECT_EQ(band->GetCategoryNames(), nullptr);
  EXPECT_DOUBLE_EQ(band->GetNoDataValue(&success), 0.0);
  EXPECT_DOUBLE_EQ(band->GetMinimum(&success), 0.0);
  EXPECT_DOUBLE_EQ(band->GetMaximum(&success), 0.0);
  EXPECT_DOUBLE_EQ(band->GetOffset(&success), 0.0);
  EXPECT_DOUBLE_EQ(band->GetScale(&success), 0.0);
  EXPECT_EQ(band->GetUnitType(), nullptr);
  EXPECT_EQ(band->GetColorInterpretation(), GCI_Undefined);
  EXPECT_EQ(band->GetColorTable(), nullptr);
  EXPECT_EQ(band->Fill(0.0, 0.0), CE_Failure);
  EXPECT_EQ(band->SetCategoryNames(nullptr), CE_Failure);
  EXPECT_EQ(band->SetNoDataValue(0.0), CE_Failure);
  EXPECT_EQ(band->DeleteNoDataValue(), CE_Failure);
  EXPECT_EQ(band->SetColorTable(nullptr), CE_Failure);
  EXPECT_EQ(band->SetColorInterpretation(GCI_Undefined), CE_Failure);
  EXPECT_EQ(band->SetOffset(0.0), CE_Failure);
  EXPECT_EQ(band->SetScale(0.0), CE_Failure);
  EXPECT_EQ(band->SetUnitType(""), CE_Failure);
  EXPECT_EQ(band->GetStatistics(TRUE, TRUE, &min_v, &max_v, &mean_v, &std_v),
            CE_Failure);
  EXPECT_EQ(band->ComputeStatistics(FALSE, &min_v, &max_v, &mean_v, &std_v,
                                    nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(band->SetStatistics(0, 0, 0, 0), CE_Failure);
  EXPECT_EQ(band->ComputeRasterMinMax(FALSE, minmax), CE_Failure);
  EXPECT_EQ(band->HasArbitraryOverviews(), 0);
  EXPECT_EQ(band->GetOverviewCount(), 0);
  EXPECT_EQ(band->GetOverview(0), nullptr);
  EXPECT_EQ(band->GetRasterSampleOverview(1), nullptr);
  EXPECT_EQ(band->BuildOverviews("NEAREST", 1, &ovr, nullptr, nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(band->AdviseRead(0, 0, 4, 4, 4, 4, GDT_Byte, nullptr), CE_Failure);
  EXPECT_EQ(band->GetHistogram(0, 10, 4, hist, TRUE, FALSE, nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(band->GetDefaultHistogram(&min_v, &max_v, &n_buckets, &def_hist,
                                      TRUE, nullptr, nullptr),
            CE_Failure);
  EXPECT_EQ(band->SetDefaultHistogram(0, 10, 4, hist), CE_Failure);
  EXPECT_EQ(band->GetDefaultRAT(), nullptr);
  EXPECT_EQ(band->SetDefaultRAT(nullptr), CE_Failure);
  EXPECT_EQ(band->GetMaskBand(), nullptr);
  EXPECT_EQ(band->GetMaskFlags(), 0);
  EXPECT_EQ(band->CreateMaskBand(0), CE_Failure);
  EXPECT_FALSE(band->IsMaskBand());
  EXPECT_EQ(band->GetMaskValueRange(), GMVR_UNKNOWN);
  EXPECT_EQ(band->GetVirtualMemAuto(GF_Read, &pspace, &lspace, nullptr),
            nullptr);
  EXPECT_EQ(band->InterpolateAtPoint(0, 0, GRIORA_NearestNeighbour, &real_val,
                                     nullptr),
            CE_Failure);
}

}  // namespace
}  // namespace gdal
