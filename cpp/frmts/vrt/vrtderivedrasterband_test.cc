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

// Tests for vrtderivedrasterband.cpp.

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
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

CPLErr SimpleInvertFunc(void** papoSources, int nSources, void* pData,
                        int nBufXSize, int nBufYSize, GDALDataType eSrcType,
                        GDALDataType eBufType, int nPixelSpace,
                        int nLineSpace) {
  if (nSources < 1 || !papoSources || !pData) return CE_Failure;
  const float* pSrc = static_cast<const float*>(papoSources[0]);
  float* pDst = static_cast<float*>(pData);
  for (int y = 0; y < nBufYSize; ++y) {
    for (int x = 0; x < nBufXSize; ++x) {
      pDst[y * nBufXSize + x] = 100.0f - pSrc[y * nBufXSize + x];
    }
  }
  return CE_None;
}

CPLErr FuncWithArgs(void** papoSources, int nSources, void* pData,
                    int nBufXSize, int nBufYSize, GDALDataType eSrcType,
                    GDALDataType eBufType, int nPixelSpace, int nLineSpace,
                    CSLConstList papszFunctionArgs) {
  if (nSources < 1 || !papoSources || !pData) return CE_Failure;
  const char* val = CSLFetchNameValue(papszFunctionArgs, "constant_arg");
  float add = val ? static_cast<float>(CPLAtof(val)) : 0.0f;
  const float* pSrc = static_cast<const float*>(papoSources[0]);
  float* pDst = static_cast<float*>(pData);
  for (int y = 0; y < nBufYSize; ++y) {
    for (int x = 0; x < nBufXSize; ++x) {
      pDst[y * nBufXSize + x] = pSrc[y * nBufXSize + x] + add;
    }
  }
  return CE_None;
}

class VrtDerivedRasterBandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    // Several tests rely on "simple_invert" being registered without
    // registering it themselves. Registration is idempotent (re-adding a name
    // replaces the previous entry and returns CE_None), so do it for every
    // test to avoid test-order dependencies.
    ASSERT_EQ(VRTDerivedRasterBand::AddPixelFunction("simple_invert",
                                                     SimpleInvertFunc),
              CE_None);
  }
};

TEST_F(VrtDerivedRasterBandTest, PixelFunctionRegistrationAndLookup) {
  EXPECT_EQ(VRTDerivedRasterBand::AddPixelFunction(nullptr, SimpleInvertFunc),
            CE_None);
  EXPECT_EQ(VRTDerivedRasterBand::AddPixelFunction("", SimpleInvertFunc),
            CE_None);
  EXPECT_EQ(VRTDerivedRasterBand::AddPixelFunction("test_func", nullptr),
            CE_None);

  EXPECT_EQ(VRTDerivedRasterBand::GetPixelFunction(nullptr), nullptr);
  EXPECT_EQ(VRTDerivedRasterBand::GetPixelFunction(""), nullptr);
  EXPECT_EQ(VRTDerivedRasterBand::GetPixelFunction("non_existent_func"),
            nullptr);

  // Register with function pointer only
  EXPECT_EQ(
      VRTDerivedRasterBand::AddPixelFunction("simple_invert", SimpleInvertFunc),
      CE_None);
  const auto* fn_pair = VRTDerivedRasterBand::GetPixelFunction("simple_invert");
  ASSERT_NE(fn_pair, nullptr);
  EXPECT_TRUE(fn_pair->first != nullptr);
  EXPECT_EQ(fn_pair->second, "");

  // Register with args and metadata
  const char* meta = "<PixelFunctionArgumentsList/>";
  EXPECT_EQ(
      VRTDerivedRasterBand::AddPixelFunction("func_args", FuncWithArgs, meta),
      CE_None);
  fn_pair = VRTDerivedRasterBand::GetPixelFunction("func_args");
  ASSERT_NE(fn_pair, nullptr);
  EXPECT_STREQ(fn_pair->second.c_str(), meta);

  // Test GetPixelFunctionNames
  std::vector<std::string> names =
      VRTDerivedRasterBand::GetPixelFunctionNames();
  EXPECT_FALSE(names.empty());
  EXPECT_NE(std::find(names.begin(), names.end(), "simple_invert"),
            names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "func_args"), names.end());

  VRTDerivedRasterBand::Cleanup();
}

