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
// Unit tests for ISO-19111 common classes (UnitOfMeasure, Measure, Scale,
// Angle, Length, DateTime, IdentifiedObject, ObjectDomain, ObjectUsage,
// DataEpoch) in proj/src/iso19111/common.cpp. Note: in google3 default
// non-GCE builds, proj.db is embedded in memory and networking is disabled.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/iso19111/common.cpp

#include "third_party/proj4/proj/include/proj/common.hpp"

#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/include/proj/coordinates.hpp"
#include "third_party/proj4/proj/include/proj/crs.hpp"
#include "third_party/proj4/proj/include/proj/datum.hpp"
#include "third_party/proj4/proj/include/proj/io.hpp"
#include "third_party/proj4/proj/include/proj/metadata.hpp"
#include "third_party/proj4/proj/include/proj/nn.hpp"
#include "third_party/proj4/proj/include/proj/util.hpp"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_json_streaming_writer.hpp"

namespace proj {
namespace {

using ::osgeo::proj::common::Angle;
using ::osgeo::proj::common::DataEpoch;
using ::osgeo::proj::common::DateTime;
using ::osgeo::proj::common::IdentifiedObject;
using ::osgeo::proj::common::Length;
using ::osgeo::proj::common::Measure;
using ::osgeo::proj::common::ObjectDomain;
using ::osgeo::proj::common::ObjectUsage;
using ::osgeo::proj::common::Scale;
using ::osgeo::proj::common::UnitOfMeasure;
using ::osgeo::proj::common::UnitOfMeasureNNPtr;
using ::osgeo::proj::coordinates::CoordinateMetadata;
using ::osgeo::proj::crs::GeographicCRS;
using ::osgeo::proj::datum::Ellipsoid;
using ::osgeo::proj::datum::PrimeMeridian;
using ::osgeo::proj::io::AuthorityFactory;
using ::osgeo::proj::io::DatabaseContext;
using ::osgeo::proj::io::DatabaseContextPtr;
using ::osgeo::proj::io::IJSONExportable;
using ::osgeo::proj::io::JSONFormatter;
using ::osgeo::proj::io::WKTFormatter;
using ::osgeo::proj::metadata::Extent;
using ::osgeo::proj::metadata::ExtentPtr;
using ::osgeo::proj::metadata::GeographicBoundingBox;
using ::osgeo::proj::metadata::GeographicExtent;
using ::osgeo::proj::metadata::GeographicExtentNNPtr;
using ::osgeo::proj::metadata::GeographicExtentPtr;
using ::osgeo::proj::metadata::Identifier;
using ::osgeo::proj::metadata::IdentifierNNPtr;
using ::osgeo::proj::metadata::TemporalExtent;
using ::osgeo::proj::metadata::TemporalExtentNNPtr;
using ::osgeo::proj::metadata::VerticalExtent;
using ::osgeo::proj::metadata::VerticalExtentNNPtr;
using ::osgeo::proj::util::ArrayOfBaseObject;
using ::osgeo::proj::util::BoxedValue;
using ::osgeo::proj::util::GenericName;
using ::osgeo::proj::util::IComparable;
using ::osgeo::proj::util::InvalidValueTypeException;
using ::osgeo::proj::util::NameFactory;
using ::osgeo::proj::util::nn;
using ::osgeo::proj::util::nn_make_shared;
using ::osgeo::proj::util::nn_static_pointer_cast;
using ::osgeo::proj::util::optional;
using ::osgeo::proj::util::PropertyMap;
using ::testing::HasSubstr;
using ::testing::Not;

constexpr double kPi = std::numbers::pi;

void AssignUnit(UnitOfMeasure& dst, const UnitOfMeasure& src) { dst = src; }

std::string UnitToWkt(const UnitOfMeasure& unit,
                      WKTFormatter::Convention convention,
                      const std::string& unit_type = std::string(),
                      bool output_id = true) {
  auto formatter = WKTFormatter::create(convention);
  formatter->setMultiLine(false);
  if (!output_id) {
    formatter->setOutputId(false);
  }
  unit._exportToWKT(formatter.get(), unit_type);
  return formatter->toString();
}

std::string UnitToJson(const UnitOfMeasure& unit) {
  auto formatter = JSONFormatter::create();
  formatter->setMultiLine(false);
  formatter->setSchema("");
  unit._exportToJSON(formatter.get());
  return formatter->toString();
}

std::string DomainToWkt(const ObjectDomain& domain,
                        WKTFormatter::Convention convention) {
  auto formatter = WKTFormatter::create(convention);
  formatter->setMultiLine(false);
  domain._exportToWKT(formatter.get());
  return formatter->toString();
}

std::string DomainToJson(const ObjectDomain& domain) {
  auto formatter = JSONFormatter::create();
  formatter->setMultiLine(false);
  formatter->setSchema("");
  {
    auto obj_ctx = formatter->MakeObjectContext("DomainWrap", false);
    domain._exportToJSON(formatter.get());
  }
  return formatter->toString();
}

IdentifierNNPtr MakeIdentifier(const std::string& auth,
                               const std::string& code) {
  return Identifier::create(code,
                            PropertyMap().set(Identifier::CODESPACE_KEY, auth));
}

// Minimal concrete IdentifiedObject leaving _isEquivalentTo(IComparable*)
// and hasEquivalentNameToUsingAlias unoverridden so base implementations can
// be tested directly.
class TestIdentifiedObject final : public IdentifiedObject {
 public:
  TestIdentifiedObject() = default;
  TestIdentifiedObject(const TestIdentifiedObject& other)
      : IdentifiedObject(other) {}
  ~TestIdentifiedObject() override = default;

  explicit TestIdentifiedObject(const PropertyMap& props) {
    setProperties(props);
  }

  static nn<std::shared_ptr<TestIdentifiedObject>> Create(
      const PropertyMap& props = PropertyMap()) {
    return nn_make_shared<TestIdentifiedObject>(props);
  }

  nn<std::shared_ptr<TestIdentifiedObject>> Clone() const {
    return nn_make_shared<TestIdentifiedObject>(*this);
  }

  void SetPropertiesForTest(const PropertyMap& props) { setProperties(props); }

  void _exportToWKT(WKTFormatter* formatter) const override {
    formatter->startNode("TESTIDOBJ", !identifiers().empty());
    formatter->addQuotedString(nameStr());
    formatID(formatter);
    formatRemarks(formatter);
    formatter->endNode();
  }

  std::string FormatIdAndRemarksJson() const {
    auto formatter = JSONFormatter::create();
    formatter->setMultiLine(false);
    formatter->setSchema("");
    {
      auto obj_ctx = formatter->MakeObjectContext("TestIdObj", false);
      formatID(formatter.get());
      formatRemarks(formatter.get());
    }
    return formatter->toString();
  }
};

// Minimal concrete ObjectUsage exposing protected setProperties,
// baseExportToWKT, baseExportToJSON, copy construction, and optional alias
// equivalence override.
class TestObjectUsage final : public ObjectUsage, public IJSONExportable {
 public:
  TestObjectUsage() = default;
  TestObjectUsage(const TestObjectUsage& other) = default;
  ~TestObjectUsage() override = default;

  explicit TestObjectUsage(
      const PropertyMap& props, bool alias_equiv = false,
      std::shared_ptr<const TestObjectUsage> child = nullptr)
      : alias_equiv_(alias_equiv), child_(std::move(child)) {
    setProperties(props);
  }

  static nn<std::shared_ptr<TestObjectUsage>> Create(
      const PropertyMap& props = PropertyMap(), bool alias_equiv = false,
      std::shared_ptr<const TestObjectUsage> child = nullptr) {
    return nn_make_shared<TestObjectUsage>(props, alias_equiv,
                                           std::move(child));
  }

  nn<std::shared_ptr<TestObjectUsage>> Clone() const {
    return nn_make_shared<TestObjectUsage>(*this);
  }

  void _exportToWKT(WKTFormatter* formatter) const override {
    formatter->startNode("TESTOBJ", !identifiers().empty());
    formatter->addQuotedString(nameStr());
    baseExportToWKT(formatter);
    formatter->endNode();
  }

