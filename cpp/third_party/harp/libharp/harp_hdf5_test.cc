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
// Unit tests for libharp HDF5 import, export, metadata, and error helpers
// (libharp/harp-hdf5.c).
//
// Google3 build notes: HDF5 is enabled (HAVE_HDF5=1, H5_USE_110_API,
// H5_USE_16_API in config.h); HDF4 is disabled (#undef HAVE_HDF4); udunits2 XML
// is embedded via //third_party/udunits.
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-hdf5.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-internal.h

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace {

// Fault-injection state for HDF5 C API wrappers below. Only active inside
// ScopedHdf5Fault scopes on a single test thread.
const char* g_fault_symbol = nullptr;
int g_fault_skip = 0;
bool g_fault_triggered = false;
int g_h5ds_depth = 0;

bool ShouldFailH5(const char* name, bool allow_in_h5ds = false) {
  if (!allow_in_h5ds && g_h5ds_depth > 0) {
    return false;
  }
  if (g_fault_symbol != nullptr && std::strcmp(g_fault_symbol, name) == 0) {
    if (g_fault_skip == 0) {
      g_fault_triggered = true;
      return true;
    }
    --g_fault_skip;
  }
  return false;
}

struct H5dsDepthGuard {
  H5dsDepthGuard() { ++g_h5ds_depth; }
  ~H5dsDepthGuard() { --g_h5ds_depth; }
};

class ScopedHdf5Fault {
 public:
  explicit ScopedHdf5Fault(const char* symbol, int skip = 0) {
    g_fault_symbol = symbol;
    g_fault_skip = skip;
    g_fault_triggered = false;
  }
  ~ScopedHdf5Fault() {
    g_fault_symbol = nullptr;
    g_fault_skip = 0;
  }
  ScopedHdf5Fault(const ScopedHdf5Fault&) = delete;
  ScopedHdf5Fault& operator=(const ScopedHdf5Fault&) = delete;

  bool triggered() const { return g_fault_triggered; }
};

}  // namespace

// Weak symbol wrappers interposing HDF5 C API calls from libstcorp_harp.so in
// shared-library fastbuild/sanitizer builds while avoiding duplicate-symbol
// errors in static builds.
#define HARP_H5_WRAP(ret_t, name, fail_val, params, args)              \
  extern "C" __attribute__((weak)) ret_t name params {                 \
    if (ShouldFailH5(#name)) return (fail_val);                        \
    using Fn = ret_t(*) params;                                        \
    static Fn real_fn = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, #name)); \
    return real_fn args;                                               \
  }

using h5_hid_t = int64_t;
using h5_herr_t = int;
using h5_htri_t = int;
using h5_hsize_t = uint64_t;
using h5_hssize_t = int64_t;

HARP_H5_WRAP(h5_hid_t, H5Fcreate, -1,
             (const char* fn, unsigned flags, h5_hid_t fcpl, h5_hid_t fapl),
             (fn, flags, fcpl, fapl))
HARP_H5_WRAP(h5_hid_t, H5Fopen, -1,
             (const char* fn, unsigned flags, h5_hid_t fapl), (fn, flags, fapl))
// Close the real file before returning -1 so fault injection does not leak
// the underlying HDF5 file handle.
extern "C" __attribute__((weak)) h5_herr_t H5Fclose(h5_hid_t fid) {
  using Fn = h5_herr_t (*)(h5_hid_t);
  static Fn real_fn = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "H5Fclose"));
  h5_herr_t rc = real_fn(fid);
  if (ShouldFailH5("H5Fclose")) return -1;
  return rc;
}
HARP_H5_WRAP(h5_hid_t, H5Gopen1, -1, (h5_hid_t loc, const char* name),
             (loc, name))
HARP_H5_WRAP(h5_hid_t, H5Gget_create_plist, -1, (h5_hid_t gid), (gid))
HARP_H5_WRAP(h5_hid_t, H5Pcreate, -1, (h5_hid_t cls_id), (cls_id))
HARP_H5_WRAP(h5_herr_t, H5Pset_link_creation_order, -1,
             (h5_hid_t plist, unsigned flags), (plist, flags))
HARP_H5_WRAP(h5_herr_t, H5Pset_attr_creation_order, -1,
             (h5_hid_t plist, unsigned flags), (plist, flags))
HARP_H5_WRAP(h5_herr_t, H5Pget_link_creation_order, -1,
             (h5_hid_t plist, unsigned* flags), (plist, flags))
HARP_H5_WRAP(h5_herr_t, H5Pset_chunk, -1,
             (h5_hid_t plist, int ndims, const h5_hsize_t dim[]),
             (plist, ndims, dim))
HARP_H5_WRAP(h5_herr_t, H5Pset_deflate, -1, (h5_hid_t plist, unsigned level),
             (plist, level))
HARP_H5_WRAP(h5_herr_t, H5Pset_fill_value, -1,
             (h5_hid_t plist, h5_hid_t tid, const void* val), (plist, tid, val))
HARP_H5_WRAP(h5_hid_t, H5Screate, -1, (int type), (type))
HARP_H5_WRAP(h5_hid_t, H5Screate_simple, -1,
             (int rank, const h5_hsize_t dims[], const h5_hsize_t maxdims[]),
             (rank, dims, maxdims))
HARP_H5_WRAP(h5_htri_t, H5Sis_simple, -1, (h5_hid_t sid), (sid))
HARP_H5_WRAP(int, H5Sget_simple_extent_ndims, -1, (h5_hid_t sid), (sid))
HARP_H5_WRAP(int, H5Sget_simple_extent_dims, -1,
             (h5_hid_t sid, h5_hsize_t dims[], h5_hsize_t maxdims[]),
             (sid, dims, maxdims))
HARP_H5_WRAP(h5_hid_t, H5Tcopy, -1, (h5_hid_t tid), (tid))
HARP_H5_WRAP(h5_herr_t, H5Tset_size, -1, (h5_hid_t tid, size_t sz), (tid, sz))
HARP_H5_WRAP(h5_herr_t, H5Tset_strpad, -1, (h5_hid_t tid, int pad), (tid, pad))
HARP_H5_WRAP(size_t, H5Tget_size, 0, (h5_hid_t tid), (tid))
HARP_H5_WRAP(h5_hid_t, H5Tget_native_type, -1, (h5_hid_t tid, int dir),
             (tid, dir))
HARP_H5_WRAP(h5_hid_t, H5Acreate1, -1,
             (h5_hid_t loc, const char* name, h5_hid_t tid, h5_hid_t sid,
              h5_hid_t acpl),
             (loc, name, tid, sid, acpl))
HARP_H5_WRAP(h5_hid_t, H5Aopen_by_name, -1,
             (h5_hid_t loc, const char* obj, const char* attr, h5_hid_t aapl,
              h5_hid_t lapl),
             (loc, obj, attr, aapl, lapl))
