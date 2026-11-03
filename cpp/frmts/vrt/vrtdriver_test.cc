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
// Tests the GDAL VRT driver registration, metadata, source parser registry,
// CreateCopy behavior, and driver error paths (vrtdriver.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/vrtdriver.cpp
//   https://gdal.org/drivers/raster/vrt.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/vrt.py

#include <memory>
#include <span>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::ElementsAre;

constexpr char kTestDir[] = "/vsimem/vrtdriver_test";

int DummyProgress(double /*dfComplete*/, const char* /*pszMessage*/,
                  void* pProgressArg) {
  if (pProgressArg != nullptr) {
    *static_cast<int*>(pProgressArg) += 1;
  }
  return TRUE;
}

VRTSource* DummySourceParser(const CPLXMLNode* /*psSrc*/,
                             const char* /*pszVRTPath*/,
                             VRTMapSharedResources& /*oMapSharedSources*/) {
  return new VRTSimpleSource();
}

GDALDatasetUniquePtr OpenRasterReadOnly(const char* filename,
                                        unsigned int extra_flags = 0) {
  return GDALDatasetUniquePtr(GDALDataset::FromHandle(
      GDALOpenEx(filename, GDAL_OF_READONLY | GDAL_OF_RASTER | extra_flags,
                 nullptr, nullptr, nullptr)));
}

class VrtDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_VRT();
    GDALRegister_MEM();
    poDriver_ = cpl::down_cast<VRTDriver*>(
        GetGDALDriverManager()->GetDriverByName("VRT"));
    ASSERT_NE(nullptr, poDriver_);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }

  VRTDriver* poDriver_ = nullptr;
};

TEST_F(VrtDriverTest, RegisterAgain) {
  // Calling GDALRegister_VRT when already registered is a no-op.
  GDALRegister_VRT();
  EXPECT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("VRT"));
}

TEST_F(VrtDriverTest, IdentifyDoesNotExistAndCorrupt) {
  GDALOpenInfo missing_info("/does_not_exist/missing.vrt", GA_ReadOnly);
  EXPECT_EQ(GDAL_IDENTIFY_FALSE, VRTDataset::Identify(&missing_info));

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(nullptr, OpenRasterReadOnly("/does_not_exist/missing.vrt",
                                        GDAL_OF_VERBOSE_ERROR));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_EQ(nullptr, OpenRasterReadOnly("<VRTDataset><BrokenXml",
                                        GDAL_OF_VERBOSE_ERROR));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(VrtDriverTest, MetadataDomainListAndMetadata) {
  std::unique_ptr<char*, decltype([](char** p) { CSLDestroy(p); })> domain_list(
      poDriver_->GetMetadataDomainList());
  ASSERT_NE(nullptr, domain_list);
  EXPECT_THAT(CslToVector(domain_list.get()), Contains("SourceParsers"));

  EXPECT_NE(nullptr, poDriver_->GetMetadata("SourceParsers"));
  EXPECT_NE(nullptr, poDriver_->GetMetadata(nullptr));

  // SetMetadata on custom domain and clean up before exit so global VRT
  // driver metadata stays isolated across tests.
  absl::Cleanup cleanup_domain = [this] {
    poDriver_->SetMetadata(nullptr, "CUSTOM_DOMAIN");
  };
  CPLStringList custom_meta;
  custom_meta.SetNameValue("KEY", "VALUE");
  EXPECT_EQ(CE_None,
            poDriver_->SetMetadata(custom_meta.List(), "CUSTOM_DOMAIN"));
  char** retrieved = poDriver_->GetMetadata("CUSTOM_DOMAIN");
  ASSERT_NE(nullptr, retrieved);
  EXPECT_STREQ("VALUE", CSLFetchNameValue(retrieved, "KEY"));
}

TEST_F(VrtDriverTest, AddAndParseSource) {
  VRTDriver driver;
  VRTMapSharedResources map_resources;

  // ParseSource on null or non-element node.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, driver.ParseSource(nullptr, nullptr, map_resources));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLXMLTreeCloser text_node(CPLCreateXMLNode(nullptr, CXT_Text, "text"));
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              driver.ParseSource(text_node.get(), nullptr, map_resources));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Register a custom source parser.
  driver.AddSourceParser("MyCustomSource", DummySourceParser);

  // Parse matching node.
  CPLXMLTreeCloser src_node(
      CPLCreateXMLNode(nullptr, CXT_Element, "MyCustomSource"));
  auto source = std::unique_ptr<VRTSource>(
      driver.ParseSource(src_node.get(), nullptr, map_resources));
  EXPECT_NE(nullptr, source);

  // Parse non-matching node.
  CPLXMLTreeCloser other_node(
      CPLCreateXMLNode(nullptr, CXT_Element, "UnknownSource"));
  EXPECT_EQ(nullptr,
            driver.ParseSource(other_node.get(), nullptr, map_resources));

  // Fallback path when m_oMapSourceParser is empty: SetMetadata with
  // "SourceParsers" clears m_oMapSourceParser but populates
  // papszSourceParsers. Duplicate first because SetMetadata destroys the old
  // list before duplicating the input.
  CPLStringList parsers_meta(CSLDuplicate(driver.GetMetadata("SourceParsers")));
  driver.SetMetadata(parsers_meta.List(), "SourceParsers");

  source.reset(driver.ParseSource(src_node.get(), nullptr, map_resources));
  EXPECT_NE(nullptr, source);
  EXPECT_EQ(nullptr,
            driver.ParseSource(other_node.get(), nullptr, map_resources));

  // Corrupt pointer string in papszSourceParsers.
  CPLStringList corrupt_meta;
  corrupt_meta.SetNameValue("MyCustomSource", "");
  driver.SetMetadata(corrupt_meta.List(), "SourceParsers");
  EXPECT_EQ(nullptr,
            driver.ParseSource(src_node.get(), nullptr, map_resources));
}