TEST_F(VrtDerivedRasterBandTest, ConstructorsAndAccessors) {
  VRTDataset ds(20, 20);
  VRTDerivedRasterBand band1(&ds, 1);
  EXPECT_EQ(band1.GetRasterDataType(), GDT_Byte);
  EXPECT_EQ(band1.GetXSize(), 20);
  EXPECT_EQ(band1.GetYSize(), 20);

  VRTDerivedRasterBand band2(&ds, 2, GDT_Float32, 20, 20, 10, 10);
  EXPECT_EQ(band2.GetRasterDataType(), GDT_Float32);

  band2.SetPixelFunctionName("simple_invert");
  EXPECT_STREQ(band2.osFuncName.c_str(), "simple_invert");

  band2.AddPixelFunctionArgument("key1", "val1");
  band2.SetSkipNonContributingSources(true);
  band2.SetSourceTransferType(GDT_Float64);
  EXPECT_EQ(band2.eSourceTransferType, GDT_Float64);

  band2.SetPixelFunctionLanguage("C");

  // Raster statistics delegators
  int success = 0;
  band2.GetMinimum(&success);
  band2.GetMaximum(&success);
  double min_max[2];
  band2.ComputeRasterMinMax(FALSE, min_max);
  double min, max, mean, stddev;
  band2.ComputeStatistics(FALSE, &min, &max, &mean, &stddev, nullptr, nullptr);
  GUIntBig hist[10] = {0};
  band2.GetHistogram(0.0, 10.0, 10, hist, FALSE, FALSE, nullptr, nullptr);

  double pct = 0.0;
  int cov = band2.IGetDataCoverageStatus(0, 0, 10, 10, 0, &pct);
  EXPECT_TRUE(cov & GDAL_DATA_COVERAGE_STATUS_DATA);
  EXPECT_DOUBLE_EQ(pct, -1.0);
}

