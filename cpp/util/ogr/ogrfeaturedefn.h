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

#ifndef THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_OGR_OGRFEATUREDEFN_H_
#define THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_OGR_OGRFEATUREDEFN_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "third_party/absl/memory/memory.h"
#include "ogr/ogr_api.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_feature.h"
#include "port/cpl_port.h"

namespace autotest2 {

// Constructs instances of OGRFeatureDefn.
//
// Assumes that there is only one geometry column and that the defaults for
// other Field and Feature attributes are okay or will be overridden after the
// instance is returned.
//
// Use wkbUnknown for features that can have any type of geometry.
//
// OGRFeatureDefn always starts with one geometry that defaults to being named
// "geo".
//
// Example:
//
//   OGRFeatureDefnBuilder builder("schema", wkbPoint);
//   builder.AddField("bool", OFTInteger, OFSTBoolean);
//   builder.AddField("real", OFTReal, OFSTNone);
//   OGRFeatureDefnReleaser fd(builder.Build());
//   auto fd_cleaner = gtl::MakeCleanup([fd] { fd->Release(); });
//   // Use fd.

class OGRFeatureDefnBuilder {
 public:
  OGRFeatureDefnBuilder(const char* name, OGRwkbGeometryType geom_type)
      : has_first_geometry_(false), feature_defn_(new OGRFeatureDefn(name)) {
    feature_defn_->Reference();
    feature_defn_->SetGeomType(geom_type);
  }
  ~OGRFeatureDefnBuilder() {
    if (feature_defn_ != nullptr) feature_defn_->Release();
  }

  void AddGeom(const char* name, OGRwkbGeometryType geom_type) {
    // A FeatureDefn starts with one geometry that an empty string as the name.
    if (!has_first_geometry_) {
      has_first_geometry_ = true;
      feature_defn_->GetGeomFieldDefn(0)->SetName(name);
      feature_defn_->SetGeomType(geom_type);
      return;
    }

    OGRGeomFieldDefn field_defn(name, geom_type);
    feature_defn_->AddGeomFieldDefn(&field_defn);
  }

  void AddField(const char* name, OGRFieldType type, OGRFieldSubType subtype) {
    OGRFieldDefn field_defn(name, type);
    field_defn.SetSubType(subtype);
    feature_defn_->AddFieldDefn(&field_defn);
  }

  // It is the callers responsibility to either pass off or call Release on the
  // returned instance.
  OGRFeatureDefn* Build() {
    if (feature_defn_ == nullptr) {
      // Building more than one time per instance is not allowed.
      return nullptr;
    }

    if (!has_first_geometry_) {
      // Give the default geometry a name other than the empty string.
      feature_defn_->GetGeomFieldDefn(0)->SetName("geo");
    }

    // Set feature_defn_ to nullptr to block reuse of the builder.
    auto feature_defn = feature_defn_;
    feature_defn_ = nullptr;
    return feature_defn;
  }

 private:
  bool has_first_geometry_;
  OGRFeatureDefn* feature_defn_;
};

}  // namespace autotest2

#endif  // THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_OGR_OGRFEATUREDEFN_H_
