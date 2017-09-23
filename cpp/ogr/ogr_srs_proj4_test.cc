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
// This is a complete rewrite of a file licensed as follows:
//
// Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
// Copyright (c) 2009-2013, Even Rouault <even dot rouault at mines-paris dot
// org> Copyright (c) 2014, Kyle Shannon <kyle at pobox dot com>
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

// https://trac.osgeo.org/gdal/browser/trunk/autotest/osr/osr_proj4.py

#include "gunit.h"
#include "autotest2/cpp/util/error_handler.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_spatialref.h"
#include "ogr/ogr_srs_api.h"
#include "port/cpl_error.h"
#include "port/cpl_port.h"

namespace autotest2 {
namespace {

TEST(Proj4Test, Empty) {
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromProj4(nullptr));
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromProj4(""));
}

// TODO(schwehr): Port osr_proj4_1
// TODO(schwehr): Port osr_proj4_2
// TODO(schwehr): Port osr_proj4_3
// TODO(schwehr): Port osr_proj4_4
// TODO(schwehr): Port osr_proj4_5
// TODO(schwehr): Port osr_proj4_6
// TODO(schwehr): Port osr_proj4_7
// TODO(schwehr): Port osr_proj4_8
// TODO(schwehr): Port osr_proj4_9
// TODO(schwehr): Port osr_proj4_10
// TODO(schwehr): Port osr_proj4_11
// TODO(schwehr): Port osr_proj4_12
// TODO(schwehr): Port osr_proj4_13
// TODO(schwehr): Port osr_proj4_14
// TODO(schwehr): Port osr_proj4_15
// TODO(schwehr): Port osr_proj4_16
// TODO(schwehr): Port osr_proj4_17
// TODO(schwehr): Port osr_proj4_18
// TODO(schwehr): Port osr_proj4_19
// TODO(schwehr): Port osr_proj4_20
// TODO(schwehr): Port osr_proj4_21
// TODO(schwehr): Port osr_proj4_22
// TODO(schwehr): Port osr_proj4_23
// TODO(schwehr): Port osr_proj4_24
// TODO(schwehr): Port osr_proj4_25
// TODO(schwehr): Port osr_proj4_26
// TODO(schwehr): Port osr_proj4_27
// TODO(schwehr): Port osr_proj4_28
// TODO(schwehr): Port osr_proj4_28_missing_proj_epsg_dict

TEST(Proj4Test, UtmZoneAndSouthOverRange) {
  WithQuietHandler error_handler;
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_CORRUPT_DATA,
            srs.importFromProj4("+proj=utm +zone=6555555555554"));
  EXPECT_EQ(OGRERR_CORRUPT_DATA,
            srs.importFromProj4("+proj=utm +zone=61"));
  // South should not have an arg.
  EXPECT_EQ(OGRERR_CORRUPT_DATA,
            srs.importFromProj4("+proj=utm +zone=6 +south=2"));
}

}  // namespace
}  // namespace autotest2
