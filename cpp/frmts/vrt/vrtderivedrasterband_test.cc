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
// Tests VRTDerivedRasterBand C pixel function registration, XML serialization,
// built-in metadata arguments, RAM-limit splitting, and Python stub paths
// (vrtderivedrasterband.cpp).
//
// google3 build caveat:
//   //third_party/gdal/BUILD defines GDAL_VRT_DISABLE_PYTHON, so
//   GDALPythonInitialize() returns false at runtime even though Python C API
//   calls in InitializePython() and IRasterIO() are compiled into the object.
//   Overall line coverage of vrtderivedrasterband.cpp is therefore ~59%
//   (covering ~99% of reachable C/C++ lines outside the Python execution
//   block).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/vrtderivedrasterband.cpp
//   https://gdal.org/drivers/raster/vrt.html

#include <climits>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;

CPLErr SimpleInvertFunc(void** papoSources, int nSources, void* pData,
                        int nBufXSize, int nBufYSize, GDALDataType /*eSrcType*/,
                        GDALDataType /*eBufType*/, int /*nPixelSpace*/,
                        int /*nLineSpace*/) {
  if (nSources < 1 || papoSources == nullptr || pData == nullptr) {
    return CE_Failure;
  }
  const float* pSrc = static_cast<const float*>(papoSources[0]);
  float* pDst = static_cast<float*>(pData);
  for (int y = 0; y < nBufYSize; ++y) {
    for (int x = 0; x < nBufXSize; ++x) {
      pDst[y * nBufXSize + x] = 100.0f - pSrc[y * nBufXSize + x];
    }
  }
  return CE_None;
}

CPLStringList* GetLastFuncArgs() {
  static auto* args = new CPLStringList();
  return args;
}

CPLErr FuncWithArgs(void** papoSources, int nSources, void* pData,
                    int nBufXSize, int nBufYSize, GDALDataType /*eSrcType*/,
                    GDALDataType /*eBufType*/, int /*nPixelSpace*/,
                    int /*nLineSpace*/, CSLConstList papszFunctionArgs) {
  GetLastFuncArgs()->Assign(CSLDuplicate(papszFunctionArgs));
  if (nSources < 1 || papoSources == nullptr || pData == nullptr) {
    return CE_Failure;
  }
  const char* val = CSLFetchNameValue(papszFunctionArgs, "constant_arg");
  const float add = val != nullptr ? static_cast<float>(CPLAtof(val)) : 0.0f;
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
    GDALRegister_VRT();
    GDALRegister_MEM();
    ASSERT_EQ(CE_None, VRTDerivedRasterBand::AddPixelFunction(
                           "simple_invert", SimpleInvertFunc));
  }

  void TearDown() override { GetLastFuncArgs()->Clear(); }
};

TEST_F(VrtDerivedRasterBandTest, PixelFunctionRegistrationAndLookup) {
  EXPECT_EQ(CE_None,
            VRTDerivedRasterBand::AddPixelFunction(nullptr, SimpleInvertFunc));
  EXPECT_EQ(CE_None,
            VRTDerivedRasterBand::AddPixelFunction("", SimpleInvertFunc));
  EXPECT_EQ(CE_None,
            VRTDerivedRasterBand::AddPixelFunction("test_func", nullptr));
  EXPECT_EQ(nullptr, VRTDerivedRasterBand::GetPixelFunction("test_func"));

  EXPECT_EQ(nullptr, VRTDerivedRasterBand::GetPixelFunction(nullptr));
  EXPECT_EQ(nullptr, VRTDerivedRasterBand::GetPixelFunction(""));
  EXPECT_EQ(nullptr, VRTDerivedRasterBand::GetPixelFunction("non_existent"));

  const auto* fn_pair = VRTDerivedRasterBand::GetPixelFunction("simple_invert");
  ASSERT_NE(nullptr, fn_pair);
  EXPECT_TRUE(fn_pair->first != nullptr);
  EXPECT_EQ("", fn_pair->second);

  constexpr char kMeta[] = "<PixelFunctionArgumentsList/>";
  EXPECT_EQ(CE_None, VRTDerivedRasterBand::AddPixelFunction(
                         "func_args", FuncWithArgs, kMeta));
  fn_pair = VRTDerivedRasterBand::GetPixelFunction("func_args");
  ASSERT_NE(nullptr, fn_pair);
  EXPECT_STREQ(kMeta, fn_pair->second.c_str());

  const std::vector<std::string> names =
      VRTDerivedRasterBand::GetPixelFunctionNames();
  EXPECT_THAT(names, Contains("simple_invert"));
  EXPECT_THAT(names, Contains("func_args"));

  VRTDerivedRasterBand::Cleanup();
}

