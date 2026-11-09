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
// Tests the named-datatype object header class callbacks (`H5O_OBJ_DATATYPE`:
// isa, open, create, get_oloc) via public commit/open/attribute/copy workflows
// and internal error checks (third_party/hdf5/src/H5Toh.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Toh.c

#include <cstdint>
#include <cstring>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match that macro around internal headers so
// struct layouts (e.g. H5O_t, H5C_cache_entry_t, H5T_t) match the library.
#ifndef NDEBUG
#define H5TOH_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
__extension__ typedef _Float16 H5__Float16;
#endif
extern "C" {
#define H5O_FRIEND
#define H5T_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5TOH_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TOH_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

// RAII guard for pushing an HDF5 API context + metadata cache tag when calling
// internal H5O_* / H5T_* routines directly.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = 0x10000) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(tag, &prev_tag_);
      tagged_ = true;
    }
  }

  ~ScopedApiContext() {
    if (tagged_) {
      H5AC_tag(prev_tag_, nullptr);
    }
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

TEST(H5TohTest, ObjClassDescriptorFieldsMatchNamedDatatype) {
  EXPECT_EQ(H5O_OBJ_DATATYPE[0].type, H5O_TYPE_NAMED_DATATYPE);
  EXPECT_STREQ(H5O_OBJ_DATATYPE[0].name, "named datatype");
  EXPECT_NE(H5O_OBJ_DATATYPE[0].isa, nullptr);
  EXPECT_NE(H5O_OBJ_DATATYPE[0].open, nullptr);
  EXPECT_NE(H5O_OBJ_DATATYPE[0].create, nullptr);
  EXPECT_NE(H5O_OBJ_DATATYPE[0].get_oloc, nullptr);
}

TEST(H5TohTest, CommitOpenInspectAttributeAndCopyCommittedDatatype) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  // 1. Commit a transient compound datatype -> exercises H5O__dtype_create.
  ScopedType comp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(comp.valid());
  ASSERT_GE(H5Tinsert(comp.get(), "id", 0, H5T_STD_I32LE), 0);
  ASSERT_GE(H5Tinsert(comp.get(), "val", 4, H5T_IEEE_F32LE), 0);
  ASSERT_GE(H5Tcommit2(file.get(), "rec_dt", comp.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  // 2. Inspect via H5Oget_info_by_name3 -> exercises H5O__dtype_isa returning
  // true.
  H5O_info2_t info{};
  ASSERT_GE(
      H5Oget_info_by_name3(file.get(), "rec_dt", &info, H5O_INFO_BASIC,
                           H5P_DEFAULT),
      0);
  EXPECT_EQ(info.type, H5O_TYPE_NAMED_DATATYPE);

  // 3. Open via H5Oopen -> exercises H5O__dtype_isa and H5O__dtype_open.
  ScopedType opened(H5Oopen(file.get(), "rec_dt", H5P_DEFAULT));
  ASSERT_TRUE(opened.valid());
  EXPECT_EQ(H5Iget_type(opened.get()), H5I_DATATYPE);
  EXPECT_GT(H5Tequal(opened.get(), comp.get()), 0);

  // 4. Attach an attribute and query object info by ID -> exercises
  // H5O__dtype_get_oloc.
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  {
    ScopedAttr attr(H5Acreate2(opened.get(), "version", H5T_NATIVE_INT32,
                               scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    int32_t ver = 7;
    ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, &ver), 0);
  }
  H5O_info2_t id_info{};
  ASSERT_GE(H5Oget_info3(opened.get(), &id_info, H5O_INFO_ALL), 0);
  EXPECT_EQ(id_info.type, H5O_TYPE_NAMED_DATATYPE);
  EXPECT_EQ(id_info.num_attrs, 1U);

  // 5. Copy the committed datatype to another path in the file.
  ASSERT_GE(H5Ocopy(file.get(), "rec_dt", file.get(), "rec_dt_copy",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  ScopedType copied(H5Oopen(file.get(), "rec_dt_copy", H5P_DEFAULT));
  ASSERT_TRUE(copied.valid());
  EXPECT_GT(H5Tequal(copied.get(), comp.get()), 0);
}

TEST(H5TohTest, ErrorPathsOnIsaOpenCreateAndGetOloc) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedGroup keep_open(
      H5Gcreate2(file.get(), "keep_open", H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT));
  ASSERT_TRUE(keep_open.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "plain_group", H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // 1. Direct H5O__dtype_isa on a group object header returns false (0).
  H5F_t* f = nullptr;
  H5O_loc_t bad_oloc{};
  H5G_name_t bad_path{};
  ASSERT_GE(H5O_loc_reset(&bad_oloc), 0);
  ASSERT_GE(H5G_name_reset(&bad_path), 0);
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5G_loc_t grp_loc{};
    ASSERT_GE(H5G_loc(grp.get(), &grp_loc), 0);
    f = grp_loc.oloc->file;
    bad_oloc.file = f;
    bad_oloc.addr = grp_loc.oloc->addr;
    H5O_t* grp_oh = H5O_protect(grp_loc.oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(grp_oh, nullptr);
    absl::Cleanup unprotect = [&] {
      H5O_unprotect(grp_loc.oloc, grp_oh, H5AC__NO_FLAGS_SET);
    };
    EXPECT_EQ(H5O_OBJ_DATATYPE[0].isa(grp_oh), 0);
  }
  ASSERT_NE(f, nullptr);
  // Close the group handle before calling H5O__dtype_open so H5FO_opened does
  // not find the group's H5G_shared_t at the same object address.
  grp.reset();

  ScopedErrorSilence silence;

  // 2. Direct H5O__dtype_open on a group location (no H5O_DTYPE_ID) returns
  // nullptr.
  {
    H5G_loc_t bad_loc{};
    bad_loc.oloc = &bad_oloc;
    bad_loc.path = &bad_path;
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5I_type_t opened_type = H5I_BADID;
    EXPECT_EQ(H5O_OBJ_DATATYPE[0].open(&bad_loc, &opened_type), nullptr);
    EXPECT_EQ(opened_type, H5I_DATATYPE);
  }

  // 3. Direct H5O__dtype_create on an immutable datatype fails inside
  // H5T__commit -> H5O__dtype_create without creating an object header.
  {
    auto* imm_dt = static_cast<H5T_t*>(H5I_object(H5T_NATIVE_INT32));
    ASSERT_NE(imm_dt, nullptr);
    H5T_obj_create_t crt_info{};
    crt_info.dt = imm_dt;
    crt_info.tcpl_id = H5P_DATATYPE_CREATE_DEFAULT;
    H5G_loc_t dummy_loc{};
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    EXPECT_EQ(H5O_OBJ_DATATYPE[0].create(f, &crt_info, &dummy_loc), nullptr);
  }

  // 4. H5O__dtype_get_oloc with invalid ID, predefined transient ID, and
  // copied transient ID all return nullptr.
  EXPECT_EQ(H5O_OBJ_DATATYPE[0].get_oloc(H5I_INVALID_HID), nullptr);
  EXPECT_EQ(H5O_OBJ_DATATYPE[0].get_oloc(H5T_NATIVE_INT32), nullptr);
  ScopedType transient(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(transient.valid());
  EXPECT_EQ(H5O_OBJ_DATATYPE[0].get_oloc(transient.get()), nullptr);
}

}  // namespace
}  // namespace hdf5
