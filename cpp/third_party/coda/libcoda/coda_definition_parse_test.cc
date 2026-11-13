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
// Tests CODA .codadef XML and archive parsing, directory scanning, type
// construction, detection rule parsing, and error handling
// (libcoda/coda-definition-parse.c).
//
// google3 build caveats:
//   - coda-definition-parse.c has no dedicated header; coda_read_definitions()
//     and coda_read_product_definition() are declared in coda-internal.h
//     (included via coda_internal_prelude.h).
//   - patches/libcoda_coda-definition-parse.c.patch rejects .codadef entries
//     claiming uncompressed size > 10000000 with CODA_ERROR_OUT_OF_MEMORY.
//   - Upstream decode_xml_string() decodes "&gt;" to '<'; tests exercise the
//     entity-decoding branch without asserting the mis-decoded character.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-definition-parse.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-definition.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-internal.h

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-definition.h"
#include "third_party/stcorp_coda/libcoda/coda-type.h"
}  // extern "C"

#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

constexpr std::string_view kNs = "http://www.stcorp.nl/coda/definition/2008/07";

// Isolates CODA definition path, environment variables, thread-local
// coda_option_read_all_definitions, and coda_errno across every test.
class ScopedDefinitionTestEnv {
 public:
  ScopedDefinitionTestEnv()
      : env_def_("CODA_DEFINITION"),
        env_leap_("CODA_LEAP_SECOND_TABLE"),
        saved_read_all_(coda_option_read_all_definitions) {
    env_def_.Unset();
    env_leap_.Unset();
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  ~ScopedDefinitionTestEnv() {
    coda_option_read_all_definitions = saved_read_all_;
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  ScopedDefinitionTestEnv(const ScopedDefinitionTestEnv&) = delete;
  ScopedDefinitionTestEnv& operator=(const ScopedDefinitionTestEnv&) = delete;

 private:
  ScopedEnvVar env_def_;
  ScopedEnvVar env_leap_;
  int saved_read_all_;
};

std::string MakeMinimalIndexXml(std::string_view class_name,
                                std::string_view type_name = "TestType",
                                std::string_view def_id = "TestProd",
                                std::string_view format = "binary",
                                std::string_view version = "1",
                                std::optional<int> match_size = 4) {
  std::string xml = std::string(
                        "<?xml version=\"1.0\"?>\n"
                        "<cd:ProductClass xmlns:cd=\"") +
                    std::string(kNs) + "\" name=\"" + std::string(class_name) +
                    "\">\n"
                    "  <cd:ProductType name=\"" +
                    std::string(type_name) +
                    "\">\n"
                    "    <cd:ProductDefinition id=\"" +
                    std::string(def_id) + "\" format=\"" + std::string(format) +
                    "\" version=\"" + std::string(version) + "\"";
  if (match_size.has_value()) {
    xml += ">\n      <cd:DetectionRule>\n        <cd:MatchSize size=\"" +
           std::to_string(*match_size) +
           "\"/>\n      </cd:DetectionRule>\n    </cd:ProductDefinition>\n";
  } else {
    xml += "/>\n";
  }
  xml += "  </cd:ProductType>\n</cd:ProductClass>\n";
  return xml;
}

std::vector<uint8_t> BuildCodadefWithVersion(
    std::string_view index_xml, std::optional<std::string_view> version_text,
    std::span<const std::pair<std::string_view, std::string_view>>
        extra_entries = {}) {
  std::vector<ZipEntrySpec> specs;
  specs.push_back({
      .name = "index.xml",
      .data = std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(index_xml.data()), index_xml.size()),
      .method = ZipMethod::kDeflated,
  });
  if (version_text.has_value()) {
    specs.push_back({
        .name = "VERSION",
        .data = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(version_text->data()),
            version_text->size()),
        .method = ZipMethod::kDeflated,
    });
  }
  for (const auto& [name, content] : extra_entries) {
    specs.push_back({
        .name = name,
        .data = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(content.data()), content.size()),
        .method = ZipMethod::kDeflated,
    });
  }
  return BuildZipBytes(specs);
}

void WriteLe32At(std::vector<uint8_t>& buf, size_t offset, uint32_t value) {
  buf[offset] = static_cast<uint8_t>(value & 0xFF);
  buf[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  buf[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  buf[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

std::optional<size_t> FindSignature(std::span<const uint8_t> data,
                                    uint32_t sig) {
  const std::array<uint8_t, 4> pattern = {
      static_cast<uint8_t>(sig & 0xFF),
      static_cast<uint8_t>((sig >> 8) & 0xFF),
      static_cast<uint8_t>((sig >> 16) & 0xFF),
      static_cast<uint8_t>((sig >> 24) & 0xFF),
  };
  for (size_t i = 0; i + 4 <= data.size(); ++i) {
    if (std::memcmp(data.data() + i, pattern.data(), 4) == 0) {
      return i;
    }
  }
  return std::nullopt;
}

TEST(CodaDefinitionParseTest,
     ReadDefinitionsScansColonPathDirectoriesAndRegularFiles) {
  const ScopedDefinitionTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  // Empty definition path succeeds with no loaded classes.
  ASSERT_EQ(CODA_SUCCESS, coda_read_definitions(""));

  const TempDir dir("coda_def_parse_dir");
  // Non-codadef file, exact ".codadef" (len == 8), and directory ending in
  // ".codadef" must all be skipped by coda_read_definitions().
  ASSERT_TRUE(WriteFileText(dir.child("readme.txt"), "not a codadef"));
  ASSERT_TRUE(WriteFileText(dir.child(".codadef"), "too short name"));
  std::error_code ec;
  ASSERT_TRUE(
      std::filesystem::create_directory(dir.child("nested.codadef"), ec));

  const std::string alpha_xml = MakeMinimalIndexXml(
      "AlphaClass", "TestType", "TestProd", "binary", "1", 4);
  ASSERT_TRUE(WriteFileBytes(
      dir.child("alpha.codadef"),
      BuildCodadefWithVersion(alpha_xml, std::string_view("5"))));

  const TempFile direct_file("coda_def_parse_direct.codadef");
  const std::string direct_xml = MakeMinimalIndexXml(
      "DirectClass", "TestType", "TestProd", "binary", "1", 8);
  ASSERT_TRUE(direct_file.WriteBytes(BuildCodadefBytes(direct_xml)));

  const std::string nonexistent =
      (dir.path() / "does_not_exist_component").native();
  const std::string search_path =
      dir.string() + ":" + nonexistent + ":" + direct_file.string() + ":";
  ASSERT_EQ(CODA_SUCCESS, coda_read_definitions(search_path.c_str()));

  EXPECT_EQ(1, coda_data_dictionary_has_product_class("AlphaClass"));
  EXPECT_EQ(1, coda_data_dictionary_has_product_class("DirectClass"));

  coda_product_class* alpha =
      coda_data_dictionary_get_product_class("AlphaClass");
  ASSERT_NE(nullptr, alpha);
  EXPECT_EQ(5, coda_product_class_get_revision(alpha));

  coda_product_class* direct =
      coda_data_dictionary_get_product_class("DirectClass");
  ASSERT_NE(nullptr, direct);
  EXPECT_EQ(0, coda_product_class_get_revision(direct));
}

TEST(CodaDefinitionParseTest, ReadDefinitionsDirectoryAndArchiveErrorPaths) {
  const ScopedDefinitionTestEnv test_env;

  // 1. Broken symlink ending in ".codadef" triggers stat() failure in directory
  //    scan inside coda_read_definitions().
  {
    ScopedCodaSession coda;
    ASSERT_EQ(CODA_SUCCESS, coda.Init());
    const TempDir dir("coda_def_parse_broken_link");
    std::error_code ec;
    std::filesystem::create_symlink(dir.child("missing_target.codadef"),
                                    dir.child("broken.codadef"), ec);
    ASSERT_FALSE(ec);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_read_definitions(dir.c_str()));
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("could not access file"));
  }

  // 2. Non-ZIP ".codadef" inside a directory triggers read_definition_file()
  //    error propagation inside readdir() loop.
  {
    ScopedCodaSession coda;
    ASSERT_EQ(CODA_SUCCESS, coda.Init());
    const TempDir dir("coda_def_parse_bad_in_dir");
    ASSERT_TRUE(WriteFileText(dir.child("corrupt.codadef"), "not a zip"));

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_read_definitions(dir.c_str()));
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("could not read data from definition file"));
  }

  // 3. Valid ZIP archive missing index.xml triggers ze_index missing error.
  {
    ScopedCodaSession coda;
    ASSERT_EQ(CODA_SUCCESS, coda.Init());
    const TempFile no_index("coda_def_parse_no_index.codadef");
    constexpr std::string_view kDummy = "hello";
    const std::array<ZipEntrySpec, 1> specs = {{
        {.name = "other.xml",
         .data = std::span<const uint8_t>(
             reinterpret_cast<const uint8_t*>(kDummy.data()), kDummy.size()),
         .method = ZipMethod::kDeflated},
    }};
    ASSERT_TRUE(no_index.WriteBytes(BuildZipBytes(specs)));

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_read_definitions(no_index.c_str()));
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("index missing"));
  }

  // 4. Corrupt deflate stream inside index.xml triggers za_read_entry failure.
  {
    ScopedCodaSession coda;
    ASSERT_EQ(CODA_SUCCESS, coda.Init());
    const TempFile bad_deflate("coda_def_parse_bad_deflate.codadef");
    const std::string xml = MakeMinimalIndexXml("BadDeflate");
    std::vector<uint8_t> zip_bytes = BuildCodadefBytes(xml);
    // Corrupt compressed payload right after 30-byte local header +
    // "index.xml".
    ASSERT_GT(zip_bytes.size(), 42u);
    zip_bytes[40] ^= 0xFF;
    zip_bytes[41] ^= 0xAA;
    ASSERT_TRUE(bad_deflate.WriteBytes(zip_bytes));

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_read_definitions(bad_deflate.c_str()));
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("could not read data from definition file"));
  }
}

