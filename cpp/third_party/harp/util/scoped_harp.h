// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// RAII wrappers for stcorp_harp C resources and global library initialization.

#ifndef THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_SCOPED_HARP_H_
#define THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_SCOPED_HARP_H_

#include <cstdlib>
#include <memory>

#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {

// Calls harp_init() on construction and pairs it with harp_done() on
// destruction, resetting harp_errno at both boundaries.
class ScopedHarpInit {
 public:
  ScopedHarpInit() : init_status_(harp_init()) {
    if (init_status_ == HARP_SUCCESS) {
      harp_set_error(HARP_SUCCESS, nullptr);
    }
  }

  ~ScopedHarpInit() {
    if (init_status_ == HARP_SUCCESS) {
      harp_done();
    }
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  ScopedHarpInit(const ScopedHarpInit&) = delete;
  ScopedHarpInit& operator=(const ScopedHarpInit&) = delete;

  int status() const { return init_status_; }

 private:
  int init_status_;
};

struct HarpProductDeleter {
  void operator()(harp_product* p) const { harp_product_delete(p); }
};
using ScopedHarpProduct = std::unique_ptr<harp_product, HarpProductDeleter>;

struct HarpVariableDeleter {
  void operator()(harp_variable* v) const { harp_variable_delete(v); }
};
using ScopedHarpVariable = std::unique_ptr<harp_variable, HarpVariableDeleter>;

struct HarpProductMetadataDeleter {
  void operator()(harp_product_metadata* m) const {
    harp_product_metadata_delete(m);
  }
};
using ScopedHarpProductMetadata =
    std::unique_ptr<harp_product_metadata, HarpProductMetadataDeleter>;

struct HarpDatasetDeleter {
  void operator()(harp_dataset* d) const { harp_dataset_delete(d); }
};
using ScopedHarpDataset = std::unique_ptr<harp_dataset, HarpDatasetDeleter>;

struct HarpCollocationResultDeleter {
  void operator()(harp_collocation_result* r) const {
    harp_collocation_result_delete(r);
  }
};
using ScopedHarpCollocationResult =
    std::unique_ptr<harp_collocation_result, HarpCollocationResultDeleter>;

struct HarpCollocationResultShallowDeleter {
  void operator()(harp_collocation_result* r) const {
    harp_collocation_result_shallow_delete(r);
  }
};
using ScopedHarpCollocationResultShallow =
    std::unique_ptr<harp_collocation_result,
                    HarpCollocationResultShallowDeleter>;

struct HarpProgramDeleter {
  void operator()(harp_program* p) const { harp_program_delete(p); }
};
using ScopedHarpProgram = std::unique_ptr<harp_program, HarpProgramDeleter>;

struct HarpOperationDeleter {
  void operator()(harp_operation* o) const { harp_operation_delete(o); }
};
using ScopedHarpOperation =
    std::unique_ptr<harp_operation, HarpOperationDeleter>;

struct HarpAreaMaskDeleter {
  void operator()(harp_area_mask* m) const { harp_area_mask_delete(m); }
};
using ScopedHarpAreaMask = std::unique_ptr<harp_area_mask, HarpAreaMaskDeleter>;

struct HarpSphericalPolygonDeleter {
  void operator()(harp_spherical_polygon* p) const {
    harp_spherical_polygon_delete(p);
  }
};
using ScopedHarpSphericalPolygon =
    std::unique_ptr<harp_spherical_polygon, HarpSphericalPolygonDeleter>;

struct HarpDimensionMaskDeleter {
  void operator()(harp_dimension_mask* m) const {
    harp_dimension_mask_delete(m);
  }
};
using ScopedHarpDimensionMask =
    std::unique_ptr<harp_dimension_mask, HarpDimensionMaskDeleter>;

struct HarpDimensionMaskSetDeleter {
  void operator()(harp_dimension_mask_set* s) const {
    harp_dimension_mask_set_delete(s);
  }
};
using ScopedHarpDimensionMaskSet =
    std::unique_ptr<harp_dimension_mask_set, HarpDimensionMaskSetDeleter>;

struct HarpCollocationMaskDeleter {
  void operator()(harp_collocation_mask* m) const {
    harp_collocation_mask_delete(m);
  }
};
using ScopedHarpCollocationMask =
    std::unique_ptr<harp_collocation_mask, HarpCollocationMaskDeleter>;

struct HarpUnitConverterDeleter {
  void operator()(harp_unit_converter* c) const {
    harp_unit_converter_delete(c);
  }
};
using ScopedHarpUnitConverter =
    std::unique_ptr<harp_unit_converter, HarpUnitConverterDeleter>;

struct HarpIngestionOptionsDeleter {
  void operator()(harp_ingestion_options* o) const {
    harp_ingestion_options_delete(o);
  }
};
using ScopedHarpIngestionOptions =
    std::unique_ptr<harp_ingestion_options, HarpIngestionOptionsDeleter>;

struct MallocDeleter {
  void operator()(void* p) const { std::free(p); }
};
using ScopedMallocChar = std::unique_ptr<char, MallocDeleter>;

}  // namespace stcorp_harp

#endif  // THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_SCOPED_HARP_H_
