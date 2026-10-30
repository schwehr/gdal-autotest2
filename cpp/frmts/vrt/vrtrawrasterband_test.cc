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
// Tests for vrtrawrasterband.cpp.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_hash_set.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class TestVRTDataset : public VRTDataset {
 public:
  TestVRTDataset(int x, int y) : VRTDataset(x, y) { eAccess = GA_Update; }
};

class TestVRTRawRasterBand : public VRTRawRasterBand {
 public:
  TestVRTRawRasterBand(GDALDataset* ds, int band, GDALDataType type)
      : VRTRawRasterBand(ds, band, type) {
    eAccess = GA_Update;
  }
};

class VrtRawRasterBandTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALAllRegister(); }

  void TearDown() override {
    CPLSetConfigOption("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", nullptr);
    CPLSetConfigOption("GDAL_VRT_ENABLE_RAWRASTERBAND", nullptr);
  }
};

TEST_F(VrtRawRasterBandTest, DisabledByConfig) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  CPLSetConfigOption("GDAL_VRT_ENABLE_RAWRASTERBAND", "NO");
  EXPECT_FALSE(VRTDataset::IsRawRasterBandEnabled());

  const char* vrt_xml =
      "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
      "subClass=\"VRTRawRasterBand\">"
      "    <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/fake.bin</SourceFilename>"
      "  </VRTRasterBand>"
      "</VRTDataset>";
  GDALDataset* poDS = static_cast<GDALDataset*>(GDALOpen(vrt_xml, GA_ReadOnly));
  EXPECT_EQ(poDS, nullptr);

  CPLSetConfigOption("GDAL_VRT_ENABLE_RAWRASTERBAND", nullptr);
  CPLPopErrorHandler();
}

TEST_F(VrtRawRasterBandTest, ConstructWithUnknownType) {
  VRTDataset ds(10, 10);
  VRTRawRasterBand band(&ds, 1, GDT_Unknown);
  EXPECT_EQ(band.GetRasterDataType(), GDT_Byte);  // VRTRasterBand default
}

TEST_F(VrtRawRasterBandTest, OperationsWithoutRawRaster) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  VRTDataset ds(10, 10);
  VRTRawRasterBand band(&ds, 1, GDT_Byte);

  uint8_t buffer[100] = {0};
  EXPECT_EQ(band.RasterIO(GF_Read, 0, 0, 10, 10, buffer, 10, 10, GDT_Byte, 0, 0,
                          nullptr),
            CE_Failure);

  EXPECT_EQ(band.ReadBlock(0, 0, buffer), CE_Failure);
  EXPECT_EQ(band.WriteBlock(0, 0, buffer), CE_Failure);

  bool warned = false;
  size_t ram = 0;
  EXPECT_EQ(band.SerializeToXML(nullptr, warned, ram), nullptr);

  int pixel_space = 0;
  GIntBig line_space = 0;
  // GetVirtualMemAuto falls back to base class when m_poRawRaster is null
  CPLVirtualMem* vmem =
      band.GetVirtualMemAuto(GF_Read, &pixel_space, &line_space, nullptr);
  EXPECT_NE(vmem, nullptr);
  if (vmem != nullptr) {
    CPLVirtualMemFree(vmem);
  }

  CPLPopErrorHandler();
}

