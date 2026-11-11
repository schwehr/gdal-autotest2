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
// Unit tests for JSON-driven gridded deformation models
// (proj/src/transformations/defmodel.cpp, defmodel.hpp, defmodel_impl.hpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/transformations/defmodel.cpp

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/include/proj/internal/include_nlohmann_json.hpp"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

// WHITEBOX(9.9.0): defmodel.hpp textual-includes defmodel_impl.hpp, which
// defines non-inline DeformationModel methods; including inside the anonymous
// namespace gives them internal linkage and avoids ODR duplicate-symbol link
// errors against defmodel.cpp in :proj4_unbundled.
#include "third_party/proj4/proj/src/transformations/defmodel.hpp"

constexpr double kPi = std::numbers::pi;
constexpr double DegToRad(double deg) { return deg * (kPi / 180.0); }
constexpr double RadToDeg(double rad) { return rad * (180.0 / kPi); }

// Minimal mock PROJ_FILE_API to simulate oversized files (> 10 MB) and
// short reads without allocating large buffers.
struct MockFileSpec {
  unsigned long long reported_size = 0;  // NOLINT(runtime/int)
  size_t bytes_to_read = 0;
};

PROJ_FILE_HANDLE* MockOpen(PJ_CONTEXT*, const char*, PROJ_OPEN_ACCESS,
                           void* user_data) {
  return reinterpret_cast<PROJ_FILE_HANDLE*>(user_data);
}

size_t MockRead(PJ_CONTEXT*, PROJ_FILE_HANDLE* file, void* buffer,
                size_t size_bytes, void*) {
  auto* spec = reinterpret_cast<MockFileSpec*>(file);
  const size_t n = std::min(size_bytes, spec->bytes_to_read);
  if (n > 0 && buffer != nullptr) {
    std::memset(buffer, ' ', n);
  }
  return n;
}

size_t MockWrite(PJ_CONTEXT*, PROJ_FILE_HANDLE*, const void*, size_t, void*) {
  return 0;
}

// NOLINTNEXTLINE(runtime/int)
int MockSeek(PJ_CONTEXT*, PROJ_FILE_HANDLE*, long long, int, void*) {
  return 1;
}

// NOLINTNEXTLINE(runtime/int)
unsigned long long MockTell(PJ_CONTEXT*, PROJ_FILE_HANDLE* file, void*) {
  return reinterpret_cast<MockFileSpec*>(file)->reported_size;
}

void MockClose(PJ_CONTEXT*, PROJ_FILE_HANDLE*, void*) {}

int MockExists(PJ_CONTEXT*, const char*, void*) { return 1; }
int MockMkdir(PJ_CONTEXT*, const char*, void*) { return 0; }
int MockUnlink(PJ_CONTEXT*, const char*, void*) { return 0; }
int MockRename(PJ_CONTEXT*, const char*, const char*, void*) { return 0; }

constexpr PROJ_FILE_API kMockFileApi = {
    /*version=*/1, &MockOpen,   &MockRead,   &MockWrite,
    &MockSeek,     &MockTell,   &MockClose,  &MockExists,
    &MockMkdir,    &MockUnlink, &MockRename,
};

TEST(DefmodelTest, MissingOrInvalidModelParameterFailsWithExpectedErrno) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());

  // 1. Missing +model= parameter.
  ScopedPj missing_model(proj_create(ctx.get(), "+proj=defmodel"));
  EXPECT_EQ(nullptr, missing_model.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_MISSING_ARG, proj_context_errno(ctx.get()));

  // 2. Nonexistent +model= file in InMemoryFileSystem.
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));
  ScopedPj missing_file(
      proj_create(ctx.get(), "+proj=defmodel +model=/mem/nonexistent.json"));
  EXPECT_EQ(nullptr, missing_file.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));

  // 3. Invalid JSON syntax in +model= file.
  fs.AddTextFile("/mem/bad_syntax.json", "{not_valid_json");
  ScopedPj bad_json(
      proj_create(ctx.get(), "+proj=defmodel +model=/mem/bad_syntax.json"));
  EXPECT_EQ(nullptr, bad_json.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
}

TEST(DefmodelTest, OversizedAndShortReadModelFilesAreRejected) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Reported size > 10 MB triggers the DoS size guard.
  MockFileSpec spec{.reported_size = 10ULL * 1024 * 1024 + 1,
                    .bytes_to_read = 0};
  ASSERT_NE(0, proj_context_set_fileapi(ctx.get(), &kMockFileApi, &spec));
  {
    ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=defmodel +model=/mem/oversized.json"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
              proj_context_errno(ctx.get()));
    EXPECT_TRUE(capture.ContainsMessage("too large"));
  }

  // 2. Short read (fewer bytes returned than reported file size) fails cleanly.
  spec.reported_size = 128;
  spec.bytes_to_read = 16;
  {
    ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
    ScopedPj pj(
        proj_create(ctx.get(), "+proj=defmodel +model=/mem/short_read.json"));
    EXPECT_EQ(nullptr, pj.get());
    EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
              proj_context_errno(ctx.get()));
    EXPECT_TRUE(capture.ContainsMessage("Cannot read"));
  }
}