  void _exportToJSON(JSONFormatter* formatter) const override {
    auto obj_ctx =
        formatter->MakeObjectContext("TestObj", !identifiers().empty());
    formatter->writer()->AddObjKey("name");
    formatter->writer()->Add(nameStr());
    baseExportToJSON(formatter);
    if (child_) {
      formatter->writer()->AddObjKey("child");
      child_->_exportToJSON(formatter);
    }
  }

 protected:
  bool hasEquivalentNameToUsingAlias(
      const IdentifiedObject* other,
      const DatabaseContextPtr& db_context) const override {
    if (alias_equiv_) {
      return true;
    }
    return ObjectUsage::hasEquivalentNameToUsingAlias(other, db_context);
  }

 private:
  bool alias_equiv_ = false;
  std::shared_ptr<const TestObjectUsage> child_;
};

// Exposes ObjectDomain's protected copy constructor.
class TestObjectDomain final : public ObjectDomain {
 public:
  TestObjectDomain(const optional<std::string>& scope_in,
                   const ExtentPtr& extent)
      : ObjectDomain(scope_in, extent) {}
  TestObjectDomain(const TestObjectDomain& other) : ObjectDomain(other) {}
  ~TestObjectDomain() override = default;

  static nn<std::shared_ptr<TestObjectDomain>> Create(
      const optional<std::string>& scope_in, const ExtentPtr& extent) {
    return nn_make_shared<TestObjectDomain>(scope_in, extent);
  }

  nn<std::shared_ptr<TestObjectDomain>> Clone() const {
    return nn_make_shared<TestObjectDomain>(*this);
  }
};

// Non-GeographicBoundingBox GeographicExtent to cover dynamic_cast false
// branches in ObjectDomain export.
class DummyGeographicExtent final : public GeographicExtent {
 public:
  DummyGeographicExtent() = default;
  ~DummyGeographicExtent() override = default;

