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
#include "proto_message_mutator.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/fuzzers/ogr.h"
#include "autotest2/cpp/ogr/ogrsf_frmts/shape/shapedatasource_fuzzer.pb.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogrsf_frmts/shape/ogrshape.h"
#include "port/cpl_error.h"
#include "port/cpl_port.h"

using third_party::gdal::autotest2::cpp::ogr::ogrsf_frmts::shape::Shape;

// Test a particular set of files written to vsimem.
void TryShape(GDALOpenInfo *open_info) {
  std::unique_ptr<OGRShapeDataSource> dataset(new OGRShapeDataSource);
  const int result = dataset->Open(open_info, FALSE, FALSE);
  CHECK(result == FALSE || result == TRUE);

  if (!result) return;

  autotest2::OGRFuzzOneInput(dataset.get());
}

DEFINE_PROTO_FUZZER(const Shape &s) {
  WithQuietHandler handler;

  const char kFilenameShp[] = "/vsimem/a.shp";
  autotest2::VsiMemMaybeTempWrapper shp(kFilenameShp, s.shp(), s.has_shp());

  auto open_info =
      gtl::MakeUnique<GDALOpenInfo>(kFilenameShp, GDAL_OF_READONLY, nullptr);

  // Try with just the shp file.
  TryShape(open_info.get());

  const char kFilenameCpg[] = "/vsimem/a.cpg";
  const char kFilenameDbf[] = "/vsimem/a.dbf";
  const char kFilenameIdm[] = "/vsimem/a.idm";
  const char kFilenameInd[] = "/vsimem/a.ind";
  const char kFilenameQix[] = "/vsimem/a.qix";
  const char kFilenamePrj[] = "/vsimem/a.prf";
  const char kFilenameSbn[] = "/vsimem/a.sbn";
  const char kFilenameShx[] = "/vsimem/a.shx";

  // Try each available file with the .shp by itself.
  if (s.has_cpg()) {
    autotest2::VsiMemTempWrapper cpg(kFilenameCpg, s.cpg());
    TryShape(open_info.get());
  }
  if (s.has_dbf()) {
    autotest2::VsiMemTempWrapper dbf(kFilenameDbf, s.dbf());
    TryShape(open_info.get());
  }
  if (s.has_idm()) {
    autotest2::VsiMemTempWrapper idm(kFilenameIdm, s.idm());
    TryShape(open_info.get());
  }
  if (s.has_ind()) {
    autotest2::VsiMemTempWrapper ind(kFilenameInd, s.ind());
    TryShape(open_info.get());
  }
  if (s.has_qix()) {
    autotest2::VsiMemTempWrapper qix(kFilenameQix, s.qix());
    TryShape(open_info.get());
  }
  if (s.has_prj()) {
    autotest2::VsiMemTempWrapper prj(kFilenamePrj, s.prj());
    TryShape(open_info.get());
  }
  if (s.has_sbn()) {
    autotest2::VsiMemTempWrapper sbn(kFilenameSbn, s.sbn());
    TryShape(open_info.get());
  }
  if (s.has_shx()) {
    autotest2::VsiMemTempWrapper shx(kFilenameShx, s.shx());
    TryShape(open_info.get());
  }

  // Try again with all available files written to vsimem.
  autotest2::VsiMemMaybeTempWrapper cpg(kFilenameCpg, s.cpg(), s.has_cpg());
  autotest2::VsiMemMaybeTempWrapper dbf(kFilenameDbf, s.dbf(), s.has_dbf());
  autotest2::VsiMemMaybeTempWrapper idm(kFilenameIdm, s.idm(), s.has_idm());
  autotest2::VsiMemMaybeTempWrapper ind(kFilenameInd, s.ind(), s.has_ind());
  autotest2::VsiMemMaybeTempWrapper qix(kFilenameQix, s.qix(), s.has_qix());
  autotest2::VsiMemMaybeTempWrapper prj(kFilenamePrj, s.prj(), s.has_prj());
  autotest2::VsiMemMaybeTempWrapper sbn(kFilenameSbn, s.sbn(), s.has_sbn());
  autotest2::VsiMemMaybeTempWrapper shx(kFilenameShx, s.shx(), s.has_shx());
  TryShape(open_info.get());
}
