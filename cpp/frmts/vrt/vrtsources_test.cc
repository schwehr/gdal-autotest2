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

#include <algorithm>
#include <climits>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/vrt/gdal_vrt.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

CPLErr MyFuncSourceRead(void* hCBData, int nXOff, int nYOff, int nXSize,
                        int nYSize, void* pData) {
  (void)hCBData;
  (void)nXOff;
  (void)nYOff;
  auto pabyData = static_cast<GByte*>(pData);
  std::fill(pabyData, pabyData + nXSize * nYSize, static_cast<GByte>(77));
  return CE_None;
}

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

class VRTSourcesTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { GDALAllRegister(); }
  static void TearDownTestSuite() { GDALDestroyDriverManager(); }

  void TearDown() override {
    for (const auto& filename : to_unlink_) {
      VSIUnlink(filename.c_str());
    }
    to_unlink_.clear();
  }

  void TrackFile(const std::string& filename) {
    to_unlink_.push_back(filename);
  }

 private:
  std::vector<std::string> to_unlink_;
};

// ---------------------------------------------------------------------------
// VRTSource Base Class & VRTParseCoreSources Tests
// ---------------------------------------------------------------------------

TEST_F(VRTSourcesTest, VRTParseCoreSourcesAndBase) {
  VRTMapSharedResources mapSharedSources;

  // Unknown source element
  {
    CPLXMLTreeCloser oTree(
        CPLParseXMLString("<UnknownSourceType><SourceFilename "
                          "relativeToVRT=\"0\">foo.tif</SourceFilename></"
                          "UnknownSourceType>"));
    ASSERT_NE(oTree.get(), nullptr);
    CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
    EXPECT_EQ(VRTParseCoreSources(oTree.get(), nullptr, mapSharedSources),
              nullptr);
  }

  // SimpleSource
  {
    CPLXMLTreeCloser oTree(
        CPLParseXMLString("<SimpleSource><SourceFilename "
                          "relativeToVRT=\"0\">foo.tif</SourceFilename></"
                          "SimpleSource>"));
    ASSERT_NE(oTree.get(), nullptr);
    VRTSource* poSrc =
        VRTParseCoreSources(oTree.get(), nullptr, mapSharedSources);
    ASSERT_NE(poSrc, nullptr);
    EXPECT_STREQ(poSrc->GetType(), "SimpleSource");
    delete poSrc;
  }

  // AveragedSource
  {
    CPLXMLTreeCloser oTree(
        CPLParseXMLString("<AveragedSource><SourceFilename "
                          "relativeToVRT=\"0\">foo.tif</SourceFilename></"
                          "AveragedSource>"));
    ASSERT_NE(oTree.get(), nullptr);
    VRTSource* poSrc =
        VRTParseCoreSources(oTree.get(), nullptr, mapSharedSources);
    ASSERT_NE(poSrc, nullptr);
    EXPECT_STREQ(poSrc->GetType(), "AveragedSource");
    delete poSrc;
  }

  // SimpleSource with Resampling="Average"
  {
    CPLXMLTreeCloser oTree(CPLParseXMLString(
        "<SimpleSource resampling=\"Average\"><SourceFilename "
        "relativeToVRT=\"0\">foo.tif</SourceFilename></"
        "SimpleSource>"));
    ASSERT_NE(oTree.get(), nullptr);
    VRTSource* poSrc =
        VRTParseCoreSources(oTree.get(), nullptr, mapSharedSources);
    ASSERT_NE(poSrc, nullptr);
    EXPECT_STREQ(poSrc->GetType(), "AveragedSource");
    delete poSrc;
  }

  // ComplexSource
  {
    CPLXMLTreeCloser oTree(
        CPLParseXMLString("<ComplexSource><SourceFilename "
                          "relativeToVRT=\"0\">foo.tif</SourceFilename></"
                          "ComplexSource>"));
    ASSERT_NE(oTree.get(), nullptr);
    VRTSource* poSrc =
        VRTParseCoreSources(oTree.get(), nullptr, mapSharedSources);
    ASSERT_NE(poSrc, nullptr);
    EXPECT_STREQ(poSrc->GetType(), "ComplexSource");
    delete poSrc;
  }

  // NoDataFromMaskSource
  {
    CPLXMLTreeCloser oTree(
        CPLParseXMLString("<NoDataFromMaskSource><SourceFilename "
                          "relativeToVRT=\"0\">foo.tif</SourceFilename></"
                          "NoDataFromMaskSource>"));
    ASSERT_NE(oTree.get(), nullptr);
    VRTSource* poSrc =
        VRTParseCoreSources(oTree.get(), nullptr, mapSharedSources);
    ASSERT_NE(poSrc, nullptr);
    EXPECT_STREQ(poSrc->GetType(), "NoDataFromMaskSource");
    delete poSrc;
  }

  // VRTSource::GetFileList default implementation (empty)
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
    int nSize = 0, nMaxSize = 0;
    CPLHashSet* hSetFiles =
        CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);
    dummy.GetFileList(&papszFileList, &nSize, &nMaxSize, hSetFiles);
    EXPECT_EQ(papszFileList, nullptr);
    CPLHashSetDestroy(hSetFiles);
  }
}

// ---------------------------------------------------------------------------
// VRTSimpleSource Properties, Accessors & Transformations
// ---------------------------------------------------------------------------

TEST_F(VRTSourcesTest, SimpleSourcePropertiesAndTransforms) {
  TestSimpleSource ss;
  EXPECT_STREQ(VRTSimpleSource::GetTypeStatic(), "SimpleSource");
  EXPECT_STREQ(ss.GetType(), "SimpleSource");
  EXPECT_TRUE(ss.IsSimpleSource());

  // Windows not set initially
  EXPECT_FALSE(ss.IsSrcWinSet());
  EXPECT_FALSE(ss.IsDstWinSet());

  // Set windows
  ss.SetSrcWindow(10, 20, 30, 40);
  EXPECT_TRUE(ss.IsSrcWinSet());
  ss.SetDstWindow(50, 60, 70, 80);
  EXPECT_TRUE(ss.IsDstWinSet());

  double dfDstXOff = 0, dfDstYOff = 0, dfDstXSize = 0, dfDstYSize = 0;
  ss.GetDstWindow(dfDstXOff, dfDstYOff, dfDstXSize, dfDstYSize);
  EXPECT_EQ(dfDstXOff, 50.0);
  EXPECT_EQ(dfDstYOff, 60.0);
  EXPECT_EQ(dfDstXSize, 70.0);
  EXPECT_EQ(dfDstYSize, 80.0);

  // DstWindowIntersects
  EXPECT_TRUE(ss.DstWindowIntersects(60, 70, 10, 10));
  EXPECT_FALSE(ss.DstWindowIntersects(0, 0, 10, 10));
  EXPECT_FALSE(ss.DstWindowIntersects(200, 200, 10, 10));

  // SrcToDst and DstToSrc
  double dfX = 0, dfY = 0;
  ss.SrcToDst(10, 20, dfX, dfY);
  EXPECT_DOUBLE_EQ(dfX, 50.0);
  EXPECT_DOUBLE_EQ(dfY, 60.0);

  ss.DstToSrc(50, 60, dfX, dfY);
  EXPECT_DOUBLE_EQ(dfX, 10.0);
  EXPECT_DOUBLE_EQ(dfY, 20.0);

  // SetResampling
  ss.SetResampling("bilinear");
  EXPECT_STREQ(ss.GetResampling().c_str(), "bilinear");

  // NeedMaxValAdjustment without band
  EXPECT_EQ(ss.NeedMaxValAdjustment(), FALSE);
  ss.SetMaxValue(100);
  EXPECT_EQ(ss.NeedMaxValAdjustment(), FALSE);  // no band yet

  // UnsetPreservedRelativeFilenames & FlushCache
  ss.UnsetPreservedRelativeFilenames();
  EXPECT_EQ(ss.FlushCache(false), CE_None);
  EXPECT_EQ(ss.FlushCache(true), CE_None);
}

TEST_F(VRTSourcesTest, SimpleSourceScaleConstructorAndComparison) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  TrackFile("/vsimem/vrt_src_test.tif");
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/vrt_src_test.tif", 40, 20, 2, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  poSrcDS.reset();

  VRTSimpleSource ss1;
  ss1.SetSrcBand("/vsimem/vrt_src_test.tif", 1);
  ss1.SetSrcWindow(0, 0, 40, 20);
  ss1.SetDstWindow(0, 0, 80, 40);

  // Scale constructor with set dst window
  VRTSimpleSource ssScaled(&ss1, 0.5, 0.5);
  double dfDstXOff = 0, dfDstYOff = 0, dfDstXSize = 0, dfDstYSize = 0;
  ssScaled.GetDstWindow(dfDstXOff, dfDstYOff, dfDstXSize, dfDstYSize);
  EXPECT_EQ(dfDstXSize, 40.0);
  EXPECT_EQ(dfDstYSize, 20.0);

  // Scale constructor without set windows
  VRTSimpleSource ssUnset;
  ssUnset.SetSrcBand("/vsimem/vrt_src_test.tif", 1);
  VRTSimpleSource ssScaledFromUnset(&ssUnset, 2.0, 2.0);
  ssScaledFromUnset.GetDstWindow(dfDstXOff, dfDstYOff, dfDstXSize, dfDstYSize);
  EXPECT_EQ(dfDstXSize, 80.0);
  EXPECT_EQ(dfDstYSize, 40.0);
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
  // TODO: Remove this once the constructor is fixed upstream to only add a
  // reference when the band was already opened by poSrcSource.
  if (auto* poBand = ssScaledFromUnset.GetRasterBand()) {
    if (auto* poDS = poBand->GetDataset()) {
      poDS->ReleaseRef();
    }
  }

  // IsSameExceptBandNumber
  VRTSimpleSource ss2;
  ss2.SetSrcBand("/vsimem/vrt_src_test.tif", 2);
  ss2.SetSrcWindow(0, 0, 40, 20);
  ss2.SetDstWindow(0, 0, 80, 40);
  EXPECT_TRUE(ss1.IsSameExceptBandNumber(&ss2));

  VRTSimpleSource ssDiff;
  ssDiff.SetSrcBand("/vsimem/vrt_src_test.tif", 2);
  ssDiff.SetSrcWindow(0, 0, 20, 10);
  ssDiff.SetDstWindow(0, 0, 80, 40);
  EXPECT_FALSE(ss1.IsSameExceptBandNumber(&ssDiff));

  // GetRasterBand, GetMaskBandMainBand
  EXPECT_NE(ss1.GetRasterBand(), nullptr);
  EXPECT_NE(ss1.GetRasterBand()->GetDataset(), nullptr);
  EXPECT_EQ(ss1.GetMaskBandMainBand(), nullptr);

  // File list
  char** papszFileList = nullptr;
  int nSize = 0, nMaxSize = 0;
  CPLHashSet* hSetFiles =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);
  ss1.GetFileList(&papszFileList, &nSize, &nMaxSize, hSetFiles);
  ASSERT_NE(papszFileList, nullptr);
  EXPECT_GT(CSLCount(papszFileList), 0);
  CSLDestroy(papszFileList);
  CPLHashSetDestroy(hSetFiles);
}

// ---------------------------------------------------------------------------
// VRTSimpleSource GetSrcDstWindow Edge Cases
// ---------------------------------------------------------------------------