  bool _isEquivalentTo(const IComparable*, Criterion,
                       const DatabaseContextPtr&) const override {
    return false;
  }
  bool contains(const GeographicExtentNNPtr&) const override { return false; }
  bool intersects(const GeographicExtentNNPtr&) const override { return false; }
  GeographicExtentPtr intersection(
      const GeographicExtentNNPtr&) const override {
    return nullptr;
  }
};

TEST(CommonTest, UnitOfMeasureConstructionCopyMoveAndAccessors) {
  // 1. Default constructor initializes UNKNOWN unit with unit scale 1.0.
  UnitOfMeasure default_unit;
  EXPECT_EQ("", default_unit.name());
  EXPECT_DOUBLE_EQ(1.0, default_unit.conversionToSI());
  EXPECT_EQ(UnitOfMeasure::Type::UNKNOWN, default_unit.type());
  EXPECT_EQ("", default_unit.codeSpace());
  EXPECT_EQ("", default_unit.code());

  // 2. Explicit constructor, copy constructor, UnitOfMeasure::create, copy/move
  // assignment (including self-assignment).
  UnitOfMeasure custom("fathom", 1.8288, UnitOfMeasure::Type::LINEAR, "CUSTOM",
                       "FATH");
  EXPECT_EQ("fathom", custom.name());
  EXPECT_DOUBLE_EQ(1.8288, custom.conversionToSI());
  EXPECT_EQ(UnitOfMeasure::Type::LINEAR, custom.type());
  EXPECT_EQ("CUSTOM", custom.codeSpace());
  EXPECT_EQ("FATH", custom.code());

  UnitOfMeasure copied(custom);
  EXPECT_TRUE(copied == custom);
  EXPECT_FALSE(copied != custom);

  UnitOfMeasureNNPtr created = UnitOfMeasure::create(custom);
  EXPECT_TRUE(*created == custom);

  UnitOfMeasure assigned;
  assigned = custom;
  EXPECT_TRUE(assigned == custom);
  AssignUnit(assigned, assigned);
  EXPECT_EQ("fathom", assigned.name());

  assigned = UnitOfMeasure("kilometre", 1000.0, UnitOfMeasure::Type::LINEAR);
  EXPECT_EQ("kilometre", assigned.name());
  EXPECT_DOUBLE_EQ(1000.0, assigned.conversionToSI());
  EXPECT_TRUE(assigned != custom);

  // 3. Equivalence criteria: STRICT compares names; EQUIVALENT compares SI
  // conversion factors within 1e-10 relative tolerance.
  UnitOfMeasure metre_alias("meter", 1.0, UnitOfMeasure::Type::LINEAR);
  UnitOfMeasure metre_near("metre_near", 1.0 + 1e-12,
                           UnitOfMeasure::Type::LINEAR);
  EXPECT_FALSE(UnitOfMeasure::METRE._isEquivalentTo(
      metre_alias, IComparable::Criterion::STRICT));
  EXPECT_TRUE(UnitOfMeasure::METRE._isEquivalentTo(
      UnitOfMeasure::METRE, IComparable::Criterion::STRICT));
  EXPECT_TRUE(UnitOfMeasure::METRE._isEquivalentTo(
      metre_alias, IComparable::Criterion::EQUIVALENT));
  EXPECT_TRUE(UnitOfMeasure::METRE._isEquivalentTo(
      metre_near, IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(UnitOfMeasure::METRE._isEquivalentTo(
      UnitOfMeasure::FOOT, IComparable::Criterion::EQUIVALENT));

  // 4. Verify predefined static UnitOfMeasure constants and DB lookup.
  EXPECT_EQ(UnitOfMeasure::Type::NONE, UnitOfMeasure::NONE.type());
  EXPECT_EQ(UnitOfMeasure::Type::SCALE, UnitOfMeasure::SCALE_UNITY.type());
  EXPECT_DOUBLE_EQ(1e-6, UnitOfMeasure::PARTS_PER_MILLION.conversionToSI());
  EXPECT_EQ(UnitOfMeasure::Type::SCALE, UnitOfMeasure::PPM_PER_YEAR.type());
  EXPECT_DOUBLE_EQ(0.3048, UnitOfMeasure::FOOT.conversionToSI());
  EXPECT_NEAR(1200.0 / 3937.0, UnitOfMeasure::US_FOOT.conversionToSI(), 1e-15);
  EXPECT_DOUBLE_EQ(1.0, UnitOfMeasure::RADIAN.conversionToSI());
  EXPECT_DOUBLE_EQ(1e-6, UnitOfMeasure::MICRORADIAN.conversionToSI());
  EXPECT_NEAR(kPi / 180.0, UnitOfMeasure::DEGREE.conversionToSI(), 1e-15);
  EXPECT_NEAR(kPi / 180.0 / 3600.0, UnitOfMeasure::ARC_SECOND.conversionToSI(),
              1e-18);
  EXPECT_NEAR(kPi / 200.0, UnitOfMeasure::GRAD.conversionToSI(), 1e-15);
  EXPECT_EQ(UnitOfMeasure::Type::ANGULAR,
            UnitOfMeasure::ARC_SECOND_PER_YEAR.type());
  EXPECT_DOUBLE_EQ(1.0, UnitOfMeasure::SECOND.conversionToSI());
  EXPECT_DOUBLE_EQ(31556925.445, UnitOfMeasure::YEAR.conversionToSI());
  EXPECT_EQ(UnitOfMeasure::Type::LINEAR, UnitOfMeasure::METRE_PER_YEAR.type());

  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  auto db_ctx = DatabaseContext::create(std::string(), {}, ctx.get());
  auto factory = AuthorityFactory::create(db_ctx, "EPSG");
  auto db_metre = factory->createUnitOfMeasure("9001");
  EXPECT_TRUE(*db_metre == UnitOfMeasure::METRE);
}

TEST(CommonTest, UnitOfMeasureExportToWktAllTypesAndDialects) {
  // 1. WKT2 emits typed *UNIT keywords per Type and UNIT for UNKNOWN/NONE.
  EXPECT_EQ("LENGTHUNIT[\"metre\",1,ID[\"EPSG\",9001]]",
            UnitToWkt(UnitOfMeasure::METRE, WKTFormatter::Convention::WKT2));
  EXPECT_THAT(UnitToWkt(UnitOfMeasure::DEGREE, WKTFormatter::Convention::WKT2),
              HasSubstr("ANGLEUNIT[\"degree\",0.017453292519943"));
  EXPECT_EQ(
      "SCALEUNIT[\"unity\",1,ID[\"EPSG\",9201]]",
      UnitToWkt(UnitOfMeasure::SCALE_UNITY, WKTFormatter::Convention::WKT2));
  EXPECT_EQ("TIMEUNIT[\"second\",1,ID[\"EPSG\",1040]]",
            UnitToWkt(UnitOfMeasure::SECOND, WKTFormatter::Convention::WKT2));

  // TIMEUNIT with conversionToSI() == 0.0 omits the factor in WKT2, while WKT1
  // still outputs 0.
  UnitOfMeasure zero_time("calendar month", 0.0, UnitOfMeasure::Type::TIME);
  EXPECT_EQ("TIMEUNIT[\"calendar month\"]",
            UnitToWkt(zero_time, WKTFormatter::Convention::WKT2));
  EXPECT_EQ("UNIT[\"calendar month\",0]",
            UnitToWkt(zero_time, WKTFormatter::Convention::WKT1_GDAL));

  // PARAMETRIC uses PARAMETRICUNIT in both WKT2 and WKT2_SIMPLIFIED (exempt
  // from forceUNITKeyword), and falls back to UNIT in WKT1_GDAL.
  UnitOfMeasure param_numeric("hectopascal", 100.0,
                              UnitOfMeasure::Type::PARAMETRIC, "EPSG", "1054");
  EXPECT_EQ("PARAMETRICUNIT[\"hectopascal\",100,ID[\"EPSG\",1054]]",
            UnitToWkt(param_numeric, WKTFormatter::Convention::WKT2));
  EXPECT_EQ(
      "PARAMETRICUNIT[\"hectopascal\",100,ID[\"EPSG\",1054]]",
      UnitToWkt(param_numeric, WKTFormatter::Convention::WKT2_SIMPLIFIED));
  EXPECT_EQ("UNIT[\"hectopascal\",100,AUTHORITY[\"EPSG\",\"1054\"]]",
            UnitToWkt(param_numeric, WKTFormatter::Convention::WKT1_GDAL));

  // UNKNOWN and NONE fall back to UNIT in WKT2.
  UnitOfMeasure unknown_unit("unknown_u", 2.0, UnitOfMeasure::Type::UNKNOWN,
                             "AUTH", "NON_INT");
  EXPECT_EQ("UNIT[\"unknown_u\",2,ID[\"AUTH\",\"NON_INT\"]]",
            UnitToWkt(unknown_unit, WKTFormatter::Convention::WKT2));
  EXPECT_EQ("UNIT[\"\",1]",
            UnitToWkt(UnitOfMeasure::NONE, WKTFormatter::Convention::WKT2));

  // 2. Explicit unitType overrides keyword; outputId=false suppresses ID.
  EXPECT_EQ("CUSTOMUNIT[\"metre\",1,ID[\"EPSG\",9001]]",
            UnitToWkt(UnitOfMeasure::METRE, WKTFormatter::Convention::WKT2,
                      "CUSTOMUNIT"));
  EXPECT_EQ("CUSTOMUNIT[\"local\",3.5]",
            UnitToWkt(UnitOfMeasure("local", 3.5, UnitOfMeasure::Type::LINEAR),
                      WKTFormatter::Convention::WKT2, "CUSTOMUNIT"));
  EXPECT_EQ("LENGTHUNIT[\"metre\",1]",
            UnitToWkt(UnitOfMeasure::METRE, WKTFormatter::Convention::WKT2,
                      std::string(), /*output_id=*/false));

  // 3. WKT2_SIMPLIFIED forces UNIT for non-PARAMETRIC units.
  EXPECT_EQ("UNIT[\"metre\",1,ID[\"EPSG\",9001]]",
            UnitToWkt(UnitOfMeasure::METRE,
                      WKTFormatter::Convention::WKT2_SIMPLIFIED));

  // 4. WKT1_GDAL emits AUTHORITY with quoted string codes.
  EXPECT_EQ(
      "UNIT[\"metre\",1,AUTHORITY[\"EPSG\",\"9001\"]]",
      UnitToWkt(UnitOfMeasure::METRE, WKTFormatter::Convention::WKT1_GDAL));

  // 5. WKT1_ESRI morphs degree, grad, and metre and leaves other names
  // unchanged without authority nodes.
  EXPECT_THAT(
      UnitToWkt(UnitOfMeasure::DEGREE, WKTFormatter::Convention::WKT1_ESRI),
      HasSubstr("UNIT[\"Degree\",0.017453292519943"));
  EXPECT_THAT(
      UnitToWkt(UnitOfMeasure::GRAD, WKTFormatter::Convention::WKT1_ESRI),
      HasSubstr("UNIT[\"Grad\",0.01570796326794"));
  EXPECT_EQ(
      "UNIT[\"Meter\",1.0]",
      UnitToWkt(UnitOfMeasure::METRE, WKTFormatter::Convention::WKT1_ESRI));
  EXPECT_EQ(
      "UNIT[\"foot\",0.3048]",
      UnitToWkt(UnitOfMeasure::FOOT, WKTFormatter::Convention::WKT1_ESRI));
}

TEST(CommonTest, UnitOfMeasureExportToJsonAndProjString) {
  // 1. JSON export covers all unit types and numeric vs string ID codes.
  EXPECT_EQ(
      "{\"type\":\"LinearUnit\",\"name\":\"metre\",\"conversion_factor\":1,"
      "\"id\":{\"authority\":\"EPSG\",\"code\":9001}}",
      UnitToJson(UnitOfMeasure::METRE));
  EXPECT_THAT(UnitToJson(UnitOfMeasure::DEGREE),
              HasSubstr("\"type\":\"AngularUnit\",\"name\":\"degree\""));
  EXPECT_EQ(
      "{\"type\":\"ScaleUnit\",\"name\":\"unity\",\"conversion_factor\":1,"
      "\"id\":{\"authority\":\"EPSG\",\"code\":9201}}",
      UnitToJson(UnitOfMeasure::SCALE_UNITY));
  EXPECT_EQ(
      "{\"type\":\"TimeUnit\",\"name\":\"second\",\"conversion_factor\":1,"
      "\"id\":{\"authority\":\"EPSG\",\"code\":1040}}",
      UnitToJson(UnitOfMeasure::SECOND));
  EXPECT_EQ(
      "{\"type\":\"ParametricUnit\",\"name\":\"hPa\",\"conversion_factor\":"
      "100,\"id\":{\"authority\":\"AUTH\",\"code\":\"HPA_CODE\"}}",
      UnitToJson(UnitOfMeasure("hPa", 100.0, UnitOfMeasure::Type::PARAMETRIC,
                               "AUTH", "HPA_CODE")));
  EXPECT_EQ("{\"type\":\"Unit\",\"name\":\"\",\"conversion_factor\":1}",
            UnitToJson(UnitOfMeasure::NONE));

  // 2. exportToPROJString matches linear and angular PROJ units and returns
  // empty string for unmatched or other unit types.
  EXPECT_EQ("m", UnitOfMeasure::METRE.exportToPROJString());
  EXPECT_EQ("ft", UnitOfMeasure::FOOT.exportToPROJString());
  EXPECT_EQ("us-ft", UnitOfMeasure::US_FOOT.exportToPROJString());
  EXPECT_EQ("", UnitOfMeasure("furlong", 201.168, UnitOfMeasure::Type::LINEAR)
                    .exportToPROJString());

  EXPECT_EQ("deg", UnitOfMeasure::DEGREE.exportToPROJString());
  EXPECT_EQ("rad", UnitOfMeasure::RADIAN.exportToPROJString());
  EXPECT_EQ("grad", UnitOfMeasure::GRAD.exportToPROJString());
  EXPECT_EQ("", UnitOfMeasure::ARC_SECOND.exportToPROJString());

  EXPECT_EQ("", UnitOfMeasure::SCALE_UNITY.exportToPROJString());
  EXPECT_EQ("", UnitOfMeasure::SECOND.exportToPROJString());
}

TEST(CommonTest, MeasureScaleAngleAndLengthArithmeticAndEquivalence) {
  // 1. Measure construction, copy, SI conversion, and equality.
  Measure empty_measure;
  EXPECT_DOUBLE_EQ(0.0, empty_measure.value());
  EXPECT_DOUBLE_EQ(0.0, empty_measure.getSIValue());
  EXPECT_EQ(UnitOfMeasure::Type::UNKNOWN, empty_measure.unit().type());

  Measure two_feet(2.0, UnitOfMeasure::FOOT);
  Measure two_feet_copy(two_feet);
  EXPECT_TRUE(two_feet == two_feet_copy);
  EXPECT_DOUBLE_EQ(2.0, two_feet.value());
  EXPECT_DOUBLE_EQ(0.6096, two_feet.getSIValue());
  EXPECT_DOUBLE_EQ(0.6096, two_feet.convertToUnit(UnitOfMeasure::METRE));
  EXPECT_FALSE(two_feet == Measure(3.0, UnitOfMeasure::FOOT));
  EXPECT_FALSE(two_feet == Measure(2.0, UnitOfMeasure::METRE));

  // 2. Equivalence: STRICT uses operator==; EQUIVALENT compares SI values and
  // handles infinities.
  Measure in_metres(0.6096, UnitOfMeasure::METRE);
  EXPECT_FALSE(
      two_feet._isEquivalentTo(in_metres, IComparable::Criterion::STRICT));
  EXPECT_TRUE(
      two_feet._isEquivalentTo(two_feet_copy, IComparable::Criterion::STRICT));
  EXPECT_TRUE(
      two_feet._isEquivalentTo(in_metres, IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(two_feet._isEquivalentTo(Measure(0.61, UnitOfMeasure::METRE),
                                        IComparable::Criterion::EQUIVALENT));

  const double pos_inf = std::numeric_limits<double>::infinity();
  const double neg_inf = -std::numeric_limits<double>::infinity();
  Measure m_pos_inf(pos_inf, UnitOfMeasure::METRE);
  Measure m_neg_inf(neg_inf, UnitOfMeasure::METRE);
  EXPECT_TRUE(
      m_pos_inf._isEquivalentTo(m_pos_inf, IComparable::Criterion::EQUIVALENT));
  EXPECT_TRUE(
      m_neg_inf._isEquivalentTo(m_neg_inf, IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(
      m_pos_inf._isEquivalentTo(m_neg_inf, IComparable::Criterion::EQUIVALENT));
  EXPECT_FALSE(
      two_feet._isEquivalentTo(m_pos_inf, IComparable::Criterion::EQUIVALENT));

  // 3. Subclasses Scale, Angle, Length (default, 1-arg, 2-arg, Measure, copy,
  // and virtual destruction).
  Scale s_def;
  Scale s_val(1.5);
  Scale s_ppm(50.0, UnitOfMeasure::PARTS_PER_MILLION);
  Scale s_from_m(Measure(2.0, UnitOfMeasure::SCALE_UNITY));
  Scale s_copy(s_ppm);
  EXPECT_DOUBLE_EQ(0.0, s_def.value());
  EXPECT_EQ(UnitOfMeasure::SCALE_UNITY, s_val.unit());
  EXPECT_DOUBLE_EQ(50e-6, s_copy.getSIValue());
  EXPECT_DOUBLE_EQ(2.0, s_from_m.value());

  Angle a_def;
  Angle a_deg(90.0);
  Angle a_rad(kPi / 2.0, UnitOfMeasure::RADIAN);
  Angle a_from_m(Measure(45.0, UnitOfMeasure::DEGREE));
  Angle a_copy(a_rad);
  EXPECT_DOUBLE_EQ(0.0, a_def.value());
  EXPECT_EQ(UnitOfMeasure::DEGREE, a_deg.unit());
  EXPECT_TRUE(
      a_deg._isEquivalentTo(a_copy, IComparable::Criterion::EQUIVALENT, 1e-12));
  EXPECT_DOUBLE_EQ(45.0, a_from_m.value());

  Length l_def;
  Length l_m(10.0);
  Length l_ft(100.0, UnitOfMeasure::FOOT);
  Length l_from_m(Measure(5.0, UnitOfMeasure::METRE));
  Length l_copy(l_ft);
  EXPECT_DOUBLE_EQ(0.0, l_def.value());
  EXPECT_EQ(UnitOfMeasure::METRE, l_m.unit());
  EXPECT_DOUBLE_EQ(30.48, l_copy.getSIValue());
  EXPECT_DOUBLE_EQ(5.0, l_from_m.value());

  EXPECT_DOUBLE_EQ(1e-10, Measure::DEFAULT_MAX_REL_ERROR);

  std::unique_ptr<Measure> poly_measure =
      std::make_unique<Measure>(3.0, UnitOfMeasure::METRE);
  std::unique_ptr<Measure> poly_scale = std::make_unique<Scale>(1.0);
  std::unique_ptr<Measure> poly_angle = std::make_unique<Angle>(1.0);
  std::unique_ptr<Measure> poly_length = std::make_unique<Length>(1.0);
  EXPECT_DOUBLE_EQ(3.0, poly_measure->value());
  EXPECT_DOUBLE_EQ(1.0, poly_scale->value());
  EXPECT_DOUBLE_EQ(1.0, poly_angle->value());
  EXPECT_DOUBLE_EQ(1.0, poly_length->value());
}

TEST(CommonTest, DateTimeIso8601ValidationCopyAndOptionalAssign) {
  // 1. Default constructor and operator= exercised via
  // util::optional<DateTime>.
  optional<DateTime> opt_dt;
  EXPECT_FALSE(opt_dt.has_value());
  EXPECT_EQ("", opt_dt->toString());

  opt_dt = DateTime::create("2026-11-10");
  ASSERT_TRUE(opt_dt.has_value());
  EXPECT_EQ("2026-11-10", opt_dt->toString());
  EXPECT_TRUE(opt_dt->isISO_8601());

  DateTime copied(*opt_dt);
  EXPECT_EQ("2026-11-10", copied.toString());

  // 2. Table-driven check for DateTime::isISO_8601() edge cases.
  struct DateCase {
    std::string_view text;
    bool expected_iso;
  };
  constexpr DateCase kCases[] = {
      {.text = "", .expected_iso = false},
      {.text = " 2026", .expected_iso = false},
      {.text = "-2026", .expected_iso = false},
      {.text = "a2026", .expected_iso = false},
      {.text = ":2026", .expected_iso = false},
      {.text = "2026-01-01 12:00:00", .expected_iso = false},
      {.text = "02026", .expected_iso = true},
      {.text = "92026", .expected_iso = true},
      {.text = "2026-01-01T12:00:00Z", .expected_iso = true},
  };
  for (const auto& tc : kCases) {
    const DateTime dt = DateTime::create(std::string(tc.text));
    EXPECT_EQ(tc.expected_iso, dt.isISO_8601()) << tc.text;
    EXPECT_EQ(tc.text, dt.toString());
  }
}

TEST(CommonTest, IdentifiedObjectPropertiesIdentifiersAliasesAndExceptions) {
  // 1. Default-constructed object has empty name, ids, aliases, remarks, and
  // isDeprecated() == false.
  auto empty_obj = TestIdentifiedObject::Create();
  EXPECT_EQ("", empty_obj->nameStr());
  EXPECT_FALSE(empty_obj->name()->description().has_value());
  EXPECT_TRUE(empty_obj->identifiers().empty());
  EXPECT_TRUE(empty_obj->aliases().empty());
  EXPECT_EQ("", empty_obj->alias());
  EXPECT_EQ(0, empty_obj->getEPSGCode());
  EXPECT_EQ("", empty_obj->remarks());
  EXPECT_FALSE(empty_obj->isDeprecated());

  // 2. Name via string or IdentifierNNPtr; CODE_KEY fallback when
  // IDENTIFIERS_KEY is absent; REMARKS_KEY and DEPRECATED_KEY.
  auto by_code_key = TestIdentifiedObject::Create(
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY, "ByCodeKey")
          .set(Identifier::CODESPACE_KEY, "epsg")
          .set(Identifier::CODE_KEY, 4326)
          .set(IdentifiedObject::ALIAS_KEY, "SingleStringAlias")
          .set(IdentifiedObject::REMARKS_KEY, "Test remark")
          .set(IdentifiedObject::DEPRECATED_KEY, true));
  EXPECT_EQ("ByCodeKey", by_code_key->nameStr());
  ASSERT_EQ(1u, by_code_key->identifiers().size());
  EXPECT_EQ(4326, by_code_key->getEPSGCode());
  EXPECT_EQ("SingleStringAlias", by_code_key->alias());
  EXPECT_EQ("Test remark", by_code_key->remarks());
  EXPECT_TRUE(by_code_key->isDeprecated());

  // Copy constructor preserves properties.
  auto cloned = by_code_key->Clone();
  EXPECT_EQ("ByCodeKey", cloned->nameStr());
  EXPECT_EQ(4326, cloned->getEPSGCode());
  EXPECT_TRUE(cloned->isDeprecated());

  // 3. Name via IdentifierNNPtr, single non-EPSG IdentifierNNPtr in
  // IDENTIFIERS_KEY (getEPSGCode() == 0), GenericName alias, deprecated=false.
  auto name_id = Identifier::create(
      "obj_id", PropertyMap().set(Identifier::DESCRIPTION_KEY, "NamedById"));
  auto non_epsg_id = MakeIdentifier("IGNF", "LAMB93");
  auto local_alias = NameFactory::createLocalName(nullptr, "LocalAlias");
  auto by_single_id = TestIdentifiedObject::Create(
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY, name_id)
          .set(IdentifiedObject::IDENTIFIERS_KEY, non_epsg_id)
          .set(IdentifiedObject::ALIAS_KEY,
               nn_static_pointer_cast<GenericName>(local_alias))
          .set(IdentifiedObject::DEPRECATED_KEY, false));
  EXPECT_EQ("NamedById", by_single_id->nameStr());
  EXPECT_EQ(0, by_single_id->getEPSGCode());
  EXPECT_EQ("LocalAlias", by_single_id->alias());
  EXPECT_FALSE(by_single_id->isDeprecated());

  // 4. Multiple identifiers and mixed aliases (GenericName + BoxedValue
  // string).
  auto id_array = ArrayOfBaseObject::create();
  id_array->add(MakeIdentifier("OGC", "CRS84"));
  id_array->add(MakeIdentifier("EPSG", "4979"));
  auto alias_array = ArrayOfBaseObject::create();
  alias_array->add(NameFactory::createLocalName(nullptr, "FirstAlias"));
  alias_array->add(nn_make_shared<BoxedValue>("SecondAlias"));
  auto by_arrays = TestIdentifiedObject::Create(
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY, "ArrayObj")
          .set(IdentifiedObject::IDENTIFIERS_KEY, id_array)
          .set(IdentifiedObject::ALIAS_KEY, alias_array));
  ASSERT_EQ(2u, by_arrays->identifiers().size());
  EXPECT_EQ(4979, by_arrays->getEPSGCode());
  ASSERT_EQ(2u, by_arrays->aliases().size());
  EXPECT_EQ("FirstAlias", by_arrays->alias());

  // String vector alias helper sets ArrayOfBaseObject of BoxedValue strings.
  auto by_str_vec = TestIdentifiedObject::Create(PropertyMap().set(
      IdentifiedObject::ALIAS_KEY, std::vector<std::string>{"VecA", "VecB"}));
  ASSERT_EQ(2u, by_str_vec->aliases().size());
  EXPECT_EQ("VecA", by_str_vec->alias());

  // 5. Invalid value types throw InvalidValueTypeException on every key.
  EXPECT_THROW(empty_obj->SetPropertiesForTest(
                   PropertyMap().set(IdentifiedObject::NAME_KEY, 123)),
               InvalidValueTypeException);
  EXPECT_THROW(empty_obj->SetPropertiesForTest(PropertyMap().set(
                   IdentifiedObject::NAME_KEY, Extent::WORLD)),
               InvalidValueTypeException);

  EXPECT_THROW(empty_obj->SetPropertiesForTest(PropertyMap().set(
                   IdentifiedObject::IDENTIFIERS_KEY, "not_an_id")),
               InvalidValueTypeException);
  auto bad_id_array = ArrayOfBaseObject::create();
  bad_id_array->add(nn_make_shared<BoxedValue>("not_an_id"));
  EXPECT_THROW(empty_obj->SetPropertiesForTest(PropertyMap().set(
                   IdentifiedObject::IDENTIFIERS_KEY, bad_id_array)),
               InvalidValueTypeException);

  EXPECT_THROW(empty_obj->SetPropertiesForTest(
                   PropertyMap().set(IdentifiedObject::ALIAS_KEY, 99)),
               InvalidValueTypeException);
  auto bad_alias_int_arr = ArrayOfBaseObject::create();
  bad_alias_int_arr->add(nn_make_shared<BoxedValue>(42));
  EXPECT_THROW(empty_obj->SetPropertiesForTest(PropertyMap().set(
                   IdentifiedObject::ALIAS_KEY, bad_alias_int_arr)),
               InvalidValueTypeException);
  auto bad_alias_obj_arr = ArrayOfBaseObject::create();
  bad_alias_obj_arr->add(Extent::WORLD);
  EXPECT_THROW(empty_obj->SetPropertiesForTest(PropertyMap().set(
                   IdentifiedObject::ALIAS_KEY, bad_alias_obj_arr)),
               InvalidValueTypeException);

  EXPECT_THROW(empty_obj->SetPropertiesForTest(
                   PropertyMap().set(IdentifiedObject::REMARKS_KEY, 42)),
               InvalidValueTypeException);
  EXPECT_THROW(empty_obj->SetPropertiesForTest(
                   PropertyMap().set(IdentifiedObject::DEPRECATED_KEY, "yes")),
               InvalidValueTypeException);
  EXPECT_THROW(empty_obj->SetPropertiesForTest(PropertyMap().set(
                   IdentifiedObject::DEPRECATED_KEY, Extent::WORLD)),
               InvalidValueTypeException);
}

TEST(CommonTest, IdentifiedObjectFormattingAndEquivalence) {
  auto id_array = ArrayOfBaseObject::create();
  id_array->add(MakeIdentifier("OGC", "CRS84"));
  id_array->add(MakeIdentifier("EPSG", "4326"));
  auto two_ids = TestIdentifiedObject::Create(
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY, "WGS 84")
          .set(IdentifiedObject::IDENTIFIERS_KEY, id_array)
          .set(IdentifiedObject::REMARKS_KEY, "Remark text"));

  auto one_id = TestIdentifiedObject::Create(
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY, "WGS_84")
          .set(Identifier::CODESPACE_KEY, "EPSG")
          .set(Identifier::CODE_KEY, 4326));

  auto no_id = TestIdentifiedObject::Create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "wgs 84"));

