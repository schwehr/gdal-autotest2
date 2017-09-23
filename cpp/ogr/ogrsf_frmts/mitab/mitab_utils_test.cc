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

#include <initializer_list>
#include <limits>
#include <vector>

#include "gunit.h"
#include "ogr/ogrsf_frmts/mitab/mitab_utils.h"
#include "port/cpl_port.h"

namespace {

// TODO(schwehr): Test TABGenerateArc.
// TODO(schwehr): Test TABCloseRing.
// TODO(schwehr): Test TABAdjustFilenameExtension.
// TODO(schwehr): Test TABGetBasename.
// TODO(schwehr): Test TAB_CSLLoad.
// TODO(schwehr): Test TABUnEscapeString.
// TODO(schwehr): Test TABEscapeString.
// TODO(schwehr): Test TABCleanFieldName.
// TODO(schwehr): Test TABUnitIdToString.
// TODO(schwehr): Test TABUnitIdFromString.

TEST(MitabUtilsTest, TABSaturatedAdd) {
  const auto max_val = std::numeric_limits<GInt32>::max();
  const auto min_val = std::numeric_limits<GInt32>::min();
  std::initializer_list<std::vector<GInt32>> tests = {
    {0, 0, 0},
    {0, -1, -1},
    {0, 1, 1},
    {-1, max_val, max_val - 1},
    {0, max_val, max_val},
    {1, max_val, max_val},
    {-1, min_val, min_val},
    {0, min_val, min_val},
    {1, min_val, min_val + 1},
    {max_val, max_val, max_val},
    {min_val, min_val, min_val},
  };
  for (const auto &v : tests) {
    GInt32 a = v[0];
    GInt32 b = v[1];
    GInt32 expected = v[2];
    TABSaturatedAdd(a, b);
    EXPECT_EQ(expected, a);
    a = v[1];
    b = v[0];
    TABSaturatedAdd(a, b);
    EXPECT_EQ(expected, a);
  }
}

}  // namespace