TEST_F(VRTSourcesTest, SimpleSourceGetSrcDstWindowBranches) {
  VRTSimpleSource ss;
  double dfReqXOff = 0, dfReqYOff = 0, dfReqXSize = 0, dfReqYSize = 0;
  int nReqXOff = 0, nReqYOff = 0, nReqXSize = 0, nReqYSize = 0;
  int nOutXOff = 0, nOutYOff = 0, nOutXSize = 0, nOutYSize = 0;
  bool bError = false;

  // 1. Unset/zero window sizes -> returns FALSE
  EXPECT_FALSE(ss.GetSrcDstWindow(0, 0, 10, 10, 10, 10, &dfReqXOff, &dfReqYOff,
                                  &dfReqXSize, &dfReqYSize, &nReqXOff,
                                  &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
                                  &nOutYOff, &nOutXSize, &nOutYSize, bError));

  TrackFile("/vsimem/srcdst_win_src.tif");
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);
  auto poSrcDS = std::unique_ptr<GDALDataset>(poDrv->Create(
      "/vsimem/srcdst_win_src.tif", 100, 100, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrcDS, nullptr);
  poSrcDS.reset();

  ss.SetSrcBand("/vsimem/srcdst_win_src.tif", 1);
  ss.SetSrcWindow(0, 0, 100, 100);
  ss.SetDstWindow(50, 50, 100, 100);

  // 2. Window completely misses dst window
  // Completely to the left
  EXPECT_FALSE(ss.GetSrcDstWindow(0, 50, 40, 40, 40, 40, &dfReqXOff, &dfReqYOff,
                                  &dfReqXSize, &dfReqYSize, &nReqXOff,
                                  &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
                                  &nOutYOff, &nOutXSize, &nOutYSize, bError));
  // Completely to the right
  EXPECT_FALSE(ss.GetSrcDstWindow(
      200, 50, 40, 40, 40, 40, &dfReqXOff, &dfReqYOff, &dfReqXSize, &dfReqYSize,
      &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff, &nOutYOff,
      &nOutXSize, &nOutYSize, bError));
  // Completely above
  EXPECT_FALSE(ss.GetSrcDstWindow(50, 0, 40, 40, 40, 40, &dfReqXOff, &dfReqYOff,
                                  &dfReqXSize, &dfReqYSize, &nReqXOff,
                                  &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
                                  &nOutYOff, &nOutXSize, &nOutYSize, bError));
  // Completely below
  EXPECT_FALSE(ss.GetSrcDstWindow(
      50, 200, 40, 40, 40, 40, &dfReqXOff, &dfReqYOff, &dfReqXSize, &dfReqYSize,
      &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff, &nOutYOff,
      &nOutXSize, &nOutYSize, bError));

  // 3. Partial overlap requiring clipping on left & top
  EXPECT_TRUE(ss.GetSrcDstWindow(30, 30, 50, 50, 50, 50, &dfReqXOff, &dfReqYOff,
                                 &dfReqXSize, &dfReqYSize, &nReqXOff, &nReqYOff,
                                 &nReqXSize, &nReqYSize, &nOutXOff, &nOutYOff,
                                 &nOutXSize, &nOutYSize, bError));
  EXPECT_GT(nOutXOff, 0);
  EXPECT_GT(nOutYOff, 0);

  // 4. Partial overlap requiring clipping on right & bottom
  EXPECT_TRUE(ss.GetSrcDstWindow(
      120, 120, 50, 50, 50, 50, &dfReqXOff, &dfReqYOff, &dfReqXSize,
      &dfReqYSize, &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
      &nOutYOff, &nOutXSize, &nOutYSize, bError));
  EXPECT_LT(nOutXSize, 50);
  EXPECT_LT(nOutYSize, 50);

  // 5. Source window with negative offsets
  ss.SetSrcWindow(-10, -10, 100, 100);
  EXPECT_TRUE(ss.GetSrcDstWindow(50, 50, 50, 50, 50, 50, &dfReqXOff, &dfReqYOff,
                                 &dfReqXSize, &dfReqYSize, &nReqXOff, &nReqYOff,
                                 &nReqXSize, &nReqYSize, &nOutXOff, &nOutYOff,
                                 &nOutXSize, &nOutYSize, bError));
  EXPECT_GE(nReqXOff, 0);
  EXPECT_GE(nReqYOff, 0);
}

// ---------------------------------------------------------------------------
// VRTSimpleSource XML Serialization & Deserialization
// ---------------------------------------------------------------------------

TEST_F(VRTSourcesTest, SimpleSourceXMLSerializationAndInit) {
  VRTMapSharedResources mapSharedSources;

  // 1. Missing SourceFilename and VRTDataset in XMLInit -> warning and failure
  {
    CPLXMLTreeCloser oTree(CPLParseXMLString(
        "<SimpleSource><SourceBand>1</SourceBand></SimpleSource>"));
    ASSERT_NE(oTree.get(), nullptr);
    VRTSimpleSource ss;
    CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
    EXPECT_EQ(ss.XMLInit(oTree.get(), nullptr, mapSharedSources), CE_Failure);
  }

  // 2. Full XMLInit with SourceProperties, OpenOptions, SrcRect, DstRect
  {
    const char* pszXML =
        "<SimpleSource resampling=\"bilinear\" name=\"test_src\">"
        "  <SourceFilename relativeToVRT=\"1\" "
        "shared=\"1\">test.tif</SourceFilename>"
        "  <OpenOptions>"
        "    <OOI key=\"OVERVIEW_LEVEL\">0</OOI>"
        "  </OpenOptions>"
        "  <SourceBand>1</SourceBand>"
        "  <SourceProperties RasterXSize=\"100\" RasterYSize=\"100\" "
        "DataType=\"Byte\" BlockXSize=\"16\" BlockYSize=\"16\"/>"
        "  <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"100\" ySize=\"100\"/>"
        "  <DstRect xOff=\"0\" yOff=\"0\" xSize=\"200\" ySize=\"200\"/>"
        "</SimpleSource>";
    CPLXMLTreeCloser oTree(CPLParseXMLString(pszXML));
    ASSERT_NE(oTree.get(), nullptr);
    VRTSimpleSource ss;
    EXPECT_EQ(ss.XMLInit(oTree.get(), "/vsimem", mapSharedSources), CE_None);
    EXPECT_STREQ(ss.GetResampling().c_str(), "bilinear");

    // Serialize back to XML
    CPLXMLTreeCloser oOutTree(ss.SerializeToXML("/vsimem"));
    ASSERT_NE(oOutTree.get(), nullptr);
    char* pszOut = CPLSerializeXMLTree(oOutTree.get());
    ASSERT_NE(pszOut, nullptr);
    EXPECT_NE(strstr(pszOut, "test.tif"), nullptr);
    EXPECT_NE(strstr(pszOut, "bilinear"), nullptr);
    CPLFree(pszOut);
  }

  // 3. Embedded VRTDataset in XMLInit
  {
    const char* pszXML =
        "<SimpleSource>"
        "  <VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">"
        "    <VRTRasterBand dataType=\"Byte\" band=\"1\"/>"
        "  </VRTDataset>"
        "  <SourceBand>1</SourceBand>"
        "  <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"10\" ySize=\"10\"/>"
        "  <DstRect xOff=\"0\" yOff=\"0\" xSize=\"10\" ySize=\"10\"/>"
        "</SimpleSource>";
    CPLXMLTreeCloser oTree(CPLParseXMLString(pszXML));
    ASSERT_NE(oTree.get(), nullptr);
    VRTSimpleSource ss;
    EXPECT_EQ(ss.XMLInit(oTree.get(), nullptr, mapSharedSources), CE_None);

    CPLXMLTreeCloser oOutTree(ss.SerializeToXML("/vsimem"));
    ASSERT_NE(oOutTree.get(), nullptr);
    char* pszOut = CPLSerializeXMLTree(oOutTree.get());
    ASSERT_NE(pszOut, nullptr);
    EXPECT_NE(strstr(pszOut, "VRTDataset"), nullptr);
    CPLFree(pszOut);
  }

  // 4. ParseSrcRectAndDstRect errors
  {
    VRTSimpleSource ss;
    // Missing SrcRect / DstRect
    CPLXMLTreeCloser oTreeEmpty(CPLParseXMLString("<SimpleSource/>"));
    EXPECT_EQ(ss.ParseSrcRectAndDstRect(oTreeEmpty.get()), CE_None);

    // Negative sizes in SrcRect
    CPLXMLTreeCloser oTreeNegSrc(
        CPLParseXMLString("<SimpleSource><SrcRect xOff=\"0\" yOff=\"0\" "
                          "xSize=\"-10\" ySize=\"10\"/></SimpleSource>"));
    CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
    EXPECT_EQ(ss.ParseSrcRectAndDstRect(oTreeNegSrc.get()), CE_Failure);

    // Negative sizes in DstRect
    CPLXMLTreeCloser oTreeNegDst(
        CPLParseXMLString("<SimpleSource><DstRect xOff=\"0\" yOff=\"0\" "
                          "xSize=\"10\" ySize=\"-10\"/></SimpleSource>"));
    EXPECT_EQ(ss.ParseSrcRectAndDstRect(oTreeNegDst.get()), CE_Failure);
  }
}

// ---------------------------------------------------------------------------
// VRTSimpleSource RasterIO, DatasetRasterIO & Statistics
// ---------------------------------------------------------------------------

