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

#include "third_party/geos/include/geos/geom/CoordinateSequence.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateFilter.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using ::geos::DoubleInfinity;
using ::geos::DoubleNegInfinity;
using ::geos::DoubleNotANumber;
using ::geos::NO_COORD_INDEX;
using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateFilter;
using ::geos::geom::CoordinateInspector;
using ::geos::geom::CoordinateMutator;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateType;
using ::geos::geom::CoordinateXY;
using ::geos::geom::CoordinateXYM;
using ::geos::geom::CoordinateXYZM;
using ::geos::geom::Envelope;
using ::geos::util::IllegalArgumentException;
using ::testing::ElementsAre;
using ::testing::IsNull;

TEST(CoordinateSequenceTest, DefaultAndDimensionConstructors) {
  CoordinateSequence def;
  EXPECT_TRUE(def.isEmpty());
  EXPECT_EQ(def.size(), 0u);
  EXPECT_EQ(def.getSize(), 0u);
  EXPECT_TRUE(def.hasZ());
  EXPECT_FALSE(def.hasM());
  EXPECT_EQ(def.getDimension(), 3u);

  CoordinateSequence empty_zero(0, std::size_t{0});
  EXPECT_TRUE(empty_zero.isEmpty());
  EXPECT_EQ(empty_zero.size(), 0u);
  EXPECT_TRUE(empty_zero.hasZ());
  EXPECT_FALSE(empty_zero.hasM());
  EXPECT_EQ(empty_zero.getDimension(), 3u);

  CoordinateSequence auto_2d(2, std::size_t{0});
  EXPECT_FALSE(auto_2d.isEmpty());
  EXPECT_EQ(auto_2d.size(), 2u);
  EXPECT_FALSE(auto_2d.hasZ());
  EXPECT_FALSE(auto_2d.hasM());
  EXPECT_EQ(auto_2d.getDimension(), 2u);
  EXPECT_EQ(auto_2d.getX(0), 0.0);
  EXPECT_EQ(auto_2d.getY(0), 0.0);
  EXPECT_TRUE(std::isnan(auto_2d.getAt<Coordinate>(0).z));

  CoordinateSequence auto_3d(2, std::size_t{0});
  auto_3d[0].z = 7.5;
  EXPECT_TRUE(auto_3d.hasZ());
  EXPECT_FALSE(auto_3d.hasM());
  EXPECT_EQ(auto_3d.getDimension(), 3u);

  CoordinateSequence seq2(3, std::size_t{2});
  EXPECT_EQ(seq2.size(), 3u);
  EXPECT_FALSE(seq2.hasZ());
  EXPECT_FALSE(seq2.hasM());
  EXPECT_EQ(seq2.getDimension(), 2u);
  EXPECT_TRUE(std::isnan(seq2.getOrdinate(0, CoordinateSequence::Z)));

  CoordinateSequence seq3(3, std::size_t{3});
  EXPECT_EQ(seq3.size(), 3u);
  EXPECT_TRUE(seq3.hasZ());
  EXPECT_FALSE(seq3.hasM());
  EXPECT_EQ(seq3.getDimension(), 3u);
  EXPECT_TRUE(std::isnan(seq3.getAt<Coordinate>(1).z));

  CoordinateSequence seq4(3, std::size_t{4});
  EXPECT_EQ(seq4.size(), 3u);
  EXPECT_TRUE(seq4.hasZ());
  EXPECT_TRUE(seq4.hasM());
  EXPECT_EQ(seq4.getDimension(), 4u);
  EXPECT_TRUE(std::isnan(seq4.getAt<CoordinateXYZM>(2).z));
  EXPECT_TRUE(std::isnan(seq4.getAt<CoordinateXYZM>(2).m));

  EXPECT_THROW(CoordinateSequence(2, std::size_t{1}), IllegalArgumentException);
  EXPECT_THROW(CoordinateSequence(2, std::size_t{5}), IllegalArgumentException);
}

TEST(CoordinateSequenceTest, HasZHasMAndFactoryConstructors) {
  CoordinateSequence xy = CoordinateSequence::XY(2);
  EXPECT_EQ(xy.size(), 2u);
  EXPECT_FALSE(xy.hasZ());
  EXPECT_FALSE(xy.hasM());
  EXPECT_EQ(xy.getDimension(), 2u);

  CoordinateSequence xyz = CoordinateSequence::XYZ(2);
  EXPECT_EQ(xyz.size(), 2u);
  EXPECT_TRUE(xyz.hasZ());
  EXPECT_FALSE(xyz.hasM());
  EXPECT_EQ(xyz.getDimension(), 3u);

  CoordinateSequence xym = CoordinateSequence::XYM(2);
  EXPECT_EQ(xym.size(), 2u);
  EXPECT_FALSE(xym.hasZ());
  EXPECT_TRUE(xym.hasM());
  EXPECT_EQ(xym.getDimension(), 3u);

  CoordinateSequence xyzm = CoordinateSequence::XYZM(2);
  EXPECT_EQ(xyzm.size(), 2u);
  EXPECT_TRUE(xyzm.hasZ());
  EXPECT_TRUE(xyzm.hasM());
  EXPECT_EQ(xyzm.getDimension(), 4u);

  EXPECT_TRUE(CoordinateSequence::XY(0).isEmpty());
  EXPECT_TRUE(CoordinateSequence::XYZ(0).isEmpty());
  EXPECT_TRUE(CoordinateSequence::XYM(0).isEmpty());
  EXPECT_TRUE(CoordinateSequence::XYZM(0).isEmpty());

  CoordinateSequence uninit_xyz(2, true, false, false);
  EXPECT_EQ(uninit_xyz.size(), 2u);
  EXPECT_TRUE(uninit_xyz.hasZ());
  EXPECT_FALSE(uninit_xyz.hasM());
  EXPECT_EQ(uninit_xyz.getDimension(), 3u);
  EXPECT_EQ(uninit_xyz[0].x, 0.0);
  EXPECT_EQ(uninit_xyz[0].y, 0.0);
  EXPECT_EQ(uninit_xyz[0].z, 0.0);

  CoordinateSequence uninit_xyzm(2, true, true, false);
  EXPECT_EQ(uninit_xyzm.size(), 2u);
  EXPECT_TRUE(uninit_xyzm.hasZ());
  EXPECT_TRUE(uninit_xyzm.hasM());
  EXPECT_EQ(uninit_xyzm.getDimension(), 4u);
  EXPECT_EQ(uninit_xyzm.getAt<CoordinateXYZM>(1).z, 0.0);
  EXPECT_EQ(uninit_xyzm.getAt<CoordinateXYZM>(1).m, 0.0);

  CoordinateSequence uninit_xy(2, false, false, false);
  EXPECT_EQ(uninit_xy.size(), 2u);
  EXPECT_FALSE(uninit_xy.hasZ());
  EXPECT_FALSE(uninit_xy.hasM());
  EXPECT_EQ(uninit_xy.getDimension(), 2u);
  EXPECT_EQ(uninit_xy.getX(0), 0.0);
  EXPECT_EQ(uninit_xy.getY(0), 0.0);
  EXPECT_TRUE(std::isnan(uninit_xy.getOrdinate(0, CoordinateSequence::Z)));

  CoordinateSequence uninit_xym(2, false, true, false);
  EXPECT_EQ(uninit_xym.size(), 2u);
  EXPECT_FALSE(uninit_xym.hasZ());
  EXPECT_TRUE(uninit_xym.hasM());
  EXPECT_EQ(uninit_xym.getDimension(), 3u);
  EXPECT_EQ(uninit_xym.getOrdinate(0, CoordinateSequence::M), 0.0);
  EXPECT_TRUE(std::isnan(uninit_xym.getOrdinate(0, CoordinateSequence::Z)));
}

