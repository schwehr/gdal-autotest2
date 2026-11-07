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
// Tests for Kerchunk JSON references virtual file system
// (vsikerchunk_json_ref.cpp).
//
// Caveats:
//   The GDAL Arrow PARQUET driver is not compiled into //third_party/gdal in
//   google3. This test exercises Parquet conversion and cached delegation
//   using the ScopedFakeParquetDriver in-memory test double from
//   zarr_test_util.h.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/vsikerchunk_json_ref.cpp

#include <array>
#include <cstdint>
#include <format>  // NOLINT
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/zarr/vsikerchunk.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class VsikerchunkJsonRefTest : public ::testing::Test {
 protected:
  void SetUp() override { VSIInstallKerchunkFileSystems(); }
  void TearDown() override {
    VSIClearPathSpecificOptions(nullptr);
    VSIKerchunkFileSystemsCleanCache();
  }
};

TEST_F(VsikerchunkJsonRefTest, StreamingParserValidRefs) {
  ScopedTestDir test_dir("kc_json_stream_ok");
  const std::string target_file = test_dir.File("data.bin");
  WriteVsiFile(target_file, "HELLO_WORLD_ZARR");

  // Base64 of "INLINE_BASE64_DATA" is "SU5MSU5FX0JBU0U2NF9EQVRB"
  const std::string json_content = std::format(
      R"json({{
        "version": 1,
        "refs": {{
          ".zgroup": {{"zarr_format": 2}},
          ".zattrs": {{"title": "Test", "flag": true, "count": 42, "empty": null, "nested": {{"a": 1}}, "arr": [1, "x", true]}},
          "plain_str": "hello string",
          "b64_str": "base64:SU5MSU5FX0JBU0U2NF9EQVRB",
          "arr/0": ["{}", 0, 5],
          "arr/1": ["{}"]
        }}
      }})json",
      target_file, target_file);

  const std::string json_file = test_dir.File("stream_valid.json");
  WriteVsiFile(json_file, json_content);

  // Force streaming parser
  VSISetPathSpecificOption(json_file.c_str(),
                           "VSIKERCHUNK_USE_STREAMING_PARSER", "YES");

  const std::string base_vsi =
      std::format("/vsikerchunk_json_ref/{{{}}}", json_file);

  // 1. Read inline string
  EXPECT_EQ("hello string", ReadVsiFile(base_vsi + "/plain_str"));

  // 2. Read base64 string
  EXPECT_EQ("INLINE_BASE64_DATA", ReadVsiFile(base_vsi + "/b64_str"));

  // 3. Read inline JSON object
  std::string zattrs_str = ReadVsiFile(base_vsi + "/.zattrs");
  EXPECT_THAT(zattrs_str, HasSubstr("\"title\":\"Test\""));
  EXPECT_THAT(zattrs_str, HasSubstr("[1,\"x\",true]"));

  // 4. Read range reference [uri, 0, 5] -> "HELLO"
  EXPECT_EQ("HELLO", ReadVsiFile(base_vsi + "/arr/0"));
  VSIStatBufL sStatRange;
  EXPECT_EQ(0, VSIStatL((base_vsi + "/arr/0").c_str(), &sStatRange));
  EXPECT_EQ(5, sStatRange.st_size);

  // 5. Read whole file reference [uri] -> "HELLO_WORLD_ZARR"
  EXPECT_EQ("HELLO_WORLD_ZARR", ReadVsiFile(base_vsi + "/arr/1"));
  VSIStatBufL sStatWhole;
  EXPECT_EQ(0, VSIStatL((base_vsi + "/arr/1").c_str(), &sStatWhole));
  EXPECT_EQ(16, sStatWhole.st_size);

  // ReadDirEx on streaming root
  const CPLStringList stream_root_files(VSIReadDirEx(base_vsi.c_str(), 0));
  ASSERT_NE(nullptr, stream_root_files.List());
  EXPECT_TRUE(stream_root_files.FindString("plain_str") >= 0);

  // 6. Streaming parser on Version 0 JSON (first key is .zgroup)
  const std::string v0_stream_json = R"json({
    ".zgroup": {"zarr_format": 2},
    "v0_str": "v0_stream_val"
  })json";
  const std::string v0_stream_file = test_dir.File("stream_v0.json");
  WriteVsiFile(v0_stream_file, v0_stream_json);
  VSISetPathSpecificOption(v0_stream_file.c_str(),
                           "VSIKERCHUNK_USE_STREAMING_PARSER", "YES");
  EXPECT_EQ("v0_stream_val",
            ReadVsiFile(std::format("/vsikerchunk_json_ref/{{{}}}/v0_str",
                                    v0_stream_file)));

  // 7. Test on gzipped file
  const std::string gz_path = test_dir.File("stream_valid.json.gz");
  {
    std::unique_ptr<VSILFILE, decltype(&VSIFCloseL)> fp_gz(
        VSIFOpenL(std::format("/vsigzip/{}", gz_path).c_str(), "wb"),
        VSIFCloseL);
    ASSERT_NE(nullptr, fp_gz);
    VSIFWriteL(json_content.data(), 1, json_content.size(), fp_gz.get());
    fp_gz.reset();
  }
  const std::string vsigzip_json = std::format("/vsigzip/{}", gz_path);
  VSISetPathSpecificOption(vsigzip_json.c_str(),
                           "VSIKERCHUNK_USE_STREAMING_PARSER", "YES");
  const std::string gz_vsi =
      std::format("/vsikerchunk_json_ref/{{{}}}", vsigzip_json);
  EXPECT_EQ("hello string", ReadVsiFile(gz_vsi + "/plain_str"));
}

