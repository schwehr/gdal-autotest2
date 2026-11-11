// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/geos/include/geos/noding/FastNodingValidator.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"
#include "third_party/geos/include/geos/noding/NodedSegmentString.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/util/TopologyException.h"

namespace geos_google {
namespace {

using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::noding::BasicSegmentString;
using geos::noding::FastNodingValidator;
using geos::noding::NodedSegmentString;
using geos::noding::SegmentString;
using geos::util::TopologyException;
using testing::HasSubstr;
using testing::StartsWith;

class ValidatorInput {
 public:
  void AddBasicXY(std::initializer_list<CoordinateXY> coords) {
    auto seq = std::make_unique<CoordinateSequence>(coords);
    CoordinateSequence* seq_ptr = seq.get();
    owned_seqs_.push_back(std::move(seq));
    auto ss = std::make_unique<BasicSegmentString>(seq_ptr, nullptr);
    raw_strings_.push_back(ss.get());
    owned_strings_.push_back(std::move(ss));
  }

  void AddNodedXY(std::initializer_list<CoordinateXY> coords) {
    auto seq = std::make_unique<CoordinateSequence>(coords);
    auto ss = std::make_unique<NodedSegmentString>(
        seq.release(), /*constructZ=*/false, /*constructM=*/false, nullptr);
    raw_strings_.push_back(ss.get());
    owned_strings_.push_back(std::move(ss));
  }

  std::vector<SegmentString*>& vec() { return raw_strings_; }

 private:
  // Destroy SegmentStrings before externally owned CoordinateSequences.
  std::vector<std::unique_ptr<CoordinateSequence>> owned_seqs_;
  std::vector<std::unique_ptr<SegmentString>> owned_strings_;
  std::vector<SegmentString*> raw_strings_;
};

TEST(FastNodingValidatorTest, GetErrorMessageBeforeExecution) {
  ValidatorInput input;
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 10.0)});
  input.AddBasicXY({CoordinateXY(0.0, 10.0), CoordinateXY(10.0, 0.0)});

  FastNodingValidator validator(input.vec());
  EXPECT_EQ(validator.getErrorMessage(), "no intersections found");
}

TEST(FastNodingValidatorTest, EmptyAndDisjointSegmentsAreValid) {
  {
    ValidatorInput empty;
    FastNodingValidator validator(empty.vec());
    EXPECT_TRUE(validator.isValid());
    EXPECT_NO_THROW(validator.checkValid());
    EXPECT_EQ(validator.getErrorMessage(), "no intersections found");
  }

  {
    ValidatorInput disjoint;
    disjoint.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(2.0, 2.0),
                         CoordinateXY(4.0, 0.0)});
    disjoint.AddBasicXY({CoordinateXY(10.0, 10.0), CoordinateXY(15.0, 15.0)});

    FastNodingValidator validator(disjoint.vec());
    EXPECT_TRUE(validator.isValid());
    // Repeated call uses cached result.
    EXPECT_TRUE(validator.isValid());
    EXPECT_NO_THROW(validator.checkValid());
    EXPECT_EQ(validator.getErrorMessage(), "no intersections found");
  }
}

TEST(FastNodingValidatorTest, ProperlyNodedEndpointsAndClosedRingAreValid) {
  ValidatorInput input;
  // Four segment strings meeting only at shared endpoint (5, 5).
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 5.0)});
  input.AddBasicXY({CoordinateXY(5.0, 5.0), CoordinateXY(10.0, 10.0)});
  input.AddBasicXY({CoordinateXY(0.0, 10.0), CoordinateXY(5.0, 5.0)});
  input.AddBasicXY({CoordinateXY(5.0, 5.0), CoordinateXY(10.0, 0.0)});
  // Non-self-intersecting closed ring far from the cross.
  input.AddBasicXY({CoordinateXY(20.0, 0.0), CoordinateXY(30.0, 0.0),
                    CoordinateXY(30.0, 10.0), CoordinateXY(20.0, 10.0),
                    CoordinateXY(20.0, 0.0)});

  FastNodingValidator validator(input.vec());
  EXPECT_NO_THROW(validator.checkValid());
  EXPECT_TRUE(validator.isValid());
  EXPECT_EQ(validator.getErrorMessage(), "no intersections found");
}

TEST(FastNodingValidatorTest, CoincidentSegmentsDoNotTriggerFailure) {
  ValidatorInput input;
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)});
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)});

  FastNodingValidator validator(input.vec());
  EXPECT_TRUE(validator.isValid());
  EXPECT_NO_THROW(validator.checkValid());
}

