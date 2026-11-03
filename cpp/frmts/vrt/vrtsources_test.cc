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
// Tests the VRTSource hierarchy: VRTSimpleSource, VRTAveragedSource,
// VRTNoDataFromMaskSource, VRTComplexSource, and VRTFuncSource
// (vrtsources.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/vrtsources.cpp
//   https://gdal.org/drivers/raster/vrt.html

#include <algorithm>
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/vrt/gdal_vrt.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/vrtsources_test";

CPLErr MyFuncSourceRead(void* /*hCBData*/, int /*nXOff*/, int /*nYOff*/,
                        int nXSize, int nYSize, void* pData) {
  auto pabyData = static_cast<GByte*>(pData);
  std::fill(pabyData, pabyData + nXSize * nYSize, static_cast<GByte>(77));
  return CE_None;
}

// Expose protected refcount/filename internals. When a local
// GDALDatasetUniquePtr owns the source dataset and a stack
// VRTSimpleSource/VRTComplexSource attaches via SetSrcBand(GDALRasterBand*),
// m_bDropRefOnSrcBand must be cleared to false because SetSrcBand(band*) does
// not increment the dataset reference count whereas ~VRTSimpleSource() calls
// ReleaseRef() whenever m_bDropRefOnSrcBand is true.
class TestSimpleSource : public VRTSimpleSource {
 public:
  using VRTSimpleSource::AddSourceFilenameNode;
  using VRTSimpleSource::IsDstWinSet;
  using VRTSimpleSource::IsSrcWinSet;
  using VRTSimpleSource::m_bDropRefOnSrcBand;
  using VRTSimpleSource::m_bRelativeToVRTOri;
  using VRTSimpleSource::m_osSrcDSName;
  using VRTSimpleSource::NeedMaxValAdjustment;
  using VRTSimpleSource::VRTSimpleSource;
};

class TestComplexSource : public VRTComplexSource {
 public:
  using VRTComplexSource::GetAdjustedNoDataValue;
  using VRTComplexSource::m_adfLUTInputs;
  using VRTComplexSource::m_adfLUTOutputs;
  using VRTComplexSource::m_bDropRefOnSrcBand;
  using VRTComplexSource::m_bSrcMinMaxDefined;
  using VRTComplexSource::m_dfNoDataValue;
  using VRTComplexSource::m_nProcessingFlags;
  using VRTComplexSource::VRTComplexSource;
};

class VrtSourcesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_VRT();
    GDALRegister_GTiff();
    GDALRegister_MEM();
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(VrtSourcesTest, VRTParseCoreSourcesAndBase) {
  VRTMapSharedResources mapSharedSources;

  // 1. Unknown source element fails.
  {
    CPLXMLTreeCloser oTree(
        CPLParseXMLString("<UnknownSourceType><SourceFilename "
                          "relativeToVRT=\"0\">foo.tif</SourceFilename></"
                          "UnknownSourceType>"));
    ASSERT_NE(nullptr, oTree.get());
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr,
              VRTParseCoreSources(oTree.get(), nullptr, mapSharedSources));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. Core source XML elements.
  struct SourceTypeCase {
    std::string_view xml;
    std::string_view expected_type;
  };
  constexpr SourceTypeCase kCases[] = {
      {.xml = "<SimpleSource><SourceFilename "
              "relativeToVRT=\"0\">foo.tif</SourceFilename></SimpleSource>",
       .expected_type = "SimpleSource"},
      {.xml = "<AveragedSource><SourceFilename "
              "relativeToVRT=\"0\">foo.tif</SourceFilename></AveragedSource>",
       .expected_type = "AveragedSource"},
      {.xml = "<SimpleSource resampling=\"Average\"><SourceFilename "
              "relativeToVRT=\"0\">foo.tif</SourceFilename></SimpleSource>",
       .expected_type = "AveragedSource"},
      {.xml = "<ComplexSource><SourceFilename "
              "relativeToVRT=\"0\">foo.tif</SourceFilename></ComplexSource>",
       .expected_type = "ComplexSource"},
      {.xml = "<NoDataFromMaskSource><SourceFilename "
              "relativeToVRT=\"0\">foo.tif</SourceFilename></"
              "NoDataFromMaskSource>",
       .expected_type = "NoDataFromMaskSource"},
  };

  for (const auto& tc : kCases) {
    const std::string xml_str(tc.xml);
    CPLXMLTreeCloser oTree(CPLParseXMLString(xml_str.c_str()));
    ASSERT_NE(nullptr, oTree.get()) << tc.expected_type;
    std::unique_ptr<VRTSource> poSrc(
        VRTParseCoreSources(oTree.get(), nullptr, mapSharedSources));
    ASSERT_NE(nullptr, poSrc) << tc.expected_type;
    EXPECT_EQ(tc.expected_type, std::string_view(poSrc->GetType()));
  }

  // 3. VRTSource::GetFileList default implementation (empty).
  {
    class DummyVRTSource : public VRTSource {
     public:
      CPLErr XMLInit(const CPLXMLNode*, const char*,
                     VRTMapSharedResources&) override {
        return CE_None;
      }
      CPLErr RasterIO(GDALDataType, int, int, int, int, void*, int, int,
                      GDALDataType, GSpacing, GSpacing, GDALRasterIOExtraArg*,
                      WorkingState&) override {
        return CE_None;
      }
      double GetMinimum(int, int, int* pbSuccess) override {
        if (pbSuccess) *pbSuccess = FALSE;
        return 0;
      }
      double GetMaximum(int, int, int* pbSuccess) override {
        if (pbSuccess) *pbSuccess = FALSE;
        return 0;
      }
      CPLErr GetHistogram(int, int, double, double, int, GUIntBig*, int, int,
                          GDALProgressFunc, void*) override {
        return CE_Failure;
      }
      CPLXMLNode* SerializeToXML(const char*) override { return nullptr; }
      const char* GetType() const override { return "Dummy"; }
    };

    DummyVRTSource dummy;
    char** papszFileList = nullptr;
    absl::Cleanup free_filelist = [&papszFileList] {
      CSLDestroy(papszFileList);
    };
    int nSize = 0;
    int nMaxSize = 0;
    CPLHashSet* hSetFiles =
        CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);
    ASSERT_NE(nullptr, hSetFiles);
    absl::Cleanup free_hashset = [hSetFiles] { CPLHashSetDestroy(hSetFiles); };
    dummy.GetFileList(&papszFileList, &nSize, &nMaxSize, hSetFiles);
    EXPECT_EQ(nullptr, papszFileList);
  }
}

TEST_F(VrtSourcesTest, SimpleSourcePropertiesAndTransforms) {
  TestSimpleSource ss;
  EXPECT_STREQ("SimpleSource", VRTSimpleSource::GetTypeStatic());
  EXPECT_STREQ("SimpleSource", ss.GetType());
  EXPECT_TRUE(ss.IsSimpleSource());

  // 1. Windows not set initially.
  EXPECT_FALSE(ss.IsSrcWinSet());
  EXPECT_FALSE(ss.IsDstWinSet());

  // 2. Set windows.
  ss.SetSrcWindow(10, 20, 30, 40);
  EXPECT_TRUE(ss.IsSrcWinSet());
  ss.SetDstWindow(50, 60, 70, 80);
  EXPECT_TRUE(ss.IsDstWinSet());

  double dfDstXOff = 0.0;
  double dfDstYOff = 0.0;
  double dfDstXSize = 0.0;
  double dfDstYSize = 0.0;
  ss.GetDstWindow(dfDstXOff, dfDstYOff, dfDstXSize, dfDstYSize);
  EXPECT_DOUBLE_EQ(50.0, dfDstXOff);
  EXPECT_DOUBLE_EQ(60.0, dfDstYOff);
  EXPECT_DOUBLE_EQ(70.0, dfDstXSize);
  EXPECT_DOUBLE_EQ(80.0, dfDstYSize);

  // 3. DstWindowIntersects.
  EXPECT_TRUE(ss.DstWindowIntersects(60, 70, 10, 10));
  EXPECT_FALSE(ss.DstWindowIntersects(0, 0, 10, 10));
  EXPECT_FALSE(ss.DstWindowIntersects(200, 200, 10, 10));

  // 4. SrcToDst and DstToSrc.
  double dfX = 0.0;
  double dfY = 0.0;
  ss.SrcToDst(10, 20, dfX, dfY);
  EXPECT_DOUBLE_EQ(50.0, dfX);
  EXPECT_DOUBLE_EQ(60.0, dfY);

  ss.DstToSrc(50, 60, dfX, dfY);
  EXPECT_DOUBLE_EQ(10.0, dfX);
  EXPECT_DOUBLE_EQ(20.0, dfY);

  // 5. SetResampling.
  ss.SetResampling("bilinear");
  EXPECT_EQ("bilinear", ss.GetResampling());

  // 6. NeedMaxValAdjustment without band.
  EXPECT_EQ(FALSE, ss.NeedMaxValAdjustment());
  ss.SetMaxValue(100);
  EXPECT_EQ(FALSE, ss.NeedMaxValAdjustment());

  // 7. UnsetPreservedRelativeFilenames & FlushCache.
  ss.UnsetPreservedRelativeFilenames();
  EXPECT_EQ(CE_None, ss.FlushCache(false));
  EXPECT_EQ(CE_None, ss.FlushCache(true));
}

