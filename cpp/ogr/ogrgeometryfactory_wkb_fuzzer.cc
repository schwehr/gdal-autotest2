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
#include <memory>

#include "logging.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/util/cpl_cstringlist.h"
#include "autotest2/cpp/util/error_handler.h"
#include "ogr/ogr_api.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_geometry.h"
#include "ogr/ogr_spatialref.h"
#include "port/cpl_conv.h"
#include "port/cpl_csv.h"
#include "port/cpl_error.h"
#include "port/cpl_port.h"
#include "port/cpl_string.h"
#include "port/cpl_vsi.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  WithQuietHandler error_handler;

  for (auto variant : {wkbVariantOldOgc, wkbVariantIso, wkbVariantPostGIS1}) {
    OGRGeometry *geomptr = nullptr;

    int bytes_consumed = 0;
    OGRGeometryFactory::createFromWkb(data, nullptr, &geomptr, size,
                                      variant, bytes_consumed);

    // bytes_consumed will be -1 on some errors.
    CHECK_GE(bytes_consumed, -1);

    if (geomptr == nullptr) return 0;
    auto geom = absl::WrapUnique(geomptr);

    geom->getGeometryType();
    geom->getDimension();
    geom->getCoordinateDimension();
    geom->IsRing();
    OGREnvelope envelope;
    geom->getEnvelope(&envelope);
    CPLFree(geom->exportToGML(nullptr));
    CPLFree(geom->exportToJson());
    CPLFree(geom->exportToKML());
    geom->WkbSize();
    geom->flattenTo2D();
    geom->swapXY();
    geom->segmentize(1000);
  }

  return 0;
}