TEST(DefmodelTest, GeographicDegreeAdditionForwardInverseAndEdgeCases) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  // 1. Build a 2-band horizontal grid (east_offset, north_offset in degrees),
  // a 1-band vertical grid (vertical_offset in metres), and a 3-band 3D grid.
  constexpr float kHorizPixels[] = {
      // 2x2 grid covering lon [0, 10], lat [0, 10], uniform (+0.01, +0.02) deg.
      0.01f, 0.02f, 0.01f, 0.02f, 0.01f, 0.02f, 0.01f, 0.02f,
  };
  constexpr std::string_view kHorizXml =
      "<GDALMetadata>"
      "<Item name=\"DESCRIPTION\" sample=\"0\">east_offset</Item>"
      "<Item name=\"DESCRIPTION\" sample=\"1\">north_offset</Item>"
      "<Item name=\"UNITTYPE\" sample=\"0\">degree</Item>"
      "<Item name=\"UNITTYPE\" sample=\"1\">degree</Item>"
      "</GDALMetadata>";
  fs.AddFile(
      "/mem/horiz_deg.tif",
      BuildMinimalGeoTiff(/*width=*/2, /*height=*/2,
                          /*samples_per_pixel=*/2, /*west=*/0.0,
                          /*north=*/10.0, /*res_x=*/10.0, /*res_y=*/10.0,
                          kHorizPixels, /*is_geographic=*/true, kHorizXml));

  constexpr float kVertPixels[] = {1.5f, 1.5f, 1.5f, 1.5f};
  constexpr std::string_view kVertXml =
      "<GDALMetadata>"
      "<Item name=\"DESCRIPTION\" sample=\"0\">vertical_offset</Item>"
      "<Item name=\"UNITTYPE\" sample=\"0\">metre</Item>"
      "</GDALMetadata>";
  fs.AddFile(
      "/mem/vert_m.tif",
      BuildMinimalGeoTiff(/*width=*/2, /*height=*/2,
                          /*samples_per_pixel=*/1, /*west=*/0.0,
                          /*north=*/10.0, /*res_x=*/10.0, /*res_y=*/10.0,
                          kVertPixels, /*is_geographic=*/true, kVertXml));

  constexpr float k3dPixels[] = {
      0.005f, -0.005f, 0.25f, 0.005f, -0.005f, 0.25f,
      0.005f, -0.005f, 0.25f, 0.005f, -0.005f, 0.25f,
  };
  constexpr std::string_view k3dXml =
      "<GDALMetadata>"
      "<Item name=\"DESCRIPTION\" sample=\"0\">east_offset</Item>"
      "<Item name=\"DESCRIPTION\" sample=\"1\">north_offset</Item>"
      "<Item name=\"DESCRIPTION\" sample=\"2\">vertical_offset</Item>"
      "<Item name=\"UNITTYPE\" sample=\"0\">degree</Item>"
      "<Item name=\"UNITTYPE\" sample=\"1\">degree</Item>"
      "<Item name=\"UNITTYPE\" sample=\"2\">metre</Item>"
      "</GDALMetadata>";
  fs.AddFile("/mem/grid3d_deg.tif",
             BuildMinimalGeoTiff(/*width=*/2, /*height=*/2,
                                 /*samples_per_pixel=*/3, /*west=*/0.0,
                                 /*north=*/10.0, /*res_x=*/10.0, /*res_y=*/10.0,
                                 k3dPixels, /*is_geographic=*/true, k3dXml));

  constexpr std::string_view kMasterJson = R"json({
    "file_type": "deformation_model_master_file",
    "format_version": "1.0",
    "source_crs": "EPSG:4326",
    "target_crs": "EPSG:4326",
    "definition_crs": "EPSG:4326",
    "horizontal_offset_unit": "degree",
    "vertical_offset_unit": "metre",
    "horizontal_offset_method": "addition",
    "extent": {
      "type": "bbox",
      "parameters": {"bbox": [0.0, 0.0, 10.0, 10.0]}
    },
    "time_extent": {
      "first": "2000-01-01T00:00:00Z",
      "last": "2030-01-01T00:00:00Z"
    },
    "components": [
      {
        "description": "uncertainty only",
        "extent": {"type": "bbox", "parameters": {"bbox": [0.0, 0.0, 10.0, 10.0]}},
        "displacement_type": "none",
        "uncertainty_type": "none",
        "spatial_model": {
          "type": "GeoTIFF",
          "interpolation_method": "bilinear",
          "filename": "/mem/unused.tif"
        },
        "time_function": {"type": "constant"}
      },
      {
        "description": "horizontal constant",
        "extent": {"type": "bbox", "parameters": {"bbox": [0.0, 0.0, 10.0, 10.0]}},
        "displacement_type": "horizontal",
        "uncertainty_type": "none",
        "spatial_model": {
          "type": "GeoTIFF",
          "interpolation_method": "bilinear",
          "filename": "/mem/horiz_deg.tif"
        },
        "time_function": {"type": "constant"}
      },
      {
        "description": "vertical velocity",
        "extent": {"type": "bbox", "parameters": {"bbox": [0.0, 0.0, 10.0, 10.0]}},
        "displacement_type": "vertical",
        "uncertainty_type": "none",
        "spatial_model": {
          "type": "GeoTIFF",
          "interpolation_method": "bilinear",
          "filename": "/mem/vert_m.tif"
        },
        "time_function": {
          "type": "velocity",
          "parameters": {"reference_epoch": "2020-01-01T00:00:00Z"}
        }
      },
      {
        "description": "3d step",
        "extent": {"type": "bbox", "parameters": {"bbox": [0.0, 0.0, 10.0, 10.0]}},
        "displacement_type": "3d",
        "uncertainty_type": "none",
        "spatial_model": {
          "type": "GeoTIFF",
          "interpolation_method": "bilinear",
          "filename": "/mem/grid3d_deg.tif"
        },
        "time_function": {
          "type": "step",
          "parameters": {"step_epoch": "2015-01-01T00:00:00Z"}
        }
      }
    ]
  })json";
  fs.AddTextFile("/mem/deg_model.json", kMasterJson);

  ScopedPj pj(
      proj_create(ctx.get(), "+proj=defmodel +model=/mem/deg_model.json"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(nullptr, pj->destructor(nullptr, 0));

  // 2. Forward and inverse at t = 2022.0:
  //    horiz_deg contributes (+0.01, +0.02) deg;
  //    vert_m contributes 2.0 * 1.5 = +3.0 m;
  //    grid3d_deg contributes (+0.005, -0.005) deg and +0.25 m.
  const PJ_COORD in = proj_coord(DegToRad(5.0), DegToRad(5.0), 10.0, 2022.0);
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(5.015, RadToDeg(fwd.lpzt.lam), 1e-6);
  EXPECT_NEAR(5.015, RadToDeg(fwd.lpzt.phi), 1e-6);
  EXPECT_NEAR(13.25, fwd.lpzt.z, 1e-5);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.lpzt.lam, inv.lpzt.lam, 1e-11);
  EXPECT_NEAR(in.lpzt.phi, inv.lpzt.phi, 1e-11);
  EXPECT_NEAR(in.lpzt.z, inv.lpzt.z, 1e-4);

  // 3. Missing time (HUGE_VAL) sets PROJ_ERR_COORD_TRANSFM_MISSING_TIME.
  const PJ_COORD missing_t =
      proj_coord(DegToRad(5.0), DegToRad(5.0), 0.0, HUGE_VAL);
  const PJ_COORD fwd_no_t = proj_trans(pj.get(), PJ_FWD, missing_t);
  EXPECT_TRUE(std::isinf(fwd_no_t.xyzt.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_MISSING_TIME, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  const PJ_COORD inv_no_t = proj_trans(pj.get(), PJ_INV, missing_t);
  EXPECT_TRUE(std::isinf(inv_no_t.xyzt.x));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_MISSING_TIME, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // 4. Outside spatial extent or outside time_extent returns HUGE_VAL.
  const PJ_COORD out_spatial =
      proj_coord(DegToRad(5.0), DegToRad(25.0), 0.0, 2022.0);
  EXPECT_TRUE(std::isinf(proj_trans(pj.get(), PJ_FWD, out_spatial).xyzt.x));
  EXPECT_TRUE(std::isinf(proj_trans(pj.get(), PJ_INV, out_spatial).xyzt.x));

  const PJ_COORD out_time =
      proj_coord(DegToRad(5.0), DegToRad(5.0), 0.0, 1990.0);
  EXPECT_TRUE(std::isinf(proj_trans(pj.get(), PJ_FWD, out_time).xyzt.x));
  EXPECT_TRUE(std::isinf(proj_trans(pj.get(), PJ_INV, out_time).xyzt.x));

  // 5. Reassigning context to the same and to a new context clears cache.
  proj_assign_context(pj.get(), ctx.get());
  ScopedContext ctx2 = MakeContext();
  ASSERT_NE(nullptr, ctx2.get());
  ASSERT_TRUE(fs.Install(ctx2.get()));
  proj_assign_context(pj.get(), ctx2.get());
  const PJ_COORD fwd_after_reassign = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(fwd.lpzt.lam, fwd_after_reassign.lpzt.lam, 1e-12);
  proj_assign_context(pj.get(), ctx.get());
}

TEST(DefmodelTest, MetreOffsetsBilinearGeocentricBilinearAndGeocentricMethod) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  // 1. 2-band horizontal metre grid (coarse resx = 10 deg -> smallResx = false)
  // and 3-band 3D metre grid (fine resx = 0.5 deg -> smallResx = true).
  constexpr float kHorizMetrePixels[] = {
      1.0f, 2.0f, 1.0f, 2.0f, 1.0f, 2.0f, 1.0f, 2.0f,
  };
  fs.AddFile("/mem/horiz_m_coarse.tif",
             BuildMinimalGeoTiff(/*width=*/2, /*height=*/2,
                                 /*samples_per_pixel=*/2, /*west=*/0.0,
                                 /*north=*/10.0, /*res_x=*/10.0, /*res_y=*/10.0,
                                 kHorizMetrePixels, /*is_geographic=*/true));

  constexpr float k3dMetrePixels[] = {
      0.5f, -0.5f, 0.1f, 0.5f, -0.5f, 0.1f,
      0.5f, -0.5f, 0.1f, 0.5f, -0.5f, 0.1f,
  };
  fs.AddFile("/mem/grid3d_m_fine.tif",
             BuildMinimalGeoTiff(/*width=*/2, /*height=*/2,
                                 /*samples_per_pixel=*/3, /*west=*/4.8,
                                 /*north=*/5.2, /*res_x=*/0.4, /*res_y=*/0.4,
                                 k3dMetrePixels, /*is_geographic=*/true));

  // 2. Test horizontal_offset_method = "geocentric" + geocentric_bilinear
  // (exercises geographicToGeocentric, geocentricToGeographic, and GridEx).
  // Also use EPSG:4979 (PJ_TYPE_GEOGRAPHIC_3D_CRS) for definition_crs.
  constexpr std::string_view kGeocentricJson = R"json({
    "file_type": "deformation_model_master_file",
    "format_version": "1.0",
    "source_crs": "EPSG:4979",
    "target_crs": "EPSG:4979",
    "definition_crs": "EPSG:4979",
    "horizontal_offset_unit": "metre",
    "vertical_offset_unit": "metre",
    "horizontal_offset_method": "geocentric",
    "extent": {
      "type": "bbox",
      "parameters": {"bbox": [0.0, 0.0, 10.0, 10.0]}
    },
    "time_extent": {
      "first": "2000-01-01T00:00:00Z",
      "last": "2030-01-01T00:00:00Z"
    },
    "components": [
      {
        "extent": {"type": "bbox", "parameters": {"bbox": [0.0, 0.0, 10.0, 10.0]}},
        "displacement_type": "horizontal",
        "uncertainty_type": "none",
        "spatial_model": {
          "type": "GeoTIFF",
          "interpolation_method": "geocentric_bilinear",
          "filename": "/mem/horiz_m_coarse.tif"
        },
        "time_function": {"type": "constant"}
      },
      {
        "extent": {"type": "bbox", "parameters": {"bbox": [4.8, 4.8, 5.2, 5.2]}},
        "displacement_type": "3d",
        "uncertainty_type": "none",
        "spatial_model": {
          "type": "GeoTIFF",
          "interpolation_method": "geocentric_bilinear",
          "filename": "/mem/grid3d_m_fine.tif"
        },
        "time_function": {"type": "constant"}
      }
    ]
  })json";
  fs.AddTextFile("/mem/geoc_model.json", kGeocentricJson);

  ScopedPj pj_geoc(proj_create(
      ctx.get(), "+proj=defmodel +model=/mem/geoc_model.json +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_geoc.get());

  const PJ_COORD in = proj_coord(DegToRad(5.0), DegToRad(5.0), 50.0, 2020.0);
  const PJ_COORD fwd = proj_trans(pj_geoc.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj_geoc.get()));
  EXPECT_GT(fwd.lpzt.lam, in.lpzt.lam);
  EXPECT_GT(fwd.lpzt.phi, in.lpzt.phi);
  EXPECT_NEAR(50.1, fwd.lpzt.z, 1e-6);

  const PJ_COORD inv = proj_trans(pj_geoc.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj_geoc.get()));
  EXPECT_NEAR(in.lpzt.lam, inv.lpzt.lam, 1e-11);
  EXPECT_NEAR(in.lpzt.phi, inv.lpzt.phi, 1e-11);
  EXPECT_NEAR(in.lpzt.z, inv.lpzt.z, 1e-4);

  // 3. Test horizontal_offset_method = "addition" with metre offsets on a
  // geographic CRS (exercises DeltaEastingNorthingToLongLat,
  // getEastingNorthingOffset, and getEastingNorthingZOffset with bilinear
  // interpolation).
  constexpr std::string_view kAdditionMetreJson = R"json({
    "file_type": "deformation_model_master_file",
    "format_version": "1.0",
    "source_crs": "EPSG:4326",
    "target_crs": "EPSG:4326",
    "definition_crs": "EPSG:4326",
    "horizontal_offset_unit": "metre",
    "vertical_offset_unit": "metre",
    "horizontal_offset_method": "addition",
    "extent": {
      "type": "bbox",
      "parameters": {"bbox": [0.0, 0.0, 10.0, 10.0]}
    },
    "time_extent": {
      "first": "2000-01-01T00:00:00Z",
      "last": "2030-01-01T00:00:00Z"
    },
    "components": [
      {
        "extent": {"type": "bbox", "parameters": {"bbox": [0.0, 0.0, 10.0, 10.0]}},
        "displacement_type": "horizontal",
        "uncertainty_type": "none",
        "spatial_model": {
          "type": "GeoTIFF",
          "interpolation_method": "bilinear",
          "filename": "/mem/horiz_m_coarse.tif"
        },
        "time_function": {"type": "constant"}
      },
      {
        "extent": {"type": "bbox", "parameters": {"bbox": [4.8, 4.8, 5.2, 5.2]}},
        "displacement_type": "3d",
        "uncertainty_type": "none",
        "spatial_model": {
          "type": "GeoTIFF",
          "interpolation_method": "bilinear",
          "filename": "/mem/grid3d_m_fine.tif"
        },
        "time_function": {"type": "constant"}
      }
    ]
  })json";
  fs.AddTextFile("/mem/add_m_model.json", kAdditionMetreJson);

  ScopedPj pj_add(proj_create(
      ctx.get(), "+proj=defmodel +model=/mem/add_m_model.json +ellps=WGS84"));
  ASSERT_NE(nullptr, pj_add.get());

  const PJ_COORD fwd_add = proj_trans(pj_add.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj_add.get()));
  EXPECT_NEAR(fwd.lpzt.lam, fwd_add.lpzt.lam, 1e-8);
  EXPECT_NEAR(fwd.lpzt.phi, fwd_add.lpzt.phi, 1e-8);
  EXPECT_NEAR(50.1, fwd_add.lpzt.z, 1e-6);

  // 4. Unrecognized definition_crs exercises the `if (P == nullptr) return
  // true;` fallback in EvaluatorIface::isGeographicCRS before pj_init_ctx
  // observes the inner proj_create errno on ctx.
  constexpr std::string_view kUnknownCrsJson = R"json({
    "file_type": "deformation_model_master_file",
    "format_version": "1.0",
    "source_crs": "CUSTOM_UNKNOWN_CRS",
    "target_crs": "CUSTOM_UNKNOWN_CRS",
    "definition_crs": "CUSTOM_UNKNOWN_CRS",
    "horizontal_offset_unit": "metre",
    "vertical_offset_unit": "metre",
    "horizontal_offset_method": "addition",
    "extent": {
      "type": "bbox",
      "parameters": {"bbox": [0.0, 0.0, 10.0, 10.0]}
    },
    "time_extent": {
      "first": "2000-01-01T00:00:00Z",
      "last": "2030-01-01T00:00:00Z"
    },
    "components": []
  })json";
  fs.AddTextFile("/mem/unknown_crs_model.json", kUnknownCrsJson);
  ScopedLogSuppressor suppress_logs(ctx.get());
  ScopedPj pj_unknown_crs(proj_create(
      ctx.get(),
      "+proj=defmodel +model=/mem/unknown_crs_model.json +ellps=WGS84"));
  EXPECT_EQ(nullptr, pj_unknown_crs.get());
}

