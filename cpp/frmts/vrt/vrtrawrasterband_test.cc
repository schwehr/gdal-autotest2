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
// Tests VRTRawRasterBand link validation, XML serialization, raw block I/O,
// file list tracking, and GDAL_VRT_ENABLE_RAWRASTERBAND handling
// (vrtrawrasterband.cpp).
//
// google3 build caveat:
//   //third_party/gdal/BUILD passes -DGDAL_VRT_ENABLE_RAWRASTERBAND=ON (see
//   b/429626570), so tests explicitly set GDAL_VRT_ENABLE_RAWRASTERBAND via
//   CPLConfigOptionSetter rather than assuming a build default.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/vrt/vrtrawrasterband.cpp
//   https://gdal.org/drivers/raster/vrt.html

#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
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

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/vrtrawrasterband_test";

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
  void SetUp() override { GDALRegister_VRT(); }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

TEST_F(VrtRawRasterBandTest, DisabledByConfig) {
  CPLConfigOptionSetter enable_raw("GDAL_VRT_ENABLE_RAWRASTERBAND", "NO",
                                   false);
  EXPECT_FALSE(VRTDataset::IsRawRasterBandEnabled());

  constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1" subClass="VRTRawRasterBand">
    <SourceFilename relativeToVRT="0">/vsimem/vrtrawrasterband_test/fake.bin</SourceFilename>
  </VRTRasterBand>
</VRTDataset>)vrt";

  WithQuietHandler quiet;
  CPLErrorReset();
  GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(GDALOpenEx(
      kVrtXml, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  EXPECT_EQ(nullptr, poDS);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(VrtRawRasterBandTest, ConstructWithUnknownType) {
  VRTDataset ds(10, 10);
  VRTRawRasterBand band(&ds, 1, GDT_Unknown);
  EXPECT_EQ(GDT_Byte, band.GetRasterDataType());
}

TEST_F(VrtRawRasterBandTest, OperationsWithoutRawRaster) {
  WithQuietHandler quiet;
  VRTDataset ds(10, 10);
  VRTRawRasterBand band(&ds, 1, GDT_Byte);

  uint8_t buffer[100] = {0};
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, band.RasterIO(GF_Read, 0, 0, 10, 10, buffer, 10, 10,
                                      GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(CE_Failure, band.ReadBlock(0, 0, buffer));
  EXPECT_EQ(CE_Failure, band.WriteBlock(0, 0, buffer));

  bool warned = false;
  size_t ram = 0;
  EXPECT_EQ(nullptr, band.SerializeToXML(nullptr, warned, ram));

  int pixel_space = 0;
  GIntBig line_space = 0;
  std::unique_ptr<CPLVirtualMem, decltype([](CPLVirtualMem* m) {
                    if (m != nullptr) CPLVirtualMemFree(m);
                  })>
      vmem(band.GetVirtualMemAuto(GF_Read, &pixel_space, &line_space, nullptr));
  EXPECT_NE(nullptr, vmem);
}

TEST_F(VrtRawRasterBandTest, SetRawLinkValidationErrors) {
  WithQuietHandler quiet;
  VRTDataset ds(10, 10);
  VRTRawRasterBand band(&ds, 1, GDT_Byte);

  // Missing filename.
  EXPECT_EQ(CE_Failure,
            band.SetRawLink(nullptr, nullptr, FALSE, 0, 1, 10, nullptr));

  // SIBLING_OR_CHILD default: relativeToVRT must be true.
  EXPECT_EQ(CE_Failure,
            band.SetRawLink("raw.bin", kTestDir, FALSE, 0, 1, 10, nullptr));

  // SIBLING_OR_CHILD: absolute path rejected.
  EXPECT_EQ(CE_Failure, band.SetRawLink("/vsimem/vrtrawrasterband_test/raw.bin",
                                        kTestDir, TRUE, 0, 1, 10, nullptr));

  // SIBLING_OR_CHILD: parent traversal rejected.
  EXPECT_EQ(CE_Failure,
            band.SetRawLink("../raw.bin", "/vsimem/vrtrawrasterband_test/sub",
                            TRUE, 0, 1, 10, nullptr));
  EXPECT_EQ(CE_Failure,
            band.SetRawLink("..\\raw.bin", "/vsimem/vrtrawrasterband_test/sub",
                            TRUE, 0, 1, 10, nullptr));

  // ONLY_REMOTE: local file rejected.
  {
    CPLConfigOptionSetter setter("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE",
                                 "ONLY_REMOTE", false);
    EXPECT_EQ(CE_Failure,
              band.SetRawLink("raw.bin", kTestDir, TRUE, 0, 1, 10, nullptr));
  }

  // Explicit allowed directory rejects parent traversal.
  {
    CPLConfigOptionSetter setter("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE",
                                 "/vsimem/vrtrawrasterband_test/dir", false);
    EXPECT_EQ(CE_Failure,
              band.SetRawLink("../raw.bin", "/vsimem/vrtrawrasterband_test/dir",
                              TRUE, 0, 1, 10, nullptr));
  }

  // Relative allowed path config option fails.
  {
    CPLConfigOptionSetter setter("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE",
                                 "relative/dir", false);
    EXPECT_EQ(CE_Failure,
              band.SetRawLink("raw.bin", "/vsimem/vrtrawrasterband_test/dir",
                              TRUE, 0, 1, 10, nullptr));
  }

  // Path outside allowed directory fails.
  {
    CPLConfigOptionSetter setter("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE",
                                 "/vsimem/vrtrawrasterband_test/other", false);
    EXPECT_EQ(CE_Failure,
              band.SetRawLink("raw.bin", "/vsimem/vrtrawrasterband_test/dir",
                              TRUE, 0, 1, 10, nullptr));
  }
}

TEST_F(VrtRawRasterBandTest, SetRawLinkFileAndByteOrderErrors) {
  CPLConfigOptionSetter enable_raw("GDAL_VRT_ENABLE_RAWRASTERBAND", "YES",
                                   false);
  CPLConfigOptionSetter allow_src("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE",
                                  "ALL", false);
  WithQuietHandler quiet;
  VRTDataset ds(10, 10);
  VRTRawRasterBand band(&ds, 1, GDT_Byte);

  // Non-existent file.
  EXPECT_EQ(CE_Failure,
            band.SetRawLink("/vsimem/vrtrawrasterband_test/nonexistent.bin",
                            nullptr, FALSE, 0, 1, 10, nullptr));

  constexpr char kRawFile[] = "/vsimem/vrtrawrasterband_test/test_raw.bin";
  VsiMemTempWrapper raw_file(kRawFile, std::string(100, '\x2A'));

  // Invalid ByteOrder.
  EXPECT_EQ(CE_Failure, band.SetRawLink(kRawFile, nullptr, FALSE, 0, 1, 10,
                                        "INVALID_ORDER"));

  // File smaller than required offset + band extent fails in
  // RAWDatasetCheckMemoryUsage ("Image file is too small").
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, band.SetRawLink(kRawFile, nullptr, FALSE, 1000000000,
                                        1000000, 1000000, "LSB"));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("too small"));
}

TEST_F(VrtRawRasterBandTest, ReadWriteSuccessAndRasterIO) {
  CPLConfigOptionSetter enable_raw("GDAL_VRT_ENABLE_RAWRASTERBAND", "YES",
                                   false);
  CPLConfigOptionSetter allow_src("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE",
                                  "ALL", false);

  constexpr char kRawFile[] = "/vsimem/vrtrawrasterband_test/rw_test.bin";
  std::string initial_data(100, '\0');
  for (int i = 0; i < 100; ++i) {
    initial_data[i] = static_cast<char>(i + 1);
  }
  VsiMemTempWrapper raw_file(kRawFile, initial_data);

  VRTDataset ds(10, 10);
  VRTRawRasterBand band(&ds, 1, GDT_Byte);
  ASSERT_EQ(CE_None,
            band.SetRawLink(kRawFile, nullptr, FALSE, 0, 1, 10, "LSB"));

  std::vector<uint8_t> block_data(10);
  EXPECT_EQ(CE_None, band.ReadBlock(0, 0, block_data.data()));
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(i + 1), block_data[i]);
  }

  // Write on read-only dataset fails.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band.RasterIO(GF_Write, 0, 0, 1, 1, block_data.data(),
                                        1, 1, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // WriteBlock and RasterIO read on GA_Update dataset.
  TestVRTDataset update_ds(10, 10);
  TestVRTRawRasterBand update_band(&update_ds, 1, GDT_Byte);
  ASSERT_EQ(CE_None,
            update_band.SetRawLink(kRawFile, nullptr, FALSE, 0, 1, 10, "MSB"));

  block_data[0] = 99;
  EXPECT_EQ(CE_None, update_band.WriteBlock(0, 0, block_data.data()));

  std::vector<uint8_t> read_buf(100);
  EXPECT_EQ(CE_None,
            update_band.RasterIO(GF_Read, 0, 0, 10, 10, read_buf.data(), 10, 10,
                                 GDT_Byte, 1, 10, nullptr));
  EXPECT_EQ(99, read_buf[0]);

  int pixel_space = 0;
  GIntBig line_space = 0;
  std::unique_ptr<CPLVirtualMem, decltype([](CPLVirtualMem* m) {
                    if (m != nullptr) CPLVirtualMemFree(m);
                  })>
      vmem(update_band.GetVirtualMemAuto(GF_Read, &pixel_space, &line_space,
                                         nullptr));

  bool warned = false;
  size_t ram = 0;
  for (const char* order : {"LSB", "MSB", "VAX"}) {
    VRTRawRasterBand order_band(&ds, 1, GDT_Byte);
    EXPECT_EQ(CE_None,
              order_band.SetRawLink(kRawFile, nullptr, FALSE, 0, 1, 10, order));
    CPLXMLTreeCloser xml(order_band.SerializeToXML(nullptr, warned, ram));
    ASSERT_NE(nullptr, xml);
    std::unique_ptr<char, CplFreeDeleter> xml_str(
        CPLSerializeXMLTree(xml.get()));
    ASSERT_NE(nullptr, xml_str);
    EXPECT_THAT(xml_str.get(), HasSubstr("VRTRawRasterBand"));
    EXPECT_THAT(xml_str.get(), HasSubstr("SourceFilename"));
    EXPECT_THAT(xml_str.get(), HasSubstr(order));
  }

  // GetFileList deduplication and internal array growth.
  char** file_list = nullptr;
  int file_list_size = 0;
  int file_list_max = 0;
  CPLHashSet* hash_set =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);
  absl::Cleanup cleanup_list = [&file_list, &hash_set] {
    CSLDestroy(file_list);
    CPLHashSetDestroy(hash_set);
  };

  update_band.GetFileList(&file_list, &file_list_size, &file_list_max,
                          hash_set);
  EXPECT_GE(file_list_size, 1);
  update_band.GetFileList(&file_list, &file_list_size, &file_list_max,
                          hash_set);
  EXPECT_GE(file_list_size, 1);

  while (file_list_size < file_list_max) {
    const int idx = file_list_size;
    file_list[file_list_size++] = CPLStrdup(
        std::format("/vsimem/vrtrawrasterband_test/fake_{}.bin", idx).c_str());
  }
  constexpr char kRawFile2[] = "/vsimem/vrtrawrasterband_test/raw2.bin";
  VsiMemTempWrapper raw_file2(kRawFile2, "");
  VRTRawRasterBand new_band(&ds, 1, GDT_Byte);
  ASSERT_EQ(CE_None,
            new_band.SetRawLink(kRawFile2, nullptr, FALSE, 0, 1, 10, "LSB"));
  new_band.GetFileList(&file_list, &file_list_size, &file_list_max, hash_set);

  // ClearRawLink clears filename; subsequent GetFileList adds nothing.
  update_band.ClearRawLink();
  char** empty_list = nullptr;
  int empty_size = 0;
  int empty_max = 0;
  CPLHashSet* empty_hash =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);
  absl::Cleanup cleanup_empty = [&empty_list, &empty_hash] {
    CSLDestroy(empty_list);
    CPLHashSetDestroy(empty_hash);
  };
  update_band.GetFileList(&empty_list, &empty_size, &empty_max, empty_hash);
  EXPECT_EQ(0, empty_size);
}

