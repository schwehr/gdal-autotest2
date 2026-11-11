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

// Unit tests for //third_party/netcdf/libdispatch/nclog.c.
//
// Exercises initialization paths (including NCLOGGING/NCTRACING environment
// variables inside execve-isolated child processes), log level filtering,
// raw text logging, vprintf forwarding, trace frame enter/exit matching,
// and ncthrow/ncbreakpoint helpers.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/nclog.c

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
#include "third_party/netcdf/include/nclog.h"
}  // extern "C"

namespace netcdf {
namespace {

class ScopedNcLogState {
 public:
  ScopedNcLogState()
      : prev_log_level_(ncsetloglevel(NCLOGOFF)),
        prev_trace_level_(nctracelevel(-1)) {
    nclogopen(nullptr);
  }

  ~ScopedNcLogState() {
    nclogopen(nullptr);
    nctracelevel(prev_trace_level_);
    nclogopen(nullptr);
    ncsetloglevel(prev_log_level_);
  }

  ScopedNcLogState(const ScopedNcLogState&) = delete;
  ScopedNcLogState& operator=(const ScopedNcLogState&) = delete;

 private:
  int prev_log_level_;
  int prev_trace_level_;
};

class ScopedLogCapture {
 public:
  explicit ScopedLogCapture(std::string_view filename)
      : file_(filename), stream_(std::fopen(file_.c_str(), "w+")) {}

  ~ScopedLogCapture() {
    nclogopen(nullptr);
    if (stream_ != nullptr) {
      std::fclose(stream_);
      stream_ = nullptr;
    }
  }

  ScopedLogCapture(const ScopedLogCapture&) = delete;
  ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

  bool valid() const { return stream_ != nullptr; }

  void Attach() { nclogopen(stream_); }

  std::string ReadAll() {
    if (stream_ == nullptr) {
      return "";
    }
    std::fflush(stream_);
    std::rewind(stream_);
    std::string out;
    char buf[512];
    while (const size_t n = std::fread(buf, 1, sizeof(buf), stream_)) {
      out.append(buf, n);
    }
    return out;
  }