TEST_F(VrtDerivedRasterBandTest, ConstructorsAndAccessors) {
  VRTDataset ds(20, 20);
  VRTDerivedRasterBand band1(&ds, 1);
  EXPECT_EQ(GDT_Byte, band1.GetRasterDataType());
  EXPECT_EQ(20, band1.GetXSize());
  EXPECT_EQ(20, band1.GetYSize());

  VRTDerivedRasterBand band2(&ds, 2, GDT_Float32, 20, 20, 10, 10);
  EXPECT_EQ(GDT_Float32, band2.GetRasterDataType());

  band2.SetPixelFunctionName("simple_invert");
  EXPECT_STREQ("simple_invert", band2.osFuncName.c_str());

  band2.AddPixelFunctionArgument("key1", "val1");
  band2.SetSkipNonContributingSources(true);
  band2.SetSourceTransferType(GDT_Float64);
  EXPECT_EQ(GDT_Float64, band2.eSourceTransferType);
  band2.SetPixelFunctionLanguage("C");

  int success = TRUE;
  band2.GetMinimum(&success);
  EXPECT_FALSE(success);
  success = TRUE;
  band2.GetMaximum(&success);
  EXPECT_FALSE(success);
  double min_max[2] = {};
  CPLErrorReset();
  EXPECT_EQ(CE_None, band2.ComputeRasterMinMax(FALSE, min_max));
  EXPECT_DOUBLE_EQ(0.0, min_max[0]);
  EXPECT_DOUBLE_EQ(0.0, min_max[1]);
  double min = 0.0, max = 0.0, mean = 0.0, stddev = 0.0;
  EXPECT_EQ(CE_None, band2.ComputeStatistics(FALSE, &min, &max, &mean, &stddev,
                                             nullptr, nullptr));
  EXPECT_DOUBLE_EQ(0.0, min);
  EXPECT_DOUBLE_EQ(0.0, max);
  GUIntBig hist[10] = {0};
  EXPECT_EQ(CE_None, band2.GetHistogram(0.0, 10.0, 10, hist, FALSE, FALSE,
                                        nullptr, nullptr));
  EXPECT_EQ(CE_None, CPLGetLastErrorType());

  double pct = 0.0;
  const int cov = band2.IGetDataCoverageStatus(0, 0, 10, 10, 0, &pct);
  EXPECT_NE(0, cov & GDAL_DATA_COVERAGE_STATUS_DATA);
  EXPECT_DOUBLE_EQ(-1.0, pct);
}

TEST_F(VrtDerivedRasterBandTest, XMLInitAndSerialize) {
  VRTDataset ds(20, 20);
  VRTMapSharedResources map_resources;

  constexpr const char* kInvalidXmls[] = {
      // Missing PixelFunctionType
      R"xml(<VRTRasterBand dataType="Float32" band="1" subClass="VRTDerivedRasterBand"/>)xml",
      // Unsupported PixelFunctionLanguage
      R"xml(
<VRTRasterBand dataType="Float32" band="1" subClass="VRTDerivedRasterBand">
  <PixelFunctionType>simple_invert</PixelFunctionType>
  <PixelFunctionLanguage>Fortran</PixelFunctionLanguage>
</VRTRasterBand>)xml",
      // PixelFunctionCode with C language
      R"xml(
<VRTRasterBand dataType="Float32" band="1" subClass="VRTDerivedRasterBand">
  <PixelFunctionType>simple_invert</PixelFunctionType>
  <PixelFunctionLanguage>C</PixelFunctionLanguage>
  <PixelFunctionCode>some_code</PixelFunctionCode>
</VRTRasterBand>)xml",
      // Invalid BufferRadius (< 0)
      R"xml(