TEST_F(VsikerchunkJsonRefTest, StreamingParserErrorBranches) {
  ScopedTestDir test_dir("kc_json_stream_err");

  auto ExpectStreamError = [&](std::string_view case_name,
                               std::string_view json_str,
                               std::string_view expected_err) {
    const std::string path = test_dir.File(std::format("{}.json", case_name));
    ASSERT_TRUE(WriteVsiFile(path, json_str));
    VSISetPathSpecificOption(path.c_str(), "VSIKERCHUNK_USE_STREAMING_PARSER",
                             "YES");

    const std::string vsi_path =
        std::format("/vsikerchunk_json_ref/{{{}}}/any_key", path);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStat;
    EXPECT_EQ(-1, VSIStatL(vsi_path.c_str(), &sStat))
        << "Failed for case: " << case_name;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType())
        << "Failed for case: " << case_name;
    if (!expected_err.empty()) {
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(expected_err))
          << "Failed for case: " << case_name;
    }
  };

  struct StreamErrorCase {
    std::string_view name;
    std::string_view json;
    std::string_view expected_err;
  };
  static constexpr auto kStreamErrors = std::to_array<StreamErrorCase>({
      {.name = "templates",
       .json = R"json({"version": 1, "templates": {}, "refs": {}})json",
       .expected_err = "'templates' key found"},
      {.name = "gen",
       .json = R"json({"version": 1, "gen": [], "refs": {}})json",
       .expected_err = "'gen' key found"},
      {.name = "bad_b64",
       .json = R"json({"version": 1, "refs": {"k": "base64:!!!"}})json",
       .expected_err = "Base64 decoding failed"},
      {.name = "empty_arr",
       .json = R"json({"version": 1, "refs": {"k": []}})json",
       .expected_err = "not of size 1 or 3"},
      {.name = "four_arr",
       .json = R"json({"version": 1, "refs": {"k": ["u", 0, 1, 2]}})json",
       .expected_err = "Unexpected content at position 3 of array"},
      {.name = "arr_idx0_not_str",
       .json = R"json({"version": 1, "refs": {"k": [123, 0, 1]}})json",
       .expected_err = "Unexpected content at position 0 of array"},
      {.name = "arr_idx1_not_num",
       .json = R"json({"version": 1, "refs": {"k": ["u", "bad", 1]}})json",
       .expected_err = "Unexpected content at position 1 of array"},
      {.name = "arr_elem_obj",
       .json = R"json({"version": 1, "refs": {"k": [{}]}})json",
       .expected_err = "Unexpected content at position 0 of array"},
      {.name = "arr_elem_arr",
       .json = R"json({"version": 1, "refs": {"k": [[]]}})json",
       .expected_err = "Unexpected content at position 0 of array"},
      {.name = "arr_idx1_neg",
       .json = R"json({"version": 1, "refs": {"k": ["u", -5, 1]}})json",
       .expected_err = "not an unsigned 64 bit integer"},
      {.name = "arr_idx1_float",
       .json = R"json({"version": 1, "refs": {"k": ["u", 1.5, 1]}})json",
       .expected_err = "not an unsigned 64 bit integer"},
      {.name = "arr_idx2_neg",
       .json = R"json({"version": 1, "refs": {"k": ["u", 0, -1]}})json",
       .expected_err = "not an unsigned 32 bit integer"},
      {.name = "arr_idx2_overflow",
       .json =
           R"json({"version": 1, "refs": {"k": ["u", 0, 999999999999]}})json",
       .expected_err = "not an unsigned 32 bit integer"},
      {.name = "number_val",
       .json = R"json({"version": 1, "refs": {"k": 123}})json",
       .expected_err = "Unexpected content"},
      {.name = "bool_val",
       .json = R"json({"version": 1, "refs": {"k": true}})json",
       .expected_err = "Unexpected content"},
      {.name = "null_val",
       .json = R"json({"version": 1, "refs": {"k": null}})json",
       .expected_err = "Unexpected content"},
      {.name = "syntax_err", .json = "{corrupt json", .expected_err = ""},
      {.name = "empty_file", .json = "", .expected_err = "Load json file"},
  });

  for (const auto& tc : kStreamErrors) {
    SCOPED_TRACE(tc.name);
    ExpectStreamError(tc.name, tc.json, tc.expected_err);
  }

  // Missing file
  {
    const std::string missing_vsi = std::format(
        "/vsikerchunk_json_ref/{{{}}}/k", test_dir.File("missing.json"));
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStat;
    EXPECT_EQ(-1, VSIStatL(missing_vsi.c_str(), &sStat));
  }

  // Forced streaming on missing file
  {
    const std::string missing_stream_file =
        test_dir.File("missing_stream.json");
    VSISetPathSpecificOption(missing_stream_file.c_str(),
                             "VSIKERCHUNK_USE_STREAMING_PARSER", "YES");
    const std::string missing_vsi =
        std::format("/vsikerchunk_json_ref/{{{}}}/k", missing_stream_file);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStat;
    EXPECT_EQ(-1, VSIStatL(missing_vsi.c_str(), &sStat));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(VsikerchunkJsonRefTest, DomParserValidAndErrors) {
  ScopedTestDir test_dir("kc_json_dom");
  const std::string target_file = test_dir.File("data.bin");
  WriteVsiFile(target_file, "DOM_PARSER_DATA");

  // 1. Valid DOM Parse (VSIKERCHUNK_USE_STREAMING_PARSER=NO)
  const std::string valid_json = std::format(
      R"json({{
        "version": 1,
        "refs": {{
          ".zgroup": {{"zarr_format": 2}},
          "plain": "dom_string",
          "b64": "base64:RE9NX0JBU0U2NA==",
          "chunk": ["{}", 0, 10]
        }}
      }})json",
      target_file);

  const std::string valid_file = test_dir.File("dom_valid.json");
  WriteVsiFile(valid_file, valid_json);
  VSISetPathSpecificOption(valid_file.c_str(),
                           "VSIKERCHUNK_USE_STREAMING_PARSER", "NO");

  const std::string base_vsi =
      std::format("/vsikerchunk_json_ref/{{{}}}", valid_file);
  EXPECT_EQ("dom_string", ReadVsiFile(base_vsi + "/plain"));
  EXPECT_EQ("DOM_BASE64", ReadVsiFile(base_vsi + "/b64"));
  EXPECT_EQ("DOM_PARSER", ReadVsiFile(base_vsi + "/chunk"));

  // 2. Valid Version 0 DOM Parse (root contains .zgroup)
  const std::string v0_json = R"json({
    ".zgroup": {"zarr_format": 2},
    "v0_key": "v0_val"
  })json";
  const std::string v0_file = test_dir.File("dom_v0.json");
  WriteVsiFile(v0_file, v0_json);
  VSISetPathSpecificOption(v0_file.c_str(), "VSIKERCHUNK_USE_STREAMING_PARSER",
                           "NO");
  EXPECT_EQ("v0_val", ReadVsiFile(std::format(
                          "/vsikerchunk_json_ref/{{{}}}/v0_key", v0_file)));

  // 3. DOM Parser Error Branches
  auto ExpectDomError = [&](std::string_view case_name,
                            std::string_view json_str,
                            std::string_view expected_err) {
    const std::string path = test_dir.File(std::format("{}.json", case_name));
    ASSERT_TRUE(WriteVsiFile(path, json_str));
    VSISetPathSpecificOption(path.c_str(), "VSIKERCHUNK_USE_STREAMING_PARSER",
                             "NO");

    const std::string vsi_path =
        std::format("/vsikerchunk_json_ref/{{{}}}/any_key", path);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStat;
    EXPECT_EQ(-1, VSIStatL(vsi_path.c_str(), &sStat))
        << "Failed for case: " << case_name;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType())
        << "Failed for case: " << case_name;
    if (!expected_err.empty()) {
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(expected_err))
          << "Failed for case: " << case_name;
    }
  };

  struct DomErrorCase {
    std::string_view name;
    std::string_view json;
    std::string_view expected_err;
  };
  static constexpr auto kDomErrors = std::to_array<DomErrorCase>({
      {.name = "v0_no_zgroup",
       .json = R"json({"key": "val"})json",
       .expected_err = "'.zgroup' key not found"},
      {.name = "ver_not_int",
       .json = R"json({"version": "1", "refs": {}})json",
       .expected_err = "'version' key not integer"},
      {.name = "ver_two",
       .json = R"json({"version": 2, "refs": {}})json",
       .expected_err = "'version' = 2 not handled"},
      {.name = "v1_templates",
       .json = R"json({"version": 1, "templates": {}, "refs": {}})json",
       .expected_err = "'templates' key found"},
      {.name = "v1_gen",
       .json = R"json({"version": 1, "gen": [], "refs": {}})json",
       .expected_err = "'gen' key found"},
      {.name = "no_refs",
       .json = R"json({"version": 1})json",
       .expected_err = "'refs' key not found"},
      {.name = "refs_not_dict",
       .json = R"json({"version": 1, "refs": [1, 2]})json",
       .expected_err = "value of 'refs' is not a dict"},
      {.name = "arr_bad_len",
       .json = R"json({"version": 1, "refs": {"k": [1, 2, 3, 4]}})json",
       .expected_err = "not of size 1 or 3"},
      {.name = "arr_not_str",
       .json = R"json({"version": 1, "refs": {"k": [123]}})json",
       .expected_err = "not a string"},
      {.name = "arr_idx1_neg",
       .json = R"json({"version": 1, "refs": {"k": ["u", -1, 5]}})json",
       .expected_err = "not an unsigned 64 bit integer"},
      {.name = "arr_idx2_neg",
       .json = R"json({"version": 1, "refs": {"k": ["u", 0, -5]}})json",
       .expected_err = "not an unsigned 32 bit integer"},
      {.name = "val_bool",
       .json = R"json({"version": 1, "refs": {"k": false}})json",
       .expected_err = "invalid value type"},
      {.name = "dom_bad_b64",
       .json = R"json({"version": 1, "refs": {"k": "base64:!!!"}})json",
       .expected_err = "Base64 decoding failed"},
      {.name = "corrupt_dom",
       .json = "{corrupt json",
       .expected_err = "cannot open"},
  });

  for (const auto& tc : kDomErrors) {
    SCOPED_TRACE(tc.name);
    ExpectDomError(tc.name, tc.json, tc.expected_err);
  }

  // 4. Auto-detection of streamable JSON
  // (VSIKERCHUNK_USE_STREAMING_PARSER=AUTO)
  {
    const std::string compact_json =
        R"json({"version":1,"refs":{".zgroup":{"zarr_format":2},"k":"auto_val"}})json";
    const std::string auto_file = test_dir.File("auto_detect.json");
    WriteVsiFile(auto_file, compact_json);
    EXPECT_EQ("auto_val", ReadVsiFile(std::format(
                              "/vsikerchunk_json_ref/{{{}}}/k", auto_file)));
  }
}

