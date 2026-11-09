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
// Tests map access property list class registration, registered property
// defaults/encoding, inherited link access behavior, and Google3 H5_HAVE_MAP_API
// disabled configuration (third_party/hdf5/src/H5Pmapl.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pmapl.c

#if !defined(NDEBUG)
#define NDEBUG 1
#define HDF5_TEST_UNDEF_NDEBUG_ 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5P_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Mprivate.h"
#include "third_party/hdf5/src/H5Ppkg.h"
}  // extern "C"
#if defined(HDF5_TEST_UNDEF_NDEBUG_)
#undef NDEBUG
#undef HDF5_TEST_UNDEF_NDEBUG_
#endif

#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

constexpr size_t kExpectedDefaultPrefetchSize = 16u * 1024u;
constexpr size_t kExpectedDefaultAllocSize = 1024u * 1024u;

TEST(H5PmaplTest, MapApiIsDisabledInGoogle3Build) {
  // H5Pset_map_iterate_hints / H5Pget_map_iterate_hints are gated under
  // #ifdef H5_HAVE_MAP_API in H5Pmapl.c (undef in H5pubconf.h).
#if defined(H5_HAVE_MAP_API)
  ADD_FAILURE() << "Expected H5_HAVE_MAP_API to be disabled in Google3";
#endif
}

TEST(H5PmaplTest, ClassMetadataAndRegisteredProperties) {
  char* class_name = H5Pget_class_name(H5P_MAP_ACCESS);
  ASSERT_NE(class_name, nullptr);
  absl::Cleanup free_name = [class_name] { H5free_memory(class_name); };
  EXPECT_STREQ(class_name, "map access");

  ScopedPlist parent(H5Pget_class_parent(H5P_MAP_ACCESS));
  ASSERT_TRUE(parent.valid());
  EXPECT_GT(H5Pequal(parent.get(), H5P_LINK_ACCESS), 0);

  EXPECT_GT(H5Pisa_class(H5P_MAP_ACCESS_DEFAULT, H5P_MAP_ACCESS), 0);
  EXPECT_GT(H5Pisa_class(H5P_MAP_ACCESS_DEFAULT, H5P_LINK_ACCESS), 0);

  // Verify H5P__macc_reg_prop registered both map access size_t properties.
  EXPECT_GT(H5Pexist(H5P_MAP_ACCESS, H5M_ACS_KEY_PREFETCH_SIZE_NAME), 0);
  EXPECT_GT(H5Pexist(H5P_MAP_ACCESS, H5M_ACS_KEY_ALLOC_SIZE_NAME), 0);

  size_t prefetch_prop_size = 0;
  size_t alloc_prop_size = 0;
  ASSERT_GE(H5Pget_size(H5P_MAP_ACCESS_DEFAULT, H5M_ACS_KEY_PREFETCH_SIZE_NAME,
                        &prefetch_prop_size),
            0);
  ASSERT_GE(H5Pget_size(H5P_MAP_ACCESS_DEFAULT, H5M_ACS_KEY_ALLOC_SIZE_NAME,
                        &alloc_prop_size),
            0);
  EXPECT_EQ(prefetch_prop_size, sizeof(size_t));
  EXPECT_EQ(alloc_prop_size, sizeof(size_t));

  // Verify internal H5P_CLS_MACC descriptor aligns with public class handles.
  EXPECT_STREQ(H5P_CLS_MACC[0].name, "map access");
  EXPECT_EQ(H5P_CLS_MACC[0].type, H5P_TYPE_MAP_ACCESS);
}

