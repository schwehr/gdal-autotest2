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

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "base/logging.h"
#include "third_party/absl/strings/str_cat.h"
#include "autotest2/cpp/util/cpl_cstringlist.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_vsi.h"

constexpr int kSuccess = 0;
constexpr int kFailure = -1;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const char kFilename[] = "/vsimem/a.zip";
  const std::string data2(reinterpret_cast<const char *>(data), size);
  autotest2::VsiMemTempWrapper wrapper(kFilename, data2);

  // Note the double slash: //
  // A single slash between the two vsi paths does not work:
  //   /vsigzip/vsimem/a
  const char kFilenameZip[] = "/vsizip//vsimem/a.zip";

  WithQuietHandler error_handler;

  char **files_csl = VSIReadDirRecursive(kFilenameZip);
  const std::vector<std::string> files = autotest2::CslToVector(files_csl);
  CSLDestroy(files_csl);

  for (const auto &filename : files) {
    std::string filepath = absl::StrCat(kFilenameZip, "/", filename);
    VSIStatBufL stat_buf;
    int result = VSIStatL(filepath.c_str(), &stat_buf);
    CHECK(result == kSuccess || result == kFailure);
  }

  return 0;
}
