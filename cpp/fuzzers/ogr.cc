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

// Vector driver fuzzer.

#include <stdio.h>

#include "commandlineflags.h"
#include "logging.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/flags/flag.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_feature.h"
#include "ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "port/cpl_error.h"
#include "port/cpl_port.h"

// Parsing the command line requires that the calling target is built with this
// build dependency.  It is okay to go without it.
//   //security/fuzzing/blaze:default_init_google_for_cc_fuzz_target
ABSL_FLAG(bool, ogr_deep_fuzz, true,
          "Disable to do light fuzzing.  Generally used for faster fuzzing "
          "to generate initial corpus for a fuzzer.");

namespace autotest2 {

void
OGRFuzzOneInput(GDALDataset *dataset) {
  if (dataset == nullptr) return;

  bool ogr_deep_fuzz = absl::GetFlag(FLAGS_ogr_deep_fuzz);

  FILE *dev_null = fopen("/dev/null", "w");
  LOG_IF_FIRST_N(INFO, dev_null == nullptr, 1) << "Unable to write to dev_null";
  auto closer = absl::MakeCleanup([dev_null] {
    if (dev_null != nullptr) fclose(dev_null);
  });

  const int layer_count = dataset->GetLayerCount();
  CHECK_LE(0, layer_count);
  for (int layer_num = 0; layer_num < layer_count; layer_num++) {
    OGRLayer *layer = dataset->GetLayer(layer_num);
    CHECK(layer != nullptr);

    layer->GetSpatialRef();
    layer->GetGeomType();
    layer->GetFIDColumn();
    layer->GetGeometryColumn();
    layer->GetName();
    layer->GetGeomType();

    // Check for light/shallow fuzzing mode.
    if (!ogr_deep_fuzz) break;

    OGRFeature *feature = nullptr;
    bool first = true;

    while ((feature = layer->GetNextFeature())) {
      if (dev_null != nullptr)
        feature->DumpReadable(dev_null);
      if (!first) {
        delete feature;
        continue;
      }
      first = false;
      feature->GetDefnRef();
      feature->GetGeometryRef();
      const int num_geom = feature->GetGeomFieldCount();
      for (int geom = 0; geom < num_geom; geom++)
          feature->GetGeomFieldRef(geom);
      const int num_fields = feature->GetFieldCount();
      for (int field = 0; field < num_fields; field++) {
        feature->IsFieldSet(field);
        feature->IsFieldNull(field);
        feature->GetRawFieldRef(field);
      }
      delete feature;
    }

    OGREnvelope envelope;
    const OGRErr extent_error = layer->GetExtent(&envelope, TRUE);
    CHECK(extent_error >= OGRERR_NONE);
    CHECK(extent_error <= OGRERR_NON_EXISTING_FEATURE);
  }
}

}  // namespace autotest2
