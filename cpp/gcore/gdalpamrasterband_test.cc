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

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal_pam.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"

namespace {

class TestPamDataset : public GDALPamDataset {
 public:
  TestPamDataset(int xsize, int ysize) {
    nRasterXSize = xsize;
    nRasterYSize = ysize;
    SetDescription("/vsimem/test_pam_band.tif");
    SetPamFlags(GPF_NOSAVE);
  }
  ~TestPamDataset() override { FlushCache(true); }

  using GDALPamDataset::PamClear;
  using GDALPamDataset::PamInitialize;
  using GDALPamDataset::SetBand;
};

class TestPamBand : public GDALPamRasterBand {
 public:
  TestPamBand(GDALDataset* ds, int band_num, GDALDataType dt = GDT_Byte,
              bool force_cached_io = false)
      : GDALPamRasterBand(force_cached_io ? TRUE : FALSE) {
    poDS = ds;
    nBand = band_num;
    eDataType = dt;
    nRasterXSize = ds ? ds->GetRasterXSize() : 4;
    nRasterYSize = ds ? ds->GetRasterYSize() : 4;
    nBlockXSize = nRasterXSize;
    nBlockYSize = 1;
  }

  CPLErr IReadBlock(int /*x*/, int y, void* buf) override {
    memset(
        buf, y + 1,
        static_cast<size_t>(nBlockXSize) * GDALGetDataTypeSizeBytes(eDataType));
    return CE_None;
  }

  using GDALPamRasterBand::MarkPamDirty;
  using GDALPamRasterBand::PamClear;
  using GDALPamRasterBand::PamInitialize;
  using GDALPamRasterBand::PamInitializeNoParent;
  using GDALPamRasterBand::SerializeToXML;
  using GDALPamRasterBand::XMLInit;
};

TEST(GdalPamRasterBandTest, PamInfoCopyFromAndLifecycle) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // Test standalone band without parent dataset and PamInitializeNoParent:
  {
    TestPamBand standalone(nullptr, 1, GDT_Byte, false);
    // Before PamInitializeNoParent, psPam is nullptr -> fallback methods run:
    int success = -1;
    EXPECT_DOUBLE_EQ(standalone.GetNoDataValue(&success), -1e10);
    EXPECT_EQ(standalone.GetNoDataValueAsInt64(&success),
              GDAL_PAM_DEFAULT_NODATA_VALUE_INT64);
    EXPECT_EQ(standalone.GetNoDataValueAsUInt64(&success),
              GDAL_PAM_DEFAULT_NODATA_VALUE_UINT64);
    EXPECT_DOUBLE_EQ(standalone.GetOffset(&success), 0.0);
    EXPECT_DOUBLE_EQ(standalone.GetScale(&success), 1.0);
    EXPECT_STREQ(standalone.GetUnitType(), "");
    EXPECT_EQ(standalone.GetCategoryNames(), nullptr);
    EXPECT_EQ(standalone.GetColorTable(), nullptr);
    EXPECT_EQ(standalone.GetColorInterpretation(), GCI_Undefined);
    EXPECT_EQ(standalone.GetDefaultRAT(), nullptr);
    EXPECT_EQ(standalone.SetNoDataValue(1.0), CE_Failure);
    EXPECT_EQ(standalone.SetNoDataValueAsInt64(1), CE_Failure);
    EXPECT_EQ(standalone.SetNoDataValueAsUInt64(1), CE_Failure);
    EXPECT_EQ(standalone.DeleteNoDataValue(), CE_Failure);
    EXPECT_EQ(standalone.SetOffset(1.0), CE_Failure);
    EXPECT_EQ(standalone.SetScale(2.0), CE_Failure);
    EXPECT_EQ(standalone.SetUnitType("m"), CE_Failure);
    EXPECT_EQ(standalone.SetCategoryNames(nullptr), CE_Failure);
    EXPECT_EQ(standalone.SetColorTable(nullptr), CE_Failure);
    EXPECT_EQ(standalone.SetColorInterpretation(GCI_RedBand), CE_Failure);
    EXPECT_EQ(standalone.SetDefaultRAT(nullptr), CE_Failure);
    EXPECT_EQ(standalone.SerializeToXML(nullptr), nullptr);

    // Now initialize without parent, then attach to a PAM dataset and call
    // PamInitialize() to exercise the replacement of parentless psPam:
    standalone.PamInitializeNoParent();
    ASSERT_NE(standalone.GetPamInfo(), nullptr);
    TestPamDataset ds(4, 4);
    ds.SetBand(1, new TestPamBand(&ds, 1));
    auto* b1 = static_cast<TestPamBand*>(ds.GetRasterBand(1));
    b1->PamInitializeNoParent();
    b1->PamInitialize();
    ASSERT_NE(b1->GetPamInfo(), nullptr);
  }

