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

// Tests for the GeoJSON collection streaming parser.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/geojson/ogrjsoncollectionstreamingparser.cpp
// third_party/gdal/ogr/ogrsf_frmts/geojson/ogrjsoncollectionstreamingparser.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/geojson/ogrjsoncollectionstreamingparser.h"

#include <json.h>

#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

class TestStreamingParser : public OGRJSONCollectionStreamingParser {
 public:
  TestStreamingParser(bool bFirstPass, bool bStoreNativeData,
                      size_t nMaxObjectSize = 0)
      : OGRJSONCollectionStreamingParser(bFirstPass, bStoreNativeData,
                                         nMaxObjectSize) {}

  void GotFeature(json_object* poObj, bool bFirstPass,
                  const std::string& osJson) override {
    nGotFeatureCalls++;
    bLastFirstPass = bFirstPass;
    osLastJson = osJson;
    if (poObj != nullptr) {
      json_object_get(poObj);  // Keep reference
      apoFeatures.push_back(poObj);
    }
  }

  void TooComplex() override { nTooComplexCalls++; }

  void Exception(const char* pszMessage) override {
    OGRJSONCollectionStreamingParser::Exception(pszMessage);
    osLastException = pszMessage ? pszMessage : "";
  }

  ~TestStreamingParser() override {
    for (auto* poObj : apoFeatures) {
      json_object_put(poObj);
    }
  }

  int nGotFeatureCalls = 0;
  bool bLastFirstPass = false;
  std::string osLastJson{};
  int nTooComplexCalls = 0;
  std::string osLastException{};
  std::vector<json_object*> apoFeatures{};
};

TEST(OGRJSONCollectionStreamingParserTest,
     NormalFeatureCollectionFirstAndSecondPass) {
  const std::string kJson = R"json({
    "type": "FeatureCollection",
    "name": "test_collection",
    "int_val": 42,
    "float_val": 3.1415,
    "bool_val": true,
    "null_val": null,
    "nested_obj": {"k": "v"},
    "nested_arr": [1, 2],
    "measures": {"enabled": true},
    "features": [
      {
        "type": "Feature",
        "id": 101,
        "properties": {
          "str": "hello \t\r\n\b\f\"\\ \u0001 world",
          "int": -12345,
          "uint": 67890,
          "float": 1.234e-5,
          "big": 12345678901234567890.12345,
          "bool_f": false,
          "bool_t": true,
          "nil": null,
          "arr": [10, "item"],
          "sub": {"subk": "subv"}
        },
        "geometry": {
          "type": "GeometryCollection",
          "geometries": [
            {
              "type": "Point",
              "coordinates": [1.5, 2.5]
            }
          ]
        }
      }
    ]
  })json";

  // First pass with native data stored
  {
    TestStreamingParser parser(true, true, 0);
    EXPECT_TRUE(parser.Parse(kJson, true));
    EXPECT_TRUE(parser.IsTypeKnown());
    EXPECT_TRUE(parser.IsFeatureCollection());
    EXPECT_TRUE(parser.HasTopLevelMeasures());
    EXPECT_TRUE(parser.CanEasilyAppend());
    EXPECT_GT(parser.GetTotalOGRFeatureMemEstimate(), 0);
    EXPECT_EQ(parser.nGotFeatureCalls, 1);
    EXPECT_FALSE(parser.osLastJson.empty());

    // Root object was kept in first pass
    json_object* poRoot = parser.StealRootObject();
    ASSERT_NE(poRoot, nullptr);
    json_object_put(poRoot);

    // After StealRootObject, subsequent steal returns nullptr
    EXPECT_EQ(parser.StealRootObject(), nullptr);
  }

  // Second pass with native data stored
  {
    TestStreamingParser parser(false, true, 0);
    EXPECT_TRUE(parser.Parse(kJson, true));
    EXPECT_EQ(parser.nGotFeatureCalls, 1);
    EXPECT_EQ(parser.apoFeatures.size(), 1);
    EXPECT_FALSE(parser.osLastJson.empty());
  }

  // First pass without stealing root object (tests destructor cleanup of
  // m_poRootObj)
  {
    TestStreamingParser parser(true, false, 0);
    EXPECT_TRUE(parser.Parse(kJson, true));
    // Do not call StealRootObject() -> destructor frees m_poRootObj
  }
}

TEST(OGRJSONCollectionStreamingParserTest, NonFeatureCollection) {
  // Point object
  const std::string kPointJson = R"json({
    "type": "Point",
    "coordinates": [10.0, 20.0]
  })json";

  TestStreamingParser parser(true, false, 0);
  EXPECT_TRUE(parser.Parse(kPointJson, true));
  EXPECT_TRUE(parser.IsTypeKnown());
  EXPECT_FALSE(parser.IsFeatureCollection());

  // Non-string "type"
  const std::string kBadTypeJson = R"json({
    "type": 12345
  })json";
  TestStreamingParser parser2(true, false, 0);
  EXPECT_TRUE(parser2.Parse(kBadTypeJson, true));
  EXPECT_FALSE(parser2.IsTypeKnown());
}

TEST(OGRJSONCollectionStreamingParserTest, FeatureDetectionState) {
  TestStreamingParser parser(true, false, 0);
  parser.ResetFeatureDetectionState();
  EXPECT_FALSE(parser.IsStartFeature());
  EXPECT_FALSE(parser.IsEndFeature());

  const std::string kJson = R"json({
    "type": "FeatureCollection",
    "features": [
      {"type": "Feature", "geometry": null}
    ]
  })json";

  // Feed in data
  EXPECT_TRUE(parser.Parse(kJson, true));
  EXPECT_FALSE(parser.ExceptionOccurred());
  // After parsing completes, check reset
  parser.ResetFeatureDetectionState();
  EXPECT_FALSE(parser.IsStartFeature());
  EXPECT_FALSE(parser.IsEndFeature());
}