TEST_F(VrtRawRasterBandTest, SetRawLinkValidationErrors) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  VRTDataset ds(10, 10);
  VRTRawRasterBand band(&ds, 1, GDT_Byte);

  // Missing filename
  EXPECT_EQ(band.SetRawLink(nullptr, nullptr, FALSE, 0, 1, 10, nullptr),
            CE_Failure);

  // SIBLING_OR_CHILD default: relativeToVRT must be true
  EXPECT_EQ(band.SetRawLink("raw.bin", "/vsimem", FALSE, 0, 1, 10, nullptr),
            CE_Failure);

  // SIBLING_OR_CHILD: absolute path rejected
  EXPECT_EQ(
      band.SetRawLink("/vsimem/raw.bin", "/vsimem", TRUE, 0, 1, 10, nullptr),
      CE_Failure);

  // SIBLING_OR_CHILD: path containing ../ or ..\ rejected
  EXPECT_EQ(
      band.SetRawLink("../raw.bin", "/vsimem/sub", TRUE, 0, 1, 10, nullptr),
      CE_Failure);
  EXPECT_EQ(
      band.SetRawLink("..\\raw.bin", "/vsimem/sub", TRUE, 0, 1, 10, nullptr),
      CE_Failure);

  // ONLY_REMOTE: local file rejected
  CPLSetConfigOption("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ONLY_REMOTE");
  EXPECT_EQ(band.SetRawLink("raw.bin", "/vsimem", TRUE, 0, 1, 10, nullptr),
            CE_Failure);

  // Explicit allowed paths
  // Path with ../ rejected
  CPLSetConfigOption("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "/vsimem/dir");
  EXPECT_EQ(
      band.SetRawLink("../raw.bin", "/vsimem/dir", TRUE, 0, 1, 10, nullptr),
      CE_Failure);

  // Config option contains relative path -> error
  CPLSetConfigOption("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "relative/dir");
  EXPECT_EQ(band.SetRawLink("raw.bin", "/vsimem/dir", TRUE, 0, 1, 10, nullptr),
            CE_Failure);

  // Not in allowed paths
  CPLSetConfigOption("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "/vsimem/other");
  EXPECT_EQ(band.SetRawLink("raw.bin", "/vsimem/dir", TRUE, 0, 1, 10, nullptr),
            CE_Failure);

  CPLSetConfigOption("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", nullptr);
  CPLPopErrorHandler();
}

TEST_F(VrtRawRasterBandTest, SetRawLinkFileAndByteOrderErrors) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  VRTDataset ds(10, 10);
  VRTRawRasterBand band(&ds, 1, GDT_Byte);

  CPLSetConfigOption("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ALL");

  // File does not exist
  EXPECT_EQ(band.SetRawLink("/vsimem/nonexistent.bin", nullptr, FALSE, 0, 1, 10,
                            nullptr),
            CE_Failure);

  // Create valid file in vsimem
  const char* raw_file = "/vsimem/test_raw.bin";
  std::vector<uint8_t> data(100, 42);
  VSILFILE* fp = VSIFOpenL(raw_file, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(VSIFWriteL(data.data(), 1, data.size(), fp), data.size());
  ASSERT_EQ(VSIFCloseL(fp), 0);

  // Invalid ByteOrder
  EXPECT_EQ(
      band.SetRawLink(raw_file, nullptr, FALSE, 0, 1, 10, "INVALID_ORDER"),
      CE_Failure);

  // Memory usage check failure (overflow / absurd offsets)
  EXPECT_EQ(band.SetRawLink(raw_file, nullptr, FALSE, 1000000000, 1000000,
                            1000000, "LSB"),
            CE_Failure);

  VSIUnlink(raw_file);
  CPLPopErrorHandler();
}

TEST_F(VrtRawRasterBandTest, ReadWriteSuccessAndRasterIO) {
  CPLSetConfigOption("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ALL");

  const char* raw_file = "/vsimem/rw_test.bin";
  std::vector<uint8_t> initial_data(100, 0);
  for (int i = 0; i < 100; ++i) {
    initial_data[i] = static_cast<uint8_t>(i + 1);
  }
  VSILFILE* fp = VSIFOpenL(raw_file, "wb");
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(VSIFWriteL(initial_data.data(), 1, initial_data.size(), fp),
            initial_data.size());
  ASSERT_EQ(VSIFCloseL(fp), 0);

  VRTDataset ds(10, 10);
  auto band = std::make_unique<VRTRawRasterBand>(&ds, 1, GDT_Byte);
  ASSERT_EQ(band->SetRawLink(raw_file, nullptr, FALSE, 0, 1, 10, "LSB"),
            CE_None);

  // Test ReadBlock
  std::vector<uint8_t> block_data(10);
  EXPECT_EQ(band->ReadBlock(0, 0, block_data.data()), CE_None);
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(block_data[i], i + 1);
  }

  // Test Write on read-only dataset -> fails
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(band->RasterIO(GF_Write, 0, 0, 1, 1, block_data.data(), 1, 1,
                           GDT_Byte, 0, 0, nullptr),
            CE_Failure);
  CPLPopErrorHandler();

  // Test WriteBlock with GA_Update
  TestVRTDataset update_ds(10, 10);
  auto update_band =
      std::make_unique<TestVRTRawRasterBand>(&update_ds, 1, GDT_Byte);
  ASSERT_EQ(update_band->SetRawLink(raw_file, nullptr, FALSE, 0, 1, 10, "MSB"),
            CE_None);

  block_data[0] = 99;
  EXPECT_EQ(update_band->WriteBlock(0, 0, block_data.data()), CE_None);

  // Test RasterIO Read
  std::vector<uint8_t> read_buf(100);
  EXPECT_EQ(update_band->RasterIO(GF_Read, 0, 0, 10, 10, read_buf.data(), 10,
                                  10, GDT_Byte, 1, 10, nullptr),
            CE_None);
  EXPECT_EQ(read_buf[0], 99);

  // Test GetVirtualMemAuto with valid raw raster
  int pixel_space = 0;
  GIntBig line_space = 0;
  CPLVirtualMem* vmem = update_band->GetVirtualMemAuto(GF_Read, &pixel_space,
                                                       &line_space, nullptr);
  if (vmem != nullptr) {
    CPLVirtualMemFree(vmem);
  }

  // Test ByteOrder LSB serialization
  bool warned = false;
  size_t ram = 0;
  auto lsb_band = std::make_unique<VRTRawRasterBand>(&ds, 1, GDT_Byte);
  EXPECT_EQ(lsb_band->SetRawLink(raw_file, nullptr, FALSE, 0, 1, 10, "LSB"),
            CE_None);
  CPLXMLNode* lsb_xml = lsb_band->SerializeToXML(nullptr, warned, ram);
  ASSERT_NE(lsb_xml, nullptr);
  char* lsb_xml_str = CPLSerializeXMLTree(lsb_xml);
  EXPECT_NE(strstr(lsb_xml_str, "LSB"), nullptr);
  CPLFree(lsb_xml_str);
  CPLDestroyXMLNode(lsb_xml);

  // Test ByteOrder VAX
  auto vax_band = std::make_unique<VRTRawRasterBand>(&ds, 1, GDT_Byte);
  EXPECT_EQ(vax_band->SetRawLink(raw_file, nullptr, FALSE, 0, 1, 10, "VAX"),
            CE_None);

  // Test SerializeToXML
  CPLXMLNode* xml = update_band->SerializeToXML(nullptr, warned, ram);
  ASSERT_NE(xml, nullptr);
  char* xml_str = CPLSerializeXMLTree(xml);
  EXPECT_NE(xml_str, nullptr);
  EXPECT_NE(strstr(xml_str, "VRTRawRasterBand"), nullptr);
  EXPECT_NE(strstr(xml_str, "SourceFilename"), nullptr);
  CPLFree(xml_str);
  CPLDestroyXMLNode(xml);

  // Serialize with VAX byte order
  CPLXMLNode* vax_xml = vax_band->SerializeToXML(nullptr, warned, ram);
  ASSERT_NE(vax_xml, nullptr);
  char* vax_xml_str = CPLSerializeXMLTree(vax_xml);
  EXPECT_NE(strstr(vax_xml_str, "VAX"), nullptr);
  CPLFree(vax_xml_str);
  CPLDestroyXMLNode(vax_xml);

  // Test GetFileList
  char** file_list = nullptr;
  int file_list_size = 0;
  int file_list_max = 0;
  CPLHashSet* hash_set =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);

  update_band->GetFileList(&file_list, &file_list_size, &file_list_max,
                           hash_set);
  EXPECT_GE(file_list_size, 1);
  // Calling again tests the duplicate suppression in hash_set
  update_band->GetFileList(&file_list, &file_list_size, &file_list_max,
                           hash_set);
  EXPECT_GE(file_list_size, 1);

  // Test growing file list array
  for (int i = file_list_size; i < file_list_max; ++i) {
    char fake_name[64];
    snprintf(fake_name, sizeof(fake_name), "/vsimem/fake_%d.bin", i);
    file_list[file_list_size] = CPLStrdup(fake_name);
    file_list_size++;
  }
  // Now file_list_size == file_list_max, calling with a new filename triggers
  // resize
  auto new_band = std::make_unique<VRTRawRasterBand>(&ds, 1, GDT_Byte);
  const char* raw_file2 = "/vsimem/raw2.bin";
  VSILFILE* fp2 = VSIFOpenL(raw_file2, "wb");
  VSIFCloseL(fp2);
  ASSERT_EQ(new_band->SetRawLink(raw_file2, nullptr, FALSE, 0, 1, 10, "LSB"),
            CE_None);
  new_band->GetFileList(&file_list, &file_list_size, &file_list_max, hash_set);
  VSIUnlink(raw_file2);

  CSLDestroy(file_list);
  CPLHashSetDestroy(hash_set);

  // Test ClearRawLink
  update_band->ClearRawLink();

  // Test GetFileList with null source filename
  char** empty_list = nullptr;
  int empty_size = 0;
  int empty_max = 0;
  CPLHashSet* empty_hash =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);
  update_band->GetFileList(&empty_list, &empty_size, &empty_max, empty_hash);
  EXPECT_EQ(empty_size, 0);
  CPLHashSetDestroy(empty_hash);

  VSIUnlink(raw_file);
}

