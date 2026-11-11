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

// Unit tests for //third_party/netcdf/libsrc4/nc4internal.c.
//
// Exercises name validation/normalization, open-file list manipulation,
// internal metadata lookups (groups, dims, vars, types, attributes),
// duplicate-name checks, reference-counted type/field/enum/att/var/dim
// teardown, reserved attribute binary search, and NC4_show_metadata().
//
// Note: LOGGING and NETCDF_ENABLE_SET_LOG_LEVEL are #undef in config.h, so
// nc_log / nc_set_log_level / log_metadata_nc are compiled out in google3.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libsrc4/nc4internal.c

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"

extern "C" {
#include "third_party/netcdf/include/nc.h"
#include "third_party/netcdf/include/ncdispatch.h"
#include "third_party/netcdf/include/ncindex.h"
#include "third_party/netcdf/include/nclist.h"
#include "third_party/netcdf/include/nc4internal.h"
#include "third_party/netcdf/include/nc4dispatch.h"
int nc4_rec_grp_del_att_data(NC_GRP_INFO_T* grp);
}  // extern "C"
// clang-format on

namespace netcdf {
namespace {

struct CFreeDeleter {
  void operator()(void* ptr) const { std::free(ptr); }
};

template <typename T>
using ScopedCMem = std::unique_ptr<T, CFreeDeleter>;

char* AllocCString(std::string_view text) {
  auto* buf = static_cast<char*>(std::malloc(text.size() + 1));
  if (buf == nullptr) {
    return nullptr;
  }
  if (!text.empty()) {
    std::memcpy(buf, text.data(), text.size());
  }
  buf[text.size()] = '\0';
  return buf;
}

unsigned short FindFreeNcidIndex(unsigned short start_hint) {
  // Keep file slot indices strictly within [1, 32767] so (candidate <<
  // ID_SHIFT) never sets the sign bit of signed 32-bit int ncid handles.
  for (unsigned int offset = 0; offset < 32767U; ++offset) {
    const unsigned int candidate = 1U + ((start_hint + offset) % 32767U);
    const int probe_ncid = static_cast<int>(candidate << ID_SHIFT);
    if (NC_check_id(probe_ncid, nullptr) == NC_EBADID) {
      return static_cast<unsigned short>(candidate);
    }
  }
  return 0;
}

TEST(Nc4InternalTest, CheckNameAndNormalizeNameValidateAndNormalizeUtf8) {
  char norm[NC_MAX_NAME + 1]{};

  EXPECT_EQ(NC_EINVAL, nc4_check_name(nullptr, norm));
  EXPECT_EQ(NC_EBADNAME, nc4_check_name("", norm));
  EXPECT_EQ(NC_EBADNAME, nc4_check_name("bad/slash", norm));
  EXPECT_EQ(NC_EBADNAME, nc4_check_name("-leading_dash", norm));
  EXPECT_EQ(NC_EBADNAME, nc4_check_name("trailing_space ", norm));
  EXPECT_EQ(NC_EBADNAME, nc4_check_name("\xFF\xFE_bad_utf8", norm));

  const std::string overlong_ascii(NC_MAX_NAME + 1, 'a');
  EXPECT_EQ(NC_EMAXNAME, nc4_check_name(overlong_ascii.c_str(), norm));

  // U+0958 (DEVANAGARI LETTER QA, 3 bytes) decomposes cleanly under NFC because
  // it carries a composition exclusion into U+0915 + U+093C (6 bytes). Sixty
  // copies total 180 raw bytes (<= NC_MAX_NAME) and normalize to 360 (> 256).
  std::string expanding_utf8;
  for (int i = 0; i < 60; ++i) {
    expanding_utf8.append("\xE0\xA5\x98");
  }
  ASSERT_LE(expanding_utf8.size(), static_cast<size_t>(NC_MAX_NAME));
  EXPECT_EQ(NC_EMAXNAME, nc4_check_name(expanding_utf8.c_str(), norm));

  // Valid ASCII passes unchanged.
  ASSERT_EQ(NC_NOERR, nc4_check_name("valid_name_1", norm));
  EXPECT_STREQ("valid_name_1", norm);

  // NFD "cafe\u0301" normalizes to NFC "caf\u00e9".
  ASSERT_EQ(NC_NOERR, nc4_check_name("cafe\xCC\x81", norm));
  EXPECT_STREQ("caf\xC3\xA9", norm);

  // Direct nc4_normalize_name coverage: valid, NFD->NFC, invalid UTF-8, max
  // len.
  ASSERT_EQ(NC_NOERR, nc4_normalize_name("plain_var", norm));
  EXPECT_STREQ("plain_var", norm);

  ASSERT_EQ(NC_NOERR, nc4_normalize_name("re\xCC\x81sume\xCC\x81", norm));
  EXPECT_STREQ("r\xC3\xA9sum\xC3\xA9", norm);

  EXPECT_EQ(NC_EBADNAME, nc4_normalize_name("\xFF\xFE", norm));
  EXPECT_EQ(NC_EMAXNAME, nc4_normalize_name(expanding_utf8.c_str(), norm));
}

TEST(Nc4InternalTest, FileListAddGetChangeNcidAndDelete) {
  void* dd = nullptr;
  EXPECT_EQ(NC_EBADID, nc4_file_list_add(-1, "bad.nc", NC_NETCDF4, &dd));
  EXPECT_EQ(NC_EBADID, nc4_file_list_get(-1, nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_EBADID, nc4_file_change_ncid(-1, 5));
  EXPECT_EQ(NC_EBADID, nc4_file_list_del(-1));

  ScopedNcFile nc1;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("file_list_1.nc", NC_NETCDF4, 0, nc1.ptr()));
  ScopedNcFile nc2;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("file_list_2.nc", NC_NETCDF4, 0, nc2.ptr()));

  // Querying with all optional output pointers NULL succeeds.
  EXPECT_EQ(NC_NOERR, nc4_file_list_get(nc1.get(), nullptr, nullptr, nullptr));

