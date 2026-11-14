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

#include "third_party/gdal/autotest2/cpp/apps/gdalwarp_bin_shim.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/commonutils.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_error_internal.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

#ifdef DO_NOT_DEFINE_GDAL_DATE_NAME
#undef DO_NOT_DEFINE_GDAL_DATE_NAME
#endif
#include "third_party/gdal/gcore/gdal_version_full/gdal_version.h"

namespace autotest2_gdalwarp_bin {

[[noreturn]] inline void exit(int code) { throw TestExitException{code}; }

// Defining nested `autotest2_gdalwarp_bin::std` (not reopening `::std` and
// without `#define std` or `#define exit`) intercepts `std::exit(...)` via
// enclosing-namespace lookup while keeping other `std::*` symbols intact.
namespace std {
using namespace ::std;  // NOLINT(build/namespaces)
[[noreturn]] inline void exit(int code) {
  throw ::autotest2_gdalwarp_bin::TestExitException{code};
}
}  // namespace std

static char** g_initial_argv = nullptr;
static GDALWarpAppOptions* g_psOptions = nullptr;
static void* g_pahSrcDS = nullptr;

static ::std::vector<GDALDatasetH>& OpenDatasets() {
  static auto* const v = new ::std::vector<GDALDatasetH>();
  return *v;
}

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

static inline void TrackingCSLDestroy(char**& papsz) {
  if (papsz != nullptr && papsz != g_initial_argv) {
    ::CSLDestroy(papsz);
  }
  papsz = nullptr;
}

static inline GDALWarpAppOptions* TrackingGDALWarpAppOptionsNew(
    char** papszArgv, GDALWarpAppOptionsForBinary* psOptionsForBinary) {
  g_psOptions = ::GDALWarpAppOptionsNew(papszArgv, psOptionsForBinary);
  return g_psOptions;
}

static inline void TrackingGDALWarpAppOptionsFree(
    GDALWarpAppOptions* psOptions) {
  if (psOptions == g_psOptions) {
    g_psOptions = nullptr;
  }
  ::GDALWarpAppOptionsFree(psOptions);
}

static inline GDALDatasetH TrackingGDALOpenEx(
    const char* pszFilename, unsigned int nOpenFlags,
    const char* const* papszAllowedDrivers, const char* const* papszOpenOptions,
    const char* const* papszSiblingFiles) {
  GDALDatasetH ds = ::GDALOpenEx(pszFilename, nOpenFlags, papszAllowedDrivers,
                                 papszOpenOptions, papszSiblingFiles);
  if (ds != nullptr) {
    OpenDatasets().push_back(ds);
  }
  return ds;
}

static inline GDALDatasetH TrackingGDALOpen(const char* pszFilename,
                                            GDALAccess eAccess) {
  GDALDatasetH ds = ::GDALOpen(pszFilename, eAccess);
  if (ds != nullptr) {
    OpenDatasets().push_back(ds);
  }
  return ds;
}

static inline GDALDatasetH TrackingGDALWarp(const char* pszDest,
                                            GDALDatasetH hDstDS, int nSrcCount,
                                            GDALDatasetH* pahSrcDS,
                                            const GDALWarpAppOptions* psOptions,
                                            int* pbUsageError) {
  GDALDatasetH ds =
      ::GDALWarp(pszDest, hDstDS, nSrcCount, pahSrcDS, psOptions, pbUsageError);
  if (ds != nullptr && ds != hDstDS) {
    OpenDatasets().push_back(ds);
  }
  return ds;
}

static inline CPLErr TrackingGDALClose(GDALDatasetH ds) {
  auto& datasets = OpenDatasets();
  auto it = ::std::find(datasets.begin(), datasets.end(), ds);
  if (it != datasets.end()) {
    datasets.erase(it);
  }
  return ::GDALClose(ds);
}

static inline void* TrackingCPLRealloc(void* ptr, size_t size) {
  void* res = ::CPLRealloc(ptr, size);
  if (ptr == nullptr || ptr == g_pahSrcDS) {
    g_pahSrcDS = res;
  }
  return res;
}

static inline void TrackingCPLFree(void* ptr) {
  if (ptr == g_pahSrcDS) {
    g_pahSrcDS = nullptr;
  }
  ::VSIFree(ptr);
}

static inline void CleanupTrackedResources() {
  auto& datasets = OpenDatasets();
  for (auto it = datasets.rbegin(); it != datasets.rend(); ++it) {
    ::GDALClose(*it);
  }
  datasets.clear();
  if (g_pahSrcDS != nullptr) {
    ::VSIFree(g_pahSrcDS);
    g_pahSrcDS = nullptr;
  }
  if (g_psOptions != nullptr) {
    ::GDALWarpAppOptionsFree(g_psOptions);
    g_psOptions = nullptr;
  }
}

#ifdef MAIN_START
#undef MAIN_START
#endif
#ifdef MAIN_END
#undef MAIN_END
#endif
#ifdef CPLFree
#undef CPLFree
#endif

#define GDALGeneralCmdLineProcessor WrappedCmdLineProcessor
#define CSLDestroy(x) ::autotest2_gdalwarp_bin::TrackingCSLDestroy(x)
#define GDALWarpAppOptionsNew TrackingGDALWarpAppOptionsNew
#define GDALWarpAppOptionsFree TrackingGDALWarpAppOptionsFree
#define GDALOpenEx TrackingGDALOpenEx
#define GDALOpen TrackingGDALOpen
#define GDALWarp TrackingGDALWarp
#define GDALClose TrackingGDALClose
#define CPLRealloc TrackingCPLRealloc
#define CPLFree TrackingCPLFree
#define GDALDestroyDriverManager() (void)0
#define OGRCleanupAll() (void)0
#define MAIN_START(argc, argv) int RunGdalWarpMainImpl(int argc, char**& argv)
#define MAIN_END

#include "third_party/gdal/apps/gdalwarp_bin.cpp"

#undef MAIN_END
#undef MAIN_START
#undef OGRCleanupAll
#undef GDALDestroyDriverManager
#undef CPLFree
#undef CPLRealloc
#undef GDALClose
#undef GDALWarp
#undef GDALOpen
#undef GDALOpenEx
#undef GDALWarpAppOptionsFree
#undef GDALWarpAppOptionsNew
#undef CSLDestroy
#undef GDALGeneralCmdLineProcessor
#define CPLFree VSIFree

int RunGdalWarpMain(const ::std::vector<::std::string>& args) {
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
    CleanupTrackedResources();
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
    ret = RunGdalWarpMainImpl(aos.Count(), argv);
  } catch (const TestExitException& e) {
    ret = e.code;
  }
  return ret;
}

}  // namespace autotest2_gdalwarp_bin
