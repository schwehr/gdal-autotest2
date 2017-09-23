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
//
// Test the basics of the GDAL raster fuzzer.
//
// These tests are very weak.  It would be possible to have GDALFuzzOneInput
// return an enum that indicated where is exists the fuzzer, but that would
// only be used for testing.
//
// See also:
//   https://trac.osgeo.org/gdal/browser/trunk/gdal/fuzzers

#include "autotest2/cpp/fuzzers/gdal.h"

#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/fuzzers/gdal.h"
#include "autotest2/cpp/util/vsimem.h"
#include "frmts/aaigrid/aaigriddataset.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_conv.h"

namespace autotest2 {
namespace {

TEST(GdalFuzzTest, Empty) {
  // Should not die.
  GDALFuzzOneInput(nullptr);
}

TEST(GdalFuzzTest, BasicLayer) {
  constexpr char kGrid[] =
      "ncols        1\n"
      "nrows        1\n"
      "xllcorner    440720.0\n"
      "yllcorner    3750120.0\n"
      "cellsize     60.0\n"
      "    107\n";

  const char kFilename[] = "/vsimem/a.asc";
  const string data2(reinterpret_cast<const char *>(kGrid));
  autotest2::VsiMemTempWrapper wrapper(kFilename, data2);

  auto open_info =
      gtl::MakeUnique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);
  auto dataset = absl::WrapUnique(AAIGDataset::Open(open_info.get()));
  ASSERT_NE(nullptr, dataset);

  GDALFuzzOneInput(dataset.get());
}

}  // namespace
}  // namespace autotest2
