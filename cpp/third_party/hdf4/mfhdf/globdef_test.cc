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
// Tests global variable definitions and initial values (mfhdf/src/globdef.c).
//
// Note:
//   globdef.c only defines global variables (ncerr, ncopts, cdf_routine_name)
//   and contains zero executable functions; LLVM coverage emits LF = 0.
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/globdef.c

#include <cstdint>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

// GoogleTest defines SUCCEED() and FAIL() as macros; undefine them before
// including HDF4 headers to avoid macro collision under --config=no_modules.
#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/mfhdf/src/mfhdf.h"
#include "third_party/hdf4/mfhdf/src/nc_priv.h"

namespace hdf4 {
namespace {

using ::testing::Eq;
using ::testing::Ne;
using ::testing::NotNull;
using ::testing::StrEq;

class GlobdefTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ncopts_ = ncopts;
    saved_ncerr_ = ncerr;
    saved_routine_name_ = cdf_routine_name;
  }

  void TearDown() override {
    HEclear();
    ncopts = saved_ncopts_;
    ncerr = saved_ncerr_;
    cdf_routine_name = saved_routine_name_;
  }

 private:
  int saved_ncopts_ = 0;
  int saved_ncerr_ = 0;
  const char* saved_routine_name_ = nullptr;
};

TEST_F(GlobdefTest, GlobalVariablesAreAccessibleAndModifiable) {
  static_assert(NC_NOERR == 0);
  static_assert((NC_FATAL | NC_VERBOSE) == 3);

  // Note: Exact startup defaults (ncopts = NC_FATAL | NC_VERBOSE,
  // cdf_routine_name = "netcdf") are intentionally not asserted here because
  // test execution order across the binary can mutate them beforehand.
  // Verify that ncerr can be read and written.
  ncerr = NC_EBADID;
  EXPECT_THAT(ncerr, Eq(NC_EBADID));
  ncerr = NC_NOERR;
  EXPECT_THAT(ncerr, Eq(NC_NOERR));

  // Verify that ncopts can be read and written.
  ncopts = 0;
  EXPECT_THAT(ncopts, Eq(0));
  ncopts = NC_VERBOSE;
  EXPECT_THAT(ncopts, Eq(NC_VERBOSE));

  // Verify that cdf_routine_name can be inspected and updated.
  ASSERT_THAT(cdf_routine_name, NotNull());
  EXPECT_THAT(std::string_view(cdf_routine_name), Ne(""));

  constexpr char kCustomName[] = "my_custom_routine";
  cdf_routine_name = kCustomName;
  EXPECT_THAT(cdf_routine_name, StrEq(kCustomName));
}

TEST_F(GlobdefTest, SDstartClearsNcoptsAndNcdiminqSetsRoutineName) {
  ncopts = 0;
  ncerr = NC_NOERR;

  // ncdiminq sets cdf_routine_name to "ncdiminq".
  EXPECT_THAT(ncdiminq(-1, 0, nullptr, nullptr), Eq(-1));
  EXPECT_THAT(cdf_routine_name, StrEq("ncdiminq"));

  // NCadvise sets ncerr.
  NCadvise(NC_EINVAL, "test advise");
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // Calling SDstart unconditionally resets ncopts to 0.
  ncopts = NC_FATAL | NC_VERBOSE;
  const std::string missing_path =
      (std::filesystem::path(::testing::TempDir()) / "missing_dir" /
       "nonexistent_file.hdf")
          .string();
  const int32_t sd_id = SDstart(missing_path.c_str(), DFACC_READ);
  EXPECT_THAT(sd_id, Eq(FAIL));
  EXPECT_THAT(ncopts, Eq(0));
}

// TODO(schwehr): globdef.c defines only global storage (LF=0; no executable
// lines).

}  // namespace
}  // namespace hdf4
