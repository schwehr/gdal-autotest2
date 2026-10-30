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
// Tests for vrtfilters.cpp.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

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

class VrtFiltersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    VSILFILE* fp = VSIFOpenL("/vsimem/dummy.bin", "wb");
    if (fp) {
      uint8_t zero = 0;
      VSIFWriteL(&zero, 1, 1, fp);
      VSIFCloseL(fp);
    }
  }

  void TearDown() override { VSIUnlink("/vsimem/dummy.bin"); }
};

TEST_F(VrtFiltersTest, FilteredSourceBasic) {
  VRTKernelFilteredSource source;
  EXPECT_STREQ(source.GetType(), "KernelFilteredSource");

  GDALDataType types[] = {GDT_Float32, GDT_Float64};
  source.SetFilteringDataTypesSupported(2, types);
  source.SetExtraEdgePixels(2);
}

TEST_F(VrtFiltersTest, SetKernelValidation) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  VRTKernelFilteredSource source;

  // Even kernel size -> error
  EXPECT_EQ(source.SetKernel(2, false, {1, 1, 1, 1}), CE_Failure);
  EXPECT_EQ(source.SetKernel(0, false, {}), CE_Failure);
  EXPECT_EQ(source.SetKernel(-1, false, {}), CE_Failure);

  // Mismatched coefficient count
  EXPECT_EQ(source.SetKernel(3, false, {1, 1}), CE_Failure);
  EXPECT_EQ(source.SetKernel(3, true, {1, 1, 1, 1}), CE_Failure);

  // Valid 3x3 square
  std::vector<double> coefs9(9, 1.0 / 9.0);
  EXPECT_EQ(source.SetKernel(3, false, coefs9), CE_None);

  // Valid 3 separable
  std::vector<double> coefs3(3, 1.0 / 3.0);
  EXPECT_EQ(source.SetKernel(3, true, coefs3), CE_None);

  CPLPopErrorHandler();
}

TEST_F(VrtFiltersTest, XMLInitValidation) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  VRTMapSharedResources map_resources;
  VRTKernelFilteredSource source;

  // Missing or Size=0
  const char* xml_size_0 =
      "<KernelFilteredSource>"
      "  <SourceFilename>/vsimem/dummy.bin</SourceFilename>"
      "  <Kernel><Size>0</Size></Kernel>"
      "</KernelFilteredSource>";
  CPLXMLNode* tree = CPLParseXMLString(xml_size_0);
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(source.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);

  // Negative Size
  const char* xml_neg =
      "<KernelFilteredSource>"
      "  <SourceFilename>/vsimem/dummy.bin</SourceFilename>"
      "  <Kernel><Size>-5</Size></Kernel>"
      "</KernelFilteredSource>";
  tree = CPLParseXMLString(xml_neg);
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(source.XMLInit(tree, nullptr, map_resources), CE_Failure);
  CPLDestroyXMLNode(tree);

  // Wrong number of coefs
  const char* xml_wrong_coefs =
      "<KernelFilteredSource>"
      "  <SourceFilename>/vsimem/dummy.bin</SourceFilename>"
      "  <Kernel>"
      "    <Size>3</Size>"
      "    <Coefs>1 2 3 4</Coefs>"
      "  </Kernel>"
      "</KernelFilteredSource>";
  tree = CPLParseXMLString(xml_wrong_coefs);
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(source.XMLInit(tree, nullptr, map_resources), CE_Failure);
  CPLDestroyXMLNode(tree);

  // Unsupported function
  const char* xml_bad_func =
      "<KernelFilteredSource>"
      "  <SourceFilename>/vsimem/dummy.bin</SourceFilename>"
      "  <Kernel>"
      "    <Size>3</Size>"
      "    <Coefs>1 1 1 1 1 1 1 1 1</Coefs>"
      "  </Kernel>"
      "  <Function>unsupported</Function>"
      "</KernelFilteredSource>";
  tree = CPLParseXMLString(xml_bad_func);
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(source.XMLInit(tree, nullptr, map_resources), CE_Failure);
  CPLDestroyXMLNode(tree);

  CPLPopErrorHandler();
}