TEST_F(VrtDriverTest, CreateCopyFromVrtDataset) {
  constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1">
    <ColorInterp>Gray</ColorInterp>
  </VRTRasterBand>
</VRTDataset>)vrt";

  GDALDatasetUniquePtr src_vrt = OpenRasterReadOnly(kVrtXml);
  ASSERT_NE(nullptr, src_vrt);

  // CreateCopy to file in /vsimem/.
  constexpr char kDstFile[] = "/vsimem/vrtdriver_test/copied.vrt";
  {
    GDALDatasetUniquePtr copy_ds(poDriver_->CreateCopy(
        kDstFile, src_vrt.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, copy_ds);
    EXPECT_EQ(10, copy_ds->GetRasterXSize());
    EXPECT_EQ(10, copy_ds->GetRasterYSize());
  }
  EXPECT_EQ(0, VSIUnlink(kDstFile));

  // CreateCopy with empty filename (in-memory).
  {
    GDALDatasetUniquePtr copy_mem(poDriver_->CreateCopy(
        "", src_vrt.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, copy_mem);
    EXPECT_EQ(10, copy_mem->GetRasterXSize());
  }

  // CreateCopy failure when directory cannot be created.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr bad_copy(
        poDriver_->CreateCopy("/does_not_exist/dir/sub/file.vrt", src_vrt.get(),
                              FALSE, nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, bad_copy);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(VrtDriverTest, CreateCopyFromRegularDataset) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  GDALDatasetUniquePtr src_ds(
      mem_driver->Create("", 20, 20, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);

  const GDALGeoTransform gt(100.0, 1.0, 0.0, 200.0, 0.0, -1.0);
  EXPECT_EQ(CE_None, src_ds->SetGeoTransform(gt));

  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.SetWellKnownGeogCS("WGS84"));
  EXPECT_EQ(CE_None, src_ds->SetSpatialRef(&srs));

  src_ds->SetMetadataItem("SRC_KEY", "SRC_VAL");
  src_ds->SetMetadataItem("RPC_KEY", "RPC_VAL", "RPC");
  src_ds->SetMetadataItem("IMD_KEY", "IMD_VAL", "IMD");
  src_ds->SetMetadataItem("GEO_KEY", "GEO_VAL", "GEOLOCATION");
  src_ds->SetMetadataItem("CUSTOM_KEY", "CUSTOM_VAL", "MY_DOMAIN");
  src_ds->SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
  src_ds->SetMetadataItem("COMPRESSION", "DEFLATE", "IMAGE_STRUCTURE");

  GDALRasterBand* band1 = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band1);
  band1->SetMetadataItem("COMPRESSION", "DEFLATE", "IMAGE_STRUCTURE");
  EXPECT_EQ(CE_None, band1->SetNoDataValue(0.0));

  char gcp_id[] = "1";
  char gcp_info[] = "";
  GDAL_GCP gcps[1] = {{gcp_id, gcp_info, 0.5, 0.5, 10.0, 20.0, 0.0}};
  EXPECT_EQ(CE_None, src_ds->SetGCPs(1, gcps, &srs));
  EXPECT_EQ(CE_None, band1->CreateMaskBand(0));

  // CreateCopy with COPY_SRC_MDD=YES and custom block size.
  {
    int progress_calls = 0;
    constexpr char kDstFile[] = "/vsimem/vrtdriver_test/raster_copy.vrt";
    CPLStringList options;
    options.SetNameValue("COPY_SRC_MDD", "YES");
    options.SetNameValue("BLOCKXSIZE", "8");
    options.SetNameValue("BLOCKYSIZE", "8");
    GDALDatasetUniquePtr vrt_copy(
        poDriver_->CreateCopy(kDstFile, src_ds.get(), FALSE, options.List(),
                              DummyProgress, &progress_calls));
    ASSERT_NE(nullptr, vrt_copy);
    EXPECT_GT(progress_calls, 0);

    EXPECT_EQ(20, vrt_copy->GetRasterXSize());
    EXPECT_EQ(20, vrt_copy->GetRasterYSize());
    EXPECT_EQ(2, vrt_copy->GetRasterCount());

    GDALGeoTransform copy_gt;
    EXPECT_EQ(CE_None, vrt_copy->GetGeoTransform(copy_gt));
    EXPECT_THAT((std::span<const double, 6>(copy_gt.data(), 6)),
                ElementsAre(100.0, 1.0, 0.0, 200.0, 0.0, -1.0));
    ASSERT_NE(nullptr, vrt_copy->GetSpatialRef());
    EXPECT_THAT(*vrt_copy->GetSpatialRef(), IsSameAs(srs));

    EXPECT_STREQ("SRC_VAL", vrt_copy->GetMetadataItem("SRC_KEY"));
    EXPECT_STREQ("RPC_VAL", vrt_copy->GetMetadataItem("RPC_KEY", "RPC"));
    EXPECT_STREQ("CUSTOM_VAL",
                 vrt_copy->GetMetadataItem("CUSTOM_KEY", "MY_DOMAIN"));
    EXPECT_STREQ("PIXEL",
                 vrt_copy->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE"));
    EXPECT_EQ(1, vrt_copy->GetGCPCount());
  }

  // COPY_SRC_MDD=NO suppresses custom metadata domains.
  {
    CPLStringList no_mdd_opts;
    no_mdd_opts.SetNameValue("COPY_SRC_MDD", "NO");
    GDALDatasetUniquePtr vrt_copy(
        poDriver_->CreateCopy("/vsimem/vrtdriver_test/no_mdd.vrt", src_ds.get(),
                              FALSE, no_mdd_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, vrt_copy);
    EXPECT_EQ(nullptr, vrt_copy->GetMetadataItem("CUSTOM_KEY", "MY_DOMAIN"));
  }

  // SRC_MDD selects specific domains.
  {
    CPLStringList src_mdd_opts;
    src_mdd_opts.AddString("SRC_MDD=RPC");
    src_mdd_opts.AddString("SRC_MDD=_DEFAULT_");
    GDALDatasetUniquePtr vrt_copy(poDriver_->CreateCopy(
        "/vsimem/vrtdriver_test/src_mdd.vrt", src_ds.get(), FALSE,
        src_mdd_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, vrt_copy);
    EXPECT_STREQ("RPC_VAL", vrt_copy->GetMetadataItem("RPC_KEY", "RPC"));
    EXPECT_STREQ("SRC_VAL", vrt_copy->GetMetadataItem("SRC_KEY"));
    EXPECT_EQ(nullptr, vrt_copy->GetMetadataItem("CUSTOM_KEY", "MY_DOMAIN"));
  }
}

TEST_F(VrtDriverTest, CreateCopyDatasetMaskBand) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  GDALDatasetUniquePtr src_ds(
      mem_driver->Create("", 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);
  EXPECT_EQ(CE_None, src_ds->CreateMaskBand(GMF_PER_DATASET));

  GDALDatasetUniquePtr vrt_copy(
      poDriver_->CreateCopy("/vsimem/vrtdriver_test/ds_mask.vrt", src_ds.get(),
                            FALSE, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, vrt_copy);

  GDALRasterBand* band = vrt_copy->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  EXPECT_NE(0, band->GetMaskFlags() & GMF_PER_DATASET);
}

TEST_F(VrtDriverTest, CreateCopyMultiDimensional) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  GDALDatasetUniquePtr src_ds(
      mem_driver->CreateMultiDimensional("", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  ASSERT_NE(nullptr, src_ds->GetRootGroup());

  int progress_count = 0;
  GDALDatasetUniquePtr copy_ds(poDriver_->CreateCopy(
      "/vsimem/vrtdriver_test/multi_dst.vrt", src_ds.get(), FALSE, nullptr,
      DummyProgress, &progress_count));
  ASSERT_NE(nullptr, copy_ds);
  EXPECT_GT(progress_count, 0);
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover remaining defensive error paths in VRTDelete and
// VRTRename in vrtdriver.cpp.
