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

// Unit tests for //third_party/netcdf/libdispatch/dglobal.c.
//
// Covers NC_getglobalstate(), NC_createglobalstate() environment variable
// handling and chunk-cache defaults, NC_clearawsparams(), and
// NC_freeglobalstate() cleanup in isolated child processes.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dglobal.c

#include <cstdlib>
#include <cstring>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/libdispatch/dglobal_shim.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

extern "C" {
#include "third_party/netcdf/include/ncglobal.h"
#include "third_party/netcdf/include/nclist.h"
#include "third_party/netcdf/include/ncrc.h"
int nc_initialize(void);
int nc_finalize(void);
}  // extern "C"

namespace netcdf {
namespace {

TEST(DglobalTest, FreeBeforeCreateIsNoOpAndCreateInitializesDefaults) {
  RunInExecveChild([]() -> bool {
    ::unsetenv(NCRCENVIGNORE);
    ::unsetenv(NCRCENVRC);

    // Calling NC_freeglobalstate() before any allocation must be a safe no-op.
    NC_freeglobalstate();

    NCglobalstate* gs1 = NC_getglobalstate();
    if (gs1 == nullptr || gs1->rcinfo == nullptr ||
        gs1->rcinfo->entries == nullptr || gs1->rcinfo->s3profiles == nullptr) {
      return false;
    }
    if (gs1->rcinfo->ignore != 0 || gs1->rcinfo->rcfile != nullptr) {
      return false;
    }
    if (gs1->chunkcache.size != DEFAULT_CHUNK_CACHE_SIZE ||
        gs1->chunkcache.nelems != DEFAULT_CHUNKS_IN_CACHE ||
        gs1->chunkcache.preemption !=
            static_cast<float>(DEFAULT_CHUNK_CACHE_PREEMPTION)) {
      return false;
    }

    // Second call returns the exact same singleton instance.
    NCglobalstate* gs2 = NC_getglobalstate();
    if (gs2 != gs1) {
      return false;
    }

    NC_freeglobalstate();
    // Repeated free after clearing must also be a safe no-op.
    NC_freeglobalstate();
    return true;
  });
}

TEST(DglobalTest, EnvironmentVariablesControlRcIgnoreAndRcfile) {
  RunInExecveChild([]() -> bool {
    // Case 1: NCRCENV_IGNORE set and NCRCENV_RC empty (ignored due to len 0).
    if (::setenv(NCRCENVIGNORE, "1", 1) != 0 ||
        ::setenv(NCRCENVRC, "", 1) != 0) {
      return false;
    }
    NCglobalstate* gs1 = NC_getglobalstate();
    if (gs1 == nullptr || gs1->rcinfo == nullptr) {
      return false;
    }
    if (gs1->rcinfo->ignore != 1 || gs1->rcinfo->rcfile != nullptr) {
      return false;
    }
    NC_freeglobalstate();

    // Case 2: Re-create after free with non-empty NCRCENV_RC path.
    ::unsetenv(NCRCENVIGNORE);
    constexpr char kCustomRcPath[] = "/nonexistent/test_netcdf_rc";
    if (::setenv(NCRCENVRC, kCustomRcPath, 1) != 0) {
      return false;
    }
    NCglobalstate* gs2 = NC_getglobalstate();
    if (gs2 == nullptr || gs2->rcinfo == nullptr || gs2->rcinfo->ignore != 0 ||
        gs2->rcinfo->rcfile == nullptr) {
      return false;
    }
    const bool rcfile_matches =
        std::strcmp(gs2->rcinfo->rcfile, kCustomRcPath) == 0;
    NC_freeglobalstate();
    return rcfile_matches;
  });
}

TEST(DglobalTest, ClearAwsParamsFreesAllFieldsAndResetsStruct) {
  RunInExecveChild([]() -> bool {
    ::setenv(NCRCENVIGNORE, "1", 1);
    NCglobalstate* gs = NC_getglobalstate();
    if (gs == nullptr) {
      return false;
    }

    // Populate all AWS fields, verify values, then clear everything.
    dglobal_shim_populate_aws(gs, "us-west-2", "/nonexistent/aws_cfg", "dev",
                              "AKIA123", "secret456");
    if (dglobal_shim_aws_is_cleared(gs) != 0) {
      return false;
    }
    if (std::strcmp(dglobal_shim_aws_default_region(gs), "us-west-2") != 0 ||
        std::strcmp(dglobal_shim_aws_config_file(gs), "/nonexistent/aws_cfg") !=
            0 ||
        std::strcmp(dglobal_shim_aws_profile(gs), "dev") != 0 ||
        std::strcmp(dglobal_shim_aws_access_key_id(gs), "AKIA123") != 0 ||
        std::strcmp(dglobal_shim_aws_secret_access_key(gs), "secret456") != 0) {
      return false;
    }

    dglobal_shim_clear_aws(gs);
    if (dglobal_shim_aws_is_cleared(gs) != 1) {
      return false;
    }

    // Populate only a subset of fields and clear again (nullfree NULL-safety).
    dglobal_shim_populate_aws(gs, "eu-central-1", nullptr, nullptr, nullptr,
                              "only_secret");
    if (std::strcmp(dglobal_shim_aws_default_region(gs), "eu-central-1") != 0 ||
        dglobal_shim_aws_config_file(gs) != nullptr ||
        dglobal_shim_aws_profile(gs) != nullptr ||
        dglobal_shim_aws_access_key_id(gs) != nullptr ||
        std::strcmp(dglobal_shim_aws_secret_access_key(gs), "only_secret") !=
            0) {
      return false;
    }
    dglobal_shim_clear_aws(gs);
    if (dglobal_shim_aws_is_cleared(gs) != 1) {
      return false;
    }

    NC_freeglobalstate();
    return true;
  });
}

TEST(DglobalTest, FreeGlobalStateCleansUpInitializedStateAndCustomFields) {
  RunInExecveChild([]() -> bool {
    ::setenv(NCRCENVIGNORE, "1", 1);
    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    NCglobalstate* gs = NC_getglobalstate();
    if (gs == nullptr || gs->tempdir == nullptr || gs->home == nullptr ||
        gs->cwd == nullptr) {
      return false;
    }

    // Populate optional owned fields so NC_freeglobalstate() frees them all.
    dglobal_shim_populate_aws(gs, "us-east-1", "/nonexistent/cfg", "default",
                              "key", "secret");
    if (gs->pluginpaths == nullptr) {
      gs->pluginpaths = nclistnew();
    }

    if (nc_finalize() != NC_NOERR) {
      return false;
    }
    // Re-creating after nc_finalize() returns a fresh, cleared global state.
    NCglobalstate* fresh = NC_getglobalstate();
    if (fresh == nullptr || dglobal_shim_aws_is_cleared(fresh) != 1) {
      return false;
    }
    NC_freeglobalstate();
    return true;
  });
}

}  // namespace
}  // namespace netcdf