TEST(CodaDefinitionParseTest, VersionParsingAndRevisionPrecedence) {
  const ScopedDefinitionTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  // Empty VERSION (0 bytes) resolves to revision 0.
  const TempFile empty_ver("coda_def_parse_empty_ver.codadef");
  ASSERT_TRUE(empty_ver.WriteBytes(BuildCodadefWithVersion(
      MakeMinimalIndexXml("EmptyVerClass", "TestType", "TestProd", "binary",
                          "1", 10),
      std::string_view(""))));
  ASSERT_EQ(CODA_SUCCESS, coda_read_definitions(empty_ver.c_str()));
  coda_product_class* empty_cls =
      coda_data_dictionary_get_product_class("EmptyVerClass");
  ASSERT_NE(nullptr, empty_cls);
  EXPECT_EQ(0, coda_product_class_get_revision(empty_cls));

  // Trailing non-digits in VERSION are ignored by coda_ascii_parse_int64(...,
  // ignore_trailing_bytes=1).
  const TempFile trailing_ver("coda_def_parse_trailing_ver.codadef");
  ASSERT_TRUE(trailing_ver.WriteBytes(BuildCodadefWithVersion(
      MakeMinimalIndexXml("TrailingVerClass", "TestType", "TestProd", "binary",
                          "1", 11),
      std::string_view("7abc"))));
  ASSERT_EQ(CODA_SUCCESS, coda_read_definitions(trailing_ver.c_str()));
  coda_product_class* trailing_cls =
      coda_data_dictionary_get_product_class("TrailingVerClass");
  ASSERT_NE(nullptr, trailing_cls);
  EXPECT_EQ(7, coda_product_class_get_revision(trailing_cls));

  // Completely non-numeric VERSION causes coda_ascii_parse_int64() to set
  // CODA_ERROR_INVALID_FORMAT; get_product_class_revision() treats the
  // revision as 0 without clearing coda_errno, so XML_Parse() still adds the
  // class and parse_entry() then reports CODA_ERROR_INVALID_FORMAT.
  const TempFile bad_ver("coda_def_parse_bad_ver.codadef");
  ASSERT_TRUE(bad_ver.WriteBytes(BuildCodadefWithVersion(
      MakeMinimalIndexXml("BadVerClass", "TestType", "TestProd", "binary", "1",
                          12),
      std::string_view("not-an-int"))));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_read_definitions(bad_ver.c_str()));
  EXPECT_EQ(CODA_ERROR_INVALID_FORMAT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  coda_product_class* bad_cls =
      coda_data_dictionary_get_product_class("BadVerClass");
  ASSERT_NE(nullptr, bad_cls);
  EXPECT_EQ(0, coda_product_class_get_revision(bad_cls));

  // Revision precedence for duplicate ProductClass "RevClass":
  // 1. Load revision 10 with ProductType "TypeV10".
  const TempFile rev10("coda_def_parse_rev10.codadef");
  ASSERT_TRUE(rev10.WriteBytes(BuildCodadefWithVersion(
      MakeMinimalIndexXml("RevClass", "TypeV10", "ProdV10", "binary", "1", 20),
      std::string_view("10"))));
  ASSERT_EQ(CODA_SUCCESS, coda_read_definitions(rev10.c_str()));

  // 2. Lower revision (5) and equal revision (10) are ignored without error.
  const TempFile rev5("coda_def_parse_rev5.codadef");
  ASSERT_TRUE(rev5.WriteBytes(BuildCodadefWithVersion(
      MakeMinimalIndexXml("RevClass", "TypeV5", "ProdV5", "binary", "1", 21),
      std::string_view("5"))));
  ASSERT_EQ(CODA_SUCCESS, coda_read_definitions(rev5.c_str()));
  coda_product_class* rev_cls =
      coda_data_dictionary_get_product_class("RevClass");
  ASSERT_NE(nullptr, rev_cls);
  EXPECT_EQ(10, coda_product_class_get_revision(rev_cls));
  EXPECT_EQ(1, coda_product_class_has_product_type(rev_cls, "TypeV10"));
  EXPECT_EQ(0, coda_product_class_has_product_type(rev_cls, "TypeV5"));

  const TempFile rev10_dup("coda_def_parse_rev10_dup.codadef");
  ASSERT_TRUE(rev10_dup.WriteBytes(BuildCodadefWithVersion(
      MakeMinimalIndexXml("RevClass", "TypeV10Dup", "ProdV10Dup", "binary", "1",
                          22),
      std::string_view("10"))));
  ASSERT_EQ(CODA_SUCCESS, coda_read_definitions(rev10_dup.c_str()));
  rev_cls = coda_data_dictionary_get_product_class("RevClass");
  ASSERT_NE(nullptr, rev_cls);
  EXPECT_EQ(1, coda_product_class_has_product_type(rev_cls, "TypeV10"));
  EXPECT_EQ(0, coda_product_class_has_product_type(rev_cls, "TypeV10Dup"));

  // 3. Higher revision (20) replaces the older class in the data dictionary.
  const TempFile rev20("coda_def_parse_rev20.codadef");
  ASSERT_TRUE(rev20.WriteBytes(BuildCodadefWithVersion(
      MakeMinimalIndexXml("RevClass", "TypeV20", "ProdV20", "binary", "1", 23),
      std::string_view("20"))));
  ASSERT_EQ(CODA_SUCCESS, coda_read_definitions(rev20.c_str()));
  rev_cls = coda_data_dictionary_get_product_class("RevClass");
  ASSERT_NE(nullptr, rev_cls);
  EXPECT_EQ(20, coda_product_class_get_revision(rev_cls));
  EXPECT_EQ(0, coda_product_class_has_product_type(rev_cls, "TypeV10"));
  EXPECT_EQ(1, coda_product_class_has_product_type(rev_cls, "TypeV20"));
}

TEST(CodaDefinitionParseTest, IndexParsesAllFormatsAndDetectionRules) {
  const ScopedDefinitionTestEnv test_env;

  constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07"
                 xmlns:ext="http://example.com/custom"
                 name="FmtClass">
  <cd:Description>Class<!--chunk--> desc &amp;amp; &amp;apos; &amp;lt; &amp;gt; &amp;quot; &amp;keep;</cd:Description>
  <ext:IgnoredBlock>
    <ext:Nested>ignored text inside foreign namespace</ext:Nested>
  </ext:IgnoredBlock>
  <cd:ProductType name="EmptyDescType">
    <cd:Description/>
    <cd:ProductDefinition id="P_ascii" format="ascii" version="1">
      <cd:Description/>
      <cd:DetectionRule>
        <cd:MatchData offset="0">ASC\n\r\t\a\b\f\v\e\\\101\007\000</cd:MatchData>
        <cd:MatchFilename offset="0">ascii_prefix</cd:MatchFilename>
        <cd:MatchSize size="16"/>
      </cd:DetectionRule>
    </cd:ProductDefinition>
    <cd:ProductDefinition id="P_binary" format="binary" version="2">
      <cd:Description>bin desc</cd:Description>
      <cd:DetectionRule>
        <cd:MatchData offset="0">BINMAGIC</cd:MatchData>
        <cd:MatchSize size="12"/>
      </cd:DetectionRule>
      <cd:DetectionRule>
        <cd:MatchData>^rgx[0]+(a|b)?{1}.*$\\\101\7\00</cd:MatchData>
      </cd:DetectionRule>
      <cd:DetectionRule>
        <cd:MatchExpression>filesize() == 20</cd:MatchExpression>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="AllFormatsType">
    <cd:Description>All remaining formats</cd:Description>
    <cd:ProductDefinition id="P_xml" format="xml" version="1">
      <cd:DetectionRule>
        <cd:MatchData path="/{http://example.com/ns}root/tag">ok</cd:MatchData>
        <cd:MatchData path="/root/tag@attr"/>
        <cd:MatchExpression path="/root">str(./tag) == "ok"</cd:MatchExpression>
      </cd:DetectionRule>
    </cd:ProductDefinition>
    <cd:ProductDefinition id="P_hdf4" format="hdf4" version="2"/>
    <cd:ProductDefinition id="P_hdf5" format="hdf5" version="3"/>
    <cd:ProductDefinition id="P_cdf" format="cdf" version="4"/>
    <cd:ProductDefinition id="P_netcdf" format="netcdf" version="5"/>
    <cd:ProductDefinition id="P_grib" format="grib" version="6"/>
    <cd:ProductDefinition id="P_rinex" format="rinex" version="7"/>
    <cd:ProductDefinition id="P_sp3" format="sp3" version="8"/>
  </cd:ProductType>
</cd:ProductClass>
)xml";

  const TempFile codadef("coda_def_parse_all_formats.codadef");
  ASSERT_TRUE(codadef.WriteBytes(BuildCodadefBytes(kIndexXml)));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef.c_str()));

  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  struct FormatCheck {
    const char* type_name;
    int version;
    coda_format format;
  };
  constexpr FormatCheck kFormatChecks[] = {
      {.type_name = "EmptyDescType", .version = 1, .format = coda_format_ascii},
      {.type_name = "EmptyDescType",
       .version = 2,
       .format = coda_format_binary},
      {.type_name = "AllFormatsType", .version = 1, .format = coda_format_xml},
      {.type_name = "AllFormatsType", .version = 2, .format = coda_format_hdf4},
      {.type_name = "AllFormatsType", .version = 3, .format = coda_format_hdf5},
      {.type_name = "AllFormatsType", .version = 4, .format = coda_format_cdf},
      {.type_name = "AllFormatsType",
       .version = 5,
       .format = coda_format_netcdf},
      {.type_name = "AllFormatsType", .version = 6, .format = coda_format_grib},
      {.type_name = "AllFormatsType",
       .version = 7,
       .format = coda_format_rinex},
      {.type_name = "AllFormatsType", .version = 8, .format = coda_format_sp3},
  };

  for (const FormatCheck& tc : kFormatChecks) {
    SCOPED_TRACE(coda_type_get_format_name(tc.format));
    coda_product_definition* def = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_data_dictionary_get_definition(
                                "FmtClass", tc.type_name, tc.version, &def));
    ASSERT_NE(nullptr, def);
    EXPECT_EQ(tc.format, def->format);
  }

  // Verify parsed detection rules recognize a matching binary file.
  const TempFile bin_product("coda_def_parse_bin_detect.dat");
  constexpr auto kBinPayload = std::to_array<uint8_t>(
      {'B', 'I', 'N', 'M', 'A', 'G', 'I', 'C', 0, 0, 0, 0});
  ASSERT_TRUE(bin_product.WriteBytes(kBinPayload));

  int64_t file_size = 0;
  coda_format detected_format = coda_format_ascii;
  const char* product_class = nullptr;
  const char* product_type = nullptr;
  int version = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_recognize_file(bin_product.c_str(), &file_size,
                                              &detected_format, &product_class,
                                              &product_type, &version));
  EXPECT_EQ(12, file_size);
  EXPECT_EQ(coda_format_binary, detected_format);
  EXPECT_STREQ("FmtClass", product_class);
  EXPECT_STREQ("EmptyDescType", product_type);
  EXPECT_EQ(2, version);
}

