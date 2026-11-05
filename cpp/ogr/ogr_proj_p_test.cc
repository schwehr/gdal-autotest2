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

#include <string>

// clang-format off
#include "third_party/gdal/port/cpl_port.h"  // IWYU pragma: keep
#include "third_party/gdal/ogr/ogr_proj_p.h"
// clang-format on
#include "proj.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

class ProjEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    char** paths = OSRGetPROJSearchPaths();
    if (paths != nullptr && paths[0] != nullptr) {
      initial_path_ = paths[0];
    }
    CSLDestroy(paths);

    if (!initial_path_.empty()) {
      CPLSetConfigOption("PROJ_LIB", initial_path_.c_str());
    }
    OSRGetPROJEnableNetwork();
    CPLSetConfigOption("PROJ_LIB", nullptr);
  }

 private:
  std::string initial_path_;
};

testing::Environment* const kProjEnv =
    testing::AddGlobalTestEnvironment(new ProjEnvironment);

TEST(OgrProjPTest, ProjVersion) {
  int major = 0;
  int minor = 0;
  int patch = 0;
  OSRGetPROJVersion(&major, &minor, &patch);
  EXPECT_GE(major, 6);

  // Null pointers
  OSRGetPROJVersion(nullptr, nullptr, nullptr);
}

TEST(OgrProjPTest, SearchPaths) {
  char** orig_paths = OSRGetPROJSearchPaths();
  auto cleanup_paths = absl::MakeCleanup([orig_paths] {
    if (orig_paths != nullptr) {
      OSRSetPROJSearchPaths(orig_paths);
      CSLDestroy(orig_paths);
    }
    OSRGetProjTLSContext();
  });

  const char* const kNewPaths[] = {"/vsimem/fake_proj_dir", nullptr};
  OSRSetPROJSearchPaths(kNewPaths);

  char** cur_paths = OSRGetPROJSearchPaths();
  ASSERT_NE(nullptr, cur_paths);
  EXPECT_STREQ("/vsimem/fake_proj_dir", cur_paths[0]);
  CSLDestroy(cur_paths);

  // Sync with context
  EXPECT_NE(nullptr, OSRGetProjTLSContext());

  // Via CPLSetConfigOption PROJ_DATA
  CPLSetConfigOption("PROJ_DATA", "/vsimem/fake_proj_data");
  cur_paths = OSRGetPROJSearchPaths();
  ASSERT_NE(nullptr, cur_paths);
  EXPECT_STREQ("/vsimem/fake_proj_data", cur_paths[0]);
  CSLDestroy(cur_paths);
  EXPECT_NE(nullptr, OSRGetProjTLSContext());
  CPLSetConfigOption("PROJ_DATA", nullptr);
}

TEST(OgrProjPTest, AuxDbPaths) {
  const char* const kAuxPaths[] = {"/vsimem/aux_db.db", nullptr};
  OSRSetPROJAuxDbPaths(kAuxPaths);

  char** cur_aux = OSRGetPROJAuxDbPaths();
  ASSERT_NE(nullptr, cur_aux);
  EXPECT_STREQ("/vsimem/aux_db.db", cur_aux[0]);
  CSLDestroy(cur_aux);

  // Context sync
  {
    WithQuietHandler quiet_handler;
    EXPECT_NE(nullptr, OSRGetProjTLSContext());
  }

  OSRSetPROJAuxDbPaths(nullptr);
}

TEST(OgrProjPTest, EnableNetwork) {
  OSRSetPROJEnableNetwork(TRUE);
  EXPECT_EQ(TRUE, OSRGetPROJEnableNetwork());
  EXPECT_NE(nullptr, OSRGetProjTLSContext());

  OSRSetPROJEnableNetwork(FALSE);
  EXPECT_EQ(FALSE, OSRGetPROJEnableNetwork());
  EXPECT_NE(nullptr, OSRGetProjTLSContext());
}

TEST(OgrProjPTest, TLSCache) {
  OSRProjTLSCache* cache = OSRGetProjTLSCache();
  ASSERT_NE(nullptr, cache);

  PJ* pj = proj_create(OSRGetProjTLSContext(), "+proj=longlat +datum=WGS84");
  ASSERT_NE(nullptr, pj);
  auto cleanup_pj = absl::MakeCleanup([pj] { proj_destroy(pj); });

  // EPSG code caching
  EXPECT_EQ(nullptr, cache->GetPJForEPSGCode(4326, false, false));
  cache->CachePJForEPSGCode(4326, false, false, pj);
  PJ* cached_epsg = cache->GetPJForEPSGCode(4326, false, false);
  ASSERT_NE(nullptr, cached_epsg);
  proj_destroy(cached_epsg);

  // WKT caching
  EXPECT_EQ(nullptr, cache->GetPJForWKT("TEST_WKT"));
  cache->CachePJForWKT("TEST_WKT", pj);
  PJ* cached_wkt = cache->GetPJForWKT("TEST_WKT");
  ASSERT_NE(nullptr, cached_wkt);
  proj_destroy(cached_wkt);

  // Clear cache and re-insert (exercises GetPJContext on cleared cache)
  cache->clear();
  EXPECT_EQ(nullptr, cache->GetPJForWKT("TEST_WKT"));
  cache->CachePJForWKT("TEST_WKT2", pj);
  cache->clear();
}

TEST(OgrProjPTest, CleanupTLSContextAndLogger) {
  OSRCleanupTLSContext();
  PJ_CONTEXT* ctx = OSRGetProjTLSContext();
  ASSERT_NE(nullptr, ctx);

  // Logger test: set level to TRACE to exercise all levels
  proj_log_level(ctx, PJ_LOG_TRACE);
  {
    WithQuietHandler quiet_handler;
    PJ* bad = proj_create(ctx, "+proj=nonexistent_proj_xyz");
    EXPECT_EQ(nullptr, bad);

    PJ* good = proj_create(ctx, "+proj=utm +zone=31 +datum=WGS84");
    if (good != nullptr) {
      proj_destroy(good);
    }
  }
  proj_log_level(ctx, PJ_LOG_ERROR);
}

}  // namespace
}  // namespace autotest2
