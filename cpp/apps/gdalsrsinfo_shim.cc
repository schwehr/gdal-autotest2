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

#include "third_party/gdal/autotest2/cpp/apps/gdalsrsinfo_shim.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/commonutils.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

#ifdef DO_NOT_DEFINE_GDAL_DATE_NAME
#undef DO_NOT_DEFINE_GDAL_DATE_NAME
#endif
#include "third_party/gdal/gcore/gdal_version_full/gdal_version.h"

namespace autotest2_gdalsrsinfo {

[[noreturn]] inline void exit(int code) { throw TestExitException{code}; }

static inline void TrackingCSLDestroy(char**& papsz) {
  ::CSLDestroy(papsz);
  papsz = nullptr;
}

#ifdef MAIN_START
#undef MAIN_START
#endif
#ifdef MAIN_END
#undef MAIN_END
#endif

#define exit ::autotest2_gdalsrsinfo::exit
#define CSLDestroy(x) ::autotest2_gdalsrsinfo::TrackingCSLDestroy(x)
#define GDALDestroyDriverManager() (void)0
#define OGRCleanupAll() (void)0
#define MAIN_START(argc, argv) \
  int RunGdalSrsInfoMainImpl(int argc, char**& argv)
#define MAIN_END

#include "third_party/gdal/apps/gdalsrsinfo.cpp"

#undef MAIN_END
#undef MAIN_START
#undef OGRCleanupAll
#undef GDALDestroyDriverManager
#undef CSLDestroy
#undef exit

int RunGdalSrsInfoMain(const std::vector<std::string>& args) {
  CPLErrorReset();
  const CPLStringList saved_config_options(CPLGetConfigOptions(),
                                           /*bTakeOwnership=*/TRUE);
  const CPLStringList saved_tl_config_options(CPLGetThreadLocalConfigOptions(),
                                              /*bTakeOwnership=*/TRUE);

  CPLStringList aos;
  for (const auto& arg : args) {
    aos.AddString(arg.c_str());
  }
  char** argv = aos.List();
  char** const kInitialArgv = argv;

  absl::Cleanup cleanup = [&] {
    if (argv != nullptr && argv != kInitialArgv) {
      ::CSLDestroy(argv);
      argv = nullptr;
    }
    CPLSetConfigOptions(saved_config_options.List());
    CPLSetThreadLocalConfigOptions(saved_tl_config_options.List());
    CPLErrorReset();
  };

  int ret = 0;
  try {
    ret = RunGdalSrsInfoMainImpl(aos.Count(), argv);
  } catch (const TestExitException& e) {
    ret = e.code;
  }
  return ret;
}

}  // namespace autotest2_gdalsrsinfo
