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
// Tests GDALPamRasterBand and GDALRasterBandPamInfo persistence, nodata,
// histograms, and cloning (gcore/gdalpamrasterband.cpp).
//
// Build caveat in google3: //third_party/gdal/BUILD compiles with PAM_ENABLED
// undefined, so GDAL_PAM_ENABLED defaults to "NO" unless overridden via
// CPLConfigOptionSetter.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalpamrasterband.cpp

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_colortable.h"
#include "third_party/gdal/gcore/gdal_pam.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

class TestPamDataset : public GDALPamDataset {
 public:
  TestPamDataset(int xsize, int ysize) {
    nRasterXSize = xsize;
    nRasterYSize = ysize;
    SetDescription("/vsimem/gdalpamrasterband_test/test_pam_band.tif");
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
    std::memset(
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
  // 1. Standalone band without parent dataset and PamInitializeNoParent:
  {
    TestPamBand standalone(nullptr, 1, GDT_Byte, false);
    // Before PamInitializeNoParent, psPam is nullptr -> fallback methods run:
    int success = -1;
    EXPECT_DOUBLE_EQ(-1e10, standalone.GetNoDataValue(&success));
    EXPECT_EQ(GDAL_PAM_DEFAULT_NODATA_VALUE_INT64,
              standalone.GetNoDataValueAsInt64(&success));
    EXPECT_EQ(GDAL_PAM_DEFAULT_NODATA_VALUE_UINT64,
              standalone.GetNoDataValueAsUInt64(&success));
    EXPECT_DOUBLE_EQ(0.0, standalone.GetOffset(&success));
    EXPECT_DOUBLE_EQ(1.0, standalone.GetScale(&success));
    EXPECT_STREQ("", standalone.GetUnitType());
    EXPECT_EQ(nullptr, standalone.GetCategoryNames());
    EXPECT_EQ(nullptr, standalone.GetColorTable());
    EXPECT_EQ(GCI_Undefined, standalone.GetColorInterpretation());
    EXPECT_EQ(nullptr, standalone.GetDefaultRAT());
    EXPECT_EQ(nullptr, standalone.SerializeToXML(nullptr));

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, standalone.SetNoDataValue(1.0));
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, standalone.SetNoDataValueAsInt64(1));
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, standalone.SetNoDataValueAsUInt64(1));
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, standalone.DeleteNoDataValue());
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, standalone.SetOffset(1.0));
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, standalone.SetScale(2.0));
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, standalone.SetUnitType("m"));
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, standalone.SetCategoryNames(nullptr));
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, standalone.SetColorTable(nullptr));
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, standalone.SetColorInterpretation(GCI_RedBand));
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, standalone.SetDefaultRAT(nullptr));
      CPLErrorReset();
    }

    // Now initialize without parent, then attach to a PAM dataset and call
    // PamInitialize() to exercise the replacement of parentless psPam:
    standalone.PamInitializeNoParent();
    ASSERT_NE(nullptr, standalone.GetPamInfo());
    TestPamDataset ds(4, 4);
    ds.SetBand(1, new TestPamBand(&ds, 1));
    auto* b1 = static_cast<TestPamBand*>(ds.GetRasterBand(1));
    b1->PamInitializeNoParent();
    b1->PamInitialize();
    ASSERT_NE(nullptr, b1->GetPamInfo());
  }

  // 2. Test GDALRasterBandPamInfo::CopyFrom with all fields populated:
  {
    TestPamDataset ds(4, 4);
    auto* b1 = new TestPamBand(&ds, 1, GDT_Float64);
    ds.SetBand(1, b1);
    b1->SetDescription("BandOne");
    EXPECT_EQ(CE_None, b1->SetNoDataValue(1.23456789));
    EXPECT_EQ(CE_None, b1->SetOffset(10.0));
    EXPECT_EQ(CE_None, b1->SetScale(2.5));
    EXPECT_EQ(CE_None, b1->SetUnitType("kelvin"));
    EXPECT_EQ(CE_None, b1->SetColorInterpretation(GCI_PaletteIndex));
    const char* cats[] = {"Zero", "", "Two", nullptr};
    EXPECT_EQ(CE_None, b1->SetCategoryNames(const_cast<char**>(cats)));
    GDALColorTable ct;
    constexpr GDALColorEntry kColorEntry = {1, 2, 3, 255};
    ct.SetColorEntry(0, &kColorEntry);
    EXPECT_EQ(CE_None, b1->SetColorTable(&ct));
    std::array<GUIntBig, 2> hist = {10, 6};
    EXPECT_EQ(CE_None, b1->SetDefaultHistogram(0.0, 10.0, 2, hist.data()));
    GDALDefaultRasterAttributeTable rat;
    EXPECT_EQ(CE_None, rat.CreateColumn("Val", GFT_Integer, GFU_Generic));
    rat.SetRowCount(1);
    EXPECT_EQ(CE_None, b1->SetDefaultRAT(&rat));

    GDALRasterBandPamInfo* info1 = b1->GetPamInfo();
    ASSERT_NE(nullptr, info1);
    info1->bHaveMinMax = TRUE;
    info1->dfMin = 1.0;
    info1->dfMax = 9.0;
    info1->bHaveStats = TRUE;
    info1->dfMean = 5.0;
    info1->dfStdDev = 1.5;

    GDALRasterBandPamInfo copy_info;
    absl::Cleanup clear_copy_info = [&copy_info] {
      GDALRasterBandPamInfo empty_info;
      copy_info.CopyFrom(empty_info);
    };
    copy_info.CopyFrom(*info1);
    EXPECT_TRUE(copy_info.bNoDataValueSet);
    EXPECT_NE(nullptr, copy_info.poColorTable);
    EXPECT_STREQ("kelvin", copy_info.pszUnitType);
    EXPECT_NE(nullptr, copy_info.papszCategoryNames);
    EXPECT_NE(nullptr, copy_info.psSavedHistograms);
    EXPECT_NE(nullptr, copy_info.poDefaultRAT);
  }
}