  // Querying path, mode, and dispatchdata populates all outputs.
  char path_buf[NC_MAX_NAME + 1]{};
  char* path_ptr = path_buf;
  int mode = 0;
  ASSERT_EQ(NC_NOERR, nc4_file_list_get(nc1.get(), &path_ptr, &mode, &dd));
  EXPECT_STREQ("file_list_1.nc", path_buf);
  EXPECT_NE(0, mode & NC_NETCDF4);
  EXPECT_NE(nullptr, dd);

  // Moving into an occupied slot fails with NC_EIO.
  const unsigned short occupied_idx = static_cast<unsigned short>(
      static_cast<unsigned int>(nc2.get()) >> ID_SHIFT);
  EXPECT_EQ(NC_EIO, nc4_file_change_ncid(nc1.get(), occupied_idx));

  // Moving into an unoccupied non-zero slot re-keys the file cleanly.
  const unsigned short old_idx = static_cast<unsigned short>(
      static_cast<unsigned int>(nc1.get()) >> ID_SHIFT);
  const unsigned short target_idx =
      FindFreeNcidIndex(static_cast<unsigned short>(old_idx + 17));
  ASSERT_NE(0, target_idx);
  const int old_nc1 = nc1.get();
  ASSERT_EQ(NC_NOERR, nc4_file_change_ncid(old_nc1, target_idx));
  const int new_nc1 =
      static_cast<int>(static_cast<unsigned int>(target_idx) << ID_SHIFT);
  nc1.release();
  nc1.reset(new_nc1);

  EXPECT_EQ(NC_EBADID, nc4_file_list_get(old_nc1, nullptr, nullptr, nullptr));
  ASSERT_EQ(NC_NOERR, nc4_file_list_get(nc1.get(), nullptr, nullptr, &dd));
  EXPECT_EQ(new_nc1, static_cast<NC_OBJ*>(dd)->id);

  int dimid = -1;
  EXPECT_EQ(NC_NOERR, nc_def_dim(nc1.get(), "moved_dim", 4, &dimid));
  EXPECT_EQ(NC_NOERR, nc1.close());
  EXPECT_EQ(NC_NOERR, nc2.close());
}

TEST(Nc4InternalTest, SyntheticNcListMetadataAddAndDeletePaths) {
  ASSERT_EQ(NC_NOERR, nc_initialize());
  ASSERT_NE(nullptr, HDF5_dispatch_table);

  NC* synth_nc = nullptr;
  ASSERT_EQ(NC_NOERR, new_NC(HDF5_dispatch_table, "synth_nc4internal.nc",
                             NC_NETCDF4, &synth_nc));
  ASSERT_NE(nullptr, synth_nc);
  ASSERT_EQ(NC_NOERR, add_to_NCList(synth_nc));
  bool has_h5 = false;
  absl::Cleanup synth_cleanup = [&synth_nc, &has_h5] {
    if (synth_nc != nullptr) {
      if (has_h5 && synth_nc->dispatchdata != nullptr) {
        EXPECT_EQ(NC_NOERR, nc4_file_list_del(synth_nc->ext_ncid));
        synth_nc->dispatchdata = nullptr;
      }
      del_from_NCList(synth_nc);
      free_NC(synth_nc);
      synth_nc = nullptr;
    }
  };

  // Before nc4_file_list_add, dispatchdata is NULL (covers NC4_move_in_NCList
  // !nc->dispatchdata branch).
  const unsigned short synth_idx = FindFreeNcidIndex(42);
  ASSERT_NE(0, synth_idx);
  EXPECT_EQ(NC_NOERR, nc4_file_change_ncid(synth_nc->ext_ncid, synth_idx));

  // Pass dispatchdata == nullptr to hit the optional-output FALSE branch.
  ASSERT_EQ(NC_NOERR,
            nc4_file_list_add(synth_nc->ext_ncid, "synth_nc4internal.nc",
                              NC_NETCDF4, nullptr));
  has_h5 = true;

  NC_GRP_INFO_T* root_grp = nullptr;
  NC_FILE_INFO_T* h5 = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_find_grp_h5(synth_nc->ext_ncid, &root_grp, &h5));
  ASSERT_NE(nullptr, root_grp);
  ASSERT_NE(nullptr, h5);

  // Add child groups with non-null and null group out-parameters.
  NC_GRP_INFO_T* child_grp = nullptr;
  char child_name[] = "synth_child";
  ASSERT_EQ(NC_NOERR, nc4_grp_list_add(h5, root_grp, child_name, &child_grp));
  ASSERT_NE(nullptr, child_grp);

  char child2_name[] = "synth_child_no_out";
  ASSERT_EQ(NC_NOERR, nc4_grp_list_add(h5, root_grp, child2_name, nullptr));

  // Add unlimited dim with explicit assignedid and dim == nullptr.
  EXPECT_EQ(NC_NOERR, nc4_dim_list_add(root_grp, "synth_unlim", NC_UNLIMITED, 5,
                                       nullptr));

  // Test nc4_dim_list_del on an in-list dim, an unlisted dim, and grp == NULL.
  NC_DIM_INFO_T* dim_in_child = nullptr;
  ASSERT_EQ(NC_NOERR,
            nc4_dim_list_add(child_grp, "dim_in_child", 7, -1, &dim_in_child));
  EXPECT_EQ(NC_NOERR, nc4_dim_list_del(child_grp, dim_in_child));

  NC_DIM_INFO_T* dim_detached = nullptr;
  ASSERT_EQ(NC_NOERR,
            nc4_dim_list_add(child_grp, "dim_detached", 8, -1, &dim_detached));
  ASSERT_EQ(1, ncindexidel(child_grp->dim, 0));
  EXPECT_EQ(NC_NOERR, nc4_dim_list_del(child_grp, dim_detached));

  NC_DIM_INFO_T* dim_null_grp = nullptr;
  ASSERT_EQ(NC_NOERR,
            nc4_dim_list_add(child_grp, "dim_null_grp", 9, -1, &dim_null_grp));
  ASSERT_EQ(1, ncindexidel(child_grp->dim, 0));
  EXPECT_EQ(NC_NOERR, nc4_dim_list_del(nullptr, dim_null_grp));

  // Test nc4_var_list_add2 (var == nullptr and var != nullptr),
  // nc4_var_list_add, and nc4_var_list_del (both i >= 0 and i < 0).
  EXPECT_EQ(NC_NOERR, nc4_var_list_add2(root_grp, "synth_var_no_out", nullptr));

