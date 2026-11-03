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
// Tests VRTKernelFilteredSource kernel validation, XML serialization, and
// spatial filter execution (vrtfilters.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/vrtfilters.cpp
//   https://gdal.org/drivers/raster/vrt.html

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kDummySourcePath[] = "/vsimem/vrtfilters_test/dummy.bin";

class VrtFiltersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_VRT();
    GDALRegister_MEM();
  }
};

TEST_F(VrtFiltersTest, FilteredSourceBasic) {
  VRTKernelFilteredSource source;
  EXPECT_STREQ("KernelFilteredSource", source.GetType());

  std::array<GDALDataType, 2> types = {GDT_Float32, GDT_Float64};
  source.SetFilteringDataTypesSupported(static_cast<int>(types.size()),
                                        types.data());
  source.SetExtraEdgePixels(2);
}

TEST_F(VrtFiltersTest, SetKernelValidation) {
  VRTKernelFilteredSource source;

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    // Even, zero, or negative kernel sizes are rejected.
    EXPECT_EQ(CE_Failure, source.SetKernel(2, false, {1, 1, 1, 1}));
    EXPECT_EQ(CE_Failure, source.SetKernel(0, false, {}));
    EXPECT_EQ(CE_Failure, source.SetKernel(-1, false, {}));

    // Mismatched coefficient count.
    EXPECT_EQ(CE_Failure, source.SetKernel(3, false, {1, 1}));
    EXPECT_EQ(CE_Failure, source.SetKernel(3, true, {1, 1, 1, 1}));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Valid 3x3 square kernel.
  std::vector<double> coefs9(9, 1.0 / 9.0);
  EXPECT_EQ(CE_None, source.SetKernel(3, false, coefs9));

  // Valid 3-element separable kernel.
  std::vector<double> coefs3(3, 1.0 / 3.0);
  EXPECT_EQ(CE_None, source.SetKernel(3, true, coefs3));
}

TEST_F(VrtFiltersTest, XMLInitValidation) {
  VsiMemTempWrapper dummy_file(kDummySourcePath, std::string(1, '\0'));
  VRTMapSharedResources map_resources;
  VRTKernelFilteredSource source;

  // Size=0 clears the kernel and succeeds.
  constexpr char kXmlSize0[] = R"xml(
<KernelFilteredSource>
  <SourceFilename>/vsimem/vrtfilters_test/dummy.bin</SourceFilename>
  <Kernel><Size>0</Size></Kernel>
</KernelFilteredSource>)xml";
  {
    CPLXMLTreeCloser tree(CPLParseXMLString(kXmlSize0));
    ASSERT_NE(nullptr, tree);
    EXPECT_EQ(CE_None, source.XMLInit(tree.get(), nullptr, map_resources));
  }

  constexpr const char* kInvalidXmls[] = {
      // Negative Size
      R"xml(
<KernelFilteredSource>
  <SourceFilename>/vsimem/vrtfilters_test/dummy.bin</SourceFilename>
  <Kernel><Size>-5</Size></Kernel>
</KernelFilteredSource>)xml",
      // Wrong number of coefs
      R"xml(
<KernelFilteredSource>
  <SourceFilename>/vsimem/vrtfilters_test/dummy.bin</SourceFilename>
  <Kernel>
    <Size>3</Size>
    <Coefs>1 2 3 4</Coefs>
  </Kernel>
</KernelFilteredSource>)xml",
      // Unsupported function
      R"xml(
<KernelFilteredSource>
  <SourceFilename>/vsimem/vrtfilters_test/dummy.bin</SourceFilename>
  <Kernel>
    <Size>3</Size>
    <Coefs>1 1 1 1 1 1 1 1 1</Coefs>
  </Kernel>
  <Function>unsupported</Function>
