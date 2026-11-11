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

#include "third_party/geos/include/geos/noding/BasicSegmentString.h"

#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/SegmentString.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::noding::BasicSegmentString;
using geos::noding::SegmentString;
using geos::util::IllegalArgumentException;

TEST(BasicSegmentStringTest, ConstructAccessorsAndExternalOwnership) {
  CoordinateSequence coords{CoordinateXY(0.0, 0.0), CoordinateXY(3.0, 4.0),
                            CoordinateXY(6.0, 0.0)};
  int context_a = 1;
  int context_b = 2;

  {
    BasicSegmentString ss(&coords, &context_a);
    const BasicSegmentString& const_ss = ss;

    EXPECT_EQ(ss.size(), 3u);
    EXPECT_EQ(ss.getData(), &context_a);
    EXPECT_EQ(ss.getCoordinates(), &coords);
    EXPECT_EQ(const_ss.getCoordinates(), &coords);
    EXPECT_FALSE(ss.isClosed());

    EXPECT_TRUE(
        ss.getCoordinate<CoordinateXY>(0).equals2D(CoordinateXY(0.0, 0.0)));
    EXPECT_TRUE(
        ss.getCoordinate<CoordinateXY>(1).equals2D(CoordinateXY(3.0, 4.0)));
    EXPECT_TRUE(ss.getCoordinate(2).equals2D(CoordinateXY(6.0, 0.0)));

    ss.setData(&context_b);
    EXPECT_EQ(ss.getData(), &context_b);
    ss.setData(nullptr);
    EXPECT_EQ(ss.getData(), nullptr);
  }

  // BasicSegmentString does not take ownership of CoordinateSequence.
  EXPECT_EQ(coords.size(), 3u);
  EXPECT_TRUE(coords.getAt<CoordinateXY>(1).equals2D(CoordinateXY(3.0, 4.0)));
}

TEST(BasicSegmentStringTest, PrintFormatsXYAndStreamOperator) {
  CoordinateSequence coords{CoordinateXY(0.0, 0.0), CoordinateXY(1.0, 2.0),
                            CoordinateXY(3.0, 4.0)};
  BasicSegmentString ss(&coords, nullptr);

  std::ostringstream direct_oss;
  std::ostream& returned = ss.print(direct_oss);
  EXPECT_EQ(&returned, &direct_oss);
  EXPECT_EQ(direct_oss.str(),
            "BasicSegmentString: \n LINESTRING(0 0, 1 2, 3 4);\n");

  std::ostringstream stream_oss;
  const SegmentString& base_ref = ss;
  stream_oss << base_ref;
  EXPECT_EQ(stream_oss.str(), direct_oss.str());
}

TEST(BasicSegmentStringTest, PrintFormatsXYZXYZMAndEmptySequence) {
  CoordinateSequence xyz{Coordinate(1.0, 2.0, 3.0), Coordinate(4.0, 5.0, 6.0)};
  BasicSegmentString ss_xyz(&xyz, nullptr);
  std::ostringstream xyz_oss;
  ss_xyz.print(xyz_oss);
  EXPECT_EQ(xyz_oss.str(),
            "BasicSegmentString: \n LINESTRING(1 2 3, 4 5 6);\n");

  CoordinateSequence xyzm{CoordinateXYZM(1.0, 2.0, 3.0, 4.0),
                          CoordinateXYZM(5.0, 6.0, 7.0, 8.0)};
  BasicSegmentString ss_xyzm(&xyzm, nullptr);
  std::ostringstream xyzm_oss;
  ss_xyzm.print(xyzm_oss);
  EXPECT_EQ(xyzm_oss.str(),
            "BasicSegmentString: \n LINESTRING(1 2 3 4, 5 6 7 8);\n");

  CoordinateSequence empty(0, false, false);
  BasicSegmentString ss_empty(&empty, nullptr);
  std::ostringstream empty_oss;
  ss_empty.print(empty_oss);
  EXPECT_EQ(empty_oss.str(), "BasicSegmentString: \n LINESTRING();\n");
}

TEST(BasicSegmentStringTest, GetSegmentOctantAllOctantsAndBounds) {
  struct OctantCase {
    double x1;
    double y1;
    int expected_octant;
  };
  const std::vector<OctantCase> cases = {
      {1.0, 0.0, 0},  {2.0, 1.0, 0},   {1.0, 1.0, 0},   {1.0, 2.0, 1},
      {0.0, 1.0, 1},  {-1.0, 2.0, 2},  {-1.0, 1.0, 3},  {-2.0, 1.0, 3},
      {-1.0, 0.0, 3}, {-2.0, -1.0, 4}, {-1.0, -1.0, 4}, {-1.0, -2.0, 5},
      {0.0, -1.0, 6}, {1.0, -2.0, 6},  {1.0, -1.0, 7},  {2.0, -1.0, 7},
  };

  for (const OctantCase& tc : cases) {
    CoordinateSequence coords{CoordinateXY(0.0, 0.0),
                              CoordinateXY(tc.x1, tc.y1)};
    BasicSegmentString ss(&coords, nullptr);
    EXPECT_EQ(ss.getSegmentOctant(0), tc.expected_octant)
        << "dx=" << tc.x1 << ", dy=" << tc.y1;
    EXPECT_EQ(ss.getSegmentOctant(1), -1);
    EXPECT_EQ(ss.getSegmentOctant(2), -1);
  }

  CoordinateSequence single{CoordinateXY(5.0, 5.0)};
  BasicSegmentString ss_single(&single, nullptr);
  EXPECT_EQ(ss_single.getSegmentOctant(0), -1);

  CoordinateSequence zero_len{CoordinateXY(5.0, 5.0), CoordinateXY(5.0, 5.0)};
  BasicSegmentString ss_zero(&zero_len, nullptr);
  EXPECT_THROW(ss_zero.getSegmentOctant(0), IllegalArgumentException);

  const SegmentString& base_ref = ss_zero;
  EXPECT_EQ(base_ref.getSegmentOctant(0), 0);
}

TEST(BasicSegmentStringTest, RingClosureAndNavigation) {
  CoordinateSequence ring{CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 0.0),
                          CoordinateXY(0.0, 10.0), CoordinateXY(0.0, 0.0)};
  BasicSegmentString ss(&ring, nullptr);

  EXPECT_TRUE(ss.isClosed());
  EXPECT_TRUE(ss.nextInRing(0).equals2D(CoordinateXY(10.0, 0.0)));
  EXPECT_TRUE(ss.nextInRing(3).equals2D(CoordinateXY(10.0, 0.0)));
  EXPECT_TRUE(ss.prevInRing(0).equals2D(CoordinateXY(0.0, 10.0)));
  EXPECT_TRUE(ss.prevInRing(1).equals2D(CoordinateXY(0.0, 0.0)));
}

}  // namespace
}  // namespace geos_google