HARP_H5_WRAP(h5_hid_t, H5Aget_space, -1, (h5_hid_t aid), (aid))
HARP_H5_WRAP(h5_hid_t, H5Aget_type, -1, (h5_hid_t aid), (aid))
HARP_H5_WRAP(h5_herr_t, H5Aread, -1, (h5_hid_t aid, h5_hid_t tid, void* buf),
             (aid, tid, buf))
HARP_H5_WRAP(h5_herr_t, H5Awrite, -1,
             (h5_hid_t aid, h5_hid_t tid, const void* buf), (aid, tid, buf))
HARP_H5_WRAP(h5_htri_t, H5Aexists, -1, (h5_hid_t obj, const char* name),
             (obj, name))
HARP_H5_WRAP(h5_hid_t, H5Dcreate1, -1,
             (h5_hid_t loc, const char* name, h5_hid_t tid, h5_hid_t sid,
              h5_hid_t dcpl),
             (loc, name, tid, sid, dcpl))
HARP_H5_WRAP(h5_hid_t, H5Dopen1, -1, (h5_hid_t loc, const char* name),
             (loc, name))
HARP_H5_WRAP(h5_hid_t, H5Dget_type, -1, (h5_hid_t did), (did))
HARP_H5_WRAP(h5_hid_t, H5Dget_space, -1, (h5_hid_t did), (did))
HARP_H5_WRAP(h5_herr_t, H5Dread, -1,
             (h5_hid_t did, h5_hid_t mtid, h5_hid_t msid, h5_hid_t fsid,
              h5_hid_t dxpl, void* buf),
             (did, mtid, msid, fsid, dxpl, buf))
HARP_H5_WRAP(h5_herr_t, H5Dwrite, -1,
             (h5_hid_t did, h5_hid_t mtid, h5_hid_t msid, h5_hid_t fsid,
              h5_hid_t dxpl, const void* buf),
             (did, mtid, msid, fsid, dxpl, buf))
#define HARP_H5_WRAP_H5DS(ret_t, name, fail_val, params, args)         \
  extern "C" __attribute__((weak)) ret_t name params {                 \
    if (ShouldFailH5(#name)) return (fail_val);                        \
    H5dsDepthGuard depth_guard;                                        \
    using Fn = ret_t(*) params;                                        \
    static Fn real_fn = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, #name)); \
    return real_fn args;                                               \
  }

// Note: H5Oget_info1 is called by harp-hdf5.c's visitor inside
// H5DSiterate_scales, so it allows fault injection even when g_h5ds_depth > 0.
extern "C" __attribute__((weak)) h5_herr_t H5Oget_info1(h5_hid_t loc,
                                                        void* oinfo) {
  if (ShouldFailH5("H5Oget_info1", /*allow_in_h5ds=*/true)) return -1;
  using Fn = h5_herr_t (*)(h5_hid_t, void*);
  static Fn real_fn = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "H5Oget_info1"));
  return real_fn(loc, oinfo);
}
HARP_H5_WRAP(h5_herr_t, H5Oget_info_by_name1, -1,
             (h5_hid_t loc, const char* name, void* oinfo, h5_hid_t lapl),
             (loc, name, oinfo, lapl))
HARP_H5_WRAP(h5_herr_t, H5Literate1, -1,
             (h5_hid_t grp, int idx_type, int order, h5_hsize_t* idx, void* op,
              void* op_data),
             (grp, idx_type, order, idx, op, op_data))
HARP_H5_WRAP_H5DS(h5_htri_t, H5DSis_scale, -1, (h5_hid_t did), (did))
HARP_H5_WRAP_H5DS(h5_herr_t, H5DSset_scale, -1,
                  (h5_hid_t dsid, const char* dim), (dsid, dim))
HARP_H5_WRAP_H5DS(h5_hssize_t, H5DSget_scale_name, -1,
                  (h5_hid_t did, char* name, size_t size), (did, name, size))
HARP_H5_WRAP_H5DS(int, H5DSget_num_scales, -1, (h5_hid_t did, unsigned dim),
                  (did, dim))
HARP_H5_WRAP_H5DS(h5_herr_t, H5DSiterate_scales, 1,
                  (h5_hid_t did, unsigned dim, int* idx, void* visitor,
                   void* data),
                  (did, dim, idx, visitor, data))
HARP_H5_WRAP_H5DS(h5_herr_t, H5DSattach_scale, -1,
                  (h5_hid_t did, h5_hid_t dsid, unsigned idx), (did, dsid, idx))

namespace stcorp_harp {
namespace {

using ::testing::HasSubstr;

int QuietPrint(const char* /*fmt*/, ...) { return 0; }

// Minimal dynamic HDF5 API loader (via dlsym on already-linked libhdf5.so)
// for synthesizing custom/edge-case HDF5 files directly in TempDir.
struct Hdf5Api {
  static constexpr unsigned kH5fAccTrunc = 0x0002u;
  static constexpr h5_hid_t kH5pDefault = 0;
  static constexpr h5_hid_t kH5sAll = 0;
  static constexpr int kH5sScalar = 0;
  static constexpr int kH5sSimple = 1;
  static constexpr int kH5sNull = 2;
  static constexpr int kH5tOpaque = 5;
  static constexpr size_t kH5tVariable = static_cast<size_t>(-1);

  template <typename T>
  static T Sym(const char* name) {
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
  }

  static h5_hid_t GlobalId(const char* name) {
    const auto* ptr = static_cast<const h5_hid_t*>(dlsym(RTLD_DEFAULT, name));
    return ptr != nullptr ? *ptr : -1;
  }

