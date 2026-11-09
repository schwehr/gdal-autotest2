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
// Tests internal attribute creation, opening, read/write conversions, table
// building, sorting, iteration, renaming, deletion, object-copy integration,
// and package lifecycle routines (third_party/hdf5/src/H5Aint.c) in Google3's
// default non-MPI Linux build (-DNDEBUG).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Aint.c

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug/stats fields from
// internal cache/object structs pulled by H5private.h -> H5CXprivate.h ->
// H5ACprivate.h -> H5Cprivate.h. Define NDEBUG around the prelude and internal
// headers so struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5A_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5A_FRIEND
#define H5F_FRIEND
#define H5O_FRIEND
#define H5S_FRIEND
#define H5T_FRIEND
#pragma push_macro("try")
#undef try
#define try h5_try_param_
#include "third_party/hdf5/src/H5Apkg.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#undef try
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5SLprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
#pragma push_macro("H5_HAVE__FLOAT16")
#undef H5_HAVE__FLOAT16
#include "third_party/hdf5/src/H5Tpkg.h"
#pragma pop_macro("H5_HAVE__FLOAT16")
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5A_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5A_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

// RAII wrapper for an internal HDF5 API context node and metadata tag.
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
      H5CX_pop(false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
  haddr_t prev_tag_ = 0;
  bool tagged_ = false;
};

struct CompoundPair {
  int32_t a;
  double b;
};

herr_t TempBkgOpaqueConv(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                         size_t nelmts, size_t buf_stride,
                         size_t /*bkg_stride*/, void* buf, void* bkg,
                         hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_TEMP;
      return 0;
    case H5T_CONV_CONV: {
      if (buf == nullptr || bkg == nullptr) {
        return -1;
      }
      auto* bytes = static_cast<uint8_t*>(buf);
      auto* scratch = static_cast<uint8_t*>(bkg);
      const size_t stride = buf_stride ? buf_stride : 4;
      for (size_t i = 0; i < nelmts; ++i) {
        scratch[i * 2] = static_cast<uint8_t>(bytes[i * stride] ^ 0x5A);
        scratch[i * 2 + 1] = 0x3C;
        bytes[i * 2] = scratch[i * 2];
        bytes[i * 2 + 1] = scratch[i * 2 + 1];
      }
      return 0;
    }
    case H5T_CONV_FREE:
      return 0;
    default:
      return -1;
  }
}

herr_t TempBkgReadOpaqueConv(hid_t /*src_id*/, hid_t /*dst_id*/,
                             H5T_cdata_t* cdata, size_t nelmts,
                             size_t /*buf_stride*/, size_t /*bkg_stride*/,
                             void* buf, void* bkg, hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_TEMP;
      return 0;
    case H5T_CONV_CONV: {
      if (buf == nullptr || bkg == nullptr) {
        return -1;
      }
      auto* bytes = static_cast<uint8_t*>(buf);
      auto* scratch = static_cast<uint8_t*>(bkg);
      for (size_t i = 0; i < nelmts; ++i) {
        scratch[i * 4] = bytes[i * 2];
        bytes[i * 4] = scratch[i * 4];
        bytes[i * 4 + 1] = 0x11;
        bytes[i * 4 + 2] = 0x22;
        bytes[i * 4 + 3] = 0x33;
      }
      return 0;
    }
    case H5T_CONV_FREE:
      return 0;
    default:
      return -1;
  }
}

herr_t FailingOpaqueConv(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                         size_t /*nelmts*/, size_t /*buf_stride*/,
                         size_t /*bkg_stride*/, void* /*buf*/, void* /*bkg*/,
                         hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      cdata->need_bkg = H5T_BKG_YES;
      return 0;
    case H5T_CONV_CONV:
      return -1;
    case H5T_CONV_FREE:
      return 0;
    default:
      return -1;
  }
}

