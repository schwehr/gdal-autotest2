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
// Unit tests for the WKT1 Bison lexer and parser wrapper
// (proj/src/wkt1_parser.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/wkt1_parser.cpp

#include "third_party/proj4/proj/src/wkt1_parser.h"

#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"

namespace proj {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

TEST(Wkt1ParserTest, ValidCoordinateSystemsAndMathTransformsReturnEmptyError) {
  struct WktCase {
    std::string_view label;
    std::string_view wkt;
  };

  constexpr WktCase kCases[] = {
      {
          .label = "geogcs_with_towgs84_axes_and_authority",
          .wkt =
              R"(GEOGCS["WGS 84", DATUM["WGS_1984", SPHEROID["WGS 84", 6378137, 298.257223563, AUTHORITY["EPSG", "7030"]], TOWGS84[-1.5, +2.5e-1, 3.0E+2, 4.1e2, -0.5, +0.25, 1.25], AUTHORITY["EPSG", "6326"]], PRIMEM["Greenwich", 0, AUTHORITY["EPSG", "8901"]], UNIT["degree", 0.0174532925199433, AUTHORITY["EPSG", "9122"]], AXIS["Lon", EAST], AXIS["Lat", north], AUTHORITY["EPSG", "4326"]])",
      },
      {
          .label = "projcs_with_extension_and_parameters",
          .wkt =
              R"(PROJCS["WGS 84 / World Mercator", GEOGCS["WGS 84", DATUM["WGS_1984", SPHEROID["WGS 84", 6378137, 298.257223563]], PRIMEM["Greenwich", 0], UNIT["degree", 0.0174532925199433]], PROJECTION["Mercator_1SP"], PARAMETER["central_meridian", 0], PARAMETER["scale_factor", 1], PARAMETER["false_easting", +0.0], PARAMETER["false_northing", -0.0], UNIT["metre", 1], EXTENSION["PROJ4", "+proj=merc +units=m"], AUTHORITY["EPSG", "3395"]])",
      },
      {
          .label = "lowercase_parens_and_all_whitespace_types",
          .wkt =
              "\t\r\n geogcs(\t\"WGS 84\",\r\n"
              "  datum(\"WGS_1984\", spheroid(\"WGS 84\", 6378137, 298.257)),\n"
              "  primem(\"Greenwich\", 0),\n"
              "  unit(\"degree\", 0.0174532925199433))\t\r\n ",
      },
      {
          .label = "geocentric_cs_three_axes",
          .wkt =
              R"(GEOCCS["WGS 84", DATUM["WGS_1984", SPHEROID["WGS 84", 6378137, 298.257223563]], PRIMEM["Greenwich", 0], UNIT["metre", 1], AXIS["Geocentric X", OTHER], AXIS["Geocentric Y", EAST], AXIS["Geocentric Z", NORTH], AUTHORITY["EPSG", "4978"]])",
      },
      {
          .label = "vertical_cs_and_compound_cs",
          .wkt =
              R"(COMPD_CS["Mercator + H", PROJCS["World Mercator", GEOGCS["WGS 84", DATUM["WGS_1984", SPHEROID["WGS 84", 6378137, 298.257223563]], PRIMEM["Greenwich", 0], UNIT["degree", 0.0174532925199433]], PROJECTION["Mercator_1SP"], UNIT["metre", 1]], VERT_CS["EGM96 height", VERT_DATUM["EGM96 geoid", 2005], UNIT["metre", 1], AXIS["Gravity-related height", UP]]])",
      },
      {
          .label = "local_cs_and_fitted_cs",
          .wkt =
              R"(FITTED_CS["Fitted", PARAM_MT["Affine", PARAMETER["num", +1e0]], LOCAL_CS["Local", LOCAL_DATUM["Local", 0], UNIT["metre", 1], AXIS["X", OTHER]]])",
      },
      {
          .label = "fitted_cs_with_concat_inverse_and_passthrough_mt",
          .wkt =
              R"(FITTED_CS["CompoundMT", CONCAT_MT[PARAM_MT["Mercator_1SP", PARAMETER["central_meridian", 0.0]], INVERSE_MT[PASSTHROUGH_MT[1, PARAM_MT["Affine"]]]], GEOGCS["WGS 84", DATUM["WGS_1984", SPHEROID["WGS 84", 6378137, 298.257223563]], PRIMEM["Greenwich", 0], UNIT["degree", 0.0174532925199433]]])",
      },
      {
          .label = "esri_vertcs_vdatum_and_linunit",
          .wkt =
              R"(GEOGCS["WGS 84", DATUM["WGS_1984", SPHEROID["WGS 84", 6378137, 298.257223563]], PRIMEM["Greenwich", 0], UNIT["degree", 0.0174532925199433], LINUNIT["metre", 1.0]], VERTCS["EGM96", VDATUM["EGM96_Geoid"], PARAMETER["Vertical_Shift", 0.0], UNIT["metre", 1.0]])",
      },
  };

  for (const auto& tc : kCases) {
    EXPECT_THAT(pj_wkt1_parse(std::string(tc.wkt)), IsEmpty()) << tc.label;
  }
}

TEST(Wkt1ParserTest, LexesNumericFormatsQuotedStringsAndMixedCaseIdentifiers) {
  // Exercise empty string "", integer 0, negative -0, positive decimal +12.34,
  // unsigned/signed exponents (e, E, e+, E-), and mixed-case T_IDENTIFIER.
  const std::string math_in_fitted_wkt =
      R"(FITTED_CS["Fmt", )"
      R"(PARAM_MT["", PARAMETER["p1", 0], PARAMETER["p2", -0], )"
      R"(PARAMETER["p3", +12.34], PARAMETER["p4", 5e2], )"
      R"(PARAMETER["p5", 6E-3], PARAMETER["p6", 7.8e+1]], )"
      R"(GEOGCS["WGS 84", DATUM["WGS_1984", SPHEROID["WGS 84", 6378137, 298.257223563]], PRIMEM["Greenwich", 0], UNIT["degree", 0.0174532925199433]]])";
  EXPECT_THAT(pj_wkt1_parse(math_in_fitted_wkt), IsEmpty());

  const std::string local_wkt =
      R"(LOCAL_CS["Local", LOCAL_DATUM["D", 0], UNIT["m", 1], )"
      R"(AXIS["X", EastWest], AXIS["Y", south], AXIS["Z", UP]])";
  EXPECT_THAT(pj_wkt1_parse(local_wkt), IsEmpty());
}

TEST(Wkt1ParserTest, InvalidSyntaxAndLexErrorsReturnDiagnosticMessage) {
  struct ErrorCase {
    std::string_view label;
    std::string_view wkt;
  };

  constexpr ErrorCase kCases[] = {
      {.label = "empty_input", .wkt = ""},
      {.label = "whitespace_only", .wkt = " \t\r\n"},
      // Unterminated double-quoted string hits EOF inside the quote scan loop.
      {.label = "unterminated_quote", .wkt = R"(GEOGCS["WGS 84)"},
      // Keyword prefix followed immediately by alpha falls through to
      // T_IDENTIFIER instead of matching the keyword token.
      {.label = "keyword_prefix_followed_by_alpha",
       .wkt = R"(GEOGCSX["WGS 84"])"},
      {.label = "extension_prefix_followed_by_alpha",
       .wkt = R"(EXTENSIONFOO["a", "b"])"},
      // Lone sign not followed by a digit falls through to single-char token.
      {.label = "lone_minus_sign", .wkt = "GEOGCS[-]"},
      {.label = "lone_plus_sign", .wkt = "GEOGCS[+]"},
      {.label = "unexpected_symbol", .wkt = "GEOGCS[@]"},
      // Multiline WKT error exercises pj_wkt1_error -> pj_wkt_error.
      {.label = "multiline_syntax_error",
       .wkt = "GEOGCS[\"WGS 84\",\n  BROKEN_NODE]"},
      {.label = "standalone_param_mt_is_not_top_level_cs",
       .wkt = R"(PARAM_MT["Affine"])"},
  };

  for (const auto& tc : kCases) {
    const std::string err = pj_wkt1_parse(std::string(tc.wkt));
    EXPECT_THAT(err, Not(IsEmpty())) << tc.label;
    EXPECT_THAT(err, HasSubstr("Parsing error")) << tc.label;
  }
}

TEST(Wkt1ParserTest, PublicCApiCreateFromWktReportsWkt1GrammarValidation) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  // 1. Valid WKT1 GEOGCS parses cleanly without grammar errors.
  char** raw_warnings = nullptr;
  char** raw_errors = nullptr;
  constexpr char kValidWkt1[] =
      R"(GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]])";
  ScopedPj valid_pj(proj_create_from_wkt(ctx.get(), kValidWkt1, nullptr,
                                         &raw_warnings, &raw_errors));
  ScopedStringList valid_warnings(raw_warnings);
  ScopedStringList valid_errors(raw_errors);
  EXPECT_NE(nullptr, valid_pj.get());
  EXPECT_EQ(nullptr, valid_errors.get());

  // 2. Malformed WKT1 GEOGCS triggers pj_wkt1_parse grammar error reporting.
  raw_warnings = nullptr;
  raw_errors = nullptr;
  constexpr char kMalformedWkt1[] =
      R"(GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]]])";
  ScopedPj invalid_pj(proj_create_from_wkt(ctx.get(), kMalformedWkt1, nullptr,
                                           &raw_warnings, &raw_errors));
  ScopedStringList invalid_warnings(raw_warnings);
  ScopedStringList invalid_errors(raw_errors);
  ASSERT_NE(nullptr, invalid_errors.get());
  ASSERT_NE(nullptr, invalid_errors.get()[0]);
  EXPECT_THAT(invalid_errors.get()[0], HasSubstr("Parsing error"));
}

}  // namespace
}  // namespace proj
