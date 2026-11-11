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

#include "third_party/netcdf/google/libhdf5/nc4mem_shim.h"

#include <string.h>

#include "third_party/netcdf/config.h"
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/nc4internal.h"
#include "third_party/netcdf/include/hdf5internal.h"

extern int NC4_open_image_file(NC_FILE_INFO_T* h5);
extern int NC4_create_image_file(NC_FILE_INFO_T* h5, size_t initialsz);

int Nc4MemShimOpenImageInvalidArgs(int null_memory, int zero_size) {
  unsigned char dummy[8] = {0};
  NC_FILE_INFO_T h5;
  NC_HDF5_FILE_INFO_T hdf5_file;

  memset(&h5, 0, sizeof(h5));
  memset(&hdf5_file, 0, sizeof(hdf5_file));
  h5.format_file_info = &hdf5_file;
  h5.mem.memio.memory = null_memory ? NULL : dummy;
  h5.mem.memio.size = zero_size ? 0 : sizeof(dummy);
  return NC4_open_image_file(&h5);
}

int Nc4MemShimCreateImageInvalidSize(void) {
  unsigned char dummy[1] = {0};
  NC_FILE_INFO_T h5;
  NC_HDF5_FILE_INFO_T hdf5_file;
  H5E_auto2_t prev_func = NULL;
  void* prev_data = NULL;
  int stat = NC_NOERR;

  H5Eget_auto2(H5E_DEFAULT, &prev_func, &prev_data);
  H5Eset_auto2(H5E_DEFAULT, NULL, NULL);

  memset(&h5, 0, sizeof(h5));
  memset(&hdf5_file, 0, sizeof(hdf5_file));
  h5.format_file_info = &hdf5_file;
  h5.mem.memio.memory = dummy;
  h5.mem.memio.size = 0;

  stat = NC4_create_image_file(&h5, 0);
  NC4_image_finalize(h5.mem.udata);

  H5Eset_auto2(H5E_DEFAULT, prev_func, prev_data);
  return stat;
}
