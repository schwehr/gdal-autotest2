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
// Tests CODA product opening, closing, format recognition, metadata accessors,
// definition binding, and product variable queries (libcoda/coda-product.c).
//
// google3 build caveats:
//   - HAVE_HDF4 is undefined in config.h (coda-hdf4* excluded); HDF4 files
//     are recognized as coda_format_hdf4 in get_format() and then rejected in
//     reopen_with_backend() with CODA_ERROR_NO_HDF4_SUPPORT.
//   - HAVE_HDF5 is enabled (1); HDF5 files are created via versioned HDF5 APIs
//     because config.h defines H5_USE_16_API.
//   - HAVE_NETCDF_H is undefined; CODA uses its built-in netCDF classic parser.
//   - Unreachable lines in libcoda/coda-product.c (~4-6% of compiled-in lines)
//     consist of malloc() OOM checks, dead error branches on
//     coda_data_dictionary_find_definition_for_product(), mid-read I/O failures
//     on regular files, coda_format_hdf4 inside coda_close(), and assert(0).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-product.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/hdf5_test_util.h"
#include "third_party/stcorp_coda/google/util/netcdf_builder.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

void AppendBe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendBe24(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendBe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
  }
}

std::vector<uint8_t> ToBytes(std::string_view sv) {
  return std::vector<uint8_t>(sv.begin(), sv.end());
}

// Builds a minimal valid NetCDF classic file (version 1 or 2) using
// NetcdfBuilder so coda_netcdf_reopen() succeeds.
std::vector<uint8_t> MakeMinimalNetcdfBytes(int version) {
  NetcdfBuilder builder(version);
  builder.AddDim({.name = "n", .size = 1});
  builder.AddGlobalAttr({
      .name = "title",
      .type = NetcdfType::kChar,
      .count = 4,
      .raw_values = {'t', 'e', 's', 't'},
  });
  builder.AddVar({
      .name = "x",
      .dim_ids = {0},
      .attrs = {},
      .type = NetcdfType::kInt,
      .vsize = 4,
      .data = {0x00, 0x00, 0x00, 0x2A},
  });
  return builder.Build();
}

// Builds a minimal valid 128-byte single-file CDF v3.0 uncompressed file
// (magic 0xCDF30001 0x0000FFFF, CDR at offset 8 -> GDR at offset 64, eof=128)
// matching coda-cdf.c read_file() and read_GDR().
std::vector<uint8_t> MakeMinimalCdf3Bytes() {
  std::vector<uint8_t> out;
  out.reserve(128);
  // Magic bytes (8 bytes).
  AppendBe32(out, 0xCDF30001U);
  AppendBe32(out, 0x0000FFFFU);
  // CDR at offset 8 (56 bytes -> ends at offset 64).
  AppendBe64(out, 56);  // RecordSize (offset 8)
  AppendBe32(out, 1);   // RecordType = CDR (offset 16)
  AppendBe64(out, 64);  // GDRoffset (offset 20)
  AppendBe32(out, 3);   // Version (offset 28)
  AppendBe32(out, 0);   // Release (offset 32)
  AppendBe32(out, 6);   // Encoding = IBMPC little-endian (offset 36)
  AppendBe32(out, 3);   // Flags = row-major | single-file (offset 40)
  AppendBe32(out, 0);   // rfuA (offset 44)
  AppendBe32(out, 0);   // rfuB (offset 48)
  AppendBe32(out, 0);   // Increment (offset 52)
  AppendBe32(out, 0);   // rfuD (offset 56)
  AppendBe32(out, 0);   // rfuE (offset 60)
  // GDR at offset 64 (64 bytes -> ends at offset 128).
  AppendBe64(out, 64);   // RecordSize (offset 64)
  AppendBe32(out, 2);    // RecordType = GDR (offset 72)
  AppendBe64(out, 0);    // rVDRhead (offset 76)
  AppendBe64(out, 0);    // zVDRhead (offset 84)
  AppendBe64(out, 0);    // ADRhead (offset 92)
  AppendBe64(out, 128);  // eof == file_size (offset 100)
  AppendBe32(out, 0);    // NrVars (offset 108)
  AppendBe32(out, 0);    // NumAttr (offset 112)
  AppendBe32(out, 0);    // rMaxRec (offset 116)
  AppendBe32(out, 0);    // rNumDims (offset 120)
  AppendBe32(out, 0);    // NzVars (offset 124)
  return out;
}

// Builds a minimal valid 84-byte GRIB Edition 1 message with a 1x1 lat/lon GDS
// and a single 8-bit data value, matching coda-grib.c read_grib1_message().
std::vector<uint8_t> MakeMinimalGrib1Bytes() {
  std::vector<uint8_t> out;
  out.reserve(84);
  // Section 0: Indicator Section (8 bytes).
  out.insert(out.end(), {'G', 'R', 'I', 'B'});
  AppendBe24(out, 84);  // total message length
  out.push_back(1);     // GRIB edition 1

  // Section 1: Product Definition Section (28 bytes).
  AppendBe24(out, 28);  // section_size
  out.push_back(2);     // table2Version
  out.push_back(98);    // centre
  out.push_back(1);     // generatingProcessIdentifier
  out.push_back(255);   // gridDefinition
  out.push_back(0x80);  // has_gds=1, has_bms=0
  out.insert(out.end(), 20, 0);

  // Section 2: Grid Description Section (32 bytes).
  AppendBe24(out, 32);  // section_size
  out.push_back(0);     // NV = 0
  out.push_back(255);   // PVL = 255 (no PV/PL)
  out.push_back(0);     // dataRepresentationType = 0 (lat/lon)
  AppendBe16(out, 1);   // Ni = 1
  AppendBe16(out, 1);   // Nj = 1
  out.insert(out.end(), 22, 0);

  // Section 4: Binary Data Section (12 bytes: 11-byte header + 1 data byte).
  AppendBe24(out, 12);  // section_size
  out.push_back(0);     // flags (simple packing, grid-point)
  AppendBe16(out, 0);   // binaryScaleFactor
  AppendBe32(out, 0);   // referenceValue (IBM float 0.0)
  out.push_back(8);     // bitsPerValue = 8 (1 element * 8 bits = 1 byte)
  out.push_back(0x2A);  // packed sample value

  // Section 5: End Section (4 bytes).
  out.insert(out.end(), {'7', '7', '7', '7'});
  return out;
}