TEST(CoordinateSequenceTest, InitializerListConstructors) {
  CoordinateSequence empty_coords(std::initializer_list<Coordinate>{});
  EXPECT_TRUE(empty_coords.isEmpty());
  EXPECT_TRUE(empty_coords.hasZ());
  EXPECT_EQ(empty_coords.getDimension(), 3u);

  CoordinateSequence coords_2d(
      std::initializer_list<Coordinate>{Coordinate(1, 2), Coordinate(3, 4)});
  EXPECT_EQ(coords_2d.size(), 2u);
  EXPECT_FALSE(coords_2d.hasZ());
  EXPECT_FALSE(coords_2d.hasM());
  EXPECT_EQ(coords_2d.getDimension(), 2u);
  EXPECT_EQ(coords_2d.getX(1), 3.0);
  EXPECT_EQ(coords_2d.getY(1), 4.0);

  CoordinateSequence coords_3d(std::initializer_list<Coordinate>{
      Coordinate(1, 2, 3), Coordinate(4, 5, 6)});
  EXPECT_EQ(coords_3d.size(), 2u);
  EXPECT_TRUE(coords_3d.hasZ());
  EXPECT_FALSE(coords_3d.hasM());
  EXPECT_EQ(coords_3d.getDimension(), 3u);
  EXPECT_EQ(coords_3d[0].z, 3.0);
  EXPECT_EQ(coords_3d[1].z, 6.0);

  CoordinateSequence empty_xy(std::initializer_list<CoordinateXY>{});
  EXPECT_TRUE(empty_xy.isEmpty());
  EXPECT_FALSE(empty_xy.hasZ());
  EXPECT_FALSE(empty_xy.hasM());
  EXPECT_EQ(empty_xy.getDimension(), 2u);

  CoordinateSequence empty_xym(std::initializer_list<CoordinateXYM>{});
  EXPECT_TRUE(empty_xym.isEmpty());
  EXPECT_FALSE(empty_xym.hasZ());
  EXPECT_TRUE(empty_xym.hasM());
  EXPECT_EQ(empty_xym.getDimension(), 3u);

  CoordinateSequence empty_xyzm(std::initializer_list<CoordinateXYZM>{});
  EXPECT_TRUE(empty_xyzm.isEmpty());
  EXPECT_TRUE(empty_xyzm.hasZ());
  EXPECT_TRUE(empty_xyzm.hasM());
  EXPECT_EQ(empty_xyzm.getDimension(), 4u);

  CoordinateSequence coords_xy(std::initializer_list<CoordinateXY>{
      CoordinateXY(10, 20), CoordinateXY(30, 40)});
  EXPECT_EQ(coords_xy.size(), 2u);
  EXPECT_FALSE(coords_xy.hasZ());
  EXPECT_FALSE(coords_xy.hasM());
  EXPECT_EQ(coords_xy.getDimension(), 2u);
  EXPECT_EQ(coords_xy.getAt<CoordinateXY>(0), CoordinateXY(10, 20));
  EXPECT_EQ(coords_xy.getAt<CoordinateXY>(1), CoordinateXY(30, 40));

  CoordinateSequence coords_xym(std::initializer_list<CoordinateXYM>{
      CoordinateXYM(1, 2, 100), CoordinateXYM(3, 4, 200)});
  EXPECT_EQ(coords_xym.size(), 2u);
  EXPECT_FALSE(coords_xym.hasZ());
  EXPECT_TRUE(coords_xym.hasM());
  EXPECT_EQ(coords_xym.getDimension(), 3u);
  EXPECT_TRUE(std::isnan(coords_xym.getOrdinate(0, CoordinateSequence::Z)));
  EXPECT_EQ(coords_xym.getOrdinate(0, CoordinateSequence::M), 100.0);
  EXPECT_EQ(coords_xym.getOrdinate(1, CoordinateSequence::M), 200.0);
  CoordinateXYM out_xym_0;
  CoordinateXYM out_xym_1;
  coords_xym.getAt(0, out_xym_0);
  coords_xym.getAt(1, out_xym_1);
  EXPECT_TRUE(out_xym_0.equals3D(CoordinateXYM(1, 2, 100)));
  EXPECT_TRUE(out_xym_1.equals3D(CoordinateXYM(3, 4, 200)));

  CoordinateSequence coords_xyzm(std::initializer_list<CoordinateXYZM>{
      CoordinateXYZM(1, 2, 3, 4), CoordinateXYZM(5, 6, 7, 8)});
  EXPECT_EQ(coords_xyzm.size(), 2u);
  EXPECT_TRUE(coords_xyzm.hasZ());
  EXPECT_TRUE(coords_xyzm.hasM());
  EXPECT_EQ(coords_xyzm.getDimension(), 4u);
  EXPECT_EQ(coords_xyzm.getAt<CoordinateXYZM>(0).z, 3.0);
  EXPECT_EQ(coords_xyzm.getAt<CoordinateXYZM>(0).m, 4.0);
  EXPECT_EQ(coords_xyzm.getAt<CoordinateXYZM>(1).z, 7.0);
  EXPECT_EQ(coords_xyzm.getAt<CoordinateXYZM>(1).m, 8.0);
}

TEST(CoordinateSequenceTest, AccessorsAndMutators) {
  CoordinateSequence seq = CoordinateSequence::XYZM(3);
  seq.setAt(CoordinateXY(1, 2), 0);
  seq.setAt(Coordinate(3, 4, 5), 1);
  seq.setAt(CoordinateXYZM(6, 7, 8, 9), 2);

  EXPECT_EQ(seq.getCoordinateType(), CoordinateType::XYZM);
  EXPECT_NE(seq.data(), nullptr);
  const CoordinateSequence& cseq = seq;
  EXPECT_EQ(cseq.data(), seq.data());

  CoordinateXY out_xy;
  cseq.getAt(0, out_xy);
  EXPECT_EQ(out_xy, CoordinateXY(1, 2));

  Coordinate out_xyz;
  cseq.getAt(1, out_xyz);
  EXPECT_TRUE(out_xyz.equals3D(Coordinate(3, 4, 5)));

  CoordinateXYM out_xym;
  cseq.getAt(2, out_xym);
  EXPECT_TRUE(out_xym.equals3D(CoordinateXYM(6, 7, 9)));

  CoordinateXYZM out_xyzm;
  cseq.getAt(2, out_xyzm);
  EXPECT_TRUE(out_xyzm.equals4D(CoordinateXYZM(6, 7, 8, 9)));

  EXPECT_EQ(seq.front<CoordinateXY>(), CoordinateXY(1, 2));
  EXPECT_EQ(cseq.front<CoordinateXY>(), CoordinateXY(1, 2));
  EXPECT_EQ(seq.back<CoordinateXY>(), CoordinateXY(6, 7));
  EXPECT_EQ(cseq.back<CoordinateXY>(), CoordinateXY(6, 7));
  seq.front<CoordinateXYZM>().m = 11.0;
  seq.back<CoordinateXYZM>().z = 88.0;
  EXPECT_EQ(cseq.getOrdinate(0, CoordinateSequence::M), 11.0);
  EXPECT_EQ(cseq.getOrdinate(2, CoordinateSequence::Z), 88.0);

  EXPECT_EQ(cseq[1], Coordinate(3, 4, 5));
  seq[1].x = 33.0;
  EXPECT_EQ(cseq.getX(1), 33.0);
  EXPECT_EQ(cseq.getY(1), 4.0);

  seq.setOrdinate(0, CoordinateSequence::X, 1.5);
  seq.setOrdinate(0, CoordinateSequence::Y, 2.5);
  seq.setOrdinate(0, CoordinateSequence::Z, 3.5);
  seq.setOrdinate(0, CoordinateSequence::M, 4.5);
  EXPECT_EQ(cseq.getOrdinate(0, CoordinateSequence::X), 1.5);
  EXPECT_EQ(cseq.getOrdinate(0, CoordinateSequence::Y), 2.5);
  EXPECT_EQ(cseq.getOrdinate(0, CoordinateSequence::Z), 3.5);
  EXPECT_EQ(cseq.getOrdinate(0, CoordinateSequence::M), 4.5);
  EXPECT_TRUE(std::isnan(cseq.getOrdinate(0, 4)));
  EXPECT_THROW(seq.setOrdinate(0, 4, 1.0), IllegalArgumentException);

  CoordinateSequence xyz(std::initializer_list<Coordinate>{
      Coordinate(1, 2, 3), Coordinate(4, 5, 6)});
  Coordinate xyz_copy_at;
  xyz.getAt(1, xyz_copy_at);
  EXPECT_TRUE(xyz_copy_at.equals3D(Coordinate(4, 5, 6)));

  CoordinateSequence xy = CoordinateSequence::XY(1);
  EXPECT_EQ(xy.getCoordinateType(), CoordinateType::XYZ);
  EXPECT_TRUE(std::isnan(xy.getOrdinate(0, CoordinateSequence::Z)));
  EXPECT_TRUE(std::isnan(xy.getOrdinate(0, CoordinateSequence::M)));
  EXPECT_THROW(xy.setOrdinate(0, CoordinateSequence::Z, 1.0),
               IllegalArgumentException);
  EXPECT_THROW(xy.setOrdinate(0, CoordinateSequence::M, 1.0),
               IllegalArgumentException);

  CoordinateSequence xym = CoordinateSequence::XYM(1);
  EXPECT_EQ(xym.getCoordinateType(), CoordinateType::XYZM);
  xym.setOrdinate(0, CoordinateSequence::M, 12.0);
  EXPECT_EQ(xym.getOrdinate(0, CoordinateSequence::M), 12.0);
  EXPECT_TRUE(std::isnan(xym.getOrdinate(0, CoordinateSequence::Z)));
  EXPECT_THROW(xym.setOrdinate(0, CoordinateSequence::Z, 1.0),
               IllegalArgumentException);

  seq.setAt(CoordinateXYM(10, 20, 30), 0);
  CoordinateXYM updated_xym;
  seq.getAt(0, updated_xym);
  EXPECT_TRUE(updated_xym.equals3D(CoordinateXYM(10, 20, 30)));
  EXPECT_TRUE(std::isnan(seq.getOrdinate(0, CoordinateSequence::Z)));
  EXPECT_EQ(seq.getOrdinate(0, CoordinateSequence::M), 30.0);
  EXPECT_TRUE(cseq.front().equals3D(Coordinate(10, 20, DoubleNotANumber)));
  EXPECT_TRUE(
      cseq.back<CoordinateXYZM>().equals4D(CoordinateXYZM(6, 7, 88, 9)));
}

