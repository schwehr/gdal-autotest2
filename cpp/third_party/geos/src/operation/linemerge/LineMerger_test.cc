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

// Unit tests for third_party/geos/src/operation/linemerge/LineMerger.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/linemerge/LineMerger.cpp

#include "third_party/geos/include/geos/operation/linemerge/LineMerger.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/LineString.h"
#include "third_party/geos/include/geos/geom/LinearRing.h"
#include "third_party/geos/include/geos/geom/PrecisionModel.h"
#include "third_party/geos/include/geos/io/WKTReader.h"
#include "third_party/geos/include/geos/io/WKTWriter.h"
#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::LinearRing;
using geos::geom::LineString;
using geos::geom::PrecisionModel;
using geos::io::WKTReader;
using geos::io::WKTWriter;
using geos::operation::linemerge::LineMerger;
using geos::util::UnsupportedOperationException;
using ::testing::IsEmpty;

struct MergeCase {
  std::string_view name;
  bool directed;
  bool normalize;
  std::span<const std::string_view> inputs;
  std::span<const std::string_view> expected;
};

void ExpectLinesEqual(std::vector<std::unique_ptr<LineString>> actual,
                      std::span<const std::string_view> expected_wkts,
                      const WKTReader& reader, bool normalize) {
  std::vector<std::unique_ptr<LineString>> expected;
  expected.reserve(expected_wkts.size());
  for (std::string_view wkt : expected_wkts) {
    expected.push_back(reader.read<LineString>(std::string(wkt)));
  }

  ASSERT_EQ(actual.size(), expected.size());

  if (normalize) {
    for (auto& line : actual) {
      line->normalize();
    }
    for (auto& line : expected) {
      line->normalize();
    }
  }

  auto by_compare_to = [](const std::unique_ptr<LineString>& a,
                          const std::unique_ptr<LineString>& b) {
    return a->compareTo(b.get()) < 0;
  };
  std::sort(actual.begin(), actual.end(), by_compare_to);
  std::sort(expected.begin(), expected.end(), by_compare_to);

  WKTWriter writer;
  for (std::size_t i = 0; i < actual.size(); ++i) {
    SCOPED_TRACE(writer.write(actual[i].get()) + " vs " +
                 writer.write(expected[i].get()));
    EXPECT_TRUE(actual[i]->equalsExact(expected[i].get(), 1e-9));
  }
}

void RunMergeCases(std::span<const MergeCase> cases) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  for (const auto& test_case : cases) {
    SCOPED_TRACE(std::string(test_case.name));
    std::vector<std::unique_ptr<Geometry>> owned_inputs;
    owned_inputs.reserve(test_case.inputs.size());
    for (std::string_view wkt : test_case.inputs) {
      owned_inputs.push_back(reader.read(std::string(wkt)));
    }

    LineMerger merger(test_case.directed);
    for (const auto& geom : owned_inputs) {
      merger.add(geom.get());
    }
    ExpectLinesEqual(merger.getMergedLineStrings(), test_case.expected, reader,
                     test_case.normalize);
  }
}

// Undirected merge test cases (order/orientation normalized).
constexpr std::array<std::string_view, 2> kReversedSecondInputs = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (2 0, 1 0)",
};
constexpr std::array<std::string_view, 1> kReversedSecondExpected = {
    "LINESTRING (0 0, 1 0, 2 0)",
};

constexpr std::array<std::string_view, 4> kUnorderedChainInputs = {
    "LINESTRING (3 0, 4 0)",
    "LINESTRING (1 0, 2 0)",
    "LINESTRING (2 0, 3 0)",
    "LINESTRING (0 0, 1 0)",
};
constexpr std::array<std::string_view, 1> kUnorderedChainExpected = {
    "LINESTRING (0 0, 1 0, 2 0, 3 0, 4 0)",
};

constexpr std::array<std::string_view, 4> kDisconnectedChainsInputs = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (1 0, 2 0)",
    "LINESTRING (10 0, 11 0)",
    "LINESTRING (11 0, 12 0)",
};
constexpr std::array<std::string_view, 2> kDisconnectedChainsExpected = {
    "LINESTRING (0 0, 1 0, 2 0)",
    "LINESTRING (10 0, 11 0, 12 0)",
};

