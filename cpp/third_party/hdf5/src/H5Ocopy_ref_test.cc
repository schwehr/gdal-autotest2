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
// Tests copying HDF5 objects with object, region, and attribute references
// (H5O_COPY_EXPAND_REFERENCE_FLAG) across legacy H5R_OBJECT1 /
// H5R_DATASET_REGION1 and modern H5R_OBJECT2 / H5R_DATASET_REGION2 / H5R_ATTR
// reference types, plus internal H5O_copy_expand_ref error paths
// (third_party/hdf5/src/H5Ocopy_ref.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ocopy_ref.c

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match its struct layout for internal headers.
#ifndef NDEBUG
#define H5O_COPY_REF_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5F_FRIEND
#define H5O_FRIEND
#define H5R_FRIEND
#define H5T_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FLprivate.h"
// H5Fpkg.h declares a `bool try` parameter in H5F__efc_open, which collides
// with the C++ `try` keyword.
#pragma push_macro("try")
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#undef try
#define try h5fpkg_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#undef try
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Rpkg.h"
#include "third_party/hdf5/src/H5SLprivate.h"
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
typedef unsigned short H5__Float16;
#endif
#include "third_party/hdf5/src/H5Tpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
H5FL_EXTERN(H5O_addr_map_t);
}  // extern "C"
#ifdef H5O_COPY_REF_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5O_COPY_REF_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

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