// Builds a minimal valid 179-byte GRIB Edition 2 message (Sections 0, 1, 3, 4,
// 5, 6, 7, 8) matching coda-grib.c read_grib2_message().
std::vector<uint8_t> MakeMinimalGrib2Bytes() {
  std::vector<uint8_t> out;
  out.reserve(179);
  // Section 0: Indicator Section (16 bytes).
  out.insert(out.end(), {'G', 'R', 'I', 'B'});
  AppendBe16(out, 0);    // reserved
  out.push_back(0);      // discipline
  out.push_back(2);      // GRIB edition 2
  AppendBe64(out, 179);  // total message length

  // Section 1: Identification Section (21 bytes).
  AppendBe32(out, 21);
  out.push_back(1);
  out.insert(out.end(), 16, 0);

  // Section 3: Grid Definition Section (72 bytes: 14 fixed + 58 template 0).
  AppendBe32(out, 72);
  out.push_back(3);    // section number
  out.push_back(0);    // sourceOfGridDefinition
  AppendBe32(out, 1);  // numberOfDataPoints = 1
  out.push_back(0);    // number_size
  out.push_back(0);    // number_interpretation
  AppendBe16(out, 0);  // gridDefinitionTemplateNumber = 0 (lat/lon)
  out.insert(out.end(), 16, 0);
  AppendBe32(out, 1);  // Ni = 1
  AppendBe32(out, 1);  // Nj = 1
  out.insert(out.end(), 34, 0);

  // Section 4: Product Definition Section (34 bytes).
  AppendBe32(out, 34);
  out.push_back(4);    // section number
  AppendBe16(out, 0);  // num_coordinate_values
  AppendBe16(out, 0);  // productDefinitionTemplate = 0
  std::vector<uint8_t> sec4_body(25, 0);
  sec4_body[13] = 255;  // typeOfFirstFixedSurface missing
  sec4_body[19] = 255;  // typeOfSecondFixedSurface missing
  out.insert(out.end(), sec4_body.begin(), sec4_body.end());

  // Section 5: Data Representation Section (20 bytes).
  AppendBe32(out, 20);
  out.push_back(5);    // section number
  AppendBe32(out, 1);  // num_elements = 1
  AppendBe16(out, 0);  // dataRepresentationTemplate = 0 (simple packing)
  AppendBe32(out, 0);  // referenceValue
  AppendBe16(out, 0);  // binaryScaleFactor
  AppendBe16(out, 0);  // decimalScaleFactor
  out.push_back(8);    // bitsPerValue = 8

  // Section 6: Bit-Map Section (6 bytes).
  AppendBe32(out, 6);
  out.push_back(6);    // section number
  out.push_back(255);  // no bit-map

  // Section 7: Data Section (6 bytes).
  AppendBe32(out, 6);
  out.push_back(7);     // section number
  out.push_back(0x2A);  // 1 byte packed value

  // Section 8: End Section (4 bytes).
  out.insert(out.end(), {'7', '7', '7', '7'});
  return out;
}

// Builds a minimal valid RINEX v3.00 Clock ('C') file: Line 1 puts
// "RINEX VERSION / TYPE" at byte offset 60 (get_format requirement), followed
// by mandatory PGM / RUN BY / DATE and END OF HEADER lines.
std::string MakeMinimalRinexClockText() {
  std::string out;
  out +=
      "     3.00           C                   G                   "
      "RINEX VERSION / TYPE\n";
  out +=
      "TESTPROG            TESTAGENCY          20200101 000000 UTC "
      "PGM / RUN BY / DATE \n";
  out +=
      "                                                            "
      "END OF HEADER       \n";
  return out;
}

// Builds a minimal valid SP3-c ('#cP') file with all 22 fixed-width header
// lines (>= 60 chars for lines 1-15) and an EOF marker.
std::string MakeMinimalSp3Text() {
  auto pad60 = [](std::string line) {
    if (line.size() < 60) {
      line.append(60 - line.size(), ' ');
    }
    line.push_back('\n');
    return line;
  };

  std::string out;
  // Line 1: spaces required at indices 7, 10, 13, 16, 19, 31, 39 for
  // get_format.
  out += pad60("#cP2020  1  1  0  0  0.00000000       0 ORBIT IGS14 HLM  IGS ");
  // Line 2: ## week sec interval mjd frac
  out += pad60("## 2086 259200.00000000   900.00000000 58849 0.0000000000000");
  // Lines 3-7: satellite count and IDs (1 satellite G01).
  out += pad60("+    1   G01");
  for (int i = 0; i < 4; ++i) {
    out += pad60("+        ");
  }
  // Lines 8-12: satellite accuracies.
  out += pad60("++         0");
  for (int i = 0; i < 4; ++i) {
    out += pad60("++       ");
  }
  // Lines 13-15: file type/time system, reserved %c, floating base factors %f.
  out += pad60("%c G  cc GPS");
  out += pad60("%c cc cc ccc");
  out += pad60("%f  1.2500000  1.025000000");
  // Lines 16-22: 7 comment/reserved lines.
  for (int i = 0; i < 7; ++i) {
    out += "/* comment\n";
  }
  out += "EOF\n";
  return out;
}

