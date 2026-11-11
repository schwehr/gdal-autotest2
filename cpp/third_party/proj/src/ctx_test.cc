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
// Unit tests for PROJ context lifecycle, cloning, error handling, search paths,
// and init-rule configuration (proj/src/ctx.cpp). Note: in google3 default
// non-GCE builds, pj_ctx::createDefault() installs google3_proj4_hooks and
// proj.db is embedded in memory.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/ctx.cpp

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

using ::testing::IsEmpty;
using ::testing::NotNull;

class ScopedEnvVar {
 public:
  explicit ScopedEnvVar(const char* name) : name_(name) {
    if (const char* existing = std::getenv(name_)) {
      original_ = existing;
    }
  }

  ~ScopedEnvVar() {
    if (original_.has_value()) {
      setenv(name_, original_->c_str(), /*replace=*/1);
    } else {
      unsetenv(name_);
    }
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

  void Set(const char* value) const { setenv(name_, value, /*replace=*/1); }
  void Unset() const { unsetenv(name_); }

 private:
  const char* name_;
  std::optional<std::string> original_;
};

TEST(CtxTest, CreateCloneAndDestroyContextBranches) {
  // 1. Destroying nullptr or the singleton default context is a safe no-op.
  EXPECT_EQ(nullptr, proj_context_destroy(nullptr));
  EXPECT_EQ(nullptr, proj_context_destroy(pj_get_default_ctx()));

  // 2. Fresh context creation produces distinct instances from default ctx.
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  EXPECT_NE(pj_get_default_ctx(), ctx.get());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  // 3. Cloning nullptr creates a fresh default-initialized context.
  ScopedContext from_null(proj_context_clone(nullptr));
  ASSERT_NE(nullptr, from_null.get());

  // 4. Cloning without and with an initialized cpp_context preserves settings.
  ctx->set_ca_bundle_path("/virtual/ca-bundle.crt");
  const std::vector<std::string> paths = {"/virtual/dir_a", "/virtual/dir_b"};
  ctx->set_search_paths(paths);
  EXPECT_EQ("/virtual/ca-bundle.crt", ctx->ca_bundle_path);
  ASSERT_EQ(2u, ctx->search_paths.size());
  ASSERT_NE(nullptr, ctx->c_compat_paths);
  EXPECT_STREQ("/virtual/dir_a", ctx->c_compat_paths[0]);

  ScopedContext cloned_without_cpp(proj_context_clone(ctx.get()));
  ASSERT_NE(nullptr, cloned_without_cpp.get());
  EXPECT_EQ("/virtual/ca-bundle.crt", cloned_without_cpp->ca_bundle_path);
  EXPECT_EQ(2u, cloned_without_cpp->search_paths.size());

  // Trigger lazy cpp_context initialization, then clone again.
  ASSERT_THAT(ctx->get_cpp_context(), NotNull());
  ScopedContext cloned_with_cpp(proj_context_clone(ctx.get()));
  ASSERT_NE(nullptr, cloned_with_cpp.get());
  EXPECT_NE(ctx->get_cpp_context(), cloned_with_cpp->get_cpp_context());

  // Clearing search paths frees c_compat_paths cleanly.
  ctx->set_search_paths({});
  EXPECT_EQ(nullptr, ctx->c_compat_paths);
  EXPECT_THAT(ctx->search_paths, IsEmpty());
}

TEST(CtxTest, ErrnoSetResetRestoreAndPjGetCtx) {
  ScopedContext ctx_a = MakeContext();
  ScopedContext ctx_b = MakeContext();
  ASSERT_NE(nullptr, ctx_a.get());
  ASSERT_NE(nullptr, ctx_b.get());

  // Reset default context cleanly at scope exit in case a null-PJ probe runs.
  absl::Cleanup reset_default = [] { proj_errno_reset(nullptr); };

  ScopedPj pj(proj_create(ctx_a.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  EXPECT_EQ(ctx_a.get(), pj_get_ctx(pj.get()));

  // pj_get_ctx falls back to the default context for nullptr or null pj->ctx.
  EXPECT_EQ(pj_get_default_ctx(), pj_get_ctx(nullptr));
  pj->ctx = nullptr;
  EXPECT_EQ(pj_get_default_ctx(), pj_get_ctx(pj.get()));
  pj->ctx = ctx_a.get();

  // proj_errno_set(P, 0) is an intentional no-op and must not clear errno.
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
            proj_errno_set(pj.get(), PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_errno(pj.get()));
  EXPECT_EQ(0, proj_errno_set(pj.get(), 0));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_errno(pj.get()));

  // proj_errno_reset clears both context last_errno and thread-local errno,
  // returning the previous errno; proj_errno_restore ignores 0 and restores
  // non-zero values.
  const int prev = proj_errno_reset(pj.get());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, prev);
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(0, errno);

  EXPECT_EQ(0, proj_errno_restore(pj.get(), 0));
  EXPECT_EQ(0, proj_errno(pj.get()));
  EXPECT_EQ(0, proj_errno_restore(pj.get(), prev));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE, proj_errno(pj.get()));
  proj_errno_reset(pj.get());

