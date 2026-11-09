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
// Tests the attribute creation property list class descriptor defined in
// third_party/hdf5/src/H5Pacpl.c and public H5P_ATTRIBUTE_CREATE operations.
//
// Note: LF=0 in default google3 non-MPI Linux build (H5Pacpl.c defines only
// the constant H5P_CLS_ACRT descriptor table with no executable functions).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pacpl.c

#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; define NDEBUG around internal headers so
// struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5PACPL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5P_FRIEND
#include "third_party/hdf5/src/H5Ppkg.h"
// Defined in H5Pacpl.c and declared in H5Pint.c rather than H5Pprivate.h.
H5_DLLVAR const H5P_libclass_t H5P_CLS_ACRT[1];
}  // extern "C"
#ifdef H5PACPL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5PACPL_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ScopedPlistClass = ScopedH5Id<H5Pclose_class>;

TEST(H5PacplTest, DescriptorMetadataAndNullCallbacksMatchSpec) {
  ASSERT_GE(H5open(), 0);

  const H5P_libclass_t& acrt = H5P_CLS_ACRT[0];
  EXPECT_STREQ(acrt.name, "attribute create");
  EXPECT_EQ(acrt.type, H5P_TYPE_ATTRIBUTE_CREATE);
  EXPECT_EQ(acrt.par_pclass, &H5P_CLS_STRING_CREATE_g);
  EXPECT_EQ(acrt.pclass, &H5P_CLS_ATTRIBUTE_CREATE_g);
  EXPECT_EQ(acrt.class_id, &H5P_CLS_ATTRIBUTE_CREATE_ID_g);
  EXPECT_EQ(acrt.def_plist_id, &H5P_LST_ATTRIBUTE_CREATE_ID_g);
  EXPECT_NE(H5P_CLS_ATTRIBUTE_CREATE_g, nullptr);
  EXPECT_EQ(*acrt.class_id, H5P_ATTRIBUTE_CREATE);
  EXPECT_EQ(*acrt.def_plist_id, H5P_ATTRIBUTE_CREATE_DEFAULT);

  EXPECT_EQ(acrt.reg_prop_func, nullptr);
  EXPECT_EQ(acrt.create_func, nullptr);
  EXPECT_EQ(acrt.create_data, nullptr);
  EXPECT_EQ(acrt.copy_func, nullptr);
  EXPECT_EQ(acrt.copy_data, nullptr);
  EXPECT_EQ(acrt.close_func, nullptr);
  EXPECT_EQ(acrt.close_data, nullptr);

  char* class_name = H5Pget_class_name(H5P_ATTRIBUTE_CREATE);
  ASSERT_NE(class_name, nullptr);
  absl::Cleanup free_name = [class_name] { H5free_memory(class_name); };
  EXPECT_STREQ(class_name, "attribute create");

  ScopedPlistClass parent(H5Pget_class_parent(H5P_ATTRIBUTE_CREATE));
  ASSERT_TRUE(parent.valid());
  EXPECT_GT(H5Pequal(parent.get(), H5P_STRING_CREATE), 0);
}

TEST(H5PacplTest, PublicApiCharEncodingCopyEncodeDecodeAndAttributeRoundTrip) {
  ScopedPlist acpl(H5Pcreate(H5P_ATTRIBUTE_CREATE));
  ASSERT_TRUE(acpl.valid());
  EXPECT_GT(H5Pisa_class(acpl.get(), H5P_ATTRIBUTE_CREATE), 0);
  EXPECT_GT(H5Pisa_class(acpl.get(), H5P_STRING_CREATE), 0);
  EXPECT_GT(H5Pisa_class(acpl.get(), H5P_ROOT), 0);

  H5T_cset_t cset = H5T_CSET_ERROR;
  ASSERT_GE(H5Pget_char_encoding(acpl.get(), &cset), 0);
  EXPECT_EQ(cset, H5T_CSET_ASCII);

  ASSERT_GE(H5Pset_char_encoding(acpl.get(), H5T_CSET_UTF8), 0);

  ScopedPlist copied(H5Pcopy(acpl.get()));
  ASSERT_TRUE(copied.valid());
  EXPECT_GT(H5Pequal(acpl.get(), copied.get()), 0);

  size_t encoded_size = 0;
  ASSERT_GE(H5Pencode2(acpl.get(), nullptr, &encoded_size, H5P_DEFAULT), 0);
  ASSERT_GT(encoded_size, 0U);
  std::vector<uint8_t> encoded(encoded_size);
  ASSERT_GE(H5Pencode2(acpl.get(), encoded.data(), &encoded_size, H5P_DEFAULT),
            0);

  ScopedPlist decoded(H5Pdecode(encoded.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(acpl.get(), decoded.get()), 0);

  ScopedFile file = CreateInMemoryFile("acpl_attr.h5");
  ASSERT_TRUE(file.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedAttr attr(H5Acreate2(file.get(), "utf8_attr", H5T_NATIVE_INT32,
                             space.get(), acpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  int32_t value = 7;
  ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, &value), 0);

  ScopedPlist attr_acpl(H5Aget_create_plist(attr.get()));
  ASSERT_TRUE(attr_acpl.valid());
  H5T_cset_t round_trip_cset = H5T_CSET_ERROR;
  ASSERT_GE(H5Pget_char_encoding(attr_acpl.get(), &round_trip_cset), 0);
  EXPECT_EQ(round_trip_cset, H5T_CSET_UTF8);
}

}  // namespace
}  // namespace hdf5