constexpr std::array<std::string_view, 2> kRepeatedVerticesInputs = {
    "LINESTRING (0 0, 0 0, 1 0)",
    "LINESTRING (1 0, 2 0, 2 0)",
};
constexpr std::array<std::string_view, 1> kRepeatedVerticesExpected = {
    "LINESTRING (0 0, 1 0, 2 0)",
};

constexpr std::array<std::string_view, 1> kFoldBackInputs = {
    "LINESTRING (0 0, 1 0, 0 0)",
};
constexpr std::array<std::string_view, 1> kFoldBackExpected = {
    "LINESTRING (0 0, 1 0, 0 0)",
};

constexpr std::array<std::string_view, 3> kTJunctionInputs = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (1 0, 2 0)",
    "LINESTRING (1 0, 1 1)",
};
constexpr std::array<std::string_view, 3> kTJunctionExpected = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (1 0, 2 0)",
    "LINESTRING (1 0, 1 1)",
};

constexpr std::array<std::string_view, 4> kYJunctionStemInputs = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (1 0, 2 0)",
    "LINESTRING (2 0, 3 1)",
    "LINESTRING (2 0, 3 -1)",
};
constexpr std::array<std::string_view, 3> kYJunctionStemExpected = {
    "LINESTRING (0 0, 1 0, 2 0)",
    "LINESTRING (2 0, 3 1)",
    "LINESTRING (2 0, 3 -1)",
};

constexpr std::array<std::string_view, 4> kCrossroadsInputs = {
    "LINESTRING (0 1, 1 1)",
    "LINESTRING (1 1, 2 1)",
    "LINESTRING (1 0, 1 1)",
    "LINESTRING (1 1, 1 2)",
};
constexpr std::array<std::string_view, 4> kCrossroadsExpected = {
    "LINESTRING (0 1, 1 1)",
    "LINESTRING (1 1, 2 1)",
    "LINESTRING (1 0, 1 1)",
    "LINESTRING (1 1, 1 2)",
};

constexpr std::array<std::string_view, 2> kUnnodedCrossingInputs = {
    "LINESTRING (0 0, 2 2)",
    "LINESTRING (0 2, 2 0)",
};
constexpr std::array<std::string_view, 2> kUnnodedCrossingExpected = {
    "LINESTRING (0 0, 2 2)",
    "LINESTRING (0 2, 2 0)",
};

constexpr std::array<std::string_view, 1> kSingleLoopInputs = {
    "LINESTRING (0 0, 1 0, 1 1, 0 1, 0 0)",
};
constexpr std::array<std::string_view, 1> kSingleLoopExpected = {
    "LINESTRING (0 0, 1 0, 1 1, 0 1, 0 0)",
};

constexpr std::array<std::string_view, 4> kFourEdgeLoopInputs = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (1 1, 1 0)",
    "LINESTRING (0 1, 1 1)",
    "LINESTRING (0 1, 0 0)",
};
constexpr std::array<std::string_view, 1> kFourEdgeLoopExpected = {
    "LINESTRING (0 0, 1 0, 1 1, 0 1, 0 0)",
};

constexpr std::array<std::string_view, 2> kTwoEdgeLoopInputs = {
    "LINESTRING (0 0, 1 1, 2 0)",
    "LINESTRING (0 0, 1 -1, 2 0)",
};
constexpr std::array<std::string_view, 1> kTwoEdgeLoopExpected = {
    "LINESTRING (0 0, 1 1, 2 0, 1 -1, 0 0)",
};

constexpr std::array<std::string_view, 2> kLollipopInputs = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (1 0, 2 0, 2 1, 1 1, 1 0)",
};
constexpr std::array<std::string_view, 2> kLollipopExpected = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (1 0, 2 0, 2 1, 1 1, 1 0)",
};