void CreateHdf5File(const std::filesystem::path& path,
                    hsize_t userblock_size = 0) {
  ScopedH5Plist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  if (userblock_size > 0) {
    ASSERT_GE(H5Pset_userblock(fcpl.get(), userblock_size), 0);
  }
  ScopedH5File file(
      H5Fcreate(path.c_str(), H5F_ACC_TRUNC, fcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(file.valid());
}

constexpr std::string_view kIndexXml = R"(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="ProdClass">
  <cd:ProductType name="BinType">
    <cd:ProductDefinition id="BinProdV1" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">CODABIN1</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
    <cd:ProductDefinition id="BinProdV2" format="binary" version="2">
      <cd:DetectionRule>
        <cd:MatchData offset="0">CODABIN2</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="AsciiType">
    <cd:ProductDefinition id="AsciiProd" format="ascii" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">ASCIITST</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="XmlType">
    <cd:ProductDefinition id="XmlProd" format="xml" version="1">
      <cd:DetectionRule>
        <cd:MatchExpression>exists(/expected)</cd:MatchExpression>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="NcType">
    <cd:ProductDefinition id="NcProd" format="netcdf" version="1">
      <cd:DetectionRule>
        <cd:MatchExpression>exists(/x)</cd:MatchExpression>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="MissingDefType">
    <cd:ProductDefinition id="MissingDefProd" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">MISDEF01</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)";

// Binary v1 definition includes scalar, array, and lazy-unused product
// variables so both initialized and uninitialized variable cleanup in
// coda_close() are exercised.
constexpr std::string_view kBinProdV1Xml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinProdV1" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="val">
      <cd:Integer>
        <cd:BitSize>16</cd:BitSize>
        <cd:NativeType>uint16</cd:NativeType>
        <cd:LittleEndian/>
      </cd:Integer>
    </cd:Field>
  </cd:Record>
  <cd:ProductVariable name="scalar_var">
    <cd:Init>$scalar_var = int(/val) + 100</cd:Init>
  </cd:ProductVariable>
  <cd:ProductVariable name="array_var">
    <cd:Dimension>3</cd:Dimension>
    <cd:Init>$array_var[0] = 10; $array_var[1] = 20; $array_var[2] = int(/val)</cd:Init>
  </cd:ProductVariable>
  <cd:ProductVariable name="unused_var">
    <cd:Init>$unused_var = 999</cd:Init>
  </cd:ProductVariable>
</cd:ProductDefinition>
)";

constexpr std::string_view kBinProdV2Xml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinProdV2" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Text>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

constexpr std::string_view kAsciiProdXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="AsciiProd" format="ascii">
  <cd:Record>
    <cd:Field name="line">
      <cd:AsciiLine/>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

constexpr std::string_view kXmlProdXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="XmlProd" format="xml">
  <cd:Record>
    <cd:Field name="expected">
      <cd:Text/>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

// NetCDF self-describing definition intentionally has no <cd:Record> root type
// so coda_get_product_root_type() tests product_definition != NULL with
// product_definition->root_type == NULL.
constexpr std::string_view kNcProdXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="NcProd" format="netcdf"/>
)";

std::vector<uint8_t> MakeProductTestCodadef() {
  const std::array<std::pair<std::string_view, std::string_view>, 5> entries = {
      {{"products/BinProdV1.xml", kBinProdV1Xml},
       {"products/BinProdV2.xml", kBinProdV2Xml},
       {"products/AsciiProd.xml", kAsciiProdXml},
       {"products/XmlProd.xml", kXmlProdXml},
       {"products/NcProd.xml", kNcProdXml}},
  };
  return BuildCodadefBytes(kIndexXml, entries);
}

constexpr auto kBinV1Bytes = std::to_array<uint8_t>(
    {'C', 'O', 'D', 'A', 'B', 'I', 'N', '1', 0x2A, 0x00});
constexpr auto kBinV2Bytes = std::to_array<uint8_t>(
    {'C', 'O', 'D', 'A', 'B', 'I', 'N', '2', 0x01, 0x02});
constexpr std::string_view kAsciiProductText = "ASCIITST hello\n";
constexpr std::string_view kExpectedXmlText =
    "<?xml version=\"1.0\"?>\n<expected>hello</expected>\n";
constexpr std::string_view kUtf8BomXmlText =
    "\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<expected>bom</expected>\n";
constexpr std::string_view kUnmatchedBomXmlText =
    "\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<unmatched>bom</unmatched>\n";
constexpr std::string_view kUnexpectedXmlText =
    "<?xml version=\"1.0\"?>\n<unexpected>bad</unexpected>\n";