TEST(CodaDefinitionParseTest,
     ParsesAllBinaryAsciiTypesAndNamedTypesWithReadAllDefinitions) {
  const ScopedDefinitionTestEnv test_env;
  coda_option_read_all_definitions = 1;

  constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="FullClass">
  <cd:ProductType name="FullType">
    <cd:ProductDefinition id="FullProd" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">FULL</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
    <cd:ProductDefinition id="OpenProd" format="binary" version="2">
      <cd:DetectionRule>
        <cd:MatchData offset="0">OPEN</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)xml";

  constexpr std::string_view kSharedIntXml = R"xml(<?xml version="1.0"?>
<cd:Integer xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07"
            name="SharedInt" format="binary">
  <cd:Description>shared int32</cd:Description>
  <cd:BitSize>32</cd:BitSize>
  <cd:NativeType>int32</cd:NativeType>
  <cd:LittleEndian/>
</cd:Integer>
)xml";

  // Exercises Record, Union, Array, Integer (all 8 int native types), Float
  // (float/double + ascii mappings), Text (char/string + escapes), Raw (fixed +
  // empty FixedValue), AsciiLine/Separator/WhiteSpace, Complex, VSFInteger,
  // Time (all 10 ascii_* and 4 binary_* presets + custom mapping), Type
  // (attribute format propagation across record/array/special/number),
  // Attribute, Conversion, NamedType (cached second lookup), and
  // ProductVariable.
  constexpr std::string_view kFullProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07"
                      id="FullProd" format="binary">
  <cd:Record>
    <cd:Description>root record</cd:Description>
    <cd:BitSize>int(./count) * 8</cd:BitSize>
    <cd:Attribute name="rec_attr_default"/>
    <cd:Attribute name="rec_attr_fixed">
      <cd:Hidden/>
      <cd:Optional/>
      <cd:Available>true</cd:Available>
      <cd:FixedValue>ATTR\n\r\t\a\b\f\v\\</cd:FixedValue>
    </cd:Attribute>
    <cd:Field name="count" real_name="RecordCount">
      <cd:Integer>
        <cd:Description>count</cd:Description>
        <cd:Unit/>
        <cd:BitSize>8</cd:BitSize>
        <cd:NativeType>uint8</cd:NativeType>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="ints">
      <cd:Record>
        <cd:Field name="i8"><cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>int8</cd:NativeType></cd:Integer></cd:Field>
        <cd:Field name="i16"><cd:Integer><cd:ByteSize>2</cd:ByteSize><cd:NativeType>int16</cd:NativeType><cd:LittleEndian/></cd:Integer></cd:Field>
        <cd:Field name="u16"><cd:Integer><cd:BitSize>16</cd:BitSize><cd:NativeType>uint16</cd:NativeType></cd:Integer></cd:Field>
        <cd:Field name="i32"><cd:Integer><cd:BitSize>32</cd:BitSize><cd:NativeType>int32</cd:NativeType></cd:Integer></cd:Field>
        <cd:Field name="u32"><cd:Integer><cd:BitSize>32</cd:BitSize><cd:NativeType>uint32</cd:NativeType></cd:Integer></cd:Field>
        <cd:Field name="i64"><cd:Integer><cd:BitSize>64</cd:BitSize><cd:NativeType>int64</cd:NativeType></cd:Integer></cd:Field>
        <cd:Field name="u64">
          <cd:Integer>
            <cd:BitSize>64</cd:BitSize>
            <cd:NativeType>uint64</cd:NativeType>
            <cd:Unit>samples</cd:Unit>
            <cd:Conversion numerator="2" denominator="1" offset="1" invalid="0">
              <cd:Unit/>
            </cd:Conversion>
            <cd:Attribute name="int_attr"><cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>int8</cd:NativeType></cd:Integer></cd:Attribute>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="expr_sized_int">
          <cd:Integer>
            <cd:BitSize>int(./count)</cd:BitSize>
            <cd:NativeType>uint8</cd:NativeType>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="expr_bytes_int">
          <cd:Integer>
            <cd:ByteSize>int(./count)</cd:ByteSize>
            <cd:NativeType>uint8</cd:NativeType>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="ascii_int">
          <cd:Integer format="ascii">
            <cd:ByteSize>4</cd:ByteSize>
            <cd:Mapping string="NA" value="-1"/>
          </cd:Integer>
        </cd:Field>
      </cd:Record>
    </cd:Field>
    <cd:Field name="floats">
      <cd:Record>
        <cd:Field name="f32">
          <cd:Float>
            <cd:Description>f32 desc</cd:Description>
            <cd:Unit/>
            <cd:BitSize>32</cd:BitSize>
            <cd:NativeType>float</cd:NativeType>
            <cd:LittleEndian/>
            <cd:Conversion numerator="1" denominator="2">
              <cd:Unit>K</cd:Unit>
            </cd:Conversion>
            <cd:Attribute name="flt_attr"/>
          </cd:Float>
        </cd:Field>
        <cd:Field name="f64">
          <cd:Float>
            <cd:Unit>m</cd:Unit>
            <cd:ByteSize>8</cd:ByteSize>
            <cd:NativeType>double</cd:NativeType>
          </cd:Float>
        </cd:Field>
        <cd:Field name="ascii_flt">
          <cd:Float format="ascii">
            <cd:ByteSize>5</cd:ByteSize>
            <cd:Mapping string="1.500" value="1.5"/>
            <cd:Mapping string="NAN__" value="nan"/>
            <cd:Mapping string="INF__" value="inf"/>
            <cd:Mapping string="+INF_" value="+inf"/>
            <cd:Mapping string="-INF_" value="-inf"/>
          </cd:Float>
        </cd:Field>
      </cd:Record>
    </cd:Field>
    <cd:Field name="texts_and_raw">
      <cd:Record>
        <cd:Field name="ch">
          <cd:Text>
            <cd:Description>single char</cd:Description>
            <cd:ByteSize>1</cd:ByteSize>
            <cd:NativeType>char</cd:NativeType>
          </cd:Text>
        </cd:Field>
        <cd:Field name="str_fixed">
          <cd:Text>
            <cd:ByteSize>4</cd:ByteSize>
            <cd:FixedValue>AB\n\\</cd:FixedValue>
            <cd:NativeType>string</cd:NativeType>
            <cd:Attribute name="txt_attr"/>
          </cd:Text>
        </cd:Field>
        <cd:Field name="str_dyn">
          <cd:Text>
            <cd:ByteSize>int(./count)</cd:ByteSize>
          </cd:Text>
        </cd:Field>
        <cd:Field name="raw_fixed">
          <cd:Raw>
            <cd:Description>raw bytes</cd:Description>
            <cd:BitSize>32</cd:BitSize>
            <cd:FixedValue>\001\e\7\00</cd:FixedValue>
          </cd:Raw>
        </cd:Field>
        <cd:Field name="raw_empty_fixed">
          <cd:Raw>
            <cd:BitSize>int(./count) * 8</cd:BitSize>
            <cd:FixedValue/>
          </cd:Raw>
        </cd:Field>
        <cd:Field name="ascii_line"><cd:AsciiLine format="ascii"><cd:Description>line</cd:Description></cd:AsciiLine></cd:Field>
        <cd:Field name="ascii_sep"><cd:AsciiLineSeparator format="ascii"><cd:Description>sep</cd:Description></cd:AsciiLineSeparator></cd:Field>
        <cd:Field name="ascii_ws"><cd:AsciiWhiteSpace format="ascii"><cd:Description>ws</cd:Description></cd:AsciiWhiteSpace></cd:Field>
      </cd:Record>
    </cd:Field>
    <cd:Field name="compounds">
      <cd:Record>
        <cd:Field name="arr">
          <cd:Array>
            <cd:Description>2d array</cd:Description>
            <cd:Dimension>2</cd:Dimension>
            <cd:Dimension>int(./count)</cd:Dimension>
            <cd:Attribute name="arr_attr"/>
            <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer>
          </cd:Array>
        </cd:Field>
        <cd:Field name="uni">
          <cd:Union>
            <cd:Description>union desc</cd:Description>
            <cd:BitSize>16</cd:BitSize>
            <cd:FieldExpression>0</cd:FieldExpression>
            <cd:Attribute name="uni_attr"/>
            <cd:Field name="u_int"><cd:Integer><cd:BitSize>16</cd:BitSize><cd:NativeType>int16</cd:NativeType></cd:Integer></cd:Field>
            <cd:Field name="u_raw"><cd:Raw><cd:BitSize>16</cd:BitSize></cd:Raw></cd:Field>
          </cd:Union>
        </cd:Field>
        <cd:Field name="cplx_flt">
          <cd:Complex>
            <cd:Description>complex float</cd:Description>
            <cd:Float><cd:BitSize>32</cd:BitSize><cd:NativeType>float</cd:NativeType></cd:Float>
          </cd:Complex>
        </cd:Field>
        <cd:Field name="cplx_int">
          <cd:Complex>
            <cd:Integer><cd:BitSize>16</cd:BitSize><cd:NativeType>int16</cd:NativeType></cd:Integer>
          </cd:Complex>
        </cd:Field>
        <cd:Field name="vsf1">
          <cd:VSFInteger>
            <cd:Description>vsf with unit</cd:Description>
            <cd:ScaleFactor>
              <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>int8</cd:NativeType></cd:Integer>
              <cd:Description>sf</cd:Description>
            </cd:ScaleFactor>
            <cd:Integer><cd:BitSize>16</cd:BitSize><cd:NativeType>int16</cd:NativeType></cd:Integer>
            <cd:Unit>Pa</cd:Unit>
          </cd:VSFInteger>
        </cd:Field>
        <cd:Field name="vsf2">
          <cd:VSFInteger>
            <cd:ScaleFactor>
              <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer>
            </cd:ScaleFactor>
            <cd:Integer><cd:BitSize>16</cd:BitSize><cd:NativeType>int16</cd:NativeType></cd:Integer>
            <cd:Unit/>
          </cd:VSFInteger>
        </cd:Field>
        <cd:Field name="wrapped_type">
          <cd:Type>
            <cd:Description>wrapped desc</cd:Description>
            <cd:Attribute name="attr_rec">
              <cd:Record>
                <cd:Attribute name="nested_attr"/>
                <cd:Field name="af"><cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>int8</cd:NativeType></cd:Integer></cd:Field>
              </cd:Record>
            </cd:Attribute>
            <cd:Attribute name="attr_arr">
              <cd:Array>
                <cd:Dimension>1</cd:Dimension>
                <cd:Complex><cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>int8</cd:NativeType></cd:Integer></cd:Complex>
              </cd:Array>
            </cd:Attribute>
            <cd:Integer format="ascii"><cd:ByteSize>2</cd:ByteSize></cd:Integer>
          </cd:Type>
        </cd:Field>
        <cd:Field name="named1"><cd:NamedType id="SharedInt"/></cd:Field>
        <cd:Field name="named2"><cd:NamedType id="SharedInt"/></cd:Field>
        <cd:Field name="opt_field">
          <cd:Hidden/>
          <cd:Optional/>
          <cd:Available>int(./count) > 0</cd:Available>
          <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer>
          <cd:BitOffset>0</cd:BitOffset>
        </cd:Field>
      </cd:Record>
    </cd:Field>
    <cd:Field name="times">
      <cd:Record>
        <cd:Field name="t_a1"><cd:Time format="ascii" timeformat="ascii_envisat_datetime"/></cd:Field>
        <cd:Field name="t_a2"><cd:Time format="ascii" timeformat="ascii_gome_datetime"/></cd:Field>
        <cd:Field name="t_a3"><cd:Time format="ascii" timeformat="ascii_eps_datetime"/></cd:Field>
        <cd:Field name="t_a4"><cd:Time format="ascii" timeformat="ascii_eps_datetime_long"/></cd:Field>
        <cd:Field name="t_a5"><cd:Time format="ascii" timeformat="ascii_ccsds_datetime_ymd1"/></cd:Field>
        <cd:Field name="t_a6"><cd:Time format="ascii" timeformat="ascii_ccsds_datetime_ymd1_with_ref"/></cd:Field>
        <cd:Field name="t_a7"><cd:Time format="ascii" timeformat="ascii_ccsds_datetime_ymd2"/></cd:Field>
        <cd:Field name="t_a8"><cd:Time format="ascii" timeformat="ascii_ccsds_datetime_ymd2_with_ref"/></cd:Field>
        <cd:Field name="t_a9"><cd:Time format="ascii" timeformat="ascii_ccsds_datetime_utc1"/></cd:Field>
        <cd:Field name="t_a10"><cd:Time format="ascii" timeformat="ascii_ccsds_datetime_utc2"/></cd:Field>
        <cd:Field name="t_b1"><cd:Time timeformat="binary_envisat_datetime"/></cd:Field>
        <cd:Field name="t_b2"><cd:Time timeformat="binary_gome_datetime"/></cd:Field>
        <cd:Field name="t_b3"><cd:Time timeformat="binary_eps_datetime"/></cd:Field>
        <cd:Field name="t_b4"><cd:Time timeformat="binary_eps_datetime_long"/></cd:Field>
        <cd:Field name="t_custom">
          <cd:Time format="ascii" timeformat="float(.)">
            <cd:Description>custom time</cd:Description>
            <cd:Text><cd:ByteSize>4</cd:ByteSize></cd:Text>
            <cd:Mapping string="BAD_" value="nan"/>
          </cd:Time>
        </cd:Field>
      </cd:Record>
    </cd:Field>
  </cd:Record>
  <cd:ProductVariable name="pvar">
    <cd:Dimension>2</cd:Dimension>
    <cd:Init>$pvar[0] = 42; $pvar[1] = 84</cd:Init>
  </cd:ProductVariable>
