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
// Tests EXIF metadata extraction and creation (gcore/gdalexif.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalexif.cpp

#include "third_party/gdal/gcore/gdalexif.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_cpp_functions.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

void AppendU16(std::vector<GByte>& buf, GUInt16 val, bool swap) {
  if (swap) {
    CPL_SWAP16PTR(&val);
  }
  const GByte* p = reinterpret_cast<const GByte*>(&val);
  buf.insert(buf.end(), p, p + 2);
}

void AppendU32(std::vector<GByte>& buf, GUInt32 val, bool swap) {
  if (swap) {
    CPL_SWAP32PTR(&val);
  }
  const GByte* p = reinterpret_cast<const GByte*>(&val);
  buf.insert(buf.end(), p, p + 4);
}

void AppendEntry(std::vector<GByte>& buf, GUInt16 tag, GUInt16 type,
                 GUInt32 count, GUInt32 offset_or_inline, bool swap) {
  AppendU16(buf, tag, swap);
  AppendU16(buf, type, swap);
  AppendU32(buf, count, swap);
  AppendU32(buf, offset_or_inline, swap);
}

void AppendEntryInlineBytes(std::vector<GByte>& buf, GUInt16 tag, GUInt16 type,
                            GUInt32 count, std::array<GByte, 4> inline_bytes,
                            bool swap) {
  AppendU16(buf, tag, swap);
  AppendU16(buf, type, swap);
  AppendU32(buf, count, swap);
  buf.insert(buf.end(), inline_bytes.begin(), inline_bytes.end());
}