TEST_F(VrtRawRasterBandTest, SetRawLinkUpdateCreateFile) {
  CPLSetConfigOption("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ALL");
  const char* new_file = "/vsimem/new_created_file.bin";
  TestVRTDataset update_ds(10, 10);
  auto update_band =
      std::make_unique<TestVRTRawRasterBand>(&update_ds, 1, GDT_Byte);
  EXPECT_EQ(update_band->SetRawLink(new_file, nullptr, FALSE, 0, 1, 10, "LSB"),
            CE_None);
  VSIUnlink(new_file);
}

TEST_F(VrtRawRasterBandTest, GetFileListRelativeToVRT) {
  CPLSetConfigOption("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ALL");
  const char* raw_file = "/vsimem/dir/rel_raw.bin";
  VSILFILE* fp = VSIFOpenL(raw_file, "wb");
  VSIFCloseL(fp);

  VRTDataset ds(10, 10);
  ds.SetDescription("/vsimem/dir/test.vrt");
  auto band = std::make_unique<VRTRawRasterBand>(&ds, 1, GDT_Byte);
  ASSERT_EQ(
      band->SetRawLink("rel_raw.bin", "/vsimem/dir", TRUE, 0, 1, 10, "LSB"),
      CE_None);

  char** file_list = nullptr;
  int file_list_size = 0;
  int file_list_max = 0;
  CPLHashSet* hash_set =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);
  band->GetFileList(&file_list, &file_list_size, &file_list_max, hash_set);
  EXPECT_EQ(file_list_size, 1);

  CSLDestroy(file_list);
  CPLHashSetDestroy(hash_set);
  VSIUnlink(raw_file);
}

