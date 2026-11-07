// Google internal only.

#include "third_party/gdal/autotest2/cpp/util/debug_util.h"

#include <stdlib.h>

#include <string>

#include "base/logging.h"
#include "file/base/filesystem.h"
#include "file/base/helpers.h"
#include "file/base/options.h"
#include "file/base/path.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/status/status.h"
#include "third_party/gloop/util/status/status_macros.h"

namespace autotest2 {

// When the test fails for mis-matching golden and result files, we dump a copy
// of the new results file somewhere we are allowed to.  If
// TEST_UNDECLARED_OUTPUTS_DIR is set, then we are allowed to put it there.  We
// assume the variable got set by Forge, so we put the golden file there and
// tell the user to look for it in sponge.  Otherwise we put it in
// ::testing::TempDir(), which is either something user gave explicitly, or
// something that GoogleInit() set from $TEST_TMPDIR.  An empty string for the
// variables turns into the current directory.
//
// See also:
//   http://go/test-interaction-with-the-filesystem
//   http://go/undeclared-output-manifest
//
// A file mismatch is most likely to occur during //third_party/gdal updates.
// The files in sponge will allow the person inspecting a test failure to see
// the contents of files during failures.

absl::Status CopyFileToSponge(const std::string &source,
                              const std::string &target_name) {
  const char *outputs_dir = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  const bool have_outputs_dir =
      (outputs_dir != nullptr && outputs_dir[0] != '\0');
  const std::string dest = file::JoinPath(
      have_outputs_dir ? outputs_dir : ::testing::TempDir(), target_name);
  LOG_IF(INFO, have_outputs_dir) << "Retrieve files from http://sponge";
  LOG_IF(ERROR, !have_outputs_dir)
      << "TEST_UNDECLARED_OUTPUTS_DIR was not set.  File was written in "
      << "TEST_TMPDIR, which might not exist after the test run.";

  RETURN_IF_ERROR(file::Copy(source, dest, file::Defaults())).LogError();
  return absl::OkStatus();
}

absl::Status SetContentsToSponge(const std::string &contents,
                                 const std::string &target_name) {
  const char *outputs_dir = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  const bool have_outputs_dir =
      (outputs_dir != nullptr && outputs_dir[0] != '\0');
  const std::string dest = file::JoinPath(
      have_outputs_dir ? outputs_dir : ::testing::TempDir(), target_name);

  LOG_IF(INFO, have_outputs_dir) << "Retrieve files from http://sponge";
  LOG_IF(ERROR, !have_outputs_dir)
      << "TEST_UNDECLARED_OUTPUTS_DIR was not set.  File was written in "
      << "TEST_TMPDIR, which might not exist after the test run.";

  RETURN_IF_ERROR(file::SetContents(dest, contents, file::Defaults()))
      .LogError();
  return absl::OkStatus();
}

}  // namespace autotest2
