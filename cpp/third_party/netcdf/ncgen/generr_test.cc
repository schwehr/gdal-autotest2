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

// Unit tests for //third_party/netcdf/ncgen/generr.c.
//
// Covers error/warning formatters (vderror, vdwarn, derror, verror, semwarn,
// semerror), format marker latching (markcdf4, getmarkcdf4, markcdf5,
// getmarkcdf5), and bounded formatting (nprintf).
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/generr.c

#include <fcntl.h>
#include <setjmp.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
char* getmarkcdf5(void);
}  // extern "C"

namespace netcdf {
namespace {

void CleanupNcgenBuffers() {
  if (filldatalist != nullptr) {
    filldatalist->readonly = 0;
    freedatalist(filldatalist);
    filldatalist = nullptr;
  }
  bbFree(codebuffer);
  codebuffer = nullptr;
  bbFree(stmt);
  stmt = nullptr;
}

class GenerrTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CleanupNcgenBuffers();
    ncgen_shim_reset_globals();
  }

  void TearDown() override {
    ncgen_shim_set_finalize_jmp(nullptr, nullptr);
    CleanupNcgenBuffers();
  }
};

template <typename Fn>
std::string CaptureStderr(Fn&& fn) {
  TempFile err_file("generr_stderr.txt");
  std::fflush(stderr);
  const int saved_fd = ::dup(STDERR_FILENO);
  if (saved_fd < 0) {
    return "";
  }
  const int tmp_fd =
      ::open(err_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (tmp_fd < 0) {
    ::close(saved_fd);
    return "";
  }
  ::dup2(tmp_fd, STDERR_FILENO);
  ::close(tmp_fd);

  {
    const absl::Cleanup restore_stderr = [saved_fd] {
      std::fflush(stderr);
      ::dup2(saved_fd, STDERR_FILENO);
      ::close(saved_fd);
    };
    fn();
  }

  std::ifstream in(err_file.path(), std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

void CallVdwarn(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vdwarn(fmt, args);
  va_end(args);
}

void CallVderror(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vderror(fmt, args);
  va_end(args);
}

TEST_F(GenerrTest, DerrorAndVderrorIncrementErrorCountAndFormatStderr) {
  const std::string output = CaptureStderr([] {
    derror("bad token %s (%d)", "foo", 7);
    CallVderror("vderror detail %d", 99);
  });
  EXPECT_EQ(2, error_count);
  EXPECT_THAT(output, ::testing::HasSubstr("bad token foo (7)\n"));
  EXPECT_THAT(output, ::testing::HasSubstr("vderror detail 99\n"));
}

TEST_F(GenerrTest, VdwarnAndSemwarnDoNotIncrementErrorCount) {
  char prog[] = "ncgen_unit";
  char cdl[] = "sample.cdl";
  progname = prog;
  cdlname = cdl;
  const absl::Cleanup reset_names = [] {
    progname = nullptr;
    cdlname = nullptr;
  };

  const std::string output = CaptureStderr([] {
    CallVdwarn("warning message %s", "abc");
    semwarn(17, "unused dim %s", "lat");
  });
  EXPECT_EQ(0, error_count);
  EXPECT_THAT(output, ::testing::HasSubstr("warning message abc\n"));
  EXPECT_THAT(output, ::testing::HasSubstr(
                          "ncgen_unit: sample.cdl line 17: unused dim lat\n"));
}

TEST_F(GenerrTest, VerrorPrependsClassicPrefixAndIncrementsErrorCount) {
  const std::string output =
      CaptureStderr([] { verror("type %s not allowed", "int64"); });
  EXPECT_EQ(1, error_count);
  EXPECT_THAT(output,
              ::testing::HasSubstr(
                  "netCDF classic: not supported: type int64 not allowed\n"));
}

TEST_F(GenerrTest, SemerrorLogsLineAndInvokesFinalizeNetcdf) {
  char prog[] = "ncgen_unit";
  char cdl[] = "input.cdl";
  progname = prog;
  cdlname = cdl;
  const absl::Cleanup reset_names = [] {
    progname = nullptr;
    cdlname = nullptr;
  };

  sigjmp_buf env;
  int retcode = -1;
  ncgen_shim_set_finalize_jmp(&env, &retcode);
  const absl::Cleanup disarm = [] {
    ncgen_shim_set_finalize_jmp(nullptr, nullptr);
  };

  const std::string output = CaptureStderr([&] {
    if (sigsetjmp(env, 1) == 0) {
      semerror(42, "duplicate symbol %s", "temp");
      FAIL() << "semerror unexpectedly returned";
    }
  });
  EXPECT_EQ(1, retcode);
  EXPECT_EQ(1, error_count);
  EXPECT_THAT(output,
              ::testing::HasSubstr(
                  "ncgen_unit: input.cdl line 42: duplicate symbol temp\n"));
}

TEST_F(GenerrTest, MarkCdf4AndMarkCdf5LatchFirstMessageOnly) {
  RunInExecveChild([]() -> bool {
    if (getmarkcdf4() != nullptr || getmarkcdf5() != nullptr) {
      return false;
    }
    markcdf4("first4");
    if (enhanced_flag != 1 || getmarkcdf4() == nullptr ||
        std::strcmp(getmarkcdf4(), "first4") != 0) {
      return false;
    }
    markcdf4("second4");
    if (std::strcmp(getmarkcdf4(), "first4") != 0) {
      return false;
    }

    markcdf5("first5");
    if (cdf5_flag != 1 || getmarkcdf5() == nullptr ||
        std::strcmp(getmarkcdf5(), "first5") != 0) {
      return false;
    }
    markcdf5("second5");
    return std::strcmp(getmarkcdf5(), "first5") == 0;
  });
}

TEST_F(GenerrTest, NprintfFormatsWithinBufferBounds) {
  char buf[32]{};
  nprintf(buf, sizeof(buf), "%s=%d", "count", 123);
  EXPECT_STREQ("count=123", buf);

  char exact[4]{};
  nprintf(exact, sizeof(exact), "%s", "abc");
  EXPECT_STREQ("abc", exact);
}

}  // namespace
}  // namespace netcdf
