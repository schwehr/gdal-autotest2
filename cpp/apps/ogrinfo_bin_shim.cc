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

#include "third_party/gdal/autotest2/cpp/apps/ogrinfo_bin_shim.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/commonutils.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_p.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

#ifdef DO_NOT_DEFINE_GDAL_DATE_NAME
#undef DO_NOT_DEFINE_GDAL_DATE_NAME
#endif
#include "third_party/gdal/gcore/gdal_version_full/gdal_version.h"

namespace autotest2_ogrinfo_bin {

[[noreturn]] inline void exit(int code) { throw TestExitException{code}; }

// Defining nested `autotest2_ogrinfo_bin::std` (not reopening `::std` and
// without `#define std` or `#define exit`) intercepts `std::exit(...)` via
// enclosing-namespace lookup while keeping other `std::*` symbols intact.
namespace std {
using namespace ::std;  // NOLINT(build/namespaces)
[[noreturn]] inline void exit(int code) {
  throw ::autotest2_ogrinfo_bin::TestExitException{code};
}
}  // namespace std

static char** g_initial_argv = nullptr;

static inline int WrappedCmdLineProcessor(int argc, char*** pargv,
                                          int nOptions) {
  const int nRet = ::GDALGeneralCmdLineProcessor(argc, pargv, nOptions);
  if (nRet < 1 && *pargv != nullptr) {
    if (*pargv != g_initial_argv) {
      ::CSLDestroy(*pargv);
    }
    *pargv = nullptr;
  }
  return nRet;
}

static inline int WrappedOGRCmdLineProcessor(int argc, char*** pargv,
                                             int nOptions) {
  const int nRet = ::OGRGeneralCmdLineProcessor(argc, pargv, nOptions);
  if (nRet < 1 && *pargv != nullptr) {
    if (*pargv != g_initial_argv) {
      ::CSLDestroy(*pargv);
    }
    *pargv = nullptr;
  }
  return nRet;
}

static inline void TrackingCSLDestroy(char**& papsz) {
  if (papsz != nullptr && papsz != g_initial_argv) {
    ::CSLDestroy(papsz);
  }
  papsz = nullptr;
}

#ifdef MAIN_START
#undef MAIN_START
#endif
#ifdef MAIN_END
#undef MAIN_END
#endif

#define GDALGeneralCmdLineProcessor WrappedCmdLineProcessor
#define OGRGeneralCmdLineProcessor WrappedOGRCmdLineProcessor
#define CSLDestroy(x) ::autotest2_ogrinfo_bin::TrackingCSLDestroy(x)
#define GDALDestroyDriverManager() (void)0
#define OGRCleanupAll() (void)0
#define GDALDestroy() (void)0
#define MAIN_START(argc, argv) int RunOgrInfoMainImpl(int argc, char**& argv)
#define MAIN_END

#include "third_party/gdal/apps/ogrinfo_bin.cpp"

#undef MAIN_END
#undef MAIN_START
#undef GDALDestroy
#undef OGRCleanupAll
#undef GDALDestroyDriverManager
#undef CSLDestroy
#undef OGRGeneralCmdLineProcessor
#undef GDALGeneralCmdLineProcessor

int RunOgrInfoMain(const ::std::vector<::std::string>& args) {
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
  g_initial_argv = kInitialArgv;

  absl::Cleanup restore_state = [&] {
    if (argv != nullptr && argv != kInitialArgv) {
      ::CSLDestroy(argv);
      argv = nullptr;
    }
    g_initial_argv = nullptr;
    CPLSetConfigOptions(saved_config_options.List());
    CPLSetThreadLocalConfigOptions(saved_tl_config_options.List());
    CPLErrorReset();
  };

  int ret = 0;
  try {
    ret = RunOgrInfoMainImpl(aos.Count(), argv);
  } catch (const TestExitException& e) {
    ret = e.code;
  }
  return ret;
}

}  // namespace autotest2_ogrinfo_bin
