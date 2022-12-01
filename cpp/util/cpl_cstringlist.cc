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

#include "autotest2/cpp/util/cpl_cstringlist.h"

#include <stddef.h>
#include <string>
#include <vector>

#include "port/cpl_conv.h"

namespace autotest2 {

std::vector<std::string> CslToVector(const char *const *string_list) {
  if (string_list == nullptr) return {};
  std::vector<std::string> result;
  for (; *string_list != NULL; ++string_list) {
    result.push_back(*string_list);
  }
  return result;
}

char **VectorToCsl(const std::vector<std::string> &vs) {
  if (vs.empty()) return nullptr;

  // Build a nullptr terminated list of C strings.
  // Calloc ensures the last entry is a nullptr.
  char **new_list =
      static_cast<char **>(CPLCalloc(1, (vs.size() + 1) * sizeof(char *)));

  for (int i = 0; i < vs.size(); i++) {
    new_list[i] = CPLStrdup(vs[i].c_str());
  }

  return new_list;
}

}  // namespace autotest2
