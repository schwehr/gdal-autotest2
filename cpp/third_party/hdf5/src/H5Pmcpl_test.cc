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
// Tests map creation property list class initialization and inheritance
// (third_party/hdf5/src/H5Pmcpl.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pmcpl.c

#include "third_party/hdf5/src/hdf5.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

TEST(H5PmcplTest, ClassMetadataAndInheritanceMatchObjectCreate) {
  char* class_name = H5Pget_class_name(H5P_MAP_CREATE);
  ASSERT_NE(class_name, nullptr);
  absl::Cleanup free_name = [class_name] { H5free_memory(class_name); };
  EXPECT_STREQ(class_name, "map create");

  ScopedPlist parent(H5Pget_class_parent(H5P_MAP_CREATE));
  ASSERT_TRUE(parent.valid());
  EXPECT_GT(H5Pequal(parent.get(), H5P_OBJECT_CREATE), 0);

  size_t mcrt_nprops = 99;
  size_t ocrt_nprops = 0;
  ASSERT_GE(H5Pget_nprops(H5P_MAP_CREATE, &mcrt_nprops), 0);
  ASSERT_GE(H5Pget_nprops(H5P_OBJECT_CREATE, &ocrt_nprops), 0);
  // H5P__mcrt_reg_prop registers no map-specific properties directly on
  // H5P_MAP_CREATE; an instance inherits all properties from H5P_OBJECT_CREATE.
  EXPECT_EQ(mcrt_nprops, 0u);
  EXPECT_GT(ocrt_nprops, 0u);

  ScopedPlist mcpl(H5Pcreate(H5P_MAP_CREATE));
  ScopedPlist ocpl(H5Pcreate(H5P_OBJECT_CREATE));
  ASSERT_TRUE(mcpl.valid());
  ASSERT_TRUE(ocpl.valid());
  size_t mcpl_nprops = 0;
  size_t ocpl_nprops = 0;
  ASSERT_GE(H5Pget_nprops(mcpl.get(), &mcpl_nprops), 0);
  ASSERT_GE(H5Pget_nprops(ocpl.get(), &ocpl_nprops), 0);
  EXPECT_EQ(mcpl_nprops, ocrt_nprops);
  EXPECT_EQ(mcpl_nprops, ocpl_nprops);

  EXPECT_GT(H5Pisa_class(H5P_MAP_CREATE_DEFAULT, H5P_MAP_CREATE), 0);
  EXPECT_GT(H5Pisa_class(H5P_MAP_CREATE_DEFAULT, H5P_OBJECT_CREATE), 0);
  EXPECT_EQ(H5Pisa_class(H5P_MAP_CREATE_DEFAULT, H5P_MAP_ACCESS), 0);
}

TEST(H5PmcplTest, DefaultListIsImmutable) {
  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_obj_track_times(H5P_MAP_CREATE_DEFAULT, false), 0);
  EXPECT_LT(H5Pset_attr_phase_change(H5P_MAP_CREATE_DEFAULT, 2, 1), 0);
}

TEST(H5PmcplTest, CreateCopyEqualAndEncodeDecodeRoundTrip) {
  ScopedPlist mcpl(H5Pcreate(H5P_MAP_CREATE));
  ASSERT_TRUE(mcpl.valid());

  ScopedPlist mcpl_class(H5Pget_class(mcpl.get()));
  ASSERT_TRUE(mcpl_class.valid());
  EXPECT_GT(H5Pequal(mcpl_class.get(), H5P_MAP_CREATE), 0);
  EXPECT_GT(H5Pequal(mcpl.get(), H5P_MAP_CREATE_DEFAULT), 0);

  // Exercise inherited H5P_OBJECT_CREATE properties through H5P_MAP_CREATE.
  ASSERT_GE(H5Pset_attr_phase_change(mcpl.get(), 4, 2), 0);
  ASSERT_GE(H5Pset_attr_creation_order(
                mcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ASSERT_GE(H5Pset_obj_track_times(mcpl.get(), false), 0);
  ASSERT_GE(H5Pset_deflate(mcpl.get(), 5), 0);
  EXPECT_EQ(H5Pequal(mcpl.get(), H5P_MAP_CREATE_DEFAULT), 0);

  ScopedPlist copied(H5Pcopy(mcpl.get()));
  ASSERT_TRUE(copied.valid());
  EXPECT_GT(H5Pequal(mcpl.get(), copied.get()), 0);

  size_t encoded_size = 0;
  ASSERT_GE(H5Pencode2(mcpl.get(), nullptr, &encoded_size, H5P_DEFAULT), 0);
  ASSERT_GT(encoded_size, 0u);

  std::vector<uint8_t> encoded(encoded_size, 0);
  ASSERT_GE(H5Pencode2(mcpl.get(), encoded.data(), &encoded_size, H5P_DEFAULT),
            0);

  ScopedPlist decoded(H5Pdecode(encoded.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pisa_class(decoded.get(), H5P_MAP_CREATE), 0);
  EXPECT_GT(H5Pequal(mcpl.get(), decoded.get()), 0);

  unsigned max_compact = 0;
  unsigned min_dense = 0;
  unsigned crt_order = 0;
  bool track_times = true;
  ASSERT_GE(H5Pget_attr_phase_change(decoded.get(), &max_compact, &min_dense),
            0);
  EXPECT_EQ(max_compact, 4u);
  EXPECT_EQ(min_dense, 2u);
  ASSERT_GE(H5Pget_attr_creation_order(decoded.get(), &crt_order), 0);
  EXPECT_EQ(crt_order,
            static_cast<unsigned>(H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED));
  ASSERT_GE(H5Pget_obj_track_times(decoded.get(), &track_times), 0);
  EXPECT_FALSE(track_times);
  EXPECT_EQ(H5Pget_nfilters(decoded.get()), 1);
}

TEST(H5PmcplTest, ReinitExercisesClassRegistrationCallback) {
  // Ensure H5P__mcrt_reg_prop executes during explicit H5P_init_phase2().
  ASSERT_GE(H5close(), 0);
  ASSERT_GE(H5open(), 0);

  ScopedPlist mcpl(H5Pcreate(H5P_MAP_CREATE));
  ASSERT_TRUE(mcpl.valid());
  EXPECT_GT(H5Pisa_class(mcpl.get(), H5P_MAP_CREATE), 0);
}

}  // namespace
}  // namespace hdf5