</cd:ProductDefinition>
)xml";

  constexpr std::string_view kOpenProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07"
                      id="OpenProd" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text><cd:ByteSize>4</cd:ByteSize></cd:Text>
    </cd:Field>
    <cd:Field name="value">
      <cd:NamedType id="SharedInt"/>
    </cd:Field>
  </cd:Record>
  <cd:ProductVariable name="v">
    <cd:Dimension>1</cd:Dimension>
    <cd:Init>$v[0] = 99</cd:Init>
  </cd:ProductVariable>
</cd:ProductDefinition>
)xml";

  const std::array<std::pair<std::string_view, std::string_view>, 3> entries = {
      {
          {"types/SharedInt.xml", kSharedIntXml},
          {"products/FullProd.xml", kFullProdXml},
          {"products/OpenProd.xml", kOpenProdXml},
      }};
  const TempFile codadef("coda_def_parse_full.codadef");
  ASSERT_TRUE(codadef.WriteBytes(BuildCodadefBytes(kIndexXml, entries)));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef.c_str()));

  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  // 1. Verify FullProd was eagerly parsed and populated.
  coda_product_definition* full_def = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_data_dictionary_get_definition(
                              "FullClass", "FullType", 1, &full_def));
  ASSERT_NE(nullptr, full_def);
  EXPECT_EQ(1, full_def->initialized);
  EXPECT_EQ(1, full_def->num_product_variables);
  ASSERT_NE(nullptr, full_def->root_type);

  coda_product_class* full_cls =
      coda_data_dictionary_get_product_class("FullClass");
  ASSERT_NE(nullptr, full_cls);
  EXPECT_EQ(1, coda_product_class_has_named_type(full_cls, "SharedInt"));

  // 2. Open a synthesized binary product matching OpenProd and read its fields
  //    and ProductVariable.
  const TempFile open_dat("coda_def_parse_open_prod.dat");
  constexpr auto kOpenBytes =
      std::to_array<uint8_t>({'O', 'P', 'E', 'N', 0x39, 0x05, 0x00, 0x00});
  ASSERT_TRUE(open_dat.WriteBytes(kOpenBytes));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(open_dat.c_str(), product.OutParam()));

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "value"));
  int32_t val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &val));
  EXPECT_EQ(1337, val);

  int64_t pvar = 0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_get_product_variable_value(product.get(), "v", 0, &pvar));
  EXPECT_EQ(99, pvar);
}

TEST(CodaDefinitionParseTest,
     XmlProductDefinitionAndLazyReadProductDefinition) {
  const ScopedDefinitionTestEnv test_env;
  ASSERT_EQ(0, coda_option_read_all_definitions);

  constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="XmlClass">
  <cd:ProductType name="XmlType">
    <cd:ProductDefinition id="XmlProd" format="xml" version="1">
      <cd:DetectionRule>
        <cd:MatchData path="/root-elem/elem-text">hello</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
    <cd:ProductDefinition id="MissingProd" format="binary" version="2">
      <cd:DetectionRule>
        <cd:MatchSize size="99"/>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)xml";

  constexpr std::string_view kXmlProd = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07"
                      id="XmlProd" format="xml">
  <cd:Record format="xml">
    <cd:Description/>
    <cd:Field name="root_elem" real_name="root-elem">
      <cd:Record>
        <cd:Attribute name="attr-one"/>
        <cd:Attribute name="attr_two" real_name="attr-two"/>
        <cd:Field name="elem_text">
          <cd:Text namexml="elem-text"/>
        </cd:Field>
        <cd:Field name="elem_rec">
          <cd:Optional/>
          <cd:Record namexml="elem-rec">
            <cd:Field name="item">
              <cd:Array>
                <cd:Dimension/>
                <cd:Dimension>   </cd:Dimension>
                <cd:Text/>
              </cd:Array>
            </cd:Field>
          </cd:Record>
        </cd:Field>
        <cd:Field name="elem_union">
          <cd:Optional/>
          <cd:Union namexml="elem-union">
            <cd:Field name="a"><cd:Text/></cd:Field>
          </cd:Union>
        </cd:Field>
        <cd:Field name="elem_type">
          <cd:Optional/>
          <cd:Type namexml="elem-type">
            <cd:Text/>
          </cd:Type>
        </cd:Field>
        <cd:Field name="elem_time">
          <cd:Optional/>
          <cd:Time timeformat="float(.)">
            <cd:Float><cd:NativeType>double</cd:NativeType></cd:Float>
            <cd:Attribute name="tz"/>
          </cd:Time>
        </cd:Field>
      </cd:Record>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  const std::array<std::pair<std::string_view, std::string_view>, 1> entries = {
      {
          {"products/XmlProd.xml", kXmlProd},
      }};
  const TempFile codadef("coda_def_parse_xml.codadef");
  ASSERT_TRUE(codadef.WriteBytes(BuildCodadefBytes(kIndexXml, entries)));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef.c_str()));

  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  // 1. Initially XmlProd is not yet parsed because read_all_definitions == 0.
  coda_product_definition* xml_def = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_data_dictionary_get_definition(
                              "XmlClass", "XmlType", 1, &xml_def));
  ASSERT_NE(nullptr, xml_def);
  EXPECT_EQ(0, xml_def->initialized);

  // 2. Call coda_read_product_definition() directly and verify root_type and
  //    namexml real names.
  ASSERT_EQ(CODA_SUCCESS, coda_read_product_definition(xml_def));
  EXPECT_EQ(1, xml_def->initialized);
  ASSERT_NE(nullptr, xml_def->root_type);

  // 3. Missing products/MissingProd.xml triggers ze_product missing error.
  coda_product_definition* missing_def = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_data_dictionary_get_definition(
                              "XmlClass", "XmlType", 2, &missing_def));
  ASSERT_NE(nullptr, missing_def);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_read_product_definition(missing_def));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("definition for product 'MissingProd' missing"));

  // 4. Open a matching XML file via coda_open() to exercise the parsed XML
  //    definition end-to-end.
  const TempFile xml_product("coda_def_parse_sample.xml");
  ASSERT_TRUE(
      xml_product.WriteText("<?xml version=\"1.0\"?>\n"
                            "<root-elem attr-one=\"v1\" attr-two=\"v2\">\n"
                            "  <elem-text>hello</elem-text>\n"
                            "</root-elem>\n"));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(xml_product.c_str(), product.OutParam()));

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "root_elem"));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "elem_text"));
  std::array<char, 16> text_buf{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, text_buf.data(),
                                    static_cast<long>(text_buf.size())));
  EXPECT_STREQ("hello", text_buf.data());
}