<VRTRasterBand dataType="Float32" band="1" subClass="VRTDerivedRasterBand">
  <PixelFunctionType>simple_invert</PixelFunctionType>
  <PixelFunctionLanguage>Python</PixelFunctionLanguage>
  <BufferRadius>-5</BufferRadius>
</VRTRasterBand>)xml",
      // BufferRadius != 0 with C language
      R"xml(
<VRTRasterBand dataType="Float32" band="1" subClass="VRTDerivedRasterBand">
  <PixelFunctionType>simple_invert</PixelFunctionType>
  <PixelFunctionLanguage>C</PixelFunctionLanguage>
  <BufferRadius>2</BufferRadius>
</VRTRasterBand>)xml",
  };

  {
    WithQuietHandler quiet;
    VRTDerivedRasterBand band(&ds, 1);
    for (const char* xml : kInvalidXmls) {
      CPLErrorReset();
      CPLXMLTreeCloser tree(CPLParseXMLString(xml));
      ASSERT_NE(nullptr, tree);
      EXPECT_EQ(CE_Failure, band.XMLInit(tree.get(), nullptr, map_resources));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
  }

  constexpr char kValidXml[] = R"xml(
<VRTRasterBand dataType="Float32" band="1" subClass="VRTDerivedRasterBand">
  <PixelFunctionType>simple_invert</PixelFunctionType>
  <PixelFunctionLanguage>C</PixelFunctionLanguage>
  <PixelFunctionArguments k1="v1" k2="v2"/>
  <SourceTransferType>Float64</SourceTransferType>
  <SkipNonContributingSources>true</SkipNonContributingSources>
</VRTRasterBand>)xml";
  VRTDerivedRasterBand b2(&ds, 1);
  {
    CPLXMLTreeCloser tree(CPLParseXMLString(kValidXml));
    ASSERT_NE(nullptr, tree);
    EXPECT_EQ(CE_None, b2.XMLInit(tree.get(), nullptr, map_resources));
  }
  EXPECT_EQ(GDT_Float64, b2.eSourceTransferType);

  bool warned = false;
  size_t ram = 0;
  {
    CPLXMLTreeCloser serialized(b2.SerializeToXML(nullptr, warned, ram));
    ASSERT_NE(nullptr, serialized);
    std::unique_ptr<char, CplFreeDeleter> str(
        CPLSerializeXMLTree(serialized.get()));
    ASSERT_NE(nullptr, str);
    EXPECT_THAT(str.get(), HasSubstr("VRTDerivedRasterBand"));
    EXPECT_THAT(str.get(), HasSubstr("simple_invert"));
    EXPECT_THAT(str.get(), HasSubstr("k1=\"v1\""));
    EXPECT_THAT(str.get(), HasSubstr("Float64"));
    EXPECT_THAT(str.get(), HasSubstr("SkipNonContributingSources>true"));
  }

  constexpr char kPyXml[] = R"xml(
<VRTRasterBand dataType="Float32" band="1" subClass="VRTDerivedRasterBand">
  <PixelFunctionType>py_func</PixelFunctionType>
  <PixelFunctionLanguage>Python</PixelFunctionLanguage>
  <PixelFunctionCode><![CDATA[def py_func(): pass]]></PixelFunctionCode>
  <BufferRadius>3</BufferRadius>
  <SkipNonContributingSources>false</SkipNonContributingSources>
