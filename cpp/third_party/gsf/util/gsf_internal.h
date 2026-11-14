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
//
// Safe C++20 prelude for GSF internal headers and non-header symbols used by
// unit tests in //third_party/gsf/google/src.

#ifndef THIRD_PARTY_GSF_GOOGLE_UTIL_GSF_INTERNAL_H_
#define THIRD_PARTY_GSF_GOOGLE_UTIL_GSF_INTERNAL_H_

// clang-format off
// Pre-include system and public GSF headers at C++ linkage before entering
// extern "C" blocks for internal GSF headers.
#include <limits.h>  // IWYU pragma: keep

#include "third_party/gsf/src/gsf.h"       // IWYU pragma: export
#include "third_party/gsf/src/gsf_ft.h"    // IWYU pragma: export
#include "third_party/gsf/src/gsf_indx.h"  // IWYU pragma: export

extern "C" {
#include "third_party/gsf/src/gsf_dec.h"   // IWYU pragma: export
#include "third_party/gsf/src/gsf_enc.h"   // IWYU pragma: export

extern int gsfError;
extern short arraySize[GSF_MAX_OPEN_FILES][GSF_MAX_PING_ARRAY_SUBRECORDS];

int EncodeCompressedUnsignedShortArray(unsigned char *sptr,
                                       const unsigned short *array,
                                       int num_beams, int subrecordID);
int DecodeCompressedUnsignedShortArray(unsigned short **array,
                                       const unsigned char *sptr, int num_beams,
                                       int compressed_size, int subrecordID,
                                       int handle);
int EncodeCompressedArray(unsigned char *sptr, const double *array,
                          int num_beams, const gsfScaleFactors *sf,
                          int subrecordID);
int DecodeCompressedArray(double **array, const unsigned char *sptr,
                          int num_beams, int compressed_size,
                          const gsfScaleFactors *sf, int subrecordID,
                          int handle);
}  // extern "C"
// clang-format on

#endif  // THIRD_PARTY_GSF_GOOGLE_UTIL_GSF_INTERNAL_H_
