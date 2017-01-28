// Copyright 2014 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_CPL_MEMORY_H_
#define THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_CPL_MEMORY_H_

#include <cstdlib>
#include "port/cpl_conv.h"

namespace autotest2 {

// Function object that calls GDAL's CPLFree on its parameter.
struct CplFreeDeleter {
  void operator()(void* ptr) { CPLFree(ptr); }
};

// Function object that calls the C free on its parameter.
struct FreeDeleter {
  void operator()(void* ptr) { std::free(ptr); }
};

}  // namespace autotest2

#endif  // THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_CPL_MEMORY_H_
