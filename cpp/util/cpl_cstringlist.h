// Copyright 2017 Google Inc. All Rights Reserved.
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

#ifndef THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_CPL_CSTRINGLIST_H_
#define THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_CPL_CSTRINGLIST_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace autotest2 {

typedef std::unique_ptr<char *, std::function<void(char **)>> StringListPtr;

// C String List (CSL) helpers.

std::vector<std::string> CslToVector(const char *const *string_list);

// Caller must delete the returned result with CSLDestroy.
char **VectorToCsl(const std::vector<std::string> &vs);

}  // namespace autotest2

#endif  // THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_CPL_CSTRINGLIST_H_