  NC_VAR_INFO_T* var_2d = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_var_list_add(root_grp, "synth_var_2d", 2, &var_2d));
  ASSERT_NE(nullptr, var_2d);
  EXPECT_EQ(NC_NOERR, nc4_var_list_del(root_grp, var_2d));

  NC_VAR_INFO_T* var_detached = nullptr;
  ASSERT_EQ(NC_NOERR,
            nc4_var_list_add2(child_grp, "synth_var_detached", &var_detached));
  ASSERT_EQ(1, ncindexidel(child_grp->vars, 0));
  EXPECT_EQ(NC_NOERR, nc4_var_list_del(child_grp, var_detached));

  // Test nc4_att_list_add with att == nullptr (zero-len empty attribute).
  EXPECT_EQ(NC_NOERR,
            nc4_att_list_add(root_grp->att, "synth_att_no_out", nullptr));

  // Test direct nc4_att_list_del on a child group attribute.
  NC_ATT_INFO_T* att_del = nullptr;
  ASSERT_EQ(NC_NOERR,
            nc4_att_list_add(child_grp->att, "synth_att_del", &att_del));
  EXPECT_EQ(NC_NOERR, nc4_att_list_del(child_grp->att, att_del));

  // Test synthetic opaque type added via nc4_type_list_add so nc4_rec_grp_del
  // exercises nc4_type_free default switch branch directly.
  NC_TYPE_INFO_T* op_type = nullptr;
  ASSERT_EQ(NC_NOERR,
            nc4_type_list_add(child_grp, 4, "synth_opaque", &op_type));
  op_type->nc_type_class = NC_OPAQUE;

  // Test NULL name guard in nc4_field_list_add.
  EXPECT_EQ(NC_EINVAL,
            nc4_field_list_add(nullptr, nullptr, 0, NC_INT, 0, nullptr));

  ASSERT_EQ(NC_NOERR, nc4_file_list_del(synth_nc->ext_ncid));
  has_h5 = false;
  synth_nc->dispatchdata = nullptr;
  del_from_NCList(synth_nc);
  free_NC(synth_nc);
  synth_nc = nullptr;
}

