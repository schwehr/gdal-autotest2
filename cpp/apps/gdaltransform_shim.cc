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

#include "third_party/gdal/autotest2/cpp/apps/gdaltransform_shim.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/alg/gdalwarper.h"
#include "third_party/gdal/apps/commonutils.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

#ifdef DO_NOT_DEFINE_GDAL_DATE_NAME
#undef DO_NOT_DEFINE_GDAL_DATE_NAME
#endif
#include "third_party/gdal/gcore/gdal_version_full/gdal_version.h"

namespace autotest2_gdaltransform {

[[noreturn]] inline void exit(int code) { throw TestExitException{code}; }

static inline void TrackingCSLDestroy(char**& papsz) {
  ::CSLDestroy(papsz);
  papsz = nullptr;
}

static std::vector<GDALDatasetH>& OpenDatasets() {
  static auto* const v = new std::vector<GDALDatasetH>();
  return *v;
}
static GDAL_GCP* g_pasGCPs = nullptr;
static int g_nGCPCount = 0;

static inline GDALDatasetH TrackingGDALOpen(const char* pszFilename,
                                            GDALAccess eAccess) {
  GDALDatasetH ds = ::GDALOpen(pszFilename, eAccess);
  if (ds != nullptr) {
    OpenDatasets().push_back(ds);
  }
  return ds;
}

static inline void TrackingGDALClose(GDALDatasetH ds) {
  auto& datasets = OpenDatasets();
  auto it = std::find(datasets.begin(), datasets.end(), ds);
  if (it != datasets.end()) {
    datasets.erase(it);
  }
  ::GDALClose(ds);
}

static inline void* TrackingCPLRealloc(void* ptr, size_t size) {
  void* res = ::VSIRealloc(ptr, size);
  if ((ptr == nullptr || ptr == g_pasGCPs) && size % sizeof(GDAL_GCP) == 0) {
    g_pasGCPs = static_cast<GDAL_GCP*>(res);
    g_nGCPCount = static_cast<int>(size / sizeof(GDAL_GCP));
  }
  return res;
}

static inline void TrackingCPLFree(void* ptr) {
  if (ptr == g_pasGCPs) {
    g_pasGCPs = nullptr;
    g_nGCPCount = 0;
  }
  ::VSIFree(ptr);
}

static inline void CleanupTrackedResources() {
  if (g_pasGCPs != nullptr) {
    ::GDALDeinitGCPs(g_nGCPCount, g_pasGCPs);
    ::VSIFree(g_pasGCPs);
    g_pasGCPs = nullptr;
    g_nGCPCount = 0;
  }
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
#ifdef CPLRealloc
#undef CPLRealloc
#endif
#ifdef CPLFree
#undef CPLFree
#endif

#define exit ::autotest2_gdaltransform::exit
#define CSLDestroy(x) ::autotest2_gdaltransform::TrackingCSLDestroy(x)
#define GDALDestroyDriverManager() (void)0
#define GDALOpen(file, access) \
  ::autotest2_gdaltransform::TrackingGDALOpen(file, access)
#define GDALClose(ds) ::autotest2_gdaltransform::TrackingGDALClose(ds)
#define CPLRealloc(ptr, size) \
  ::autotest2_gdaltransform::TrackingCPLRealloc(ptr, size)
#define CPLFree(ptr) ::autotest2_gdaltransform::TrackingCPLFree(ptr)
#define MAIN_START(argc, argv) \
  int RunGdalTransformMainImpl(int argc, char**& argv)
#define MAIN_END

#include "third_party/gdal/apps/gdaltransform.cpp"

#undef MAIN_END
#undef MAIN_START
#undef CPLFree
#undef CPLRealloc
#undef GDALClose
#undef GDALOpen
#undef GDALDestroyDriverManager
#undef CSLDestroy
#undef exit
#define CPLRealloc VSIRealloc
#define CPLFree VSIFree

int RunGdalTransformMain(const std::vector<std::string>& args) {
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
    CleanupTrackedResources();
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
    ret = RunGdalTransformMainImpl(aos.Count(), argv);
  } catch (const TestExitException& e) {
    ret = e.code;
  }
  return ret;
}

}  // namespace autotest2_gdaltransform
