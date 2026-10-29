// Copyright 2026 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests for GDAL's process spawning utilities (cpl_spawn.cpp).
//
// Host dependency: the tests that spawn external executables only rely on
// /bin/sh (guaranteed by POSIX) and shell builtins, so they do not depend on
// coreutils such as cat or true being installed in the test sandbox.

#include "third_party/gdal/port/cpl_spawn.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstring>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

// Shell builtin equivalents of cat and true.
constexpr char kShell[] = "/bin/sh";
constexpr char kCatScript[] =
    "while IFS= read -r l; do printf '%s\\n' \"$l\"; done";
constexpr char kTrueScript[] = ":";

int ChildEchoAndExit(CPL_FILE_HANDLE fin, CPL_FILE_HANDLE fout) {
  char buf[16] = {};
  if (CPLPipeRead(fin, buf, 5)) {
    CPLPipeWrite(fout, "ACK:", 4);
    CPLPipeWrite(fout, buf, 5);
  }
  return 7;
}

int ChildKillSelf(CPL_FILE_HANDLE /*fin*/, CPL_FILE_HANDLE /*fout*/) {
  kill(getpid(), SIGKILL);
  return 0;
}

TEST(CplSpawnTest, CPLSpawnWithVsiFilesAndStderr) {
  WithQuietHandler quiet_handler;

  const std::string in_path = "/vsimem/spawn_in.txt";
  const std::string out_path = "/vsimem/spawn_out.txt";
  const std::string payload = "hello from vsimem\n";
  VsiMemTempWrapper in_file(in_path, payload);

  VSILFILE* fin = VSIFOpenL(in_path.c_str(), "rb");
  VSILFILE* fout = VSIFOpenL(out_path.c_str(), "wb+");
  ASSERT_NE(fin, nullptr);
  ASSERT_NE(fout, nullptr);

  const char* const kCatArgs[] = {kShell, "-c", kCatScript, nullptr};
  EXPECT_EQ(CPLSpawn(kCatArgs, fin, fout, FALSE), 0);
  VSIFCloseL(fin);

  VSIFSeekL(fout, 0, SEEK_SET);
  char read_back[64] = {};
  size_t nread = VSIFReadL(read_back, 1, sizeof(read_back) - 1, fout);
  VSIFCloseL(fout);
  VSIUnlink(out_path.c_str());
  EXPECT_EQ(std::string(read_back, nread), payload);

  // Test stderr display and non-zero exit code.
  const char* const kErrArgs[] = {kShell, "-c", "echo err_out >&2; exit 3",
                                  nullptr};
  EXPECT_EQ(CPLSpawn(kErrArgs, nullptr, nullptr, TRUE), 3);

  // Test magic stderr string "An error occurred while forking process" which
  // forces bDisplayErr = TRUE even when passed FALSE.
  const char* const kForkErrArgs[] = {
      kShell, "-c", "echo 'An error occurred while forking process' >&2",
      nullptr};
  EXPECT_EQ(CPLSpawn(kForkErrArgs, nullptr, nullptr, FALSE), 0);

  // Non-existent executable fails posix_spawnp and returns -1.
  const char* const kBadArgs[] = {"/nonexistent_dir_xyz/nonexistent_bin",
                                  nullptr};
  EXPECT_EQ(CPLSpawn(kBadArgs, nullptr, nullptr, FALSE), -1);
}