  h5_herr_t (*open)() = Sym<h5_herr_t (*)()>("H5open");
  h5_hid_t (*fcreate)(const char*, unsigned, h5_hid_t, h5_hid_t) =
      Sym<h5_hid_t (*)(const char*, unsigned, h5_hid_t, h5_hid_t)>("H5Fcreate");
  h5_herr_t (*fclose)(h5_hid_t) = Sym<h5_herr_t (*)(h5_hid_t)>("H5Fclose");
  h5_hid_t (*gcreate2)(h5_hid_t, const char*, h5_hid_t, h5_hid_t, h5_hid_t) =
      Sym<h5_hid_t (*)(h5_hid_t, const char*, h5_hid_t, h5_hid_t, h5_hid_t)>(
          "H5Gcreate2");
  h5_herr_t (*gclose)(h5_hid_t) = Sym<h5_herr_t (*)(h5_hid_t)>("H5Gclose");
  h5_hid_t (*dcreate2)(h5_hid_t, const char*, h5_hid_t, h5_hid_t, h5_hid_t,
                       h5_hid_t, h5_hid_t) =
      Sym<h5_hid_t (*)(h5_hid_t, const char*, h5_hid_t, h5_hid_t, h5_hid_t,
                       h5_hid_t, h5_hid_t)>("H5Dcreate2");
  h5_herr_t (*dwrite)(h5_hid_t, h5_hid_t, h5_hid_t, h5_hid_t, h5_hid_t,
                      const void*) =
      Sym<h5_herr_t (*)(h5_hid_t, h5_hid_t, h5_hid_t, h5_hid_t, h5_hid_t,
                        const void*)>("H5Dwrite");
  h5_herr_t (*dclose)(h5_hid_t) = Sym<h5_herr_t (*)(h5_hid_t)>("H5Dclose");
  h5_hid_t (*acreate2)(h5_hid_t, const char*, h5_hid_t, h5_hid_t, h5_hid_t,
                       h5_hid_t) =
      Sym<h5_hid_t (*)(h5_hid_t, const char*, h5_hid_t, h5_hid_t, h5_hid_t,
                       h5_hid_t)>("H5Acreate2");
  h5_herr_t (*awrite)(h5_hid_t, h5_hid_t, const void*) =
      Sym<h5_herr_t (*)(h5_hid_t, h5_hid_t, const void*)>("H5Awrite");
  h5_herr_t (*aclose)(h5_hid_t) = Sym<h5_herr_t (*)(h5_hid_t)>("H5Aclose");
  h5_hid_t (*screate)(int) = Sym<h5_hid_t (*)(int)>("H5Screate");
  h5_hid_t (*screate_simple)(int, const h5_hsize_t[], const h5_hsize_t[]) =
      Sym<h5_hid_t (*)(int, const h5_hsize_t[], const h5_hsize_t[])>(
          "H5Screate_simple");
  h5_herr_t (*sclose)(h5_hid_t) = Sym<h5_herr_t (*)(h5_hid_t)>("H5Sclose");
  h5_hid_t (*tcreate)(int,
                      size_t) = Sym<h5_hid_t (*)(int, size_t)>("H5Tcreate");
  h5_hid_t (*tcopy)(h5_hid_t) = Sym<h5_hid_t (*)(h5_hid_t)>("H5Tcopy");
  h5_herr_t (*tset_size)(h5_hid_t, size_t) =
      Sym<h5_herr_t (*)(h5_hid_t, size_t)>("H5Tset_size");
  h5_herr_t (*tclose)(h5_hid_t) = Sym<h5_herr_t (*)(h5_hid_t)>("H5Tclose");
  h5_herr_t (*ds_set_scale)(h5_hid_t, const char*) =
      Sym<h5_herr_t (*)(h5_hid_t, const char*)>("H5DSset_scale");
  h5_herr_t (*ds_attach_scale)(h5_hid_t, h5_hid_t, unsigned) =
      Sym<h5_herr_t (*)(h5_hid_t, h5_hid_t, unsigned)>("H5DSattach_scale");

  Hdf5Api() {
    if (open != nullptr) {
      open();
    }
  }

  bool ok() const {
    return fcreate != nullptr && fclose != nullptr && gcreate2 != nullptr &&
           gclose != nullptr && dcreate2 != nullptr && dwrite != nullptr &&
           dclose != nullptr && acreate2 != nullptr && awrite != nullptr &&
           aclose != nullptr && screate != nullptr &&
           screate_simple != nullptr && sclose != nullptr &&
           tcreate != nullptr && tcopy != nullptr && tset_size != nullptr &&
           tclose != nullptr && ds_set_scale != nullptr &&
           ds_attach_scale != nullptr;
  }

  void WriteFixedStringAttr(h5_hid_t obj, const char* name,
                            const char* value) const {
    h5_hid_t tid = tcopy(GlobalId("H5T_C_S1_g"));
    tset_size(tid, std::strlen(value));
    h5_hid_t sid = screate(kH5sScalar);
    h5_hid_t aid = acreate2(obj, name, tid, sid, kH5pDefault, kH5pDefault);
    awrite(aid, tid, value);
    aclose(aid);
    sclose(sid);
    tclose(tid);
  }

  void WriteInt32Attr(h5_hid_t obj, const char* name, int32_t value) const {
    h5_hid_t sid = screate(kH5sScalar);
    h5_hid_t tid = GlobalId("H5T_NATIVE_INT_g");
    h5_hid_t aid = acreate2(obj, name, tid, sid, kH5pDefault, kH5pDefault);
    awrite(aid, tid, &value);
    aclose(aid);
    sclose(sid);
  }

  h5_hid_t CreateHarpFile(const char* filename) const {
    h5_hid_t fid = fcreate(filename, kH5fAccTrunc, kH5pDefault, kH5pDefault);
    if (fid >= 0) {
      WriteFixedStringAttr(fid, "Conventions", HARP_CONVENTION);
    }
    return fid;
  }
};

// Helper to create and add a variable to `product` (transferring ownership).
void AddVar(harp_product* product, const char* name, harp_data_type data_type,
            int num_dims, const harp_dimension_type* dim_types,
            const long* dims, harp_variable** out_var = nullptr) {
  harp_variable* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new(name, data_type, num_dims,
                                            dim_types, dims, &raw));
  ScopedHarpVariable var(raw);
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(product, var.get()));
  harp_variable* borrowed = var.release();
  if (out_var != nullptr) {
    *out_var = borrowed;
  }
}

template <typename Fn>
void SweepH5Faults(const char* symbol, Fn&& fn) {
  for (int skip = 0;; ++skip) {
    ScopedHdf5Fault fault(symbol, skip);
    harp_set_error(HARP_SUCCESS, nullptr);
    int rc = fn();
    if (!fault.triggered()) {
      break;
    }
    EXPECT_EQ(-1, rc) << "symbol=" << symbol << " skip=" << skip;
  }
}

