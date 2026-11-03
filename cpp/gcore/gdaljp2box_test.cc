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
// Tests JP2000 box reading, writing, and dumping (gdaljp2box.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdaljp2box.cpp

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdaljp2metadata.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kSuiteDir[] = "/vsimem/gdaljp2box_test";

TEST(GdalJp2BoxTest, WriteAndCreateBoxes) {
  GDALJP2Box box1;
  box1.SetType("ftyp");
  box1.AppendUInt32(0x01020304);
  box1.AppendUInt16(0x0506);
  box1.AppendUInt8(0x07);
  EXPECT_EQ(7, box1.GetDataLength());
  EXPECT_NE(nullptr, box1.GetWritableData());

  std::unique_ptr<GByte, CplFreeDeleter> raw(box1.GetWritableBoxData());
  ASSERT_NE(nullptr, raw);
  EXPECT_EQ(0, std::memcmp(raw.get() + 4, "ftyp", 4));

  // SetWritableData replaces existing data.
  constexpr std::array<GByte, 3> kPayload = {10, 20, 30};
  box1.SetWritableData(kPayload.size(), kPayload.data());
  EXPECT_EQ(3, box1.GetDataLength());

  // UUID boxes (GeoTIFF, MSI, XMP).
  constexpr std::array<GByte, 16> kUuidGeoTiff = {
      0xB1, 0x4B, 0xF8, 0xBD, 0x08, 0x3D, 0x4B, 0x43,
      0xA5, 0xAE, 0x8C, 0xD7, 0xD5, 0xA6, 0xCE, 0x03};
  constexpr std::array<GByte, 16> kUuidMsi = {
      0x96, 0xA9, 0xF1, 0xF1, 0xDC, 0x98, 0x40, 0x2D,
      0xA7, 0xAE, 0xD6, 0x8E, 0x34, 0x45, 0x18, 0x09};
  constexpr std::array<GByte, 16> kUuidXmp = {
      0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8,
      0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC};

  std::unique_ptr<GDALJP2Box> b_gtiff(GDALJP2Box::CreateUUIDBox(
      kUuidGeoTiff.data(), kPayload.size(), kPayload.data()));
  std::unique_ptr<GDALJP2Box> b_msi(GDALJP2Box::CreateUUIDBox(
      kUuidMsi.data(), kPayload.size(), kPayload.data()));
  std::unique_ptr<GDALJP2Box> b_xmp(GDALJP2Box::CreateUUIDBox(
      kUuidXmp.data(), kPayload.size(), kPayload.data()));

  std::unique_ptr<GDALJP2Box> b_lbl(GDALJP2Box::CreateLblBox("my_label"));
  std::unique_ptr<GDALJP2Box> b_xml_assoc(
      GDALJP2Box::CreateLabelledXMLAssoc("gml.root", "<root/>"));

  std::unique_ptr<GDALJP2Box> b_jumd(
      GDALJP2Box::CreateJUMBFDescriptionBox(kUuidGeoTiff.data(), "jumbf_lbl"));
  const GDALJP2Box* sub_boxes[] = {b_lbl.get()};
  std::unique_ptr<GDALJP2Box> b_jumb(
      GDALJP2Box::CreateJUMBFBox(b_jumd.get(), 1, sub_boxes));

  // Write all created boxes into a /vsimem/ file and read them back.
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kSuiteDir); };
  constexpr char kMemPath[] = "/vsimem/gdaljp2box_test/jp2box_test.jp2";
  VSILFILE* fp = VSIFOpenL(kMemPath, "wb+");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [&fp] {
    if (fp != nullptr) {
      EXPECT_EQ(0, VSIFCloseL(fp));
    }
  };

  auto write_box = [fp](const GDALJP2Box* b) {
    ASSERT_NE(nullptr, b);
    std::unique_ptr<GByte, CplFreeDeleter> data(b->GetWritableBoxData());
    ASSERT_NE(nullptr, data);
    const size_t box_len = static_cast<size_t>(b->GetBoxLength());
    EXPECT_EQ(box_len, VSIFWriteL(data.get(), 1, box_len, fp));
  };
  write_box(&box1);
  write_box(b_gtiff.get());
  write_box(b_msi.get());
  write_box(b_xmp.get());
  write_box(b_xml_assoc.get());
  write_box(b_jumb.get());

  // Append a 64-bit length box (nLBox == 1, XLBox == 20: 16 header + 4 data).
  GUInt32 lbox1 = CPL_MSBWORD32(1);
  EXPECT_EQ(1U, VSIFWriteL(&lbox1, 4, 1, fp));
  EXPECT_EQ(1U, VSIFWriteL("xlbx", 4, 1, fp));
  GUInt64 xlbox = 20;
  CPL_MSBPTR64(&xlbox);
  EXPECT_EQ(1U, VSIFWriteL(&xlbox, 8, 1, fp));
  EXPECT_EQ(1U, VSIFWriteL("ABCD", 4, 1, fp));

  // Append a box with length == 0 (extends to end of file).
  GUInt32 lbox0 = 0;
  EXPECT_EQ(1U, VSIFWriteL(&lbox0, 4, 1, fp));
  EXPECT_EQ(1U, VSIFWriteL("endb", 4, 1, fp));
  EXPECT_EQ(1U, VSIFWriteL("XYZ", 3, 1, fp));

  // Now read and dump all boxes.
  char* dump_buf_raw = nullptr;
  size_t dump_size = 0;
  FILE* dump_fp = open_memstream(&dump_buf_raw, &dump_size);
  ASSERT_NE(nullptr, dump_fp);
  absl::Cleanup dump_fp_closer = [&dump_fp, &dump_buf_raw] {
    if (dump_fp != nullptr) {
      EXPECT_EQ(0, fclose(dump_fp));
    }
    std::free(dump_buf_raw);
  };

  GDALJP2Box reader(fp);
  EXPECT_EQ(fp, reader.GetFILE());
  ASSERT_TRUE(reader.ReadFirst());
  EXPECT_STREQ("ftyp", reader.GetType());
  EXPECT_FALSE(reader.IsSuperBox());
  std::unique_ptr<GByte, CplFreeDeleter> read_data(reader.ReadBoxData());
  ASSERT_NE(nullptr, read_data);
  reader.DumpReadable(dump_fp);

  while (reader.ReadNext()) {
    reader.DumpReadable(dump_fp);
    if (reader.IsSuperBox()) {
      GDALJP2Box child(fp);
      ASSERT_TRUE(child.ReadFirstChild(&reader));
      do {
        std::unique_ptr<GByte, CplFreeDeleter> cdata(child.ReadBoxData());
        EXPECT_NE(nullptr, cdata);
      } while (child.ReadNextChild(&reader));
    }
  }
  EXPECT_EQ(0, fclose(dump_fp));
  dump_fp = nullptr;
  std::unique_ptr<char, FreeDeleter> dump_buf(
      std::exchange(dump_buf_raw, nullptr));
  const std::string_view dump_view(dump_buf.get(), dump_size);
  EXPECT_THAT(dump_view, HasSubstr("(GeoTIFF)"));
  EXPECT_THAT(dump_view, HasSubstr("(MSI Worldfile)"));
  EXPECT_THAT(dump_view, HasSubstr("(XMP)"));
  EXPECT_THAT(dump_view, HasSubstr("(super)"));

  // ReadFirstChild / ReadNextChild with nullptr superbox.
  GDALJP2Box null_parent_reader(fp);
  EXPECT_TRUE(null_parent_reader.ReadFirstChild(nullptr));
  EXPECT_TRUE(null_parent_reader.ReadNextChild(nullptr));

  // ReadFirstChild on non-superbox returns FALSE.
  ASSERT_TRUE(reader.ReadFirst());
  GDALJP2Box bad_child(fp);
  EXPECT_FALSE(bad_child.ReadFirstChild(&reader));

  EXPECT_EQ(0, VSIFCloseL(fp));
  fp = nullptr;
}

