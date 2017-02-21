// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests WKTReader.cpp.
//
// See also:
//   https://trac.osgeo.org/geos/browser/trunk/src/io/WKTReader.cpp
//   https://trac.osgeo.org/geos/browser/trunk/tests/unit/io/WKTReaderTest.cpp

#include <memory>

#include "gunit.h"
#include "geom/CoordinateSequence.h"
#include "geom/Geometry.h"
#include "io/WKTReader.h"

namespace geos {
namespace io {
namespace {

typedef std::unique_ptr<geos::geom::Geometry> GeomPtr;
typedef std::unique_ptr<geos::geom::CoordinateSequence> CoordSeqPtr;

TEST(WktreaderTest, Points) {
  WKTReader reader;
  GeomPtr geom(reader.read("POINT(0 0)"));
  ASSERT_NE(nullptr, geom);
  CoordSeqPtr coords(geom->getCoordinates());
  ASSERT_NE(nullptr, coords);
  EXPECT_EQ(2, coords->getDimension());
  EXPECT_EQ(0, coords->getX(0));
  EXPECT_EQ(0, coords->getY(0));
}

}  // namespace
}  // namespace io
}  // namespace geos