TEST_F(VRTSourcesTest, SimpleSourceRasterIOAndStatistics) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  TrackFile("/vsimem/ss_rio_src.tif");
  auto poSrc = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/ss_rio_src.tif", 20, 20, 1, GDT_Float32, nullptr));
  ASSERT_NE(poSrc, nullptr);
  std::vector<float> afData(400);
  for (int i = 0; i < 400; ++i) afData[i] = static_cast<float>(i);
  EXPECT_EQ(
      poSrc->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 20, 20, afData.data(),
                                        20, 20, GDT_Float32, 4, 80, nullptr),
      CE_None);
  poSrc->GetRasterBand(1)->SetStatistics(0.0, 399.0, 199.5, 50.0);
  poSrc->GetRasterBand(1)->SetMetadataItem("NBITS", "12", "IMAGE_STRUCTURE");
  poSrc.reset();

  VRTSimpleSource ss;
  ss.SetSrcBand("/vsimem/ss_rio_src.tif", 1);
  ss.SetSrcWindow(0, 0, 20, 20);
  ss.SetDstWindow(0, 0, 20, 20);
  ss.SetMaxValue(100);  // Clamps max value to 100!

  VRTSource::WorkingState oWorkingState;

  // 1. Lossy conversion (source Float32 to VRT Byte) with MaxValue clamp and
  // progress
  std::vector<GByte> abyOut(400, 0);
  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);
  int nProgCalls = 0;
  sExtraArg.pfnProgress = [](double, const char*, void* pData) -> int {
    (*static_cast<int*>(pData))++;
    return 1;
  };
  sExtraArg.pProgressData = &nProgCalls;

  EXPECT_EQ(ss.RasterIO(GDT_Byte, 0, 0, 20, 20, abyOut.data(), 20, 20, GDT_Byte,
                        1, 20, &sExtraArg, oWorkingState),
            CE_None);
  EXPECT_GT(nProgCalls, 0);
  EXPECT_EQ(abyOut[0], 0);
  EXPECT_LE(abyOut[399], 100);  // Clamped to 100!

  // 2. Same-type conversion
  std::vector<float> afOut(400, 0);
  EXPECT_EQ(ss.RasterIO(GDT_Float32, 0, 0, 20, 20, afOut.data(), 20, 20,
                        GDT_Float32, 4, 80, nullptr, oWorkingState),
            CE_None);
  EXPECT_FLOAT_EQ(afOut[0], 0.0f);

  // 3. GetMinimum, GetMaximum, GetHistogram
  int bSuccess = FALSE;
  double dfMin = ss.GetMinimum(20, 20, &bSuccess);
  EXPECT_TRUE(bSuccess);
  EXPECT_EQ(dfMin, 0.0);

  double dfMax = ss.GetMaximum(20, 20, &bSuccess);
  EXPECT_TRUE(bSuccess);
  EXPECT_EQ(dfMax, 100.0);  // Clamped by m_nMaxValue

  GUIntBig anHistogram[256];
  // Fails when NeedMaxValAdjustment() is true
  EXPECT_EQ(ss.GetHistogram(20, 20, -0.5, 255.5, 256, anHistogram, FALSE, TRUE,
                            nullptr, nullptr),
            CE_Failure);

  // Succeeds when MaxValue is cleared
  ss.SetMaxValue(0);
  EXPECT_EQ(ss.GetHistogram(20, 20, -0.5, 255.5, 256, anHistogram, FALSE, TRUE,
                            nullptr, nullptr),
            CE_None);

  // 4. DatasetRasterIO
  int anBandMap[1] = {1};
  std::vector<GByte> abyDSOut(400, 0);
  EXPECT_EQ(ss.DatasetRasterIO(GDT_Byte, 0, 0, 20, 20, abyDSOut.data(), 20, 20,
                               GDT_Byte, 1, anBandMap, 1, 20, 400, nullptr),
            CE_None);

  // DatasetRasterIO failure on non-SimpleSource
  VRTComplexSource cs;
  CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
  EXPECT_EQ(cs.DatasetRasterIO(GDT_Byte, 0, 0, 20, 20, abyDSOut.data(), 20, 20,
                               GDT_Byte, 1, anBandMap, 1, 20, 400, nullptr),
            CE_Failure);
}

// ---------------------------------------------------------------------------
// VRTAveragedSource Tests
// ---------------------------------------------------------------------------

TEST_F(VRTSourcesTest, AveragedSourceRasterIOAndStatistics) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  TrackFile("/vsimem/avg_src.tif");
  auto poSrc = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/avg_src.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrc, nullptr);
  std::vector<GByte> abyData(400, 100);
  // Set 2x2 corner to 10, 20, 30, 40 -> average is 25
  abyData[0] = 10;
  abyData[1] = 20;
  abyData[20] = 30;
  abyData[21] = 40;
  EXPECT_EQ(
      poSrc->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 20, 20, abyData.data(),
                                        20, 20, GDT_Byte, 1, 20, nullptr),
      CE_None);
  poSrc.reset();

  VRTAveragedSource as;
  EXPECT_STREQ(VRTAveragedSource::GetTypeStatic(), "AveragedSource");
  EXPECT_STREQ(as.GetType(), "AveragedSource");

  as.SetSrcBand("/vsimem/avg_src.tif", 1);
  as.SetSrcWindow(0, 0, 20, 20);
  as.SetDstWindow(0, 0, 10, 10);  // 2x downsampling!

  VRTSource::WorkingState oWorkingState;

  // 1. Downsampling to 10x10 Byte buffer
  std::vector<GByte> abyOut(100, 0);
  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);
  int nProgCalls = 0;
  sExtraArg.pfnProgress = [](double, const char*, void* pData) -> int {
    (*static_cast<int*>(pData))++;
    return 1;
  };
  sExtraArg.pProgressData = &nProgCalls;

  EXPECT_EQ(as.RasterIO(GDT_Byte, 0, 0, 10, 10, abyOut.data(), 10, 10, GDT_Byte,
                        1, 10, &sExtraArg, oWorkingState),
            CE_None);
  EXPECT_GT(nProgCalls, 0);
  EXPECT_EQ(abyOut[0], 25);  // Average of 10, 20, 30, 40!

  // 2. Downsampling to Float32 buffer
  std::vector<float> afOut(100, 0);
  EXPECT_EQ(as.RasterIO(GDT_Byte, 0, 0, 10, 10, afOut.data(), 10, 10,
                        GDT_Float32, 4, 40, nullptr, oWorkingState),
            CE_None);
  EXPECT_FLOAT_EQ(afOut[0], 25.0f);

  // 3. Upsampling (<100% scale factor, triggers nearest-neighbour branch)
  std::vector<GByte> abyUpOut(1600, 0);
  EXPECT_EQ(as.RasterIO(GDT_Byte, 0, 0, 10, 10, abyUpOut.data(), 40, 40,
                        GDT_Byte, 1, 40, nullptr, oWorkingState),
            CE_None);

  // 4. With NoDataValue set
  as.SetNoDataValue(20.0);
  EXPECT_EQ(as.RasterIO(GDT_Byte, 0, 0, 10, 10, abyOut.data(), 10, 10, GDT_Byte,
                        1, 10, nullptr, oWorkingState),
            CE_None);
  // Pixel 20 was excluded from average of (10, 30, 40) -> 80 / 3 = 26.66 ->
  // rounded to 27
  EXPECT_EQ(abyOut[0], 27);

  // 5. SerializeToXML
  CPLXMLTreeCloser oTree(as.SerializeToXML("/vsimem"));
  ASSERT_NE(oTree.get(), nullptr);
  EXPECT_STREQ(oTree->pszValue, "AveragedSource");

  // 6. GetMinimum, GetMaximum, GetHistogram
  int bSuccess = FALSE;
  EXPECT_EQ(as.GetMinimum(10, 10, &bSuccess), 0.0);
  EXPECT_FALSE(bSuccess);
  EXPECT_EQ(as.GetMaximum(10, 10, &bSuccess), 0.0);
  EXPECT_FALSE(bSuccess);
  EXPECT_EQ(as.GetHistogram(10, 10, 0, 255, 256, nullptr, FALSE, TRUE, nullptr,
                            nullptr),
            CE_Failure);
}

// ---------------------------------------------------------------------------
// VRTNoDataFromMaskSource Tests
// ---------------------------------------------------------------------------

TEST_F(VRTSourcesTest, NoDataFromMaskSourceRasterIOAndXML) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  TrackFile("/vsimem/ndm_src.tif");
  auto poSrc = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/ndm_src.tif", 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrc, nullptr);
  std::vector<GByte> abyData(100, 50);
  EXPECT_EQ(
      poSrc->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 10, 10, abyData.data(),
                                        10, 10, GDT_Byte, 1, 10, nullptr),
      CE_None);
  // Create mask band: top half 0 (nodata), bottom half 255 (valid)
  EXPECT_EQ(poSrc->CreateMaskBand(GMF_PER_DATASET), CE_None);
  std::vector<GByte> abyMask(100, 255);
  std::fill(abyMask.begin(), abyMask.begin() + 50, 0);
  EXPECT_EQ(poSrc->GetRasterBand(1)->GetMaskBand()->RasterIO(
                GF_Write, 0, 0, 10, 10, abyMask.data(), 10, 10, GDT_Byte, 1, 10,
                nullptr),
            CE_None);
  poSrc.reset();

  VRTNoDataFromMaskSource ndm;
  EXPECT_STREQ(VRTNoDataFromMaskSource::GetTypeStatic(),
               "NoDataFromMaskSource");
  EXPECT_STREQ(ndm.GetType(), "NoDataFromMaskSource");

  ndm.SetSrcBand("/vsimem/ndm_src.tif", 1);
  ndm.SetSrcWindow(0, 0, 10, 10);
  ndm.SetDstWindow(0, 0, 10, 10);

  VRTSource::WorkingState oWorkingState;

  // 1. Without nodata set -> forwards to VRTSimpleSource::RasterIO
  std::vector<GByte> abyOut(100, 0);
  EXPECT_EQ(ndm.RasterIO(GDT_Byte, 0, 0, 10, 10, abyOut.data(), 10, 10,
                         GDT_Byte, 1, 10, nullptr, oWorkingState),
            CE_None);
  EXPECT_EQ(abyOut[0], 50);

  // 2. With parameters set: mask <= 0 remapped to 0
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

  EXPECT_EQ(ndm.RasterIO(GDT_Byte, 0, 0, 10, 10, abyOut.data(), 10, 10,
                         GDT_Byte, 1, 10, &sExtraArg, oWorkingState),
            CE_None);
  EXPECT_GT(nProgCalls, 0);
  EXPECT_EQ(abyOut[0], 0);    // Top half remapped to 0
  EXPECT_EQ(abyOut[99], 50);  // Bottom half remains 50

  // 3. SerializeToXML and XMLInit
  CPLXMLTreeCloser oTree(ndm.SerializeToXML("/vsimem"));
  ASSERT_NE(oTree.get(), nullptr);
  EXPECT_STREQ(oTree->pszValue, "NoDataFromMaskSource");

  VRTMapSharedResources mapSharedSources;
  VRTNoDataFromMaskSource ndm2;
  EXPECT_EQ(ndm2.XMLInit(oTree.get(), nullptr, mapSharedSources), CE_None);

  // 4. GetMinimum, GetMaximum, GetHistogram
  int bSuccess = FALSE;
  EXPECT_EQ(ndm.GetMinimum(10, 10, &bSuccess), 0.0);
  EXPECT_FALSE(bSuccess);
  EXPECT_EQ(ndm.GetMaximum(10, 10, &bSuccess), 0.0);
  EXPECT_FALSE(bSuccess);
  EXPECT_EQ(ndm.GetHistogram(10, 10, 0, 255, 256, nullptr, FALSE, TRUE, nullptr,
                             nullptr),
            CE_Failure);
}

// ---------------------------------------------------------------------------
// VRTComplexSource Properties, LUT & XML Tests
// ---------------------------------------------------------------------------