TEST(GdalJp2BoxTest, EdgeCasesAndErrors) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kSuiteDir); };
  constexpr char kMemPath[] = "/vsimem/gdaljp2box_test/jp2box_err.jp2";

  // 1. Truncated 64-bit box header.
  {
    std::string bytes;
    const GUInt32 lbox1 = CPL_MSBWORD32(1);
    bytes.append(reinterpret_cast<const char*>(&lbox1), sizeof(lbox1));
    bytes.append("xlbx", 4);
    bytes.append("123", 3);  // less than 8 bytes for XLBox
    VsiMemTempWrapper temp_file(kMemPath, bytes);
    VSILFILE* fp = VSIFOpenL(kMemPath, "rb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    GDALJP2Box reader(fp);
    EXPECT_FALSE(reader.ReadFirst());
  }

  // 2. Negative 64-bit box length.
  {
    std::string bytes;
    const GUInt32 lbox1 = CPL_MSBWORD32(1);
    bytes.append(reinterpret_cast<const char*>(&lbox1), sizeof(lbox1));
    bytes.append("xlbx", 4);
    GIntBig neg_len = -10;
    CPL_MSBPTR64(&neg_len);
    bytes.append(reinterpret_cast<const char*>(&neg_len), sizeof(neg_len));
    VsiMemTempWrapper temp_file(kMemPath, bytes);
    VSILFILE* fp = VSIFOpenL(kMemPath, "rb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    GDALJP2Box reader(fp);
    EXPECT_FALSE(reader.ReadFirst());
  }

  // 3. Truncated uuid box.
  {
    std::string bytes;
    const GUInt32 lbox = CPL_MSBWORD32(24);
    bytes.append(reinterpret_cast<const char*>(&lbox), sizeof(lbox));
    bytes.append("uuid", 4);
    bytes.append("short", 5);  // less than 16 bytes for UUID
    VsiMemTempWrapper temp_file(kMemPath, bytes);
    VSILFILE* fp = VSIFOpenL(kMemPath, "rb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    GDALJP2Box reader(fp);
    EXPECT_FALSE(reader.ReadFirst());
  }

  // 4. Box length smaller than header (GetDataLength() < 0).
  {
    std::string bytes;
    const GUInt32 lbox = CPL_MSBWORD32(4);  // < 8
    bytes.append(reinterpret_cast<const char*>(&lbox), sizeof(lbox));
    bytes.append("badl", 4);
    VsiMemTempWrapper temp_file(kMemPath, bytes);
    VSILFILE* fp = VSIFOpenL(kMemPath, "rb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    GDALJP2Box reader(fp);
    EXPECT_FALSE(reader.ReadFirst());
  }

  // 5. Box > 100 MB in ReadBoxData().
  {
    std::string bytes;
    const GUInt32 lbox_big = CPL_MSBWORD32(101 * 1024 * 1024);
    bytes.append(reinterpret_cast<const char*>(&lbox_big), sizeof(lbox_big));
    bytes.append("bigb", 4);
    VsiMemTempWrapper temp_file(kMemPath, bytes);
    VSILFILE* fp = VSIFOpenL(kMemPath, "rb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    GDALJP2Box reader(fp);
    ASSERT_TRUE(reader.ReadFirst());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, reader.ReadBoxData());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 6. Truncated box content in ReadBoxData().
  {
    std::string bytes;
    const GUInt32 lbox_trunc = CPL_MSBWORD32(100);
    bytes.append(reinterpret_cast<const char*>(&lbox_trunc),
                 sizeof(lbox_trunc));
    bytes.append("trnc", 4);
    bytes.append("abc", 3);
    VsiMemTempWrapper temp_file(kMemPath, bytes);
    VSILFILE* fp = VSIFOpenL(kMemPath, "rb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    GDALJP2Box reader(fp);
    ASSERT_TRUE(reader.ReadFirst());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, reader.ReadBoxData());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