TEST_F(VrtRawRasterBandTest, SetRawLinkUpdateCreateFile) {
  CPLConfigOptionSetter enable_raw("GDAL_VRT_ENABLE_RAWRASTERBAND", "YES",
                                   false);
  CPLConfigOptionSetter allow_src("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE",
                                  "ALL", false);
  TestVRTDataset update_ds(10, 10);
  TestVRTRawRasterBand update_band(&update_ds, 1, GDT_Byte);
  EXPECT_EQ(CE_None, update_band.SetRawLink(
                         "/vsimem/vrtrawrasterband_test/new_created_file.bin",
                         nullptr, FALSE, 0, 1, 10, "LSB"));
}

TEST_F(VrtRawRasterBandTest, GetFileListRelativeToVRT) {
  CPLConfigOptionSetter enable_raw("GDAL_VRT_ENABLE_RAWRASTERBAND", "YES",
                                   false);
  CPLConfigOptionSetter allow_src("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE",
                                  "ALL", false);
  VsiMemTempWrapper raw_file("/vsimem/vrtrawrasterband_test/dir/rel_raw.bin",
                             "");

  VRTDataset ds(10, 10);
  ds.SetDescription("/vsimem/vrtrawrasterband_test/dir/test.vrt");
  VRTRawRasterBand band(&ds, 1, GDT_Byte);
  ASSERT_EQ(CE_None,
            band.SetRawLink("rel_raw.bin", "/vsimem/vrtrawrasterband_test/dir",
                            TRUE, 0, 1, 10, "LSB"));

  char** file_list = nullptr;
  int file_list_size = 0;
  int file_list_max = 0;
  CPLHashSet* hash_set =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, nullptr);
  absl::Cleanup cleanup_list = [&file_list, &hash_set] {
    CSLDestroy(file_list);
    CPLHashSetDestroy(hash_set);
  };
  band.GetFileList(&file_list, &file_list_size, &file_list_max, hash_set);
  EXPECT_EQ(1, file_list_size);
}

