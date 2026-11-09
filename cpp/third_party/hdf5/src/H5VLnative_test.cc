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
// Tests the native VOL connector registration, introspection, token/address
// conversion, and file struct resolution (third_party/hdf5/src/H5VLnative.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLnative.c

#ifndef NDEBUG
#define H5VL_NATIVE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5VL_FRIEND
#include "third_party/hdf5/src/H5VLpkg.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5VLnative_private.h"
}  // extern "C"

#ifdef H5VL_NATIVE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5VL_NATIVE_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5VLnativeTest, RegisterUnregisterAndIntrospection) {
  ASSERT_GE(H5open(), 0);
  EXPECT_NE(H5VL_NATIVE_conn_g, nullptr);
  EXPECT_NE(H5VL_NATIVE_g, H5I_INVALID_HID);

  // Calling register again when already initialized is a safe idempotent no-op.
  EXPECT_GE(H5VL__native_register(), 0);

  // Public capability flag introspection through FAPL and connector ID.
  ScopedPlist fapl = MakeCoreFapl();
  uint64_t fapl_caps = 0;
  ASSERT_GE(H5Pget_vol_cap_flags(fapl.get(), &fapl_caps), 0);
  uint64_t direct_caps = 0;
  ASSERT_GE(H5VLget_cap_flags(H5VL_NATIVE, &direct_caps), 0);
  EXPECT_EQ(fapl_caps, direct_caps);
  EXPECT_NE(fapl_caps & H5VL_CAP_FLAG_NATIVE_FILES, 0u);

  uint64_t internal_caps = 0;
  ASSERT_GE(H5VL__native_introspect_get_cap_flags(nullptr, &internal_caps), 0);
  EXPECT_EQ(internal_caps, direct_caps);

  const H5VL_class_t* conn_cls = nullptr;
  ASSERT_GE(H5VL__native_introspect_get_conn_cls(
                nullptr, H5VL_GET_CONN_LVL_CURR, &conn_cls),
            0);
  ASSERT_NE(conn_cls, nullptr);
  EXPECT_EQ(conn_cls->value, H5_VOL_NATIVE);
  EXPECT_STREQ(conn_cls->name, H5VL_NATIVE_NAME);

  // Closing the library invokes H5VL__native_unregister; reopening registers
  // the native connector freshly.
  fapl = ScopedPlist{};
  ASSERT_GE(H5close(), 0);
  EXPECT_EQ(H5VL_NATIVE_conn_g, nullptr);
  EXPECT_EQ(H5VL_NATIVE_g, H5I_INVALID_HID);

  ASSERT_GE(H5open(), 0);
  EXPECT_NE(H5VL_NATIVE_conn_g, nullptr);
  EXPECT_NE(H5VL_NATIVE_g, H5I_INVALID_HID);
}