constexpr auto kUndirectedShapeCases = std::to_array<MergeCase>({
    {
        .name = "reversed_second_segment",
        .directed = false,
        .normalize = true,
        .inputs = kReversedSecondInputs,
        .expected = kReversedSecondExpected,
    },
    {
        .name = "unordered_four_segment_chain",
        .directed = false,
        .normalize = true,
        .inputs = kUnorderedChainInputs,
        .expected = kUnorderedChainExpected,
    },
    {
        .name = "disconnected_chains",
        .directed = false,
        .normalize = true,
        .inputs = kDisconnectedChainsInputs,
        .expected = kDisconnectedChainsExpected,
    },
    {
        .name = "strips_repeated_consecutive_vertices",
        .directed = false,
        .normalize = true,
        .inputs = kRepeatedVerticesInputs,
        .expected = kRepeatedVerticesExpected,
    },
    {
        .name = "backtracking_fold_line",
        .directed = false,
        .normalize = true,
        .inputs = kFoldBackInputs,
        .expected = kFoldBackExpected,
    },
    {
        .name = "stops_at_degree_3_t_junction",
        .directed = false,
        .normalize = true,
        .inputs = kTJunctionInputs,
        .expected = kTJunctionExpected,
    },
    {
        .name = "merges_stem_up_to_degree_3_y_junction",
        .directed = false,
        .normalize = true,
        .inputs = kYJunctionStemInputs,
        .expected = kYJunctionStemExpected,
    },
    {
        .name = "stops_at_degree_4_crossroads",
        .directed = false,
        .normalize = true,
        .inputs = kCrossroadsInputs,
        .expected = kCrossroadsExpected,
    },
    {
        .name = "unnoded_crossing_lines_do_not_merge",
        .directed = false,
        .normalize = true,
        .inputs = kUnnodedCrossingInputs,
        .expected = kUnnodedCrossingExpected,
    },
    {
        .name = "single_edge_closed_loop",
        .directed = false,
        .normalize = true,
        .inputs = kSingleLoopInputs,
        .expected = kSingleLoopExpected,
    },
    {
        .name = "four_edge_mixed_direction_loop",
        .directed = false,
        .normalize = true,
        .inputs = kFourEdgeLoopInputs,
        .expected = kFourEdgeLoopExpected,
    },
    {
        .name = "two_edge_parallel_loop",
        .directed = false,
        .normalize = true,
        .inputs = kTwoEdgeLoopInputs,
        .expected = kTwoEdgeLoopExpected,
    },
    {
        .name = "lollipop_tail_and_loop_at_degree_3",
        .directed = false,
        .normalize = true,
        .inputs = kLollipopInputs,
        .expected = kLollipopExpected,
    },
});

// Exact-orientation cases (majority direction & directed mode,
// normalize=false).
constexpr std::array<std::string_view, 3> kMajorityForwardInputs = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (1 0, 2 0)",
    "LINESTRING (3 0, 2 0)",
};
constexpr std::array<std::string_view, 1> kMajorityForwardExpected = {
    "LINESTRING (0 0, 1 0, 2 0, 3 0)",
};

constexpr std::array<std::string_view, 3> kMajorityReverseInputs = {
    "LINESTRING (1 0, 0 0)",
    "LINESTRING (2 0, 1 0)",
    "LINESTRING (2 0, 3 0)",
};
constexpr std::array<std::string_view, 1> kMajorityReverseExpected = {
    "LINESTRING (3 0, 2 0, 1 0, 0 0)",
};

constexpr std::array<std::string_view, 2> kTieDirectionInputs = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (2 0, 1 0)",
};
constexpr std::array<std::string_view, 1> kTieDirectionExpected = {
    "LINESTRING (0 0, 1 0, 2 0)",
};

constexpr std::array<std::string_view, 3> kDirectedAlignedInputs = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (1 0, 2 0)",
    "LINESTRING (2 0, 3 0)",
};
constexpr std::array<std::string_view, 1> kDirectedAlignedExpected = {
    "LINESTRING (0 0, 1 0, 2 0, 3 0)",
};

constexpr std::array<std::string_view, 3> kDirectedAlignedReversedInputs = {
    "LINESTRING (3 0, 2 0)",
    "LINESTRING (2 0, 1 0)",
    "LINESTRING (1 0, 0 0)",
};
constexpr std::array<std::string_view, 1> kDirectedAlignedReversedExpected = {
    "LINESTRING (3 0, 2 0, 1 0, 0 0)",
};

constexpr std::array<std::string_view, 2> kDirectedOpposingIncomingInputs = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (2 0, 1 0)",
};
constexpr std::array<std::string_view, 2> kDirectedOpposingIncomingExpected = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (2 0, 1 0)",
};