TEST(CoordinateSequenceTest, SetPointsAndToVector) {
  CoordinateSequence seq = CoordinateSequence::XYZM(1);
  std::vector<Coordinate> pts_3d = {Coordinate(1, 2, 3), Coordinate(4, 5, 6)};
  seq.setPoints(pts_3d);
  EXPECT_EQ(seq.size(), 2u);
  EXPECT_TRUE(seq.hasZ());
  EXPECT_FALSE(seq.hasM());
  EXPECT_EQ(seq.getDimension(), 3u);
  EXPECT_TRUE(seq[0].equals3D(Coordinate(1, 2, 3)));
  EXPECT_TRUE(seq[1].equals3D(Coordinate(4, 5, 6)));

  std::vector<Coordinate> pts_nan_z = {Coordinate(7, 8), Coordinate(9, 10)};
  seq.setPoints(pts_nan_z);
  EXPECT_FALSE(seq.hasZ());
  EXPECT_EQ(seq.getDimension(), 2u);

  std::vector<CoordinateXY> pts_xy = {CoordinateXY(11, 12),
                                      CoordinateXY(13, 14)};
  seq.setPoints(pts_xy);
  EXPECT_EQ(seq.size(), 2u);
  EXPECT_FALSE(seq.hasZ());
  EXPECT_FALSE(seq.hasM());
  EXPECT_EQ(seq.getDimension(), 2u);
  EXPECT_EQ(seq.getAt<CoordinateXY>(0), CoordinateXY(11, 12));
  EXPECT_EQ(seq.getAt<CoordinateXY>(1), CoordinateXY(13, 14));

  // toVector(Coordinate) fast path for XYZ.
  CoordinateSequence xyz(std::initializer_list<Coordinate>{
      Coordinate(1, 2, 3), Coordinate(4, 5, 6)});
  std::vector<Coordinate> out_3d = {Coordinate(0, 0, 0)};
  xyz.toVector(out_3d);
  ASSERT_EQ(out_3d.size(), 3u);
  EXPECT_TRUE(out_3d[0].equals3D(Coordinate(0, 0, 0)));
  EXPECT_TRUE(out_3d[1].equals3D(Coordinate(1, 2, 3)));
  EXPECT_TRUE(out_3d[2].equals3D(Coordinate(4, 5, 6)));

  // toVector(Coordinate) else-if (hasZ()) path for XYZM.
  CoordinateSequence xyzm(
      std::initializer_list<CoordinateXYZM>{CoordinateXYZM(10, 20, 30, 40)});
  std::vector<Coordinate> out_xyzm;
  xyzm.toVector(out_xyzm);
  ASSERT_EQ(out_xyzm.size(), 1u);
  EXPECT_TRUE(out_xyzm[0].equals3D(Coordinate(10, 20, 30)));

  // toVector(Coordinate) else (!hasZ()) path for XYM.
  CoordinateSequence xym(
      std::initializer_list<CoordinateXYM>{CoordinateXYM(5, 6, 7)});
  std::vector<Coordinate> out_xym;
  xym.toVector(out_xym);
  ASSERT_EQ(out_xym.size(), 1u);
  EXPECT_EQ(out_xym[0].x, 5.0);
  EXPECT_EQ(out_xym[0].y, 6.0);
  EXPECT_TRUE(std::isnan(out_xym[0].z));

  // toVector(CoordinateXY) preserves existing items and omits Z/M.
  std::vector<CoordinateXY> out_xy = {CoordinateXY(-1, -2)};
  xyzm.toVector(out_xy);
  xym.toVector(out_xy);
  EXPECT_THAT(out_xy, ElementsAre(CoordinateXY(-1, -2), CoordinateXY(10, 20),
                                  CoordinateXY(5, 6)));

  seq.setPoints(std::vector<Coordinate>{});
  EXPECT_TRUE(seq.isEmpty());
  seq.setPoints(std::vector<CoordinateXY>{});
  EXPECT_TRUE(seq.isEmpty());
}