TEST_F(VrtDerivedRasterBandTest, XMLInitAndSerialize) {
  VRTDataset ds(20, 20);
  VRTMapSharedResources map_resources;

  // XMLInit missing PixelFunctionType
  const char* no_func_xml =
      "<VRTRasterBand dataType=\"Float32\" band=\"1\" "
      "subClass=\"VRTDerivedRasterBand\"/>";
  CPLXMLNode* tree = CPLParseXMLString(no_func_xml);
  VRTDerivedRasterBand b1(&ds, 1);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(b1.XMLInit(tree, nullptr, map_resources), CE_Failure);
  CPLPopErrorHandler();
  CPLDestroyXMLNode(tree);

  // XMLInit unsupported PixelFunctionLanguage
  const char* bad_lang_xml =
      "<VRTRasterBand dataType=\"Float32\" band=\"1\" "
      "subClass=\"VRTDerivedRasterBand\">"
      "  <PixelFunctionType>simple_invert</PixelFunctionType>"
      "  <PixelFunctionLanguage>Fortran</PixelFunctionLanguage>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(bad_lang_xml);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(b1.XMLInit(tree, nullptr, map_resources), CE_Failure);
  CPLPopErrorHandler();
  CPLDestroyXMLNode(tree);

  // XMLInit PixelFunctionCode with C language
  const char* code_c_xml =
      "<VRTRasterBand dataType=\"Float32\" band=\"1\" "
      "subClass=\"VRTDerivedRasterBand\">"
      "  <PixelFunctionType>simple_invert</PixelFunctionType>"
      "  <PixelFunctionLanguage>C</PixelFunctionLanguage>"
      "  <PixelFunctionCode>some_code</PixelFunctionCode>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(code_c_xml);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(b1.XMLInit(tree, nullptr, map_resources), CE_Failure);
  CPLPopErrorHandler();
  CPLDestroyXMLNode(tree);

  // XMLInit BufferRadius invalid (< 0 or > 1024)
  const char* bad_radius_xml =
      "<VRTRasterBand dataType=\"Float32\" band=\"1\" "
      "subClass=\"VRTDerivedRasterBand\">"
      "  <PixelFunctionType>simple_invert</PixelFunctionType>"
      "  <PixelFunctionLanguage>Python</PixelFunctionLanguage>"
      "  <BufferRadius>-5</BufferRadius>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(bad_radius_xml);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(b1.XMLInit(tree, nullptr, map_resources), CE_Failure);
  CPLPopErrorHandler();
  CPLDestroyXMLNode(tree);

  // XMLInit BufferRadius != 0 with C language
  const char* radius_c_xml =
      "<VRTRasterBand dataType=\"Float32\" band=\"1\" "
      "subClass=\"VRTDerivedRasterBand\">"
      "  <PixelFunctionType>simple_invert</PixelFunctionType>"
      "  <PixelFunctionLanguage>C</PixelFunctionLanguage>"
      "  <BufferRadius>2</BufferRadius>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(radius_c_xml);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(b1.XMLInit(tree, nullptr, map_resources), CE_Failure);
  CPLPopErrorHandler();
  CPLDestroyXMLNode(tree);

  // Valid XMLInit with full features
  const char* valid_xml =
      "<VRTRasterBand dataType=\"Float32\" band=\"1\" "
      "subClass=\"VRTDerivedRasterBand\">"
      "  <PixelFunctionType>simple_invert</PixelFunctionType>"
      "  <PixelFunctionLanguage>C</PixelFunctionLanguage>"
      "  <PixelFunctionArguments k1=\"v1\" k2=\"v2\"/>"
      "  <SourceTransferType>Float64</SourceTransferType>"
      "  <SkipNonContributingSources>true</SkipNonContributingSources>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(valid_xml);
  VRTDerivedRasterBand b2(&ds, 1);
  EXPECT_EQ(b2.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);
  EXPECT_EQ(b2.eSourceTransferType, GDT_Float64);

  // Serialize to XML
  bool warned = false;
  size_t ram = 0;
  CPLXMLNode* serialized = b2.SerializeToXML(nullptr, warned, ram);
  ASSERT_NE(serialized, nullptr);
  char* str = CPLSerializeXMLTree(serialized);
  EXPECT_NE(strstr(str, "VRTDerivedRasterBand"), nullptr);
  EXPECT_NE(strstr(str, "simple_invert"), nullptr);
  EXPECT_NE(strstr(str, "k1=\"v1\""), nullptr);
  EXPECT_NE(strstr(str, "Float64"), nullptr);
  EXPECT_NE(strstr(str, "SkipNonContributingSources>true"), nullptr);
  CPLFree(str);
  CPLDestroyXMLNode(serialized);

  // Test Python serialization paths (language, code, buffer radius)
  b2.SetPixelFunctionLanguage("Python");
  b2.SetPixelFunctionName("py_func");
  b2.AddPixelFunctionArgument("arg_a", "val_a");
  const char* py_xml =
      "<VRTRasterBand dataType=\"Float32\" band=\"1\" "
      "subClass=\"VRTDerivedRasterBand\">"
      "  <PixelFunctionType>py_func</PixelFunctionType>"
      "  <PixelFunctionLanguage>Python</PixelFunctionLanguage>"
      "  <PixelFunctionCode><![CDATA[def py_func(): pass]]></PixelFunctionCode>"
      "  <BufferRadius>3</BufferRadius>"
      "  <SkipNonContributingSources>false</SkipNonContributingSources>"
      "</VRTRasterBand>";
  tree = CPLParseXMLString(py_xml);
  VRTDerivedRasterBand b3(&ds, 1);
  EXPECT_EQ(b3.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);

  serialized = b3.SerializeToXML(nullptr, warned, ram);
  ASSERT_NE(serialized, nullptr);
  str = CPLSerializeXMLTree(serialized);
  EXPECT_NE(strstr(str, "PixelFunctionLanguage>Python"), nullptr);
  EXPECT_NE(strstr(str, "BufferRadius>3"), nullptr);
  EXPECT_NE(strstr(str, "def py_func(): pass"), nullptr);
  CPLFree(str);
  CPLDestroyXMLNode(serialized);
}