constexpr std::array<std::string_view, 2> kDirectedOpposingOutgoingInputs = {
    "LINESTRING (1 0, 0 0)",
    "LINESTRING (1 0, 2 0)",
};
constexpr std::array<std::string_view, 2> kDirectedOpposingOutgoingExpected = {
    "LINESTRING (1 0, 0 0)",
    "LINESTRING (1 0, 2 0)",
};

constexpr std::array<std::string_view, 4> kDirectedAlignedLoopInputs = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (1 0, 1 1)",
    "LINESTRING (1 1, 0 1)",
    "LINESTRING (0 1, 0 0)",
};
constexpr std::array<std::string_view, 1> kDirectedAlignedLoopExpected = {
    "LINESTRING (0 0, 1 0, 1 1, 0 1, 0 0)",
};

constexpr std::array<std::string_view, 1> kDirectedSingleEdgeLoopInputs = {
    "LINESTRING (0 0, 1 0, 1 1, 0 0)",
};
constexpr std::array<std::string_view, 1> kDirectedSingleEdgeLoopExpected = {
    "LINESTRING (0 0, 1 0, 1 1, 0 0)",
};

constexpr std::array<std::string_view, 3> kDirectedOpposedTriangleInputs = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (1 0, 1 1)",
    "LINESTRING (0 0, 1 1)",
};
constexpr std::array<std::string_view, 2> kDirectedOpposedTriangleExpected = {
    "LINESTRING (0 0, 1 0, 1 1)",
    "LINESTRING (0 0, 1 1)",
};

constexpr std::array<std::string_view, 3> kDirectedTJunctionInputs = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (1 0, 2 0)",
    "LINESTRING (1 0, 1 1)",
};
constexpr std::array<std::string_view, 3> kDirectedTJunctionExpected = {
    "LINESTRING (0 0, 1 0)",
    "LINESTRING (1 0, 1 1)",
    "LINESTRING (1 0, 2 0)",
};

constexpr auto kExactOrientationCases = std::to_array<MergeCase>({
    {
        .name = "undirected_majority_forward",
        .directed = false,
        .normalize = false,
        .inputs = kMajorityForwardInputs,
        .expected = kMajorityForwardExpected,
    },
    {
        .name = "undirected_majority_reverse",
        .directed = false,
        .normalize = false,
        .inputs = kMajorityReverseInputs,
        .expected = kMajorityReverseExpected,
    },
    {
        .name = "undirected_tie_direction_keeps_traversal_order",
        .directed = false,
        .normalize = false,
        .inputs = kTieDirectionInputs,
        .expected = kTieDirectionExpected,
    },
    {
        .name = "directed_aligned_forward_chain",
        .directed = true,
        .normalize = false,
        .inputs = kDirectedAlignedInputs,
        .expected = kDirectedAlignedExpected,
    },
    {
        .name = "directed_aligned_reverse_chain",
        .directed = true,
        .normalize = false,
        .inputs = kDirectedAlignedReversedInputs,
        .expected = kDirectedAlignedReversedExpected,
    },
    {
        .name = "directed_opposing_incoming_edges_do_not_merge",
        .directed = true,
        .normalize = false,
        .inputs = kDirectedOpposingIncomingInputs,
        .expected = kDirectedOpposingIncomingExpected,
    },
    {
        .name = "directed_opposing_outgoing_edges_do_not_merge",
        .directed = true,
        .normalize = false,
        .inputs = kDirectedOpposingOutgoingInputs,
        .expected = kDirectedOpposingOutgoingExpected,
    },
    {
        .name = "directed_aligned_four_edge_loop",
        .directed = true,
        .normalize = false,
        .inputs = kDirectedAlignedLoopInputs,
        .expected = kDirectedAlignedLoopExpected,
    },
    {
        .name = "directed_single_edge_closed_loop",
        .directed = true,
        .normalize = false,
        .inputs = kDirectedSingleEdgeLoopInputs,
        .expected = kDirectedSingleEdgeLoopExpected,
    },
    {
        .name = "directed_opposed_triangle_splits_at_start_and_end_nodes",
        .directed = true,
        .normalize = false,
        .inputs = kDirectedOpposedTriangleInputs,
        .expected = kDirectedOpposedTriangleExpected,
    },
    {
        .name = "directed_t_junction_stops_merging",
        .directed = true,
        .normalize = false,
        .inputs = kDirectedTJunctionInputs,
        .expected = kDirectedTJunctionExpected,
    },
});

