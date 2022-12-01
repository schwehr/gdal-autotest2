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
// Fuzz GeoTIFF format raster grids.
//
// Using the driver Open is not as direct as using GTiffDataset and does not
// allow using all the methods of GTiffDataset, but it's better than GDALOpenEx.
//
// See also:
//   https://trac.osgeo.org/gdal/browser/trunk/gdal/fuzzers/gdal_fuzzer.cpp

#include <memory>

#include "logging.h"
#include "security/fuzzing/blaze/proto_message_mutator.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/frmts/gtiff/gtiff_fuzzer.pb.h"
#include "autotest2/cpp/frmts/gtiff/gtiff_fuzzer.proto.h"
#include "autotest2/cpp/fuzzers/gdal.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_frmts.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_error.h"
#include "port/cpl_string.h"

constexpr char kDriverName[] = "GTiff";

using third_party::gdal::autotest2::cpp::frmts::gtiff::Gtiff;

DEFINE_PROTO_FUZZER(const Gtiff &s) {
  const char kFilenameAux[] = "/vsimem/a.aux";
  const char kFilenameImd[] = "/vsimem/a.imd";
  const char kFilenameOvr[] = "/vsimem/a.ovr";
  const char kFilenamePrj[] = "/vsimem/a.prj";
  const char kFilenameRpb[] = "/vsimem/a.rpb";
  const char kFilenameRpc[] = "/vsimem/a.rpc";
  const char kFilenameRrd[] = "/vsimem/a.rrd";
  const char kFilenameTif[] = "/vsimem/a.tif";
  const char kFilenameVrt[] = "/vsimem/a.vrt";
  const char kFilenameXml[] = "/vsimem/a.aux.xml";

  autotest2::VsiMemMaybeTempWrapper aux(kFilenameAux, s.aux(), s.has_aux());
  autotest2::VsiMemMaybeTempWrapper imd(kFilenameImd, s.imd(), s.has_imd());
  autotest2::VsiMemMaybeTempWrapper ovr(kFilenameOvr, s.ovr(), s.has_ovr());
  autotest2::VsiMemMaybeTempWrapper prj(kFilenamePrj, s.prj(), s.has_prj());
  autotest2::VsiMemMaybeTempWrapper rpb(kFilenameRpb, s.rpb(), s.has_rpb());
  autotest2::VsiMemMaybeTempWrapper rpc(kFilenameRpc, s.rpc(), s.has_rpc());
  autotest2::VsiMemMaybeTempWrapper rrd(kFilenameRrd, s.rrd(), s.has_rrd());
  // tif is required.
  autotest2::VsiMemMaybeTempWrapper tif(kFilenameTif, s.tif(), true);
  autotest2::VsiMemMaybeTempWrapper vrt(kFilenameVrt, s.vrt(), s.has_vrt());
  autotest2::VsiMemMaybeTempWrapper xml(kFilenameXml, s.xml(), s.has_xml());

  WithQuietHandler error_handler;

  // Directly test the driver class, but because of the header situation,
  // this pulls the open function out of the driver's setup.
  GDALRegister_GTiff();
  {
    GDALDriverManager *drv_manager = GetGDALDriverManager();
    GDALDriver *driver = drv_manager->GetDriverByName(kDriverName);
    CHECK(driver != nullptr);
    auto open_info = absl::make_unique<GDALOpenInfo>(kFilenameTif,
                                                     GDAL_OF_READONLY, nullptr);

    auto dataset = absl::WrapUnique(driver->pfnOpen(open_info.get()));

    if (dataset != nullptr) autotest2::GDALFuzzOneInput(dataset.get());
  }

  // Testing opening via the GDAL lookup infrastructure, but via the VRT
  // driver.  With only the GTiff and VRT drivers, this should focus in on
  // more interesting cases such as a GeoTiff used as a band in a VRT and
  // transformations like external color tables, warps, and masks.
  if (!s.has_vrt()) return;

  GDALRegister_VRT();
  auto vrt_dataset = GDALOpen(kFilenameVrt, GA_ReadOnly);
  if (vrt_dataset == nullptr) return;
  autotest2::GDALFuzzOneInput(static_cast<GDALDataset *>(vrt_dataset));
  GDALClose(vrt_dataset);
}