TEST_F(VrtDerivedRasterBandTest, RasterIOExecution) {
  // Create an in-memory source dataset
  auto mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(mem_drv, nullptr);
  std::unique_ptr<GDALDataset> src_ds(
      mem_drv->Create("", 10, 10, 1, GDT_Float32, nullptr));
  ASSERT_NE(src_ds, nullptr);
  std::vector<float> src_data(100);
  for (int i = 0; i < 100; ++i) src_data[i] = static_cast<float>(i);
  ASSERT_EQ(src_ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 10, 10,
                                               src_data.data(), 10, 10,
                                               GDT_Float32, 0, 0, nullptr),
            CE_None);

  // Register function
  EXPECT_EQ(
      VRTDerivedRasterBand::AddPixelFunction("simple_invert", SimpleInvertFunc),
      CE_None);

  VRTDataset vrt_ds(10, 10);
  auto derived_band =
      std::make_unique<VRTDerivedRasterBand>(&vrt_ds, 1, GDT_Float32, 10, 10);
  derived_band->SetPixelFunctionName("simple_invert");

  // Add source to derived band
  auto src = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src->SetSrcBand(src_ds->GetRasterBand(1));
  src->SetSrcWindow(0, 0, 10, 10);
  src->SetDstWindow(0, 0, 10, 10);
  derived_band->AddSource(src.release());

  // Error: Write is not supported
  std::vector<float> dst_data(100, 0.0f);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(derived_band->RasterIO(GF_Write, 0, 0, 10, 10, dst_data.data(), 10,
                                   10, GDT_Float32, 0, 0, nullptr),
            CE_Failure);
  CPLPopErrorHandler();

  // Success: Read through derived band
  EXPECT_EQ(derived_band->RasterIO(GF_Read, 0, 0, 10, 10, dst_data.data(), 10,
                                   10, GDT_Float32, 0, 0, nullptr),
            CE_None);
  for (int i = 0; i < 100; ++i) {
    EXPECT_FLOAT_EQ(dst_data[i], 100.0f - static_cast<float>(i));
  }

  // Test with unregistered pixel function
  derived_band->SetPixelFunctionName("unregistered_func");
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(derived_band->RasterIO(GF_Read, 0, 0, 10, 10, dst_data.data(), 10,
                                   10, GDT_Float32, 0, 0, nullptr),
            CE_Failure);
  CPLPopErrorHandler();

  // Test with SourceTransferType set
  derived_band->SetPixelFunctionName("simple_invert");
  derived_band->SetSourceTransferType(GDT_Float32);
  EXPECT_EQ(derived_band->RasterIO(GF_Read, 0, 0, 10, 10, dst_data.data(), 10,
                                   10, GDT_Float32, 0, 0, nullptr),
            CE_None);

  // Test with RAM limit triggering SplitRasterIO
  CPLSetConfigOption("VRT_DERIVED_DATASET_ALLOWED_RAM_USAGE", "20");
  VRTDerivedRasterBand ram_band(&vrt_ds, 2, GDT_Float32, 10, 10);
  ram_band.SetPixelFunctionName("simple_invert");
  auto src2 = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src2->SetSrcBand(src_ds->GetRasterBand(1));
  src2->SetSrcWindow(0, 0, 10, 10);
  src2->SetDstWindow(0, 0, 10, 10);
  ram_band.AddSource(src2.release());
  EXPECT_EQ(ram_band.RasterIO(GF_Read, 0, 0, 10, 10, dst_data.data(), 10, 10,
                              GDT_Float32, 0, 0, nullptr),
            CE_None);
  CPLSetConfigOption("VRT_DERIVED_DATASET_ALLOWED_RAM_USAGE", nullptr);
}