TEST_F(VrtSourcesTest, SimpleSourceScaleConstructorAndComparison) {
  GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poDrv);

  constexpr char kSrcTest[] = "/vsimem/vrtsources_test/vrt_src_test.tif";
  {
    GDALDatasetUniquePtr poSrcDS(
        poDrv->Create(kSrcTest, 40, 20, 2, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrcDS);
  }

  VRTSimpleSource ss1;
  ss1.SetSrcBand(kSrcTest, 1);
  ss1.SetSrcWindow(0, 0, 40, 20);
  ss1.SetDstWindow(0, 0, 80, 40);

  // 1. Scale constructor with set dst window.
  VRTSimpleSource ssScaled(&ss1, 0.5, 0.5);
  double dfDstXOff = 0.0;
  double dfDstYOff = 0.0;
  double dfDstXSize = 0.0;
  double dfDstYSize = 0.0;
  ssScaled.GetDstWindow(dfDstXOff, dfDstYOff, dfDstXSize, dfDstYSize);
  EXPECT_DOUBLE_EQ(40.0, dfDstXSize);
  EXPECT_DOUBLE_EQ(20.0, dfDstYSize);

  // 2. Scale constructor without set windows.
  VRTSimpleSource ssUnset;
  ssUnset.SetSrcBand(kSrcTest, 1);
  VRTSimpleSource ssScaledFromUnset(&ssUnset, 2.0, 2.0);
  ssScaledFromUnset.GetDstWindow(dfDstXOff, dfDstYOff, dfDstXSize, dfDstYSize);
  EXPECT_DOUBLE_EQ(80.0, dfDstXSize);
  EXPECT_DOUBLE_EQ(40.0, dfDstYSize);
  // Workaround for an upstream GDAL reference-count bug in
  // VRTSimpleSource::VRTSimpleSource(const VRTSimpleSource*, double, double)
  // (vrtsources.cpp): ssUnset never opened its source band, so the copied
  // m_poRasterBand is null. Because no src/dst window is set and the ratios
  // differ from 1.0, the constructor calls GetRasterBand(), which runs
  // OpenSource() and creates the proxy pool dataset with a reference count of
  // 1 that is owned by the new source. The constructor then unconditionally
  // calls poDS->Reference() (since m_bDropRefOnSrcBand is true), bumping the
  // count to 2, whereas ~VRTSimpleSource() only releases a single reference.
  // The extra reference would therefore leak the proxy dataset; drop it here.
  // TODO(b/335317901): Remove this once the constructor is fixed upstream to
  // only add a reference when the band was already opened by poSrcSource.
  if (auto* poBand = ssScaledFromUnset.GetRasterBand()) {
    if (auto* poDS = poBand->GetDataset()) {
      poDS->ReleaseRef();
    }
  }

  // 3. IsSameExceptBandNumber.
  VRTSimpleSource ss2;
  ss2.SetSrcBand(kSrcTest, 2);
  ss2.SetSrcWindow(0, 0, 40, 20);
  ss2.SetDstWindow(0, 0, 80, 40);
  EXPECT_TRUE(ss1.IsSameExceptBandNumber(&ss2));

  VRTSimpleSource ssDiff;
  ssDiff.SetSrcBand(kSrcTest, 2);
  ssDiff.SetSrcWindow(0, 0, 20, 10);
  ssDiff.SetDstWindow(0, 0, 80, 40);
  EXPECT_FALSE(ss1.IsSameExceptBandNumber(&ssDiff));

  // 4. GetRasterBand, GetMaskBandMainBand.
  EXPECT_NE(nullptr, ss1.GetRasterBand());
  EXPECT_NE(nullptr, ss1.GetRasterBand()->GetDataset());
  EXPECT_EQ(nullptr, ss1.GetMaskBandMainBand());

  // 5. File list.
  char** papszFileList = nullptr;
  absl::Cleanup free_filelist = [&papszFileList] { CSLDestroy(papszFileList); };
  int nSize = 0;
  int nMaxSize = 0;
  CPLHashSet* hSetFiles =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);
  ASSERT_NE(nullptr, hSetFiles);
  absl::Cleanup free_hashset = [hSetFiles] { CPLHashSetDestroy(hSetFiles); };
  ss1.GetFileList(&papszFileList, &nSize, &nMaxSize, hSetFiles);
  ASSERT_NE(nullptr, papszFileList);
  EXPECT_GT(CSLCount(papszFileList), 0);
}

TEST_F(VrtSourcesTest, SimpleSourceGetSrcDstWindowBranches) {
  VRTSimpleSource ss;
  double dfReqXOff = 0.0;
  double dfReqYOff = 0.0;
  double dfReqXSize = 0.0;
  double dfReqYSize = 0.0;
  int nReqXOff = 0;
  int nReqYOff = 0;
  int nReqXSize = 0;
  int nReqYSize = 0;
  int nOutXOff = 0;
  int nOutYOff = 0;
  int nOutXSize = 0;
  int nOutYSize = 0;
  bool bError = false;

  // 1. Unset/zero window sizes -> returns FALSE.
  EXPECT_FALSE(ss.GetSrcDstWindow(0, 0, 10, 10, 10, 10, &dfReqXOff, &dfReqYOff,
                                  &dfReqXSize, &dfReqYSize, &nReqXOff,
                                  &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
                                  &nOutYOff, &nOutXSize, &nOutYSize, bError));

  constexpr char kWinSrc[] = "/vsimem/vrtsources_test/srcdst_win_src.tif";
  {
    GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, poDrv);
    GDALDatasetUniquePtr poSrcDS(
        poDrv->Create(kWinSrc, 100, 100, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrcDS);
  }

  ss.SetSrcBand(kWinSrc, 1);
  ss.SetSrcWindow(0, 0, 100, 100);
  ss.SetDstWindow(50, 50, 100, 100);

  // 2. Window completely misses dst window (left, right, above, below).
  EXPECT_FALSE(ss.GetSrcDstWindow(0, 50, 40, 40, 40, 40, &dfReqXOff, &dfReqYOff,
                                  &dfReqXSize, &dfReqYSize, &nReqXOff,
                                  &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
                                  &nOutYOff, &nOutXSize, &nOutYSize, bError));
  EXPECT_FALSE(ss.GetSrcDstWindow(
      200, 50, 40, 40, 40, 40, &dfReqXOff, &dfReqYOff, &dfReqXSize, &dfReqYSize,
      &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff, &nOutYOff,
      &nOutXSize, &nOutYSize, bError));
  EXPECT_FALSE(ss.GetSrcDstWindow(50, 0, 40, 40, 40, 40, &dfReqXOff, &dfReqYOff,
                                  &dfReqXSize, &dfReqYSize, &nReqXOff,
                                  &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
                                  &nOutYOff, &nOutXSize, &nOutYSize, bError));
  EXPECT_FALSE(ss.GetSrcDstWindow(
      50, 200, 40, 40, 40, 40, &dfReqXOff, &dfReqYOff, &dfReqXSize, &dfReqYSize,
      &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff, &nOutYOff,
      &nOutXSize, &nOutYSize, bError));

  // 3. Partial overlap requiring clipping on left & top.
  EXPECT_TRUE(ss.GetSrcDstWindow(30, 30, 50, 50, 50, 50, &dfReqXOff, &dfReqYOff,
                                 &dfReqXSize, &dfReqYSize, &nReqXOff, &nReqYOff,
                                 &nReqXSize, &nReqYSize, &nOutXOff, &nOutYOff,
                                 &nOutXSize, &nOutYSize, bError));
  EXPECT_GT(nOutXOff, 0);
  EXPECT_GT(nOutYOff, 0);

  // 4. Partial overlap requiring clipping on right & bottom.
  EXPECT_TRUE(ss.GetSrcDstWindow(
      120, 120, 50, 50, 50, 50, &dfReqXOff, &dfReqYOff, &dfReqXSize,
      &dfReqYSize, &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
      &nOutYOff, &nOutXSize, &nOutYSize, bError));
  EXPECT_LT(nOutXSize, 50);
  EXPECT_LT(nOutYSize, 50);

  // 5. Source window with negative offsets.
  ss.SetSrcWindow(-10, -10, 100, 100);
  EXPECT_TRUE(ss.GetSrcDstWindow(50, 50, 50, 50, 50, 50, &dfReqXOff, &dfReqYOff,
                                 &dfReqXSize, &dfReqYSize, &nReqXOff, &nReqYOff,
                                 &nReqXSize, &nReqYSize, &nOutXOff, &nOutYOff,
                                 &nOutXSize, &nOutYSize, bError));
  EXPECT_GE(nReqXOff, 0);
  EXPECT_GE(nReqYOff, 0);
}