TEST(CoordinateSequenceTest, AddSingleAndIterators) {
  CoordinateSequence seq = CoordinateSequence::XYZ(0);
  seq.add(1.0, 2.0);
  seq.add(Coordinate(3.0, 4.0, 5.0));
  seq.add(Coordinate(3.0, 4.0, 99.0), false);  // Equal 2D -> skipped.
  EXPECT_EQ(seq.size(), 2u);
  seq.add(Coordinate(3.0, 4.0, 6.0), true);  // Allowed.
  EXPECT_EQ(seq.size(), 3u);

  // Positional insert with deduplication against prev and next.
  seq.add(std::size_t{0}, Coordinate(1.0, 2.0), false);  // Matches next[0].
  seq.add(std::size_t{1}, Coordinate(1.0, 2.0), false);  // Matches prev[0].
  seq.add(std::size_t{1}, Coordinate(3.0, 4.0), false);  // Matches next[1].
  seq.add(std::size_t{3}, Coordinate(3.0, 4.0), false);  // Matches prev[2].
  EXPECT_EQ(seq.size(), 3u);

  seq.add(std::size_t{1}, Coordinate(2.0, 3.0, 4.0), false);
  EXPECT_EQ(seq.size(), 4u);
  EXPECT_TRUE(seq[1].equals3D(Coordinate(2.0, 3.0, 4.0)));

  // Empty sequence positional insert with allowRepeated=false, then true.
  CoordinateSequence single = CoordinateSequence::XY(0);
  single.add(std::size_t{0}, CoordinateXY(9, 8), false);
  EXPECT_EQ(single.size(), 1u);
  EXPECT_EQ(single.getAt<CoordinateXY>(0), CoordinateXY(9, 8));
  single.add(std::size_t{0}, CoordinateXY(9, 8), true);
  EXPECT_EQ(single.size(), 2u);
  EXPECT_EQ(single.getAt<CoordinateXY>(0), CoordinateXY(9, 8));
  EXPECT_EQ(single.getAt<CoordinateXY>(1), CoordinateXY(9, 8));

  // Self-referential add when reallocation occurs and when capacity suffices.
  CoordinateSequence self_seq = CoordinateSequence::XYZ(0);
  self_seq.add(Coordinate(1, 2, 3));
  self_seq.add(self_seq[0]);  // Likely exceeds capacity.
  self_seq.reserve(16);
  self_seq.add(self_seq[0]);  // Within capacity.
  ASSERT_EQ(self_seq.size(), 3u);
  EXPECT_TRUE(self_seq[1].equals3D(Coordinate(1, 2, 3)));
  EXPECT_TRUE(self_seq[2].equals3D(Coordinate(1, 2, 3)));

  // Range-based add over vector iterators.
  std::vector<Coordinate> extra = {Coordinate(3, 4, 6), Coordinate(7, 8, 9),
                                   Coordinate(7, 8, 10)};
  seq.add(extra.begin(), extra.end(), false);
  EXPECT_EQ(seq.size(), 5u);
  EXPECT_TRUE(seq.back().equals3D(Coordinate(7, 8, 9)));

  // Positional iterator range insert.
  std::vector<Coordinate> mid = {Coordinate(10, 11, 12),
                                 Coordinate(13, 14, 15)};
  seq.add(std::size_t{1}, mid.begin(), mid.end());
  EXPECT_EQ(seq.size(), 7u);
  EXPECT_TRUE(seq[1].equals3D(Coordinate(10, 11, 12)));
  EXPECT_TRUE(seq[2].equals3D(Coordinate(13, 14, 15)));
}

TEST(CoordinateSequenceTest, AddSequenceVariants) {
  CoordinateSequence dst_xyz = CoordinateSequence::XYZ(0);
  CoordinateSequence src_xyz(std::initializer_list<Coordinate>{
      Coordinate(1, 1, 10), Coordinate(2, 2, 20), Coordinate(3, 3, 30)});
  dst_xyz.add(src_xyz);
  ASSERT_EQ(dst_xyz.size(), 3u);
  EXPECT_TRUE(dst_xyz[2].equals3D(Coordinate(3, 3, 30)));

  // Cross-type add: XYZ -> XYZM, XYZM -> XYZ, XY -> XYM, and XYZM -> XYZM.
  CoordinateSequence dst_xyzm = CoordinateSequence::XYZM(0);
  dst_xyzm.add(src_xyz, std::size_t{1}, std::size_t{2});
  ASSERT_EQ(dst_xyzm.size(), 2u);
  EXPECT_TRUE(dst_xyzm.getAt<CoordinateXYZM>(0).equals3D(Coordinate(2, 2, 20)));
  EXPECT_TRUE(std::isnan(dst_xyzm.getAt<CoordinateXYZM>(0).m));

  CoordinateSequence src_xyzm(std::initializer_list<CoordinateXYZM>{
      CoordinateXYZM(4, 4, 40, 400), CoordinateXYZM(5, 5, 50, 500)});
  dst_xyzm.add(src_xyzm);
  ASSERT_EQ(dst_xyzm.size(), 4u);
  EXPECT_TRUE(dst_xyzm.getAt<CoordinateXYZM>(3).equals4D(
      CoordinateXYZM(5, 5, 50, 500)));

  dst_xyz.add(src_xyzm);
  ASSERT_EQ(dst_xyz.size(), 5u);
  EXPECT_TRUE(dst_xyz[3].equals3D(Coordinate(4, 4, 40)));

  CoordinateSequence dst_xym = CoordinateSequence::XYM(0);
  CoordinateSequence src_xy(std::initializer_list<CoordinateXY>{
      CoordinateXY(7, 8), CoordinateXY(9, 10)});
  dst_xym.add(src_xy);
  ASSERT_EQ(dst_xym.size(), 2u);
  EXPECT_EQ(dst_xym.getX(1), 9.0);
  EXPECT_TRUE(std::isnan(dst_xym.getOrdinate(1, CoordinateSequence::Z)));
  EXPECT_TRUE(std::isnan(dst_xym.getOrdinate(1, CoordinateSequence::M)));

  // Empty source with allowRepeated does nothing.
  CoordinateSequence empty = CoordinateSequence::XYZ(0);
  dst_xyz.add(empty, false);
  dst_xyz.add(empty, false, false);
  EXPECT_EQ(dst_xyz.size(), 5u);

  // Deduplicating range add: leading dups only -> early return.
  CoordinateSequence base(
      std::initializer_list<Coordinate>{Coordinate(0, 0), Coordinate(1, 1)});
  CoordinateSequence all_dup(
      std::initializer_list<Coordinate>{Coordinate(1, 1), Coordinate(1, 1)});
  base.add(all_dup, false);
  EXPECT_EQ(base.size(), 2u);

  // Leading dups + middle dup run + trailing dup run ending at `to`.
  CoordinateSequence dup_end(std::initializer_list<Coordinate>{
      Coordinate(1, 1), Coordinate(1, 1), Coordinate(2, 2), Coordinate(3, 3),
      Coordinate(3, 3), Coordinate(3, 3), Coordinate(4, 4), Coordinate(4, 4)});
  base.add(dup_end, false);
  std::vector<CoordinateXY> out;
  base.toVector(out);
  EXPECT_THAT(out, ElementsAre(CoordinateXY(0, 0), CoordinateXY(1, 1),
                               CoordinateXY(2, 2), CoordinateXY(3, 3),
                               CoordinateXY(4, 4)));

  // Empty target with duplicate pair right at the start of source.
  CoordinateSequence dedup_start = CoordinateSequence::XY(0);
  dedup_start.add(dup_end, std::size_t{0}, std::size_t{2}, false);
  out.clear();
  dedup_start.toVector(out);
  EXPECT_THAT(out, ElementsAre(CoordinateXY(1, 1), CoordinateXY(2, 2)));

  // Single-element subrange [2, 2] with allowRepeated=false.
  CoordinateSequence single_sub = CoordinateSequence::XY(0);
  single_sub.add(dup_end, std::size_t{2}, std::size_t{2}, false);
  out.clear();
  single_sub.toVector(out);
  EXPECT_THAT(out, ElementsAre(CoordinateXY(2, 2)));

  // Middle dup run followed by multiple unique trailing points.
  CoordinateSequence dedup_mid = CoordinateSequence::XY(0);
  CoordinateSequence src_mid(std::initializer_list<CoordinateXY>{
      CoordinateXY(0, 0), CoordinateXY(1, 1), CoordinateXY(1, 1),
      CoordinateXY(2, 2), CoordinateXY(3, 3)});
  dedup_mid.add(src_mid, false);
  out.clear();
  dedup_mid.toVector(out);
  EXPECT_THAT(out, ElementsAre(CoordinateXY(0, 0), CoordinateXY(1, 1),
                               CoordinateXY(2, 2), CoordinateXY(3, 3)));

  // allowRepeated=true subrange and forward/reverse direction overloads.
  CoordinateSequence dir_dst(
      std::initializer_list<Coordinate>{Coordinate(1, 1)});
  CoordinateSequence dir_src(std::initializer_list<Coordinate>{
      Coordinate(3, 3), Coordinate(2, 2), Coordinate(2, 2), Coordinate(1, 1)});
  dir_dst.add(dir_src, std::size_t{1}, std::size_t{2}, true);
  EXPECT_EQ(dir_dst.size(), 3u);
  dir_dst.add(dir_src, false, true);
  dir_dst.add(dir_src, false, false);
  out.clear();
  dir_dst.toVector(out);
  EXPECT_THAT(out, ElementsAre(CoordinateXY(1, 1), CoordinateXY(2, 2),
                               CoordinateXY(2, 2), CoordinateXY(3, 3),
                               CoordinateXY(2, 2), CoordinateXY(1, 1),
                               CoordinateXY(2, 2), CoordinateXY(3, 3)));

  CoordinateSequence rev_all = CoordinateSequence::XY(0);
  rev_all.add(dir_src, true, false);
  out.clear();
  rev_all.toVector(out);
  EXPECT_THAT(out, ElementsAre(CoordinateXY(1, 1), CoordinateXY(2, 2),
                               CoordinateXY(2, 2), CoordinateXY(3, 3)));
}