TEST_F(VrtDerivedRasterBandTest, PixelFunctionArgumentsAndBuiltins) {
  auto mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> src_ds(
      mem_drv->Create("", 10, 10, 1, GDT_Float32, nullptr));
  std::vector<float> src_data(100, 5.0f);
  ASSERT_EQ(src_ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 10, 10,
                                               src_data.data(), 10, 10,
                                               GDT_Float32, 0, 0, nullptr),
            CE_None);

  const char* meta =
      "<PixelFunctionArgumentsList>"
      "  <Argument name=\"constant_arg\" type=\"constant\" value=\"10\"/>"
      "  <Argument name=\"nodata_arg\" type=\"builtin\" value=\"NoData\" "
      "optional=\"true\"/>"
      "  <Argument name=\"scale_arg\" type=\"builtin\" value=\"scale\" "
      "optional=\"true\"/>"
      "  <Argument name=\"offset_arg\" type=\"builtin\" value=\"offset\" "
      "optional=\"true\"/>"
      "  <Argument name=\"xoff_arg\" type=\"builtin\" value=\"xoff\" "
      "optional=\"true\"/>"
      "  <Argument name=\"yoff_arg\" type=\"builtin\" value=\"yoff\" "
      "optional=\"true\"/>"
      "  <Argument name=\"gt_arg\" type=\"builtin\" value=\"geotransform\" "
      "optional=\"true\"/>"
      "  <Argument name=\"names_arg\" type=\"builtin\" value=\"source_names\" "
      "optional=\"true\"/>"
      "</PixelFunctionArgumentsList>";

  EXPECT_EQ(
      VRTDerivedRasterBand::AddPixelFunction("func_args", FuncWithArgs, meta),
      CE_None);

  VRTDataset vrt_ds(10, 10);
  GDALGeoTransform gt = {100.0, 1.0, 0.0, 200.0, 0.0, -1.0};
  vrt_ds.SetGeoTransform(gt);

  VRTDerivedRasterBand band(&vrt_ds, 1, GDT_Float32, 10, 10);
  band.SetPixelFunctionName("func_args");
  band.SetNoDataValue(-999.0);
  band.SetScale(2.0);
  band.SetOffset(1.0);

  auto src = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src->SetSrcBand(src_ds->GetRasterBand(1));
  src->SetSrcWindow(0, 0, 10, 10);
  src->SetDstWindow(0, 0, 10, 10);
  band.AddSource(src.release());

  std::vector<float> dst(100, 0.0f);
  EXPECT_EQ(band.RasterIO(GF_Read, 0, 0, 10, 10, dst.data(), 10, 10,
                          GDT_Float32, 0, 0, nullptr),
            CE_None);
  // 5.0 + 10 = 15.0
  EXPECT_FLOAT_EQ(dst[0], 15.0f);

  // Missing required builtin -> error
  const char* bad_meta =
      "<PixelFunctionArgumentsList>"
      "  <Argument name=\"nodata_arg\" type=\"builtin\" value=\"NoData\" "
      "optional=\"false\"/>"
      "</PixelFunctionArgumentsList>";
  EXPECT_EQ(VRTDerivedRasterBand::AddPixelFunction("bad_builtin", FuncWithArgs,
                                                   bad_meta),
            CE_None);
  VRTDerivedRasterBand band_bad(&vrt_ds, 2, GDT_Float32, 10, 10);
  band_bad.SetPixelFunctionName("bad_builtin");
  auto src2 = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src2->SetSrcBand(src_ds->GetRasterBand(1));
  src2->SetSrcWindow(0, 0, 10, 10);
  src2->SetDstWindow(0, 0, 10, 10);
  band_bad.AddSource(src2.release());

  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(band_bad.RasterIO(GF_Read, 0, 0, 10, 10, dst.data(), 10, 10,
                              GDT_Float32, 0, 0, nullptr),
            CE_Failure);
  CPLPopErrorHandler();

  // Unsupported builtin argument -> error
  const char* unsupported_meta =
      "<PixelFunctionArgumentsList>"
      "  <Argument name=\"unknown\" type=\"builtin\" "
      "value=\"unsupported_name\"/>"
      "</PixelFunctionArgumentsList>";
  EXPECT_EQ(VRTDerivedRasterBand::AddPixelFunction("unsupp", FuncWithArgs,
                                                   unsupported_meta),
            CE_None);
  band_bad.SetPixelFunctionName("unsupp");
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(band_bad.RasterIO(GF_Read, 0, 0, 10, 10, dst.data(), 10, 10,
                              GDT_Float32, 0, 0, nullptr),
            CE_Failure);
  CPLPopErrorHandler();
}