TEST(CodaDefinitionParseTest,
     ReadProductDefinitionFailsWhenCodadefFileRemovedAfterInit) {
  const ScopedDefinitionTestEnv test_env;
  TempFile codadef("coda_def_parse_removed.codadef");
  ASSERT_TRUE(
      codadef.WriteBytes(BuildCodadefBytes(MakeMinimalIndexXml("RemClass"))));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef.c_str()));

  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  coda_product_definition* def = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_data_dictionary_get_definition(
                              "RemClass", "TestType", 1, &def));
  ASSERT_NE(nullptr, def);

  std::error_code ec;
  ASSERT_TRUE(std::filesystem::remove(codadef.path(), ec));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_read_product_definition(def));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("could not read data from definition file"));
}

TEST(CodaDefinitionParseTest,
     PatchesOversizedCodadefEntryRejectedWithOutOfMemory) {
  // Triggers google3 patch patches/libcoda_coda-definition-parse.c.patch:
  // uncompressed_size > 10000000 returns CODA_ERROR_OUT_OF_MEMORY without
  // allocating 10 MB. ziparchive.c requires entry->uncompressed_size / 1032 <=
  // entry->compressed_size + 1, so we generate ~10.5 KB of incompressible
  // deflated data and patch the 32-bit uncompressed size in the ZIP headers.
  const ScopedDefinitionTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  std::vector<uint8_t> incompressible(10500);
  uint32_t state = 0x12345678U;
  for (uint8_t& b : incompressible) {
    state = state * 1664525U + 1013904223U;
    b = static_cast<uint8_t>((state >> 24) & 0xFF);
  }

  const std::array<ZipEntrySpec, 1> specs = {{
      {.name = "index.xml",
       .data = incompressible,
       .method = ZipMethod::kDeflated},
  }};
  std::vector<uint8_t> zip_bytes = BuildZipBytes(specs);

  constexpr uint32_t kClaimedSize = 10000001U;
  const std::optional<size_t> local_off = FindSignature(zip_bytes, 0x04034B50U);
  const std::optional<size_t> cd_off = FindSignature(zip_bytes, 0x02014B50U);
  ASSERT_TRUE(local_off.has_value());
  ASSERT_TRUE(cd_off.has_value());
  // Local file header: uncompressed_size at +22.
  WriteLe32At(zip_bytes, *local_off + 22, kClaimedSize);
  // Central directory file header: uncompressed_size at +24.
  WriteLe32At(zip_bytes, *cd_off + 24, kClaimedSize);

  const TempFile oversized("coda_def_parse_oversized.codadef");
  ASSERT_TRUE(oversized.WriteBytes(zip_bytes));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_read_definitions(oversized.c_str()));
  EXPECT_EQ(CODA_ERROR_OUT_OF_MEMORY, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("filesize unreasonably large"));
}

struct ParseErrorCase {
  std::string name;
  std::string index_xml;
  std::optional<std::string> type_name;
  std::optional<std::string> type_xml;
  std::optional<std::string> prod_name;
  std::optional<std::string> prod_xml;
  bool read_all_definitions = true;
  int expected_errno;
  std::string expected_msg;
};

std::string WrapProductXml(std::string_view body,
                           std::string_view format = "binary",
                           std::string_view id = "P") {
  return std::string(
             "<?xml version=\"1.0\"?>\n"
             "<cd:ProductDefinition xmlns:cd=\"") +
         std::string(kNs) + "\" id=\"" + std::string(id) + "\" format=\"" +
         std::string(format) + "\">\n" + std::string(body) +
         "\n</cd:ProductDefinition>\n";
}

std::string WrapFieldXml(std::string_view field_body,
                         std::string_view format = "binary") {
  return WrapProductXml(std::string("<cd:Record><cd:Field name=\"f\">") +
                            std::string(field_body) + "</cd:Field></cd:Record>",
                        format);
}

void RunParseErrorCases(std::span<const ParseErrorCase> cases) {
  for (const ParseErrorCase& tc : cases) {
    SCOPED_TRACE(tc.name);
    coda_option_read_all_definitions = tc.read_all_definitions ? 1 : 0;
    ScopedCodaSession coda;
    ASSERT_EQ(CODA_SUCCESS, coda.Init());

    std::vector<std::pair<std::string_view, std::string_view>> extra;
    std::string type_entry;
    if (tc.type_name.has_value() && tc.type_xml.has_value()) {
      type_entry = "types/" + std::string(*tc.type_name) + ".xml";
      extra.emplace_back(type_entry, *tc.type_xml);
    }
    std::string prod_entry;
    if (tc.prod_name.has_value() && tc.prod_xml.has_value()) {
      prod_entry = "products/" + std::string(*tc.prod_name) + ".xml";
      extra.emplace_back(prod_entry, *tc.prod_xml);
    }

    const TempFile bad_def("coda_def_parse_error_case.codadef");
    ASSERT_TRUE(bad_def.WriteBytes(BuildCodadefBytes(tc.index_xml, extra)));

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_read_definitions(bad_def.c_str()));
    EXPECT_EQ(tc.expected_errno, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr(tc.expected_msg));
  }
}