TEST(CoordinateSequenceTest, CloneClearReserveResizePopBack) {
  CoordinateSequence orig(std::initializer_list<CoordinateXYZM>{
      CoordinateXYZM(1, 2, 3, 4), CoordinateXYZM(5, 6, 7, 8)});
  std::unique_ptr<CoordinateSequence> copy = orig.clone();
  EXPECT_TRUE(orig.equalsIdentical(*copy));

  copy->pop_back();
  EXPECT_EQ(copy->size(), 1u);
  EXPECT_EQ(orig.size(), 2u);

  CoordinateSequence xyz(std::initializer_list<Coordinate>{
      Coordinate(1, 2, 3), Coordinate(4, 5, 6)});
  xyz.pop_back();
  EXPECT_EQ(xyz.size(), 1u);

  copy->resize(4);
  EXPECT_EQ(copy->size(), 4u);
  copy->reserve(10);
  EXPECT_EQ(copy->size(), 4u);
  copy->clear();
  EXPECT_TRUE(copy->isEmpty());
}

TEST(CoordinateSequenceTest, RingAndCloseRing) {
  CoordinateSequence empty = CoordinateSequence::XYZ(0);
  EXPECT_FALSE(empty.isRing());
  empty.closeRing();
  EXPECT_TRUE(empty.isEmpty());

  CoordinateSequence short_closed(std::initializer_list<Coordinate>{
      Coordinate(0, 0), Coordinate(1, 0), Coordinate(0, 0)});
  EXPECT_FALSE(short_closed.isRing());

  CoordinateSequence single(
      std::initializer_list<Coordinate>{Coordinate(2, 3, 4)});
  single.closeRing(false);
  EXPECT_EQ(single.size(), 1u);
  single.closeRing(true);
  EXPECT_EQ(single.size(), 2u);
  EXPECT_TRUE(single[1].equals3D(Coordinate(2, 3, 4)));

  CoordinateSequence four_open(std::initializer_list<Coordinate>{
      Coordinate(0, 0), Coordinate(1, 0), Coordinate(1, 1), Coordinate(0, 1)});
  EXPECT_FALSE(four_open.isRing());

  CoordinateSequence open_ring(std::initializer_list<CoordinateXYZM>{
      CoordinateXYZM(0, 0, 10, 20), CoordinateXYZM(1, 0, 11, 21),
      CoordinateXYZM(1, 1, 12, 22)});
  EXPECT_FALSE(open_ring.isRing());
  open_ring.closeRing();
  EXPECT_EQ(open_ring.size(), 4u);
  EXPECT_TRUE(open_ring.isRing());
  EXPECT_TRUE(
      open_ring.back<CoordinateXYZM>().equals4D(CoordinateXYZM(0, 0, 10, 20)));

  // Already closed: allowRepeated=false leaves size unchanged, true appends.
  open_ring.closeRing(false);
  EXPECT_EQ(open_ring.size(), 4u);
  open_ring.closeRing(true);
  EXPECT_EQ(open_ring.size(), 5u);
}

TEST(CoordinateSequenceTest, HasRepeatedAndInvalidPoints) {
  CoordinateSequence empty = CoordinateSequence::XYZ(0);
  EXPECT_FALSE(empty.hasRepeatedPoints());

  CoordinateSequence single(
      std::initializer_list<Coordinate>{Coordinate(1, 2)});
  EXPECT_FALSE(single.hasRepeatedPoints());
  EXPECT_FALSE(single.hasRepeatedOrInvalidPoints());

  CoordinateSequence non_consec(std::initializer_list<Coordinate>{
      Coordinate(0, 0), Coordinate(0, 5), Coordinate(1, 0), Coordinate(0, 0)});
  EXPECT_FALSE(non_consec.hasRepeatedPoints());
  EXPECT_FALSE(non_consec.hasRepeatedOrInvalidPoints());

  CoordinateSequence single_bad(
      std::initializer_list<Coordinate>{Coordinate(DoubleNotANumber, 0)});
  EXPECT_TRUE(single_bad.hasRepeatedOrInvalidPoints());

  CoordinateSequence consec(std::initializer_list<Coordinate>{
      Coordinate(0, 0, 1), Coordinate(1, 0, 2), Coordinate(1, 0, 3)});
  EXPECT_TRUE(consec.hasRepeatedPoints());
  EXPECT_TRUE(consec.hasRepeatedOrInvalidPoints());

  CoordinateSequence bad_first_x(std::initializer_list<Coordinate>{
      Coordinate(DoubleNotANumber, 0), Coordinate(1, 1)});
  EXPECT_TRUE(bad_first_x.hasRepeatedOrInvalidPoints());

  CoordinateSequence bad_first_y(std::initializer_list<Coordinate>{
      Coordinate(0, DoubleInfinity), Coordinate(1, 1)});
  EXPECT_TRUE(bad_first_y.hasRepeatedOrInvalidPoints());

  CoordinateSequence bad_later_x(std::initializer_list<Coordinate>{
      Coordinate(0, 0), Coordinate(DoubleNegInfinity, 1)});
  EXPECT_TRUE(bad_later_x.hasRepeatedOrInvalidPoints());

  CoordinateSequence bad_later_y(std::initializer_list<Coordinate>{
      Coordinate(0, 0), Coordinate(1, DoubleNotANumber)});
  EXPECT_TRUE(bad_later_y.hasRepeatedOrInvalidPoints());

  CoordinateSequence xyzm_valid(std::initializer_list<CoordinateXYZM>{
      CoordinateXYZM(1, 2, 3, 4), CoordinateXYZM(5, 6, 7, 8)});
  EXPECT_FALSE(xyzm_valid.hasRepeatedPoints());
  EXPECT_FALSE(xyzm_valid.hasRepeatedOrInvalidPoints());

  CoordinateSequence xyzm_rep(std::initializer_list<CoordinateXYZM>{
      CoordinateXYZM(1, 2, 3, 4), CoordinateXYZM(1, 2, 9, 10)});
  EXPECT_TRUE(xyzm_rep.hasRepeatedPoints());
  EXPECT_TRUE(xyzm_rep.hasRepeatedOrInvalidPoints());

  CoordinateSequence xyzm_bad(std::initializer_list<CoordinateXYZM>{
      CoordinateXYZM(1, 2, 3, 4), CoordinateXYZM(1, DoubleNotANumber, 3, 4)});
  EXPECT_TRUE(xyzm_bad.hasRepeatedOrInvalidPoints());
}

