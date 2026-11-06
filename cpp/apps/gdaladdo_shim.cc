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

#include "third_party/gdal/autotest2/cpp/apps/gdaladdo_shim.h"

#include <string>
#include <vector>

#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/commonutils.h"
#include "third_party/gdal/apps/gdalargumentparser.h"
#include "third_party/gdal/frmts/vrt/vrt_priv.h"
#include "third_party/gdal/frmts/vrt/vrtdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

#ifdef DO_NOT_DEFINE_GDAL_DATE_NAME
#undef DO_NOT_DEFINE_GDAL_DATE_NAME
#endif
#include "third_party/gdal/gcore/gdal_version_full/gdal_version.h"

namespace autotest2_gdaladdo {

[[noreturn]] inline void exit(int code) { throw TestExitException{code}; }

static inline int WrappedCmdLineProcessor(int argc, char*** pargv,
                                          int nOptions) {
  int nRet = GDALGeneralCmdLineProcessor(argc, pargv, nOptions);
  if (nRet < 1 && *pargv != nullptr) {
    CSLDestroy(*pargv);
    *pargv = nullptr;
  }
  return nRet;
}

#ifdef MAIN_START
#undef MAIN_START
#endif
#ifdef MAIN_END
#undef MAIN_END
#endif

// apps/gdaladdo.cpp calls both unqualified `exit(...)` and `std::exit(1)`.
// Defining nested `autotest2_gdaladdo::std` (not reopening `::std` and
// without `#define std`) lets `std::exit(1)` resolve via enclosing-namespace
// lookup to `autotest2_gdaladdo::std::exit` while other `std::*` names fall
// through `using namespace ::std;`.
namespace std {
using namespace ::std;  // NOLINT(build/namespaces)
[[noreturn]] inline void exit(int code) {
  throw ::autotest2_gdaladdo::TestExitException{code};
}
}  // namespace std

#define GDALGeneralCmdLineProcessor WrappedCmdLineProcessor
#define GDALDestroyDriverManager() (void)0
#define MAIN_START(argc, argv) int RunGdalAddoMainImpl(int argc, char** argv)
#define MAIN_END

#include "third_party/gdal/apps/gdaladdo.cpp"

#undef MAIN_END
#undef MAIN_START
#undef GDALDestroyDriverManager
#undef GDALGeneralCmdLineProcessor

int RunGdalAddoMain(const ::std::vector<::std::string>& args) {
  aoErrors.clear();
  CPLErrorReset();
  const CPLStringList saved_config_options(CPLGetConfigOptions(),
                                           /*bTakeOwnership=*/TRUE);
  const CPLStringList saved_tl_config_options(CPLGetThreadLocalConfigOptions(),
                                              /*bTakeOwnership=*/TRUE);
  absl::Cleanup restore_state = [&] {
    CPLSetConfigOptions(saved_config_options.List());
    CPLSetThreadLocalConfigOptions(saved_tl_config_options.List());
    aoErrors.clear();
    CPLErrorReset();
  };

  CPLStringList aos;
  for (const auto& arg : args) {
    aos.AddString(arg.c_str());
  }

  int ret = 0;
  try {
    ret = RunGdalAddoMainImpl(aos.Count(), aos.List());
  } catch (const TestExitException& e) {
    ret = e.code;
  }
  return ret;
}

}  // namespace autotest2_gdaladdo
