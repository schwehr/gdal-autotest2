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
// Tests for GDALArgumentParser (apps/gdalargumentparser.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalargumentparser.cpp

#include "third_party/gdal/apps/gdalargumentparser.h"

#include <stdexcept>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

TEST(GdalArgumentParserTest, ConstructLibraryMode) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  EXPECT_THAT(parser.usage(), HasSubstr("Usage: test_prog"));
}

TEST(GdalArgumentParserTest, ConstructBinaryModeGeneralHelp) {
  GDALArgumentParser parser("test_binary", /*bForBinary=*/true);
  parser.parse_args(CPLStringList{"test_binary", "--help-general"});
  EXPECT_TRUE(parser.is_used("--help-general"));
}

TEST(GdalArgumentParserTest, ConstructBinaryModeHelpFlagsExit) {
  GDALArgumentParser parser("test_binary", /*bForBinary=*/true);
  EXPECT_EXIT(parser.parse_args(CPLStringList{"test_binary", "-h"}),
              ::testing::ExitedWithCode(0), "");
  EXPECT_EXIT(parser.parse_args(CPLStringList{"test_binary", "--help"}),
              ::testing::ExitedWithCode(0), "");
  EXPECT_EXIT(parser.parse_args(CPLStringList{"test_binary", "--help-doc"}),
              ::testing::ExitedWithCode(0), "");
  EXPECT_EXIT(parser.parse_args(CPLStringList{"test_binary", "--long-usage"}),
              ::testing::ExitedWithCode(0), "");
  EXPECT_EXIT(
      parser.parse_args(CPLStringList{"test_binary", "--utility_version"}),
      ::testing::ExitedWithCode(0), "");
}

TEST(GdalArgumentParserTest, ExtraUsageHint) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  parser.add_extra_usage_hint("See https://gdal.org for details.");
  EXPECT_THAT(parser.usage(), HasSubstr("See https://gdal.org for details."));
}

TEST(GdalArgumentParserTest, DisplayErrorAndUsage) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  std::runtime_error err("Simulated error occurred");
  ::testing::internal::CaptureStderr();
  ::testing::internal::CaptureStdout();
  parser.display_error_and_usage(err);
  const std::string err_str = ::testing::internal::GetCapturedStderr();
  const std::string out_str = ::testing::internal::GetCapturedStdout();
  EXPECT_THAT(err_str, HasSubstr("Error: Simulated error occurred"));
  EXPECT_THAT(out_str,
              HasSubstr("Note: test_prog --long-usage for full help."));
}

TEST(GdalArgumentParserTest, AddQuietArgumentWithVariable) {
  {
    GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
    bool quiet = false;
    parser.add_quiet_argument(&quiet);
    parser.parse_args(CPLStringList{"test_prog", "-q"});
    EXPECT_TRUE(quiet);
  }
  {
    GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
    bool quiet = false;
    parser.add_quiet_argument(&quiet);
    parser.parse_args(CPLStringList{"test_prog", "--quiet"});
    EXPECT_TRUE(quiet);
  }
}

TEST(GdalArgumentParserTest, AddQuietArgumentWithNullptr) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  parser.add_quiet_argument(nullptr);
  parser.parse_args(CPLStringList{"test_prog", "-q"});
  EXPECT_TRUE(parser.is_used("-q"));
}

TEST(GdalArgumentParserTest, AddInputFormatArgumentRecognized) {
  GDALRegister_MEM();
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  CPLStringList if_formats;
  parser.add_input_format_argument(&if_formats);

  parser.parse_args(CPLStringList{"test_prog", "-if", "MEM"});
  ASSERT_EQ(1, if_formats.size());
  EXPECT_STREQ("MEM", if_formats[0]);
}

TEST(GdalArgumentParserTest, AddInputFormatArgumentUnrecognizedWarns) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  CPLStringList if_formats;
  parser.add_input_format_argument(&if_formats);

  WithQuietHandler quiet;
  CPLErrorReset();
  parser.parse_args(CPLStringList{"test_prog", "-if", "NOT_A_REAL_DRIVER"});
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("not a recognized driver"));
  ASSERT_EQ(1, if_formats.size());
  EXPECT_STREQ("NOT_A_REAL_DRIVER", if_formats[0]);
}

TEST(GdalArgumentParserTest, AddInputFormatArgumentNullptr) {
  GDALRegister_MEM();
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  parser.add_input_format_argument(nullptr);
  parser.parse_args(CPLStringList{"test_prog", "-if", "MEM"});
  EXPECT_TRUE(parser.is_used("-if"));
}