TEST(DefmodelTest, ProjectedCrsModelTransformsProjectedMeters) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  constexpr float kProjPixels[] = {
      2.0f, -3.0f, 0.5f, 2.0f, -3.0f, 0.5f,
      2.0f, -3.0f, 0.5f, 2.0f, -3.0f, 0.5f,
  };
  fs.AddFile("/mem/proj_grid.tif",
             BuildMinimalGeoTiff(/*width=*/2, /*height=*/2,
                                 /*samples_per_pixel=*/3, /*west=*/500000.0,
                                 /*north=*/6010000.0, /*res_x=*/10000.0,
                                 /*res_y=*/10000.0, kProjPixels,
                                 /*is_geographic=*/false));

  constexpr std::string_view kProjectedJson = R"json({
    "file_type": "deformation_model_master_file",
    "format_version": "1.0",
    "source_crs": "EPSG:32632",
    "target_crs": "EPSG:32632",
    "definition_crs": "EPSG:32632",
    "horizontal_offset_unit": "metre",
    "vertical_offset_unit": "metre",
    "horizontal_offset_method": "addition",
    "extent": {
      "type": "bbox",
      "parameters": {"bbox": [500000.0, 6000000.0, 510000.0, 6010000.0]}
    },
    "time_extent": {
      "first": "2000-01-01T00:00:00Z",
      "last": "2030-01-01T00:00:00Z"
    },
    "components": [
      {
        "extent": {
          "type": "bbox",
          "parameters": {"bbox": [500000.0, 6000000.0, 510000.0, 6010000.0]}
        },
        "displacement_type": "3d",
        "uncertainty_type": "none",
        "spatial_model": {
          "type": "GeoTIFF",
          "interpolation_method": "bilinear",
          "filename": "/mem/proj_grid.tif"
        },
        "time_function": {"type": "constant"}
      }
    ]
  })json";
  fs.AddTextFile("/mem/proj_model.json", kProjectedJson);

  ScopedPj pj(
      proj_create(ctx.get(), "+proj=defmodel +model=/mem/proj_model.json"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(PJ_IO_UNITS_PROJECTED, pj->left);
  EXPECT_EQ(PJ_IO_UNITS_PROJECTED, pj->right);

  const PJ_COORD in = proj_coord(505000.0, 6005000.0, 100.0, 2020.0);
  const PJ_COORD fwd = proj_trans(pj.get(), PJ_FWD, in);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(505002.0, fwd.xyzt.x, 1e-6);
  EXPECT_NEAR(6004997.0, fwd.xyzt.y, 1e-6);
  EXPECT_NEAR(100.5, fwd.xyzt.z, 1e-6);

  const PJ_COORD inv = proj_trans(pj.get(), PJ_INV, fwd);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_NEAR(in.xyzt.x, inv.xyzt.x, 1e-6);
  EXPECT_NEAR(in.xyzt.y, inv.xyzt.y, 1e-6);
  EXPECT_NEAR(in.xyzt.z, inv.xyzt.z, 1e-6);
}

TEST(DefmodelTest, GridValidationAndLookupFailureBranches) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress_logs(ctx.get());
  InMemoryFileSystem fs;
  ASSERT_TRUE(fs.Install(ctx.get()));

  // Build helper grids with various invalid band counts, descriptions, or
  // units.
  constexpr float kOneSample[] = {1.0f, 1.0f, 1.0f, 1.0f};
  fs.AddFile("/mem/one_sample.tif",
             BuildMinimalGeoTiff(2, 2, 1, 0.0, 10.0, 10.0, 10.0, kOneSample));

  constexpr float kTwoSamples[] = {
      1.0f, 2.0f, 1.0f, 2.0f, 1.0f, 2.0f, 1.0f, 2.0f,
  };
  fs.AddFile("/mem/two_samples.tif",
             BuildMinimalGeoTiff(2, 2, 2, 0.0, 10.0, 10.0, 10.0, kTwoSamples));

  constexpr std::string_view kBadHorizDescXml =
      "<GDALMetadata>"
      "<Item name=\"DESCRIPTION\" sample=\"0\">east_offset</Item>"
      "<Item name=\"DESCRIPTION\" sample=\"1\">unexpected_offset</Item>"
      "</GDALMetadata>";
  fs.AddFile("/mem/bad_horiz_desc.tif",
             BuildMinimalGeoTiff(2, 2, 2, 0.0, 10.0, 10.0, 10.0, kTwoSamples,
                                 true, kBadHorizDescXml));

  constexpr std::string_view kBadHorizUnitXml =
      "<GDALMetadata>"
      "<Item name=\"UNITTYPE\" sample=\"0\">foot</Item>"
      "</GDALMetadata>";
  fs.AddFile("/mem/bad_horiz_unit.tif",
             BuildMinimalGeoTiff(2, 2, 2, 0.0, 10.0, 10.0, 10.0, kTwoSamples,
                                 true, kBadHorizUnitXml));

  constexpr std::string_view kBadVertDescXml =
      "<GDALMetadata>"
      "<Item name=\"DESCRIPTION\" sample=\"0\">wrong_vertical</Item>"
      "</GDALMetadata>";
  fs.AddFile("/mem/bad_vert_desc.tif",
             BuildMinimalGeoTiff(2, 2, 1, 0.0, 10.0, 10.0, 10.0, kOneSample,
                                 true, kBadVertDescXml));

  constexpr std::string_view kBadVertUnitXml =
      "<GDALMetadata>"
      "<Item name=\"UNITTYPE\" sample=\"0\">foot</Item>"
      "</GDALMetadata>";
  fs.AddFile("/mem/bad_vert_unit.tif",
             BuildMinimalGeoTiff(2, 2, 1, 0.0, 10.0, 10.0, 10.0, kOneSample,
                                 true, kBadVertUnitXml));

  // Grid covering only [0, 0, 2, 2] while component bbox is [0, 0, 10, 10].
  fs.AddFile("/mem/small_extent.tif",
             BuildMinimalGeoTiff(2, 2, 2, 0.0, 2.0, 2.0, 2.0, kTwoSamples));

  auto make_model = [&](std::string_view disp_type, std::string_view horiz_unit,
                        std::string_view grid_file) {
    return std::string(R"json({
      "file_type": "deformation_model_master_file",
      "format_version": "1.0",
      "source_crs": "EPSG:4326",
      "target_crs": "EPSG:4326",
      "definition_crs": "EPSG:4326",
      "horizontal_offset_unit": ")json") +
           std::string(horiz_unit) + R"json(",
      "vertical_offset_unit": "metre",
      "horizontal_offset_method": "addition",
      "extent": {"type": "bbox", "parameters": {"bbox": [0.0, 0.0, 10.0, 10.0]}},
      "time_extent": {
        "first": "2000-01-01T00:00:00Z",
        "last": "2030-01-01T00:00:00Z"
      },
      "components": [{
        "extent": {"type": "bbox", "parameters": {"bbox": [0.0, 0.0, 10.0, 10.0]}},
        "displacement_type": ")json" +
           std::string(disp_type) + R"json(",
        "uncertainty_type": "none",
        "spatial_model": {
          "type": "GeoTIFF",
          "interpolation_method": "bilinear",
          "filename": ")json" +
           std::string(grid_file) + R"json("
        },
        "time_function": {"type": "constant"}
      }]
    })json";
  };

  struct FailureCase {
    std::string_view name;
    std::string_view disp_type;
    std::string_view horiz_unit;
    std::string_view grid_file;
  };
  constexpr FailureCase kFailureCases[] = {
      // EvaluatorIface::open failure (missing grid file).
      {"/mem/m1.json", "horizontal", "degree", "/mem/does_not_exist.tif"},
      // checkHorizontal: samplesPerPixel < 2 (via getLongLatOffset &
      // getEastingNorthingOffset).
      {"/mem/m2.json", "horizontal", "degree", "/mem/one_sample.tif"},
      {"/mem/m3.json", "horizontal", "metre", "/mem/one_sample.tif"},
      // checkHorizontal: foundDesc && (!foundDescX || !foundDescY).
      {"/mem/m4.json", "horizontal", "degree", "/mem/bad_horiz_desc.tif"},
      // checkHorizontal: unit != expectedUnit.
      {"/mem/m5.json", "horizontal", "degree", "/mem/bad_horiz_unit.tif"},
      // getZOffset: samplesPerPixel == 2 (< 3 and != 1) in vertical and 3d
      // modes.
      {"/mem/m6.json", "vertical", "degree", "/mem/two_samples.tif"},
      {"/mem/m7.json", "3d", "degree", "/mem/two_samples.tif"},
      {"/mem/m8.json", "3d", "metre", "/mem/two_samples.tif"},
      // getZOffset: foundDesc && !foundDescZ.
      {"/mem/m9.json", "vertical", "degree", "/mem/bad_vert_desc.tif"},
      // getZOffset: unit != "metre".
      {"/mem/m10.json", "vertical", "degree", "/mem/bad_vert_unit.tif"},
  };

  const PJ_COORD pt = proj_coord(DegToRad(5.0), DegToRad(5.0), 0.0, 2020.0);
  for (const auto& tc : kFailureCases) {
    fs.AddTextFile(std::string(tc.name),
                   make_model(tc.disp_type, tc.horiz_unit, tc.grid_file));
    const std::string proj_str =
        "+proj=defmodel +model=" + std::string(tc.name);
    ScopedPj pj(proj_create(ctx.get(), proj_str.c_str()));
    ASSERT_NE(nullptr, pj.get()) << tc.name;
    const PJ_COORD out = proj_trans(pj.get(), PJ_FWD, pt);
    EXPECT_TRUE(std::isinf(out.xyzt.x)) << tc.name;
  }

  // Point inside component bbox [0, 0, 10, 10] but outside actual grid extent
  // [0, 0, 2, 2] triggers GridSet::gridAt returning nullptr and skips
  // component.
  fs.AddTextFile("/mem/small_ext.json",
                 make_model("horizontal", "degree", "/mem/small_extent.tif"));
  ScopedPj pj_small(
      proj_create(ctx.get(), "+proj=defmodel +model=/mem/small_ext.json"));
  ASSERT_NE(nullptr, pj_small.get());
  const PJ_COORD out_skipped = proj_trans(pj_small.get(), PJ_FWD, pt);
  EXPECT_EQ(0, proj_errno(pj_small.get()));
  EXPECT_NEAR(pt.lpzt.lam, out_skipped.lpzt.lam, 1e-12);
}