TEST_F(VRTSourcesTest, ComplexSourcePropertiesLUTAndXML) {
  TestComplexSource cs;
  EXPECT_STREQ(VRTComplexSource::GetTypeStatic(), "ComplexSource");
  EXPECT_STREQ(cs.GetType(), "ComplexSource");

  // Default: AreValuesUnchanged is true
  EXPECT_TRUE(cs.AreValuesUnchanged());

  // SetLinearScaling
  cs.SetLinearScaling(10.0, 2.0);
  EXPECT_FALSE(cs.AreValuesUnchanged());

  // SetPowerScaling
  cs.SetPowerScaling(2.0, 0.0, 100.0, 0.0, 255.0, /*bClip=*/true);
  EXPECT_FALSE(cs.AreValuesUnchanged());

  // SetColorTableComponent
  cs.SetColorTableComponent(1);
  EXPECT_FALSE(cs.AreValuesUnchanged());

  // SetNoDataValue
  cs.SetNoDataValue(99.0);
  EXPECT_EQ(cs.GetAdjustedNoDataValue(), 99.0);
  cs.SetNoDataValue(VRT_NODATA_UNSET);
  EXPECT_EQ(cs.GetAdjustedNoDataValue(), VRT_NODATA_UNSET);

  // LookupValue tests
  cs.m_adfLUTInputs = {0.0, 10.0, 20.0};
  cs.m_adfLUTOutputs = {0.0, 50.0, 100.0};
  EXPECT_DOUBLE_EQ(cs.LookupValue(-5.0), 0.0);    // Below min
  EXPECT_DOUBLE_EQ(cs.LookupValue(0.0), 0.0);     // Exact match
  EXPECT_DOUBLE_EQ(cs.LookupValue(5.0), 25.0);    // Interpolated
  EXPECT_DOUBLE_EQ(cs.LookupValue(10.0), 50.0);   // Exact match
  EXPECT_DOUBLE_EQ(cs.LookupValue(30.0), 100.0);  // Above max

  // LookupValue with NaN input at index 0
  cs.m_adfLUTInputs = {std::numeric_limits<double>::quiet_NaN(), 0.0, 10.0};
  cs.m_adfLUTOutputs = {-999.0, 0.0, 100.0};
  EXPECT_DOUBLE_EQ(cs.LookupValue(std::numeric_limits<double>::quiet_NaN()),
                   -999.0);

  // SerializeToXML with power scaling and LUT
  cs.SetNoDataValue(42.0);
  cs.SetUseMaskBand(true);
  CPLXMLTreeCloser oTree(cs.SerializeToXML("/vsimem"));
  ASSERT_NE(oTree.get(), nullptr);
  EXPECT_STREQ(oTree->pszValue, "ComplexSource");

  // XMLInit parsing valid LUT
  VRTMapSharedResources mapSharedSources;
  const char* pszXML =
      "<ComplexSource>"
      "  <SourceFilename relativeToVRT=\"0\">foo.tif</SourceFilename>"
      "  <SourceBand>1</SourceBand>"
      "  <ScaleOffset>10</ScaleOffset>"
      "  <ScaleRatio>2</ScaleRatio>"
      "  <NODATA>42</NODATA>"
      "  <UseMaskBand>true</UseMaskBand>"
      "  <ColorTableComponent>2</ColorTableComponent>"
      "  <LUT>0:0,10:50,20:100</LUT>"
      "</ComplexSource>";
  CPLXMLTreeCloser oValidTree(CPLParseXMLString(pszXML));
  ASSERT_NE(oValidTree.get(), nullptr);
  VRTComplexSource cs2;
  EXPECT_EQ(cs2.XMLInit(oValidTree.get(), nullptr, mapSharedSources), CE_None);

  // XMLInit error: NaN not at index 0
  const char* pszBadNaNXML =
      "<ComplexSource>"
      "  <SourceFilename relativeToVRT=\"0\">foo.tif</SourceFilename>"
      "  <LUT>0:0,nan:50,20:100</LUT>"
      "</ComplexSource>";
  CPLXMLTreeCloser oBadNaNDoc(CPLParseXMLString(pszBadNaNXML));
  ASSERT_NE(oBadNaNDoc.get(), nullptr);
  VRTComplexSource csBadNaN;
  {
    CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
    EXPECT_EQ(csBadNaN.XMLInit(oBadNaNDoc.get(), nullptr, mapSharedSources),
              CE_Failure);
  }

  // XMLInit error: non-monotonic LUT
  const char* pszNonMonoXML =
      "<ComplexSource>"
      "  <SourceFilename relativeToVRT=\"0\">foo.tif</SourceFilename>"
      "  <LUT>10:0,5:50,20:100</LUT>"
      "</ComplexSource>";
  CPLXMLTreeCloser oNonMonoDoc(CPLParseXMLString(pszNonMonoXML));
  ASSERT_NE(oNonMonoDoc.get(), nullptr);
  VRTComplexSource csNonMono;
  {
    CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
    EXPECT_EQ(csNonMono.XMLInit(oNonMonoDoc.get(), nullptr, mapSharedSources),
              CE_Failure);
  }
}

// ---------------------------------------------------------------------------
// VRTComplexSource RasterIO Operations
// ---------------------------------------------------------------------------

TEST_F(VRTSourcesTest, ComplexSourceRasterIOVariants) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  TrackFile("/vsimem/cs_rio_src.tif");
  auto poSrc = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/cs_rio_src.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poSrc, nullptr);
  std::vector<GByte> abyData(400);
  for (int i = 0; i < 400; ++i) abyData[i] = static_cast<GByte>(i % 50);
  EXPECT_EQ(
      poSrc->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 20, 20, abyData.data(),
                                        20, 20, GDT_Byte, 1, 20, nullptr),
      CE_None);
  poSrc.reset();

  VRTSource::WorkingState oWorkingState;

  // 1. Only NODATA processing flag (RasterIOProcessNoData)
  {
    VRTComplexSource cs;
    cs.SetSrcBand("/vsimem/cs_rio_src.tif", 1);
    cs.SetSrcWindow(0, 0, 20, 20);
    cs.SetDstWindow(0, 0, 20, 20);
    cs.SetNoDataValue(0.0);

    std::vector<GByte> abyOut(400, 255);
    EXPECT_EQ(cs.RasterIO(GDT_Byte, 0, 0, 20, 20, abyOut.data(), 20, 20,
                          GDT_Byte, 1, 20, nullptr, oWorkingState),
              CE_None);
    EXPECT_EQ(abyOut[0], 255);  // Source 0 was nodata, buffer untouched!
    EXPECT_EQ(abyOut[1], 1);
  }

  // 2. Linear scaling (val * 2 + 10)
  {
    VRTComplexSource cs;
    cs.SetSrcBand("/vsimem/cs_rio_src.tif", 1);
    cs.SetSrcWindow(0, 0, 20, 20);
    cs.SetDstWindow(0, 0, 20, 20);
    cs.SetLinearScaling(10.0, 2.0);

    std::vector<float> afOut(400, 0);
    EXPECT_EQ(cs.RasterIO(GDT_Byte, 0, 0, 20, 20, afOut.data(), 20, 20,
                          GDT_Float32, 4, 80, nullptr, oWorkingState),
              CE_None);
    EXPECT_FLOAT_EQ(afOut[0], 10.0f);  // 0 * 2 + 10 = 10
    EXPECT_FLOAT_EQ(afOut[1], 12.0f);  // 1 * 2 + 10 = 12
  }

  // 3. Constant value optimization (scaleRatio == 0 && !bNoDataSet)
  {
    VRTComplexSource cs;
    cs.SetSrcBand("/vsimem/cs_rio_src.tif", 1);
    cs.SetSrcWindow(0, 0, 20, 20);
    cs.SetDstWindow(0, 0, 20, 20);
    cs.SetLinearScaling(255.0, 0.0);

    std::vector<GByte> abyOut(400, 0);
    EXPECT_EQ(cs.RasterIO(GDT_Byte, 0, 0, 20, 20, abyOut.data(), 20, 20,
                          GDT_Byte, 1, 20, nullptr, oWorkingState),
              CE_None);
    EXPECT_EQ(abyOut[0], 255);
    EXPECT_EQ(abyOut[399], 255);
  }

  // 4. Power scaling (exponent 2.0, src 0..50 -> dst 0..100)
  {
    VRTComplexSource cs;
    cs.SetSrcBand("/vsimem/cs_rio_src.tif", 1);
    cs.SetSrcWindow(0, 0, 20, 20);
    cs.SetDstWindow(0, 0, 20, 20);
    cs.SetPowerScaling(2.0, 0.0, 50.0, 0.0, 100.0, /*bClip=*/true);

    std::vector<float> afOut(400, 0);
    EXPECT_EQ(cs.RasterIO(GDT_Byte, 0, 0, 20, 20, afOut.data(), 20, 20,
                          GDT_Float32, 4, 80, nullptr, oWorkingState),
              CE_None);
    EXPECT_FLOAT_EQ(afOut[0], 0.0f);
  }

  // 5. LUT transformation
  {
    VRTMapSharedResources mapSharedSources;
    const char* pszXML =
        "<ComplexSource>"
        "  <SourceFilename "
        "relativeToVRT=\"0\">/vsimem/cs_rio_src.tif</SourceFilename>"
        "  <SourceBand>1</SourceBand>"
        "  <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>"
        "  <DstRect xOff=\"0\" yOff=\"0\" xSize=\"20\" ySize=\"20\"/>"
        "  <LUT>0:0,25:50,50:100</LUT>"
        "</ComplexSource>";
    CPLXMLTreeCloser oTree(CPLParseXMLString(pszXML));
    ASSERT_NE(oTree.get(), nullptr);
    VRTComplexSource cs;
    EXPECT_EQ(cs.XMLInit(oTree.get(), nullptr, mapSharedSources), CE_None);

    std::vector<float> afOut(400, 0);
    EXPECT_EQ(cs.RasterIO(GDT_Byte, 0, 0, 20, 20, afOut.data(), 20, 20,
                          GDT_Float32, 4, 80, nullptr, oWorkingState),
              CE_None);
    EXPECT_FLOAT_EQ(afOut[0], 0.0f);
  }

  // 6. Color table component expansion
  {
    TrackFile("/vsimem/cs_ct_src.tif");
    auto poCTDS = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/cs_ct_src.tif", 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(poCTDS, nullptr);
    GDALColorTable oCT;
    GDALColorEntry sEntry = {10, 20, 30, 255};
    oCT.SetColorEntry(1, &sEntry);
    poCTDS->GetRasterBand(1)->SetColorTable(&oCT);
    std::vector<GByte> abyCTData(100, 1);
    EXPECT_EQ(poCTDS->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 10, 10,
                                                 abyCTData.data(), 10, 10,
                                                 GDT_Byte, 1, 10, nullptr),
              CE_None);
    poCTDS.reset();

    VRTComplexSource cs;
    cs.SetSrcBand("/vsimem/cs_ct_src.tif", 1);
    cs.SetSrcWindow(0, 0, 10, 10);
    cs.SetDstWindow(0, 0, 10, 10);
    cs.SetColorTableComponent(2);  // Green component (20)

    std::vector<GByte> abyOut(100, 0);
    EXPECT_EQ(cs.RasterIO(GDT_Byte, 0, 0, 10, 10, abyOut.data(), 10, 10,
                          GDT_Byte, 1, 10, nullptr, oWorkingState),
              CE_None);
    EXPECT_EQ(abyOut[0], 20);  // Green component extracted!
  }

  // 7. Complex data types (GDT_CFloat32)
  {
    TrackFile("/vsimem/cs_cfloat_src.tif");
    auto poCDataset = std::unique_ptr<GDALDataset>(poDrv->Create(
        "/vsimem/cs_cfloat_src.tif", 10, 10, 1, GDT_CFloat32, nullptr));
    ASSERT_NE(poCDataset, nullptr);
    std::vector<std::complex<float>> acData(100, {10.0f, 20.0f});
    EXPECT_EQ(poCDataset->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, 0, 10, 10, acData.data(), 10, 10, GDT_CFloat32,
                  8, 80, nullptr),
              CE_None);
    poCDataset.reset();

    VRTComplexSource cs;
    cs.SetSrcBand("/vsimem/cs_cfloat_src.tif", 1);
    cs.SetSrcWindow(0, 0, 10, 10);
    cs.SetDstWindow(0, 0, 10, 10);
    cs.SetLinearScaling(5.0, 2.0);

    std::vector<std::complex<float>> acOut(100);
    EXPECT_EQ(cs.RasterIO(GDT_CFloat32, 0, 0, 10, 10, acOut.data(), 10, 10,
                          GDT_CFloat32, 8, 80, nullptr, oWorkingState),
              CE_None);
    // Real: 10 * 2 + 5 = 25, Imag: 20 * 2 + 5 = 45
    EXPECT_FLOAT_EQ(acOut[0].real(), 25.0f);
    EXPECT_FLOAT_EQ(acOut[0].imag(), 45.0f);
  }

  // 8. Statistics forwarding when unchanged vs when changed
  {
    VRTComplexSource csUnchanged;
    csUnchanged.SetSrcBand("/vsimem/cs_rio_src.tif", 1);
    csUnchanged.SetSrcWindow(0, 0, 20, 20);
    csUnchanged.SetDstWindow(0, 0, 20, 20);
    int bSuccess = FALSE;
    csUnchanged.GetMinimum(20, 20, &bSuccess);
    csUnchanged.GetMaximum(20, 20, &bSuccess);

    VRTComplexSource csChanged;
    csChanged.SetSrcBand("/vsimem/cs_rio_src.tif", 1);
    csChanged.SetLinearScaling(10.0, 2.0);
    EXPECT_EQ(csChanged.GetMinimum(20, 20, &bSuccess), 0.0);
    EXPECT_FALSE(bSuccess);
    EXPECT_EQ(csChanged.GetMaximum(20, 20, &bSuccess), 0.0);
    EXPECT_FALSE(bSuccess);
    EXPECT_EQ(csChanged.GetHistogram(20, 20, 0, 255, 256, nullptr, FALSE, TRUE,
                                     nullptr, nullptr),
              CE_Failure);
  }
}