TEST_F(VrtRawRasterBandTest, XMLInitValidation) {
  CPLConfigOptionSetter enable_raw("GDAL_VRT_ENABLE_RAWRASTERBAND", "YES",
                                   false);
  CPLConfigOptionSetter allow_src("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE",
                                  "ALL", false);
  VsiMemTempWrapper raw_file("/vsimem/vrtrawrasterband_test/xml_raw.bin",
                             std::string(100, '\x07'));

  VRTDataset ds(10, 10);
  VRTRawRasterBand band(&ds, 1, GDT_Byte);
  VRTMapSharedResources map_resources;

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band.XMLInit(nullptr, nullptr, map_resources));

    CPLXMLTreeCloser invalid_node(
        CPLCreateXMLNode(nullptr, CXT_Text, "Invalid"));
    EXPECT_EQ(CE_Failure,
              band.XMLInit(invalid_node.get(), nullptr, map_resources));

    CPLXMLTreeCloser wrong_elem(
        CPLCreateXMLNode(nullptr, CXT_Element, "WrongElement"));
    EXPECT_EQ(CE_Failure,
              band.XMLInit(wrong_elem.get(), nullptr, map_resources));

    constexpr const char* kBadXmls[] = {
        R"xml(<VRTRasterBand subClass="VRTSourcedRasterBand"/>)xml",
        R"xml(<VRTRasterBand subClass="VRTRawRasterBand"/>)xml",
        R"xml(
<VRTRasterBand dataType="Byte" subClass="VRTRawRasterBand">
  <SourceFilename>/vsimem/vrtrawrasterband_test/xml_raw.bin</SourceFilename>
  <PixelOffset>0</PixelOffset>
</VRTRasterBand>)xml",
        R"xml(