TEST_F(VrtSourcesTest, SimpleSourceXMLSerializationAndInit) {
  VRTMapSharedResources mapSharedSources;

  // 1. Missing SourceFilename and VRTDataset in XMLInit -> failure.
  {
    CPLXMLTreeCloser oTree(CPLParseXMLString(
        "<SimpleSource><SourceBand>1</SourceBand></SimpleSource>"));
    ASSERT_NE(nullptr, oTree.get());
    VRTSimpleSource ss;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ss.XMLInit(oTree.get(), nullptr, mapSharedSources));
    CPLErrorReset();
  }

  // 2. Full XMLInit with SourceProperties, OpenOptions, SrcRect, DstRect.
  {
    constexpr char kXml[] = R"xml(
<SimpleSource resampling="bilinear" name="test_src">
  <SourceFilename relativeToVRT="1" shared="1">test.tif</SourceFilename>
  <OpenOptions>
    <OOI key="OVERVIEW_LEVEL">0</OOI>
  </OpenOptions>
  <SourceBand>1</SourceBand>
  <SourceProperties RasterXSize="100" RasterYSize="100" DataType="Byte" BlockXSize="16" BlockYSize="16"/>
  <SrcRect xOff="0" yOff="0" xSize="100" ySize="100"/>
  <DstRect xOff="0" yOff="0" xSize="200" ySize="200"/>
</SimpleSource>)xml";
    CPLXMLTreeCloser oTree(CPLParseXMLString(kXml));
    ASSERT_NE(nullptr, oTree.get());
    VRTSimpleSource ss;
    EXPECT_EQ(CE_None, ss.XMLInit(oTree.get(), kTestDir, mapSharedSources));
    EXPECT_EQ("bilinear", ss.GetResampling());

    CPLXMLTreeCloser oOutTree(ss.SerializeToXML(kTestDir));
    ASSERT_NE(nullptr, oOutTree.get());
    std::unique_ptr<char, CplFreeDeleter> pszOut(
        CPLSerializeXMLTree(oOutTree.get()));
    ASSERT_NE(nullptr, pszOut);
    EXPECT_THAT(std::string_view(pszOut.get()), HasSubstr("test.tif"));
    EXPECT_THAT(std::string_view(pszOut.get()), HasSubstr("bilinear"));
  }

  // 3. Embedded VRTDataset in XMLInit.
  {
    constexpr char kXml[] = R"xml(
<SimpleSource>
  <VRTDataset rasterXSize="10" rasterYSize="10">
    <VRTRasterBand dataType="Byte" band="1"/>
  </VRTDataset>
  <SourceBand>1</SourceBand>
  <SrcRect xOff="0" yOff="0" xSize="10" ySize="10"/>
  <DstRect xOff="0" yOff="0" xSize="10" ySize="10"/>
</SimpleSource>)xml";
    CPLXMLTreeCloser oTree(CPLParseXMLString(kXml));
    ASSERT_NE(nullptr, oTree.get());
    VRTSimpleSource ss;
    EXPECT_EQ(CE_None, ss.XMLInit(oTree.get(), nullptr, mapSharedSources));

    CPLXMLTreeCloser oOutTree(ss.SerializeToXML(kTestDir));
    ASSERT_NE(nullptr, oOutTree.get());
    std::unique_ptr<char, CplFreeDeleter> pszOut(
        CPLSerializeXMLTree(oOutTree.get()));
    ASSERT_NE(nullptr, pszOut);
    EXPECT_THAT(std::string_view(pszOut.get()), HasSubstr("VRTDataset"));
  }

  // 4. ParseSrcRectAndDstRect errors.
  {
    VRTSimpleSource ss;
    CPLXMLTreeCloser oTreeEmpty(CPLParseXMLString("<SimpleSource/>"));
    EXPECT_EQ(CE_None, ss.ParseSrcRectAndDstRect(oTreeEmpty.get()));

    CPLXMLTreeCloser oTreeNegSrc(
        CPLParseXMLString("<SimpleSource><SrcRect xOff=\"0\" yOff=\"0\" "
                          "xSize=\"-10\" ySize=\"10\"/></SimpleSource>"));
    {
      CPLErrorReset();
      WithQuietHandler quiet;
      EXPECT_EQ(CE_Failure, ss.ParseSrcRectAndDstRect(oTreeNegSrc.get()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }

    CPLXMLTreeCloser oTreeNegDst(
        CPLParseXMLString("<SimpleSource><DstRect xOff=\"0\" yOff=\"0\" "
                          "xSize=\"10\" ySize=\"-10\"/></SimpleSource>"));
    {
      CPLErrorReset();
      WithQuietHandler quiet;
      EXPECT_EQ(CE_Failure, ss.ParseSrcRectAndDstRect(oTreeNegDst.get()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
    }
  }
}

TEST_F(VrtSourcesTest, SimpleSourceRasterIOAndStatistics) {
  constexpr char kSsRioSrc[] = "/vsimem/vrtsources_test/ss_rio_src.tif";
  {
    GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, poDrv);
    GDALDatasetUniquePtr poSrc(
        poDrv->Create(kSsRioSrc, 20, 20, 1, GDT_Float32, nullptr));
    ASSERT_NE(nullptr, poSrc);
    std::vector<float> afData(400);
    for (int i = 0; i < 400; ++i) afData[i] = static_cast<float>(i);
    ASSERT_EQ(CE_None, poSrc->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 20, 20, afData.data(), 20, 20,
                           GDT_Float32, 4, 80, nullptr));
    EXPECT_EQ(CE_None,
              poSrc->GetRasterBand(1)->SetStatistics(0.0, 399.0, 199.5, 50.0));
    EXPECT_EQ(CE_None, poSrc->GetRasterBand(1)->SetMetadataItem(
                           "NBITS", "12", "IMAGE_STRUCTURE"));
  }

  VRTSimpleSource ss;
  ss.SetSrcBand(kSsRioSrc, 1);
  ss.SetSrcWindow(0, 0, 20, 20);
  ss.SetDstWindow(0, 0, 20, 20);
  ss.SetMaxValue(100);

  VRTSource::WorkingState oWorkingState;

  // 1. Lossy conversion with MaxValue clamp and progress callback.
  std::vector<GByte> abyOut(400, 0);
  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);
  int nProgCalls = 0;
  sExtraArg.pfnProgress = [](double, const char*, void* pData) -> int {
    (*static_cast<int*>(pData))++;
    return 1;
  };
  sExtraArg.pProgressData = &nProgCalls;

  EXPECT_EQ(CE_None, ss.RasterIO(GDT_Byte, 0, 0, 20, 20, abyOut.data(), 20, 20,
                                 GDT_Byte, 1, 20, &sExtraArg, oWorkingState));
  EXPECT_GT(nProgCalls, 0);
  EXPECT_EQ(0, abyOut[0]);
  EXPECT_LE(abyOut[399], 100);

  // 2. Same-type conversion.
  std::vector<float> afOut(400, 0.0f);
  EXPECT_EQ(CE_None,
            ss.RasterIO(GDT_Float32, 0, 0, 20, 20, afOut.data(), 20, 20,
                        GDT_Float32, 4, 80, nullptr, oWorkingState));
  EXPECT_FLOAT_EQ(0.0f, afOut[0]);

  // 3. GetMinimum, GetMaximum, GetHistogram.
  int bSuccess = FALSE;
  EXPECT_DOUBLE_EQ(0.0, ss.GetMinimum(20, 20, &bSuccess));
  EXPECT_TRUE(bSuccess);

  EXPECT_DOUBLE_EQ(100.0, ss.GetMaximum(20, 20, &bSuccess));
  EXPECT_TRUE(bSuccess);

  GUIntBig anHistogram[256] = {};
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, ss.GetHistogram(20, 20, -0.5, 255.5, 256, anHistogram,
                                          FALSE, TRUE, nullptr, nullptr));
    CPLErrorReset();
  }

  ss.SetMaxValue(0);
  EXPECT_EQ(CE_None, ss.GetHistogram(20, 20, -0.5, 255.5, 256, anHistogram,
                                     FALSE, TRUE, nullptr, nullptr));

  // 4. DatasetRasterIO.
  int anBandMap[1] = {1};
  std::vector<GByte> abyDSOut(400, 0);
  EXPECT_EQ(CE_None,
            ss.DatasetRasterIO(GDT_Byte, 0, 0, 20, 20, abyDSOut.data(), 20, 20,
                               GDT_Byte, 1, anBandMap, 1, 20, 400, nullptr));

  VRTComplexSource cs;
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, cs.DatasetRasterIO(
                              GDT_Byte, 0, 0, 20, 20, abyDSOut.data(), 20, 20,
                              GDT_Byte, 1, anBandMap, 1, 20, 400, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(VrtSourcesTest, AveragedSourceRasterIOAndStatistics) {
  constexpr char kAvgSrc[] = "/vsimem/vrtsources_test/avg_src.tif";
  {
    GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, poDrv);
    GDALDatasetUniquePtr poSrc(
        poDrv->Create(kAvgSrc, 20, 20, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrc);
    std::vector<GByte> abyData(400, 100);
    abyData[0] = 10;
    abyData[1] = 20;
    abyData[20] = 30;
    abyData[21] = 40;
    ASSERT_EQ(CE_None, poSrc->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 20, 20, abyData.data(), 20, 20,
                           GDT_Byte, 1, 20, nullptr));
  }

  VRTAveragedSource as;
  EXPECT_STREQ("AveragedSource", VRTAveragedSource::GetTypeStatic());
  EXPECT_STREQ("AveragedSource", as.GetType());

  as.SetSrcBand(kAvgSrc, 1);
  as.SetSrcWindow(0, 0, 20, 20);
  as.SetDstWindow(0, 0, 10, 10);

  VRTSource::WorkingState oWorkingState;

  // 1. Downsampling to 10x10 Byte buffer.
  std::vector<GByte> abyOut(100, 0);
  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);
  int nProgCalls = 0;
  sExtraArg.pfnProgress = [](double, const char*, void* pData) -> int {
    (*static_cast<int*>(pData))++;
    return 1;
  };
  sExtraArg.pProgressData = &nProgCalls;

  EXPECT_EQ(CE_None, as.RasterIO(GDT_Byte, 0, 0, 10, 10, abyOut.data(), 10, 10,
                                 GDT_Byte, 1, 10, &sExtraArg, oWorkingState));
  EXPECT_GT(nProgCalls, 0);
  EXPECT_EQ(25, abyOut[0]);

  // 2. Downsampling to Float32 buffer.
  std::vector<float> afOut(100, 0.0f);
  EXPECT_EQ(CE_None, as.RasterIO(GDT_Byte, 0, 0, 10, 10, afOut.data(), 10, 10,
                                 GDT_Float32, 4, 40, nullptr, oWorkingState));
  EXPECT_FLOAT_EQ(25.0f, afOut[0]);

  // 3. Upsampling.
  std::vector<GByte> abyUpOut(1600, 0);
  EXPECT_EQ(CE_None, as.RasterIO(GDT_Byte, 0, 0, 10, 10, abyUpOut.data(), 40,
                                 40, GDT_Byte, 1, 40, nullptr, oWorkingState));

  // 4. With NoDataValue set.
  as.SetNoDataValue(20.0);
  EXPECT_EQ(CE_None, as.RasterIO(GDT_Byte, 0, 0, 10, 10, abyOut.data(), 10, 10,
                                 GDT_Byte, 1, 10, nullptr, oWorkingState));
  EXPECT_EQ(27, abyOut[0]);

  // 5. SerializeToXML.
  {
    CPLXMLTreeCloser oTree(as.SerializeToXML(kTestDir));
    ASSERT_NE(nullptr, oTree.get());
    EXPECT_STREQ("AveragedSource", oTree->pszValue);
  }

  // 6. GetMinimum, GetMaximum, GetHistogram.
  int bSuccess = FALSE;
  EXPECT_DOUBLE_EQ(0.0, as.GetMinimum(10, 10, &bSuccess));
  EXPECT_FALSE(bSuccess);
  EXPECT_DOUBLE_EQ(0.0, as.GetMaximum(10, 10, &bSuccess));
  EXPECT_FALSE(bSuccess);
  EXPECT_EQ(CE_Failure, as.GetHistogram(10, 10, 0, 255, 256, nullptr, FALSE,
                                        TRUE, nullptr, nullptr));
}

TEST_F(VrtSourcesTest, NoDataFromMaskSourceRasterIOAndXML) {
  constexpr char kNdmSrc[] = "/vsimem/vrtsources_test/ndm_src.tif";
  {
    GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, poDrv);
    GDALDatasetUniquePtr poSrc(
        poDrv->Create(kNdmSrc, 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrc);
    std::vector<GByte> abyData(100, 50);
    ASSERT_EQ(CE_None, poSrc->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 10, 10, abyData.data(), 10, 10,
                           GDT_Byte, 1, 10, nullptr));
    EXPECT_EQ(CE_None, poSrc->CreateMaskBand(GMF_PER_DATASET));
    std::vector<GByte> abyMask(100, 255);
    std::fill_n(abyMask.begin(), 50, static_cast<GByte>(0));
    ASSERT_EQ(CE_None, poSrc->GetRasterBand(1)->GetMaskBand()->RasterIO(
                           GF_Write, 0, 0, 10, 10, abyMask.data(), 10, 10,
                           GDT_Byte, 1, 10, nullptr));
  }

  VRTNoDataFromMaskSource ndm;
  EXPECT_STREQ("NoDataFromMaskSource",
               VRTNoDataFromMaskSource::GetTypeStatic());
  EXPECT_STREQ("NoDataFromMaskSource", ndm.GetType());

  ndm.SetSrcBand(kNdmSrc, 1);
  ndm.SetSrcWindow(0, 0, 10, 10);
  ndm.SetDstWindow(0, 0, 10, 10);

  VRTSource::WorkingState oWorkingState;

  // 1. Without nodata set -> forwards to VRTSimpleSource::RasterIO.
  std::vector<GByte> abyOut(100, 0);
  EXPECT_EQ(CE_None, ndm.RasterIO(GDT_Byte, 0, 0, 10, 10, abyOut.data(), 10, 10,
                                  GDT_Byte, 1, 10, nullptr, oWorkingState));
  EXPECT_EQ(50, abyOut[0]);

  // 2. With parameters set: mask <= 0 remapped to 0.
  ndm.SetParameters(/*dfNoDataValue=*/0.0, /*dfMaskValueThreshold=*/0.0,
                    /*dfRemappedValue=*/0.0);

  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);
  int nProgCalls = 0;
  sExtraArg.pfnProgress = [](double, const char*, void* pData) -> int {
    (*static_cast<int*>(pData))++;
    return 1;
  };
  sExtraArg.pProgressData = &nProgCalls;

  EXPECT_EQ(CE_None, ndm.RasterIO(GDT_Byte, 0, 0, 10, 10, abyOut.data(), 10, 10,
                                  GDT_Byte, 1, 10, &sExtraArg, oWorkingState));
  EXPECT_GT(nProgCalls, 0);
  EXPECT_EQ(0, abyOut[0]);
  EXPECT_EQ(50, abyOut[99]);

  // 3. SerializeToXML and XMLInit.
  {
    CPLXMLTreeCloser oTree(ndm.SerializeToXML(kTestDir));
    ASSERT_NE(nullptr, oTree.get());
    EXPECT_STREQ("NoDataFromMaskSource", oTree->pszValue);

    VRTMapSharedResources mapSharedSources;
    VRTNoDataFromMaskSource ndm2;
    EXPECT_EQ(CE_None, ndm2.XMLInit(oTree.get(), nullptr, mapSharedSources));
  }

  // 4. GetMinimum, GetMaximum, GetHistogram.
  int bSuccess = FALSE;
  EXPECT_DOUBLE_EQ(0.0, ndm.GetMinimum(10, 10, &bSuccess));
  EXPECT_FALSE(bSuccess);
  EXPECT_DOUBLE_EQ(0.0, ndm.GetMaximum(10, 10, &bSuccess));
  EXPECT_FALSE(bSuccess);
  EXPECT_EQ(CE_Failure, ndm.GetHistogram(10, 10, 0, 255, 256, nullptr, FALSE,
                                         TRUE, nullptr, nullptr));
}

