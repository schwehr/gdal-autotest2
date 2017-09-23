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

#include "autotest2/cpp/util/cpl_cstringlist.h"
#include "autotest2/cpp/util/error_handler.h"
#include "ogr/ogr_api.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_geometry.h"
#include "ogr/ogr_spatialref.h"
#include "port/cpl_conv.h"
#include "port/cpl_csv.h"
#include "port/cpl_error.h"
#include "port/cpl_string.h"
#include "port/cpl_vsi.h"
#include "port/cpl_port.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const string data2(reinterpret_cast<const char *>(data), size);

  WithQuietHandler error_handler;
  OGRGeometryH geom_handle = OGR_G_CreateFromGML(data2.c_str());
  if (geom_handle == nullptr) return 0;

  auto geom = static_cast<OGRGeometry *>(geom_handle);
  geom->getGeometryType();
  geom->getDimension();
  geom->getCoordinateDimension();
  geom->IsRing();
  OGREnvelope envelope;
  geom->getEnvelope(&envelope);
  CPLFree(geom->exportToGML(nullptr));
  delete geom;

  return 0;
}