TEST(Nc4InternalTest, LookupHelpersFindGroupsVarsDimsTypesAttsAndDuplicates) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("lookup_helpers.nc", NC_NETCDF4, 0, nc.ptr()));

  int grp_empty = -1;
  int grp_a = -1;
  int grp_grand = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "grp_empty", &grp_empty));
  ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "grp_a", &grp_a));
  ASSERT_EQ(NC_NOERR, nc_def_grp(grp_a, "grp_grand", &grp_grand));

  int dim_root = -1;
  int dim_a = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "dim_root", 3, &dim_root));
  ASSERT_EQ(NC_NOERR, nc_def_dim(grp_a, "x_coord", NC_UNLIMITED, &dim_a));

  // User-defined types across root, grp_a, and grp_grand.
  nc_type enum_root = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_INT, "enum_root", &enum_root));
  const int32_t e1 = 1;
  const int32_t e2 = 2;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), enum_root, "ONE", &e1));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), enum_root, "TWO", &e2));

  struct CompoundSample {
    int32_t id;
    double grid[2][3];
  };
  nc_type cmp_grand = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_compound(grp_grand, sizeof(CompoundSample),
                                      "cmp_grand", &cmp_grand));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(grp_grand, cmp_grand, "id",
                               NC_COMPOUND_OFFSET(CompoundSample, id), NC_INT));
  constexpr int kFieldDims[2] = {2, 3};
  ASSERT_EQ(NC_NOERR,
            nc_insert_array_compound(grp_grand, cmp_grand, "grid",
                                     NC_COMPOUND_OFFSET(CompoundSample, grid),
                                     NC_DOUBLE, 2, kFieldDims));

  nc_type vlen_a = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_vlen(grp_a, "vlen_a", NC_INT, &vlen_a));

  nc_type op_a = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(grp_a, 8, "op_a", &op_a));

  // Variables and attributes; reuse enum_root and cmp_grand across multiple
  // variables to exercise nc4_type_free reference counting (rc > 1 -> rc == 0).
  int var_scalar = -1;
  int var_enum1 = -1;
  int var_enum2 = -1;
  int var_noncoord = -1;
  int var_cmp1 = -1;
  int var_cmp2 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "var_scalar", NC_INT, 0, nullptr,
                                 &var_scalar));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "var_enum1", enum_root, 1, &dim_root,
                                 &var_enum1));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "var_enum2", enum_root, 0, nullptr,
                                 &var_enum2));
  // Scalar variable sharing dim name "x_coord" triggers non-coord alt_name.
  ASSERT_EQ(NC_NOERR,
            nc_def_var(grp_a, "x_coord", NC_DOUBLE, 0, nullptr, &var_noncoord));
  ASSERT_EQ(NC_NOERR, nc_def_var(grp_grand, "var_cmp1", cmp_grand, 0, nullptr,
                                 &var_cmp1));
  ASSERT_EQ(NC_NOERR, nc_def_var(grp_grand, "var_cmp2", cmp_grand, 0, nullptr,
                                 &var_cmp2));

  const char* g_strs[] = {"hello", "netcdf4"};
  ASSERT_EQ(NC_NOERR,
            nc_put_att_string(nc.get(), NC_GLOBAL, "g_str", 2, g_strs));
  const char* g_del[] = {"to_delete"};
  ASSERT_EQ(NC_NOERR,
            nc_put_att_string(nc.get(), NC_GLOBAL, "g_del", 1, g_del));
  ASSERT_EQ(NC_NOERR, nc_del_att(nc.get(), NC_GLOBAL, "g_del"));

  const int32_t v_int_val = 99;
  ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), var_scalar, "v_int", NC_INT, 1,
                                     &v_int_val));

  // Test nc4_find_nc4_grp / nc4_find_grp_h5 / nc4_find_nc_grp_h5.
  const int bad_grp_ncid = nc.get() | 0x00FF;
  NC* nc_ptr = nullptr;
  NC_GRP_INFO_T* root_grp = nullptr;
  NC_GRP_INFO_T* grand_grp = nullptr;
  NC_GRP_INFO_T* a_grp = nullptr;
  NC_FILE_INFO_T* h5 = nullptr;

  EXPECT_EQ(NC_EBADID, nc4_find_nc4_grp(-1, &root_grp));
  EXPECT_EQ(NC_EBADID, nc4_find_nc4_grp(bad_grp_ncid, &root_grp));
  EXPECT_EQ(NC_NOERR, nc4_find_nc4_grp(nc.get(), nullptr));
  ASSERT_EQ(NC_NOERR, nc4_find_nc4_grp(nc.get(), &root_grp));
  ASSERT_NE(nullptr, root_grp);

  EXPECT_EQ(NC_EBADID, nc4_find_grp_h5(-1, &a_grp, &h5));
  EXPECT_EQ(NC_NOERR, nc4_find_grp_h5(nc.get(), nullptr, nullptr));
  ASSERT_EQ(NC_NOERR, nc4_find_grp_h5(grp_a, &a_grp, &h5));
  ASSERT_NE(nullptr, a_grp);
  ASSERT_NE(nullptr, h5);

  EXPECT_EQ(NC_EBADID, nc4_find_nc_grp_h5(-1, &nc_ptr, &grand_grp, &h5));
  EXPECT_EQ(NC_EBADID,
            nc4_find_nc_grp_h5(bad_grp_ncid, &nc_ptr, &grand_grp, &h5));
  EXPECT_EQ(NC_NOERR, nc4_find_nc_grp_h5(grp_grand, nullptr, nullptr, nullptr));
  ASSERT_EQ(NC_NOERR, nc4_find_nc_grp_h5(grp_grand, &nc_ptr, &grand_grp, &h5));
  EXPECT_NE(nullptr, nc_ptr);
  EXPECT_NE(nullptr, grand_grp);

  // Test nc4_find_grp_h5_var.
  NC_VAR_INFO_T* found_var = nullptr;
  EXPECT_EQ(NC_EBADID, nc4_find_grp_h5_var(-1, 0, &h5, &root_grp, &found_var));
  EXPECT_EQ(NC_ENOTVAR,
            nc4_find_grp_h5_var(nc.get(), 99, &h5, &root_grp, &found_var));
  EXPECT_EQ(NC_NOERR, nc4_find_grp_h5_var(nc.get(), var_scalar, nullptr,
                                          nullptr, nullptr));
  ASSERT_EQ(NC_NOERR, nc4_find_grp_h5_var(nc.get(), var_scalar, &h5, &root_grp,
                                          &found_var));
  ASSERT_NE(nullptr, found_var);
  EXPECT_STREQ("var_scalar", found_var->hdr.name);

  // Test nc4_find_dim.
  NC_DIM_INFO_T* found_dim = nullptr;
  NC_GRP_INFO_T* dim_grp = nullptr;
  EXPECT_EQ(NC_EBADDIM, nc4_find_dim(grand_grp, 999, &found_dim, &dim_grp));
  EXPECT_EQ(NC_NOERR, nc4_find_dim(grand_grp, dim_root, &found_dim, nullptr));
  ASSERT_EQ(NC_NOERR, nc4_find_dim(grand_grp, dim_root, &found_dim, &dim_grp));
  ASSERT_NE(nullptr, found_dim);
  EXPECT_EQ(root_grp, dim_grp);

  // Test nc4_find_var.
  NC_VAR_INFO_T* v_lookup = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_find_var(root_grp, "var_scalar", &v_lookup));
  EXPECT_EQ(found_var, v_lookup);
  ASSERT_EQ(NC_NOERR, nc4_find_var(root_grp, "nonexistent_var", &v_lookup));
  EXPECT_EQ(nullptr, v_lookup);

  // Test nc4_rec_find_named_type across empty sibling -> nested grandchild.
  char t_root_name[] = "enum_root";
  char t_grand_name[] = "cmp_grand";
  char t_missing_name[] = "no_such_type";
  EXPECT_NE(nullptr, nc4_rec_find_named_type(root_grp, t_root_name));
  EXPECT_NE(nullptr, nc4_rec_find_named_type(root_grp, t_grand_name));
  EXPECT_EQ(nullptr, nc4_rec_find_named_type(root_grp, t_missing_name));
  // Exercise nc4_rec_find_named_type() skipping a NULL child entry safely.
  ASSERT_EQ(1, nclistpush(root_grp->children->list, nullptr));
  EXPECT_EQ(nullptr, nc4_rec_find_named_type(root_grp, t_missing_name));
  EXPECT_EQ(nullptr, nclistpop(root_grp->children->list));

  // Test nc4_find_type error & success branches.
  NC_TYPE_INFO_T* found_type = nullptr;
  EXPECT_EQ(NC_EINVAL, nc4_find_type(h5, -1, &found_type));
  EXPECT_EQ(NC_EINVAL, nc4_find_type(h5, NC_INT, nullptr));
  ASSERT_EQ(NC_NOERR, nc4_find_type(h5, NC_INT, &found_type));
  EXPECT_EQ(nullptr, found_type);
  ASSERT_EQ(NC_NOERR, nc4_find_type(h5, NC_STRING, &found_type));
  EXPECT_EQ(nullptr, found_type);
  EXPECT_EQ(NC_EBADTYPID,
            nc4_find_type(h5, NC_FIRSTUSERTYPEID + 99, &found_type));
  ASSERT_EQ(NC_NOERR, nc4_find_type(h5, cmp_grand, &found_type));
  ASSERT_NE(nullptr, found_type);
  EXPECT_STREQ("cmp_grand", found_type->hdr.name);

  // Test nc4_find_nc_att and nc4_find_grp_att.
  NC_ATT_INFO_T* found_att = nullptr;
  EXPECT_EQ(NC_EBADID, nc4_find_nc_att(-1, NC_GLOBAL, "g_str", 0, &found_att));
  EXPECT_EQ(NC_ENOTVAR, nc4_find_nc_att(nc.get(), 99, "v_int", 0, &found_att));
  EXPECT_EQ(NC_ENOTATT,
            nc4_find_nc_att(nc.get(), NC_GLOBAL, "missing_att", 0, &found_att));
  EXPECT_EQ(NC_ENOTATT,
            nc4_find_nc_att(nc.get(), var_scalar, nullptr, 99, &found_att));
  ASSERT_EQ(NC_NOERR,
            nc4_find_nc_att(nc.get(), NC_GLOBAL, "g_str", -1, &found_att));
  EXPECT_STREQ("g_str", found_att->hdr.name);
  ASSERT_EQ(NC_NOERR,
            nc4_find_nc_att(nc.get(), NC_GLOBAL, nullptr, 0, &found_att));
  EXPECT_STREQ("g_str", found_att->hdr.name);
  ASSERT_EQ(NC_NOERR,
            nc4_find_nc_att(nc.get(), var_scalar, "v_int", -1, &found_att));
  EXPECT_STREQ("v_int", found_att->hdr.name);
  ASSERT_EQ(NC_NOERR,
            nc4_find_nc_att(nc.get(), var_scalar, nullptr, 0, &found_att));
  EXPECT_STREQ("v_int", found_att->hdr.name);

  // Test nc4_check_dup_name collisions against type, child group, and var.
  char dup_type[] = "op_a";
  char dup_grp[] = "grp_grand";
  char dup_var[] = "x_coord";
  char free_name[] = "completely_fresh_name";
  EXPECT_EQ(NC_ENAMEINUSE, nc4_check_dup_name(a_grp, dup_type));
  EXPECT_EQ(NC_ENAMEINUSE, nc4_check_dup_name(a_grp, dup_grp));
  EXPECT_EQ(NC_ENAMEINUSE, nc4_check_dup_name(a_grp, dup_var));
  EXPECT_EQ(NC_NOERR, nc4_check_dup_name(a_grp, free_name));

  EXPECT_EQ(NC_NOERR, nc_enddef(nc.get()));
  EXPECT_EQ(NC_NOERR, nc.close());
}