  // 1. WKT2 emits all IDs and REMARK; WKT1 emits only the first ID.
  auto wkt2 = WKTFormatter::create(WKTFormatter::Convention::WKT2);
  wkt2->setMultiLine(false);
  two_ids->_exportToWKT(wkt2.get());
  EXPECT_EQ(
      "TESTIDOBJ[\"WGS 84\",ID[\"OGC\",\"CRS84\"],ID[\"EPSG\",4326],"
      "REMARK[\"Remark text\"]]",
      wkt2->toString());

  auto wkt1 = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
  wkt1->setMultiLine(false);
  two_ids->_exportToWKT(wkt1.get());
  EXPECT_EQ(
      "TESTIDOBJ[\"WGS 84\",AUTHORITY[\"OGC\",\"CRS84\"],REMARK[\"Remark "
      "text\"]]",
      wkt1->toString());

  // 2. JSON formatID emits nothing for 0 IDs, "id" for 1 ID, and "ids" for >1
  // IDs; formatRemarks omits empty remarks.
  EXPECT_EQ("{\"type\":\"TestIdObj\"}", no_id->FormatIdAndRemarksJson());
  EXPECT_EQ(
      "{\"type\":\"TestIdObj\",\"id\":{\"authority\":\"EPSG\",\"code\":4326}}",
      one_id->FormatIdAndRemarksJson());
  EXPECT_EQ(
      "{\"type\":\"TestIdObj\",\"ids\":[{\"authority\":\"OGC\",\"code\":"
      "\"CRS84\"},{\"authority\":\"EPSG\",\"code\":4326}],\"remarks\":\"Remark "
      "text\"}",
      two_ids->FormatIdAndRemarksJson());

