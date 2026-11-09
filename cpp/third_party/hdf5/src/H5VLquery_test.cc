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
// Tests internal VOL object query and reset helpers
// (third_party/hdf5/src/H5VLquery.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLquery.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5VL_FRIEND
#include "third_party/hdf5/src/H5VLnative_private.h"
#include "third_party/hdf5/src/H5VLpkg.h"
}  // extern "C"

#include <cstddef>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

TEST(H5VlQueryTest, QueriesLiveCoreFileAndGroupVolObjects) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  H5VL_object_t* file_vol_obj = H5VL_vol_object(file.get());
  ASSERT_NE(file_vol_obj, nullptr);
  EXPECT_GE(H5VL_obj_get_rc(file_vol_obj), size_t{1});
  EXPECT_EQ(H5VL_OBJ_RC(file_vol_obj), H5VL_obj_get_rc(file_vol_obj));
  EXPECT_EQ(H5VL_obj_get_connector(file_vol_obj), H5VL_NATIVE_conn_g);
  EXPECT_EQ(H5VL_OBJ_CONNECTOR(file_vol_obj), H5VL_NATIVE_conn_g);
  EXPECT_NE(H5VL_obj_get_data(file_vol_obj), nullptr);
  EXPECT_EQ(H5VL_OBJ_DATA(file_vol_obj), H5VL_obj_get_data(file_vol_obj));

  ScopedGroup group(H5Gcreate2(file.get(), "grp", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
  ASSERT_TRUE(group.valid());

  H5VL_object_t* group_vol_obj = H5VL_vol_object(group.get());
  ASSERT_NE(group_vol_obj, nullptr);
  EXPECT_EQ(H5VL_obj_get_rc(group_vol_obj), size_t{1});
  EXPECT_EQ(H5VL_obj_get_connector(group_vol_obj), H5VL_NATIVE_conn_g);
  EXPECT_NE(H5VL_obj_get_data(group_vol_obj), nullptr);
  EXPECT_NE(H5VL_obj_get_data(group_vol_obj), H5VL_obj_get_data(file_vol_obj));
}

TEST(H5VlQueryTest, ReadsFieldsAndResetsDataOnVolObjectStruct) {
  ASSERT_GE(H5open(), 0);

  int payload = 42;
  H5VL_object_t vol_obj{};
  vol_obj.data = &payload;
  vol_obj.connector = H5VL_NATIVE_conn_g;
  vol_obj.rc = 7;

  EXPECT_EQ(H5VL_obj_get_rc(&vol_obj), size_t{7});
  EXPECT_EQ(H5VL_obj_get_connector(&vol_obj), H5VL_NATIVE_conn_g);
  EXPECT_EQ(H5VL_obj_get_data(&vol_obj), &payload);

  H5VL_obj_reset_data(&vol_obj);
  EXPECT_EQ(H5VL_obj_get_data(&vol_obj), nullptr);
  EXPECT_EQ(vol_obj.data, nullptr);

  // Verify the non-H5VL_MODULE wrapper macro forwards to H5VL_obj_reset_data().
  vol_obj.data = &payload;
  EXPECT_EQ(H5VL_OBJ_DATA(&vol_obj), &payload);
  H5VL_OBJ_DATA_RESET(&vol_obj);
  EXPECT_EQ(H5VL_obj_get_data(&vol_obj), nullptr);
}

}  // namespace
}  // namespace hdf5