TEST(Nc4InternalTest, AbortBeforeEnddefReclaimsVarFillValueChunksAndAltName) {
  TempFile tf("nc4internal_abort_fill.nc");
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create(tf.c_str(), NC_NETCDF4 | NC_CLOBBER, nc.ptr()));

  int dim_x = -1;
  int dim_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 4, &dim_x));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "y", 3, &dim_y));

  // Variable named "x" with dimension "y" (non-coordinate -> allocates
  // alt_name), chunked storage (allocates chunksizes), custom fill value (sets
  // fill_value before enddef), and a variable attribute (exercises var->att
  // loop in var_free).
  int var_x = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "x", NC_DOUBLE, 1, &dim_y, &var_x));
  constexpr size_t kChunk = 2;
  ASSERT_EQ(NC_NOERR,
            nc_def_var_chunking(nc.get(), var_x, NC_CHUNKED, &kChunk));
  constexpr double kFillVal = -123.5;
  ASSERT_EQ(NC_NOERR, nc_def_var_fill(nc.get(), var_x, NC_FILL, &kFillVal));
  const char kUnits[] = "kelvin";
  ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), var_x, "units",
                                      sizeof(kUnits) - 1, kUnits));

  NC_VAR_INFO_T* var_info = nullptr;
  ASSERT_EQ(NC_NOERR,
            nc4_find_grp_h5_var(nc.get(), var_x, nullptr, nullptr, &var_info));
  ASSERT_NE(nullptr, var_info);
  EXPECT_NE(nullptr, var_info->alt_name);
  EXPECT_NE(nullptr, var_info->chunksizes);
  EXPECT_NE(nullptr, var_info->fill_value);

  // Aborting prior to nc_enddef leaves hdf_datasetid == 0, so var_free() in
  // nc4internal.c reclaims fill_value directly via NC_reclaim_data_all().
  EXPECT_EQ(NC_NOERR, nc.abort());
}

TEST(Nc4InternalTest, AttFreeHandlesGroupStringDataVarIntDataAndEmptyAtt) {
  ScopedNcFile host_nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("att_free_host.nc", NC_NETCDF4, 0, host_nc.ptr()));

  int varid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(host_nc.get(), "host_var", NC_INT, 0, nullptr, &varid));

  NC_GRP_INFO_T* grp = nullptr;
  NC_VAR_INFO_T* var = nullptr;
  ASSERT_EQ(NC_NOERR,
            nc4_find_grp_h5_var(host_nc.get(), varid, nullptr, &grp, &var));
  ASSERT_NE(nullptr, grp);
  ASSERT_NE(nullptr, var);

  // Standalone group-owned attribute with heap-allocated NC_STRING data.
  {
    ScopedCMem<NC_ATT_INFO_T> grp_att(
        static_cast<NC_ATT_INFO_T*>(std::calloc(1, sizeof(NC_ATT_INFO_T))));
    ASSERT_NE(nullptr, grp_att);
    ScopedCMem<char> name(AllocCString("standalone_grp_str"));
    ASSERT_NE(nullptr, name);
    ScopedCMem<char*> strs(static_cast<char**>(std::calloc(2, sizeof(char*))));
    ASSERT_NE(nullptr, strs);
    ScopedCMem<char> s0(AllocCString("alpha"));
    ScopedCMem<char> s1(AllocCString("beta"));
    ASSERT_NE(nullptr, s0);
    ASSERT_NE(nullptr, s1);

    strs.get()[0] = s0.release();
    strs.get()[1] = s1.release();
    grp_att->hdr.sort = NCATT;
    grp_att->hdr.name = name.release();
    grp_att->container = reinterpret_cast<NC_OBJ*>(grp);
    grp_att->nc_typeid = NC_STRING;
    grp_att->len = 2;
    grp_att->data = strs.release();
    EXPECT_EQ(NC_NOERR, nc4_att_free(grp_att.release()));
  }

  // Standalone variable-owned attribute with initialized NC_INT data.
  {
    ScopedCMem<NC_ATT_INFO_T> var_att(
        static_cast<NC_ATT_INFO_T*>(std::calloc(1, sizeof(NC_ATT_INFO_T))));
    ASSERT_NE(nullptr, var_att);
    ScopedCMem<char> name(AllocCString("standalone_var_int"));
    ASSERT_NE(nullptr, name);
    ScopedCMem<int> vals(static_cast<int*>(std::calloc(2, sizeof(int))));
    ASSERT_NE(nullptr, vals);

    vals.get()[0] = 11;
    vals.get()[1] = 22;
    var_att->hdr.sort = NCATT;
    var_att->hdr.name = name.release();
    var_att->container = reinterpret_cast<NC_OBJ*>(var);
    var_att->nc_typeid = NC_INT;
    var_att->len = 2;
    var_att->data = vals.release();
    EXPECT_EQ(NC_NOERR, nc4_att_free(var_att.release()));
  }

  // Attribute with unknown user typeid triggers NC_reclaim_data failure while
  // still freeing NC_ATT_INFO_T itself (vals remains owned by ScopedCMem).
  {
    ScopedCMem<NC_ATT_INFO_T> bad_type_att(
        static_cast<NC_ATT_INFO_T*>(std::calloc(1, sizeof(NC_ATT_INFO_T))));
    ASSERT_NE(nullptr, bad_type_att);
    ScopedCMem<char> name(AllocCString("standalone_bad_type"));
    ASSERT_NE(nullptr, name);
    ScopedCMem<int> vals(static_cast<int*>(std::calloc(1, sizeof(int))));
    ASSERT_NE(nullptr, vals);

    bad_type_att->hdr.sort = NCATT;
    bad_type_att->hdr.name = name.release();
    bad_type_att->container = reinterpret_cast<NC_OBJ*>(grp);
    bad_type_att->nc_typeid = NC_FIRSTUSERTYPEID + 999;
    bad_type_att->len = 1;
    bad_type_att->data = vals.get();
    EXPECT_EQ(NC_EBADTYPID, nc4_att_free(bad_type_att.release()));
  }

  // Standalone attribute with neither name nor data.
  {
    ScopedCMem<NC_ATT_INFO_T> empty_att(
        static_cast<NC_ATT_INFO_T*>(std::calloc(1, sizeof(NC_ATT_INFO_T))));
    ASSERT_NE(nullptr, empty_att);
    empty_att->hdr.sort = NCATT;
    EXPECT_EQ(NC_NOERR, nc4_att_free(empty_att.release()));
  }

  EXPECT_EQ(NC_NOERR, host_nc.close());
}

