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

// Unit tests for third_party/geos/include/geos/io/OrdinateSet.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/io/OrdinateSet.h

#include "third_party/geos/include/geos/io/OrdinateSet.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::io::OrdinateSet;
using geos::util::GEOSException;
using ::testing::HasSubstr;

struct FactorySpec {
  std::string_view name;
  OrdinateSet (*create)();
  bool expected_z;
  bool expected_m;
  int expected_size;
};

constexpr auto kFactorySpecs = std::to_array<FactorySpec>({
    {
        .name = "XY",
        .create = &OrdinateSet::createXY,
        .expected_z = false,
        .expected_m = false,
        .expected_size = 2,
    },
    {
        .name = "XYZ",
        .create = &OrdinateSet::createXYZ,
        .expected_z = true,
        .expected_m = false,
        .expected_size = 3,
    },
    {
        .name = "XYM",
        .create = &OrdinateSet::createXYM,
        .expected_z = false,
        .expected_m = true,
        .expected_size = 3,
    },
    {
        .name = "XYZM",
        .create = &OrdinateSet::createXYZM,
        .expected_z = true,
        .expected_m = true,
        .expected_size = 4,
    },
});

TEST(OrdinateSetTest, FactoryDefaultsAndAccessors) {
  for (const auto& spec : kFactorySpecs) {
    SCOPED_TRACE(spec.name);
    const OrdinateSet set = spec.create();
    EXPECT_EQ(set.hasZ(), spec.expected_z);
    EXPECT_EQ(set.hasM(), spec.expected_m);
    EXPECT_EQ(set.size(), spec.expected_size);
    EXPECT_TRUE(set.changesAllowed());
  }
}

TEST(OrdinateSetTest, EqualityAndInequalityAcrossAllVariants) {
  for (std::size_t i = 0; i < kFactorySpecs.size(); ++i) {
    for (std::size_t j = 0; j < kFactorySpecs.size(); ++j) {
      SCOPED_TRACE(std::string(kFactorySpecs[i].name) + " vs " +
                   std::string(kFactorySpecs[j].name));
      const OrdinateSet a = kFactorySpecs[i].create();
      const OrdinateSet b = kFactorySpecs[j].create();
      if (i == j) {
        EXPECT_TRUE(a == b);
        EXPECT_FALSE(a != b);
        EXPECT_EQ(a, b);
      } else {
        EXPECT_FALSE(a == b);
        EXPECT_TRUE(a != b);
        EXPECT_NE(a, b);
      }
    }
  }
}

TEST(OrdinateSetTest, EqualityIgnoresChangesAllowedFlag) {
  for (const auto& spec : kFactorySpecs) {
    SCOPED_TRACE(spec.name);
    const OrdinateSet mutable_set = spec.create();
    OrdinateSet locked_set = spec.create();
    locked_set.setChangesAllowed(false);

    EXPECT_TRUE(mutable_set.changesAllowed());
    EXPECT_FALSE(locked_set.changesAllowed());
    EXPECT_TRUE(mutable_set == locked_set);
    EXPECT_FALSE(mutable_set != locked_set);
    EXPECT_TRUE(locked_set == mutable_set);
    EXPECT_FALSE(locked_set != mutable_set);
  }
}