TEST_F(VrtSourcesTest, ComplexSourcePropertiesLUTAndXML) {
  TestComplexSource cs;
  EXPECT_STREQ("ComplexSource", VRTComplexSource::GetTypeStatic());
  EXPECT_STREQ("ComplexSource", cs.GetType());

  // 1. AreValuesUnchanged and scaling/color table flags.
  EXPECT_TRUE(cs.AreValuesUnchanged());
  cs.SetLinearScaling(10.0, 2.0);
  EXPECT_FALSE(cs.AreValuesUnchanged());
  cs.SetPowerScaling(2.0, 0.0, 100.0, 0.0, 255.0, /*bClip=*/true);
  EXPECT_FALSE(cs.AreValuesUnchanged());
  cs.SetColorTableComponent(1);
  EXPECT_FALSE(cs.AreValuesUnchanged());

  // 2. SetNoDataValue.
  cs.SetNoDataValue(99.0);
  EXPECT_DOUBLE_EQ(99.0, cs.GetAdjustedNoDataValue());
  cs.SetNoDataValue(VRT_NODATA_UNSET);
  EXPECT_DOUBLE_EQ(VRT_NODATA_UNSET, cs.GetAdjustedNoDataValue());

  // 3. LookupValue tests.
  cs.m_adfLUTInputs = {0.0, 10.0, 20.0};
  cs.m_adfLUTOutputs = {0.0, 50.0, 100.0};
  EXPECT_DOUBLE_EQ(0.0, cs.LookupValue(-5.0));
  EXPECT_DOUBLE_EQ(0.0, cs.LookupValue(0.0));
  EXPECT_DOUBLE_EQ(25.0, cs.LookupValue(5.0));
  EXPECT_DOUBLE_EQ(50.0, cs.LookupValue(10.0));
  EXPECT_DOUBLE_EQ(100.0, cs.LookupValue(30.0));

  cs.m_adfLUTInputs = {std::numeric_limits<double>::quiet_NaN(), 0.0, 10.0};
  cs.m_adfLUTOutputs = {-999.0, 0.0, 100.0};
  EXPECT_DOUBLE_EQ(-999.0,
                   cs.LookupValue(std::numeric_limits<double>::quiet_NaN()));

  // 4. SerializeToXML with power scaling and LUT.
  cs.SetNoDataValue(42.0);
  cs.SetUseMaskBand(true);
  {
    CPLXMLTreeCloser oTree(cs.SerializeToXML(kTestDir));
    ASSERT_NE(nullptr, oTree.get());
    EXPECT_STREQ("ComplexSource", oTree->pszValue);
  }

  // 5. XMLInit valid and invalid LUTs.
  VRTMapSharedResources mapSharedSources;
  {
    constexpr char kXml[] = R"xml(
<ComplexSource>
  <SourceFilename relativeToVRT="0">foo.tif</SourceFilename>
  <SourceBand>1</SourceBand>
  <ScaleOffset>10</ScaleOffset>
  <ScaleRatio>2</ScaleRatio>
  <NODATA>42</NODATA>
  <UseMaskBand>true</UseMaskBand>
  <ColorTableComponent>2</ColorTableComponent>
  <LUT>0:0,10:50,20:100</LUT>
</ComplexSource>)xml";
    CPLXMLTreeCloser oValidTree(CPLParseXMLString(kXml));
    ASSERT_NE(nullptr, oValidTree.get());
    VRTComplexSource cs2;
    EXPECT_EQ(CE_None,
              cs2.XMLInit(oValidTree.get(), nullptr, mapSharedSources));
  }

  constexpr std::string_view kBadLutXmls[] = {
      "<ComplexSource>"
      "  <SourceFilename relativeToVRT=\"0\">foo.tif</SourceFilename>"
      "  <LUT>0:0,nan:50,20:100</LUT>"
      "</ComplexSource>",
      "<ComplexSource>"
      "  <SourceFilename relativeToVRT=\"0\">foo.tif</SourceFilename>"
      "  <LUT>10:0,5:50,20:100</LUT>"
      "</ComplexSource>",
  };
  for (std::string_view xml : kBadLutXmls) {
    const std::string xml_str(xml);
    CPLXMLTreeCloser oTree(CPLParseXMLString(xml_str.c_str()));
    ASSERT_NE(nullptr, oTree.get()) << xml;
    VRTComplexSource csBad;
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, csBad.XMLInit(oTree.get(), nullptr, mapSharedSources))
        << xml;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType()) << xml;
    CPLErrorReset();
  }
}

TEST_F(VrtSourcesTest, ComplexSourceRasterIOVariants) {
  GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poDrv);

  constexpr char kCsRioSrc[] = "/vsimem/vrtsources_test/cs_rio_src.tif";
  {
    GDALDatasetUniquePtr poSrc(
        poDrv->Create(kCsRioSrc, 20, 20, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrc);
    std::vector<GByte> abyData(400);
    for (int i = 0; i < 400; ++i) abyData[i] = static_cast<GByte>(i % 50);
    ASSERT_EQ(CE_None, poSrc->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 20, 20, abyData.data(), 20, 20,
                           GDT_Byte, 1, 20, nullptr));
  }

  VRTSource::WorkingState oWorkingState;

  // 1. Only NODATA processing flag.
  {
    VRTComplexSource cs;
    cs.SetSrcBand(kCsRioSrc, 1);
    cs.SetSrcWindow(0, 0, 20, 20);
    cs.SetDstWindow(0, 0, 20, 20);
    cs.SetNoDataValue(0.0);

    std::vector<GByte> abyOut(400, 255);
    EXPECT_EQ(CE_None,
              cs.RasterIO(GDT_Byte, 0, 0, 20, 20, abyOut.data(), 20, 20,
                          GDT_Byte, 1, 20, nullptr, oWorkingState));
    EXPECT_EQ(255, abyOut[0]);
    EXPECT_EQ(1, abyOut[1]);
  }

  // 2. Linear scaling (val * 2 + 10).
  {
    VRTComplexSource cs;
    cs.SetSrcBand(kCsRioSrc, 1);
    cs.SetSrcWindow(0, 0, 20, 20);
    cs.SetDstWindow(0, 0, 20, 20);
    cs.SetLinearScaling(10.0, 2.0);

    std::vector<float> afOut(400, 0.0f);
    EXPECT_EQ(CE_None, cs.RasterIO(GDT_Byte, 0, 0, 20, 20, afOut.data(), 20, 20,
                                   GDT_Float32, 4, 80, nullptr, oWorkingState));
    EXPECT_FLOAT_EQ(10.0f, afOut[0]);
    EXPECT_FLOAT_EQ(12.0f, afOut[1]);
  }

  // 3. Constant value optimization (scaleRatio == 0 && !bNoDataSet).
  {
    VRTComplexSource cs;
    cs.SetSrcBand(kCsRioSrc, 1);
    cs.SetSrcWindow(0, 0, 20, 20);
    cs.SetDstWindow(0, 0, 20, 20);
    cs.SetLinearScaling(255.0, 0.0);

    std::vector<GByte> abyOut(400, 0);
    EXPECT_EQ(CE_None,
              cs.RasterIO(GDT_Byte, 0, 0, 20, 20, abyOut.data(), 20, 20,
                          GDT_Byte, 1, 20, nullptr, oWorkingState));
    EXPECT_EQ(255, abyOut[0]);
    EXPECT_EQ(255, abyOut[399]);
  }

  // 4. Power scaling.
  {
    VRTComplexSource cs;
    cs.SetSrcBand(kCsRioSrc, 1);
    cs.SetSrcWindow(0, 0, 20, 20);
    cs.SetDstWindow(0, 0, 20, 20);
    cs.SetPowerScaling(2.0, 0.0, 50.0, 0.0, 100.0, /*bClip=*/true);

    std::vector<float> afOut(400, 0.0f);
    EXPECT_EQ(CE_None, cs.RasterIO(GDT_Byte, 0, 0, 20, 20, afOut.data(), 20, 20,
                                   GDT_Float32, 4, 80, nullptr, oWorkingState));
    EXPECT_FLOAT_EQ(0.0f, afOut[0]);
  }

  // 5. LUT transformation.
  {
    VRTMapSharedResources mapSharedSources;
    constexpr char kXml[] = R"xml(
<ComplexSource>
  <SourceFilename relativeToVRT="0">/vsimem/vrtsources_test/cs_rio_src.tif</SourceFilename>
  <SourceBand>1</SourceBand>
  <SrcRect xOff="0" yOff="0" xSize="20" ySize="20"/>
  <DstRect xOff="0" yOff="0" xSize="20" ySize="20"/>
  <LUT>0:0,25:50,50:100</LUT>
</ComplexSource>)xml";
    CPLXMLTreeCloser oTree(CPLParseXMLString(kXml));
    ASSERT_NE(nullptr, oTree.get());
    VRTComplexSource cs;
    EXPECT_EQ(CE_None, cs.XMLInit(oTree.get(), nullptr, mapSharedSources));

    std::vector<float> afOut(400, 0.0f);
    EXPECT_EQ(CE_None, cs.RasterIO(GDT_Byte, 0, 0, 20, 20, afOut.data(), 20, 20,
                                   GDT_Float32, 4, 80, nullptr, oWorkingState));
    EXPECT_FLOAT_EQ(0.0f, afOut[0]);
  }

  // 6. Color table component expansion.
  {
    constexpr char kCsCtSrc[] = "/vsimem/vrtsources_test/cs_ct_src.tif";
    {
      GDALDatasetUniquePtr poCTDS(
          poDrv->Create(kCsCtSrc, 10, 10, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, poCTDS);
      GDALColorTable oCT;
      GDALColorEntry sEntry = {10, 20, 30, 255};
      oCT.SetColorEntry(1, &sEntry);
      EXPECT_EQ(CE_None, poCTDS->GetRasterBand(1)->SetColorTable(&oCT));
      std::vector<GByte> abyCTData(100, 1);
      ASSERT_EQ(CE_None, poCTDS->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 10, 10, abyCTData.data(), 10, 10,
                             GDT_Byte, 1, 10, nullptr));
    }

    VRTComplexSource cs;
    cs.SetSrcBand(kCsCtSrc, 1);
    cs.SetSrcWindow(0, 0, 10, 10);
    cs.SetDstWindow(0, 0, 10, 10);
    cs.SetColorTableComponent(2);

    std::vector<GByte> abyOut(100, 0);
    EXPECT_EQ(CE_None,
              cs.RasterIO(GDT_Byte, 0, 0, 10, 10, abyOut.data(), 10, 10,
                          GDT_Byte, 1, 10, nullptr, oWorkingState));
    EXPECT_EQ(20, abyOut[0]);
  }

  // 7. Complex data types (GDT_CFloat32).
  {
    constexpr char kCsCfloatSrc[] = "/vsimem/vrtsources_test/cs_cfloat_src.tif";
    {
      GDALDatasetUniquePtr poCDataset(
          poDrv->Create(kCsCfloatSrc, 10, 10, 1, GDT_CFloat32, nullptr));
      ASSERT_NE(nullptr, poCDataset);
      std::vector<std::complex<float>> acData(100, {10.0f, 20.0f});
      ASSERT_EQ(CE_None, poCDataset->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 10, 10, acData.data(), 10, 10,
                             GDT_CFloat32, 8, 80, nullptr));
    }

    VRTComplexSource cs;
    cs.SetSrcBand(kCsCfloatSrc, 1);
    cs.SetSrcWindow(0, 0, 10, 10);
    cs.SetDstWindow(0, 0, 10, 10);
    cs.SetLinearScaling(5.0, 2.0);

    std::vector<std::complex<float>> acOut(100);
    EXPECT_EQ(CE_None,
              cs.RasterIO(GDT_CFloat32, 0, 0, 10, 10, acOut.data(), 10, 10,
                          GDT_CFloat32, 8, 80, nullptr, oWorkingState));
    EXPECT_FLOAT_EQ(25.0f, acOut[0].real());
    EXPECT_FLOAT_EQ(45.0f, acOut[0].imag());
  }

  // 8. Statistics forwarding when unchanged vs when changed.
  {
    constexpr char kStatsSrc[] = "/vsimem/vrtsources_test/cs_stats_src.tif";
    {
      GDALDatasetUniquePtr poStatsDS(
          poDrv->Create(kStatsSrc, 20, 20, 1, GDT_Float32, nullptr));
      ASSERT_NE(nullptr, poStatsDS);
      std::vector<float> afData(400);
      for (int i = 0; i < 400; ++i) afData[i] = static_cast<float>(i);
      GDALRasterBand* poBand = poStatsDS->GetRasterBand(1);
      ASSERT_NE(nullptr, poBand);
      ASSERT_EQ(CE_None, poBand->RasterIO(GF_Write, 0, 0, 20, 20, afData.data(),
                                          20, 20, GDT_Float32, 4, 80, nullptr));
      EXPECT_EQ(CE_None, poBand->SetStatistics(0.0, 399.0, 199.5, 115.47));
    }

    VRTComplexSource csUnchanged;
    csUnchanged.SetSrcBand(kStatsSrc, 1);
    csUnchanged.SetSrcWindow(0, 0, 20, 20);
    csUnchanged.SetDstWindow(0, 0, 20, 20);
    int bSuccess = FALSE;
    EXPECT_DOUBLE_EQ(0.0, csUnchanged.GetMinimum(20, 20, &bSuccess));
    EXPECT_TRUE(bSuccess);
    bSuccess = FALSE;
    EXPECT_DOUBLE_EQ(399.0, csUnchanged.GetMaximum(20, 20, &bSuccess));
    EXPECT_TRUE(bSuccess);
    std::vector<GUIntBig> histogram(10, 0);
    EXPECT_EQ(CE_None,
              csUnchanged.GetHistogram(20, 20, 0.0, 400.0, 10, histogram.data(),
                                       FALSE, TRUE, nullptr, nullptr));

    VRTComplexSource csChanged;
    csChanged.SetSrcBand(kStatsSrc, 1);
    csChanged.SetSrcWindow(0, 0, 20, 20);
    csChanged.SetDstWindow(0, 0, 20, 20);
    csChanged.SetLinearScaling(10.0, 2.0);
    bSuccess = TRUE;
    EXPECT_DOUBLE_EQ(0.0, csChanged.GetMinimum(20, 20, &bSuccess));
    EXPECT_FALSE(bSuccess);
    bSuccess = TRUE;
    EXPECT_DOUBLE_EQ(0.0, csChanged.GetMaximum(20, 20, &bSuccess));
    EXPECT_FALSE(bSuccess);
    EXPECT_EQ(CE_Failure,
              csChanged.GetHistogram(20, 20, 0.0, 400.0, 10, histogram.data(),
                                     FALSE, TRUE, nullptr, nullptr));
  }
}