TEST(Nc4InternalTest, ReservedAttributesTableAndShowMetadata) {
  NC_initialize_reserved();

  struct ReservedExpectation {
    const char* name;
    int flags;
  };
  constexpr ReservedExpectation kReserved[] = {
      {NC_ATT_CLASS, READONLYFLAG | HIDDENATTRFLAG},
      {NC_ATT_DIMENSION_LIST, READONLYFLAG | HIDDENATTRFLAG},
      {NC_ATT_NAME, READONLYFLAG | HIDDENATTRFLAG},
      {NC_ATT_REFERENCE_LIST, READONLYFLAG | HIDDENATTRFLAG},
      {NC_XARRAY_DIMS, READONLYFLAG | HIDDENATTRFLAG},
      {NC_ATT_CODECS, VARFLAG | READONLYFLAG | NAMEONLYFLAG},
      {NC_ATT_FORMAT, READONLYFLAG},
      {ISNETCDF4ATT, READONLYFLAG | NAMEONLYFLAG | VIRTUALFLAG},
      {NCPROPS, READONLYFLAG | NAMEONLYFLAG | HIDDENATTRFLAG},
      {NC_ATT_COORDINATES, READONLYFLAG | HIDDENATTRFLAG},
      {NC_ATT_DIMID_NAME, READONLYFLAG | HIDDENATTRFLAG},
      {SUPERBLOCKATT, READONLYFLAG | NAMEONLYFLAG | VIRTUALFLAG},
      {NC_ATT_NC3_STRICT_NAME, READONLYFLAG},
      {NC_NCZARR_ATTR, READONLYFLAG | HIDDENATTRFLAG},
      {NC_NCZARR_GROUP, READONLYFLAG | HIDDENATTRFLAG},
      {NC_NCZARR_ARRAY, READONLYFLAG | HIDDENATTRFLAG},
      {NC_NCZARR_SUPERBLOCK, READONLYFLAG | HIDDENATTRFLAG},
  };

  for (const auto& entry : kReserved) {
    SCOPED_TRACE(entry.name);
    const NC_reservedatt* found = NC_findreserved(entry.name);
    ASSERT_NE(nullptr, found);
    EXPECT_STREQ(entry.name, found->name);
    EXPECT_EQ(entry.flags, found->flags);
  }

  constexpr const char* kNonReserved[] = {
      "000_before_all",    "M_between_entries", NC_FillValue,
      "_not_reserved_mid", "zzz_after_all",
  };
  for (const char* name : kNonReserved) {
    SCOPED_TRACE(name);
    EXPECT_EQ(nullptr, NC_findreserved(name));
  }

  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create_mem("show_meta.nc", NC_NETCDF4, 0, nc.ptr()));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 0, nullptr, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));
    const int32_t val = 7;
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), varid, &val));
    EXPECT_EQ(NC_NOERR, nc_show_metadata(nc.get()));
    EXPECT_EQ(NC_NOERR, NC4_show_metadata(nc.get()));
    // With LOGGING undefined in config.h, NC4_show_metadata returns NC_NOERR.
    EXPECT_EQ(NC_NOERR, NC4_show_metadata(-1));
    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR, nc_open_mem("show_meta.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_nc.ptr()));
  int is_nc4 = -1;
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(ro_nc.get(), NC_GLOBAL, ISNETCDF4ATT, &is_nc4));
  EXPECT_EQ(1, is_nc4);

  int superblock = -1;
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(ro_nc.get(), NC_GLOBAL, SUPERBLOCKATT, &superblock));
  EXPECT_GE(superblock, 0);

  size_t props_len = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_attlen(ro_nc.get(), NC_GLOBAL, NCPROPS, &props_len));
  std::string props(props_len + 1, '\0');
  ASSERT_EQ(NC_NOERR,
            nc_get_att_text(ro_nc.get(), NC_GLOBAL, NCPROPS, props.data()));
  props.resize(std::strlen(props.c_str()));
  EXPECT_THAT(props, ::testing::HasSubstr("version="));
  EXPECT_EQ(NC_NOERR, ro_nc.close());
}

