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

// Unit tests for //third_party/netcdf/libdispatch/doffsets.c.
//
// Covers lazy initialization via NC_class_alignment, idempotency of
// NC_compute_alignments, every supported netCDF type class alignment,
// NULL alignp handling, and invalid/unalignable class fallback logging.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/doffsets.c

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
#include "third_party/netcdf/include/nclog.h"
#include "third_party/netcdf/include/ncoffsets.h"
}

namespace netcdf {
namespace {

TEST(DoffsetsTest, ClassAlignmentCoversAllSupportedClassesAndNullOutPtr) {
  struct AlignCase {
    const char* name;
    int ncclass;
    size_t expected;
  };
  constexpr AlignCase kCases[] = {
      {"NC_BYTE", NC_BYTE, alignof(unsigned char)},
      {"NC_CHAR", NC_CHAR, alignof(char)},
      {"NC_SHORT", NC_SHORT, alignof(short)},
      {"NC_INT", NC_INT, alignof(int)},
      {"NC_FLOAT", NC_FLOAT, alignof(float)},
      {"NC_DOUBLE", NC_DOUBLE, alignof(double)},
      {"NC_UBYTE", NC_UBYTE, alignof(unsigned char)},
      {"NC_USHORT", NC_USHORT, alignof(unsigned short)},
      {"NC_UINT", NC_UINT, alignof(unsigned int)},
      {"NC_INT64", NC_INT64, alignof(long long)},
      {"NC_UINT64", NC_UINT64, alignof(unsigned long long)},
      {"NC_STRING", NC_STRING, alignof(void*)},
      {"NC_VLEN", NC_VLEN, alignof(nc_vlen_t)},
      {"NC_OPAQUE", NC_OPAQUE, alignof(unsigned char)},
  };

  for (const AlignCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    size_t align = 0;
    EXPECT_EQ(NC_NOERR, NC_class_alignment(tc.ncclass, &align));
    EXPECT_EQ(tc.expected, align);
    EXPECT_EQ(NC_NOERR, NC_class_alignment(tc.ncclass, nullptr));
  }
}

TEST(DoffsetsTest, ComputeAlignmentsIsIdempotentAfterInitialization) {
  size_t align_before = 0;
  ASSERT_EQ(NC_NOERR, NC_class_alignment(NC_DOUBLE, &align_before));
  EXPECT_EQ(alignof(double), align_before);

  // Subsequent direct calls hit the already-computed fast return path.
  NC_compute_alignments();
  NC_compute_alignments();

  size_t align_after = 0;
  ASSERT_EQ(NC_NOERR, NC_class_alignment(NC_DOUBLE, &align_after));
  EXPECT_EQ(align_before, align_after);
}

TEST(DoffsetsTest, UnalignableAndUnknownClassesLogAndPreserveOutValue) {
  TempFile log_file("doffsets_nclog.txt");
  FILE* fp = std::fopen(log_file.c_str(), "w");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup restore_log = [&fp]() {
    nclogopen(nullptr);
    if (fp != nullptr) {
      std::fclose(fp);
      fp = nullptr;
    }
  };
  nclogopen(fp);

  constexpr size_t kSentinel = 12345;
  for (const int bad_class : {NC_ENUM, NC_COMPOUND, NC_NAT, -1, 9999}) {
    SCOPED_TRACE(bad_class);
    size_t align = kSentinel;
    EXPECT_EQ(NC_NOERR, NC_class_alignment(bad_class, &align));
    EXPECT_EQ(kSentinel, align);
    EXPECT_EQ(NC_NOERR, NC_class_alignment(bad_class, nullptr));
  }

  std::fflush(fp);
  nclogopen(nullptr);
  std::fclose(fp);
  fp = nullptr;

  std::ifstream in(log_file.c_str());
  const std::string logged((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
  EXPECT_THAT(logged, ::testing::HasSubstr("cannot be aligned"));
}

TEST(DoffsetsTest, PublicAuxClassAlignmentIntegratesCleanly) {
  size_t align = 0;
  EXPECT_EQ(NC_NOERR, ncaux_class_alignment(NC_INT, &align));
  EXPECT_EQ(alignof(int), align);

  TempFile log_file("doffsets_aux_nclog.txt");
  FILE* fp = std::fopen(log_file.c_str(), "w");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup restore_log = [&fp]() {
    nclogopen(nullptr);
    if (fp != nullptr) {
      std::fclose(fp);
      fp = nullptr;
    }
  };
  nclogopen(fp);

  size_t bad_align = 77;
  EXPECT_EQ(NC_EINVAL, ncaux_class_alignment(NC_COMPOUND, &bad_align));
  EXPECT_EQ(0u, bad_align);
}

}  // namespace
}  // namespace netcdf