TEST(GdalPamRasterBandTest, SerializeXMLInitNoDataHistogramsAndCloneInfo) {
  TestPamDataset ds(4, 4);
  auto* b_double = new TestPamBand(&ds, 1, GDT_Float64);
  auto* b_i64 = new TestPamBand(&ds, 2, GDT_Int64);
  auto* b_u64 = new TestPamBand(&ds, 3, GDT_UInt64);
  ds.SetBand(1, b_double);
  ds.SetBand(2, b_i64);
  ds.SetBand(3, b_u64);

  // 1. NoDataValue variants (double, NaN, Int64, UInt64, cross-type getters):
  int success = 0;
  EXPECT_EQ(CE_None,
            b_double->SetNoDataValue(std::numeric_limits<double>::quiet_NaN()));
  EXPECT_TRUE(std::isnan(b_double->GetNoDataValue(&success)));
  EXPECT_TRUE(success);
  {
    CPLXMLTreeCloser nan_xml(b_double->SerializeToXML(nullptr));
    ASSERT_NE(nullptr, nan_xml.get());
  }

  EXPECT_EQ(CE_None, b_double->SetNoDataValue(0.1234567890123456));
  // Calling GetNoDataValueAsInt64 / UInt64 on Float64 band fails:
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(GDAL_PAM_DEFAULT_NODATA_VALUE_INT64,
              b_double->GetNoDataValueAsInt64(&success));
    EXPECT_FALSE(success);
    CPLErrorReset();
    EXPECT_EQ(GDAL_PAM_DEFAULT_NODATA_VALUE_UINT64,
              b_double->GetNoDataValueAsUInt64(&success));
    EXPECT_FALSE(success);
    CPLErrorReset();
  }

  // Int64 band:
  EXPECT_EQ(CE_None, b_i64->SetNoDataValueAsInt64(-1234567890123LL));
  EXPECT_EQ(-1234567890123LL, b_i64->GetNoDataValueAsInt64(&success));
  EXPECT_TRUE(success);
  EXPECT_DOUBLE_EQ(static_cast<double>(-1234567890123LL),
                   b_i64->GetNoDataValue(&success));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(GDAL_PAM_DEFAULT_NODATA_VALUE_UINT64,
              b_i64->GetNoDataValueAsUInt64(&success));
    EXPECT_FALSE(success);
    CPLErrorReset();
  }

  // UInt64 band:
  EXPECT_EQ(CE_None, b_u64->SetNoDataValueAsUInt64(1234567890123ULL));
  EXPECT_EQ(1234567890123ULL, b_u64->GetNoDataValueAsUInt64(&success));
  EXPECT_TRUE(success);
  EXPECT_DOUBLE_EQ(static_cast<double>(1234567890123ULL),
                   b_u64->GetNoDataValue(&success));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(GDAL_PAM_DEFAULT_NODATA_VALUE_INT64,
              b_u64->GetNoDataValueAsInt64(&success));
    EXPECT_FALSE(success);
    CPLErrorReset();
  }

  // 2. Full properties on b_double + SerializeToXML + XMLInit into target:
  b_double->SetDescription("Elevation");
  EXPECT_EQ(CE_None, b_double->SetUnitType("meters"));
  EXPECT_EQ(CE_None, b_double->SetUnitType(""));  // clears pszUnitType
  EXPECT_EQ(CE_None, b_double->SetUnitType("feet"));
  EXPECT_EQ(CE_None, b_double->SetOffset(5.0));
  EXPECT_EQ(CE_None, b_double->SetScale(0.5));
  EXPECT_EQ(CE_None, b_double->SetColorInterpretation(GCI_GrayIndex));
  const char* cats[] = {"CatA", "", "CatC", nullptr};
  EXPECT_EQ(CE_None, b_double->SetCategoryNames(const_cast<char**>(cats)));
  GDALColorTable ct;
  constexpr GDALColorEntry kColorEntry = {10, 20, 30, 255};
  ct.SetColorEntry(0, &kColorEntry);
  EXPECT_EQ(CE_None, b_double->SetColorTable(&ct));
  EXPECT_EQ(CE_None, b_double->SetMetadataItem("BKEY", "BVAL"));
  CPLStringList bmd;
  bmd.SetNameValue("K1", "V1");
  EXPECT_EQ(CE_None, b_double->SetMetadata(bmd.List()));

  b_double->GetPamInfo()->bHaveMinMax = TRUE;
  b_double->GetPamInfo()->dfMin = 0.0;
  b_double->GetPamInfo()->dfMax = 100.0;
  b_double->GetPamInfo()->bHaveStats = TRUE;
  b_double->GetPamInfo()->dfMean = 50.0;
  b_double->GetPamInfo()->dfStdDev = 12.0;

  // Histograms:
  std::array<GUIntBig, 4> hist_counts = {0, 0, 0, 0};
  EXPECT_EQ(CE_None, b_double->GetHistogram(0.0, 10.0, 4, hist_counts.data(),
                                            TRUE, FALSE, nullptr, nullptr));
  // Second call hits cached histogram via PamFindMatchingHistogram:
  EXPECT_EQ(CE_None, b_double->GetHistogram(0.0, 10.0, 4, hist_counts.data(),
                                            TRUE, FALSE, nullptr, nullptr));
  // SetDefaultHistogram replaces matching histogram:
  std::array<GUIntBig, 4> def_hist = {1, 2, 3, 4};
  EXPECT_EQ(CE_None,
            b_double->SetDefaultHistogram(0.0, 10.0, 4, def_hist.data()));
  double hmin = 0.0;
  double hmax = 0.0;
  int hbuckets = 0;
  {
    GUIntBig* raw_parsed_hist = nullptr;
    EXPECT_EQ(CE_None, b_double->GetDefaultHistogram(&hmin, &hmax, &hbuckets,
                                                     &raw_parsed_hist, FALSE,
                                                     nullptr, nullptr));
    std::unique_ptr<GUIntBig, CplFreeDeleter> parsed_hist(raw_parsed_hist);
    EXPECT_EQ(4, hbuckets);
  }

  // RAT:
  GDALDefaultRasterAttributeTable rat;
  EXPECT_EQ(CE_None, rat.CreateColumn("Class", GFT_String, GFU_Name));
  rat.SetRowCount(2);
  rat.SetValue(0, 0, "Forest");
  EXPECT_EQ(CE_None, b_double->SetDefaultRAT(&rat));
  EXPECT_NE(nullptr, b_double->GetDefaultRAT());

  // Serialize all 3 bands and XMLInit into a fresh dataset:
  CPLXMLTreeCloser xml1(b_double->SerializeToXML(nullptr));
  CPLXMLTreeCloser xml2(b_i64->SerializeToXML(nullptr));
  CPLXMLTreeCloser xml3(b_u64->SerializeToXML(nullptr));
  ASSERT_NE(nullptr, xml1.get());
  ASSERT_NE(nullptr, xml2.get());
  ASSERT_NE(nullptr, xml3.get());

  TestPamDataset ds2(4, 4);
  auto* dst1 = new TestPamBand(&ds2, 1, GDT_Float64);
  auto* dst2 = new TestPamBand(&ds2, 2, GDT_Int64);
  auto* dst3 = new TestPamBand(&ds2, 3, GDT_UInt64);
  ds2.SetBand(1, dst1);
  ds2.SetBand(2, dst2);
  ds2.SetBand(3, dst3);

  EXPECT_EQ(CE_None, dst1->XMLInit(xml1.get(), nullptr));
  EXPECT_EQ(CE_None, dst2->XMLInit(xml2.get(), nullptr));
  EXPECT_EQ(CE_None, dst3->XMLInit(xml3.get(), nullptr));

  EXPECT_STREQ("Elevation", dst1->GetDescription());
  EXPECT_STREQ("feet", dst1->GetUnitType());
  EXPECT_DOUBLE_EQ(5.0, dst1->GetOffset());
  EXPECT_DOUBLE_EQ(0.5, dst1->GetScale());
  EXPECT_EQ(-1234567890123LL, dst2->GetNoDataValueAsInt64(&success));
  EXPECT_EQ(1234567890123ULL, dst3->GetNoDataValueAsUInt64(&success));

  // 3. CloneInfo (with and without GCIF_ONLY_IF_MISSING):
  TestPamDataset ds3(4, 4);
  auto* clone1 = new TestPamBand(&ds3, 1, GDT_Float64);
  auto* clone2 = new TestPamBand(&ds3, 2, GDT_Int64);
  auto* clone3 = new TestPamBand(&ds3, 3, GDT_UInt64);
  ds3.SetBand(1, clone1);
  ds3.SetBand(2, clone2);
  ds3.SetBand(3, clone3);

  EXPECT_EQ(CE_None, clone1->CloneInfo(b_double, GCIF_PAM_DEFAULT));
  EXPECT_EQ(CE_None, clone1->CloneInfo(
                         b_double, GCIF_PAM_DEFAULT & ~GCIF_ONLY_IF_MISSING));
  EXPECT_EQ(CE_None,
            clone2->CloneInfo(b_i64, GCIF_NODATA | GCIF_ONLY_IF_MISSING));
  EXPECT_EQ(CE_None, clone2->CloneInfo(b_i64, GCIF_NODATA));
  EXPECT_EQ(CE_None,
            clone3->CloneInfo(b_u64, GCIF_NODATA | GCIF_ONLY_IF_MISSING));
  EXPECT_EQ(CE_None, clone3->CloneInfo(b_u64, GCIF_NODATA));

  // DeleteNoDataValue and clearing ColorTable / RAT:
  EXPECT_EQ(CE_None, clone1->DeleteNoDataValue());
  EXPECT_EQ(CE_None, clone1->SetColorTable(nullptr));
  EXPECT_EQ(CE_None, clone1->SetDefaultRAT(nullptr));

  // 4. PamParseHistogram / PamHistogramToXMLTree edge cases:
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE, PamParseHistogram(nullptr, &hmin, &hmax, &hbuckets,
                                       nullptr, nullptr, nullptr));
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              PamHistogramToXMLTree(0, 1, std::numeric_limits<int>::max(),
                                    def_hist.data(), TRUE, FALSE));
    CPLErrorReset();
    CPLXMLTreeCloser bad_hist(CPLParseXMLString(
        "<HistItem><HistMin>0</HistMin><HistMax>1</HistMax>"
        "<BucketCount>5</BucketCount><HistCounts>1</HistCounts></HistItem>"));
    ASSERT_NE(nullptr, bad_hist.get());
    GUIntBig* bad_parsed_hist = nullptr;
    EXPECT_EQ(FALSE, PamParseHistogram(bad_hist.get(), &hmin, &hmax, &hbuckets,
                                       &bad_parsed_hist, nullptr, nullptr));
    std::unique_ptr<GUIntBig, CplFreeDeleter> bad_parsed_hist_holder(
        bad_parsed_hist);
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