TEST(OGRJSONCollectionStreamingParserTest, SpecialNumberValues) {
  // Call Number directly in an array context to exercise NaN, Inf, and invalid
  // cases
  TestStreamingParser parser(true, true, 0);
  parser.StartObject();
  parser.StartObjectMember("features");
  parser.StartArray();
  parser.StartArrayMember();
  parser.StartObject();
  parser.StartObjectMember("coordinates");
  parser.StartArray();

  parser.StartArrayMember();
  parser.Number("Infinity");

  parser.StartArrayMember();
  parser.Number("-Infinity");

  parser.StartArrayMember();
  parser.Number("+Infinity");

  parser.StartArrayMember();
  parser.Number("NaN");

  parser.StartArrayMember();
  parser.Number("-nan");

  parser.StartArrayMember();
  parser.Number("nan");

  // Invalid numbers triggering EmitException
  WithQuietHandler quiet;
  parser.StartArrayMember();
  parser.Number("123bad_int");

  parser.StartArrayMember();
  parser.Number("1.23bad_float");

  parser.StartArrayMember();
  parser.Number("12345678901234567890.1bad_long_float");

  CPLErrorReset();

  parser.EndArray();
  parser.EndObject();
  parser.EndArray();
  parser.EndObject();
  EXPECT_TRUE(parser.ExceptionOccurred());
}

TEST(OGRJSONCollectionStreamingParserTest, ComplexityLimits) {
  // Test nMaxObjectSize = 1 triggering TooComplex in various callbacks
  {
    TestStreamingParser parser(true, true, 1);
    parser.StartObject();
    parser.StartObjectMember(
        "first");  // Adds ESTIMATE_OBJECT_ELT_SIZE (32 > 1)
    parser.StartObjectMember("second");
    EXPECT_GT(parser.nTooComplexCalls, 0);
  }

  {
    TestStreamingParser parser(true, true, 1);
    parser.StartObject();
    parser.StartObjectMember("first");
    parser.String("val");
    EXPECT_GT(parser.nTooComplexCalls, 0);
  }

  {
    TestStreamingParser parser(true, true, 1);
    parser.StartObject();
    parser.StartObjectMember("features");
    parser.StartArray();
    parser.StartArrayMember();
    parser.StartObject();
    parser.StartObjectMember("prop");
    parser.StartObject();  // Depth >= 2 inside features with nMaxObjectSize = 1
    EXPECT_GT(parser.nTooComplexCalls, 0);
  }

  {
    TestStreamingParser parser(true, true, 1);
    parser.StartObject();
    parser.StartObjectMember("features");
    parser.StartArray();
    parser.StartArrayMember();
    parser.StartObject();
    parser.StartObjectMember("arr");
    parser.StartArray();  // Array inside features with nMaxObjectSize = 1
    EXPECT_GT(parser.nTooComplexCalls, 0);
  }

  {
    TestStreamingParser parser(true, true, 1);
    parser.StartObject();
    parser.StartObjectMember("features");
    parser.StartArray();
    parser.StartArrayMember();
    parser.StartObject();
    parser.StartObjectMember("num");
    parser.Number("123456789");
    EXPECT_GT(parser.nTooComplexCalls, 0);
  }

  {
    TestStreamingParser parser(true, true, 1);
    parser.StartObject();
    parser.StartObjectMember("features");
    parser.StartArray();
    parser.StartArrayMember();
    parser.StartObject();
    parser.StartObjectMember("bool");
    parser.Boolean(true);
    EXPECT_GT(parser.nTooComplexCalls, 0);
  }

  {
    TestStreamingParser parser(true, true, 1);
    parser.StartObject();
    parser.StartObjectMember("features");
    parser.StartArray();
    parser.StartArrayMember();
    parser.StartObject();
    parser.StartObjectMember("nil");
    parser.Null();
    EXPECT_GT(parser.nTooComplexCalls, 0);
  }

  // EndObject triggering TooComplex when finishing feature and osJson exceeds
  // nMaxObjectSize
  {
    TestStreamingParser parser(true, true, 2);
    parser.StartObject();
    parser.StartObjectMember("features");
    parser.StartArray();
    parser.StartArrayMember();
    parser.StartObject();
    parser.StartObjectMember("a");
    parser.String("b");
    parser.EndObject();
    EXPECT_GT(parser.nTooComplexCalls, 0);
  }

  // Non-featurecollection object exceeding memory limit
  {
    TestStreamingParser parser(true, false, 5);
    parser.StartObject();
    parser.StartObjectMember("type");
    parser.String("NotFeatureCollection");
    parser.StartObjectMember("sub");
    parser.StartObject();
    parser.StartObjectMember("sub2");
    parser.StartObject();
    EXPECT_GT(parser.nTooComplexCalls, 0);
  }
}

TEST(OGRJSONCollectionStreamingParserTest, MeasuresDisabled) {
  const std::string kJson = R"json({
    "type": "FeatureCollection",
    "measures": {"enabled": false},
    "features": []
  })json";

  TestStreamingParser parser(true, false, 0);
  EXPECT_TRUE(parser.Parse(kJson, true));
  EXPECT_FALSE(parser.HasTopLevelMeasures());
}

}  // namespace
}  // namespace autotest2