  // Reassigning context updates pj->ctx and children/alternatives.
  proj_assign_context(nullptr, ctx_b.get());
  proj_assign_context(pj.get(), ctx_b.get());
  EXPECT_EQ(ctx_b.get(), pj_get_ctx(pj.get()));

  ScopedPj crs_to_crs(
      proj_create_crs_to_crs(ctx_a.get(), "EPSG:4326", "EPSG:3857", nullptr));
  ASSERT_NE(nullptr, crs_to_crs.get());
  proj_assign_context(crs_to_crs.get(), ctx_b.get());
  EXPECT_EQ(ctx_b.get(), pj_get_ctx(crs_to_crs.get()));
}

TEST(CtxTest, CreateDefaultParsesProjDebugEnvVar) {
  ScopedEnvVar env_debug("PROJ_DEBUG");

  struct DebugCase {
    const char* env_value;
    int expected_level;
  };
  constexpr DebugCase kCases[] = {
      {.env_value = "ON", .expected_level = PJ_LOG_DEBUG},
      {.env_value = "OFF", .expected_level = PJ_LOG_ERROR},
      {.env_value = "3", .expected_level = PJ_LOG_TRACE},
      {.env_value = "99", .expected_level = 99},
      {.env_value = "-2", .expected_level = -2},
      {.env_value = "-99", .expected_level = PJ_LOG_TRACE},
      {.env_value = "invalid_level", .expected_level = PJ_LOG_ERROR},
  };

  for (const auto& tc : kCases) {
    env_debug.Set(tc.env_value);
    const pj_ctx ctx = pj_ctx::createDefault();
    EXPECT_EQ(tc.expected_level, ctx.debug_level) << tc.env_value;
  }

  env_debug.Unset();
  const pj_ctx default_ctx = pj_ctx::createDefault();
  EXPECT_EQ(PJ_LOG_ERROR, default_ctx.debug_level);
}

TEST(CtxTest, Proj4InitRulesHonorsContextAndEnvVar) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogSuppressor suppress(ctx.get());
  ScopedEnvVar env_rules("PROJ_USE_PROJ4_INIT_RULES");

  env_rules.Unset();
  EXPECT_EQ(0, proj_context_get_use_proj4_init_rules(ctx.get(), 0));
  EXPECT_EQ(1, proj_context_get_use_proj4_init_rules(ctx.get(), 1));

  proj_context_use_proj4_init_rules(ctx.get(), 1);
  EXPECT_EQ(1, proj_context_get_use_proj4_init_rules(ctx.get(), 0));
  proj_context_use_proj4_init_rules(ctx.get(), 0);
  EXPECT_EQ(0, proj_context_get_use_proj4_init_rules(ctx.get(), 1));

  // Verify nullptr context branch updates and restores default context cleanly.
  const int prev_default = proj_context_get_use_proj4_init_rules(nullptr, -1);
  absl::Cleanup restore_default = [prev_default] {
    proj_context_use_proj4_init_rules(nullptr, prev_default);
  };
  proj_context_use_proj4_init_rules(nullptr, 1);
  EXPECT_EQ(1, proj_context_get_use_proj4_init_rules(nullptr, 0));

  constexpr const char* kTruthy[] = {"yes", "on", "true"};
  for (const char* v : kTruthy) {
    env_rules.Set(v);
    EXPECT_EQ(1, proj_context_get_use_proj4_init_rules(ctx.get(), 0)) << v;
  }

  constexpr const char* kFalsy[] = {"no", "off", "false"};
  for (const char* v : kFalsy) {
    env_rules.Set(v);
    EXPECT_EQ(0, proj_context_get_use_proj4_init_rules(ctx.get(), 1)) << v;
  }

  env_rules.Set("not_a_boolean");
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);
  EXPECT_EQ(0, proj_context_get_use_proj4_init_rules(ctx.get(), 1));
  EXPECT_TRUE(capture.ContainsMessage("PROJ_USE_PROJ4_INIT_RULES"));
}

}  // namespace
}  // namespace proj
