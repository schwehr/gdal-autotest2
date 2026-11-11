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

#include "third_party/netcdf/google/libhdf5/hdf5set_format_compatibility_shim.h"

#include "third_party/netcdf/config.h"  // NOLINT
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/hdf5internal.h"

static void EnsureHdf5Initialized(void) {
  if (!nc4_hdf5_initialized) {
    nc4_hdf5_initialize();
  }
}

int NetcdfTestSetFormatCompatibilityValidFapl(int* low_is_v18,
                                              int* high_is_latest) {
  EnsureHdf5Initialized();
  if (low_is_v18) {
    *low_is_v18 = 0;
  }
  if (high_is_latest) {
    *high_is_latest = 0;
  }

  hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
  if (fapl < 0) {
    return NC_EHDFERR;
  }

  int status = hdf5set_format_compatibility(fapl);
  if (status == NC_NOERR) {
    H5F_libver_t low = H5F_LIBVER_ERROR;
    H5F_libver_t high = H5F_LIBVER_ERROR;
    if (H5Pget_libver_bounds(fapl, &low, &high) < 0) {
      H5Pclose(fapl);
      return NC_EHDFERR;
    }
    if (low_is_v18) {
      *low_is_v18 = (low == H5F_LIBVER_V18) ? 1 : 0;
    }
    if (high_is_latest) {
      *high_is_latest = (high == H5F_LIBVER_LATEST) ? 1 : 0;
    }
  }

  if (H5Pclose(fapl) < 0 && status == NC_NOERR) {
    return NC_EHDFERR;
  }
  return status;
}

int NetcdfTestSetFormatCompatibilityInvalidHid(void) {
  EnsureHdf5Initialized();
  return hdf5set_format_compatibility((hid_t)-1);
}

int NetcdfTestSetFormatCompatibilityDefaultPlist(void) {
  EnsureHdf5Initialized();
  return hdf5set_format_compatibility(H5P_DEFAULT);
}

int NetcdfTestSetFormatCompatibilityWrongPlistClass(void) {
  EnsureHdf5Initialized();
  hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
  if (dcpl < 0) {
    return NC_EHDFERR;
  }
  int status = hdf5set_format_compatibility(dcpl);
  if (H5Pclose(dcpl) < 0 && status == NC_NOERR) {
    return NC_EHDFERR;
  }
  return status;
}
