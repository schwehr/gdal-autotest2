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
#include "autotest2/cpp/frmts/hfa/hfadataset_fuzzer.pb.h"
#include "autotest2/cpp/frmts/hfa/hfadataset_fuzzer.proto.h"
#include "autotest2/cpp/fuzzers/gdal.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "frmts/hfa/hfadataset.h"
#include "gcore/gdal.h"
#include "gcore/gdal_frmts.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_conv.h"
#include "port/cpl_error.h"
#include "port/cpl_port.h"
#include "port/cpl_string.h"

using third_party::gdal::autotest2::cpp::frmts::hfa::Hfa;

DEFINE_PROTO_FUZZER(const Hfa &s) {
  const char kFilenameAux[] = "/vsimem/a.aux";
  const char kFilenameImg[] = "/vsimem/a.img";
  const char kFilenamePrj[] = "/vsimem/a.prj";
  const char kFilenameRrd[] = "/vsimem/a.rrd";
  const char kFilenameVrt[] = "/vsimem/a.vrt";
  const char kFilenameXml[] = "/vsimem/a.aux.xml";

  autotest2::VsiMemMaybeTempWrapper aux(kFilenameAux, s.aux(), s.has_aux());
  // img is required.
  autotest2::VsiMemMaybeTempWrapper img(kFilenameImg, s.img(), true);
  autotest2::VsiMemMaybeTempWrapper prj(kFilenamePrj, s.prj(), s.has_prj());
  autotest2::VsiMemMaybeTempWrapper rrd(kFilenameRrd, s.rrd(), s.has_rrd());
  autotest2::VsiMemMaybeTempWrapper vrt(kFilenameVrt, s.vrt(), s.has_vrt());
  autotest2::VsiMemMaybeTempWrapper xml(kFilenameXml, s.xml(), s.has_xml());

  WithQuietHandler error_handler;

  // Directly test the driver class.
  {
    auto open_info = absl::make_unique<GDALOpenInfo>(kFilenameImg,
                                                     GDAL_OF_READONLY, nullptr);
    const int result = HFADataset::Identify(open_info.get());
    CHECK_LE(-1, result);
    CHECK_GE(1, result);
    auto dataset = absl::WrapUnique(HFADataset::Open(open_info.get()));

    if (dataset != nullptr) autotest2::GDALFuzzOneInput(dataset.get());
  }

  // Test opening via the GDAL lookup infrastructure with only the HFA driver
  // active.
  GDALRegister_HFA();
  {
    auto dataset = GDALOpen(kFilenameImg, GA_ReadOnly);
    if (dataset != nullptr) {
      autotest2::GDALFuzzOneInput(static_cast<GDALDataset *>(dataset));
      GDALClose(dataset);
    }
  }

  // Testing opening via the GDAL lookup infrastructure, but via the VRT
  // driver.  With only the HFA and VRT drivers, this should focus in on
  // more interesting cases such as an HFA used as a band in a VRT and
  // transformations like external color tables, warps, and masks.
  GDALRegister_VRT();

  if (!s.has_vrt()) return;
  auto vrt_dataset = GDALOpen(kFilenameVrt, GA_ReadOnly);
  if (vrt_dataset == nullptr) return;
  autotest2::GDALFuzzOneInput(static_cast<GDALDataset *>(vrt_dataset));
  GDALClose(vrt_dataset);
}