TEST_F(VrtDerivedRasterBandTest, SkipNonContributingSources) {
  auto mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> src_ds(
      mem_drv->Create("", 10, 10, 1, GDT_Float32, nullptr));

  VRTDataset vrt_ds(20, 20);
  VRTDerivedRasterBand band(&vrt_ds, 1, GDT_Float32, 20, 20);
  band.SetPixelFunctionName("simple_invert");
  band.SetSkipNonContributingSources(true);

  // Source covers (0, 0, 5, 5)
  auto src = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src->SetSrcBand(src_ds->GetRasterBand(1));
  src->SetSrcWindow(0, 0, 5, 5);
  src->SetDstWindow(0, 0, 5, 5);
  band.AddSource(src.release());

  std::vector<float> dst(25, 42.0f);
  // Request window (10, 10, 5, 5) -> source does not contribute!
  EXPECT_EQ(band.RasterIO(GF_Read, 10, 10, 5, 5, dst.data(), 5, 5, GDT_Float32,
                          0, 0, nullptr),
            CE_None);
  // Background value is 0
  EXPECT_FLOAT_EQ(dst[0], 0.0f);
}

TEST_F(VrtDerivedRasterBandTest, PythonInitializationErrorHandling) {
  VRTDataset vrt_ds(10, 10);
  VRTDerivedRasterBand band(&vrt_ds, 1, GDT_Float32, 10, 10);
  band.SetPixelFunctionName("py_func");
  band.SetPixelFunctionLanguage("Python");

  std::vector<float> dst(100, 0.0f);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(band.RasterIO(GF_Read, 0, 0, 10, 10, dst.data(), 10, 10,
                          GDT_Float32, 0, 0, nullptr),
            CE_Failure);
  CPLPopErrorHandler();

  // Test unsupported complex type with Python
  VRTDerivedRasterBand c_band(&vrt_ds, 2, GDT_CInt16, 10, 10);
  c_band.SetPixelFunctionName("py_func");
  c_band.SetPixelFunctionLanguage("Python");
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(c_band.RasterIO(GF_Read, 0, 0, 10, 10, dst.data(), 10, 10,
                            GDT_CInt16, 0, 0, nullptr),
            CE_Failure);
  CPLPopErrorHandler();

  band.SetSourceTransferType(GDT_CInt16);
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(band.RasterIO(GF_Read, 0, 0, 10, 10, dst.data(), 10, 10,
                          GDT_Float32, 0, 0, nullptr),
            CE_Failure);
  CPLPopErrorHandler();
}