TEST(CoordinateSequenceTest, MinCoordinateIndexOfScrollIncreasingDirection) {
  CoordinateSequence empty = CoordinateSequence::XYZ(0);
  EXPECT_THAT(empty.minCoordinate(), IsNull());
  EXPECT_EQ(CoordinateSequence::indexOf(&CoordinateXY::getNull(), &empty),
            NO_COORD_INDEX);
  CoordinateSequence::scroll(&empty, &CoordinateXY::getNull());
  EXPECT_TRUE(empty.isEmpty());
  EXPECT_EQ(CoordinateSequence::increasingDirection(empty), 1);

  CoordinateSequence seq(std::initializer_list<CoordinateXYZM>{
      CoordinateXYZM(2, 5, 10, 100), CoordinateXYZM(1, 9, 20, 200),
      CoordinateXYZM(1, 3, 30, 300), CoordinateXYZM(2, 5, 40, 400)});

  const CoordinateXY* min_c = seq.minCoordinate();
  ASSERT_NE(min_c, nullptr);
  EXPECT_EQ(*min_c, CoordinateXY(1, 3));

  CoordinateXY target_first(2, 5);
  CoordinateXY target_mid(1, 3);
  CoordinateXY target_missing(9, 9);
  EXPECT_EQ(CoordinateSequence::indexOf(&target_first, &seq), 0u);
  EXPECT_EQ(CoordinateSequence::indexOf(&target_mid, &seq), 2u);
  EXPECT_EQ(CoordinateSequence::indexOf(&target_missing, &seq), NO_COORD_INDEX);

  // Scroll with already-first or missing is a no-op.
  CoordinateSequence::scroll(&seq, &target_first);
  EXPECT_EQ(seq.front<CoordinateXY>(), CoordinateXY(2, 5));
  CoordinateSequence::scroll(&seq, &target_missing);
  EXPECT_EQ(seq.front<CoordinateXY>(), CoordinateXY(2, 5));

  // Scroll to middle rotates coordinates and preserves Z/M.
  CoordinateSequence::scroll(&seq, &target_mid);
  EXPECT_TRUE(
      seq.getAt<CoordinateXYZM>(0).equals4D(CoordinateXYZM(1, 3, 30, 300)));
  EXPECT_TRUE(
      seq.getAt<CoordinateXYZM>(1).equals4D(CoordinateXYZM(2, 5, 40, 400)));
  EXPECT_TRUE(
      seq.getAt<CoordinateXYZM>(2).equals4D(CoordinateXYZM(2, 5, 10, 100)));
  EXPECT_TRUE(
      seq.getAt<CoordinateXYZM>(3).equals4D(CoordinateXYZM(1, 9, 20, 200)));

  // increasingDirection checks lexicographical compareTo from ends inward.
  EXPECT_EQ(CoordinateSequence::increasingDirection(CoordinateSequence(
                std::initializer_list<CoordinateXY>{CoordinateXY(1, 1)})),
            1);
  EXPECT_EQ(
      CoordinateSequence::increasingDirection(
          CoordinateSequence(std::initializer_list<CoordinateXY>{
              CoordinateXY(0, 0), CoordinateXY(1, 0), CoordinateXY(0, 0)})),
      1);
  EXPECT_EQ(CoordinateSequence::increasingDirection(
                CoordinateSequence(std::initializer_list<CoordinateXY>{
                    CoordinateXY(0, 0), CoordinateXY(1, 0), CoordinateXY(1, 0),
                    CoordinateXY(0, 0)})),
            1);
  EXPECT_EQ(CoordinateSequence::increasingDirection(
                CoordinateSequence(std::initializer_list<CoordinateXY>{
                    CoordinateXY(0, 0), CoordinateXY(1, 0), CoordinateXY(2, 0),
                    CoordinateXY(0, 0)})),
            -1);
  EXPECT_EQ(CoordinateSequence::increasingDirection(
                CoordinateSequence(std::initializer_list<CoordinateXY>{
                    CoordinateXY(0, 0), CoordinateXY(2, 0), CoordinateXY(1, 0),
                    CoordinateXY(0, 0)})),
            1);
}