TEST_F(VrtSourcesTest, FuncSourceRasterIOAndStats) {
  VRTFuncSource fs;
  EXPECT_STREQ("FuncSource", fs.GetType());
  EXPECT_EQ(nullptr, fs.SerializeToXML(nullptr));

  fs.pfnReadFunc = MyFuncSourceRead;
  fs.eType = GDT_Byte;

  VRTSource::WorkingState oWorkingState;

  // 1. Regular matching request.
  std::vector<GByte> abyData(100, 0);
  EXPECT_EQ(CE_None, fs.RasterIO(GDT_Byte, 0, 0, 10, 10, abyData.data(), 10, 10,
                                 GDT_Byte, 1, 10, nullptr, oWorkingState));
  EXPECT_EQ(77, abyData[0]);
  EXPECT_EQ(77, abyData[99]);

  // 2. Irregular request -> CE_Failure.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure,
              fs.RasterIO(GDT_Byte, 0, 0, 10, 10, abyData.data(), 5, 5,
                          GDT_Byte, 1, 5, nullptr, oWorkingState));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 3. Minimum, Maximum, Histogram.
  int bSuccess = FALSE;
  EXPECT_DOUBLE_EQ(0.0, fs.GetMinimum(10, 10, &bSuccess));
  EXPECT_FALSE(bSuccess);
  EXPECT_DOUBLE_EQ(0.0, fs.GetMaximum(10, 10, &bSuccess));
  EXPECT_FALSE(bSuccess);
  EXPECT_EQ(CE_Failure, fs.GetHistogram(10, 10, 0, 255, 256, nullptr, FALSE,
                                        TRUE, nullptr, nullptr));
}

TEST_F(VrtSourcesTest, SimpleSourceSetBandMaskAndSpecialSubdataset) {
  GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poDrv);

  constexpr char kSubdsTest[] = "/vsimem/vrtsources_test/vrt_subds_test.tif";
  GDALDatasetUniquePtr poDS(
      poDrv->Create(kSubdsTest, 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, poDS);
  EXPECT_EQ(CE_None, poDS->CreateMaskBand(GMF_PER_DATASET));
  GDALRasterBand* poBand = poDS->GetRasterBand(1);
  ASSERT_NE(nullptr, poBand);

  // 1. SetSrcBand, SetSourceDatasetName, SetSrcMaskBand.
  TestSimpleSource ss;
  ss.m_bDropRefOnSrcBand = false;
  ss.SetSrcBand(poBand);
  EXPECT_EQ(poBand, ss.GetRasterBand());

  ss.SetSourceDatasetName("custom_name.tif", true);
  ss.SetSrcMaskBand(poBand);
  EXPECT_NE(nullptr, ss.GetMaskBandMainBand());

  // 2. Nested VRTDataset in SerializeToXML.
  {
    GDALDriver* poVRTDriver = GetGDALDriverManager()->GetDriverByName("VRT");
    ASSERT_NE(nullptr, poVRTDriver);
    GDALDatasetUniquePtr poVRT(poVRTDriver->Create(
        "/vsimem/vrtsources_test/nested.vrt", 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poVRT);
    TestSimpleSource ssNested;
    ssNested.m_bDropRefOnSrcBand = false;
    ssNested.SetSrcBand(poVRT->GetRasterBand(1));
    ssNested.m_osSrcDSName.clear();
    CPLXMLTreeCloser oXML(ssNested.SerializeToXML(kTestDir));
    EXPECT_NE(nullptr, oXML.get());
  }

  // 3. AddSourceFilenameNode with special syntax (NITF_IM, RASTERLITE, TILEDB).
  {
    CPLXMLTreeCloser oRoot(
        CPLCreateXMLNode(nullptr, CXT_Element, "SimpleSource"));
    ASSERT_NE(nullptr, oRoot.get());

    constexpr std::string_view kSpecialNames[] = {
        "NITF_IM:0:/vsimem/vrtsources_test/nonexistent.ntf",
        "NITF_IM:0:C:/nonexistent.ntf",
        "RASTERLITE:/vsimem/vrtsources_test/nonexistent.sqlite,table=layer",
        "RASTERLITE:C:/nonexistent.sqlite,table=layer",
        "TILEDB:\"/vsimem/vrtsources_test/nonexistent\":layer",
        "NETCDF:\"/vsimem/vrtsources_test/nonexistent.nc\":var",
        "/vsicurl/http://example.com/test.tif",
    };
    for (std::string_view name : kSpecialNames) {
      TestSimpleSource ssSpecial;
      ssSpecial.m_bRelativeToVRTOri = -1;
      ssSpecial.m_osSrcDSName = std::string(name);
      ssSpecial.AddSourceFilenameNode(kTestDir, oRoot.get());
    }

    constexpr char kExistTif[] = "/vsimem/vrtsources_test/vrt_exist.tif";
    GDALDatasetUniquePtr poExist(
        poDrv->Create(kExistTif, 1, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poExist);

    TestSimpleSource ssRelSrc;
    ssRelSrc.m_bRelativeToVRTOri = -1;
    ssRelSrc.m_osSrcDSName = "vrt_exist.tif";
    ssRelSrc.AddSourceFilenameNode("/vsimem/vrtsources_test/dir", oRoot.get());

    TestSimpleSource ssAbsSrc;
    ssAbsSrc.m_bRelativeToVRTOri = -1;
    ssAbsSrc.m_osSrcDSName = kExistTif;
    ssAbsSrc.AddSourceFilenameNode("relative_dir", oRoot.get());

    TestSimpleSource ssRel;
    ssRel.SetSourceDatasetName("/vsimem/vrtsources_test/fake.tif", true);
    ssRel.AddSourceFilenameNode(kTestDir, oRoot.get());
  }
}

TEST_F(VrtSourcesTest, SimpleSourceWindowClampingAndFloatingPoint) {
  GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poDrv);

  constexpr char kClampSrc[] = "/vsimem/vrtsources_test/vrt_clamp_src.tif";
  GDALDatasetUniquePtr poDS(
      poDrv->Create(kClampSrc, 20, 20, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, poDS);

  std::vector<GByte> abyInit(20 * 20, 120);
  ASSERT_EQ(CE_None, poDS->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 20, 20,
                                                      abyInit.data(), 20, 20,
                                                      GDT_Byte, 0, 0));
  ASSERT_EQ(CE_None, poDS->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 20, 20,
                                                      abyInit.data(), 20, 20,
                                                      GDT_Byte, 0, 0));

  TestSimpleSource ss;
  ss.m_bDropRefOnSrcBand = false;
  ss.SetSrcBand(poDS->GetRasterBand(1));
  ss.SetSrcWindow(0, 0, 20, 20);
  ss.SetDstWindow(0, 0, 20, 20);

  double dfReqXOff = 0.0;
  double dfReqYOff = 0.0;
  double dfReqXSize = 0.0;
  double dfReqYSize = 0.0;
  int nReqXOff = 0;
  int nReqYOff = 0;
  int nReqXSize = 0;
  int nReqYSize = 0;
  int nOutXOff = 0;
  int nOutYOff = 0;
  int nOutXSize = 0;
  int nOutYSize = 0;
  bool bError = false;

  // 1. ONE_MINUS_EPS trigger.
  EXPECT_TRUE(ss.GetSrcDstWindow(
      0.99999999999, 0.99999999999, 10.0, 10.0, 10, 10, &dfReqXOff, &dfReqYOff,
      &dfReqXSize, &dfReqYSize, &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize,
      &nOutXOff, &nOutYOff, &nOutXSize, &nOutYSize, bError));

  // 2. Large window (> INT_MAX).
  EXPECT_FALSE(ss.GetSrcDstWindow(
      0.0, 0.0, 1e12, 1e12, 10, 10, &dfReqXOff, &dfReqYOff, &dfReqXSize,
      &dfReqYSize, &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
      &nOutYOff, &nOutXSize, &nOutYSize, bError));

  // 3. Small window width/height -> zero size clamped to 1.
  EXPECT_TRUE(ss.GetSrcDstWindow(
      0.0, 0.0, 0.1, 0.1, 10, 10, &dfReqXOff, &dfReqYOff, &dfReqXSize,
      &dfReqYSize, &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
      &nOutYOff, &nOutXSize, &nOutYSize, bError));

  // 4. Overhang right and bottom edges.
  EXPECT_TRUE(ss.GetSrcDstWindow(
      15.0, 15.0, 10.0, 10.0, 10, 10, &dfReqXOff, &dfReqYOff, &dfReqXSize,
      &dfReqYSize, &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
      &nOutYOff, &nOutXSize, &nOutYSize, bError));

  // 5. Completely outside -> returns FALSE.
  EXPECT_FALSE(ss.GetSrcDstWindow(
      50.0, 50.0, 10.0, 10.0, 10, 10, &dfReqXOff, &dfReqYOff, &dfReqXSize,
      &dfReqYSize, &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
      &nOutYOff, &nOutXSize, &nOutYSize, bError));

  // 6. Overhang with modified bounds.
  EXPECT_TRUE(ss.GetSrcDstWindow(
      0.0, 0.0, 30.0, 30.0, 5, 5, &dfReqXOff, &dfReqYOff, &dfReqXSize,
      &dfReqYSize, &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
      &nOutYOff, &nOutXSize, &nOutYSize, bError));

  // 7. RasterIO with bFloatingPointWindowValidity.
  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);
  sExtraArg.bFloatingPointWindowValidity = TRUE;
  sExtraArg.dfXOff = 0.5;
  sExtraArg.dfYOff = 0.5;
  sExtraArg.dfXSize = 5.0;
  sExtraArg.dfYSize = 5.0;

  VRTSource::WorkingState oWorkingState;
  std::vector<GByte> abyBuf(25, 0);
  EXPECT_EQ(CE_None, ss.RasterIO(GDT_Byte, 0, 0, 5, 5, abyBuf.data(), 5, 5,
                                 GDT_Byte, 1, 5, &sExtraArg, oWorkingState));

  // 8. DatasetRasterIO with bFloatingPointWindowValidity and
  // NeedMaxValAdjustment.
  ss.SetMaxValue(100);
  int panBandMap[2] = {1, 2};
  std::vector<GByte> abyDSBuf(50, 0);
  EXPECT_EQ(CE_None,
            ss.DatasetRasterIO(poDS->GetRasterBand(1)->GetRasterDataType(), 0,
                               0, 5, 5, abyDSBuf.data(), 5, 5, GDT_Byte, 2,
                               panBandMap, 1, 5, 25, &sExtraArg));
  EXPECT_EQ(100, abyDSBuf[0]);

  // 9. VRTAveragedSource with bFloatingPointWindowValidity.
  VRTAveragedSource as;
  as.SetSrcBand(poDS->GetRasterBand(1));
  poDS->Reference();
  as.SetSrcWindow(0, 0, 20, 20);
  as.SetDstWindow(0, 0, 20, 20);
  EXPECT_EQ(CE_None, as.RasterIO(GDT_Byte, 0, 0, 5, 5, abyBuf.data(), 5, 5,
                                 GDT_Byte, 1, 5, &sExtraArg, oWorkingState));
}