</KernelFilteredSource>)xml",
  };

  WithQuietHandler quiet;
  for (const char* xml : kInvalidXmls) {
    CPLErrorReset();
    CPLXMLTreeCloser tree(CPLParseXMLString(xml));
    ASSERT_NE(nullptr, tree);
    EXPECT_EQ(CE_Failure, source.XMLInit(tree.get(), nullptr, map_resources));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(VrtFiltersTest, XMLInitAndSerialize) {
  VsiMemTempWrapper dummy_file(kDummySourcePath, std::string(1, '\0'));
  VRTMapSharedResources map_resources;
  VRTKernelFilteredSource source;

  constexpr char kValidXml[] = R"xml(
<KernelFilteredSource>
  <SourceFilename>/vsimem/vrtfilters_test/dummy.bin</SourceFilename>
  <Kernel normalized="1">
    <Size>3</Size>
    <Coefs>1 2 1 2 4 2 1 2 1</Coefs>
  </Kernel>
  <Function>median</Function>
</KernelFilteredSource>)xml";
  {
    CPLXMLTreeCloser tree(CPLParseXMLString(kValidXml));
    ASSERT_NE(nullptr, tree);
    EXPECT_EQ(CE_None, source.XMLInit(tree.get(), nullptr, map_resources));
  }

  CPLXMLTreeCloser serialized(source.SerializeToXML(nullptr));
  ASSERT_NE(nullptr, serialized);
  std::unique_ptr<char, CplFreeDeleter> xml_str(
      CPLSerializeXMLTree(serialized.get()));
  ASSERT_NE(nullptr, xml_str);
  EXPECT_THAT(xml_str.get(), HasSubstr("KernelFilteredSource"));
  EXPECT_THAT(xml_str.get(), HasSubstr("median"));
  EXPECT_THAT(xml_str.get(), HasSubstr("normalized=\"1\""));

  // Serialize when kernel size == 0 omits <Kernel>.
  VRTKernelFilteredSource empty_source;
  CPLXMLTreeCloser empty_ser(empty_source.SerializeToXML(nullptr));
  ASSERT_NE(nullptr, empty_ser);
  EXPECT_EQ(nullptr, CPLGetXMLNode(empty_ser.get(), "Kernel"));
}

TEST_F(VrtFiltersTest, VRTParseFilterSourcesHelper) {
  VsiMemTempWrapper dummy_file(kDummySourcePath, std::string(1, '\0'));
  VRTMapSharedResources map_resources;

  // Matching node returns initialized VRTKernelFilteredSource.
  constexpr char kXmlStr[] = R"xml(
<KernelFilteredSource>
  <SourceFilename>/vsimem/vrtfilters_test/dummy.bin</SourceFilename>
  <Kernel><Size>0</Size></Kernel>
</KernelFilteredSource>)xml";
  {
    CPLXMLTreeCloser tree(CPLParseXMLString(kXmlStr));
    ASSERT_NE(nullptr, tree);
    auto src = std::unique_ptr<VRTSource>(
        VRTParseFilterSources(tree.get(), nullptr, map_resources));
    EXPECT_NE(nullptr, src);
  }

  // Non-matching node returns nullptr without error.
  {
    CPLXMLTreeCloser tree(CPLParseXMLString("<OtherSource/>"));
    ASSERT_NE(nullptr, tree);
    EXPECT_EQ(nullptr,
              VRTParseFilterSources(tree.get(), nullptr, map_resources));
  }

  // Failure during XMLInit returns nullptr.
  constexpr char kBadXml[] = R"xml(
<KernelFilteredSource>
  <SourceFilename>/vsimem/vrtfilters_test/dummy.bin</SourceFilename>
  <Kernel><Size>-1</Size></Kernel>
