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

#include "port/cpl_minixml.h"

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "port/cpl_conv.h"
#include "autotest2/cpp/util/cpl_memory_closer.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  // Ensure there is a NUL at the end of the data passed to CPLParseXMLString.
  const string s(reinterpret_cast<const char *>(data), size);

  CPLXMLTreeCloser r(CPLParseXMLString(s.c_str()));
  autotest2::CPLMemoryCloser<char> tree(CPLSerializeXMLTree(r.get()));

  return 0;
}