 private:
  TempFile file_;
  std::FILE* stream_ = nullptr;
};

void ForwardNcvlog(int level, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ncvlog(level, fmt, ap);
  va_end(ap);
}

TEST(NclogTest, ColdInitReadsEnvVarsAndImplicitInitPathsWork) {
  // Valid NCLOGGING tag (case-insensitive) and NCTRACING preset levels.
  RunInExecveChild([]() -> bool {
    ::setenv(NCENVLOGGING, "warn", 1);
    ::setenv(NCENVTRACING, "2", 1);
    ncloginit();
    // Second call hits the already-initialized early return.
    ncloginit();
    const int log_level = ncsetloglevel(-1);
    const int trace_level = nctracelevel(-1);
    return log_level == NCLOGNOTE && trace_level == 2;
  });

  // NCLOGGING="off" (level == 0) does not elevate loglevel; unset NCTRACING.
  RunInExecveChild([]() -> bool {
    ::setenv(NCENVLOGGING, "off", 1);
    ::unsetenv(NCENVTRACING);
    ncloginit();
    return ncsetloglevel(-1) == NCLOGOFF && nctracelevel(-1) == -1;
  });

  // Unrecognized NCLOGGING tag scans the full tag table and falls back to OFF.
  RunInExecveChild([]() -> bool {
    ::setenv(NCENVLOGGING, "no_such_tag", 1);
    ::unsetenv(NCENVTRACING);
    ncloginit();
    return ncsetloglevel(-1) == NCLOGOFF;
  });

  // Implicit initialization via each entry point before ncloginit().
  RunInExecveChild([]() -> bool {
    ::unsetenv(NCENVLOGGING);
    ::unsetenv(NCENVTRACING);
    return ncsetloglevel(NCLOGWARN) == NCLOGOFF;
  });
  RunInExecveChild([]() -> bool {
    ::unsetenv(NCENVLOGGING);
    ::unsetenv(NCENVTRACING);
    return nclogopen(nullptr) == 1;
  });
  RunInExecveChild([]() -> bool {
    ::unsetenv(NCENVLOGGING);
    ::unsetenv(NCENVTRACING);
    nclog(NCLOGNOTE, "ignored_at_off");
    return ncsetloglevel(-1) == NCLOGOFF;
  });
  RunInExecveChild([]() -> bool {
    ::unsetenv(NCENVLOGGING);
    ::unsetenv(NCENVTRACING);
    return nctracelevel(1) == -1;
  });
  RunInExecveChild([]() -> bool {
    ::unsetenv(NCENVLOGGING);
    ::unsetenv(NCENVTRACING);
    nctrace(5, "cold_fn", nullptr);
    return ncuntrace("cold_fn", NC_NOERR, nullptr) == NC_NOERR;
  });
}

TEST(NclogTest, SetLogLevelAndFormattedLoggingFilterAndTagProperly) {
  ScopedNcLogState log_guard;
  ScopedLogCapture capture("nclog_formatted.log");
  ASSERT_TRUE(capture.valid());
  capture.Attach();

  // Out-of-range levels leave loglevel unchanged.
  EXPECT_EQ(NCLOGOFF, ncsetloglevel(-1));
  EXPECT_EQ(NCLOGOFF, ncsetloglevel(NCLOGDEBUG + 1));
  EXPECT_EQ(NCLOGOFF, ncsetloglevel(-1));

  // At NCLOGOFF, WARN is filtered out, ERR is forced, and negative tags map
  // to "unknown".
  nclog(NCLOGERR, nullptr);
  nclog(NCLOGWARN, "suppressed %d", 1);
  nclog(NCLOGERR, "forced %s", "error");
  nclog(-1, "negtag");
  ForwardNcvlog(NCLOGERR, nullptr);

  EXPECT_EQ(NCLOGOFF, ncsetloglevel(NCLOGDEBUG));
  nclog(NCLOGOFF, "off_msg");
  nclog(NCLOGWARN, "warn_msg");
  nclog(NCLOGNOTE, "note_msg");
  nclog(NCLOGDEBUG, "debug_msg");

  const std::string output = capture.ReadAll();
  EXPECT_THAT(output, ::testing::Not(::testing::HasSubstr("suppressed")));
  EXPECT_THAT(output, ::testing::HasSubstr("ERR: forced error\n"));
  EXPECT_THAT(output, ::testing::HasSubstr("unknown: negtag\n"));
  EXPECT_THAT(output, ::testing::HasSubstr("ERR: \n"));
  EXPECT_THAT(output, ::testing::HasSubstr("OFF: off_msg\n"));
  EXPECT_THAT(output, ::testing::HasSubstr("WARN: warn_msg\n"));
  EXPECT_THAT(output, ::testing::HasSubstr("NOTE: note_msg\n"));
  EXPECT_THAT(output, ::testing::HasSubstr("unknown: debug_msg\n"));
}

TEST(NclogTest, LogTextAndLogTextNRespectLogLevelThreshold) {
  ScopedNcLogState log_guard;
  ScopedLogCapture capture("nclog_text.log");
  ASSERT_TRUE(capture.valid());
  capture.Attach();

  ncsetloglevel(NCLOGWARN);
  nclogtext(NCLOGERR, "suppressed_text\n");
  nclogtextn(NCLOGERR, "suppressed_n\n", 12);
  nclogtext(NCLOGWARN, "kept_warn|");
  nclogtextn(NCLOGNOTE, "prefix_ignored", 6);

  EXPECT_EQ("kept_warn|prefix", capture.ReadAll());
}

TEST(NclogTest, TraceFramesHandleEnterMoreExitErrorsAndMismatches) {
  ScopedNcLogState log_guard;
  ScopedLogCapture capture("nclog_trace.log");
  ASSERT_TRUE(capture.valid());

  // nctracelevel >= 0 resets stream to stderr, so attach capture afterward.
  EXPECT_EQ(-1, nctracelevel(2));
  capture.Attach();

  // Unmatched untrace at depth == 0.
  EXPECT_EQ(NC_NOERR, ncuntrace("orphan", NC_NOERR, nullptr));

  // Balanced enter/more/nested/exit trace sequence.
  nctrace(1, "outer", "arg=%d", 42);
  nctracemore(1, "detail=%s", "mid");
  nctrace(2, "inner", nullptr);
  nctrace(5, "silent", "suppressed");
  EXPECT_EQ(NC_NOERR, ncuntrace("silent", NC_NOERR, nullptr));
  EXPECT_EQ(NC_NOERR, ncuntrace("wrong_name", NC_NOERR, nullptr));
  EXPECT_EQ(NC_EINVAL, ncuntrace("outer", NC_EINVAL, "failed"));

  // Normal exit with err == NC_NOERR and non-null format.
  nctrace(0, "clean", nullptr);
  EXPECT_EQ(NC_NOERR, ncuntrace("clean", NC_NOERR, "done=%d", 7));

  // Negative tracelevel preserves custom stream.
  EXPECT_EQ(2, nctracelevel(-1));
  nctrace(0, "quiet_fn", "hidden");
  EXPECT_EQ(NC_NOERR, ncuntrace("quiet_fn", NC_NOERR, nullptr));

  const std::string output = capture.ReadAll();
  EXPECT_THAT(output,
              ::testing::HasSubstr("*** Unmatched untrace: orphan: depth==0"));
  EXPECT_THAT(output, ::testing::HasSubstr("Enter: (1): outer:arg=42\n"));
  EXPECT_THAT(output, ::testing::HasSubstr("detail=mid\n"));
  EXPECT_THAT(output, ::testing::HasSubstr("Enter: (2): inner:\n"));
  EXPECT_THAT(output, ::testing::Not(::testing::HasSubstr("silent")));
  EXPECT_THAT(output,
              ::testing::HasSubstr(
                  "*** Unmatched untrace: fcn=inner expected=wrong_name"));
  EXPECT_THAT(output, ::testing::HasSubstr("Exit: (1): outer: err=(" +
                                           std::to_string(NC_EINVAL) + ")"));
  EXPECT_THAT(output, ::testing::HasSubstr("Exit: (0): clean: done=7\n"));
  EXPECT_THAT(output, ::testing::Not(::testing::HasSubstr("quiet_fn")));
}

TEST(NclogTest, ThrowAndBreakpointReturnPassedErrorCodes) {
  EXPECT_EQ(0, ncthrow(0, __FILE__, __LINE__));
  EXPECT_EQ(NC_EINVAL, ncthrow(NC_EINVAL, __FILE__, __LINE__));
  EXPECT_EQ(NC_EBADID, ncbreakpoint(NC_EBADID));
}

}  // namespace
}  // namespace netcdf