</KernelFilteredSource>)xml";
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLXMLTreeCloser tree(CPLParseXMLString(kBadXml));
    ASSERT_NE(nullptr, tree);
    EXPECT_EQ(nullptr,
              VRTParseFilterSources(tree.get(), nullptr, map_resources));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(VrtFiltersTest, FilterDataExecutionAndFunctions) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->Create("", 5, 5, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, src_ds);

  GDALRasterBand* src_band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, src_band);
  EXPECT_EQ(CE_None, src_band->SetNoDataValue(-999.0));

  std::vector<float> data(25);
  for (int i = 0; i < 25; ++i) {
    data[i] = static_cast<float>(i + 1);
  }
  data[12] = -999.0f;
  ASSERT_EQ(CE_None, src_band->RasterIO(GF_Write, 0, 0, 5, 5, data.data(), 5, 5,
                                        GDT_Float32, 0, 0, nullptr));

  // Exercise all supported kernel functions and normalized mean.
  constexpr std::string_view kFunctions[] = {
      "min", "max", "median", "mode", "stddev", "",
  };
  for (std::string_view func : kFunctions) {
    SCOPED_TRACE(std::string(func));
    src_ds->Reference();
    // filt_source owns a reference to src_ds, which vrt_ds's destructor
    // releases.

    VRTDataset vrt_ds(5, 5);
    ASSERT_EQ(CE_None, vrt_ds.AddBand(GDT_Float32, nullptr));
    auto* vrt_band =
        cpl::down_cast<VRTSourcedRasterBand*>(vrt_ds.GetRasterBand(1));

    auto filt_source = std::make_unique<VRTKernelFilteredSource>();
    filt_source->SetSrcBand(src_band);
    filt_source->SetSrcWindow(0, 0, 5, 5);
    filt_source->SetDstWindow(0, 0, 5, 5);

    std::vector<double> coefs(9, 1.0);
    ASSERT_EQ(CE_None, filt_source->SetKernel(3, false, coefs));
    if (!func.empty()) {
      filt_source->SetFunction(std::string(func).c_str());
    } else {
      filt_source->SetNormalized(true);
    }

    ASSERT_EQ(CE_None, vrt_band->AddSource(filt_source.release()));

    std::vector<float> out(25, 0.0f);
    EXPECT_EQ(CE_None, vrt_ds.GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 5, 5, out.data(), 5, 5, GDT_Float32,
                           0, 0, nullptr));
    EXPECT_TRUE(std::isfinite(out[0]));
  }

  // Separable kernel path (without nodata).
  {
    GDALDatasetUniquePtr sep_ds(
        mem_drv->Create("", 5, 5, 1, GDT_Float32, nullptr));
    ASSERT_NE(nullptr, sep_ds);
    GDALRasterBand* sep_band = sep_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, sep_band);
    std::vector<float> sep_data(25);
    for (int i = 0; i < 25; ++i) {
      sep_data[i] = static_cast<float>(i + 1);
    }
    ASSERT_EQ(CE_None, sep_band->RasterIO(GF_Write, 0, 0, 5, 5, sep_data.data(),
                                          5, 5, GDT_Float32, 0, 0, nullptr));

    VRTDataset vrt_ds(5, 5);
    ASSERT_EQ(CE_None, vrt_ds.AddBand(GDT_Float32, nullptr));
    auto* vrt_band =
        cpl::down_cast<VRTSourcedRasterBand*>(vrt_ds.GetRasterBand(1));
    auto filt_source = std::make_unique<VRTKernelFilteredSource>();
    sep_ds->Reference();
    filt_source->SetSrcBand(sep_band);
    filt_source->SetSrcWindow(0, 0, 5, 5);
    filt_source->SetDstWindow(0, 0, 5, 5);

    std::vector<double> coefs(3, 1.0 / 3.0);
    ASSERT_EQ(CE_None, filt_source->SetKernel(3, true, coefs));
    ASSERT_EQ(CE_None, vrt_band->AddSource(filt_source.release()));

    std::vector<float> out(25, 0.0f);
    EXPECT_EQ(CE_None, vrt_ds.GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 5, 5, out.data(), 5, 5, GDT_Float32,
                           0, 0, nullptr));
  }

  // Byte source dataset without nodata.
  {
    GDALDatasetUniquePtr byte_ds(
        mem_drv->Create("", 5, 5, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, byte_ds);
    std::vector<uint8_t> byte_data(25, 42);
    ASSERT_EQ(CE_None, byte_ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 5, 5, byte_data.data(), 5, 5,
                           GDT_Byte, 0, 0, nullptr));

    VRTDataset vrt_ds(5, 5);
    ASSERT_EQ(CE_None, vrt_ds.AddBand(GDT_Float32, nullptr));
    auto* vrt_band =
        cpl::down_cast<VRTSourcedRasterBand*>(vrt_ds.GetRasterBand(1));
    auto filt_source = std::make_unique<VRTKernelFilteredSource>();
    byte_ds->Reference();
    filt_source->SetSrcBand(byte_ds->GetRasterBand(1));
    filt_source->SetSrcWindow(0, 0, 5, 5);
    filt_source->SetDstWindow(0, 0, 5, 5);
    std::vector<double> coefs(9, 1.0);
    ASSERT_EQ(CE_None, filt_source->SetKernel(3, false, coefs));
    ASSERT_EQ(CE_None, vrt_band->AddSource(filt_source.release()));

    std::vector<float> out(25, 0.0f);
    EXPECT_EQ(CE_None, vrt_ds.GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 5, 5, out.data(), 5, 5, GDT_Float32,
                           0, 0, nullptr));
  }

  // Resampled window request (5x5 -> 2x2).
  {
    VRTDataset vrt_ds(5, 5);
    ASSERT_EQ(CE_None, vrt_ds.AddBand(GDT_Float32, nullptr));
    auto* vrt_band =
        cpl::down_cast<VRTSourcedRasterBand*>(vrt_ds.GetRasterBand(1));
    auto filt_source = std::make_unique<VRTKernelFilteredSource>();
    src_ds->Reference();
    filt_source->SetSrcBand(src_band);
    filt_source->SetSrcWindow(0, 0, 5, 5);
    filt_source->SetDstWindow(0, 0, 5, 5);

    std::vector<double> coefs(9, 1.0);
    ASSERT_EQ(CE_None, filt_source->SetKernel(3, false, coefs));
    ASSERT_EQ(CE_None, vrt_band->AddSource(filt_source.release()));

    std::vector<float> out(4, 0.0f);
    EXPECT_EQ(CE_None, vrt_ds.GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 5, 5, out.data(), 2, 2, GDT_Float32,
                           0, 0, nullptr));
  }

  // Floating-point window request.
  {
    VRTDataset vrt_ds(5, 5);
    ASSERT_EQ(CE_None, vrt_ds.AddBand(GDT_Float32, nullptr));
    auto* vrt_band =
        cpl::down_cast<VRTSourcedRasterBand*>(vrt_ds.GetRasterBand(1));
    auto filt_source = std::make_unique<VRTKernelFilteredSource>();
    src_ds->Reference();
    filt_source->SetSrcBand(src_band);
    filt_source->SetSrcWindow(0, 0, 5, 5);
    filt_source->SetDstWindow(0, 0, 5, 5);

    std::vector<double> coefs(9, 1.0);
    ASSERT_EQ(CE_None, filt_source->SetKernel(3, false, coefs));
    ASSERT_EQ(CE_None, vrt_band->AddSource(filt_source.release()));

    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.bFloatingPointWindowValidity = TRUE;
    extra.dfXOff = 0.0;
    extra.dfYOff = 0.0;
    extra.dfXSize = 5.0;
    extra.dfYSize = 5.0;

    std::vector<float> out(25, 0.0f);
    EXPECT_EQ(CE_None, vrt_ds.GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 5, 5, out.data(), 5, 5, GDT_Float32,
                           0, 0, &extra));
  }

  // FilterData directly with unsupported GDT_Byte fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    VRTKernelFilteredSource filt_source;
    uint8_t dummy[10] = {0};
    EXPECT_EQ(CE_Failure, filt_source.FilterData(1, 1, GDT_Byte, dummy, dummy));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover remaining defensive error branches in
// VRTKernelFilteredSource::FilterData and SerializeToXML in vrtfilters.cpp.
