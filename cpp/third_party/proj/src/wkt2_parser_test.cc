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
// Unit tests for the WKT2 Bison/flex grammar entrypoint and lexer
// (proj/src/wkt2_parser.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/wkt2_parser.cpp

#include "third_party/proj4/proj/src/wkt2_parser.h"

#include <array>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

TEST(Wkt2ParserTest, ValidWkt2ConstructsAndLexerEdgeCases) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct ValidCase {
    std::string_view label;
    std::string_view wkt;
    PJ_TYPE expected_type;
  };

  // Exercises case-insensitivity, spaces/tabs/LF/CR (including trailing
  // whitespace at EOF), keyword prefix disambiguation (ELLIPSOID/ELLIPSOIDAL,
  // NORTH/NORTHEAST/NORTHWEST, TEMPORAL/TEMPORALMEASURE), single-digit
  // tokens '1'/'2'/'3' vs multi-digit unsigned integers ('0', '4'..'9', and
  // leading '1'/'2'/'3' followed by digits), scientific notation ('E', '+',
  // '-'), escaped double quotes (""), empty string "", parentheses (...),
  // ISO-8601 datetimes ('T', 'Z', ':'), and major WKT2:2015 / WKT2:2019 nodes.
  constexpr std::array<ValidCase, 12> kValidCases = {{
      {.label = "Lowercase3DGeogCrsWithEscapedQuotesAndAxisRanges",
       .wkt =
           "\tgeogcrs[\"WGS \"\"84\"\"\",\r\n"
           "  datum[\"World Geodetic System 1984\",\n"
           "    ellipsoid[\"WGS 84\",6378137,298.257223563,"
           "lengthunit[\"metre\",1]]],\n"
           "  cs[ellipsoidal,3],\n"
           "    axis[\"geodetic latitude (Lat)\",north,order[1],"
           "angleunit[\"degree\",0.0174532925199433]],\n"
           "    axis[\"geodetic longitude (Lon)\",east,order[2],"
           "angleunit[\"degree\",0.0174532925199433],"
           "axisminvalue[-180],axismaxvalue[180],rangemeaning[wraparound]],\n"
           "    axis[\"ellipsoidal height (h)\",up,order[3],"
           "lengthunit[\"metre\",1],"
           "axisminvalue[-1000],axismaxvalue[10000],rangemeaning[exact]],\n"
           "  usage[scope[\"3D GPS\"],area[\"World\"],bbox[-90,-180,90,180]],\n"
           "  id[\"EPSG\",4979],\n"
           "  remark[\"\"]]\r\n\t ",
       .expected_type = PJ_TYPE_GEOGRAPHIC_3D_CRS},
      {.label = "ProjCrsWithParenthesesAndScientificNotation",
       .wkt = "PROJCRS(\"Custom Mercator\","
              "BASEGEOGCRS(\"WGS 84\","
              "DATUM(\"World Geodetic System 1984\","
              "ELLIPSOID(\"WGS 84\",6378137,298.257223563,"
              "LENGTHUNIT(\"metre\",1)))),"
              "CONVERSION(\"Mercator\","
              "METHOD(\"Mercator (variant A)\",ID(\"EPSG\",9804)),"
              "PARAMETER(\"Latitude of natural origin\",+0.0E+0,"
              "ANGLEUNIT(\"degree\",1.74532925199433E-2)),"
              "PARAMETER(\"Longitude of natural origin\",-0.0E+0,"
              "ANGLEUNIT(\"degree\",0.0174532925199433)),"
              "PARAMETER(\"Scale factor at natural origin\",1.0E+0,"
              "SCALEUNIT(\"unity\",1)),"
              "PARAMETER(\"False easting\",0,LENGTHUNIT(\"metre\",1)),"
              "PARAMETER(\"False northing\",0,LENGTHUNIT(\"metre\",1))),"
              "CS(Cartesian,2),"
              "AXIS(\"Easting (E)\",east,ORDER(1)),"
              "AXIS(\"Northing (N)\",north,ORDER(2)),"
              "LENGTHUNIT(\"metre\",1),"
              "ID(\"EPSG\",32600,\"v1.0\",CITATION(\"OGP\"),"
              "URI(\"urn:ogc:def:crs:EPSG::32600\")))",
       .expected_type = PJ_TYPE_PROJECTED_CRS},
      {.label = "GeocentricGeodCrsWithDynamicFrame",
       .wkt = "GEODCRS[\"ITRF2014\","
              "DYNAMIC[FRAMEEPOCH[2010.0],MODEL[\"HTDP\"]],\n"
              "DATUM[\"International Terrestrial Reference Frame 2014\","
              "ELLIPSOID[\"GRS 1980\",6378137,298.257222101,"
              "LENGTHUNIT[\"metre\",1]]],\n"
              "CS[Cartesian,3],\n"
              "AXIS[\"(X)\",geocentricX,ORDER[1]],\n"
              "AXIS[\"(Y)\",geocentricY,ORDER[2]],\n"
              "AXIS[\"(Z)\",geocentricZ,ORDER[3]],\n"
              "LENGTHUNIT[\"metre\",1]]",
       .expected_type = PJ_TYPE_GEOCENTRIC_CRS},
      {.label = "GeogCrsWithDatumEnsemble",
       .wkt = "GEOGCRS[\"WGS 84\","
              "ENSEMBLE[\"World Geodetic System 1984 ensemble\","
              "MEMBER[\"World Geodetic System 1984 (Transit)\"],"
              "MEMBER[\"World Geodetic System 1984 (G2139)\"],"
              "ELLIPSOID[\"WGS 84\",6378137,298.257223563,"
              "LENGTHUNIT[\"metre\",1]],"
              "ENSEMBLEACCURACY[2.0]],"
              "CS[ellipsoidal,2],"
              "AXIS[\"latitude\",north,ORDER[1]],"
              "AXIS[\"longitude\",east,ORDER[2]],"
              "ANGLEUNIT[\"degree\",0.0174532925199433]]",
       .expected_type = PJ_TYPE_GEOGRAPHIC_2D_CRS},
      {.label = "VertCrsWithVerticalExtent",
       .wkt = "VERTCRS[\"Mean Sea Level height\","
              "VDATUM[\"Mean Sea Level\",ANCHOR[\"Tide "
              "gauge\"],ANCHOREPOCH[2020]],"
              "CS[vertical,1],"
              "AXIS[\"gravity-related height (H)\",up],"
              "LENGTHUNIT[\"metre\",1],"
              "USAGE[SCOPE[\"Topographic mapping\"],AREA[\"World\"],"
              "VERTICALEXTENT[-100,8850,LENGTHUNIT[\"metre\",1]]]]",
       .expected_type = PJ_TYPE_VERTICAL_CRS},
      {.label = "CompoundCrsGeog2DPlusVert",
       .wkt = "COMPOUNDCRS[\"WGS 84 + MSL height\","
              "GEOGCRS[\"WGS 84\","
              "DATUM[\"World Geodetic System 1984\","
              "ELLIPSOID[\"WGS 84\",6378137,298.257223563,"
              "LENGTHUNIT[\"metre\",1]]],"
              "CS[ellipsoidal,2],"
              "AXIS[\"latitude\",north,ORDER[1]],"
              "AXIS[\"longitude\",east,ORDER[2]],"
              "ANGLEUNIT[\"degree\",0.0174532925199433]],"
              "VERTCRS[\"MSL height\","
              "VDATUM[\"Mean Sea Level\"],"
              "CS[vertical,1],"
              "AXIS[\"height\",up],"
              "LENGTHUNIT[\"metre\",1]]]",
       .expected_type = PJ_TYPE_COMPOUND_CRS},
      {.label = "DerivedProjCrsWithAffineConversion",
       .wkt = "DERIVEDPROJCRS[\"Derived Grid\","
              "BASEPROJCRS[\"UTM zone 32N\","
              "BASEGEOGCRS[\"WGS 84\","
              "DATUM[\"World Geodetic System 1984\","
              "ELLIPSOID[\"WGS 84\",6378137,298.257223563,"
              "LENGTHUNIT[\"metre\",1]]]],"
              "CONVERSION[\"UTM zone 32N\","
              "METHOD[\"Transverse Mercator\",ID[\"EPSG\",9807]],"
              "PARAMETER[\"Latitude of natural origin\",0,"
              "ANGLEUNIT[\"degree\",0.0174532925199433]],"
              "PARAMETER[\"Longitude of natural origin\",9,"
              "ANGLEUNIT[\"degree\",0.0174532925199433]],"
              "PARAMETER[\"Scale factor at natural origin\",0.9996,"
              "SCALEUNIT[\"unity\",1]],"
              "PARAMETER[\"False easting\",500000,LENGTHUNIT[\"metre\",1]],"
              "PARAMETER[\"False northing\",0,LENGTHUNIT[\"metre\",1]]]],"
              "DERIVINGCONVERSION[\"Affine Shift\","
              "METHOD[\"Affine parametric transformation\",ID[\"EPSG\",9624]],"
              "PARAMETER[\"A0\",100,LENGTHUNIT[\"metre\",1]],"
              "PARAMETER[\"A1\",1,SCALEUNIT[\"unity\",1]],"
              "PARAMETER[\"A2\",0,SCALEUNIT[\"unity\",1]],"
              "PARAMETER[\"B0\",200,LENGTHUNIT[\"metre\",1]],"
              "PARAMETER[\"B1\",0,SCALEUNIT[\"unity\",1]],"
              "PARAMETER[\"B2\",1,SCALEUNIT[\"unity\",1]]],"
              "CS[Cartesian,2],"
              "AXIS[\"X\",east,ORDER[1]],"
              "AXIS[\"Y\",north,ORDER[2]],"
              "LENGTHUNIT[\"metre\",1]]",
       .expected_type = PJ_TYPE_DERIVED_PROJECTED_CRS},
      {.label = "BoundCrsWithAbridgedTransformation",
       .wkt = "BOUNDCRS["
              "SOURCECRS[GEOGCRS[\"Local Geog\","
              "DATUM[\"Local Datum\","
              "ELLIPSOID[\"Bessel 1841\",6377397.155,299.1528128,"
              "LENGTHUNIT[\"metre\",1]]],"
              "CS[ellipsoidal,2],"
              "AXIS[\"latitude\",north,ORDER[1]],"
              "AXIS[\"longitude\",east,ORDER[2]],"
              "ANGLEUNIT[\"degree\",0.0174532925199433]]],"
              "TARGETCRS[GEOGCRS[\"WGS 84\","
              "DATUM[\"World Geodetic System 1984\","
              "ELLIPSOID[\"WGS 84\",6378137,298.257223563,"
              "LENGTHUNIT[\"metre\",1]]],"
              "CS[ellipsoidal,2],"
              "AXIS[\"latitude\",north,ORDER[1]],"
              "AXIS[\"longitude\",east,ORDER[2]],"
              "ANGLEUNIT[\"degree\",0.0174532925199433]]],"
              "ABRIDGEDTRANSFORMATION[\"Local to WGS84\","
              "METHOD[\"Geocentric translations (geog2D domain)\","
              "ID[\"EPSG\",9603]],"
              "PARAMETER[\"X-axis translation\",10,ID[\"EPSG\",8605]],"
              "PARAMETER[\"Y-axis translation\",20,ID[\"EPSG\",8606]],"
              "PARAMETER[\"Z-axis translation\",30,ID[\"EPSG\",8607]]]]",
       .expected_type = PJ_TYPE_BOUND_CRS},
      {.label = "EngineeringCrsWithNorthEastAndNorthWestAxes",
       .wkt = "ENGCRS[\"Plant Grid\","
              "EDATUM[\"Plant Origin\",ANCHOR[\"Benchmark 1\"]],"
              "CS[Cartesian,2],"
              "AXIS[\"x\",northEast,ORDER[1]],"
              "AXIS[\"y\",northWest,ORDER[2]],"
              "LENGTHUNIT[\"metre\",1]]",
       .expected_type = PJ_TYPE_ENGINEERING_CRS},
      {.label = "ParametricCrsWithParametricUnit",
       .wkt = "PARAMETRICCRS[\"Isobaric Pressure\","
              "PDATUM[\"Mean Sea Level Pressure\"],"
              "CS[parametric,1],"
              "AXIS[\"Pressure (p)\",unspecified],"
              "PARAMETRICUNIT[\"hPa\",100]]",
       .expected_type = PJ_TYPE_OTHER_CRS},
      {.label = "TemporalCrsWithIso8601TimeOriginAndExtent",
       .wkt = "TIMECRS[\"Unix Seconds\","
              "TDATUM[\"Unix epoch\",CALENDAR[\"prolepticGregorian\"],"
              "TIMEORIGIN[1970-01-01T00:00:00Z]],"
              "CS[TemporalMeasure,1],"
              "AXIS[\"Time (t)\",future,TIMEUNIT[\"second\",1]],"
              "USAGE[SCOPE[\"Temporal referencing\"],AREA[\"World\"],"
              "TIMEEXTENT[2000-01-01T00:00:00Z,2030-12-31T23:59:59Z]]]",
       .expected_type = PJ_TYPE_TEMPORAL_CRS},
      {.label = "CoordinateMetadataWithDynamicCrsAndEpoch",
       .wkt = "COORDINATEMETADATA["
              "GEOGCRS[\"ITRF2014\","
              "DYNAMIC[FRAMEEPOCH[2010.0]],"
              "DATUM[\"ITRF2014\","
              "ELLIPSOID[\"GRS 1980\",6378137,298.257222101,"
              "LENGTHUNIT[\"metre\",1]]],"
              "CS[ellipsoidal,2],"
              "AXIS[\"latitude\",north,ORDER[1]],"
              "AXIS[\"longitude\",east,ORDER[2]],"
              "ANGLEUNIT[\"degree\",0.0174532925199433]],"
              "EPOCH[2020.5]]",
       .expected_type = PJ_TYPE_COORDINATE_METADATA},
  }};

  for (const auto& tc : kValidCases) {
    SCOPED_TRACE(tc.label);
    const std::string owned_wkt(tc.wkt);
    EXPECT_THAT(pj_wkt2_parse(owned_wkt), IsEmpty());

    PROJ_STRING_LIST raw_warnings = nullptr;
    PROJ_STRING_LIST raw_grammar_errors = nullptr;
    ScopedPj obj(proj_create_from_wkt(ctx.get(), owned_wkt.c_str(), nullptr,
                                      &raw_warnings, &raw_grammar_errors));
    ScopedStringList warnings(raw_warnings);
    ScopedStringList grammar_errors(raw_grammar_errors);
    EXPECT_EQ(nullptr, grammar_errors.get());
    ASSERT_NE(nullptr, obj.get());
    EXPECT_EQ(tc.expected_type, proj_get_type(obj.get()));
  }
}