// ---------------------------------------------------------------------------
// VRTFuncSource Tests
// ---------------------------------------------------------------------------

TEST_F(VRTSourcesTest, FuncSourceRasterIOAndStats) {
  VRTFuncSource fs;
  EXPECT_STREQ(fs.GetType(), "FuncSource");
  EXPECT_EQ(fs.SerializeToXML(nullptr), nullptr);

  fs.pfnReadFunc = MyFuncSourceRead;
  fs.eType = GDT_Byte;

  VRTSource::WorkingState oWorkingState;

  // 1. Regular matching request
  std::vector<GByte> abyData(100, 0);
  EXPECT_EQ(fs.RasterIO(GDT_Byte, 0, 0, 10, 10, abyData.data(), 10, 10,
                        GDT_Byte, 1, 10, nullptr, oWorkingState),
            CE_None);
  EXPECT_EQ(abyData[0], 77);
  EXPECT_EQ(abyData[99], 77);

  // 2. Irregular request (e.g. buffer size mismatch) -> CE_Failure
  {
    CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
    EXPECT_EQ(fs.RasterIO(GDT_Byte, 0, 0, 10, 10, abyData.data(), 5, 5,
                          GDT_Byte, 1, 5, nullptr, oWorkingState),
              CE_Failure);
  }

  // 3. Minimum, Maximum, Histogram
  int bSuccess = FALSE;
  EXPECT_EQ(fs.GetMinimum(10, 10, &bSuccess), 0.0);
  EXPECT_FALSE(bSuccess);
  EXPECT_EQ(fs.GetMaximum(10, 10, &bSuccess), 0.0);
  EXPECT_FALSE(bSuccess);
  EXPECT_EQ(fs.GetHistogram(10, 10, 0, 255, 256, nullptr, FALSE, TRUE, nullptr,
                            nullptr),
            CE_Failure);
}

// ---------------------------------------------------------------------------
// Extended Tests for Complete Branch Coverage
// ---------------------------------------------------------------------------

TEST_F(VRTSourcesTest, SimpleSourceSetBandMaskAndSpecialSubdataset) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  TrackFile("/vsimem/vrt_subds_test.tif");
  auto poDS = std::unique_ptr<GDALDataset>(poDrv->Create(
      "/vsimem/vrt_subds_test.tif", 20, 20, 1, GDT_Byte, nullptr));
  ASSERT_NE(poDS, nullptr);
  poDS->CreateMaskBand(GMF_PER_DATASET);
  auto poBand = poDS->GetRasterBand(1);
  ASSERT_NE(poBand, nullptr);

  // SetSrcBand with live band
  TestSimpleSource ss;
  ss.m_bDropRefOnSrcBand = false;
  ss.SetSrcBand(poBand);
  EXPECT_EQ(ss.GetRasterBand(), poBand);

  // SetSourceDatasetName
  ss.SetSourceDatasetName("custom_name.tif", true);

  // SetSrcMaskBand with live band
  ss.SetSrcMaskBand(poBand);
  EXPECT_NE(ss.GetMaskBandMainBand(), nullptr);

  // Nested VRTDataset in SerializeToXML
  {
    auto poVRTDriver = GetGDALDriverManager()->GetDriverByName("VRT");
    ASSERT_NE(poVRTDriver, nullptr);
    TrackFile("/vsimem/nested.vrt");
    auto poVRT = std::unique_ptr<GDALDataset>(poVRTDriver->Create(
        "/vsimem/nested.vrt", 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(poVRT, nullptr);
    TestSimpleSource ssNested;
    ssNested.m_bDropRefOnSrcBand = false;
    ssNested.SetSrcBand(poVRT->GetRasterBand(1));
    ssNested.m_osSrcDSName.clear();
    CPLXMLTreeCloser oXML(ssNested.SerializeToXML("/vsimem"));
    EXPECT_NE(oXML.get(), nullptr);
  }

  // AddSourceFilenameNode with special syntax (NITF_IM, RASTERLITE, TILEDB)
  {
    CPLXMLTreeCloser oRoot(
        CPLCreateXMLNode(nullptr, CXT_Element, "SimpleSource"));

    TestSimpleSource ssNitf;
    ssNitf.m_bRelativeToVRTOri = -1;
    ssNitf.m_osSrcDSName = "NITF_IM:0:/vsimem/nonexistent.ntf";
    ssNitf.AddSourceFilenameNode("/vsimem", oRoot.get());

    TestSimpleSource ssNitfWin;
    ssNitfWin.m_bRelativeToVRTOri = -1;
    ssNitfWin.m_osSrcDSName = "NITF_IM:0:C:/nonexistent.ntf";
    ssNitfWin.AddSourceFilenameNode("/vsimem", oRoot.get());

    TestSimpleSource ssRasterlite;
    ssRasterlite.m_bRelativeToVRTOri = -1;
    ssRasterlite.m_osSrcDSName =
        "RASTERLITE:/vsimem/nonexistent.sqlite,table=layer";
    ssRasterlite.AddSourceFilenameNode("/vsimem", oRoot.get());

    TestSimpleSource ssRasterliteWin;
    ssRasterliteWin.m_bRelativeToVRTOri = -1;
    ssRasterliteWin.m_osSrcDSName =
        "RASTERLITE:C:/nonexistent.sqlite,table=layer";
    ssRasterliteWin.AddSourceFilenameNode("/vsimem", oRoot.get());

    TestSimpleSource ssTiledb;
    ssTiledb.m_bRelativeToVRTOri = -1;
    ssTiledb.m_osSrcDSName = "TILEDB:\"/vsimem/nonexistent\":layer";
    ssTiledb.AddSourceFilenameNode("/vsimem", oRoot.get());

    TestSimpleSource ssNetcdf;
    ssNetcdf.m_bRelativeToVRTOri = -1;
    ssNetcdf.m_osSrcDSName = "NETCDF:\"/vsimem/nonexistent.nc\":var";
    ssNetcdf.AddSourceFilenameNode("/vsimem", oRoot.get());

    TestSimpleSource ssSlow;
    ssSlow.m_bRelativeToVRTOri = -1;
    ssSlow.m_osSrcDSName = "/vsicurl/http://example.com/test.tif";
    ssSlow.AddSourceFilenameNode("/vsimem", oRoot.get());

    TrackFile("/vsimem/vrt_exist.tif");
    auto poExist = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/vrt_exist.tif", 1, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(poExist, nullptr);

    TestSimpleSource ssRelSrc;
    ssRelSrc.m_bRelativeToVRTOri = -1;
    ssRelSrc.m_osSrcDSName = "vrt_exist.tif";
    ssRelSrc.AddSourceFilenameNode("/vsimem/dir", oRoot.get());

    TestSimpleSource ssAbsSrc;
    ssAbsSrc.m_bRelativeToVRTOri = -1;
    ssAbsSrc.m_osSrcDSName = "/vsimem/vrt_exist.tif";
    ssAbsSrc.AddSourceFilenameNode("relative_dir", oRoot.get());

    TestSimpleSource ssRel;
    ssRel.SetSourceDatasetName("/vsimem/fake.tif", true);
    ssRel.AddSourceFilenameNode("/vsimem", oRoot.get());
  }
}

TEST_F(VRTSourcesTest, SimpleSourceWindowClampingAndFloatingPoint) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  TrackFile("/vsimem/vrt_clamp_src.tif");
  auto poDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/vrt_clamp_src.tif", 20, 20, 2, GDT_Byte, nullptr));
  ASSERT_NE(poDS, nullptr);
  poDS->Reference();  // For ss

  std::vector<GByte> abyInit(20 * 20, 120);
  EXPECT_EQ(poDS->GetRasterBand(1)->RasterIO(
                GF_Write, 0, 0, 20, 20, abyInit.data(), 20, 20, GDT_Byte, 0, 0),
            CE_None);
  EXPECT_EQ(poDS->GetRasterBand(2)->RasterIO(
                GF_Write, 0, 0, 20, 20, abyInit.data(), 20, 20, GDT_Byte, 0, 0),
            CE_None);

  TestSimpleSource ss;
  ss.SetSrcBand(poDS->GetRasterBand(1));
  ss.SetSrcWindow(0, 0, 20, 20);
  ss.SetDstWindow(0, 0, 20, 20);

  double dfReqXOff = 0, dfReqYOff = 0, dfReqXSize = 0, dfReqYSize = 0;
  int nReqXOff = 0, nReqYOff = 0, nReqXSize = 0, nReqYSize = 0;
  int nOutXOff = 0, nOutYOff = 0, nOutXSize = 0, nOutYSize = 0;
  bool bError = false;

  // 1. ONE_MINUS_EPS trigger
  EXPECT_TRUE(ss.GetSrcDstWindow(
      0.99999999999, 0.99999999999, 10.0, 10.0, 10, 10, &dfReqXOff, &dfReqYOff,
      &dfReqXSize, &dfReqYSize, &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize,
      &nOutXOff, &nOutYOff, &nOutXSize, &nOutYSize, bError));

  // 2. Large window (> INT_MAX)
  EXPECT_FALSE(ss.GetSrcDstWindow(
      0.0, 0.0, 1e12, 1e12, 10, 10, &dfReqXOff, &dfReqYOff, &dfReqXSize,
      &dfReqYSize, &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
      &nOutYOff, &nOutXSize, &nOutYSize, bError));

  // 3. Small window width / height -> zero size clamped to 1
  EXPECT_TRUE(ss.GetSrcDstWindow(
      0.0, 0.0, 0.1, 0.1, 10, 10, &dfReqXOff, &dfReqYOff, &dfReqXSize,
      &dfReqYSize, &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
      &nOutYOff, &nOutXSize, &nOutYSize, bError));

  // 4. Overhang right and bottom edges
  EXPECT_TRUE(ss.GetSrcDstWindow(
      15.0, 15.0, 10.0, 10.0, 10, 10, &dfReqXOff, &dfReqYOff, &dfReqXSize,
      &dfReqYSize, &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
      &nOutYOff, &nOutXSize, &nOutYSize, bError));

  // 5. Completely outside -> returns FALSE
  EXPECT_FALSE(ss.GetSrcDstWindow(
      50.0, 50.0, 10.0, 10.0, 10, 10, &dfReqXOff, &dfReqYOff, &dfReqXSize,
      &dfReqYSize, &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
      &nOutYOff, &nOutXSize, &nOutYSize, bError));

  // 6. Overhang with modified bounds
  EXPECT_TRUE(ss.GetSrcDstWindow(
      0.0, 0.0, 30.0, 30.0, 5, 5, &dfReqXOff, &dfReqYOff, &dfReqXSize,
      &dfReqYSize, &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize, &nOutXOff,
      &nOutYOff, &nOutXSize, &nOutYSize, bError));

  // 7. RasterIO with bFloatingPointWindowValidity
  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);
  sExtraArg.bFloatingPointWindowValidity = TRUE;
  sExtraArg.dfXOff = 0.5;
  sExtraArg.dfYOff = 0.5;
  sExtraArg.dfXSize = 5.0;
  sExtraArg.dfYSize = 5.0;

  VRTSource::WorkingState oWorkingState;
  std::vector<GByte> abyBuf(25, 0);
  EXPECT_EQ(ss.RasterIO(GDT_Byte, 0, 0, 5, 5, abyBuf.data(), 5, 5, GDT_Byte, 1,
                        5, &sExtraArg, oWorkingState),
            CE_None);

  // 8. DatasetRasterIO with bFloatingPointWindowValidity and
  // NeedMaxValAdjustment
  ss.SetMaxValue(100);
  int panBandMap[2] = {1, 2};
  std::vector<GByte> abyDSBuf(50, 0);
  EXPECT_EQ(ss.DatasetRasterIO(poDS->GetRasterBand(1)->GetRasterDataType(), 0,
                               0, 5, 5, abyDSBuf.data(), 5, 5, GDT_Byte, 2,
                               panBandMap, 1, 5, 25, &sExtraArg),
            CE_None);
  // Verify max value clamping: 120 clamped to 100
  EXPECT_EQ(abyDSBuf[0], 100);

  // 9. VRTAveragedSource with bFloatingPointWindowValidity
  poDS->Reference();  // For as
  VRTAveragedSource as;
  as.SetSrcBand(poDS->GetRasterBand(1));
  as.SetSrcWindow(0, 0, 20, 20);
  as.SetDstWindow(0, 0, 20, 20);
  EXPECT_EQ(as.RasterIO(GDT_Byte, 0, 0, 5, 5, abyBuf.data(), 5, 5, GDT_Byte, 1,
                        5, &sExtraArg, oWorkingState),
            CE_None);
}