  // 3. _isEquivalentTo(const IComparable*, ...) handles nullptr and
  // non-IdentifiedObject inputs and forwards IdentifiedObject inputs.
  EXPECT_FALSE(
      two_ids->_isEquivalentTo(static_cast<const IComparable*>(nullptr),
                               IComparable::Criterion::STRICT, nullptr));
  EXPECT_FALSE(two_ids->_isEquivalentTo(
      Extent::WORLD.get(), IComparable::Criterion::EQUIVALENT, nullptr));

  // STRICT uses case-insensitive name comparison.
  EXPECT_TRUE(two_ids->_isEquivalentTo(
      no_id.get(), IComparable::Criterion::STRICT, nullptr));
  EXPECT_FALSE(two_ids->_isEquivalentTo(
      one_id.get(), IComparable::Criterion::STRICT, nullptr));

  // EQUIVALENT matches canonicalized names, alias override, unknown/unnamed.
  EXPECT_TRUE(two_ids->_isEquivalentTo(
      one_id.get(), IComparable::Criterion::EQUIVALENT, nullptr));

  auto other_name = TestIdentifiedObject::Create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "CompletelyDifferent"));
  EXPECT_FALSE(two_ids->_isEquivalentTo(
      other_name.get(), IComparable::Criterion::EQUIVALENT, nullptr));

  auto alias_equiv_obj = TestObjectUsage::Create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "AliasMatch"),
      /*alias_equiv=*/true);
  auto other_usage = TestObjectUsage::Create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "CompletelyDifferent"));
  EXPECT_TRUE(alias_equiv_obj->_isEquivalentTo(
      other_usage.get(), IComparable::Criterion::EQUIVALENT, nullptr));

  auto unknown_obj = TestIdentifiedObject::Create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "unknown"));
  auto unnamed_obj = TestIdentifiedObject::Create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "unnamed"));
  EXPECT_TRUE(unknown_obj->_isEquivalentTo(
      unnamed_obj.get(), IComparable::Criterion::EQUIVALENT, nullptr));
  EXPECT_TRUE(unnamed_obj->_isEquivalentTo(
      unknown_obj.get(), IComparable::Criterion::EQUIVALENT, nullptr));
  EXPECT_FALSE(unknown_obj->_isEquivalentTo(
      other_name.get(), IComparable::Criterion::EQUIVALENT, nullptr));

  // 4. Integration check with real PROJ IdentifiedObject / CRS copy clone.
  auto wgs84_clone = GeographicCRS::EPSG_4326->shallowClone();
  EXPECT_TRUE(wgs84_clone->isEquivalentTo(GeographicCRS::EPSG_4326.get(),
                                          IComparable::Criterion::STRICT));
  EXPECT_FALSE(Ellipsoid::WGS84->isEquivalentTo(
      Ellipsoid::CLARKE_1866.get(), IComparable::Criterion::EQUIVALENT));
}