</VRTRasterBand>)xml";
  VRTDerivedRasterBand b3(&ds, 1);
  {
    CPLXMLTreeCloser tree(CPLParseXMLString(kPyXml));
    ASSERT_NE(nullptr, tree);
    EXPECT_EQ(CE_None, b3.XMLInit(tree.get(), nullptr, map_resources));
  }
  b3.AddPixelFunctionArgument("arg_a", "val_a");
  {
    CPLXMLTreeCloser serialized(b3.SerializeToXML(nullptr, warned, ram));
    ASSERT_NE(nullptr, serialized);
    std::unique_ptr<char, CplFreeDeleter> str(
        CPLSerializeXMLTree(serialized.get()));
    ASSERT_NE(nullptr, str);
    EXPECT_THAT(str.get(), HasSubstr("PixelFunctionLanguage>Python"));
    EXPECT_THAT(str.get(), HasSubstr("BufferRadius>3"));
    EXPECT_THAT(str.get(), HasSubstr("def py_func(): pass"));
  }
}

TEST_F(VrtDerivedRasterBandTest, RasterIOExecution) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->Create("", 10, 10, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, src_ds);
  std::vector<float> src_data(100);
  for (int i = 0; i < 100; ++i) {
    src_data[i] = static_cast<float>(i);
  }
  ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 10, 10, src_data.data(), 10, 10,
                         GDT_Float32, 0, 0, nullptr));

  VRTDataset vrt_ds(10, 10);
  VRTDerivedRasterBand derived_band(&vrt_ds, 1, GDT_Float32, 10, 10);
  derived_band.SetPixelFunctionName("simple_invert");

  auto src = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src->SetSrcBand(src_ds->GetRasterBand(1));
  src->SetSrcWindow(0, 0, 10, 10);
  src->SetDstWindow(0, 0, 10, 10);
  ASSERT_EQ(CE_None, derived_band.AddSource(src.release()));

  std::vector<float> dst_data(100, 0.0f);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              derived_band.RasterIO(GF_Write, 0, 0, 10, 10, dst_data.data(), 10,
                                    10, GDT_Float32, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  EXPECT_EQ(CE_None,
            derived_band.RasterIO(GF_Read, 0, 0, 10, 10, dst_data.data(), 10,
                                  10, GDT_Float32, 0, 0, nullptr));
  for (int i = 0; i < 100; ++i) {
    EXPECT_FLOAT_EQ(100.0f - static_cast<float>(i), dst_data[i]);
  }

  derived_band.SetPixelFunctionName("unregistered_func");
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              derived_band.RasterIO(GF_Read, 0, 0, 10, 10, dst_data.data(), 10,
                                    10, GDT_Float32, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  derived_band.SetPixelFunctionName("simple_invert");
  derived_band.SetSourceTransferType(GDT_Float32);
  EXPECT_EQ(CE_None,
            derived_band.RasterIO(GF_Read, 0, 0, 10, 10, dst_data.data(), 10,
                                  10, GDT_Float32, 0, 0, nullptr));

  // Force SplitRasterIO recursion via low allowed RAM usage.
  CPLConfigOptionSetter ram_limit("VRT_DERIVED_DATASET_ALLOWED_RAM_USAGE", "20",
                                  false);
  VRTDerivedRasterBand ram_band(&vrt_ds, 2, GDT_Float32, 10, 10);
  ram_band.SetPixelFunctionName("simple_invert");
  auto src2 = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src2->SetSrcBand(src_ds->GetRasterBand(1));
  src2->SetSrcWindow(0, 0, 10, 10);
  src2->SetDstWindow(0, 0, 10, 10);
  ASSERT_EQ(CE_None, ram_band.AddSource(src2.release()));
  std::fill(dst_data.begin(), dst_data.end(), -1.0f);
  EXPECT_EQ(CE_None, ram_band.RasterIO(GF_Read, 0, 0, 10, 10, dst_data.data(),
                                       10, 10, GDT_Float32, 0, 0, nullptr));
  for (int i = 0; i < 100; ++i) {
    EXPECT_FLOAT_EQ(100.0f - static_cast<float>(i), dst_data[i]);
  }
}

TEST_F(VrtDerivedRasterBandTest, PixelFunctionArgumentsAndBuiltins) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->Create("", 10, 10, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, src_ds);
  std::vector<float> src_data(100, 5.0f);
  ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 10, 10, src_data.data(), 10, 10,
                         GDT_Float32, 0, 0, nullptr));

  constexpr char kMeta[] = R"xml(