TEST_F(VrtFiltersTest, XMLInitAndSerialize) {
  VRTMapSharedResources map_resources;
  VRTKernelFilteredSource source;

  const char* valid_xml =
      "<KernelFilteredSource>"
      "  <SourceFilename>/vsimem/dummy.bin</SourceFilename>"
      "  <Kernel normalized=\"1\">"
      "    <Size>3</Size>"
      "    <Coefs>1 2 1 2 4 2 1 2 1</Coefs>"
      "  </Kernel>"
      "  <Function>median</Function>"
      "</KernelFilteredSource>";
  CPLXMLNode* tree = CPLParseXMLString(valid_xml);
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(source.XMLInit(tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(tree);

  CPLXMLNode* serialized = source.SerializeToXML(nullptr);
  ASSERT_NE(serialized, nullptr);
  char* str = CPLSerializeXMLTree(serialized);
  EXPECT_NE(strstr(str, "KernelFilteredSource"), nullptr);
  EXPECT_NE(strstr(str, "median"), nullptr);
  EXPECT_NE(strstr(str, "normalized=\"1\""), nullptr);
  CPLFree(str);
  CPLDestroyXMLNode(serialized);

  // Serialize when size == 0
  VRTKernelFilteredSource empty_source;
  CPLXMLNode* empty_ser = empty_source.SerializeToXML(nullptr);
  ASSERT_NE(empty_ser, nullptr);
  EXPECT_EQ(CPLGetXMLNode(empty_ser, "Kernel"), nullptr);
  CPLDestroyXMLNode(empty_ser);
}

TEST_F(VrtFiltersTest, VRTParseFilterSourcesHelper) {
  VRTMapSharedResources map_resources;

  // Matching node
  const char* xml_str =
      "<KernelFilteredSource>"
      "  <SourceFilename>/vsimem/dummy.bin</SourceFilename>"
      "  <Kernel><Size>0</Size></Kernel>"
      "</KernelFilteredSource>";
  CPLXMLNode* tree = CPLParseXMLString(xml_str);
  ASSERT_NE(tree, nullptr);
  VRTSource* src = VRTParseFilterSources(tree, nullptr, map_resources);
  EXPECT_NE(src, nullptr);
  delete src;
  CPLDestroyXMLNode(tree);

  // Other node
  const char* other_xml = "<OtherSource/>";
  tree = CPLParseXMLString(other_xml);
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(VRTParseFilterSources(tree, nullptr, map_resources), nullptr);
  CPLDestroyXMLNode(tree);

  // Failure during XMLInit
  CPLPushErrorHandler(CPLQuietErrorHandler);
  const char* bad_xml =
      "<KernelFilteredSource>"
      "  <SourceFilename>/vsimem/dummy.bin</SourceFilename>"
      "  <Kernel><Size>-1</Size></Kernel>"
      "</KernelFilteredSource>";
  tree = CPLParseXMLString(bad_xml);
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(VRTParseFilterSources(tree, nullptr, map_resources), nullptr);
  CPLDestroyXMLNode(tree);
  CPLPopErrorHandler();
}

TEST_F(VrtFiltersTest, FilterDataExecutionAndFunctions) {
  // Create a MEM dataset with known values
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(mem_drv, nullptr);
  GDALDataset* src_ds = mem_drv->Create("", 5, 5, 1, GDT_Float32, nullptr);
  ASSERT_NE(src_ds, nullptr);

  GDALRasterBand* src_band = src_ds->GetRasterBand(1);
  ASSERT_NE(src_band, nullptr);
  src_band->SetNoDataValue(-999.0);

  std::vector<float> data(25);
  for (int i = 0; i < 25; ++i) {
    data[i] = static_cast<float>(i + 1);
  }
  // Put a nodata value in one pixel
  data[12] = -999.0f;
  ASSERT_EQ(src_band->RasterIO(GF_Write, 0, 0, 5, 5, data.data(), 5, 5,
                               GDT_Float32, 0, 0, nullptr),
            CE_None);

  // Test different functions via VRTKernelFilteredSource
  const char* functions[] = {"min", "max", "median", "mode", "stddev", ""};

  for (const char* func : functions) {
    src_ds->Reference();
    VRTDataset vrt_ds(5, 5);
    vrt_ds.AddBand(GDT_Float32, nullptr);
    auto* vrt_band =
        static_cast<VRTSourcedRasterBand*>(vrt_ds.GetRasterBand(1));

    auto filt_source = std::make_unique<VRTKernelFilteredSource>();
    filt_source->SetSrcBand(src_band);
    filt_source->SetSrcWindow(0, 0, 5, 5);
    filt_source->SetDstWindow(0, 0, 5, 5);

    std::vector<double> coefs(9, 1.0);
    filt_source->SetKernel(3, false, coefs);
    if (strlen(func) > 0) {
      filt_source->SetFunction(func);
    } else {
      filt_source->SetNormalized(true);
    }

    vrt_band->AddSource(filt_source.release());

    std::vector<float> out(25, 0.0f);
    EXPECT_EQ(
        vrt_ds.GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 5, 5, out.data(), 5, 5,
                                          GDT_Float32, 0, 0, nullptr),
        CE_None);
    // At center pixel (which was nodata in input), it should propagate nodata
    // or valid
    EXPECT_TRUE(std::isfinite(out[0]));
  }

  // Separable kernel test (without nodata; see upstream VRTKernelFilteredSource
  // separable+nodata halo initialization behavior)
  {
    GDALDataset* sep_ds = mem_drv->Create("", 5, 5, 1, GDT_Float32, nullptr);
    ASSERT_NE(sep_ds, nullptr);
    sep_ds->Reference();
    GDALRasterBand* sep_band = sep_ds->GetRasterBand(1);
    ASSERT_NE(sep_band, nullptr);
    std::vector<float> sep_data(25);
    for (int i = 0; i < 25; ++i) {
      sep_data[i] = static_cast<float>(i + 1);
    }
    ASSERT_EQ(sep_band->RasterIO(GF_Write, 0, 0, 5, 5, sep_data.data(), 5, 5,
                                 GDT_Float32, 0, 0, nullptr),
              CE_None);

    {
      VRTDataset vrt_ds(5, 5);
      vrt_ds.AddBand(GDT_Float32, nullptr);
      auto* vrt_band =
          static_cast<VRTSourcedRasterBand*>(vrt_ds.GetRasterBand(1));
      auto filt_source = std::make_unique<VRTKernelFilteredSource>();
      filt_source->SetSrcBand(sep_band);
      filt_source->SetSrcWindow(0, 0, 5, 5);
      filt_source->SetDstWindow(0, 0, 5, 5);

      std::vector<double> coefs(3, 1.0 / 3.0);
      filt_source->SetKernel(3, true, coefs);
      vrt_band->AddSource(filt_source.release());

      std::vector<float> out(25, 0.0f);
      EXPECT_EQ(
          vrt_ds.GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 5, 5, out.data(), 5,
                                            5, GDT_Float32, 0, 0, nullptr),
          CE_None);
    }
    GDALClose(sep_ds);
  }

  // Test without nodata value and with byte data type
  {
    GDALDataset* byte_ds = mem_drv->Create("", 5, 5, 1, GDT_Byte, nullptr);
    ASSERT_NE(byte_ds, nullptr);
    byte_ds->Reference();
    std::vector<uint8_t> byte_data(25, 42);
    ASSERT_EQ(byte_ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 5, 5,
                                                  byte_data.data(), 5, 5,
                                                  GDT_Byte, 0, 0, nullptr),
              CE_None);
    {
      VRTDataset vrt_ds(5, 5);
      vrt_ds.AddBand(GDT_Float32, nullptr);
      auto* vrt_band =
          static_cast<VRTSourcedRasterBand*>(vrt_ds.GetRasterBand(1));
      auto filt_source = std::make_unique<VRTKernelFilteredSource>();
      filt_source->SetSrcBand(byte_ds->GetRasterBand(1));
      filt_source->SetSrcWindow(0, 0, 5, 5);
      filt_source->SetDstWindow(0, 0, 5, 5);
      std::vector<double> coefs(9, 1.0);
      filt_source->SetKernel(3, false, coefs);
      vrt_band->AddSource(filt_source.release());

      std::vector<float> out(25, 0.0f);
      EXPECT_EQ(
          vrt_ds.GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 5, 5, out.data(), 5,
                                            5, GDT_Float32, 0, 0, nullptr),
          CE_None);
    }
    GDALClose(byte_ds);
  }
  {
    src_ds->Reference();
    VRTDataset vrt_ds(5, 5);
    vrt_ds.AddBand(GDT_Float32, nullptr);
    auto* vrt_band =
        static_cast<VRTSourcedRasterBand*>(vrt_ds.GetRasterBand(1));
    auto filt_source = std::make_unique<VRTKernelFilteredSource>();
    filt_source->SetSrcBand(src_band);
    filt_source->SetSrcWindow(0, 0, 5, 5);
    filt_source->SetDstWindow(0, 0, 5, 5);

    std::vector<double> coefs(9, 1.0);
    filt_source->SetKernel(3, false, coefs);
    vrt_band->AddSource(filt_source.release());

    std::vector<float> out(4, 0.0f);
    EXPECT_EQ(
        vrt_ds.GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 5, 5, out.data(), 2, 2,
                                          GDT_Float32, 0, 0, nullptr),
        CE_None);
  }

  // Floating point window request
  {
    src_ds->Reference();
    VRTDataset vrt_ds(5, 5);
    vrt_ds.AddBand(GDT_Float32, nullptr);
    auto* vrt_band =
        static_cast<VRTSourcedRasterBand*>(vrt_ds.GetRasterBand(1));
    auto filt_source = std::make_unique<VRTKernelFilteredSource>();
    filt_source->SetSrcBand(src_band);
    filt_source->SetSrcWindow(0, 0, 5, 5);
    filt_source->SetDstWindow(0, 0, 5, 5);

    std::vector<double> coefs(9, 1.0);
    filt_source->SetKernel(3, false, coefs);
    vrt_band->AddSource(filt_source.release());

    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.bFloatingPointWindowValidity = TRUE;
    extra.dfXOff = 0.0;
    extra.dfYOff = 0.0;
    extra.dfXSize = 5.0;
    extra.dfYSize = 5.0;

    std::vector<float> out(25, 0.0f);
    EXPECT_EQ(
        vrt_ds.GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 5, 5, out.data(), 5, 5,
                                          GDT_Float32, 0, 0, &extra),
        CE_None);
  }

  // Test FilterData directly with wrong data type -> error
  {
    CPLPushErrorHandler(CPLQuietErrorHandler);
    VRTKernelFilteredSource filt_source;
    uint8_t dummy[10] = {0};
    EXPECT_EQ(filt_source.FilterData(1, 1, GDT_Byte, dummy, dummy), CE_Failure);
    CPLPopErrorHandler();
  }

  GDALClose(src_ds);
}

}  // namespace
}  // namespace autotest2