TEST(H5PmaplTest, DefaultAndModifiedMapPropertiesAndParentInheritance) {
  ScopedPlist mapl(H5Pcreate(H5P_MAP_ACCESS));
  ASSERT_TRUE(mapl.valid());

  H5P_genplist_t* def_plist =
      H5P_object_verify(H5P_MAP_ACCESS_DEFAULT, H5P_MAP_ACCESS, true);
  ASSERT_NE(def_plist, nullptr);

  size_t prefetch_size = 0;
  size_t alloc_size = 0;
  ASSERT_GE(
      H5P_get(def_plist, H5M_ACS_KEY_PREFETCH_SIZE_NAME, &prefetch_size), 0);
  ASSERT_GE(H5P_get(def_plist, H5M_ACS_KEY_ALLOC_SIZE_NAME, &alloc_size), 0);
  EXPECT_EQ(prefetch_size, kExpectedDefaultPrefetchSize);
  EXPECT_EQ(alloc_size, kExpectedDefaultAllocSize);

  H5P_genplist_t* plist = H5P_object_verify(mapl.get(), H5P_MAP_ACCESS, false);
  ASSERT_NE(plist, nullptr);

  constexpr size_t kCustomPrefetch = 4096u;
  constexpr size_t kCustomAlloc = 262144u;
  ASSERT_GE(
      H5P_set(plist, H5M_ACS_KEY_PREFETCH_SIZE_NAME, &kCustomPrefetch), 0);
  ASSERT_GE(H5P_set(plist, H5M_ACS_KEY_ALLOC_SIZE_NAME, &kCustomAlloc), 0);
  EXPECT_EQ(H5Pequal(mapl.get(), H5P_MAP_ACCESS_DEFAULT), 0);

  // Inherited H5P_LINK_ACCESS properties remain functional on MAPL.
  ASSERT_GE(H5Pset_nlinks(mapl.get(), 32u), 0);
  size_t nlinks = 0;
  ASSERT_GE(H5Pget_nlinks(mapl.get(), &nlinks), 0);
  EXPECT_EQ(nlinks, 32u);

  ScopedPlist copied(H5Pcopy(mapl.get()));
  ASSERT_TRUE(copied.valid());
  EXPECT_GT(H5Pequal(mapl.get(), copied.get()), 0);

  size_t encoded_size = 0;
  ASSERT_GE(H5Pencode2(mapl.get(), nullptr, &encoded_size, H5P_DEFAULT), 0);
  ASSERT_GT(encoded_size, 0u);
  std::vector<uint8_t> encoded(encoded_size, 0);
  ASSERT_GE(H5Pencode2(mapl.get(), encoded.data(), &encoded_size, H5P_DEFAULT),
            0);

  ScopedPlist decoded(H5Pdecode(encoded.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pisa_class(decoded.get(), H5P_MAP_ACCESS), 0);
  EXPECT_GT(H5Pequal(mapl.get(), decoded.get()), 0);

  H5P_genplist_t* decoded_plist =
      H5P_object_verify(decoded.get(), H5P_MAP_ACCESS, false);
  ASSERT_NE(decoded_plist, nullptr);
  ASSERT_GE(
      H5P_get(decoded_plist, H5M_ACS_KEY_PREFETCH_SIZE_NAME, &prefetch_size),
      0);
  ASSERT_GE(H5P_get(decoded_plist, H5M_ACS_KEY_ALLOC_SIZE_NAME, &alloc_size),
            0);
  EXPECT_EQ(prefetch_size, kCustomPrefetch);
  EXPECT_EQ(alloc_size, kCustomAlloc);
}

TEST(H5PmaplTest, RegPropRejectsDuplicatePropertyNames) {
  size_t def_val = 0;

  ScopedH5Id<H5Pclose_class> cls_prefetch(
      H5Pcreate_class(H5P_ROOT, "dup_macc_prefetch", nullptr, nullptr, nullptr,
                      nullptr, nullptr, nullptr));
  ASSERT_TRUE(cls_prefetch.valid());
  ASSERT_GE(H5Pregister2(cls_prefetch.get(), H5M_ACS_KEY_PREFETCH_SIZE_NAME,
                         sizeof(size_t), &def_val, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr),
            0);
  H5P_genclass_t* pclass_prefetch = static_cast<H5P_genclass_t*>(
      H5I_object_verify(cls_prefetch.get(), H5I_GENPROP_CLS));
  ASSERT_NE(pclass_prefetch, nullptr);

  ScopedH5Id<H5Pclose_class> cls_alloc(
      H5Pcreate_class(H5P_ROOT, "dup_macc_alloc", nullptr, nullptr, nullptr,
                      nullptr, nullptr, nullptr));
  ASSERT_TRUE(cls_alloc.valid());
  ASSERT_GE(H5Pregister2(cls_alloc.get(), H5M_ACS_KEY_ALLOC_SIZE_NAME,
                         sizeof(size_t), &def_val, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr),
            0);
  H5P_genclass_t* pclass_alloc = static_cast<H5P_genclass_t*>(
      H5I_object_verify(cls_alloc.get(), H5I_GENPROP_CLS));
  ASSERT_NE(pclass_alloc, nullptr);

  ScopedErrorSilence silence;
  EXPECT_LT(H5P_CLS_MACC[0].reg_prop_func(pclass_prefetch), 0);
  EXPECT_LT(H5P_CLS_MACC[0].reg_prop_func(pclass_alloc), 0);
}

TEST(H5PmaplTest, ReinitExercisesClassRegistrationCallback) {
  ASSERT_GE(H5close(), 0);
  ASSERT_GE(H5open(), 0);

  ScopedPlist mapl(H5Pcreate(H5P_MAP_ACCESS));
  ASSERT_TRUE(mapl.valid());
  EXPECT_GT(H5Pexist(mapl.get(), H5M_ACS_KEY_PREFETCH_SIZE_NAME), 0);
  EXPECT_GT(H5Pexist(mapl.get(), H5M_ACS_KEY_ALLOC_SIZE_NAME), 0);
}

}  // namespace
}  // namespace hdf5