TEST(OrdinateSetTest, SetZUpdatesAndNoOpsWhenChangesAllowed) {
  struct TransitionCase {
    std::string_view name;
    OrdinateSet (*initial)();
    bool set_z;
    OrdinateSet (*expected)();
  };

  constexpr auto kCases = std::to_array<TransitionCase>({
      {
          .name = "XY_enable_Z",
          .initial = &OrdinateSet::createXY,
          .set_z = true,
          .expected = &OrdinateSet::createXYZ,
      },
      {
          .name = "XYM_enable_Z",
          .initial = &OrdinateSet::createXYM,
          .set_z = true,
          .expected = &OrdinateSet::createXYZM,
      },
      {
          .name = "XYZ_disable_Z",
          .initial = &OrdinateSet::createXYZ,
          .set_z = false,
          .expected = &OrdinateSet::createXY,
      },
      {
          .name = "XYZM_disable_Z",
          .initial = &OrdinateSet::createXYZM,
          .set_z = false,
          .expected = &OrdinateSet::createXYM,
      },
      {
          .name = "XY_noop_false",
          .initial = &OrdinateSet::createXY,
          .set_z = false,
          .expected = &OrdinateSet::createXY,
      },
      {
          .name = "XYZ_noop_true",
          .initial = &OrdinateSet::createXYZ,
          .set_z = true,
          .expected = &OrdinateSet::createXYZ,
      },
      {
          .name = "XYM_noop_false",
          .initial = &OrdinateSet::createXYM,
          .set_z = false,
          .expected = &OrdinateSet::createXYM,
      },
      {
          .name = "XYZM_noop_true",
          .initial = &OrdinateSet::createXYZM,
          .set_z = true,
          .expected = &OrdinateSet::createXYZM,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    OrdinateSet set = tc.initial();
    const OrdinateSet expected = tc.expected();
    set.setZ(tc.set_z);
    EXPECT_EQ(set, expected);
    EXPECT_EQ(set.hasZ(), tc.set_z);
    EXPECT_EQ(set.size(), expected.size());
  }
}

TEST(OrdinateSetTest, SetMUpdatesAndNoOpsWhenChangesAllowed) {
  struct TransitionCase {
    std::string_view name;
    OrdinateSet (*initial)();
    bool set_m;
    OrdinateSet (*expected)();
  };

  constexpr auto kCases = std::to_array<TransitionCase>({
      {
          .name = "XY_enable_M",
          .initial = &OrdinateSet::createXY,
          .set_m = true,
          .expected = &OrdinateSet::createXYM,
      },
      {
          .name = "XYZ_enable_M",
          .initial = &OrdinateSet::createXYZ,
          .set_m = true,
          .expected = &OrdinateSet::createXYZM,
      },
      {
          .name = "XYM_disable_M",
          .initial = &OrdinateSet::createXYM,
          .set_m = false,
          .expected = &OrdinateSet::createXY,
      },
      {
          .name = "XYZM_disable_M",
          .initial = &OrdinateSet::createXYZM,
          .set_m = false,
          .expected = &OrdinateSet::createXYZ,
      },
      {
          .name = "XY_noop_false",
          .initial = &OrdinateSet::createXY,
          .set_m = false,
          .expected = &OrdinateSet::createXY,
      },
      {
          .name = "XYZ_noop_false",
          .initial = &OrdinateSet::createXYZ,
          .set_m = false,
          .expected = &OrdinateSet::createXYZ,
      },
      {
          .name = "XYM_noop_true",
          .initial = &OrdinateSet::createXYM,
          .set_m = true,
          .expected = &OrdinateSet::createXYM,
      },
      {
          .name = "XYZM_noop_true",
          .initial = &OrdinateSet::createXYZM,
          .set_m = true,
          .expected = &OrdinateSet::createXYZM,
      },
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    OrdinateSet set = tc.initial();
    const OrdinateSet expected = tc.expected();
    set.setM(tc.set_m);
    EXPECT_EQ(set, expected);
    EXPECT_EQ(set.hasM(), tc.set_m);
    EXPECT_EQ(set.size(), expected.size());
  }
}

TEST(OrdinateSetTest, DisallowedChangesThrowOnChangeAndAllowNoOps) {
  OrdinateSet xy = OrdinateSet::createXY();
  xy.setChangesAllowed(false);
  EXPECT_FALSE(xy.changesAllowed());

  // No-op calls do not mutate and must not throw even when locked.
  xy.setZ(false);
  xy.setM(false);
  EXPECT_EQ(xy, OrdinateSet::createXY());

  try {
    xy.setZ(true);
    FAIL() << "Expected GEOSException when adding Z while locked";
  } catch (const GEOSException& e) {
    EXPECT_THAT(e.what(), HasSubstr("Cannot add additional ordinates"));
  }
  EXPECT_EQ(xy, OrdinateSet::createXY());

  try {
    xy.setM(true);
    FAIL() << "Expected GEOSException when adding M while locked";
  } catch (const GEOSException& e) {
    EXPECT_THAT(e.what(), HasSubstr("Cannot add additional ordinates"));
  }
  EXPECT_EQ(xy, OrdinateSet::createXY());

  OrdinateSet xyzm = OrdinateSet::createXYZM();
  xyzm.setChangesAllowed(false);

  xyzm.setZ(true);
  xyzm.setM(true);
  EXPECT_EQ(xyzm, OrdinateSet::createXYZM());

  EXPECT_THROW(xyzm.setZ(false), GEOSException);
  EXPECT_EQ(xyzm, OrdinateSet::createXYZM());

  EXPECT_THROW(xyzm.setM(false), GEOSException);
  EXPECT_EQ(xyzm, OrdinateSet::createXYZM());

  // Re-enabling changes allows modifications again.
  xyzm.setChangesAllowed(true);
  EXPECT_TRUE(xyzm.changesAllowed());
  xyzm.setZ(false);
  xyzm.setM(false);
  EXPECT_EQ(xyzm, OrdinateSet::createXY());
}

TEST(OrdinateSetTest, CopyPreservesOrdinatesAndLockState) {
  OrdinateSet original = OrdinateSet::createXYZ();
  original.setChangesAllowed(false);

  OrdinateSet copy_constructed(original);
  EXPECT_EQ(copy_constructed, original);
  EXPECT_FALSE(copy_constructed.changesAllowed());
  EXPECT_THROW(copy_constructed.setM(true), GEOSException);

  OrdinateSet copy_assigned = OrdinateSet::createXY();
  copy_assigned = original;
  EXPECT_EQ(copy_assigned, original);
  EXPECT_FALSE(copy_assigned.changesAllowed());
  EXPECT_THROW(copy_assigned.setZ(false), GEOSException);
}

}  // namespace
}  // namespace geos_google