TEST_F(VsikerchunkJsonRefTest, VfsOperationsAndSyntaxVariants) {
  ScopedTestDir test_dir("kc_json_vfs");
  const std::string json_content = R"json({
    "version": 1,
    "refs": {
      "sub/.zgroup": {"zarr_format": 2},
      "sub/arr/0": "val0"
    }
  })json";

  const std::string json_file = test_dir.File("store.json");
  WriteVsiFile(json_file, json_content);

  // 1. Unbraced syntax: /vsikerchunk_json_ref//path/to/store.json/key
  {
    const std::string unbraced_path =
        std::format("/vsikerchunk_json_ref/{}/sub/arr/0", json_file);
    EXPECT_EQ("val0", ReadVsiFile(unbraced_path));
  }

  // Nested braces in filename: a{b}.json
  {
    const std::string nested_brace_file = test_dir.File("a{b}.json");
    WriteVsiFile(nested_brace_file, json_content);
    const std::string nested_path = std::format(
        "/vsikerchunk_json_ref/{{{}}}/sub/arr/0", nested_brace_file);
    EXPECT_EQ("val0", ReadVsiFile(nested_path));
  }

  // 2. Syntax errors:
  // Missing .json in unbraced path
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStat;
    EXPECT_EQ(-1,
              VSIStatL("/vsikerchunk_json_ref/path/without/ext/key", &sStat));
  }

  // Ambiguous multiple .json in unbraced path
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStat;
    EXPECT_EQ(-1,
              VSIStatL("/vsikerchunk_json_ref/first.json/second.json", &sStat));
  }

  // Unclosed '{' on Stat, Open, and ReadDir
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStat;
    EXPECT_EQ(-1, VSIStatL("/vsikerchunk_json_ref/{/unclosed/path", &sStat));
    std::unique_ptr<VSILFILE, decltype(&VSIFCloseL)> fp(
        VSIFOpenL("/vsikerchunk_json_ref/{/unclosed/path", "rb"), VSIFCloseL);
    EXPECT_EQ(nullptr, fp);
    EXPECT_EQ(nullptr,
              VSIReadDirEx("/vsikerchunk_json_ref/{/unclosed/path", 0));
  }

  // Missing file on Open and ReadDir
  {
    const std::string missing_json = test_dir.File("no_such_store.json");
    const std::string missing_vsi =
        std::format("/vsikerchunk_json_ref/{{{}}}", missing_json);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<VSILFILE, decltype(&VSIFCloseL)> fp(
        VSIFOpenL((missing_vsi + "/key").c_str(), "rb"), VSIFCloseL);
    EXPECT_EQ(nullptr, fp);
    EXPECT_EQ(nullptr, VSIReadDirEx(missing_vsi.c_str(), 0));
  }

  // Missing key on Open
  {
    const std::string missing_key_vsi =
        std::format("/vsikerchunk_json_ref/{{{}}}/no_such_key", json_file);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<VSILFILE, decltype(&VSIFCloseL)> fp(
        VSIFOpenL(missing_key_vsi.c_str(), "rb"), VSIFCloseL);
    EXPECT_EQ(nullptr, fp);
  }

  // Unsupported URI and missing target URI
  {
    const std::string uri_json = R"json({
      "version": 1,
      "refs": {
        "ftp_ref": ["ftp://example.com/file.bin"],
        "ftp_range": ["ftp://example.com/file.bin", 0, 10],
        "missing_local": ["/vsimem/no_such_file_local_target.bin", 0, 10]
      }
    })json";
    const std::string uri_file = test_dir.File("uri_refs.json");
    WriteVsiFile(uri_file, uri_json);
    const std::string uri_vsi =
        std::format("/vsikerchunk_json_ref/{{{}}}", uri_file);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStat;
    EXPECT_EQ(-1, VSIStatL((uri_vsi + "/ftp_ref").c_str(), &sStat));
    std::unique_ptr<VSILFILE, decltype(&VSIFCloseL)> fp1(
        VSIFOpenL((uri_vsi + "/ftp_ref").c_str(), "rb"), VSIFCloseL);
    EXPECT_EQ(nullptr, fp1);
    std::unique_ptr<VSILFILE, decltype(&VSIFCloseL)> fp2(
        VSIFOpenL((uri_vsi + "/ftp_range").c_str(), "rb"), VSIFCloseL);
    EXPECT_EQ(nullptr, fp2);
    std::unique_ptr<VSILFILE, decltype(&VSIFCloseL)> fp3(
        VSIFOpenL((uri_vsi + "/missing_local").c_str(), "rb"), VSIFCloseL);
    EXPECT_EQ(nullptr, fp3);
  }

  // Write mode rejected
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<VSILFILE, decltype(&VSIFCloseL)> fp(
        VSIFOpenL(
            std::format("/vsikerchunk_json_ref/{{{}}}/k", json_file).c_str(),
            "wb"),
        VSIFCloseL);
    EXPECT_EQ(nullptr, fp);
  }

  // 3. Stat on directory and virtual directory
  const std::string base_vsi =
      std::format("/vsikerchunk_json_ref/{{{}}}", json_file);
  {
    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(base_vsi.c_str(), &sStat));
    EXPECT_TRUE(VSI_ISDIR(sStat.st_mode));

    EXPECT_EQ(0, VSIStatL((base_vsi + "/sub").c_str(), &sStat));
    EXPECT_TRUE(VSI_ISDIR(sStat.st_mode));

    EXPECT_EQ(-1, VSIStatL((base_vsi + "/missing_dir").c_str(), &sStat));
  }

  // 4. ReadDirEx on root and sub
  {
    const CPLStringList papszRoot(VSIReadDirEx(base_vsi.c_str(), 0));
    ASSERT_NE(nullptr, papszRoot.List());
    EXPECT_TRUE(papszRoot.FindString("sub") >= 0);

    const CPLStringList papszSub(VSIReadDirEx((base_vsi + "/sub").c_str(), 0));
    ASSERT_NE(nullptr, papszSub.List());
    EXPECT_TRUE(papszSub.FindString("arr") >= 0);
    EXPECT_TRUE(papszSub.FindString(".zgroup") >= 0);
  }
}

