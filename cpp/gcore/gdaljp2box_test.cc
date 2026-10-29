// Copyright 2026 Google Inc. All Rights Reserved.
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

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdaljp2metadata.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace gdal {
namespace {

TEST(GdalJp2BoxTest, WriteAndCreateBoxes) {
  GDALJP2Box box1;
  box1.SetType("ftyp");
  box1.AppendUInt32(0x01020304);
  box1.AppendUInt16(0x0506);
  box1.AppendUInt8(0x07);
  EXPECT_EQ(box1.GetDataLength(), 7);
  EXPECT_NE(box1.GetWritableData(), nullptr);

  GByte* raw = box1.GetWritableBoxData();
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(memcmp(raw + 4, "ftyp", 4), 0);
  CPLFree(raw);

  // SetWritableData replaces existing data
  const GByte payload[] = {10, 20, 30};
  box1.SetWritableData(sizeof(payload), payload);
  EXPECT_EQ(box1.GetDataLength(), 3);

  // UUID boxes (GeoTIFF, MSI, XMP)
  GByte uuid_geotiff[16] = {0xB1, 0x4B, 0xF8, 0xBD, 0x08, 0x3D, 0x4B, 0x43,
                            0xA5, 0xAE, 0x8C, 0xD7, 0xD5, 0xA6, 0xCE, 0x03};
  GByte uuid_msi[16] = {0x96, 0xA9, 0xF1, 0xF1, 0xDC, 0x98, 0x40, 0x2D,
                        0xA7, 0xAE, 0xD6, 0x8E, 0x34, 0x45, 0x18, 0x09};
  GByte uuid_xmp[16] = {0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8,
                        0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC};

  std::unique_ptr<GDALJP2Box> b_gtiff(
      GDALJP2Box::CreateUUIDBox(uuid_geotiff, sizeof(payload), payload));
  std::unique_ptr<GDALJP2Box> b_msi(
      GDALJP2Box::CreateUUIDBox(uuid_msi, sizeof(payload), payload));
  std::unique_ptr<GDALJP2Box> b_xmp(
      GDALJP2Box::CreateUUIDBox(uuid_xmp, sizeof(payload), payload));

  std::unique_ptr<GDALJP2Box> b_lbl(GDALJP2Box::CreateLblBox("my_label"));
  std::unique_ptr<GDALJP2Box> b_xml_assoc(
      GDALJP2Box::CreateLabelledXMLAssoc("gml.root", "<root/>"));

  std::unique_ptr<GDALJP2Box> b_jumd(
      GDALJP2Box::CreateJUMBFDescriptionBox(uuid_geotiff, "jumbf_lbl"));
  const GDALJP2Box* sub_boxes[] = {b_lbl.get()};
  std::unique_ptr<GDALJP2Box> b_jumb(
      GDALJP2Box::CreateJUMBFBox(b_jumd.get(), 1, sub_boxes));

  // Write all created boxes into a /vsimem/ file and read them back
  const char* mem_path = "/vsimem/jp2box_test.jp2";
  VSILFILE* fp = VSIFOpenL(mem_path, "wb+");
  ASSERT_NE(fp, nullptr);

  auto write_box = [fp](const GDALJP2Box* b) {
    GByte* data = b->GetWritableBoxData();
    VSIFWriteL(data, 1, static_cast<size_t>(b->GetBoxLength()), fp);
    CPLFree(data);
  };
  write_box(&box1);
  write_box(b_gtiff.get());
  write_box(b_msi.get());
  write_box(b_xmp.get());
  write_box(b_xml_assoc.get());
  write_box(b_jumb.get());

  // Append a 64-bit length box (nLBox == 1, XLBox == 20: 16 header + 4 data)
  GUInt32 lbox1 = CPL_MSBWORD32(1);
  VSIFWriteL(&lbox1, 4, 1, fp);
  VSIFWriteL("xlbx", 4, 1, fp);
  GUInt64 xlbox = 20;
  CPL_MSBPTR64(&xlbox);
  VSIFWriteL(&xlbox, 8, 1, fp);
  VSIFWriteL("ABCD", 4, 1, fp);

  // Append a box with length == 0 (extends to end of file)
  GUInt32 lbox0 = 0;
  VSIFWriteL(&lbox0, 4, 1, fp);
  VSIFWriteL("endb", 4, 1, fp);
  VSIFWriteL("XYZ", 3, 1, fp);

  // Now read and dump all boxes
  char* dump_buf = nullptr;
  size_t dump_size = 0;
  FILE* dump_fp = open_memstream(&dump_buf, &dump_size);
  ASSERT_NE(dump_fp, nullptr);

  GDALJP2Box reader(fp);
  EXPECT_EQ(reader.GetFILE(), fp);
  ASSERT_TRUE(reader.ReadFirst());
  EXPECT_STREQ(reader.GetType(), "ftyp");
  EXPECT_FALSE(reader.IsSuperBox());
  GByte* read_data = reader.ReadBoxData();
  ASSERT_NE(read_data, nullptr);
  CPLFree(read_data);
  reader.DumpReadable(dump_fp);

  while (reader.ReadNext()) {
    reader.DumpReadable(dump_fp);
    if (reader.IsSuperBox()) {
      GDALJP2Box child(fp);
      ASSERT_TRUE(child.ReadFirstChild(&reader));
      do {
        GByte* cdata = child.ReadBoxData();
        CPLFree(cdata);
      } while (child.ReadNextChild(&reader));
    }
  }
  fclose(dump_fp);
  EXPECT_THAT(std::string(dump_buf, dump_size),
              testing::HasSubstr("(GeoTIFF)"));
  EXPECT_THAT(std::string(dump_buf, dump_size),
              testing::HasSubstr("(MSI Worldfile)"));
  EXPECT_THAT(std::string(dump_buf, dump_size), testing::HasSubstr("(XMP)"));
  EXPECT_THAT(std::string(dump_buf, dump_size), testing::HasSubstr("(super)"));
  free(dump_buf);

  // ReadFirstChild / ReadNextChild with nullptr superbox
  GDALJP2Box null_parent_reader(fp);
  EXPECT_TRUE(null_parent_reader.ReadFirstChild(nullptr));
  EXPECT_TRUE(null_parent_reader.ReadNextChild(nullptr));

  // ReadFirstChild on non-superbox returns FALSE
  ASSERT_TRUE(reader.ReadFirst());
  GDALJP2Box bad_child(fp);
  EXPECT_FALSE(bad_child.ReadFirstChild(&reader));

  VSIFCloseL(fp);
  VSIUnlink(mem_path);
}

TEST(GdalJp2BoxTest, EdgeCasesAndErrors) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  const char* mem_path = "/vsimem/jp2box_err.jp2";

  // 1. Truncated 64-bit box header
  {
    VSILFILE* fp = VSIFOpenL(mem_path, "wb+");
    GUInt32 lbox1 = CPL_MSBWORD32(1);
    VSIFWriteL(&lbox1, 4, 1, fp);
    VSIFWriteL("xlbx", 4, 1, fp);
    VSIFWriteL("123", 3, 1, fp);  // less than 8 bytes for XLBox
    GDALJP2Box reader(fp);
    EXPECT_FALSE(reader.ReadFirst());
    VSIFCloseL(fp);
  }

  // 2. Negative 64-bit box length
  {
    VSILFILE* fp = VSIFOpenL(mem_path, "wb+");
    GUInt32 lbox1 = CPL_MSBWORD32(1);
    VSIFWriteL(&lbox1, 4, 1, fp);
    VSIFWriteL("xlbx", 4, 1, fp);
    GIntBig neg_len = -10;
    CPL_MSBPTR64(&neg_len);
    VSIFWriteL(&neg_len, 8, 1, fp);
    GDALJP2Box reader(fp);
    EXPECT_FALSE(reader.ReadFirst());
    VSIFCloseL(fp);
  }

  // 3. Truncated uuid box
  {
    VSILFILE* fp = VSIFOpenL(mem_path, "wb+");
    GUInt32 lbox = CPL_MSBWORD32(24);
    VSIFWriteL(&lbox, 4, 1, fp);
    VSIFWriteL("uuid", 4, 1, fp);
    VSIFWriteL("short", 5, 1, fp);  // less than 16 bytes for UUID
    GDALJP2Box reader(fp);
    EXPECT_FALSE(reader.ReadFirst());
    VSIFCloseL(fp);
  }

  // 4. Box length smaller than header (GetDataLength() < 0)
  {
    VSILFILE* fp = VSIFOpenL(mem_path, "wb+");
    GUInt32 lbox = CPL_MSBWORD32(4);  // < 8
    VSIFWriteL(&lbox, 4, 1, fp);
    VSIFWriteL("badl", 4, 1, fp);
    GDALJP2Box reader(fp);
    EXPECT_FALSE(reader.ReadFirst());
    VSIFCloseL(fp);
  }

  // 5. Box > 100 MB in ReadBoxData() and truncated box content in ReadBoxData()
  {
    VSILFILE* fp = VSIFOpenL(mem_path, "wb+");
    GUInt32 lbox_big = CPL_MSBWORD32(101 * 1024 * 1024);
    VSIFWriteL(&lbox_big, 4, 1, fp);
    VSIFWriteL("bigb", 4, 1, fp);
    GDALJP2Box reader(fp);
    ASSERT_TRUE(reader.ReadFirst());
    EXPECT_EQ(reader.ReadBoxData(), nullptr);
    VSIFCloseL(fp);
  }

  {
    VSILFILE* fp = VSIFOpenL(mem_path, "wb+");
    GUInt32 lbox_trunc = CPL_MSBWORD32(100);
    VSIFWriteL(&lbox_trunc, 4, 1, fp);
    VSIFWriteL("trnc", 4, 1, fp);
    VSIFWriteL("abc", 3, 1, fp);
    GDALJP2Box reader(fp);
    ASSERT_TRUE(reader.ReadFirst());
    EXPECT_EQ(reader.ReadBoxData(), nullptr);
    VSIFCloseL(fp);
  }

  VSIUnlink(mem_path);
}

}  // namespace
}  // namespace gdal
