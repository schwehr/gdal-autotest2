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

#include "logging.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_vsi.h"

constexpr int kSuccess = 0;
constexpr int kFailure = -1;

class CplVsiLFileCloser {
 public:
  CplVsiLFileCloser(VSILFILE *file) : file_(CHECK_NOTNULL(file)) {}
  ~CplVsiLFileCloser() { CHECK_EQ(kSuccess, VSIFCloseL(file_)); }

 private:
  VSILFILE *file_;
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const char kFilename[] = "/vsimem/a";
  // Note the double slash: //
  // A single slash between the two vsi paths does not work:
  //   /vsigzip/vsimem/a
  const char kFilenameGzip[] = "/vsigzip//vsimem/a";
  const string data2(reinterpret_cast<const char *>(data), size);
  autotest2::VsiMemTempWrapper wrapper(kFilename, data2);

  WithQuietHandler error_handler;

  constexpr int kBufSize = 10000;

  // Read uncompressed.
  {
    // Doing this on the stack is not a great idea, but this is just testing.
    char buf[kBufSize] = {};
    VSILFILE *file = VSIFOpenL(kFilenameGzip, "rb");
    if (file != nullptr) {
      CplVsiLFileCloser closer(file);
      CHECK_GT(kBufSize, VSIFReadL(buf, 1, kBufSize - 1, file));
    }
  }

  return 0;
}