TEST_F(VRTSourcesTest, NoDataFromMaskSourceAdvanced) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  TrackFile("/vsimem/vrt_nd_adv.tif");
  auto poDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/vrt_nd_adv.tif", 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(poDS, nullptr);
  poDS->Reference();  // For nd
  poDS->Reference();  // For ndByte
  poDS->CreateMaskBand(GMF_PER_DATASET);
  auto poBand = poDS->GetRasterBand(1);
  ASSERT_NE(poBand, nullptr);

  std::vector<GByte> abyData = {10, 20, 30, 10, 50, 60, 70, 80, 90, 100};
  abyData.resize(100, 10);
  EXPECT_EQ(poBand->RasterIO(GF_Write, 0, 0, 10, 10, abyData.data(), 10, 10,
                             GDT_Byte, 0, 0),
            CE_None);
  std::vector<GByte> abyMask = {0, 255, 255, 0, 255, 255, 255, 255, 255, 255};
  abyMask.resize(100, 255);
  EXPECT_EQ(poBand->GetMaskBand()->RasterIO(
                GF_Write, 0, 0, 10, 10, abyMask.data(), 10, 10, GDT_Byte, 0, 0),
            CE_None);

  VRTNoDataFromMaskSource nd;
  nd.SetSrcBand(poBand);
  nd.SetSrcWindow(0, 0, 10, 10);
  nd.SetDstWindow(0, 0, 10, 10);

  // 1. SetParameters 3-arg overload
  nd.SetParameters(10.0, 0.0, 99.0);

  // 2. RasterIO with bFloatingPointWindowValidity and eBufType = GDT_Float32
  GDALRasterIOExtraArg sExtraArg;
  INIT_RASTERIO_EXTRA_ARG(sExtraArg);
  sExtraArg.bFloatingPointWindowValidity = TRUE;
  sExtraArg.dfXOff = 0.0;
  sExtraArg.dfYOff = 0.0;
  sExtraArg.dfXSize = 10.0;
  sExtraArg.dfYSize = 10.0;

  VRTSource::WorkingState oWorkingState;
  std::vector<float> afBuf(100, 0.0f);
  EXPECT_EQ(
      nd.RasterIO(GDT_Byte, 0, 0, 10, 10, afBuf.data(), 10, 10, GDT_Float32,
                  sizeof(float), 10 * sizeof(float), &sExtraArg, oWorkingState),
      CE_None);
  EXPECT_FLOAT_EQ(afBuf[0], 10.0f);  // mask was 0 -> nodata (10.0)
  EXPECT_FLOAT_EQ(afBuf[1], 20.0f);  // mask was 255 -> value 20.0

  // 3. Auto-remapping for Byte, UInt16, Int16 without remapped value
  {
    // Byte nodata = 10 (!= 255) -> remapped = 11
    VRTNoDataFromMaskSource ndByte;
    ndByte.SetSrcBand(poBand);
    ndByte.SetSrcWindow(0, 0, 10, 10);
    ndByte.SetDstWindow(0, 0, 10, 10);
    ndByte.SetParameters(10.0, 0.0);
    std::vector<GByte> abyOut(100, 0);
    EXPECT_EQ(ndByte.RasterIO(GDT_Byte, 0, 0, 10, 10, abyOut.data(), 10, 10,
                              GDT_Byte, 1, 10, nullptr, oWorkingState),
              CE_None);

    // Byte nodata = 255 -> remapped = 254
    ndByte.SetParameters(255.0, 0.0);
    EXPECT_EQ(ndByte.RasterIO(GDT_Byte, 0, 0, 10, 10, abyOut.data(), 10, 10,
                              GDT_Byte, 1, 10, nullptr, oWorkingState),
              CE_None);
  }

  // 4. UInt16 and Int16 datasets for auto-remapping
  {
    TrackFile("/vsimem/vrt_nd_u16.tif");
    auto poDS16 = std::unique_ptr<GDALDataset>(poDrv->Create(
        "/vsimem/vrt_nd_u16.tif", 10, 10, 1, GDT_UInt16, nullptr));
    ASSERT_NE(poDS16, nullptr);
    poDS16->Reference();  // For ndU16
    poDS16->CreateMaskBand(GMF_PER_DATASET);
    VRTNoDataFromMaskSource ndU16;
    ndU16.SetSrcBand(poDS16->GetRasterBand(1));
    ndU16.SetSrcWindow(0, 0, 10, 10);
    ndU16.SetDstWindow(0, 0, 10, 10);

    ndU16.SetParameters(100.0, 0.0);
    std::vector<uint16_t> anOut(100, 0);
    EXPECT_EQ(ndU16.RasterIO(GDT_UInt16, 0, 0, 10, 10, anOut.data(), 10, 10,
                             GDT_UInt16, sizeof(uint16_t),
                             10 * sizeof(uint16_t), nullptr, oWorkingState),
              CE_None);

    ndU16.SetParameters(65535.0, 0.0);
    EXPECT_EQ(ndU16.RasterIO(GDT_UInt16, 0, 0, 10, 10, anOut.data(), 10, 10,
                             GDT_UInt16, sizeof(uint16_t),
                             10 * sizeof(uint16_t), nullptr, oWorkingState),
              CE_None);
  }
  {
    TrackFile("/vsimem/vrt_nd_s16.tif");
    auto poDS16 = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/vrt_nd_s16.tif", 10, 10, 1, GDT_Int16, nullptr));
    ASSERT_NE(poDS16, nullptr);
    poDS16->Reference();  // For ndS16
    poDS16->CreateMaskBand(GMF_PER_DATASET);
    VRTNoDataFromMaskSource ndS16;
    ndS16.SetSrcBand(poDS16->GetRasterBand(1));
    ndS16.SetSrcWindow(0, 0, 10, 10);
    ndS16.SetDstWindow(0, 0, 10, 10);

    ndS16.SetParameters(100.0, 0.0);
    std::vector<int16_t> anOut(100, 0);
    EXPECT_EQ(ndS16.RasterIO(GDT_Int16, 0, 0, 10, 10, anOut.data(), 10, 10,
                             GDT_Int16, sizeof(int16_t), 10 * sizeof(int16_t),
                             nullptr, oWorkingState),
              CE_None);

    ndS16.SetParameters(32767.0, 0.0);
    EXPECT_EQ(ndS16.RasterIO(GDT_Int16, 0, 0, 10, 10, anOut.data(), 10, 10,
                             GDT_Int16, sizeof(int16_t), 10 * sizeof(int16_t),
                             nullptr, oWorkingState),
              CE_None);
  }

  // 5. SerializeToXML with float max
  {
    TrackFile("/vsimem/vrt_nd_f32.tif");
    auto poDSF32 = std::unique_ptr<GDALDataset>(poDrv->Create(
        "/vsimem/vrt_nd_f32.tif", 10, 10, 1, GDT_Float32, nullptr));
    ASSERT_NE(poDSF32, nullptr);
    poDSF32->Reference();  // For ndF32
    VRTNoDataFromMaskSource ndF32;
    ndF32.SetSrcBand(poDSF32->GetRasterBand(1));
    ndF32.SetParameters(std::numeric_limits<float>::max(), 0.0);
    CPLXMLTreeCloser oXML(ndF32.SerializeToXML("/vsimem"));
    EXPECT_NE(oXML.get(), nullptr);
  }

  // 6. Minimum, Maximum, Histogram
  int bSuccess = FALSE;
  EXPECT_EQ(nd.GetMinimum(10, 10, &bSuccess), 0.0);
  EXPECT_FALSE(bSuccess);
  EXPECT_EQ(nd.GetMaximum(10, 10, &bSuccess), 0.0);
  EXPECT_FALSE(bSuccess);
  EXPECT_EQ(nd.GetHistogram(10, 10, 0, 255, 256, nullptr, FALSE, TRUE, nullptr,
                            nullptr),
            CE_Failure);
}