TEST(HarpHdf5Test, NullAndMissingPathErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_export_hdf5(nullptr, prod.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_export_hdf5("unused.h5", nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // Calling harp_hdf5_add_error_message() with an empty HDF5 error stack does
  // not trigger H5Eget_major/H5Eget_minor (which leak in HDF5 1.8+/2.1.0).
  harp_set_error(HARP_ERROR_HDF5, "synthetic hdf5 error");
  harp_hdf5_add_error_message();
  EXPECT_EQ(HARP_ERROR_HDF5, harp_errno);

  harp_product_metadata* raw_meta = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
  ScopedHarpProductMetadata meta(raw_meta);

  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_import_metadata_hdf5(nullptr, meta.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
}

TEST(HarpHdf5Test, NonHdf5AndEmptyFileRejectedBeforeHdf5Open) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  const TempFile missing("harp_hdf5_missing_via_public.h5");
  harp_product* imported = nullptr;
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_import(missing.c_str(), nullptr, nullptr, &imported));
  EXPECT_EQ(HARP_ERROR_FILE_NOT_FOUND, harp_errno);
  EXPECT_EQ(nullptr, imported);

  const TempFile empty("harp_hdf5_empty.h5");
  ASSERT_TRUE(WriteFileBytes(empty.path(), {}));
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_import(empty.c_str(), nullptr, nullptr, &imported));
  EXPECT_EQ(-1, harp_import_test(empty.c_str(), QuietPrint));

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_export(empty.c_str(), "unknown_fmt", prod.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
}

TEST(HarpHdf5Test, RoundTripAllTypesDimensionsAttributesAndEnums) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_set_source_product(prod.get(), "synthetic_src"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_set_history(prod.get(), "2026-01-01T00:00:00Z test"));

  const harp_dimension_type kTimeDim[1] = {harp_dimension_time};
  const long kTimeLen[1] = {2};
  const harp_dimension_type kLatDim[1] = {harp_dimension_latitude};
  const long kLatLen[1] = {2};
  const harp_dimension_type kLonDim[1] = {harp_dimension_longitude};
  const long kLonLen[1] = {2};
  const harp_dimension_type kVertDim[1] = {harp_dimension_vertical};
  const long kVertLen[1] = {2};
  const harp_dimension_type kSpecDim[1] = {harp_dimension_spectral};
  const long kSpecLen[1] = {2};
  const harp_dimension_type kIndepDim[1] = {harp_dimension_independent};
  const long kIndepLen3[1] = {3};
  const harp_dimension_type kTimeIndepDim[2] = {harp_dimension_time,
                                                harp_dimension_independent};
  const long kTimeIndepLen[2] = {2, 3};

  harp_variable* var_time = nullptr;
  AddVar(prod.get(), "datetime", harp_type_double, 1, kTimeDim, kTimeLen,
         &var_time);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_unit(var_time, "days since 2000-01-01"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_description(var_time, "measurement time"));
  var_time->valid_min.double_data = 0.0;
  var_time->valid_max.double_data = 100000.0;
  var_time->data.double_data[0] = 100.5;
  var_time->data.double_data[1] = 102.25;

  harp_variable* var_time_coord = nullptr;
  AddVar(prod.get(), "time", harp_type_double, 1, kTimeDim, kTimeLen,
         &var_time_coord);
  var_time_coord->data.double_data[0] = 0.0;
  var_time_coord->data.double_data[1] = 1.0;

  harp_variable* var_lat = nullptr;
  AddVar(prod.get(), "latitude", harp_type_float, 1, kLatDim, kLatLen,
         &var_lat);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(var_lat, "degree_north"));
  var_lat->valid_min.float_data = -90.0f;
  var_lat->valid_max.float_data = 90.0f;
  var_lat->data.float_data[0] = -45.0f;
  var_lat->data.float_data[1] = 45.0f;

  harp_variable* var_lon_val = nullptr;
  AddVar(prod.get(), "lon_sample", harp_type_int16, 1, kLonDim, kLonLen,
         &var_lon_val);
  var_lon_val->valid_min.int16_data = -180;
  var_lon_val->valid_max.int16_data = 180;
  var_lon_val->data.int16_data[0] = -10;
  var_lon_val->data.int16_data[1] = 20;

  harp_variable* var_vert_val = nullptr;
  AddVar(prod.get(), "vert_sample", harp_type_int32, 1, kVertDim, kVertLen,
         &var_vert_val);
  var_vert_val->valid_min.int32_data = 0;
  var_vert_val->valid_max.int32_data = 50000;
  var_vert_val->data.int32_data[0] = 100;
  var_vert_val->data.int32_data[1] = 200;

  harp_variable* var_spec_val = nullptr;
  AddVar(prod.get(), "spec_sample", harp_type_float, 1, kSpecDim, kSpecLen,
         &var_spec_val);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(var_spec_val, ""));
  var_spec_val->data.float_data[0] = 550.5f;
  var_spec_val->data.float_data[1] = static_cast<float>(harp_nan());

  harp_variable* var_flag = nullptr;
  AddVar(prod.get(), "quality_flag", harp_type_int8, 1, kTimeDim, kTimeLen,
         &var_flag);
  const char* kFlagNames[] = {"good", "bad"};
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(var_flag, 2, kFlagNames));
  var_flag->valid_min.int8_data = 0;
  var_flag->valid_max.int8_data = 1;
  var_flag->data.int8_data[0] = 0;
  var_flag->data.int8_data[1] = 1;

  harp_variable* var_str = nullptr;
  AddVar(prod.get(), "label", harp_type_string, 1, kIndepDim, kIndepLen3,
         &var_str);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(var_str, 0, "a"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(var_str, 1, "longer_str"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(var_str, 2, ""));

  harp_variable* var_scalar_str = nullptr;
  AddVar(prod.get(), "scalar_str", harp_type_string, 0, nullptr, nullptr,
         &var_scalar_str);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(var_scalar_str, 0, "scalar"));

  harp_variable* var_2d = nullptr;
  AddVar(prod.get(), "grid_val", harp_type_double, 2, kTimeIndepDim,
         kTimeIndepLen, &var_2d);
  for (int i = 0; i < 6; ++i) {
    var_2d->data.double_data[i] = static_cast<double>(i) * 1.5;
  }

  const TempFile h5_file("harp_hdf5_roundtrip.h5");
  ASSERT_EQ(HARP_SUCCESS, harp_export(h5_file.c_str(), "hdf5", prod.get()));

  harp_product* raw_imported = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_import_hdf5(h5_file.c_str(), &raw_imported));
  ScopedHarpProduct imported(raw_imported);

  EXPECT_STREQ("synthetic_src", imported->source_product);
  EXPECT_STREQ("2026-01-01T00:00:00Z test", imported->history);
  EXPECT_EQ(2L, imported->dimension[harp_dimension_time]);

  harp_variable* got = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(imported.get(),
                                                            "datetime", &got));
  EXPECT_STREQ("days since 2000-01-01", got->unit);
  EXPECT_STREQ("measurement time", got->description);
  EXPECT_DOUBLE_EQ(100.5, got->data.double_data[0]);
  EXPECT_DOUBLE_EQ(102.25, got->data.double_data[1]);

  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              imported.get(), "quality_flag", &got));
  EXPECT_EQ(2, got->num_enum_values);
  EXPECT_STREQ("good", got->enum_name[0]);
  EXPECT_STREQ("bad", got->enum_name[1]);

  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(imported.get(), "label", &got));
  EXPECT_STREQ("a", got->data.string_data[0]);
  EXPECT_STREQ("longer_str", got->data.string_data[1]);
  EXPECT_STREQ("", got->data.string_data[2]);

  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              imported.get(), "spec_sample", &got));
  EXPECT_STREQ("", got->unit);
  EXPECT_TRUE(std::isnan(got->data.float_data[1]));

  harp_product_metadata* raw_meta = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_import_product_metadata(h5_file.c_str(), nullptr, &raw_meta));
  ScopedHarpProductMetadata meta(raw_meta);
  EXPECT_DOUBLE_EQ(100.5, meta->datetime_start);
  EXPECT_DOUBLE_EQ(102.25, meta->datetime_stop);
  EXPECT_STREQ("HARP_HDF5", meta->format);

  harp_product* raw_pub = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_import(h5_file.c_str(), nullptr, nullptr, &raw_pub));
  ScopedHarpProduct pub(raw_pub);
  EXPECT_EQ(0, harp_import_test(h5_file.c_str(), QuietPrint));
}