  // Test GDALRasterBandPamInfo::CopyFrom with all fields populated:
  {
    TestPamDataset ds(4, 4);
    auto* b1 = new TestPamBand(&ds, 1, GDT_Float64);
    ds.SetBand(1, b1);
    b1->SetDescription("BandOne");
    b1->SetNoDataValue(1.23456789);
    b1->SetOffset(10.0);
    b1->SetScale(2.5);
    b1->SetUnitType("kelvin");
    b1->SetColorInterpretation(GCI_PaletteIndex);
    const char* cats[] = {"Zero", "", "Two", nullptr};
    b1->SetCategoryNames(const_cast<char**>(cats));
    GDALColorTable ct;
    GDALColorEntry ce = {1, 2, 3, 255};
    ct.SetColorEntry(0, &ce);
    b1->SetColorTable(&ct);
    GUIntBig hist[2] = {10, 6};
    b1->SetDefaultHistogram(0.0, 10.0, 2, hist);
    GDALDefaultRasterAttributeTable rat;
    rat.CreateColumn("Val", GFT_Integer, GFU_Generic);
    rat.SetRowCount(1);
    b1->SetDefaultRAT(&rat);

    GDALRasterBandPamInfo* info1 = b1->GetPamInfo();
    ASSERT_NE(info1, nullptr);
    info1->bHaveMinMax = TRUE;
    info1->dfMin = 1.0;
    info1->dfMax = 9.0;
    info1->bHaveStats = TRUE;
    info1->dfMean = 5.0;
    info1->dfStdDev = 1.5;

    GDALRasterBandPamInfo copy_info;
    copy_info.CopyFrom(*info1);
    EXPECT_TRUE(copy_info.bNoDataValueSet);
    EXPECT_NE(copy_info.poColorTable, nullptr);
    EXPECT_STREQ(copy_info.pszUnitType, "kelvin");
    EXPECT_NE(copy_info.papszCategoryNames, nullptr);
    EXPECT_NE(copy_info.psSavedHistograms, nullptr);
    EXPECT_NE(copy_info.poDefaultRAT, nullptr);
    // Clean up copy_info owned pointers:
    GDALRasterBandPamInfo empty_info;
    copy_info.CopyFrom(empty_info);
  }

  CPLPopErrorHandler();
}