TEST_F(VRTSourcesTest, ComplexSourceConstructorsLUTAndXML) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  TrackFile("/vsimem/vrt_cs_xml.tif");
  auto poDS = std::unique_ptr<GDALDataset>(
      poDrv->Create("/vsimem/vrt_cs_xml.tif", 10, 10, 1, GDT_Float32, nullptr));
  ASSERT_NE(poDS, nullptr);

  // 1. Scale constructor
  TestComplexSource cs1;
  cs1.m_bDropRefOnSrcBand = false;
  cs1.SetSrcBand(poDS->GetRasterBand(1));
  cs1.SetSrcWindow(0, 0, 10, 10);
  cs1.SetDstWindow(0, 0, 10, 10);
  cs1.SetLinearScaling(5.0, 2.0);
  cs1.SetPowerScaling(1.5, 0.0, 100.0, 0.0, 255.0);

  TestComplexSource csScale(&cs1, 2.0, 2.0);
  csScale.m_bDropRefOnSrcBand = false;
  EXPECT_STREQ(csScale.GetType(), "ComplexSource");

  // 2. SerializeToXML variants
  // - Close LUT entries
  TestComplexSource csLUT;
  csLUT.m_bDropRefOnSrcBand = false;
  csLUT.SetSrcBand(poDS->GetRasterBand(1));
  csLUT.SetSrcWindow(0, 0, 10, 10);
  csLUT.SetDstWindow(0, 0, 10, 10);
  csLUT.m_adfLUTInputs = {1.0, 1.0, 2.0, 2.0};
  csLUT.m_adfLUTOutputs = {10.0, 20.0, 30.0, 40.0};
  CPLXMLTreeCloser oXMLLUT(csLUT.SerializeToXML("/vsimem"));
  EXPECT_NE(oXMLLUT.get(), nullptr);

  // - Float max nodata
  TestComplexSource csF32;
  csF32.m_bDropRefOnSrcBand = false;
  csF32.SetSrcBand(poDS->GetRasterBand(1));
  csF32.SetSrcWindow(0, 0, 10, 10);
  csF32.SetDstWindow(0, 0, 10, 10);
  csF32.SetNoDataValue(std::numeric_limits<float>::max());
  CPLXMLTreeCloser oXMLF32(csF32.SerializeToXML("/vsimem"));
  EXPECT_NE(oXMLF32.get(), nullptr);

  // 3. XMLInit branches
  VRTMapSharedResources mapShared;
  {
    // Linear scaling XML
    CPLXMLTreeCloser oTree(CPLParseXMLString(
        "<ComplexSource>"
        "  <SourceFilename "
        "relativeToVRT=\"0\">/vsimem/vrt_cs_xml.tif</SourceFilename>"
        "  <SourceBand>1</SourceBand>"
        "  <ScaleOffset>10</ScaleOffset>"
        "  <ScaleRatio>2.5</ScaleRatio>"
        "</ComplexSource>"));
    ASSERT_NE(oTree.get(), nullptr);
    TestComplexSource csXML;
    EXPECT_EQ(csXML.XMLInit(oTree.get(), "/vsimem", mapShared), CE_None);
  }
  {
    // Exponential scaling XML with SrcMin / SrcMax
    CPLXMLTreeCloser oTree(CPLParseXMLString(
        "<ComplexSource>"
        "  <SourceFilename "
        "relativeToVRT=\"0\">/vsimem/vrt_cs_xml.tif</SourceFilename>"
        "  <SourceBand>1</SourceBand>"
        "  <Exponent>2.0</Exponent>"
        "  <SrcMin>0.0</SrcMin>"
        "  <SrcMax>100.0</SrcMax>"
        "  <DstMin>0.0</DstMin>"
        "  <DstMax>255.0</DstMax>"
        "</ComplexSource>"));
    ASSERT_NE(oTree.get(), nullptr);
    TestComplexSource csXML;
    EXPECT_EQ(csXML.XMLInit(oTree.get(), "/vsimem", mapShared), CE_None);
  }
  {
    // Invalid XML: unsorted LUT entries
    CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
    CPLXMLTreeCloser oTree(CPLParseXMLString(
        "<ComplexSource>"
        "  <SourceFilename "
        "relativeToVRT=\"0\">/vsimem/vrt_cs_xml.tif</SourceFilename>"
        "  <SourceBand>1</SourceBand>"
        "  <LUT>20:100,10:50</LUT>"
        "</ComplexSource>"));
    ASSERT_NE(oTree.get(), nullptr);
    TestComplexSource csXML;
    EXPECT_EQ(csXML.XMLInit(oTree.get(), "/vsimem", mapShared), CE_Failure);
  }
  {
    // Invalid XML: NaN LUT entry not at index 0
    CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
    CPLXMLTreeCloser oTree(CPLParseXMLString(
        "<ComplexSource>"
        "  <SourceFilename "
        "relativeToVRT=\"0\">/vsimem/vrt_cs_xml.tif</SourceFilename>"
        "  <SourceBand>1</SourceBand>"
        "  <LUT>10:100,nan:50</LUT>"
        "</ComplexSource>"));
    ASSERT_NE(oTree.get(), nullptr);
    TestComplexSource csXML;
    EXPECT_EQ(csXML.XMLInit(oTree.get(), "/vsimem", mapShared), CE_Failure);
  }
}

