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
// Tests for vrtdriver.cpp.

#include <cstring>
#include <memory>

#include "testing/base/public/gunit.h"
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

int DummyProgress(double dfComplete, const char* pszMessage,
                  void* pProgressArg) {
  if (pProgressArg != nullptr) {
    *static_cast<int*>(pProgressArg) += 1;
  }
  return TRUE;
}

VRTSource* DummySourceParser(const CPLXMLNode* psSrc, const char* pszVRTPath,
                             VRTMapSharedResources& oMapSharedSources) {
  return new VRTSimpleSource();
}

class VrtDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    poDriver_ =
        static_cast<VRTDriver*>(GetGDALDriverManager()->GetDriverByName("VRT"));
    ASSERT_NE(poDriver_, nullptr);
  }

  VRTDriver* poDriver_ = nullptr;
};

TEST_F(VrtDriverTest, RegisterAgain) {
  // Calling GDALRegister_VRT when already registered is a no-op
  GDALRegister_VRT();
  EXPECT_NE(GetGDALDriverManager()->GetDriverByName("VRT"), nullptr);
}

TEST_F(VrtDriverTest, MetadataDomainListAndMetadata) {
  char** domain_list = poDriver_->GetMetadataDomainList();
  ASSERT_NE(domain_list, nullptr);
  EXPECT_GE(CSLFindString(domain_list, "SourceParsers"), 0);
  CSLDestroy(domain_list);

  char** source_parsers = poDriver_->GetMetadata("SourceParsers");
  EXPECT_NE(source_parsers, nullptr);

  char** default_meta = poDriver_->GetMetadata(nullptr);
  EXPECT_NE(default_meta, nullptr);

  // SetMetadata on custom domain
  char* custom_meta[] = {CPLStrdup("KEY=VALUE"), nullptr};
  EXPECT_EQ(poDriver_->SetMetadata(custom_meta, "CUSTOM_DOMAIN"), CE_None);
  char** retrieved = poDriver_->GetMetadata("CUSTOM_DOMAIN");
  ASSERT_NE(retrieved, nullptr);
  EXPECT_STREQ(CSLFetchNameValue(retrieved, "KEY"), "VALUE");
  CPLFree(custom_meta[0]);
}

TEST_F(VrtDriverTest, AddAndParseSource) {
  VRTDriver driver;
  VRTMapSharedResources map_resources;

  // ParseSource on null or non-element node
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(driver.ParseSource(nullptr, nullptr, map_resources), nullptr);
  CPLXMLNode* text_node = CPLCreateXMLNode(nullptr, CXT_Text, "text");
  EXPECT_EQ(driver.ParseSource(text_node, nullptr, map_resources), nullptr);
  CPLDestroyXMLNode(text_node);
  CPLPopErrorHandler();

  // Add a source parser
  driver.AddSourceParser("MyCustomSource", DummySourceParser);

  // Parse matching node
  CPLXMLNode* src_node =
      CPLCreateXMLNode(nullptr, CXT_Element, "MyCustomSource");
  VRTSource* source = driver.ParseSource(src_node, nullptr, map_resources);
  EXPECT_NE(source, nullptr);
  delete source;

  // Parse non-matching node
  CPLXMLNode* other_node =
      CPLCreateXMLNode(nullptr, CXT_Element, "UnknownSource");
  EXPECT_EQ(driver.ParseSource(other_node, nullptr, map_resources), nullptr);

  // Test the fallback path when m_oMapSourceParser is empty:
  // SetMetadata with "SourceParsers" clears m_oMapSourceParser but sets
  // papszSourceParsers. Duplicate first because SetMetadata destroys
  // papszSourceParsers before duplicating the input list.
  CPLStringList parsers_meta(CSLDuplicate(driver.GetMetadata("SourceParsers")));
  driver.SetMetadata(parsers_meta.List(), "SourceParsers");

  // Now m_oMapSourceParser is empty, but papszSourceParsers has
  // "MyCustomSource"
  source = driver.ParseSource(src_node, nullptr, map_resources);
  EXPECT_NE(source, nullptr);
  delete source;

  EXPECT_EQ(driver.ParseSource(other_node, nullptr, map_resources), nullptr);

  // With corrupt pointer string in papszSourceParsers
  char* corrupt_meta[] = {CPLStrdup("MyCustomSource="), nullptr};
  driver.SetMetadata(corrupt_meta, "SourceParsers");
  EXPECT_EQ(driver.ParseSource(src_node, nullptr, map_resources), nullptr);
  CPLFree(corrupt_meta[0]);

  CPLDestroyXMLNode(src_node);
  CPLDestroyXMLNode(other_node);
}

