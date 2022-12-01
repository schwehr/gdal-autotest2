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

#include <memory>

#include "logging.h"
#include "security/fuzzing/blaze/proto_message_mutator.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/frmts/raw/ehdr/ehdrdataset_fuzzer.pb.h"
#include "autotest2/cpp/frmts/raw/ehdr/ehdrdataset_fuzzer.proto.h"
#include "autotest2/cpp/fuzzers/gdal.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "frmts/raw/ehdrdataset.h"
#include "gcore/gdal.h"
#include "gcore/gdal_frmts.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_conv.h"
#include "port/cpl_error.h"
#include "port/cpl_port.h"
#include "port/cpl_string.h"

using third_party::gdal::autotest2::cpp::frmts::raw::ehdr::Ehdr;

DEFINE_PROTO_FUZZER(const Ehdr &s) {
  const char kFilenameBil[] = "/vsimem/a.bil";
  const char kFilenameHdr[] = "/vsimem/a.hdr";
  const char kFilenamePrj[] = "/vsimem/a.prj";
  const char kFilenameVrt[] = "/vsimem/a.vrt";
  const char kFilenameXml[] = "/vsimem/a.aux.xml";

  autotest2::VsiMemMaybeTempWrapper bil(kFilenameBil, s.bil(), true);
  autotest2::VsiMemMaybeTempWrapper hdr(kFilenameHdr, s.hdr(), true);
  autotest2::VsiMemMaybeTempWrapper prj(kFilenamePrj, s.prj(), s.has_prj());
  autotest2::VsiMemMaybeTempWrapper vrt(kFilenameVrt, s.vrt(), s.has_vrt());
  autotest2::VsiMemMaybeTempWrapper xml(kFilenameXml, s.xml(), s.has_xml());

  WithQuietHandler error_handler;

  // Directly test the driver class.
  {
    auto open_info =
        std::make_unique<GDALOpenInfo>(kFilenameBil, GDAL_OF_READONLY, nullptr);
    auto dataset = absl::WrapUnique(EHdrDataset::Open(open_info.get()));

    if (dataset != nullptr) autotest2::GDALFuzzOneInput(dataset.get());
  }

  // Test opening via the GDAL lookup infrastructure with only the EHDR driver
  // active.
  GDALRegister_EHdr();
  {
    auto dataset = GDALOpen(kFilenameBil, GA_ReadOnly);
    if (dataset != nullptr) {
      autotest2::GDALFuzzOneInput(static_cast<GDALDataset *>(dataset));
      GDALClose(dataset);
    }
  }

  // Testing opening via the GDAL lookup infrastructure, but via the VRT
  // driver.  With only the EHDR and VRT drivers, this should focus in on
  // more interesting cases such as an EHDR used as a band in a VRT and
  // transformations like external color tables, warps, and masks.
  {
    if (!s.has_vrt()) return;

    GDALRegister_VRT();

    auto vrt_dataset = GDALOpen(kFilenameVrt, GA_ReadOnly);
    if (vrt_dataset == nullptr) return;
    autotest2::GDALFuzzOneInput(static_cast<GDALDataset *>(vrt_dataset));
    GDALClose(vrt_dataset);
  }
}
