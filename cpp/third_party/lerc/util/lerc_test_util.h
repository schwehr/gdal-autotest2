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

#ifndef THIRD_PARTY_LERC_GOOGLE_UTIL_LERC_TEST_UTIL_H_
#define THIRD_PARTY_LERC_GOOGLE_UTIL_LERC_TEST_UTIL_H_

#include <cstdlib>
#include <memory>

namespace lerc {

// Deleter for malloc/calloc buffers allocated inside LercLib (such as
// fpl_EsriHuffman, fpl_Compression, and fpl_Lerc2Ext).
struct FreeDeleter {
  void operator()(void* ptr) const noexcept { std::free(ptr); }
};

using OwnedMallocBytes = std::unique_ptr<unsigned char, FreeDeleter>;
using OwnedMallocChars = std::unique_ptr<char, FreeDeleter>;

}  // namespace lerc

#endif  // THIRD_PARTY_LERC_GOOGLE_UTIL_LERC_TEST_UTIL_H_