TEST(FastNodingValidatorTest,
     ProperCrossingDetectedAndThrowsTopologyException) {
  ValidatorInput input;
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 10.0)});
  input.AddBasicXY({CoordinateXY(0.0, 10.0), CoordinateXY(10.0, 0.0)});

  FastNodingValidator validator(input.vec());
  EXPECT_FALSE(validator.isValid());
  // Repeated call uses cached intersection state.
  EXPECT_FALSE(validator.isValid());

  const std::string err = validator.getErrorMessage();
  EXPECT_THAT(err,
              StartsWith("found non-noded intersection between LINESTRING ("));
  EXPECT_THAT(err, HasSubstr("0 0, 10 10"));
  EXPECT_THAT(err, HasSubstr("0 10, 10 0"));

  try {
    validator.checkValid();
    FAIL() << "Expected TopologyException";
  } catch (TopologyException& e) {
    EXPECT_THAT(e.what(), HasSubstr("found non-noded intersection between"));
    EXPECT_TRUE(e.getCoordinate().equals2D(CoordinateXY(5.0, 5.0)));
  }
}

TEST(FastNodingValidatorTest, TJunctionWithSegmentInteriorThrowsDirectly) {
  ValidatorInput input;
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)});
  input.AddBasicXY({CoordinateXY(5.0, 0.0), CoordinateXY(5.0, 5.0)});

  FastNodingValidator validator(input.vec());
  try {
    validator.checkValid();
    FAIL() << "Expected TopologyException";
  } catch (TopologyException& e) {
    EXPECT_THAT(e.what(), HasSubstr("0 0, 10 0"));
    EXPECT_THAT(e.what(), HasSubstr("5 0, 5 5"));
    EXPECT_TRUE(e.getCoordinate().equals2D(CoordinateXY(5.0, 0.0)));
  }

  EXPECT_FALSE(validator.isValid());
  EXPECT_THAT(validator.getErrorMessage(), HasSubstr("5 0, 5 5"));
}

TEST(FastNodingValidatorTest, InteriorVertexTouchingInteriorVertexIsInvalid) {
  ValidatorInput input;
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 5.0),
                    CoordinateXY(10.0, 0.0)});
  input.AddBasicXY({CoordinateXY(0.0, 10.0), CoordinateXY(5.0, 5.0),
                    CoordinateXY(10.0, 10.0)});

  FastNodingValidator validator(input.vec());
  EXPECT_FALSE(validator.isValid());
  try {
    validator.checkValid();
    FAIL() << "Expected TopologyException";
  } catch (TopologyException& e) {
    EXPECT_TRUE(e.getCoordinate().equals2D(CoordinateXY(5.0, 5.0)));
  }
}

TEST(FastNodingValidatorTest, SelfIntersectingSingleSegmentStringIsInvalid) {
  {
    ValidatorInput bow_tie;
    bow_tie.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 10.0),
                        CoordinateXY(10.0, 0.0), CoordinateXY(0.0, 10.0)});
    FastNodingValidator validator(bow_tie.vec());
    EXPECT_FALSE(validator.isValid());
    try {
      validator.checkValid();
      FAIL() << "Expected TopologyException";
    } catch (TopologyException& e) {
      EXPECT_TRUE(e.getCoordinate().equals2D(CoordinateXY(5.0, 5.0)));
    }
  }

  {
    ValidatorInput self_touch;
    self_touch.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 0.0),
                           CoordinateXY(10.0, 0.0), CoordinateXY(10.0, 5.0),
                           CoordinateXY(5.0, 5.0), CoordinateXY(5.0, 0.0)});
    FastNodingValidator validator(self_touch.vec());
    EXPECT_FALSE(validator.isValid());
    try {
      validator.checkValid();
      FAIL() << "Expected TopologyException";
    } catch (TopologyException& e) {
      EXPECT_TRUE(e.getCoordinate().equals2D(CoordinateXY(5.0, 0.0)));
    }
  }
}

TEST(FastNodingValidatorTest, CollinearOverlappingSegmentsAreInvalid) {
  ValidatorInput input;
  input.AddBasicXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0)});
  input.AddBasicXY({CoordinateXY(5.0, 0.0), CoordinateXY(15.0, 0.0)});

  FastNodingValidator validator(input.vec());
  EXPECT_FALSE(validator.isValid());
  EXPECT_THROW(validator.checkValid(), TopologyException);
}

TEST(FastNodingValidatorTest, WorksWithNodedSegmentStrings) {
  ValidatorInput valid_input;
  valid_input.AddNodedXY({CoordinateXY(0.0, 0.0), CoordinateXY(5.0, 0.0)});
  valid_input.AddNodedXY({CoordinateXY(5.0, 0.0), CoordinateXY(10.0, 0.0)});
  FastNodingValidator valid_validator(valid_input.vec());
  EXPECT_TRUE(valid_validator.isValid());

  ValidatorInput invalid_input;
  invalid_input.AddNodedXY({CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 10.0)});
  invalid_input.AddNodedXY({CoordinateXY(0.0, 10.0), CoordinateXY(10.0, 0.0)});
  FastNodingValidator invalid_validator(invalid_input.vec());
  EXPECT_FALSE(invalid_validator.isValid());
  EXPECT_THROW(invalid_validator.checkValid(), TopologyException);
}

}  // namespace
}  // namespace geos_google