TEST(CodaProductTest, OpenAndRecognizeFileHandleFilesystemErrors) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Missing file triggers ENOENT -> CODA_ERROR_FILE_NOT_FOUND.
  const std::filesystem::path missing_path =
      std::filesystem::path(::testing::TempDir()) /
      "coda_product_missing_file.dat";
  std::filesystem::remove(missing_path);

  ScopedProduct raw_product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing_path.c_str(), raw_product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
  EXPECT_EQ(nullptr, raw_product.get());

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_recognize_file(missing_path.c_str(), nullptr, nullptr,
                                    nullptr, nullptr, nullptr));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);

  // 2. Directory path fails regular-file check -> CODA_ERROR_FILE_OPEN.
  TempDir dir("coda_product_dir_not_regular");
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(dir.c_str(), raw_product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_OPEN, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("not a regular file"));

  // 3. Path traversing through a regular file triggers ENOTDIR ->
  //    CODA_ERROR_FILE_OPEN (non-ENOENT stat failure).
  TempFile regular_file("coda_product_not_a_dir.dat");
  ASSERT_TRUE(regular_file.WriteBytes(kBinV1Bytes));
  const std::filesystem::path not_dir_child = regular_file.path() / "child";
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(not_dir_child.c_str(), raw_product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_OPEN, coda_errno);
}

TEST(CodaProductTest, RecognizeFileDetectsAllSupportedFormatsAndSizes) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Small/unrecognized files below each get_format() size threshold (<4, <8,
  //    <40, <80, and >512 bytes without HDF5 userblock) fall back to binary.
  struct BinaryFallbackCase {
    std::string_view name;
    std::vector<uint8_t> bytes;
  };
  const BinaryFallbackCase kBinaryCases[] = {
      {.name = "bin_lt4.dat", .bytes = {0x01, 0x02, 0x03}},
      {.name = "bin_lt8.dat", .bytes = {'C', 'D', 'F', 0x03, 0x00}},
      {.name = "bin_lt40.dat", .bytes = {'G', 'R', 'I', 'B', 0, 0, 0, 3, 9, 9}},
      {.name = "bin_lt80.dat", .bytes = std::vector<uint8_t>(45, 0x41)},
      {.name = "bin_gt1024.dat", .bytes = std::vector<uint8_t>(1030, 0x5A)},
  };
  for (const BinaryFallbackCase& tc : kBinaryCases) {
    SCOPED_TRACE(tc.name);
    TempFile file(tc.name);
    ASSERT_TRUE(file.WriteBytes(tc.bytes));
    int64_t size = -1;
    coda_format fmt = coda_format_sp3;
    const char* pclass = "sentinel";
    const char* ptype = "sentinel";
    int version = 99;
    ASSERT_EQ(CODA_SUCCESS, coda_recognize_file(file.c_str(), &size, &fmt,
                                                &pclass, &ptype, &version));
    EXPECT_EQ(static_cast<int64_t>(tc.bytes.size()), size);
    EXPECT_EQ(coda_format_binary, fmt);
    EXPECT_EQ(nullptr, pclass);
    EXPECT_EQ(nullptr, ptype);
    EXPECT_EQ(-1, version);
  }

  // 2. Self-describing formats with valid payloads succeed recognition.
  struct FormatCase {
    std::string_view name;
    coda_format expected_format;
    std::vector<uint8_t> bytes;
  };
  const FormatCase kFormatCases[] = {
      {.name = "rec_xml.xml",
       .expected_format = coda_format_xml,
       .bytes = ToBytes(kExpectedXmlText)},
      {.name = "rec_xml_bom.xml",
       .expected_format = coda_format_xml,
       .bytes = ToBytes(kUtf8BomXmlText)},
      {.name = "rec_nc1.nc",
       .expected_format = coda_format_netcdf,
       .bytes = MakeMinimalNetcdfBytes(1)},
      {.name = "rec_nc2.nc",
       .expected_format = coda_format_netcdf,
       .bytes = MakeMinimalNetcdfBytes(2)},
      {.name = "rec_cdf3.cdf",
       .expected_format = coda_format_cdf,
       .bytes = MakeMinimalCdf3Bytes()},
      {.name = "rec_grib1.grb",
       .expected_format = coda_format_grib,
       .bytes = MakeMinimalGrib1Bytes()},
      {.name = "rec_grib2.grb",
       .expected_format = coda_format_grib,
       .bytes = MakeMinimalGrib2Bytes()},
      {.name = "rec_rinex.rnx",
       .expected_format = coda_format_rinex,
       .bytes = ToBytes(MakeMinimalRinexClockText())},
      {.name = "rec_sp3.sp3",
       .expected_format = coda_format_sp3,
       .bytes = ToBytes(MakeMinimalSp3Text())},
  };
  for (const FormatCase& tc : kFormatCases) {
    SCOPED_TRACE(tc.name);
    TempFile file(tc.name);
    ASSERT_TRUE(file.WriteBytes(tc.bytes));
    int64_t size = -1;
    coda_format fmt = coda_format_binary;
    const char* pclass = "sentinel";
    const char* ptype = "sentinel";
    int version = 99;
    auto recognize_with_leak_guard = [&](auto&& fn) {
      if (tc.expected_format == coda_format_rinex) {
        // Upstream coda_rinex_close() never frees product->mem_ptr allocated
        // in read_main_header() via coda_mem_float_new().
        const absl::LeakCheckDisabler leak_check_disabler;
        return fn();
      }
      return fn();
    };
    ASSERT_EQ(CODA_SUCCESS, recognize_with_leak_guard([&] {
                return coda_recognize_file(file.c_str(), &size, &fmt, &pclass,
                                           &ptype, &version);
              }));
    EXPECT_EQ(static_cast<int64_t>(tc.bytes.size()), size);
    EXPECT_EQ(tc.expected_format, fmt);
    EXPECT_EQ(nullptr, pclass);
    EXPECT_EQ(nullptr, ptype);
    EXPECT_EQ(-1, version);

    // Also verify all optional out-parameters may be NULL.
    EXPECT_EQ(CODA_SUCCESS, recognize_with_leak_guard([&] {
                return coda_recognize_file(file.c_str(), nullptr, nullptr,
                                           nullptr, nullptr, nullptr);
              }));
  }

  // 3. HDF5 at offset 0 and with a 1024-byte userblock (exercises offset=512,
  //    offset*=2, and match at 1024 in get_format()).
  for (const hsize_t userblock : {hsize_t{0}, hsize_t{1024}}) {
    TempFile h5_file(userblock == 0 ? "rec_hdf5_0.h5" : "rec_hdf5_1024.h5");
    ASSERT_NO_FATAL_FAILURE(CreateHdf5File(h5_file.path(), userblock));
    coda_format fmt = coda_format_binary;
    int64_t size = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_recognize_file(h5_file.c_str(), &size, &fmt,
                                                nullptr, nullptr, nullptr));
    EXPECT_GT(size, static_cast<int64_t>(userblock));
    EXPECT_EQ(coda_format_hdf5, fmt);
  }
}