TEST(Wkt2ParserTest, PrintedCurlyQuotesParseInBothLexerAndWktImporter) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // OGC 12-063r5 printed curly quotes (U+201C ... U+201D) trigger the special
  // printed-quote scanning path in pj_wkt2_lex.
  const std::string curly_wkt =
      "GEOGCRS[\xE2\x80\x9C"
      "WGS 84\xE2\x80\x9D,"
      "DATUM[\xE2\x80\x9C"
      "World Geodetic System 1984\xE2\x80\x9D,"
      "ELLIPSOID[\xE2\x80\x9C"
      "WGS 84\xE2\x80\x9D,6378137,298.257223563,"
      "LENGTHUNIT[\xE2\x80\x9C"
      "metre\xE2\x80\x9D,1]]],"
      "CS[ellipsoidal,2],"
      "AXIS[\xE2\x80\x9C"
      "latitude\xE2\x80\x9D,north,ORDER[1]],"
      "AXIS[\xE2\x80\x9C"
      "longitude\xE2\x80\x9D,east,ORDER[2]],"
      "ANGLEUNIT[\xE2\x80\x9C"
      "degree\xE2\x80\x9D,0.0174532925199433]]";

  EXPECT_THAT(pj_wkt2_parse(curly_wkt), IsEmpty());

  PROJ_STRING_LIST raw_warnings = nullptr;
  PROJ_STRING_LIST raw_grammar_errors = nullptr;
  ScopedPj obj(proj_create_from_wkt(ctx.get(), curly_wkt.c_str(), nullptr,
                                    &raw_warnings, &raw_grammar_errors));
  ScopedStringList warnings(raw_warnings);
  ScopedStringList grammar_errors(raw_grammar_errors);
  EXPECT_EQ(nullptr, grammar_errors.get());
  ASSERT_NE(nullptr, obj.get());
  EXPECT_EQ(PJ_TYPE_GEOGRAPHIC_2D_CRS, proj_get_type(obj.get()));
}

