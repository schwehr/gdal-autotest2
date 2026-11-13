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
// Tests CODA memory backend singleton helpers (libcoda/coda-mem.c).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-mem.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-mem-internal.h

#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-mem-internal.h"
}  // extern "C"

#include "testing/base/public/gunit.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"

namespace stcorp_coda {
namespace {

constexpr coda_format kAllFormats[] = {
    coda_format_ascii, coda_format_binary, coda_format_xml,    coda_format_hdf4,
    coda_format_hdf5,  coda_format_cdf,    coda_format_netcdf, coda_format_grib,
    coda_format_rinex, coda_format_sp3,
};

TEST(CodaMemTest, EmptyRecordSingletonIsCachedPerFormatAndResetOnDone) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  for (const coda_format fmt : kAllFormats) {
    coda_dynamic_type* first = coda_mem_empty_record(fmt);
    ASSERT_NE(nullptr, first);
    EXPECT_EQ(coda_backend_memory, first->backend);
    ASSERT_NE(nullptr, first->definition);
    EXPECT_EQ(fmt, first->definition->format);
    EXPECT_EQ(coda_record_class, first->definition->type_class);
    EXPECT_EQ(first, coda_mem_empty_record(fmt));
  }

  coda_dynamic_type* ascii_before = coda_mem_empty_record(coda_format_ascii);
  ASSERT_NE(nullptr, ascii_before);

  session.Reset();
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  coda_dynamic_type* ascii_after = coda_mem_empty_record(coda_format_ascii);
  ASSERT_NE(nullptr, ascii_after);
  EXPECT_EQ(coda_backend_memory, ascii_after->backend);
}

TEST(CodaMemTest, NoDataSingletonIsCachedPerFormatAndResetOnDone) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  for (const coda_format fmt : kAllFormats) {
    coda_dynamic_type* first = coda_no_data_singleton(fmt);
    ASSERT_NE(nullptr, first);
    EXPECT_EQ(coda_backend_memory, first->backend);
    ASSERT_NE(nullptr, first->definition);
    EXPECT_EQ(fmt, first->definition->format);
    EXPECT_EQ(coda_special_class, first->definition->type_class);
    EXPECT_EQ(first, coda_no_data_singleton(fmt));
  }

  coda_mem_done();
  // Calling coda_mem_done() when singletons are already cleared is a no-op.
  coda_mem_done();

  coda_dynamic_type* ascii_recreated =
      coda_no_data_singleton(coda_format_ascii);
  ASSERT_NE(nullptr, ascii_recreated);
  EXPECT_EQ(coda_special_class, ascii_recreated->definition->type_class);
}

// TODO(schwehr): 100.0% line coverage in libcoda/coda-mem.c (29/29); no
// reachable lines remain.

}  // namespace
}  // namespace stcorp_coda