TEST(CommonTest, ObjectDomainWktJsonFormattingCopyAndEquivalence) {
  // 1. Null extent and empty scope: WKT2 emits nothing, WKT2_2019 emits
  // SCOPE["unknown"].
  auto empty_domain = ObjectDomain::create(optional<std::string>(), nullptr);
  EXPECT_FALSE(empty_domain->scope().has_value());
  EXPECT_EQ(nullptr, empty_domain->domainOfValidity());
  EXPECT_EQ("", DomainToWkt(*empty_domain, WKTFormatter::Convention::WKT2));
  EXPECT_EQ("SCOPE[\"unknown\"]",
            DomainToWkt(*empty_domain, WKTFormatter::Convention::WKT2_2019));
  EXPECT_EQ("{\"type\":\"DomainWrap\"}", DomainToJson(*empty_domain));

  // Copy constructor via TestObjectDomain preserves fields.
  auto copy_src = TestObjectDomain::Create(optional<std::string>("Mapping"),
                                           Extent::WORLD.as_nullable());
  auto copied_domain = copy_src->Clone();
  ASSERT_TRUE(copied_domain->scope().has_value());
  EXPECT_EQ("Mapping", *copied_domain->scope());
  EXPECT_NE(nullptr, copied_domain->domainOfValidity());

  // 2. Full Extent with description, 1 bbox, 1 vertical extent (non-METRE
  // unit), and 1 temporal extent (ISO start, non-ISO stop).
  auto bbox = GeographicBoundingBox::create(-10.0, 40.0, 20.0, 60.0);
  auto vert_ft = VerticalExtent::create(
      0.0, 1000.0, UnitOfMeasure::create(UnitOfMeasure::FOOT));
  auto temp_iso_noniso = TemporalExtent::create("2020-01-01", "end of epoch");
  auto full_extent = Extent::create(
      optional<std::string>("Europe"), std::vector<GeographicExtentNNPtr>{bbox},
      std::vector<VerticalExtentNNPtr>{vert_ft},
      std::vector<TemporalExtentNNPtr>{temp_iso_noniso});
  auto full_domain = ObjectDomain::create(optional<std::string>("Geodesy"),
                                          full_extent.as_nullable());

  EXPECT_EQ(
      "SCOPE[\"Geodesy\"],AREA[\"Europe\"],BBOX[40,-10,60,20],"
      "VERTICALEXTENT[0,1000,LENGTHUNIT[\"foot\",0.3048,ID[\"EPSG\",9002]]],"
      "TIMEEXTENT[2020-01-01,\"end of epoch\"]",
      DomainToWkt(*full_domain, WKTFormatter::Convention::WKT2));
  EXPECT_EQ(
      "{\"type\":\"DomainWrap\",\"scope\":\"Geodesy\",\"area\":\"Europe\","
      "\"bbox\":{\"south_latitude\":40,\"west_longitude\":-10,"
      "\"north_latitude\":60,\"east_longitude\":20},"
      "\"vertical_extent\":{\"minimum\":0,\"maximum\":1000,\"unit\":{\"type\":"
      "\"LinearUnit\",\"name\":\"foot\",\"conversion_factor\":0.3048,\"id\":{"
      "\"authority\":\"EPSG\",\"code\":9002}}},"
      "\"temporal_extent\":{\"start\":\"2020-01-01\",\"end\":\"end of "
      "epoch\"}}",
      DomainToJson(*full_domain));

  // Non-ISO start and ISO stop reverses quoting in TIMEEXTENT, and METRE unit
  // omits "unit" key in JSON vertical_extent.
  auto vert_m = VerticalExtent::create(
      -50.0, 250.0, UnitOfMeasure::create(UnitOfMeasure::METRE));
  auto temp_noniso_iso = TemporalExtent::create("before 2020", "2025-12-31");
  auto metre_extent = Extent::create(
      optional<std::string>(), std::vector<GeographicExtentNNPtr>{},
      std::vector<VerticalExtentNNPtr>{vert_m},
      std::vector<TemporalExtentNNPtr>{temp_noniso_iso});
  auto metre_domain =
      ObjectDomain::create(optional<std::string>(), metre_extent.as_nullable());
  EXPECT_EQ(
      "VERTICALEXTENT[-50,250,LENGTHUNIT[\"metre\",1,ID[\"EPSG\",9001]]],"
      "TIMEEXTENT[\"before 2020\",2025-12-31]",
      DomainToWkt(*metre_domain, WKTFormatter::Convention::WKT2));
  EXPECT_EQ(
      "{\"type\":\"DomainWrap\",\"vertical_extent\":{\"minimum\":-50,"
      "\"maximum\":250},\"temporal_extent\":{\"start\":\"before 2020\","
      "\"end\":\"2025-12-31\"}}",
      DomainToJson(*metre_domain));

  // Single non-BoundingBox GeographicExtent omits BBOX in WKT and JSON.
  auto non_bbox_extent = Extent::create(
      optional<std::string>(),
      std::vector<GeographicExtentNNPtr>{
          nn_make_shared<DummyGeographicExtent>()},
      std::vector<VerticalExtentNNPtr>{}, std::vector<TemporalExtentNNPtr>{});
  auto non_bbox_domain = ObjectDomain::create(optional<std::string>("Poly"),
                                              non_bbox_extent.as_nullable());
  EXPECT_EQ("SCOPE[\"Poly\"]",
            DomainToWkt(*non_bbox_domain, WKTFormatter::Convention::WKT2));
  EXPECT_EQ("{\"type\":\"DomainWrap\",\"scope\":\"Poly\"}",
            DomainToJson(*non_bbox_domain));

  // Multiple elements (>1) skip BBOX/VERTICALEXTENT/TIMEEXTENT.
  auto multi_extent = Extent::create(
      optional<std::string>(), std::vector<GeographicExtentNNPtr>{bbox, bbox},
      std::vector<VerticalExtentNNPtr>{vert_m, vert_m},
      std::vector<TemporalExtentNNPtr>{temp_iso_noniso, temp_noniso_iso});
  auto multi_domain = ObjectDomain::create(optional<std::string>("Multi"),
                                           multi_extent.as_nullable());
  EXPECT_EQ("SCOPE[\"Multi\"]",
            DomainToWkt(*multi_domain, WKTFormatter::Convention::WKT2));
  EXPECT_EQ("{\"type\":\"DomainWrap\",\"scope\":\"Multi\"}",
            DomainToJson(*multi_domain));

  // 3. _isEquivalentTo branches.
  auto scoped_null_a =
      ObjectDomain::create(optional<std::string>("S1"), nullptr);
  auto scoped_null_b =
      ObjectDomain::create(optional<std::string>("S1"), nullptr);
  auto scoped_null_diff =
      ObjectDomain::create(optional<std::string>("S2"), nullptr);
  auto scoped_world = ObjectDomain::create(optional<std::string>("S1"),
                                           Extent::WORLD.as_nullable());
  auto scoped_other_extent = ObjectDomain::create(optional<std::string>("S1"),
                                                  full_extent.as_nullable());

  EXPECT_FALSE(scoped_null_a->_isEquivalentTo(Extent::WORLD.get()));
  EXPECT_FALSE(scoped_null_a->_isEquivalentTo(empty_domain.get()));
  EXPECT_FALSE(scoped_null_a->_isEquivalentTo(scoped_null_diff.get()));
  EXPECT_FALSE(scoped_null_a->_isEquivalentTo(scoped_world.get()));
  EXPECT_TRUE(scoped_null_a->_isEquivalentTo(scoped_null_b.get()));
  EXPECT_TRUE(empty_domain->_isEquivalentTo(empty_domain.get()));
  EXPECT_TRUE(scoped_world->_isEquivalentTo(scoped_world.get()));
  EXPECT_FALSE(scoped_world->_isEquivalentTo(scoped_other_extent.get()));
}