TEST(Wkt2ParserTest, CoordinateAndConcatenatedOperationsParseCleanly) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  const std::string coord_op_wkt =
      "COORDINATEOPERATION[\"Paris to Greenwich\","
      "SOURCECRS[GEOGCRS[\"NTF (Paris)\","
      "DATUM[\"Nouvelle Triangulation Francaise (Paris)\","
      "ELLIPSOID[\"Clarke 1880 (IGN)\",6378249.2,293.466021293627,"
      "LENGTHUNIT[\"metre\",1]]],"
      "PRIMEM[\"Paris\",2.5969213,"
      "ANGLEUNIT[\"grad\",0.015707963267948967]],"
      "CS[ellipsoidal,2],"
      "AXIS[\"latitude\",north,ORDER[1]],"
      "AXIS[\"longitude\",east,ORDER[2]],"
      "ANGLEUNIT[\"grad\",0.015707963267948967]]],"
      "TARGETCRS[GEOGCRS[\"NTF\","
      "DATUM[\"Nouvelle Triangulation Francaise\","
      "ELLIPSOID[\"Clarke 1880 (IGN)\",6378249.2,293.466021293627,"
      "LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],"
      "AXIS[\"latitude\",north,ORDER[1]],"
      "AXIS[\"longitude\",east,ORDER[2]],"
      "ANGLEUNIT[\"degree\",0.0174532925199433]]],"
      "METHOD[\"Longitude rotation\",ID[\"EPSG\",9601]],"
      "PARAMETER[\"Longitude offset\",2.33722917,"
      "ANGLEUNIT[\"degree\",0.0174532925199433]],"
      "OPERATIONACCURACY[0.0]]";

  EXPECT_THAT(pj_wkt2_parse(coord_op_wkt), IsEmpty());

  PROJ_STRING_LIST raw_warnings = nullptr;
  PROJ_STRING_LIST raw_grammar_errors = nullptr;
  ScopedPj op(proj_create_from_wkt(ctx.get(), coord_op_wkt.c_str(), nullptr,
                                   &raw_warnings, &raw_grammar_errors));
  ScopedStringList warnings(raw_warnings);
  ScopedStringList grammar_errors(raw_grammar_errors);
  EXPECT_EQ(nullptr, grammar_errors.get());
  ASSERT_NE(nullptr, op.get());
  EXPECT_EQ(PJ_TYPE_TRANSFORMATION, proj_get_type(op.get()));

  const std::string concat_op_wkt =
      "CONCATENATEDOPERATION[\"Two-step rotation\","
      "SOURCECRS[GEOGCRS[\"CRS A\","
      "DATUM[\"Datum A\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],AXIS[\"latitude\",north,ORDER[1]],"
      "AXIS[\"longitude\",east,ORDER[2]],"
      "ANGLEUNIT[\"degree\",0.0174532925199433]]],"
      "TARGETCRS[GEOGCRS[\"CRS C\","
      "DATUM[\"Datum C\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],AXIS[\"latitude\",north,ORDER[1]],"
      "AXIS[\"longitude\",east,ORDER[2]],"
      "ANGLEUNIT[\"degree\",0.0174532925199433]]],"
      "STEP[COORDINATEOPERATION[\"Step 1\","
      "SOURCECRS[GEOGCRS[\"CRS A\","
      "DATUM[\"Datum A\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],AXIS[\"latitude\",north,ORDER[1]],"
      "AXIS[\"longitude\",east,ORDER[2]],"
      "ANGLEUNIT[\"degree\",0.0174532925199433]]],"
      "TARGETCRS[GEOGCRS[\"CRS B\","
      "DATUM[\"Datum B\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],AXIS[\"latitude\",north,ORDER[1]],"
      "AXIS[\"longitude\",east,ORDER[2]],"
      "ANGLEUNIT[\"degree\",0.0174532925199433]]],"
      "METHOD[\"Longitude rotation\",ID[\"EPSG\",9601]],"
      "PARAMETER[\"Longitude offset\",1.0,"
      "ANGLEUNIT[\"degree\",0.0174532925199433]]]],"
      "STEP[COORDINATEOPERATION[\"Step 2\","
      "SOURCECRS[GEOGCRS[\"CRS B\","
      "DATUM[\"Datum B\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],AXIS[\"latitude\",north,ORDER[1]],"
      "AXIS[\"longitude\",east,ORDER[2]],"
      "ANGLEUNIT[\"degree\",0.0174532925199433]]],"
      "TARGETCRS[GEOGCRS[\"CRS C\","
      "DATUM[\"Datum C\","
      "ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],AXIS[\"latitude\",north,ORDER[1]],"
      "AXIS[\"longitude\",east,ORDER[2]],"
      "ANGLEUNIT[\"degree\",0.0174532925199433]]],"
      "METHOD[\"Longitude rotation\",ID[\"EPSG\",9601]],"
      "PARAMETER[\"Longitude offset\",2.0,"
      "ANGLEUNIT[\"degree\",0.0174532925199433]]]],"
      "OPERATIONACCURACY[0.5]]";

  EXPECT_THAT(pj_wkt2_parse(concat_op_wkt), IsEmpty());

  PROJ_STRING_LIST raw_concat_warnings = nullptr;
  PROJ_STRING_LIST raw_concat_errors = nullptr;
  ScopedPj concat_op(proj_create_from_wkt(ctx.get(), concat_op_wkt.c_str(),
                                          nullptr, &raw_concat_warnings,
                                          &raw_concat_errors));
  ScopedStringList concat_warnings(raw_concat_warnings);
  ScopedStringList concat_errors(raw_concat_errors);
  EXPECT_EQ(nullptr, concat_errors.get());
  ASSERT_NE(nullptr, concat_op.get());
  EXPECT_EQ(PJ_TYPE_CONCATENATED_OPERATION, proj_get_type(concat_op.get()));
}

