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

#include "third_party/netcdf/google/libhdf5/hdf5open_shim.h"

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/netcdf/config.h"
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
#include "third_party/netcdf/include/nc.h"
#include "third_party/netcdf/include/ncglobal.h"
#include "third_party/netcdf/include/ncpathmgr.h"
#include "third_party/netcdf/include/nc4internal.h"
#include "third_party/netcdf/include/hdf5internal.h"

extern herr_t H5VL__reparse_def_vol_conn_variable_test(void);
extern herr_t H5Z_register(const H5Z_class2_t *cls);
extern hid_t nc4_H5Fopen(const char *filename0, unsigned flags, hid_t fapl_id);

typedef struct ShimNamedCmp {
  int32_t a;
  double b;
  int32_t arr[2][3];
  const char *str;
} ShimNamedCmp;

typedef struct ShimAnonCmp {
  float x;
  int64_t y;
} ShimAnonCmp;

static int WriteIntAttr(hid_t loc_id, const char *name, const int *vals,
                        hsize_t len, int scalar) {
  hid_t sid = -1;
  hid_t aid = -1;
  int rc = -1;

  if (scalar) {
    sid = H5Screate(H5S_SCALAR);
  } else if (len == 0) {
    sid = H5Screate(H5S_NULL);
  } else {
    sid = H5Screate_simple(1, &len, NULL);
  }
  if (sid < 0) goto cleanup;

  aid = H5Acreate2(loc_id, name, H5T_NATIVE_INT, sid, H5P_DEFAULT, H5P_DEFAULT);
  if (aid < 0) goto cleanup;
  if (scalar || len > 0) {
    if (H5Awrite(aid, H5T_NATIVE_INT, vals) < 0) goto cleanup;
  }
  rc = 0;

cleanup:
  if (aid >= 0) H5Aclose(aid);
  if (sid >= 0) H5Sclose(sid);
  return rc;
}

static int WriteFixedStrAttr(hid_t loc_id, const char *name, size_t str_len,
                             int is_array, hsize_t array_len, const void *buf) {
  hid_t tid = -1;
  hid_t sid = -1;
  hid_t aid = -1;
  int rc = -1;

  tid = H5Tcopy(H5T_C_S1);
  if (tid < 0) goto cleanup;
  if (H5Tset_size(tid, str_len) < 0) goto cleanup;
  if (H5Tset_strpad(tid, H5T_STR_NULLTERM) < 0) goto cleanup;

  sid = is_array ? H5Screate_simple(1, &array_len, NULL) : H5Screate(H5S_SCALAR);
  if (sid < 0) goto cleanup;

  aid = H5Acreate2(loc_id, name, tid, sid, H5P_DEFAULT, H5P_DEFAULT);
  if (aid < 0) goto cleanup;
  if (H5Awrite(aid, tid, buf) < 0) goto cleanup;
  rc = 0;

cleanup:
  if (aid >= 0) H5Aclose(aid);
  if (sid >= 0) H5Sclose(sid);
  if (tid >= 0) H5Tclose(tid);
  return rc;
}

static int WriteVlenStrAttr(hid_t loc_id, const char *name, hsize_t len,
                            const char *const *vals) {
  hid_t tid = -1;
  hid_t sid = -1;
  hid_t aid = -1;
  int rc = -1;

  tid = H5Tcopy(H5T_C_S1);
  if (tid < 0) goto cleanup;
  if (H5Tset_size(tid, H5T_VARIABLE) < 0) goto cleanup;
  sid = H5Screate_simple(1, &len, NULL);
  if (sid < 0) goto cleanup;
  aid = H5Acreate2(loc_id, name, tid, sid, H5P_DEFAULT, H5P_DEFAULT);
  if (aid < 0) goto cleanup;
  if (H5Awrite(aid, tid, vals) < 0) goto cleanup;
  rc = 0;

cleanup:
  if (aid >= 0) H5Aclose(aid);
  if (sid >= 0) H5Sclose(sid);
  if (tid >= 0) H5Tclose(tid);
  return rc;
}

static int WriteRefAttr(hid_t loc_id, hid_t ref_target_root, const char *name) {
  hid_t sid = -1;
  hid_t aid = -1;
  hobj_ref_t ref = 0;
  int rc = -1;

  if (H5Rcreate(&ref, ref_target_root, "/", H5R_OBJECT, -1) < 0) goto cleanup;
  sid = H5Screate(H5S_SCALAR);
  if (sid < 0) goto cleanup;
  aid = H5Acreate2(loc_id, name, H5T_STD_REF_OBJ, sid, H5P_DEFAULT,
                   H5P_DEFAULT);
  if (aid < 0) goto cleanup;
  if (H5Awrite(aid, H5T_STD_REF_OBJ, &ref) < 0) goto cleanup;
  rc = 0;

cleanup:
  if (aid >= 0) H5Aclose(aid);
  if (sid >= 0) H5Sclose(sid);
  return rc;
}

static hid_t BuildNamedCmpType(void) {
  hid_t str_t = -1;
  hid_t arr_t = -1;
  hid_t cmp_t = -1;
  const hsize_t arr_dims[2] = {2, 3};

  str_t = H5Tcopy(H5T_C_S1);
  if (str_t < 0) goto fail;
  if (H5Tset_size(str_t, H5T_VARIABLE) < 0) goto fail;

  arr_t = H5Tarray_create2(H5T_NATIVE_INT32, 2, arr_dims);
  if (arr_t < 0) goto fail;

  cmp_t = H5Tcreate(H5T_COMPOUND, sizeof(ShimNamedCmp));
  if (cmp_t < 0) goto fail;
  if (H5Tinsert(cmp_t, "a", offsetof(ShimNamedCmp, a), H5T_NATIVE_INT32) < 0)
    goto fail;
  if (H5Tinsert(cmp_t, "b", offsetof(ShimNamedCmp, b), H5T_NATIVE_DOUBLE) < 0)
    goto fail;
  if (H5Tinsert(cmp_t, "arr", offsetof(ShimNamedCmp, arr), arr_t) < 0)
    goto fail;
  if (H5Tinsert(cmp_t, "str", offsetof(ShimNamedCmp, str), str_t) < 0)
    goto fail;

  H5Tclose(arr_t);
  H5Tclose(str_t);
  return cmp_t;

fail:
  if (cmp_t >= 0) H5Tclose(cmp_t);
  if (arr_t >= 0) H5Tclose(arr_t);
  if (str_t >= 0) H5Tclose(str_t);
  return -1;
}