<PixelFunctionArgumentsList>
  <Argument name="constant_arg" type="constant" value="10"/>
  <Argument name="nodata_arg" type="builtin" value="NoData" optional="true"/>
  <Argument name="scale_arg" type="builtin" value="scale" optional="true"/>
  <Argument name="offset_arg" type="builtin" value="offset" optional="true"/>
  <Argument name="xoff_arg" type="builtin" value="xoff" optional="true"/>
  <Argument name="yoff_arg" type="builtin" value="yoff" optional="true"/>
  <Argument name="gt_arg" type="builtin" value="geotransform" optional="true"/>
  <Argument name="names_arg" type="builtin" value="source_names" optional="true"/>
</PixelFunctionArgumentsList>)xml";

  ASSERT_EQ(CE_None, VRTDerivedRasterBand::AddPixelFunction(
                         "func_args", FuncWithArgs, kMeta));

  VRTDataset vrt_ds(10, 10);
  GDALGeoTransform gt(100.0, 1.0, 0.0, 200.0, 0.0, -1.0);
  EXPECT_EQ(CE_None, vrt_ds.SetGeoTransform(gt));

  VRTDerivedRasterBand band(&vrt_ds, 1, GDT_Float32, 10, 10);
  band.SetPixelFunctionName("func_args");
  EXPECT_EQ(CE_None, band.SetNoDataValue(-999.0));
  EXPECT_EQ(CE_None, band.SetScale(2.0));
  EXPECT_EQ(CE_None, band.SetOffset(1.0));

  auto src = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src->SetSrcBand(src_ds->GetRasterBand(1));
  src->SetSrcWindow(0, 0, 10, 10);
  src->SetDstWindow(0, 0, 10, 10);
  ASSERT_EQ(CE_None, band.AddSource(src.release()));

  GetLastFuncArgs()->Clear();
  std::vector<float> dst(100, 0.0f);
  EXPECT_EQ(CE_None, band.RasterIO(GF_Read, 0, 0, 10, 10, dst.data(), 10, 10,
                                   GDT_Float32, 0, 0, nullptr));
  EXPECT_FLOAT_EQ(15.0f, dst[0]);
  EXPECT_STREQ("10", GetLastFuncArgs()->FetchNameValue("constant_arg"));
  EXPECT_STREQ("-999", GetLastFuncArgs()->FetchNameValue("NoData"));
  EXPECT_STREQ("2", GetLastFuncArgs()->FetchNameValue("scale"));
  EXPECT_STREQ("1", GetLastFuncArgs()->FetchNameValue("offset"));
  EXPECT_STREQ("0", GetLastFuncArgs()->FetchNameValue("xoff"));
  EXPECT_STREQ("0", GetLastFuncArgs()->FetchNameValue("yoff"));
  EXPECT_STREQ("100,1,0,200,0,-1",
               GetLastFuncArgs()->FetchNameValue("geotransform"));
  EXPECT_STREQ("B1", GetLastFuncArgs()->FetchNameValue("source_names"));

  constexpr char kBadMeta[] = R"xml(
<PixelFunctionArgumentsList>
  <Argument name="nodata_arg" type="builtin" value="NoData" optional="false"/>
