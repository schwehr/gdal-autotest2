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

// Unit tests for third_party/geos/src/io/Writer.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/io/Writer.cpp

#include "third_party/geos/include/geos/io/Writer.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using geos::io::Writer;

static_assert(std::is_default_constructible_v<Writer>);
static_assert(std::is_nothrow_destructible_v<Writer>);
static_assert(std::is_same_v<decltype(std::declval<Writer&>().toString()),
                             const std::string&>);
static_assert(std::is_same_v<decltype(std::declval<Writer&>().write(
                                 std::declval<const std::string&>())),
                             void>);
static_assert(std::is_same_v<decltype(std::declval<Writer&>().reserve(
                                 std::declval<std::size_t>())),
                             void>);

struct ChunkCase {
  std::string_view name;
  std::string_view chunk;
  std::string_view expected_accumulated;
};

constexpr auto kSequentialChunks = std::to_array<ChunkCase>({
    {
        .name = "geometry_keyword",
        .chunk = "POLYGON ",
        .expected_accumulated = "POLYGON ",
    },
    {
        .name = "open_rings",
        .chunk = "((",
        .expected_accumulated = "POLYGON ((",
    },
    {
        .name = "ring_coordinates",
        .chunk = "0 0, 1 0, 1 1, 0 1, 0 0",
        .expected_accumulated = "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0",
    },
    {
        .name = "close_rings",
        .chunk = "))",
        .expected_accumulated = "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))",
    },
});

TEST(WriterTest, DefaultConstructsWithEmptyString) {
  Writer writer;
  EXPECT_TRUE(writer.toString().empty());
  EXPECT_EQ(writer.toString().size(), 0u);
  EXPECT_EQ(writer.toString(), "");

  auto heap_writer = std::make_unique<Writer>();
  EXPECT_TRUE(heap_writer->toString().empty());
  EXPECT_EQ(heap_writer->toString(), "");
}

TEST(WriterTest, WriteEmptyStringLeavesContentUnchanged) {
  Writer writer;
  writer.write("");
  EXPECT_TRUE(writer.toString().empty());

  writer.write("POINT (1 2)");
  writer.write("");
  EXPECT_EQ(writer.toString(), "POINT (1 2)");
}

TEST(WriterTest, SequentialWritesConcatenateInOrder) {
  Writer writer;
  for (const ChunkCase& tc : kSequentialChunks) {
    SCOPED_TRACE(tc.name);
    writer.write(std::string(tc.chunk));
    EXPECT_EQ(writer.toString(), tc.expected_accumulated);
  }
}

TEST(WriterTest, ToStringReturnsReferenceToInternalBuffer) {
  Writer writer;
  const std::string& first_ref = writer.toString();
  const std::string& second_ref = writer.toString();
  EXPECT_EQ(&first_ref, &second_ref);

  writer.write("LINESTRING ");
  EXPECT_EQ(first_ref, "LINESTRING ");
  EXPECT_EQ(&first_ref, &writer.toString());

  writer.write("(0 0, 1 1)");
  EXPECT_EQ(first_ref, "LINESTRING (0 0, 1 1)");
}

TEST(WriterTest, ReserveAdjustsCapacityWithoutMutatingContent) {
  Writer writer;
  writer.reserve(0);
  EXPECT_TRUE(writer.toString().empty());

  writer.reserve(512);
  EXPECT_GE(writer.toString().capacity(), 512u);
  EXPECT_TRUE(writer.toString().empty());

  const void* reserved_data = writer.toString().data();
  writer.write("LINESTRING (0 0, 10 20)");
  EXPECT_EQ(writer.toString(), "LINESTRING (0 0, 10 20)");
  EXPECT_GE(writer.toString().capacity(), 512u);
  EXPECT_EQ(static_cast<const void*>(writer.toString().data()), reserved_data);

  // Requesting a smaller capacity preserves existing content and invariants.
  writer.reserve(4);
  EXPECT_EQ(writer.toString(), "LINESTRING (0 0, 10 20)");
  EXPECT_GE(writer.toString().capacity(), writer.toString().size());

  // Growing capacity on a non-empty Writer preserves existing content.
  writer.reserve(1024);
  EXPECT_GE(writer.toString().capacity(), 1024u);
  EXPECT_EQ(writer.toString(), "LINESTRING (0 0, 10 20)");
}

TEST(WriterTest, WriteBeyondReservedCapacityGrowsCleanly) {
  Writer writer;
  writer.reserve(16);
  EXPECT_GE(writer.toString().capacity(), 16u);

  const std::string chunk(64, 'x');
  std::string expected;
  for (int i = 0; i < 40; ++i) {
    writer.write(chunk);
    expected.append(chunk);
  }

  EXPECT_EQ(writer.toString().size(), 64u * 40u);
  EXPECT_EQ(writer.toString(), expected);
}

TEST(WriterTest, PreservesEmbeddedNullBytesWhitespaceAndUtf8) {
  Writer writer;
  const std::string with_nulls("a\0b\0c", 5);
  const std::string trailing = "\n\t\r UTF8-\xC3\xA9";

  writer.write(with_nulls);
  writer.write(trailing);

  const std::string expected = with_nulls + trailing;
  EXPECT_EQ(writer.toString().size(), expected.size());
  EXPECT_EQ(writer.toString(), expected);
}

TEST(WriterTest, SelfAppendFromToStringDuplicatesContent) {
  Writer small_writer;
  small_writer.write("GEOS");
  small_writer.write(small_writer.toString());
  EXPECT_EQ(small_writer.toString(), "GEOSGEOS");

  Writer reallocating_writer;
  const std::string initial(128, 'A');
  reallocating_writer.write(initial);
  reallocating_writer.write(reallocating_writer.toString());
  EXPECT_EQ(reallocating_writer.toString(), std::string(256, 'A'));
}

}  // namespace
}  // namespace geos_google
