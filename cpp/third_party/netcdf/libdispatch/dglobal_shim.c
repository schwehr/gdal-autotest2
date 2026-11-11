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

#include "third_party/netcdf/google/libdispatch/dglobal_shim.h"

#include "third_party/netcdf/config.h"
#include <stdlib.h>
#include <string.h>

#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/ncglobal.h"

static char* dup_or_null(const char* s) {
  if (s == NULL) {
    return NULL;
  }
  size_t len = strlen(s) + 1;
  char* copy = (char*)malloc(len);
  if (copy != NULL) {
    memcpy(copy, s, len);
  }
  return copy;
}

void dglobal_shim_populate_aws(struct NCglobalstate* gs,
                               const char* default_region,
                               const char* config_file,
                               const char* profile,
                               const char* access_key_id,
                               const char* secret_access_key) {
  if (gs == NULL) {
    return;
  }
  free(gs->aws.default_region);
  free(gs->aws.config_file);
  free(gs->aws.profile);
  free(gs->aws.access_key_id);
  free(gs->aws.secret_access_key);

  gs->aws.default_region = dup_or_null(default_region);
  gs->aws.config_file = dup_or_null(config_file);
  gs->aws.profile = dup_or_null(profile);
  gs->aws.access_key_id = dup_or_null(access_key_id);
  gs->aws.secret_access_key = dup_or_null(secret_access_key);
}

void dglobal_shim_clear_aws(struct NCglobalstate* gs) {
  if (gs == NULL) {
    return;
  }
  NC_clearawsparams(&gs->aws);
}

int dglobal_shim_aws_is_cleared(const struct NCglobalstate* gs) {
  if (gs == NULL) {
    return 0;
  }
  return gs->aws.default_region == NULL && gs->aws.config_file == NULL &&
         gs->aws.profile == NULL && gs->aws.access_key_id == NULL &&
         gs->aws.secret_access_key == NULL;
}

const char* dglobal_shim_aws_default_region(const struct NCglobalstate* gs) {
  return gs != NULL ? gs->aws.default_region : NULL;
}

const char* dglobal_shim_aws_config_file(const struct NCglobalstate* gs) {
  return gs != NULL ? gs->aws.config_file : NULL;
}

const char* dglobal_shim_aws_profile(const struct NCglobalstate* gs) {
  return gs != NULL ? gs->aws.profile : NULL;
}

const char* dglobal_shim_aws_access_key_id(const struct NCglobalstate* gs) {
  return gs != NULL ? gs->aws.access_key_id : NULL;
}

const char* dglobal_shim_aws_secret_access_key(const struct NCglobalstate* gs) {
  return gs != NULL ? gs->aws.secret_access_key : NULL;
}
