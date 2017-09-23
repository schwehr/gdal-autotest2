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

#ifndef THIRD_PARTY_GDAL_AUTOTEST2_CPP_FUZZERS_OGR_H_
#define THIRD_PARTY_GDAL_AUTOTEST2_CPP_FUZZERS_OGR_H_

class GDALDataset;

namespace autotest2 {

// Excecise an open vector dataset through the fuzzer.
// Problems are detected by the program crashing or leaks being detected by the
// fuzzing architecture.
void OGRFuzzOneInput(GDALDataset *dataset);

}  // namespace autotest2

#endif  // THIRD_PARTY_GDAL_AUTOTEST2_CPP_FUZZERS_OGR_H_