TEST(Nc4InternalTest, ErrorPropagationInRecDelAndSizeOverflowOomPaths) {
  ASSERT_EQ(NC_NOERR, nc_initialize());
  ASSERT_NE(nullptr, HDF5_dispatch_table);

  NC* err_nc = nullptr;
  ASSERT_EQ(NC_NOERR, new_NC(HDF5_dispatch_table, "err_nc4internal.nc",
                             NC_NETCDF4, &err_nc));
  ASSERT_NE(nullptr, err_nc);
  ASSERT_EQ(NC_NOERR, add_to_NCList(err_nc));
  bool has_h5 = false;
  absl::Cleanup err_nc_cleanup = [&err_nc, &has_h5] {
    if (err_nc != nullptr) {
      if (has_h5 && err_nc->dispatchdata != nullptr) {
        EXPECT_EQ(NC_NOERR, nc4_file_list_del(err_nc->ext_ncid));
        err_nc->dispatchdata = nullptr;
      }
      del_from_NCList(err_nc);
      free_NC(err_nc);
      err_nc = nullptr;
    }
  };

  ASSERT_EQ(NC_NOERR, nc4_file_list_add(err_nc->ext_ncid, "err_nc4internal.nc",
                                        NC_NETCDF4, nullptr));
  has_h5 = true;

  NC_GRP_INFO_T* root_grp = nullptr;
  NC_FILE_INFO_T* h5 = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_find_grp_h5(err_nc->ext_ncid, &root_grp, &h5));
  ASSERT_NE(nullptr, root_grp);
  ASSERT_NE(nullptr, h5);

  NC_GRP_INFO_T* child_grp = nullptr;
  char child_name[] = "err_child";
  ASSERT_EQ(NC_NOERR, nc4_grp_list_add(h5, root_grp, child_name, &child_grp));
  ASSERT_NE(nullptr, child_grp);

  // 1. Error propagation from nc4_rec_grp_del_att_data(child) ->
  // nc4_rec_grp_del_att_data(root) -> nc4_nc4f_list_del -> nc4_file_list_del.
  NC_ATT_INFO_T* child_att = nullptr;
  ASSERT_EQ(NC_NOERR,
            nc4_att_list_add(child_grp->att, "err_child_att", &child_att));
  ASSERT_NE(nullptr, child_att);
  child_att->nc_typeid = NC_FIRSTUSERTYPEID + 999;
  {
    absl::Cleanup reset_child_att = [child_att] {
      child_att->nc_typeid = NC_INT;
    };
    EXPECT_EQ(NC_EBADTYPID, nc4_file_list_del(err_nc->ext_ncid));
  }

  // 2. Error propagation from a variable attribute in nc4_rec_grp_del_att_data.
  NC_VAR_INFO_T* v_att_err = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_var_list_add2(child_grp, "v_att_err", &v_att_err));
  ASSERT_NE(nullptr, v_att_err);
  NC_ATT_INFO_T* v_bad_att = nullptr;
  ASSERT_EQ(NC_NOERR,
            nc4_att_list_add(v_att_err->att, "err_var_att", &v_bad_att));
  ASSERT_NE(nullptr, v_bad_att);
  v_bad_att->nc_typeid = NC_FIRSTUSERTYPEID + 999;
  {
    absl::Cleanup reset_var_att = [v_bad_att] {
      v_bad_att->nc_typeid = NC_INT;
    };
    EXPECT_EQ(NC_EBADTYPID, nc4_rec_grp_del_att_data(child_grp));
  }
  EXPECT_EQ(NC_NOERR, nc4_var_list_del(child_grp, v_att_err));

  // 3. Error propagation inside nc4_var_list_del -> var_free -> nc4_att_free.
  {
    NC_VAR_INFO_T* v_del_err = nullptr;
    ASSERT_EQ(NC_NOERR, nc4_var_list_add2(child_grp, "v_del_err", &v_del_err));
    ASSERT_NE(nullptr, v_del_err);
    NC_ATT_INFO_T* att_err = nullptr;
    ASSERT_EQ(NC_NOERR, nc4_att_list_add(v_del_err->att, "att_err", &att_err));
    ASSERT_NE(nullptr, att_err);
    ScopedCMem<int> vals(static_cast<int*>(std::calloc(1, sizeof(int))));
    ASSERT_NE(nullptr, vals);

    att_err->container = reinterpret_cast<NC_OBJ*>(v_del_err);
    att_err->nc_typeid = NC_FIRSTUSERTYPEID + 999;
    att_err->len = 1;
    att_err->data = vals.get();

    const int del_rc = nc4_var_list_del(child_grp, v_del_err);
    EXPECT_EQ(NC_EBADTYPID, del_rc);
    if (del_rc != NC_NOERR) {
      ncindexfree(v_del_err->att);
      std::free(v_del_err->hdr.name);
      std::free(v_del_err);
    }
  }

  // 4. Error propagation inside nc4_rec_grp_del (child recursion & grp->att).
  {
    auto make_detached_grp = [h5](std::string_view name) -> NC_GRP_INFO_T* {
      auto* g =
          static_cast<NC_GRP_INFO_T*>(std::calloc(1, sizeof(NC_GRP_INFO_T)));
      if (g == nullptr) {
        return nullptr;
      }
      g->hdr.sort = NCGRP;
      g->hdr.name = AllocCString(name);
      g->nc4_info = h5;
      g->children = ncindexnew(0);
      g->dim = ncindexnew(0);
      g->att = ncindexnew(0);
      g->type = ncindexnew(0);
      g->vars = ncindexnew(0);
      if (g->hdr.name == nullptr || g->children == nullptr ||
          g->dim == nullptr || g->att == nullptr || g->type == nullptr ||
          g->vars == nullptr) {
        ncindexfree(g->children);
        ncindexfree(g->dim);
        ncindexfree(g->att);
        ncindexfree(g->type);
        ncindexfree(g->vars);
        std::free(g->hdr.name);
        std::free(g);
        return nullptr;
      }
      return g;
    };

    NC_GRP_INFO_T* parent_grp = make_detached_grp("detached_parent");
    NC_GRP_INFO_T* sub_grp = make_detached_grp("detached_sub");
    ASSERT_NE(nullptr, parent_grp);
    ASSERT_NE(nullptr, sub_grp);
    bool sub_children_freed = false;
    bool g_bad_att_freed = false;
    NC_ATT_INFO_T* g_bad_att = nullptr;
    absl::Cleanup detached_cleanup = [&parent_grp, &sub_grp,
                                      &sub_children_freed, &g_bad_att,
                                      &g_bad_att_freed] {
      if (!g_bad_att_freed && g_bad_att != nullptr) {
        std::free(g_bad_att->hdr.name);
        std::free(g_bad_att);
      }
      if (sub_grp != nullptr) {
        if (!sub_children_freed) {
          ncindexfree(sub_grp->children);
        }
        ncindexfree(sub_grp->dim);
        ncindexfree(sub_grp->att);
        ncindexfree(sub_grp->type);
        ncindexfree(sub_grp->vars);
        std::free(sub_grp->hdr.name);
        std::free(sub_grp);
      }
      if (parent_grp != nullptr) {
        ncindexfree(parent_grp->children);
        ncindexfree(parent_grp->dim);
        ncindexfree(parent_grp->att);
        ncindexfree(parent_grp->type);
        ncindexfree(parent_grp->vars);
        std::free(parent_grp->hdr.name);
        std::free(parent_grp);
      }
    };

    ASSERT_EQ(NC_NOERR,
              nc4_att_list_add(sub_grp->att, "g_bad_att", &g_bad_att));
    ASSERT_NE(nullptr, g_bad_att);
    ScopedCMem<int> vals(static_cast<int*>(std::calloc(1, sizeof(int))));
    ASSERT_NE(nullptr, vals);
    ASSERT_EQ(1, ncindexadd(parent_grp->children,
                            reinterpret_cast<NC_OBJ*>(sub_grp)));

    g_bad_att->container = reinterpret_cast<NC_OBJ*>(sub_grp);
    g_bad_att->nc_typeid = NC_FIRSTUSERTYPEID + 999;
    g_bad_att->len = 1;
    g_bad_att->data = vals.get();

    // nc4_rec_grp_del() enters sub_grp, frees sub_grp->children and g_bad_att,
    // and returns NC_EBADTYPID up through parent_grp without freeing
    // parent_grp.
    sub_children_freed = true;
    g_bad_att_freed = true;
    const int grp_del_rc = nc4_rec_grp_del(parent_grp);
    if (grp_del_rc == NC_NOERR) {
      sub_grp = nullptr;
      parent_grp = nullptr;
    }
    EXPECT_EQ(NC_EBADTYPID, grp_del_rc);
  }