TEST_F(VrtDerivedRasterBandTest, RasterIOEdgeCases) {
  auto mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> src_ds(
      mem_drv->Create("", 10, 10, 1, GDT_Float32, nullptr));

  VRTDataset vrt_ds(10, 10);
  VRTDerivedRasterBand band(&vrt_ds, 1, GDT_Float32, 10, 10);
  band.SetPixelFunctionName("simple_invert");
  auto src = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src->SetSrcBand(src_ds->GetRasterBand(1));
  src->SetSrcWindow(0, 0, 10, 10);
  src->SetDstWindow(0, 0, 10, 10);
  band.AddSource(src.release());

  // LineSpace overflow check
  std::vector<float> buf(100, 0.0f);
  GSpacing huge_linespace = static_cast<GSpacing>(INT_MAX) + 100;
  // nBufYSize == 1 -> resets nLineSpace to 0
  EXPECT_EQ(band.RasterIO(GF_Read, 0, 0, 10, 1, buf.data(), 10, 1, GDT_Float32,
                          sizeof(float), huge_linespace, nullptr),
            CE_None);

  // nBufYSize > 1 -> fails
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(band.RasterIO(GF_Read, 0, 0, 10, 2, buf.data(), 10, 2, GDT_Float32,
                          sizeof(float), huge_linespace, nullptr),
            CE_Failure);
  CPLPopErrorHandler();

  // Source band is null -> source RasterIO returns error
  VRTDerivedRasterBand band_null_src(&vrt_ds, 2, GDT_Float32, 10, 10);
  band_null_src.SetPixelFunctionName("simple_invert");
  auto null_src = std::make_unique<VRTSimpleSource>();
  band_null_src.AddSource(null_src.release());
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(band_null_src.RasterIO(GF_Read, 0, 0, 10, 10, buf.data(), 10, 10,
                                   GDT_Float32, 0, 0, nullptr),
            CE_Failure);
  CPLPopErrorHandler();

  // Complex source without band
  VRTDerivedRasterBand band_complex_src(&vrt_ds, 3, GDT_Float32, 10, 10);
  band_complex_src.SetPixelFunctionName("simple_invert");
  auto complex_src = std::make_unique<VRTComplexSource>();
  band_complex_src.AddSource(complex_src.release());
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(band_complex_src.RasterIO(GF_Read, 0, 0, 10, 10, buf.data(), 10, 10,
                                      GDT_Float32, 0, 0, nullptr),
            CE_Failure);
  CPLPopErrorHandler();

  // Buffer initialization with non-zero NoData
  VRTDerivedRasterBand band_nodata(&vrt_ds, 4, GDT_Float32, 10, 10);
  band_nodata.SetPixelFunctionName("simple_invert");
  band_nodata.SetNoDataValue(-999.0);
  auto partial_src = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  partial_src->SetSrcBand(src_ds->GetRasterBand(1));
  partial_src->SetSrcWindow(0, 0, 5, 5);
  partial_src->SetDstWindow(2, 2, 5, 5);
  band_nodata.AddSource(partial_src.release());
  EXPECT_EQ(band_nodata.RasterIO(GF_Read, 0, 0, 10, 10, buf.data(), 10, 10,
                                 GDT_Float32, sizeof(float), 10 * sizeof(float),
                                 nullptr),
            CE_None);
}

TEST_F(VrtDerivedRasterBandTest, ExtraMetadataArguments) {
  auto mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> src_ds(
      mem_drv->Create("", 10, 10, 1, GDT_Float32, nullptr));

  const char* meta =
      "<PixelFunctionArgumentsList>"
      "  <Argument name=\"gt_arg\" type=\"builtin\" value=\"geotransform\" "
      "optional=\"true\"/>"
      "  <Argument name=\"nodata_arg\" type=\"builtin\" value=\"NoData\" "
      "optional=\"true\"/>"
      "  <Argument name=\"names_arg\" type=\"builtin\" value=\"source_names\" "
      "optional=\"true\"/>"
      "</PixelFunctionArgumentsList>";

  EXPECT_EQ(
      VRTDerivedRasterBand::AddPixelFunction("multi_meta", FuncWithArgs, meta),
      CE_None);

  // Dataset WITHOUT geotransform
  VRTDataset vrt_ds(10, 10);
  VRTDerivedRasterBand band(&vrt_ds, 1, GDT_Float32, 10, 10);
  band.SetPixelFunctionName("multi_meta");

  // Multiple sources, some named, some unnamed
  auto src1 = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src1->SetSrcBand(src_ds->GetRasterBand(1));
  src1->SetName("source_one");
  band.AddSource(src1.release());

  auto src2 = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src2->SetSrcBand(src_ds->GetRasterBand(1));
  band.AddSource(src2.release());

  std::vector<float> buf(100, 0.0f);
  EXPECT_EQ(band.RasterIO(GF_Read, 0, 0, 10, 10, buf.data(), 10, 10,
                          GDT_Float32, 0, 0, nullptr),
            CE_None);
}

}  // namespace
}  // namespace autotest2