TEST(CodaProductTest, Hdf4MagicBytesFailWithNoHdf4Support) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  constexpr auto kHdf4Bytes =
      std::to_array<uint8_t>({0x0E, 0x03, 0x13, 0x01, 0x00, 0x00, 0x00, 0x00});
  TempFile hdf4_file("coda_product_hdf4.hdf");
  ASSERT_TRUE(hdf4_file.WriteBytes(kHdf4Bytes));

  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(hdf4_file.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_recognize_file(hdf4_file.c_str(), nullptr, nullptr,
                                    nullptr, nullptr, nullptr));
  EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);
}

TEST(CodaProductTest, OpenPropagatesBackendFailuresForMalformedFormats) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  struct CorruptCase {
    std::string_view name;
    std::vector<uint8_t> bytes;
    bool allow_upstream_leak = false;
  };
  const CorruptCase kCorruptCases[] = {
      {.name = "bad_xml.xml",
       .bytes = ToBytes("<?xml version=\"1.0\"?><a/><b>")},
      // coda-hdf5.c:146-148 leaks H5Eget_major/H5Eget_minor strings and leaves
      // HDF5 error stack allocations when H5Fopen() fails.
      {.name = "bad_hdf5.h5",
       .bytes = {0x89, 'H', 'D', 'F', '\r', '\n', 0x1A, '\n'},
       .allow_upstream_leak = true},
      {.name = "bad_cdf_v2.cdf",
       .bytes = {0xCD, 0xF2, 0x60, 0x02, 0x00, 0x00, 0xFF, 0xFF}},
      {.name = "bad_cdf_old.cdf",
       .bytes = {0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF}},
      {.name = "bad_cdf_v2_comp.cdf",
       .bytes = {0xCD, 0xF2, 0x60, 0x02, 0xCC, 0xCC, 0x00, 0x01}},
      {.name = "bad_cdf_v3_comp.cdf",
       .bytes = {0xCD, 0xF3, 0x00, 0x01, 0xCC, 0xCC, 0x00, 0x01}},
      {.name = "bad_cdf_trunc.cdf",
       .bytes = {0xCD, 0xF3, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xFF}},
      {.name = "bad_netcdf.nc", .bytes = {'C', 'D', 'F', 0x01, 0, 0, 0, 0}},
      {.name = "bad_netcdf5.nc", .bytes = {'C', 'D', 'F', 0x05, 0, 0, 0, 0}},
      {.name = "bad_grib1.grb", .bytes = {'G', 'R', 'I', 'B', 0, 0, 0, 1}},
      {.name = "bad_grib2.grb", .bytes = {'G', 'R', 'I', 'B', 0, 0, 0, 2}},
      // read_main_header() allocates product->mem_ptr via coda_mem_float_new()
      // before read_observation_header() fails, and upstream coda_rinex_close()
      // never frees product->mem_ptr.
      {.name = "bad_rinex.rnx",
       .bytes = ToBytes(
           "     3.00           O                   G                   "
           "RINEX VERSION / TYPE\n"),
       .allow_upstream_leak = true},
      {.name = "bad_sp3.sp3",
       .bytes = ToBytes(
           "#aP2020  1  1  0  0  0.00000000       0 ORBIT IGS14 HLM  IGS \n")},
  };

  for (const CorruptCase& tc : kCorruptCases) {
    SCOPED_TRACE(tc.name);
    TempFile file(tc.name);
    ASSERT_TRUE(file.WriteBytes(tc.bytes));
    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    int open_rc = 0;
    if (tc.allow_upstream_leak) {
      const absl::LeakCheckDisabler leak_check_disabler;
      open_rc = coda_open(file.c_str(), product.OutParam());
    } else {
      open_rc = coda_open(file.c_str(), product.OutParam());
    }
    EXPECT_EQ(-1, open_rc);
    EXPECT_NE(CODA_SUCCESS, coda_errno);
    EXPECT_EQ(nullptr, product.get());
  }
}