TEST(CoordinateSequenceTest, ReverseSortEqualsAndEqualsIdentical) {
  CoordinateSequence empty = CoordinateSequence::XYZ(0);
  empty.reverse();
  empty.sort();
  EXPECT_TRUE(empty.isEmpty());

  CoordinateSequence xyz_single(
      std::initializer_list<Coordinate>{Coordinate(9, 8, 7)});
  xyz_single.reverse();
  EXPECT_TRUE(xyz_single[0].equals3D(Coordinate(9, 8, 7)));

  CoordinateSequence xyz_two(std::initializer_list<Coordinate>{
      Coordinate(1, 2, 3), Coordinate(4, 5, 6)});
  xyz_two.reverse();
  EXPECT_TRUE(xyz_two[0].equals3D(Coordinate(4, 5, 6)));
  EXPECT_TRUE(xyz_two[1].equals3D(Coordinate(1, 2, 3)));

  CoordinateSequence xyz_odd(std::initializer_list<Coordinate>{
      Coordinate(3, 1, 30), Coordinate(1, 2, 10), Coordinate(2, 0, 20)});
  xyz_odd.reverse();
  EXPECT_TRUE(xyz_odd[0].equals3D(Coordinate(2, 0, 20)));
  EXPECT_TRUE(xyz_odd[1].equals3D(Coordinate(1, 2, 10)));
  EXPECT_TRUE(xyz_odd[2].equals3D(Coordinate(3, 1, 30)));

  xyz_odd.sort();
  EXPECT_TRUE(xyz_odd[0].equals3D(Coordinate(1, 2, 10)));
  EXPECT_TRUE(xyz_odd[1].equals3D(Coordinate(2, 0, 20)));
  EXPECT_TRUE(xyz_odd[2].equals3D(Coordinate(3, 1, 30)));

  CoordinateSequence xyzm_even(std::initializer_list<CoordinateXYZM>{
      CoordinateXYZM(2, 3, 30, 300), CoordinateXYZM(1, 5, 10, 100),
      CoordinateXYZM(2, 1, 20, 200), CoordinateXYZM(0, 0, 40, 400)});
  xyzm_even.reverse();
  EXPECT_TRUE(xyzm_even.getAt<CoordinateXYZM>(0).equals4D(
      CoordinateXYZM(0, 0, 40, 400)));
  EXPECT_TRUE(xyzm_even.getAt<CoordinateXYZM>(3).equals4D(
      CoordinateXYZM(2, 3, 30, 300)));

  xyzm_even.sort();
  EXPECT_TRUE(xyzm_even.getAt<CoordinateXYZM>(0).equals4D(
      CoordinateXYZM(0, 0, 40, 400)));
  EXPECT_TRUE(xyzm_even.getAt<CoordinateXYZM>(1).equals4D(
      CoordinateXYZM(1, 5, 10, 100)));
  EXPECT_TRUE(xyzm_even.getAt<CoordinateXYZM>(2).equals4D(
      CoordinateXYZM(2, 1, 20, 200)));
  EXPECT_TRUE(xyzm_even.getAt<CoordinateXYZM>(3).equals4D(
      CoordinateXYZM(2, 3, 30, 300)));

  // equals / operator== / operator!= (2D).
  EXPECT_TRUE(CoordinateSequence::equals(nullptr, nullptr));
  EXPECT_FALSE(CoordinateSequence::equals(&xyz_odd, nullptr));
  EXPECT_FALSE(CoordinateSequence::equals(nullptr, &xyz_odd));
  EXPECT_TRUE(CoordinateSequence::equals(&xyz_odd, &xyz_odd));
  EXPECT_FALSE(xyz_odd == xyzm_even);
  EXPECT_TRUE(xyz_odd != xyzm_even);

  CoordinateSequence xyz_same_xy(std::initializer_list<Coordinate>{
      Coordinate(1, 2, 99), Coordinate(2, 0, 88), Coordinate(3, 1, 77)});
  EXPECT_TRUE(xyz_odd == xyz_same_xy);
  EXPECT_FALSE(xyz_odd.equalsIdentical(xyz_same_xy));

  CoordinateSequence xyz_diff_xy(std::initializer_list<Coordinate>{
      Coordinate(1, 2, 10), Coordinate(2, 9, 20), Coordinate(3, 1, 30)});
  EXPECT_FALSE(xyz_odd == xyz_diff_xy);

  // equalsIdentical checks self, size, hasZ, hasM, and NaN == NaN.
  EXPECT_TRUE(xyz_odd.equalsIdentical(xyz_odd));
  EXPECT_FALSE(xyz_odd.equalsIdentical(xyzm_even));

  CoordinateSequence xy_1(
      std::initializer_list<CoordinateXY>{CoordinateXY(1, 2)});
  CoordinateSequence xyz_nan_1(
      std::initializer_list<Coordinate>{Coordinate(1, 2, DoubleNotANumber)});
  CoordinateSequence xyz_fin_1(
      std::initializer_list<Coordinate>{Coordinate(1, 2, 3)});
  // Both xy_1 and xyz_nan_1 have hasZ() == false (auto-detected NaN Z).
  EXPECT_TRUE(xy_1.equalsIdentical(xyz_nan_1));

  CoordinateSequence xyz_declared_1 = CoordinateSequence::XYZ(1);
  xyz_declared_1[0] = Coordinate(1, 2, DoubleNotANumber);
  EXPECT_FALSE(xy_1.equalsIdentical(xyz_declared_1));

  CoordinateSequence xyz_declared_copy = CoordinateSequence::XYZ(1);
  xyz_declared_copy[0] = Coordinate(1, 2, DoubleNotANumber);
  EXPECT_TRUE(xyz_declared_1.equalsIdentical(xyz_declared_copy));
  EXPECT_FALSE(xyz_declared_1.equalsIdentical(xyz_fin_1));
  EXPECT_FALSE(xyz_fin_1.equalsIdentical(xyz_declared_1));

  CoordinateSequence xym_1(
      std::initializer_list<CoordinateXYM>{CoordinateXYM(1, 2, 3)});
  EXPECT_FALSE(xy_1.equalsIdentical(xym_1));

  CoordinateSequence xyzm_1(
      std::initializer_list<CoordinateXYZM>{CoordinateXYZM(1, 2, 3, 4)});
  CoordinateSequence xyzm_diff_m(
      std::initializer_list<CoordinateXYZM>{CoordinateXYZM(1, 2, 3, 9)});
  EXPECT_FALSE(xyz_fin_1.equalsIdentical(xyzm_1));
  EXPECT_FALSE(xyzm_1.equalsIdentical(xyzm_diff_m));
  EXPECT_TRUE(xyzm_1.equalsIdentical(*xyzm_1.clone()));

  CoordinateSequence xyzm_nan_m_1(std::initializer_list<CoordinateXYZM>{
      CoordinateXYZM(1, 2, 3, DoubleNotANumber)});
  CoordinateSequence xyzm_nan_m_2(std::initializer_list<CoordinateXYZM>{
      CoordinateXYZM(1, 2, 3, DoubleNotANumber)});
  EXPECT_TRUE(xyzm_nan_m_1.equalsIdentical(xyzm_nan_m_2));
  EXPECT_FALSE(xyzm_nan_m_1.equalsIdentical(xyzm_1));

  CoordinateSequence xy_sort(std::initializer_list<CoordinateXY>{
      CoordinateXY(2, 0), CoordinateXY(1, 0)});
  xy_sort.sort();
  EXPECT_EQ(xy_sort.getAt<CoordinateXY>(0), CoordinateXY(1, 0));

  CoordinateSequence xym_sort(std::initializer_list<CoordinateXYM>{
      CoordinateXYM(2, 0, 20), CoordinateXYM(1, 0, 10)});
  xym_sort.sort();
  CoordinateXYM xym_sorted_0;
  CoordinateXYM xym_sorted_1;
  xym_sort.getAt(0, xym_sorted_0);
  xym_sort.getAt(1, xym_sorted_1);
  EXPECT_TRUE(xym_sorted_0.equals3D(CoordinateXYM(1, 0, 10)));
  EXPECT_TRUE(xym_sorted_1.equals3D(CoordinateXYM(2, 0, 20)));
  EXPECT_EQ(xym_sort.getOrdinate(0, CoordinateSequence::M), 10.0);
  EXPECT_EQ(xym_sort.getOrdinate(1, CoordinateSequence::M), 20.0);

  CoordinateSequence xyzm_odd(std::initializer_list<CoordinateXYZM>{
      CoordinateXYZM(1, 2, 3, 4), CoordinateXYZM(5, 6, 7, 8),
      CoordinateXYZM(9, 10, 11, 12)});
  xyzm_odd.reverse();
  EXPECT_TRUE(xyzm_odd.getAt<CoordinateXYZM>(0).equals4D(
      CoordinateXYZM(9, 10, 11, 12)));
  EXPECT_TRUE(
      xyzm_odd.getAt<CoordinateXYZM>(1).equals4D(CoordinateXYZM(5, 6, 7, 8)));
  EXPECT_TRUE(
      xyzm_odd.getAt<CoordinateXYZM>(2).equals4D(CoordinateXYZM(1, 2, 3, 4)));

  CoordinateSequence xyzm_empty = CoordinateSequence::XYZM(0);
  xyzm_empty.sort();
  EXPECT_TRUE(xyzm_empty.isEmpty());
}

TEST(CoordinateSequenceTest, EnvelopesAndAtLeastNCoordinatesOrNothing) {
  CoordinateSequence empty = CoordinateSequence::XYZ(0);
  EXPECT_TRUE(empty.getEnvelope().isNull());
  Envelope env;
  empty.expandEnvelope(env);
  EXPECT_TRUE(env.isNull());

  CoordinateSequence pts(std::initializer_list<Coordinate>{
      Coordinate(-3, 5), Coordinate(4, -2), Coordinate(1, 8)});
  Envelope tight = pts.getEnvelope();
  EXPECT_FALSE(tight.isNull());
  EXPECT_EQ(tight.getMinX(), -3.0);
  EXPECT_EQ(tight.getMaxX(), 4.0);
  EXPECT_EQ(tight.getMinY(), -2.0);
  EXPECT_EQ(tight.getMaxY(), 8.0);

  Envelope grown(0, 10, 0, 1);
  pts.expandEnvelope(grown);
  EXPECT_EQ(grown.getMinX(), -3.0);
  EXPECT_EQ(grown.getMaxX(), 10.0);
  EXPECT_EQ(grown.getMinY(), -2.0);
  EXPECT_EQ(grown.getMaxY(), 8.0);

  CoordinateSequence xyzm_pts(std::initializer_list<CoordinateXYZM>{
      CoordinateXYZM(-2, 10, 1, 2), CoordinateXYZM(5, -3, 4, 5)});
  Envelope xyzm_env = xyzm_pts.getEnvelope();
  EXPECT_EQ(xyzm_env.getMinX(), -2.0);
  EXPECT_EQ(xyzm_env.getMaxX(), 5.0);
  EXPECT_EQ(xyzm_env.getMinY(), -3.0);
  EXPECT_EQ(xyzm_env.getMaxY(), 10.0);

  EXPECT_EQ(CoordinateSequence::atLeastNCoordinatesOrNothing(0, &empty),
            &empty);
  EXPECT_EQ(CoordinateSequence::atLeastNCoordinatesOrNothing(3, &pts), &pts);
  EXPECT_EQ(CoordinateSequence::atLeastNCoordinatesOrNothing(2, &pts), &pts);

  CoordinateSequence* nothing =
      CoordinateSequence::atLeastNCoordinatesOrNothing(4, &pts);
  ASSERT_NE(nothing, &pts);
  std::unique_ptr<CoordinateSequence> owned_nothing(nothing);
  EXPECT_TRUE(owned_nothing->isEmpty());
  EXPECT_EQ(owned_nothing->getDimension(), pts.getDimension());
}