TEST(Wkt2ParserTest, DatabaseCrsWkt2RoundTripPassesGrammar) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  constexpr std::array<const char*, 5> kCodes = {
      "EPSG:4326", "EPSG:4978", "EPSG:4979", "EPSG:32632", "EPSG:7405",
  };

  for (const char* code : kCodes) {
    SCOPED_TRACE(code);
    ScopedPj crs(proj_create(ctx.get(), code));
    ASSERT_NE(nullptr, crs.get());

    const char* wkt2019 =
        proj_as_wkt(ctx.get(), crs.get(), PJ_WKT2_2019, nullptr);
    ASSERT_NE(nullptr, wkt2019);
    const std::string owned_2019(wkt2019);
    EXPECT_THAT(pj_wkt2_parse(owned_2019), IsEmpty());

    const char* wkt2015 =
        proj_as_wkt(ctx.get(), crs.get(), PJ_WKT2_2015, nullptr);
    ASSERT_NE(nullptr, wkt2015);
    const std::string owned_2015(wkt2015);
    EXPECT_THAT(pj_wkt2_parse(owned_2015), IsEmpty());

    PROJ_STRING_LIST raw_warnings = nullptr;
    PROJ_STRING_LIST raw_grammar_errors = nullptr;
    ScopedPj reparsed(proj_create_from_wkt(ctx.get(), owned_2019.c_str(),
                                           nullptr, &raw_warnings,
                                           &raw_grammar_errors));
    ScopedStringList warnings(raw_warnings);
    ScopedStringList grammar_errors(raw_grammar_errors);
    EXPECT_EQ(nullptr, grammar_errors.get());
    EXPECT_NE(nullptr, reparsed.get());
  }
}

