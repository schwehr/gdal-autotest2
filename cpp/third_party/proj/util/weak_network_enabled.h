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
// Force-included (-include) by //third_party/proj4:proj4_unbundled so that the
// google3 AUTOPATCHES stub definition of proj_context_is_network_enabled() in
// proj/src/networkfilemanager.cpp is emitted as a weak symbol. This allows
// filemanager_test and networkfilemanager_test to supply a strong in-process
// override for testing PROJ's network-callback plumbing without modifying
// proj/src/networkfilemanager.cpp or production //third_party/proj4:proj4.

#ifndef THIRD_PARTY_PROJ4_GOOGLE_UTIL_WEAK_NETWORK_ENABLED_H_
#define THIRD_PARTY_PROJ4_GOOGLE_UTIL_WEAK_NETWORK_ENABLED_H_

#ifdef __cplusplus
extern "C" {
#endif

struct pj_ctx;
__attribute__((weak)) int proj_context_is_network_enabled(struct pj_ctx* ctx);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // THIRD_PARTY_PROJ4_GOOGLE_UTIL_WEAK_NETWORK_ENABLED_H_