#if defined(__has_feature)
#define NC4INTERNAL_HAS_FEATURE(x) __has_feature(x)
#else
#define NC4INTERNAL_HAS_FEATURE(x) 0
#endif

#if !defined(ADDRESS_SANITIZER) && !defined(__SANITIZE_ADDRESS__) &&          \
    !NC4INTERNAL_HAS_FEATURE(address_sanitizer) &&                            \
    !defined(HWADDRESS_SANITIZER) && !defined(__SANITIZE_HWADDRESS__) &&      \
    !NC4INTERNAL_HAS_FEATURE(hwaddress_sanitizer) &&                          \
    !defined(MEMORY_SANITIZER) &&                                             \
    !NC4INTERNAL_HAS_FEATURE(memory_sanitizer) &&                             \
    !defined(THREAD_SANITIZER) && !defined(__SANITIZE_THREAD__) &&            \
    !NC4INTERNAL_HAS_FEATURE(thread_sanitizer) && !defined(LEAK_SANITIZER) && \
    !NC4INTERNAL_HAS_FEATURE(leak_sanitizer)
  // 5. Overflow/exabyte allocation sizes deterministically trigger NC_ENOMEM in
  // calloc/malloc without affecting process memory. Skipped under sanitizers
  // whose allocators abort on oversized allocation requests.
  {
    NC_VAR_INFO_T* oom_var = nullptr;
    EXPECT_EQ(NC_ENOMEM, nc4_var_list_add(child_grp, "oom_var", -1, &oom_var));
    ASSERT_NE(nullptr, oom_var);
    oom_var->ndims = 0;
    EXPECT_EQ(NC_NOERR, nc4_var_list_del(child_grp, oom_var));

    EXPECT_EQ(NC_ENOMEM,
              nc4_field_list_add(nullptr, "oom_field", 0, NC_INT, -1, nullptr));

    const int dummy_enum_val = 42;
    EXPECT_EQ(NC_ENOMEM,
              nc4_enum_member_add(nullptr, SIZE_MAX, "oom_enum_member",
                                  &dummy_enum_val));
  }
#endif
#undef NC4INTERNAL_HAS_FEATURE

  // 6. Error propagation from var->fill_value -> var_free -> nc4_rec_grp_del
  // -> nc4_nc4f_list_del -> nc4_file_list_del, followed by clean teardown.
  const size_t child_grpid = static_cast<size_t>(child_grp->hdr.id);
  NC_VAR_INFO_T* v_fill_err = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_var_list_add2(root_grp, "v_fill_err", &v_fill_err));
  ASSERT_NE(nullptr, v_fill_err);
  ScopedCMem<int> fill_mem(static_cast<int*>(std::calloc(1, sizeof(int))));
  ASSERT_NE(nullptr, fill_mem);
  NC_TYPE_INFO_T* bad_type = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_type_new(sizeof(int), "bad_fill_type",
                                   NC_FIRSTUSERTYPEID + 999, &bad_type));
  ASSERT_NE(nullptr, bad_type);
  bad_type->rc = 1;
  v_fill_err->type_info = bad_type;
  v_fill_err->fill_value = fill_mem.release();

  const int fill_err_rc = nc4_file_list_del(err_nc->ext_ncid);
  EXPECT_EQ(NC_EBADTYPID, fill_err_rc);
  if (fill_err_rc == NC_NOERR) {
    has_h5 = false;
  } else {
    bad_type->hdr.id = NC_INT;
    nclistset(h5->allgroups, child_grpid, nullptr);
    root_grp->children = ncindexnew(0);
    root_grp->att = ncindexnew(0);
    v_fill_err->att = ncindexnew(0);
  }
}

}  // namespace
}  // namespace netcdf