TEST_F(VrtDriverTest, CreateCopyFromVrtDataset) {
  const char* vrt_xml =
      "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\">"
      "    <ColorInterp>Gray</ColorInterp>"
      "  </VRTRasterBand>"
      "</VRTDataset>";

  GDALDataset* src_vrt =
      static_cast<GDALDataset*>(GDALOpen(vrt_xml, GA_ReadOnly));
  ASSERT_NE(src_vrt, nullptr);

  // CreateCopy to file in vsimem
  const char* dst_file = "/vsimem/copied.vrt";
  GDALDataset* copy_ds = poDriver_->CreateCopy(dst_file, src_vrt, FALSE,
                                               nullptr, nullptr, nullptr);
  ASSERT_NE(copy_ds, nullptr);
  EXPECT_EQ(copy_ds->GetRasterXSize(), 10);
  EXPECT_EQ(copy_ds->GetRasterYSize(), 10);
  GDALClose(copy_ds);
  VSIUnlink(dst_file);

  // CreateCopy with empty filename (in-memory)
  GDALDataset* copy_mem =
      poDriver_->CreateCopy("", src_vrt, FALSE, nullptr, nullptr, nullptr);
  ASSERT_NE(copy_mem, nullptr);
  EXPECT_EQ(copy_mem->GetRasterXSize(), 10);
  GDALClose(copy_mem);

  // CreateCopy failure when file cannot be created
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALDataset* bad_copy =
      poDriver_->CreateCopy("/does_not_exist/dir/sub/file.vrt", src_vrt, FALSE,
                            nullptr, nullptr, nullptr);
  EXPECT_EQ(bad_copy, nullptr);
  CPLPopErrorHandler();

  GDALClose(src_vrt);
}

TEST_F(VrtDriverTest, CreateCopyFromRegularDataset) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(mem_driver, nullptr);

  GDALDataset* src_ds = mem_driver->Create("", 20, 20, 2, GDT_Byte, nullptr);
  ASSERT_NE(src_ds, nullptr);

  // GeoTransform
  const GDALGeoTransform gt(100.0, 1.0, 0.0, 200.0, 0.0, -1.0);
  src_ds->SetGeoTransform(gt);

  // SpatialReference
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  src_ds->SetSpatialRef(&srs);

  // Metadata: default, special domains, custom domain, reserved domains
  src_ds->SetMetadataItem("SRC_KEY", "SRC_VAL");
  src_ds->SetMetadataItem("RPC_KEY", "RPC_VAL", "RPC");
  src_ds->SetMetadataItem("IMD_KEY", "IMD_VAL", "IMD");
  src_ds->SetMetadataItem("GEO_KEY", "GEO_VAL", "GEOLOCATION");
  src_ds->SetMetadataItem("CUSTOM_KEY", "CUSTOM_VAL", "MY_DOMAIN");
  src_ds->SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
  src_ds->SetMetadataItem("COMPRESSION", "DEFLATE", "IMAGE_STRUCTURE");

  // Band metadata
  GDALRasterBand* band1 = src_ds->GetRasterBand(1);
  ASSERT_NE(band1, nullptr);
  band1->SetMetadataItem("COMPRESSION", "DEFLATE", "IMAGE_STRUCTURE");
  band1->SetNoDataValue(0.0);

  // GCPs
  GDAL_GCP gcps[1];
  gcps[0].pszId = CPLStrdup("1");
  gcps[0].pszInfo = CPLStrdup("");
  gcps[0].dfGCPPixel = 0.5;
  gcps[0].dfGCPLine = 0.5;
  gcps[0].dfGCPX = 10.0;
  gcps[0].dfGCPY = 20.0;
  gcps[0].dfGCPZ = 0.0;
  src_ds->SetGCPs(1, gcps, &srs);
  CPLFree(gcps[0].pszId);
  CPLFree(gcps[0].pszInfo);

  // Per-band mask
  EXPECT_EQ(band1->CreateMaskBand(0), CE_None);

  // CreateCopy with various options
  int progress_calls = 0;
  const char* dst_file = "/vsimem/raster_copy.vrt";
  const char* options[] = {"COPY_SRC_MDD=YES", "BLOCKXSIZE=8", "BLOCKYSIZE=8",
                           nullptr};
  GDALDataset* vrt_copy = poDriver_->CreateCopy(dst_file, src_ds, FALSE,
                                                const_cast<char**>(options),
                                                DummyProgress, &progress_calls);
  ASSERT_NE(vrt_copy, nullptr);
  EXPECT_GT(progress_calls, 0);

  EXPECT_EQ(vrt_copy->GetRasterXSize(), 20);
  EXPECT_EQ(vrt_copy->GetRasterYSize(), 20);
  EXPECT_EQ(vrt_copy->GetRasterCount(), 2);

  GDALGeoTransform copy_gt;
  EXPECT_EQ(vrt_copy->GetGeoTransform(copy_gt), CE_None);
  EXPECT_DOUBLE_EQ(copy_gt[0], 100.0);

  EXPECT_STREQ(vrt_copy->GetMetadataItem("SRC_KEY"), "SRC_VAL");
  EXPECT_STREQ(vrt_copy->GetMetadataItem("RPC_KEY", "RPC"), "RPC_VAL");
  EXPECT_STREQ(vrt_copy->GetMetadataItem("CUSTOM_KEY", "MY_DOMAIN"),
               "CUSTOM_VAL");
  EXPECT_STREQ(vrt_copy->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE"),
               "PIXEL");

  EXPECT_EQ(vrt_copy->GetGCPCount(), 1);

  GDALClose(vrt_copy);
  VSIUnlink(dst_file);

  // Test with COPY_SRC_MDD=NO
  const char* no_mdd_opts[] = {"COPY_SRC_MDD=NO", nullptr};
  vrt_copy =
      poDriver_->CreateCopy("/vsimem/no_mdd.vrt", src_ds, FALSE,
                            const_cast<char**>(no_mdd_opts), nullptr, nullptr);
  ASSERT_NE(vrt_copy, nullptr);
  EXPECT_EQ(vrt_copy->GetMetadataItem("CUSTOM_KEY", "MY_DOMAIN"), nullptr);
  GDALClose(vrt_copy);
  VSIUnlink("/vsimem/no_mdd.vrt");

  // Test with SRC_MDD options
  const char* src_mdd_opts[] = {"SRC_MDD=RPC", "SRC_MDD=_DEFAULT_", nullptr};
  vrt_copy =
      poDriver_->CreateCopy("/vsimem/src_mdd.vrt", src_ds, FALSE,
                            const_cast<char**>(src_mdd_opts), nullptr, nullptr);
  ASSERT_NE(vrt_copy, nullptr);
  EXPECT_STREQ(vrt_copy->GetMetadataItem("RPC_KEY", "RPC"), "RPC_VAL");
  EXPECT_STREQ(vrt_copy->GetMetadataItem("SRC_KEY"), "SRC_VAL");
  EXPECT_EQ(vrt_copy->GetMetadataItem("CUSTOM_KEY", "MY_DOMAIN"), nullptr);
  GDALClose(vrt_copy);
  VSIUnlink("/vsimem/src_mdd.vrt");

  GDALClose(src_ds);
}

