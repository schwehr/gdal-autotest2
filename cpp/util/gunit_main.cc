// Use this module in place of //testing/base/public:gunit_main to have tests
// run with a local Spanner test universe.

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "testing/base/public/benchmark.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/flags/flag.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_error.h"

int main(int argc, char **argv) {
  absl::SetFlag(&FLAGS_logtostderr, true);
  testing::InitGUnit(&argc, &argv);

  CPLSetErrorHandler(CPLGoogleLogErrorHandler);

  RunSpecifiedBenchmarks();

  return RUN_ALL_TESTS();
}