TEST(LineMergerTest, EmptyAndNonLinealInputsReturnEmpty) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  {
    LineMerger empty_merger;
    EXPECT_THAT(empty_merger.getMergedLineStrings(), IsEmpty());
  }

  {
    LineMerger vec_merger(true);
    std::vector<const Geometry*> empty_vec;
    vec_merger.add(&empty_vec);
    EXPECT_THAT(vec_merger.getMergedLineStrings(), IsEmpty());
  }

  {
    constexpr std::array<std::string_view, 7> kIgnorableWkts = {
        "LINESTRING EMPTY",          "MULTILINESTRING EMPTY",
        "GEOMETRYCOLLECTION EMPTY",  "POLYGON EMPTY",
        "LINESTRING (5 5, 5 5)",     "POINT (1 2)",
        "MULTIPOINT ((1 2), (3 4))",
    };
    std::vector<std::unique_ptr<Geometry>> owned;
    std::vector<const Geometry*> ptrs;
    for (std::string_view wkt : kIgnorableWkts) {
      owned.push_back(reader.read(std::string(wkt)));
      ptrs.push_back(owned.back().get());
    }

    LineMerger merger;
    merger.add(&ptrs);
    EXPECT_THAT(merger.getMergedLineStrings(), IsEmpty());
  }
}

TEST(LineMergerTest, DirectLineStringOverloadPreservesSingleLine) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());
  std::unique_ptr<LineString> line =
      reader.read<LineString>("LINESTRING (0 0, 1 1, 2 0)");

  LineMerger merger;
  merger.add(line.get());

  constexpr std::array<std::string_view, 1> kExpected = {
      "LINESTRING (0 0, 1 1, 2 0)",
  };
  ExpectLinesEqual(merger.getMergedLineStrings(), kExpected, reader,
                   /*normalize=*/false);
}

TEST(LineMergerTest, UndirectedMergeShapes) {
  RunMergeCases(kUndirectedShapeCases);
}

TEST(LineMergerTest, ExactOrientationAndDirectedMerges) {
  RunMergeCases(kExactOrientationCases);
}

TEST(LineMergerTest, ExtractsLineworkFromPolygonsCollectionsAndVectorAdd) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<Geometry> multi_line =
      reader.read("MULTILINESTRING ((0 0, 1 0), (1 0, 2 0))");
  std::unique_ptr<Geometry> poly_with_hole = reader.read(
      "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))");
  std::unique_ptr<Geometry> point = reader.read("POINT (99 99)");
  std::unique_ptr<Geometry> collection =
      reader.read("GEOMETRYCOLLECTION (LINESTRING (2 0, 3 0), POINT (5 5))");
  std::unique_ptr<LinearRing> ring = factory->createLinearRing(
      CoordinateSequence({Coordinate(20.0, 20.0), Coordinate(22.0, 20.0),
                          Coordinate(22.0, 22.0), Coordinate(20.0, 22.0),
                          Coordinate(20.0, 20.0)}));

  std::vector<const Geometry*> inputs = {
      multi_line.get(),
      poly_with_hole.get(),
      point.get(),
      collection.get(),
  };

  LineMerger merger;
  merger.add(&inputs);
  merger.add(static_cast<const Geometry*>(ring.get()));

  constexpr std::array<std::string_view, 4> kExpected = {
      "LINESTRING (0 0, 1 0, 2 0, 3 0)",
      "LINESTRING (0 0, 10 0, 10 10, 0 10, 0 0)",
      "LINESTRING (2 2, 2 4, 4 4, 4 2, 2 2)",
      "LINESTRING (20 20, 22 20, 22 22, 20 22, 20 20)",
  };
  ExpectLinesEqual(merger.getMergedLineStrings(), kExpected, reader,
                   /*normalize=*/true);
}