void WriteIntAttr(hid_t loc_id, const char* name, int32_t value,
                  hid_t acpl_id = H5P_DEFAULT) {
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedAttr attr(
      H5Acreate2(loc_id, name, H5T_NATIVE_INT32, space, acpl_id, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  ASSERT_GE(H5Awrite(attr, H5T_NATIVE_INT32, &value), 0);
}

ScopedGroup CreateGroupWithAttrProps(hid_t file_id, const char* name,
                                     unsigned crt_order_flags,
                                     unsigned max_compact, unsigned min_dense) {
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  if (!gcpl.valid()) {
    return ScopedGroup();
  }
  if (H5Pset_attr_creation_order(gcpl, crt_order_flags) < 0 ||
      H5Pset_attr_phase_change(gcpl, max_compact, min_dense) < 0) {
    return ScopedGroup();
  }
  return ScopedGroup(H5Gcreate2(file_id, name, H5P_DEFAULT, gcpl, H5P_DEFAULT));
}

herr_t CollectNamesOp2(hid_t /*loc_id*/, const char* attr_name,
                       const H5A_info_t* /*ainfo*/, void* op_data) {
  auto* names = static_cast<std::vector<std::string>*>(op_data);
  names->emplace_back(attr_name);
  return 0;
}

herr_t StopAfterTwoOp2(hid_t /*loc_id*/, const char* attr_name,
                       const H5A_info_t* /*ainfo*/, void* op_data) {
  auto* names = static_cast<std::vector<std::string>*>(op_data);
  names->emplace_back(attr_name);
  return names->size() >= 2 ? 1 : 0;
}

herr_t FailingOp2(hid_t /*loc_id*/, const char* /*attr_name*/,
                  const H5A_info_t* /*ainfo*/, void* /*op_data*/) {
  return -1;
}

herr_t CollectNamesOp1(hid_t /*loc_id*/, const char* attr_name, void* op_data) {
  auto* names = static_cast<std::vector<std::string>*>(op_data);
  names->emplace_back(attr_name);
  return names->size() >= 2 ? 1 : 0;
}

herr_t FailingOp1(hid_t /*loc_id*/, const char* /*attr_name*/,
                  void* /*op_data*/) {
  return -1;
}

std::vector<std::string> CollectAttrNames(hid_t loc_id, H5_index_t idx_type,
                                          H5_iter_order_t order) {
  std::vector<std::string> names;
  hsize_t idx = 0;
  EXPECT_GE(H5Aiterate2(loc_id, idx_type, order, &idx, CollectNamesOp2, &names),
            0);
  return names;
}

std::vector<uint8_t> FlushToMemoryImage(hid_t file_id) {
  EXPECT_GE(H5Fflush(file_id, H5F_SCOPE_GLOBAL), 0);
  const ssize_t image_size = H5Fget_file_image(file_id, nullptr, 0);
  if (image_size <= 0) {
    return {};
  }
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  EXPECT_EQ(H5Fget_file_image(file_id, image.data(), image.size()), image_size);
  return image;
}

ScopedFile OpenMemoryImage(std::vector<uint8_t>& image,
                           unsigned flags = H5F_ACC_RDWR,
                           const char* name = "reopen_in_mem.h5") {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_file_image(fapl, image.data(), image.size()) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fopen(name, flags, fapl));
}

TEST(H5AintTest, PackageLifecycleReinitAndOpenAttrCloseCallback) {
  absl::Cleanup ensure_open = [] { H5open(); };
  // 1. Cycle through shutdown when no attribute handles remain (covers
  // H5A_term_package + H5A__init_package on reopen).
  ASSERT_GE(H5close(), 0);
  ASSERT_GE(H5open(), 0);

  // 2. Leave an open H5I_ATTR handle across H5close() so H5A_top_term_package
  // calls H5I_clear_type -> H5A__close_cb.
  ScopedFile file = CreateInMemoryFile("lifecycle.h5");
  ASSERT_TRUE(file.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedAttr attr(H5Acreate2(file, "dangling_at_close", H5T_NATIVE_INT32, space,
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());

  (void)attr.release();
  (void)space.release();
  (void)file.release();
  ASSERT_GE(H5close(), 0);
  ASSERT_GE(H5open(), 0);

  ScopedApiContext ctx;
  EXPECT_GE(H5A_init(), 0);
}

TEST(H5AintTest, CreateAndCreateByNameErrorAndValidationPaths) {
  ScopedFile file = CreateInMemoryFile("create_errs.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file, "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  ScopedAttr existing(H5Acreate2(grp, "dup", H5T_NATIVE_INT32, scalar,
                                 H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(existing.valid());

  {
    ScopedErrorSilence silence;
    // Duplicate attribute name.
    EXPECT_EQ(H5Acreate2(grp, "dup", H5T_NATIVE_INT32, scalar, H5P_DEFAULT,
                         H5P_DEFAULT),
              H5I_INVALID_HID);

    // Dataspace with unset extent (H5S_SIMPLE).
    ScopedSpace no_extent(H5Screate(H5S_SIMPLE));
    ASSERT_TRUE(no_extent.valid());
    EXPECT_EQ(H5Acreate2(grp, "no_ext", H5T_NATIVE_INT32, no_extent,
                         H5P_DEFAULT, H5P_DEFAULT),
              H5I_INVALID_HID);

    // Non-sensible datatypes (empty compound / empty enum).
    ScopedType empty_compound(H5Tcreate(H5T_COMPOUND, 8));
    ScopedType empty_enum(H5Tcreate(H5T_ENUM, 4));
    ASSERT_TRUE(empty_compound.valid());
    ASSERT_TRUE(empty_enum.valid());
    EXPECT_EQ(H5Acreate2(grp, "bad_comp", empty_compound, scalar, H5P_DEFAULT,
                         H5P_DEFAULT),
              H5I_INVALID_HID);
    EXPECT_EQ(H5Acreate2(grp, "bad_enum", empty_enum, scalar, H5P_DEFAULT,
                         H5P_DEFAULT),
              H5I_INVALID_HID);

    // Invalid ACPL ID and wrong plist class passed to H5Acreate2.
    EXPECT_EQ(H5Acreate2(grp, "bad_acpl_id", H5T_NATIVE_INT32, scalar,
                         H5I_INVALID_HID, H5P_DEFAULT),
              H5I_INVALID_HID);
    ScopedPlist fapl_wrong_cls(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl_wrong_cls.valid());
    EXPECT_EQ(H5Acreate2(grp, "wrong_plist", H5T_NATIVE_INT32, scalar,
                         fapl_wrong_cls, H5P_DEFAULT),
              H5I_INVALID_HID);

    // Unusual unused numeric bits fail H5O__attr_create on v1 (non-checksummed)
    // object headers and roll back the attribute cleanly.
    ScopedPlist fapl_v1 = MakeCoreFapl();
    ASSERT_TRUE(fapl_v1.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl_v1, H5F_LIBVER_EARLIEST, H5F_LIBVER_V18), 0);
    ScopedFile v1_file(
        H5Fcreate("v1_unusual.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v1));
    ASSERT_TRUE(v1_file.valid());
    ScopedGroup v1_grp(
        H5Gcreate2(v1_file, "/v1_grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(v1_grp.valid());
    ScopedType unusual_int(H5Tcopy(H5T_STD_I32LE));
    ASSERT_TRUE(unusual_int.valid());
    ASSERT_GE(H5Tset_precision(unusual_int, 8), 0);
    EXPECT_EQ(H5Acreate2(v1_grp, "unusual_bits", unusual_int, scalar,
                         H5P_DEFAULT, H5P_DEFAULT),
              H5I_INVALID_HID);

    // H5Acreate_by_name missing object vs H5A__create failure on valid object.
    EXPECT_EQ(H5Acreate_by_name(file, "/missing_obj", "a", H5T_NATIVE_INT32,
                                scalar, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              H5I_INVALID_HID);
    EXPECT_EQ(H5Acreate_by_name(file, "/grp", "dup", H5T_NATIVE_INT32, scalar,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              H5I_INVALID_HID);
  }

  // Create attributes by name on dataset and committed datatype.
  ScopedDataset dset(H5Dcreate2(file, "/dset", H5T_NATIVE_INT32, scalar,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedAttr dset_attr(H5Acreate_by_name(file, "/dset", "by_name_dset",
                                         H5T_NATIVE_INT32, scalar, H5P_DEFAULT,
                                         H5P_DEFAULT, H5P_DEFAULT));
  EXPECT_TRUE(dset_attr.valid());
}

TEST(H5AintTest, CommittedDatatypesSohmSharingAndLibverVersionSelection) {
  // 1. Committed datatype shared within same file & converted across files.
  ScopedFile f_src = CreateInMemoryFile("comm_src.h5");
  ScopedFile f_other = CreateInMemoryFile("comm_other.h5");
  ASSERT_TRUE(f_src.valid());
  ASSERT_TRUE(f_other.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedType comm_dt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(comm_dt.valid());
  ASSERT_GE(
      H5Tcommit2(f_src, "/my_i32", comm_dt, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
      0);

  ScopedAttr same_file_attr(H5Acreate2(f_src, "same_comm", comm_dt, scalar,
                                       H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(same_file_attr.valid());
  constexpr int32_t kCommVal = 55;
  ASSERT_GE(H5Awrite(same_file_attr, comm_dt, &kCommVal), 0);

  ScopedType reopened_same_dt(H5Aget_type(same_file_attr));
  ASSERT_TRUE(reopened_same_dt.valid());
  EXPECT_GT(H5Tcommitted(reopened_same_dt), 0);

  ScopedAttr cross_file_attr(H5Acreate2(f_other, "cross_comm", comm_dt, scalar,
                                        H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(cross_file_attr.valid());
  ScopedType reopened_cross_dt(H5Aget_type(cross_file_attr));
  ASSERT_TRUE(reopened_cross_dt.valid());
  EXPECT_EQ(H5Tcommitted(reopened_cross_dt), 0);

  // 2. SOHM-enabled file sharing datatype and dataspace (hits H5O_ATTR_VERSION_2).
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl, 1), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(
                fcpl, 0, H5O_SHMESG_DTYPE_FLAG | H5O_SHMESG_SDSPACE_FLAG, 1),
            0);
  ScopedFile f_sohm = CreateInMemoryFile("sohm_parts.h5", H5F_ACC_TRUNC, fcpl);
  ASSERT_TRUE(f_sohm.valid());

  ScopedType comp_dt(H5Tcreate(H5T_COMPOUND, sizeof(CompoundPair)));
  ASSERT_TRUE(comp_dt.valid());
  ASSERT_GE(H5Tinsert(comp_dt, "a", HOFFSET(CompoundPair, a), H5T_NATIVE_INT32),
            0);
  ASSERT_GE(
      H5Tinsert(comp_dt, "b", HOFFSET(CompoundPair, b), H5T_NATIVE_DOUBLE), 0);
  constexpr hsize_t kDim = 2;
  ScopedSpace vec_space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(vec_space.valid());

  ScopedAttr sohm_a1(H5Acreate2(f_sohm, "sohm_1", comp_dt, vec_space,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sohm_a1.valid());
  ScopedAttr sohm_a2(H5Acreate2(f_sohm, "sohm_2", comp_dt, vec_space,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sohm_a2.valid());

  // 3. Libver bounds: EARLIEST encodes ASCII attrs as v1 (or v3 when UTF-8),
  // whereas V18 low bound upgrades ASCII attrs to v3.
  ScopedPlist fapl_earliest = MakeCoreFapl();
  ASSERT_TRUE(fapl_earliest.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl_earliest, H5F_LIBVER_EARLIEST,
                                 H5F_LIBVER_V18),
            0);
  ScopedFile f_early(H5Fcreate("earliest.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                               fapl_earliest));
  ASSERT_TRUE(f_early.valid());

  ScopedAttr v1_attr(H5Acreate2(f_early, "ascii_v1", H5T_NATIVE_INT32, scalar,
                                H5P_DEFAULT, H5P_DEFAULT));
  EXPECT_TRUE(v1_attr.valid());

  ScopedPlist acpl_utf8(H5Pcreate(H5P_ATTRIBUTE_CREATE));
  ASSERT_TRUE(acpl_utf8.valid());
  ASSERT_GE(H5Pset_char_encoding(acpl_utf8, H5T_CSET_UTF8), 0);
  ScopedAttr utf8_v3_attr(H5Acreate2(f_early, "utf8_v3", H5T_NATIVE_INT32,
                                     scalar, acpl_utf8, H5P_DEFAULT));
  EXPECT_TRUE(utf8_v3_attr.valid());

  ScopedPlist fapl_v18 = MakeCoreFapl();
  ASSERT_TRUE(fapl_v18.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl_v18, H5F_LIBVER_V18, H5F_LIBVER_V18), 0);
  ScopedFile f_v18(H5Fcreate("v18.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v18));
  ASSERT_TRUE(f_v18.valid());
  ScopedAttr v18_attr(H5Acreate2(f_v18, "ascii_upgraded", H5T_NATIVE_INT32,
                                 scalar, H5P_DEFAULT, H5P_DEFAULT));
  EXPECT_TRUE(v18_attr.valid());
}

TEST(H5AintTest, OpenReadWriteConversionsAndMetadataQueries) {
  ScopedFile file = CreateInMemoryFile("rw_meta.h5");
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file, "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(null_space.valid());

  // 1. Open error paths.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Aopen(grp, "missing", H5P_DEFAULT), H5I_INVALID_HID);
    EXPECT_EQ(H5Aopen_by_name(file, "/no_obj", "a", H5P_DEFAULT, H5P_DEFAULT),
              H5I_INVALID_HID);
    EXPECT_EQ(H5Aopen_by_name(file, "/grp", "missing", H5P_DEFAULT, H5P_DEFAULT),
              H5I_INVALID_HID);
    EXPECT_EQ(H5Aopen_by_idx(file, "/no_obj", H5_INDEX_NAME, H5_ITER_INC, 0,
                             H5P_DEFAULT, H5P_DEFAULT),
              H5I_INVALID_HID);
    EXPECT_EQ(H5Aopen_by_idx(file, "/grp", H5_INDEX_NAME, H5_ITER_INC, 5,
                             H5P_DEFAULT, H5P_DEFAULT),
              H5I_INVALID_HID);
  }

  // 2. Unwritten attribute reads back as zeroed memory.
  ScopedAttr unwritten(H5Acreate2(grp, "unwritten_long_name", H5T_NATIVE_INT32,
                                  scalar, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(unwritten.valid());
  uint8_t out_bytes[sizeof(int32_t)];
  std::memset(out_bytes, 0xA5, sizeof(out_bytes));
  ASSERT_GE(H5Aread(unwritten, H5T_NATIVE_INT32, out_bytes), 0);
  EXPECT_THAT(out_bytes, ElementsAre(0, 0, 0, 0));

  // Multiple open handles share H5A_shared_t (nrefs > 1 -> decrements on close).
  {
    ScopedAttr second_open(H5Aopen(grp, "unwritten_long_name", H5P_DEFAULT));
    ASSERT_TRUE(second_open.valid());
  }

  // 3. Name truncation, null buffer, copy_len == 0, space, type, plist, info.
  char zero_buf[4] = {'x', 'x', 'x', 'x'};
  EXPECT_EQ(H5Aget_name(unwritten, 0, nullptr), 19);
  EXPECT_EQ(H5Aget_name(unwritten, 1, zero_buf), 19);
  std::array<char, 5> trunc_buf{};
  EXPECT_EQ(H5Aget_name(unwritten, trunc_buf.size(), trunc_buf.data()), 19);
  EXPECT_STREQ(trunc_buf.data(), "unwr");
  std::array<char, 32> full_buf{};
  EXPECT_EQ(H5Aget_name(unwritten, full_buf.size(), full_buf.data()), 19);
  EXPECT_STREQ(full_buf.data(), "unwritten_long_name");

  ScopedSpace got_space(H5Aget_space(unwritten));
  EXPECT_TRUE(got_space.valid());
  ScopedType got_type(H5Aget_type(unwritten));
  EXPECT_TRUE(got_type.valid());
  ScopedPlist got_acpl(H5Aget_create_plist(unwritten));
  ASSERT_TRUE(got_acpl.valid());
  H5T_cset_t cset = H5T_CSET_ERROR;
  ASSERT_GE(H5Pget_char_encoding(got_acpl, &cset), 0);
  EXPECT_EQ(cset, H5T_CSET_ASCII);

  H5A_info_t info{};
  ASSERT_GE(H5Aget_info(unwritten, &info), 0);
  EXPECT_FALSE(info.corder_valid);
  EXPECT_EQ(info.corder, 0U);
  EXPECT_EQ(info.cset, H5T_CSET_ASCII);
  EXPECT_EQ(info.data_size, sizeof(int32_t));

  // UTF-8 encoding plist round-trip.
  ScopedPlist utf8_acpl(H5Pcreate(H5P_ATTRIBUTE_CREATE));
  ASSERT_TRUE(utf8_acpl.valid());
  ASSERT_GE(H5Pset_char_encoding(utf8_acpl, H5T_CSET_UTF8), 0);
  ScopedAttr utf8_attr(H5Acreate2(grp, "utf8_name", H5T_NATIVE_INT32, scalar,
                                  utf8_acpl, H5P_DEFAULT));
  ASSERT_TRUE(utf8_attr.valid());
  ScopedPlist utf8_got(H5Aget_create_plist(utf8_attr));
  ASSERT_TRUE(utf8_got.valid());
  ASSERT_GE(H5Pget_char_encoding(utf8_got, &cset), 0);
  EXPECT_EQ(cset, H5T_CSET_UTF8);

  // 4. H5S_NULL dataspace read/write (nelmts == 0).
  ScopedAttr null_attr(H5Acreate2(grp, "null_attr", H5T_NATIVE_INT32,
                                  null_space, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(null_attr.valid());
  int32_t dummy = 123;
  EXPECT_GE(H5Awrite(null_attr, H5T_NATIVE_INT32, &dummy), 0);
  EXPECT_GE(H5Aread(null_attr, H5T_NATIVE_INT32, &dummy), 0);

  // 5. Noop writes x2 and unconvertible types.
  constexpr int32_t kFirst = 10;
  constexpr int32_t kSecond = 25;
  ASSERT_GE(H5Awrite(unwritten, H5T_NATIVE_INT32, &kFirst), 0);
  ASSERT_GE(H5Awrite(unwritten, H5T_NATIVE_INT32, &kSecond), 0);
  int32_t read_back = 0;
  ASSERT_GE(H5Aread(unwritten, H5T_NATIVE_INT32, &read_back), 0);
  EXPECT_EQ(read_back, kSecond);

  {
    ScopedErrorSilence silence;
    ScopedType opaque_t(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opaque_t.valid());
    char opaque_buf[4] = {};
    EXPECT_LT(H5Awrite(unwritten, opaque_t, opaque_buf), 0);
    EXPECT_LT(H5Aread(unwritten, opaque_t, opaque_buf), 0);
  }

  // 6. Non-noop conversion (double <-> BE int32) written twice and read back.
  constexpr hsize_t kTwo = 2;
  ScopedSpace vec2(H5Screate_simple(1, &kTwo, nullptr));
  ASSERT_TRUE(vec2.valid());
  ScopedAttr conv_attr(H5Acreate2(grp, "conv_be", H5T_STD_I32BE, vec2,
                                  H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(conv_attr.valid());
  const std::array<double, 2> d_vals1 = {3.0, -7.0};
  const std::array<double, 2> d_vals2 = {11.0, 42.0};
  ASSERT_GE(H5Awrite(conv_attr, H5T_NATIVE_DOUBLE, d_vals1.data()), 0);
  ASSERT_GE(H5Awrite(conv_attr, H5T_NATIVE_DOUBLE, d_vals2.data()), 0);
  std::array<double, 2> d_out{};
  ASSERT_GE(H5Aread(conv_attr, H5T_NATIVE_DOUBLE, d_out.data()), 0);
  EXPECT_THAT(d_out, ElementsAre(11.0, 42.0));

  // 7. Compound partial-field read (need_bkg = H5T_BKG_YES preserves a).
  ScopedType comp_disk(H5Tcreate(H5T_COMPOUND, sizeof(CompoundPair)));
  ASSERT_TRUE(comp_disk.valid());
  ASSERT_GE(
      H5Tinsert(comp_disk, "a", HOFFSET(CompoundPair, a), H5T_NATIVE_INT32), 0);
  ASSERT_GE(
      H5Tinsert(comp_disk, "b", HOFFSET(CompoundPair, b), H5T_NATIVE_DOUBLE),
      0);
  ScopedAttr comp_attr(H5Acreate2(grp, "compound", comp_disk, scalar,
                                  H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(comp_attr.valid());
  const CompoundPair disk_pair{100, 9.5};
  ASSERT_GE(H5Awrite(comp_attr, comp_disk, &disk_pair), 0);

  ScopedType comp_mem_partial(H5Tcreate(H5T_COMPOUND, sizeof(CompoundPair)));
  ASSERT_TRUE(comp_mem_partial.valid());
  ASSERT_GE(H5Tinsert(comp_mem_partial, "b", HOFFSET(CompoundPair, b),
                      H5T_NATIVE_DOUBLE),
            0);
  CompoundPair mem_pair{777, 0.0};
  ASSERT_GE(H5Aread(comp_attr, comp_mem_partial, &mem_pair), 0);
  EXPECT_EQ(mem_pair.a, 777);
  EXPECT_DOUBLE_EQ(mem_pair.b, 9.5);

  // 8. VLEN attribute written twice (need_bkg = H5T_BKG_YES on VLEN write).
  ScopedType vl_i32(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vl_i32.valid());
  ScopedAttr vl_attr(
      H5Acreate2(grp, "vlen", vl_i32, scalar, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(vl_attr.valid());
  std::array<int32_t, 2> vdata1 = {1, 2};
  std::array<int32_t, 3> vdata2 = {4, 5, 6};
  hvl_t w1{vdata1.size(), vdata1.data()};
  hvl_t w2{vdata2.size(), vdata2.data()};
  ASSERT_GE(H5Awrite(vl_attr, vl_i32, &w1), 0);
  ASSERT_GE(H5Awrite(vl_attr, vl_i32, &w2), 0);
  hvl_t vl_out{0, nullptr};
  absl::Cleanup reclaim_vl = [&] { H5Treclaim(vl_i32, scalar, H5P_DEFAULT, &vl_out); };
  ASSERT_GE(H5Aread(vl_attr, vl_i32, &vl_out), 0);
  ASSERT_EQ(vl_out.len, 3U);
  auto* vl_ints = static_cast<int32_t*>(vl_out.p);
  EXPECT_EQ(vl_ints[0], 4);
  EXPECT_EQ(vl_ints[1], 5);
  EXPECT_EQ(vl_ints[2], 6);

  // 9. Custom registered conversion with H5T_BKG_TEMP (written twice + read)
  // and custom failing conversion during H5Awrite + H5Aread.
  ScopedType src_tmp(H5Tcreate(H5T_OPAQUE, 4));
  ScopedType dst_tmp(H5Tcreate(H5T_OPAQUE, 2));
  ASSERT_TRUE(src_tmp.valid());
  ASSERT_TRUE(dst_tmp.valid());
  ASSERT_GE(H5Tset_tag(src_tmp, "h5aint_src_tmp"), 0);
  ASSERT_GE(H5Tset_tag(dst_tmp, "h5aint_dst_tmp"), 0);

  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "h5aint_bkg_temp_w", src_tmp, dst_tmp,
                        TempBkgOpaqueConv),
            0);
  absl::Cleanup unreg_w = [&] {
    H5Tunregister(H5T_PERS_HARD, "h5aint_bkg_temp_w", src_tmp, dst_tmp,
                  TempBkgOpaqueConv);
  };
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "h5aint_bkg_temp_r", dst_tmp, src_tmp,
                        TempBkgReadOpaqueConv),
            0);
  absl::Cleanup unreg_r = [&] {
    H5Tunregister(H5T_PERS_HARD, "h5aint_bkg_temp_r", dst_tmp, src_tmp,
                  TempBkgReadOpaqueConv);
  };

  ScopedAttr bkg_tmp_attr(H5Acreate2(grp, "bkg_temp", dst_tmp, scalar,
                                     H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(bkg_tmp_attr.valid());
  const std::array<uint8_t, 4> in1 = {0x01, 0x02, 0x03, 0x04};
  const std::array<uint8_t, 4> in2 = {0x10, 0x20, 0x30, 0x40};
  ASSERT_GE(H5Awrite(bkg_tmp_attr, src_tmp, in1.data()), 0);
  ASSERT_GE(H5Awrite(bkg_tmp_attr, src_tmp, in2.data()), 0);
  std::array<uint8_t, 4> tmp_read{};
  ASSERT_GE(H5Aread(bkg_tmp_attr, src_tmp, tmp_read.data()), 0);
  EXPECT_EQ(tmp_read[0], 0x10 ^ 0x5A);
  EXPECT_EQ(tmp_read[1], 0x11);

  ScopedType fail_src(H5Tcreate(H5T_OPAQUE, 3));
  ScopedType fail_dst(H5Tcreate(H5T_OPAQUE, 5));
  ASSERT_TRUE(fail_src.valid());
  ASSERT_TRUE(fail_dst.valid());
  ASSERT_GE(H5Tset_tag(fail_src, "h5aint_fail_src"), 0);
  ASSERT_GE(H5Tset_tag(fail_dst, "h5aint_fail_dst"), 0);
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "h5aint_fail_w", fail_src, fail_dst,
                        FailingOpaqueConv),
            0);
  absl::Cleanup unreg_fw = [&] {
    H5Tunregister(H5T_PERS_HARD, "h5aint_fail_w", fail_src, fail_dst,
                  FailingOpaqueConv);
  };
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "h5aint_fail_r", fail_dst, fail_src,
                        FailingOpaqueConv),
            0);
  absl::Cleanup unreg_fr = [&] {
    H5Tunregister(H5T_PERS_HARD, "h5aint_fail_r", fail_dst, fail_src,
                  FailingOpaqueConv);
  };

  ScopedAttr fail_attr(H5Acreate2(grp, "conv_fail", fail_dst, scalar,
                                  H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(fail_attr.valid());
  const std::array<uint8_t, 5> valid_bytes = {1, 2, 3, 4, 5};
  ASSERT_GE(H5Awrite(fail_attr, fail_dst, valid_bytes.data()), 0);
  {
    ScopedErrorSilence silence;
    std::array<uint8_t, 3> bad_buf = {9, 8, 7};
    // Read while attr->shared->data is populated -> invokes failing converter.
    EXPECT_LT(H5Aread(fail_attr, fail_src, bad_buf.data()), 0);
    // Write failure with H5T_BKG_YES releases/nulls attr->shared->data on error.
    EXPECT_LT(H5Awrite(fail_attr, fail_src, bad_buf.data()), 0);
  }
}

TEST(H5AintTest, CompactAndDenseTablesSortingIterationRenameAndDelete) {
  ScopedFile file = CreateInMemoryFile("tables_iter.h5");
  ASSERT_TRUE(file.valid());

  // 1. Empty object table & no-corder compact table doubling across 5 attrs.
  ScopedGroup empty_grp(
      H5Gcreate2(file, "/empty", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(empty_grp.valid());
  EXPECT_THAT(CollectAttrNames(empty_grp, H5_INDEX_NAME, H5_ITER_INC),
              IsEmpty());

  ScopedGroup no_corder_grp = CreateGroupWithAttrProps(
      file, "/compact_no_corder", /*crt_order_flags=*/0, /*max_compact=*/8,
      /*min_dense=*/6);
  ASSERT_TRUE(no_corder_grp.valid());
  WriteIntAttr(no_corder_grp, "c", 1);
  WriteIntAttr(no_corder_grp, "a", 2);
  WriteIntAttr(no_corder_grp, "e", 3);
  WriteIntAttr(no_corder_grp, "b", 4);
  WriteIntAttr(no_corder_grp, "d", 5);

  EXPECT_THAT(CollectAttrNames(no_corder_grp, H5_INDEX_NAME, H5_ITER_INC),
              ElementsAre("a", "b", "c", "d", "e"));
  EXPECT_THAT(CollectAttrNames(no_corder_grp, H5_INDEX_NAME, H5_ITER_DEC),
              ElementsAre("e", "d", "c", "b", "a"));
  EXPECT_THAT(CollectAttrNames(no_corder_grp, H5_INDEX_NAME, H5_ITER_NATIVE),
              ElementsAre("c", "a", "e", "b", "d"));

  // 2. Compact group WITH creation order tracking (not dense).
  ScopedGroup corder_grp = CreateGroupWithAttrProps(
      file, "/compact_corder", H5P_CRT_ORDER_TRACKED, /*max_compact=*/8,
      /*min_dense=*/6);
  ASSERT_TRUE(corder_grp.valid());
  WriteIntAttr(corder_grp, "z", 10);
  WriteIntAttr(corder_grp, "a", 20);
  WriteIntAttr(corder_grp, "m", 30);

  ScopedAttr corder_attr(H5Aopen(corder_grp, "m", H5P_DEFAULT));
  ASSERT_TRUE(corder_attr.valid());
  H5A_info_t info{};
  ASSERT_GE(H5Aget_info(corder_attr, &info), 0);
  EXPECT_TRUE(info.corder_valid);
  EXPECT_EQ(info.corder, 2U);

  EXPECT_THAT(CollectAttrNames(corder_grp, H5_INDEX_CRT_ORDER, H5_ITER_INC),
              ElementsAre("z", "a", "m"));
  EXPECT_THAT(CollectAttrNames(corder_grp, H5_INDEX_CRT_ORDER, H5_ITER_DEC),
              ElementsAre("m", "a", "z"));
  EXPECT_THAT(CollectAttrNames(corder_grp, H5_INDEX_CRT_ORDER, H5_ITER_NATIVE),
              ElementsAre("z", "a", "m"));

  // 3. Callback short-circuit, idx == nullptr, resume, errors, H5Aiterate_by_name,
  // and deprecated H5Aiterate1 (covers H5A_ATTR_OP_APP / H5A__iterate_old).
  std::vector<std::string> partial;
  hsize_t idx = 0;
  EXPECT_EQ(H5Aiterate2(no_corder_grp, H5_INDEX_NAME, H5_ITER_INC, &idx,
                        StopAfterTwoOp2, &partial),
            1);
  EXPECT_THAT(partial, ElementsAre("a", "b"));
  EXPECT_EQ(idx, 2U);

  partial.clear();
  EXPECT_GE(H5Aiterate2(no_corder_grp, H5_INDEX_NAME, H5_ITER_INC, &idx,
                        CollectNamesOp2, &partial),
            0);
  EXPECT_THAT(partial, ElementsAre("c", "d", "e"));

  partial.clear();
  EXPECT_EQ(H5Aiterate2(no_corder_grp, H5_INDEX_NAME, H5_ITER_INC, nullptr,
                        StopAfterTwoOp2, &partial),
            1);
  EXPECT_THAT(partial, ElementsAre("a", "b"));

  partial.clear();
  EXPECT_GE(H5Aiterate_by_name(file, "/compact_corder", H5_INDEX_NAME,
                               H5_ITER_INC, nullptr, CollectNamesOp2, &partial,
                               H5P_DEFAULT),
            0);
  EXPECT_THAT(partial, ElementsAre("a", "m", "z"));

  // Deprecated H5Aiterate1: null attr_num, non-null attr_num stop & error.
  std::vector<std::string> old_names;
  unsigned old_idx = 0;
  EXPECT_EQ(H5Aiterate1(corder_grp, &old_idx, CollectNamesOp1, &old_names), 1);
  EXPECT_THAT(old_names, ElementsAre("z", "a"));
  EXPECT_EQ(old_idx, 2U);

  old_names.clear();
  EXPECT_GE(H5Aiterate1(corder_grp, nullptr, CollectNamesOp1, &old_names), 0);
  EXPECT_THAT(old_names, ElementsAre("z", "a"));

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Aiterate2(no_corder_grp, H5_INDEX_NAME, H5_ITER_INC, &idx,
                          FailingOp2, nullptr),
              0);
    EXPECT_LT(H5Aiterate1(corder_grp, &old_idx, FailingOp1, nullptr), 0);
    EXPECT_LT(H5Aiterate_by_name(file, "/no_such", H5_INDEX_NAME, H5_ITER_INC,
                                 nullptr, CollectNamesOp2, &partial,
                                 H5P_DEFAULT),
              0);
    EXPECT_LT(H5Aiterate_by_name(file, "/compact_corder", H5_INDEX_NAME,
                                 H5_ITER_INC, nullptr, FailingOp2, nullptr,
                                 H5P_DEFAULT),
              0);
  }

  // 4. Dense storage with corder tracked (NOT indexed) -> hits
  // H5A__dense_build_table + H5A__dense_build_table_cb + dense delete/open by
  // H5_INDEX_CRT_ORDER, then shrink below min_dense.
  ScopedGroup dense_grp = CreateGroupWithAttrProps(
      file, "/dense_tracked", H5P_CRT_ORDER_TRACKED, /*max_compact=*/2,
      /*min_dense=*/1);
  ASSERT_TRUE(dense_grp.valid());
  WriteIntAttr(dense_grp, "d_first", 1);
  WriteIntAttr(dense_grp, "b_second", 2);
  WriteIntAttr(dense_grp, "c_third", 3);
  WriteIntAttr(dense_grp, "a_fourth", 4);

  {
    ScopedAttr by_corder(H5Aopen_by_idx(file, "/dense_tracked",
                                        H5_INDEX_CRT_ORDER, H5_ITER_INC, 2,
                                        H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(by_corder.valid());
    std::array<char, 32> name_buf{};
    ASSERT_GT(H5Aget_name(by_corder, name_buf.size(), name_buf.data()), 0);
    EXPECT_STREQ(name_buf.data(), "c_third");
  }
  ASSERT_GE(H5Adelete_by_idx(file, "/dense_tracked", H5_INDEX_CRT_ORDER,
                             H5_ITER_DEC, 0, H5P_DEFAULT),
            0);
  EXPECT_THAT(CollectAttrNames(dense_grp, H5_INDEX_CRT_ORDER, H5_ITER_INC),
              ElementsAre("d_first", "b_second", "c_third"));

  // Rename, exists, delete by name, and negative cases.
  htri_t ex = H5Aexists_by_name(file, "/compact_corder", "a", H5P_DEFAULT);
  EXPECT_GT(ex, 0);
  ex = H5Aexists_by_name(file, "/compact_corder", "missing", H5P_DEFAULT);
  EXPECT_EQ(ex, 0);

  ASSERT_GE(H5Arename_by_name(file, "/compact_corder", "a", "a_renamed",
                              H5P_DEFAULT),
            0);
  EXPECT_GT(
      H5Aexists_by_name(file, "/compact_corder", "a_renamed", H5P_DEFAULT), 0);
  ASSERT_GE(
      H5Adelete_by_name(file, "/compact_corder", "a_renamed", H5P_DEFAULT), 0);
  EXPECT_EQ(
      H5Aexists_by_name(file, "/compact_corder", "a_renamed", H5P_DEFAULT), 0);

  // Shrink dense_grp down to 0 attributes (dense -> compact transition & empty).
  ASSERT_GE(H5Adelete_by_name(file, "/dense_tracked", "d_first", H5P_DEFAULT),
            0);
  ASSERT_GE(H5Adelete_by_name(file, "/dense_tracked", "b_second", H5P_DEFAULT),
            0);
  ASSERT_GE(H5Adelete_by_name(file, "/dense_tracked", "c_third", H5P_DEFAULT),
            0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Aexists_by_name(file, "/no_obj", "a", H5P_DEFAULT), 0);
    EXPECT_LT(
        H5Arename_by_name(file, "/no_obj", "a", "b", H5P_DEFAULT), 0);
    EXPECT_LT(
        H5Arename_by_name(file, "/compact_corder", "no_attr", "b", H5P_DEFAULT),
        0);
    EXPECT_LT(H5Adelete_by_name(file, "/no_obj", "a", H5P_DEFAULT), 0);
    EXPECT_LT(
        H5Adelete_by_name(file, "/compact_corder", "no_attr", H5P_DEFAULT), 0);
    EXPECT_LT(H5Adelete_by_idx(file, "/no_obj", H5_INDEX_NAME, H5_ITER_INC, 0,
                               H5P_DEFAULT),
              0);
    EXPECT_LT(H5Adelete_by_idx(file, "/compact_corder", H5_INDEX_NAME,
                               H5_ITER_INC, 99, H5P_DEFAULT),
              0);
  }
}

TEST(H5AintTest, ObjectCopyCopiesCompactAndDenseComplexAttributes) {
  // Build source file with earliest-to-latest compatible SOHM or standard layout,
  // plus copy to a SOHM destination and non-SOHM destinations with and without
  // H5O_COPY_EXPAND_REFERENCE_FLAG & H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG.
  ScopedFile src = CreateInMemoryFile("ocopy_src.h5");
  ASSERT_TRUE(src.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedDataset target_dset(H5Dcreate2(src, "/ref_target", H5T_NATIVE_INT32,
                                       scalar, H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT));
  ASSERT_TRUE(target_dset.valid());
  constexpr int32_t kTargetData = 4242;
  ASSERT_GE(H5Dwrite(target_dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, &kTargetData),
            0);

  ScopedType comm_dt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(comm_dt.valid());
  ASSERT_GE(
      H5Tcommit2(src, "/shared_i32", comm_dt, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
      0);

  ScopedType vl_i32(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vl_i32.valid());

  H5R_ref_t obj_ref{};
  ASSERT_GE(H5Rcreate_object(src, "/ref_target", H5P_DEFAULT, &obj_ref), 0);
  absl::Cleanup destroy_ref = [&] { H5Rdestroy(&obj_ref); };

  auto populate_attrs = [&](hid_t loc_id, bool extra_for_dense) {
    // Unwritten attribute (attr_src->shared->data == NULL).
    ScopedAttr a_unwritten(H5Acreate2(loc_id, "01_unwritten", H5T_NATIVE_INT32,
                                      scalar, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a_unwritten.valid());

    // Committed datatype attribute.
    ScopedAttr a_comm(H5Acreate2(loc_id, "02_comm", comm_dt, scalar,
                                 H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a_comm.valid());
    constexpr int32_t kVal = 77;
    ASSERT_GE(H5Awrite(a_comm, comm_dt, &kVal), 0);

    // VLEN attribute.
    ScopedAttr a_vlen(H5Acreate2(loc_id, "03_vlen", vl_i32, scalar, H5P_DEFAULT,
                                 H5P_DEFAULT));
    ASSERT_TRUE(a_vlen.valid());
    std::array<int32_t, 3> elems = {10, 20, 30};
    hvl_t v{elems.size(), elems.data()};
    ASSERT_GE(H5Awrite(a_vlen, vl_i32, &v), 0);

    // Reference attribute pointing to /ref_target.
    ScopedAttr a_ref(H5Acreate2(loc_id, "04_ref", H5T_STD_REF, scalar,
                                H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a_ref.valid());
    ASSERT_GE(H5Awrite(a_ref, H5T_STD_REF, &obj_ref), 0);

    if (extra_for_dense) {
      WriteIntAttr(loc_id, "05_plain", 999);
    }
  };

  ScopedGroup compact_grp = CreateGroupWithAttrProps(
      src, "/compact_grp", H5P_CRT_ORDER_TRACKED, /*max_compact=*/8,
      /*min_dense=*/6);
  ASSERT_TRUE(compact_grp.valid());
  populate_attrs(compact_grp, /*extra_for_dense=*/false);

  ScopedGroup dense_grp = CreateGroupWithAttrProps(
      src, "/dense_grp", H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED,
      /*max_compact=*/2, /*min_dense=*/1);
  ASSERT_TRUE(dense_grp.valid());
  populate_attrs(dense_grp, /*extra_for_dense=*/true);

  // Destination 1: SOHM-enabled + expand references + merge committed dtype.
  ScopedPlist fcpl_sohm(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl_sohm.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl_sohm, 1), 0);
  ASSERT_GE(
      H5Pset_shared_mesg_index(
          fcpl_sohm, 0, H5O_SHMESG_DTYPE_FLAG | H5O_SHMESG_SDSPACE_FLAG, 1),
      0);
  ScopedFile dst_expand =
      CreateInMemoryFile("ocopy_dst_expand.h5", H5F_ACC_TRUNC, fcpl_sohm);
  ASSERT_TRUE(dst_expand.valid());

  // Pre-commit matching datatype + pre-create destination object with committed
  // dt attr so H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG invokes H5A_type().
  ScopedType dst_comm(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(dst_comm.valid());
  ASSERT_GE(H5Tcommit2(dst_expand, "/existing_i32", dst_comm, H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  ScopedGroup dst_seed(H5Gcreate2(dst_expand, "/seed_obj", H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dst_seed.valid());
  ScopedAttr seed_attr(H5Acreate2(dst_seed, "seed_a", dst_comm, scalar,
                                  H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(seed_attr.valid());
  seed_attr.reset();
  dst_seed.reset();

  ScopedPlist ocpypl_expand(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpypl_expand.valid());
  ASSERT_GE(H5Pset_copy_object(ocpypl_expand,
                               H5O_COPY_EXPAND_REFERENCE_FLAG |
                                   H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG),
            0);
  ASSERT_GE(H5Ocopy(src, "/compact_grp", dst_expand, "/compact_grp",
                    ocpypl_expand, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ocopy(src, "/dense_grp", dst_expand, "/dense_grp", ocpypl_expand,
                    H5P_DEFAULT),
            0);

  // Verify dense copied attributes including expanded reference & VLEN.
  {
    ScopedGroup copied_dense(H5Gopen2(dst_expand, "/dense_grp", H5P_DEFAULT));
    ASSERT_TRUE(copied_dense.valid());

    ScopedAttr ref_a(H5Aopen(copied_dense, "04_ref", H5P_DEFAULT));
    ASSERT_TRUE(ref_a.valid());
    H5R_ref_t copied_ref{};
    absl::Cleanup destroy_copied_ref = [&] { H5Rdestroy(&copied_ref); };
    ASSERT_GE(H5Aread(ref_a, H5T_STD_REF, &copied_ref), 0);
    ScopedObject opened_ref_obj(
        H5Ropen_object(&copied_ref, H5P_DEFAULT, H5P_DEFAULT));
    EXPECT_TRUE(opened_ref_obj.valid());

    ScopedAttr vlen_a(H5Aopen(copied_dense, "03_vlen", H5P_DEFAULT));
    ASSERT_TRUE(vlen_a.valid());
    hvl_t copied_vl{0, nullptr};
    absl::Cleanup reclaim_copied_vl = [&] {
      H5Treclaim(vl_i32, scalar, H5P_DEFAULT, &copied_vl);
    };
    ASSERT_GE(H5Aread(vlen_a, vl_i32, &copied_vl), 0);
    ASSERT_EQ(copied_vl.len, 3U);
    EXPECT_EQ(static_cast<int32_t*>(copied_vl.p)[2], 30);
  }

  // Destination 2: without H5O_COPY_EXPAND_REFERENCE_FLAG -> zeroes reference.
  ScopedFile dst_no_expand = CreateInMemoryFile("ocopy_dst_noexp.h5");
  ASSERT_TRUE(dst_no_expand.valid());
  ASSERT_GE(H5Ocopy(src, "/compact_grp", dst_no_expand, "/compact_grp",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Ocopy(src, "/dense_grp", dst_no_expand, "/dense_grp",
                    H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedGroup copied_compact(H5Gopen2(dst_no_expand, "/compact_grp", H5P_DEFAULT));
  ASSERT_TRUE(copied_compact.valid());
  ScopedAttr unexp_ref_a(H5Aopen(copied_compact, "04_ref", H5P_DEFAULT));
  ASSERT_TRUE(unexp_ref_a.valid());
  H5R_ref_t zeroed_ref{};
  absl::Cleanup destroy_zeroed_ref = [&] { H5Rdestroy(&zeroed_ref); };
  ASSERT_GE(H5Aread(unexp_ref_a, H5T_STD_REF, &zeroed_ref), 0);
  {
    ScopedErrorSilence silence;
    ScopedObject opened_zeroed(
        H5Ropen_object(&zeroed_ref, H5P_DEFAULT, H5P_DEFAULT));
    EXPECT_FALSE(opened_zeroed.valid());
  }

  // Destination 3: version upgrade (LIBVER_EARLIEST compact v1 attr -> LATEST).
  ScopedPlist fapl_early = MakeCoreFapl();
  ASSERT_TRUE(fapl_early.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl_early, H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
      0);
  ScopedFile src_v1(
      H5Fcreate("ocopy_v1.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_early));
  ASSERT_TRUE(src_v1.valid());
  ScopedGroup v1_grp(
      H5Gcreate2(src_v1, "/g_v1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(v1_grp.valid());
  WriteIntAttr(v1_grp, "a_v1", 5);

  ScopedPlist fapl_latest = MakeCoreFapl();
  ASSERT_TRUE(fapl_latest.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl_latest, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
      0);
  ScopedFile dst_latest(
      H5Fcreate("ocopy_latest.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_latest));
  ASSERT_TRUE(dst_latest.valid());
  ASSERT_GE(H5Ocopy(src_v1, "/g_v1", dst_latest, "/g_v1", H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_GT(H5Aexists_by_name(dst_latest, "/g_v1", "a_v1", H5P_DEFAULT), 0);
}

TEST(H5AintTest, DirectInternalHelpersAndDecodedAinfoReopenPaths) {
  std::vector<uint8_t> img;
  {
    ScopedFile f_create = CreateInMemoryFile("reopen_src.h5");
    ASSERT_TRUE(f_create.valid());

    // Compact object with AINFO message (v2 object header via corder tracked).
    ScopedGroup compact_v2 = CreateGroupWithAttrProps(
        f_create, "/compact_v2", H5P_CRT_ORDER_TRACKED, /*max_compact=*/8,
        /*min_dense=*/6);
    ASSERT_TRUE(compact_v2.valid());
    WriteIntAttr(compact_v2, "a1", 1);
    WriteIntAttr(compact_v2, "a2", 2);

    // Dense object with untracked creation order -> crt_idx == MAX on all.
    ScopedGroup dense_untracked = CreateGroupWithAttrProps(
        f_create, "/dense_untracked", /*crt_order_flags=*/0, /*max_compact=*/2,
        /*min_dense=*/1);
    ASSERT_TRUE(dense_untracked.valid());
    WriteIntAttr(dense_untracked, "x1", 10);
    WriteIntAttr(dense_untracked, "x2", 20);
    WriteIntAttr(dense_untracked, "x3", 30);

    img = FlushToMemoryImage(f_create);
    ASSERT_FALSE(img.empty());
  }

  ScopedFile f_reopen = OpenMemoryImage(img, H5F_ACC_RDWR);
  ASSERT_TRUE(f_reopen.valid());

  // 1. Decoded compact AINFO (nattrs == HSIZET_MAX -> oh->attr_msgs_seen) and
  // decoded dense AINFO (nattrs == HSIZET_MAX -> H5B2_get_nrec).
  EXPECT_GT(H5Aexists_by_name(f_reopen, "/compact_v2", "a1", H5P_DEFAULT), 0);
  EXPECT_GT(H5Aexists_by_name(f_reopen, "/dense_untracked", "x2", H5P_DEFAULT),
            0);

  // 2. Direct internal calls: H5A_oloc, H5A_nameof, H5A_type,
  // H5A__rename_by_name equal names, empty H5A__dense_build_table (nrec==0),
  // and H5A__dense_build_table with equal crt_idx (covers comparator `==`).
  ScopedGroup compact_reopen(H5Gopen2(f_reopen, "/compact_v2", H5P_DEFAULT));
  ASSERT_TRUE(compact_reopen.valid());
  ScopedAttr a1_id(H5Aopen(compact_reopen, "a1", H5P_DEFAULT));
  ASSERT_TRUE(a1_id.valid());

  ScopedGroup dense_reopen(H5Gopen2(f_reopen, "/dense_untracked", H5P_DEFAULT));
  ASSERT_TRUE(dense_reopen.valid());

  {
    ScopedApiContext ctx;
    H5A_t* a1_ptr = static_cast<H5A_t*>(H5VL_object_verify(a1_id, H5I_ATTR));
    ASSERT_NE(a1_ptr, nullptr);
    EXPECT_NE(H5A_oloc(a1_ptr), nullptr);
    EXPECT_NE(H5A_nameof(a1_ptr), nullptr);
    EXPECT_NE(H5A_type(a1_ptr), nullptr);

    H5G_loc_t grp_loc{};
    ASSERT_GE(H5G_loc(compact_reopen, &grp_loc), 0);
    EXPECT_GE(H5A__rename_by_name(grp_loc, ".", "a1", "a1"), 0);

    // Inspect empty H5A__dense_build_table (nrec == 0).
    H5O_ainfo_t empty_ainfo{};
    empty_ainfo.fheap_addr = HADDR_UNDEF;
    empty_ainfo.name_bt2_addr = HADDR_UNDEF;
    empty_ainfo.corder_bt2_addr = HADDR_UNDEF;
    ASSERT_GE(H5A__dense_create(grp_loc.oloc->file, &empty_ainfo), 0);
    absl::Cleanup del_dense = [&] {
      H5A__dense_delete(grp_loc.oloc->file, &empty_ainfo);
    };

    H5A_attr_table_t empty_table{};
    ASSERT_GE(H5A__dense_build_table(grp_loc.oloc->file, &empty_ainfo,
                                     H5_INDEX_NAME, H5_ITER_INC, &empty_table),
              0);
    EXPECT_EQ(empty_table.num_attrs, 0U);
    EXPECT_EQ(empty_table.attrs, nullptr);
    EXPECT_GE(H5A__attr_release_table(&empty_table), 0);

    // Retrieve dense_untracked's AINFO and sort by H5_INDEX_CRT_ORDER so
    // equal H5O_MAX_CRT_ORDER_IDX triggers both `else ret_value = 0` branches.
    H5G_loc_t dense_loc{};
    ASSERT_GE(H5G_loc(dense_reopen, &dense_loc), 0);
    H5O_t* oh = H5O_protect(dense_loc.oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    H5O_ainfo_t dense_ainfo{};
    const htri_t has_ainfo =
        H5A__get_ainfo(dense_loc.oloc->file, oh, &dense_ainfo);
    ASSERT_GE(H5O_unprotect(dense_loc.oloc, oh, H5AC__NO_FLAGS_SET), 0);
    ASSERT_GT(has_ainfo, 0);

    for (H5_iter_order_t order : {H5_ITER_INC, H5_ITER_DEC, H5_ITER_NATIVE}) {
      H5A_attr_table_t tbl{};
      ASSERT_GE(H5A__dense_build_table(dense_loc.oloc->file, &dense_ainfo,
                                       H5_INDEX_CRT_ORDER, order, &tbl),
                0);
      EXPECT_EQ(tbl.num_attrs, 3U);
      ASSERT_GE(H5A__attr_release_table(&tbl), 0);
    }
  }
}

struct CompWithVlen {
  int32_t tag;
  hvl_t seq;
};

struct VlenAllocTracker {
  std::vector<void*> ptrs;

  ~VlenAllocTracker() {
    for (void* p : ptrs) {
      std::free(p);
    }
  }
};

void* TrackVlenAlloc(size_t size, void* alloc_info) {
  auto* tracker = static_cast<VlenAllocTracker*>(alloc_info);
  void* p = std::malloc(size);
  if (p != nullptr && tracker != nullptr) {
    tracker->ptrs.push_back(p);
  }
  return p;
}

void TrackVlenFree(void* mem, void* free_info) {
  auto* tracker = static_cast<VlenAllocTracker*>(free_info);
  if (mem != nullptr && tracker != nullptr) {
    auto it = std::find(tracker->ptrs.begin(), tracker->ptrs.end(), mem);
    if (it != tracker->ptrs.end()) {
      tracker->ptrs.erase(it);
    }
  }
  std::free(mem);
}

herr_t FreeNoopCb(void* /*item*/, void* /*key*/, void* /*op_data*/) {
  return 0;
}

H5F_t* FileStructFromId(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

void ExpectNullAttr(H5A_t* attr) {
  if (attr != nullptr) {
    H5A__close(attr);
  }
  EXPECT_EQ(attr, nullptr);
}

TEST(H5AintTest, PublicDeletedAttrWriteOpenByNameAndObjectCopyEdgePaths) {
  ScopedFile src = CreateInMemoryFile("pub_edge_src.h5");
  ASSERT_TRUE(src.valid());
  ScopedGroup grp(
      H5Gcreate2(src, "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  // 1. Public H5Aopen_by_name (covers H5A__open_by_name lines 681, 685).
  WriteIntAttr(grp, "dup", 123);
  ScopedAttr by_name(
      H5Aopen_by_name(src, "/grp", "dup", H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(by_name.valid());
  int32_t out_val = 0;
  ASSERT_GE(H5Aread(by_name, H5T_NATIVE_INT32, &out_val), 0);
  EXPECT_EQ(out_val, 123);

  // 2. Write via an open attr_id after deleting the attribute (hits line 931).
  ScopedAttr doomed(H5Acreate2(grp, "temp_a", H5T_NATIVE_INT32, scalar,
                               H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(doomed.valid());
  int32_t v = 7;
  ASSERT_GE(H5Awrite(doomed, H5T_NATIVE_INT32, &v), 0);
  ASSERT_GE(H5Adelete(grp, "temp_a"), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Awrite(doomed, H5T_NATIVE_INT32, &v), 0);
  }

  // 3. Object copy of compound-with-VLEN attribute (needs bkg buffer: lines
  // 2288, 2300, 2342).
  ScopedType vlen_i32(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vlen_i32.valid());
  ScopedType comp_vlen_t(H5Tcreate(H5T_COMPOUND, sizeof(CompWithVlen)));
  ASSERT_TRUE(comp_vlen_t.valid());
  ASSERT_GE(H5Tinsert(comp_vlen_t, "tag", HOFFSET(CompWithVlen, tag),
                      H5T_NATIVE_INT32),
            0);
  ASSERT_GE(
      H5Tinsert(comp_vlen_t, "seq", HOFFSET(CompWithVlen, seq), vlen_i32), 0);

  ScopedGroup comp_vlen_grp(
      H5Gcreate2(src, "/comp_vlen", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(comp_vlen_grp.valid());
  std::array<int32_t, 2> raw_seq = {11, 22};
  CompWithVlen cv_in{9, {raw_seq.size(), raw_seq.data()}};
  {
    ScopedAttr a_cv(H5Acreate2(comp_vlen_grp, "cv", comp_vlen_t, scalar,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a_cv.valid());
    ASSERT_GE(H5Awrite(a_cv, comp_vlen_t, &cv_in), 0);
  }

  ScopedFile dst_ok = CreateInMemoryFile("pub_edge_dst_ok.h5");
  ASSERT_TRUE(dst_ok.valid());
  ASSERT_GE(
      H5Ocopy(src, "/comp_vlen", dst_ok, "/comp_vlen", H5P_DEFAULT, H5P_DEFAULT),
      0);

  // 4. Compact & dense UTF-8 attribute copy failures into an EARLIEST dst
  // (hits 2323 on compact and 2473, 2535 on dense) and bogus H5T_STD_REF_OBJ
  // copy with H5O_COPY_EXPAND_REFERENCE_FLAG (hits 2430 compact, 2476 dense).
  ScopedPlist utf8_acpl(H5Pcreate(H5P_ATTRIBUTE_CREATE));
  ASSERT_TRUE(utf8_acpl.valid());
  ASSERT_GE(H5Pset_char_encoding(utf8_acpl, H5T_CSET_UTF8), 0);

  ScopedGroup compact_utf8(
      H5Gcreate2(src, "/c_utf8", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(compact_utf8.valid());
  WriteIntAttr(compact_utf8, "u8", 1, utf8_acpl);

  ScopedGroup dense_utf8 = CreateGroupWithAttrProps(
      src, "/d_utf8", H5P_CRT_ORDER_TRACKED, /*max_compact=*/1,
      /*min_dense=*/0);
  ASSERT_TRUE(dense_utf8.valid());
  WriteIntAttr(dense_utf8, "d1", 1, utf8_acpl);
  WriteIntAttr(dense_utf8, "d2", 2, utf8_acpl);

  ScopedGroup compact_bad_ref(
      H5Gcreate2(src, "/c_bad_ref", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(compact_bad_ref.valid());
  {
    ScopedAttr ar(H5Acreate2(compact_bad_ref, "bad_ref", H5T_STD_REF_OBJ,
                             scalar, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ar.valid());
    hobj_ref_t bogus_ref = 0xdeadbeef0ULL;
    ASSERT_GE(H5Awrite(ar, H5T_STD_REF_OBJ, &bogus_ref), 0);
  }

  ScopedGroup dense_bad_ref = CreateGroupWithAttrProps(
      src, "/d_bad_ref", H5P_CRT_ORDER_TRACKED, /*max_compact=*/1,
      /*min_dense=*/0);
  ASSERT_TRUE(dense_bad_ref.valid());
  {
    ScopedAttr ar1(H5Acreate2(dense_bad_ref, "bad_ref", H5T_STD_REF_OBJ,
                              scalar, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ar1.valid());
    hobj_ref_t bogus_ref = 0xdeadbeef0ULL;
    ASSERT_GE(H5Awrite(ar1, H5T_STD_REF_OBJ, &bogus_ref), 0);
    WriteIntAttr(dense_bad_ref, "other", 2);
  }

  ScopedPlist expand_ocpypl(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(expand_ocpypl.valid());
  ASSERT_GE(H5Pset_copy_object(expand_ocpypl, H5O_COPY_EXPAND_REFERENCE_FLAG),
            0);

  ScopedFile dst_bad = CreateInMemoryFile("pub_edge_dst_bad.h5");
  ASSERT_TRUE(dst_bad.valid());
  H5F_t* dst_bad_f = FileStructFromId(dst_bad);
  ASSERT_NE(dst_bad_f, nullptr);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Ocopy(src, "/c_bad_ref", dst_bad, "/c_bad_ref", expand_ocpypl,
                      H5P_DEFAULT),
              0);
    EXPECT_LT(H5Ocopy(src, "/d_bad_ref", dst_bad, "/d_bad_ref", expand_ocpypl,
                      H5P_DEFAULT),
              0);

    const H5F_libver_t saved_high = dst_bad_f->shared->high_bound;
    dst_bad_f->shared->high_bound = H5F_LIBVER_EARLIEST;
    absl::Cleanup restore_high = [&] {
      dst_bad_f->shared->high_bound = saved_high;
    };
    EXPECT_LT(H5Ocopy(src, "/c_utf8", dst_bad, "/c_utf8", H5P_DEFAULT,
                      H5P_DEFAULT),
              0);
    EXPECT_LT(H5Ocopy(src, "/d_utf8", dst_bad, "/d_utf8", H5P_DEFAULT,
                      H5P_DEFAULT),
              0);
  }
}

TEST(H5AintTest, InternalCreateReadWriteGetTypeAndTableFailurePaths) {
  ScopedFile f = CreateInMemoryFile("int_errs.h5");
  ASSERT_TRUE(f.valid());
  H5F_t* f_ptr = FileStructFromId(f);
  ASSERT_NE(f_ptr, nullptr);

  ScopedGroup grp(
      H5Gcreate2(f, "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(null_space.valid());
  ScopedPlist utf8_acpl(H5Pcreate(H5P_ATTRIBUTE_CREATE));
  ASSERT_TRUE(utf8_acpl.valid());
  ASSERT_GE(H5Pset_char_encoding(utf8_acpl, H5T_CSET_UTF8), 0);

  ScopedType i32_t(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(i32_t.valid());
  constexpr hsize_t kArrDims[1] = {2};
  ScopedType arr_t(H5Tarray_create2(H5T_NATIVE_INT32, 1, kArrDims));
  ASSERT_TRUE(arr_t.valid());

  ScopedType vlen_i32(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vlen_i32.valid());
  ScopedType zero_memb_comp(H5Tcreate(H5T_COMPOUND, sizeof(hvl_t)));
  ASSERT_TRUE(zero_memb_comp.valid());
  ASSERT_GE(H5Tinsert(zero_memb_comp, "v", 0, vlen_i32), 0);

  ScopedType comm_i32(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(comm_i32.valid());
  ASSERT_GE(
      H5Tcommit2(f, "/comm_i32", comm_i32, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
      0);

  ScopedAttr read_write_attr(H5Acreate2(grp, "rw", H5T_NATIVE_INT32, scalar,
                                        H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(read_write_attr.valid());
  int32_t init_v = 42;
  ASSERT_GE(H5Awrite(read_write_attr, H5T_NATIVE_INT32, &init_v), 0);

  // SOHM-enabled file for H5A__create H5SM_try_share failure lines (376, 378).
  ScopedPlist sohm_fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(sohm_fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(sohm_fcpl, 1), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(
                sohm_fcpl, 0, H5O_SHMESG_DTYPE_FLAG | H5O_SHMESG_SDSPACE_FLAG, 1),
            0);
  ScopedPlist sohm_fapl = MakeCoreFapl();
  ASSERT_TRUE(sohm_fapl.valid());
  ScopedFile f_sohm(
      H5Fcreate("int_sohm.h5", H5F_ACC_TRUNC, sohm_fcpl, sohm_fapl));
  ASSERT_TRUE(f_sohm.valid());
  H5F_t* f_sohm_ptr = FileStructFromId(f_sohm);
  ASSERT_NE(f_sohm_ptr, nullptr);

  ScopedType sohm_comp_dt(H5Tcreate(H5T_COMPOUND, sizeof(CompoundPair)));
  ASSERT_TRUE(sohm_comp_dt.valid());
  ASSERT_GE(
      H5Tinsert(sohm_comp_dt, "a", HOFFSET(CompoundPair, a), H5T_NATIVE_INT32),
      0);
  ASSERT_GE(
      H5Tinsert(sohm_comp_dt, "b", HOFFSET(CompoundPair, b), H5T_NATIVE_DOUBLE),
      0);
  ScopedType sohm_comm_dt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(sohm_comm_dt.valid());
  ASSERT_GE(H5Tcommit2(f_sohm, "/sohm_comm", sohm_comm_dt, H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  constexpr hsize_t kVecDims[2] = {2, 3};
  ScopedSpace vec_space(H5Screate_simple(2, kVecDims, nullptr));
  ASSERT_TRUE(vec_space.valid());

  // 1. Direct H5A__create, H5A__read, H5A__write, H5A__exists_by_name, and
  // H5A__iterate error branches.
  {
    ScopedApiContext ctx;
    ScopedErrorSilence silence;

    H5G_loc_t grp_loc{};
    ASSERT_GE(H5G_loc(grp, &grp_loc), 0);

    H5O_loc_t bad_oloc = *grp_loc.oloc;
    bad_oloc.addr = static_cast<haddr_t>(0x7fffff00ULL);
    H5G_loc_t bad_loc{&bad_oloc, grp_loc.path};

    auto* scalar_ptr = static_cast<H5S_t*>(H5I_object(scalar));
    auto* null_ptr = static_cast<H5S_t*>(H5I_object(null_space));
    auto* i32_ptr = static_cast<H5T_t*>(H5I_object(i32_t));
    auto* arr_ptr = static_cast<H5T_t*>(H5I_object(arr_t));
    auto* zero_comp_ptr = static_cast<H5T_t*>(H5I_object(zero_memb_comp));
    auto* comm_i32_ptr =
        static_cast<H5T_t*>(H5VL_object_verify(comm_i32, H5I_DATATYPE));
    auto* rw_attr_ptr =
        static_cast<H5A_t*>(H5VL_object_verify(read_write_attr, H5I_ATTR));
    ASSERT_NE(scalar_ptr, nullptr);
    ASSERT_NE(null_ptr, nullptr);
    ASSERT_NE(i32_ptr, nullptr);
    ASSERT_NE(arr_ptr, nullptr);
    ASSERT_NE(zero_comp_ptr, nullptr);
    ASSERT_NE(comm_i32_ptr, nullptr);
    ASSERT_NE(rw_attr_ptr, nullptr);

    // Line 303, 1467, 2658: bogus oloc addr fails H5O__attr_exists /
    // H5O_open_by_loc.
    ExpectNullAttr(H5A__create(&bad_loc, "bad_a", i32_ptr, scalar_ptr,
                               H5P_ATTRIBUTE_CREATE_DEFAULT));
    bool exists = false;
    EXPECT_LT(H5A__exists_by_name(bad_loc, ".", "bad_a", &exists), 0);
    hsize_t iter_idx = 0;
    EXPECT_LT(H5A__iterate(&bad_loc, ".", H5_INDEX_NAME, H5_ITER_INC, &iter_idx,
                           CollectNamesOp2, nullptr),
              0);

    // Line 351: H5T_set_loc failure on compound member with zero old_size.
    H5T_t* memb_vlen = zero_comp_ptr->shared->u.compnd.memb[0].type;
    const size_t saved_memb_size = memb_vlen->shared->size;
    memb_vlen->shared->size = 0;
    {
      absl::Cleanup restore_memb = [&] {
        memb_vlen->shared->size = saved_memb_size;
      };
      ExpectNullAttr(H5A__create(&grp_loc, "zero_memb_vlen", zero_comp_ptr,
                                 scalar_ptr, H5P_ATTRIBUTE_CREATE_DEFAULT));
    }

    // Lines 355, 362, 412, 2088: high_bound == H5F_LIBVER_EARLIEST rejects
    // H5T_ARRAY, H5S_NULL (v2 dataspace), and UTF-8 attributes (v3 attr).
    auto* vec_ptr = static_cast<H5S_t*>(H5I_object(vec_space));
    ASSERT_NE(vec_ptr, nullptr);
    {
      const H5F_libver_t saved_low = f_ptr->shared->low_bound;
      const H5F_libver_t saved_high = f_ptr->shared->high_bound;
      f_ptr->shared->low_bound = H5F_LIBVER_EARLIEST;
      f_ptr->shared->high_bound = H5F_LIBVER_EARLIEST;
      absl::Cleanup restore_bounds = [&] {
        f_ptr->shared->low_bound = saved_low;
        f_ptr->shared->high_bound = saved_high;
      };
      ExpectNullAttr(H5A__create(&grp_loc, "arr_early", arr_ptr, scalar_ptr,
                                 H5P_ATTRIBUTE_CREATE_DEFAULT));
      ExpectNullAttr(H5A__create(&grp_loc, "null_early", i32_ptr, null_ptr,
                                 H5P_ATTRIBUTE_CREATE_DEFAULT));
      ExpectNullAttr(
          H5A__create(&grp_loc, "utf8_early", i32_ptr, vec_ptr, utf8_acpl));

      // Also exercise H5A__set_version bounds rejection directly.
      f_ptr->shared->low_bound = H5F_LIBVER_LATEST;
      EXPECT_LT(H5A__set_version(f_ptr, rw_attr_ptr), 0);
    }

    // Lines 376, 378: H5SM_try_share failure with bogus sohm_addr.
    {
      H5G_loc_t sohm_root_loc{};
      ASSERT_GE(H5G_loc(f_sohm, &sohm_root_loc), 0);
      auto* sohm_comp_ptr = static_cast<H5T_t*>(H5I_object(sohm_comp_dt));
      auto* sohm_comm_ptr =
          static_cast<H5T_t*>(H5VL_object_verify(sohm_comm_dt, H5I_DATATYPE));
      ASSERT_NE(sohm_comm_ptr, nullptr);
      const haddr_t saved_sohm_addr = f_sohm_ptr->shared->sohm_addr;
      f_sohm_ptr->shared->sohm_addr = static_cast<haddr_t>(0x7ffffe00ULL);
      absl::Cleanup restore_sohm = [&] {
        f_sohm_ptr->shared->sohm_addr = saved_sohm_addr;
      };
      ExpectNullAttr(H5A__create(&sohm_root_loc, "bad_sohm_dt", sohm_comp_ptr,
                                 scalar_ptr, H5P_ATTRIBUTE_CREATE_DEFAULT));
      ExpectNullAttr(H5A__create(&sohm_root_loc, "bad_sohm_ds", sohm_comm_ptr,
                                 vec_ptr, H5P_ATTRIBUTE_CREATE_DEFAULT));
    }

    // Line 387: H5T_link failure on committed datatype with invalid oloc.addr.
    {
      const haddr_t saved_comm_addr = comm_i32_ptr->oloc.addr;
      comm_i32_ptr->oloc.addr = static_cast<haddr_t>(0x7ffffd00ULL);
      absl::Cleanup restore_comm_addr = [&] {
        comm_i32_ptr->oloc.addr = saved_comm_addr;
      };
      ExpectNullAttr(H5A__create(&grp_loc, "bad_comm_link", comm_i32_ptr,
                                 scalar_ptr, H5P_ATTRIBUTE_CREATE_DEFAULT));
    }

    // Lines 398, 741, 854: invalid extent.nelem returns < 0 from
    // H5S_GET_EXTENT_NPOINTS in H5A__create, H5A__read, and H5A__write.
    {
      auto* rw_ds = rw_attr_ptr->shared->ds;
      const hsize_t saved_rw_nelem = rw_ds->extent.nelem;
      const hsize_t saved_sc_nelem = scalar_ptr->extent.nelem;
      scalar_ptr->extent.nelem = static_cast<hsize_t>(-1);
      rw_ds->extent.nelem = static_cast<hsize_t>(-1);
      absl::Cleanup restore_nelem = [&] {
        scalar_ptr->extent.nelem = saved_sc_nelem;
        rw_ds->extent.nelem = saved_rw_nelem;
      };
      ExpectNullAttr(H5A__create(&grp_loc, "bad_nelem", i32_ptr, scalar_ptr,
                                 H5P_ATTRIBUTE_CREATE_DEFAULT));
      int32_t buf = 0;
      EXPECT_LT(H5A__read(rw_attr_ptr, i32_ptr, &buf), 0);
      EXPECT_LT(H5A__write(rw_attr_ptr, i32_ptr, &buf), 0);
    }
  }

  // 2. Build reopenable file with compact (2 attrs), dense, and committed
  // compound-vlen attr to cover H5A__get_type (1056, 1060, 1073, 1082),
  // H5A__compact_build_table (1576, 1587, 1588), H5A__dense_build_table (1665),
  // H5A__attr_iterate_table (1926, 1928), and H5A__get_ainfo (2010, 2018).
  std::vector<uint8_t> img;
  {
    ScopedFile f_reopen_src = CreateInMemoryFile("int_reopen_src.h5");
    ASSERT_TRUE(f_reopen_src.valid());

    ScopedGroup c_grp(H5Gcreate2(f_reopen_src, "/c_two", H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(c_grp.valid());
    WriteIntAttr(c_grp, "c1", 10);
    WriteIntAttr(c_grp, "c2", 20);

    ScopedGroup d_grp = CreateGroupWithAttrProps(
        f_reopen_src, "/d_one", H5P_CRT_ORDER_TRACKED, /*max_compact=*/1,
        /*min_dense=*/0);
    ASSERT_TRUE(d_grp.valid());
    WriteIntAttr(d_grp, "d1", 100);
    WriteIntAttr(d_grp, "d2", 200);

    ScopedType comm_comp_vlen(H5Tcreate(H5T_COMPOUND, sizeof(CompWithVlen)));
    ASSERT_TRUE(comm_comp_vlen.valid());
    ASSERT_GE(H5Tinsert(comm_comp_vlen, "tag", HOFFSET(CompWithVlen, tag),
                        H5T_NATIVE_INT32),
              0);
    ASSERT_GE(H5Tinsert(comm_comp_vlen, "seq", HOFFSET(CompWithVlen, seq),
                        vlen_i32),
              0);
    ASSERT_GE(H5Tcommit2(f_reopen_src, "/comm_comp_vlen", comm_comp_vlen,
                         H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              0);

    ScopedAttr comm_attr(H5Acreate2(c_grp, "has_comm_cv", comm_comp_vlen,
                                    scalar, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(comm_attr.valid());
    std::array<int32_t, 1> item = {5};
    CompWithVlen cv_v{1, {item.size(), item.data()}};
    ASSERT_GE(H5Awrite(comm_attr, comm_comp_vlen, &cv_v), 0);

    img = FlushToMemoryImage(f_reopen_src);
    ASSERT_FALSE(img.empty());
  }

  ScopedFile f_ro = OpenMemoryImage(img, H5F_ACC_RDWR);
  ASSERT_TRUE(f_ro.valid());
  H5F_t* f_ro_ptr = FileStructFromId(f_ro);
  ASSERT_NE(f_ro_ptr, nullptr);

  ScopedGroup c_ro(H5Gopen2(f_ro, "/c_two", H5P_DEFAULT));
  ScopedGroup d_ro(H5Gopen2(f_ro, "/d_one", H5P_DEFAULT));
  ScopedAttr comm_cv_attr(H5Aopen(c_ro, "has_comm_cv", H5P_DEFAULT));
  ScopedType comm_cv_open(H5Topen2(f_ro, "/comm_comp_vlen", H5P_DEFAULT));
  ASSERT_TRUE(c_ro.valid());
  ASSERT_TRUE(d_ro.valid());
  ASSERT_TRUE(comm_cv_attr.valid());
  ASSERT_TRUE(comm_cv_open.valid());

  {
    ScopedApiContext ctx;
    ScopedErrorSilence silence;

    // Lines 1073, 1082: direct H5A__get_type on committed-dt attr with NULL
    // VOL wrap context causes H5VL_wrap_register to fail cleanly.
    auto* comm_cv_attr_ptr =
        static_cast<H5A_t*>(H5VL_object_verify(comm_cv_attr, H5I_ATTR));
    ASSERT_NE(comm_cv_attr_ptr, nullptr);
    EXPECT_EQ(H5A__get_type(comm_cv_attr_ptr), H5I_INVALID_HID);

    // Line 1060: H5T_copy_reopen reuses already-opened committed datatype
    // (reopened_fo != NULL); temporarily set member size == 0 so
    // H5T_set_loc(MEMORY) hits old_size == 0 check.
    auto* comm_cv_dt_ptr = static_cast<H5T_t*>(H5I_object(comm_cv_open));
    ASSERT_NE(comm_cv_dt_ptr, nullptr);
    H5T_t* opened_memb_vlen = comm_cv_dt_ptr->shared->u.compnd.memb[1].type;
    const size_t saved_vlen_size = opened_memb_vlen->shared->size;
    const H5T_loc_t saved_vlen_loc = opened_memb_vlen->shared->u.vlen.loc;
    opened_memb_vlen->shared->size = 0;
    opened_memb_vlen->shared->u.vlen.loc = H5T_LOC_DISK;
    {
      absl::Cleanup restore_vlen = [&] {
        opened_memb_vlen->shared->size = saved_vlen_size;
        opened_memb_vlen->shared->u.vlen.loc = saved_vlen_loc;
      };
      EXPECT_EQ(H5A__get_type(comm_cv_attr_ptr), H5I_INVALID_HID);
    }

    // Line 1056: synthetic attribute whose dt copy fails H5T_copy_reopen via
    // invalid reference type during H5T_set_loc(MEMORY).
    H5A_t bad_ref_attr = *comm_cv_attr_ptr;
    H5A_shared_t bad_ref_shared = *comm_cv_attr_ptr->shared;
    H5T_t* bad_ref_dt = H5T_copy(static_cast<H5T_t*>(H5I_object(i32_t)),
                                 H5T_COPY_TRANSIENT);
    ASSERT_NE(bad_ref_dt, nullptr);
    absl::Cleanup close_bad_ref_dt = [&] { H5T_close(bad_ref_dt); };
    bad_ref_dt->shared->type = H5T_REFERENCE;
    bad_ref_dt->shared->force_conv = true;
    bad_ref_dt->shared->u.atomic.u.r.loc = H5T_LOC_DISK;
    bad_ref_dt->shared->u.atomic.u.r.opaque = false;
    bad_ref_dt->shared->u.atomic.u.r.rtype = H5R_MAXTYPE;
    bad_ref_shared.dt = bad_ref_dt;
    bad_ref_attr.shared = &bad_ref_shared;
    EXPECT_EQ(H5A__get_type(&bad_ref_attr), H5I_INVALID_HID);

    // Line 1926, 1928: H5A__attr_iterate_table with invalid op_type.
    H5A_attr_table_t fake_table{};
    fake_table.num_attrs = 1;
    fake_table.max_attrs = 1;
    std::array<H5A_t*, 1> fake_ptrs = {comm_cv_attr_ptr};
    fake_table.attrs = fake_ptrs.data();
    H5A_attr_iter_op_t bad_op{};
    bad_op.op_type = static_cast<H5A_attr_iter_op_type_t>(99);
    EXPECT_LT(H5A__attr_iterate_table(&fake_table, 0, nullptr, c_ro, &bad_op,
                                      nullptr),
              0);

    // Lines 1576, 1587, 1588: H5A__compact_build_table cleanup after second
    // attribute decode failure.
    H5G_loc_t c_ro_loc{};
    ASSERT_GE(H5G_loc(c_ro, &c_ro_loc), 0);
    H5O_t* c_oh = H5O_protect(c_ro_loc.oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(c_oh, nullptr);
    absl::Cleanup unprotect_c = [&] {
      H5O_unprotect(c_ro_loc.oloc, c_oh, H5AC__NO_FLAGS_SET);
    };
    int seen_attrs = 0;
    size_t second_idx = c_oh->nmesgs;
    for (size_t i = 0; i < c_oh->nmesgs; ++i) {
      if (c_oh->mesg[i].type && c_oh->mesg[i].type->id == H5O_ATTR_ID) {
        if (seen_attrs == 1) {
          second_idx = i;
          break;
        }
        seen_attrs++;
      }
    }
    ASSERT_LT(second_idx, c_oh->nmesgs);
    ASSERT_NE(c_oh->mesg[second_idx].raw, nullptr);
    void* saved_attr_native = c_oh->mesg[second_idx].native;
    const uint8_t saved_attr_raw0 = c_oh->mesg[second_idx].raw[0];
    c_oh->mesg[second_idx].native = nullptr;
    c_oh->mesg[second_idx].raw[0] = 0;
    {
      absl::Cleanup restore_attr_mesg = [&] {
        c_oh->mesg[second_idx].native = saved_attr_native;
        c_oh->mesg[second_idx].raw[0] = saved_attr_raw0;
      };
      H5A_attr_table_t partial_table{};
      EXPECT_LT(H5A__compact_build_table(f_ro_ptr, c_oh, H5_INDEX_NAME,
                                         H5_ITER_INC, &partial_table),
                0);
    }

    // Lines 1665, 2010, 2018: H5A__dense_build_table & H5A__get_ainfo failures.
    H5G_loc_t d_ro_loc{};
    ASSERT_GE(H5G_loc(d_ro, &d_ro_loc), 0);
    H5O_t* d_oh = H5O_protect(d_ro_loc.oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(d_oh, nullptr);
    absl::Cleanup unprotect_d = [&] {
      H5O_unprotect(d_ro_loc.oloc, d_oh, H5AC__NO_FLAGS_SET);
    };

    size_t ainfo_idx = d_oh->nmesgs;
    for (size_t i = 0; i < d_oh->nmesgs; ++i) {
      if (d_oh->mesg[i].type && d_oh->mesg[i].type->id == H5O_AINFO_ID) {
        ainfo_idx = i;
        break;
      }
    }
    ASSERT_LT(ainfo_idx, d_oh->nmesgs);
    ASSERT_NE(d_oh->mesg[ainfo_idx].raw, nullptr);
    void* saved_ainfo_native = d_oh->mesg[ainfo_idx].native;
    const uint8_t saved_ainfo_raw0 = d_oh->mesg[ainfo_idx].raw[0];
    d_oh->mesg[ainfo_idx].native = nullptr;
    d_oh->mesg[ainfo_idx].raw[0] = 0xff;
    H5O_ainfo_t ainfo_out{};
    {
      absl::Cleanup restore_ainfo_raw = [&] {
        d_oh->mesg[ainfo_idx].native = saved_ainfo_native;
        d_oh->mesg[ainfo_idx].raw[0] = saved_ainfo_raw0;
      };
      EXPECT_LT(H5A__get_ainfo(f_ro_ptr, d_oh, &ainfo_out), 0);
    }

    // Decode valid AINFO then corrupt its cached name_bt2_addr + nattrs.
    ASSERT_GT(H5A__get_ainfo(f_ro_ptr, d_oh, &ainfo_out), 0);
    ASSERT_NE(d_oh->mesg[ainfo_idx].native, nullptr);
    auto* cached_ainfo =
        static_cast<H5O_ainfo_t*>(d_oh->mesg[ainfo_idx].native);
    const haddr_t saved_bt2_addr = cached_ainfo->name_bt2_addr;
    const hsize_t saved_nattrs = cached_ainfo->nattrs;
    cached_ainfo->name_bt2_addr = static_cast<haddr_t>(0x7ffffc00ULL);
    cached_ainfo->nattrs = HSIZET_MAX;
    {
      absl::Cleanup restore_cached_ainfo = [&] {
        cached_ainfo->name_bt2_addr = saved_bt2_addr;
        cached_ainfo->nattrs = saved_nattrs;
      };
      EXPECT_LT(H5A__get_ainfo(f_ro_ptr, d_oh, &ainfo_out), 0);
    }

    H5O_ainfo_t bad_ainfo = ainfo_out;
    bad_ainfo.name_bt2_addr = static_cast<haddr_t>(0x7ffffb00ULL);
    H5A_attr_table_t bad_dense_tbl{};
    EXPECT_LT(H5A__dense_build_table(f_ro_ptr, &bad_ainfo, H5_INDEX_NAME,
                                     H5_ITER_INC, &bad_dense_tbl),
              0);
  }
}

TEST(H5AintTest, InternalAttrCopyAndPostCopyFailurePaths) {
  // Build source file with a real VLEN(int32) attribute, committed int32 attr,
  // and plain scalar attr, then flush & reopen to exercise disk-based paths.
  std::vector<uint8_t> src_img;
  {
    ScopedFile f_src = CreateInMemoryFile("copy_edge_src.h5");
    ASSERT_TRUE(f_src.valid());
    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    ScopedType vlen_i32(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vlen_i32.valid());

    ScopedAttr a_vlen(H5Acreate2(f_src, "a_vlen", vlen_i32, scalar, H5P_DEFAULT,
                                 H5P_DEFAULT));
    ASSERT_TRUE(a_vlen.valid());
    std::array<int32_t, 2> seq = {7, 8};
    hvl_t vl_in{seq.size(), seq.data()};
    ASSERT_GE(H5Awrite(a_vlen, vlen_i32, &vl_in), 0);

    ScopedType comm_i32(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(comm_i32.valid());
    ASSERT_GE(H5Tcommit2(f_src, "/comm_i32", comm_i32, H5P_DEFAULT, H5P_DEFAULT,
                         H5P_DEFAULT),
              0);
    ScopedAttr a_comm(H5Acreate2(f_src, "a_comm", comm_i32, scalar, H5P_DEFAULT,
                                 H5P_DEFAULT));
    ASSERT_TRUE(a_comm.valid());
    int32_t cv = 5;
    ASSERT_GE(H5Awrite(a_comm, comm_i32, &cv), 0);

    WriteIntAttr(f_src, "a_i32", 99);
    src_img = FlushToMemoryImage(f_src);
    ASSERT_FALSE(src_img.empty());
  }

  std::vector<uint8_t> rdonly_dst_img;
  {
    ScopedFile f_tmp = CreateInMemoryFile("rdonly_dst_seed.h5");
    ASSERT_TRUE(f_tmp.valid());
    WriteIntAttr(f_tmp, "seed", 1);
    rdonly_dst_img = FlushToMemoryImage(f_tmp);
    ASSERT_FALSE(rdonly_dst_img.empty());
  }

  ScopedFile f_src_ro =
      OpenMemoryImage(src_img, H5F_ACC_RDWR, "copy_edge_src_ro.h5");
  ScopedFile f_dst_rdonly =
      OpenMemoryImage(rdonly_dst_img, H5F_ACC_RDONLY, "copy_edge_rdonly_dst.h5");
  ASSERT_TRUE(f_src_ro.valid());
  ASSERT_TRUE(f_dst_rdonly.valid());

  ScopedPlist sohm_fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(sohm_fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(sohm_fcpl, 1), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(
                sohm_fcpl, 0, H5O_SHMESG_DTYPE_FLAG | H5O_SHMESG_SDSPACE_FLAG, 1),
            0);
  ScopedPlist sohm_fapl = MakeCoreFapl();
  ASSERT_TRUE(sohm_fapl.valid());
  ScopedFile f_dst_sohm(
      H5Fcreate("copy_edge_sohm_dst.h5", H5F_ACC_TRUNC, sohm_fcpl, sohm_fapl));
  ASSERT_TRUE(f_dst_sohm.valid());

  ScopedGroup dense_src_f = CreateGroupWithAttrProps(
      f_src_ro, "/dense_src", H5P_CRT_ORDER_TRACKED, /*max_compact=*/1,
      /*min_dense=*/0);
  ASSERT_TRUE(dense_src_f.valid());
  WriteIntAttr(dense_src_f, "ds1", 1);
  WriteIntAttr(dense_src_f, "ds2", 2);

  ScopedAttr src_vlen_id(H5Aopen(f_src_ro, "a_vlen", H5P_DEFAULT));
  ScopedAttr src_comm_id(H5Aopen(f_src_ro, "a_comm", H5P_DEFAULT));
  ScopedAttr src_i32_id(H5Aopen(f_src_ro, "a_i32", H5P_DEFAULT));
  ASSERT_TRUE(src_vlen_id.valid());
  ASSERT_TRUE(src_comm_id.valid());
  ASSERT_TRUE(src_i32_id.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  constexpr hsize_t kVecDims[2] = {2, 3};
  ScopedSpace vec_space(H5Screate_simple(2, kVecDims, nullptr));
  ScopedType comp_dt(H5Tcreate(H5T_COMPOUND, sizeof(CompoundPair)));
  ASSERT_GE(H5Tinsert(comp_dt, "a", HOFFSET(CompoundPair, a), H5T_NATIVE_INT32),
            0);
  ASSERT_GE(
      H5Tinsert(comp_dt, "b", HOFFSET(CompoundPair, b), H5T_NATIVE_DOUBLE), 0);
  ScopedType vlen_i32(H5Tvlen_create(H5T_NATIVE_INT32));
  ScopedType zero_memb_comp(H5Tcreate(H5T_COMPOUND, sizeof(hvl_t)));
  ASSERT_GE(H5Tinsert(zero_memb_comp, "v", 0, vlen_i32), 0);

  {
    ScopedApiContext ctx;
    ScopedErrorSilence silence;

    H5F_t* src_f = FileStructFromId(f_src_ro);
    H5F_t* dst_rdonly_f = FileStructFromId(f_dst_rdonly);
    H5F_t* dst_sohm_f = FileStructFromId(f_dst_sohm);
    ASSERT_NE(src_f, nullptr);
    ASSERT_NE(dst_rdonly_f, nullptr);
    ASSERT_NE(dst_sohm_f, nullptr);
    ASSERT_EQ(dst_rdonly_f->shared->flags & H5F_ACC_RDWR, 0U);

    auto* src_vlen_ptr =
        static_cast<H5A_t*>(H5VL_object_verify(src_vlen_id, H5I_ATTR));
    auto* src_comm_ptr =
        static_cast<H5A_t*>(H5VL_object_verify(src_comm_id, H5I_ATTR));
    auto* src_i32_ptr =
        static_cast<H5A_t*>(H5VL_object_verify(src_i32_id, H5I_ATTR));
    ASSERT_NE(src_vlen_ptr, nullptr);
    ASSERT_NE(src_comm_ptr, nullptr);
    ASSERT_NE(src_i32_ptr, nullptr);

    H5O_copy_t cpy_info{};
    cpy_info.max_depth = -1;
    cpy_info.map_list = H5SL_create(H5SL_TYPE_OBJ, nullptr);
    ASSERT_NE(cpy_info.map_list, nullptr);
    absl::Cleanup destroy_map = [&] {
      H5SL_destroy(cpy_info.map_list, FreeNoopCb, nullptr);
    };

    bool recompute = false;

    // 1. Line 2168: H5T_set_loc failure in H5A__attr_copy_file.
    {
      auto* zero_comp_ptr = static_cast<H5T_t*>(H5I_object(zero_memb_comp));
      H5T_t* memb = zero_comp_ptr->shared->u.compnd.memb[0].type;
      const size_t saved_sz = memb->shared->size;
      memb->shared->size = 0;
      absl::Cleanup restore_sz = [&] { memb->shared->size = saved_sz; };

      H5A_t synth_attr = *src_i32_ptr;
      H5A_shared_t synth_sh = *src_i32_ptr->shared;
      synth_sh.dt = zero_comp_ptr;
      synth_sh.data = nullptr;
      synth_sh.data_size = 0;
      synth_attr.shared = &synth_sh;
      ExpectNullAttr(
          H5A__attr_copy_file(&synth_attr, dst_sohm_f, &recompute, &cpy_info));
    }

    // 2. Lines 2195, 2197: H5SM_try_share failure with bogus dst sohm_addr.
    {
      const haddr_t saved_sohm = dst_sohm_f->shared->sohm_addr;
      dst_sohm_f->shared->sohm_addr = static_cast<haddr_t>(0x7ffffa00ULL);
      absl::Cleanup restore_sohm = [&] {
        dst_sohm_f->shared->sohm_addr = saved_sohm;
      };

      H5A_t synth_attr = *src_i32_ptr;
      H5A_shared_t synth_sh = *src_i32_ptr->shared;
      synth_sh.dt = static_cast<H5T_t*>(H5I_object(comp_dt));
      synth_sh.data = nullptr;
      synth_sh.data_size = 0;
      synth_attr.shared = &synth_sh;
      ExpectNullAttr(
          H5A__attr_copy_file(&synth_attr, dst_sohm_f, &recompute, &cpy_info));

      H5A_t synth_comm = *src_comm_ptr;
      H5A_shared_t synth_comm_sh = *src_comm_ptr->shared;
      synth_comm_sh.ds = static_cast<H5S_t*>(H5I_object(vec_space));
      synth_comm_sh.data = nullptr;
      synth_comm_sh.data_size = 0;
      synth_comm.shared = &synth_comm_sh;
      ExpectNullAttr(
          H5A__attr_copy_file(&synth_comm, dst_sohm_f, &recompute, &cpy_info));
    }

    // 3. Line 2215: invalid extent.nelem in H5A__attr_copy_file.
    {
      const hsize_t saved_nelem = src_i32_ptr->shared->ds->extent.nelem;
      src_i32_ptr->shared->ds->extent.nelem = static_cast<hsize_t>(-1);
      absl::Cleanup restore_nelem = [&] {
        src_i32_ptr->shared->ds->extent.nelem = saved_nelem;
      };
      ExpectNullAttr(
          H5A__attr_copy_file(src_i32_ptr, dst_sohm_f, &recompute, &cpy_info));
    }

    // 4. Line 2220: dst_dt_size == 0 (integer datatype copy with size = 0).
    {
      H5T_t* zero_i32 =
          H5T_copy(src_i32_ptr->shared->dt, H5T_COPY_TRANSIENT);
      ASSERT_NE(zero_i32, nullptr);
      absl::Cleanup close_zero_i32 = [&] { H5T_close(zero_i32); };
      zero_i32->shared->size = 0;

      H5A_t synth_attr = *src_i32_ptr;
      H5A_shared_t synth_sh = *src_i32_ptr->shared;
      synth_sh.dt = zero_i32;
      synth_sh.data = nullptr;
      synth_sh.data_size = 0;
      synth_attr.shared = &synth_sh;
      ExpectNullAttr(
          H5A__attr_copy_file(&synth_attr, dst_sohm_f, &recompute, &cpy_info));
    }

    // 5. Line 2254 (src_dt_size == 0) & line 2264 (data_size < src_dt_size).
    {
      H5T_t* vlen_disk =
          H5T_copy(src_vlen_ptr->shared->dt, H5T_COPY_TRANSIENT);
      ASSERT_NE(vlen_disk, nullptr);
      absl::Cleanup close_vlen_disk = [&] { H5T_close(vlen_disk); };
      ASSERT_GE(H5T_set_loc(vlen_disk, H5F_VOL_OBJ(src_f), H5T_LOC_DISK), 0);

      uint8_t dummy_data[16] = {};
      H5A_t synth_attr = *src_vlen_ptr;
      H5A_shared_t synth_sh = *src_vlen_ptr->shared;
      synth_sh.dt = vlen_disk;
      synth_sh.data = dummy_data;
      synth_attr.shared = &synth_sh;

      // 2264: data_size = 1 (< disk vlen size = 12).
      synth_sh.data_size = 1;
      ExpectNullAttr(
          H5A__attr_copy_file(&synth_attr, dst_sohm_f, &recompute, &cpy_info));

      // 2254: vlen_disk size temporarily 0.
      const size_t saved_disk_sz = vlen_disk->shared->size;
      vlen_disk->shared->size = 0;
      synth_sh.data_size = 16;
      ExpectNullAttr(
          H5A__attr_copy_file(&synth_attr, dst_sohm_f, &recompute, &cpy_info));
      vlen_disk->shared->size = saved_disk_sz;
    }

    // 6. Line 2294: disk VLEN attribute with seq_len=1 and bogus blob addr fails
    // H5T_convert(src -> mem).
    {
      H5T_t* vlen_disk =
          H5T_copy(src_vlen_ptr->shared->dt, H5T_COPY_TRANSIENT);
      ASSERT_NE(vlen_disk, nullptr);
      absl::Cleanup close_vlen_disk = [&] { H5T_close(vlen_disk); };
      ASSERT_GE(H5T_set_loc(vlen_disk, H5F_VOL_OBJ(src_f), H5T_LOC_DISK), 0);

      std::array<uint8_t, 32> bogus_vl{};
      bogus_vl[0] = 1;  // seq_len = 1 (LE uint32); rest non-zero bogus blob ID.
      for (size_t i = 4; i < vlen_disk->shared->size; ++i) {
        bogus_vl[i] = 0xff;
      }
      H5A_t synth_attr = *src_vlen_ptr;
      H5A_shared_t synth_sh = *src_vlen_ptr->shared;
      synth_sh.dt = vlen_disk;
      synth_sh.data = bogus_vl.data();
      synth_sh.data_size = vlen_disk->shared->size;
      synth_attr.shared = &synth_sh;
      ExpectNullAttr(
          H5A__attr_copy_file(&synth_attr, dst_sohm_f, &recompute, &cpy_info));
    }

    // 7. Line 2305: valid disk VLEN attribute copied into RDONLY dst fails in
    // H5T_convert(mem -> dst); use custom VLEN tracker so zero memory leaks.
    {
      ASSERT_GE(
          H5T_set_loc(src_vlen_ptr->shared->dt, H5F_VOL_OBJ(src_f), H5T_LOC_DISK),
          0);
      H5T_vlen_alloc_info_t saved_vl{};
      ASSERT_GE(H5CX_get_vlen_alloc_info(&saved_vl), 0);
      VlenAllocTracker tracker;
      ASSERT_GE(H5CX_set_vlen_alloc_info(TrackVlenAlloc, &tracker,
                                         TrackVlenFree, &tracker),
                0);
      absl::Cleanup restore_vl = [&] {
        H5CX_set_vlen_alloc_info(saved_vl.alloc_func, saved_vl.alloc_info,
                                 saved_vl.free_func, saved_vl.free_info);
      };
      ExpectNullAttr(
          H5A__attr_copy_file(src_vlen_ptr, dst_rdonly_f, &recompute, &cpy_info));
    }

    // 8. Lines 2401, 2411, 2413: H5A__attr_post_copy_file failure paths.
    H5G_loc_t src_root_loc{};
    H5G_loc_t dst_sohm_loc{};
    ASSERT_GE(H5G_loc(f_src_ro, &src_root_loc), 0);
    ASSERT_GE(H5G_loc(f_dst_sohm, &dst_sohm_loc), 0);

    H5A_t* copied_i32 =
        H5A__attr_copy_file(src_i32_ptr, dst_sohm_f, &recompute, &cpy_info);
    ASSERT_NE(copied_i32, nullptr);
    absl::Cleanup close_copied_i32 = [&] { H5A__close(copied_i32); };

    H5A_t* copied_comm =
        H5A__attr_copy_file(src_comm_ptr, dst_sohm_f, &recompute, &cpy_info);
    ASSERT_NE(copied_comm, nullptr);
    absl::Cleanup close_copied_comm = [&] { H5A__close(copied_comm); };

    // Line 2401: committed datatype with bogus src oloc addr fails
    // H5O_copy_header_map.
    {
      const haddr_t saved_comm_addr = src_comm_ptr->shared->dt->oloc.addr;
      src_comm_ptr->shared->dt->oloc.addr = static_cast<haddr_t>(0x7ffff900ULL);
      absl::Cleanup restore_comm = [&] {
        src_comm_ptr->shared->dt->oloc.addr = saved_comm_addr;
      };
      EXPECT_LT(H5A__attr_post_copy_file(src_root_loc.oloc, src_comm_ptr,
                                         dst_sohm_loc.oloc, copied_comm,
                                         &cpy_info),
                0);
    }

    // Lines 2411, 2413: deferred SOHM datatype & dataspace sharing failure.
    {
      const haddr_t saved_sohm = dst_sohm_f->shared->sohm_addr;
      dst_sohm_f->shared->sohm_addr = static_cast<haddr_t>(0x7ffff800ULL);
      absl::Cleanup restore_sohm = [&] {
        dst_sohm_f->shared->sohm_addr = saved_sohm;
      };

      const H5O_shared_t saved_dt_sh = copied_i32->shared->dt->sh_loc;
      const H5O_shared_t saved_ds_sh = copied_i32->shared->ds->extent.sh_loc;
      absl::Cleanup restore_sh = [&] {
        copied_i32->shared->dt->sh_loc = saved_dt_sh;
        copied_i32->shared->ds->extent.sh_loc = saved_ds_sh;
      };

      copied_i32->shared->dt->sh_loc.type = H5O_SHARE_TYPE_HERE;
      EXPECT_LT(H5A__attr_post_copy_file(src_root_loc.oloc, src_i32_ptr,
                                         dst_sohm_loc.oloc, copied_i32,
                                         &cpy_info),
                0);

      copied_i32->shared->dt->sh_loc.type = H5O_SHARE_TYPE_UNSHARED;
      copied_i32->shared->ds->extent.sh_loc.type = H5O_SHARE_TYPE_HERE;
      EXPECT_LT(H5A__attr_post_copy_file(src_root_loc.oloc, src_i32_ptr,
                                         dst_sohm_loc.oloc, copied_i32,
                                         &cpy_info),
                0);
    }

    // 9. Line 2487: H5A__dense_post_copy_file_all fails inserting into bogus
    // destination dense storage (bad_dst_ainfo.fheap_addr).
    H5G_loc_t d_src_loc{};
    ASSERT_GE(H5G_loc(dense_src_f, &d_src_loc), 0);
    H5O_t* d_src_oh = H5O_protect(d_src_loc.oloc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(d_src_oh, nullptr);
    H5O_ainfo_t d_src_ainfo{};
    const htri_t got_ainfo =
        H5A__get_ainfo(src_f, d_src_oh, &d_src_ainfo);
    ASSERT_GE(H5O_unprotect(d_src_loc.oloc, d_src_oh, H5AC__NO_FLAGS_SET), 0);
    ASSERT_GT(got_ainfo, 0);

    H5O_ainfo_t bad_dst_ainfo = d_src_ainfo;
    bad_dst_ainfo.fheap_addr = static_cast<haddr_t>(0x7ffff700ULL);
    EXPECT_LT(
        H5A__dense_post_copy_file_all(d_src_loc.oloc, &d_src_ainfo,
                                      dst_sohm_loc.oloc, &bad_dst_ainfo,
                                      &cpy_info),
        0);

    // 10. Lines 2088, 2323, 2473: H5A__set_version bounds failure inside
    // H5A__attr_copy_file and H5A__dense_post_copy_file_cb.
    {
      const H5F_libver_t saved_low = dst_sohm_f->shared->low_bound;
      const H5F_libver_t saved_high = dst_sohm_f->shared->high_bound;
      dst_sohm_f->shared->low_bound = H5F_LIBVER_LATEST;
      dst_sohm_f->shared->high_bound = H5F_LIBVER_EARLIEST;
      absl::Cleanup restore_bounds = [&] {
        dst_sohm_f->shared->low_bound = saved_low;
        dst_sohm_f->shared->high_bound = saved_high;
      };
      ExpectNullAttr(
          H5A__attr_copy_file(src_i32_ptr, dst_sohm_f, &recompute, &cpy_info));
      EXPECT_LT(
          H5A__dense_post_copy_file_all(d_src_loc.oloc, &d_src_ainfo,
                                        dst_sohm_loc.oloc, &bad_dst_ainfo,
                                        &cpy_info),
          0);
    }
  }
}

}  // namespace
}  // namespace hdf5