TEST(CodaProductTest, OpenAndCloseAllCompiledInBackendsAndAccessors) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();
  const ScopedCodaOptions restore_options;

  TempFile codadef_file("coda_product_backends.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(MakeProductTestCodadef()));
  const absl::Cleanup clear_def = [] { coda_set_definition_path(nullptr); };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Open and inspect every self-describing format without a codadef match.
  struct SelfDescribingCase {
    std::string_view name;
    coda_format expected_format;
    coda_type_class expected_root_class;
    std::vector<uint8_t> bytes;
  };
  const SelfDescribingCase kSelfDescribing[] = {
      {.name = "open_xml.xml",
       .expected_format = coda_format_xml,
       .expected_root_class = coda_record_class,
       .bytes = ToBytes(kUnmatchedBomXmlText)},
      {.name = "open_cdf3.cdf",
       .expected_format = coda_format_cdf,
       .expected_root_class = coda_record_class,
       .bytes = MakeMinimalCdf3Bytes()},
      {.name = "open_grib1.grb",
       .expected_format = coda_format_grib,
       .expected_root_class = coda_array_class,
       .bytes = MakeMinimalGrib1Bytes()},
      {.name = "open_rinex.rnx",
       .expected_format = coda_format_rinex,
       .expected_root_class = coda_record_class,
       .bytes = ToBytes(MakeMinimalRinexClockText())},
      {.name = "open_sp3.sp3",
       .expected_format = coda_format_sp3,
       .expected_root_class = coda_record_class,
       .bytes = ToBytes(MakeMinimalSp3Text())},
  };

  for (const SelfDescribingCase& tc : kSelfDescribing) {
    SCOPED_TRACE(tc.name);
    TempFile file(tc.name);
    ASSERT_TRUE(file.WriteBytes(tc.bytes));
    ScopedProduct product;
    // Upstream coda_rinex_close() never frees product->mem_ptr allocated in
    // read_main_header() via coda_mem_float_new(), so free it before
    // ScopedProduct calls coda_close().
    const absl::Cleanup free_rinex_mem_ptr = [&] {
      if (tc.expected_format == coda_format_rinex && product.get() != nullptr) {
        std::free(product.get()->mem_ptr);
        product.get()->mem_ptr = nullptr;
      }
    };
    ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

    const char* filename = nullptr;
    int64_t file_size = -1;
    coda_format format = coda_format_binary;
    const char* pclass = "sentinel";
    const char* ptype = "sentinel";
    int version = 99;
    const char* def_file = "sentinel";
    coda_type* root_type = nullptr;
    coda_type_class root_class = coda_special_class;

    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_filename(product.get(), &filename));
    EXPECT_STREQ(file.c_str(), filename);
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_file_size(product.get(), &file_size));
    EXPECT_EQ(static_cast<int64_t>(tc.bytes.size()), file_size);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &format));
    EXPECT_EQ(tc.expected_format, format);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_class(product.get(), &pclass));
    EXPECT_EQ(nullptr, pclass);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_type(product.get(), &ptype));
    EXPECT_EQ(nullptr, ptype);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_version(product.get(), &version));
    EXPECT_EQ(-1, version);
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_definition_file(product.get(), &def_file));
    EXPECT_EQ(nullptr, def_file);
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_root_type(product.get(), &root_type));
    ASSERT_NE(nullptr, root_type);
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_class(root_type, &root_class));
    EXPECT_EQ(tc.expected_root_class, root_class);
  }

  // 2. HDF5 backend open + close.
  {
    TempFile h5_file("open_hdf5.h5");
    ASSERT_NO_FATAL_FAILURE(CreateHdf5File(h5_file.path()));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
    coda_format format = coda_format_binary;
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &format));
    EXPECT_EQ(coda_format_hdf5, format);
    coda_type* root_type = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_root_type(product.get(), &root_type));
    ASSERT_NE(nullptr, root_type);
  }

  // 3. Defined products via codadef: binary, ascii, xml, and netcdf (with
  //    product_definition->root_type == NULL).
  {
    TempFile bin_file("open_bin_v1.dat");
    ASSERT_TRUE(bin_file.WriteBytes(kBinV1Bytes));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(bin_file.c_str(), product.OutParam()));
    coda_format format = coda_format_ascii;
    const char* pclass = nullptr;
    const char* ptype = nullptr;
    int version = -1;
    const char* def_file = nullptr;
    coda_type* root_type = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &format));
    EXPECT_EQ(coda_format_binary, format);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_class(product.get(), &pclass));
    EXPECT_STREQ("ProdClass", pclass);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_type(product.get(), &ptype));
    EXPECT_STREQ("BinType", ptype);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_version(product.get(), &version));
    EXPECT_EQ(1, version);
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_definition_file(product.get(), &def_file));
    EXPECT_STREQ(codadef_file.c_str(), def_file);
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_root_type(product.get(), &root_type));
    ASSERT_NE(nullptr, root_type);
  }

  {
    TempFile ascii_file("open_ascii.txt");
    ASSERT_TRUE(ascii_file.WriteText(kAsciiProductText));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(ascii_file.c_str(), product.OutParam()));
    coda_format format = coda_format_binary;
    const char* ptype = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &format));
    EXPECT_EQ(coda_format_ascii, format);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_type(product.get(), &ptype));
    EXPECT_STREQ("AsciiType", ptype);
    // Read the AsciiLine field to confirm ascii backend initialization works.
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&cursor, "line"));
    std::array<char, 32> buf{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&cursor, buf.data(),
                                      static_cast<long>(buf.size())));
    EXPECT_STREQ("ASCIITST hello", buf.data());
  }

  {
    TempFile xml_file("open_xml_def.xml");
    ASSERT_TRUE(xml_file.WriteText(kExpectedXmlText));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), product.OutParam()));
    const char* ptype = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_type(product.get(), &ptype));
    EXPECT_STREQ("XmlType", ptype);
    coda_type* root_type = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_root_type(product.get(), &root_type));
    ASSERT_NE(nullptr, root_type);
  }

  {
    TempFile nc_file("open_nc_def.nc");
    ASSERT_TRUE(nc_file.WriteBytes(MakeMinimalNetcdfBytes(1)));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(nc_file.c_str(), product.OutParam()));
    coda_format format = coda_format_binary;
    const char* ptype = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &format));
    EXPECT_EQ(coda_format_netcdf, format);
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_type(product.get(), &ptype));
    EXPECT_STREQ("NcType", ptype);
    // NcProd has no <cd:Record> root_type, so coda_get_product_root_type()
    // falls back to product->root_type via coda_get_type_for_dynamic_type().
    coda_type* root_type = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_root_type(product.get(), &root_type));
    ASSERT_NE(nullptr, root_type);
    coda_format type_fmt = coda_format_binary;
    ASSERT_EQ(CODA_SUCCESS, coda_type_get_format(root_type, &type_fmt));
    EXPECT_EQ(coda_format_netcdf, type_fmt);
  }
}

