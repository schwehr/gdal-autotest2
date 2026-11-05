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
// Tests for GMLReader::HugeFileResolver and GMLReader::ParseXMLHugeFile
// (hugefileresolver.cpp).
//
// In google3, HAVE_SQLITE is not defined in //third_party/gdal, so
// hugefileresolver.cpp compiles the non-SQLite stub methods returning
// CPLE_NotSupported.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gml/hugefileresolver.cpp

#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gml/gmlreaderp.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

// C++20 explicit template instantiation trick to access private
// ParseXMLHugeFile.
template <typename Tag, typename Tag::type M>
struct MemberRobber {
  friend typename Tag::type GetMember(Tag) { return M; }
};

struct ParseXMLHugeFileTag {
  using type = bool (GMLReader::*)(const char*, const bool, const int);
  friend type GetMember(ParseXMLHugeFileTag);
};

template struct MemberRobber<ParseXMLHugeFileTag, &GMLReader::ParseXMLHugeFile>;

TEST(HugeFileResolverTest, HugeFileResolverFailsWithoutSqlite) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);

  WithQuietHandler quiet_handler;
  CPLErrorReset();
  EXPECT_FALSE(reader->HugeFileResolver("/vsimem/fake.gml", false, 0));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
}

TEST(HugeFileResolverTest, ParseXMLHugeFileFailsWithoutSqlite) {
  auto reader =
      std::make_unique<GMLReader>(true, false, false, GML_SWAP_AUTO, false);
  ASSERT_NE(nullptr, reader);

  WithQuietHandler quiet_handler;
  CPLErrorReset();
  auto parse_fn = GetMember(ParseXMLHugeFileTag{});
  EXPECT_FALSE((reader.get()->*parse_fn)("/vsimem/fake_out.gml", false, 0));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
}

}  // namespace
}  // namespace autotest2
