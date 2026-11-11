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

// Self-tests for scoped_geos_c.h and temp_file.h test helpers.

#define GEOS_USE_ONLY_R_API

#include "third_party/geos/google/util/scoped_geos_c.h"

#include <filesystem>  // NOLINT
#include <fstream>
#include <string>
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/capi/geos_c.h"
#include "third_party/geos/google/util/temp_file.h"

namespace geos_google {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

TEST(ScopedGeosCTest, ContextCapturesErrorsAndSupportsMove) {
  ScopedContext ctx;
  ASSERT_TRUE(ctx.valid());

  ScopedContext moved_ctx = std::move(ctx);
  EXPECT_FALSE(ctx.valid());
  ASSERT_TRUE(moved_ctx.valid());

  ScopedWKTReader reader(moved_ctx, GEOSWKTReader_create_r(moved_ctx));
  ASSERT_TRUE(reader);

  ScopedGeom valid(moved_ctx,
                   GEOSWKTReader_read_r(moved_ctx, reader, "POINT (3 4)"));
  ASSERT_TRUE(valid);
  EXPECT_EQ(GEOSGeomTypeId_r(moved_ctx, valid), GEOS_POINT);
  EXPECT_THAT(moved_ctx.errors(), IsEmpty());

  ScopedGeom bad(moved_ctx, GEOSWKTReader_read_r(moved_ctx, reader, "NOT_WKT"));
  EXPECT_FALSE(bad);
  ASSERT_THAT(moved_ctx.errors(), Not(IsEmpty()));
  EXPECT_THAT(moved_ctx.errors().front(), HasSubstr("ParseException"));

  moved_ctx.ClearMessages();
  EXPECT_THAT(moved_ctx.errors(), IsEmpty());
}

TEST(ScopedGeosCTest, BufferAndPreparedGeomRaii) {
  ScopedContext ctx;
  ASSERT_TRUE(ctx.valid());

  ScopedGeom poly(
      ctx, GEOSGeomFromWKT_r(ctx, "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))"));
  ASSERT_TRUE(poly);

  ScopedPreparedGeom prepared(ctx, GEOSPrepare_r(ctx, poly));
  ASSERT_TRUE(prepared);

  ScopedGeom pt(ctx, GEOSGeom_createPointFromXY_r(ctx, 1.0, 1.0));
  ASSERT_TRUE(pt);
  EXPECT_EQ(GEOSPreparedContains_r(ctx, prepared, pt), 1);

  ScopedWKTWriter writer(ctx, GEOSWKTWriter_create_r(ctx));
  ASSERT_TRUE(writer);
  ScopedCString wkt(ctx, GEOSWKTWriter_write_r(ctx, writer, pt));
  ASSERT_TRUE(wkt);
  EXPECT_EQ(std::string(wkt.get()), "POINT (1 1)");
}

TEST(TempFileTest, CreatesAndCleansUpFileAndDir) {
  std::filesystem::path file_path;
  {
    TempFile tf("geos_util_test/sample.wkt");
    file_path = tf.path();
    std::ofstream out(tf.path());
    ASSERT_TRUE(out.is_open());
    out << "POINT (1 2)\n";
    out.close();
    EXPECT_TRUE(std::filesystem::exists(file_path));
  }
  EXPECT_FALSE(std::filesystem::exists(file_path));

  std::filesystem::path dir_path;
  {
    TempDir td("geos_util_dir_test");
    dir_path = td.path();
    EXPECT_TRUE(std::filesystem::exists(dir_path));
    std::ofstream out(td.child("child.txt"));
    out << "ok";
  }
  EXPECT_FALSE(std::filesystem::exists(dir_path));
}

}  // namespace
}  // namespace geos_google
