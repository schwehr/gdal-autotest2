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
// Unit tests for PROJ internal tracing helpers and google3 build configuration
// (proj/src/tracing.cpp and proj/include/proj/internal/tracing.hpp).
// Note: google3 builds omit -DENABLE_TRACING, so proj/src/tracing.cpp compiles
// 0 instrumented lines and tracing.hpp macros compile to no-ops.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/tracing.cpp

#include "third_party/proj4/proj/include/proj/internal/tracing.hpp"

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"

namespace proj {
namespace {

#ifdef ENABLE_TRACING
#error "ENABLE_TRACING is unexpectedly defined in google3"
constexpr bool kEnableTracingDefined = true;
#else
constexpr bool kEnableTracingDefined = false;
#endif
static_assert(!kEnableTracingDefined,
              "google3 PROJ builds compile tracing.hpp as no-ops");

void NestedTracedHelper() {
  ENTER_FUNCTION();
  logTrace("helper_event");
}

TEST(TracingTest, MacrosAndLogTraceCompileAndExecuteAsNoOps) {
  ENTER_FUNCTION();
  {
    ENTER_BLOCK("outer_block");
    logTrace("single_arg_trace");
    logTrace("component_trace", "operation_factory");
    NestedTracedHelper();
  }

  // Exercise PROJ operation factory call sites that invoke ENTER_FUNCTION() and
  // ENTER_BLOCK() internally when building and executing a transformation.
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedPj op(
      proj_create_crs_to_crs(ctx.get(), "EPSG:4326", "EPSG:3857", nullptr));
  ASSERT_NE(nullptr, op.get());
  const PJ_COORD out =
      proj_trans(op.get(), PJ_FWD, proj_coord(0.0, 0.0, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(op.get()));
  EXPECT_NEAR(0.0, out.xy.x, 1e-9);
  EXPECT_NEAR(0.0, out.xy.y, 1e-9);
}

}  // namespace
}  // namespace proj
