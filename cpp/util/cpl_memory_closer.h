// Wrap CPLStrdup to call CPLFree when an instance goes out of scope.
//
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

#ifndef THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_CPL_MEMORY_CLOSER_H_
#define THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_CPL_MEMORY_CLOSER_H_

#include "port/cpl_conv.h"

namespace autotest2 {

// Use CPLFree to release the memory for a GDAL heap object.
// Similar to unique_ptr.
template <typename T>
class CPLMemoryCloser {
 public:
  explicit CPLMemoryCloser(T *data) {
    the_data_ = data;
  }

  ~CPLMemoryCloser() {
    if (the_data_)
      CPLFree(the_data_);
  }

  // Modifying the contents pointed to by the return is allowed.
  T* const get() const {
    return the_data_;
  }

 private:
  T* the_data_;
};

}  // namespace autotest2

#endif  // THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_CPL_MEMORY_CLOSER_H_
