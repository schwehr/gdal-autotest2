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
// Tests error logging and advisory routines (mfhdf/src/error.c).
//
// Note: Google3 compiles NetCDF-2 symbols (ncopts, ncerr, nc_serror, NCadvise)
// with the H4_ prefix via nc_priv.h. Death tests assume a forkable test
// process to verify NC_FATAL exit status.
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/error.c

#include <cerrno>
#include <cstring>
#include <string>

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
#include "third_party/hdf4/mfhdf/src/nc_priv.h"

extern "C" {
#include "third_party/hdf4/mfhdf/src/error_priv.h"
}

namespace hdf4 {
namespace {

using ::testing::Eq;
using ::testing::ExitedWithCode;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

class ErrorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ncopts_ = ncopts;
    saved_ncerr_ = ncerr;
    saved_routine_name_ = cdf_routine_name;
    saved_errno_ = errno;
    ncopts = 0;
    ncerr = NC_NOERR;
  }

  void TearDown() override {
    ncopts = saved_ncopts_;
    ncerr = saved_ncerr_;
    cdf_routine_name = saved_routine_name_;
    errno = saved_errno_;
  }

 private:
  int saved_ncopts_ = 0;
  int saved_ncerr_ = 0;
  const char* saved_routine_name_ = nullptr;
  int saved_errno_ = 0;
};

TEST_F(ErrorTest, NcSerrorQuietDoesNotPrintOrExit) {
  ncopts = 0;
  ncerr = NC_NOERR;
  errno = ENOENT;

  ::testing::internal::CaptureStderr();
  nc_serror("quiet error %d", 42);
  const std::string output = ::testing::internal::GetCapturedStderr();

  EXPECT_THAT(output, IsEmpty());
  EXPECT_THAT(errno, Eq(ENOENT));
  EXPECT_THAT(ncerr, Eq(NC_NOERR));
}

TEST_F(ErrorTest, NcSerrorVerboseWithErrnoZeroSetsNoErr) {
  ncopts = NC_VERBOSE;
  errno = 0;
  cdf_routine_name = "test_routine_zero";

  ::testing::internal::CaptureStderr();
  nc_serror("hello %s %d", "world", 123);
  const std::string output = ::testing::internal::GetCapturedStderr();

  EXPECT_THAT(ncerr, Eq(NC_NOERR));
  EXPECT_THAT(errno, Eq(0));
  EXPECT_THAT(output, HasSubstr("test_routine_zero: hello world 123\n"));
}

TEST_F(ErrorTest, NcSerrorVerboseWithErrnoNonZeroSetsSysErr) {
  ncopts = NC_VERBOSE;
  errno = ENOENT;
  cdf_routine_name = "test_routine_sys";

  ::testing::internal::CaptureStderr();
  nc_serror("cannot open file %s", "missing.nc");
  const std::string output = ::testing::internal::GetCapturedStderr();

  EXPECT_THAT(ncerr, Eq(NC_SYSERR));
  EXPECT_THAT(errno, Eq(0));
  EXPECT_THAT(output,
              HasSubstr("test_routine_sys: cannot open file missing.nc: "));
  // Assumes C/POSIX locale error string formatting for strerror(ENOENT).
  EXPECT_THAT(output, HasSubstr(strerror(ENOENT)));
}

TEST_F(ErrorTest, NcAdviseQuietSetsNcerrWithoutOutput) {
  ncopts = 0;
  ncerr = NC_NOERR;

  ::testing::internal::CaptureStderr();
  NCadvise(NC_EINVAL, "quiet advice %d", 99);
  const std::string output = ::testing::internal::GetCapturedStderr();

  EXPECT_THAT(output, IsEmpty());
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));
}

TEST_F(ErrorTest, NcAdviseVerbosePrintsMessage) {
  ncopts = NC_VERBOSE;
  cdf_routine_name = "advise_routine";

  ::testing::internal::CaptureStderr();
  NCadvise(NC_EBADID, "bad cdfid %d", 17);
  const std::string output = ::testing::internal::GetCapturedStderr();

  EXPECT_THAT(ncerr, Eq(NC_EBADID));
  EXPECT_THAT(output, HasSubstr("advise_routine: bad cdfid 17\n"));
}

TEST_F(ErrorTest, NcAdviseFatalWithNoErrDoesNotExit) {
  ncopts = NC_FATAL | NC_VERBOSE;
  cdf_routine_name = "benign_routine";

  ::testing::internal::CaptureStderr();
  NCadvise(NC_NOERR, "harmless notification %d", 1);
  const std::string output = ::testing::internal::GetCapturedStderr();

  EXPECT_THAT(ncerr, Eq(NC_NOERR));
  EXPECT_THAT(output, HasSubstr("benign_routine: harmless notification 1\n"));
}

TEST_F(ErrorTest, NcSerrorFatalExitsProcess) {
  EXPECT_EXIT(
      {
        ncopts = NC_FATAL;
        nc_serror("fatal system error");
      },
      ExitedWithCode(NC_FATAL), "");
}

TEST_F(ErrorTest, NcAdviseFatalExitsProcessWhenErrNotNoErr) {
  EXPECT_EXIT(
      {
        ncopts = NC_FATAL;
        NCadvise(NC_EBADID, "fatal advise error");
      },
      ExitedWithCode(NC_FATAL), "");
  EXPECT_EXIT(
      {
        ncopts = NC_FATAL | NC_VERBOSE;
        NCadvise(NC_EBADID, "verbose fatal %d", 7);
      },
      ExitedWithCode(NC_FATAL | NC_VERBOSE), "verbose fatal 7\n");
}

// TODO(schwehr): error.c has 94.1% isolated line coverage (32/34 lines); the
// only uncovered lines are the defensive NC_VERBOSE-without-errno fallback in
// nc_serror/NCadvise when cdf_routine_name == nullptr.

}  // namespace
}  // namespace hdf4