<VRTRasterBand dataType="Byte" subClass="VRTRawRasterBand">
  <SourceFilename>/vsimem/vrtrawrasterband_test/xml_raw.bin</SourceFilename>
  <PixelOffset>1000000000</PixelOffset>
</VRTRasterBand>)xml",
    };
    for (const char* xml : kBadXmls) {
      CPLErrorReset();
      CPLXMLTreeCloser tree(CPLParseXMLString(xml));
      ASSERT_NE(nullptr, tree);
      EXPECT_EQ(CE_Failure, band.XMLInit(tree.get(), nullptr, map_resources));
    }
  }

  constexpr char kValidXml[] = R"xml(
<VRTRasterBand dataType="Byte" subClass="VRTRawRasterBand">
  <SourceFilename relativeToVRT="0">/vsimem/vrtrawrasterband_test/xml_raw.bin</SourceFilename>
  <ImageOffset>0</ImageOffset>
  <PixelOffset>1</PixelOffset>
  <LineOffset>10</LineOffset>
  <ByteOrder>LSB</ByteOrder>
</VRTRasterBand>)xml";
  {
    CPLXMLTreeCloser tree(CPLParseXMLString(kValidXml));
    ASSERT_NE(nullptr, tree);
    EXPECT_EQ(CE_None, band.XMLInit(tree.get(), nullptr, map_resources));
  }

  constexpr char kDefaultLineXml[] = R"xml(