TEST(LineMergerTest, ThrowsUnsupportedOperationExceptionOnCurvedGeometries) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  constexpr std::array<std::string_view, 3> kCurvedWkts = {
      "CIRCULARSTRING (0 0, 1 1, 2 0)",
      "COMPOUNDCURVE (CIRCULARSTRING (0 0, 1 1, 2 0), (2 0, 3 0))",
      "GEOMETRYCOLLECTION (LINESTRING (0 0, 1 0), CIRCULARSTRING (1 0, 2 1, 3 "
      "0))",
  };

  for (std::string_view wkt : kCurvedWkts) {
    SCOPED_TRACE(std::string(wkt));
    std::unique_ptr<Geometry> curved = reader.read(std::string(wkt));
    LineMerger merger;
    EXPECT_THROW(merger.add(curved.get()), UnsupportedOperationException);
    EXPECT_THAT(merger.getMergedLineStrings(), IsEmpty());
  }
}

TEST(LineMergerTest, IncrementalMergingAndRepeatedGetMergedLineStrings) {
  GeometryFactory::Ptr factory = GeometryFactory::create();
  WKTReader reader(factory.get());

  std::unique_ptr<Geometry> s1 = reader.read("LINESTRING (0 0, 1 0)");
  std::unique_ptr<Geometry> s2 = reader.read("LINESTRING (1 0, 2 0)");
  std::unique_ptr<Geometry> s3 = reader.read("LINESTRING (2 0, 3 0)");
  std::unique_ptr<Geometry> s4 = reader.read("LINESTRING (10 0, 11 0)");

  for (bool directed : {false, true}) {
    SCOPED_TRACE(directed ? "directed" : "undirected");
    LineMerger merger(directed);
    merger.add(s1.get());
    merger.add(s2.get());

    constexpr std::array<std::string_view, 1> kFirstExpected = {
        "LINESTRING (0 0, 1 0, 2 0)",
    };
    ExpectLinesEqual(merger.getMergedLineStrings(), kFirstExpected, reader,
                     /*normalize=*/false);

    // Calling getMergedLineStrings() again resets marks and clears old
    // EdgeStrings.
    ExpectLinesEqual(merger.getMergedLineStrings(), kFirstExpected, reader,
                     /*normalize=*/false);

    // Adding more geometries incrementally re-merges all lines in the graph.
    merger.add(s3.get());
    merger.add(s4.get());

    constexpr std::array<std::string_view, 2> kSecondExpected = {
        "LINESTRING (0 0, 1 0, 2 0, 3 0)",
        "LINESTRING (10 0, 11 0)",
    };
    ExpectLinesEqual(merger.getMergedLineStrings(), kSecondExpected, reader,
                     /*normalize=*/false);
  }
}

TEST(LineMergerTest, PreservesFactorySridAndZCoordinates) {
  PrecisionModel pm(100.0);
  GeometryFactory::Ptr factory = GeometryFactory::create(&pm, 4326);

  std::unique_ptr<LineString> l1 = factory->createLineString(CoordinateSequence(
      {Coordinate(0.0, 0.0, 10.0), Coordinate(1.0, 0.0, 20.0)}));
  std::unique_ptr<LineString> l2 = factory->createLineString(CoordinateSequence(
      {Coordinate(1.0, 0.0, 20.0), Coordinate(2.0, 0.0, 30.0)}));

  LineMerger merger;
  merger.add(l1.get());
  merger.add(l2.get());

  std::vector<std::unique_ptr<LineString>> merged =
      merger.getMergedLineStrings();
  ASSERT_EQ(merged.size(), 1u);
  EXPECT_EQ(merged[0]->getFactory(), factory.get());
  EXPECT_EQ(merged[0]->getSRID(), 4326);
  EXPECT_TRUE(merged[0]->hasZ());
  ASSERT_EQ(merged[0]->getNumPoints(), 3u);

  const CoordinateSequence* coords = merged[0]->getCoordinatesRO();
  EXPECT_TRUE(coords->getAt(0).equals3D(Coordinate(0.0, 0.0, 10.0)));
  EXPECT_TRUE(coords->getAt(1).equals3D(Coordinate(1.0, 0.0, 20.0)));
  EXPECT_TRUE(coords->getAt(2).equals3D(Coordinate(2.0, 0.0, 30.0)));
}

}  // namespace
}  // namespace geos_google