TEST(CodaDefinitionParseTest, RejectsInvalidIndexTypeAndProductDefinitions) {
  const ScopedDefinitionTestEnv test_env;
  coda_option_read_all_definitions = 1;

  const std::string kValidIndex = MakeMinimalIndexXml("ErrClass", "T", "P");
  const std::string kValidXmlIndex =
      MakeMinimalIndexXml("ErrClass", "T", "P", "xml");

  const std::vector<ParseErrorCase> kCases = {
      {.name = "malformed_xml",
       .index_xml = "<cd:ProductClass",
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "xml parse error"},
      {.name = "disallowed_root_in_index",
       .index_xml = "<?xml version=\"1.0\"?><cd:Record xmlns:cd=\"" +
                    std::string(kNs) + "\" format=\"binary\"/>",
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not allowed as root element"},
      {.name = "disallowed_root_in_types",
       .index_xml = kValidIndex,
       .type_name = "T1",
       .type_xml = "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
                   std::string(kNs) + "\" name=\"C\"/>",
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:NamedType id=\"T1\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not allowed as root element"},
      {.name = "disallowed_root_in_products",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = "<?xml version=\"1.0\"?><cd:Record xmlns:cd=\"" +
                   std::string(kNs) + "\" format=\"binary\"/>",
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not allowed as root element"},
      {.name = "disallowed_child_no_format",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:Integer format=\"binary\"/></cd:ProductClass>",
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not allowed within element 'ProductClass'"},
      {.name = "disallowed_child_with_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml("<cd:Record><cd:Init/></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not allowed within element 'Record'{binary}"},
      {.name = "non_whitespace_text_in_container",
       .index_xml = "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
                    std::string(kNs) +
                    "\" name=\"C\">badtext</cd:ProductClass>",
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "non-whitespace character data not allowed here"},
      {.name = "product_class_missing_name",
       .index_xml = "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
                    std::string(kNs) + "\"/>",
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'name' missing"},
      {.name = "product_class_invalid_name",
       .index_xml = "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
                    std::string(kNs) + "\" name=\"1bad\"/>",
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not a valid identifier"},
      {.name = "product_type_missing_name",
       .index_xml = "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
                    std::string(kNs) +
                    "\" name=\"C\"><cd:ProductType/></cd:ProductClass>",
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'name' missing"},
      {.name = "product_def_missing_id",
       .index_xml = "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
                    std::string(kNs) +
                    "\" name=\"C\"><cd:ProductType name=\"T\">"
                    "<cd:ProductDefinition format=\"binary\" version=\"1\"/>"
                    "</cd:ProductType></cd:ProductClass>",
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'id' missing"},
      {.name = "product_def_missing_format",
       .index_xml = "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
                    std::string(kNs) +
                    "\" name=\"C\"><cd:ProductType name=\"T\">"
                    "<cd:ProductDefinition id=\"P\" version=\"1\"/>"
                    "</cd:ProductType></cd:ProductClass>",
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'format' missing"},
      {.name = "product_def_bad_format",
       .index_xml = MakeMinimalIndexXml("C", "T", "P", "bogus", "1"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'format' attribute value"},
      {.name = "product_def_missing_version",
       .index_xml = "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
                    std::string(kNs) +
                    "\" name=\"C\"><cd:ProductType name=\"T\">"
                    "<cd:ProductDefinition id=\"P\" format=\"binary\"/>"
                    "</cd:ProductType></cd:ProductClass>",
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'version' missing"},
      {.name = "product_def_bad_version",
       .index_xml = MakeMinimalIndexXml("C", "T", "P", "binary", "abc"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'version' attribute value"},
      {.name = "product_def_missing_product_file",
       .index_xml = kValidIndex,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "definition for product 'P' missing"},
      {.name = "product_def_sub_missing_id",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = "<?xml version=\"1.0\"?><cd:ProductDefinition xmlns:cd=\"" +
                   std::string(kNs) + "\" format=\"binary\"/>",
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'id' missing"},
      {.name = "product_def_sub_wrong_id",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml("", "binary", "WrongId"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid id attribute value"},
      {.name = "product_def_sub_missing_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = "<?xml version=\"1.0\"?><cd:ProductDefinition xmlns:cd=\"" +
                   std::string(kNs) + "\" id=\"P\"/>",
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'format' missing"},
      {.name = "product_def_sub_bad_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml("", "bogus"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'format' attribute value"},
      {.name = "product_def_format_mismatch",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml("", "ascii"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "differs between index and product file"},
      {.name = "xml_root_wrong_format",
       .index_xml = kValidXmlIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml("<cd:Record format=\"binary\"/>", "xml"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "binary record not allowed for xml product definition"},
      {.name = "xml_root_forbidden_name",
       .index_xml = kValidXmlIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml("<cd:Record name=\"r\"/>", "xml"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "attribute 'name' not allowed for xml root record"},
      {.name = "xml_root_forbidden_namexml",
       .index_xml = kValidXmlIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml("<cd:Record namexml=\"r\"/>", "xml"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "attribute 'namexml' not allowed for xml root record"},
      {.name = "xml_field_namexml_with_real_name",
       .index_xml = kValidXmlIndex,
       .prod_name = "P",
       .prod_xml =
           WrapProductXml("<cd:Record><cd:Field name=\"f\" real_name=\"F\">"
                          "<cd:Text namexml=\"X\"/></cd:Field></cd:Record>",
                          "xml"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "attribute 'namexml' not allowed if 'real_name'"},
      {.name = "named_type_missing_id",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:NamedType/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'id' missing"},
      {.name = "named_type_missing_file",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:NamedType id=\"NoSuchType\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "definition for type 'NoSuchType' missing"},
      {.name = "named_type_missing_name_attr",
       .index_xml = kValidIndex,
       .type_name = "T1",
       .type_xml =
           "<?xml version=\"1.0\"?><cd:Integer xmlns:cd=\"" + std::string(kNs) +
           "\" format=\"binary\"><cd:BitSize>8</cd:BitSize></cd:Integer>",
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:NamedType id=\"T1\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'name' missing"},
      {.name = "named_type_wrong_name_attr",
       .index_xml = kValidIndex,
       .type_name = "T1",
       .type_xml = "<?xml version=\"1.0\"?><cd:Integer xmlns:cd=\"" +
                   std::string(kNs) +
                   "\" name=\"Other\" "
                   "format=\"binary\"><cd:BitSize>8</cd:BitSize></cd:Integer>",
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:NamedType id=\"T1\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "has incorrect 'name' attribute"},
      {.name = "named_type_invalid_identifier",
       .index_xml = kValidIndex,
       .type_name = "1bad",
       .type_xml = "<?xml version=\"1.0\"?><cd:Integer xmlns:cd=\"" +
                   std::string(kNs) +
                   "\" name=\"1bad\" "
                   "format=\"binary\"><cd:BitSize>8</cd:BitSize></cd:Integer>",
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:NamedType id=\"1bad\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not a valid identifier"},
      {.name = "named_type_empty_foreign_ns",
       .index_xml = kValidIndex,
       .type_name = "EmptyT",
       .type_xml =
           "<?xml version=\"1.0\"?><other:Root xmlns:other=\"http://x\"/>",
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:NamedType id=\"EmptyT\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "named type definition for 'EmptyT' is empty"},
      {.name = "named_type_missing_format_attr",
       .index_xml = kValidIndex,
       .type_name = "T1",
       .type_xml = "<?xml version=\"1.0\"?><cd:Integer xmlns:cd=\"" +
                   std::string(kNs) + "\" name=\"T1\"/>",
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:NamedType id=\"T1\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'format' missing"},
      {.name = "named_type_bad_format_attr",
       .index_xml = kValidIndex,
       .type_name = "T1",
       .type_xml = "<?xml version=\"1.0\"?><cd:Integer xmlns:cd=\"" +
                   std::string(kNs) + "\" name=\"T1\" format=\"bogus\"/>",
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:NamedType id=\"T1\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'format' attribute value"},
      {.name = "nested_type_forbidden_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Integer name=\"bad\"><cd:BitSize>8</cd:BitSize></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "may not have a 'name' attribute"},
      {.name = "nested_type_bad_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Integer format=\"bogus\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'format' attribute value"},
      {.name = "field_forbidden_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record><cd:Field name=\"f\" format=\"binary\"/></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "attribute 'format' not allowed for Field"},
      {.name = "field_forbidden_namexml",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record><cd:Field name=\"f\" namexml=\"x\"/></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "attribute 'namexml' not allowed for Field"},
      {.name = "field_missing_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml("<cd:Record><cd:Field/></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'name' missing"},
      {.name = "field_bit_offset_not_allowed_for_xml",
       .index_xml = kValidXmlIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record><cd:Field name=\"f\"><cd:Text/>"
           "<cd:BitOffset>8</cd:BitOffset></cd:Field></cd:Record>",
           "xml"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "bit offset expression not allowed"},
      {.name = "attribute_forbidden_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml("<cd:Record><cd:Attribute name=\"a\" "
                                  "format=\"binary\"/></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "attribute 'format' not allowed for Attribute"},
      {.name = "attribute_forbidden_namexml",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record><cd:Attribute name=\"a\" namexml=\"x\"/></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "attribute 'namexml' not allowed for Attribute"},
      {.name = "attribute_missing_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml("<cd:Record><cd:Attribute/></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'name' missing"},
      {.name = "attribute_fixed_value_bad_escape",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml("<cd:Record><cd:Attribute "
                                  "name=\"a\"><cd:FixedValue>\\z</"
                                  "cd:FixedValue></cd:Attribute></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid escape sequence"},
      {.name = "attribute_fixed_value_with_explicit_type",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record><cd:Attribute name=\"a\">"
           "<cd:Text><cd:ByteSize>2</cd:ByteSize></cd:Text>"
           "<cd:FixedValue>ab</cd:FixedValue></cd:Attribute></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "fixed value should be provided as part of type"},
      {.name = "type_forbidden_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Type name=\"t\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "attribute 'name' not allowed for Type"},
      {.name = "scale_factor_forbidden_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:VSFInteger><cd:ScaleFactor "
                                "format=\"binary\"/></cd:VSFInteger>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "attribute 'format' not allowed for ScaleFactor"},
      {.name = "scale_factor_forbidden_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:VSFInteger><cd:ScaleFactor name=\"s\"/></cd:VSFInteger>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "attribute 'name' not allowed for ScaleFactor"},
      {.name = "scale_factor_forbidden_namexml",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:VSFInteger><cd:ScaleFactor namexml=\"s\"/></cd:VSFInteger>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "attribute 'namexml' not allowed for ScaleFactor"},
      {.name = "scale_factor_missing_type",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapFieldXml("<cd:VSFInteger><cd:ScaleFactor/></cd:VSFInteger>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "missing type for ScaleFactor"},
      {.name = "vsf_integer_wrong_child_order",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapFieldXml("<cd:VSFInteger><cd:Integer><cd:BitSize>8</cd:BitSize>"
                        "</cd:Integer></cd:VSFInteger>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "value should be second field of a vsf integer record"},
      {.name = "vsf_integer_duplicate_scale_factor",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:VSFInteger>"
           "<cd:ScaleFactor><cd:Integer><cd:BitSize>8</cd:BitSize>"
           "<cd:NativeType>int8</cd:NativeType></cd:Integer></cd:ScaleFactor>"
           "<cd:ScaleFactor><cd:Integer><cd:BitSize>8</cd:BitSize>"
           "<cd:NativeType>int8</cd:NativeType></cd:Integer></cd:ScaleFactor>"
           "<cd:Integer><cd:BitSize>16</cd:BitSize>"
           "<cd:NativeType>int16</cd:NativeType></cd:Integer>"
           "</cd:VSFInteger>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "scale factor should be first field"},
      {.name = "conversion_zero_denominator",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Integer><cd:BitSize>8</cd:BitSize>"
           "<cd:Conversion numerator=\"1\" denominator=\"0\"/></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "denominator may not be 0"},
      {.name = "conversion_missing_numerator",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapFieldXml("<cd:Integer><cd:BitSize>8</cd:BitSize>"
                        "<cd:Conversion denominator=\"1\"/></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'numerator' missing"},
      {.name = "conversion_missing_denominator",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapFieldXml("<cd:Integer><cd:BitSize>8</cd:BitSize>"
                        "<cd:Conversion numerator=\"1\"/></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'denominator' missing"},
      {.name = "conversion_bad_numerator",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Integer><cd:BitSize>8</cd:BitSize>"
           "<cd:Conversion numerator=\"abc\" denominator=\"1\"/></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid value 'abc' for 'numerator'"},
      {.name = "conversion_bad_denominator",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Integer><cd:BitSize>8</cd:BitSize>"
           "<cd:Conversion numerator=\"1\" denominator=\"abc\"/></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid value 'abc' for 'denominator'"},
      {.name = "conversion_bad_offset",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapFieldXml("<cd:Integer><cd:BitSize>8</cd:BitSize>"
                        "<cd:Conversion numerator=\"1\" denominator=\"1\" "
                        "offset=\"bad\"/></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid value 'bad' for 'offset'"},
      {.name = "conversion_bad_invalid",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapFieldXml("<cd:Integer><cd:BitSize>8</cd:BitSize>"
                        "<cd:Conversion numerator=\"1\" denominator=\"1\" "
                        "invalid=\"bad\"/></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid value 'bad' for 'invalid'"},
      {.name = "native_type_empty",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Integer><cd:BitSize>8</"
                                "cd:BitSize><cd:NativeType/></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid native type"},
      {.name = "native_type_bogus",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>bogus</"
           "cd:NativeType></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid native type"},
      {.name = "native_type_bytes_rejected_on_text",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Text><cd:ByteSize>4</cd:ByteSize><cd:NativeType>bytes</"
           "cd:NativeType></cd:Text>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid read type (bytes)"},
      {.name = "mapping_missing_string",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Integer format=\"ascii\"><cd:Mapping "
                                "value=\"1\"/></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'string' missing"},
      {.name = "mapping_missing_value",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Integer format=\"ascii\"><cd:Mapping "
                                "string=\"x\"/></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'value' missing"},
      {.name = "mapping_bad_integer_value",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Integer format=\"ascii\"><cd:Mapping "
                                "string=\"x\" value=\"abc\"/></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'value' attribute integer value"},
      {.name = "mapping_bad_float_value",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Float format=\"ascii\"><cd:Mapping "
                                "string=\"x\" value=\"abc\"/></cd:Float>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'value' attribute float value"},
      {.name = "text_fixed_value_bad_escape",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Text><cd:ByteSize>2</cd:ByteSize><cd:FixedValue>\\z</"
           "cd:FixedValue></cd:Text>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid escape sequence"},
      {.name = "raw_fixed_value_bad_escape",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Raw><cd:BitSize>16</cd:BitSize><cd:FixedValue>\\z</"
           "cd:FixedValue></cd:Raw>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid escape sequence"},
      {.name = "time_missing_timeformat",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Time/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'timeformat' missing"},
      {.name = "time_bad_expr",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Time timeformat=\"(\"/>"),
       .expected_errno = CODA_ERROR_EXPRESSION,
       .expected_msg = "syntax error"},
      {.name = "time_non_float_expr",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Time timeformat=\"1\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "time expression is not a float expression"},
      {.name = "bool_expr_empty",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record><cd:Field "
           "name=\"f\"><cd:Integer><cd:BitSize>8</cd:BitSize></cd:Integer>"
           "<cd:Available>   </cd:Available></cd:Field></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "empty boolean expression"},
      {.name = "bool_expr_bad_syntax",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record><cd:Field "
           "name=\"f\"><cd:Integer><cd:BitSize>8</cd:BitSize></cd:Integer>"
           "<cd:Available>(</cd:Available></cd:Field></cd:Record>"),
       .expected_errno = CODA_ERROR_EXPRESSION,
       .expected_msg = "syntax error"},
      {.name = "bool_expr_wrong_type",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record><cd:Field "
           "name=\"f\"><cd:Integer><cd:BitSize>8</cd:BitSize></cd:Integer>"
           "<cd:Available>1</cd:Available></cd:Field></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not a boolean expression"},
      {.name = "int_expr_empty",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapProductXml("<cd:Record><cd:BitSize>  </cd:BitSize></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "empty integer expression"},
      {.name = "int_expr_bad_syntax",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapProductXml("<cd:Record><cd:BitSize>(</cd:BitSize></cd:Record>"),
       .expected_errno = CODA_ERROR_EXPRESSION,
       .expected_msg = "syntax error"},
      {.name = "int_expr_wrong_type",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record><cd:BitSize>true</cd:BitSize></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not an integer expression"},
      {.name = "int_const_or_expr_empty",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapFieldXml("<cd:Integer><cd:BitSize>  </cd:BitSize></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "empty integer expression"},
      {.name = "int_const_or_expr_bad_syntax",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapFieldXml("<cd:Integer><cd:BitSize>(</cd:BitSize></cd:Integer>"),
       .expected_errno = CODA_ERROR_EXPRESSION,
       .expected_msg = "syntax error"},
      {.name = "int_const_or_expr_wrong_type",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Integer><cd:BitSize>true</cd:BitSize></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not an integer expression"},
      {.name = "opt_int_expr_bad_syntax",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Array><cd:Dimension>(</cd:Dimension><cd:Integer><cd:BitSize>8</"
           "cd:BitSize></cd:Integer></cd:Array>"),
       .expected_errno = CODA_ERROR_EXPRESSION,
       .expected_msg = "syntax error"},
      {.name = "opt_int_expr_wrong_type",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Array><cd:Dimension>true</"
                                "cd:Dimension><cd:Integer><cd:BitSize>8</"
                                "cd:BitSize></cd:Integer></cd:Array>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not an integer expression"},
      {.name = "product_variable_missing_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml("<cd:Record/><cd:ProductVariable/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'name' missing"},
      {.name = "void_expr_empty",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record/><cd:ProductVariable name=\"v\"><cd:Init>  "
           "</cd:Init></cd:ProductVariable>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "empty void expression"},
      {.name = "void_expr_bad_syntax",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record/><cd:ProductVariable "
           "name=\"v\"><cd:Init>(</cd:Init></cd:ProductVariable>"),
       .expected_errno = CODA_ERROR_EXPRESSION,
       .expected_msg = "syntax error"},
      {.name = "void_expr_wrong_type",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record/><cd:ProductVariable "
           "name=\"v\"><cd:Init>1</cd:Init></cd:ProductVariable>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not a void expression"},
      {.name = "match_data_both_path_and_offset",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"xml\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchData path=\"/r\" "
           "offset=\"0\">x</cd:MatchData>"
           "</cd:DetectionRule></cd:ProductDefinition></cd:ProductType></"
           "cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "both 'path' and 'offset'"},
      {.name = "match_data_invalid_offset",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchData offset=\"bad\">x</cd:MatchData>"
           "</cd:DetectionRule></cd:ProductDefinition></cd:ProductType></"
           "cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'offset' attribute value"},
      {.name = "match_data_empty_with_offset",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchData offset=\"0\"/></cd:DetectionRule>"
           "</cd:ProductDefinition></cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "empty string not allowed for data match value"},
      {.name = "match_data_empty_no_offset_no_path",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchData/></cd:DetectionRule>"
           "</cd:ProductDefinition></cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "empty string not allowed for data match value"},
      {.name = "match_data_bad_escape",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchData "
           "offset=\"0\">\\z</cd:MatchData></cd:DetectionRule>"
           "</cd:ProductDefinition></cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid escape sequence"},
      // Note: add_detection_rule_entry_for_path() in coda-definition-parse.c
      // dereferences rule->product_definition->name on unclosed "{ns" or
      // non-trailing "@attr" paths before
      // coda_product_definition_add_detection_rule() assigns product_definition
      // (leaving it NULL and trapping/crashing).
      {.name = "detection_rule_empty",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule/>"
           "</cd:ProductDefinition></cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "should have at least one entry"},
      {.name = "match_expression_invalid_path",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchExpression "
           "path=\"1\">true</cd:MatchExpression></cd:DetectionRule>"
           "</cd:ProductDefinition></cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not a valid path for detection rule"},
      {.name = "match_expression_empty",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchExpression>  "
           "</cd:MatchExpression></cd:DetectionRule>"
           "</cd:ProductDefinition></cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "empty detection expression"},
      {.name = "match_expression_bad_syntax",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchExpression>(</cd:MatchExpression></"
           "cd:DetectionRule>"
           "</cd:ProductDefinition></cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_EXPRESSION,
       .expected_msg = "syntax error"},
      {.name = "match_expression_wrong_type",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchExpression>1</cd:MatchExpression></"
           "cd:DetectionRule>"
           "</cd:ProductDefinition></cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not a boolean expression"},
      {.name = "match_filename_missing_offset",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchFilename>a</cd:MatchFilename></"
           "cd:DetectionRule>"
           "</cd:ProductDefinition></cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'offset' missing"},
      {.name = "match_filename_bad_offset",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchFilename "
           "offset=\"bad\">a</cd:MatchFilename></cd:DetectionRule>"
           "</cd:ProductDefinition></cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'offset' attribute value"},
      {.name = "match_filename_empty",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchFilename "
           "offset=\"0\"/></cd:DetectionRule>"
           "</cd:ProductDefinition></cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "empty string not allowed for filename match value"},
      {.name = "match_filename_bad_escape",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchFilename "
           "offset=\"0\">\\z</cd:MatchFilename></cd:DetectionRule>"
           "</cd:ProductDefinition></cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid escape sequence"},
      {.name = "match_size_missing_size",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchSize/></cd:DetectionRule>"
           "</cd:ProductDefinition></cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "mandatory attribute 'size' missing"},
      {.name = "match_size_bad_size",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P\" format=\"binary\" version=\"1\">"
           "<cd:DetectionRule><cd:MatchSize size=\"bad\"/></cd:DetectionRule>"
           "</cd:ProductDefinition></cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'size' attribute value"},
  };

  ASSERT_NO_FATAL_FAILURE(RunParseErrorCases(kCases));
}

TEST(CodaDefinitionParseTest,
     EmptyProductClassDescriptionAndForeignTextInsideDescription) {
  const ScopedDefinitionTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  // 1. Self-closing <cd:Description/> under ProductClass sets description to
  // "".
  constexpr std::string_view kEmptyDescXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07"
                 name="EmptyClassDesc">
  <cd:Description/>
</cd:ProductClass>
)xml";
  const TempFile empty_def("coda_def_parse_empty_cls_desc.codadef");
  ASSERT_TRUE(empty_def.WriteBytes(BuildCodadefBytes(kEmptyDescXml)));
  ASSERT_EQ(CODA_SUCCESS, coda_read_definitions(empty_def.c_str()));

  coda_product_class* empty_cls =
      coda_data_dictionary_get_product_class("EmptyClassDesc");
  ASSERT_NE(nullptr, empty_cls);
  EXPECT_STREQ("", empty_cls->description);

  // 2. Foreign-namespace element nested inside <cd:Description> triggers
  //    string_handler() with info->unparsed_depth > 0 and ignores inner text.
  constexpr std::string_view kForeignInDescXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07"
                 xmlns:ext="http://example.com/custom"
                 name="ForeignInDescClass">
  <cd:Description><ext:Note>ignored text</ext:Note>kept</cd:Description>
</cd:ProductClass>
)xml";
  const TempFile foreign_def("coda_def_parse_foreign_in_desc.codadef");
  ASSERT_TRUE(foreign_def.WriteBytes(BuildCodadefBytes(kForeignInDescXml)));
  ASSERT_EQ(CODA_SUCCESS, coda_read_definitions(foreign_def.c_str()));

  coda_product_class* foreign_cls =
      coda_data_dictionary_get_product_class("ForeignInDescClass");
  ASSERT_NE(nullptr, foreign_cls);
  EXPECT_STREQ("kept", foreign_cls->description);
}

TEST(CodaDefinitionParseTest, CorruptVersionEntryFailsProductClassInit) {
  const ScopedDefinitionTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  // Place VERSION first so its local file header is at offset 0. Flipping
  // byte 2 of its PK\x03\x04 signature keeps the leading "PK" magic intact for
  // za_open() and keeps index.xml intact, then causes za_read_entry("VERSION")
  // inside get_product_class_revision() to fail.
  const std::string index_xml = MakeMinimalIndexXml(
      "CorruptVerDataClass", "TestType", "TestProd", "binary", "1", 4);
  constexpr std::string_view kVersionText = "1";
  const std::array<ZipEntrySpec, 2> specs = {{
      {.name = "VERSION",
       .data = std::span<const uint8_t>(
           reinterpret_cast<const uint8_t*>(kVersionText.data()),
           kVersionText.size()),
       .method = ZipMethod::kDeflated},
      {.name = "index.xml",
       .data = std::span<const uint8_t>(
           reinterpret_cast<const uint8_t*>(index_xml.data()),
           index_xml.size()),
       .method = ZipMethod::kDeflated},
  }};
  std::vector<uint8_t> zip_bytes = BuildZipBytes(specs);
  ASSERT_GE(zip_bytes.size(), 4u);
  zip_bytes[2] ^= 0x01;

  const TempFile bad_ver("coda_def_parse_corrupt_ver_entry.codadef");
  ASSERT_TRUE(bad_ver.WriteBytes(zip_bytes));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_read_definitions(bad_ver.c_str()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("could not read data from definition file"));
}

TEST(CodaDefinitionParseTest, UnreadableDirectoryFailsReadDefinitions) {
  if (geteuid() == 0) {
    GTEST_SKIP() << "root bypasses directory mode bits for opendir()";
  }
  const ScopedDefinitionTestEnv test_env;
  ScopedCodaSession coda;
  ASSERT_EQ(CODA_SUCCESS, coda.Init());

  const TempDir dir("coda_def_parse_unreadable_dir");
  ASSERT_EQ(0, chmod(dir.c_str(), 0));
  const absl::Cleanup restore_perms = [&dir]() { chmod(dir.c_str(), 0755); };

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_read_definitions(dir.c_str()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("could not access directory"));
}

TEST(CodaDefinitionParseTest,
     RejectsAdditionalIndexTypeAndProductValidationErrors) {
  const ScopedDefinitionTestEnv test_env;
  const std::string kValidIndex = MakeMinimalIndexXml("ErrClass", "T", "P");
  const std::string kValidXmlIndex =
      MakeMinimalIndexXml("ErrClass", "T", "P", "xml");

  const std::vector<ParseErrorCase> kExtraCases = {
      {.name = "duplicate_product_type",
       .index_xml = "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
                    std::string(kNs) +
                    "\" name=\"C\"><cd:ProductType name=\"T\"/>"
                    "<cd:ProductType name=\"T\"/></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "duplicate product type T"},
      {.name = "product_def_invalid_id",
       .index_xml = MakeMinimalIndexXml("C", "T", "1bad", "binary", "1"),
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not a valid identifier"},
      {.name = "product_type_duplicate_definition_version",
       .index_xml =
           "<?xml version=\"1.0\"?><cd:ProductClass xmlns:cd=\"" +
           std::string(kNs) +
           "\" name=\"C\"><cd:ProductType name=\"T\">"
           "<cd:ProductDefinition id=\"P1\" format=\"binary\" version=\"1\"/>"
           "<cd:ProductDefinition id=\"P2\" format=\"binary\" version=\"1\"/>"
           "</cd:ProductType></cd:ProductClass>",
       .read_all_definitions = false,
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "multiple product definitions with version number 1"},
      {.name = "product_def_duplicate_root_type",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml("<cd:Record/><cd:Record/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "already has a root type"},
      {.name = "product_def_duplicate_product_variable",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapProductXml("<cd:Record/>"
                          "<cd:ProductVariable name=\"v\"><cd:Init>$v[0] = "
                          "1</cd:Init></cd:ProductVariable>"
                          "<cd:ProductVariable name=\"v\"><cd:Init>$v[0] = "
                          "2</cd:Init></cd:ProductVariable>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "duplicate product variable v"},
      {.name = "product_variable_invalid_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapProductXml("<cd:Record/><cd:ProductVariable name=\"1bad\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not a valid identifier"},
      {.name = "product_variable_duplicate_dimension",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record/><cd:ProductVariable name=\"v\">"
           "<cd:Dimension>1</cd:Dimension><cd:Dimension>2</cd:Dimension>"
           "</cd:ProductVariable>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "already has a size expression"},
      {.name = "product_variable_duplicate_init",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record/><cd:ProductVariable name=\"v\">"
           "<cd:Init>$v[0] = 1</cd:Init><cd:Init>$v[0] = 2</cd:Init>"
           "</cd:ProductVariable>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "already has an init expression"},
      {.name = "field_invalid_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapProductXml("<cd:Record><cd:Field name=\"1bad\"/></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "not a valid identifier"},
      {.name = "field_duplicate_available",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record><cd:Field name=\"f\">"
           "<cd:Available>true</cd:Available><cd:Available>false</cd:Available>"
           "</cd:Field></cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "already has an available expression"},
      {.name = "record_duplicate_field",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record>"
           "<cd:Field "
           "name=\"a\"><cd:Raw><cd:BitSize>8</cd:BitSize></cd:Raw></cd:Field>"
           "<cd:Field "
           "name=\"a\"><cd:Raw><cd:BitSize>8</cd:BitSize></cd:Raw></cd:Field>"
           "</cd:Record>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "duplicate field with name a"},
      {.name = "record_bad_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Record format=\"bogus\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'format' attribute value"},
      {.name = "record_forbidden_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Record name=\"bad\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "may not have a 'name' attribute"},
      {.name = "record_namexml_with_real_name",
       .index_xml = kValidXmlIndex,
       .prod_name = "P",
       .prod_xml =
           WrapProductXml("<cd:Record><cd:Field name=\"f\" real_name=\"F\">"
                          "<cd:Record namexml=\"X\"/></cd:Field></cd:Record>",
                          "xml"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "attribute 'namexml' not allowed if 'real_name'"},
      {.name = "float_bad_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Float format=\"bogus\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'format' attribute value"},
      {.name = "float_forbidden_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Float name=\"bad\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "may not have a 'name' attribute"},
      {.name = "float_duplicate_conversion",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Float><cd:BitSize>32</cd:BitSize><cd:NativeType>float</"
           "cd:NativeType>"
           "<cd:Conversion numerator=\"1\" denominator=\"1\"/>"
           "<cd:Conversion numerator=\"2\" denominator=\"1\"/></cd:Float>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "already has a conversion"},
      {.name = "integer_duplicate_conversion",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Integer><cd:BitSize>8</cd:BitSize>"
           "<cd:Conversion numerator=\"1\" denominator=\"1\"/>"
           "<cd:Conversion numerator=\"2\" denominator=\"1\"/></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "already has a conversion"},
      {.name = "mapping_integer_string_too_large",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapFieldXml("<cd:Integer format=\"ascii\"><cd:Mapping string=\"" +
                        std::string(65, 'x') + "\" value=\"1\"/></cd:Integer>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "string too large"},
      {.name = "mapping_float_string_too_large",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapFieldXml("<cd:Float format=\"ascii\"><cd:Mapping string=\"" +
                        std::string(65, 'x') + "\" value=\"1.0\"/></cd:Float>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "string too large"},
      {.name = "raw_bad_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Raw format=\"bogus\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'format' attribute value"},
      {.name = "raw_forbidden_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Raw name=\"bad\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "may not have a 'name' attribute"},
      {.name = "raw_duplicate_fixed_value",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapFieldXml("<cd:Raw><cd:BitSize>16</cd:BitSize>"
                        "<cd:FixedValue>ab</cd:FixedValue><cd:FixedValue>cd</"
                        "cd:FixedValue></cd:Raw>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "already has a fixed value"},
      {.name = "text_bad_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Text format=\"bogus\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'format' attribute value"},
      {.name = "text_forbidden_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Text name=\"bad\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "may not have a 'name' attribute"},
      {.name = "time_bad_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapFieldXml("<cd:Time format=\"bogus\" timeformat=\"float(.)\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'format' attribute value"},
      {.name = "time_forbidden_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml =
           WrapFieldXml("<cd:Time name=\"bad\" timeformat=\"float(.)\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "may not have a 'name' attribute"},
      {.name = "time_mapping_before_base_type",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Time format=\"ascii\" timeformat=\"float(.)\">"
           "<cd:Mapping string=\"NA\" value=\"nan\"/>"
           "<cd:Text><cd:ByteSize>2</cd:ByteSize></cd:Text></cd:Time>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "does not have a base type"},
      {.name = "type_bad_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Type format=\"bogus\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'format' attribute value"},
      {.name = "type_namexml_with_real_name",
       .index_xml = kValidXmlIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record><cd:Field name=\"f\" real_name=\"F\">"
           "<cd:Type namexml=\"X\"><cd:Text/></cd:Type></cd:Field></cd:Record>",
           "xml"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "attribute 'namexml' not allowed if 'real_name'"},
      {.name = "union_bad_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Union format=\"bogus\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'format' attribute value"},
      {.name = "union_forbidden_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:Union name=\"bad\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "may not have a 'name' attribute"},
      {.name = "union_namexml_with_real_name",
       .index_xml = kValidXmlIndex,
       .prod_name = "P",
       .prod_xml = WrapProductXml(
           "<cd:Record><cd:Field name=\"f\" real_name=\"F\">"
           "<cd:Union namexml=\"X\"><cd:Field name=\"a\"><cd:Text/></cd:Field>"
           "</cd:Union></cd:Field></cd:Record>",
           "xml"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "attribute 'namexml' not allowed if 'real_name'"},
      {.name = "union_duplicate_field_expression",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Union><cd:FieldExpression>0</cd:FieldExpression>"
           "<cd:FieldExpression>1</cd:FieldExpression>"
           "<cd:Field "
           "name=\"a\"><cd:Raw><cd:BitSize>8</cd:BitSize></cd:Raw></cd:Field>"
           "</cd:Union>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "already has a union field expression"},
      {.name = "union_duplicate_field",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml(
           "<cd:Union><cd:FieldExpression>0</cd:FieldExpression>"
           "<cd:Field "
           "name=\"a\"><cd:Raw><cd:BitSize>8</cd:BitSize></cd:Raw></cd:Field>"
           "<cd:Field "
           "name=\"a\"><cd:Raw><cd:BitSize>8</cd:BitSize></cd:Raw></cd:Field>"
           "</cd:Union>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "duplicate field with name a"},
      {.name = "vsf_integer_bad_format",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:VSFInteger format=\"bogus\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "invalid 'format' attribute value"},
      {.name = "vsf_integer_forbidden_name",
       .index_xml = kValidIndex,
       .prod_name = "P",
       .prod_xml = WrapFieldXml("<cd:VSFInteger name=\"bad\"/>"),
       .expected_errno = CODA_ERROR_DATA_DEFINITION,
       .expected_msg = "may not have a 'name' attribute"},
  };

  ASSERT_NO_FATAL_FAILURE(RunParseErrorCases(kExtraCases));
}

// TODO(schwehr): 90.3% line coverage in libcoda/coda-definition-parse.c
// (2311/2559); remaining missed lines are malloc/strdup OOM branches and Expat
// parser creation/allocation error paths.

}  // namespace
}  // namespace stcorp_coda