TEST(CoordinateSequenceTest, FormattingAndToString) {
  EXPECT_EQ(CoordinateSequence::XYZ(0).toString(), "()");
  EXPECT_EQ(CoordinateSequence(
                std::initializer_list<CoordinateXY>{CoordinateXY(1, 2)})
                .toString(),
            "(1 2)");
  EXPECT_EQ(CoordinateSequence(std::initializer_list<Coordinate>{
                                   Coordinate(1, 2, 3), Coordinate(4, 5, 6)})
                .toString(),
            "(1 2 3, 4 5 6)");

  std::ostringstream expected_xym;
  expected_xym << "(1 2 " << DoubleNotANumber << " 7)";
  EXPECT_EQ(CoordinateSequence(
                std::initializer_list<CoordinateXYM>{CoordinateXYM(1, 2, 7)})
                .toString(),
            expected_xym.str());

  EXPECT_EQ(CoordinateSequence(std::initializer_list<CoordinateXYZM>{
                                   CoordinateXYZM(1, 2, 3, 4)})
                .toString(),
            "(1 2 3 4)");
}

TEST(CoordinateSequenceTest, FiltersForEachApplyAtAndIteratorOperations) {
  struct AddAndStopFilter : public CoordinateFilter {
    explicit AddAndStopFilter(std::size_t stop_after)
        : stop_after_(stop_after) {}

    using CoordinateFilter::filter_ro;
    using CoordinateFilter::filter_rw;

    bool isDone() const override { return visited_ >= stop_after_; }

    void filter_rw(CoordinateXY* c) const override {
      c->x += 10.0;
      ++visited_;
    }

    void filter_ro(const CoordinateXY* c) override {
      sum_x += c->x;
      ++visited_;
    }

    std::size_t stop_after_;
    mutable std::size_t visited_ = 0;
    double sum_x = 0.0;
  };

  CoordinateSequence seq(std::initializer_list<Coordinate>{
      Coordinate(1, 2), Coordinate(3, 4), Coordinate(5, 6)});
  EXPECT_EQ(seq.getDimension(), 2u);

  AddAndStopFilter ro_filter(2);
  seq.apply_ro(&ro_filter);
  EXPECT_EQ(ro_filter.sum_x, 4.0);

  AddAndStopFilter rw_filter(2);
  seq.apply_rw(&rw_filter);
  EXPECT_EQ(seq.getX(0), 11.0);
  EXPECT_EQ(seq.getX(1), 13.0);
  EXPECT_EQ(seq.getX(2), 5.0);

  // apply_rw resets m_hasdim/m_hasz so added Z is detected on next
  // getDimension.
  struct SetZMutator : public CoordinateMutator<SetZMutator> {
    void filter(CoordinateXY*) const {}
    void filter(Coordinate* c) const { c->z = 42.0; }
    void filter(CoordinateXYM*) const {}
    void filter(CoordinateXYZM* c) const { c->z = 42.0; }
  };
  SetZMutator set_z;
  seq.apply_rw(&set_z);
  EXPECT_TRUE(seq.hasZ());
  EXPECT_EQ(seq.getDimension(), 3u);

  struct SumInspector : public CoordinateInspector<SumInspector> {
    void filter(const CoordinateXY* c) { total += c->x + c->y; }
    void filter(const Coordinate* c) { total += c->x + c->y + c->z; }
    void filter(const CoordinateXYM* c) { total += c->x + c->y + c->m; }
    void filter(const CoordinateXYZM* c) { total += c->x + c->y + c->z + c->m; }
    double total = 0.0;
  };
  SumInspector inspector;
  CoordinateSequence xyzm(
      std::initializer_list<CoordinateXYZM>{CoordinateXYZM(1, 2, 3, 4)});
  xyzm.apply_ro(&inspector);
  EXPECT_EQ(inspector.total, 10.0);
  xyzm.apply_rw(&set_z);
  EXPECT_TRUE(xyzm.hasZ());
  EXPECT_TRUE(xyzm.hasM());
  EXPECT_EQ(xyzm.getDimension(), 4u);
  EXPECT_EQ(xyzm.getOrdinate(0, CoordinateSequence::Z), 42.0);
  EXPECT_EQ(xyzm.getOrdinate(0, CoordinateSequence::M), 4.0);

  CoordinateSequence xyzm_two(std::initializer_list<CoordinateXYZM>{
      CoordinateXYZM(1, 2, 3, 4), CoordinateXYZM(5, 6, 7, 8)});
  AddAndStopFilter ro_xyzm(1);
  xyzm_two.apply_ro(&ro_xyzm);
  EXPECT_EQ(ro_xyzm.sum_x, 1.0);
  AddAndStopFilter rw_xyzm(1);
  xyzm_two.apply_rw(&rw_xyzm);
  EXPECT_EQ(xyzm_two.getX(0), 11.0);
  EXPECT_EQ(xyzm_two.getX(1), 5.0);

  // applyAt and forEach variants.
  EXPECT_EQ(xyzm.applyAt(0, [](const auto& c) { return c.x + c.y; }), 3.0);
  EXPECT_EQ(seq.applyAt(0, [](const auto& c) { return c.x; }), 11.0);

  double sum_y = 0.0;
  seq.forEach([&sum_y](const auto& c) { sum_y += c.y; });
  EXPECT_EQ(sum_y, 12.0);

  double xyzm_sum = 0.0;
  xyzm_two.forEach([&xyzm_sum](const auto& c) { xyzm_sum += c.x + c.y; });
  EXPECT_EQ(xyzm_sum, 24.0);

  double all_sum_x = 0.0;
  seq.forEach<CoordinateXY>(
      [&all_sum_x](const CoordinateXY& c) { all_sum_x += c.x; });
  EXPECT_EQ(all_sum_x, 29.0);

  double sub_sum_x = 0.0;
  seq.forEach<CoordinateXY>(
      1, 2, [&sub_sum_x](const CoordinateXY& c) { sub_sum_x += c.x; });
  EXPECT_EQ(sub_sum_x, 18.0);

  // Random-access iterator operations on items<T>().
  auto items = seq.items<Coordinate>();
  auto it = items.begin();
  EXPECT_EQ((*it).x, 11.0);
  EXPECT_EQ(it[2].x, 5.0);
  EXPECT_EQ((items.end() - items.begin()), 3);
  EXPECT_TRUE(items.begin() < items.end());
  EXPECT_TRUE(items.begin() <= items.begin());
  EXPECT_TRUE(items.end() > items.begin());
  EXPECT_TRUE(items.end() >= items.end());
  EXPECT_TRUE(items.begin() != items.end());
  EXPECT_TRUE(items.begin() == items.begin());

  auto it2 = it + 1;
  EXPECT_EQ((*it2).x, 13.0);
  it2 += 1;
  EXPECT_EQ((*it2).x, 5.0);
  it2 -= 1;
  EXPECT_EQ((*it2).x, 13.0);
  EXPECT_EQ((*(it2 - 1)).x, 11.0);
  EXPECT_EQ((*(it++)).x, 11.0);
  EXPECT_EQ((*it).x, 13.0);
  EXPECT_EQ((*(it--)).x, 13.0);
  EXPECT_EQ((*it).x, 11.0);
  ++it;
  EXPECT_EQ((*it).x, 13.0);
  --it;
  EXPECT_EQ((*it).x, 11.0);

  const CoordinateSequence& cseq = seq;
  auto citems = cseq.items<CoordinateXY>();
  EXPECT_EQ(citems.cend() - citems.cbegin(), 3);
  EXPECT_EQ(citems.end() - citems.begin(), 3);
}

}  // namespace
}  // namespace geos_google
