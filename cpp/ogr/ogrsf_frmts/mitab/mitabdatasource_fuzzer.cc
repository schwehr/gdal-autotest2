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
#include "third_party/absl/memory/memory.h"
#include "security/fuzzing/blaze/proto_message_mutator.h"
#include "autotest2/cpp/fuzzers/ogr.h"
#include "autotest2/cpp/ogr/ogrsf_frmts/mitab/mitabdatasource_fuzzer.pb.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogrsf_frmts/mitab/mitab_ogr_driver.h"
#include "port/cpl_error.h"
#include "port/cpl_port.h"

using third_party::gdal::autotest2::cpp::ogr::ogrsf_frmts::mitab::Mitab;

// Test a particular set of files written to vsimem.
void TryMitab(GDALOpenInfo *open_info) {
  std::unique_ptr<OGRTABDataSource> dataset(new OGRTABDataSource);
  const int result = dataset->Open(open_info, FALSE);
  CHECK(result == FALSE || result == TRUE);

  if (!result) return;

  autotest2::OGRFuzzOneInput(dataset.get());
}

DEFINE_PROTO_FUZZER(const Mitab &m) {
  WithQuietHandler handler;

  // Try with just the map file.
  const char kFilenameMap[] = "/vsimem/a.map";
  autotest2::VsiMemMaybeTempWrapper map(kFilenameMap, m.map(), m.has_map());
  auto open_info =
      gtl::MakeUnique<GDALOpenInfo>(kFilenameMap, GDAL_OF_READONLY, nullptr);
  TryMitab(open_info.get());

  const char kFilenameMif[] = "/vsimem/a.mif";
  const char kFilenameMid[] = "/vsimem/a.mid";
  const char kFilenameTab[] = "/vsimem/a.tab";
  const char kFilenameInd[] = "/vsimem/a.ind";
  const char kFilenameDat[] = "/vsimem/a.dat";
  const char kFilenameId[] = "/vsimem/a.id";

  // Try each available file with the .map by itself.
  if (m.has_mif()) {
    autotest2::VsiMemTempWrapper mif(kFilenameMif, m.mif());
    TryMitab(open_info.get());
  }
  if (m.has_mid()) {
    autotest2::VsiMemTempWrapper mid(kFilenameMid, m.mid());
    TryMitab(open_info.get());
  }
  if (m.has_tab()) {
    autotest2::VsiMemTempWrapper tab(kFilenameTab, m.tab());
    TryMitab(open_info.get());
  }
  if (m.has_ind()) {
    autotest2::VsiMemTempWrapper ind(kFilenameInd, m.ind());
    TryMitab(open_info.get());
  }
  if (m.has_dat()) {
    autotest2::VsiMemTempWrapper dat(kFilenameDat, m.dat());
    TryMitab(open_info.get());
  }
  if (m.has_id()) {
    autotest2::VsiMemTempWrapper id(kFilenameId, m.id());
    TryMitab(open_info.get());
  }

  // Try again with all available files written to vsimem.
  autotest2::VsiMemMaybeTempWrapper mif(kFilenameMif, m.mif(), m.has_mif());
  autotest2::VsiMemMaybeTempWrapper mid(kFilenameMid, m.mid(), m.has_mid());
  autotest2::VsiMemMaybeTempWrapper tab(kFilenameTab, m.tab(), m.has_tab());
  autotest2::VsiMemMaybeTempWrapper ind(kFilenameInd, m.ind(), m.has_ind());
  autotest2::VsiMemMaybeTempWrapper dat(kFilenameDat, m.dat(), m.has_dat());
  autotest2::VsiMemMaybeTempWrapper id(kFilenameId, m.id(), m.has_id());
  TryMitab(open_info.get());
}