TEST(HarpHdf5Test, NonCoordVariableWithDimensionNameAndDimensionMismatch) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  const harp_dimension_type kTimeDim[1] = {harp_dimension_time};
  const long kTimeLen[1] = {2};
  harp_variable* dt = nullptr;
  AddVar(prod.get(), "datetime", harp_type_double, 1, kTimeDim, kTimeLen, &dt);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(dt, "days since 2000-01-01"));
  dt->data.double_data[0] = 1.0;
  dt->data.double_data[1] = 2.0;

  const harp_dimension_type kNonCoordDims[2] = {harp_dimension_time,
                                                harp_dimension_independent};
  const long kNonCoordLens[2] = {2, 2};
  harp_variable* non_coord_time = nullptr;
  AddVar(prod.get(), "time", harp_type_int32, 2, kNonCoordDims, kNonCoordLens,
         &non_coord_time);
  non_coord_time->data.int32_data[0] = 10;
  non_coord_time->data.int32_data[1] = 20;
  non_coord_time->data.int32_data[2] = 30;
  non_coord_time->data.int32_data[3] = 40;

  const TempFile h5_file("harp_hdf5_non_coord.h5");
  ASSERT_EQ(HARP_SUCCESS, harp_export_hdf5(h5_file.c_str(), prod.get()));

  harp_product* raw_imported = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_import_hdf5(h5_file.c_str(), &raw_imported));
  ScopedHarpProduct imported(raw_imported);

  harp_variable* got = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(imported.get(), "time", &got));
  EXPECT_EQ(2, got->num_dimensions);
  EXPECT_EQ(10, got->data.int32_data[0]);
  EXPECT_EQ(40, got->data.int32_data[3]);

  // Simulate mismatched product->dimension[time] == 0 so attach_dimensions
  // fails with "no dimension of type 'time' and length 2".
  prod->dimension[harp_dimension_time] = 0;
  const TempFile bad_dim_file("harp_hdf5_bad_dim_attach.h5");
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_export_hdf5(bad_dim_file.c_str(), prod.get()));
  EXPECT_EQ(HARP_ERROR_EXPORT, harp_errno);
}

TEST(HarpHdf5Test, MetadataDefaultsAndManyIndependentDimensions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  for (long len = 1; len <= 18; ++len) {
    const harp_dimension_type dim_type[1] = {harp_dimension_independent};
    const long dim_len[1] = {len};
    const std::string name = "indep_" + std::to_string(len);
    harp_variable* v = nullptr;
    AddVar(prod.get(), name.c_str(), harp_type_int32, 1, dim_type, dim_len, &v);
    for (long k = 0; k < len; ++k) {
      v->data.int32_data[k] = static_cast<int32_t>(k);
    }
  }

  const TempFile h5_file("harp_hdf5_many_dims.h5");
  ASSERT_EQ(HARP_SUCCESS, harp_export_hdf5(h5_file.c_str(), prod.get()));

  harp_product_metadata* raw_meta = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
  ScopedHarpProductMetadata meta(raw_meta);
  ASSERT_EQ(HARP_SUCCESS,
            harp_import_metadata_hdf5(h5_file.c_str(), meta.get()));
  EXPECT_EQ(1, harp_ismininf(meta->datetime_start));
  EXPECT_EQ(1, harp_isplusinf(meta->datetime_stop));
  EXPECT_STREQ("HARP_HDF5", meta->format);
  EXPECT_STREQ("harp_hdf5_many_dims.h5", meta->source_product);
  EXPECT_EQ(nullptr, meta->history);

  harp_product* raw_imported = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_import_hdf5(h5_file.c_str(), &raw_imported));
  ScopedHarpProduct imported(raw_imported);
  EXPECT_EQ(18, imported->num_variables);
}

TEST(HarpHdf5Test, CompressionOptionRoundTrip) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const int prev_compression = harp_get_option_hdf5_compression();
  auto restore_compression = absl::MakeCleanup([prev_compression] {
    harp_set_option_hdf5_compression(prev_compression);
  });
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  ASSERT_EQ(HARP_SUCCESS, harp_set_option_hdf5_compression(6));

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  harp_variable* scalar = nullptr;
  AddVar(prod.get(), "scalar_val", harp_type_double, 0, nullptr, nullptr,
         &scalar);
  scalar->data.double_data[0] = 3.14;

  const harp_dimension_type kTimeDim[1] = {harp_dimension_time};
  const long kTimeLen[1] = {4};
  harp_variable* arr = nullptr;
  AddVar(prod.get(), "arr_val", harp_type_int32, 1, kTimeDim, kTimeLen, &arr);
  for (int i = 0; i < 4; ++i) {
    arr->data.int32_data[i] = i * 11;
  }

  harp_variable* str_arr = nullptr;
  AddVar(prod.get(), "str_arr", harp_type_string, 1, kTimeDim, kTimeLen,
         &str_arr);
  for (int i = 0; i < 4; ++i) {
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_set_string_data_element(str_arr, i, "compressed"));
  }

  const TempFile h5_file("harp_hdf5_compressed.h5");
  ASSERT_EQ(HARP_SUCCESS, harp_export_hdf5(h5_file.c_str(), prod.get()));

  harp_product* raw_imported = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_import_hdf5(h5_file.c_str(), &raw_imported));
  ScopedHarpProduct imported(raw_imported);

  harp_variable* got = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(imported.get(), "arr_val", &got));
  EXPECT_EQ(33, got->data.int32_data[3]);
}