TEST(DefmodelTest, MasterFileParseMetadataTimeFunctionsAndPrototypes) {
  constexpr std::string_view kFullJson = R"json({
    "file_type": "deformation_model_master_file",
    "format_version": "1.0",
    "name": "Test Deformation Model",
    "version": "2026.1",
    "license": "CC-BY-4.0",
    "description": "Synthetic test model",
    "publication_date": "2026-01-15",
    "authority": {
      "name": "Test Agency",
      "url": "https://example.com",
      "address": "123 Geodesy Way",
      "email": "info@example.com"
    },
    "links": [
      {
        "href": "https://example.com/about",
        "rel": "about",
        "type": "text/html",
        "title": "About"
      }
    ],
    "source_crs": "EPSG:4326",
    "target_crs": "EPSG:4326",
    "definition_crs": "EPSG:4326",
    "reference_epoch": "2020-01-01T00:00:00Z",
    "uncertainty_reference_epoch": "2020-01-01T00:00:00Z",
    "horizontal_offset_unit": "metre",
    "vertical_offset_unit": "metre",
    "horizontal_uncertainty_type": "circular 95%",
    "horizontal_uncertainty_unit": "metre",
    "vertical_uncertainty_type": "linear 95%",
    "vertical_uncertainty_unit": "metre",
    "horizontal_offset_method": "addition",
    "extent": {
      "type": "bbox",
      "parameters": {"bbox": [170.0, -48.0, 180.0, -34.0]}
    },
    "time_extent": {
      "first": "1900-01-01T00:00:00Z",
      "last": "2050-01-01T00:00:00Z"
    },
    "components": [
      {
        "description": "reverse step",
        "extent": {"type": "bbox", "parameters": {"bbox": [170.0, -48.0, 180.0, -34.0]}},
        "displacement_type": "vertical",
        "uncertainty_type": "vertical",
        "horizontal_uncertainty": 0.01,
        "vertical_uncertainty": 0.02,
        "spatial_model": {
          "type": "GeoTIFF",
          "interpolation_method": "bilinear",
          "filename": "rev_step.tif",
          "md5_checksum": "d41d8cd98f00b204e9800998ecf8427e"
        },
        "time_function": {
          "type": "reverse_step",
          "parameters": {"step_epoch": "2010-01-01T00:00:00Z"}
        }
      },
      {
        "description": "piecewise linear",
        "extent": {"type": "bbox", "parameters": {"bbox": [170.0, -48.0, 180.0, -34.0]}},
        "displacement_type": "horizontal",
        "uncertainty_type": "none",
        "spatial_model": {
          "type": "GeoTIFF",
          "interpolation_method": "bilinear",
          "filename": "piecewise.tif"
        },
        "time_function": {
          "type": "piecewise",
          "parameters": {
            "before_first": "linear",
            "after_last": "linear",
            "model": [
              {"epoch": "2000-01-01T00:00:00Z", "scale_factor": 0.0},
              {"epoch": "2010-01-01T00:00:00Z", "scale_factor": 2.0},
              {"epoch": "2010-01-01T00:00:00Z", "scale_factor": 5.0},
              {"epoch": "2020-01-01T00:00:00Z", "scale_factor": 10.0}
            ]
          }
        }
      },
      {
        "description": "exponential decay",
        "extent": {"type": "bbox", "parameters": {"bbox": [170.0, -48.0, 180.0, -34.0]}},
        "displacement_type": "3d",
        "uncertainty_type": "none",
        "spatial_model": {
          "type": "GeoTIFF",
          "interpolation_method": "bilinear",
          "filename": "exp.tif"
        },
        "time_function": {
          "type": "exponential",
          "parameters": {
            "reference_epoch": "2010-01-01T00:00:00Z",
            "end_epoch": "2020-01-01T00:00:00Z",
            "relaxation_constant": 5.0,
            "before_scale_factor": -1.0,
            "initial_scale_factor": 1.0,
            "final_scale_factor": 3.0
          }
        }
      }
    ]
  })json";

  auto model = DeformationModel::MasterFile::parse(std::string(kFullJson));
  ASSERT_NE(nullptr, model.get());
  EXPECT_EQ("deformation_model_master_file", model->fileType());
  EXPECT_EQ("1.0", model->formatVersion());
  EXPECT_EQ("Test Deformation Model", model->name());
  EXPECT_EQ("2026.1", model->version());
  EXPECT_EQ("CC-BY-4.0", model->license());
  EXPECT_EQ("Synthetic test model", model->description());
  EXPECT_EQ("2026-01-15", model->publicationDate());
  EXPECT_EQ("Test Agency", model->authority().name);
  EXPECT_EQ("https://example.com", model->authority().url);
  EXPECT_EQ("123 Geodesy Way", model->authority().address);
  EXPECT_EQ("info@example.com", model->authority().email);
  ASSERT_EQ(1U, model->links().size());
  EXPECT_EQ("https://example.com/about", model->links()[0].href);
  EXPECT_EQ("EPSG:4326", model->sourceCRS());
  EXPECT_EQ("EPSG:4326", model->targetCRS());
  EXPECT_EQ("EPSG:4326", model->definitionCRS());
  EXPECT_EQ("2020-01-01T00:00:00Z", model->referenceEpoch());
  EXPECT_EQ("2020-01-01T00:00:00Z", model->uncertaintyReferenceEpoch());
  EXPECT_EQ("metre", model->horizontalOffsetUnit());
  EXPECT_EQ("metre", model->verticalOffsetUnit());
  EXPECT_EQ("circular 95%", model->horizontalUncertaintyType());
  EXPECT_EQ("metre", model->horizontalUncertaintyUnit());
  EXPECT_EQ("linear 95%", model->verticalUncertaintyType());
  EXPECT_EQ("metre", model->verticalUncertaintyUnit());
  EXPECT_EQ("addition", model->horizontalOffsetMethod());
  EXPECT_DOUBLE_EQ(170.0, model->extent().minx());
  EXPECT_DOUBLE_EQ(-48.0, model->extent().miny());
  EXPECT_DOUBLE_EQ(180.0, model->extent().maxx());
  EXPECT_DOUBLE_EQ(-34.0, model->extent().maxy());
  EXPECT_DOUBLE_EQ(1900.0, model->timeExtent().first.toDecimalYear());

  ASSERT_EQ(3U, model->components().size());
  const auto& c0 = model->components()[0];
  EXPECT_EQ("reverse step", c0.description());
  EXPECT_EQ("vertical", c0.uncertaintyType());
  EXPECT_DOUBLE_EQ(0.01, c0.horizontalUncertainty());
  EXPECT_DOUBLE_EQ(0.02, c0.verticalUncertainty());
  EXPECT_EQ("d41d8cd98f00b204e9800998ecf8427e", c0.spatialModel().md5Checksum);
  EXPECT_DOUBLE_EQ(-1.0, c0.timeFunction()->evaluateAt(2005.0));
  EXPECT_DOUBLE_EQ(0.0, c0.timeFunction()->evaluateAt(2015.0));

  const auto& c1 = model->components()[1];
  EXPECT_NEAR(-1.0, c1.timeFunction()->evaluateAt(1995.0), 1e-9);
  EXPECT_NEAR(1.0, c1.timeFunction()->evaluateAt(2005.0), 1e-9);
  EXPECT_NEAR(7.5, c1.timeFunction()->evaluateAt(2015.0), 1e-9);
  EXPECT_NEAR(12.5, c1.timeFunction()->evaluateAt(2025.0), 1e-9);

  const auto& c2 = model->components()[2];
  EXPECT_DOUBLE_EQ(-1.0, c2.timeFunction()->evaluateAt(2005.0));
  EXPECT_DOUBLE_EQ(1.0, c2.timeFunction()->evaluateAt(2010.0));
  const double at_2020 = c2.timeFunction()->evaluateAt(2020.0);
  const double at_2030 = c2.timeFunction()->evaluateAt(2030.0);
  EXPECT_DOUBLE_EQ(at_2020, at_2030);

  // Verify default prototype methods throw UnimplementedException.
  DeformationModel::GridPrototype grid_proto;
  double dummy1 = 0.0;
  double dummy2 = 0.0;
  double dummy3 = 0.0;
  EXPECT_THROW(grid_proto.getLongLatOffset(0, 0, dummy1, dummy2),
               DeformationModel::UnimplementedException);
  EXPECT_THROW(grid_proto.getZOffset(0, 0, dummy1),
               DeformationModel::UnimplementedException);
  EXPECT_THROW(grid_proto.getEastingNorthingOffset(0, 0, dummy1, dummy2),
               DeformationModel::UnimplementedException);
  EXPECT_THROW(grid_proto.getLongLatZOffset(0, 0, dummy1, dummy2, dummy3),
               DeformationModel::UnimplementedException);
  EXPECT_THROW(
      grid_proto.getEastingNorthingZOffset(0, 0, dummy1, dummy2, dummy3),
      DeformationModel::UnimplementedException);

  DeformationModel::GridSetPrototype<> gridset_proto;
  EXPECT_THROW(gridset_proto.gridAt(0.0, 0.0),
               DeformationModel::UnimplementedException);

  DeformationModel::EvaluatorIfacePrototype<> iface_proto;
  EXPECT_THROW(iface_proto.open("foo"),
               DeformationModel::UnimplementedException);
  EXPECT_THROW(iface_proto.geographicToGeocentric(0, 0, 0, 1, 1, 0, dummy1,
                                                  dummy2, dummy3),
               DeformationModel::UnimplementedException);
  EXPECT_THROW(iface_proto.geocentricToGeographic(0, 0, 0, 1, 1, 0, dummy1,
                                                  dummy2, dummy3),
               DeformationModel::UnimplementedException);
  EXPECT_THROW(iface_proto.isGeographicCRS("EPSG:4326"),
               DeformationModel::UnimplementedException);
}

}  // namespace
}  // namespace proj
