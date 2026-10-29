// Tests for cpl_cpu_features:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_cpu_features.h
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_cpu_features.cpp
//
// Copyright 2026 Google Inc. All Rights Reserved.
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

// NOTE: //third_party/gdal:port_lib is compiled without the
// HAVE_{SSE,SSSE3,AVX}_AT_COMPILE_TIME macros, so none of the cpuid-based
// runtime detection in cpl_cpu_features.cpp is linked into this binary.
// Defining the macros here only instantiates the header's static inline
// functions, which on x86_64 unconditionally return true (SSE), or true when
// the matching compiler flag is set (SSSE3/AVX).  This test is therefore only
// a compile smoke test for the header; it is restricted to x86_64 because on
// other architectures the header declares non-inline functions that port_lib
// does not define, which would fail to link.

#include "testing/base/public/gunit.h"

#if defined(__x86_64) || defined(_M_X64)

#define HAVE_SSE_AT_COMPILE_TIME
#if defined(__SSSE3__)
#define HAVE_SSSE3_AT_COMPILE_TIME
#endif
#if defined(__AVX__)
#define HAVE_AVX_AT_COMPILE_TIME
#endif
#include "third_party/gdal/port/cpl_cpu_features.h"

namespace autotest2 {
namespace {

// Compile smoke test only; see the note at the top of this file.
TEST(CplCpuFeaturesTest, HeaderCompilesOnX86_64) {
  EXPECT_TRUE(CPLHaveRuntimeSSE());
#if defined(__SSSE3__)
  EXPECT_TRUE(CPLHaveRuntimeSSSE3());
#endif
#if defined(__AVX__)
  EXPECT_TRUE(CPLHaveRuntimeAVX());
#endif
}

}  // namespace
}  // namespace autotest2

#endif  // defined(__x86_64) || defined(_M_X64)