TEST_F(VsikerchunkJsonRefTest, ParquetConversionAndCaching) {
  ScopedTestDir test_dir("kc_json_parq_conv");
  const std::string target_file = test_dir.File("data.bin");
  WriteVsiFile(target_file, "CONVERTED_CHUNKS");

  const std::string json_content = std::format(
      R"json({{
        "version": 1,
        "refs": {{
          ".zattrs": {{"dataset": "test"}},
          "arr/.zarray": {{
            "zarr_format": 2,
            "shape": [2],
            "chunks": [1],
            "dtype": "<i4"
          }},
          "arr/0": "inline_chunk_data",
          "arr/1": ["{}", 0, 9],
          "arr0d/.zarray": {{
            "zarr_format": 2,
            "shape": [],
            "chunks": [],
            "dtype": "<i4"
          }},
          "arr0d/0": "abcd"
        }}
      }})json",
      target_file);

  const std::string json_file = test_dir.File("refs.json");
  WriteVsiFile(json_file, json_content);

  // 1. Without PARQUET driver, conversion and cached VFS fail cleanly
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(VSIKerchunkConvertJSONToParquet(
        json_file.c_str(), test_dir.File("out_parq").c_str(), nullptr,
        nullptr));
    VSIStatBufL sStat;
    EXPECT_EQ(-1,
              VSIStatL(std::format("/vsikerchunk_json_ref_cached/{{{}}}/arr/0",
                                   json_file)
                           .c_str(),
                       &sStat));
  }

  // 2. With ScopedFakeParquetDriver
  ScopedFakeParquetDriver fake_parquet_driver;

  // Convert JSON to Parquet successfully
  const std::string dst_parquet_dir = test_dir.File("dst_parq");
  EXPECT_TRUE(VSIKerchunkConvertJSONToParquet(
      json_file.c_str(), dst_parquet_dir.c_str(),
      [](double, const char*, void*) { return TRUE; }, nullptr));

  // Cancellation callback
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(VSIKerchunkConvertJSONToParquet(
        json_file.c_str(), test_dir.File("dst_cancel").c_str(),
        [](double, const char*, void*) { return FALSE; }, nullptr));
  }

  // Convert with streaming parser forced on JSON file
  {
    VSISetPathSpecificOption(json_file.c_str(),
                             "VSIKERCHUNK_USE_STREAMING_PARSER", "YES");
    const std::string dst_stream_parq = test_dir.File("dst_stream_parq");
    EXPECT_TRUE(VSIKerchunkConvertJSONToParquet(
        json_file.c_str(), dst_stream_parq.c_str(),
        [](double, const char*, void*) { return TRUE; }, nullptr));
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_FALSE(VSIKerchunkConvertJSONToParquet(
          json_file.c_str(), test_dir.File("dst_stream_cancel").c_str(),
          [](double, const char*, void*) { return FALSE; }, nullptr));
    }
    VSIClearPathSpecificOptions(nullptr);
  }

  // Conversion error: missing source JSON
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(VSIKerchunkConvertJSONToParquet(
        test_dir.File("no_such.json").c_str(),
        test_dir.File("dst_missing").c_str(), nullptr, nullptr));
  }

  // Conversion error: corrupt .zarray
  {
    const std::string bad_zarray_json = R"json({
      "version": 1,
      "refs": {
        "arr/.zarray": "{corrupt json"
      }
    })json";
    const std::string bad_json_file = test_dir.File("bad_zarray.json");
    WriteVsiFile(bad_json_file, bad_zarray_json);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(VSIKerchunkConvertJSONToParquet(
        bad_json_file.c_str(), test_dir.File("dst_bad").c_str(), nullptr,
        nullptr));
  }

  // Conversion error: 0 shape or mismatched chunks
  {
    const std::string zero_shape_json = R"json({
      "version": 1,
      "refs": {
        "arr/.zarray": {"shape": [0], "chunks": [1]}
      }
    })json";
    const std::string zero_json_file = test_dir.File("zero_shape.json");
    WriteVsiFile(zero_json_file, zero_shape_json);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(VSIKerchunkConvertJSONToParquet(
        zero_json_file.c_str(), test_dir.File("dst_zero").c_str(), nullptr,
        nullptr));

    const std::string mismatch_shape_json = R"json({
      "version": 1,
      "refs": {
        "arr/.zarray": {"shape": [2], "chunks": [1, 1]}
      }
    })json";
    const std::string mismatch_json_file = test_dir.File("mismatch_shape.json");
    WriteVsiFile(mismatch_json_file, mismatch_shape_json);
    EXPECT_FALSE(VSIKerchunkConvertJSONToParquet(
        mismatch_json_file.c_str(), test_dir.File("dst_mismatch").c_str(),
        nullptr, nullptr));
  }

  // Conversion error: invalid or out-of-range chunk indices
  {
    const std::string bad_idx_json = R"json({
      "version": 1,
      "refs": {
        "arr/.zarray": {"shape": [2], "chunks": [1]},
        "arr/bad_idx": "x",
        "arr/99": "x"
      }
    })json";
    const std::string bad_idx_file = test_dir.File("bad_idx.json");
    WriteVsiFile(bad_idx_file, bad_idx_json);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(VSIKerchunkConvertJSONToParquet(
        bad_idx_file.c_str(), test_dir.File("dst_bad_idx").c_str(), nullptr,
        nullptr));
  }

  // Conversion error: path traversal in array name
  {
    const std::string traversal_json = R"json({
      "version": 1,
      "refs": {
        "../evil/.zarray": {"shape": [1], "chunks": [1]},
        "../evil/0": "x"
      }
    })json";
    const std::string traversal_file = test_dir.File("traversal.json");
    WriteVsiFile(traversal_file, traversal_json);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(VSIKerchunkConvertJSONToParquet(
        traversal_file.c_str(), test_dir.File("dst_traversal").c_str(), nullptr,
        nullptr));
  }

  // Conversion errors with fake Parquet driver failures
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    FakeParquetRegistry::Instance().fail_create = true;
    EXPECT_FALSE(VSIKerchunkConvertJSONToParquet(
        json_file.c_str(), test_dir.File("dst_fail_create").c_str(), nullptr,
        nullptr));
    FakeParquetRegistry::Instance().fail_create = false;

    FakeParquetRegistry::Instance().fail_create_layer = true;
    EXPECT_FALSE(VSIKerchunkConvertJSONToParquet(
        json_file.c_str(), test_dir.File("dst_fail_layer").c_str(), nullptr,
        nullptr));
    FakeParquetRegistry::Instance().fail_create_layer = false;

    FakeParquetRegistry::Instance().fail_create_feature = true;
    EXPECT_FALSE(VSIKerchunkConvertJSONToParquet(
        json_file.c_str(), test_dir.File("dst_fail_feat").c_str(), nullptr,
        nullptr));
    FakeParquetRegistry::Instance().fail_create_feature = false;

    FakeParquetRegistry::Instance().fail_close = true;
    EXPECT_FALSE(VSIKerchunkConvertJSONToParquet(
        json_file.c_str(), test_dir.File("dst_fail_close").c_str(), nullptr,
        nullptr));
    FakeParquetRegistry::Instance().fail_close = false;
  }

  // 3. Test /vsikerchunk_json_ref_cached/ using POSIX cache dir
  ScopedPosixDir posix_cache("kc_cache_test");
  const std::string posix_cache_dir = posix_cache.path();

  CPLConfigOptionSetter cache_opt("GDAL_CACHE_DIRECTORY",
                                  posix_cache_dir.c_str(), false);
  CPLConfigOptionSetter debug_opt("VSIKERCHUNK_FOR_TESTS",
                                  "SHORT_DELAY_STALLED_LOCK", false);

  // Fresh JSON file for lock + conversion test
  const std::string fresh_cached_json = test_dir.File("fresh_cached.json");
  WriteVsiFile(fresh_cached_json, json_content);
  const std::string fresh_cached_vsi =
      std::format("/vsikerchunk_json_ref_cached/{{{}}}", fresh_cached_json);

  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL((fresh_cached_vsi + "/arr/0").c_str(), &sStat));
  EXPECT_EQ("inline_chunk_data", ReadVsiFile(fresh_cached_vsi + "/arr/0"));

  // Pre-seeded Parquet VFS delegation (vsikerchunk_json_ref.cpp:940-966)
  const std::string preseeded_json = test_dir.File("preseeded_cached.json");
  WriteVsiFile(preseeded_json, json_content);
  VSIStatBufL st;
  ASSERT_EQ(0, VSIStatL(preseeded_json.c_str(), &st));
  const std::string preseeded_cache_dir = std::format(
      "{}/preseeded_cached_{}_{}.zarr", posix_cache_dir,
      static_cast<uint64_t>(st.st_size), static_cast<uint64_t>(st.st_mtime));
  ASSERT_TRUE(VSIKerchunkConvertJSONToParquet(
      json_file.c_str(), preseeded_cache_dir.c_str(), nullptr, nullptr));

  VSISetPathSpecificOption(preseeded_json.c_str(), "VSIKERCHUNK_CACHE_DIR",
                           posix_cache_dir.c_str());
  const std::string preseeded_vsi =
      std::format("/vsikerchunk_json_ref_cached/{{{}}}", preseeded_json);

  EXPECT_EQ(0, VSIStatL((preseeded_vsi + "/arr/0").c_str(), &sStat));
  EXPECT_EQ("inline_chunk_data", ReadVsiFile(preseeded_vsi + "/arr/0"));

  const CPLStringList preseeded_dir(VSIReadDirEx(preseeded_vsi.c_str(), 0));
  ASSERT_NE(nullptr, preseeded_dir.List());
  EXPECT_GE(preseeded_dir.FindString("arr"), 0);
  VSIClearPathSpecificOptions(nullptr);

  // Cached VFS on missing JSON file
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStatMissing;
    EXPECT_EQ(-1,
              VSIStatL("/vsikerchunk_json_ref_cached/{/vsimem/missing.json}/k",
                       &sStatMissing));
  }

  // VSIKERCHUNK_CACHE_DIR pointing to regular file
  {
    const std::string cache_err_json = test_dir.File("cache_err.json");
    WriteVsiFile(cache_err_json, json_content);
    const std::string cache_err_vsi =
        std::format("/vsikerchunk_json_ref_cached/{{{}}}", cache_err_json);
    const std::string reg_file = CPLFormFilenameSafe(
        posix_cache_dir.c_str(), "regular_file.txt", nullptr);
    WriteVsiFile(reg_file, "not_a_dir");
    VSISetPathSpecificOption(cache_err_json.c_str(), "VSIKERCHUNK_CACHE_DIR",
                             reg_file.c_str());
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStatReg;
    EXPECT_EQ(-1, VSIStatL((cache_err_vsi + "/arr/0").c_str(), &sStatReg));
    VSIClearPathSpecificOptions(nullptr);
  }

  // fail_create = true during cache generation on fresh JSON file
  {
    const std::string fail_json = test_dir.File("fail.json");
    WriteVsiFile(fail_json, json_content);
    const std::string fail_cached_vsi =
        std::format("/vsikerchunk_json_ref_cached/{{{}}}", fail_json);
    FakeParquetRegistry::Instance().fail_create = true;
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    VSIStatBufL sStatFail;
    EXPECT_EQ(-1, VSIStatL((fail_cached_vsi + "/arr/0").c_str(), &sStatFail));
    FakeParquetRegistry::Instance().fail_create = false;
  }
}

// TODO: b/335317901 - Test additional Kerchunk reference formats and templates.

}  // namespace
}  // namespace autotest2