TEST_F(VrtRawRasterBandTest, XMLInitValidation) {
  CPLSetConfigOption("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ALL");

  const char* raw_file = "/vsimem/xml_raw.bin";
  std::vector<uint8_t> data(100, 7);
  VSILFILE* fp = VSIFOpenL(raw_file, "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(data.data(), 1, data.size(), fp);
  VSIFCloseL(fp);

  VRTDataset ds(10, 10);
  VRTRawRasterBand band(&ds, 1, GDT_Byte);
  VRTMapSharedResources map_resources;

  // XMLInit error: invalid root node
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(band.XMLInit(nullptr, nullptr, map_resources), CE_Failure);

  CPLXMLNode* invalid_node = CPLCreateXMLNode(nullptr, CXT_Text, "Invalid");
  EXPECT_EQ(band.XMLInit(invalid_node, nullptr, map_resources), CE_Failure);
  CPLDestroyXMLNode(invalid_node);

  CPLXMLNode* wrong_elem =
      CPLCreateXMLNode(nullptr, CXT_Element, "WrongElement");
  EXPECT_EQ(band.XMLInit(wrong_elem, nullptr, map_resources), CE_Failure);
  CPLDestroyXMLNode(wrong_elem);

  CPLXMLNode* wrong_subclass =
      CPLCreateXMLNode(nullptr, CXT_Element, "VRTRasterBand");
  CPLAddXMLAttributeAndValue(wrong_subclass, "subClass",
                             "VRTSourcedRasterBand");
  EXPECT_EQ(band.XMLInit(wrong_subclass, nullptr, map_resources), CE_Failure);
  CPLDestroyXMLNode(wrong_subclass);

  // Missing SourceFilename
  CPLXMLNode* missing_source =
      CPLCreateXMLNode(nullptr, CXT_Element, "VRTRasterBand");
  CPLAddXMLAttributeAndValue(missing_source, "subClass", "VRTRawRasterBand");
  EXPECT_EQ(band.XMLInit(missing_source, nullptr, map_resources), CE_Failure);
  CPLDestroyXMLNode(missing_source);

  // Invalid PixelOffset <= 0
  const char* invalid_pixel_offset_xml =
      "<VRTRasterBand dataType=\"Byte\" subClass=\"VRTRawRasterBand\">"
      "  <SourceFilename>/vsimem/xml_raw.bin</SourceFilename>"
      "  <PixelOffset>0</PixelOffset>"
      "</VRTRasterBand>";
  CPLXMLNode* invalid_po_tree = CPLParseXMLString(invalid_pixel_offset_xml);
  ASSERT_NE(invalid_po_tree, nullptr);
  EXPECT_EQ(band.XMLInit(invalid_po_tree, nullptr, map_resources), CE_Failure);
  CPLDestroyXMLNode(invalid_po_tree);

  // Integer overflow in LineOffset calculation
  const char* overflow_line_offset_xml =
      "<VRTRasterBand dataType=\"Byte\" subClass=\"VRTRawRasterBand\">"
      "  <SourceFilename>/vsimem/xml_raw.bin</SourceFilename>"
      "  <PixelOffset>1000000000</PixelOffset>"
      "</VRTRasterBand>";
  CPLXMLNode* overflow_tree = CPLParseXMLString(overflow_line_offset_xml);
  ASSERT_NE(overflow_tree, nullptr);
  EXPECT_EQ(band.XMLInit(overflow_tree, nullptr, map_resources), CE_Failure);
  CPLDestroyXMLNode(overflow_tree);

  CPLPopErrorHandler();

  // Valid XMLInit with all fields
  const char* valid_xml =
      "<VRTRasterBand dataType=\"Byte\" subClass=\"VRTRawRasterBand\">"
      "  <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/xml_raw.bin</SourceFilename>"
      "  <ImageOffset>0</ImageOffset>"
      "  <PixelOffset>1</PixelOffset>"
      "  <LineOffset>10</LineOffset>"
      "  <ByteOrder>LSB</ByteOrder>"
      "</VRTRasterBand>";
  CPLXMLNode* valid_tree = CPLParseXMLString(valid_xml);
  ASSERT_NE(valid_tree, nullptr);
  EXPECT_EQ(band.XMLInit(valid_tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(valid_tree);

  // Valid XMLInit with default LineOffset (not specified)
  const char* default_line_xml =
      "<VRTRasterBand dataType=\"Byte\" subClass=\"VRTRawRasterBand\">"
      "  <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/xml_raw.bin</SourceFilename>"
      "  <PixelOffset>1</PixelOffset>"
      "</VRTRasterBand>";
  CPLXMLNode* default_line_tree = CPLParseXMLString(default_line_xml);
  ASSERT_NE(default_line_tree, nullptr);
  EXPECT_EQ(band.XMLInit(default_line_tree, nullptr, map_resources), CE_None);
  CPLDestroyXMLNode(default_line_tree);

  VSIUnlink(raw_file);
}

TEST_F(VrtRawRasterBandTest, OpenDatasetFromVsimem) {
  CPLSetConfigOption("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", "ALL");

  const char* raw_file = "/vsimem/ds_raw.bin";
  std::vector<uint8_t> data(100, 123);
  VSILFILE* fp = VSIFOpenL(raw_file, "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(data.data(), 1, data.size(), fp);
  VSIFCloseL(fp);

  const char* vrt_file = "/vsimem/ds_test.vrt";
  const char* vrt_xml =
      "<VRTDataset rasterXSize=\"10\" rasterYSize=\"10\">"
      "  <VRTRasterBand dataType=\"Byte\" band=\"1\" "
      "subClass=\"VRTRawRasterBand\">"
      "    <SourceFilename "
      "relativeToVRT=\"0\">/vsimem/ds_raw.bin</SourceFilename>"
      "    <ImageOffset>0</ImageOffset>"
      "    <PixelOffset>1</PixelOffset>"
      "    <LineOffset>10</LineOffset>"
      "    <ByteOrder>LSB</ByteOrder>"
      "  </VRTRasterBand>"
      "</VRTDataset>";

  fp = VSIFOpenL(vrt_file, "wb");
  ASSERT_NE(fp, nullptr);
  VSIFWriteL(vrt_xml, 1, strlen(vrt_xml), fp);
  VSIFCloseL(fp);

  GDALDataset* poDS =
      static_cast<GDALDataset*>(GDALOpen(vrt_file, GA_ReadOnly));
  ASSERT_NE(poDS, nullptr);
  EXPECT_EQ(poDS->GetRasterXSize(), 10);
  EXPECT_EQ(poDS->GetRasterYSize(), 10);
  EXPECT_EQ(poDS->GetRasterCount(), 1);

  GDALRasterBand* poBand = poDS->GetRasterBand(1);
  ASSERT_NE(poBand, nullptr);
  uint8_t pixel_val = 0;
  EXPECT_EQ(poBand->RasterIO(GF_Read, 0, 0, 1, 1, &pixel_val, 1, 1, GDT_Byte, 0,
                             0, nullptr),
            CE_None);
  EXPECT_EQ(pixel_val, 123);

  GDALClose(poDS);
  VSIUnlink(vrt_file);
  VSIUnlink(raw_file);
}

}  // namespace
}  // namespace autotest2
