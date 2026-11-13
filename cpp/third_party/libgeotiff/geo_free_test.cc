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
// Tests GTIFFree resource deallocation across GeoKey types and PROJ contexts
// (geo_free.c).
//
// See also:
//   https://github.com/OSGeo/libgeotiff/blob/master/libgeotiff/geo_free.c

#include "testing/base/public/gunit.h"
#include "third_party/libgeotiff/geo_normalize.h"
// clang-format off
#include "third_party/libgeotiff/geokeys.h"
#include "third_party/libgeotiff/geotiff.h"
#include "third_party/libgeotiff/geo_tiffp.h"  // IWYU pragma: keep
#include "third_party/libgeotiff/geo_keyp.h"
// clang-format on
#include "third_party/libgeotiff/geovalues.h"
#include "third_party/libgeotiff/google/util/scoped_geotiff.h"

namespace libgeotiff {
namespace {

TEST(GeoFreeTest, FreeNullIsNoOp) { GTIFFree(nullptr); }

TEST(GeoFreeTest, FreeEmptyHandleWithoutDoublesOrProjContext) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.st);
  ASSERT_NE(nullptr, handle.gtif);

  EXPECT_NE(nullptr, handle.gtif->gt_short);
  EXPECT_NE(nullptr, handle.gtif->gt_double);
  EXPECT_EQ(0, handle.gtif->own_pj_context);
  EXPECT_EQ(nullptr, handle.gtif->pj_context);

  handle.gtif.reset();
}

TEST(GeoFreeTest, FreeHandleWithAsciiDoublesAndOwnedProjContext) {
  SimpleTagsGtif handle = MakeSimpleTagsGtif();
  ASSERT_NE(nullptr, handle.st);
  ASSERT_NE(nullptr, handle.gtif);

  // 1. Allocate SHORT, ASCII, and DOUBLE key entries plus gt_double array.
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTModelTypeGeoKey, TYPE_SHORT, 1,
                          ModelTypeProjected));
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GTCitationGeoKey, TYPE_ASCII, 0,
                          "Owned ASCII Buffer"));
  ASSERT_EQ(1, GTIFKeySet(handle.gtif.get(), GeogSemiMajorAxisGeoKey,
                          TYPE_DOUBLE, 1, 6378137.0));
  ASSERT_EQ(1, GTIFWriteKeys(handle.gtif.get()));
  EXPECT_NE(nullptr, handle.gtif->gt_double);

  // 2. Lazily allocate an owned PJ_CONTEXT inside GTIF so GTIFFree destroys it.
  int owned = 0;
  void* ctx = GTIFGetPROJContext(handle.gtif.get(), 1, &owned);
  ASSERT_NE(nullptr, ctx);
  EXPECT_EQ(1, owned);
  EXPECT_EQ(1, handle.gtif->own_pj_context);

  // 3. Free GTIF before ST_Destroy runs (enforced by SimpleTagsGtif order).
  handle.gtif.reset();
}

TEST(GeoFreeTest, FreeManuallyClearedMinimalStruct) {
  auto* raw = static_cast<GTIF*>(_GTIFcalloc(sizeof(GTIF)));
  ASSERT_NE(nullptr, raw);
  EXPECT_EQ(nullptr, raw->gt_keys);
  EXPECT_EQ(nullptr, raw->gt_keyindex);
  GTIFFree(raw);
}

}  // namespace
}  // namespace libgeotiff