TEST(CodaProductTest, OpenRejectsUnrecognizedBinaryAndMissingDefinitionXml) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();

  TempFile codadef_file("coda_product_missing_xml.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(MakeProductTestCodadef()));
  const absl::Cleanup clear_def = [] { coda_set_definition_path(nullptr); };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Unrecognized binary file has definition == NULL -> unsupported product.
  TempFile unknown_bin("coda_product_unknown_bin.dat");
  ASSERT_TRUE(unknown_bin.WriteText("UNKNOWN_BINARY_DATA"));
  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(unknown_bin.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  // 2. File matching MissingDefProd triggers coda_read_product_definition()
  //    failure inside set_definition() because products/MissingDefProd.xml is
  //    absent from the .codadef archive.
  TempFile missing_def_file("coda_product_missing_def.dat");
  ASSERT_TRUE(missing_def_file.WriteText("MISDEF01__"));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing_def_file.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_EQ(nullptr, product.get());
}

TEST(CodaProductTest, OpenAsSupportsVersionSelectionNullClassAndErrors) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();

  TempFile codadef_file("coda_product_open_as.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(MakeProductTestCodadef()));
  const absl::Cleanup clear_def = [] { coda_set_definition_path(nullptr); };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  TempFile bin_v1("coda_product_as_bin1.dat");
  ASSERT_TRUE(bin_v1.WriteBytes(kBinV1Bytes));
  TempFile bin_v2("coda_product_as_bin2.dat");
  ASSERT_TRUE(bin_v2.WriteBytes(kBinV2Bytes));
  TempFile xml_expected("coda_product_as_xml_ok.xml");
  ASSERT_TRUE(xml_expected.WriteText(kExpectedXmlText));
  TempFile xml_unexpected("coda_product_as_xml_bad.xml");
  ASSERT_TRUE(xml_unexpected.WriteText(kUnexpectedXmlText));

  // 1. Auto-recognize v2 binary product via coda_recognize_file().
  {
    int64_t file_size = -1;
    coda_format fmt = coda_format_ascii;
    const char* pclass = nullptr;
    const char* ptype = nullptr;
    int version = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_recognize_file(bin_v2.c_str(), &file_size, &fmt, &pclass,
                                  &ptype, &version));
    EXPECT_EQ(static_cast<int64_t>(kBinV2Bytes.size()), file_size);
    EXPECT_EQ(coda_format_binary, fmt);
    EXPECT_STREQ("ProdClass", pclass);
    EXPECT_STREQ("BinType", ptype);
    EXPECT_EQ(2, version);
    EXPECT_EQ(CODA_SUCCESS,
              coda_recognize_file(bin_v2.c_str(), nullptr, nullptr, nullptr,
                                  nullptr, nullptr));
  }

  // 2. version == -1 selects latest version (version 2) even on v1 bytes.
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(bin_v1.c_str(), "ProdClass", "BinType",
                                         -1, product.OutParam()));
    int version = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_version(product.get(), &version));
    EXPECT_EQ(2, version);
  }

  // 3. Explicit version == 1 selects version 1.
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(bin_v1.c_str(), "ProdClass", "BinType",
                                         1, product.OutParam()));
    int version = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_version(product.get(), &version));
    EXPECT_EQ(1, version);
  }

  // 4. Opening a self-describing XML file as binary/ascii definition works via
  //    force_binary = 1 in coda_open_as().
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(xml_expected.c_str(), "ProdClass",
                                         "BinType", 2, product.OutParam()));
    coda_format format = coda_format_xml;
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &format));
    EXPECT_EQ(coda_format_binary, format);
  }

  // 5. product_class == NULL bypasses codadef matching: succeeds on
  //    self-describing XML (leaving product_class == NULL) and fails on binary.
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(xml_expected.c_str(), nullptr, nullptr,
                                         -1, product.OutParam()));
    const char* pclass = "sentinel";
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_class(product.get(), &pclass));
    EXPECT_EQ(nullptr, pclass);
  }
  {
    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open_as(bin_v1.c_str(), nullptr, nullptr, -1,
                               product.OutParam()));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
    EXPECT_EQ(nullptr, product.get());
  }

  // 6. Unknown product_class / product_type / version fail in data dictionary.
  struct MissingDefCase {
    std::string_view label;
    const char* pclass;
    const char* ptype;
    int version;
  };
  constexpr MissingDefCase kMissingDefCases[] = {
      {.label = "unknown_class",
       .pclass = "NoSuchClass",
       .ptype = "BinType",
       .version = 1},
      {.label = "unknown_type",
       .pclass = "ProdClass",
       .ptype = "NoSuchType",
       .version = 1},
      {.label = "unknown_version",
       .pclass = "ProdClass",
       .ptype = "BinType",
       .version = 99},
  };
  for (const MissingDefCase& tc : kMissingDefCases) {
    SCOPED_TRACE(tc.label);
    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open_as(bin_v1.c_str(), tc.pclass, tc.ptype, tc.version,
                               product.OutParam()));
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
    EXPECT_EQ(nullptr, product.get());
  }

  // 7. Incompatible format definition (XML definition applied to binary file).
  {
    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open_as(bin_v1.c_str(), "ProdClass", "XmlType", 1,
                               product.OutParam()));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("cannot use xml definition for binary product"));
    EXPECT_EQ(nullptr, product.get());
  }

  // 8. XML schema mismatch triggers coda_xml_reopen_with_definition() failure
  //    inside set_definition().
  {
    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open_as(xml_unexpected.c_str(), "ProdClass", "XmlType",
                               1, product.OutParam()));
    EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
    EXPECT_EQ(nullptr, product.get());
  }

  // 9. Missing file passed to coda_open_as() fails in open_file().
  {
    const std::filesystem::path missing_path =
        std::filesystem::path(::testing::TempDir()) /
        "coda_product_open_as_missing.dat";
    std::filesystem::remove(missing_path);
    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open_as(missing_path.c_str(), "ProdClass", "BinType", 1,
                               product.OutParam()));
    EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
    EXPECT_EQ(nullptr, product.get());
  }
}