TEST_F(VrtSourcesTest, NoDataFromMaskSourceAdvanced) {
  GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poDrv);

  constexpr char kNdAdv[] = "/vsimem/vrtsources_test/vrt_nd_adv.tif";
  GDALDatasetUniquePtr poDS(
      poDrv->Create(kNdAdv, 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, poDS);
  EXPECT_EQ(CE_None, poDS->CreateMaskBand(GMF_PER_DATASET));
  GDALRasterBand* poBand = poDS->GetRasterBand(1);
  ASSERT_NE(nullptr, poBand);

  std::vector<GByte> abyData = {10, 20, 30, 10, 50, 60, 70, 80, 90, 100};
  abyData.resize(100, 10);
  ASSERT_EQ(CE_None, poBand->RasterIO(GF_Write, 0, 0, 10, 10, abyData.data(),
                                      10, 10, GDT_Byte, 0, 0));
  std::vector<GByte> abyMask = {0, 255, 255, 0, 255, 255, 255, 255, 255, 255};
  abyMask.resize(100, 255);
  ASSERT_EQ(CE_None, poBand->GetMaskBand()->RasterIO(GF_Write, 0, 0, 10, 10,
                                                     abyMask.data(), 10, 10,
                                                     GDT_Byte, 0, 0));

  VRTNoDataFromMaskSource nd;
  nd.SetSrcBand(poBand);
  poDS->Reference();
  nd.SetSrcWindow(0, 0, 10, 10);
  nd.SetDstWindow(0, 0, 10, 10);

  // 1. SetParameters 3-arg overload.
  nd.SetParameters(10.0, 0.0, 99.0);

  // 2. RasterIO with bFloatingPointWindowValidity and eBufType = GDT_Float32.
  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);
  sExtraArg.bFloatingPointWindowValidity = TRUE;
  sExtraArg.dfXOff = 0.0;
  sExtraArg.dfYOff = 0.0;
  sExtraArg.dfXSize = 10.0;
  sExtraArg.dfYSize = 10.0;

  VRTSource::WorkingState oWorkingState;
  std::vector<float> afBuf(100, 0.0f);
  EXPECT_EQ(CE_None, nd.RasterIO(GDT_Byte, 0, 0, 10, 10, afBuf.data(), 10, 10,
                                 GDT_Float32, sizeof(float), 10 * sizeof(float),
                                 &sExtraArg, oWorkingState));
  EXPECT_FLOAT_EQ(10.0f, afBuf[0]);
  EXPECT_FLOAT_EQ(20.0f, afBuf[1]);

  // 3. Auto-remapping for Byte without remapped value.
  {
    VRTNoDataFromMaskSource ndByte;
    ndByte.SetSrcBand(poBand);
    poDS->Reference();
    ndByte.SetSrcWindow(0, 0, 10, 10);
    ndByte.SetDstWindow(0, 0, 10, 10);
    ndByte.SetParameters(10.0, 0.0);
    std::vector<GByte> abyOut(100, 0);
    EXPECT_EQ(CE_None,
              ndByte.RasterIO(GDT_Byte, 0, 0, 10, 10, abyOut.data(), 10, 10,
                              GDT_Byte, 1, 10, nullptr, oWorkingState));

    ndByte.SetParameters(255.0, 0.0);
    EXPECT_EQ(CE_None,
              ndByte.RasterIO(GDT_Byte, 0, 0, 10, 10, abyOut.data(), 10, 10,
                              GDT_Byte, 1, 10, nullptr, oWorkingState));
  }

  // 4. UInt16 and Int16 datasets for auto-remapping.
  {
    GDALDatasetUniquePtr poDS16(
        poDrv->Create("/vsimem/vrtsources_test/vrt_nd_u16.tif", 10, 10, 1,
                      GDT_UInt16, nullptr));
    ASSERT_NE(nullptr, poDS16);
    EXPECT_EQ(CE_None, poDS16->CreateMaskBand(GMF_PER_DATASET));
    VRTNoDataFromMaskSource ndU16;
    ndU16.SetSrcBand(poDS16->GetRasterBand(1));
    poDS16->Reference();
    ndU16.SetSrcWindow(0, 0, 10, 10);
    ndU16.SetDstWindow(0, 0, 10, 10);

    ndU16.SetParameters(100.0, 0.0);
    std::vector<uint16_t> anOut(100, 0);
    EXPECT_EQ(CE_None,
              ndU16.RasterIO(GDT_UInt16, 0, 0, 10, 10, anOut.data(), 10, 10,
                             GDT_UInt16, sizeof(uint16_t),
                             10 * sizeof(uint16_t), nullptr, oWorkingState));

    ndU16.SetParameters(65535.0, 0.0);
    EXPECT_EQ(CE_None,
              ndU16.RasterIO(GDT_UInt16, 0, 0, 10, 10, anOut.data(), 10, 10,
                             GDT_UInt16, sizeof(uint16_t),
                             10 * sizeof(uint16_t), nullptr, oWorkingState));
  }
  {
    GDALDatasetUniquePtr poDS16(
        poDrv->Create("/vsimem/vrtsources_test/vrt_nd_s16.tif", 10, 10, 1,
                      GDT_Int16, nullptr));
    ASSERT_NE(nullptr, poDS16);
    EXPECT_EQ(CE_None, poDS16->CreateMaskBand(GMF_PER_DATASET));
    VRTNoDataFromMaskSource ndS16;
    ndS16.SetSrcBand(poDS16->GetRasterBand(1));
    poDS16->Reference();
    ndS16.SetSrcWindow(0, 0, 10, 10);
    ndS16.SetDstWindow(0, 0, 10, 10);

    ndS16.SetParameters(100.0, 0.0);
    std::vector<int16_t> anOut(100, 0);
    EXPECT_EQ(CE_None,
              ndS16.RasterIO(GDT_Int16, 0, 0, 10, 10, anOut.data(), 10, 10,
                             GDT_Int16, sizeof(int16_t), 10 * sizeof(int16_t),
                             nullptr, oWorkingState));

    ndS16.SetParameters(32767.0, 0.0);
    EXPECT_EQ(CE_None,
              ndS16.RasterIO(GDT_Int16, 0, 0, 10, 10, anOut.data(), 10, 10,
                             GDT_Int16, sizeof(int16_t), 10 * sizeof(int16_t),
                             nullptr, oWorkingState));
  }

  // 5. SerializeToXML with float max.
  {
    GDALDatasetUniquePtr poDSF32(
        poDrv->Create("/vsimem/vrtsources_test/vrt_nd_f32.tif", 10, 10, 1,
                      GDT_Float32, nullptr));
    ASSERT_NE(nullptr, poDSF32);
    VRTNoDataFromMaskSource ndF32;
    ndF32.SetSrcBand(poDSF32->GetRasterBand(1));
    poDSF32->Reference();
    ndF32.SetParameters(std::numeric_limits<float>::max(), 0.0);
    CPLXMLTreeCloser oXML(ndF32.SerializeToXML(kTestDir));
    EXPECT_NE(nullptr, oXML.get());
  }

  // 6. Minimum, Maximum, Histogram.
  int bSuccess = FALSE;
  EXPECT_DOUBLE_EQ(0.0, nd.GetMinimum(10, 10, &bSuccess));
  EXPECT_FALSE(bSuccess);
  EXPECT_DOUBLE_EQ(0.0, nd.GetMaximum(10, 10, &bSuccess));
  EXPECT_FALSE(bSuccess);
  EXPECT_EQ(CE_Failure, nd.GetHistogram(10, 10, 0, 255, 256, nullptr, FALSE,
                                        TRUE, nullptr, nullptr));
}