TEST(Wkt2ParserTest, LexerAndSyntaxErrorsInvokeErrorHandlerWithCaret) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());

  struct ErrorCase {
    std::string_view label;
    std::string_view wkt;
  };

  constexpr std::array<ErrorCase, 8> kErrorCases = {{
      {.label = "EmptyInput", .wkt = ""},
      {.label = "WhitespaceOnly", .wkt = " \t\n\r "},
      {.label = "UnknownKeyword", .wkt = "FOOBAR[\"WGS 84\"]"},
      {.label = "KnownKeywordFollowedByAlpha", .wkt = "GEOGCRSX[\"WGS 84\"]"},
      {.label = "UnterminatedDoubleQuote",
       .wkt = "GEOGCRS[\"unterminated \"\"quote"},
      {.label = "UnterminatedPrintedCurlyQuote",
       .wkt = "GEOGCRS[\xE2\x80\x9Cunclosed"},
      {.label = "UnexpectedPunctuationCharacter", .wkt = "GEOGCRS[@]"},
      {.label = "MultilineMissingRequiredChildren",
       .wkt = "GEOGCRS[\"WGS 84\",\n  DATUM[\"World Geodetic System 1984\"]]"},
  }};

  for (const auto& tc : kErrorCases) {
    SCOPED_TRACE(tc.label);
    const std::string err = pj_wkt2_parse(std::string(tc.wkt));
    EXPECT_THAT(err, Not(IsEmpty()));
    EXPECT_THAT(err, HasSubstr("Parsing error"));
    EXPECT_THAT(err, HasSubstr("^"));
  }

  // Verify proj_create_from_wkt surfaces pj_wkt2_parse diagnostics via
  // out_grammar_errors when parsing syntactically invalid WKT2.
  const char* const kStrictOptions[] = {"STRICT=YES", nullptr};
  const char* bad_wkt2 =
      "GEOGCRS[\"Bad\",DATUM[\"Unknown\","
      "ELLIPSOID[\"Sphere\",6371000,0,LENGTHUNIT[\"metre\",1]]],"
      "CS[ellipsoidal,2],AXIS[\"lat\",north,ORDER[1]],"
      "AXIS[\"lon\",east,ORDER[2]],ANGLEUNIT[\"degree\",0.0174533],"
      "BAD_NODE[\"foo\"]]";
  PROJ_STRING_LIST raw_warnings = nullptr;
  PROJ_STRING_LIST raw_grammar_errors = nullptr;
  ScopedPj bad_obj(proj_create_from_wkt(ctx.get(), bad_wkt2, kStrictOptions,
                                        &raw_warnings, &raw_grammar_errors));
  ScopedStringList warnings(raw_warnings);
  ScopedStringList grammar_errors(raw_grammar_errors);
  EXPECT_EQ(nullptr, bad_obj.get());
  ASSERT_NE(nullptr, grammar_errors.get());
  ASSERT_NE(nullptr, grammar_errors.get()[0]);
  EXPECT_THAT(grammar_errors.get()[0], HasSubstr("Parsing error"));
}

}  // namespace
}  // namespace proj