TEST_F(VRTSourcesTest, ComplexSourceRasterIOVariantsExtended) {
  auto poDrv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(poDrv, nullptr);

  // 1. RasterIOInternalNoProcessing on Byte with word optimization
  {
    TrackFile("/vsimem/vrt_cs_words.tif");
    auto poDS = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/vrt_cs_words.tif", 10, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(poDS, nullptr);
    // Pixels 0..3: all non-zero; Pixels 4..7: all zero (nodata); Pixels 8..9:
    // mixed / trailing
    std::vector<GByte> abyData = {1, 2, 3, 4, 0, 0, 0, 0, 5, 0};
    EXPECT_EQ(poDS->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, 0, 10, 1, abyData.data(), 10, 1, GDT_Byte, 0, 0),
              CE_None);

    TestComplexSource cs;
    cs.m_bDropRefOnSrcBand = false;
    cs.SetSrcBand(poDS->GetRasterBand(1));
    cs.SetSrcWindow(0, 0, 10, 1);
    cs.SetDstWindow(0, 0, 10, 1);
    cs.SetNoDataValue(0.0);

    VRTSource::WorkingState oWorkingState;
    std::vector<GByte> abyOut(10, 99);
    EXPECT_EQ(cs.RasterIO(GDT_Byte, 0, 0, 10, 1, abyOut.data(), 10, 1, GDT_Byte,
                          1, 10, nullptr, oWorkingState),
              CE_None);
    EXPECT_EQ(abyOut[0], 1);
    EXPECT_EQ(abyOut[3], 4);
    EXPECT_EQ(abyOut[4], 99);  // Nodata was skipped
    EXPECT_EQ(abyOut[8], 5);
    EXPECT_EQ(abyOut[9], 99);

    // Mixed word [1, 0, 2, 0]
    std::vector<GByte> abyMixed = {1, 0, 2, 0, 1, 0, 2, 0, 1, 0};
    EXPECT_EQ(
        poDS->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 10, 1, abyMixed.data(),
                                         10, 1, GDT_Byte, 0, 0),
        CE_None);
    std::fill(abyOut.begin(), abyOut.end(), 99);
    EXPECT_EQ(cs.RasterIO(GDT_Byte, 0, 0, 10, 1, abyOut.data(), 10, 1, GDT_Byte,
                          1, 10, nullptr, oWorkingState),
              CE_None);
    EXPECT_EQ(abyOut[0], 1);
    EXPECT_EQ(abyOut[1], 99);

    // Direct non-lossy conversion: buffer GDT_Int32 with eVRTBandDataType =
    // GDT_Int16
    std::vector<int32_t> anOut32(10, 0);
    EXPECT_EQ(cs.RasterIO(GDT_Int16, 0, 0, 10, 1, anOut32.data(), 10, 1,
                          GDT_Int32, sizeof(int32_t), 10 * sizeof(int32_t),
                          nullptr, oWorkingState),
              CE_None);

    // Two-step lossy conversion: source GDT_Int16, eVRTBandDataType = GDT_Byte,
    // buffer GDT_Int32
    TrackFile("/vsimem/vrt_cs_s16.tif");
    auto poDS16 = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/vrt_cs_s16.tif", 5, 1, 1, GDT_Int16, nullptr));
    ASSERT_NE(poDS16, nullptr);
    std::vector<int16_t> anData16 = {10, 0, 20, 0, 30};
    EXPECT_EQ(poDS16->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, 0, 5, 1, anData16.data(), 5, 1, GDT_Int16, 0, 0),
              CE_None);

    TestComplexSource csLossy;
    csLossy.m_bDropRefOnSrcBand = false;
    csLossy.SetSrcBand(poDS16->GetRasterBand(1));
    csLossy.SetSrcWindow(0, 0, 5, 1);
    csLossy.SetDstWindow(0, 0, 5, 1);
    csLossy.SetNoDataValue(0.0);
    std::vector<int32_t> anOutLossy(5, 0);
    EXPECT_EQ(csLossy.RasterIO(GDT_Byte, 0, 0, 5, 1, anOutLossy.data(), 5, 1,
                               GDT_Int32, sizeof(int32_t), 5 * sizeof(int32_t),
                               nullptr, oWorkingState),
              CE_None);
  }

  // 2. RasterIOProcessNoData for Int16 and UInt16 with all destination types
  {
    TrackFile("/vsimem/vrt_cs_types.tif");
    auto poDS16 = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/vrt_cs_types.tif", 4, 1, 1, GDT_Int16, nullptr));
    ASSERT_NE(poDS16, nullptr);
    std::vector<int16_t> anData = {1, -999, 3, 4};
    EXPECT_EQ(poDS16->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, 0, 4, 1, anData.data(), 4, 1, GDT_Int16, 0, 0),
              CE_None);

    TestComplexSource cs16;
    cs16.m_bDropRefOnSrcBand = false;
    cs16.SetSrcBand(poDS16->GetRasterBand(1));
    cs16.SetSrcWindow(0, 0, 4, 1);
    cs16.SetDstWindow(0, 0, 4, 1);
    cs16.SetNoDataValue(-999.0);
    cs16.SetResampling("bilinear");

    VRTSource::WorkingState oWorkingState;
    // Test all destination types in CopyWord:
    // GDT_Int8
    int8_t nI8[4] = {0};
    EXPECT_EQ(cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nI8, 4, 1, GDT_Int8, 1, 4,
                            nullptr, oWorkingState),
              CE_None);
    // GDT_UInt16
    uint16_t nU16[4] = {0};
    EXPECT_EQ(cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nU16, 4, 1, GDT_UInt16, 2, 8,
                            nullptr, oWorkingState),
              CE_None);
    // GDT_Int16
    int16_t nI16[4] = {0};
    EXPECT_EQ(cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nI16, 4, 1, GDT_Int16, 2, 8,
                            nullptr, oWorkingState),
              CE_None);
    // GDT_UInt32
    uint32_t nU32[4] = {0};
    EXPECT_EQ(cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nU32, 4, 1, GDT_UInt32, 4,
                            16, nullptr, oWorkingState),
              CE_None);
    // GDT_Int32
    int32_t nI32[4] = {0};
    EXPECT_EQ(cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nI32, 4, 1, GDT_Int32, 4, 16,
                            nullptr, oWorkingState),
              CE_None);
    // GDT_UInt64
    uint64_t nU64[4] = {0};
    EXPECT_EQ(cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nU64, 4, 1, GDT_UInt64, 8,
                            32, nullptr, oWorkingState),
              CE_None);
    // GDT_Int64
    int64_t nI64[4] = {0};
    EXPECT_EQ(cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nI64, 4, 1, GDT_Int64, 8, 32,
                            nullptr, oWorkingState),
              CE_None);
    // GDT_Float32
    float f32[4] = {0};
    EXPECT_EQ(cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, f32, 4, 1, GDT_Float32, 4,
                            16, nullptr, oWorkingState),
              CE_None);
    // GDT_Float64
    double f64[4] = {0};
    EXPECT_EQ(cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, f64, 4, 1, GDT_Float64, 8,
                            32, nullptr, oWorkingState),
              CE_None);

    // Out of range nodata for Int16
    cs16.SetNoDataValue(1000000.0);
    EXPECT_EQ(cs16.RasterIO(GDT_Int16, 0, 0, 4, 1, nI16, 4, 1, GDT_Int16, 2, 8,
                            nullptr, oWorkingState),
              CE_None);

    // UInt16 dataset
    TrackFile("/vsimem/vrt_cs_u16.tif");
    auto poDSU16 = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/vrt_cs_u16.tif", 4, 1, 1, GDT_UInt16, nullptr));
    ASSERT_NE(poDSU16, nullptr);
    TestComplexSource csU16;
    csU16.m_bDropRefOnSrcBand = false;
    csU16.SetSrcBand(poDSU16->GetRasterBand(1));
    csU16.SetSrcWindow(0, 0, 4, 1);
    csU16.SetDstWindow(0, 0, 4, 1);
    csU16.SetNoDataValue(500.0);
    EXPECT_EQ(csU16.RasterIO(GDT_UInt16, 0, 0, 4, 1, nU16, 4, 1, GDT_UInt16, 2,
                             8, nullptr, oWorkingState),
              CE_None);
    // Out of range nodata for UInt16
    csU16.SetNoDataValue(-100.0);
    EXPECT_EQ(csU16.RasterIO(GDT_UInt16, 0, 0, 4, 1, nU16, 4, 1, GDT_UInt16, 2,
                             8, nullptr, oWorkingState),
              CE_None);
  }

  // 3. RasterIOInternal<double> for Int32 / Float64
  {
    TrackFile("/vsimem/vrt_cs_f64.tif");
    auto poDSF64 = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/vrt_cs_f64.tif", 2, 2, 1, GDT_Float64, nullptr));
    ASSERT_NE(poDSF64, nullptr);
    TestComplexSource csF64;
    csF64.m_bDropRefOnSrcBand = false;
    csF64.SetSrcBand(poDSF64->GetRasterBand(1));
    csF64.SetSrcWindow(0, 0, 2, 2);
    csF64.SetDstWindow(0, 0, 2, 2);
    csF64.SetLinearScaling(10.0, 2.0);

    VRTSource::WorkingState oWorkingState;
    std::vector<double> adfOut(4, 0.0);
    EXPECT_EQ(csF64.RasterIO(GDT_Float64, 0, 0, 2, 2, adfOut.data(), 2, 2,
                             GDT_Float64, sizeof(double), 2 * sizeof(double),
                             nullptr, oWorkingState),
              CE_None);
  }

  // 4. ColorTable component extraction
  {
    TrackFile("/vsimem/vrt_cs_ct.tif");
    auto poDSCT = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/vrt_cs_ct.tif", 2, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(poDSCT, nullptr);
    GDALColorTable oCT;
    GDALColorEntry oEntry1 = {10, 20, 30, 255};
    oCT.SetColorEntry(0, &oEntry1);
    poDSCT->GetRasterBand(1)->SetColorTable(&oCT);

    TestComplexSource csCT;
    csCT.m_bDropRefOnSrcBand = false;
    csCT.SetSrcBand(poDSCT->GetRasterBand(1));
    csCT.SetSrcWindow(0, 0, 2, 1);
    csCT.SetDstWindow(0, 0, 2, 1);

    VRTSource::WorkingState oWorkingState;
    std::vector<GByte> abyOut(2, 0);

    for (int comp = 1; comp <= 4; comp++) {
      csCT.SetColorTableComponent(comp);
      EXPECT_EQ(csCT.RasterIO(GDT_Byte, 0, 0, 2, 1, abyOut.data(), 2, 1,
                              GDT_Byte, 1, 2, nullptr, oWorkingState),
                CE_None);
    }
  }

  // 5. Exponential scaling with automatic min/max computation and clipping
  {
    TrackFile("/vsimem/vrt_cs_exp.tif");
    auto poDSExp = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/vrt_cs_exp.tif", 4, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(poDSExp, nullptr);
    std::vector<GByte> abyExp = {10, 20, 30, 40};
    EXPECT_EQ(poDSExp->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, 0, 4, 1, abyExp.data(), 4, 1, GDT_Byte, 0, 0),
              CE_None);

    TestComplexSource csExp;
    csExp.m_bDropRefOnSrcBand = false;
    csExp.SetSrcBand(poDSExp->GetRasterBand(1));
    csExp.SetSrcWindow(0, 0, 4, 1);
    csExp.SetDstWindow(0, 0, 4, 1);
    // Exponent 2.0 with clipping
    csExp.SetPowerScaling(2.0, 0.0, 100.0, 0.0, 255.0);
    // Unset min/max so ComputeRasterMinMax is invoked
    csExp.m_bSrcMinMaxDefined = false;

    VRTSource::WorkingState oWorkingState;
    std::vector<GByte> abyOut(4, 0);
    EXPECT_EQ(csExp.RasterIO(GDT_Byte, 0, 0, 4, 1, abyOut.data(), 4, 1,
                             GDT_Byte, 1, 4, nullptr, oWorkingState),
              CE_None);
  }

  // 6. Complex data types (CFloat32 / CFloat64)
  {
    TrackFile("/vsimem/vrt_cs_cpx.tif");
    auto poDSCpx = std::unique_ptr<GDALDataset>(poDrv->Create(
        "/vsimem/vrt_cs_cpx.tif", 2, 1, 1, GDT_CFloat32, nullptr));
    ASSERT_NE(poDSCpx, nullptr);

    TestComplexSource csCpx;
    csCpx.m_bDropRefOnSrcBand = false;
    csCpx.SetSrcBand(poDSCpx->GetRasterBand(1));
    csCpx.SetSrcWindow(0, 0, 2, 1);
    csCpx.SetDstWindow(0, 0, 2, 1);
    csCpx.SetLinearScaling(10.0, 2.0);

    VRTSource::WorkingState oWorkingState;
    std::vector<std::complex<float>> aCpxOut(2);
    EXPECT_EQ(
        csCpx.RasterIO(GDT_CFloat32, 0, 0, 2, 1, aCpxOut.data(), 2, 1,
                       GDT_CFloat32, sizeof(std::complex<float>),
                       2 * sizeof(std::complex<float>), nullptr, oWorkingState),
        CE_None);
  }

  // 7. Constant value writing optimization (m_dfScaleRatio == 0 && bNoDataSet
  // == FALSE)
  {
    TrackFile("/vsimem/vrt_cs_const.tif");
    auto poDSConst = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/vrt_cs_const.tif", 2, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(poDSConst, nullptr);

    TestComplexSource csConst;
    csConst.m_bDropRefOnSrcBand = false;
    csConst.SetSrcBand(poDSConst->GetRasterBand(1));
    csConst.SetSrcWindow(0, 0, 2, 1);
    csConst.SetDstWindow(0, 0, 2, 1);
    csConst.SetLinearScaling(50.0, 0.0);
    csConst.SetMaxValue(100);

    VRTSource::WorkingState oWorkingState;
    std::vector<GByte> abyOut(2, 0);
    EXPECT_EQ(csConst.RasterIO(GDT_Byte, 0, 0, 2, 1, abyOut.data(), 2, 1,
                               GDT_Byte, 1, 2, nullptr, oWorkingState),
              CE_None);
    EXPECT_EQ(abyOut[0], 50);
  }

  // 8. Two-step conversion eVRTBandDataType == GDT_Float32 && eBufType ==
  // GDT_Float64
  {
    TrackFile("/vsimem/vrt_cs_s32.tif");
    auto poDSS32 = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/vrt_cs_s32.tif", 2, 1, 1, GDT_Int32, nullptr));
    ASSERT_NE(poDSS32, nullptr);

    TestComplexSource csS32;
    csS32.m_bDropRefOnSrcBand = false;
    csS32.SetSrcBand(poDSS32->GetRasterBand(1));
    csS32.SetSrcWindow(0, 0, 2, 1);
    csS32.SetDstWindow(0, 0, 2, 1);
    csS32.SetLinearScaling(0.0, 1.0);

    VRTSource::WorkingState oWorkingState;
    std::vector<double> adfOut(2, 0.0);
    EXPECT_EQ(csS32.RasterIO(GDT_Float32, 0, 0, 2, 1, adfOut.data(), 2, 1,
                             GDT_Float64, sizeof(double), 2 * sizeof(double),
                             nullptr, oWorkingState),
              CE_None);
  }

  // 9. PROCESSING_FLAG_USE_MASK_BAND
  {
    TrackFile("/vsimem/vrt_cs_mask.tif");
    auto poDSMask = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/vrt_cs_mask.tif", 4, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(poDSMask, nullptr);
    poDSMask->CreateMaskBand(GMF_PER_DATASET);
    auto poMask = poDSMask->GetRasterBand(1)->GetMaskBand();
    GByte abyMask[4] = {255, 0, 255, 0};
    EXPECT_EQ(
        poMask->RasterIO(GF_Write, 0, 0, 4, 1, abyMask, 4, 1, GDT_Byte, 0, 0),
        CE_None);

    TestComplexSource csMask;
    csMask.m_bDropRefOnSrcBand = false;
    csMask.SetSrcBand(poDSMask->GetRasterBand(1));
    csMask.SetSrcWindow(0, 0, 4, 1);
    csMask.SetDstWindow(0, 0, 4, 1);
    csMask.SetLinearScaling(10.0, 1.0);
    csMask.SetUseMaskBand(true);

    VRTSource::WorkingState oWorkingState;
    std::vector<GByte> abyOut(4, 0);
    EXPECT_EQ(csMask.RasterIO(GDT_Byte, 0, 0, 4, 1, abyOut.data(), 4, 1,
                              GDT_Byte, 1, 4, nullptr, oWorkingState),
              CE_None);
  }

  // 10. Floating point window validity in extra args
  {
    TrackFile("/vsimem/vrt_cs_fp.tif");
    auto poDSFP = std::unique_ptr<GDALDataset>(
        poDrv->Create("/vsimem/vrt_cs_fp.tif", 4, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(poDSFP, nullptr);

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
    EXPECT_EQ(csFP.RasterIO(GDT_Byte, 0, 0, 4, 1, abyOut.data(), 4, 1, GDT_Byte,
                            1, 4, &sExtra, oWorkingState),
              CE_None);
  }

  // 11. Out-of-range nodata value for Byte (delegating to VRTSimpleSource)
  {
    TrackFile("/vsimem/vrt_cs_oor_byte.tif");
    auto poDSOOR = std::unique_ptr<GDALDataset>(poDrv->Create(
        "/vsimem/vrt_cs_oor_byte.tif", 4, 1, 1, GDT_Byte, nullptr));
    ASSERT_NE(poDSOOR, nullptr);

    TestComplexSource csOOR;
    csOOR.m_bDropRefOnSrcBand = false;
    csOOR.SetSrcBand(poDSOOR->GetRasterBand(1));
    csOOR.SetSrcWindow(0, 0, 4, 1);
    csOOR.SetDstWindow(0, 0, 4, 1);
    csOOR.SetNoDataValue(300.0);

    VRTSource::WorkingState oWorkingState;
    std::vector<GByte> abyOut(4, 0);
    EXPECT_EQ(csOOR.RasterIO(GDT_Byte, 0, 0, 4, 1, abyOut.data(), 4, 1,
                             GDT_Byte, 1, 4, nullptr, oWorkingState),
              CE_None);
  }
}

}  // namespace