</PixelFunctionArgumentsList>)xml";
  ASSERT_EQ(CE_None, VRTDerivedRasterBand::AddPixelFunction(
                         "bad_builtin", FuncWithArgs, kBadMeta));
  VRTDerivedRasterBand band_bad(&vrt_ds, 2, GDT_Float32, 10, 10);
  band_bad.SetPixelFunctionName("bad_builtin");
  auto src2 = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src2->SetSrcBand(src_ds->GetRasterBand(1));
  src2->SetSrcWindow(0, 0, 10, 10);
  src2->SetDstWindow(0, 0, 10, 10);
  ASSERT_EQ(CE_None, band_bad.AddSource(src2.release()));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              band_bad.RasterIO(GF_Read, 0, 0, 10, 10, dst.data(), 10, 10,
                                GDT_Float32, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  constexpr char kUnsupportedMeta[] = R"xml(
<PixelFunctionArgumentsList>
  <Argument name="unknown" type="builtin" value="unsupported_name"/>
</PixelFunctionArgumentsList>)xml";
  ASSERT_EQ(CE_None, VRTDerivedRasterBand::AddPixelFunction(
                         "unsupp", FuncWithArgs, kUnsupportedMeta));
  band_bad.SetPixelFunctionName("unsupp");
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              band_bad.RasterIO(GF_Read, 0, 0, 10, 10, dst.data(), 10, 10,
                                GDT_Float32, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(VrtDerivedRasterBandTest, SkipNonContributingSources) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->Create("", 10, 10, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, src_ds);

  VRTDataset vrt_ds(20, 20);
  VRTDerivedRasterBand band(&vrt_ds, 1, GDT_Float32, 20, 20);
  band.SetPixelFunctionName("simple_invert");
  band.SetSkipNonContributingSources(true);

  auto src = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src->SetSrcBand(src_ds->GetRasterBand(1));
  src->SetSrcWindow(0, 0, 5, 5);
  src->SetDstWindow(0, 0, 5, 5);
  ASSERT_EQ(CE_None, band.AddSource(src.release()));

  std::vector<float> dst(25, 42.0f);
  EXPECT_EQ(CE_None, band.RasterIO(GF_Read, 10, 10, 5, 5, dst.data(), 5, 5,
                                   GDT_Float32, 0, 0, nullptr));
  EXPECT_FLOAT_EQ(0.0f, dst[0]);
}