TEST(GdalArgumentParserTest, AddOutputFormatArgument) {
  {
    GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
    std::string format;
    parser.add_output_format_argument(format);
    parser.parse_args(CPLStringList{"test_prog", "-of", "GTiff"});
    EXPECT_EQ("GTiff", format);
  }
  {
    GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
    std::string format;
    parser.add_output_format_argument(format);
    parser.parse_args(CPLStringList{"test_prog", "-f", "COG"});
    EXPECT_EQ("COG", format);
  }
}

TEST(GdalArgumentParserTest, AddCreationOptionsArgument) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  CPLStringList co;
  parser.add_creation_options_argument(co);

  parser.parse_args(CPLStringList{"test_prog", "-co", "COMPRESS=DEFLATE", "-co",
                                  "TILED=YES"});
  ASSERT_EQ(2, co.size());
  EXPECT_STREQ("COMPRESS=DEFLATE", co[0]);
  EXPECT_STREQ("TILED=YES", co[1]);
}

TEST(GdalArgumentParserTest, AddMetadataItemOptionsArgument) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  CPLStringList mo;
  parser.add_metadata_item_options_argument(mo);

  parser.parse_args(CPLStringList{"test_prog", "-mo", "AUTHOR=Google"});
  ASSERT_EQ(1, mo.size());
  EXPECT_STREQ("AUTHOR=Google", mo[0]);
}

TEST(GdalArgumentParserTest, AddOpenOptionsArgumentReference) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  CPLStringList oo;
  parser.add_open_options_argument(oo);

  parser.parse_args(CPLStringList{"test_prog", "-oo", "NUM_THREADS=2"});
  ASSERT_EQ(1, oo.size());
  EXPECT_STREQ("NUM_THREADS=2", oo[0]);
}

TEST(GdalArgumentParserTest, AddOpenOptionsArgumentPointer) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  CPLStringList oo;
  parser.add_open_options_argument(&oo);

  parser.parse_args(CPLStringList{"test_prog", "-oo", "CACHE=YES"});
  ASSERT_EQ(1, oo.size());
  EXPECT_STREQ("CACHE=YES", oo[0]);

  GDALArgumentParser parser_null("test_prog", /*bForBinary=*/false);
  parser_null.add_open_options_argument(nullptr);
  parser_null.parse_args(CPLStringList{"test_prog", "-oo", "TEST=1"});
  EXPECT_TRUE(parser_null.is_used("-oo"));
}

TEST(GdalArgumentParserTest, AddOutputTypeArgumentValid) {
  {
    GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
    GDALDataType dt = GDT_Unknown;
    parser.add_output_type_argument(dt);
    parser.parse_args(CPLStringList{"test_prog", "-ot", "Byte"});
    EXPECT_EQ(GDT_Byte, dt);
  }
  {
    GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
    GDALDataType dt = GDT_Unknown;
    parser.add_output_type_argument(dt);
    parser.parse_args(CPLStringList{"test_prog", "-ot", "Float32"});
    EXPECT_EQ(GDT_Float32, dt);
  }
}

TEST(GdalArgumentParserTest, AddOutputTypeArgumentInvalidThrows) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  GDALDataType dt = GDT_Unknown;
  parser.add_output_type_argument(dt);

  EXPECT_THROW(
      parser.parse_args(CPLStringList{"test_prog", "-ot", "InvalidPixelType"}),
      std::invalid_argument);
}

TEST(GdalArgumentParserTest, AddLayerCreationOptionsArgument) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  CPLStringList lco;
  parser.add_layer_creation_options_argument(lco);

  parser.parse_args(CPLStringList{"test_prog", "-lco", "SPATIAL_INDEX=YES"});
  ASSERT_EQ(1, lco.size());
  EXPECT_STREQ("SPATIAL_INDEX=YES", lco[0]);
}

TEST(GdalArgumentParserTest, AddDatasetCreationOptionsArgument) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  CPLStringList dsco;
  parser.add_dataset_creation_options_argument(dsco);

  parser.parse_args(CPLStringList{"test_prog", "-dsco", "VERSION=1.3"});
  ASSERT_EQ(1, dsco.size());
  EXPECT_STREQ("VERSION=1.3", dsco[0]);
}