<VRTRasterBand dataType="Byte" subClass="VRTRawRasterBand">
  <SourceFilename relativeToVRT="0">/vsimem/vrtrawrasterband_test/xml_raw.bin</SourceFilename>
  <PixelOffset>1</PixelOffset>
</VRTRasterBand>)xml";
  {
    CPLXMLTreeCloser tree(CPLParseXMLString(kDefaultLineXml));
    ASSERT_NE(nullptr, tree);
    EXPECT_EQ(CE_None, band.XMLInit(tree.get(), nullptr, map_resources));
  }
}

TEST_F(VrtRawRasterBandTest, OpenDatasetFromVsimem) {
  CPLConfigOptionSetter enable_raw("GDAL_VRT_ENABLE_RAWRASTERBAND", "YES",
                                   false);
  CPLConfigOptionSetter allow_src("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE",
                                  "ALL", false);

  VsiMemTempWrapper raw_file("/vsimem/vrtrawrasterband_test/ds_raw.bin",
                             std::string(100, '\x7B'));
  constexpr char kVrtXml[] = R"vrt(
<VRTDataset rasterXSize="10" rasterYSize="10">
  <VRTRasterBand dataType="Byte" band="1" subClass="VRTRawRasterBand">
    <SourceFilename relativeToVRT="0">/vsimem/vrtrawrasterband_test/ds_raw.bin</SourceFilename>
    <ImageOffset>0</ImageOffset>
    <PixelOffset>1</PixelOffset>
    <LineOffset>10</LineOffset>
    <ByteOrder>LSB</ByteOrder>
  </VRTRasterBand>
</VRTDataset>)vrt";
  constexpr char kVrtFile[] = "/vsimem/vrtrawrasterband_test/ds_test.vrt";
  VsiMemTempWrapper vrt_file(kVrtFile, kVrtXml);

  GDALDatasetUniquePtr poDS(GDALDataset::FromHandle(GDALOpenEx(
      kVrtFile, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poDS);
  EXPECT_EQ(10, poDS->GetRasterXSize());
  EXPECT_EQ(10, poDS->GetRasterYSize());
  EXPECT_EQ(1, poDS->GetRasterCount());

  GDALRasterBand* poBand = poDS->GetRasterBand(1);
  ASSERT_NE(nullptr, poBand);
  uint8_t pixel_val = 0;
  EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 0, 0, 1, 1, &pixel_val, 1, 1,
                                      GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(123, pixel_val);
}

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover remaining defensive OOM/I/O error paths in
// VRTRawRasterBand::SetRawLink and ReadBlock/WriteBlock in
// vrtrawrasterband.cpp.