TEST(H5VLnativeTest, AddrToTokenAndTokenToAddrAcrossSizesAndObjectTypes) {
  ASSERT_GE(H5open(), 0);

  constexpr std::array<size_t, 3> kAddrSizes = {2, 4, 8};
  for (size_t addr_size : kAddrSizes) {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_sizes(fcpl.get(), addr_size, addr_size), 0);

    ScopedPlist fapl = MakeCoreFapl();
    const std::string filename =
        "in_memory_native_sizes_" + std::to_string(addr_size) + ".h5";
    ScopedFile file(H5Fcreate(filename.c_str(), H5F_ACC_TRUNC, fcpl.get(),
                              fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedGroup group(
        H5Gcreate2(file.get(), "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(group.valid());

    ScopedType named_dt(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(named_dt.valid());
    ASSERT_GE(H5Tcommit2(group.get(), "dtype", named_dt.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);

    ScopedSpace space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());

    ScopedDataset dset(H5Dcreate2(group.get(), "dset", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    const int32_t value = 42;
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, &value),
              0);

    ScopedAttr attr(H5Acreate2(dset.get(), "attr", H5T_NATIVE_INT32,
                               space.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());

    const std::array<hid_t, 5> loc_ids = {file.get(), group.get(),
                                          named_dt.get(), dset.get(),
                                          attr.get()};
    const haddr_t sample_addr =
        addr_size == 2
            ? haddr_t{0x1234}
            : (addr_size == 4 ? haddr_t{0x12345678}
                              : haddr_t{0x0123456789abcdefULL});

    for (hid_t loc_id : loc_ids) {
      size_t actual_addr_len = 0;
      ASSERT_GE(H5VL_native_get_file_addr_len(loc_id, &actual_addr_len), 0);
      EXPECT_EQ(actual_addr_len, addr_size);

      H5O_token_t token{};
      std::memset(&token, 0xff, sizeof(token));
      ASSERT_GE(H5VLnative_addr_to_token(loc_id, sample_addr, &token), 0);

      // Trailing bytes beyond addr_size must be zero-filled.
      for (size_t i = addr_size; i < sizeof(H5O_token_t); ++i) {
        EXPECT_EQ(token.__data[i], 0u);
      }

      haddr_t decoded_addr = HADDR_UNDEF;
      ASSERT_GE(H5VLnative_token_to_addr(loc_id, token, &decoded_addr), 0);
      EXPECT_EQ(decoded_addr, sample_addr);
    }

    // Verify object info tokens from the file round-trip through token<->addr.
    H5O_info2_t oinfo{};
    ASSERT_GE(H5Oget_info3(dset.get(), &oinfo, H5O_INFO_BASIC), 0);
    haddr_t obj_addr = HADDR_UNDEF;
    ASSERT_GE(H5VLnative_token_to_addr(file.get(), oinfo.token, &obj_addr), 0);
    EXPECT_NE(obj_addr, HADDR_UNDEF);

    H5O_token_t roundtrip_token{};
    ASSERT_GE(
        H5VLnative_addr_to_token(dset.get(), obj_addr, &roundtrip_token), 0);
    int cmp = -1;
    ASSERT_GE(H5Otoken_cmp(file.get(), &oinfo.token, &roundtrip_token, &cmp),
              0);
    EXPECT_EQ(cmp, 0);
  }
}

TEST(H5VLnativeTest, AddrTokenAndGetFileStructErrorPaths) {
  ASSERT_GE(H5open(), 0);
  ScopedFile file = CreateInMemoryFile("in_memory_native_errors.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup group(
      H5Gcreate2(file.get(), "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(group.valid());

  ScopedType named_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(named_dt.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "named_dt", named_dt.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedType transient_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(transient_dt.valid());

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());

  ScopedDataset dset(H5Dcreate2(file.get(), "dset", H5T_NATIVE_INT, space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  ScopedAttr attr(H5Acreate2(dset.get(), "attr", H5T_NATIVE_INT, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());

  // Verify H5VL_native_get_file_struct resolves every supported object type.
  H5F_t* file_ptr = nullptr;
  ASSERT_GE(H5VL_native_get_file_struct(H5VL_object(file.get()), H5I_FILE,
                                        &file_ptr),
            0);
  EXPECT_NE(file_ptr, nullptr);

  H5F_t* resolved = nullptr;
  ASSERT_GE(H5VL_native_get_file_struct(H5VL_object(group.get()), H5I_GROUP,
                                        &resolved),
            0);
  EXPECT_EQ(resolved, file_ptr);

  resolved = nullptr;
  ASSERT_GE(H5VL_native_get_file_struct(H5VL_object(named_dt.get()),
                                        H5I_DATATYPE, &resolved),
            0);
  EXPECT_EQ(resolved, file_ptr);

  resolved = nullptr;
  ASSERT_GE(H5VL_native_get_file_struct(H5VL_object(dset.get()), H5I_DATASET,
                                        &resolved),
            0);
  EXPECT_EQ(resolved, file_ptr);

  resolved = nullptr;
  ASSERT_GE(H5VL_native_get_file_struct(H5VL_object(attr.get()), H5I_ATTR,
                                        &resolved),
            0);
  EXPECT_EQ(resolved, file_ptr);

  // Error paths under silenced error stack.
  ScopedErrorSilence silence;

  size_t addr_len = 0;
  EXPECT_LT(H5VL_native_get_file_addr_len(H5I_INVALID_HID, &addr_len), 0);
  EXPECT_LT(H5VL_native_get_file_addr_len(space.get(), &addr_len), 0);

  H5O_token_t token{};
  haddr_t addr = 0;
  EXPECT_LT(H5VLnative_addr_to_token(file.get(), 0, nullptr), 0);
  EXPECT_LT(H5VLnative_addr_to_token(H5I_INVALID_HID, 0, &token), 0);
  EXPECT_LT(H5VLnative_addr_to_token(space.get(), 0, &token), 0);

  EXPECT_LT(H5VLnative_token_to_addr(file.get(), token, nullptr), 0);
  EXPECT_LT(H5VLnative_token_to_addr(H5I_INVALID_HID, token, &addr), 0);
  EXPECT_LT(H5VLnative_token_to_addr(space.get(), token, &addr), 0);

  // Direct internal error paths for unsupported / invalid ID types.
  void* dummy_obj = &addr_len;
  EXPECT_LT(H5VL__native_get_file_addr_len(dummy_obj, H5I_MAP, &addr_len), 0);
  EXPECT_LT(H5VL_native_addr_to_token(dummy_obj, H5I_MAP, 0, &token), 0);
  EXPECT_LT(H5VL_native_token_to_addr(dummy_obj, H5I_MAP, token, &addr), 0);

  resolved = nullptr;
  EXPECT_LT(H5VL_native_get_file_struct(nullptr, H5I_FILE, &resolved), 0);
  EXPECT_LT(H5VL_native_get_file_struct(H5I_object(transient_dt.get()),
                                        H5I_DATATYPE, &resolved),
            0);
  EXPECT_LT(H5VL_native_get_file_struct(dummy_obj, H5I_MAP, &resolved), 0);

  constexpr std::array<H5I_type_t, 14> kNonFileTypes = {
      H5I_UNINIT,       H5I_BADID,           H5I_DATASPACE,
      H5I_VFL,          H5I_VOL,             H5I_GENPROP_CLS,
      H5I_GENPROP_LST,  H5I_ERROR_CLASS,     H5I_ERROR_MSG,
      H5I_ERROR_STACK,  H5I_SPACE_SEL_ITER,  H5I_EVENTSET,
      H5I_NTYPES,       static_cast<H5I_type_t>(30),
  };
  for (H5I_type_t bad_type : kNonFileTypes) {
    EXPECT_LT(H5VL_native_get_file_struct(dummy_obj, bad_type, &resolved), 0);
  }
}

}  // namespace
}  // namespace hdf5