TEST(CplSpawnTest, CPLSpawnAsyncMagicPipeArgsAndPosixSpawnCombinations) {
  // 1) Magic {pipe_in}, {pipe_out}, {pipe_err} substitutions (bDup2* = false).
  //    The substituted file descriptor numbers become $0, $1 and $2 of the
  //    (no-op) shell script.
  {
    const char* const kMagicArgs[] = {kShell,      "-c",         kTrueScript,
                                      "{pipe_in}", "{pipe_out}", "{pipe_err}",
                                      nullptr};
    CPLSpawnedProcess* sp =
        CPLSpawnAsync(nullptr, kMagicArgs, TRUE, TRUE, TRUE, nullptr);
    ASSERT_NE(sp, nullptr);
    EXPECT_GT(CPLSpawnAsyncGetChildProcessId(sp), 0);
    EXPECT_GE(CPLSpawnAsyncGetInputFileHandle(sp), 0);
    EXPECT_GE(CPLSpawnAsyncGetOutputFileHandle(sp), 0);
    EXPECT_GE(CPLSpawnAsyncGetErrorFileHandle(sp), 0);
    EXPECT_EQ(CPLSpawnAsyncFinish(sp, TRUE, FALSE), 0);
  }

  // 2) Only output pipe created (initializes actions in if(bDup2Out)).
  {
    const char* const kTrueArgs[] = {kShell, "-c", kTrueScript, nullptr};
    CPLSpawnedProcess* sp =
        CPLSpawnAsync(nullptr, kTrueArgs, FALSE, TRUE, FALSE, nullptr);
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(CPLSpawnAsyncFinish(sp, TRUE, FALSE), 0);
  }

  // 3) Only error pipe created (initializes actions in if(bDup2Err)).
  {
    const char* const kTrueArgs[] = {kShell, "-c", kTrueScript, nullptr};
    CPLSpawnedProcess* sp =
        CPLSpawnAsync(nullptr, kTrueArgs, FALSE, FALSE, TRUE, nullptr);
    ASSERT_NE(sp, nullptr);
    EXPECT_EQ(CPLSpawnAsyncFinish(sp, TRUE, FALSE), 0);
  }
}

TEST(CplSpawnTest, CPLSpawnAsyncWithForkMainAndPipeReadWrite) {
  WithQuietHandler quiet_handler;

  // 1) Spawn in-process function via fork() with pipes.
  CPLSpawnedProcess* sp =
      CPLSpawnAsync(ChildEchoAndExit, nullptr, TRUE, TRUE, TRUE, nullptr);
  ASSERT_NE(sp, nullptr);

  CPL_FILE_HANDLE from_child = CPLSpawnAsyncGetInputFileHandle(sp);
  CPL_FILE_HANDLE to_child = CPLSpawnAsyncGetOutputFileHandle(sp);
  EXPECT_TRUE(CPLPipeWrite(to_child, "HELLO", 5));
  CPLSpawnAsyncCloseOutputFileHandle(sp);

  char response[16] = {};
  EXPECT_TRUE(CPLPipeRead(from_child, response, 9));
  EXPECT_STREQ(response, "ACK:HELLO");
  // Further read at EOF returns FALSE.
  EXPECT_FALSE(CPLPipeRead(from_child, response, 1));
  EXPECT_EQ(CPLSpawnAsyncFinish(sp, TRUE, FALSE), 7);

  // 2) Child terminated by signal -> WIFEXITED is false -> returns -1.
  CPLSpawnedProcess* sp_sig =
      CPLSpawnAsync(ChildKillSelf, nullptr, FALSE, FALSE, FALSE, nullptr);
  ASSERT_NE(sp_sig, nullptr);
  EXPECT_EQ(CPLSpawnAsyncFinish(sp_sig, TRUE, FALSE), -1);

  // 3) Finish with bWait = FALSE only frees the handle without reaping the
  //    child, so reap it here to avoid leaving a zombie behind.
  const char* const kTrueArgs[] = {kShell, "-c", kTrueScript, nullptr};
  CPLSpawnedProcess* sp_nowait =
      CPLSpawnAsync(nullptr, kTrueArgs, FALSE, FALSE, FALSE, nullptr);
  ASSERT_NE(sp_nowait, nullptr);
  const CPL_PID nowait_pid = CPLSpawnAsyncGetChildProcessId(sp_nowait);
  EXPECT_EQ(CPLSpawnAsyncFinish(sp_nowait, FALSE, FALSE), 0);
  int status = 0;
  EXPECT_EQ(waitpid(nowait_pid, &status, 0), nowait_pid);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);

  // 4) Error cases: invalid file descriptor in CPLPipeRead / CPLPipeWrite.
  char dummy[4] = {};
  EXPECT_FALSE(CPLPipeRead(-1, dummy, 4));
  EXPECT_FALSE(CPLPipeWrite(-1, "test", 4));
}

}  // namespace
}  // namespace autotest2