TEST_F(VrtDriverTest, CreateCopyDatasetMaskBand) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(mem_driver, nullptr);

  GDALDataset* src_ds = mem_driver->Create("", 10, 10, 1, GDT_Byte, nullptr);
  ASSERT_NE(src_ds, nullptr);

  // Dataset-level mask band
  EXPECT_EQ(src_ds->CreateMaskBand(GMF_PER_DATASET), CE_None);

  const char* dst_file = "/vsimem/ds_mask.vrt";
  GDALDataset* vrt_copy =
      poDriver_->CreateCopy(dst_file, src_ds, FALSE, nullptr, nullptr, nullptr);
  ASSERT_NE(vrt_copy, nullptr);

  GDALRasterBand* band = vrt_copy->GetRasterBand(1);
  ASSERT_NE(band, nullptr);
  EXPECT_TRUE(band->GetMaskFlags() & GMF_PER_DATASET);

  GDALClose(vrt_copy);
  VSIUnlink(dst_file);
  GDALClose(src_ds);
}

TEST_F(VrtDriverTest, CreateCopyMultiDimensional) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(mem_driver, nullptr);

  auto src_ds = std::unique_ptr<GDALDataset>(
      mem_driver->CreateMultiDimensional("", nullptr, nullptr));
  ASSERT_NE(src_ds, nullptr);
  auto root_group = src_ds->GetRootGroup();
  ASSERT_NE(root_group, nullptr);

  const char* dst_file = "/vsimem/multi_dst.vrt";
  int progress_count = 0;
  GDALDataset* copy_ds = poDriver_->CreateCopy(
      dst_file, src_ds.get(), FALSE, nullptr, DummyProgress, &progress_count);
  ASSERT_NE(copy_ds, nullptr);
  EXPECT_GT(progress_count, 0);

  GDALClose(copy_ds);
  VSIUnlink(dst_file);
}

}  // namespace
}  // namespace autotest2