TEST(CommonTest, ObjectUsagePropertiesWktJsonExportAndEquivalence) {
  // 1. SCOPE_KEY alone, DOMAIN_OF_VALIDITY_KEY alone, and both together.
  auto scope_only =
      TestObjectUsage::Create(PropertyMap()
                                  .set(IdentifiedObject::NAME_KEY, "ScopeOnly")
                                  .set(ObjectUsage::SCOPE_KEY, "Navigation"));
  ASSERT_EQ(1u, scope_only->domains().size());
  EXPECT_EQ("Navigation", *(scope_only->domains()[0]->scope()));
  EXPECT_EQ(nullptr, scope_only->domains()[0]->domainOfValidity());

  auto extent_only = TestObjectUsage::Create(
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY, "ExtentOnly")
          .set(ObjectUsage::DOMAIN_OF_VALIDITY_KEY, Extent::WORLD));
  ASSERT_EQ(1u, extent_only->domains().size());
  EXPECT_FALSE(extent_only->domains()[0]->scope().has_value());
  EXPECT_NE(nullptr, extent_only->domains()[0]->domainOfValidity());

  auto scope_and_extent = TestObjectUsage::Create(
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY, "ScopeAndExtent")
          .set(ObjectUsage::SCOPE_KEY, "Surveying")
          .set(ObjectUsage::DOMAIN_OF_VALIDITY_KEY, Extent::WORLD));
  ASSERT_EQ(1u, scope_and_extent->domains().size());
  EXPECT_EQ("Surveying", *(scope_and_extent->domains()[0]->scope()));
  EXPECT_NE(nullptr, scope_and_extent->domains()[0]->domainOfValidity());

  // Single OBJECT_DOMAIN_KEY and ArrayOfBaseObject OBJECT_DOMAIN_KEY.
  auto d1 = ObjectDomain::create(optional<std::string>("Scope1"),
                                 Extent::WORLD.as_nullable());
  auto d2 = ObjectDomain::create(optional<std::string>("Scope2"), nullptr);
  auto single_obj_domain = TestObjectUsage::Create(
      PropertyMap().set(ObjectUsage::OBJECT_DOMAIN_KEY, d1));
  ASSERT_EQ(1u, single_obj_domain->domains().size());

  auto dom_array = ArrayOfBaseObject::create();
  dom_array->add(d1);
  dom_array->add(d2);
  auto multi_domain_obj = TestObjectUsage::Create(
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY, "MultiUsage")
          .set(Identifier::CODESPACE_KEY, "EPSG")
          .set(Identifier::CODE_KEY, 9999)
          .set(IdentifiedObject::REMARKS_KEY, "Usage remark")
          .set(ObjectUsage::OBJECT_DOMAIN_KEY, dom_array));
  ASSERT_EQ(2u, multi_domain_obj->domains().size());

  // Copy constructor preserves domains.
  auto cloned_usage = multi_domain_obj->Clone();
  ASSERT_EQ(2u, cloned_usage->domains().size());

  // 2. Invalid SCOPE_KEY, DOMAIN_OF_VALIDITY_KEY, and OBJECT_DOMAIN_KEY types
  // throw.
  EXPECT_THROW(
      TestObjectUsage::Create(PropertyMap().set(ObjectUsage::SCOPE_KEY, 123)),
      InvalidValueTypeException);
  EXPECT_THROW(TestObjectUsage::Create(PropertyMap().set(
                   ObjectUsage::DOMAIN_OF_VALIDITY_KEY, "bad")),
               InvalidValueTypeException);
  EXPECT_THROW(TestObjectUsage::Create(
                   PropertyMap().set(ObjectUsage::OBJECT_DOMAIN_KEY, 123)),
               InvalidValueTypeException);
  auto bad_dom_array = ArrayOfBaseObject::create();
  bad_dom_array->add(Extent::WORLD);
  EXPECT_THROW(TestObjectUsage::Create(PropertyMap().set(
                   ObjectUsage::OBJECT_DOMAIN_KEY, bad_dom_array)),
               InvalidValueTypeException);

  // 3. baseExportToWKT branches: WKT2 (only first domain), WKT2_2019 (all
  // domains inside USAGE[...]), pushDisableUsage(), setOutputId(false), WKT1.
  auto wkt2 = WKTFormatter::create(WKTFormatter::Convention::WKT2);
  wkt2->setMultiLine(false);
  multi_domain_obj->_exportToWKT(wkt2.get());
  EXPECT_EQ(
      "TESTOBJ[\"MultiUsage\",SCOPE[\"Scope1\"],AREA[\"World\"],"
      "BBOX[-90,-180,90,180],ID[\"EPSG\",9999],REMARK[\"Usage remark\"]]",
      wkt2->toString());

  auto wkt2_2019 = WKTFormatter::create(WKTFormatter::Convention::WKT2_2019);
  wkt2_2019->setMultiLine(false);
  multi_domain_obj->_exportToWKT(wkt2_2019.get());
  EXPECT_EQ(
      "TESTOBJ[\"MultiUsage\","
      "USAGE[SCOPE[\"Scope1\"],AREA[\"World\"],BBOX[-90,-180,90,180]],"
      "USAGE[SCOPE[\"Scope2\"]],"
      "ID[\"EPSG\",9999],REMARK[\"Usage remark\"]]",
      wkt2_2019->toString());

  auto wkt_disabled_usage =
      WKTFormatter::create(WKTFormatter::Convention::WKT2_2019);
  wkt_disabled_usage->setMultiLine(false);
  wkt_disabled_usage->pushDisableUsage();
  {
    absl::Cleanup pop_usage = [&wkt_disabled_usage] {
      wkt_disabled_usage->popDisableUsage();
    };
    multi_domain_obj->_exportToWKT(wkt_disabled_usage.get());
  }
  EXPECT_EQ("TESTOBJ[\"MultiUsage\",ID[\"EPSG\",9999]]",
            wkt_disabled_usage->toString());

  auto wkt_no_id = WKTFormatter::create(WKTFormatter::Convention::WKT2_2019);
  wkt_no_id->setMultiLine(false);
  wkt_no_id->setOutputId(false);
  multi_domain_obj->_exportToWKT(wkt_no_id.get());
  EXPECT_EQ("TESTOBJ[\"MultiUsage\"]", wkt_no_id->toString());

  auto wkt1 = WKTFormatter::create(WKTFormatter::Convention::WKT1_GDAL);
  wkt1->setMultiLine(false);
  multi_domain_obj->_exportToWKT(wkt1.get());
  EXPECT_EQ("TESTOBJ[\"MultiUsage\",AUTHORITY[\"EPSG\",\"9999\"]]",
            wkt1->toString());

  // 4. baseExportToJSON branches: 0, 1, and >1 domains, plus nested child where
  // outputUsage() is false and parent ID suppresses UnitOfMeasure ID too.
  auto empty_usage = TestObjectUsage::Create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "EmptyUsage"));
  auto json_empty = JSONFormatter::create();
  json_empty->setMultiLine(false);
  json_empty->setSchema("");
  EXPECT_EQ("{\"type\":\"TestObj\",\"name\":\"EmptyUsage\"}",
            empty_usage->exportToJSON(json_empty.get()));

  auto json_one = JSONFormatter::create();
  json_one->setMultiLine(false);
  json_one->setSchema("");
  EXPECT_EQ(
      "{\"type\":\"TestObj\",\"name\":\"ScopeOnly\",\"scope\":\"Navigation\"}",
      scope_only->exportToJSON(json_one.get()));

  auto json_multi = JSONFormatter::create();
  json_multi->setMultiLine(false);
  json_multi->setSchema("");
  const std::string multi_json =
      multi_domain_obj->exportToJSON(json_multi.get());
  EXPECT_THAT(multi_json, HasSubstr("\"usages\":[{\"scope\":\"Scope1\""));
  EXPECT_THAT(multi_json, HasSubstr("{\"scope\":\"Scope2\"}]"));
  EXPECT_THAT(multi_json,
              HasSubstr("\"id\":{\"authority\":\"EPSG\",\"code\":9999}"));
  EXPECT_THAT(multi_json, HasSubstr("\"remarks\":\"Usage remark\""));

  // Nested child suppresses outputUsage(); when parent has ID, child ID and
  // child VerticalExtent UnitOfMeasure ID are also suppressed.
  auto vert_foot_extent = Extent::create(
      optional<std::string>(), std::vector<GeographicExtentNNPtr>{},
      std::vector<VerticalExtentNNPtr>{VerticalExtent::create(
          0.0, 100.0, UnitOfMeasure::create(UnitOfMeasure::FOOT))},
      std::vector<TemporalExtentNNPtr>{});
  auto foot_id_obj = TestObjectUsage::Create(
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY, "HasIdWithFootVert")
          .set(Identifier::CODESPACE_KEY, "EPSG")
          .set(Identifier::CODE_KEY, 8888)
          .set(ObjectUsage::DOMAIN_OF_VALIDITY_KEY, vert_foot_extent));
  auto json_foot = JSONFormatter::create();
  json_foot->setMultiLine(false);
  json_foot->setSchema("");
  const std::string foot_json = foot_id_obj->exportToJSON(json_foot.get());
  EXPECT_THAT(foot_json, HasSubstr("\"conversion_factor\":0.3048}"));
  EXPECT_THAT(foot_json, Not(HasSubstr("\"code\":9002")));

  auto parent_with_child = TestObjectUsage::Create(
      PropertyMap()
          .set(IdentifiedObject::NAME_KEY, "Parent")
          .set(Identifier::CODESPACE_KEY, "EPSG")
          .set(Identifier::CODE_KEY, 1111),
      /*alias_equiv=*/false, multi_domain_obj.as_nullable());
  auto json_nested = JSONFormatter::create();
  json_nested->setMultiLine(false);
  json_nested->setSchema("");
  const std::string nested_json =
      parent_with_child->exportToJSON(json_nested.get());
  EXPECT_THAT(nested_json, HasSubstr("\"child\":{\"type\":\"TestObj\","
                                     "\"name\":\"MultiUsage\","
                                     "\"remarks\":\"Usage remark\"}"));
  EXPECT_THAT(nested_json, Not(HasSubstr("\"usages\"")));

  // 5. ObjectUsage::_isEquivalentTo rejects non-ObjectUsage and delegates for
  // ObjectUsage.
  auto non_usage = TestIdentifiedObject::Create(
      PropertyMap().set(IdentifiedObject::NAME_KEY, "MultiUsage"));
  EXPECT_FALSE(multi_domain_obj->_isEquivalentTo(
      non_usage.get(), IComparable::Criterion::EQUIVALENT, nullptr));
  EXPECT_FALSE(multi_domain_obj->_isEquivalentTo(
      PrimeMeridian::GREENWICH.get(), IComparable::Criterion::EQUIVALENT,
      nullptr));
  EXPECT_TRUE(multi_domain_obj->_isEquivalentTo(
      cloned_usage.get(), IComparable::Criterion::STRICT, nullptr));
  EXPECT_FALSE(multi_domain_obj->_isEquivalentTo(
      scope_only.get(), IComparable::Criterion::STRICT, nullptr));
}

TEST(CommonTest, DataEpochConstructionCopyAndCoordinateMetadata) {
  // 1. Default construction via util::optional<DataEpoch>.
  optional<DataEpoch> empty_epoch;
  EXPECT_FALSE(empty_epoch.has_value());
  EXPECT_DOUBLE_EQ(0.0, empty_epoch->coordinateEpoch().value());

  // 2. Explicit Measure construction and copy construction.
  DataEpoch epoch_2020(Measure(2020.5, UnitOfMeasure::YEAR));
  DataEpoch copied(epoch_2020);
  EXPECT_DOUBLE_EQ(2020.5, copied.coordinateEpoch().value());
  EXPECT_EQ(UnitOfMeasure::YEAR, copied.coordinateEpoch().unit());

  // 3. Integration check through CoordinateMetadata.
  auto coord_meta =
      CoordinateMetadata::create(GeographicCRS::EPSG_4326, 2022.25);
  ASSERT_TRUE(coord_meta->coordinateEpoch().has_value());
  EXPECT_DOUBLE_EQ(2022.25,
                   coord_meta->coordinateEpoch()->coordinateEpoch().value());
}

}  // namespace
}  // namespace proj