H5F_t* GetH5File(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5T_t* GetH5Type(hid_t type_id) {
  return static_cast<H5T_t*>(H5I_object_verify(type_id, H5I_DATATYPE));
}

TEST(H5OcopyRefTest, LegacyObjectAndRegionReferencesExpandedInDatasetsAndAttrs) {
  ScopedFile src = CreateInMemoryFile("ref1_src.h5");
  ScopedFile dst = CreateInMemoryFile("ref1_dst.h5");
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());

  // Target objects in source file: a group, a 2D dataset, a named datatype.
  ScopedGroup tgt_grp(
      H5Gcreate2(src.get(), "/tgt_grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(tgt_grp.valid());

  const hsize_t ddims[2] = {4, 4};
  ScopedSpace dspace(H5Screate_simple(2, ddims, nullptr));
  ASSERT_TRUE(dspace.valid());
  ScopedDataset tgt_dset(H5Dcreate2(src.get(), "/tgt_dset", H5T_NATIVE_INT,
                                    dspace.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
  ASSERT_TRUE(tgt_dset.valid());
  int vals[16];
  for (int i = 0; i < 16; ++i) vals[i] = i + 100;
  ASSERT_GE(H5Dwrite(tgt_dset.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, vals),
            0);

  ScopedType tgt_type(H5Tcopy(H5T_STD_I32LE));
  ASSERT_GE(H5Tcommit2(src.get(), "/tgt_type", tgt_type.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Build legacy object references: null, group, dset, type, and duplicate dset
  // (exercises H5O__copy_obj_by_ref reuse where H5O_copy_header_map returns 0).
  std::array<hobj_ref_t, 5> obj_refs{};
  ASSERT_GE(H5Rcreate(&obj_refs[1], src.get(), "/tgt_grp", H5R_OBJECT, -1), 0);
  ASSERT_GE(H5Rcreate(&obj_refs[2], src.get(), "/tgt_dset", H5R_OBJECT, -1), 0);
  ASSERT_GE(H5Rcreate(&obj_refs[3], src.get(), "/tgt_type", H5R_OBJECT, -1), 0);
  ASSERT_GE(H5Rcreate(&obj_refs[4], src.get(), "/tgt_dset", H5R_OBJECT, -1), 0);

  // Build legacy dataset region references: null, hyperslab, and duplicate.
  std::array<hdset_reg_ref_t, 3> reg_refs{};
  {
    const hsize_t start[2] = {1, 1};
    const hsize_t count[2] = {2, 2};
    ASSERT_GE(
        H5Sselect_hyperslab(dspace.get(), H5S_SELECT_SET, start, nullptr, count,
                            nullptr),
        0);
    ASSERT_GE(H5Rcreate(&reg_refs[1], src.get(), "/tgt_dset",
                        H5R_DATASET_REGION, dspace.get()),
              0);
    ASSERT_GE(H5Rcreate(&reg_refs[2], src.get(), "/tgt_dset",
                        H5R_DATASET_REGION, dspace.get()),
              0);
  }
  // Close referenced targets before H5Ocopy so recursive H5O__copy_header_real
  // keeps cpy_info->shared_fo NULL across multi-chunk iteration.
  tgt_type.reset();
  tgt_dset.reset();
  tgt_grp.reset();

  // Write legacy obj_ref dataset (chunked) + compact obj_ref attribute +
  // region_ref dataset + region_ref attribute.
  {
    const hsize_t odim = obj_refs.size();
    const hsize_t ocdim = 2;
    ScopedSpace ospace(H5Screate_simple(1, &odim, nullptr));
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &ocdim), 0);
    ScopedDataset ds(H5Dcreate2(src.get(), "/obj_ref_dset", H5T_STD_REF_OBJ,
                                ospace.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    ASSERT_GE(H5Dwrite(ds.get(), H5T_STD_REF_OBJ, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       obj_refs.data()),
              0);

    ScopedAttr att(H5Acreate2(ds.get(), "obj_ref_attr", H5T_STD_REF_OBJ,
                              ospace.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(att.valid());
    ASSERT_GE(H5Awrite(att.get(), H5T_STD_REF_OBJ, obj_refs.data()), 0);
  }
  {
    const hsize_t rdim = reg_refs.size();
    ScopedSpace rspace(H5Screate_simple(1, &rdim, nullptr));
    ScopedDataset ds(H5Dcreate2(src.get(), "/reg_ref_dset",
                                H5T_STD_REF_DSETREG, rspace.get(), H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    ASSERT_GE(H5Dwrite(ds.get(), H5T_STD_REF_DSETREG, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, reg_refs.data()),
              0);

    ScopedAttr att(H5Acreate2(ds.get(), "reg_ref_attr", H5T_STD_REF_DSETREG,
                              rspace.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(att.valid());
    ASSERT_GE(H5Awrite(att.get(), H5T_STD_REF_DSETREG, reg_refs.data()), 0);
  }

  ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpypl.valid());
  ASSERT_GE(H5Pset_copy_object(ocpypl.get(), H5O_COPY_EXPAND_REFERENCE_FLAG),
            0);

  ASSERT_GE(H5Ocopy(src.get(), "/obj_ref_dset", dst.get(), "/dst_obj_ref_dset",
                    ocpypl.get(), H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ocopy(src.get(), "/reg_ref_dset", dst.get(), "/dst_reg_ref_dset",
                    ocpypl.get(), H5P_DEFAULT),
            0);

  // Verify copied references dereference cleanly inside destination file.
  {
    ScopedDataset dset(H5Dopen2(dst.get(), "/dst_obj_ref_dset", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::array<hobj_ref_t, 5> dst_refs{};
    ASSERT_GE(H5Dread(dset.get(), H5T_STD_REF_OBJ, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, dst_refs.data()),
              0);
    EXPECT_EQ(dst_refs[0], 0U);
    H5O_type_t otype = H5O_TYPE_UNKNOWN;
    ASSERT_GE(
        H5Rget_obj_type2(dset.get(), H5R_OBJECT, &dst_refs[1], &otype), 0);
    EXPECT_EQ(otype, H5O_TYPE_GROUP);
    ASSERT_GE(
        H5Rget_obj_type2(dset.get(), H5R_OBJECT, &dst_refs[2], &otype), 0);
    EXPECT_EQ(otype, H5O_TYPE_DATASET);
    EXPECT_EQ(dst_refs[2], dst_refs[4]);
  }
  {
    ScopedDataset dset(H5Dopen2(dst.get(), "/dst_reg_ref_dset", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    std::array<hdset_reg_ref_t, 3> dst_reg_refs{};
    ASSERT_GE(H5Dread(dset.get(), H5T_STD_REF_DSETREG, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, dst_reg_refs.data()),
              0);
    ScopedSpace reg_sp(
        H5Rget_region(dset.get(), H5R_DATASET_REGION, &dst_reg_refs[1]));
    ASSERT_TRUE(reg_sp.valid());
    EXPECT_EQ(H5Sget_select_npoints(reg_sp.get()), 4);
  }
}

TEST(H5OcopyRefTest, ModernObjectRegionAndAttrReferencesExpanded) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
      0);

  ScopedFile src(
      H5Fcreate("ref2_src.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ScopedFile dst(
      H5Fcreate("ref2_dst.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());

  // Target dataset with attribute for H5R_OBJECT2, H5R_DATASET_REGION2,
  // H5R_ATTR.
  const hsize_t ddims[1] = {6};
  ScopedSpace dspace(H5Screate_simple(1, ddims, nullptr));
  ScopedDataset tgt(H5Dcreate2(src.get(), "/target", H5T_NATIVE_INT,
                               dspace.get(), H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
  ASSERT_TRUE(tgt.valid());
  const int data[6] = {10, 11, 12, 13, 14, 15};
  ASSERT_GE(H5Dwrite(tgt.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     data),
            0);

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ScopedAttr tgt_attr(H5Acreate2(tgt.get(), "tag", H5T_NATIVE_INT, scalar.get(),
                                 H5P_DEFAULT, H5P_DEFAULT));
  const int attr_val = 77;
  ASSERT_GE(H5Awrite(tgt_attr.get(), H5T_NATIVE_INT, &attr_val), 0);

  // Create modern H5R_ref_t array: [null, obj, region, attr, dup obj].
  std::array<H5R_ref_t, 5> refs{};
  absl::Cleanup destroy_refs = [&] {
    for (size_t i = 1; i < refs.size(); ++i) {
      H5Rdestroy(&refs[i]);
    }
  };
  ASSERT_GE(H5Rcreate_object(src.get(), "/target", H5P_DEFAULT, &refs[1]), 0);
  const hsize_t start[1] = {2};
  const hsize_t count[1] = {3};
  ASSERT_GE(H5Sselect_hyperslab(dspace.get(), H5S_SELECT_SET, start, nullptr,
                                count, nullptr),
            0);
  ASSERT_GE(
      H5Rcreate_region(src.get(), "/target", dspace.get(), H5P_DEFAULT,
                       &refs[2]),
      0);
  ASSERT_GE(
      H5Rcreate_attr(src.get(), "/target", "tag", H5P_DEFAULT, &refs[3]), 0);
  ASSERT_GE(H5Rcreate_object(src.get(), "/target", H5P_DEFAULT, &refs[4]), 0);

  const hsize_t rdim = refs.size();
  ScopedSpace rspace(H5Screate_simple(1, &rdim, nullptr));
  ScopedDataset holder(H5Dcreate2(src.get(), "/modern_refs", H5T_STD_REF,
                                  rspace.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
  ASSERT_TRUE(holder.valid());
  ASSERT_GE(H5Dwrite(holder.get(), H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     refs.data()),
            0);

  ScopedAttr holder_attr(H5Acreate2(holder.get(), "modern_attr", H5T_STD_REF,
                                    rspace.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(H5Awrite(holder_attr.get(), H5T_STD_REF, refs.data()), 0);
  holder_attr.reset();
  holder.reset();
  tgt_attr.reset();
  tgt.reset();

  ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpypl.valid());
  ASSERT_GE(H5Pset_copy_object(ocpypl.get(), H5O_COPY_EXPAND_REFERENCE_FLAG),
            0);
  ASSERT_GE(H5Ocopy(src.get(), "/modern_refs", dst.get(), "/dst_modern_refs",
                    ocpypl.get(), H5P_DEFAULT),
            0);

  // Read back from destination and validate obj, region, attr references.
  ScopedDataset dst_holder(
      H5Dopen2(dst.get(), "/dst_modern_refs", H5P_DEFAULT));
  ASSERT_TRUE(dst_holder.valid());
  std::array<H5R_ref_t, 5> dst_refs{};
  ASSERT_GE(H5Dread(dst_holder.get(), H5T_STD_REF, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, dst_refs.data()),
            0);
  absl::Cleanup destroy_dst_refs = [&] {
    for (H5R_ref_t& r : dst_refs) {
      H5Rdestroy(&r);
    }
  };
  EXPECT_EQ(H5Rget_type(&dst_refs[1]), H5R_OBJECT2);
  EXPECT_EQ(H5Rget_type(&dst_refs[2]), H5R_DATASET_REGION2);
  EXPECT_EQ(H5Rget_type(&dst_refs[3]), H5R_ATTR);

  ScopedAttr opened_attr(H5Ropen_attr(&dst_refs[3], H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(opened_attr.valid());
  int out_attr_val = -1;
  ASSERT_GE(H5Aread(opened_attr.get(), H5T_NATIVE_INT, &out_attr_val), 0);
  EXPECT_EQ(out_attr_val, 77);
}

herr_t FreeAddrMapCb(void* _item, void* /*key*/, void* /*op_data*/) {
  auto* item = static_cast<H5O_addr_map_t*>(_item);
  if (item->udata && item->obj_class &&
      item->obj_class->free_copy_file_udata) {
    item->obj_class->free_copy_file_udata(item->udata);
  }
  H5FL_FREE(H5O_addr_map_t, item);
  return 0;
}

herr_t NoopConvCb(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                  size_t /*nelmts*/, size_t /*buf_stride*/,
                  size_t /*bkg_stride*/, void* /*buf*/, void* /*bkg*/,
                  hid_t /*dxpl_id*/) {
  if (cdata->command == H5T_CONV_INIT) {
    cdata->need_bkg = H5T_BKG_NO;
  }
  return 0;
}

struct RefErrHook {
  bool attach_immutable_parents = false;
  H5T_t* dt_mem = nullptr;
  H5T_t* dt_dst = nullptr;
  H5T_t* parent_mem = nullptr;
  H5T_t* parent_dst = nullptr;
  hid_t close_file_id = H5I_INVALID_HID;
};

RefErrHook* g_ref_err_hook = nullptr;

herr_t RestoreRefNoopCb(hid_t src_id, hid_t dst_id, H5T_cdata_t* cdata,
                        size_t /*nelmts*/, size_t /*buf_stride*/,
                        size_t /*bkg_stride*/, void* /*buf*/, void* /*bkg*/,
                        hid_t /*dxpl_id*/) {
  if (cdata->command == H5T_CONV_INIT) {
    H5T_t* src = GetH5Type(src_id);
    H5T_t* dst = GetH5Type(dst_id);
    if (src == nullptr || dst == nullptr ||
        src->shared->u.atomic.u.r.opaque || dst->shared->u.atomic.u.r.opaque ||
        src->shared->u.atomic.u.r.rtype != dst->shared->u.atomic.u.r.rtype) {
      return -1;
    }
    cdata->need_bkg = H5T_BKG_NO;
  }
  return 0;
}

herr_t OnlyMemRefCb(hid_t src_id, hid_t dst_id, H5T_cdata_t* cdata,
                    size_t /*nelmts*/, size_t /*buf_stride*/,
                    size_t /*bkg_stride*/, void* /*buf*/, void* /*bkg*/,
                    hid_t /*dxpl_id*/) {
  if (cdata->command == H5T_CONV_INIT) {
    H5T_t* src = GetH5Type(src_id);
    H5T_t* dst = GetH5Type(dst_id);
    if (src == nullptr || dst == nullptr) {
      return -1;
    }
    // Accept opaque refs so H5Tregister()'s scan of existing cached "ref"
    // paths in H5T_g.path does not fail on H5T.c's sticky ret_value;
    // unregistering "only_mem_ref" purges those cached entries cleanly so
    // "ref" repopulates them on subsequent calls.
    if (!src->shared->u.atomic.u.r.opaque && !dst->shared->u.atomic.u.r.opaque) {
      if (src->shared->u.atomic.u.r.loc != H5T_LOC_MEMORY ||
          dst->shared->u.atomic.u.r.loc != H5T_LOC_MEMORY) {
        return -1;
      }
    }
    cdata->need_bkg = H5T_BKG_NO;
  }
  return 0;
}

herr_t FailOnConvCb(hid_t src_id, hid_t dst_id, H5T_cdata_t* cdata,
                    size_t /*nelmts*/, size_t /*buf_stride*/,
                    size_t /*bkg_stride*/, void* /*buf*/, void* /*bkg*/,
                    hid_t /*dxpl_id*/) {
  if (cdata->command == H5T_CONV_INIT) {
    cdata->need_bkg = H5T_BKG_NO;
    return 0;
  }
  if (cdata->command == H5T_CONV_CONV) {
    if (g_ref_err_hook != nullptr) {
      if (g_ref_err_hook->attach_immutable_parents &&
          g_ref_err_hook->parent_mem != nullptr &&
          g_ref_err_hook->parent_dst != nullptr) {
        H5T_t* m = GetH5Type(src_id);
        H5T_t* d = GetH5Type(dst_id);
        if (m != nullptr && d != nullptr) {
          g_ref_err_hook->parent_mem->shared->state = H5T_STATE_IMMUTABLE;
          g_ref_err_hook->parent_dst->shared->state = H5T_STATE_IMMUTABLE;
          m->shared->parent = g_ref_err_hook->parent_mem;
          d->shared->parent = g_ref_err_hook->parent_dst;
          g_ref_err_hook->dt_mem = m;
          g_ref_err_hook->dt_dst = d;
        }
      }
      if (g_ref_err_hook->close_file_id != H5I_INVALID_HID) {
        H5I_dec_app_ref(g_ref_err_hook->close_file_id);
        H5I_dec_ref(g_ref_err_hook->close_file_id);
        g_ref_err_hook->close_file_id = H5I_INVALID_HID;
      }
    }
    return -1;
  }
  return 0;
}

TEST(H5OcopyRefTest, InternalExpandRefErrorPaths) {
  // Use 4-byte file addresses so an 8-byte hobj_ref_t with 4 zero addr bytes +
  // trailing non-zero padding fails zero-memcmp yet decodes address 0.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_sizes(fcpl.get(), 4, 4), 0);

  ScopedFile src = CreateInMemoryFile("ref_err_src.h5", H5F_ACC_TRUNC,
                                      fcpl.get());
  ScopedFile dst = CreateInMemoryFile("ref_err_dst.h5", H5F_ACC_TRUNC,
                                      fcpl.get());
  ASSERT_TRUE(src.valid());
  ASSERT_TRUE(dst.valid());

  // Create real target group + 1D dataset so we can construct valid legacy
  // object/region refs and valid H5R_OBJECT2 tokens via public APIs.
  const hsize_t dim = 4;
  ScopedSpace dspace(H5Screate_simple(1, &dim, nullptr));
  ASSERT_TRUE(dspace.valid());
  ScopedDataset tgt_ds(H5Dcreate2(src.get(), "/tgt_ds", H5T_NATIVE_INT,
                                  dspace.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
  ASSERT_TRUE(tgt_ds.valid());
  hobj_ref_t real_obj1{};
  ASSERT_GE(H5Rcreate(&real_obj1, src.get(), "/tgt_ds", H5R_OBJECT, -1), 0);
  hdset_reg_ref_t real_reg1{};
  const hsize_t start = 0;
  const hsize_t count = 2;
  ASSERT_GE(H5Sselect_hyperslab(dspace.get(), H5S_SELECT_SET, &start, nullptr,
                                &count, nullptr),
            0);
  ASSERT_GE(H5Rcreate(&real_reg1, src.get(), "/tgt_ds", H5R_DATASET_REGION,
                      dspace.get()),
            0);

  H5R_ref_t real_obj2{};
  ASSERT_GE(H5Rcreate_object(src.get(), "/tgt_ds", H5P_DEFAULT, &real_obj2), 0);
  absl::Cleanup destroy_real_obj2 = [&] { H5Rdestroy(&real_obj2); };
  H5O_token_t real_tok{};
  size_t real_tok_size = 0;
  {
    const auto* priv = reinterpret_cast<const H5R_ref_priv_t*>(&real_obj2);
    ASSERT_GE(H5R__get_obj_token(priv, &real_tok, &real_tok_size), 0);
    ASSERT_GT(real_tok_size, 0U);
  }
  H5O_loc_t* tgt_oloc = H5O_get_loc(tgt_ds.get());
  ASSERT_NE(tgt_oloc, nullptr);
  const haddr_t tgt_addr = tgt_oloc->addr;
  tgt_ds.reset();

  // Create a read-only on-disk destination file for testing H5R__encode_heap
  // write failure in H5O__copy_expand_ref_region1.
  TempFile ro_dst_path("ref_err_ro_dst.h5");
  {
    ScopedFile ro_init(H5Fcreate(ro_dst_path.c_str(), H5F_ACC_TRUNC,
                                 fcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(ro_init.valid());
  }
  ScopedFile ro_dst(
      H5Fopen(ro_dst_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(ro_dst.valid());

  H5F_t* f_src = GetH5File(src.get());
  H5F_t* f_dst = GetH5File(dst.get());
  H5F_t* f_ro_dst = GetH5File(ro_dst.get());
  ASSERT_NE(f_src, nullptr);
  ASSERT_NE(f_dst, nullptr);
  ASSERT_NE(f_ro_dst, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  H5O_copy_t cpy_info{};
  cpy_info.lcpl_id = H5P_DEFAULT;
  cpy_info.expand_ref = true;
  cpy_info.file_dst = f_dst;
  cpy_info.map_list = H5SL_create(H5SL_TYPE_OBJ, nullptr);
  ASSERT_NE(cpy_info.map_list, nullptr);
  absl::Cleanup free_map = [&] {
    H5SL_destroy(cpy_info.map_list, FreeAddrMapCb, nullptr);
  };

  // Pre-build a reference dtype + on-disk variant before entering
  // ScopedErrorSilence (also ensure unregister happens before files close).
  ScopedType dt_disk_id(H5Tcopy(H5T_STD_REF));
  ASSERT_TRUE(dt_disk_id.valid());
  H5T_t* dt_disk = GetH5Type(dt_disk_id.get());
  ASSERT_NE(dt_disk, nullptr);
  ASSERT_GE(H5T_set_loc(dt_disk, H5F_VOL_OBJ(f_src), H5T_LOC_DISK), 0);

  ScopedType dt_bad_loc_id(H5Tcopy(H5T_STD_REF));
  H5T_t* dt_bad_loc = GetH5Type(dt_bad_loc_id.get());
  ASSERT_NE(dt_bad_loc, nullptr);
  dt_bad_loc->shared->u.atomic.offset = 8;
  dt_bad_loc->shared->u.atomic.u.r.opaque = false;
  dt_bad_loc->shared->u.atomic.u.r.loc = H5T_LOC_BADLOC;

  ScopedType dt_disk2_id(H5Tcopy(H5T_STD_REF));
  ScopedType dt_mem2_id(H5Tcopy(H5T_STD_REF));
  ScopedType dt_dst2_id(H5Tcopy(H5T_STD_REF));
  H5T_t* dt_disk2 = GetH5Type(dt_disk2_id.get());
  H5T_t* dt_mem2 = GetH5Type(dt_mem2_id.get());
  H5T_t* dt_dst2 = GetH5Type(dt_dst2_id.get());
  ASSERT_NE(dt_disk2, nullptr);
  ASSERT_NE(dt_mem2, nullptr);
  ASSERT_NE(dt_dst2, nullptr);
  dt_disk2->shared->u.atomic.offset = 2;
  dt_mem2->shared->u.atomic.offset = 2;
  dt_dst2->shared->u.atomic.offset = 2;
  ASSERT_GE(H5T_set_loc(dt_disk2, H5F_VOL_OBJ(f_src), H5T_LOC_DISK), 0);
  ASSERT_GE(H5T_set_loc(dt_dst2, H5F_VOL_OBJ(f_dst), H5T_LOC_DISK), 0);
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "test_ref_381_fail", dt_mem2_id.get(),
                        dt_dst2_id.get(), FailOnConvCb),
            0);
  absl::Cleanup unreg_381 = [&] {
    H5Tunregister(H5T_PERS_HARD, "test_ref_381_fail", dt_mem2_id.get(),
                  dt_dst2_id.get(), FailOnConvCb);
  };

  ScopedType dt_disk3_id(H5Tcopy(H5T_STD_REF));
  ScopedType dt_mem3_id(H5Tcopy(H5T_STD_REF));
  ScopedType dt_dst3_id(H5Tcopy(H5T_STD_REF));
  H5T_t* dt_disk3 = GetH5Type(dt_disk3_id.get());
  H5T_t* dt_mem3 = GetH5Type(dt_mem3_id.get());
  H5T_t* dt_dst3 = GetH5Type(dt_dst3_id.get());
  ASSERT_NE(dt_disk3, nullptr);
  ASSERT_NE(dt_mem3, nullptr);
  ASSERT_NE(dt_dst3, nullptr);
  dt_disk3->shared->u.atomic.offset = 3;
  dt_mem3->shared->u.atomic.offset = 3;
  dt_dst3->shared->u.atomic.offset = 3;
  ASSERT_GE(H5T_set_loc(dt_disk3, H5F_VOL_OBJ(f_src), H5T_LOC_DISK), 0);
  ASSERT_GE(H5T_set_loc(dt_dst3, H5F_VOL_OBJ(f_dst), H5T_LOC_DISK), 0);
  dt_disk3->shared->size = sizeof(H5R_ref_priv_t);
  dt_disk3->shared->u.atomic.prec = 8 * sizeof(H5R_ref_priv_t);
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "test_ref_386_r", dt_disk3_id.get(),
                        dt_mem3_id.get(), NoopConvCb),
            0);
  absl::Cleanup unreg_386_r = [&] {
    H5Tunregister(H5T_PERS_HARD, "test_ref_386_r", dt_disk3_id.get(),
                  dt_mem3_id.get(), NoopConvCb);
  };
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "test_ref_386_w", dt_mem3_id.get(),
                        dt_dst3_id.get(), NoopConvCb),
            0);
  absl::Cleanup unreg_386_w = [&] {
    H5Tunregister(H5T_PERS_HARD, "test_ref_386_w", dt_mem3_id.get(),
                  dt_dst3_id.get(), NoopConvCb);
  };

  ScopedType dt_disk4_id(H5Tcopy(H5T_STD_REF));
  ScopedType dt_mem4_id(H5Tcopy(H5T_STD_REF));
  H5T_t* dt_disk4 = GetH5Type(dt_disk4_id.get());
  H5T_t* dt_mem4 = GetH5Type(dt_mem4_id.get());
  ASSERT_NE(dt_disk4, nullptr);
  ASSERT_NE(dt_mem4, nullptr);
  dt_disk4->shared->u.atomic.offset = 4;
  dt_mem4->shared->u.atomic.offset = 4;
  ASSERT_GE(H5T_set_loc(dt_disk4, H5F_VOL_OBJ(f_src), H5T_LOC_DISK), 0);
  dt_disk4->shared->size = sizeof(H5R_ref_priv_t);
  dt_disk4->shared->u.atomic.prec = 8 * sizeof(H5R_ref_priv_t);
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "test_ref_368_r", dt_disk4_id.get(),
                        dt_mem4_id.get(), NoopConvCb),
            0);
  absl::Cleanup unreg_368_r = [&] {
    H5Tunregister(H5T_PERS_HARD, "test_ref_368_r", dt_disk4_id.get(),
                  dt_mem4_id.get(), NoopConvCb);
  };

  ScopedFile tmp_dst_400 =
      CreateInMemoryFile("ref_err_close_400.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(tmp_dst_400.valid());
  H5F_t* f_tmp_dst = GetH5File(tmp_dst_400.get());
  ASSERT_NE(f_tmp_dst, nullptr);

  ScopedType dt_disk5_id(H5Tcopy(H5T_STD_REF));
  ScopedType dt_mem5_id(H5Tcopy(H5T_STD_REF));
  ScopedType dt_dst5_id(H5Tcopy(H5T_STD_REF));
  H5T_t* dt_disk5 = GetH5Type(dt_disk5_id.get());
  H5T_t* dt_mem5 = GetH5Type(dt_mem5_id.get());
  H5T_t* dt_dst5 = GetH5Type(dt_dst5_id.get());
  ASSERT_NE(dt_disk5, nullptr);
  ASSERT_NE(dt_mem5, nullptr);
  ASSERT_NE(dt_dst5, nullptr);
  dt_disk5->shared->u.atomic.offset = 5;
  dt_mem5->shared->u.atomic.offset = 5;
  dt_dst5->shared->u.atomic.offset = 5;
  ASSERT_GE(H5T_set_loc(dt_disk5, H5F_VOL_OBJ(f_src), H5T_LOC_DISK), 0);
  ASSERT_GE(H5T_set_loc(dt_dst5, H5F_VOL_OBJ(f_tmp_dst), H5T_LOC_DISK), 0);
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "test_ref_400_fail", dt_mem5_id.get(),
                        dt_dst5_id.get(), FailOnConvCb),
            0);
  absl::Cleanup unreg_400 = [&] {
    H5Tunregister(H5T_PERS_HARD, "test_ref_400_fail", dt_mem5_id.get(),
                  dt_dst5_id.get(), FailOnConvCb);
  };

  ScopedType dt_322_id(H5Tcopy(H5T_STD_REF));
  H5T_t* dt_322 = GetH5Type(dt_322_id.get());
  ASSERT_NE(dt_322, nullptr);
  dt_322->shared->u.atomic.offset = 6;
  dt_322->shared->u.atomic.u.r.opaque = false;

  ScopedType dt_324_id(H5Tcopy(H5T_STD_REF));
  H5T_t* dt_324 = GetH5Type(dt_324_id.get());
  ASSERT_NE(dt_324, nullptr);
  dt_324->shared->u.atomic.offset = 7;
  dt_324->shared->u.atomic.u.r.opaque = false;

  ScopedErrorSilence silence;

  // 1. Invalid datatype class -> H5R_BADTYPE / default switch branch.
  {
    std::array<uint8_t, 8> src_buf{};
    std::array<uint8_t, 8> dst_buf{};
    H5T_t* dt_int = GetH5Type(H5T_NATIVE_INT);
    ASSERT_NE(dt_int, nullptr);
    EXPECT_LT(H5O_copy_expand_ref(f_src, dt_int, src_buf.data(),
                                  sizeof(int), f_dst, dst_buf.data(),
                                  &cpy_info),
              0);
  }

  // 2. H5R_OBJECT1 error paths: zero decoded addr, bogus addr, oversized
  // token_size for decode_token_obj_compat, and H5L_link failure on invalid lcpl.
  {
    H5T_t* dt_obj1 = GetH5Type(H5T_STD_REF_OBJ);
    ASSERT_NE(dt_obj1, nullptr);
    std::array<uint8_t, H5R_OBJ_REF_BUF_SIZE> bad_zero_addr{};
    bad_zero_addr[H5R_OBJ_REF_BUF_SIZE - 1] = 0x01;
    std::array<uint8_t, H5R_OBJ_REF_BUF_SIZE> dst_buf{};
    EXPECT_LT(H5O_copy_expand_ref(f_src, dt_obj1, bad_zero_addr.data(),
                                  bad_zero_addr.size(), f_dst, dst_buf.data(),
                                  &cpy_info),
              0);

    std::array<uint8_t, H5R_OBJ_REF_BUF_SIZE> bogus_addr{};
    bogus_addr[0] = 0xFF;
    bogus_addr[1] = 0xEE;
    EXPECT_LT(H5O_copy_expand_ref(f_src, dt_obj1, bogus_addr.data(),
                                  bogus_addr.size(), f_dst, dst_buf.data(),
                                  &cpy_info),
              0);

    H5F_t dummy_src = *f_src;
    H5F_shared_t dummy_sh = *f_src->shared;
    dummy_sh.sizeof_addr = 16;
    dummy_src.shared = &dummy_sh;
    EXPECT_LT(H5O_copy_expand_ref(&dummy_src, dt_obj1, bogus_addr.data(),
                                  bogus_addr.size(), f_dst, dst_buf.data(),
                                  &cpy_info),
              0);

    cpy_info.lcpl_id = H5I_INVALID_HID;
    EXPECT_LT(H5O_copy_expand_ref(f_src, dt_obj1, &real_obj1,
                                  sizeof(real_obj1), f_dst, dst_buf.data(),
                                  &cpy_info),
              0);
    cpy_info.lcpl_id = H5P_DEFAULT;
  }

  // 3. H5R_DATASET_REGION1 error paths: corrupt heap ID, zero decoded addr in
  // heap, bogus nonzero addr in heap, and read-only destination heap write fail.
  {
    H5T_t* dt_reg1 = GetH5Type(H5T_STD_REF_DSETREG);
    ASSERT_NE(dt_reg1, nullptr);
    std::array<uint8_t, H5R_DSET_REG_REF_BUF_SIZE> bogus_reg{};
    bogus_reg[0] = 0xAA;
    std::array<uint8_t, H5R_DSET_REG_REF_BUF_SIZE> dst_buf{};
    EXPECT_LT(H5O_copy_expand_ref(f_src, dt_reg1, bogus_reg.data(),
                                  bogus_reg.size(), f_dst, dst_buf.data(),
                                  &cpy_info),
              0);

    // Heap record with zero object address triggers lines 248-250.
    std::array<uint8_t, 16> zero_payload{};
    std::array<uint8_t, H5R_DSET_REG_REF_BUF_SIZE> zero_heap_ref{};
    size_t sz = zero_heap_ref.size();
    ASSERT_GE(H5R__encode_heap(f_src, zero_heap_ref.data(), &sz,
                               zero_payload.data(), zero_payload.size()),
              0);
    EXPECT_LT(H5O_copy_expand_ref(f_src, dt_reg1, zero_heap_ref.data(),
                                  zero_heap_ref.size(), f_dst, dst_buf.data(),
                                  &cpy_info),
              0);

    // Heap record with bogus nonzero address triggers lines 255-257.
    std::array<uint8_t, 16> bogus_payload{};
    bogus_payload[0] = 0xEE;
    bogus_payload[1] = 0xEE;
    bogus_payload[2] = 0xEE;
    bogus_payload[3] = 0x01;
    std::array<uint8_t, H5R_DSET_REG_REF_BUF_SIZE> bogus_heap_ref{};
    sz = bogus_heap_ref.size();
    ASSERT_GE(H5R__encode_heap(f_src, bogus_heap_ref.data(), &sz,
                               bogus_payload.data(), bogus_payload.size()),
              0);
    EXPECT_LT(H5O_copy_expand_ref(f_src, dt_reg1, bogus_heap_ref.data(),
                                  bogus_heap_ref.size(), f_dst, dst_buf.data(),
                                  &cpy_info),
              0);

    // Locked address-map entry pointing into read-only destination triggers
    // H5R__encode_heap failure at lines 265-267.
    H5O_copy_t ro_cpy_info{};
    ro_cpy_info.lcpl_id = H5P_DEFAULT;
    ro_cpy_info.expand_ref = true;
    ro_cpy_info.file_dst = f_ro_dst;
    ro_cpy_info.map_list = H5SL_create(H5SL_TYPE_OBJ, nullptr);
    ASSERT_NE(ro_cpy_info.map_list, nullptr);
    absl::Cleanup free_ro_map = [&] {
      H5SL_destroy(ro_cpy_info.map_list, nullptr, nullptr);
    };
    H5O_addr_map_t fake_map{};
    H5F_GET_FILENO(f_src, fake_map.src_obj_pos.fileno);
    fake_map.src_obj_pos.addr = tgt_addr;
    fake_map.dst_addr = 0x1000;
    fake_map.is_locked = true;
    ASSERT_GE(H5SL_insert(ro_cpy_info.map_list, &fake_map,
                          &fake_map.src_obj_pos),
              0);
    EXPECT_LT(H5O_copy_expand_ref(f_src, dt_reg1, &real_reg1,
                                  sizeof(real_reg1), f_ro_dst, dst_buf.data(),
                                  &ro_cpy_info),
              0);
    ASSERT_EQ(H5SL_remove(ro_cpy_info.map_list, &fake_map.src_obj_pos),
              &fake_map);
  }

  // 4. H5O__copy_expand_ref_object2 error paths.
  {
    std::vector<uint8_t> disk_buf(H5T_get_size(dt_disk), 0);
    std::vector<uint8_t> dst_buf(H5R_REF_BUF_SIZE, 0);

    // H5T_copy(dt_src, H5T_COPY_TRANSIENT) fail (line 310).
    std::vector<uint8_t> bad_buf(H5R_REF_BUF_SIZE, 0);
    EXPECT_LT(H5O_copy_expand_ref(f_src, dt_bad_loc, bad_buf.data(),
                                  H5T_get_size(dt_bad_loc), f_dst,
                                  dst_buf.data(), &cpy_info),
              0);

    // Invalid encoded ref type byte triggers H5T_convert fail (line 335).
    disk_buf[0] = 99;
    EXPECT_LT(H5O_copy_expand_ref(f_src, dt_disk, disk_buf.data(),
                                  disk_buf.size(), f_dst, dst_buf.data(),
                                  &cpy_info),
              0);
    std::memset(disk_buf.data(), 0, disk_buf.size());

    // Unregistered H5F_t stack copy triggers H5F_get_id fail (line 339).
    {
      H5F_t dummy_f = *f_dst;
      EXPECT_LT(H5O_copy_expand_ref(f_src, dt_disk, disk_buf.data(),
                                    disk_buf.size(), &dummy_f, dst_buf.data(),
                                    &cpy_info),
                0);
    }

    // Encoded H5R_OBJECT2 with token_size == 0 triggers H5R__get_obj_token
    // failure (line 352). H5T__ref_mem_write already attached an app_ref on src
    // in conv_buf before early error frees conv_buf without reclaim; balance it.
    disk_buf[0] = static_cast<uint8_t>(H5R_OBJECT2);
    disk_buf[1] = 0;
    disk_buf[2] = 0;
    EXPECT_LT(H5O_copy_expand_ref(f_src, dt_disk, disk_buf.data(),
                                  disk_buf.size(), f_dst, dst_buf.data(),
                                  &cpy_info),
              0);
    EXPECT_GE(H5I_dec_app_ref(src.get()), 0);

    // Encoded H5R_OBJECT2 with bogus nonzero addr fails copy_obj_by_ref
    // (lines 359, 466); balance the app_ref attached in conv_buf as above.
    disk_buf[0] = static_cast<uint8_t>(H5R_OBJECT2);
    disk_buf[1] = 0;
    disk_buf[2] = static_cast<uint8_t>(H5F_SIZEOF_ADDR(f_src));
    disk_buf[3] = 0xEE;
    disk_buf[4] = 0xEE;
    EXPECT_LT(H5O_copy_expand_ref(f_src, dt_disk, disk_buf.data(),
                                  disk_buf.size(), f_dst, dst_buf.data(),
                                  &cpy_info),
              0);
    EXPECT_GE(H5I_dec_app_ref(src.get()), 0);

    // tpath_mem_dst conversion failure on null ref (line 381).
    std::vector<uint8_t> zeros2(H5T_get_size(dt_disk2), 0);
    std::vector<uint8_t> dst_buf2(H5T_get_size(dt_disk2), 0);
    EXPECT_LT(H5O_copy_expand_ref(f_src, dt_disk2, zeros2.data(), zeros2.size(),
                                  f_dst, dst_buf2.data(), &cpy_info),
              0);

    // Invalid ref.loc_id triggers H5R__set_loc_id failure (line 368).
    {
      H5R_ref_priv_t bad_loc_ref{};
      bad_loc_ref.type = static_cast<int8_t>(H5R_OBJECT2);
      bad_loc_ref.loc_id = 0x777777;
      bad_loc_ref.app_ref = false;
      bad_loc_ref.token_size = static_cast<uint8_t>(real_tok_size);
      std::memcpy(&bad_loc_ref.info.obj.token, &real_tok, sizeof(H5O_token_t));
      std::array<uint8_t, sizeof(H5R_ref_priv_t)> src4_buf{};
      std::memcpy(src4_buf.data(), &bad_loc_ref, sizeof(bad_loc_ref));
      EXPECT_LT(H5O_copy_expand_ref(f_src, dt_disk4, src4_buf.data(),
                                    src4_buf.size(), f_dst, dst_buf.data(),
                                    &cpy_info),
                0);
    }

    // H5R_BADTYPE in reclaim_buf triggers H5T_reclaim failure (line 386).
    {
      H5R_ref_priv_t bad_type_ref{};
      bad_type_ref.type = static_cast<int8_t>(H5R_BADTYPE);
      bad_type_ref.loc_id = H5I_INVALID_HID;
      bad_type_ref.app_ref = false;
      bad_type_ref.token_size = static_cast<uint8_t>(real_tok_size);
      std::memcpy(&bad_type_ref.info.obj.token, &real_tok, sizeof(H5O_token_t));
      std::array<uint8_t, sizeof(H5R_ref_priv_t)> src3_buf{};
      std::memcpy(src3_buf.data(), &bad_type_ref, sizeof(bad_type_ref));
      EXPECT_LT(H5O_copy_expand_ref(f_src, dt_disk3, src3_buf.data(),
                                    src3_buf.size(), f_dst, dst_buf.data(),
                                    &cpy_info),
                0);
      // H5R__set_loc_id incremented dst's ID refcount right before reclaim
      // failed cleanly on H5R_BADTYPE; balance that internal ref here.
      EXPECT_GE(H5I_dec_ref(dst.get()), 0);
    }

    // H5T_close(dt_mem) < 0 and H5T_close(dt_dst) < 0 in done (lines 392, 394).
    {
      RefErrHook hook{};
      hook.attach_immutable_parents = true;
      hook.parent_mem =
          H5T_copy(GetH5Type(H5T_NATIVE_INT), H5T_COPY_TRANSIENT);
      hook.parent_dst =
          H5T_copy(GetH5Type(H5T_NATIVE_INT), H5T_COPY_TRANSIENT);
      ASSERT_NE(hook.parent_mem, nullptr);
      ASSERT_NE(hook.parent_dst, nullptr);
      g_ref_err_hook = &hook;
      absl::Cleanup cleanup_hook = [&] {
        g_ref_err_hook = nullptr;
        if (hook.parent_mem != nullptr) {
          hook.parent_mem->shared->state = H5T_STATE_TRANSIENT;
          H5T_close_real(hook.parent_mem);
        }
        if (hook.parent_dst != nullptr) {
          hook.parent_dst->shared->state = H5T_STATE_TRANSIENT;
          H5T_close_real(hook.parent_dst);
        }
        if (hook.dt_mem != nullptr) {
          H5T_close_real(hook.dt_mem);
        }
        if (hook.dt_dst != nullptr) {
          H5T_close_real(hook.dt_dst);
        }
      };
      EXPECT_LT(H5O_copy_expand_ref(f_src, dt_disk2, zeros2.data(),
                                    zeros2.size(), f_dst, dst_buf2.data(),
                                    &cpy_info),
                0);
    }

    // H5I_dec_ref(dst_loc_id) < 0 in done (line 400).
    {
      RefErrHook hook{};
      hook.close_file_id = tmp_dst_400.release();
      g_ref_err_hook = &hook;
      absl::Cleanup cleanup_hook = [&] {
        g_ref_err_hook = nullptr;
        if (hook.close_file_id != H5I_INVALID_HID) {
          H5Fclose(hook.close_file_id);
        }
      };
      std::vector<uint8_t> zeros5(H5T_get_size(dt_disk5), 0);
      std::vector<uint8_t> dst_buf5(H5T_get_size(dt_disk5), 0);
      EXPECT_LT(H5O_copy_expand_ref(f_src, dt_disk5, zeros5.data(),
                                    zeros5.size(), f_tmp_dst, dst_buf5.data(),
                                    &cpy_info),
                0);
    }

    // tpath_src_mem == NULL (line 322) and tpath_mem_dst == NULL (line 324)
    // when generic non-opaque reference no-op soft paths are absent.
    {
      ASSERT_GE(H5Tunregister(H5T_PERS_SOFT, "objref", H5I_INVALID_HID,
                              H5I_INVALID_HID, nullptr),
                0);
      ASSERT_GE(H5Tunregister(H5T_PERS_SOFT, "regref", H5I_INVALID_HID,
                              H5I_INVALID_HID, nullptr),
                0);
      absl::Cleanup restore_ref_soft = [&] {
        H5Tregister(H5T_PERS_SOFT, "objref", H5T_STD_REF_OBJ, H5T_STD_REF_OBJ,
                    RestoreRefNoopCb);
        H5Tregister(H5T_PERS_SOFT, "regref", H5T_STD_REF_DSETREG,
                    H5T_STD_REF_DSETREG, RestoreRefNoopCb);
      };
      std::vector<uint8_t> zeros6(H5T_get_size(dt_322), 0);
      EXPECT_LT(H5O_copy_expand_ref(f_src, dt_322, zeros6.data(), zeros6.size(),
                                    f_dst, dst_buf.data(), &cpy_info),
                0);
      ASSERT_GE(H5Tregister(H5T_PERS_SOFT, "only_mem_ref", H5T_STD_REF_OBJ,
                            H5T_STD_REF_OBJ, OnlyMemRefCb),
                0);
      absl::Cleanup unreg_mem_ref = [&] {
        H5Tunregister(H5T_PERS_SOFT, "only_mem_ref", H5I_INVALID_HID,
                      H5I_INVALID_HID, OnlyMemRefCb);
      };
      std::vector<uint8_t> zeros7(H5T_get_size(dt_324), 0);
      EXPECT_LT(H5O_copy_expand_ref(f_src, dt_324, zeros7.data(), zeros7.size(),
                                    f_dst, dst_buf.data(), &cpy_info),
                0);
    }
  }
}

}  // namespace
}  // namespace hdf5
