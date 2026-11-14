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

#include "third_party/gdal/autotest2/cpp/apps/gdalinfo_bin_shim.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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
#include "third_party/gdal/port/cpl_multiproc.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

#ifdef DO_NOT_DEFINE_GDAL_DATE_NAME
#undef DO_NOT_DEFINE_GDAL_DATE_NAME
#endif
#include "third_party/gdal/gcore/gdal_version_full/gdal_version.h"

namespace autotest2_gdalinfo_bin {

[[noreturn]] inline void exit(int code) { throw TestExitException{code}; }

static inline int WrappedCmdLineProcessor(int argc, char*** pargv,
                                          int nOptions) {
  char** orig_argv = *pargv;
  int nRet = GDALGeneralCmdLineProcessor(argc, pargv, nOptions);
  if (nRet < 1 && *pargv != nullptr && *pargv != orig_argv) {
    CSLDestroy(*pargv);
    *pargv = nullptr;
  }
  return nRet;
}

static std::vector<GDALDatasetH>& OpenDatasets() {
  static auto* const v = new std::vector<GDALDatasetH>();
  return *v;
}

static inline void TrackDataset(GDALDatasetH ds) {
  if (ds == nullptr) return;
  auto& datasets = OpenDatasets();
  if (std::find(datasets.begin(), datasets.end(), ds) == datasets.end()) {
    datasets.push_back(ds);
  }
}

static inline GDALDatasetH TrackingGDALOpenEx(const char* pszFilename,
                                              unsigned int nOpenFlags,
                                              CSLConstList papszAllowedDrivers,
                                              CSLConstList papszOpenOptions,
                                              CSLConstList papszSiblingFiles) {
  GDALDatasetH ds = ::GDALOpenEx(pszFilename, nOpenFlags, papszAllowedDrivers,
                                 papszOpenOptions, papszSiblingFiles);
  TrackDataset(ds);
  return ds;
}

static inline GDALDatasetH TrackingGDALOpen(const char* pszFilename,
                                            GDALAccess eAccess) {
  GDALDatasetH ds = ::GDALOpen(pszFilename, eAccess);
  TrackDataset(ds);
  return ds;
}

static inline CPLErr TrackingGDALClose(GDALDatasetH ds) {
  if (ds != nullptr) {
    auto& datasets = OpenDatasets();
    std::erase(datasets, ds);
  }
  return ::GDALClose(ds);
}

static inline void CleanupTrackedResources() {
  auto& datasets = OpenDatasets();
  for (GDALDatasetH ds : datasets) {
    ::GDALClose(ds);
  }
  datasets.clear();
}

#ifdef MAIN_START
#undef MAIN_START
#endif
#ifdef MAIN_END
#undef MAIN_END
#endif

namespace std {
using namespace ::std;  // NOLINT(build/namespaces)
[[noreturn]] inline void exit(int code) {
  throw ::autotest2_gdalinfo_bin::TestExitException{code};
}
}  // namespace std

#define GDALGeneralCmdLineProcessor WrappedCmdLineProcessor
#define GDALDestroyDriverManager() (void)0
#define OGRCleanupAll() (void)0
#define GDALOpenEx TrackingGDALOpenEx
#define GDALOpen TrackingGDALOpen
#define GDALClose TrackingGDALClose
#define MAIN_START(argc, argv) int RunGdalInfoMainImpl(int argc, char** argv)
#define MAIN_END

#include "third_party/gdal/apps/gdalinfo_bin.cpp"

#undef MAIN_END
#undef MAIN_START
#undef GDALClose
#undef GDALOpen
#undef GDALOpenEx
#undef OGRCleanupAll
#undef GDALDestroyDriverManager
#undef GDALGeneralCmdLineProcessor

int RunGdalInfoMain(const ::std::vector<::std::string>& args) {
  CPLErrorReset();
  const CPLStringList saved_config_options(CPLGetConfigOptions(),
                                           /*bTakeOwnership=*/TRUE);
  const CPLStringList saved_tl_config_options(CPLGetThreadLocalConfigOptions(),
                                              /*bTakeOwnership=*/TRUE);
  absl::Cleanup restore_state = [&] {
    CleanupTrackedResources();
    CPLSetConfigOptions(saved_config_options.List());
    CPLSetThreadLocalConfigOptions(saved_tl_config_options.List());
    CPLErrorReset();
  };

  CPLStringList aos;
  for (const auto& arg : args) {
    aos.AddString(arg.c_str());
  }

  int ret = 0;
  try {
    ret = RunGdalInfoMainImpl(aos.Count(), aos.List());
  } catch (const TestExitException& e) {
    ret = e.code;
  }
  return ret;
}

}  // namespace autotest2_gdalinfo_bin