TEST_F(VrtDerivedRasterBandTest, PythonInitializationErrorHandling) {
  VRTDataset vrt_ds(10, 10);
  VRTDerivedRasterBand band(&vrt_ds, 1, GDT_Float32, 10, 10);
  band.SetPixelFunctionName("py_func");
  band.SetPixelFunctionLanguage("Python");

  std::vector<float> dst(100, 0.0f);
  WithQuietHandler quiet;

  CPLErrorReset();
  EXPECT_EQ(CE_Failure, band.RasterIO(GF_Read, 0, 0, 10, 10, dst.data(), 10, 10,
                                      GDT_Float32, 0, 0, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  VRTDerivedRasterBand c_band(&vrt_ds, 2, GDT_CInt16, 10, 10);
  c_band.SetPixelFunctionName("py_func");
  c_band.SetPixelFunctionLanguage("Python");
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, c_band.RasterIO(GF_Read, 0, 0, 10, 10, dst.data(), 10,
                                        10, GDT_CInt16, 0, 0, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  band.SetSourceTransferType(GDT_CInt16);
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, band.RasterIO(GF_Read, 0, 0, 10, 10, dst.data(), 10, 10,
                                      GDT_Float32, 0, 0, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(VrtDerivedRasterBandTest, RasterIOEdgeCases) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->Create("", 10, 10, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, src_ds);

  VRTDataset vrt_ds(10, 10);
  VRTDerivedRasterBand band(&vrt_ds, 1, GDT_Float32, 10, 10);
  band.SetPixelFunctionName("simple_invert");
  auto src = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src->SetSrcBand(src_ds->GetRasterBand(1));
  src->SetSrcWindow(0, 0, 10, 10);
  src->SetDstWindow(0, 0, 10, 10);
  ASSERT_EQ(CE_None, band.AddSource(src.release()));

  std::vector<float> buf(100, 0.0f);
  const GSpacing huge_linespace = static_cast<GSpacing>(INT_MAX) + 100;
  // nBufYSize == 1 resets nLineSpace to 0.
  EXPECT_EQ(CE_None,
            band.RasterIO(GF_Read, 0, 0, 10, 1, buf.data(), 10, 1, GDT_Float32,
                          sizeof(float), huge_linespace, nullptr));

  // nBufYSize > 1 with nLineSpace > INT_MAX fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band.RasterIO(GF_Read, 0, 0, 10, 2, buf.data(), 10, 2,
                                        GDT_Float32, sizeof(float),
                                        huge_linespace, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Simple source without underlying band fails.
  {
    WithQuietHandler quiet;
    VRTDerivedRasterBand band_null_src(&vrt_ds, 2, GDT_Float32, 10, 10);
    band_null_src.SetPixelFunctionName("simple_invert");
    auto null_src = std::make_unique<VRTSimpleSource>();
    ASSERT_EQ(CE_None, band_null_src.AddSource(null_src.release()));
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              band_null_src.RasterIO(GF_Read, 0, 0, 10, 10, buf.data(), 10, 10,
                                     GDT_Float32, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Complex source without underlying band fails.
  {
    WithQuietHandler quiet;
    VRTDerivedRasterBand band_complex_src(&vrt_ds, 3, GDT_Float32, 10, 10);
    band_complex_src.SetPixelFunctionName("simple_invert");
    auto complex_src = std::make_unique<VRTComplexSource>();
    ASSERT_EQ(CE_None, band_complex_src.AddSource(complex_src.release()));
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              band_complex_src.RasterIO(GF_Read, 0, 0, 10, 10, buf.data(), 10,
                                        10, GDT_Float32, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Buffer initialization with non-zero NoData and partial source placement.
  {
    VRTDerivedRasterBand band_nodata(&vrt_ds, 4, GDT_Float32, 10, 10);
    band_nodata.SetPixelFunctionName("simple_invert");
    EXPECT_EQ(CE_None, band_nodata.SetNoDataValue(-999.0));
    auto partial_src = std::make_unique<VRTSimpleSource>();
    src_ds->Reference();
    partial_src->SetSrcBand(src_ds->GetRasterBand(1));
    partial_src->SetSrcWindow(0, 0, 5, 5);
    partial_src->SetDstWindow(2, 2, 5, 5);
    ASSERT_EQ(CE_None, band_nodata.AddSource(partial_src.release()));
    EXPECT_EQ(CE_None, band_nodata.RasterIO(GF_Read, 0, 0, 10, 10, buf.data(),
                                            10, 10, GDT_Float32, sizeof(float),
                                            10 * sizeof(float), nullptr));
  }
}

TEST_F(VrtDerivedRasterBandTest, ExtraMetadataArguments) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->Create("", 10, 10, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, src_ds);

  constexpr char kMeta[] = R"xml(
<PixelFunctionArgumentsList>
  <Argument name="gt_arg" type="builtin" value="geotransform" optional="true"/>
  <Argument name="nodata_arg" type="builtin" value="NoData" optional="true"/>
  <Argument name="names_arg" type="builtin" value="source_names" optional="true"/>
</PixelFunctionArgumentsList>)xml";

  ASSERT_EQ(CE_None, VRTDerivedRasterBand::AddPixelFunction(
                         "multi_meta", FuncWithArgs, kMeta));

  // Dataset without geotransform exercises optional geotransform skip path.
  VRTDataset vrt_ds(10, 10);
  VRTDerivedRasterBand band(&vrt_ds, 1, GDT_Float32, 10, 10);
  band.SetPixelFunctionName("multi_meta");

  auto src1 = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src1->SetSrcBand(src_ds->GetRasterBand(1));
  src1->SetName("source_one");
  ASSERT_EQ(CE_None, band.AddSource(src1.release()));

  auto src2 = std::make_unique<VRTSimpleSource>();
  src_ds->Reference();
  src2->SetSrcBand(src_ds->GetRasterBand(1));
  ASSERT_EQ(CE_None, band.AddSource(src2.release()));

  GetLastFuncArgs()->Clear();
  std::vector<float> buf(100, 0.0f);
  EXPECT_EQ(CE_None, band.RasterIO(GF_Read, 0, 0, 10, 10, buf.data(), 10, 10,
                                   GDT_Float32, 0, 0, nullptr));
  EXPECT_STREQ("source_one|B2",
               GetLastFuncArgs()->FetchNameValue("source_names"));
  EXPECT_EQ(nullptr, GetLastFuncArgs()->FetchNameValue("geotransform"));
  EXPECT_EQ(nullptr, GetLastFuncArgs()->FetchNameValue("NoData"));
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover muparser/ExprTk expression evaluation and Python pixel
// function execution in vrtderivedrasterband.cpp if enabled in
// //third_party/gdal/BUILD.