TEST_F(VrtSourcesTest, ComplexSourceConstructorsLUTAndXML) {
  GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poDrv);

  constexpr char kCsXmlTif[] = "/vsimem/vrtsources_test/vrt_cs_xml.tif";
  GDALDatasetUniquePtr poDS(
      poDrv->Create(kCsXmlTif, 10, 10, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, poDS);

  // 1. Scale constructor.
  TestComplexSource cs1;
  cs1.m_bDropRefOnSrcBand = false;
  cs1.SetSrcBand(poDS->GetRasterBand(1));
  cs1.SetSrcWindow(0, 0, 10, 10);
  cs1.SetDstWindow(0, 0, 10, 10);
  cs1.SetLinearScaling(5.0, 2.0);
  cs1.SetPowerScaling(1.5, 0.0, 100.0, 0.0, 255.0);

  TestComplexSource csScale(&cs1, 2.0, 2.0);
  csScale.m_bDropRefOnSrcBand = false;
  EXPECT_STREQ("ComplexSource", csScale.GetType());

  // 2. SerializeToXML variants.
  TestComplexSource csLUT;
  csLUT.m_bDropRefOnSrcBand = false;
  csLUT.SetSrcBand(poDS->GetRasterBand(1));
  csLUT.SetSrcWindow(0, 0, 10, 10);
  csLUT.SetDstWindow(0, 0, 10, 10);
  csLUT.m_adfLUTInputs = {1.0, 1.0, 2.0, 2.0};
  csLUT.m_adfLUTOutputs = {10.0, 20.0, 30.0, 40.0};
  {
    CPLXMLTreeCloser oXMLLUT(csLUT.SerializeToXML(kTestDir));
    EXPECT_NE(nullptr, oXMLLUT.get());
  }

  TestComplexSource csF32;
  csF32.m_bDropRefOnSrcBand = false;
  csF32.SetSrcBand(poDS->GetRasterBand(1));
  csF32.SetSrcWindow(0, 0, 10, 10);
  csF32.SetDstWindow(0, 0, 10, 10);
  csF32.SetNoDataValue(std::numeric_limits<float>::max());
  {
    CPLXMLTreeCloser oXMLF32(csF32.SerializeToXML(kTestDir));
    EXPECT_NE(nullptr, oXMLF32.get());
  }

  // 3. XMLInit branches.
  VRTMapSharedResources mapShared;
  {
    constexpr char kLinearXml[] = R"xml(
<ComplexSource>
  <SourceFilename relativeToVRT="0">/vsimem/vrtsources_test/vrt_cs_xml.tif</SourceFilename>
  <SourceBand>1</SourceBand>
  <ScaleOffset>10</ScaleOffset>
  <ScaleRatio>2.5</ScaleRatio>
</ComplexSource>)xml";
    CPLXMLTreeCloser oTree(CPLParseXMLString(kLinearXml));
    ASSERT_NE(nullptr, oTree.get());
    TestComplexSource csXML;
    EXPECT_EQ(CE_None, csXML.XMLInit(oTree.get(), kTestDir, mapShared));
  }
  {
    constexpr char kExpXml[] = R"xml(
<ComplexSource>
  <SourceFilename relativeToVRT="0">/vsimem/vrtsources_test/vrt_cs_xml.tif</SourceFilename>
  <SourceBand>1</SourceBand>
  <Exponent>2.0</Exponent>
  <SrcMin>0.0</SrcMin>
  <SrcMax>100.0</SrcMax>
  <DstMin>0.0</DstMin>
  <DstMax>255.0</DstMax>
</ComplexSource>)xml";
    CPLXMLTreeCloser oTree(CPLParseXMLString(kExpXml));
    ASSERT_NE(nullptr, oTree.get());
    TestComplexSource csXML;
    EXPECT_EQ(CE_None, csXML.XMLInit(oTree.get(), kTestDir, mapShared));
  }
  {
    constexpr char kUnsortedLutXml[] = R"xml(
<ComplexSource>
  <SourceFilename relativeToVRT="0">/vsimem/vrtsources_test/vrt_cs_xml.tif</SourceFilename>
  <SourceBand>1</SourceBand>
  <LUT>20:100,10:50</LUT>
</ComplexSource>)xml";
    CPLXMLTreeCloser oTree(CPLParseXMLString(kUnsortedLutXml));
    ASSERT_NE(nullptr, oTree.get());
    TestComplexSource csXML;
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, csXML.XMLInit(oTree.get(), kTestDir, mapShared));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  {
    constexpr char kBadNanLutXml[] = R"xml(
<ComplexSource>
  <SourceFilename relativeToVRT="0">/vsimem/vrtsources_test/vrt_cs_xml.tif</SourceFilename>
  <SourceBand>1</SourceBand>
  <LUT>10:100,nan:50</LUT>
</ComplexSource>)xml";
    CPLXMLTreeCloser oTree(CPLParseXMLString(kBadNanLutXml));
    ASSERT_NE(nullptr, oTree.get());
    TestComplexSource csXML;
    CPLErrorReset();
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, csXML.XMLInit(oTree.get(), kTestDir, mapShared));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(VrtSourcesTest, ComplexSourceRasterIOVariantsExtended) {
  GDALDriver* poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poDrv);

  // 1. RasterIOInternalNoProcessing on Byte with word optimization.
  {
    GDALDatasetUniquePtr poDS(
        poDrv->Create("/vsimem/vrtsources_test/vrt_cs_words.tif", 10, 1, 1,
                      GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDS);
    std::vector<GByte> abyData = {1, 2, 3, 4, 0, 0, 0, 0, 5, 0};
    ASSERT_EQ(CE_None, poDS->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 10, 1,
                                                        abyData.data(), 10, 1,
                                                        GDT_Byte, 0, 0));

    TestComplexSource cs;
    cs.m_bDropRefOnSrcBand = false;
    cs.SetSrcBand(poDS->GetRasterBand(1));
    cs.SetSrcWindow(0, 0, 10, 1);
    cs.SetDstWindow(0, 0, 10, 1);
    cs.SetNoDataValue(0.0);

    VRTSource::WorkingState oWorkingState;
    std::vector<GByte> abyOut(10, 99);
    EXPECT_EQ(CE_None, cs.RasterIO(GDT_Byte, 0, 0, 10, 1, abyOut.data(), 10, 1,
                                   GDT_Byte, 1, 10, nullptr, oWorkingState));
    EXPECT_EQ(1, abyOut[0]);
    EXPECT_EQ(4, abyOut[3]);
    EXPECT_EQ(99, abyOut[4]);
    EXPECT_EQ(5, abyOut[8]);
    EXPECT_EQ(99, abyOut[9]);

    // Mixed word [1, 0, 2, 0].
    std::vector<GByte> abyMixed = {1, 0, 2, 0, 1, 0, 2, 0, 1, 0};
    ASSERT_EQ(CE_None, poDS->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 10, 1,
                                                        abyMixed.data(), 10, 1,
                                                        GDT_Byte, 0, 0));
    std::fill(abyOut.begin(), abyOut.end(), 99);
    EXPECT_EQ(CE_None, cs.RasterIO(GDT_Byte, 0, 0, 10, 1, abyOut.data(), 10, 1,
                                   GDT_Byte, 1, 10, nullptr, oWorkingState));
    EXPECT_EQ(1, abyOut[0]);
    EXPECT_EQ(99, abyOut[1]);

    // Direct non-lossy conversion.
    std::vector<int32_t> anOut32(10, 0);
    EXPECT_EQ(CE_None,
              cs.RasterIO(GDT_Int16, 0, 0, 10, 1, anOut32.data(), 10, 1,
                          GDT_Int32, sizeof(int32_t), 10 * sizeof(int32_t),
                          nullptr, oWorkingState));

    // Two-step lossy conversion: source GDT_Int16, VRT GDT_Byte, buf GDT_Int32.
    GDALDatasetUniquePtr poDS16(poDrv->Create(
        "/vsimem/vrtsources_test/vrt_cs_s16.tif", 5, 1, 1, GDT_Int16, nullptr));
    ASSERT_NE(nullptr, poDS16);
    std::vector<int16_t> anData16 = {10, 0, 20, 0, 30};
    ASSERT_EQ(CE_None, poDS16->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 5, 1,
                                                          anData16.data(), 5, 1,
                                                          GDT_Int16, 0, 0));

    TestComplexSource csLossy;
    csLossy.m_bDropRefOnSrcBand = false;
    csLossy.SetSrcBand(poDS16->GetRasterBand(1));
    csLossy.SetSrcWindow(0, 0, 5, 1);
    csLossy.SetDstWindow(0, 0, 5, 1);
    csLossy.SetNoDataValue(0.0);
    std::vector<int32_t> anOutLossy(5, 0);
    EXPECT_EQ(CE_None,
              csLossy.RasterIO(GDT_Byte, 0, 0, 5, 1, anOutLossy.data(), 5, 1,
                               GDT_Int32, sizeof(int32_t), 5 * sizeof(int32_t),
                               nullptr, oWorkingState));
  }

  // 2. RasterIOProcessNoData for Int16 and UInt16 with all destination types.
  {
    GDALDatasetUniquePtr poDS16(
        poDrv->Create("/vsimem/vrtsources_test/vrt_cs_types.tif", 4, 1, 1,
                      GDT_Int16, nullptr));
    ASSERT_NE(nullptr, poDS16);
    std::vector<int16_t> anData = {1, -999, 3, 4};
    ASSERT_EQ(CE_None,
              poDS16->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, 0, 4, 1, anData.data(), 4, 1, GDT_Int16, 0, 0));

    TestComplexSource cs16;
    cs16.m_bDropRefOnSrcBand = false;
    cs16.SetSrcBand(poDS16->GetRasterBand(1));
    cs16.SetSrcWindow(0, 0, 4, 1);
    cs16.SetDstWindow(0, 0, 4, 1);
    cs16.SetNoDataValue(-999.0);
    cs16.SetResampling("bilinear");

    VRTSource::WorkingState oWorkingState;
    int8_t nI8[4] = {};
    EXPECT_EQ(CE_None, cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nI8, 4, 1, GDT_Int8,
                                     1, 4, nullptr, oWorkingState));
    uint16_t nU16[4] = {};
    EXPECT_EQ(CE_None, cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nU16, 4, 1,
                                     GDT_UInt16, 2, 8, nullptr, oWorkingState));
    int16_t nI16[4] = {};
    EXPECT_EQ(CE_None, cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nI16, 4, 1,
                                     GDT_Int16, 2, 8, nullptr, oWorkingState));
    uint32_t nU32[4] = {};
    EXPECT_EQ(CE_None,
              cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nU32, 4, 1, GDT_UInt32, 4,
                            16, nullptr, oWorkingState));
    int32_t nI32[4] = {};
    EXPECT_EQ(CE_None, cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nI32, 4, 1,
                                     GDT_Int32, 4, 16, nullptr, oWorkingState));
    uint64_t nU64[4] = {};
    EXPECT_EQ(CE_None,
              cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nU64, 4, 1, GDT_UInt64, 8,
                            32, nullptr, oWorkingState));
    int64_t nI64[4] = {};
    EXPECT_EQ(CE_None, cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nI64, 4, 1,
                                     GDT_Int64, 8, 32, nullptr, oWorkingState));
    float f32[4] = {};
    EXPECT_EQ(CE_None,
              cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, f32, 4, 1, GDT_Float32, 4,
                            16, nullptr, oWorkingState));
    double f64[4] = {};
    EXPECT_EQ(CE_None,
              cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, f64, 4, 1, GDT_Float64, 8,
                            32, nullptr, oWorkingState));

    // Out of range nodata for Int16.
    cs16.SetNoDataValue(1000000.0);
    EXPECT_EQ(CE_None, cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nI16, 4, 1,
                                     GDT_Int16, 2, 8, nullptr, oWorkingState));

    // UInt16 dataset.
    GDALDatasetUniquePtr poDSU16(
        poDrv->Create("/vsimem/vrtsources_test/vrt_cs_u16.tif", 4, 1, 1,
                      GDT_UInt16, nullptr));
    ASSERT_NE(nullptr, poDSU16);
    TestComplexSource csU16;
    csU16.m_bDropRefOnSrcBand = false;
    csU16.SetSrcBand(poDSU16->GetRasterBand(1));
    csU16.SetSrcWindow(0, 0, 4, 1);
    csU16.SetDstWindow(0, 0, 4, 1);
    csU16.SetNoDataValue(500.0);
    EXPECT_EQ(CE_None,
              csU16.RasterIO(GDT_UInt16, 0, 0, 4, 1, nU16, 4, 1, GDT_UInt16, 2,
                             8, nullptr, oWorkingState));
    csU16.SetNoDataValue(-100.0);
    EXPECT_EQ(CE_None,
              csU16.RasterIO(GDT_UInt16, 0, 0, 4, 1, nU16, 4, 1, GDT_UInt16, 2,
                             8, nullptr, oWorkingState));
  }

  // 3. RasterIOInternal<double> for Float64.
  {
    GDALDatasetUniquePtr poDSF64(
        poDrv->Create("/vsimem/vrtsources_test/vrt_cs_f64.tif", 2, 2, 1,
                      GDT_Float64, nullptr));
    ASSERT_NE(nullptr, poDSF64);
    TestComplexSource csF64;
    csF64.m_bDropRefOnSrcBand = false;
    csF64.SetSrcBand(poDSF64->GetRasterBand(1));
    csF64.SetSrcWindow(0, 0, 2, 2);
    csF64.SetDstWindow(0, 0, 2, 2);
    csF64.SetLinearScaling(10.0, 2.0);

    VRTSource::WorkingState oWorkingState;
    std::vector<double> adfOut(4, 0.0);
    EXPECT_EQ(CE_None,
              csF64.RasterIO(GDT_Float64, 0, 0, 2, 2, adfOut.data(), 2, 2,
                             GDT_Float64, sizeof(double), 2 * sizeof(double),
                             nullptr, oWorkingState));
  }

  // 4. ColorTable component extraction.
  {
    GDALDatasetUniquePtr poDSCT(poDrv->Create(
        "/vsimem/vrtsources_test/vrt_cs_ct.tif", 2, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDSCT);
    GDALColorTable oCT;
    GDALColorEntry oEntry1 = {10, 20, 30, 255};
    oCT.SetColorEntry(0, &oEntry1);
    EXPECT_EQ(CE_None, poDSCT->GetRasterBand(1)->SetColorTable(&oCT));

    TestComplexSource csCT;
    csCT.m_bDropRefOnSrcBand = false;
    csCT.SetSrcBand(poDSCT->GetRasterBand(1));
    csCT.SetSrcWindow(0, 0, 2, 1);
    csCT.SetDstWindow(0, 0, 2, 1);

    VRTSource::WorkingState oWorkingState;
    std::vector<GByte> abyOut(2, 0);
    for (int comp = 1; comp <= 4; ++comp) {
      csCT.SetColorTableComponent(comp);
      EXPECT_EQ(CE_None,
                csCT.RasterIO(GDT_Byte, 0, 0, 2, 1, abyOut.data(), 2, 1,
                              GDT_Byte, 1, 2, nullptr, oWorkingState));
    }
  }

  // 5. Exponential scaling with automatic min/max computation and clipping.
  {
    GDALDatasetUniquePtr poDSExp(poDrv->Create(
        "/vsimem/vrtsources_test/vrt_cs_exp.tif", 4, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDSExp);
    std::vector<GByte> abyExp = {10, 20, 30, 40};
    ASSERT_EQ(CE_None,
              poDSExp->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, 0, 4, 1, abyExp.data(), 4, 1, GDT_Byte, 0, 0));

    TestComplexSource csExp;
    csExp.m_bDropRefOnSrcBand = false;
    csExp.SetSrcBand(poDSExp->GetRasterBand(1));
    csExp.SetSrcWindow(0, 0, 4, 1);
    csExp.SetDstWindow(0, 0, 4, 1);
    csExp.SetPowerScaling(2.0, 0.0, 100.0, 0.0, 255.0);
    csExp.m_bSrcMinMaxDefined = false;

    VRTSource::WorkingState oWorkingState;
    std::vector<GByte> abyOut(4, 0);
    EXPECT_EQ(CE_None, csExp.RasterIO(GDT_Byte, 0, 0, 4, 1, abyOut.data(), 4, 1,
                                      GDT_Byte, 1, 4, nullptr, oWorkingState));
  }

  // 6. Complex data types (CFloat32).
  {
    GDALDatasetUniquePtr poDSCpx(
        poDrv->Create("/vsimem/vrtsources_test/vrt_cs_cpx.tif", 2, 1, 1,
                      GDT_CFloat32, nullptr));
    ASSERT_NE(nullptr, poDSCpx);

    TestComplexSource csCpx;
    csCpx.m_bDropRefOnSrcBand = false;
    csCpx.SetSrcBand(poDSCpx->GetRasterBand(1));
    csCpx.SetSrcWindow(0, 0, 2, 1);
    csCpx.SetDstWindow(0, 0, 2, 1);
    csCpx.SetLinearScaling(10.0, 2.0);

    VRTSource::WorkingState oWorkingState;
    std::vector<std::complex<float>> aCpxOut(2);
    EXPECT_EQ(CE_None,
              csCpx.RasterIO(GDT_CFloat32, 0, 0, 2, 1, aCpxOut.data(), 2, 1,
                             GDT_CFloat32, sizeof(std::complex<float>),
                             2 * sizeof(std::complex<float>), nullptr,
                             oWorkingState));
  }

  // 7. Constant value writing optimization with MaxValue clamp.
  {
    GDALDatasetUniquePtr poDSConst(
        poDrv->Create("/vsimem/vrtsources_test/vrt_cs_const.tif", 2, 1, 1,
                      GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDSConst);

    TestComplexSource csConst;
    csConst.m_bDropRefOnSrcBand = false;
    csConst.SetSrcBand(poDSConst->GetRasterBand(1));
    csConst.SetSrcWindow(0, 0, 2, 1);
    csConst.SetDstWindow(0, 0, 2, 1);
    csConst.SetLinearScaling(50.0, 0.0);
    csConst.SetMaxValue(100);

    VRTSource::WorkingState oWorkingState;
    std::vector<GByte> abyOut(2, 0);
    EXPECT_EQ(CE_None,
              csConst.RasterIO(GDT_Byte, 0, 0, 2, 1, abyOut.data(), 2, 1,
                               GDT_Byte, 1, 2, nullptr, oWorkingState));
    EXPECT_EQ(50, abyOut[0]);
  }

  // 8. Two-step conversion eVRTBandDataType == GDT_Float32 && eBufType ==
  // GDT_Float64.
  {
    GDALDatasetUniquePtr poDSS32(poDrv->Create(
        "/vsimem/vrtsources_test/vrt_cs_s32.tif", 2, 1, 1, GDT_Int32, nullptr));
    ASSERT_NE(nullptr, poDSS32);

    TestComplexSource csS32;
    csS32.m_bDropRefOnSrcBand = false;
    csS32.SetSrcBand(poDSS32->GetRasterBand(1));
    csS32.SetSrcWindow(0, 0, 2, 1);
    csS32.SetDstWindow(0, 0, 2, 1);
    csS32.SetLinearScaling(0.0, 1.0);

    VRTSource::WorkingState oWorkingState;
    std::vector<double> adfOut(2, 0.0);
    EXPECT_EQ(CE_None,
              csS32.RasterIO(GDT_Float32, 0, 0, 2, 1, adfOut.data(), 2, 1,
                             GDT_Float64, sizeof(double), 2 * sizeof(double),
                             nullptr, oWorkingState));
  }

  // 9. PROCESSING_FLAG_USE_MASK_BAND.
  {
    GDALDatasetUniquePtr poDSMask(poDrv->Create(
        "/vsimem/vrtsources_test/vrt_cs_mask.tif", 4, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDSMask);
    EXPECT_EQ(CE_None, poDSMask->CreateMaskBand(GMF_PER_DATASET));
    GDALRasterBand* poMask = poDSMask->GetRasterBand(1)->GetMaskBand();
    GByte abyMask[4] = {255, 0, 255, 0};
    ASSERT_EQ(CE_None, poMask->RasterIO(GF_Write, 0, 0, 4, 1, abyMask, 4, 1,
                                        GDT_Byte, 0, 0));

    TestComplexSource csMask;
    csMask.m_bDropRefOnSrcBand = false;
    csMask.SetSrcBand(poDSMask->GetRasterBand(1));
    csMask.SetSrcWindow(0, 0, 4, 1);
    csMask.SetDstWindow(0, 0, 4, 1);
    csMask.SetLinearScaling(10.0, 1.0);
    csMask.SetUseMaskBand(true);

    VRTSource::WorkingState oWorkingState;
    std::vector<GByte> abyOut(4, 0);
    EXPECT_EQ(CE_None,
              csMask.RasterIO(GDT_Byte, 0, 0, 4, 1, abyOut.data(), 4, 1,
                              GDT_Byte, 1, 4, nullptr, oWorkingState));
  }

  // 10. Floating point window validity in extra args.
  {
    GDALDatasetUniquePtr poDSFP(poDrv->Create(
        "/vsimem/vrtsources_test/vrt_cs_fp.tif", 4, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDSFP);

    TestComplexSource csFP;
    csFP.m_bDropRefOnSrcBand = false;
    csFP.SetSrcBand(poDSFP->GetRasterBand(1));
    csFP.SetSrcWindow(0, 0, 4, 1);
    csFP.SetDstWindow(0, 0, 4, 1);
    csFP.SetLinearScaling(0.0, 1.0);

    GDALRasterIOExtraArg sExtra;
    INIT_RASTERIO_EXTRA_ARG(sExtra);
    sExtra.bFloatingPointWindowValidity = TRUE;
    sExtra.dfXOff = 0.5;
    sExtra.dfYOff = 0.0;
    sExtra.dfXSize = 2.0;
    sExtra.dfYSize = 1.0;

    VRTSource::WorkingState oWorkingState;
    std::vector<GByte> abyOut(4, 0);
    EXPECT_EQ(CE_None, csFP.RasterIO(GDT_Byte, 0, 0, 4, 1, abyOut.data(), 4, 1,
                                     GDT_Byte, 1, 4, &sExtra, oWorkingState));
  }

  // 11. Out-of-range nodata value for Byte (delegating to VRTSimpleSource).
  {
    GDALDatasetUniquePtr poDSOOR(
        poDrv->Create("/vsimem/vrtsources_test/vrt_cs_oor_byte.tif", 4, 1, 1,
                      GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poDSOOR);

    TestComplexSource csOOR;
    csOOR.m_bDropRefOnSrcBand = false;
    csOOR.SetSrcBand(poDSOOR->GetRasterBand(1));
    csOOR.SetSrcWindow(0, 0, 4, 1);
    csOOR.SetDstWindow(0, 0, 4, 1);
    csOOR.SetNoDataValue(300.0);

    VRTSource::WorkingState oWorkingState;
    std::vector<GByte> abyOut(4, 0);
    EXPECT_EQ(CE_None, csOOR.RasterIO(GDT_Byte, 0, 0, 4, 1, abyOut.data(), 4, 1,
                                      GDT_Byte, 1, 4, nullptr, oWorkingState));
  }
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover remaining defensive error paths in VRTComplexSource LUT
// interpolation and overview open fallbacks in vrtsources.cpp.