TEST(GdalArgumentParserTest, ParseArgsWithoutBinaryName) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  bool quiet = false;
  parser.add_quiet_argument(&quiet);

  CPLStringList args;
  args.AddString("-q");
  parser.parse_args_without_binary_name(args.List());
  EXPECT_TRUE(quiet);

  GDALArgumentParser parser_null("test_prog", /*bForBinary=*/false);
  parser_null.parse_args_without_binary_name(nullptr);
}

TEST(GdalArgumentParserTest, CaseInsensitiveArgumentLookup) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  bool quiet = false;
  parser.add_quiet_argument(&quiet);

  parser.parse_args(CPLStringList{"test_prog", "-Q"});
  EXPECT_TRUE(quiet);
}

TEST(GdalArgumentParserTest, GetNonPositionalArguments) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  bool quiet = false;
  parser.add_quiet_argument(&quiet);
  parser.add_argument("src");
  parser.add_argument("dst");

  CPLStringList args({"input.tif", "-q", "output.tif"});
  CPLStringList non_pos = parser.get_non_positional_arguments(args);
  ASSERT_EQ(1, non_pos.size());
  EXPECT_STREQ("-q", non_pos[0]);
}

TEST(GdalArgumentParserTest, GetNonPositionalArgumentsZeroExpectedThrows) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  CPLStringList args({"unexpected_pos_arg"});
  EXPECT_THROW(parser.get_non_positional_arguments(args), std::runtime_error);
}

TEST(GdalArgumentParserTest, GetNonPositionalArgumentsMaxExceededThrows) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  parser.add_argument("src");
  CPLStringList args({"src_file", "extra_file"});
  EXPECT_THROW(parser.get_non_positional_arguments(args), std::runtime_error);
}

TEST(GdalArgumentParserTest, GetNonPositionalArgumentsUnknownThrows) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  CPLStringList args({"--totally-unknown-flag"});
  EXPECT_THROW(parser.get_non_positional_arguments(args), std::runtime_error);
}

TEST(GdalArgumentParserTest, AddInvertedLogicFlag) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  bool overwrite = true;
  parser.add_inverted_logic_flag("--no-overwrite", &overwrite,
                                 "Do not overwrite.");

  parser.parse_args(CPLStringList{"test_prog", "--no-overwrite"});
  EXPECT_FALSE(overwrite);

  GDALArgumentParser parser_null("test_prog", /*bForBinary=*/false);
  parser_null.add_inverted_logic_flag("--no-check", nullptr, "No check.");
  parser_null.parse_args(CPLStringList{"test_prog", "--no-check"});
  EXPECT_TRUE(parser_null.is_used("--no-check"));
}

TEST(GdalArgumentParserTest, SubparsersAndIsUsedGlobally) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  auto* sub = parser.add_subparser("convert", /*bForBinary=*/false);
  ASSERT_THAT(sub, NotNull());

  EXPECT_EQ(sub, parser.get_subparser("convert"));
  EXPECT_EQ(sub, parser.get_subparser("CONVERT"));
  EXPECT_EQ(nullptr, parser.get_subparser("nonexistent"));

  bool sub_flag = false;
  sub->add_argument("--sub-opt").flag().store_into(sub_flag);

  parser.parse_args(CPLStringList{"test_prog", "convert", "--sub-opt"});
  EXPECT_TRUE(sub_flag);
  EXPECT_TRUE(parser.is_used_globally("--sub-opt"));
  EXPECT_FALSE(parser.is_used_globally("--never-passed"));
  EXPECT_FALSE(parser.is_used_globally("unregistered_argument"));
}

TEST(GdalArgumentParserTest, ParseArgsEdgeCases) {
  GDALArgumentParser parser("test_prog", /*bForBinary=*/false);
  CPLStringList prog_only{"test_prog"};
  parser.parse_args(prog_only);

  GDALArgumentParser p_no_pos("test_prog", /*bForBinary=*/false);
  EXPECT_THROW(
      p_no_pos.parse_args(CPLStringList{"test_prog", "unexpected_pos"}),
      std::runtime_error);

  GDALArgumentParser p_max_pos("test_prog", /*bForBinary=*/false);
  p_max_pos.add_argument("only_one");
  EXPECT_THROW(
      p_max_pos.parse_args(CPLStringList{"test_prog", "first", "second"}),
      std::runtime_error);

  EXPECT_THROW(
      p_no_pos.parse_args(CPLStringList{"test_prog", "--unknown-option"}),
      std::runtime_error);
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdalargumentparser.cpp.