TEST(HarpHdf5Test, CustomHdf5SchemasCoverVerificationAndTypeErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  Hdf5Api h5;
  ASSERT_TRUE(h5.ok());

  // 1. Missing Conventions -> HARP_ERROR_UNSUPPORTED_PRODUCT.
  {
    const TempFile f("harp_hdf5_no_conv.h5");
    h5_hid_t fid = h5.fcreate(f.c_str(), Hdf5Api::kH5fAccTrunc,
                              Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    ASSERT_GE(fid, 0);
    h5.fclose(fid);

    harp_product* p = nullptr;
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_hdf5(f.c_str(), &p));
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, harp_errno);

    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_metadata_hdf5(f.c_str(), meta.get()));
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, harp_errno);
  }

  // 2. Non-HARP Conventions and future HARP version.
  {
    const TempFile f("harp_hdf5_bad_conv.h5");
    h5_hid_t fid = h5.fcreate(f.c_str(), Hdf5Api::kH5fAccTrunc,
                              Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    ASSERT_GE(fid, 0);
    h5.WriteFixedStringAttr(fid, "Conventions", "CF-1.6");
    h5.fclose(fid);

    harp_product* p = nullptr;
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_hdf5(f.c_str(), &p));
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, harp_errno);
  }
  {
    const TempFile f("harp_hdf5_future_conv.h5");
    h5_hid_t fid = h5.fcreate(f.c_str(), Hdf5Api::kH5fAccTrunc,
                              Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    ASSERT_GE(fid, 0);
    h5.WriteFixedStringAttr(fid, "Conventions", "HARP-9.9");
    h5.fclose(fid);

    harp_product* p = nullptr;
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_hdf5(f.c_str(), &p));
    EXPECT_EQ(HARP_ERROR_FILE_OPEN, harp_errno);
  }

  // 3. Invalid datetime_start / datetime_stop attribute types in metadata.
  for (const char* attr_name : {"datetime_start", "datetime_stop"}) {
    const TempFile f("harp_hdf5_bad_dt_attr.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);
    h5.WriteInt32Attr(fid, attr_name, 123);
    h5.fclose(fid);

    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_metadata_hdf5(f.c_str(), meta.get()));
    EXPECT_EQ(HARP_ERROR_IMPORT, harp_errno);
  }

  // 4. Valid custom file without link creation order tracking, subgroup,
  // units="1", and 1-element array attribute.
  {
    const TempFile f("harp_hdf5_custom_valid.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);

    h5_hid_t grp = h5.gcreate2(fid, "subgrp", Hdf5Api::kH5pDefault,
                               Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    h5.gclose(grp);

    const h5_hsize_t dims[1] = {1};
    h5_hid_t sid = h5.screate_simple(1, dims, nullptr);
    h5_hid_t tid = Hdf5Api::GlobalId("H5T_NATIVE_DOUBLE_g");
    h5_hid_t did = h5.dcreate2(fid, "x", tid, sid, Hdf5Api::kH5pDefault,
                               Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    const double val = 7.5;
    h5.dwrite(did, tid, Hdf5Api::kH5sAll, Hdf5Api::kH5sAll,
              Hdf5Api::kH5pDefault, &val);
    h5.WriteFixedStringAttr(did, "units", "1");
    const double vmin = 0.0;
    h5_hid_t aid = h5.acreate2(did, "valid_min", tid, sid, Hdf5Api::kH5pDefault,
                               Hdf5Api::kH5pDefault);
    h5.awrite(aid, tid, &vmin);
    h5.aclose(aid);
    h5.dclose(did);
    h5.sclose(sid);
    h5.fclose(fid);

    harp_product* raw_p = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_import_hdf5(f.c_str(), &raw_p));
    ScopedHarpProduct p(raw_p);
    harp_variable* got = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_get_variable_by_name(p.get(), "x", &got));
    EXPECT_STREQ("", got->unit);
    EXPECT_DOUBLE_EQ(0.0, got->valid_min.double_data);
  }

  // 5. Unsupported dataset types in get_harp_type: int64, uint32, long double,
  // and opaque.
  for (const char* type_global : {"H5T_STD_I64LE_g", "H5T_STD_U32LE_g",
                                  "H5T_NATIVE_LDOUBLE_g", "OPAQUE"}) {
    const TempFile f("harp_hdf5_bad_dtype.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);
    h5_hid_t tid = std::strcmp(type_global, "OPAQUE") == 0
                       ? h5.tcreate(Hdf5Api::kH5tOpaque, 4)
                       : Hdf5Api::GlobalId(type_global);
    const h5_hsize_t dims[1] = {1};
    h5_hid_t sid = h5.screate_simple(1, dims, nullptr);
    h5_hid_t did = h5.dcreate2(fid, "bad", tid, sid, Hdf5Api::kH5pDefault,
                               Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    ASSERT_GE(did, 0) << type_global;
    h5.dclose(did);
    h5.sclose(sid);
    if (std::strcmp(type_global, "OPAQUE") == 0) {
      h5.tclose(tid);
    }
    h5.fclose(fid);

    harp_product* p = nullptr;
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_hdf5(f.c_str(), &p)) << type_global;
    EXPECT_EQ(HARP_ERROR_IMPORT, harp_errno) << type_global;
  }

  // 6. String attribute errors: non-scalar dataspace, variable-length string,
  // and non-string type.
  {
    const TempFile f("harp_hdf5_str_attr_nonscalar.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);
    const h5_hsize_t dims[1] = {2};
    h5_hid_t sid = h5.screate_simple(1, dims, nullptr);
    h5_hid_t tid = h5.tcopy(Hdf5Api::GlobalId("H5T_C_S1_g"));
    h5.tset_size(tid, 2);
    h5_hid_t aid = h5.acreate2(fid, "source_product", tid, sid,
                               Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    h5.awrite(aid, tid, "abcd");
    h5.aclose(aid);
    h5.tclose(tid);
    h5.sclose(sid);
    h5.fclose(fid);

    harp_product* p = nullptr;
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_hdf5(f.c_str(), &p));
    EXPECT_EQ(HARP_ERROR_IMPORT, harp_errno);
  }
  {
    const TempFile f("harp_hdf5_str_attr_vlen.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);
    h5_hid_t sid = h5.screate(Hdf5Api::kH5sScalar);
    h5_hid_t tid = h5.tcopy(Hdf5Api::GlobalId("H5T_C_S1_g"));
    h5.tset_size(tid, Hdf5Api::kH5tVariable);
    h5_hid_t aid = h5.acreate2(fid, "source_product", tid, sid,
                               Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    const char* str_ptr = "vlen";
    h5.awrite(aid, tid, &str_ptr);
    h5.aclose(aid);
    h5.tclose(tid);
    h5.sclose(sid);
    h5.fclose(fid);

    harp_product* p = nullptr;
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_hdf5(f.c_str(), &p));
    EXPECT_EQ(HARP_ERROR_IMPORT, harp_errno);
  }

  // 7. Numeric attribute errors: 2-element array, H5S_NULL, string attribute
  // read as numeric, and mismatched valid_min / valid_max type.
  {
    const TempFile f("harp_hdf5_num_attr_2elem.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);
    const h5_hsize_t dims[1] = {2};
    h5_hid_t sid = h5.screate_simple(1, dims, nullptr);
    h5_hid_t tid = Hdf5Api::GlobalId("H5T_NATIVE_DOUBLE_g");
    const double vals[2] = {1.0, 2.0};
    h5_hid_t aid = h5.acreate2(fid, "datetime_start", tid, sid,
                               Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    h5.awrite(aid, tid, vals);
    h5.aclose(aid);
    h5.sclose(sid);
    h5.fclose(fid);

    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_metadata_hdf5(f.c_str(), meta.get()));
    EXPECT_EQ(HARP_ERROR_IMPORT, harp_errno);
  }
  {
    const TempFile f("harp_hdf5_num_attr_null.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);
    h5_hid_t sid = h5.screate(Hdf5Api::kH5sNull);
    h5_hid_t tid = Hdf5Api::GlobalId("H5T_NATIVE_DOUBLE_g");
    h5_hid_t aid = h5.acreate2(fid, "datetime_start", tid, sid,
                               Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    h5.aclose(aid);
    h5.sclose(sid);
    h5.fclose(fid);

    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_metadata_hdf5(f.c_str(), meta.get()));
    EXPECT_EQ(HARP_ERROR_IMPORT, harp_errno);
  }
  for (const char* attr_name : {"valid_min", "valid_max"}) {
    const TempFile f("harp_hdf5_bad_valid_range_type.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);
    const h5_hsize_t dims[1] = {1};
    h5_hid_t sid = h5.screate_simple(1, dims, nullptr);
    h5_hid_t tid = Hdf5Api::GlobalId("H5T_NATIVE_DOUBLE_g");
    h5_hid_t did = h5.dcreate2(fid, "x", tid, sid, Hdf5Api::kH5pDefault,
                               Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    const double val = 1.0;
    h5.dwrite(did, tid, Hdf5Api::kH5sAll, Hdf5Api::kH5sAll,
              Hdf5Api::kH5pDefault, &val);
    h5.WriteInt32Attr(did, attr_name, 0);
    h5.dclose(did);
    h5.sclose(sid);
    h5.fclose(fid);

    harp_product* p = nullptr;
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_hdf5(f.c_str(), &p));
    EXPECT_EQ(HARP_ERROR_IMPORT, harp_errno);
  }
  {
    const TempFile f("harp_hdf5_num_attr_is_str.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);
    const h5_hsize_t dims[1] = {1};
    h5_hid_t sid = h5.screate_simple(1, dims, nullptr);
    h5_hid_t tid = Hdf5Api::GlobalId("H5T_NATIVE_DOUBLE_g");
    h5_hid_t did = h5.dcreate2(fid, "x", tid, sid, Hdf5Api::kH5pDefault,
                               Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    h5.WriteFixedStringAttr(did, "valid_min", "bad");
    h5.dclose(did);
    h5.sclose(sid);
    h5.fclose(fid);

    harp_product* p = nullptr;
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_hdf5(f.c_str(), &p));
    EXPECT_EQ(HARP_ERROR_IMPORT, harp_errno);
  }

  // 8. Invalid flag_meanings on int8 dataset.
  {
    const TempFile f("harp_hdf5_bad_flag_meanings.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);
    const h5_hsize_t dims[1] = {1};
    h5_hid_t sid = h5.screate_simple(1, dims, nullptr);
    h5_hid_t tid = Hdf5Api::GlobalId("H5T_NATIVE_SCHAR_g");
    h5_hid_t did = h5.dcreate2(fid, "q", tid, sid, Hdf5Api::kH5pDefault,
                               Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    const int8_t v = 0;
    h5.dwrite(did, tid, Hdf5Api::kH5sAll, Hdf5Api::kH5sAll,
              Hdf5Api::kH5pDefault, &v);
    h5.WriteFixedStringAttr(did, "flag_meanings", "123bad");
    h5.dclose(did);
    h5.sclose(sid);
    h5.fclose(fid);

    harp_product* p = nullptr;
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_hdf5(f.c_str(), &p));
  }

  // 9. Dataset with 9 dimensions (> HARP_MAX_NUM_DIMS == 8).
  {
    const TempFile f("harp_hdf5_9dims.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);
    const h5_hsize_t dims[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    h5_hid_t sid = h5.screate_simple(9, dims, nullptr);
    h5_hid_t tid = Hdf5Api::GlobalId("H5T_NATIVE_DOUBLE_g");
    h5_hid_t did = h5.dcreate2(fid, "d9", tid, sid, Hdf5Api::kH5pDefault,
                               Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    h5.dclose(did);
    h5.sclose(sid);
    h5.fclose(fid);

    harp_product* p = nullptr;
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_hdf5(f.c_str(), &p));
    EXPECT_EQ(HARP_ERROR_IMPORT, harp_errno);
  }

  // 10. Dataset with 2 attached dimension scales on dim 0.
  {
    const TempFile f("harp_hdf5_2scales.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);
    const h5_hsize_t dims[1] = {2};
    h5_hid_t sid = h5.screate_simple(1, dims, nullptr);
    h5_hid_t tid = Hdf5Api::GlobalId("H5T_NATIVE_DOUBLE_g");
    h5_hid_t s1 = h5.dcreate2(fid, "s1", tid, sid, Hdf5Api::kH5pDefault,
                              Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    h5_hid_t s2 = h5.dcreate2(fid, "s2", tid, sid, Hdf5Api::kH5pDefault,
                              Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    h5.ds_set_scale(s1, "s1");
    h5.ds_set_scale(s2, "s2");
    h5_hid_t did = h5.dcreate2(fid, "v", tid, sid, Hdf5Api::kH5pDefault,
                               Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    h5.ds_attach_scale(did, s1, 0);
    h5.ds_attach_scale(did, s2, 0);
    h5.dclose(did);
    h5.dclose(s2);
    h5.dclose(s1);
    h5.sclose(sid);
    h5.fclose(fid);

    harp_product* p = nullptr;
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_hdf5(f.c_str(), &p));
    EXPECT_EQ(HARP_ERROR_IMPORT, harp_errno);
  }

  // 11. 2D dimension scale dataset (expected 1D).
  {
    const TempFile f("harp_hdf5_2d_scale.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);
    const h5_hsize_t dims[2] = {2, 2};
    h5_hid_t sid = h5.screate_simple(2, dims, nullptr);
    h5_hid_t tid = Hdf5Api::GlobalId("H5T_NATIVE_DOUBLE_g");
    h5_hid_t s2d = h5.dcreate2(fid, "time", tid, sid, Hdf5Api::kH5pDefault,
                               Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    h5.ds_set_scale(s2d, "time");
    h5.dclose(s2d);
    h5.sclose(sid);
    h5.fclose(fid);

    harp_product* p = nullptr;
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_hdf5(f.c_str(), &p));
    EXPECT_EQ(HARP_ERROR_IMPORT, harp_errno);
  }

  // 12. Invalid HARP variable name ("123bad") -> harp_variable_new fails.
  {
    const TempFile f("harp_hdf5_bad_varname.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);
    const h5_hsize_t dims[1] = {1};
    h5_hid_t sid = h5.screate_simple(1, dims, nullptr);
    h5_hid_t tid = Hdf5Api::GlobalId("H5T_NATIVE_DOUBLE_g");
    h5_hid_t did = h5.dcreate2(fid, "123bad", tid, sid, Hdf5Api::kH5pDefault,
                               Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    h5.dclose(did);
    h5.sclose(sid);
    h5.fclose(fid);

    harp_product* p = nullptr;
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_hdf5(f.c_str(), &p));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // 13. Duplicate HARP variable name ("x" and "_nc4_non_coord_x") ->
  // harp_product_add_variable fails and read_variable deletes `variable`.
  {
    const TempFile f("harp_hdf5_dup_varname.h5");
    h5_hid_t fid = h5.CreateHarpFile(f.c_str());
    ASSERT_GE(fid, 0);
    const h5_hsize_t dims[1] = {1};
    h5_hid_t sid = h5.screate_simple(1, dims, nullptr);
    h5_hid_t tid = Hdf5Api::GlobalId("H5T_NATIVE_DOUBLE_g");
    h5_hid_t d1 = h5.dcreate2(fid, "x", tid, sid, Hdf5Api::kH5pDefault,
                              Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    h5_hid_t d2 =
        h5.dcreate2(fid, "_nc4_non_coord_x", tid, sid, Hdf5Api::kH5pDefault,
                    Hdf5Api::kH5pDefault, Hdf5Api::kH5pDefault);
    h5.dclose(d2);
    h5.dclose(d1);
    h5.sclose(sid);
    h5.fclose(fid);

    harp_product* p = nullptr;
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_EQ(-1, harp_import_hdf5(f.c_str(), &p));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }
}

TEST(HarpHdf5Test, FaultInjectionCoversExportAndImportErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const int prev_compression = harp_get_option_hdf5_compression();
  auto restore_compression = absl::MakeCleanup([prev_compression] {
    harp_set_option_hdf5_compression(prev_compression);
  });
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // Build a product where all used physical dimensions (`time`) have matching
  // coordinate variables (`time(time)`), avoiding write_dimension() where
  // upstream harp-hdf5.c leaves `dataset_id` uninitialized on failure.
  // Also omit `int8` enum (`flag_values`/`flag_meanings`) from export fault
  // injection because upstream write_variable() omits H5Dclose(dataset_id) on
  // flag attribute write failures (harp-hdf5.c:1918-1938).
  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);
  ASSERT_EQ(HARP_SUCCESS, harp_product_set_source_product(prod.get(), "src"));
  ASSERT_EQ(HARP_SUCCESS, harp_product_set_history(prod.get(), "hist"));

  const harp_dimension_type kTimeDim[1] = {harp_dimension_time};
  const long kTimeLen[1] = {2};
  harp_variable* dt = nullptr;
  AddVar(prod.get(), "datetime", harp_type_double, 1, kTimeDim, kTimeLen, &dt);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(dt, "days since 2000-01-01"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_description(dt, "desc"));
  dt->valid_min.double_data = 0.0;
  dt->valid_max.double_data = 100.0;
  dt->data.double_data[0] = 1.0;
  dt->data.double_data[1] = 2.0;

  harp_variable* tcoord = nullptr;
  AddVar(prod.get(), "time", harp_type_double, 1, kTimeDim, kTimeLen, &tcoord);
  tcoord->data.double_data[0] = 0.0;
  tcoord->data.double_data[1] = 1.0;

  harp_variable* sarr = nullptr;
  AddVar(prod.get(), "sarr", harp_type_string, 1, kTimeDim, kTimeLen, &sarr);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_string_data_element(sarr, 0, "x"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_string_data_element(sarr, 1, "yy"));

  harp_variable* empty_str = nullptr;
  AddVar(prod.get(), "empty_u", harp_type_int16, 0, nullptr, nullptr,
         &empty_str);
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(empty_str, ""));

  // 1. Export fault-injection sweep (compression enabled to cover
  // chunk/deflate).
  ASSERT_EQ(HARP_SUCCESS, harp_set_option_hdf5_compression(3));
  int export_idx = 0;
  constexpr const char* kExportSymbols[] = {
      "H5Pcreate",
      "H5Pset_link_creation_order",
      "H5Pset_attr_creation_order",
      "H5Fcreate",
      "H5Fclose",
      "H5Gopen1",
      "H5Screate",
      "H5Screate_simple",
      "H5Tcopy",
      "H5Tset_size",
      "H5Tset_strpad",
      "H5Acreate1",
      "H5Awrite",
      "H5Pset_chunk",
      "H5Pset_deflate",
      "H5Pset_fill_value",
      "H5Dcreate1",
      "H5Dwrite",
      "H5Dopen1",
      "H5DSset_scale",
      "H5DSis_scale",
      "H5DSattach_scale",
  };
  for (const char* sym : kExportSymbols) {
    SweepH5Faults(sym, [&] {
      const TempFile out_h5("harp_hdf5_fault_out_" +
                            std::to_string(export_idx++) + ".h5");
      return harp_export_hdf5(out_h5.c_str(), prod.get());
    });
  }
  ASSERT_EQ(HARP_SUCCESS, harp_set_option_hdf5_compression(0));

  // Sweep H5Acreate1 on a product with both a non-coord physical dim and an
  // independent dim to cover NC_DIMID_ATT_NAME write failures cleanly (since
  // write_dimension does not call H5Acreate1, dataset_id is always
  // initialized).
  {
    harp_product* raw_dim_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_dim_prod));
    ScopedHarpProduct dim_prod(raw_dim_prod);
    const harp_dimension_type kLonIndep[2] = {harp_dimension_longitude,
                                              harp_dimension_independent};
    const long kLonIndepLens[2] = {2, 3};
    AddVar(dim_prod.get(), "lon_indep", harp_type_int32, 2, kLonIndep,
           kLonIndepLens);
    SweepH5Faults("H5Acreate1", [&] {
      const TempFile out_h5("harp_hdf5_fault_dim_attr_" +
                            std::to_string(export_idx++) + ".h5");
      return harp_export_hdf5(out_h5.c_str(), dim_prod.get());
    });
  }

  // Add `qflag` enum variable for import/metadata fault-injection reference
  // file.
  harp_variable* flag = nullptr;
  AddVar(prod.get(), "qflag", harp_type_int8, 1, kTimeDim, kTimeLen, &flag);
  const char* kFlags[] = {"ok", "bad"};
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(flag, 2, kFlags));
  flag->data.int8_data[0] = 0;
  flag->data.int8_data[1] = 1;

  const TempFile ref_h5("harp_hdf5_fault_ref.h5");
  ASSERT_EQ(HARP_SUCCESS, harp_export_hdf5(ref_h5.c_str(), prod.get()));

  // 2. Import fault-injection sweep (skip H5Pget_link_creation_order which
  // leaks gcpl_id in upstream get_link_iteration_index_type).
  constexpr const char* kImportSymbols[] = {
      "H5Fopen",
      "H5Gopen1",
      "H5Gget_create_plist",
      "H5Literate1",
      "H5Oget_info_by_name1",
      "H5Oget_info1",
      "H5Dopen1",
      "H5Dget_type",
      "H5Dget_space",
      "H5Dread",
      "H5Tget_native_type",
      "H5Tcopy",
      "H5Tset_size",
      "H5Tset_strpad",
      "H5Tget_size",
      "H5Sis_simple",
      "H5Sget_simple_extent_ndims",
      "H5Sget_simple_extent_dims",
      "H5Aexists",
      "H5Aopen_by_name",
      "H5Aget_space",
      "H5Aget_type",
      "H5Aread",
      "H5DSis_scale",
      "H5DSget_scale_name",
      "H5DSget_num_scales",
      "H5DSiterate_scales",
  };
  for (const char* sym : kImportSymbols) {
    SweepH5Faults(sym, [&] {
      harp_product* p = nullptr;
      int rc = harp_import_hdf5(ref_h5.c_str(), &p);
      if (rc == 0) {
        harp_product_delete(p);
      }
      return rc;
    });
  }

  // 3. Metadata import fault-injection sweep.
  constexpr const char* kMetadataSymbols[] = {
      "H5Fopen",      "H5Gopen1",    "H5Aexists", "H5Aopen_by_name",
      "H5Aget_space", "H5Aget_type", "H5Aread",   "H5Literate1",
  };
  for (const char* sym : kMetadataSymbols) {
    SweepH5Faults(sym, [&] {
      harp_product_metadata* raw_meta = nullptr;
      if (harp_product_metadata_new(&raw_meta) != 0) {
        return -1;
      }
      ScopedHarpProductMetadata meta(raw_meta);
      return harp_import_metadata_hdf5(ref_h5.c_str(), meta.get());
    });
  }
}

}  // namespace
}  // namespace stcorp_harp