int H5OpenShimCreateRawHdf5File(const char *path, int use_crt_order,
                                int include_classic_attr) {
  hid_t fcpl = H5P_DEFAULT;
  hid_t gcpl = H5P_DEFAULT;
  hid_t fid = -1;
  hid_t subgid = -1;
  hid_t named_enum = -1;
  hid_t named_vlen = -1;
  hid_t named_opaque = -1;
  hid_t named_cmp = -1;
  hid_t named_str = -1;
  hid_t coord_x_did = -1;
  hid_t unlim_no_var_did = -1;
  hid_t pure_dim_did = -1;
  hid_t non_coord_did = -1;
  hid_t unlim_data_did = -1;
  hid_t coord_hint_did = -1;
  hid_t be_deflate_did = -1;
  hid_t szip_did = -1;
  hid_t compact_did = -1;
  hid_t virtual_did = -1;
  hid_t contig_did = -1;
  hid_t square_did = -1;
  hid_t unlim_phony_did = -1;
  hid_t unlim_phony2_did = -1;
  hid_t fixed5_did = -1;
  hid_t fixed_str_did = -1;
  hid_t fixed_char_did = -1;
  hid_t anon_enum_did = -1;
  hid_t anon_cmp_did = -1;
  hid_t anon_vlen_did = -1;
  hid_t anon_opaque_did = -1;
  hid_t ignored_ref_did = -1;
  hid_t ignored_bit_did = -1;
  hid_t sub_dim_did = -1;
  hid_t sub_cmp_did = -1;
  hid_t sid = -1;
  hid_t dcpl = -1;
  hid_t tid = -1;
  hid_t aid = -1;
  hid_t src_sid = -1;
  int rc = -1;

  if (!path) return -1;

  if (use_crt_order) {
    fcpl = H5Pcreate(H5P_FILE_CREATE);
    if (fcpl < 0) goto cleanup;
    if (H5Pset_link_creation_order(
            fcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
      goto cleanup;
    }
    if (H5Pset_attr_creation_order(
            fcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
      goto cleanup;
    }

    gcpl = H5Pcreate(H5P_GROUP_CREATE);
    if (gcpl < 0) goto cleanup;
    if (H5Pset_link_creation_order(
            gcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
      goto cleanup;
    }
    if (H5Pset_attr_creation_order(
            gcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
      goto cleanup;
    }
  }

  fid = H5Fcreate(path, H5F_ACC_TRUNC, fcpl, H5P_DEFAULT);
  if (fid < 0) goto cleanup;

  /* Committed user-defined types on root group. */
  named_enum = H5Tenum_create(H5T_STD_I32LE);
  if (named_enum < 0) goto cleanup;
  {
    const int32_t v0 = 0;
    const int32_t v1 = 1;
    const int32_t v2 = 2;
    if (H5Tenum_insert(named_enum, "RED", &v0) < 0 ||
        H5Tenum_insert(named_enum, "GREEN", &v1) < 0 ||
        H5Tenum_insert(named_enum, "BLUE", &v2) < 0) {
      goto cleanup;
    }
  }
  if (H5Tcommit2(fid, "named_enum", named_enum, H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT) < 0) {
    goto cleanup;
  }

  named_vlen = H5Tvlen_create(H5T_STD_I32LE);
  if (named_vlen < 0) goto cleanup;
  if (H5Tcommit2(fid, "named_vlen", named_vlen, H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT) < 0) {
    goto cleanup;
  }

  named_opaque = H5Tcreate(H5T_OPAQUE, 4);
  if (named_opaque < 0) goto cleanup;
  if (H5Tset_tag(named_opaque, "shim_tag") < 0) goto cleanup;
  if (H5Tcommit2(fid, "named_opaque", named_opaque, H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT) < 0) {
    goto cleanup;
  }

  named_cmp = BuildNamedCmpType();
  if (named_cmp < 0) goto cleanup;
  if (H5Tcommit2(fid, "named_cmp", named_cmp, H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT) < 0) {
    goto cleanup;
  }

  named_str = H5Tcopy(H5T_C_S1);
  if (named_str < 0) goto cleanup;
  if (H5Tset_size(named_str, H5T_VARIABLE) < 0) goto cleanup;
  if (H5Tcommit2(fid, "named_str", named_str, H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT) < 0) {
    goto cleanup;
  }

  /* Global attributes exercising atomic, string, null, user-type, and ref. */
  {
    const int scalar_val = 123;
    if (WriteIntAttr(fid, "global_scalar_i32", &scalar_val, 1, 1) < 0) {
      goto cleanup;
    }
    if (WriteIntAttr(fid, "global_zero_len", NULL, 0, 0) < 0) goto cleanup;
    if (WriteFixedStrAttr(fid, "global_fixed_char", 5, 0, 0, "hello") < 0) {
      goto cleanup;
    }
    const char fixed_str_arr[2][6] = {"alpha", "beta"};
    if (WriteFixedStrAttr(fid, "global_fixed_str_arr", 6, 1, 2,
                          fixed_str_arr) < 0) {
      goto cleanup;
    }
    const char *const vlen_strs[2] = {"one", "two"};
    if (WriteVlenStrAttr(fid, "global_vlen_str", 2, vlen_strs) < 0) {
      goto cleanup;
    }
    if (WriteRefAttr(fid, fid, "global_ref_ignored") < 0) goto cleanup;

    sid = H5Screate(H5S_SCALAR);
    if (sid < 0) goto cleanup;
    aid = H5Acreate2(fid, "global_enum_att", named_enum, sid, H5P_DEFAULT,
                     H5P_DEFAULT);
    if (aid < 0) goto cleanup;
    const int32_t blue_val = 2;
    if (H5Awrite(aid, named_enum, &blue_val) < 0) goto cleanup;
    H5Aclose(aid);
    aid = -1;
    H5Sclose(sid);
    sid = -1;

    if (include_classic_attr) {
      const int strict_flag = 1;
      if (WriteIntAttr(fid, NC3_STRICT_ATT_NAME, &strict_flag, 1, 1) < 0) {
        goto cleanup;
      }
    }
  }

  /* 1) coord_x: coordinate dimscale with explicit _Netcdf4Dimid = 5. */
  {
    const hsize_t dim3 = 3;
    const int32_t coords[3] = {10, 20, 30};
    const int dimid_val = 5;
    sid = H5Screate_simple(1, &dim3, NULL);
    if (sid < 0) goto cleanup;
    coord_x_did = H5Dcreate2(fid, "coord_x", H5T_NATIVE_INT32, sid, H5P_DEFAULT,
                             H5P_DEFAULT, H5P_DEFAULT);
    if (coord_x_did < 0) goto cleanup;
    if (H5Dwrite(coord_x_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 coords) < 0) {
      goto cleanup;
    }
    if (H5DSset_scale(coord_x_did, NULL) < 0) goto cleanup;
    if (WriteIntAttr(coord_x_did, NC_DIMID_ATT_NAME, &dimid_val, 1, 1) < 0) {
      goto cleanup;
    }
    H5Sclose(sid);
    sid = -1;
  }

  /* 2) unlim_no_var: unlimited chunked DIM_WITHOUT_VARIABLE scale. */
  {
    const hsize_t cur3 = 3;
    const hsize_t max_u = H5S_UNLIMITED;
    const hsize_t chunk3 = 3;
    const int32_t init3[3] = {0, 0, 0};
    sid = H5Screate_simple(1, &cur3, &max_u);
    if (sid < 0) goto cleanup;
    dcpl = H5Pcreate(H5P_DATASET_CREATE);
    if (dcpl < 0) goto cleanup;
    if (H5Pset_chunk(dcpl, 1, &chunk3) < 0) goto cleanup;
    unlim_no_var_did = H5Dcreate2(fid, "unlim_no_var", H5T_NATIVE_INT32, sid,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT);
    if (unlim_no_var_did < 0) goto cleanup;
    if (H5Dwrite(unlim_no_var_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                 H5P_DEFAULT, init3) < 0) {
      goto cleanup;
    }
    if (H5DSset_scale(unlim_no_var_did, DIM_WITHOUT_VARIABLE) < 0) {
      goto cleanup;
    }
    H5Pclose(dcpl);
    dcpl = -1;
    H5Sclose(sid);
    sid = -1;
  }

  /* 3) pure_dim: fixed DIM_WITHOUT_VARIABLE scale + _nc4_non_coord_pure_dim. */
  {
    const hsize_t dim2 = 2;
    const hsize_t dim3 = 3;
    const int32_t init2[2] = {0, 0};
    const int32_t non_coord_vals[3] = {7, 8, 9};
    sid = H5Screate_simple(1, &dim2, NULL);
    if (sid < 0) goto cleanup;
    pure_dim_did = H5Dcreate2(fid, "pure_dim", H5T_NATIVE_INT32, sid,
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (pure_dim_did < 0) goto cleanup;
    if (H5Dwrite(pure_dim_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 init2) < 0) {
      goto cleanup;
    }
    if (H5DSset_scale(pure_dim_did, DIM_WITHOUT_VARIABLE) < 0) goto cleanup;
    H5Sclose(sid);
    sid = -1;

    sid = H5Screate_simple(1, &dim3, NULL);
    if (sid < 0) goto cleanup;
    non_coord_did = H5Dcreate2(fid, NON_COORD_PREPEND "pure_dim",
                               H5T_NATIVE_INT32, sid, H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT);
    if (non_coord_did < 0) goto cleanup;
    if (H5Dwrite(non_coord_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                 H5P_DEFAULT, non_coord_vals) < 0) {
      goto cleanup;
    }
    if (H5DSattach_scale(non_coord_did, coord_x_did, 0) < 0) goto cleanup;
    H5Sclose(sid);
    sid = -1;
  }

  /* 4) unlim_data attached to unlim_no_var so nc4_find_dim_len computes 3. */
  {
    const hsize_t cur3 = 3;
    const hsize_t max_u = H5S_UNLIMITED;
    const hsize_t chunk3 = 3;
    const int32_t vals[3] = {100, 200, 300};
    sid = H5Screate_simple(1, &cur3, &max_u);
    if (sid < 0) goto cleanup;
    dcpl = H5Pcreate(H5P_DATASET_CREATE);
    if (dcpl < 0) goto cleanup;
    if (H5Pset_chunk(dcpl, 1, &chunk3) < 0) goto cleanup;
    unlim_data_did = H5Dcreate2(fid, "unlim_data", H5T_NATIVE_INT32, sid,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT);
    if (unlim_data_did < 0) goto cleanup;
    if (H5Dwrite(unlim_data_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                 H5P_DEFAULT, vals) < 0) {
      goto cleanup;
    }
    if (H5DSattach_scale(unlim_data_did, unlim_no_var_did, 0) < 0) {
      goto cleanup;
    }
    H5Pclose(dcpl);
    dcpl = -1;
    H5Sclose(sid);
    sid = -1;
  }

  /* 5) Non-dimscale dataset with COORDINATES attr AND attached dimscale. */
  {
    const hsize_t dim3 = 3;
    const int32_t vals[3] = {1, 2, 3};
    const int coord_dimid = 5;
    sid = H5Screate_simple(1, &dim3, NULL);
    if (sid < 0) goto cleanup;
    coord_hint_did = H5Dcreate2(fid, "coord_hint_var", H5T_NATIVE_INT32, sid,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (coord_hint_did < 0) goto cleanup;
    if (H5Dwrite(coord_hint_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                 H5P_DEFAULT, vals) < 0) {
      goto cleanup;
    }
    if (H5DSattach_scale(coord_hint_did, coord_x_did, 0) < 0) goto cleanup;
    if (WriteIntAttr(coord_hint_did, COORDINATES, &coord_dimid, 1, 0) < 0) {
      goto cleanup;
    }
    H5Sclose(sid);
    sid = -1;
  }

  /* 6) Big-endian chunked deflated var with fill value and variable attrs. */
  {
    const hsize_t dim3 = 3;
    const int32_t fill_v = -99;
    const int32_t vals[3] = {11, 22, 33};
    sid = H5Screate_simple(1, &dim3, NULL);
    if (sid < 0) goto cleanup;
    dcpl = H5Pcreate(H5P_DATASET_CREATE);
    if (dcpl < 0) goto cleanup;
    if (H5Pset_chunk(dcpl, 1, &dim3) < 0) goto cleanup;
    if (H5Pset_deflate(dcpl, 3) < 0) goto cleanup;
    if (H5Pset_fill_value(dcpl, H5T_NATIVE_INT32, &fill_v) < 0) goto cleanup;
    be_deflate_did = H5Dcreate2(fid, "be_deflate_var", H5T_STD_I32BE, sid,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT);
    if (be_deflate_did < 0) goto cleanup;
    if (H5Dwrite(be_deflate_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                 H5P_DEFAULT, vals) < 0) {
      goto cleanup;
    }
    if (H5DSattach_scale(be_deflate_did, coord_x_did, 0) < 0) goto cleanup;
    {
      const int v_attr = 42;
      const char v_fixed[2][5] = {"vx", "vy"};
      if (WriteIntAttr(be_deflate_did, "v_int", &v_attr, 1, 1) < 0) {
        goto cleanup;
      }
      if (WriteFixedStrAttr(be_deflate_did, "v_fixed_str_arr", 5, 1, 2,
                            v_fixed) < 0) {
        goto cleanup;
      }
      if (WriteRefAttr(be_deflate_did, fid, "v_ref_ignored") < 0) goto cleanup;
    }
    H5Pclose(dcpl);
    dcpl = -1;
    H5Sclose(sid);
    sid = -1;
  }

  /* 7) SZIP compressed chunked dataset (no dimscale -> phony dim 16). */
  {
    const hsize_t dim16 = 16;
    int32_t vals[16];
    for (int i = 0; i < 16; ++i) vals[i] = i;
    sid = H5Screate_simple(1, &dim16, NULL);
    if (sid < 0) goto cleanup;
    dcpl = H5Pcreate(H5P_DATASET_CREATE);
    if (dcpl < 0) goto cleanup;
    if (H5Pset_chunk(dcpl, 1, &dim16) < 0) goto cleanup;
    if (H5Pset_szip(dcpl, H5_SZIP_NN_OPTION_MASK, 8) < 0) goto cleanup;
    szip_did = H5Dcreate2(fid, "szip_var", H5T_STD_I32LE, sid, H5P_DEFAULT,
                          dcpl, H5P_DEFAULT);
    if (szip_did < 0) goto cleanup;
    if (H5Dwrite(szip_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 vals) < 0) {
      goto cleanup;
    }
    H5Pclose(dcpl);
    dcpl = -1;
    H5Sclose(sid);
    sid = -1;
  }

  /* 8) Compact scalar, virtual scalar, contiguous 1D, square 4x4, phony dims. */
  {
    const int32_t compact_val = 77;
    sid = H5Screate(H5S_SCALAR);
    if (sid < 0) goto cleanup;
    dcpl = H5Pcreate(H5P_DATASET_CREATE);
    if (dcpl < 0) goto cleanup;
    if (H5Pset_layout(dcpl, H5D_COMPACT) < 0) goto cleanup;
    compact_did = H5Dcreate2(fid, "compact_i32", H5T_STD_I32LE, sid,
                             H5P_DEFAULT, dcpl, H5P_DEFAULT);
    if (compact_did < 0) goto cleanup;
    if (H5Dwrite(compact_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 &compact_val) < 0) {
      goto cleanup;
    }
    H5Pclose(dcpl);
    dcpl = -1;

    src_sid = H5Screate(H5S_SCALAR);
    if (src_sid < 0) goto cleanup;
    dcpl = H5Pcreate(H5P_DATASET_CREATE);
    if (dcpl < 0) goto cleanup;
    if (H5Pset_virtual(dcpl, sid, ".", "/compact_i32", src_sid) < 0) {
      goto cleanup;
    }
    virtual_did = H5Dcreate2(fid, "virtual_i32", H5T_STD_I32LE, sid,
                             H5P_DEFAULT, dcpl, H5P_DEFAULT);
    if (virtual_did < 0) goto cleanup;
    H5Pclose(dcpl);
    dcpl = -1;
    H5Sclose(src_sid);
    src_sid = -1;
    H5Sclose(sid);
    sid = -1;

    const hsize_t dim4 = 4;
    const int32_t contig_vals[4] = {1, 2, 3, 4};
    sid = H5Screate_simple(1, &dim4, NULL);
    if (sid < 0) goto cleanup;
    dcpl = H5Pcreate(H5P_DATASET_CREATE);
    if (dcpl < 0) goto cleanup;
    if (H5Pset_layout(dcpl, H5D_CONTIGUOUS) < 0) goto cleanup;
    contig_did = H5Dcreate2(fid, "contig_i32", H5T_STD_I32LE, sid, H5P_DEFAULT,
                            dcpl, H5P_DEFAULT);
    if (contig_did < 0) goto cleanup;
    if (H5Dwrite(contig_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 contig_vals) < 0) {
      goto cleanup;
    }
    H5Pclose(dcpl);
    dcpl = -1;
    H5Sclose(sid);
    sid = -1;

    const hsize_t sq_dims[2] = {4, 4};
    int32_t sq_vals[4][4];
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) sq_vals[i][j] = i * 10 + j;
    }
    sid = H5Screate_simple(2, sq_dims, NULL);
    if (sid < 0) goto cleanup;
    square_did = H5Dcreate2(fid, "square_no_scale", H5T_STD_I32LE, sid,
                            H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (square_did < 0) goto cleanup;
    if (H5Dwrite(square_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 sq_vals) < 0) {
      goto cleanup;
    }
    H5Sclose(sid);
    sid = -1;

    const hsize_t cur5 = 5;
    const hsize_t max_u = H5S_UNLIMITED;
    const int32_t vals5[5] = {5, 6, 7, 8, 9};
    sid = H5Screate_simple(1, &cur5, &max_u);
    if (sid < 0) goto cleanup;
    dcpl = H5Pcreate(H5P_DATASET_CREATE);
    if (dcpl < 0) goto cleanup;
    if (H5Pset_chunk(dcpl, 1, &cur5) < 0) goto cleanup;
    unlim_phony_did = H5Dcreate2(fid, "unlim_phony_var", H5T_STD_I32LE, sid,
                                 H5P_DEFAULT, dcpl, H5P_DEFAULT);
    if (unlim_phony_did < 0) goto cleanup;
    if (H5Dwrite(unlim_phony_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                 H5P_DEFAULT, vals5) < 0) {
      goto cleanup;
    }
    unlim_phony2_did = H5Dcreate2(fid, "unlim_phony_var2", H5T_STD_I32LE, sid,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT);
    if (unlim_phony2_did < 0) goto cleanup;
    if (H5Dwrite(unlim_phony2_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                 H5P_DEFAULT, vals5) < 0) {
      goto cleanup;
    }
    H5Pclose(dcpl);
    dcpl = -1;
    H5Sclose(sid);
    sid = -1;

    sid = H5Screate_simple(1, &cur5, NULL);
    if (sid < 0) goto cleanup;
    fixed5_did = H5Dcreate2(fid, "fixed_len5_var", H5T_STD_I32LE, sid,
                            H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (fixed5_did < 0) goto cleanup;
    if (H5Dwrite(fixed5_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 vals5) < 0) {
      goto cleanup;
    }
    H5Sclose(sid);
    sid = -1;
  }

  /* 9) Fixed-length string dataset (>1 byte -> NC_STRING) and 1-byte NC_CHAR. */
  {
    const hsize_t dim2 = 2;
    const char fstrs[2][8] = {"alpha", "beta"};
    tid = H5Tcopy(H5T_C_S1);
    if (tid < 0) goto cleanup;
    if (H5Tset_size(tid, 8) < 0) goto cleanup;
    if (H5Tset_strpad(tid, H5T_STR_NULLTERM) < 0) goto cleanup;
    sid = H5Screate_simple(1, &dim2, NULL);
    if (sid < 0) goto cleanup;
    fixed_str_did = H5Dcreate2(fid, "fixed_str_var", tid, sid, H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT);
    if (fixed_str_did < 0) goto cleanup;
    if (H5Dwrite(fixed_str_did, tid, H5S_ALL, H5S_ALL, H5P_DEFAULT, fstrs) <
        0) {
      goto cleanup;
    }
    H5Sclose(sid);
    sid = -1;
    H5Tclose(tid);
    tid = -1;

    const hsize_t dim4 = 4;
    const char chars[4] = {'a', 'b', 'c', 'd'};
    tid = H5Tcopy(H5T_C_S1);
    if (tid < 0) goto cleanup;
    if (H5Tset_size(tid, 1) < 0) goto cleanup;
    sid = H5Screate_simple(1, &dim4, NULL);
    if (sid < 0) goto cleanup;
    fixed_char_did = H5Dcreate2(fid, "fixed_char_var", tid, sid, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT);
    if (fixed_char_did < 0) goto cleanup;
    if (H5Dwrite(fixed_char_did, tid, H5S_ALL, H5S_ALL, H5P_DEFAULT, chars) <
        0) {
      goto cleanup;
    }
    H5Sclose(sid);
    sid = -1;
    H5Tclose(tid);
    tid = -1;
  }

  /* 10) Transient/anonymous types (structurally distinct from committed ones). */
  {
    sid = H5Screate(H5S_SCALAR);
    if (sid < 0) goto cleanup;

    tid = H5Tenum_create(H5T_STD_U16LE);
    if (tid < 0) goto cleanup;
    {
      const uint16_t e0 = 10;
      const uint16_t e1 = 20;
      if (H5Tenum_insert(tid, "E0", &e0) < 0 ||
          H5Tenum_insert(tid, "E1", &e1) < 0) {
        goto cleanup;
      }
      anon_enum_did = H5Dcreate2(fid, "anon_enum_var", tid, sid, H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT);
      if (anon_enum_did < 0) goto cleanup;
      if (H5Dwrite(anon_enum_did, tid, H5S_ALL, H5S_ALL, H5P_DEFAULT, &e1) <
          0) {
        goto cleanup;
      }
    }
    H5Tclose(tid);
    tid = -1;

    tid = H5Tcreate(H5T_COMPOUND, sizeof(ShimAnonCmp));
    if (tid < 0) goto cleanup;
    if (H5Tinsert(tid, "x", offsetof(ShimAnonCmp, x), H5T_NATIVE_FLOAT) < 0 ||
        H5Tinsert(tid, "y", offsetof(ShimAnonCmp, y), H5T_NATIVE_INT64) < 0) {
      goto cleanup;
    }
    anon_cmp_did = H5Dcreate2(fid, "anon_cmp_var", tid, sid, H5P_DEFAULT,
                              H5P_DEFAULT, H5P_DEFAULT);
    if (anon_cmp_did < 0) goto cleanup;
    {
      const ShimAnonCmp cmp_val = {1.5f, 999};
      if (H5Dwrite(anon_cmp_did, tid, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                   &cmp_val) < 0) {
        goto cleanup;
      }
    }
    H5Tclose(tid);
    tid = -1;

    tid = H5Tvlen_create(H5T_IEEE_F64LE);
    if (tid < 0) goto cleanup;
    anon_vlen_did = H5Dcreate2(fid, "anon_vlen_var", tid, sid, H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT);
    if (anon_vlen_did < 0) goto cleanup;
    {
      double vdata[2] = {2.5, 4.5};
      hvl_t vlen_val = {2, vdata};
      if (H5Dwrite(anon_vlen_did, tid, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                   &vlen_val) < 0) {
        goto cleanup;
      }
    }
    H5Tclose(tid);
    tid = -1;

    tid = H5Tcreate(H5T_OPAQUE, 8);
    if (tid < 0) goto cleanup;
    if (H5Tset_tag(tid, "anon_tag") < 0) goto cleanup;
    anon_opaque_did = H5Dcreate2(fid, "anon_opaque_var", tid, sid, H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT);
    if (anon_opaque_did < 0) goto cleanup;
    {
      const uint8_t opq[8] = {1, 2, 3, 4, 5, 6, 7, 8};
      if (H5Dwrite(anon_opaque_did, tid, H5S_ALL, H5S_ALL, H5P_DEFAULT, opq) <
          0) {
        goto cleanup;
      }
    }
    H5Tclose(tid);
    tid = -1;

    /* Datasets skipped transparently due to unsupported HDF5 datatypes. */
    {
      hobj_ref_t obj_ref = 0;
      if (H5Rcreate(&obj_ref, fid, "/", H5R_OBJECT, -1) < 0) goto cleanup;
      ignored_ref_did =
          H5Dcreate2(fid, "ignored_ref_var", H5T_STD_REF_OBJ, sid, H5P_DEFAULT,
                     H5P_DEFAULT, H5P_DEFAULT);
      if (ignored_ref_did < 0) goto cleanup;
      if (H5Dwrite(ignored_ref_did, H5T_STD_REF_OBJ, H5S_ALL, H5S_ALL,
                   H5P_DEFAULT, &obj_ref) < 0) {
        goto cleanup;
      }
    }

    {
      const uint8_t bits = 0xA5;
      ignored_bit_did =
          H5Dcreate2(fid, "ignored_bit_var", H5T_STD_B8LE, sid, H5P_DEFAULT,
                     H5P_DEFAULT, H5P_DEFAULT);
      if (ignored_bit_did < 0) goto cleanup;
      if (H5Dwrite(ignored_bit_did, H5T_NATIVE_B8, H5S_ALL, H5S_ALL,
                   H5P_DEFAULT, &bits) < 0) {
        goto cleanup;
      }
    }

    H5Sclose(sid);
    sid = -1;
  }

  /* 11) Subgroup reusing parent dimscale and parent committed compound type. */
  subgid = H5Gcreate2(fid, "subgrp", H5P_DEFAULT, gcpl, H5P_DEFAULT);
  if (subgid < 0) goto cleanup;
  {
    const hsize_t dim3 = 3;
    const int32_t sub_vals[3] = {101, 102, 103};
    sid = H5Screate_simple(1, &dim3, NULL);
    if (sid < 0) goto cleanup;
    sub_dim_did = H5Dcreate2(subgid, "sub_parent_dim_var", H5T_NATIVE_INT32,
                             sid, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (sub_dim_did < 0) goto cleanup;
    if (H5Dwrite(sub_dim_did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 sub_vals) < 0) {
      goto cleanup;
    }
    if (H5DSattach_scale(sub_dim_did, coord_x_did, 0) < 0) goto cleanup;
    H5Sclose(sid);
    sid = -1;

    sid = H5Screate(H5S_SCALAR);
    if (sid < 0) goto cleanup;
    sub_cmp_did = H5Dcreate2(subgid, "sub_parent_type_var", named_cmp, sid,
                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (sub_cmp_did < 0) goto cleanup;
    {
      const ShimNamedCmp val = {7, 2.25, {{1, 2, 3}, {4, 5, 6}}, "sub_hi"};
      if (H5Dwrite(sub_cmp_did, named_cmp, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                   &val) < 0) {
        goto cleanup;
      }
    }
    H5Sclose(sid);
    sid = -1;
  }

  rc = 0;

cleanup:
  if (sub_cmp_did >= 0) H5Dclose(sub_cmp_did);
  if (sub_dim_did >= 0) H5Dclose(sub_dim_did);
  if (subgid >= 0) H5Gclose(subgid);
  if (ignored_bit_did >= 0) H5Dclose(ignored_bit_did);
  if (ignored_ref_did >= 0) H5Dclose(ignored_ref_did);
  if (anon_opaque_did >= 0) H5Dclose(anon_opaque_did);
  if (anon_vlen_did >= 0) H5Dclose(anon_vlen_did);
  if (anon_cmp_did >= 0) H5Dclose(anon_cmp_did);
  if (anon_enum_did >= 0) H5Dclose(anon_enum_did);
  if (fixed_char_did >= 0) H5Dclose(fixed_char_did);
  if (fixed_str_did >= 0) H5Dclose(fixed_str_did);
  if (fixed5_did >= 0) H5Dclose(fixed5_did);
  if (unlim_phony2_did >= 0) H5Dclose(unlim_phony2_did);
  if (unlim_phony_did >= 0) H5Dclose(unlim_phony_did);
  if (square_did >= 0) H5Dclose(square_did);
  if (contig_did >= 0) H5Dclose(contig_did);
  if (virtual_did >= 0) H5Dclose(virtual_did);
  if (compact_did >= 0) H5Dclose(compact_did);
  if (szip_did >= 0) H5Dclose(szip_did);
  if (be_deflate_did >= 0) H5Dclose(be_deflate_did);
  if (coord_hint_did >= 0) H5Dclose(coord_hint_did);
  if (unlim_data_did >= 0) H5Dclose(unlim_data_did);
  if (non_coord_did >= 0) H5Dclose(non_coord_did);
  if (pure_dim_did >= 0) H5Dclose(pure_dim_did);
  if (unlim_no_var_did >= 0) H5Dclose(unlim_no_var_did);
  if (coord_x_did >= 0) H5Dclose(coord_x_did);
  if (aid >= 0) H5Aclose(aid);
  if (src_sid >= 0) H5Sclose(src_sid);
  if (sid >= 0) H5Sclose(sid);
  if (dcpl != H5P_DEFAULT && dcpl >= 0) H5Pclose(dcpl);
  if (tid >= 0) H5Tclose(tid);
  if (named_str >= 0) H5Tclose(named_str);
  if (named_cmp >= 0) H5Tclose(named_cmp);
  if (named_opaque >= 0) H5Tclose(named_opaque);
  if (named_vlen >= 0) H5Tclose(named_vlen);
  if (named_enum >= 0) H5Tclose(named_enum);
  if (fid >= 0) H5Fclose(fid);
  if (gcpl != H5P_DEFAULT && gcpl >= 0) H5Pclose(gcpl);
  if (fcpl != H5P_DEFAULT && fcpl >= 0) H5Pclose(fcpl);
  return rc;
}

static int CreateFilteredDataset(hid_t fid, const char *name, hid_t sid,
                                 H5Z_filter_t filter_id, unsigned flags,
                                 size_t cd_nelems, const unsigned *cd_values) {
  hid_t dcpl = -1;
  hid_t did = -1;
  const hsize_t chunk4 = 4;
  int rc = -1;

  dcpl = H5Pcreate(H5P_DATASET_CREATE);
  if (dcpl < 0) goto cleanup;
  if (H5Pset_chunk(dcpl, 1, &chunk4) < 0) goto cleanup;
  if (H5Pset_filter(dcpl, filter_id, flags, cd_nelems, cd_values) < 0) {
    goto cleanup;
  }
  did = H5Dcreate2(fid, name, H5T_STD_I32LE, sid, H5P_DEFAULT, dcpl,
                   H5P_DEFAULT);
  if (did < 0) goto cleanup;
  rc = 0;

cleanup:
  if (did >= 0) H5Dclose(did);
  if (dcpl >= 0) H5Pclose(dcpl);
  return rc;
}

int H5OpenShimCreateBadFilterFile(const char *path) {
  hid_t fcpl = -1;
  hid_t fid = -1;
  hid_t sid = -1;
  hid_t vlen_str_t = -1;
  hid_t dcpl = -1;
  hid_t did = -1;
  const hsize_t dim4 = 4;
  const unsigned p2[2] = {7, 9};
  const unsigned deflate_bad2[2] = {4, 5};
  const unsigned deflate_bad_lvl[1] = {99};
  int rc = -1;

  if (!path) return -1;

  fcpl = H5Pcreate(H5P_FILE_CREATE);
  if (fcpl < 0) goto cleanup;
  if (H5Pset_link_creation_order(
          fcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
    goto cleanup;
  }
  if (H5Pset_attr_creation_order(
          fcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
    goto cleanup;
  }

  fid = H5Fcreate(path, H5F_ACC_TRUNC, fcpl, H5P_DEFAULT);
  if (fid < 0) goto cleanup;
  sid = H5Screate_simple(1, &dim4, NULL);
  if (sid < 0) goto cleanup;

  if (CreateFilteredDataset(fid, "unreg_noparams", sid, 400, H5Z_FLAG_OPTIONAL,
                            0, NULL) < 0) {
    goto cleanup;
  }
  if (CreateFilteredDataset(fid, "unreg_withparams", sid, 401,
                            H5Z_FLAG_OPTIONAL, 2, p2) < 0) {
    goto cleanup;
  }
  if (CreateFilteredDataset(fid, "bad_deflate_nelems", sid, H5Z_FILTER_DEFLATE,
                            H5Z_FLAG_OPTIONAL, 2, deflate_bad2) < 0) {
    goto cleanup;
  }
  if (CreateFilteredDataset(fid, "bad_deflate_level", sid, H5Z_FILTER_DEFLATE,
                            H5Z_FLAG_OPTIONAL, 1, deflate_bad_lvl) < 0) {
    goto cleanup;
  }

  vlen_str_t = H5Tcopy(H5T_C_S1);
  if (vlen_str_t < 0) goto cleanup;
  if (H5Tset_size(vlen_str_t, H5T_VARIABLE) < 0) goto cleanup;
  dcpl = H5Pcreate(H5P_DATASET_CREATE);
  if (dcpl < 0) goto cleanup;
  if (H5Pset_chunk(dcpl, 1, &dim4) < 0) goto cleanup;
  if (H5Pset_fletcher32(dcpl) < 0) goto cleanup;
  did = H5Dcreate2(fid, "mandatory_vlen_str", vlen_str_t, sid, H5P_DEFAULT, dcpl,
                   H5P_DEFAULT);
  if (did < 0) goto cleanup;

  rc = 0;

cleanup:
  if (did >= 0) H5Dclose(did);
  if (dcpl >= 0) H5Pclose(dcpl);
  if (vlen_str_t >= 0) H5Tclose(vlen_str_t);
  if (sid >= 0) H5Sclose(sid);
  if (fid >= 0) H5Fclose(fid);
  if (fcpl >= 0) H5Pclose(fcpl);
  return rc;
}

int H5OpenShimCreateNegativeFile(const char *path, int variant) {
  hid_t fcpl = -1;
  hid_t fid = -1;
  hid_t sid = -1;
  hid_t did = -1;
  hid_t aid = -1;
  int rc = -1;

  if (!path) return -1;

  fcpl = H5Pcreate(H5P_FILE_CREATE);
  if (fcpl < 0) goto cleanup;
  if (H5Pset_link_creation_order(
          fcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
    goto cleanup;
  }
  if (H5Pset_attr_creation_order(
          fcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
    goto cleanup;
  }

  fid = H5Fcreate(path, H5F_ACC_TRUNC, fcpl, H5P_DEFAULT);
  if (fid < 0) goto cleanup;

  switch (variant) {
    case kH5OpenShimNegH5sNullVar:
      sid = H5Screate(H5S_NULL);
      if (sid < 0) goto cleanup;
      did = H5Dcreate2(fid, "null_space_var", H5T_STD_I32LE, sid, H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT);
      if (did < 0) goto cleanup;
      break;

    case kH5OpenShimNegBadCoordsLen: {
      const hsize_t dim3 = 3;
      const int bad_coords[2] = {0, 1};
      const int32_t vals[3] = {1, 2, 3};
      sid = H5Screate_simple(1, &dim3, NULL);
      if (sid < 0) goto cleanup;
      did = H5Dcreate2(fid, "bad_coords_var", H5T_NATIVE_INT32, sid,
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      if (did < 0) goto cleanup;
      if (H5Dwrite(did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals) <
          0) {
        goto cleanup;
      }
      if (WriteIntAttr(did, COORDINATES, bad_coords, 2, 0) < 0) goto cleanup;
      break;
    }

    case kH5OpenShimNeg2dAttr: {
      const hsize_t dims2x2[2] = {2, 2};
      const int vals[2][2] = {{1, 2}, {3, 4}};
      sid = H5Screate_simple(2, dims2x2, NULL);
      if (sid < 0) goto cleanup;
      aid = H5Acreate2(fid, "matrix_attr", H5T_NATIVE_INT, sid, H5P_DEFAULT,
                       H5P_DEFAULT);
      if (aid < 0) goto cleanup;
      if (H5Awrite(aid, H5T_NATIVE_INT, vals) < 0) goto cleanup;
      break;
    }

    case kH5OpenShimNegCommittedBitfield: {
      hid_t bt = H5Tcopy(H5T_STD_B8LE);
      if (bt < 0) goto cleanup;
      if (H5Tcommit2(fid, "bad_bitfield_type", bt, H5P_DEFAULT, H5P_DEFAULT,
                     H5P_DEFAULT) < 0) {
        H5Tclose(bt);
        goto cleanup;
      }
      H5Tclose(bt);
      break;
    }

    case kH5OpenShimNegDanglingLink:
      if (H5Lcreate_soft("/no_such_target", fid, "dangling_link", H5P_DEFAULT,
                         H5P_DEFAULT) < 0) {
        goto cleanup;
      }
      break;

    default:
      goto cleanup;
  }

  rc = 0;

cleanup:
  if (aid >= 0) H5Aclose(aid);
  if (did >= 0) H5Dclose(did);
  if (sid >= 0) H5Sclose(sid);
  if (fid >= 0) H5Fclose(fid);
  if (fcpl >= 0) H5Pclose(fcpl);
  return rc;
}

int H5OpenShimProbeDirectOpen(int kind, const char *path, int mode,
                              void *memio_void) {
  NC *nc = NULL;
  NC_memio empty_mem = {0, NULL, 0};
  int retval;

  if (!path) return NC_EINVAL;
  if ((retval = nc_initialize()) != NC_NOERR) return retval;

  switch (kind) {
    case kH5OpenShimDirectBadNcid:
      return NC4_open(path, mode, 0, NULL, NULL, HDF5_dispatch_table, -123);

    case kH5OpenShimDirectIllegalMmap:
      return NC4_open(path, mode | NC_MMAP, 0, NULL, NULL, HDF5_dispatch_table,
                      -1);

    case kH5OpenShimDirectDisklessInmem:
      return NC4_open(path, mode | NC_DISKLESS | NC_INMEMORY, 0, NULL, NULL,
                      HDF5_dispatch_table, -1);

    case kH5OpenShimDirectInmemNullParams:
    case kH5OpenShimDirectInmemEmptyMem: {
      void *params = (kind == kH5OpenShimDirectInmemEmptyMem) ? &empty_mem : NULL;
      if ((retval = new_NC(HDF5_dispatch_table, path, mode | NC_INMEMORY, &nc))) {
        return retval;
      }
      add_to_NCList(nc);
      retval = NC4_open(nc->path, nc->mode, 0, NULL, params,
                        HDF5_dispatch_table, nc->ext_ncid);
      del_from_NCList(nc);
      free_NC(nc);
      return retval;
    }

    case kH5OpenShimDirectColdInit: {
      nc4_hdf5_finalize();
      if ((retval =
               new_NC(HDF5_dispatch_table, path, mode | NC_INMEMORY, &nc))) {
        return retval;
      }
      add_to_NCList(nc);
      retval = NC4_open(nc->path, nc->mode, 0, NULL, memio_void,
                        HDF5_dispatch_table, nc->ext_ncid);
      if (retval == NC_NOERR) {
        nc_close(nc->ext_ncid);
      } else {
        del_from_NCList(nc);
        free_NC(nc);
      }
      return retval;
    }

    default:
      return NC_EINVAL;
  }
}

int H5OpenShimProbeGetVarMetaTwice(int ncid, int varid) {
  NC_FILE_INFO_T *h5 = NULL;
  NC_GRP_INFO_T *grp = NULL;
  NC_VAR_INFO_T *var = NULL;
  int retval;

  if ((retval = nc4_hdf5_find_grp_h5_var(ncid, varid, &h5, &grp, &var))) {
    return retval;
  }
  return nc4_get_var_meta(var);
}

static hid_t CreateH5FileWithOrder(const char *path, int track_links,
                                   int track_attrs) {
  hid_t fcpl = -1;
  hid_t fid = -1;
  unsigned crt = H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED;

  fcpl = H5Pcreate(H5P_FILE_CREATE);
  if (fcpl < 0) return -1;
  if (track_links && H5Pset_link_creation_order(fcpl, crt) < 0) goto cleanup;
  if (track_attrs && H5Pset_attr_creation_order(fcpl, crt) < 0) goto cleanup;
  fid = H5Fcreate(path, H5F_ACC_TRUNC, fcpl, H5P_DEFAULT);

cleanup:
  if (fcpl >= 0) H5Pclose(fcpl);
  return fid;
}

static hid_t CreateSimpleH5Dataset(hid_t loc_id, const char *name, hid_t dtype,
                                   int ndims, const hsize_t *dims,
                                   const hsize_t *maxdims, hid_t dcpl_id) {
  hid_t sid = -1;
  hid_t did = -1;

  sid = (ndims == 0) ? H5Screate(H5S_SCALAR)
                     : H5Screate_simple(ndims, dims, maxdims);
  if (sid < 0) return -1;
  did = H5Dcreate2(loc_id, name, dtype, sid, H5P_DEFAULT, dcpl_id, H5P_DEFAULT);
  H5Sclose(sid);
  return did;
}

static int WriteVlenZeroRefDimList(hid_t did) {
  hid_t vlen_t = -1;
  hid_t sid = -1;
  hid_t aid = -1;
  hobj_ref_t zero_ref;
  hvl_t vl;
  hsize_t one = 1;
  int rc = -1;

  memset(&zero_ref, 0, sizeof(zero_ref));
  vl.len = 1;
  vl.p = &zero_ref;
  vlen_t = H5Tvlen_create(H5T_STD_REF_OBJ);
  if (vlen_t < 0) goto cleanup;
  sid = H5Screate_simple(1, &one, NULL);
  if (sid < 0) goto cleanup;
  aid = H5Acreate2(did, "DIMENSION_LIST", vlen_t, sid, H5P_DEFAULT,
                   H5P_DEFAULT);
  if (aid < 0) goto cleanup;
  if (H5Awrite(aid, vlen_t, &vl) < 0) goto cleanup;
  rc = 0;

cleanup:
  if (aid >= 0) H5Aclose(aid);
  if (sid >= 0) H5Sclose(sid);
  if (vlen_t >= 0) H5Tclose(vlen_t);
  return rc;
}

int H5OpenShimCreateExtraFixtureFile(const char *path, int kind) {
  hid_t fid = -1;
  hid_t gid = -1;
  hid_t gcpl = -1;
  hid_t tid = -1;
  hid_t sub_t = -1;
  hid_t dcpl = -1;
  hid_t did = -1;
  hid_t scale_did = -1;
  hid_t sid = -1;
  hid_t aid = -1;
  const hsize_t dim2 = 2;
  const hsize_t dims2x2[2] = {2, 2};
  char long_name[NC_MAX_NAME + 16];
  int rc = -1;

  if (!path) return -1;
  memset(long_name, 'm', NC_MAX_NAME + 8);
  long_name[NC_MAX_NAME + 8] = '\0';

  fid = CreateH5FileWithOrder(path, 1, 1);
  if (fid < 0) goto cleanup;

  switch (kind) {
    case kH5OpenShimExtraCommittedVarStr:
      tid = H5Tcopy(H5T_C_S1);
      if (tid < 0 || H5Tset_size(tid, H5T_VARIABLE) < 0) goto cleanup;
      if (H5Tcommit2(fid, "c_vstr", tid, H5P_DEFAULT, H5P_DEFAULT,
                     H5P_DEFAULT) < 0) {
        goto cleanup;
      }
      did = CreateSimpleH5Dataset(fid, "sv", tid, 1, &dim2, NULL, H5P_DEFAULT);
      if (did < 0) goto cleanup;
      break;

    case kH5OpenShimExtraLdblGlobalAttr: {
      unsigned char ld_buf[sizeof(long double)];
      memset(ld_buf, 0, sizeof(ld_buf));
      sid = H5Screate(H5S_SCALAR);
      if (sid < 0) goto cleanup;
      aid = H5Acreate2(fid, "ld_global", H5T_NATIVE_LDOUBLE, sid, H5P_DEFAULT,
                       H5P_DEFAULT);
      if (aid < 0 || H5Awrite(aid, H5T_NATIVE_LDOUBLE, ld_buf) < 0) {
        goto cleanup;
      }
      did = CreateSimpleH5Dataset(fid, "v", H5T_NATIVE_INT, 1, &dim2, NULL,
                                  H5P_DEFAULT);
      if (did < 0) goto cleanup;
      break;
    }

    case kH5OpenShimExtraChunkedVarStrDeflate: {
      const unsigned deflate_lvl = 1;
      tid = H5Tcopy(H5T_C_S1);
      if (tid < 0 || H5Tset_size(tid, H5T_VARIABLE) < 0) goto cleanup;
      dcpl = H5Pcreate(H5P_DATASET_CREATE);
      if (dcpl < 0 || H5Pset_chunk(dcpl, 1, &dim2) < 0) goto cleanup;
      if (H5Pset_filter(dcpl, H5Z_FILTER_DEFLATE, H5Z_FLAG_MANDATORY, 1,
                        &deflate_lvl) < 0) {
        goto cleanup;
      }
      did = CreateSimpleH5Dataset(fid, "vstr_def", tid, 1, &dim2, NULL, dcpl);
      if (did < 0) goto cleanup;
      break;
    }

    case kH5OpenShimExtraUntrackedSubgroup:
      gcpl = H5Pcreate(H5P_GROUP_CREATE);
      if (gcpl < 0) goto cleanup;
      gid = H5Gcreate2(fid, "sub", H5P_DEFAULT, gcpl, H5P_DEFAULT);
      if (gid < 0) goto cleanup;
      break;

    case kH5OpenShimExtraVaxFloatInSubgroup:
      gcpl = H5Pcreate(H5P_GROUP_CREATE);
      if (gcpl < 0) goto cleanup;
      if (H5Pset_link_creation_order(
              gcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0 ||
          H5Pset_attr_creation_order(
              gcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
        goto cleanup;
      }
      gid = H5Gcreate2(fid, "sub", H5P_DEFAULT, gcpl, H5P_DEFAULT);
      if (gid < 0) goto cleanup;
      did = CreateSimpleH5Dataset(gid, "vax", H5T_VAX_F32, 1, &dim2, NULL,
                                  H5P_DEFAULT);
      if (did < 0) goto cleanup;
      break;

    case kH5OpenShimExtra2dScaleNoCoords:
      did = CreateSimpleH5Dataset(fid, "s2d", H5T_NATIVE_INT, 2, dims2x2, NULL,
                                  H5P_DEFAULT);
      if (did < 0 || H5DSset_scale(did, NULL) < 0) goto cleanup;
      break;

    case kH5OpenShimExtraBadDimListZeroRef:
    case kH5OpenShimExtraBadDimListWithCoords: {
      const int coord0 = 0;
      scale_did = CreateSimpleH5Dataset(fid, "d", H5T_NATIVE_INT, 1, &dim2,
                                        NULL, H5P_DEFAULT);
      if (scale_did < 0 || H5DSset_scale(scale_did, "d") < 0) goto cleanup;
      did = CreateSimpleH5Dataset(fid, "v", H5T_NATIVE_INT, 1, &dim2, NULL,
                                  H5P_DEFAULT);
      if (did < 0) goto cleanup;
      if (kind == kH5OpenShimExtraBadDimListWithCoords &&
          WriteIntAttr(did, COORDINATES, &coord0, 1, 0) < 0) {
        goto cleanup;
      }
      if (WriteVlenZeroRefDimList(did) < 0) goto cleanup;
      break;
    }

    case kH5OpenShimExtraBadQuantizeStrAttr: {
      const char bad_nsd[4] = "bad";
      did = CreateSimpleH5Dataset(fid, "q", H5T_NATIVE_FLOAT, 1, &dim2, NULL,
                                  H5P_DEFAULT);
      if (did < 0) goto cleanup;
      if (WriteFixedStrAttr(did, NC_QUANTIZE_BITGROOM_ATT_NAME,
                            sizeof(bad_nsd), 0, 1, bad_nsd) < 0) {
        goto cleanup;
      }
      break;
    }

    case kH5OpenShimExtraReadTypeCmpLongField:
      tid = H5Tcreate(H5T_COMPOUND, sizeof(int));
      if (tid < 0 || H5Tinsert(tid, long_name, 0, H5T_NATIVE_INT) < 0) {
        goto cleanup;
      }
      if (H5Tcommit2(fid, "cmp_long", tid, H5P_DEFAULT, H5P_DEFAULT,
                     H5P_DEFAULT) < 0) {
        goto cleanup;
      }
      break;

    case kH5OpenShimExtraReadTypeCmpArrayLdbl:
      sub_t = H5Tarray_create2(H5T_NATIVE_LDOUBLE, 1, &dim2);
      if (sub_t < 0) goto cleanup;
      tid = H5Tcreate(H5T_COMPOUND, H5Tget_size(sub_t));
      if (tid < 0 || H5Tinsert(tid, "a", 0, sub_t) < 0) goto cleanup;
      if (H5Tcommit2(fid, "cmp_arr_ld", tid, H5P_DEFAULT, H5P_DEFAULT,
                     H5P_DEFAULT) < 0) {
        goto cleanup;
      }
      break;

    case kH5OpenShimExtraReadTypeCmpScalarLdbl:
      tid = H5Tcreate(H5T_COMPOUND, sizeof(long double));
      if (tid < 0 || H5Tinsert(tid, "ld", 0, H5T_NATIVE_LDOUBLE) < 0) {
        goto cleanup;
      }
      if (H5Tcommit2(fid, "cmp_scl_ld", tid, H5P_DEFAULT, H5P_DEFAULT,
                     H5P_DEFAULT) < 0) {
        goto cleanup;
      }
      break;

    case kH5OpenShimExtraReadTypeVlenLdbl:
      tid = H5Tvlen_create(H5T_NATIVE_LDOUBLE);
      if (tid < 0) goto cleanup;
      if (H5Tcommit2(fid, "vlen_ld", tid, H5P_DEFAULT, H5P_DEFAULT,
                     H5P_DEFAULT) < 0) {
        goto cleanup;
      }
      break;

    case kH5OpenShimExtraReadTypeEnum24Bit: {
      int32_t zero = 0;
      sub_t = H5Tcopy(H5T_STD_I32LE);
      if (sub_t < 0 || H5Tset_precision(sub_t, 24) < 0) goto cleanup;
      tid = H5Tenum_create(sub_t);
      if (tid < 0 || H5Tenum_insert(tid, "E0", &zero) < 0) goto cleanup;
      if (H5Tcommit2(fid, "enum_24", tid, H5P_DEFAULT, H5P_DEFAULT,
                     H5P_DEFAULT) < 0) {
        goto cleanup;
      }
      break;
    }

    case kH5OpenShimExtraReadTypeEnumLongMember: {
      int32_t zero = 0;
      tid = H5Tenum_create(H5T_STD_I32LE);
      if (tid < 0 || H5Tenum_insert(tid, long_name, &zero) < 0) goto cleanup;
      if (H5Tcommit2(fid, "enum_long", tid, H5P_DEFAULT, H5P_DEFAULT,
                     H5P_DEFAULT) < 0) {
        goto cleanup;
      }
      break;
    }

    default:
      goto cleanup;
  }

  rc = 0;

cleanup:
  if (aid >= 0) H5Aclose(aid);
  if (sid >= 0) H5Sclose(sid);
  if (scale_did >= 0) H5Dclose(scale_did);
  if (did >= 0) H5Dclose(did);
  if (dcpl >= 0) H5Pclose(dcpl);
  if (gid >= 0) H5Gclose(gid);
  if (gcpl >= 0) H5Pclose(gcpl);
  if (sub_t >= 0) H5Tclose(sub_t);
  if (tid >= 0) H5Tclose(tid);
  if (fid >= 0) H5Fclose(fid);
  return rc;
}

int H5OpenShimProbeInvalidPreemption(const char *path) {
  NCglobalstate *gs = NULL;
  float prev = 0.0F;
  int ncid = -1;
  int retval;

  if (!path) return NC_EINVAL;
  if ((retval = nc_initialize()) != NC_NOERR) return retval;

  gs = NC_getglobalstate();
  prev = gs->chunkcache.preemption;
  gs->chunkcache.preemption = 2.0F;
  retval = nc_open(path, NC_NOWRITE, &ncid);
  if (retval == NC_NOERR && ncid >= 0) {
    nc_close(ncid);
  }
  gs->chunkcache.preemption = prev;
  return retval;
}

static size_t StubSzipFilter(unsigned int flags, size_t cd_nelmts,
                             const unsigned int cd_values[], size_t nbytes,
                             size_t *buf_size, void **buf) {
  (void)flags;
  (void)cd_nelmts;
  (void)cd_values;
  (void)nbytes;
  (void)buf_size;
  (void)buf;
  return 0;
}

static int CreateStubSzipFile(const char *path, size_t cd_nelems,
                              const unsigned int *cd_vals) {
  hid_t fid = -1;
  hid_t dcpl = -1;
  hid_t did = -1;
  const hsize_t dim4 = 4;
  int rc = -1;

  fid = CreateH5FileWithOrder(path, 1, 1);
  if (fid < 0) goto cleanup;
  dcpl = H5Pcreate(H5P_DATASET_CREATE);
  if (dcpl < 0 || H5Pset_chunk(dcpl, 1, &dim4) < 0) goto cleanup;
  if (H5Pset_filter(dcpl, H5Z_FILTER_SZIP, H5Z_FLAG_OPTIONAL, cd_nelems,
                    cd_vals) < 0) {
    goto cleanup;
  }
  did = CreateSimpleH5Dataset(fid, "s", H5T_STD_I32LE, 1, &dim4, NULL, dcpl);
  if (did < 0) goto cleanup;
  rc = 0;

cleanup:
  if (did >= 0) H5Dclose(did);
  if (dcpl >= 0) H5Pclose(dcpl);
  if (fid >= 0) H5Fclose(fid);
  return rc;
}

int H5OpenShimRunLeakyRawSuite(const char *dir) {
  char fpath[4096];
  int n = 0;
  int ncid = -1;
  int vid = -1;
  int storage = 0;
  int mode = 0;
  int nsd = 0;
  const unsigned int szip2[2] = {4, 16};
  /* Skip kH5OpenShimExtraVaxFloatInSubgroup: upstream get_type_info2 returns
   * NC_EBADTYPE after calloc(*type_info) with rc=0/name=NULL, tripping
   * assert(type && type->rc && type->hdr.name) in var_free(). */
  const int leaky_kinds[6] = {
      kH5OpenShimExtraBadDimListZeroRef,
      kH5OpenShimExtraReadTypeCmpLongField,
      kH5OpenShimExtraReadTypeCmpArrayLdbl,
      kH5OpenShimExtraReadTypeCmpScalarLdbl,
      kH5OpenShimExtraReadTypeVlenLdbl,
      kH5OpenShimExtraReadTypeEnumLongMember,
  };
  const H5Z_class2_t stub_szip = {
      H5Z_CLASS_T_VERS, H5Z_FILTER_SZIP, 1, 1, "stub_szip",
      NULL, NULL, StubSzipFilter,
  };

  if (!dir) return -1;
  if (nc_initialize() != NC_NOERR) return -1;
  H5Eset_auto2(H5E_DEFAULT, NULL, NULL);

  for (size_t i = 0; i < sizeof(leaky_kinds) / sizeof(leaky_kinds[0]); ++i) {
    n = snprintf(fpath, sizeof(fpath), "%s/leaky_%d.h5", dir, leaky_kinds[i]);
    if (n < 0 || (size_t)n >= sizeof(fpath)) return -1;
    if (H5OpenShimCreateExtraFixtureFile(fpath, leaky_kinds[i]) != 0) return -1;
    ncid = -1;
    if (nc_open(fpath, NC_NOWRITE, &ncid) == NC_NOERR) {
      nc_close(ncid);
      remove(fpath);
      return -1;
    }
    remove(fpath);
  }

  /* Enum24Bit reaches read_type:2245 and returns NC_EBADTYPID. */
  n = snprintf(fpath, sizeof(fpath), "%s/leaky_enum24.h5", dir);
  if (n < 0 || (size_t)n >= sizeof(fpath)) return -1;
  if (H5OpenShimCreateExtraFixtureFile(
          fpath, kH5OpenShimExtraReadTypeEnum24Bit) != 0) {
    return -1;
  }
  ncid = -1;
  if (nc_open(fpath, NC_NOWRITE, &ncid) == NC_NOERR) {
    nc_close(ncid);
    remove(fpath);
    return -1;
  }
  remove(fpath);

  /* BadDimListWithCoords opens cleanly, then fails nc_inq_var_chunking at 1530. */
  n = snprintf(fpath, sizeof(fpath), "%s/leaky_coords_dimlist.h5", dir);
  if (n < 0 || (size_t)n >= sizeof(fpath)) return -1;
  if (H5OpenShimCreateExtraFixtureFile(
          fpath, kH5OpenShimExtraBadDimListWithCoords) != 0) {
    return -1;
  }
  if (nc_open(fpath, NC_NOWRITE, &ncid) != NC_NOERR) {
    remove(fpath);
    return -1;
  }
  if (nc_inq_varid(ncid, "v", &vid) != NC_NOERR ||
      nc_inq_var_chunking(ncid, vid, &storage, NULL) == NC_NOERR) {
    nc_close(ncid);
    remove(fpath);
    return -1;
  }
  nc_close(ncid);
  remove(fpath);

  /* BadQuantizeStrAttr opens cleanly, then fails nc_inq_var_quantize at 1268. */
  n = snprintf(fpath, sizeof(fpath), "%s/leaky_quant_str.h5", dir);
  if (n < 0 || (size_t)n >= sizeof(fpath)) return -1;
  if (H5OpenShimCreateExtraFixtureFile(
          fpath, kH5OpenShimExtraBadQuantizeStrAttr) != 0) {
    return -1;
  }
  if (nc_open(fpath, NC_NOWRITE, &ncid) != NC_NOERR) {
    remove(fpath);
    return -1;
  }
  if (nc_inq_varid(ncid, "q", &vid) != NC_NOERR ||
      nc_inq_var_quantize(ncid, vid, &mode, &nsd) == NC_NOERR) {
    nc_close(ncid);
    remove(fpath);
    return -1;
  }
  nc_close(ncid);
  remove(fpath);

  /* Register stub SZIP filter to exercise 1142, 1144, and 1147. */
  if (H5Z_register(&stub_szip) < 0) return -1;

  n = snprintf(fpath, sizeof(fpath), "%s/leaky_szip0.h5", dir);
  if (n < 0 || (size_t)n >= sizeof(fpath)) return -1;
  if (CreateStubSzipFile(fpath, 0, NULL) != 0) return -1;
  if (nc_open(fpath, NC_NOWRITE, &ncid) != NC_NOERR) {
    remove(fpath);
    return -1;
  }
  if (nc_inq_varid(ncid, "s", &vid) != NC_NOERR ||
      nc_inq_var_chunking(ncid, vid, &storage, NULL) != NC_NOERR) {
    nc_close(ncid);
    remove(fpath);
    return -1;
  }
  nc_close(ncid);
  remove(fpath);

  n = snprintf(fpath, sizeof(fpath), "%s/leaky_szip2.h5", dir);
  if (n < 0 || (size_t)n >= sizeof(fpath)) return -1;
  if (CreateStubSzipFile(fpath, 2, szip2) != 0) return -1;
  if (nc_open(fpath, NC_NOWRITE, &ncid) != NC_NOERR) {
    remove(fpath);
    return -1;
  }
  if (nc_inq_varid(ncid, "s", &vid) != NC_NOERR ||
      nc_inq_var_chunking(ncid, vid, &storage, NULL) == NC_NOERR) {
    nc_close(ncid);
    remove(fpath);
    return -1;
  }
  nc_close(ncid);
  remove(fpath);

  /* NCPD_WIN triggers NCpathcvt returning NULL inside nc4_H5Fopen (line 2955). */
  NCpathsetplatform(NCPD_WIN);
  {
    hid_t bad_fid = nc4_H5Fopen("/unconvertible/winpath.nc", H5F_ACC_RDONLY,
                                H5P_DEFAULT);
    NCpathsetplatform(NCPD_UNKNOWN);
    if (bad_fid != H5I_INVALID_HID) {
      H5Fclose(bad_fid);
      return -1;
    }
  }

  return 0;
}

static herr_t AlwaysFailPropCreate(const char *name, size_t size, void *value) {
  (void)name;
  (void)size;
  (void)value;
  return -1;
}

int H5OpenShimRunH5pClassFailureSuite(const char *path) {
  int dummy = 0;
  int ncid = -1;
  NCglobalstate *gs = NULL;

  if (!path) return -1;
  if (nc_initialize() != NC_NOERR) return -1;
  H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
  gs = NC_getglobalstate();

  /* Fail H5Pcreate(H5P_FILE_ACCESS) at line 785. */
  if (H5Pregister2(H5P_FILE_ACCESS, "bad_fapl_prop", sizeof(int), &dummy,
                   AlwaysFailPropCreate, NULL, NULL, NULL, NULL, NULL,
                   NULL) < 0) {
    return -1;
  }
  if (nc_open(path, NC_NOWRITE, &ncid) == NC_NOERR) {
    nc_close(ncid);
    return -1;
  }
  if (H5Punregister(H5P_FILE_ACCESS, "bad_fapl_prop") < 0) return -1;

  /* Fail H5Pset_fapl_core() at line 889 via NC_DISKLESS. */
  if (H5Punregister(H5P_FILE_ACCESS, "vfd_info") < 0) return -1;
  if (nc_open(path, NC_NOWRITE | NC_DISKLESS, &ncid) == NC_NOERR) {
    nc_close(ncid);
    return -1;
  }

  /* Fail H5Pset_meta_block_size() at lines 849-850. */
  if (nc_set_meta_block_size(4096U) != NC_NOERR) return -1;
  if (H5Punregister(H5P_FILE_ACCESS, "meta_block_size") < 0) return -1;
  if (nc_open(path, NC_NOWRITE, &ncid) == NC_NOERR) {
    nc_close(ncid);
    return -1;
  }
  gs->meta_block_size.defined = 0;

  /* Fail H5Pset_alignment() at line 841. */
  if (nc_set_alignment(1024, 4096) != NC_NOERR) return -1;
  if (H5Punregister(H5P_FILE_ACCESS, "align") < 0) return -1;
  if (nc_open(path, NC_NOWRITE, &ncid) == NC_NOERR) {
    nc_close(ncid);
    return -1;
  }
  gs->alignment.defined = 0;

  /* Fail hdf5set_format_compatibility() at line 859. */
  if (H5Punregister(H5P_FILE_ACCESS, "libver_low_bound") < 0) return -1;
  if (nc_open(path, NC_NOWRITE, &ncid) == NC_NOERR) {
    nc_close(ncid);
    return -1;
  }

  /* Fail H5Pset_fclose_degree() at line 788. */
  if (H5Punregister(H5P_FILE_ACCESS, "close_degree") < 0) return -1;
  if (nc_open(path, NC_NOWRITE, &ncid) == NC_NOERR) {
    nc_close(ncid);
    return -1;
  }

  return 0;
}

typedef struct FaultVolState {
  int fail_file_get_fcpl;
  int bad_file_get_fcpl_fapl;

  int fail_group_open_nth;
  int group_open_calls;
  int fail_group_get_gcpl;
  int bad_group_get_gcpl_fapl;
  int arm_deferred_group_close;
  void *deferred_child_grp_obj;

  int fail_obj_get_info_once;
  int fake_map_type_once;
  int fail_obj_get_by_token;
  void *token_opened_obj;

  int fail_dset_open_nth;
  int dset_open_calls;
  int fail_dset_get_dapl;
  int bad_dset_get_dapl_gcpl;
  int fail_dset_get_dcpl;
  int bad_dset_get_dcpl_gcpl;
  int bad_dset_get_dcpl_no_pline;
  int bad_dset_get_dcpl_no_fill;
  int bad_dset_get_dcpl_opaque_fill;
  int fail_dset_get_space_nth;
  int dset_get_space_calls;
  int bad_dset_get_space_type_nth;
  int fail_dset_get_type_once;
  int bad_dset_get_type_space_once;
  void *fail_close_dset_obj;

  int arm_dtype_close_on_dset_type;
  int fail_dtype_close_once;

  const char *fail_attr_open_name;
  const char *watch_attr_name;
  void *watched_attr_obj;
  int fail_watched_attr_get_type;
  int bad_watched_attr_type_space;
  int fail_watched_attr_get_space;
  int bad_watched_attr_space_type;
  int fail_watched_attr_read;

  const char *fail_attr_close_name;
  void *watched_close_attr_obj;

  const char *fail_attr_exists_name;
  int fail_attr_exists_nth;
  int attr_exists_calls;

  int fail_token_cmp;
} FaultVolState;

static FaultVolState g_fault_vol;

static void ResetFaultVolState(void) {
  memset(&g_fault_vol, 0, sizeof(g_fault_vol));
}

static void *OpenFaultVolAttrCreate(void *obj,
                                     const H5VL_loc_params_t *loc_params,
                                     const char *attr_name, hid_t type_id,
                                     hid_t space_id, hid_t acpl_id,
                                     hid_t aapl_id, hid_t dxpl_id, void **req) {
  return H5VLattr_create(obj, loc_params, H5VL_NATIVE, attr_name, type_id,
                         space_id, acpl_id, aapl_id, dxpl_id, req);
}

static void *OpenFaultVolAttrOpen(void *obj,
                                   const H5VL_loc_params_t *loc_params,
                                   const char *attr_name, hid_t aapl_id,
                                   hid_t dxpl_id, void **req) {
  void *ret = NULL;
  if (g_fault_vol.fail_attr_open_name && attr_name &&
      strcmp(attr_name, g_fault_vol.fail_attr_open_name) == 0) {
    g_fault_vol.fail_attr_open_name = NULL;
    return NULL;
  }
  ret = H5VLattr_open(obj, loc_params, H5VL_NATIVE, attr_name, aapl_id,
                      dxpl_id, req);
  if (ret && attr_name) {
    if (g_fault_vol.watch_attr_name &&
        strcmp(attr_name, g_fault_vol.watch_attr_name) == 0) {
      g_fault_vol.watched_attr_obj = ret;
    }
    if (g_fault_vol.fail_attr_close_name &&
        strcmp(attr_name, g_fault_vol.fail_attr_close_name) == 0) {
      g_fault_vol.watched_close_attr_obj = ret;
    }
  }
  return ret;
}

static herr_t OpenFaultVolAttrRead(void *attr, hid_t mem_type_id, void *buf,
                                   hid_t dxpl_id, void **req) {
  if (g_fault_vol.fail_watched_attr_read &&
      attr == g_fault_vol.watched_attr_obj) {
    g_fault_vol.fail_watched_attr_read = 0;
    return -1;
  }
  return H5VLattr_read(attr, H5VL_NATIVE, mem_type_id, buf, dxpl_id, req);
}

static herr_t OpenFaultVolAttrWrite(void *attr, hid_t mem_type_id,
                                    const void *buf, hid_t dxpl_id,
                                    void **req) {
  return H5VLattr_write(attr, H5VL_NATIVE, mem_type_id, buf, dxpl_id, req);
}

static herr_t OpenFaultVolAttrGet(void *obj, H5VL_attr_get_args_t *args,
                                  hid_t dxpl_id, void **req) {
  if (obj == g_fault_vol.watched_attr_obj) {
    if (args->op_type == H5VL_ATTR_GET_TYPE) {
      if (g_fault_vol.fail_watched_attr_get_type) {
        g_fault_vol.fail_watched_attr_get_type = 0;
        return -1;
      }
      if (g_fault_vol.bad_watched_attr_type_space) {
        g_fault_vol.bad_watched_attr_type_space = 0;
        args->args.get_type.type_id = H5Screate(H5S_SCALAR);
        return (args->args.get_type.type_id < 0) ? -1 : 0;
      }
    } else if (args->op_type == H5VL_ATTR_GET_SPACE) {
      if (g_fault_vol.fail_watched_attr_get_space) {
        g_fault_vol.fail_watched_attr_get_space = 0;
        return -1;
      }
      if (g_fault_vol.bad_watched_attr_space_type) {
        g_fault_vol.bad_watched_attr_space_type = 0;
        args->args.get_space.space_id = H5Tcopy(H5T_NATIVE_INT);
        return (args->args.get_space.space_id < 0) ? -1 : 0;
      }
    }
  }
  return H5VLattr_get(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolAttrSpecific(void *obj,
                                       const H5VL_loc_params_t *loc_params,
                                       H5VL_attr_specific_args_t *args,
                                       hid_t dxpl_id, void **req) {
  if (args->op_type == H5VL_ATTR_EXISTS && g_fault_vol.fail_attr_exists_name &&
      args->args.exists.name &&
      strcmp(args->args.exists.name, g_fault_vol.fail_attr_exists_name) == 0) {
    g_fault_vol.attr_exists_calls++;
    if (g_fault_vol.fail_attr_exists_nth == 0 ||
        g_fault_vol.attr_exists_calls == g_fault_vol.fail_attr_exists_nth) {
      g_fault_vol.fail_attr_exists_name = NULL;
      return -1;
    }
  }
  return H5VLattr_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolAttrOptional(void *obj, H5VL_optional_args_t *args,
                                       hid_t dxpl_id, void **req) {
  return H5VLattr_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolAttrClose(void *attr, hid_t dxpl_id, void **req) {
  if (g_fault_vol.fail_attr_close_name &&
      attr == g_fault_vol.watched_close_attr_obj) {
    g_fault_vol.fail_attr_close_name = NULL;
    g_fault_vol.watched_close_attr_obj = NULL;
    return -1;
  }
  return H5VLattr_close(attr, H5VL_NATIVE, dxpl_id, req);
}

static void *OpenFaultVolDatasetCreate(void *obj,
                                        const H5VL_loc_params_t *loc_params,
                                        const char *name, hid_t lcpl_id,
                                        hid_t type_id, hid_t space_id,
                                        hid_t dcpl_id, hid_t dapl_id,
                                        hid_t dxpl_id, void **req) {
  return H5VLdataset_create(obj, loc_params, H5VL_NATIVE, name, lcpl_id,
                            type_id, space_id, dcpl_id, dapl_id, dxpl_id, req);
}

static void *OpenFaultVolDatasetOpen(void *obj,
                                      const H5VL_loc_params_t *loc_params,
                                      const char *name, hid_t dapl_id,
                                      hid_t dxpl_id, void **req) {
  g_fault_vol.dset_open_calls++;
  if (g_fault_vol.fail_dset_open_nth > 0 &&
      g_fault_vol.dset_open_calls == g_fault_vol.fail_dset_open_nth) {
    g_fault_vol.fail_dset_open_nth = 0;
    return NULL;
  }
  return H5VLdataset_open(obj, loc_params, H5VL_NATIVE, name, dapl_id, dxpl_id,
                          req);
}

static herr_t OpenFaultVolDatasetRead(size_t count, void *dset[],
                                      hid_t mem_type_id[],
                                      hid_t mem_space_id[],
                                      hid_t file_space_id[], hid_t dxpl_id,
                                      void *buf[], void **req) {
  return H5VLdataset_read(count, dset, H5VL_NATIVE, mem_type_id, mem_space_id,
                          file_space_id, dxpl_id, buf, req);
}

static herr_t OpenFaultVolDatasetWrite(size_t count, void *dset[],
                                       hid_t mem_type_id[],
                                       hid_t mem_space_id[],
                                       hid_t file_space_id[], hid_t dxpl_id,
                                       const void *buf[], void **req) {
  return H5VLdataset_write(count, dset, H5VL_NATIVE, mem_type_id, mem_space_id,
                           file_space_id, dxpl_id, buf, req);
}

static herr_t OpenFaultVolDatasetGet(void *obj, H5VL_dataset_get_args_t *args,
                                     hid_t dxpl_id, void **req) {
  if (args->op_type == H5VL_DATASET_GET_DAPL) {
    if (g_fault_vol.fail_dset_get_dapl) {
      g_fault_vol.fail_dset_get_dapl = 0;
      return -1;
    }
    if (g_fault_vol.bad_dset_get_dapl_gcpl) {
      g_fault_vol.bad_dset_get_dapl_gcpl = 0;
      args->args.get_dapl.dapl_id = H5Pcreate(H5P_GROUP_CREATE);
      return (args->args.get_dapl.dapl_id < 0) ? -1 : 0;
    }
  } else if (args->op_type == H5VL_DATASET_GET_DCPL) {
    if (g_fault_vol.fail_dset_get_dcpl) {
      g_fault_vol.fail_dset_get_dcpl = 0;
      return -1;
    }
    if (g_fault_vol.bad_dset_get_dcpl_gcpl) {
      g_fault_vol.bad_dset_get_dcpl_gcpl = 0;
      args->args.get_dcpl.dcpl_id = H5Pcreate(H5P_GROUP_CREATE);
      return (args->args.get_dcpl.dcpl_id < 0) ? -1 : 0;
    }
    if (g_fault_vol.bad_dset_get_dcpl_no_pline) {
      hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
      g_fault_vol.bad_dset_get_dcpl_no_pline = 0;
      if (dcpl < 0 || H5Premove(dcpl, "pline") < 0) {
        if (dcpl >= 0) H5Pclose(dcpl);
        return -1;
      }
      args->args.get_dcpl.dcpl_id = dcpl;
      return 0;
    }
    if (g_fault_vol.bad_dset_get_dcpl_no_fill) {
      hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
      g_fault_vol.bad_dset_get_dcpl_no_fill = 0;
      if (dcpl < 0 || H5Premove(dcpl, "fill_value") < 0) {
        if (dcpl >= 0) H5Pclose(dcpl);
        return -1;
      }
      args->args.get_dcpl.dcpl_id = dcpl;
      return 0;
    }
    if (g_fault_vol.bad_dset_get_dcpl_opaque_fill) {
      hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
      hid_t opq = H5Tcreate(H5T_OPAQUE, 4);
      const unsigned char fill_bytes[4] = {1, 2, 3, 4};
      g_fault_vol.bad_dset_get_dcpl_opaque_fill = 0;
      if (dcpl < 0 || opq < 0 || H5Pset_fill_value(dcpl, opq, fill_bytes) < 0) {
        if (opq >= 0) H5Tclose(opq);
        if (dcpl >= 0) H5Pclose(dcpl);
        return -1;
      }
      H5Tclose(opq);
      args->args.get_dcpl.dcpl_id = dcpl;
      return 0;
    }
  } else if (args->op_type == H5VL_DATASET_GET_SPACE) {
    g_fault_vol.dset_get_space_calls++;
    if (g_fault_vol.fail_dset_get_space_nth > 0 &&
        g_fault_vol.dset_get_space_calls ==
            g_fault_vol.fail_dset_get_space_nth) {
      g_fault_vol.fail_dset_get_space_nth = 0;
      return -1;
    }
    if (g_fault_vol.bad_dset_get_space_type_nth > 0 &&
        g_fault_vol.dset_get_space_calls ==
            g_fault_vol.bad_dset_get_space_type_nth) {
      g_fault_vol.bad_dset_get_space_type_nth = 0;
      args->args.get_space.space_id = H5Tcopy(H5T_NATIVE_INT);
      return (args->args.get_space.space_id < 0) ? -1 : 0;
    }
  } else if (args->op_type == H5VL_DATASET_GET_TYPE) {
    if (g_fault_vol.fail_dset_get_type_once) {
      g_fault_vol.fail_dset_get_type_once = 0;
      return -1;
    }
    if (g_fault_vol.bad_dset_get_type_space_once) {
      g_fault_vol.bad_dset_get_type_space_once = 0;
      args->args.get_type.type_id = H5Screate(H5S_SCALAR);
      return (args->args.get_type.type_id < 0) ? -1 : 0;
    }
    if (g_fault_vol.arm_dtype_close_on_dset_type) {
      g_fault_vol.arm_dtype_close_on_dset_type = 0;
      g_fault_vol.fail_dtype_close_once = 1;
    }
  }
  return H5VLdataset_get(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolDatasetSpecific(void *obj,
                                          H5VL_dataset_specific_args_t *args,
                                          hid_t dxpl_id, void **req) {
  return H5VLdataset_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolDatasetOptional(void *obj,
                                          H5VL_optional_args_t *args,
                                          hid_t dxpl_id, void **req) {
  return H5VLdataset_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolDatasetClose(void *dset, hid_t dxpl_id, void **req) {
  if (g_fault_vol.fail_close_dset_obj &&
      dset == g_fault_vol.fail_close_dset_obj) {
    g_fault_vol.fail_close_dset_obj = NULL;
    return -1;
  }
  return H5VLdataset_close(dset, H5VL_NATIVE, dxpl_id, req);
}

static void *OpenFaultVolDatatypeCommit(void *obj,
                                         const H5VL_loc_params_t *loc_params,
                                         const char *name, hid_t type_id,
                                         hid_t lcpl_id, hid_t tcpl_id,
                                         hid_t tapl_id, hid_t dxpl_id,
                                         void **req) {
  return H5VLdatatype_commit(obj, loc_params, H5VL_NATIVE, name, type_id,
                             lcpl_id, tcpl_id, tapl_id, dxpl_id, req);
}

static void *OpenFaultVolDatatypeOpen(void *obj,
                                       const H5VL_loc_params_t *loc_params,
                                       const char *name, hid_t tapl_id,
                                       hid_t dxpl_id, void **req) {
  return H5VLdatatype_open(obj, loc_params, H5VL_NATIVE, name, tapl_id, dxpl_id,
                           req);
}

static herr_t OpenFaultVolDatatypeGet(void *obj, H5VL_datatype_get_args_t *args,
                                      hid_t dxpl_id, void **req) {
  return H5VLdatatype_get(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolDatatypeSpecific(void *obj,
                                           H5VL_datatype_specific_args_t *args,
                                           hid_t dxpl_id, void **req) {
  return H5VLdatatype_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolDatatypeOptional(void *obj,
                                           H5VL_optional_args_t *args,
                                           hid_t dxpl_id, void **req) {
  return H5VLdatatype_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolDatatypeClose(void *dt, hid_t dxpl_id, void **req) {
  if (g_fault_vol.fail_dtype_close_once) {
    g_fault_vol.fail_dtype_close_once = 0;
    return -1;
  }
  return H5VLdatatype_close(dt, H5VL_NATIVE, dxpl_id, req);
}

static void *OpenFaultVolFileCreate(const char *name, unsigned flags,
                                     hid_t fcpl_id, hid_t fapl_id,
                                     hid_t dxpl_id, void **req) {
  hid_t under_fapl = H5Pcopy(fapl_id);
  void *ret = NULL;
  if (under_fapl < 0) return NULL;
  if (H5Pset_vol(under_fapl, H5VL_NATIVE, NULL) >= 0) {
    ret = H5VLfile_create(name, flags, fcpl_id, under_fapl, dxpl_id, req);
  }
  H5Pclose(under_fapl);
  return ret;
}

static void *OpenFaultVolFileOpen(const char *name, unsigned flags,
                                   hid_t fapl_id, hid_t dxpl_id, void **req) {
  hid_t under_fapl = H5Pcopy(fapl_id);
  void *ret = NULL;
  if (under_fapl < 0) return NULL;
  if (H5Pset_vol(under_fapl, H5VL_NATIVE, NULL) >= 0) {
    ret = H5VLfile_open(name, flags, under_fapl, dxpl_id, req);
  }
  H5Pclose(under_fapl);
  return ret;
}

static herr_t OpenFaultVolFileGet(void *file, H5VL_file_get_args_t *args,
                                  hid_t dxpl_id, void **req) {
  if (args->op_type == H5VL_FILE_GET_FCPL) {
    if (g_fault_vol.fail_file_get_fcpl) {
      g_fault_vol.fail_file_get_fcpl = 0;
      return -1;
    }
    if (g_fault_vol.bad_file_get_fcpl_fapl) {
      g_fault_vol.bad_file_get_fcpl_fapl = 0;
      args->args.get_fcpl.fcpl_id = H5Pcreate(H5P_FILE_ACCESS);
      return (args->args.get_fcpl.fcpl_id < 0) ? -1 : 0;
    }
  }
  return H5VLfile_get(file, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolFileSpecific(void *obj,
                                       H5VL_file_specific_args_t *args,
                                       hid_t dxpl_id, void **req) {
  return H5VLfile_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolFileOptional(void *file, H5VL_optional_args_t *args,
                                       hid_t dxpl_id, void **req) {
  return H5VLfile_optional(file, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolFileClose(void *file, hid_t dxpl_id, void **req) {
  return H5VLfile_close(file, H5VL_NATIVE, dxpl_id, req);
}

static void *OpenFaultVolGroupCreate(void *obj,
                                      const H5VL_loc_params_t *loc_params,
                                      const char *name, hid_t lcpl_id,
                                      hid_t gcpl_id, hid_t gapl_id,
                                      hid_t dxpl_id, void **req) {
  return H5VLgroup_create(obj, loc_params, H5VL_NATIVE, name, lcpl_id, gcpl_id,
                          gapl_id, dxpl_id, req);
}

static void *OpenFaultVolGroupOpen(void *obj,
                                    const H5VL_loc_params_t *loc_params,
                                    const char *name, hid_t gapl_id,
                                    hid_t dxpl_id, void **req) {
  g_fault_vol.group_open_calls++;
  if (g_fault_vol.fail_group_open_nth > 0 &&
      g_fault_vol.group_open_calls == g_fault_vol.fail_group_open_nth) {
    g_fault_vol.fail_group_open_nth = 0;
    return NULL;
  }
  return H5VLgroup_open(obj, loc_params, H5VL_NATIVE, name, gapl_id, dxpl_id,
                        req);
}

static herr_t OpenFaultVolGroupGet(void *obj, H5VL_group_get_args_t *args,
                                   hid_t dxpl_id, void **req) {
  if (args->op_type == H5VL_GROUP_GET_GCPL) {
    if (g_fault_vol.fail_group_get_gcpl) {
      g_fault_vol.fail_group_get_gcpl = 0;
      return -1;
    }
    if (g_fault_vol.bad_group_get_gcpl_fapl) {
      g_fault_vol.bad_group_get_gcpl_fapl = 0;
      args->args.get_gcpl.gcpl_id = H5Pcreate(H5P_FILE_ACCESS);
      return (args->args.get_gcpl.gcpl_id < 0) ? -1 : 0;
    }
  }
  return H5VLgroup_get(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolGroupSpecific(void *obj,
                                        H5VL_group_specific_args_t *args,
                                        hid_t dxpl_id, void **req) {
  return H5VLgroup_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolGroupOptional(void *obj, H5VL_optional_args_t *args,
                                        hid_t dxpl_id, void **req) {
  return H5VLgroup_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolGroupClose(void *grp, hid_t dxpl_id, void **req) {
  if (g_fault_vol.deferred_child_grp_obj &&
      grp == g_fault_vol.deferred_child_grp_obj) {
    g_fault_vol.deferred_child_grp_obj = NULL;
    return -1;
  }
  return H5VLgroup_close(grp, H5VL_NATIVE, dxpl_id, req);
}

static herr_t OpenFaultVolLinkCreate(H5VL_link_create_args_t *args, void *obj,
                                     const H5VL_loc_params_t *loc_params,
                                     hid_t lcpl_id, hid_t lapl_id,
                                     hid_t dxpl_id, void **req) {
  return H5VLlink_create(args, obj, loc_params, H5VL_NATIVE, lcpl_id, lapl_id,
                         dxpl_id, req);
}

static herr_t OpenFaultVolLinkCopy(void *src_obj,
                                   const H5VL_loc_params_t *loc_params1,
                                   void *dst_obj,
                                   const H5VL_loc_params_t *loc_params2,
                                   hid_t lcpl_id, hid_t lapl_id, hid_t dxpl_id,
                                   void **req) {
  return H5VLlink_copy(src_obj, loc_params1, dst_obj, loc_params2, H5VL_NATIVE,
                       lcpl_id, lapl_id, dxpl_id, req);
}

static herr_t OpenFaultVolLinkMove(void *src_obj,
                                   const H5VL_loc_params_t *loc_params1,
                                   void *dst_obj,
                                   const H5VL_loc_params_t *loc_params2,
                                   hid_t lcpl_id, hid_t lapl_id, hid_t dxpl_id,
                                   void **req) {
  return H5VLlink_move(src_obj, loc_params1, dst_obj, loc_params2, H5VL_NATIVE,
                       lcpl_id, lapl_id, dxpl_id, req);
}

static herr_t OpenFaultVolLinkGet(void *obj,
                                  const H5VL_loc_params_t *loc_params,
                                  H5VL_link_get_args_t *args, hid_t dxpl_id,
                                  void **req) {
  return H5VLlink_get(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolLinkSpecific(void *obj,
                                       const H5VL_loc_params_t *loc_params,
                                       H5VL_link_specific_args_t *args,
                                       hid_t dxpl_id, void **req) {
  return H5VLlink_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolLinkOptional(void *obj,
                                       const H5VL_loc_params_t *loc_params,
                                       H5VL_optional_args_t *args,
                                       hid_t dxpl_id, void **req) {
  return H5VLlink_optional(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

static void *OpenFaultVolObjectOpen(void *obj,
                                     const H5VL_loc_params_t *loc_params,
                                     H5I_type_t *opened_type, hid_t dxpl_id,
                                     void **req) {
  void *ret = H5VLobject_open(obj, loc_params, H5VL_NATIVE, opened_type,
                              dxpl_id, req);
  if (ret && loc_params && loc_params->type == H5VL_OBJECT_BY_TOKEN) {
    g_fault_vol.token_opened_obj = ret;
  }
  if (ret && opened_type && *opened_type == H5I_GROUP &&
      g_fault_vol.arm_deferred_group_close) {
    g_fault_vol.arm_deferred_group_close = 0;
    g_fault_vol.deferred_child_grp_obj = ret;
  }
  return ret;
}

static herr_t OpenFaultVolObjectCopy(void *src_obj,
                                     const H5VL_loc_params_t *loc_params1,
                                     const char *src_name, void *dst_obj,
                                     const H5VL_loc_params_t *loc_params2,
                                     const char *dst_name, hid_t ocpypl_id,
                                     hid_t lcpl_id, hid_t dxpl_id, void **req) {
  return H5VLobject_copy(src_obj, loc_params1, src_name, dst_obj, loc_params2,
                         dst_name, H5VL_NATIVE, ocpypl_id, lcpl_id, dxpl_id,
                         req);
}

static herr_t OpenFaultVolObjectGet(void *obj,
                                    const H5VL_loc_params_t *loc_params,
                                    H5VL_object_get_args_t *args, hid_t dxpl_id,
                                    void **req) {
  herr_t ret;
  if (args->op_type == H5VL_OBJECT_GET_INFO) {
    if (g_fault_vol.fail_obj_get_by_token &&
        obj == g_fault_vol.token_opened_obj) {
      g_fault_vol.fail_obj_get_by_token = 0;
      g_fault_vol.token_opened_obj = NULL;
      return -1;
    }
    if (g_fault_vol.fail_obj_get_info_once) {
      g_fault_vol.fail_obj_get_info_once = 0;
      return -1;
    }
  }
  ret = H5VLobject_get(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
  if (ret >= 0 && args->op_type == H5VL_OBJECT_GET_INFO &&
      g_fault_vol.fake_map_type_once && args->args.get_info.oinfo &&
      args->args.get_info.oinfo->type == H5O_TYPE_DATASET) {
    g_fault_vol.fake_map_type_once = 0;
    args->args.get_info.oinfo->type = H5O_TYPE_MAP;
    g_fault_vol.fail_close_dset_obj = obj;
  }
  return ret;
}

static herr_t OpenFaultVolObjectSpecific(void *obj,
                                         const H5VL_loc_params_t *loc_params,
                                         H5VL_object_specific_args_t *args,
                                         hid_t dxpl_id, void **req) {
  return H5VLobject_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolObjectOptional(void *obj,
                                         const H5VL_loc_params_t *loc_params,
                                         H5VL_optional_args_t *args,
                                         hid_t dxpl_id, void **req) {
  return H5VLobject_optional(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

static herr_t OpenFaultVolIntrospectGetConnCls(
    void *obj, H5VL_get_conn_lvl_t lvl, const H5VL_class_t **conn_cls) {
  return H5VLintrospect_get_conn_cls(obj, H5VL_NATIVE, lvl, conn_cls);
}

static herr_t OpenFaultVolIntrospectGetCapFlags(const void *info,
                                                uint64_t *cap_flags) {
  (void)info;
  return H5VLintrospect_get_cap_flags(NULL, H5VL_NATIVE, cap_flags);
}

static herr_t OpenFaultVolIntrospectOptQuery(void *obj, H5VL_subclass_t cls,
                                             int opt_type, uint64_t *flags) {
  return H5VLintrospect_opt_query(obj, H5VL_NATIVE, cls, opt_type, flags);
}

static herr_t OpenFaultVolBlobPut(void *obj, const void *buf, size_t size,
                                  void *blob_id, void *ctx) {
  return H5VLblob_put(obj, H5VL_NATIVE, buf, size, blob_id, ctx);
}

static herr_t OpenFaultVolBlobGet(void *obj, const void *blob_id, void *buf,
                                  size_t size, void *ctx) {
  return H5VLblob_get(obj, H5VL_NATIVE, blob_id, buf, size, ctx);
}

static herr_t OpenFaultVolBlobSpecific(void *obj, void *blob_id,
                                       H5VL_blob_specific_args_t *args) {
  return H5VLblob_specific(obj, H5VL_NATIVE, blob_id, args);
}

static herr_t OpenFaultVolTokenCmp(void *obj, const H5O_token_t *token1,
                                   const H5O_token_t *token2, int *cmp_value) {
  if (g_fault_vol.fail_token_cmp) {
    g_fault_vol.fail_token_cmp = 0;
    return -1;
  }
  return H5VLtoken_cmp(obj, H5VL_NATIVE, token1, token2, cmp_value);
}

static herr_t OpenFaultVolTokenToStr(void *obj, H5I_type_t obj_type,
                                     const H5O_token_t *token,
                                     char **token_str) {
  return H5VLtoken_to_str(obj, obj_type, H5VL_NATIVE, token, token_str);
}

static herr_t OpenFaultVolTokenFromStr(void *obj, H5I_type_t obj_type,
                                       const char *token_str,
                                       H5O_token_t *token) {
  return H5VLtoken_from_str(obj, obj_type, H5VL_NATIVE, token_str, token);
}

static const H5VL_class_t kOpenFaultVolClass = {
    .version = H5VL_VERSION,
    .value = (H5VL_class_value_t)51009,
    .name = "open_fault_vol",
    .conn_version = 1,
    .cap_flags = 0,
    .initialize = NULL,
    .terminate = NULL,
    .info_cls = {0},
    .wrap_cls = {0},
    .attr_cls =
        {
            .create = OpenFaultVolAttrCreate,
            .open = OpenFaultVolAttrOpen,
            .read = OpenFaultVolAttrRead,
            .write = OpenFaultVolAttrWrite,
            .get = OpenFaultVolAttrGet,
            .specific = OpenFaultVolAttrSpecific,
            .optional = OpenFaultVolAttrOptional,
            .close = OpenFaultVolAttrClose,
        },
    .dataset_cls =
        {
            .create = OpenFaultVolDatasetCreate,
            .open = OpenFaultVolDatasetOpen,
            .read = OpenFaultVolDatasetRead,
            .write = OpenFaultVolDatasetWrite,
            .get = OpenFaultVolDatasetGet,
            .specific = OpenFaultVolDatasetSpecific,
            .optional = OpenFaultVolDatasetOptional,
            .close = OpenFaultVolDatasetClose,
        },
    .datatype_cls =
        {
            .commit = OpenFaultVolDatatypeCommit,
            .open = OpenFaultVolDatatypeOpen,
            .get = OpenFaultVolDatatypeGet,
            .specific = OpenFaultVolDatatypeSpecific,
            .optional = OpenFaultVolDatatypeOptional,
            .close = OpenFaultVolDatatypeClose,
        },
    .file_cls =
        {
            .create = OpenFaultVolFileCreate,
            .open = OpenFaultVolFileOpen,
            .get = OpenFaultVolFileGet,
            .specific = OpenFaultVolFileSpecific,
            .optional = OpenFaultVolFileOptional,
            .close = OpenFaultVolFileClose,
        },
    .group_cls =
        {
            .create = OpenFaultVolGroupCreate,
            .open = OpenFaultVolGroupOpen,
            .get = OpenFaultVolGroupGet,
            .specific = OpenFaultVolGroupSpecific,
            .optional = OpenFaultVolGroupOptional,
            .close = OpenFaultVolGroupClose,
        },
    .link_cls =
        {
            .create = OpenFaultVolLinkCreate,
            .copy = OpenFaultVolLinkCopy,
            .move = OpenFaultVolLinkMove,
            .get = OpenFaultVolLinkGet,
            .specific = OpenFaultVolLinkSpecific,
            .optional = OpenFaultVolLinkOptional,
        },
    .object_cls =
        {
            .open = OpenFaultVolObjectOpen,
            .copy = OpenFaultVolObjectCopy,
            .get = OpenFaultVolObjectGet,
            .specific = OpenFaultVolObjectSpecific,
            .optional = OpenFaultVolObjectOptional,
        },
    .introspect_cls =
        {
            .get_conn_cls = OpenFaultVolIntrospectGetConnCls,
            .get_cap_flags = OpenFaultVolIntrospectGetCapFlags,
            .opt_query = OpenFaultVolIntrospectOptQuery,
        },
    .request_cls = {0},
    .blob_cls =
        {
            .put = OpenFaultVolBlobPut,
            .get = OpenFaultVolBlobGet,
            .specific = OpenFaultVolBlobSpecific,
            .optional = NULL,
        },
    .token_cls =
        {
            .cmp = OpenFaultVolTokenCmp,
            .to_str = OpenFaultVolTokenToStr,
            .from_str = OpenFaultVolTokenFromStr,
        },
    .optional = NULL,
};

static int ExpectOpenFail(const char *path) {
  int ncid = -1;
  int rc = nc_open(path, NC_NOWRITE, &ncid);
  if (rc == NC_NOERR) {
    nc_close(ncid);
    return -1;
  }
  ResetFaultVolState();
  return 0;
}

static int ExpectVarChunkingFail(const char *path, const char *var_name,
                                 void (*arm_fault)(void)) {
  int ncid = -1;
  int vid = -1;
  int storage = 0;

  ResetFaultVolState();
  if (nc_open(path, NC_NOWRITE, &ncid) != NC_NOERR) return -1;
  if (nc_inq_varid(ncid, var_name, &vid) != NC_NOERR) {
    nc_close(ncid);
    return -1;
  }
  arm_fault();
  if (nc_inq_var_chunking(ncid, vid, &storage, NULL) == NC_NOERR) {
    ResetFaultVolState();
    nc_close(ncid);
    return -1;
  }
  ResetFaultVolState();
  nc_close(ncid);
  return 0;
}

static int ExpectNattsFailWithClose(const char *path, void (*arm_fault)(void),
                                    int close_file) {
  int ncid = -1;
  int natts = 0;
  int rc = 0;

  ResetFaultVolState();
  if (nc_open(path, NC_NOWRITE, &ncid) != NC_NOERR) return -1;
  arm_fault();
  rc = nc_inq_natts(ncid, &natts);
  ResetFaultVolState();
  if (close_file) nc_close(ncid);
  return (rc == NC_NOERR) ? -1 : 0;
}

static int ExpectNattsFail(const char *path, void (*arm_fault)(void)) {
  return ExpectNattsFailWithClose(path, arm_fault, 1);
}

static int ExpectNattsFailNoClose(const char *path, void (*arm_fault)(void)) {
  return ExpectNattsFailWithClose(path, arm_fault, 0);
}

static void ArmFailDapl(void) { g_fault_vol.fail_dset_get_dapl = 1; }
static void ArmBadDaplGcpl(void) { g_fault_vol.bad_dset_get_dapl_gcpl = 1; }
static void ArmFailDcpl(void) { g_fault_vol.fail_dset_get_dcpl = 1; }
static void ArmBadDcplGcpl(void) { g_fault_vol.bad_dset_get_dcpl_gcpl = 1; }
static void ArmBadDcplNoPline(void) {
  g_fault_vol.bad_dset_get_dcpl_no_pline = 1;
}
static void ArmBadDcplNoFill(void) {
  g_fault_vol.bad_dset_get_dcpl_no_fill = 1;
}
static void ArmBadDcplOpaqueFill(void) {
  g_fault_vol.bad_dset_get_dcpl_opaque_fill = 1;
}
static void ArmQuantizeClose(void) {
  g_fault_vol.fail_attr_close_name = NC_QUANTIZE_BITGROOM_ATT_NAME;
}

static void ArmFailAttrOpenGInt(void) {
  g_fault_vol.fail_attr_open_name = "g_int";
}
static void ArmFailAttrGetTypeGInt(void) {
  g_fault_vol.watch_attr_name = "g_int";
  g_fault_vol.fail_watched_attr_get_type = 1;
}
static void ArmBadAttrTypeSpaceGInt(void) {
  g_fault_vol.watch_attr_name = "g_int";
  g_fault_vol.bad_watched_attr_type_space = 1;
}
static void ArmFailAttrGetSpaceGInt(void) {
  g_fault_vol.watch_attr_name = "g_int";
  g_fault_vol.fail_watched_attr_get_space = 1;
}
static void ArmBadAttrSpaceTypeGInt(void) {
  g_fault_vol.watch_attr_name = "g_int";
  g_fault_vol.bad_watched_attr_space_type = 1;
}
static void ArmFailAttrReadFixStr(void) {
  g_fault_vol.watch_attr_name = "g_fixstr";
  g_fault_vol.fail_watched_attr_read = 1;
}
static void ArmFailAttrReadAndCloseGInt(void) {
  g_fault_vol.watch_attr_name = "g_int";
  g_fault_vol.fail_watched_attr_read = 1;
  g_fault_vol.fail_attr_close_name = "g_int";
}

static int CreateVolFixtures(const char *plain_path, const char *sub_path,
                             const char *scale_no_coords_path,
                             const char *multi_path, const char *quant_path,
                             const char *big_chunk_path,
                             const char *comm_enum_path) {
  /* 1. plain_v: global g_fixstr[2] + global g_int + 1D int var v */
  {
    const char fix_strs[8] = {'a', 'b', 'c', '\0', 'd', 'e', 'f', '\0'};
    const int g_val = 7;
    const hsize_t dim2 = 2;
    hid_t fid = CreateH5FileWithOrder(plain_path, 1, 1);
    hid_t did = -1;
    if (fid < 0) return -1;
    if (WriteFixedStrAttr(fid, "g_fixstr", 4, 1, 2, fix_strs) < 0 ||
        WriteIntAttr(fid, "g_int", &g_val, 1, 1) < 0) {
      H5Fclose(fid);
      return -1;
    }
    did = CreateSimpleH5Dataset(fid, "v", H5T_NATIVE_INT, 1, &dim2, NULL,
                                H5P_DEFAULT);
    if (did < 0) {
      H5Fclose(fid);
      return -1;
    }
    H5Dclose(did);
    H5Fclose(fid);
  }

  /* 2. sub_v: tracked root + subgroup sub + var x */
  {
    int ncid = -1, gid = -1, d = -1, v = -1;
    if (nc_create(sub_path, NC_NETCDF4 | NC_CLOBBER, &ncid) != NC_NOERR) {
      return -1;
    }
    if (nc_def_grp(ncid, "sub", &gid) != NC_NOERR ||
        nc_def_dim(gid, "d", 2, &d) != NC_NOERR ||
        nc_def_var(gid, "x", NC_INT, 1, &d, &v) != NC_NOERR) {
      nc_close(ncid);
      return -1;
    }
    nc_close(ncid);
  }

  /* 3. scale_no_coords: tracked root -> sub with 1D dimscale d attached to y */
  {
    const hsize_t dim2 = 2;
    const unsigned crt = H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED;
    hid_t fid = CreateH5FileWithOrder(scale_no_coords_path, 1, 1);
    hid_t gcpl = -1, gid = -1, s_did = -1, y_did = -1;
    if (fid < 0) return -1;
    gcpl = H5Pcreate(H5P_GROUP_CREATE);
    if (gcpl < 0 || H5Pset_link_creation_order(gcpl, crt) < 0 ||
        H5Pset_attr_creation_order(gcpl, crt) < 0) {
      if (gcpl >= 0) H5Pclose(gcpl);
      H5Fclose(fid);
      return -1;
    }
    gid = H5Gcreate2(fid, "sub", H5P_DEFAULT, gcpl, H5P_DEFAULT);
    H5Pclose(gcpl);
    if (gid < 0) {
      H5Fclose(fid);
      return -1;
    }
    s_did = CreateSimpleH5Dataset(gid, "d", H5T_NATIVE_INT, 1, &dim2, NULL,
                                  H5P_DEFAULT);
    y_did = CreateSimpleH5Dataset(gid, "y", H5T_NATIVE_INT, 1, &dim2, NULL,
                                  H5P_DEFAULT);
    if (s_did < 0 || y_did < 0 || H5DSset_scale(s_did, "d") < 0 ||
        H5DSattach_scale(y_did, s_did, 0) < 0) {
      if (y_did >= 0) H5Dclose(y_did);
      if (s_did >= 0) H5Dclose(s_did);
      H5Gclose(gid);
      H5Fclose(fid);
      return -1;
    }
    H5Dclose(y_did);
    H5Dclose(s_did);
    H5Gclose(gid);
    H5Fclose(fid);
  }

  /* 4. multi_coords: 2D coord var writes _Netcdf4Coordinates & _Netcdf4Dimid */
  {
    int ncid = -1, d1 = -1, d2 = -1, v = -1;
    int dims[2];
    if (nc_create(multi_path, NC_NETCDF4 | NC_CLOBBER, &ncid) != NC_NOERR) {
      return -1;
    }
    if (nc_def_dim(ncid, "d1", 2, &d1) != NC_NOERR ||
        nc_def_dim(ncid, "d2", 2, &d2) != NC_NOERR) {
      nc_close(ncid);
      return -1;
    }
    dims[0] = d1;
    dims[1] = d2;
    if (nc_def_var(ncid, "d1", NC_INT, 2, dims, &v) != NC_NOERR) {
      nc_close(ncid);
      return -1;
    }
    nc_close(ncid);
    {
      hid_t fid = H5Fopen(multi_path, H5F_ACC_RDWR, H5P_DEFAULT);
      hid_t did = -1;
      htri_t exists = -1;
      if (fid < 0) return -1;
      did = H5Dopen2(fid, "d1", H5P_DEFAULT);
      if (did < 0) {
        H5Fclose(fid);
        return -1;
      }
      exists = H5Aexists(did, NC_DIMID_ATT_NAME);
      if (exists < 0 ||
          (exists == 0 &&
           WriteIntAttr(did, NC_DIMID_ATT_NAME, &d1, 1, 1) != 0)) {
        H5Dclose(did);
        H5Fclose(fid);
        return -1;
      }
      H5Dclose(did);
      H5Fclose(fid);
    }
  }

  /* 5. quant_v: 1D float var with bitgroom quantization */
  {
    int ncid = -1, d = -1, v = -1;
    if (nc_create(quant_path, NC_NETCDF4 | NC_CLOBBER, &ncid) != NC_NOERR) {
      return -1;
    }
    if (nc_def_dim(ncid, "d", 4, &d) != NC_NOERR ||
        nc_def_var(ncid, "q", NC_FLOAT, 1, &d, &v) != NC_NOERR ||
        nc_def_var_quantize(ncid, v, NC_QUANTIZE_BITGROOM, 3) != NC_NOERR) {
      nc_close(ncid);
      return -1;
    }
    nc_close(ncid);
  }

  /* 6. big_chunk_v: chunk 20000 ints (>67KB), deflated -> triggers reopen */
  {
    int ncid = -1, d = -1, v = -1;
    size_t chunk20k = 20000;
    if (nc_create(big_chunk_path, NC_NETCDF4 | NC_CLOBBER, &ncid) != NC_NOERR) {
      return -1;
    }
    if (nc_def_dim(ncid, "d", 20000, &d) != NC_NOERR ||
        nc_def_var(ncid, "b", NC_INT, 1, &d, &v) != NC_NOERR ||
        nc_def_var_chunking(ncid, v, NC_CHUNKED, &chunk20k) != NC_NOERR ||
        nc_def_var_deflate(ncid, v, 0, 1, 1) != NC_NOERR) {
      nc_close(ncid);
      return -1;
    }
    nc_close(ncid);
  }

  /* 7. comm_enum: committed enum type + dataset using it */
  {
    const hsize_t dim2 = 2;
    int32_t zero = 0;
    hid_t fid = CreateH5FileWithOrder(comm_enum_path, 1, 1);
    hid_t et = -1, ev = -1;
    if (fid < 0) return -1;
    et = H5Tenum_create(H5T_STD_I32LE);
    if (et < 0 || H5Tenum_insert(et, "Z", &zero) < 0 ||
        H5Tcommit2(fid, "et", et, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT) < 0) {
      if (et >= 0) H5Tclose(et);
      H5Fclose(fid);
      return -1;
    }
    ev = CreateSimpleH5Dataset(fid, "ev", et, 1, &dim2, NULL, H5P_DEFAULT);
    H5Tclose(et);
    if (ev < 0) {
      H5Fclose(fid);
      return -1;
    }
    H5Dclose(ev);
    H5Fclose(fid);
  }

  return 0;
}

int H5OpenShimRunOpenFaultVolSuite(const char *dir) {
  char plain_path[4096];
  char sub_path[4096];
  char scale_nc_path[4096];
  char multi_path[4096];
  char quant_path[4096];
  char big_chunk_path[4096];
  char comm_enum_path[4096];
  hid_t vol_id = -1;
  int ncid = -1;
  int vid = -1;
  int storage = 0;
  int rc = -1;

  if (!dir) return -1;
  if (snprintf(plain_path, sizeof(plain_path), "%s/v_plain.h5", dir) < 0 ||
      snprintf(sub_path, sizeof(sub_path), "%s/v_sub.h5", dir) < 0 ||
      snprintf(scale_nc_path, sizeof(scale_nc_path), "%s/v_sc_nc.h5", dir) <
          0 ||
      snprintf(multi_path, sizeof(multi_path), "%s/v_multi.h5", dir) < 0 ||
      snprintf(quant_path, sizeof(quant_path), "%s/v_quant.h5", dir) < 0 ||
      snprintf(big_chunk_path, sizeof(big_chunk_path), "%s/v_bigc.h5", dir) <
          0 ||
      snprintf(comm_enum_path, sizeof(comm_enum_path), "%s/v_enum.h5", dir) <
          0) {
    return -1;
  }

  if (nc_initialize() != NC_NOERR) return -1;
  H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
  setenv("HDF5_USE_FILE_LOCKING", "FALSE", 1);

  if (CreateVolFixtures(plain_path, sub_path, scale_nc_path, multi_path,
                        quant_path, big_chunk_path, comm_enum_path) != 0) {
    goto cleanup;
  }

  ResetFaultVolState();
  vol_id = H5VLregister_connector(&kOpenFaultVolClass, H5P_DEFAULT);
  if (vol_id < 0) goto cleanup;
  if (setenv("HDF5_VOL_CONNECTOR", "open_fault_vol", 1) != 0) goto cleanup;
  if (H5VL__reparse_def_vol_conn_variable_test() < 0) goto cleanup;

  /* 1. get_type_info2 (155, 160, 263) */
  g_fault_vol.fail_dset_get_type_once = 1;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  g_fault_vol.bad_dset_get_type_space_once = 1;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  /* Skip arm_dtype_close_on_dset_type: upstream get_type_info2 returns
   * NC_EHDFERR on H5Tclose() before incrementing var->type_info->rc, causing
   * var_free() and nc4_rec_grp_del() to double-free the committed type. */

  /* 2. read_coord_dimids (308, 317, 320, 324, 326+348, 335+350, 352) */
  g_fault_vol.fail_attr_exists_name = COORDINATES;
  if (ExpectOpenFail(multi_path) != 0) goto cleanup;

  g_fault_vol.fail_attr_open_name = COORDINATES;
  if (ExpectOpenFail(multi_path) != 0) goto cleanup;

  g_fault_vol.watch_attr_name = COORDINATES;
  g_fault_vol.fail_watched_attr_get_type = 1;
  if (ExpectOpenFail(multi_path) != 0) goto cleanup;

  g_fault_vol.watch_attr_name = COORDINATES;
  g_fault_vol.fail_watched_attr_get_space = 1;
  if (ExpectOpenFail(multi_path) != 0) goto cleanup;

  g_fault_vol.watch_attr_name = COORDINATES;
  g_fault_vol.bad_watched_attr_space_type = 1;
  if (ExpectOpenFail(multi_path) != 0) goto cleanup;

  g_fault_vol.watch_attr_name = COORDINATES;
  g_fault_vol.bad_watched_attr_type_space = 1;
  if (ExpectOpenFail(multi_path) != 0) goto cleanup;

  /* 3. dimscale_visitor (380), create_phony_dims (428, 442+510), */
  /*    rec_match_dimscales (547, 618) */
  g_fault_vol.fail_obj_get_by_token = 1;
  if (ExpectOpenFail(scale_nc_path) != 0) goto cleanup;

  g_fault_vol.fail_token_cmp = 1;
  if (ExpectOpenFail(scale_nc_path) != 0) goto cleanup;

  g_fault_vol.fail_dset_get_space_nth = 3;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  g_fault_vol.bad_dset_get_space_type_nth = 3;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  /* 4. check_for_classic_model (678+986) & H5Fget_create_plist (967, 969) */
  g_fault_vol.fail_attr_exists_name = NC3_STRICT_ATT_NAME;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  g_fault_vol.fail_file_get_fcpl = 1;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  g_fault_vol.bad_file_get_fcpl_fapl = 1;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  /* 5. nc4_get_var_meta (1495, 1500, 1505, 1301+1509, 1099, */
  /*    1191+1517, 1209+1517, 1270+1526, 1522) */
  if (ExpectVarChunkingFail(plain_path, "v", ArmFailDapl) != 0) goto cleanup;
  if (ExpectVarChunkingFail(plain_path, "v", ArmBadDaplGcpl) != 0) goto cleanup;
  if (ExpectVarChunkingFail(plain_path, "v", ArmFailDcpl) != 0) goto cleanup;
  if (ExpectVarChunkingFail(plain_path, "v", ArmBadDcplGcpl) != 0) goto cleanup;
  if (ExpectVarChunkingFail(plain_path, "v", ArmBadDcplNoPline) != 0) {
    goto cleanup;
  }
  if (ExpectVarChunkingFail(plain_path, "v", ArmBadDcplNoFill) != 0) {
    goto cleanup;
  }
  if (ExpectVarChunkingFail(plain_path, "v", ArmBadDcplOpaqueFill) != 0) {
    goto cleanup;
  }
  if (ExpectVarChunkingFail(quant_path, "q", ArmQuantizeClose) != 0) {
    goto cleanup;
  }

  /* 1522: nc4_adjust_var_cache -> nc4_reopen_dataset failure */
  ResetFaultVolState();
  if (nc_set_chunk_cache(CHUNK_CACHE_SIZE, DEFAULT_CHUNKS_IN_CACHE, 0.75F) !=
      NC_NOERR) {
    goto cleanup;
  }
  if (nc_open(big_chunk_path, NC_NOWRITE, &ncid) != NC_NOERR) goto cleanup;
  if (nc_inq_varid(ncid, "b", &vid) != NC_NOERR) {
    nc_close(ncid);
    goto cleanup;
  }
  g_fault_vol.fail_dset_open_nth = 1;
  if (nc_inq_var_chunking(ncid, vid, &storage, NULL) == NC_NOERR) {
    nc_close(ncid);
    goto cleanup;
  }
  ResetFaultVolState();
  nc_close(ncid);
  ncid = -1;
  nc_set_chunk_cache(DEFAULT_CHUNK_CACHE_SIZE, DEFAULT_CHUNKS_IN_CACHE,
                     DEFAULT_CHUNK_CACHE_PREEMPTION);

  /* 6. read_hdf5_att & att_read_callbk */
  /*    (2353, 1818, 1821+1970, 1837, 1839+1972, 1930-1932, 1956+2375) */
  if (ExpectNattsFail(plain_path, ArmFailAttrOpenGInt) != 0) goto cleanup;
  if (ExpectNattsFail(plain_path, ArmFailAttrGetTypeGInt) != 0) goto cleanup;
  if (ExpectNattsFail(plain_path, ArmBadAttrTypeSpaceGInt) != 0) goto cleanup;
  if (ExpectNattsFail(plain_path, ArmFailAttrGetSpaceGInt) != 0) goto cleanup;
  if (ExpectNattsFail(plain_path, ArmBadAttrSpaceTypeGInt) != 0) goto cleanup;
  /* Upstream read_hdf5_att() allocates att->data via malloc() before fixed-str
   * H5Aread(); failing H5Aread leaves uninitialized pointers that crash
   * nc4_rec_grp_del_att_data() on nc_close(). Child process exits via _exit(). */
  if (ExpectNattsFailNoClose(plain_path, ArmFailAttrReadFixStr) != 0) {
    goto cleanup;
  }

  /* 7. read_scale (2474, 2479, 2482, 2560) */
  g_fault_vol.fail_attr_exists_name = NC_DIMID_ATT_NAME;
  if (ExpectOpenFail(multi_path) != 0) goto cleanup;

  g_fault_vol.fail_attr_open_name = NC_DIMID_ATT_NAME;
  if (ExpectOpenFail(multi_path) != 0) goto cleanup;

  g_fault_vol.watch_attr_name = NC_DIMID_ATT_NAME;
  g_fault_vol.fail_watched_attr_read = 1;
  g_fault_vol.fail_attr_close_name = NC_DIMID_ATT_NAME;
  if (ExpectOpenFail(multi_path) != 0) goto cleanup;
  g_fault_vol.fail_attr_close_name = COORDINATES;
  if (ExpectOpenFail(multi_path) != 0) goto cleanup;
  ResetFaultVolState();

  /* 8. read_dataset (2608, 2610+2640, 2614) */
  g_fault_vol.fail_dset_get_space_nth = 1;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  g_fault_vol.bad_dset_get_space_type_nth = 1;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  g_fault_vol.fail_attr_exists_name = "CLASS";
  g_fault_vol.fail_attr_exists_nth = 1;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  /* 9. read_hdf5_obj & rec_read_metadata */
  /*    (2716, 2774+2781, 2839+2908, 2849, 2856, 2858, 2925) */
  g_fault_vol.fail_obj_get_info_once = 1;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  g_fault_vol.fake_map_type_once = 1;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  g_fault_vol.fail_group_open_nth = 1;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  g_fault_vol.fail_group_open_nth = 2;
  if (ExpectOpenFail(sub_path) != 0) goto cleanup;

  g_fault_vol.fail_group_get_gcpl = 1;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  g_fault_vol.bad_group_get_gcpl_fapl = 1;
  if (ExpectOpenFail(plain_path) != 0) goto cleanup;

  if (ExpectNattsFail(plain_path, ArmFailAttrReadAndCloseGInt) != 0) {
    goto cleanup;
  }

  g_fault_vol.arm_deferred_group_close = 1;
  if (ExpectOpenFail(sub_path) != 0) goto cleanup;

  rc = 0;

cleanup:
  ResetFaultVolState();
  unsetenv("HDF5_VOL_CONNECTOR");
  H5VL__reparse_def_vol_conn_variable_test();
  if (vol_id >= 0) H5VLunregister_connector(vol_id);
  remove(plain_path);
  remove(sub_path);
  remove(scale_nc_path);
  remove(multi_path);
  remove(quant_path);
  remove(big_chunk_path);
  remove(comm_enum_path);
  return rc;
}