TEST(GdalPamRasterBandTest, SerializeXMLInitNoDataHistogramsAndCloneInfo) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  TestPamDataset ds(4, 4);
  auto* b_double = new TestPamBand(&ds, 1, GDT_Float64);
  auto* b_i64 = new TestPamBand(&ds, 2, GDT_Int64);
  auto* b_u64 = new TestPamBand(&ds, 3, GDT_UInt64);
  ds.SetBand(1, b_double);
  ds.SetBand(2, b_i64);
  ds.SetBand(3, b_u64);

  // 1. NoDataValue variants (double, NaN, Int64, UInt64, cross-type getters):
  int success = 0;
  EXPECT_EQ(b_double->SetNoDataValue(std::numeric_limits<double>::quiet_NaN()),
            CE_None);
  EXPECT_TRUE(std::isnan(b_double->GetNoDataValue(&success)));
  EXPECT_TRUE(success);
  CPLXMLNode* nan_xml = b_double->SerializeToXML(nullptr);
  ASSERT_NE(nan_xml, nullptr);
  CPLDestroyXMLNode(nan_xml);

  EXPECT_EQ(b_double->SetNoDataValue(0.1234567890123456), CE_None);
  // Calling GetNoDataValueAsInt64 / UInt64 on Float64 band fails:
  EXPECT_EQ(b_double->GetNoDataValueAsInt64(&success),
            GDAL_PAM_DEFAULT_NODATA_VALUE_INT64);
  EXPECT_FALSE(success);
  EXPECT_EQ(b_double->GetNoDataValueAsUInt64(&success),
            GDAL_PAM_DEFAULT_NODATA_VALUE_UINT64);
  EXPECT_FALSE(success);

  // Int64 band:
  EXPECT_EQ(b_i64->SetNoDataValueAsInt64(-1234567890123LL), CE_None);
  EXPECT_EQ(b_i64->GetNoDataValueAsInt64(&success), -1234567890123LL);
  EXPECT_TRUE(success);
  EXPECT_DOUBLE_EQ(b_i64->GetNoDataValue(&success),
                   static_cast<double>(-1234567890123LL));
  EXPECT_EQ(b_i64->GetNoDataValueAsUInt64(&success),
            GDAL_PAM_DEFAULT_NODATA_VALUE_UINT64);
  EXPECT_FALSE(success);

  // UInt64 band:
  EXPECT_EQ(b_u64->SetNoDataValueAsUInt64(1234567890123ULL), CE_None);
  EXPECT_EQ(b_u64->GetNoDataValueAsUInt64(&success), 1234567890123ULL);
  EXPECT_TRUE(success);
  EXPECT_DOUBLE_EQ(b_u64->GetNoDataValue(&success),
                   static_cast<double>(1234567890123ULL));
  EXPECT_EQ(b_u64->GetNoDataValueAsInt64(&success),
            GDAL_PAM_DEFAULT_NODATA_VALUE_INT64);
  EXPECT_FALSE(success);

  // 2. Full properties on b_double + SerializeToXML + XMLInit into target:
  b_double->SetDescription("Elevation");
  b_double->SetUnitType("meters");
  b_double->SetUnitType("");  // clears pszUnitType
  b_double->SetUnitType("feet");
  b_double->SetOffset(5.0);
  b_double->SetScale(0.5);
  b_double->SetColorInterpretation(GCI_GrayIndex);
  const char* cats[] = {"CatA", "", "CatC", nullptr};
  b_double->SetCategoryNames(const_cast<char**>(cats));
  GDALColorTable ct;
  GDALColorEntry ce = {10, 20, 30, 255};
  ct.SetColorEntry(0, &ce);
  b_double->SetColorTable(&ct);
  b_double->SetMetadataItem("BKEY", "BVAL");
  CPLStringList bmd;
  bmd.SetNameValue("K1", "V1");
  b_double->SetMetadata(bmd.List());

  b_double->GetPamInfo()->bHaveMinMax = TRUE;
  b_double->GetPamInfo()->dfMin = 0.0;
  b_double->GetPamInfo()->dfMax = 100.0;
  b_double->GetPamInfo()->bHaveStats = TRUE;
  b_double->GetPamInfo()->dfMean = 50.0;
  b_double->GetPamInfo()->dfStdDev = 12.0;

  // Histograms:
  GUIntBig hist_counts[4] = {0, 0, 0, 0};
  EXPECT_EQ(b_double->GetHistogram(0.0, 10.0, 4, hist_counts, TRUE, FALSE,
                                   nullptr, nullptr),
            CE_None);
  // Second call hits cached histogram via PamFindMatchingHistogram:
  EXPECT_EQ(b_double->GetHistogram(0.0, 10.0, 4, hist_counts, TRUE, FALSE,
                                   nullptr, nullptr),
            CE_None);
  // SetDefaultHistogram replaces matching histogram:
  GUIntBig def_hist[4] = {1, 2, 3, 4};
  EXPECT_EQ(b_double->SetDefaultHistogram(0.0, 10.0, 4, def_hist), CE_None);
  double hmin = 0, hmax = 0;
  int hbuckets = 0;
  GUIntBig* parsed_hist = nullptr;
  EXPECT_EQ(b_double->GetDefaultHistogram(&hmin, &hmax, &hbuckets, &parsed_hist,
                                          FALSE, nullptr, nullptr),
            CE_None);
  EXPECT_EQ(hbuckets, 4);
  CPLFree(parsed_hist);

  // RAT:
  GDALDefaultRasterAttributeTable rat;
  rat.CreateColumn("Class", GFT_String, GFU_Name);
  rat.SetRowCount(2);
  rat.SetValue(0, 0, "Forest");
  EXPECT_EQ(b_double->SetDefaultRAT(&rat), CE_None);
  EXPECT_NE(b_double->GetDefaultRAT(), nullptr);

  // Serialize all 3 bands and XMLInit into a fresh dataset:
  CPLXMLNode* xml1 = b_double->SerializeToXML(nullptr);
  CPLXMLNode* xml2 = b_i64->SerializeToXML(nullptr);
  CPLXMLNode* xml3 = b_u64->SerializeToXML(nullptr);
  ASSERT_NE(xml1, nullptr);
  ASSERT_NE(xml2, nullptr);
  ASSERT_NE(xml3, nullptr);

  TestPamDataset ds2(4, 4);
  auto* dst1 = new TestPamBand(&ds2, 1, GDT_Float64);
  auto* dst2 = new TestPamBand(&ds2, 2, GDT_Int64);
  auto* dst3 = new TestPamBand(&ds2, 3, GDT_UInt64);
  ds2.SetBand(1, dst1);
  ds2.SetBand(2, dst2);
  ds2.SetBand(3, dst3);

  EXPECT_EQ(dst1->XMLInit(xml1, nullptr), CE_None);
  EXPECT_EQ(dst2->XMLInit(xml2, nullptr), CE_None);
  EXPECT_EQ(dst3->XMLInit(xml3, nullptr), CE_None);
  CPLDestroyXMLNode(xml1);
  CPLDestroyXMLNode(xml2);
  CPLDestroyXMLNode(xml3);

  EXPECT_STREQ(dst1->GetDescription(), "Elevation");
  EXPECT_STREQ(dst1->GetUnitType(), "feet");
  EXPECT_DOUBLE_EQ(dst1->GetOffset(), 5.0);
  EXPECT_DOUBLE_EQ(dst1->GetScale(), 0.5);
  EXPECT_EQ(dst2->GetNoDataValueAsInt64(&success), -1234567890123LL);
  EXPECT_EQ(dst3->GetNoDataValueAsUInt64(&success), 1234567890123ULL);

  // 3. CloneInfo (with and without GCIF_ONLY_IF_MISSING):
  TestPamDataset ds3(4, 4);
  auto* clone1 = new TestPamBand(&ds3, 1, GDT_Float64);
  auto* clone2 = new TestPamBand(&ds3, 2, GDT_Int64);
  auto* clone3 = new TestPamBand(&ds3, 3, GDT_UInt64);
  ds3.SetBand(1, clone1);
  ds3.SetBand(2, clone2);
  ds3.SetBand(3, clone3);

  EXPECT_EQ(clone1->CloneInfo(b_double, GCIF_PAM_DEFAULT), CE_None);
  EXPECT_EQ(
      clone1->CloneInfo(b_double, GCIF_PAM_DEFAULT & ~GCIF_ONLY_IF_MISSING),
      CE_None);
  EXPECT_EQ(clone2->CloneInfo(b_i64, GCIF_NODATA | GCIF_ONLY_IF_MISSING),
            CE_None);
  EXPECT_EQ(clone2->CloneInfo(b_i64, GCIF_NODATA), CE_None);
  EXPECT_EQ(clone3->CloneInfo(b_u64, GCIF_NODATA | GCIF_ONLY_IF_MISSING),
            CE_None);
  EXPECT_EQ(clone3->CloneInfo(b_u64, GCIF_NODATA), CE_None);

  // DeleteNoDataValue and clearing ColorTable / RAT:
  EXPECT_EQ(clone1->DeleteNoDataValue(), CE_None);
  EXPECT_EQ(clone1->SetColorTable(nullptr), CE_None);
  EXPECT_EQ(clone1->SetDefaultRAT(nullptr), CE_None);

  // 4. PamParseHistogram / PamHistogramToXMLTree edge cases:
  EXPECT_EQ(PamParseHistogram(nullptr, &hmin, &hmax, &hbuckets, nullptr,
                              nullptr, nullptr),
            FALSE);
  EXPECT_EQ(PamHistogramToXMLTree(0, 1, std::numeric_limits<int>::max(),
                                  def_hist, TRUE, FALSE),
            nullptr);
  CPLXMLNode* bad_hist = CPLParseXMLString(
      "<HistItem><HistMin>0</HistMin><HistMax>1</HistMax>"
      "<BucketCount>5</BucketCount><HistCounts>1</HistCounts></HistItem>");
  ASSERT_NE(bad_hist, nullptr);
  EXPECT_EQ(PamParseHistogram(bad_hist, &hmin, &hmax, &hbuckets, &parsed_hist,
                              nullptr, nullptr),
            FALSE);
  CPLDestroyXMLNode(bad_hist);

  CPLPopErrorHandler();
}

}  // namespace