TEST(CodaProductTest, NullArgumentChecksAcrossAllPublicProductApis) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();

  TempFile codadef_file("coda_product_null_args.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(MakeProductTestCodadef()));
  const absl::Cleanup clear_def = [] { coda_set_definition_path(nullptr); };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  TempFile bin_file("coda_product_null_args.dat");
  ASSERT_TRUE(bin_file.WriteBytes(kBinV1Bytes));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(bin_file.c_str(), product.OutParam()));

  // coda_open / coda_open_as / coda_close NULL checks.
  ScopedProduct out_prod;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(nullptr, out_prod.OutParam()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(bin_file.c_str(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open_as(nullptr, "ProdClass", "BinType", 1,
                             out_prod.OutParam()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open_as(bin_file.c_str(), "ProdClass", nullptr, 1,
                             out_prod.OutParam()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_open_as(bin_file.c_str(), "ProdClass", "BinType", 1, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_close(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // Accessor NULL product and NULL out-parameter checks.
  const char* str_out = nullptr;
  int64_t i64_out = 0;
  coda_format fmt_out = coda_format_binary;
  int int_out = 0;
  coda_type* type_out = nullptr;

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_filename(nullptr, &str_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_file_size(nullptr, &i64_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_file_size(product.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_format(nullptr, &fmt_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_format(product.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_class(nullptr, &str_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_class(product.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_type(nullptr, &str_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_type(product.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_version(nullptr, &int_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_version(product.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_root_type(nullptr, &type_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_root_type(product.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_definition_file(nullptr, &str_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_definition_file(product.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_get_product_variable_value(nullptr, "scalar_var", 0, &i64_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_get_product_variable_value(product.get(), nullptr, 0, &i64_out));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
}

TEST(CodaProductTest, ProductVariablesEvaluateValidateIndicesAndFreeOnClose) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  restore_def.Unset();

  TempFile codadef_file("coda_product_vars.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(MakeProductTestCodadef()));
  const absl::Cleanup clear_def = [] { coda_set_definition_path(nullptr); };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  TempFile bin_file("coda_product_vars.dat");
  ASSERT_TRUE(bin_file.WriteBytes(kBinV1Bytes));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(bin_file.c_str(), product.OutParam()));

  // 1. Scalar product variable (uses /val = 42 -> 42 + 100 = 142).
  int64_t value = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_get_product_variable_value(
                              product.get(), "scalar_var", 0, &value));
  EXPECT_EQ(142, value);

  // 2. Array product variable (size 3: [10, 20, 42]).
  for (long i = 0; i < 3; ++i) {
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_variable_value(
                                product.get(), "array_var", i, &value));
    EXPECT_EQ(i == 0 ? 10 : (i == 1 ? 20 : 42), value);
  }

  // 3. Internal coda_product_variable_get_size / get_pointer checks.
  long var_size = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_product_variable_get_size(
                              product.get(), "array_var", &var_size));
  EXPECT_EQ(3L, var_size);

  int64_t* var_ptr = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_product_variable_get_pointer(
                              product.get(), "array_var", 2, &var_ptr));
  ASSERT_NE(nullptr, var_ptr);
  EXPECT_EQ(42, *var_ptr);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_variable_get_pointer(product.get(), "array_var", 3,
                                                  &var_ptr));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 4. Unknown variable name -> CODA_ERROR_INVALID_NAME.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_get_product_variable_value(product.get(), "no_such_var", 0,
                                                &value));
  EXPECT_EQ(CODA_ERROR_INVALID_NAME, coda_errno);

  // 5. Negative and out-of-range indices -> CODA_ERROR_INVALID_INDEX.
  for (const long bad_index : {-1L, 3L}) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_get_product_variable_value(product.get(), "array_var",
                                                  bad_index, &value));
    EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);
  }

  // 6. Leave `unused_var` uninitialized so coda_close() exercises both
  //    non-NULL and NULL product_variable[i] cleanup branches.
  EXPECT_EQ(CODA_SUCCESS, coda_close(product.release()));
}

// TODO(schwehr): 93.8% line coverage in libcoda/coda-product.c (392/418);
// remaining 26 missed lines are OOM branches and disabled HDF4 format dispatch
// paths.

}  // namespace
}  // namespace stcorp_coda