TEST(GdalExifTest, ExtractMetadataLittleAndBigEndianAllTypesAndErrors) {
  absl::Cleanup cleanup_dir = [] {
    VSIRmdirRecursive("/vsimem/gdalexif_test");
  };

  // 1. Error reading directory count (EOF and overflow offset):
  {
    constexpr char kPath[] = "/vsimem/gdalexif_test/exif_empty.bin";
    VSILFILE* fp = VSIFOpenL(kPath, "wb+");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };

    char** md = nullptr;
    absl::Cleanup free_md = [&md] { CSLDestroy(md); };
    int exif_off = 0;
    int inter_off = 0;
    int gps_off = 0;
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, EXIFExtractMetadata(md, fp, 0, FALSE, 0, exif_off,
                                                inter_off, gps_off));
      CPLErrorReset();
      EXPECT_EQ(CE_Failure,
                EXIFExtractMetadata(md, fp, std::numeric_limits<int>::max(),
                                    FALSE, 10, exif_off, inter_off, gps_off));
      CPLErrorReset();
    }

    // nEntryCount == 0 -> CE_None:
    constexpr GUInt16 kZero = 0;
    EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_SET));
    EXPECT_EQ(2u, VSIFWriteL(&kZero, 1, 2, fp));
    EXPECT_EQ(CE_None, EXIFExtractMetadata(md, fp, 0, FALSE, 0, exif_off,
                                           inter_off, gps_off));

    // nEntryCount > 125 -> CE_Warning:
    constexpr GUInt16 kHuge = 200;
    EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_SET));
    EXPECT_EQ(2u, VSIFWriteL(&kHuge, 1, 2, fp));
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Warning, EXIFExtractMetadata(md, fp, 0, FALSE, 0, exif_off,
                                                inter_off, gps_off));
      CPLErrorReset();
    }

    // nEntryCount == 2 but truncated entries -> CE_Failure:
    constexpr GUInt16 kTwo = 2;
    EXPECT_EQ(0, VSIFSeekL(fp, 0, SEEK_SET));
    EXPECT_EQ(2u, VSIFWriteL(&kTwo, 1, 2, fp));
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, EXIFExtractMetadata(md, fp, 0, FALSE, 0, exif_off,
                                                inter_off, gps_off));
      CPLErrorReset();
    }
  }

  // 2. Test both swap=false and swap=true across all TIFF data types, inline
  //    (<=4 bytes) and offline (>4 bytes), GPS IFD, Interoperability IFD,
  //    IFD0Tags, UserComment, ExifVersion, unknown tags, and error tags:
  for (bool swap : {false, true}) {
    std::vector<GByte> data(2048, 0);
    // Offline data region starts at offset 512:
    // At 512: 2 x RATIONAL (16 bytes): (72/1, 0/0)
    {
      std::array<GUInt32, 4> rats = {72, 1, 0, 0};
      if (swap) {
        for (auto& v : rats) CPL_SWAP32PTR(&v);
      }
      std::memcpy(data.data() + 512, rats.data(),
                  rats.size() * sizeof(GUInt32));
    }
    // At 528: 2 x SRATIONAL (16 bytes): (-3/2, 0/1)
    {
      std::array<GInt32, 4> srats = {-3, 2, 0, 1};
      if (swap) {
        for (auto& v : srats) CPL_SWAP32PTR(&v);
      }
      std::memcpy(data.data() + 528, srats.data(),
                  srats.size() * sizeof(GInt32));
    }
    // At 544: 1 x DOUBLE (8 bytes): 3.14159
    {
      double dval = 3.14159;
      if (swap) {
        CPL_SWAPDOUBLE(&dval);
      }
      std::memcpy(data.data() + 544, &dval, sizeof(dval));
    }
    // At 560: 2 x FLOAT (8 bytes): 1.5f, -2.5f
    {
      std::array<float, 2> fvals = {1.5f, -2.5f};
      if (swap) {
        CPL_SWAP32PTR(&fvals[0]);
        CPL_SWAP32PTR(&fvals[1]);
      }
      std::memcpy(data.data() + 560, fvals.data(),
                  fvals.size() * sizeof(float));
    }
    // At 576: 2 x LONG (8 bytes): 100000, 200000
    {
      std::array<GUInt32, 2> lvals = {100000, 200000};
      if (swap) {
        CPL_SWAP32PTR(&lvals[0]);
        CPL_SWAP32PTR(&lvals[1]);
      }
      std::memcpy(data.data() + 576, lvals.data(),
                  lvals.size() * sizeof(GUInt32));
    }
    // At 592: 2 x SLONG (8 bytes): -100, 200
    {
      std::array<GInt32, 2> slvals = {-100, 200};
      if (swap) {
        CPL_SWAP32PTR(&slvals[0]);
        CPL_SWAP32PTR(&slvals[1]);
      }
      std::memcpy(data.data() + 592, slvals.data(),
                  slvals.size() * sizeof(GInt32));
    }
    // At 608: 3 x SHORT (6 bytes): 10, 20, 30
    {
      std::array<GUInt16, 3> svals = {10, 20, 30};
      if (swap) {
        for (auto& v : svals) CPL_SWAP16PTR(&v);
      }
      std::memcpy(data.data() + 608, svals.data(),
                  svals.size() * sizeof(GUInt16));
    }
    // At 624: 3 x SSHORT (6 bytes): -10, 20, -30
    {
      std::array<GInt16, 3> ssvals = {-10, 20, -30};
      if (swap) {
        for (auto& v : ssvals) CPL_SWAP16PTR(&v);
      }
      std::memcpy(data.data() + 624, ssvals.data(),
                  ssvals.size() * sizeof(GInt16));
    }
    // At 640: 6 x SBYTE: -1, 2, -3, 4, -5, 6
    {
      constexpr std::array<int8_t, 6> kSbvals = {-1, 2, -3, 4, -5, 6};
      std::memcpy(data.data() + 640, kSbvals.data(), kSbvals.size());
    }
    // At 656: UserComment (8-byte header + "MyComment  \0")
    {
      constexpr char kUc[] = "ASCII\0\0\0MyComment  ";
      std::memcpy(data.data() + 656, kUc, sizeof(kUc));
    }

    // Build directory at offset 0:
    std::vector<GByte> dir;
    constexpr GUInt16 kNumEntries = 22;
    AppendU16(dir, kNumEntries, swap);

    // 1-3: Offset tags (EXIFOFFSETTAG=0x8769, INTEROPERABILITYOFFSET=0xA005,
    //      GPSOFFSETTAG=0x8825)
    AppendEntry(dir, 0x8769, TIFF_LONG, 1, 100, swap);
    AppendEntry(dir, 0xA005, TIFF_LONG, 1, 200, swap);
    AppendEntry(dir, 0x8825, TIFF_LONG, 1, 300, swap);

    // 4: IFD0Tag DNG_UniqueCameraModel (0xC614), inline ASCII "DNG\0"
    AppendEntryInlineBytes(dir, 0xC614, TIFF_ASCII, 4, {'D', 'N', 'G', 0},
                           swap);
    // 5: Inline SHORT (count=2, 4 bytes): EXIF_SubjectLocation (0xA214)
    AppendEntry(dir, 0xA214, TIFF_SHORT, 2, 0x00020001, swap);
    // 6: Inline SSHORT (count=1, 2 bytes): unknown tag 0x9991
    AppendEntry(dir, 0x9991, TIFF_SSHORT, 1, 0x0000FFFE, swap);
    // 7: Inline LONG (count=1, 4 bytes): EXIF_PixelXDimension (0xA002)
    AppendEntry(dir, 0xA002, TIFF_LONG, 1, 1920, swap);
    // 8: Inline SLONG (count=1, 4 bytes): EXIF_PixelYDimension (0xA003)
    AppendEntry(dir, 0xA003, TIFF_SLONG, 1, 1080, swap);
    // 9: Inline FLOAT (count=1, 4 bytes): EXIF_FlashEnergy (0xA20B)
    AppendEntry(dir, 0xA20B, TIFF_FLOAT, 1, std::bit_cast<GUInt32>(2.5f), swap);
    // 10: Inline BYTE (count=4): EXIF_GPSVersionID (0x0001)
    AppendEntryInlineBytes(dir, 0x0001, TIFF_BYTE, 4, {1, 2, 3, 4}, swap);
    // 11: Inline ExifVersion (0x9000, UNDEFINED -> ASCII "0231"):
    AppendEntryInlineBytes(dir, 0x9000, TIFF_UNDEFINED, 4, {'0', '2', '3', '1'},
                           swap);

    // 12-19: Offline entries pointing into 512..656:
    AppendEntry(dir, 0x013E, TIFF_RATIONAL, 2, 512, swap);    // WhitePoint
    AppendEntry(dir, 0x9201, TIFF_SRATIONAL, 2, 528, swap);   // ShutterSpeed
    AppendEntry(dir, 0x011A, TIFF_DOUBLE, 1, 544, swap);      // XResolution
    AppendEntry(dir, 0x011B, TIFF_FLOAT, 2, 560, swap);       // YResolution
    AppendEntry(dir, 0x8831, TIFF_LONG, 2, 576, swap);        // SOS
    AppendEntry(dir, 0x8832, TIFF_SLONG, 2, 592, swap);       // REI
    AppendEntry(dir, 0x8827, TIFF_SHORT, 3, 608, swap);       // ISOSpeedRatings
    AppendEntry(dir, 0x9208, TIFF_SSHORT, 3, 624, swap);      // LightSource
    AppendEntry(dir, 0x9209, TIFF_SBYTE, 6, 640, swap);       // Flash
    AppendEntry(dir, 0x9286, TIFF_UNDEFINED, 20, 656, swap);  // UserComment

    // 21: Too many bytes in tag (> MAXSTRINGLENGTH):
    AppendEntry(dir, 0x010E, TIFF_ASCII, 70000, 512, swap);
    // 22: Invalid EXIF data type (TIFF_IFD = 13):
    AppendEntry(dir, 0x010F, TIFF_IFD, 1, 512, swap);

    std::memcpy(data.data(), dir.data(), dir.size());

    constexpr char kPath[] = "/vsimem/gdalexif_test/exif_test.bin";
    VSILFILE* fp = VSIFOpenL(kPath, "wb+");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp] {
      EXPECT_EQ(0, VSIFCloseL(fp));
      VSIUnlink("/vsimem/gdalexif_test/exif_test.bin");
    };
    EXPECT_EQ(data.size(), VSIFWriteL(data.data(), 1, data.size(), fp));

    char** md = nullptr;
    absl::Cleanup free_md = [&md] { CSLDestroy(md); };
    int exif_off = 0;
    int inter_off = 0;
    int gps_off = 0;
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_None, EXIFExtractMetadata(md, fp, 0, swap ? TRUE : FALSE, 0,
                                             exif_off, inter_off, gps_off));
      CPLErrorReset();
    }
    EXPECT_EQ(100, exif_off);
    EXPECT_EQ(200, inter_off);
    EXPECT_EQ(300, gps_off);
    ASSERT_NE(nullptr, md);
    EXPECT_STREQ("MyComment", CSLFetchNameValue(md, "EXIF_UserComment"));
    CSLDestroy(md);
    md = nullptr;

    // Also call with nOffset == gps_off and nOffset == inter_off to cover
    // gpstags and intr_tags lookup loops:
    gps_off = 0;
    inter_off = 0;
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_None, EXIFExtractMetadata(md, fp, 0, swap ? TRUE : FALSE, 0,
                                             exif_off, inter_off, gps_off));
      CPLErrorReset();
    }
  }

  // 3. Test truncation warning in EXIFPrintData when formatting 16000 BYTEs
  //    (each byte takes ~5 chars " 0x00", exceeding MAXSTRINGLENGTH=65535):
  {
    std::vector<GByte> data(20000, 0xAB);
    std::vector<GByte> dir;
    AppendU16(dir, 1, false);
    AppendEntry(dir, 0x8828, TIFF_UNDEFINED, 16000, 64, false);  // EXIF_OECF
    std::memcpy(data.data(), dir.data(), dir.size());

    constexpr char kPath[] = "/vsimem/gdalexif_test/exif_trunc.bin";
    VSILFILE* fp = VSIFOpenL(kPath, "wb+");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    EXPECT_EQ(data.size(), VSIFWriteL(data.data(), 1, data.size(), fp));

    char** md = nullptr;
    absl::Cleanup free_md = [&md] { CSLDestroy(md); };
    int exif_off = 0;
    int inter_off = 0;
    int gps_off = 0;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, EXIFExtractMetadata(md, fp, 0, FALSE, 0, exif_off,
                                           inter_off, gps_off));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST(GdalExifTest, EXIFCreateAllTagTypesAndThumbnail) {
  absl::Cleanup cleanup_dir = [] {
    VSIRmdirRecursive("/vsimem/gdalexif_test");
  };

  // 1. Empty metadata & no thumbnail -> nullptr:
  GUInt32 out_size = 0;
  EXPECT_EQ(nullptr, EXIFCreate(nullptr, nullptr, 0, 0, 0, &out_size));
  CPLStringList non_exif_md;
  non_exif_md.SetNameValue("OTHER_KEY", "VAL");
  EXPECT_EQ(nullptr,
            EXIFCreate(non_exif_md.List(), nullptr, 0, 0, 0, &out_size));

  // 2. Comprehensive metadata covering MAIN_IFD, EXIF_IFD, GPS_IFD, all types,
  //    truncation/padding warnings, hex/non-hex UNDEFINED, RATIONAL/SRATIONAL:
  CPLStringList md;
  md.SetNameValue("NON_EXIF_TAG", "ignored");
  md.SetNameValue("EXIF_UnknownTag", "warn");
  md.SetNameValue("EXIF_Compression", "1");  // TIFF_NOTYPE -> ignored

  // TIFF_ASCII:
  md.SetNameValue("EXIF_Make", "Google");                   // length == 0
  md.SetNameValue("EXIF_DateTime", "2026:10:29 12:34:56");  // exact length 20
  md.SetNameValue("EXIF_DateTimeOriginal",
                  "2026:10:29 12:34:56_TOO_LONG_STRING");  // > 20 truncated
  md.SetNameValue("EXIF_DateTimeDigitized", "2026");       // < 20 padded

  // TIFF_UNDEFINED / TIFF_BYTE:
  md.SetNameValue("EXIF_ExifVersion", "0231");        // exact length 4
  md.SetNameValue("EXIF_FlashpixVersion", "010099");  // > 4 truncated
  md.SetNameValue("EXIF_ComponentsConfiguration", "0x1 0x02 00");  // < 4 padded
  md.SetNameValue("EXIF_OECF", "0xA 0x1f 0x2B 00");     // hex escapes
  md.SetNameValue("EXIF_UserComment", "PlainComment");  // non-hex 0x9286
  md.SetNameValue("EXIF_MakerNote", "RawNote");         // non-hex other

  // TIFF_SHORT / TIFF_LONG:
  md.SetNameValue("EXIF_ISOSpeedRatings", "100 200 400");    // length == 0
  md.SetNameValue("EXIF_Orientation", "1");                  // exact length 1
  md.SetNameValue("EXIF_ResolutionUnit", "2 3");             // > 1 truncated
  md.SetNameValue("EXIF_SubjectLocation", "50");             // < 2 padded
  md.SetNameValue("EXIF_StandardOutputSensitivity", "200");  // TIFF_LONG

  // TIFF_RATIONAL:
  md.SetNameValue("EXIF_XResolution", "(72)");      // integer
  md.SetNameValue("EXIF_YResolution", "0.25 1.5");  // > 1 truncated
  md.SetNameValue("EXIF_WhitePoint", "0.3127");     // < 2 padded
  md.SetNameValue("EXIF_PrimaryChromaticities",
                  "-1.0 nan 0.5 1.5 2 0");  // negative & nan

  // TIFF_SRATIONAL:
  md.SetNameValue("EXIF_ShutterSpeedValue", "-2");       // negative int
  md.SetNameValue("EXIF_BrightnessValue", "-0.5 -2.5");  // neg frac & >1
  md.SetNameValue("EXIF_ExposureBiasValue", "0.5 2.5");  // pos frac & >1
  md.SetNameValue("EXIF_ApertureValue", "3");            // pos int

  // GPS_IFD:
  md.SetNameValue("EXIF_GPSVersionID", "0x2 0x2 00 00");
  md.SetNameValue("EXIF_GPSLatitudeRef", "N");
  md.SetNameValue("EXIF_GPSLatitude", "37 25.5 0.125");
  md.SetNameValue("EXIF_GPSUnknown", "123");

  std::vector<GByte> thumb(64, 0xFF);
  std::unique_ptr<GByte, CplFreeDeleter> exif_buf;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    exif_buf.reset(EXIFCreate(md.List(), thumb.data(),
                              static_cast<GUInt32>(thumb.size()), 16, 16,
                              &out_size));
    CPLErrorReset();
  }
  ASSERT_NE(nullptr, exif_buf);
  EXPECT_GT(out_size, 64u);

  // Round-trip the generated EXIF buffer through EXIFExtractMetadata:
  {
    constexpr char kPath[] = "/vsimem/gdalexif_test/exif_roundtrip.bin";
    VSILFILE* fp = VSIFOpenL(kPath, "wb+");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
    EXPECT_EQ(out_size, VSIFWriteL(exif_buf.get(), 1, out_size, fp));
    exif_buf.reset();

    char** parsed_md = nullptr;
    absl::Cleanup free_parsed_md = [&parsed_md] { CSLDestroy(parsed_md); };
    int exif_off = 0;
    int inter_off = 0;
    int gps_off = 0;
    // EXIF header is 6 bytes ("Exif\0\0"), then 8-byte TIFF header (IFD0 at 8):
    EXPECT_EQ(CE_None, EXIFExtractMetadata(parsed_md, fp, 8, FALSE, 6, exif_off,
                                           inter_off, gps_off));
    EXPECT_GT(exif_off, 0);
    EXPECT_GT(gps_off, 0);
    EXPECT_EQ(CE_None, EXIFExtractMetadata(parsed_md, fp, exif_off, FALSE, 6,
                                           exif_off, inter_off, gps_off));
    EXPECT_EQ(CE_None, EXIFExtractMetadata(parsed_md, fp, gps_off, FALSE, 6,
                                           exif_off, inter_off, gps_off));
    EXPECT_STREQ("Google", CSLFetchNameValue(parsed_md, "EXIF_Make"));
    EXPECT_STREQ("PlainComment",
                 CSLFetchNameValue(parsed_md, "EXIF_UserComment"));
    EXPECT_STREQ("N", CSLFetchNameValue(parsed_md, "EXIF_GPSLatitudeRef"));
  }

  // 3. Exceeding 65536 bytes returns nullptr:
  {
    std::vector<GByte> huge_thumb(66000, 0);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, EXIFCreate(md.List(), huge_thumb.data(),
                                  static_cast<GUInt32>(huge_thumb.size()), 100,
                                  100, &out_size));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
