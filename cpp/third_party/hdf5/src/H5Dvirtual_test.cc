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
// Tests Virtual Dataset (VDS) mapping validation, layout encoding/decoding,
// printf source name expansion, dynamic unlimited extents, R-tree spatial
// indexing, fill/flush/refresh semantics, and corrupt heap metadata handling
// (third_party/hdf5/src/H5Dvirtual.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dvirtual.c

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits debug fields from metadata
// structures pulled by H5private.h and H5Dpkg.h.
#ifndef NDEBUG
#define H5DVIRTUAL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5D_FRIEND
#define H5F_FRIEND
#define H5S_FRIEND
// H5B2private.h (pulled by H5Dpkg.h) and H5Fpkg.h use `try` as a parameter
// name, colliding with C++ keyword `try`.
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dpkg.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5HGprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5RTprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5DVIRTUAL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DVIRTUAL_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::Each;
using ::testing::ElementsAre;

// RAII wrapper for pushing/popping an HDF5 internal API context.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t dxpl_id = H5P_DEFAULT) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (dxpl_id != H5P_DEFAULT) {
        H5CX_set_dxpl(dxpl_id);
      }
    }
  }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

// Creates an OS-backed HDF5 file with an int32 contiguous or chunked dataset
// populated with `data`.
void WriteInt32DatasetFile(const std::string& path, const char* dset_name,
                           const std::vector<hsize_t>& dims,
                           const std::vector<hsize_t>& maxdims,
                           const std::vector<hsize_t>& chunk_dims,
                           const std::vector<int32_t>& data,
                           bool create_new = true) {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl, H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);

  ScopedFile file;
  if (create_new || !std::filesystem::exists(path)) {
    file.reset(H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  } else {
    file.reset(H5Fopen(path.c_str(), H5F_ACC_RDWR, fapl));
  }
  ASSERT_TRUE(file.valid());

  ScopedSpace space(
      H5Screate_simple(static_cast<int>(dims.size()), dims.data(),
                       maxdims.empty() ? nullptr : maxdims.data()));
  ASSERT_TRUE(space.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  if (!chunk_dims.empty()) {
    ASSERT_GE(H5Pset_chunk(dcpl, static_cast<int>(chunk_dims.size()),
                           chunk_dims.data()),
              0);
  }

  ScopedDataset dset(H5Dcreate2(file, dset_name, H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  if (!data.empty()) {
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       data.data()),
              0);
  }
}

// Serializes `space_id` into `out` via internal H5S_SELECT_SERIALIZE.
void AppendSerializedSelection(hid_t space_id, std::vector<uint8_t>* out) {
  auto* space = static_cast<H5S_t*>(H5I_object_verify(space_id, H5I_DATASPACE));
  ASSERT_NE(space, nullptr);
  const hssize_t sz = H5S_SELECT_SERIAL_SIZE(space);
  ASSERT_GT(sz, 0);
  const size_t old_size = out->size();
  out->resize(old_size + static_cast<size_t>(sz));
  uint8_t* p = out->data() + old_size;
  ASSERT_GE(H5S_SELECT_SERIALIZE(space, &p), 0);
  ASSERT_EQ(static_cast<size_t>(p - out->data()), out->size());
}

void AppendEncodedLength(H5F_t* f, hsize_t val, std::vector<uint8_t>* out) {
  const size_t len = H5F_SIZEOF_SIZE(f);
  const size_t old_size = out->size();
  out->resize(old_size + len);
  uint8_t* p = out->data() + old_size;
  H5F_ENCODE_LENGTH(f, p, val);
}

void AppendCString(const std::string& s, std::vector<uint8_t>* out,
                   bool include_nul = true) {
  out->insert(out->end(), s.begin(), s.end());
  if (include_nul) {
    out->push_back(0);
  }
}

void AppendChecksum(std::vector<uint8_t>* out) {
  const uint32_t csum = H5_checksum_metadata(out->data(), out->size(), 0);
  const size_t old_size = out->size();
  out->resize(old_size + 4);
  uint8_t* p = out->data() + old_size;
  UINT32ENCODE(p, csum);
}

TEST(H5DvirtualTest, MappingValidationPrePostAndConstruct) {
  ScopedSpace v4(
      H5Screate_simple(1, std::array<hsize_t, 1>{4}.data(), nullptr));
  ScopedSpace s4(
      H5Screate_simple(1, std::array<hsize_t, 1>{4}.data(), nullptr));
  ScopedSpace s5(
      H5Screate_simple(1, std::array<hsize_t, 1>{5}.data(), nullptr));
  ASSERT_TRUE(v4.valid() && s4.valid() && s5.valid());

  // Point selections are rejected in both virtual and source dataspaces.
  {
    ScopedErrorSilence silence;
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ScopedSpace vpts(H5Scopy(v4));
    const std::array<hsize_t, 2> coords = {0, 2};
    ASSERT_GE(H5Sselect_elements(vpts, H5S_SELECT_SET, 2, coords.data()), 0);
    EXPECT_LT(H5Pset_virtual(dcpl, vpts, ".", "src", s4), 0);

    ScopedSpace spts(H5Scopy(s4));
    ASSERT_GE(H5Sselect_elements(spts, H5S_SELECT_SET, 2, coords.data()), 0);
    EXPECT_LT(H5Pset_virtual(dcpl, v4, ".", "src", spts), 0);

    // Limited element count mismatch.
    EXPECT_LT(H5Pset_virtual(dcpl, v4, ".", "src", s5), 0);
  }

  // Unlimited-to-unlimited: non-unlimited count mismatch and >1 unlim dims.
  {
    ScopedErrorSilence silence;
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    const std::array<hsize_t, 2> d_a = {1, 4};
    const std::array<hsize_t, 2> d_b = {1, 5};
    const std::array<hsize_t, 2> max_unlim1 = {H5S_UNLIMITED, 4};
    const std::array<hsize_t, 2> max_unlim1b = {H5S_UNLIMITED, 5};
    const std::array<hsize_t, 2> start0 = {0, 0};
    const std::array<hsize_t, 2> cnt_u_a = {H5S_UNLIMITED, 4};
    const std::array<hsize_t, 2> cnt_u_b = {H5S_UNLIMITED, 5};
    ScopedSpace va(H5Screate_simple(2, d_a.data(), max_unlim1.data()));
    ScopedSpace sb(H5Screate_simple(2, d_b.data(), max_unlim1b.data()));
    ASSERT_GE(H5Sselect_hyperslab(va, H5S_SELECT_SET, start0.data(), nullptr,
                                  cnt_u_a.data(), nullptr),
              0);
    ASSERT_GE(H5Sselect_hyperslab(sb, H5S_SELECT_SET, start0.data(), nullptr,
                                  cnt_u_b.data(), nullptr),
              0);
    EXPECT_LT(H5Pset_virtual(dcpl, va, ".", "src", sb), 0);

    // Direct pre-check where `select.num_elem == H5S_UNLIMITED` has no
    // `num_elem_non_unlim` callback (H5S_SEL_ALL) so
    // `H5S_get_select_num_elem_non_unlim` fails cleanly.
    ScopedSpace v2u(H5Scopy(s4));
    ASSERT_TRUE(v2u.valid());
    auto* v2u_p = static_cast<H5S_t*>(H5I_object_verify(v2u, H5I_DATASPACE));
    auto* va_p = static_cast<H5S_t*>(H5I_object_verify(va, H5I_DATASPACE));
    ASSERT_NE(v2u_p, nullptr);
    ASSERT_NE(va_p, nullptr);
    const hsize_t orig_nelmts = v2u_p->select.num_elem;
    v2u_p->select.num_elem = H5S_UNLIMITED;
    EXPECT_LT(
        H5D_virtual_check_mapping_pre(v2u_p, va_p, H5O_VIRTUAL_STATUS_USER), 0);
    EXPECT_LT(
        H5D_virtual_check_mapping_pre(va_p, v2u_p, H5O_VIRTUAL_STATUS_USER), 0);
    v2u_p->select.num_elem = orig_nelmts;
  }

  // Printf mapping post-checks and format errors.
  // Note: H5Pset_virtual leaves an internal hash-table entry dangling if
  // H5D_virtual_check_mapping_post fails, so use a fresh dcpl per negative
  // call.
  {
    ScopedErrorSilence silence;
    const std::array<hsize_t, 1> d0 = {0};
    const std::array<hsize_t, 1> maxu = {H5S_UNLIMITED};
    const std::array<hsize_t, 1> start0 = {0};
    const std::array<hsize_t, 1> cnt_u = {H5S_UNLIMITED};
    const std::array<hsize_t, 1> blk4 = {4};
    ScopedSpace vu_hyper(H5Screate_simple(1, d0.data(), maxu.data()));
    ASSERT_GE(H5Sselect_hyperslab(vu_hyper, H5S_SELECT_SET, start0.data(),
                                  blk4.data(), cnt_u.data(), blk4.data()),
              0);

    // Printf selection without %b specifier.
    {
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      EXPECT_LT(H5Pset_virtual(dcpl, vu_hyper, "static.h5", "static_ds", s4),
                0);
    }

    // Printf block element count != limited source element count.
    {
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      EXPECT_LT(H5Pset_virtual(dcpl, vu_hyper, "f_%b.h5", "src", s5), 0);
    }

    // Limited selection with %b specifier (disallowed in non-printf).
    {
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      EXPECT_LT(H5Pset_virtual(dcpl, v4, "f_%b.h5", "src", s4), 0);
    }
    {
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      EXPECT_LT(H5Pset_virtual(dcpl, v4, ".", "d_%b", s4), 0);
    }

    // Invalid format specifiers in source file or dataset name.
    {
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      EXPECT_LT(H5Pset_virtual(dcpl, vu_hyper, "bad_%z.h5", "src", s4), 0);
    }
    {
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      EXPECT_LT(H5Pset_virtual(dcpl, vu_hyper, ".", "bad_%", s4), 0);
    }

    // Direct post-check: unlimited virtual selection (H5S_SEL_ALL with
    // synthetic H5S_UNLIMITED nelmts) paired with %b specifier is rejected
    // because printf mappings require hyperslab selections.
    {
      ScopedApiContext ctx;
      ScopedSpace vu_all(H5Screate_simple(1, d0.data(), maxu.data()));
      ASSERT_GE(H5Sselect_all(vu_all), 0);
      auto* vu_all_p =
          static_cast<H5S_t*>(H5I_object_verify(vu_all, H5I_DATASPACE));
      auto* s4_p = static_cast<H5S_t*>(H5I_object_verify(s4, H5I_DATASPACE));
      ASSERT_NE(vu_all_p, nullptr);
      ASSERT_NE(s4_p, nullptr);
      const hsize_t orig_nelmts = vu_all_p->select.num_elem;
      vu_all_p->select.num_elem = H5S_UNLIMITED;

      H5O_storage_virtual_ent_t ent;
      std::memset(&ent, 0, sizeof(ent));
      ent.source_dset.virtual_select = vu_all_p;
      ent.source_select = s4_p;
      ent.psfn_nsubs = 1;
      ent.source_space_status = H5O_VIRTUAL_STATUS_USER;
      EXPECT_LT(H5D_virtual_check_mapping_post(&ent), 0);
      vu_all_p->select.num_elem = orig_nelmts;
    }
  }

  // min_dims checks on H5Dcreate2 and H5Dset_extent, plus H5S_ALL / H5S_NONE.
  {
    ScopedFile file = CreateInMemoryFile("vds_mindims.h5");
    ASSERT_TRUE(file.valid());

    // Source dataset [4].
    ScopedDataset src(H5Dcreate2(file, "src", H5T_NATIVE_INT32, s4, H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(src.valid());
    const std::array<int32_t, 4> vals = {10, 20, 30, 40};
    ASSERT_GE(H5Dwrite(src, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       vals.data()),
              0);

    // Mapping using H5S_SEL_ALL and H5S_SEL_NONE triggers update_min_dims
    // early exit without setting min_dims.
    ScopedPlist dcpl_all(H5Pcreate(H5P_DATASET_CREATE));
    ScopedSpace s_none(H5Scopy(s4));
    ScopedSpace s_zero(H5Scopy(s4));
    ASSERT_GE(H5Sselect_none(s_none), 0);
    ASSERT_GE(H5Sselect_none(s_zero), 0);
    ASSERT_GE(H5Pset_virtual(dcpl_all, s_none, ".", "src", s_zero), 0);
    ASSERT_GE(H5Pset_virtual(dcpl_all, v4, ".", "src", s4), 0);
    ScopedDataset d_all(H5Dcreate2(file, "v_all", H5T_NATIVE_INT32, v4,
                                   H5P_DEFAULT, dcpl_all, H5P_DEFAULT));
    ASSERT_TRUE(d_all.valid());

    // Hyperslab mapping at [4..7] requires min_dims >= 8.
    const std::array<hsize_t, 1> d8 = {8};
    const std::array<hsize_t, 1> max16 = {16};
    const std::array<hsize_t, 1> start4 = {4};
    const std::array<hsize_t, 1> cnt4 = {4};
    ScopedSpace v8(H5Screate_simple(1, d8.data(), max16.data()));
    ASSERT_GE(H5Sselect_hyperslab(v8, H5S_SELECT_SET, start4.data(), nullptr,
                                  cnt4.data(), nullptr),
              0);
    ScopedPlist dcpl_min(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl_min, v8, ".", "src", s4), 0);

    {
      ScopedErrorSilence silence;
      // Creating with size [4] fails check_min_dims (< 8).
      ScopedDataset fail_create(H5Dcreate2(file, "v_small", H5T_NATIVE_INT32,
                                           v4, H5P_DEFAULT, dcpl_min,
                                           H5P_DEFAULT));
      EXPECT_FALSE(fail_create.valid());
    }

    ScopedSpace v_init8(H5Screate_simple(1, d8.data(), max16.data()));
    ScopedDataset v_ok(H5Dcreate2(file, "v_min", H5T_NATIVE_INT32, v_init8,
                                  H5P_DEFAULT, dcpl_min, H5P_DEFAULT));
    ASSERT_TRUE(v_ok.valid());
    {
      ScopedErrorSilence silence;
      // Shrinking extent below min_dims fails check_min_dims.
      const std::array<hsize_t, 1> d6 = {6};
      EXPECT_LT(H5Dset_extent(v_ok, d6.data()), 0);
    }
  }

  // Layout version out-of-bounds in H5D__virtual_construct.
  {
    ScopedErrorSilence silence;
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl, H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
              0);
    ScopedFile file(
        H5Fcreate("vds_v18_bounds.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl, v4, ".", "src", s4), 0);
    ScopedDataset dset(H5Dcreate2(file, "v_fail", H5T_NATIVE_INT32, v4,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
    EXPECT_FALSE(dset.valid());
  }
}

TEST(H5DvirtualTest, ParseAndBuildSourceNames) {
  ScopedApiContext ctx;
  // Direct tests for H5D_virtual_parse_source_name & str_append realloc.
  {
    H5O_storage_virtual_name_seg_t* parsed = nullptr;
    size_t static_len = 0;
    size_t nsubs = 0;

    // Multiple %% escapes where second segment forces buffer reallocation.
    ASSERT_GE(H5D_virtual_parse_source_name(
                  "a%%very_long_segment_01234567890123456789%%tail", &parsed,
                  &static_len, &nsubs),
              0);
    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(nsubs, 0u);
    EXPECT_STREQ(parsed->name_segment,
                 "a%very_long_segment_01234567890123456789%tail");
    EXPECT_EQ(static_len, std::strlen(parsed->name_segment));
    EXPECT_GE(H5D_virtual_free_parsed_name(parsed), 0);
    parsed = nullptr;

    // Escaped %% right at end so trailing `*p == '\0'`.
    ASSERT_GE(
        H5D_virtual_parse_source_name("end%%", &parsed, &static_len, &nsubs),
        0);
    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(nsubs, 0u);
    EXPECT_STREQ(parsed->name_segment, "end%");
    EXPECT_GE(H5D_virtual_free_parsed_name(parsed), 0);
    parsed = nullptr;

    // Invalid specifiers return FAIL and clean up intermediate segments.
    ScopedErrorSilence silence;
    EXPECT_LT(H5D_virtual_parse_source_name("pre_%b_bad_%q", &parsed,
                                            &static_len, &nsubs),
              0);
    EXPECT_EQ(parsed, nullptr);
    EXPECT_LT(H5D_virtual_parse_source_name("trailing_%", &parsed, &static_len,
                                            &nsubs),
              0);
    EXPECT_EQ(parsed, nullptr);
  }

  // End-to-end printf reads across multi-digit block numbers (>= 10),
  // consecutive %b%b, leading %b, trailing %b, and static literal %% names.
  TempDir dir("vds_printf_names");
  const std::string esc_file = dir.child("src%lit.h5").string();
  const std::vector<hsize_t> d2 = {2};
  for (int b = 0; b <= 12; ++b) {
    const std::string fname = dir.child(absl::StrCat("f_", b, ".h5")).string();
    const std::string dname = absl::StrCat("/d", b, "_", b);
    WriteInt32DatasetFile(fname, dname.c_str(), d2, {}, {},
                          {b * 10 + 1, b * 10 + 2});
    // Also put datasets in escaped-%% single file (accessed with "%b").
    const std::string esc_dname = absl::StrCat("/pre_", b);
    WriteInt32DatasetFile(esc_file, esc_dname.c_str(), d2, {}, {},
                          {100 + b, 200 + b}, /*create_new=*/b == 0);
  }

  const std::string vds_path = dir.child("vds_names.h5").string();
  {
    ScopedFile vfile(
        H5Fcreate(vds_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(vfile.valid());

    const std::array<hsize_t, 1> d0 = {0};
    const std::array<hsize_t, 1> dmax = {H5S_UNLIMITED};
    const std::array<hsize_t, 1> start0 = {0};
    const std::array<hsize_t, 1> stride2 = {2};
    const std::array<hsize_t, 1> cnt_u = {H5S_UNLIMITED};
    const std::array<hsize_t, 1> blk2 = {2};

    ScopedSpace s2(H5Screate_simple(1, d2.data(), nullptr));
    ScopedSpace vu1(H5Screate_simple(1, d0.data(), dmax.data()));
    ASSERT_GE(H5Sselect_hyperslab(vu1, H5S_SELECT_SET, start0.data(),
                                  stride2.data(), cnt_u.data(), blk2.data()),
              0);

    // 1) Printf in both file and consecutive %b%b in dataset name.
    ScopedPlist dcpl1(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(
        H5Pset_virtual(dcpl1, vu1, dir.child("f_%b.h5").c_str(), "/d%b_%b", s2),
        0);
    ScopedDataset v1(H5Dcreate2(vfile, "v_multi", H5T_NATIVE_INT32, vu1,
                                H5P_DEFAULT, dcpl1, H5P_DEFAULT));
    ASSERT_TRUE(v1.valid());

    // 2) Escaped %% static file name (parsed_name != nullptr, nsubs == 0) +
    // trailing %b dataset name.
    ScopedPlist dcpl2(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl2, vu1, dir.child("src%%lit.h5").c_str(),
                             "/pre_%b", s2),
              0);
    ScopedDataset v2(H5Dcreate2(vfile, "v_esc", H5T_NATIVE_INT32, vu1,
                                H5P_DEFAULT, dcpl2, H5P_DEFAULT));
    ASSERT_TRUE(v2.valid());

    // Copy v2 within the same file via H5Ocopy to exercise copy_parsed_name
    // and static parsed file_name handling in H5D__virtual_copy_layout.
    ASSERT_GE(
        H5Ocopy(vfile, "v_esc", vfile, "v_esc_copy", H5P_DEFAULT, H5P_DEFAULT),
        0);
  }

  ScopedFile vfile(H5Fopen(vds_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(vfile.valid());
  ScopedDataset v1(H5Dopen2(vfile, "v_multi", H5P_DEFAULT));
  ASSERT_TRUE(v1.valid());
  ScopedSpace sp1(H5Dget_space(v1));
  hsize_t cur_dim = 0;
  ASSERT_EQ(H5Sget_simple_extent_dims(sp1, &cur_dim, nullptr), 1);
  ASSERT_EQ(cur_dim, 26u);  // 13 blocks (0..12) * 2
  std::vector<int32_t> out1(26, -1);
  ASSERT_GE(
      H5Dread(v1, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, out1.data()),
      0);
  // Check block 0 and block 12 (multi-digit formatting).
  EXPECT_EQ(out1[0], 1);
  EXPECT_EQ(out1[1], 2);
  EXPECT_EQ(out1[24], 121);
  EXPECT_EQ(out1[25], 122);

  ScopedDataset v2(H5Dopen2(vfile, "v_esc_copy", H5P_DEFAULT));
  ASSERT_TRUE(v2.valid());
  ScopedSpace sp2(H5Dget_space(v2));
  ASSERT_EQ(H5Sget_simple_extent_dims(sp2, &cur_dim, nullptr), 1);
  ASSERT_EQ(cur_dim, 26u);
  std::vector<int32_t> out2(26, -1);
  ASSERT_GE(
      H5Dread(v2, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, out2.data()),
      0);
  EXPECT_EQ(out2[0], 100);
  EXPECT_EQ(out2[1], 200);
  EXPECT_EQ(out2[24], 112);
  EXPECT_EQ(out2[25], 212);
}

TEST(H5DvirtualTest, LayoutSerializationVersionsCopyAndDelete) {
  TempDir dir("vds_vers");
  const std::string ext_src_long =
      dir.child("external_long_source.h5").string();
  const std::string ext_src_short = dir.child("s.h5").string();
  WriteInt32DatasetFile(ext_src_long, "/long_dataset_name", {2}, {}, {},
                        {11, 22});
  WriteInt32DatasetFile(ext_src_short, "/d", {2}, {}, {}, {33, 44});

  const std::string f_v0_path = dir.child("v0.h5").string();
  const std::string f_v1_path = dir.child("v1.h5").string();
  const std::string f_copy_path = dir.child("copy_dst.h5").string();

  const std::array<hsize_t, 1> d2 = {2};
  const std::array<hsize_t, 1> d10 = {10};
  const std::array<hsize_t, 1> cnt2 = {2};
  ScopedSpace s2(H5Screate_simple(1, d2.data(), nullptr));
  ASSERT_TRUE(s2.valid());

  // 1) Empty VDS (0 mappings) + v0 encoding with V110 bounds (repeated names).
  {
    ScopedPlist fapl_v110(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_libver_bounds(fapl_v110, H5F_LIBVER_V110, H5F_LIBVER_V110),
              0);
    ScopedFile fv0(
        H5Fcreate(f_v0_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v110));
    ASSERT_TRUE(fv0.valid());

    // Source in "."
    ScopedDataset src(H5Dcreate2(fv0, "local_src", H5T_NATIVE_INT32, s2,
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    const std::array<int32_t, 2> lv = {55, 66};
    ASSERT_GE(H5Dwrite(src, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       lv.data()),
              0);

    // 0-mapping VDS: serial_list_hobjid remains HADDR_UNDEF.
    ScopedPlist dcpl_empty(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_layout(dcpl_empty, H5D_VIRTUAL), 0);
    ScopedDataset v_empty(H5Dcreate2(fv0, "v_empty", H5T_NATIVE_INT32, s2,
                                     H5P_DEFAULT, dcpl_empty, H5P_DEFAULT));
    ASSERT_TRUE(v_empty.valid());

    // v0 multi-mapping VDS with repeated file & dataset names.
    ScopedPlist dcpl_v0(H5Pcreate(H5P_DATASET_CREATE));
    ScopedSpace vs(H5Screate_simple(1, d10.data(), nullptr));
    const std::array<hsize_t, 1> off0 = {0};
    const std::array<hsize_t, 1> off2 = {2};
    const std::array<hsize_t, 1> off4 = {4};
    ASSERT_GE(H5Sselect_hyperslab(vs, H5S_SELECT_SET, off0.data(), nullptr,
                                  cnt2.data(), nullptr),
              0);
    ASSERT_GE(H5Pset_virtual(dcpl_v0, vs, ext_src_long.c_str(),
                             "/long_dataset_name", s2),
              0);
    ASSERT_GE(H5Sselect_hyperslab(vs, H5S_SELECT_SET, off2.data(), nullptr,
                                  cnt2.data(), nullptr),
              0);
    ASSERT_GE(H5Pset_virtual(dcpl_v0, vs, ext_src_long.c_str(),
                             "/long_dataset_name", s2),
              0);
    ASSERT_GE(H5Sselect_hyperslab(vs, H5S_SELECT_SET, off4.data(), nullptr,
                                  cnt2.data(), nullptr),
              0);
    ASSERT_GE(H5Pset_virtual(dcpl_v0, vs, ".", "/local_src", s2), 0);

    ScopedDataset v0(H5Dcreate2(fv0, "v0_dset", H5T_NATIVE_INT32, vs,
                                H5P_DEFAULT, dcpl_v0, H5P_DEFAULT));
    ASSERT_TRUE(v0.valid());
  }

  // Reopen v0 file, verify reads & delete empty/non-empty VDS
  // (H5D__virtual_delete).
  {
    ScopedFile fv0(H5Fopen(f_v0_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ASSERT_TRUE(fv0.valid());
    ScopedDataset v_empty(H5Dopen2(fv0, "v_empty", H5P_DEFAULT));
    std::array<int32_t, 2> eb = {7, 7};
    ASSERT_GE(H5Dread(v_empty, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      eb.data()),
              0);
    EXPECT_THAT(eb, ElementsAre(0, 0));
    v_empty.reset();
    ASSERT_GE(H5Ldelete(fv0, "v_empty", H5P_DEFAULT), 0);

    ScopedDataset v0(H5Dopen2(fv0, "v0_dset", H5P_DEFAULT));
    std::array<int32_t, 10> out{};
    ASSERT_GE(H5Dread(v0, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      out.data()),
              0);
    EXPECT_THAT(out, ElementsAre(11, 22, 11, 22, 55, 66, 0, 0, 0, 0));
    v0.reset();
    ASSERT_GE(H5Ldelete(fv0, "v0_dset", H5P_DEFAULT), 0);
  }

  // 2) V200..LATEST: single external mapping falls back to vers 0, while
  // multi-mapping with "." x2 + repeated long names + repeated short names
  // (<8B) picks vers 1 and exercises every flag combination on encode & decode.
  {
    ScopedPlist fapl_v2(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_libver_bounds(fapl_v2, H5F_LIBVER_V200, H5F_LIBVER_LATEST),
              0);
    ScopedFile fv1(
        H5Fcreate(f_v1_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v2));
    ASSERT_TRUE(fv1.valid());

    ScopedDataset src(H5Dcreate2(fv1, "local_long_dset", H5T_NATIVE_INT32, s2,
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    const std::array<int32_t, 2> lv = {77, 88};
    ASSERT_GE(H5Dwrite(src, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       lv.data()),
              0);

    // 1 external mapping under V200: block_size_1 > block_size -> vers 0.
    ScopedPlist dcpl_one_ext(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl_one_ext, s2, ext_src_short.c_str(), "/d", s2),
              0);
    ScopedDataset v_one(H5Dcreate2(fv1, "v_one_ext", H5T_NATIVE_INT32, s2,
                                   H5P_DEFAULT, dcpl_one_ext, H5P_DEFAULT));
    ASSERT_TRUE(v_one.valid());

    // Multi-mapping triggering vers 1: "." twice, long shared file/dset names,
    // plus repeated short (< 8 bytes w/ NUL) file & dset names (not shared).
    ScopedPlist dcpl_v1(H5Pcreate(H5P_DATASET_CREATE));
    const std::array<hsize_t, 1> d14 = {14};
    ScopedSpace vs(H5Screate_simple(1, d14.data(), nullptr));
    auto add_map = [&](hsize_t off, const char* fn, const char* dn) {
      ASSERT_GE(H5Sselect_hyperslab(vs, H5S_SELECT_SET, &off, nullptr,
                                    cnt2.data(), nullptr),
                0);
      ASSERT_GE(H5Pset_virtual(dcpl_v1, vs, fn, dn, s2), 0);
    };
    add_map(0, ".", "/local_long_dset");
    add_map(2, ".", "/local_long_dset");
    add_map(4, ext_src_long.c_str(), "/long_dataset_name");
    add_map(6, ext_src_long.c_str(), "/long_dataset_name");
    add_map(8, ext_src_short.c_str(), "/d");
    add_map(10, ext_src_short.c_str(), "/d");
    add_map(12, ".", "%%esc_not_used");  // tests parsed_source_dset_name copy

    // Add missing local dataset for off=12 so we can test copy/read cleanly.
    ScopedDataset esc_src(H5Dcreate2(fv1, "%esc_not_used", H5T_NATIVE_INT32, s2,
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    const std::array<int32_t, 2> ev = {90, 91};
    ASSERT_GE(H5Dwrite(esc_src, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       ev.data()),
              0);

    ScopedPlist dcpl_copy(H5Pcopy(dcpl_v1));
    ASSERT_TRUE(dcpl_copy.valid());

    ScopedDataset v1(H5Dcreate2(fv1, "v1_shared", H5T_NATIVE_INT32, vs,
                                H5P_DEFAULT, dcpl_copy, H5P_DEFAULT));
    ASSERT_TRUE(v1.valid());

    // Copy to another file via H5Ocopy -> H5D__virtual_copy -> store_layout.
    ScopedFile fdst(
        H5Fcreate(f_copy_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v2));
    ASSERT_TRUE(fdst.valid());
    ASSERT_GE(
        H5Ocopy(fv1, "v1_shared", fdst, "v1_copied", H5P_DEFAULT, H5P_DEFAULT),
        0);
    ASSERT_GE(H5Ocopy(fv1, "local_long_dset", fdst, "local_long_dset",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Ocopy(fv1, "%esc_not_used", fdst, "%esc_not_used", H5P_DEFAULT,
                      H5P_DEFAULT),
              0);
  }

  // Reopen both files and verify decoded v0 fallback & v1 shared entries.
  ScopedFile fv1(H5Fopen(f_v1_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(fv1.valid());
  ScopedDataset v_one(H5Dopen2(fv1, "v_one_ext", H5P_DEFAULT));
  std::array<int32_t, 2> out_one{};
  ASSERT_GE(H5Dread(v_one, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    out_one.data()),
            0);
  EXPECT_THAT(out_one, ElementsAre(33, 44));

  ScopedFile fdst(H5Fopen(f_copy_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(fdst.valid());
  ScopedDataset v1c(H5Dopen2(fdst, "v1_copied", H5P_DEFAULT));
  std::array<int32_t, 14> out_v1{};
  ASSERT_GE(H5Dread(v1c, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    out_v1.data()),
            0);
  EXPECT_THAT(out_v1, ElementsAre(77, 88, 77, 88, 11, 22, 11, 22, 33, 44, 33,
                                  44, 90, 91));
}

TEST(H5DvirtualTest, CorruptGlobalHeapLayoutLoadErrors) {
  ScopedFile file = CreateInMemoryFile("vds_corrupt_gh.h5");
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file, H5I_FILE));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ScopedErrorSilence silence;

  // Create valid serialized selections to reuse inside corrupt blocks.
  const std::array<hsize_t, 1> d2 = {2};
  const std::array<hsize_t, 1> d_u = {H5S_UNLIMITED};
  const std::array<hsize_t, 1> coord = {0};
  ScopedSpace s_all(H5Screate_simple(1, d2.data(), nullptr));
  ASSERT_GE(H5Sselect_all(s_all), 0);
  ScopedSpace s_pts(H5Screate_simple(1, d2.data(), nullptr));
  ASSERT_GE(H5Sselect_elements(s_pts, H5S_SELECT_SET, 1, coord.data()), 0);
  ScopedSpace s_unlim(H5Screate_simple(1, d2.data(), d_u.data()));
  const std::array<hsize_t, 1> start0 = {0};
  const std::array<hsize_t, 1> cntu = {H5S_UNLIMITED};
  ASSERT_GE(H5Sselect_hyperslab(s_unlim, H5S_SELECT_SET, start0.data(), nullptr,
                                cntu.data(), nullptr),
            0);

  // Valid zero-entry heap block (version=1, nused=0) succeeds without error.
  {
    std::vector<uint8_t> blk;
    blk.push_back(H5O_LAYOUT_VDS_GH_ENC_VERS_1);
    AppendEncodedLength(f, 0, &blk);
    AppendChecksum(&blk);

    H5O_layout_t layout;
    std::memset(&layout, 0, sizeof(layout));
    layout.type = H5D_VIRTUAL;
    layout.storage.type = H5D_VIRTUAL;
    layout.storage.u.virt.serial_list_hobjid.addr = HADDR_UNDEF;
    layout.storage.u.virt.source_fapl = H5I_INVALID_HID;
    layout.storage.u.virt.source_dapl = H5I_INVALID_HID;
    ASSERT_GE(H5HG_insert(f, blk.size(), blk.data(),
                          &layout.storage.u.virt.serial_list_hobjid),
              0);
    EXPECT_GE(H5D__virtual_load_layout(f, &layout), 0);
    EXPECT_EQ(layout.storage.u.virt.list_nused, 0u);
    EXPECT_EQ(layout.storage.u.virt.list, nullptr);
    EXPECT_GE(H5D__virtual_reset_layout(&layout), 0);
  }

  // Helper that inserts `blk`, runs H5D__virtual_load_layout, and expects FAIL.
  auto expect_load_fail = [&](const std::vector<uint8_t>& blk) {
    H5O_layout_t layout;
    std::memset(&layout, 0, sizeof(layout));
    layout.type = H5D_VIRTUAL;
    layout.storage.type = H5D_VIRTUAL;
    layout.storage.u.virt.serial_list_hobjid.addr = HADDR_UNDEF;
    layout.storage.u.virt.source_fapl = H5I_INVALID_HID;
    layout.storage.u.virt.source_dapl = H5I_INVALID_HID;
    ASSERT_GE(H5HG_insert(f, blk.size(), blk.data(),
                          &layout.storage.u.virt.serial_list_hobjid),
              0);
    EXPECT_LT(H5D__virtual_load_layout(f, &layout), 0);
    EXPECT_GE(H5D__virtual_reset_layout(&layout), 0);
  };

  // 1) Missing heap object (H5HG_read failure): reuse a valid heap addr w/ bad
  // idx.
  {
    const std::array<uint8_t, 4> dummy = {1, 2, 3, 4};
    H5HG_t good_id{};
    ASSERT_GE(H5HG_insert(f, dummy.size(), dummy.data(), &good_id), 0);
    H5O_layout_t layout;
    std::memset(&layout, 0, sizeof(layout));
    layout.type = H5D_VIRTUAL;
    layout.storage.type = H5D_VIRTUAL;
    layout.storage.u.virt.serial_list_hobjid.addr = good_id.addr;
    layout.storage.u.virt.serial_list_hobjid.idx = 9999;
    layout.storage.u.virt.source_fapl = H5I_INVALID_HID;
    layout.storage.u.virt.source_dapl = H5I_INVALID_HID;
    EXPECT_LT(H5D__virtual_load_layout(f, &layout), 0);
    EXPECT_GE(H5D__virtual_reset_layout(&layout), 0);
  }

  // 2) Bad version (> 1) and truncated entry count.
  {
    std::vector<uint8_t> blk;
    blk.push_back(2);  // invalid version
    AppendEncodedLength(f, 0, &blk);
    AppendChecksum(&blk);
    expect_load_fail(blk);

    std::vector<uint8_t> trunc_count = {0, 1, 2};  // < 1 + sizeof_size
    expect_load_fail(trunc_count);
  }

  // 3) Empty buffer after nused = 1.
  {
    std::vector<uint8_t> blk;
    blk.push_back(0);
    AppendEncodedLength(f, 1, &blk);
    expect_load_fail(blk);
  }

  // 4) Bad flags on vers = 1.
  {
    std::vector<uint8_t> blk;
    blk.push_back(1);
    AppendEncodedLength(f, 1, &blk);
    blk.push_back(0x80);
    expect_load_fail(blk);
  }

  // 5) SOURCE_FILE_SHARED truncated & orig >= i.
  {
    std::vector<uint8_t> blk;
    blk.push_back(1);
    AppendEncodedLength(f, 1, &blk);
    blk.push_back(H5O_LAYOUT_VDS_SOURCE_FILE_SHARED);
    blk.push_back(0);
    expect_load_fail(blk);

    blk.clear();
    blk.push_back(1);
    AppendEncodedLength(f, 1, &blk);
    blk.push_back(H5O_LAYOUT_VDS_SOURCE_FILE_SHARED);
    AppendEncodedLength(f, 0, &blk);  // orig == i (0 >= 0)
    expect_load_fail(blk);
  }

  // 6) Unterminated source file name.
  {
    std::vector<uint8_t> blk;
    blk.push_back(0);
    AppendEncodedLength(f, 1, &blk);
    AppendCString("no_nul_file", &blk, /*include_nul=*/false);
    expect_load_fail(blk);
  }

  // 7) SOURCE_DSET_SHARED truncated & orig >= i.
  {
    std::vector<uint8_t> blk;
    blk.push_back(1);
    AppendEncodedLength(f, 1, &blk);
    blk.push_back(H5O_LAYOUT_VDS_SOURCE_SAME_FILE |
                  H5O_LAYOUT_VDS_SOURCE_DSET_SHARED);
    blk.push_back(0);
    expect_load_fail(blk);

    blk.clear();
    blk.push_back(1);
    AppendEncodedLength(f, 1, &blk);
    blk.push_back(H5O_LAYOUT_VDS_SOURCE_SAME_FILE |
                  H5O_LAYOUT_VDS_SOURCE_DSET_SHARED);
    AppendEncodedLength(f, 1, &blk);  // orig > i (1 >= 0)
    expect_load_fail(blk);
  }

  // 8) Unterminated source dset name.
  {
    std::vector<uint8_t> blk;
    blk.push_back(0);
    AppendEncodedLength(f, 1, &blk);
    AppendCString(".", &blk);
    AppendCString("no_nul_dset", &blk, /*include_nul=*/false);
    expect_load_fail(blk);
  }

  // 9) Missing & corrupt source_select, and missing & corrupt virtual_select.
  {
    std::vector<uint8_t> blk;
    blk.push_back(0);
    AppendEncodedLength(f, 1, &blk);
    AppendCString(".", &blk);
    AppendCString("d", &blk);
    expect_load_fail(blk);  // missing source_select

    blk.push_back(0x7f);
    blk.push_back(0x7f);
    expect_load_fail(blk);  // corrupt source_select

    blk.clear();
    blk.push_back(0);
    AppendEncodedLength(f, 1, &blk);
    AppendCString(".", &blk);
    AppendCString("d", &blk);
    AppendSerializedSelection(s_all, &blk);
    expect_load_fail(blk);  // missing virtual_select

    blk.push_back(0x7f);
    blk.push_back(0x7f);
    expect_load_fail(blk);  // corrupt virtual_select
  }

  // 10) Invalid %z printf in loaded file/dset names.
  {
    std::vector<uint8_t> blk;
    blk.push_back(0);
    AppendEncodedLength(f, 1, &blk);
    AppendCString("bad_%z.h5", &blk);
    AppendCString("d", &blk);
    AppendSerializedSelection(s_all, &blk);
    AppendSerializedSelection(s_all, &blk);
    expect_load_fail(blk);

    blk.clear();
    blk.push_back(0);
    AppendEncodedLength(f, 1, &blk);
    AppendCString(".", &blk);
    AppendCString("bad_%z", &blk);
    AppendSerializedSelection(s_all, &blk);
    AppendSerializedSelection(s_all, &blk);
    expect_load_fail(blk);
  }

  // 11) Invalid mapping pre/post checks inside load_layout.
  {
    // Point selection in decoded selection fails check_mapping_pre.
    std::vector<uint8_t> blk;
    blk.push_back(0);
    AppendEncodedLength(f, 1, &blk);
    AppendCString(".", &blk);
    AppendCString("d", &blk);
    AppendSerializedSelection(s_pts, &blk);
    AppendSerializedSelection(s_all, &blk);
    AppendChecksum(&blk);
    expect_load_fail(blk);

    // Limited spaces with %b fails check_mapping_post.
    blk.clear();
    blk.push_back(0);
    AppendEncodedLength(f, 1, &blk);
    AppendCString(".", &blk);
    AppendCString("d_%b", &blk);
    AppendSerializedSelection(s_all, &blk);
    AppendSerializedSelection(s_all, &blk);
    AppendChecksum(&blk);
    expect_load_fail(blk);
  }

  // 12) Truncated checksum, bad checksum, and trailing extra bytes.
  {
    std::vector<uint8_t> base;
    base.push_back(0);
    AppendEncodedLength(f, 1, &base);
    AppendCString(".", &base);
    AppendCString("d", &base);
    AppendSerializedSelection(s_unlim, &base);
    AppendSerializedSelection(s_unlim, &base);

    std::vector<uint8_t> trunc_csum = base;
    trunc_csum.push_back(0);
    trunc_csum.push_back(0);
    expect_load_fail(trunc_csum);

    std::vector<uint8_t> bad_csum = base;
    AppendChecksum(&bad_csum);
    bad_csum.back() ^= 0xff;
    expect_load_fail(bad_csum);

    // Compute valid checksum INCLUDING an extra unconsumed byte before
    // checksum.
    std::vector<uint8_t> extra_byte = base;
    extra_byte.push_back(0xAA);
    AppendChecksum(&extra_byte);
    expect_load_fail(extra_byte);
  }
}

TEST(H5DvirtualTest, UnlimitedNonPrintfAndPrintfExtents) {
  TempDir dir("vds_unlim");
  const std::string src_a = dir.child("src_a.h5").string();
  const std::string src_b = dir.child("src_b.h5").string();
  const std::string vds_path = dir.child("vds_unlim.h5").string();

  const std::array<hsize_t, 2> src_init_dims = {2, 2};
  const std::array<hsize_t, 2> src_max_dims = {H5S_UNLIMITED, 2};
  const std::array<hsize_t, 2> start0 = {0, 0};
  const std::array<hsize_t, 2> v_cnt_u = {H5S_UNLIMITED, 2};

  // Start with src_a existing ([2, 2]) and src_b initially missing.
  WriteInt32DatasetFile(src_a, "s", {2, 2}, {H5S_UNLIMITED, 2}, {2, 2},
                        {1, 2, 3, 4});

  {
    ScopedFile vfile(
        H5Fcreate(vds_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(vfile.valid());

    // Local limited dataset [2, 2] mapped at row [4..5] so min_dims[0] = 6.
    ScopedSpace s22(H5Screate_simple(2, src_init_dims.data(), nullptr));
    ScopedDataset local(H5Dcreate2(vfile, "local_lim", H5T_NATIVE_INT32, s22,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    const std::array<int32_t, 4> lv = {90, 91, 92, 93};
    ASSERT_GE(H5Dwrite(local, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       lv.data()),
              0);

    ScopedSpace su(
        H5Screate_simple(2, src_init_dims.data(), src_max_dims.data()));
    ASSERT_GE(H5Sselect_hyperslab(su, H5S_SELECT_SET, start0.data(), nullptr,
                                  v_cnt_u.data(), nullptr),
              0);

    const std::array<hsize_t, 2> v_dims0 = {0, 4};
    const std::array<hsize_t, 2> v_max = {H5S_UNLIMITED, 4};
    ScopedSpace vu_a(H5Screate_simple(2, v_dims0.data(), v_max.data()));
    ASSERT_GE(H5Sselect_hyperslab(vu_a, H5S_SELECT_SET, start0.data(), nullptr,
                                  v_cnt_u.data(), nullptr),
              0);

    ScopedSpace vu_b(H5Screate_simple(2, v_dims0.data(), v_max.data()));
    const std::array<hsize_t, 2> start_b = {0, 2};
    ASSERT_GE(H5Sselect_hyperslab(vu_b, H5S_SELECT_SET, start_b.data(), nullptr,
                                  v_cnt_u.data(), nullptr),
              0);

    ScopedPlist dcpl_np(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl_np, vu_a, src_a.c_str(), "s", su), 0);
    ASSERT_GE(H5Pset_virtual(dcpl_np, vu_b, src_b.c_str(), "s", su), 0);
    ScopedDataset v_np(H5Dcreate2(vfile, "v_nonprintf", H5T_NATIVE_INT32, vu_a,
                                  H5P_DEFAULT, dcpl_np, H5P_DEFAULT));
    ASSERT_TRUE(v_np.valid());

    // Mixed limited + unlimited to exercise min_dims clamping in
    // set_extent_unlim.
    const std::array<hsize_t, 2> v_lim_start = {4, 0};
    const std::array<hsize_t, 2> v_lim_cnt = {2, 2};
    const std::array<hsize_t, 2> v_dims6 = {6, 4};
    ScopedSpace vu_clamp(H5Screate_simple(2, v_dims0.data(), v_max.data()));
    ASSERT_GE(H5Sselect_hyperslab(vu_clamp, H5S_SELECT_SET, v_lim_start.data(),
                                  nullptr, v_lim_cnt.data(), nullptr),
              0);
    ScopedPlist dcpl_clamp(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl_clamp, vu_a, src_a.c_str(), "s", su), 0);
    ASSERT_GE(H5Pset_virtual(dcpl_clamp, vu_clamp, ".", "local_lim", s22), 0);
    ScopedSpace v_clamp_init(H5Screate_simple(2, v_dims6.data(), v_max.data()));
    ScopedDataset v_clamp(H5Dcreate2(vfile, "v_clamp", H5T_NATIVE_INT32,
                                     v_clamp_init, H5P_DEFAULT, dcpl_clamp,
                                     H5P_DEFAULT));
    ASSERT_TRUE(v_clamp.valid());

    // 1D printf VDS.
    const std::array<hsize_t, 1> p_d0 = {0};
    const std::array<hsize_t, 1> p_max = {H5S_UNLIMITED};
    const std::array<hsize_t, 1> p_s2 = {2};
    const std::array<hsize_t, 1> p_start0 = {0};
    const std::array<hsize_t, 1> p_stride2 = {2};
    const std::array<hsize_t, 1> p_cntu = {H5S_UNLIMITED};
    ScopedSpace ps(H5Screate_simple(1, p_s2.data(), nullptr));
    ScopedSpace pvu(H5Screate_simple(1, p_d0.data(), p_max.data()));
    ASSERT_GE(H5Sselect_hyperslab(pvu, H5S_SELECT_SET, p_start0.data(),
                                  p_stride2.data(), p_cntu.data(), p_s2.data()),
              0);
    ScopedPlist dcpl_pf(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(
        H5Pset_virtual(dcpl_pf, pvu, dir.child("p_%b.h5").c_str(), "p", ps), 0);
    ScopedDataset v_pf(H5Dcreate2(vfile, "v_printf", H5T_NATIVE_INT32, pvu,
                                  H5P_DEFAULT, dcpl_pf, H5P_DEFAULT));
    ASSERT_TRUE(v_pf.valid());
  }

  // Test 1: H5D_VDS_FIRST_MISSING with missing src_b -> extent 0; create src_b
  // -> extends; grow sources twice to exercise FIRST_MISSING re-clipping; also
  // check H5F_ACC_RDONLY skips H5D__mark and clamping hits min_dims[0] = 6.
  {
    ScopedPlist dapl_first(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_virtual_view(dapl_first, H5D_VDS_FIRST_MISSING), 0);

    ScopedFile vfile_ro(H5Fopen(vds_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ScopedDataset v_clamp(H5Dopen2(vfile_ro, "v_clamp", dapl_first));
    ScopedSpace sp_c(H5Dget_space(v_clamp));
    std::array<hsize_t, 2> dims{};
    ASSERT_EQ(H5Sget_simple_extent_dims(sp_c, dims.data(), nullptr), 2);
    EXPECT_EQ(dims[0], 6u);  // src_a has 2 < min_dims 6 -> clamped to 6
    EXPECT_EQ(dims[1], 4u);
    // Repeated H5Dget_space hits cached extent on RO file.
    sp_c.reset(H5Dget_space(v_clamp));
    ASSERT_EQ(H5Sget_simple_extent_dims(sp_c, dims.data(), nullptr), 2);
    EXPECT_EQ(dims[0], 6u);

    ScopedDataset v_np_ro(H5Dopen2(vfile_ro, "v_nonprintf", dapl_first));
    sp_c.reset(H5Dget_space(v_np_ro));
    ASSERT_EQ(H5Sget_simple_extent_dims(sp_c, dims.data(), nullptr), 2);
    EXPECT_EQ(dims[0], 0u);  // src_b missing -> min(2, 0) == 0
  }

  // Create src_b with 4 rows, open RDWR VDS, query space repeatedly across
  // source growth in BOTH FIRST_MISSING and LAST_AVAILABLE modes.
  WriteInt32DatasetFile(src_b, "s", {4, 2}, {H5S_UNLIMITED, 2}, {2, 2},
                        {10, 11, 12, 13, 14, 15, 16, 17});

  {
    ScopedPlist dapl_first(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_virtual_view(dapl_first, H5D_VDS_FIRST_MISSING), 0);
    ScopedFile vfile(H5Fopen(vds_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ScopedDataset v_np(H5Dopen2(vfile, "v_nonprintf", dapl_first));

    std::array<hsize_t, 2> dims{};
    ScopedSpace sp(H5Dget_space(v_np));
    ASSERT_EQ(H5Sget_simple_extent_dims(sp, dims.data(), nullptr), 2);
    EXPECT_EQ(dims[0], 2u);  // min(src_a=2, src_b=4)

    // Extend src_a to 3 rows, re-query H5Dget_space (re-clips both selections).
    {
      ScopedFile fs_a(H5Fopen(src_a.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
      ScopedDataset ds_a(H5Dopen2(fs_a, "s", H5P_DEFAULT));
      const std::array<hsize_t, 2> ext3 = {3, 2};
      ASSERT_GE(H5Dset_extent(ds_a, ext3.data()), 0);
    }
    sp.reset(H5Dget_space(v_np));
    ASSERT_EQ(H5Sget_simple_extent_dims(sp, dims.data(), nullptr), 2);
    EXPECT_EQ(dims[0], 3u);
    // Repeated query with unchanged src extent uses cached clip_size.
    sp.reset(H5Dget_space(v_np));
    ASSERT_EQ(H5Sget_simple_extent_dims(sp, dims.data(), nullptr), 2);
    EXPECT_EQ(dims[0], 3u);
  }

  {
    ScopedPlist dapl_last(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_virtual_view(dapl_last, H5D_VDS_LAST_AVAILABLE), 0);
    ScopedFile vfile(H5Fopen(vds_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ScopedDataset v_np(H5Dopen2(vfile, "v_nonprintf", dapl_last));

    std::array<hsize_t, 2> dims{};
    ScopedSpace sp(H5Dget_space(v_np));
    ASSERT_EQ(H5Sget_simple_extent_dims(sp, dims.data(), nullptr), 2);
    EXPECT_EQ(dims[0], 4u);  // max(src_a=3, src_b=4)

    // Extend src_b from 4 -> 5 rows and re-query LAST_AVAILABLE to free/re-clip
    // previous clipped_virtual_select and clipped_source_select.
    {
      ScopedFile fs_b(H5Fopen(src_b.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
      ScopedDataset ds_b(H5Dopen2(fs_b, "s", H5P_DEFAULT));
      const std::array<hsize_t, 2> ext5 = {5, 2};
      ASSERT_GE(H5Dset_extent(ds_b, ext5.data()), 0);
    }
    sp.reset(H5Dget_space(v_np));
    ASSERT_EQ(H5Sget_simple_extent_dims(sp, dims.data(), nullptr), 2);
    EXPECT_EQ(dims[0], 5u);
  }

  // Test 2: Printf extents with 0 files, holes, printf_gap=2, printf_gap=132,
  // FIRST_MISSING, and LAST_AVAILABLE.
  {
    ScopedFile vfile(H5Fopen(vds_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ASSERT_TRUE(vfile.valid());
    ScopedPlist dapl_last(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_virtual_view(dapl_last, H5D_VDS_LAST_AVAILABLE), 0);
    ASSERT_GE(H5Pset_virtual_printf_gap(dapl_last, 2), 0);

    // Initially no p_%b.h5 exists -> first_missing == 0 -> extent 0.
    ScopedDataset v_pf(H5Dopen2(vfile, "v_printf", dapl_last));
    ScopedSpace sp(H5Dget_space(v_pf));
    hsize_t dim0 = 99;
    ASSERT_EQ(H5Sget_simple_extent_dims(sp, &dim0, nullptr), 1);
    EXPECT_EQ(dim0, 0u);
    v_pf.reset();

    // Create p_1.h5 (gap of 1 from 0 <= gap 2), skip p_2 & p_3, create p_5.h5
    // (beyond gap 2). Extent under LAST_AVAILABLE+gap=2 should be 4 ([2..3]).
    WriteInt32DatasetFile(dir.child("p_1.h5").string(), "p", {2}, {}, {},
                          {10, 11});
    WriteInt32DatasetFile(dir.child("p_5.h5").string(), "p", {2}, {}, {},
                          {50, 51});
    v_pf.reset(H5Dopen2(vfile, "v_printf", dapl_last));
    sp.reset(H5Dget_space(v_pf));
    ASSERT_EQ(H5Sget_simple_extent_dims(sp, &dim0, nullptr), 1);
    EXPECT_EQ(dim0, 4u);
    // Repeated H5Dget_space hits dset_exists + cached clip_size_virtual.
    sp.reset(H5Dget_space(v_pf));
    ASSERT_EQ(H5Sget_simple_extent_dims(sp, &dim0, nullptr), 1);
    EXPECT_EQ(dim0, 4u);
    v_pf.reset();

    // Create p_0.h5 -> now p_0 and p_1 exist, p_2 missing, so FIRST_MISSING=4.
    WriteInt32DatasetFile(dir.child("p_0.h5").string(), "p", {2}, {}, {},
                          {1, 2});
    ScopedPlist dapl_first(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_virtual_view(dapl_first, H5D_VDS_FIRST_MISSING), 0);
    v_pf.reset(H5Dopen2(vfile, "v_printf", dapl_first));
    sp.reset(H5Dget_space(v_pf));
    ASSERT_EQ(H5Sget_simple_extent_dims(sp, &dim0, nullptr), 1);
    EXPECT_EQ(dim0, 4u);
    v_pf.reset();

    // Large printf_gap = 132 triggers sub_dset reallocation (> 128).
    ScopedPlist dapl_biggap(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_virtual_view(dapl_biggap, H5D_VDS_LAST_AVAILABLE), 0);
    ASSERT_GE(H5Pset_virtual_printf_gap(dapl_biggap, 132), 0);
    v_pf.reset(H5Dopen2(vfile, "v_printf", dapl_biggap));
    sp.reset(H5Dget_space(v_pf));
    ASSERT_EQ(H5Sget_simple_extent_dims(sp, &dim0, nullptr), 1);
    EXPECT_EQ(dim0, 12u);  // p_5 now within gap 132 -> bounds_end + 1 == 12
  }
}

TEST(H5DvirtualTest, InitAllWithoutGetSpaceAndPartialBlocks) {
  TempDir dir("vds_init_all");
  const std::string src_path = dir.child("src.h5").string();
  const std::string vds_path = dir.child("vds.h5").string();
  WriteInt32DatasetFile(src_path, "s", {4}, {H5S_UNLIMITED}, {4},
                        {10, 20, 30, 40});
  WriteInt32DatasetFile(dir.child("pf_0.h5").string(), "d", {4}, {}, {},
                        {1, 2, 3, 4});
  WriteInt32DatasetFile(dir.child("pf_1.h5").string(), "d", {4}, {}, {},
                        {5, 6, 7, 8});

  {
    ScopedFile vf(
        H5Fcreate(vds_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    const std::array<hsize_t, 1> d2 = {2};
    const std::array<hsize_t, 1> d4 = {4};
    const std::array<hsize_t, 1> d6 = {6};
    const std::array<hsize_t, 1> dmax = {H5S_UNLIMITED};
    const std::array<hsize_t, 1> start0 = {0};
    const std::array<hsize_t, 1> stride4 = {4};
    const std::array<hsize_t, 1> cntu = {H5S_UNLIMITED};

    ScopedSpace su(H5Screate_simple(1, d2.data(), dmax.data()));
    ASSERT_GE(H5Sselect_hyperslab(su, H5S_SELECT_SET, start0.data(), nullptr,
                                  cntu.data(), nullptr),
              0);

    // Non-printf VDS initially sized [2] <= source [4].
    ScopedPlist dcpl_np(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl_np, su, src_path.c_str(), "s", su), 0);
    ScopedDataset v_np(H5Dcreate2(vf, "v_np", H5T_NATIVE_INT32, su, H5P_DEFAULT,
                                  dcpl_np, H5P_DEFAULT));
    ASSERT_TRUE(v_np.valid());

    // Non-printf VDS whose source file is missing (leaves clipped_* NULL in
    // init_all).
    ScopedPlist dcpl_miss(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl_miss, su, "missing_src.h5", "s", su), 0);
    ScopedSpace sp_miss(H5Screate_simple(1, d2.data(), dmax.data()));
    ScopedDataset v_miss(H5Dcreate2(vf, "v_miss", H5T_NATIVE_INT32, sp_miss,
                                    H5P_DEFAULT, dcpl_miss, H5P_DEFAULT));
    ASSERT_TRUE(v_miss.valid());

    // Printf VDS sized [6] (block=4 -> partial_block=true at pf_1) and
    // extendable.
    ScopedSpace ps4(H5Screate_simple(1, d4.data(), nullptr));
    ScopedSpace pvu(H5Screate_simple(1, d6.data(), dmax.data()));
    ASSERT_GE(H5Sselect_hyperslab(pvu, H5S_SELECT_SET, start0.data(),
                                  stride4.data(), cntu.data(), d4.data()),
              0);
    ScopedPlist dcpl_pf(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(
        H5Pset_virtual(dcpl_pf, pvu, dir.child("pf_%b.h5").c_str(), "d", ps4),
        0);
    ScopedDataset v_pf(H5Dcreate2(vf, "v_pf", H5T_NATIVE_INT32, pvu,
                                  H5P_DEFAULT, dcpl_pf, H5P_DEFAULT));
    ASSERT_TRUE(v_pf.valid());
  }

  ScopedFile vf(H5Fopen(vds_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
  ASSERT_TRUE(vf.valid());

  // 1) Read non-printf directly WITHOUT calling H5Dget_space first -> init_all
  // takes clip_size (2) <= source_dims (4).
  {
    ScopedDataset v_np(H5Dopen2(vf, "v_np", H5P_DEFAULT));
    std::array<int32_t, 2> buf{};
    ASSERT_GE(H5Dread(v_np, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      buf.data()),
              0);
    EXPECT_THAT(buf, ElementsAre(10, 20));

    // Extend VDS to [6] (> source_dims 4) -> H5Dset_extent invalidates
    // storage->init, and next H5Dread runs init_all's clip_size > source_dims
    // branch while closing the old clipped_* selections!
    const std::array<hsize_t, 1> d6 = {6};
    ASSERT_GE(H5Dset_extent(v_np, d6.data()), 0);
    std::array<int32_t, 6> buf6{};
    ASSERT_GE(H5Dread(v_np, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      buf6.data()),
              0);
    EXPECT_THAT(buf6, ElementsAre(10, 20, 30, 40, 0, 0));
  }

  // 2) Missing non-printf unlim source read via init_all -> fill value only.
  {
    ScopedDataset v_miss(H5Dopen2(vf, "v_miss", H5P_DEFAULT));
    std::array<int32_t, 2> buf = {9, 9};
    ASSERT_GE(H5Dread(v_miss, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      buf.data()),
              0);
    EXPECT_THAT(buf, ElementsAre(0, 0));
  }

  // 3) Printf VDS read without H5Dget_space: curr_dims=6 -> partial_block=true,
  // clips pf_1 cleanly in pre_io; then H5Dset_extent(14) grows sub_dset >
  // sub_dset_nalloc and tests partial block when pf_3 is missing.
  {
    ScopedDataset v_pf(H5Dopen2(vf, "v_pf", H5P_DEFAULT));
    std::array<int32_t, 6> buf6{};
    ASSERT_GE(H5Dread(v_pf, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      buf6.data()),
              0);
    EXPECT_THAT(buf6, ElementsAre(1, 2, 3, 4, 5, 6));

    // Extend to 14 (blocks 0,1,2,3 -> sub_dset_max = 4 > sub_dset_nalloc = 2,
    // block 3 is partial and missing pf_3.h5).
    const std::array<hsize_t, 1> d14 = {14};
    ASSERT_GE(H5Dset_extent(v_pf, d14.data()), 0);
    std::array<int32_t, 14> buf14{};
    ASSERT_GE(H5Dread(v_pf, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      buf14.data()),
              0);
    EXPECT_THAT(buf14, ElementsAre(1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0));
  }
}

TEST(H5DvirtualTest, PrefixLookupEnvAndSourceOpenErrors) {
  TempDir dir("vds_prefix");
  const std::filesystem::path subdir = dir.child("nested");
  std::error_code ec;
  std::filesystem::create_directories(subdir, ec);

  WriteInt32DatasetFile((subdir / "src_rel.h5").string(), "d", {2}, {}, {},
                        {42, 43});

  // Also create a file with a group (not a dataset) to exercise H5D__open_name
  // failure on existing non-dataset object.
  const std::string bad_obj_file = dir.child("grp_not_dset.h5").string();
  {
    ScopedFile gf(H5Fcreate(bad_obj_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                            H5P_DEFAULT));
    ScopedGroup grp(
        H5Gcreate2(gf, "/not_a_dset", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
  }

  const std::string vds_path = dir.child("vds_pref.h5").string();
  {
    ScopedFile vf(
        H5Fcreate(vds_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    const std::array<hsize_t, 1> d2 = {2};
    ScopedSpace s2(H5Screate_simple(1, d2.data(), nullptr));

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl, s2, "src_rel.h5", "d", s2), 0);
    ScopedDataset v_rel(H5Dcreate2(vf, "v_rel", H5T_NATIVE_INT32, s2,
                                   H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(v_rel.valid());

    ScopedPlist dcpl_grp(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(
        H5Pset_virtual(dcpl_grp, s2, bad_obj_file.c_str(), "/not_a_dset", s2),
        0);
    ScopedDataset v_grp(H5Dcreate2(vf, "v_grp", H5T_NATIVE_INT32, s2,
                                   H5P_DEFAULT, dcpl_grp, H5P_DEFAULT));
    ASSERT_TRUE(v_grp.valid());

    ScopedPlist dcpl_miss_ds(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(
        H5Pset_virtual(dcpl_miss_ds, s2, bad_obj_file.c_str(), "/absent", s2),
        0);
    ScopedDataset v_mds(H5Dcreate2(vf, "v_miss_ds", H5T_NATIVE_INT32, s2,
                                   H5P_DEFAULT, dcpl_miss_ds, H5P_DEFAULT));
    ASSERT_TRUE(v_mds.valid());
  }

  // 1) H5Pset_virtual_prefix opens nested/src_rel.h5.
  {
    ScopedFile vf(H5Fopen(vds_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_virtual_prefix(dapl, subdir.c_str()), 0);
    ScopedDataset v_rel(H5Dopen2(vf, "v_rel", dapl));
    std::array<int32_t, 2> out{};
    ASSERT_GE(H5Dread(v_rel, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      out.data()),
              0);
    EXPECT_THAT(out, ElementsAre(42, 43));
  }

  // 2) HDF5_VDS_PREFIX environment variable (restored via absl::Cleanup).
  {
    const char* prev = std::getenv("HDF5_VDS_PREFIX");
    const std::string saved_prev = prev ? prev : "";
    const bool had_prev = (prev != nullptr);
    absl::Cleanup restore_env = [=]() {
      if (had_prev) {
        setenv("HDF5_VDS_PREFIX", saved_prev.c_str(), 1);
      } else {
        unsetenv("HDF5_VDS_PREFIX");
      }
    };
    ASSERT_EQ(setenv("HDF5_VDS_PREFIX", subdir.c_str(), 1), 0);

    ScopedFile vf(H5Fopen(vds_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ScopedDataset v_rel(H5Dopen2(vf, "v_rel", H5P_DEFAULT));
    std::array<int32_t, 2> out{};
    ASSERT_GE(H5Dread(v_rel, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      out.data()),
              0);
    EXPECT_THAT(out, ElementsAre(42, 43));
  }

  // 3) Group object instead of dataset fails open, missing dataset returns
  // fill.
  {
    ScopedFile vf(H5Fopen(vds_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ScopedDataset v_grp(H5Dopen2(vf, "v_grp", H5P_DEFAULT));
    std::array<int32_t, 2> out = {9, 9};
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Dread(v_grp, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                        out.data()),
                0);
    }

    ScopedDataset v_mds(H5Dopen2(vf, "v_miss_ds", H5P_DEFAULT));
    ASSERT_GE(H5Dread(v_mds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      out.data()),
              0);
    EXPECT_THAT(out, ElementsAre(0, 0));
  }
}

TEST(H5DvirtualTest, SpatialRTreeMixedUnlimitedAndScalar) {
  ScopedFile file = CreateInMemoryFile("vds_rtree.h5");
  ASSERT_TRUE(file.valid());

  // Local 1-D source datasets [1] and [1] unlimited.
  const std::array<hsize_t, 1> d1 = {1};
  const std::array<hsize_t, 1> d1u = {H5S_UNLIMITED};
  const std::array<hsize_t, 1> c1 = {1};
  ScopedSpace s1(H5Screate_simple(1, d1.data(), nullptr));
  ScopedSpace s1u(H5Screate_simple(1, d1.data(), d1u.data()));
  ScopedDataset src_lim(H5Dcreate2(file, "src_lim", H5T_NATIVE_INT32, s1,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  const int32_t v_init = 0;
  ASSERT_GE(H5Dwrite(src_lim, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     &v_init),
            0);

  ScopedPlist src_u_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(H5Pset_chunk(src_u_dcpl, 1, c1.data()), 0);
  ScopedDataset src_u1(H5Dcreate2(file, "src_u1", H5T_NATIVE_INT32, s1u,
                                  H5P_DEFAULT, src_u_dcpl, H5P_DEFAULT));
  const int32_t u1_val = 701;
  ASSERT_GE(H5Dwrite(src_u1, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     &u1_val),
            0);

  // 1) 49 mappings (< 50 threshold): tree is not built.
  {
    const std::array<hsize_t, 1> d49 = {49};
    ScopedSpace v49(H5Screate_simple(1, d49.data(), nullptr));
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    for (hsize_t i = 0; i < 49; ++i) {
      ASSERT_GE(H5Sselect_hyperslab(v49, H5S_SELECT_SET, &i, nullptr, c1.data(),
                                    nullptr),
                0);
      ASSERT_GE(H5Pset_virtual(dcpl, v49, ".", "src_lim", s1), 0);
    }
    ScopedDataset vd(H5Dcreate2(file, "v49", H5T_NATIVE_INT32, v49, H5P_DEFAULT,
                                dcpl, H5P_DEFAULT));
    ASSERT_TRUE(vd.valid());
    auto* d = static_cast<H5D_t*>(H5VL_object_verify(vd, H5I_DATASET));
    std::array<int32_t, 49> buf{};
    ASSERT_GE(H5Dread(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      buf.data()),
              0);
    EXPECT_EQ(d->shared->layout.storage.u.virt.tree, nullptr);
  }

  // 2) 52 limited mappings with spatial tree DISABLED via DAPL, vs ENABLED.
  {
    const std::array<hsize_t, 1> d52 = {52};
    ScopedSpace s52(H5Screate_simple(1, d52.data(), nullptr));
    ScopedDataset src52(H5Dcreate2(file, "src52", H5T_NATIVE_INT32, s52,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(src52.valid());
    ScopedSpace v52(H5Screate_simple(1, d52.data(), nullptr));
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    for (hsize_t i = 0; i < 52; ++i) {
      ASSERT_GE(H5Sselect_hyperslab(v52, H5S_SELECT_SET, &i, nullptr, c1.data(),
                                    nullptr),
                0);
      ASSERT_GE(H5Sselect_hyperslab(s52, H5S_SELECT_SET, &i, nullptr, c1.data(),
                                    nullptr),
                0);
      ASSERT_GE(H5Pset_virtual(dcpl, v52, ".", "src52", s52), 0);
    }

    ScopedPlist dapl_notree(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_virtual_spatial_tree(dapl_notree, false), 0);
    ScopedDataset vd_off(H5Dcreate2(file, "v52_notree", H5T_NATIVE_INT32, v52,
                                    H5P_DEFAULT, dcpl, dapl_notree));
    auto* d_off = static_cast<H5D_t*>(H5VL_object_verify(vd_off, H5I_DATASET));
    std::array<int32_t, 52> buf{};
    ASSERT_GE(H5Dread(vd_off, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      buf.data()),
              0);
    EXPECT_EQ(d_off->shared->layout.storage.u.virt.tree, nullptr);

    // Tree enabled: build on write, read subset, read H5Sselect_none
    // (nelmts=0).
    ScopedDataset vd_on(H5Dcreate2(file, "v52_tree", H5T_NATIVE_INT32, v52,
                                   H5P_DEFAULT, dcpl, H5P_DEFAULT));
    auto* d_on = static_cast<H5D_t*>(H5VL_object_verify(vd_on, H5I_DATASET));

    const std::array<hsize_t, 1> wstart = {10};
    const std::array<hsize_t, 1> wcnt = {5};
    ScopedSpace wfs(H5Screate_simple(1, d52.data(), nullptr));
    ASSERT_GE(H5Sselect_hyperslab(wfs, H5S_SELECT_SET, wstart.data(), nullptr,
                                  wcnt.data(), nullptr),
              0);
    ScopedSpace wms(H5Screate_simple(1, wcnt.data(), nullptr));
    const std::array<int32_t, 5> wvals = {1, 2, 3, 4, 5};
    ASSERT_GE(
        H5Dwrite(vd_on, H5T_NATIVE_INT32, wms, wfs, H5P_DEFAULT, wvals.data()),
        0);
    EXPECT_NE(d_on->shared->layout.storage.u.virt.tree, nullptr);

    // Second read hits `if (storage->tree)` already built, reading subset.
    std::array<int32_t, 5> rvals{};
    ASSERT_GE(
        H5Dread(vd_on, H5T_NATIVE_INT32, wms, wfs, H5P_DEFAULT, rvals.data()),
        0);
    EXPECT_THAT(rvals, ElementsAre(1, 2, 3, 4, 5));

    // Read with H5Sselect_none (nelmts == 0) skips H5RT_search.
    ScopedSpace none_fs(H5Screate_simple(1, d52.data(), nullptr));
    ASSERT_GE(H5Sselect_none(none_fs), 0);
    ScopedSpace none_ms(H5Screate_simple(1, d52.data(), nullptr));
    ASSERT_GE(H5Sselect_none(none_ms), 0);
    ASSERT_GE(H5Dread(vd_on, H5T_NATIVE_INT32, none_ms, none_fs, H5P_DEFAULT,
                      buf.data()),
              0);
  }

  // 3) Mixed 52 limited + 2 unlimited mappings: populates BOTH tree and
  // not_in_tree_list, plus internal H5D__virtual_copy_layout on open tree.
  {
    const std::array<hsize_t, 2> d2d = {54, 2};
    const std::array<hsize_t, 2> d2d_max = {H5S_UNLIMITED, 2};
    const std::array<hsize_t, 2> c2d = {1, 1};
    const std::array<hsize_t, 2> dsrc2d = {1, 1};
    const std::array<hsize_t, 2> dsrc2d_max = {H5S_UNLIMITED, 1};
    ScopedSpace s2d_lim(H5Screate_simple(2, dsrc2d.data(), nullptr));
    ScopedDataset src2d_lim(H5Dcreate2(file, "src2d_lim", H5T_NATIVE_INT32,
                                       s2d_lim, H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT));
    const int32_t lim2d_val = 42;
    ASSERT_GE(H5Dwrite(src2d_lim, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, &lim2d_val),
              0);
    ScopedSpace s2d_u(H5Screate_simple(2, dsrc2d.data(), dsrc2d_max.data()));
    const std::array<hsize_t, 2> s0 = {0, 0};
    const std::array<hsize_t, 2> cu = {H5S_UNLIMITED, 1};
    ASSERT_GE(H5Sselect_hyperslab(s2d_u, H5S_SELECT_SET, s0.data(), nullptr,
                                  cu.data(), nullptr),
              0);

    ScopedPlist dcpl_mix(H5Pcreate(H5P_DATASET_CREATE));
    ScopedSpace v2d(H5Screate_simple(2, d2d.data(), d2d_max.data()));
    for (hsize_t i = 0; i < 52; ++i) {
      const std::array<hsize_t, 2> st = {i, 0};
      ASSERT_GE(H5Sselect_hyperslab(v2d, H5S_SELECT_SET, st.data(), nullptr,
                                    c2d.data(), nullptr),
                0);
      ASSERT_GE(H5Pset_virtual(dcpl_mix, v2d, ".", "src2d_lim", s2d_lim), 0);
    }
    ScopedPlist c2d_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_chunk(c2d_dcpl, 2, c2d.data()), 0);
    ScopedDataset src2d_u_real(H5Dcreate2(file, "src2d_u_real",
                                          H5T_NATIVE_INT32, s2d_u, H5P_DEFAULT,
                                          c2d_dcpl, H5P_DEFAULT));
    const int32_t u2_val = 808;
    ASSERT_GE(H5Dwrite(src2d_u_real, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, &u2_val),
              0);

    const std::array<hsize_t, 2> st_u = {0, 1};
    ASSERT_GE(H5Sselect_hyperslab(v2d, H5S_SELECT_SET, st_u.data(), nullptr,
                                  cu.data(), nullptr),
              0);
    ASSERT_GE(H5Pset_virtual(dcpl_mix, v2d, ".", "src2d_u_real", s2d_u), 0);

    const std::array<hsize_t, 2> st_u2 = {52, 0};
    ASSERT_GE(H5Sselect_hyperslab(v2d, H5S_SELECT_SET, st_u2.data(), nullptr,
                                  cu.data(), nullptr),
              0);
    ASSERT_GE(H5Pset_virtual(dcpl_mix, v2d, ".", "src2d_u_real", s2d_u), 0);

    const std::array<hsize_t, 2> d54x2 = {54, 2};
    ScopedSpace v_init_2d(H5Screate_simple(2, d54x2.data(), d2d_max.data()));
    ScopedDataset vd_mix(H5Dcreate2(file, "v_mix", H5T_NATIVE_INT32, v_init_2d,
                                    H5P_DEFAULT, dcpl_mix, H5P_DEFAULT));
    ASSERT_TRUE(vd_mix.valid());

    // Write & read through both tree leaves and not_in_tree_list.
    const std::array<hsize_t, 2> w_st = {0, 1};
    const std::array<hsize_t, 2> w_cnt = {1, 1};
    ScopedSpace wfs(H5Screate_simple(2, d54x2.data(), d2d_max.data()));
    ASSERT_GE(H5Sselect_hyperslab(wfs, H5S_SELECT_SET, w_st.data(), nullptr,
                                  w_cnt.data(), nullptr),
              0);
    ScopedSpace wms(H5Screate_simple(2, w_cnt.data(), nullptr));
    const int32_t new_u = 909;
    ASSERT_GE(H5Dwrite(vd_mix, H5T_NATIVE_INT32, wms, wfs, H5P_DEFAULT, &new_u),
              0);

    std::vector<int32_t> full_buf(54 * 2, -1);
    ASSERT_GE(H5Dread(vd_mix, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      full_buf.data()),
              0);
    EXPECT_EQ(full_buf[1], 909);
    EXPECT_EQ(full_buf[52 * 2], 909);

    auto* d_mix = static_cast<H5D_t*>(H5VL_object_verify(vd_mix, H5I_DATASET));
    ASSERT_NE(d_mix->shared->layout.storage.u.virt.tree, nullptr);
    EXPECT_EQ(d_mix->shared->layout.storage.u.virt.not_in_tree_nused, 2u);

    // Exercise H5D__virtual_copy_layout when tree && not_in_tree_list != NULL.
    ScopedApiContext ctx;
    H5O_layout_t copy_layout = d_mix->shared->layout;
    ASSERT_GE(H5D__virtual_copy_layout(&copy_layout), 0);
    EXPECT_NE(copy_layout.storage.u.virt.tree, nullptr);
    EXPECT_EQ(copy_layout.storage.u.virt.not_in_tree_nused, 2u);
    EXPECT_GE(H5D__virtual_reset_layout(&copy_layout), 0);
  }

  // 4) 66 unlimited mappings (> H5D_VIRTUAL_NOT_IN_TREE_INIT_SIZE = 64):
  // hits not_in_tree_grow and num_leaves == 0 in build_tree.
  {
    const std::array<hsize_t, 2> d2d = {1, 66};
    const std::array<hsize_t, 2> d2d_max = {H5S_UNLIMITED, 66};
    const std::array<hsize_t, 2> dsrc = {1, 1};
    const std::array<hsize_t, 2> dsrc_max = {H5S_UNLIMITED, 1};
    const std::array<hsize_t, 2> s0 = {0, 0};
    const std::array<hsize_t, 2> cu = {H5S_UNLIMITED, 1};

    ScopedSpace s2d_u(H5Screate_simple(2, dsrc.data(), dsrc_max.data()));
    ASSERT_GE(H5Sselect_hyperslab(s2d_u, H5S_SELECT_SET, s0.data(), nullptr,
                                  cu.data(), nullptr),
              0);
    ScopedPlist dcpl66(H5Pcreate(H5P_DATASET_CREATE));
    ScopedSpace v2d(H5Screate_simple(2, d2d.data(), d2d_max.data()));
    for (hsize_t col = 0; col < 66; ++col) {
      const std::array<hsize_t, 2> st = {0, col};
      ASSERT_GE(H5Sselect_hyperslab(v2d, H5S_SELECT_SET, st.data(), nullptr,
                                    cu.data(), nullptr),
                0);
      ASSERT_GE(H5Pset_virtual(dcpl66, v2d, ".", "src2d_u_real", s2d_u), 0);
    }
    ScopedDataset vd66(H5Dcreate2(file, "v66_unlim", H5T_NATIVE_INT32, v2d,
                                  H5P_DEFAULT, dcpl66, H5P_DEFAULT));
    std::array<int32_t, 66> rbuf{};
    ASSERT_GE(H5Dread(vd66, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      rbuf.data()),
              0);
    EXPECT_THAT(rbuf, Each(909));
    // Reopen so the prior not_in_tree_list allocation is released cleanly
    // before the write rebuilds it when num_leaves == 0 (tree == NULL).
    vd66.reset();
    vd66.reset(H5Dopen2(file, "v66_unlim", H5P_DEFAULT));
    std::array<int32_t, 66> wbuf{};
    wbuf.fill(333);
    ASSERT_GE(H5Dwrite(vd66, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       wbuf.data()),
              0);
  }

  // 5) 0-D scalar dataspace VDS (single mapping succeeds; >=50 triggers rank ==
  // 0 error in read/write when attempting tree construction).
  {
    ScopedSpace sc(H5Screate(H5S_SCALAR));
    ScopedDataset src_sc(H5Dcreate2(file, "src_sc", H5T_NATIVE_INT32, sc,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    const int32_t sval = 515;
    ASSERT_GE(H5Dwrite(src_sc, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       &sval),
              0);

    ScopedPlist dcpl1(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl1, sc, ".", "src_sc", sc), 0);
    ScopedDataset vd_sc1(H5Dcreate2(file, "v_sc1", H5T_NATIVE_INT32, sc,
                                    H5P_DEFAULT, dcpl1, H5P_DEFAULT));
    int32_t rout = 0;
    ASSERT_GE(
        H5Dread(vd_sc1, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, &rout),
        0);
    EXPECT_EQ(rout, 515);

    ScopedPlist dcpl50(H5Pcreate(H5P_DATASET_CREATE));
    for (int i = 0; i < 50; ++i) {
      ASSERT_GE(H5Pset_virtual(dcpl50, sc, ".", "src_sc", sc), 0);
    }
    ScopedDataset vd_sc50(H5Dcreate2(file, "v_sc50", H5T_NATIVE_INT32, sc,
                                     H5P_DEFAULT, dcpl50, H5P_DEFAULT));
    ScopedErrorSilence silence;
    EXPECT_LT(H5Dread(vd_sc50, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      &rout),
              0);
    EXPECT_LT(H5Dwrite(vd_sc50, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       &rout),
              0);
  }
}

TEST(H5DvirtualTest, FillValuesUnmappedWritesFlushRefreshAndCachedQuery) {
  TempDir dir("vds_fill_refresh");
  const std::string ext_src = dir.child("ext_src.h5").string();
  const std::string ext_pf = dir.child("ext_pf_0.h5").string();
  const std::string vds_path = dir.child("vds_ops.h5").string();

  WriteInt32DatasetFile(ext_src, "s", {4}, {H5S_UNLIMITED}, {2},
                        {10, 20, 30, 40});
  WriteInt32DatasetFile(ext_pf, "p", {2}, {}, {}, {100, 200});

  {
    ScopedFile vf(
        H5Fcreate(vds_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(vf.valid());

    const std::array<hsize_t, 1> d8 = {8};
    const std::array<hsize_t, 1> d4 = {4};
    const std::array<hsize_t, 1> d2 = {2};
    const std::array<hsize_t, 1> d0 = {0};
    const std::array<hsize_t, 1> dmax = {H5S_UNLIMITED};
    const std::array<hsize_t, 1> start0 = {0};
    const std::array<hsize_t, 1> start2 = {2};
    const std::array<hsize_t, 1> stride4 = {4};
    const std::array<hsize_t, 1> cntu = {H5S_UNLIMITED};

    ScopedSpace s4(H5Screate_simple(1, d4.data(), nullptr));
    ScopedSpace s2(H5Screate_simple(1, d2.data(), nullptr));
    ScopedSpace vs(H5Screate_simple(1, d8.data(), nullptr));
    ASSERT_GE(H5Sselect_hyperslab(vs, H5S_SELECT_SET, start2.data(), nullptr,
                                  d4.data(), nullptr),
              0);

    // 1) Defined fill value (-42) on unmapped gaps.
    const int32_t fill_val = -42;
    ScopedPlist dcpl_fill(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_fill_value(dcpl_fill, H5T_NATIVE_INT32, &fill_val), 0);
    ASSERT_GE(H5Pset_virtual(dcpl_fill, vs, ext_src.c_str(), "s", s4), 0);
    ScopedDataset v_fill(H5Dcreate2(vf, "v_fill", H5T_NATIVE_INT32, vs,
                                    H5P_DEFAULT, dcpl_fill, H5P_DEFAULT));
    ASSERT_TRUE(v_fill.valid());

    // 2) Undefined fill value (H5D_FILL_TIME_NEVER + no fill value).
    ScopedPlist dcpl_nofill(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_fill_value(dcpl_nofill, H5T_NATIVE_INT32, nullptr), 0);
    ASSERT_GE(H5Pset_fill_time(dcpl_nofill, H5D_FILL_TIME_NEVER), 0);
    ASSERT_GE(H5Pset_virtual(dcpl_nofill, vs, ext_src.c_str(), "s", s4), 0);
    ScopedDataset v_nofill(H5Dcreate2(vf, "v_nofill", H5T_NATIVE_INT32, vs,
                                      H5P_DEFAULT, dcpl_nofill, H5P_DEFAULT));
    ASSERT_TRUE(v_nofill.valid());

    // 3) Combined non-printf + printf dataset for fill + flush + refresh.
    ScopedSpace vs_sub(H5Screate_simple(1, d8.data(), dmax.data()));
    ASSERT_GE(H5Sselect_hyperslab(vs_sub, H5S_SELECT_SET, start2.data(),
                                  nullptr, d4.data(), nullptr),
              0);
    ScopedSpace vs_pf8(H5Screate_simple(1, d0.data(), dmax.data()));
    ASSERT_GE(H5Sselect_hyperslab(vs_pf8, H5S_SELECT_SET, start0.data(),
                                  stride4.data(), cntu.data(), d2.data()),
              0);
    ScopedPlist dcpl_comb(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_fill_value(dcpl_comb, H5T_NATIVE_INT32, &fill_val), 0);
    ASSERT_GE(H5Pset_virtual(dcpl_comb, vs_sub, ext_src.c_str(), "s", s4), 0);
    ASSERT_GE(H5Pset_virtual(dcpl_comb, vs_pf8,
                             dir.child("ext_pf_%b.h5").c_str(), "p", s2),
              0);
    ScopedSpace vs_comb_ext(H5Screate_simple(1, d8.data(), dmax.data()));
    ScopedDataset v_comb(H5Dcreate2(vf, "v_comb", H5T_NATIVE_INT32, vs_comb_ext,
                                    H5P_DEFAULT, dcpl_comb, H5P_DEFAULT));
    ASSERT_TRUE(v_comb.valid());

    // Compact local source (ops->is_data_cached == NULL) & chunked local
    // source.
    ScopedPlist dcpl_cmp(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_layout(dcpl_cmp, H5D_COMPACT), 0);
    ScopedDataset local_cmp(H5Dcreate2(vf, "local_cmp", H5T_NATIVE_INT32, s4,
                                       H5P_DEFAULT, dcpl_cmp, H5P_DEFAULT));
    ScopedPlist dcpl_vcmp(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl_vcmp, s4, ".", "local_cmp", s4), 0);
    ScopedDataset v_cmp(H5Dcreate2(vf, "v_cmp", H5T_NATIVE_INT32, s4,
                                   H5P_DEFAULT, dcpl_vcmp, H5P_DEFAULT));
    ASSERT_TRUE(v_cmp.valid());
  }

  ScopedFile vf(H5Fopen(vds_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
  ASSERT_TRUE(vf.valid());

  // Defined vs undefined fill value reads, plus unmapped write failure.
  {
    ScopedDataset v_fill(H5Dopen2(vf, "v_fill", H5P_DEFAULT));
    std::array<int32_t, 8> buf{};
    buf.fill(999);
    ASSERT_GE(H5Dread(v_fill, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      buf.data()),
              0);
    EXPECT_THAT(buf, ElementsAre(-42, -42, 10, 20, 30, 40, -42, -42));

    // Write covering unmapped [0..7] fails.
    ScopedErrorSilence silence;
    EXPECT_LT(H5Dwrite(v_fill, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       buf.data()),
              0);
  }

  {
    ScopedDataset v_nofill(H5Dopen2(vf, "v_nofill", H5P_DEFAULT));
    std::array<int32_t, 8> buf{};
    buf.fill(999);
    ASSERT_GE(H5Dread(v_nofill, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      buf.data()),
              0);
    EXPECT_THAT(buf, ElementsAre(999, 999, 10, 20, 30, 40, 999, 999));
  }

  // Combined read + write + H5Dflush + H5Drefresh on open non-printf & printf.
  {
    ScopedDataset v_comb(H5Dopen2(vf, "v_comb", H5P_DEFAULT));
    std::array<int32_t, 8> rbuf{};
    ASSERT_GE(H5Dread(v_comb, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      rbuf.data()),
              0);
    EXPECT_THAT(rbuf, ElementsAre(100, 200, 10, 20, 30, 40, -42, -42));

    // Write directly to mapped [0..5] across both printf (ext_pf_0) and
    // non-printf (ext_src).
    const std::array<hsize_t, 1> d8 = {8};
    const std::array<hsize_t, 1> s0 = {0};
    const std::array<hsize_t, 1> cnt6 = {6};
    ScopedSpace wfs(H5Screate_simple(1, d8.data(), nullptr));
    ASSERT_GE(H5Sselect_hyperslab(wfs, H5S_SELECT_SET, s0.data(), nullptr,
                                  cnt6.data(), nullptr),
              0);
    ScopedSpace wms(H5Screate_simple(1, cnt6.data(), nullptr));
    const std::array<int32_t, 6> wv = {101, 201, 11, 21, 31, 41};
    ASSERT_GE(
        H5Dwrite(v_comb, H5T_NATIVE_INT32, wms, wfs, H5P_DEFAULT, wv.data()),
        0);

    // Query H5D_LOPS_VIRTUAL[0].is_data_cached while source chunked dataset
    // is dirty in cache (true), and on compact source dataset (false).
    auto* d_comb = static_cast<H5D_t*>(H5VL_object_verify(v_comb, H5I_DATASET));
    ASSERT_NE(d_comb, nullptr);
    {
      ScopedApiContext ctx;
      EXPECT_TRUE(H5D_LOPS_VIRTUAL[0].is_data_cached(d_comb->shared));
    }

    // Flush & refresh open virtual dataset (touches both ext_src & ext_pf_0).
    EXPECT_GE(H5Dflush(v_comb), 0);
    EXPECT_GE(H5Drefresh(v_comb), 0);

    // Compact source dataset has ops->is_data_cached == nullptr -> returns
    // false.
    ScopedDataset v_cmp(H5Dopen2(vf, "v_cmp", H5P_DEFAULT));
    std::array<int32_t, 4> cmp_buf{};
    ASSERT_GE(H5Dread(v_cmp, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      cmp_buf.data()),
              0);
    auto* d_cmp = static_cast<H5D_t*>(H5VL_object_verify(v_cmp, H5I_DATASET));
    ASSERT_NE(d_cmp, nullptr);
    {
      ScopedApiContext ctx;
      EXPECT_FALSE(H5D_LOPS_VIRTUAL[0].is_data_cached(d_cmp->shared));
    }
  }
}

extern "C" {
static herr_t FailBoundsFn(const H5S_t*, hsize_t*, hsize_t*) { return -1; }
static hssize_t FailSerialSizeFn(H5S_t*) { return -1; }
static herr_t FailSerializeFn(H5S_t*, uint8_t**) { return -1; }
static int g_serial_size_countdown = 0;
static hssize_t CountdownSerialSizeFn(H5S_t* space) {
  if (g_serial_size_countdown-- <= 0) {
    return -1;
  }
  return H5S_sel_hyper->serial_size(space);
}
}  // extern "C"

TEST(H5DvirtualTest,
     StaticPercentLiteralDoubleCopyConstructInitAndHeapEdgeCases) {
  TempDir dir("vds_edge_10");
  const std::string src_lit = dir.child("src%lit.h5").string();
  const std::string vds_path = dir.child("v_lit.h5").string();

  WriteInt32DatasetFile(src_lit, "/pre_0", {2}, {}, {}, {7, 8});

  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_libver_bounds(fapl, H5F_LIBVER_V110, H5F_LIBVER_LATEST),
              0);
    ScopedFile vf(
        H5Fcreate(vds_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(vf.valid());

    const std::array<hsize_t, 1> d2 = {2};
    ScopedSpace s2(H5Screate_simple(1, d2.data(), nullptr));
    // Local trivial contiguous dataset for internal H5D__open_name ref test.
    ScopedDataset loc_s(H5Dcreate2(vf, "loc_s", H5T_NATIVE_INT32, s2,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(loc_s.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl, s2, dir.child("src%%lit.h5").c_str(),
                             "/pre_0", s2),
              0);
    ScopedDataset vd(H5Dcreate2(vf, "v_lit", H5T_NATIVE_INT32, s2, H5P_DEFAULT,
                                dcpl, H5P_DEFAULT));
    ASSERT_TRUE(vd.valid());
  }

  // Reopen RDWR to hit H5D__virtual_load_layout parsed_source_file_name path,
  // double H5D__virtual_copy_layout, construct, init, and corrupt heap checks.
  {
    ScopedFile vf(H5Fopen(vds_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ASSERT_TRUE(vf.valid());
    auto* f = static_cast<H5F_t*>(H5VL_object_verify(vf, H5I_FILE));
    ASSERT_NE(f, nullptr);

    ScopedDataset vd(H5Dopen2(vf, "v_lit", H5P_DEFAULT));
    ASSERT_TRUE(vd.valid());
    std::array<int32_t, 2> rbuf{};
    ASSERT_GE(H5Dread(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      rbuf.data()),
              0);
    EXPECT_THAT(rbuf, ElementsAre(7, 8));

    auto* d_lit = static_cast<H5D_t*>(H5VL_object_verify(vd, H5I_DATASET));
    ASSERT_NE(d_lit, nullptr);

    ScopedApiContext ctx;

    // Double copy_layout exercises both branches for unescaped literal names
    // (1055-1057, 1062-1063 -> 1060-1061), plus resetting an open source_dset.
    H5O_layout_t copy1{};
    std::memset(&copy1, 0, sizeof(copy1));
    copy1 = d_lit->shared->layout;
    ASSERT_GE(H5D__virtual_copy_layout(&copy1), 0);
    absl::Cleanup cleanup_copy1 = [&copy1]() {
      EXPECT_GE(H5D__virtual_reset_layout(&copy1), 0);
    };

    H5O_layout_t copy2{};
    std::memset(&copy2, 0, sizeof(copy2));
    copy2 = copy1;
    ASSERT_GE(H5D__virtual_copy_layout(&copy2), 0);
    absl::Cleanup cleanup_copy2 = [&copy2]() {
      EXPECT_GE(H5D__virtual_reset_layout(&copy2), 0);
    };

    H5G_loc_t root_loc{};
    root_loc.oloc = H5G_oloc(H5G_rootof(f));
    root_loc.path = H5G_nameof(H5G_rootof(f));
    ASSERT_NE(root_loc.oloc, nullptr);
    ASSERT_NE(root_loc.path, nullptr);
    ScopedPlist dapl_loc(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl_loc.valid());
    H5D_t* opened_loc = H5D__open_name(&root_loc, "loc_s", dapl_loc);
    ASSERT_NE(opened_loc, nullptr);
    copy1.storage.u.virt.list[0].source_dset.dset = opened_loc;

    // Construct version upgrade (v3 -> v4) on V110+ succeeds, and fails on
    // V18 bounded file.
    {
      const unsigned orig_ver = d_lit->shared->layout.version;
      absl::Cleanup restore_ver = [d_lit, orig_ver]() {
        d_lit->shared->layout.version = orig_ver;
      };
      d_lit->shared->layout.version = H5O_LAYOUT_VERSION_3;
      EXPECT_GE(H5D_LOPS_VIRTUAL[0].construct(f, d_lit), 0);
      EXPECT_EQ(d_lit->shared->layout.version, H5O_LAYOUT_VERSION_4);

      ScopedPlist fapl18(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_GE(H5Pset_fapl_core(fapl18, 4096, false), 0);
      ASSERT_GE(H5Pset_libver_bounds(fapl18, H5F_LIBVER_V18, H5F_LIBVER_V18),
                0);
      ScopedFile vf18(
          H5Fcreate("v18_mem.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl18));
      ASSERT_TRUE(vf18.valid());
      auto* f18 = static_cast<H5F_t*>(H5VL_object_verify(vf18, H5I_FILE));
      ASSERT_NE(f18, nullptr);
      d_lit->shared->layout.version = H5O_LAYOUT_VERSION_3;
      ScopedErrorSilence silence;
      EXPECT_LT(H5D_LOPS_VIRTUAL[0].construct(f18, d_lit), 0);
    }

    // Corrupt heap object: 0-byte block (695).
    {
      H5HG_t h0{};
      ASSERT_GE(H5HG_insert(f, 0, nullptr, &h0), 0);
      absl::Cleanup cleanup_h0 = [f, &h0]() {
        EXPECT_GE(H5HG_remove(f, &h0), 0);
      };
      H5O_layout_t bad_l{};
      std::memset(&bad_l, 0, sizeof(bad_l));
      bad_l.type = H5D_VIRTUAL;
      bad_l.version = H5O_LAYOUT_VERSION_4;
      bad_l.storage.type = H5D_VIRTUAL;
      bad_l.storage.u.virt.serial_list_hobjid = h0;
      bad_l.storage.u.virt.source_fapl = H5I_INVALID_HID;
      bad_l.storage.u.virt.source_dapl = H5I_INVALID_HID;
      ScopedErrorSilence silence;
      EXPECT_LT(H5D__virtual_load_layout(f, &bad_l), 0);
      EXPECT_GE(H5D__virtual_reset_layout(&bad_l), 0);
    }

    // Corrupt heap object: valid checksum with +1 extra byte so stored size
    // check fails (922).
    {
      ScopedSpace s_all(
          H5Screate_simple(1, std::array<hsize_t, 1>{2}.data(), nullptr));
      ASSERT_TRUE(s_all.valid());
      bool found_block = false;
      std::vector<uint8_t> blk;
      for (int trial = 0; trial < 32 && !found_block; ++trial) {
        std::vector<uint8_t> prefix;
        prefix.push_back(0);  // heap_vers = 0
        AppendEncodedLength(f, 1, &prefix);
        AppendCString(absl::StrCat("k", trial), &prefix);
        AppendCString("d", &prefix);
        AppendSerializedSelection(s_all, &prefix);
        AppendSerializedSelection(s_all, &prefix);
        const size_t l = prefix.size();
        for (int b = 0; b < 256; ++b) {
          blk = prefix;
          blk.resize(l + 5, 0);
          blk[l] = static_cast<uint8_t>(b);
          const uint32_t csum = H5_checksum_metadata(blk.data(), l + 1, 0);
          std::array<uint8_t, 4> enc{};
          uint8_t* ep = enc.data();
          UINT32ENCODE(ep, csum);
          if (enc[0] == static_cast<uint8_t>(b)) {
            blk[l + 1] = enc[1];
            blk[l + 2] = enc[2];
            blk[l + 3] = enc[3];
            blk[l + 4] = 0xAA;
            found_block = true;
            break;
          }
        }
      }
      ASSERT_TRUE(found_block);
      H5HG_t hsz{};
      ASSERT_GE(H5HG_insert(f, blk.size(), blk.data(), &hsz), 0);
      absl::Cleanup cleanup_hsz = [f, &hsz]() {
        EXPECT_GE(H5HG_remove(f, &hsz), 0);
      };
      H5O_layout_t bad_l{};
      std::memset(&bad_l, 0, sizeof(bad_l));
      bad_l.type = H5D_VIRTUAL;
      bad_l.version = H5O_LAYOUT_VERSION_4;
      bad_l.storage.type = H5D_VIRTUAL;
      bad_l.storage.u.virt.serial_list_hobjid = hsz;
      bad_l.storage.u.virt.source_fapl = H5I_INVALID_HID;
      bad_l.storage.u.virt.source_dapl = H5I_INVALID_HID;
      ScopedErrorSilence silence;
      EXPECT_LT(H5D__virtual_load_layout(f, &bad_l), 0);
      EXPECT_GE(H5D__virtual_reset_layout(&bad_l), 0);
    }

    // Invalid property list IDs in H5D__virtual_copy_layout (1134, 1140, 1151)
    // and H5D__virtual_reset_layout (1275, 1280).
    {
      ScopedErrorSilence silence;
      H5O_layout_t copy_bad{};
      std::memset(&copy_bad, 0, sizeof(copy_bad));
      copy_bad = d_lit->shared->layout;
      copy_bad.storage.u.virt.source_fapl = vd;
      EXPECT_LT(H5D__virtual_copy_layout(&copy_bad), 0);

      std::memset(&copy_bad, 0, sizeof(copy_bad));
      copy_bad = d_lit->shared->layout;
      copy_bad.storage.u.virt.source_dapl = vd;
      EXPECT_LT(H5D__virtual_copy_layout(&copy_bad), 0);

      H5O_layout_t bogus_l{};
      std::memset(&bogus_l, 0, sizeof(bogus_l));
      bogus_l.type = H5D_VIRTUAL;
      bogus_l.storage.type = H5D_VIRTUAL;
      bogus_l.storage.u.virt.source_fapl = static_cast<hid_t>(9999999);
      bogus_l.storage.u.virt.source_dapl = static_cast<hid_t>(9999998);
      EXPECT_LT(H5D__virtual_reset_layout(&bogus_l), 0);
    }

    // H5D_LOPS_VIRTUAL[0].init negative paths (2720, 2756, 2760).
    {
      ScopedErrorSilence silence;
      ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
      ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
      H5D_shared_t fake_shared{};
      std::memset(&fake_shared, 0, sizeof(fake_shared));
      fake_shared.space = d_lit->shared->space;
      fake_shared.layout.type = H5D_VIRTUAL;
      fake_shared.layout.storage.type = H5D_VIRTUAL;
      fake_shared.layout.storage.u.virt.serial_list_hobjid.addr = HADDR_UNDEF;
      fake_shared.layout.storage.u.virt.source_fapl = H5I_INVALID_HID;
      fake_shared.layout.storage.u.virt.source_dapl = H5I_INVALID_HID;
      H5D_t fake_dset{};
      std::memset(&fake_dset, 0, sizeof(fake_dset));
      fake_dset.shared = &fake_shared;

      EXPECT_LT(H5D_LOPS_VIRTUAL[0].init(f, &fake_dset, H5I_INVALID_HID, true),
                0);
      EXPECT_LT(H5D_LOPS_VIRTUAL[0].init(f, &fake_dset, fcpl, true), 0);

      fake_shared.layout.storage.u.virt.serial_list_hobjid =
          d_lit->shared->layout.storage.u.virt.serial_list_hobjid;
      fake_shared.layout.storage.u.virt.serial_list_hobjid.idx = 9999;
      EXPECT_LT(H5D_LOPS_VIRTUAL[0].init(f, &fake_dset, dapl, true), 0);
      EXPECT_GE(H5D__virtual_reset_layout(&fake_shared.layout), 0);
    }
  }

  // Reopen RDONLY: copy and delete fail because heap is not writable
  // (644, 1324, 1365).
  {
    ScopedFile f_ro_id(H5Fopen(vds_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(f_ro_id.valid());
    auto* f_ro = static_cast<H5F_t*>(H5VL_object_verify(f_ro_id, H5I_FILE));
    ASSERT_NE(f_ro, nullptr);
    ScopedDataset vd(H5Dopen2(f_ro_id, "v_lit", H5P_DEFAULT));
    ASSERT_TRUE(vd.valid());
    auto* d_ro = static_cast<H5D_t*>(H5VL_object_verify(vd, H5I_DATASET));
    ASSERT_NE(d_ro, nullptr);

    ScopedApiContext ctx;
    ScopedErrorSilence silence;

    H5O_layout_t copy_ro{};
    std::memset(&copy_ro, 0, sizeof(copy_ro));
    copy_ro = d_ro->shared->layout;
    ASSERT_GE(H5D__virtual_copy_layout(&copy_ro), 0);
    absl::Cleanup cleanup_ro = [&copy_ro]() {
      EXPECT_GE(H5D__virtual_reset_layout(&copy_ro), 0);
    };
    EXPECT_LT(H5D__virtual_copy(f_ro, &copy_ro), 0);

    H5O_storage_t st_ro{};
    std::memset(&st_ro, 0, sizeof(st_ro));
    st_ro.type = H5D_VIRTUAL;
    st_ro.u.virt.serial_list_hobjid =
        d_ro->shared->layout.storage.u.virt.serial_list_hobjid;
    EXPECT_LT(H5D__virtual_delete(f_ro, &st_ro), 0);
  }
}

TEST(H5DvirtualTest, PrintfPartialReadWithoutGetSpaceReclipClampAndCached) {
  TempDir dir("vds_printf_2");
  const std::string pf2_pat = dir.child("pf2_%b.h5").string();
  const std::string ctl_path = dir.child("ctl.h5").string();
  const std::string pf_c_pat = dir.child("pf_c_%b.h5").string();
  const std::string pf_cache_pat = dir.child("pf_cache_%b.h5").string();
  const std::string vds_path = dir.child("v_pr2.h5").string();

  WriteInt32DatasetFile(dir.child("pf2_0.h5").string(), "p", {4}, {}, {},
                        {10, 11, 12, 13});
  WriteInt32DatasetFile(dir.child("pf2_1.h5").string(), "p", {4}, {}, {},
                        {14, 15, 16, 17});
  WriteInt32DatasetFile(ctl_path, "c", {6}, {H5S_UNLIMITED}, {4},
                        {1, 2, 3, 4, 5, 6});
  WriteInt32DatasetFile(dir.child("pf_c_0.h5").string(), "p", {2}, {}, {},
                        {20, 21});
  WriteInt32DatasetFile(dir.child("pf_cache_0.h5").string(), "p", {4},
                        {H5S_UNLIMITED}, {2}, {30, 31, 32, 33});

  // 2D FIRST_MISSING setup: dim0 uses printf (partially clipped at 6),
  // dim1 grows from 4 to 8 on H5Dget_space so dim0 keeps clipped sub_dset
  // across growth (2377) and we test H5S_set_extent bounds failures.
  const std::string pf2d_pat = dir.child("pf2d_%b.h5").string();
  const std::string ctl0_path = dir.child("ctl0.h5").string();
  const std::string ctl1_path = dir.child("ctl1.h5").string();
  WriteInt32DatasetFile(dir.child("pf2d_0.h5").string(), "p", {4, 4}, {}, {},
                        std::vector<int32_t>(16, 44));
  WriteInt32DatasetFile(dir.child("pf2d_1.h5").string(), "p", {4, 4}, {}, {},
                        std::vector<int32_t>(16, 55));
  WriteInt32DatasetFile(ctl0_path, "c0", {6, 4}, {H5S_UNLIMITED, H5S_UNLIMITED},
                        {4, 4}, std::vector<int32_t>(24, 1));
  WriteInt32DatasetFile(ctl1_path, "c1", {6, 4}, {H5S_UNLIMITED, H5S_UNLIMITED},
                        {4, 4}, std::vector<int32_t>(24, 2));

  {
    ScopedFile vf(
        H5Fcreate(vds_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(vf.valid());

    const std::array<hsize_t, 1> d0 = {0};
    const std::array<hsize_t, 1> d4 = {4};
    const std::array<hsize_t, 1> d6 = {6};
    const std::array<hsize_t, 1> dmax = {H5S_UNLIMITED};
    const std::array<hsize_t, 1> s0 = {0};
    const std::array<hsize_t, 1> str4 = {4};
    const std::array<hsize_t, 1> cntu = {H5S_UNLIMITED};

    // 1) FIRST_MISSING: printf + non-printf unlim ctl [6].
    {
      ScopedSpace vs_pf(H5Screate_simple(1, d0.data(), dmax.data()));
      ASSERT_GE(H5Sselect_hyperslab(vs_pf, H5S_SELECT_SET, s0.data(),
                                    str4.data(), cntu.data(), d4.data()),
                0);
      ScopedSpace ss4(H5Screate_simple(1, d4.data(), nullptr));

      ScopedSpace vs_ctl(H5Screate_simple(1, d0.data(), dmax.data()));
      ASSERT_GE(H5Sselect_hyperslab(vs_ctl, H5S_SELECT_SET, s0.data(), nullptr,
                                    cntu.data(), nullptr),
                0);
      ScopedSpace ss_ctl(H5Screate_simple(1, d0.data(), dmax.data()));
      ASSERT_GE(H5Sselect_hyperslab(ss_ctl, H5S_SELECT_SET, s0.data(), nullptr,
                                    cntu.data(), nullptr),
                0);

      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_GE(H5Pset_virtual(dcpl, vs_ctl, ctl_path.c_str(), "c", ss_ctl), 0);
      ASSERT_GE(H5Pset_virtual(dcpl, vs_pf, pf2_pat.c_str(), "p", ss4), 0);
      ScopedSpace vs_init(H5Screate_simple(1, d6.data(), dmax.data()));
      ScopedDataset vd(H5Dcreate2(vf, "v_reclip", H5T_NATIVE_INT32, vs_init,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(vd.valid());
    }

    // 2) Clamp (2976): LAST_AVAILABLE (gap=0), printf only pf_c_0 + limited
    // [6].
    {
      const std::array<hsize_t, 1> d2 = {2};
      const std::array<hsize_t, 1> str2 = {2};
      ScopedSpace vs_pf(H5Screate_simple(1, d0.data(), dmax.data()));
      ASSERT_GE(H5Sselect_hyperslab(vs_pf, H5S_SELECT_SET, s0.data(),
                                    str2.data(), cntu.data(), d2.data()),
                0);
      ScopedSpace ss2(H5Screate_simple(1, d2.data(), nullptr));
      ScopedSpace vs_lim(H5Screate_simple(1, d6.data(), nullptr));
      ASSERT_GE(H5Sselect_hyperslab(vs_lim, H5S_SELECT_SET, s0.data(), nullptr,
                                    d6.data(), nullptr),
                0);
      ScopedSpace ss6(H5Screate_simple(1, d6.data(), nullptr));
      ASSERT_GE(H5Sselect_hyperslab(ss6, H5S_SELECT_SET, s0.data(), nullptr,
                                    d6.data(), nullptr),
                0);
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      const int32_t fv = -7;
      ASSERT_GE(H5Pset_fill_value(dcpl, H5T_NATIVE_INT32, &fv), 0);
      ASSERT_GE(H5Pset_virtual(dcpl, vs_lim, ctl_path.c_str(), "c", ss6), 0);
      ASSERT_GE(H5Pset_virtual(dcpl, vs_pf, pf_c_pat.c_str(), "p", ss2), 0);
      ScopedSpace vs_init(H5Screate_simple(1, d6.data(), dmax.data()));
      ScopedDataset vd(H5Dcreate2(vf, "v_clamp", H5T_NATIVE_INT32, vs_init,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(vd.valid());
    }

    // 3) Printf-only chunked source for is_data_cached (2871, 2873-2878).
    {
      ScopedSpace vs_pf(H5Screate_simple(1, d0.data(), dmax.data()));
      ASSERT_GE(H5Sselect_hyperslab(vs_pf, H5S_SELECT_SET, s0.data(),
                                    str4.data(), cntu.data(), d4.data()),
                0);
      ScopedSpace ss4(H5Screate_simple(1, d4.data(), nullptr));
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_GE(H5Pset_virtual(dcpl, vs_pf, pf_cache_pat.c_str(), "p", ss4), 0);
      ScopedSpace vs_init(H5Screate_simple(1, d4.data(), dmax.data()));
      ScopedDataset vd(H5Dcreate2(vf, "v_pf_cache", H5T_NATIVE_INT32, vs_init,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(vd.valid());
    }

    // 4) 2D dataset (v_2d_reclip): list[0]=ctl1 (dim1 4->8), list[1]=pf2d.
    {
      const std::array<hsize_t, 2> d2_0 = {0, 0};
      const std::array<hsize_t, 2> d2_max = {H5S_UNLIMITED, H5S_UNLIMITED};
      const std::array<hsize_t, 2> d2_blk = {4, 4};
      const std::array<hsize_t, 2> s2_0 = {0, 0};
      const std::array<hsize_t, 2> str2_4 = {4, 1};
      const std::array<hsize_t, 2> cnt_u_dim0 = {H5S_UNLIMITED, 1};
      const std::array<hsize_t, 2> cnt_dim0_6_u1 = {6, H5S_UNLIMITED};
      const std::array<hsize_t, 2> d2_init = {6, 4};

      ScopedSpace vs_pf(H5Screate_simple(2, d2_0.data(), d2_max.data()));
      ASSERT_GE(
          H5Sselect_hyperslab(vs_pf, H5S_SELECT_SET, s2_0.data(), str2_4.data(),
                              cnt_u_dim0.data(), d2_blk.data()),
          0);
      ScopedSpace ss44(H5Screate_simple(2, d2_blk.data(), nullptr));

      ScopedSpace vs_ctl0(H5Screate_simple(2, d2_0.data(), d2_max.data()));
      ASSERT_GE(H5Sselect_hyperslab(vs_ctl0, H5S_SELECT_SET, s2_0.data(),
                                    nullptr, cnt_dim0_6_u1.data(), nullptr),
                0);
      ScopedSpace ss_ctl0(H5Screate_simple(2, d2_0.data(), d2_max.data()));
      ASSERT_GE(H5Sselect_hyperslab(ss_ctl0, H5S_SELECT_SET, s2_0.data(),
                                    nullptr, cnt_dim0_6_u1.data(), nullptr),
                0);

      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_GE(H5Pset_virtual(dcpl, vs_ctl0, ctl1_path.c_str(), "c1", ss_ctl0),
                0);
      ASSERT_GE(H5Pset_virtual(dcpl, vs_pf, pf2d_pat.c_str(), "p", ss44), 0);

      ScopedSpace vs_init(H5Screate_simple(2, d2_init.data(), d2_max.data()));
      ScopedDataset vd(H5Dcreate2(vf, "v_2d_reclip", H5T_NATIVE_INT32, vs_init,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(vd.valid());
    }
  }

  ScopedFile vf(H5Fopen(vds_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
  ASSERT_TRUE(vf.valid());

  // Pass A: Read v_reclip (FIRST_MISSING) WITHOUT H5Dget_space (2990 on partial
  // j=1), then extend ctl.h5 {6} -> {8} and call H5Dget_space (2316-2337).
  {
    ScopedPlist dapl_fm(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_virtual_view(dapl_fm, H5D_VDS_FIRST_MISSING), 0);
    ScopedDataset vd(H5Dopen2(vf, "v_reclip", dapl_fm));
    ASSERT_TRUE(vd.valid());
    std::array<int32_t, 6> rbuf{};
    ASSERT_GE(H5Dread(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      rbuf.data()),
              0);
    EXPECT_THAT(rbuf, ElementsAre(10, 11, 12, 13, 14, 15));

    {
      ScopedFile cf(H5Fopen(ctl_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
      ScopedDataset cd(H5Dopen2(cf, "c", H5P_DEFAULT));
      ASSERT_GE(H5Dset_extent(cd, std::array<hsize_t, 1>{8}.data()), 0);
    }
    ScopedSpace sp(H5Dget_space(vd));
    ASSERT_TRUE(sp.valid());
    EXPECT_EQ(H5Sget_simple_extent_npoints(sp), 8);
  }

  // Clamp (2976): LAST_AVAILABLE (gap=0) with printf sub_dset_nused == 1.
  {
    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_GE(H5Pset_virtual_view(dapl, H5D_VDS_LAST_AVAILABLE), 0);
    ASSERT_GE(H5Pset_virtual_printf_gap(dapl, 0), 0);
    ScopedDataset vd(H5Dopen2(vf, "v_clamp", dapl));
    ASSERT_TRUE(vd.valid());
    ScopedSpace sp(H5Dget_space(vd));
    EXPECT_EQ(H5Sget_simple_extent_npoints(sp), 6);
    std::array<int32_t, 6> rbuf{};
    ASSERT_GE(H5Dread(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      rbuf.data()),
              0);
    EXPECT_THAT(rbuf, ElementsAre(20, 21, 3, 4, 5, 6));
  }

  // Printf-only chunked source: write + is_data_cached (2871, 2873-2878).
  {
    ScopedDataset vd(H5Dopen2(vf, "v_pf_cache", H5P_DEFAULT));
    ASSERT_TRUE(vd.valid());
    const std::array<int32_t, 4> wv = {31, 32, 33, 34};
    ASSERT_GE(H5Dwrite(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       wv.data()),
              0);
    auto* d = static_cast<H5D_t*>(H5VL_object_verify(vd, H5I_DATASET));
    ASSERT_NE(d, nullptr);
    ScopedApiContext ctx;
    EXPECT_TRUE(H5D_LOPS_VIRTUAL[0].is_data_cached(d->shared));
  }

  // 2D reclip/resize: H5Dread creates clipped_virtual_select, then ctl1 grows
  // dim1 4->8 -> hits 2377; capping max[1]=6 hits 2225, 2361, 2366, 2373, 2378.
  {
    ScopedDataset vd(H5Dopen2(vf, "v_2d_reclip", H5P_DEFAULT));
    ASSERT_TRUE(vd.valid());
    std::vector<int32_t> rbuf(6 * 4, 0);
    ASSERT_GE(H5Dread(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      rbuf.data()),
              0);
    EXPECT_EQ(rbuf[0], 44);
    EXPECT_EQ(rbuf[4 * 4], 55);

    {
      ScopedFile cf(H5Fopen(ctl1_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
      ScopedDataset cd(H5Dopen2(cf, "c1", H5P_DEFAULT));
      ASSERT_GE(H5Dset_extent(cd, std::array<hsize_t, 2>{6, 8}.data()), 0);
    }

    auto* d2d = static_cast<H5D_t*>(H5VL_object_verify(vd, H5I_DATASET));
    ASSERT_NE(d2d, nullptr);
    auto* list = d2d->shared->layout.storage.u.virt.list;
    ASSERT_NE(list[1].sub_dset[1].clipped_virtual_select, nullptr);
    ASSERT_NE(list[1].sub_dset[1].clipped_virtual_select,
              list[1].sub_dset[1].virtual_select);
    const std::array<hsize_t, 2> d2_pre = {6, 4};

    ScopedApiContext ctx;
    {
      ScopedErrorSilence silence;
      // 1) Cap d2d->shared->space->extent.max[1] -> H5S_set_extent fails
      // (2225).
      const hsize_t orig_max = d2d->shared->space->extent.max[1];
      d2d->shared->space->extent.max[1] = 6;
      {
        absl::Cleanup r = [&]() {
          d2d->shared->space->extent.max[1] = orig_max;
        };
        EXPECT_LT(H5D__virtual_set_extent_unlim(d2d), 0);
      }

      // 2) Cap list[0].source_dset.clipped_virtual_select->extent.max[1]
      // (2366).
      ASSERT_NE(list[0].source_dset.clipped_virtual_select, nullptr);
      ASSERT_NE(list[0].source_dset.clipped_virtual_select,
                list[0].source_dset.virtual_select);
      const hsize_t orig_cmax =
          list[0].source_dset.clipped_virtual_select->extent.max[1];
      list[0].source_dset.clipped_virtual_select->extent.max[1] = 6;
      {
        absl::Cleanup r = [&]() {
          list[0].source_dset.clipped_virtual_select->extent.max[1] = orig_cmax;
          EXPECT_GE(H5S_set_extent(list[0].source_dset.clipped_virtual_select,
                                   d2_pre.data()),
                    0);
          EXPECT_GE(H5S_set_extent(d2d->shared->space, d2_pre.data()), 0);
        };
        EXPECT_LT(H5D__virtual_set_extent_unlim(d2d), 0);
      }

      // 3) Cap list[0].source_dset.virtual_select->extent.max[1] (2361).
      const hsize_t orig_vmax =
          list[0].source_dset.virtual_select->extent.max[1];
      EXPECT_GE(
          H5S_set_extent(list[0].source_dset.virtual_select, d2_pre.data()), 0);
      list[0].source_dset.virtual_select->extent.max[1] = 6;
      {
        absl::Cleanup r = [&]() {
          list[0].source_dset.virtual_select->extent.max[1] = orig_vmax;
          EXPECT_GE(
              H5S_set_extent(list[0].source_dset.virtual_select, d2_pre.data()),
              0);
          EXPECT_GE(H5S_set_extent(d2d->shared->space, d2_pre.data()), 0);
        };
        EXPECT_LT(H5D__virtual_set_extent_unlim(d2d), 0);
      }

      // 4) Cap list[1].sub_dset[1].clipped_virtual_select->extent.max[1]
      // (2378).
      const hsize_t orig_s1_cmax =
          list[1].sub_dset[1].clipped_virtual_select->extent.max[1];
      EXPECT_GE(H5S_set_extent(list[1].sub_dset[1].clipped_virtual_select,
                               d2_pre.data()),
                0);
      list[1].sub_dset[1].clipped_virtual_select->extent.max[1] = 6;
      {
        absl::Cleanup r = [&]() {
          list[1].sub_dset[1].clipped_virtual_select->extent.max[1] =
              orig_s1_cmax;
          EXPECT_GE(H5S_set_extent(list[1].sub_dset[1].clipped_virtual_select,
                                   d2_pre.data()),
                    0);
          EXPECT_GE(H5S_set_extent(d2d->shared->space, d2_pre.data()), 0);
        };
        EXPECT_LT(H5D__virtual_set_extent_unlim(d2d), 0);
      }

      // 5) Cap list[1].sub_dset[1].virtual_select->extent.max[1] (2373).
      const hsize_t orig_s1_vmax =
          list[1].sub_dset[1].virtual_select->extent.max[1];
      EXPECT_GE(
          H5S_set_extent(list[1].sub_dset[1].virtual_select, d2_pre.data()), 0);
      list[1].sub_dset[1].virtual_select->extent.max[1] = 6;
      {
        absl::Cleanup r = [&]() {
          list[1].sub_dset[1].virtual_select->extent.max[1] = orig_s1_vmax;
          EXPECT_GE(
              H5S_set_extent(list[1].sub_dset[1].virtual_select, d2_pre.data()),
              0);
          EXPECT_GE(H5S_set_extent(d2d->shared->space, d2_pre.data()), 0);
        };
        EXPECT_LT(H5D__virtual_set_extent_unlim(d2d), 0);
      }
    }

    // Finally call cleanly: hits 2377 updating
    // sub_dset[1].clipped_virtual_select.
    ScopedSpace sp(H5Dget_space(vd));
    ASSERT_TRUE(sp.valid());
    std::array<hsize_t, 2> got{};
    ASSERT_EQ(H5Sget_simple_extent_dims(sp, got.data(), nullptr), 2);
    EXPECT_EQ(got[0], 8u);
    EXPECT_EQ(got[1], 8u);
  }
}

TEST(H5DvirtualTest, IoConversionOpenSelectionFillAndTreeErrors) {
  TempDir dir("vds_io_err");
  const std::string opq_path = dir.child("opq_src.h5").string();
  const std::string opq_pf_0 = dir.child("opq_pf_0.h5").string();
  const std::string opq_u_path = dir.child("opq_u.h5").string();
  const std::string grp_path = dir.child("grp_src.h5").string();
  const std::string grp_pf_0 = dir.child("grp_pf_0.h5").string();
  const std::string int1_path = dir.child("int1.h5").string();
  const std::string vds_path = dir.child("v_io_err.h5").string();

  WriteInt32DatasetFile(int1_path, "s", {1}, {}, {}, {99});
  WriteInt32DatasetFile(dir.child("int_fill.h5").string(), "s", {2}, {}, {},
                        {11, 22});
  WriteInt32DatasetFile(dir.child("pf_fill_0.h5").string(), "p", {2}, {}, {},
                        {33, 44});

  ScopedType opq_type(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(opq_type.valid());
  ASSERT_GE(H5Tset_tag(opq_type, "OPAQUE_SRC"), 0);

  auto write_opaque = [&](const std::string& p, const char* name, bool unlim) {
    ScopedFile f(H5Fcreate(p.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(f.valid());
    const std::array<hsize_t, 1> d1 = {1};
    const std::array<hsize_t, 1> dmax = {H5S_UNLIMITED};
    ScopedSpace sp(
        H5Screate_simple(1, d1.data(), unlim ? dmax.data() : nullptr));
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    if (unlim) {
      ASSERT_GE(H5Pset_chunk(dcpl, 1, d1.data()), 0);
    }
    ScopedDataset ds(
        H5Dcreate2(f, name, opq_type, sp, H5P_DEFAULT, dcpl, H5P_DEFAULT));
    const std::array<uint8_t, 4> bytes = {1, 2, 3, 4};
    ASSERT_GE(
        H5Dwrite(ds, opq_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, bytes.data()), 0);
  };
  write_opaque(opq_path, "opq", false);
  write_opaque(opq_pf_0, "p", false);
  write_opaque(opq_u_path, "opq_u", true);

  auto write_group = [](const std::string& p) {
    ScopedFile f(H5Fcreate(p.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(f.valid());
    ScopedGroup g(H5Gcreate2(f, "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
  };
  write_group(grp_path);
  write_group(grp_pf_0);

  {
    ScopedFile vf(
        H5Fcreate(vds_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(vf.valid());

    const std::array<hsize_t, 1> d0 = {0};
    const std::array<hsize_t, 1> d1 = {1};
    const std::array<hsize_t, 1> d2 = {2};
    const std::array<hsize_t, 1> d4 = {4};
    const std::array<hsize_t, 1> d8 = {8};
    const std::array<hsize_t, 1> d51 = {51};
    const std::array<hsize_t, 1> d52 = {52};
    const std::array<hsize_t, 1> d54 = {54};
    const std::array<hsize_t, 1> dmax = {H5S_UNLIMITED};
    const std::array<hsize_t, 1> s0 = {0};
    const std::array<hsize_t, 1> cntu = {H5S_UNLIMITED};
    const std::array<hsize_t, 1> str4 = {4};

    ScopedSpace s1(H5Screate_simple(1, d1.data(), nullptr));
    ScopedSpace s2(H5Screate_simple(1, d2.data(), nullptr));
    ScopedSpace s4(H5Screate_simple(1, d4.data(), nullptr));

    // 1) Non-printf int32 VDS mapped to H5T_OPAQUE source.
    {
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_GE(H5Pset_virtual(dcpl, s1, opq_path.c_str(), "opq", s1), 0);
      ScopedDataset vd(H5Dcreate2(vf, "v_opq", H5T_NATIVE_INT32, s1,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(vd.valid());
    }

    // 2) Printf int32 VDS mapped to H5T_OPAQUE source.
    {
      ScopedSpace vpf(H5Screate_simple(1, d0.data(), dmax.data()));
      ASSERT_GE(H5Sselect_hyperslab(vpf, H5S_SELECT_SET, s0.data(), d2.data(),
                                    cntu.data(), d1.data()),
                0);
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_GE(
          H5Pset_virtual(dcpl, vpf, dir.child("opq_pf_%b.h5").c_str(), "p", s1),
          0);
      ScopedSpace vinit(H5Screate_simple(1, d1.data(), dmax.data()));
      ScopedDataset vd(H5Dcreate2(vf, "v_pf_opq", H5T_NATIVE_INT32, vinit,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(vd.valid());
    }

    // 3) 51-mapping R-tree int32 VDS with [0] = opq leaf, [1..49] = int1,
    // [50] = unlim opq_u (not_in_tree_list).
    {
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ScopedSpace vs(H5Screate_simple(1, d51.data(), dmax.data()));
      for (hsize_t i = 0; i < 50; ++i) {
        ASSERT_GE(H5Sselect_hyperslab(vs, H5S_SELECT_SET, &i, nullptr,
                                      d1.data(), nullptr),
                  0);
        if (i == 0) {
          ASSERT_GE(H5Pset_virtual(dcpl, vs, opq_path.c_str(), "opq", s1), 0);
        } else {
          ASSERT_GE(H5Pset_virtual(dcpl, vs, int1_path.c_str(), "s", s1), 0);
        }
      }
      const hsize_t st50 = 50;
      ASSERT_GE(H5Sselect_hyperslab(vs, H5S_SELECT_SET, &st50, nullptr,
                                    cntu.data(), nullptr),
                0);
      ScopedSpace ssu(H5Screate_simple(1, d1.data(), dmax.data()));
      ASSERT_GE(H5Sselect_hyperslab(ssu, H5S_SELECT_SET, s0.data(), nullptr,
                                    cntu.data(), nullptr),
                0);
      ASSERT_GE(H5Pset_virtual(dcpl, vs, opq_u_path.c_str(), "opq_u", ssu), 0);
      ScopedSpace vinit(H5Screate_simple(1, d51.data(), dmax.data()));
      ScopedDataset vd(H5Dcreate2(vf, "v_rt_opq", H5T_NATIVE_INT32, vinit,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(vd.valid());
    }

    // 4) Non-printf unlim mapped to Group "g".
    {
      ScopedSpace vs(H5Screate_simple(1, d0.data(), dmax.data()));
      ASSERT_GE(H5Sselect_hyperslab(vs, H5S_SELECT_SET, s0.data(), nullptr,
                                    cntu.data(), nullptr),
                0);
      ScopedSpace ss(H5Screate_simple(1, d0.data(), dmax.data()));
      ASSERT_GE(H5Sselect_hyperslab(ss, H5S_SELECT_SET, s0.data(), nullptr,
                                    cntu.data(), nullptr),
                0);
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_GE(H5Pset_virtual(dcpl, vs, grp_path.c_str(), "g", ss), 0);
      ScopedSpace vinit(H5Screate_simple(1, d1.data(), dmax.data()));
      ScopedDataset vd(H5Dcreate2(vf, "v_grp", H5T_NATIVE_INT32, vinit,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(vd.valid());
    }

    // 5) Printf partial (extent {2}, block 4 -> 2990-2991) & full (extent {4}
    // -> 3075) mapped to Group "g", plus "s/sub" traversal failure (1435).
    {
      ScopedSpace vs(H5Screate_simple(1, d0.data(), dmax.data()));
      ASSERT_GE(H5Sselect_hyperslab(vs, H5S_SELECT_SET, s0.data(), str4.data(),
                                    cntu.data(), d4.data()),
                0);
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_GE(
          H5Pset_virtual(dcpl, vs, dir.child("grp_pf_%b.h5").c_str(), "g", s4),
          0);
      ScopedSpace vpart(H5Screate_simple(1, d2.data(), dmax.data()));
      ScopedDataset vp(H5Dcreate2(vf, "v_pf_grp_part", H5T_NATIVE_INT32, vpart,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ScopedSpace vfull(H5Screate_simple(1, d4.data(), dmax.data()));
      ScopedDataset vfu(H5Dcreate2(vf, "v_pf_grp_full", H5T_NATIVE_INT32, vfull,
                                   H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(vp.valid() && vfu.valid());

      ScopedPlist dcpl_trav(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_GE(H5Pset_virtual(dcpl_trav, s1, int1_path.c_str(), "s/sub", s1),
                0);
      ScopedDataset vt(H5Dcreate2(vf, "v_trav_err", H5T_NATIVE_INT32, s1,
                                  H5P_DEFAULT, dcpl_trav, H5P_DEFAULT));
      ASSERT_TRUE(vt.valid());
    }

    // 6) R-tree Group "g" ([0] leaf + [50] printf not_in_tree_list ->
    // 3187/3194).
    {
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ScopedSpace vs(H5Screate_simple(1, d54.data(), dmax.data()));
      for (hsize_t i = 0; i < 50; ++i) {
        ASSERT_GE(H5Sselect_hyperslab(vs, H5S_SELECT_SET, &i, nullptr,
                                      d1.data(), nullptr),
                  0);
        if (i == 0) {
          ASSERT_GE(H5Pset_virtual(dcpl, vs, grp_path.c_str(), "g", s1), 0);
        } else {
          ASSERT_GE(H5Pset_virtual(dcpl, vs, int1_path.c_str(), "s", s1), 0);
        }
      }
      const hsize_t st50 = 50;
      ASSERT_GE(H5Sselect_hyperslab(vs, H5S_SELECT_SET, &st50, str4.data(),
                                    cntu.data(), d4.data()),
                0);
      ASSERT_GE(
          H5Pset_virtual(dcpl, vs, dir.child("grp_pf_%b.h5").c_str(), "g", s4),
          0);
      ScopedSpace vinit(H5Screate_simple(1, d54.data(), dmax.data()));
      ScopedDataset vd(H5Dcreate2(vf, "v_rt_grp", H5T_NATIVE_INT32, vinit,
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(vd.valid());
    }

    // 7) Fill-value datasets with point mem_space subtraction failure
    // (3453 on printf, 3458 on non-printf).
    {
      const int32_t fv = -5;
      ScopedPlist dcpl_np(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_GE(H5Pset_fill_value(dcpl_np, H5T_NATIVE_INT32, &fv), 0);
      ScopedSpace vs8(H5Screate_simple(1, d8.data(), nullptr));
      ASSERT_GE(H5Sselect_hyperslab(vs8, H5S_SELECT_SET, s0.data(), nullptr,
                                    d2.data(), nullptr),
                0);
      ASSERT_GE(H5Pset_virtual(dcpl_np, vs8, dir.child("int_fill.h5").c_str(),
                               "s", s2),
                0);
      ScopedDataset vnp(H5Dcreate2(vf, "v_fill_np", H5T_NATIVE_INT32, vs8,
                                   H5P_DEFAULT, dcpl_np, H5P_DEFAULT));
      ASSERT_TRUE(vnp.valid());

      ScopedPlist dcpl_pf(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_GE(H5Pset_fill_value(dcpl_pf, H5T_NATIVE_INT32, &fv), 0);
      ScopedSpace vspf(H5Screate_simple(1, d0.data(), dmax.data()));
      ASSERT_GE(H5Sselect_hyperslab(vspf, H5S_SELECT_SET, s0.data(),
                                    str4.data(), cntu.data(), d2.data()),
                0);
      ASSERT_GE(H5Pset_virtual(dcpl_pf, vspf,
                               dir.child("pf_fill_%b.h5").c_str(), "p", s2),
                0);
      ScopedSpace vs8u(H5Screate_simple(1, d8.data(), dmax.data()));
      ScopedDataset vpf(H5Dcreate2(vf, "v_fill_pf", H5T_NATIVE_INT32, vs8u,
                                   H5P_DEFAULT, dcpl_pf, H5P_DEFAULT));
      ASSERT_TRUE(vpf.valid());
    }

    // 8) 52-mapping dataset with DAPL tree disabled + missing external target
    // for efc/tree/mapping error checks.
    {
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ScopedSpace vs(H5Screate_simple(1, d52.data(), nullptr));
      for (hsize_t i = 0; i < 52; ++i) {
        ASSERT_GE(H5Sselect_hyperslab(vs, H5S_SELECT_SET, &i, nullptr,
                                      d1.data(), nullptr),
                  0);
        ASSERT_GE(H5Pset_virtual(dcpl, vs, int1_path.c_str(), "s", s1), 0);
      }
      ScopedDataset vd(H5Dcreate2(vf, "v52", H5T_NATIVE_INT32, vs, H5P_DEFAULT,
                                  dcpl, H5P_DEFAULT));
      ASSERT_TRUE(vd.valid());

      // Scalar 50-mapping dataset for rank==0 (3997, 3633).
      ScopedSpace sc(H5Screate(H5S_SCALAR));
      ScopedPlist dcpl_sc(H5Pcreate(H5P_DATASET_CREATE));
      for (int i = 0; i < 50; ++i) {
        ASSERT_GE(H5Pset_virtual(dcpl_sc, sc, int1_path.c_str(), "s", s1), 0);
      }
      ScopedDataset vsc(H5Dcreate2(vf, "v_sc50_err", H5T_NATIVE_INT32, sc,
                                   H5P_DEFAULT, dcpl_sc, H5P_DEFAULT));
      ASSERT_TRUE(vsc.valid());

      // Local trivial missing external dataset for H5F__efc_open fail (1413).
      ScopedPlist dcpl_miss(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_GE(H5Pset_virtual(dcpl_miss, s1, dir.child("nofile.h5").c_str(),
                               "s", s1),
                0);
      ScopedDataset vmiss(H5Dcreate2(vf, "v_efc_miss", H5T_NATIVE_INT32, s1,
                                     H5P_DEFAULT, dcpl_miss, H5P_DEFAULT));
      ASSERT_TRUE(vmiss.valid());
    }
  }

  ScopedFile vf(H5Fopen(vds_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
  ASSERT_TRUE(vf.valid());
  ScopedErrorSilence silence;

  // Non-printf & printf H5T_OPAQUE read/write failures.
  {
    int32_t val = 0;
    ScopedDataset v_opq(H5Dopen2(vf, "v_opq", H5P_DEFAULT));
    EXPECT_LT(
        H5Dread(v_opq, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val),
        0);
    EXPECT_LT(
        H5Dwrite(v_opq, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val),
        0);

    ScopedDataset v_pf(H5Dopen2(vf, "v_pf_opq", H5P_DEFAULT));
    EXPECT_LT(
        H5Dread(v_pf, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val),
        0);
    EXPECT_LT(
        H5Dwrite(v_pf, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val),
        0);
  }

  // R-tree [0] and [50] H5T_OPAQUE read/write failures (reopened cleanly).
  for (hsize_t target : {hsize_t{0}, hsize_t{50}}) {
    const std::array<hsize_t, 1> d51 = {51};
    const std::array<hsize_t, 1> d1 = {1};
    ScopedSpace fs(H5Screate_simple(1, d51.data(), nullptr));
    ASSERT_GE(H5Sselect_hyperslab(fs, H5S_SELECT_SET, &target, nullptr,
                                  d1.data(), nullptr),
              0);
    ScopedSpace ms(H5Screate_simple(1, d1.data(), nullptr));
    int32_t val = 0;

    {
      ScopedDataset vd(H5Dopen2(vf, "v_rt_opq", H5P_DEFAULT));
      EXPECT_LT(H5Dread(vd, H5T_NATIVE_INT32, ms, fs, H5P_DEFAULT, &val), 0);
    }
    {
      ScopedDataset vd(H5Dopen2(vf, "v_rt_opq", H5P_DEFAULT));
      EXPECT_LT(H5Dwrite(vd, H5T_NATIVE_INT32, ms, fs, H5P_DEFAULT, &val), 0);
    }
  }

  // Group "g" and traversal failures.
  {
    int32_t val = 0;
    {
      ScopedDataset vd(H5Dopen2(vf, "v_grp", H5P_DEFAULT));
      EXPECT_FALSE(ScopedSpace(H5Dget_space(vd)).valid());
    }
    {
      ScopedDataset vd(H5Dopen2(vf, "v_grp", H5P_DEFAULT));
      EXPECT_LT(
          H5Dread(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val),
          0);
    }
    {
      ScopedDataset vd(H5Dopen2(vf, "v_grp", H5P_DEFAULT));
      EXPECT_LT(
          H5Dwrite(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val),
          0);
    }

    // Printf Group "g" (2138, 2990-2991, 3075) + "s/sub" traversal (1435).
    std::array<int32_t, 4> v4{};
    {
      ScopedDataset vd(H5Dopen2(vf, "v_pf_grp_part", H5P_DEFAULT));
      EXPECT_FALSE(ScopedSpace(H5Dget_space(vd)).valid());
    }
    {
      ScopedDataset vd(H5Dopen2(vf, "v_pf_grp_part", H5P_DEFAULT));
      EXPECT_LT(H5Dread(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                        v4.data()),
                0);
    }
    {
      ScopedDataset vd(H5Dopen2(vf, "v_pf_grp_full", H5P_DEFAULT));
      EXPECT_LT(H5Dread(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                        v4.data()),
                0);
    }
    {
      ScopedDataset vt(H5Dopen2(vf, "v_trav_err", H5P_DEFAULT));
      EXPECT_LT(
          H5Dread(vt, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val),
          0);
    }

    // R-tree Group "g" [0] (3187) and [50] (3194).
    const std::array<hsize_t, 1> d54 = {54};
    const std::array<hsize_t, 1> d1 = {1};
    ScopedSpace ms1(H5Screate_simple(1, d1.data(), nullptr));
    {
      ScopedDataset vd(H5Dopen2(vf, "v_rt_grp", H5P_DEFAULT));
      ScopedSpace fs(H5Screate_simple(1, d54.data(), nullptr));
      const hsize_t s0 = 0;
      ASSERT_GE(H5Sselect_hyperslab(fs, H5S_SELECT_SET, &s0, nullptr, d1.data(),
                                    nullptr),
                0);
      EXPECT_LT(H5Dread(vd, H5T_NATIVE_INT32, ms1, fs, H5P_DEFAULT, &val), 0);
    }
    {
      ScopedDataset vd(H5Dopen2(vf, "v_rt_grp", H5P_DEFAULT));
      ScopedSpace fs(H5Screate_simple(1, d54.data(), nullptr));
      const hsize_t s50 = 50;
      ASSERT_GE(H5Sselect_hyperslab(fs, H5S_SELECT_SET, &s50, nullptr,
                                    d1.data(), nullptr),
                0);
      EXPECT_LT(H5Dread(vd, H5T_NATIVE_INT32, ms1, fs, H5P_DEFAULT, &val), 0);
    }
  }

  // H5F__efc_open failure (1413) with source_fapl set to non-FAPL fcpl.
  {
    ScopedDataset vd(H5Dopen2(vf, "v_efc_miss", H5P_DEFAULT));
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    auto* d = static_cast<H5D_t*>(H5VL_object_verify(vd, H5I_DATASET));
    ASSERT_NE(d, nullptr);
    const hid_t orig_fapl = d->shared->layout.storage.u.virt.source_fapl;
    d->shared->layout.storage.u.virt.source_fapl = fcpl;
    absl::Cleanup r = [&]() {
      d->shared->layout.storage.u.virt.source_fapl = orig_fapl;
    };
    int32_t val = 0;
    EXPECT_LT(
        H5Dread(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val), 0);
  }

  // H5Sselect_none on printf VDS: write hits pre_io (2954, 3649).
  {
    ScopedDataset vd(H5Dopen2(vf, "v_fill_pf", H5P_DEFAULT));
    const std::array<hsize_t, 1> d8 = {8};
    ScopedSpace fs_none(H5Screate_simple(1, d8.data(), nullptr));
    ASSERT_GE(H5Sselect_none(fs_none), 0);
    ScopedSpace ms_none(H5Screate_simple(1, d8.data(), nullptr));
    ASSERT_GE(H5Sselect_none(ms_none), 0);
    std::array<int32_t, 8> buf{};
    // With none selection on printf mapping, pre_io computes bounds -> fail.
    EXPECT_LT(H5Dwrite(vd, H5T_NATIVE_INT32, ms_none, fs_none, H5P_DEFAULT,
                       buf.data()),
              0);
  }

  // Point mem_space on fill read hits H5S_select_subtract failure (3453, 3458).
  {
    const std::array<hsize_t, 1> d8 = {8};
    const std::array<hsize_t, 8> pts = {0, 1, 2, 3, 4, 5, 6, 7};
    ScopedSpace ms_pts(H5Screate_simple(1, d8.data(), nullptr));
    ASSERT_GE(H5Sselect_elements(ms_pts, H5S_SELECT_SET, 8, pts.data()), 0);
    std::array<int32_t, 8> buf{};

    ScopedDataset vnp(H5Dopen2(vf, "v_fill_np", H5P_DEFAULT));
    EXPECT_LT(H5Dread(vnp, H5T_NATIVE_INT32, ms_pts, H5S_ALL, H5P_DEFAULT,
                      buf.data()),
              0);

    ScopedDataset vpf(H5Dopen2(vf, "v_fill_pf", H5P_DEFAULT));
    EXPECT_LT(H5Dread(vpf, H5T_NATIVE_INT32, ms_pts, H5S_ALL, H5P_DEFAULT,
                      buf.data()),
              0);
  }

  // Direct ser_read fill errors (3436 bad fill.size, 3463 H5D__fill failure).
  {
    ScopedDataset vnp(H5Dopen2(vf, "v_fill_np", H5P_DEFAULT));
    auto* d = static_cast<H5D_t*>(H5VL_object_verify(vnp, H5I_DATASET));
    ASSERT_NE(d, nullptr);
    const std::array<hsize_t, 1> d8 = {8};
    ScopedSpace sp(H5Screate_simple(1, d8.data(), nullptr));
    auto* s_ptr = static_cast<H5S_t*>(H5I_object_verify(sp, H5I_DATASPACE));
    ASSERT_NE(s_ptr, nullptr);
    auto* t_opq =
        static_cast<H5T_t*>(H5I_object_verify(opq_type, H5I_DATATYPE));
    ASSERT_NE(t_opq, nullptr);

    ScopedApiContext ctx;
    std::array<int32_t, 8> buf{};
    H5D_io_info_t io_info{};
    std::memset(&io_info, 0, sizeof(io_info));
    H5D_dset_io_info_t dset_info{};
    std::memset(&dset_info, 0, sizeof(dset_info));
    dset_info.dset = d;
    dset_info.mem_space = s_ptr;
    dset_info.file_space = s_ptr;
    dset_info.buf.vp = buf.data();
    dset_info.type_info.dst_type = d->shared->type;
    dset_info.type_info.mem_type = d->shared->type;

    {
      const ssize_t orig_sz = d->shared->dcpl_cache.fill.size;
      d->shared->dcpl_cache.fill.size = -2;
      absl::Cleanup r = [&]() { d->shared->dcpl_cache.fill.size = orig_sz; };
      EXPECT_LT(H5D_LOPS_VIRTUAL[0].ser_read(&io_info, &dset_info), 0);
    }

    dset_info.type_info.mem_type = t_opq;
    EXPECT_LT(H5D_LOPS_VIRTUAL[0].ser_read(&io_info, &dset_info), 0);
  }

  // v52 DAPL / mappings_to_leaves / pre-existing not_in_tree_list (4267, 4270,
  // 3374, 3620, 3991, 4100, 3387, 4128-4129), plus scalar v_sc50_err (3997,
  // 3633).
  {
    std::array<int32_t, 52> buf{};
    {
      ScopedDataset vd(H5Dopen2(vf, "v52", H5P_DEFAULT));
      auto* d = static_cast<H5D_t*>(H5VL_object_verify(vd, H5I_DATASET));
      ASSERT_NE(d, nullptr);
      ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
      const hid_t orig_dapl = d->shared->dapl_id;
      absl::Cleanup restore_dapl = [&]() { d->shared->dapl_id = orig_dapl; };

      d->shared->dapl_id = static_cast<hid_t>(999999);
      EXPECT_LT(H5Dread(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                        buf.data()),
                0);
      EXPECT_LT(H5Dwrite(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                         buf.data()),
                0);

      d->shared->dapl_id = fcpl;
      EXPECT_LT(H5Dread(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                        buf.data()),
                0);

      d->shared->dapl_id = orig_dapl;
      // list[0].virtual_select == NULL -> mappings_to_leaves fails (3991, 4100,
      // 3387).
      H5S_t* orig_vs0 =
          d->shared->layout.storage.u.virt.list[0].source_dset.virtual_select;
      d->shared->layout.storage.u.virt.list[0].source_dset.virtual_select =
          nullptr;
      {
        absl::Cleanup r = [&]() {
          d->shared->layout.storage.u.virt.list[0].source_dset.virtual_select =
              orig_vs0;
        };
        EXPECT_LT(H5Dread(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                          buf.data()),
                  0);
      }
      // Pre-existing not_in_tree_list freed cleanly when build_tree has 0
      // non-tree mappings (4128-4129).
      d->shared->layout.storage.u.virt.not_in_tree_list =
          static_cast<H5O_storage_virtual_ent_t**>(
              H5MM_malloc(sizeof(H5O_storage_virtual_ent_t*)));
      ASSERT_GE(H5Dread(vd, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                        buf.data()),
                0);
      EXPECT_EQ(d->shared->layout.storage.u.virt.not_in_tree_list, nullptr);
    }

    // Scalar v_sc50_err: swap shared->space to 1D so rank!=0 reaches
    // mappings_to_leaves -> rank==0 fails (3997, 3633).
    {
      ScopedDataset vsc(H5Dopen2(vf, "v_sc50_err", H5P_DEFAULT));
      auto* d = static_cast<H5D_t*>(H5VL_object_verify(vsc, H5I_DATASET));
      ASSERT_NE(d, nullptr);
      ScopedSpace sp1(
          H5Screate_simple(1, std::array<hsize_t, 1>{1}.data(), nullptr));
      auto* s1_ptr = static_cast<H5S_t*>(H5I_object_verify(sp1, H5I_DATASPACE));
      H5S_t* orig_sp = d->shared->space;
      d->shared->space = s1_ptr;
      absl::Cleanup restore_sp = [&]() { d->shared->space = orig_sp; };
      ScopedApiContext ctx;
      int32_t sval = 0;
      H5D_io_info_t io_info{};
      std::memset(&io_info, 0, sizeof(io_info));
      H5D_dset_io_info_t dinfo{};
      std::memset(&dinfo, 0, sizeof(dinfo));
      dinfo.dset = d;
      dinfo.mem_space = s1_ptr;
      dinfo.file_space = s1_ptr;
      dinfo.buf.cvp = &sval;
      EXPECT_LT(H5D_LOPS_VIRTUAL[0].ser_write(&io_info, &dinfo), 0);
    }
  }
}

TEST(H5DvirtualTest, InternalDataspaceAndExtentErrorChecks) {
  TempDir dir("vds_internal_err");
  const std::string ext_path = dir.child("ext.h5").string();
  const std::string ext1_path = dir.child("ext1.h5").string();
  const std::string pf_path = dir.child("pf_0.h5").string();
  const std::string vds_path = dir.child("v_int_err.h5").string();

  WriteInt32DatasetFile(ext_path, "s", {4}, {}, {}, {1, 2, 3, 4});
  WriteInt32DatasetFile(ext1_path, "s", {1}, {}, {}, {99});
  WriteInt32DatasetFile(pf_path, "p", {4}, {}, {}, {5, 6, 7, 8});

  const std::array<hsize_t, 1> d0 = {0};
  const std::array<hsize_t, 1> d1 = {1};
  const std::array<hsize_t, 1> d4 = {4};
  const std::array<hsize_t, 1> d52 = {52};
  const std::array<hsize_t, 1> dmax = {H5S_UNLIMITED};
  const std::array<hsize_t, 1> s0 = {0};
  const std::array<hsize_t, 1> str4 = {4};
  const std::array<hsize_t, 1> cntu = {H5S_UNLIMITED};

  ScopedPlist fapl_v200(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_libver_bounds(fapl_v200, H5F_LIBVER_V200, H5F_LIBVER_LATEST),
            0);

  // VDS with printf dataset + "." local copy test + 52-mapping tree dataset.
  {
    ScopedFile vf(
        H5Fcreate(vds_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v200));
    ASSERT_TRUE(vf.valid());

    ScopedSpace s4(H5Screate_simple(1, d4.data(), nullptr));
    ScopedSpace s4_hyp(H5Screate_simple(1, d4.data(), nullptr));
    ASSERT_GE(H5Sselect_hyperslab(s4_hyp, H5S_SELECT_SET, s0.data(), nullptr,
                                  d4.data(), nullptr),
              0);
    ScopedSpace s4_src_hyp(H5Screate_simple(1, d4.data(), nullptr));
    ASSERT_GE(H5Sselect_hyperslab(s4_src_hyp, H5S_SELECT_SET, s0.data(),
                                  nullptr, d4.data(), nullptr),
              0);
    ScopedDataset loc(H5Dcreate2(vf, "loc", H5T_NATIVE_INT32, s4, H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
    ScopedPlist dcpl_loc(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl_loc, s4_hyp, ".", "loc", s4_src_hyp), 0);
    ScopedDataset v_loc(H5Dcreate2(vf, "v_loc", H5T_NATIVE_INT32, s4,
                                   H5P_DEFAULT, dcpl_loc, H5P_DEFAULT));

    ScopedSpace vs_pf(H5Screate_simple(1, d0.data(), dmax.data()));
    ASSERT_GE(H5Sselect_hyperslab(vs_pf, H5S_SELECT_SET, s0.data(), str4.data(),
                                  cntu.data(), d4.data()),
              0);
    ScopedPlist dcpl_pf(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(
        H5Pset_virtual(dcpl_pf, vs_pf, dir.child("pf_%b.h5").c_str(), "p", s4),
        0);
    ScopedSpace vinit(H5Screate_simple(1, d4.data(), dmax.data()));
    ScopedDataset v_pf(H5Dcreate2(vf, "v_pf", H5T_NATIVE_INT32, vinit,
                                  H5P_DEFAULT, dcpl_pf, H5P_DEFAULT));

    ScopedPlist dcpl52(H5Pcreate(H5P_DATASET_CREATE));
    ScopedSpace s1(H5Screate_simple(1, d1.data(), nullptr));
    ScopedSpace vs52(H5Screate_simple(1, d52.data(), nullptr));
    for (hsize_t i = 0; i < 52; ++i) {
      ASSERT_GE(H5Sselect_hyperslab(vs52, H5S_SELECT_SET, &i, nullptr,
                                    d1.data(), nullptr),
                0);
      ASSERT_GE(H5Pset_virtual(dcpl52, vs52, ext1_path.c_str(), "s", s1), 0);
    }
    ScopedDataset v52(H5Dcreate2(vf, "v52", H5T_NATIVE_INT32, vs52, H5P_DEFAULT,
                                 dcpl52, H5P_DEFAULT));
    ASSERT_TRUE(v_loc.valid() && v_pf.valid() && v52.valid());
  }

  ScopedFile vf(H5Fopen(vds_path.c_str(), H5F_ACC_RDWR, fapl_v200));
  ASSERT_TRUE(vf.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object_verify(vf, H5I_FILE));
  ASSERT_NE(f, nullptr);

  H5S_select_class_t err_sel = H5S_sel_hyper[0];
  err_sel.type = H5S_SEL_ERROR;

  H5S_select_class_t fail_bounds_cls = H5S_sel_hyper[0];
  fail_bounds_cls.bounds = FailBoundsFn;

  H5S_select_class_t fail_ssize_cls = H5S_sel_hyper[0];
  fail_ssize_cls.serial_size = FailSerialSizeFn;

  H5S_select_class_t countdown_ssize_cls = H5S_sel_hyper[0];
  countdown_ssize_cls.serial_size = CountdownSerialSizeFn;

  H5S_select_class_t fail_ser_cls = H5S_sel_hyper[0];
  fail_ser_cls.serialize = FailSerializeFn;

  ScopedApiContext ctx;
  ScopedErrorSilence silence;

  // 1) check_mapping_pre, update_min_dims, check_min_dims, store_layout checks.
  {
    ScopedDataset v_loc(H5Dopen2(vf, "v_loc", H5P_DEFAULT));
    auto* d_loc = static_cast<H5D_t*>(H5VL_object_verify(v_loc, H5I_DATASET));
    ASSERT_NE(d_loc, nullptr);

    ScopedSpace sp_a(H5Screate_simple(1, d4.data(), nullptr));
    ScopedSpace sp_b(H5Screate_simple(1, d4.data(), nullptr));
    ASSERT_GE(H5Sselect_hyperslab(sp_a, H5S_SELECT_SET, s0.data(), nullptr,
                                  d4.data(), nullptr),
              0);
    ASSERT_GE(H5Sselect_hyperslab(sp_b, H5S_SELECT_SET, s0.data(), nullptr,
                                  d4.data(), nullptr),
              0);
    auto* sa = static_cast<H5S_t*>(H5I_object_verify(sp_a, H5I_DATASPACE));
    auto* sb = static_cast<H5S_t*>(H5I_object_verify(sp_b, H5I_DATASPACE));

    // check_mapping_pre (204, 209).
    {
      const H5S_select_class_t* orig = sa->select.type;
      sa->select.type = &err_sel;
      absl::Cleanup r = [&]() { sa->select.type = orig; };
      EXPECT_LT(
          H5D_virtual_check_mapping_pre(sa, sb, H5O_VIRTUAL_STATUS_INVALID), 0);
    }
    {
      const H5S_select_class_t* orig = sb->select.type;
      sb->select.type = &err_sel;
      absl::Cleanup r = [&]() { sb->select.type = orig; };
      EXPECT_LT(
          H5D_virtual_check_mapping_pre(sa, sb, H5O_VIRTUAL_STATUS_INVALID), 0);
    }

    // update_min_dims (356, 364, 368) and check_min_dims (406).
    H5O_layout_t copy_loc = d_loc->shared->layout;
    ASSERT_GE(H5D__virtual_copy_layout(&copy_loc), 0);
    absl::Cleanup cleanup_copy = [&]() {
      EXPECT_GE(H5D__virtual_reset_layout(&copy_loc), 0);
    };
    H5S_t* vs_loc = copy_loc.storage.u.virt.list[0].source_dset.virtual_select;
    H5S_t* ss_loc = copy_loc.storage.u.virt.list[0].source_select;
    {
      const H5S_select_class_t* orig = vs_loc->select.type;
      vs_loc->select.type = &err_sel;
      {
        absl::Cleanup r = [&]() { vs_loc->select.type = orig; };
        EXPECT_LT(H5D_virtual_update_min_dims(&copy_loc, 0), 0);
      }
      vs_loc->select.type = &fail_bounds_cls;
      {
        absl::Cleanup r = [&]() { vs_loc->select.type = orig; };
        EXPECT_LT(H5D_virtual_update_min_dims(&copy_loc, 0), 0);
      }
      const H5S_class_t orig_et = vs_loc->extent.type;
      vs_loc->extent.type = H5S_NO_CLASS;
      {
        absl::Cleanup r = [&]() { vs_loc->extent.type = orig_et; };
        EXPECT_LT(H5D_virtual_update_min_dims(&copy_loc, 0), 0);
      }
    }
    {
      const H5S_class_t orig_et = d_loc->shared->space->extent.type;
      d_loc->shared->space->extent.type = H5S_NO_CLASS;
      absl::Cleanup r = [&]() { d_loc->shared->space->extent.type = orig_et; };
      EXPECT_LT(H5D_virtual_check_min_dims(d_loc), 0);
    }

    // store_layout failures (502, 507, 546, 551, 631, 635).
    copy_loc.storage.u.virt.serial_list_hobjid.addr = HADDR_UNDEF;
    const H5S_select_class_t* orig_ss = ss_loc->select.type;
    const H5S_select_class_t* orig_vs = vs_loc->select.type;
    {
      ss_loc->select.type = &fail_ssize_cls;
      absl::Cleanup r = [&]() { ss_loc->select.type = orig_ss; };
      EXPECT_LT(H5D__virtual_store_layout(f, &copy_loc), 0);
    }
    {
      vs_loc->select.type = &fail_ssize_cls;
      absl::Cleanup r = [&]() { vs_loc->select.type = orig_vs; };
      EXPECT_LT(H5D__virtual_store_layout(f, &copy_loc), 0);
    }
    {
      ss_loc->select.type = &countdown_ssize_cls;
      vs_loc->select.type = &countdown_ssize_cls;
      absl::Cleanup r = [&]() {
        ss_loc->select.type = orig_ss;
        vs_loc->select.type = orig_vs;
      };
      g_serial_size_countdown = 2;
      EXPECT_LT(H5D__virtual_store_layout(f, &copy_loc), 0);
      g_serial_size_countdown = 3;
      EXPECT_LT(H5D__virtual_store_layout(f, &copy_loc), 0);
    }
    {
      ss_loc->select.type = &fail_ser_cls;
      absl::Cleanup r = [&]() { ss_loc->select.type = orig_ss; };
      EXPECT_LT(H5D__virtual_store_layout(f, &copy_loc), 0);
    }
    {
      vs_loc->select.type = &fail_ser_cls;
      absl::Cleanup r = [&]() { vs_loc->select.type = orig_vs; };
      EXPECT_LT(H5D__virtual_store_layout(f, &copy_loc), 0);
    }
  }

  // 2) set_extent_unlim, init_all, pre_io on printf VDS (1944, 2174, 2185,
  // 2425, 2950, 3006).
  {
    ScopedDataset v_pf(H5Dopen2(vf, "v_pf", H5P_DEFAULT));
    auto* d_pf = static_cast<H5D_t*>(H5VL_object_verify(v_pf, H5I_DATASET));
    ASSERT_NE(d_pf, nullptr);

    // 1944 and 2425: invalid extent on shared->space BEFORE init.
    {
      const H5S_class_t orig_et = d_pf->shared->space->extent.type;
      d_pf->shared->space->extent.type = H5S_NO_CLASS;
      absl::Cleanup r = [&]() { d_pf->shared->space->extent.type = orig_et; };
      EXPECT_LT(H5D__virtual_set_extent_unlim(d_pf), 0);
      std::array<int32_t, 4> buf{};
      EXPECT_LT(H5Dread(v_pf, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                        buf.data()),
                0);
    }

    // Initialize via H5Dget_space (creates sub_dset[0] and [1] on
    // FIRST_MISSING, wait gap=0 leaves sub_dset[0] as valid, sub_dset[1] as
    // first_missing).
    EXPECT_GE(H5D__virtual_set_extent_unlim(d_pf), 0);
    auto* virt = &d_pf->shared->layout.storage.u.virt;
    ASSERT_GE(virt->list[0].sub_dset_nused, 1u);

    // 2185 (FIRST_MISSING bounds on sub_dset[1].virtual_select).
    {
      H5S_t* sub1_vs = virt->list[0].sub_dset[1].virtual_select;
      ASSERT_NE(sub1_vs, nullptr);
      const H5D_vds_view_t orig_view = virt->view;
      const H5S_select_class_t* orig = sub1_vs->select.type;
      virt->view = H5D_VDS_FIRST_MISSING;
      virt->list[0].clip_size_virtual = HSIZE_UNDEF;
      sub1_vs->select.type = &fail_bounds_cls;
      absl::Cleanup r = [&]() {
        sub1_vs->select.type = orig;
        virt->view = orig_view;
      };
      EXPECT_LT(H5D__virtual_set_extent_unlim(d_pf), 0);
    }
    // 2174 (LAST_AVAILABLE bounds on sub_dset[0].virtual_select).
    {
      H5S_t* sub0_vs = virt->list[0].sub_dset[0].virtual_select;
      ASSERT_NE(sub0_vs, nullptr);
      const H5D_vds_view_t orig_view = virt->view;
      const H5S_select_class_t* orig = sub0_vs->select.type;
      virt->view = H5D_VDS_LAST_AVAILABLE;
      virt->list[0].clip_size_virtual = HSIZE_UNDEF;
      sub0_vs->select.type = &fail_bounds_cls;
      absl::Cleanup r = [&]() {
        sub0_vs->select.type = orig;
        virt->view = orig_view;
      };
      EXPECT_LT(H5D__virtual_set_extent_unlim(d_pf), 0);
    }

    // 2950: pre_io rank failure on initialized printf dataset.
    EXPECT_GE(H5D__virtual_set_extent_unlim(d_pf), 0);
    {
      ScopedSpace sp4(H5Screate_simple(1, d4.data(), nullptr));
      auto* s4_ptr = static_cast<H5S_t*>(H5I_object_verify(sp4, H5I_DATASPACE));
      std::array<int32_t, 4> buf{};
      H5D_io_info_t io_info{};
      std::memset(&io_info, 0, sizeof(io_info));
      H5D_dset_io_info_t dinfo{};
      std::memset(&dinfo, 0, sizeof(dinfo));
      dinfo.dset = d_pf;
      dinfo.mem_space = s4_ptr;
      dinfo.file_space = s4_ptr;
      dinfo.buf.vp = buf.data();
      dinfo.type_info.dst_type = d_pf->shared->type;
      dinfo.type_info.mem_type = d_pf->shared->type;

      const H5S_class_t orig_et = d_pf->shared->space->extent.type;
      d_pf->shared->space->extent.type = H5S_NO_CLASS;
      absl::Cleanup r = [&]() { d_pf->shared->space->extent.type = orig_et; };
      EXPECT_LT(H5D_LOPS_VIRTUAL[0].ser_read(&io_info, &dinfo), 0);
    }

    // 3006: pre_io partial block virtual_select bounds failure.
    {
      ASSERT_GE(
          H5S_set_extent(d_pf->shared->space, std::array<hsize_t, 1>{2}.data()),
          0);
      virt->list[0].sub_dset[0].clipped_virtual_select = nullptr;
      H5S_t* sub0_vs = virt->list[0].sub_dset[0].virtual_select;
      const H5S_select_class_t* orig = sub0_vs->select.type;
      sub0_vs->select.type = &fail_bounds_cls;
      absl::Cleanup r = [&]() { sub0_vs->select.type = orig; };
      ScopedSpace sp2(
          H5Screate_simple(1, std::array<hsize_t, 1>{2}.data(), nullptr));
      std::array<int32_t, 2> buf{};
      EXPECT_LT(
          H5Dread(v_pf, H5T_NATIVE_INT32, sp2, sp2, H5P_DEFAULT, buf.data()),
          0);
    }
  }

  // 3) 52-mapping tree error checks (3381, 3394, 3627, 3640, 3994).
  {
    ScopedDataset v52(H5Dopen2(vf, "v52", H5P_DEFAULT));
    auto* d52_ptr = static_cast<H5D_t*>(H5VL_object_verify(v52, H5I_DATASET));
    ASSERT_NE(d52_ptr, nullptr);

    ScopedSpace sp52(H5Screate_simple(1, d52.data(), nullptr));
    auto* s52_ptr = static_cast<H5S_t*>(H5I_object_verify(sp52, H5I_DATASPACE));
    ASSERT_NE(s52_ptr, nullptr);
    std::array<int32_t, 52> buf{};
    H5D_io_info_t io_info{};
    std::memset(&io_info, 0, sizeof(io_info));
    H5D_dset_io_info_t dinfo{};
    std::memset(&dinfo, 0, sizeof(dinfo));
    dinfo.dset = d52_ptr;
    dinfo.mem_space = s52_ptr;
    dinfo.file_space = s52_ptr;
    dinfo.buf.vp = buf.data();
    dinfo.type_info.dst_type = d52_ptr->shared->type;
    dinfo.type_info.mem_type = d52_ptr->shared->type;

    // 3381 & 3627: shared->space->extent.type = H5S_NO_CLASS before build_tree.
    ASSERT_GE(H5D__virtual_set_extent_unlim(d52_ptr), 0);
    {
      const H5S_class_t orig_et = d52_ptr->shared->space->extent.type;
      d52_ptr->shared->space->extent.type = H5S_NO_CLASS;
      absl::Cleanup r = [&]() {
        d52_ptr->shared->space->extent.type = orig_et;
      };
      EXPECT_LT(H5D_LOPS_VIRTUAL[0].ser_read(&io_info, &dinfo), 0);
      EXPECT_LT(H5D_LOPS_VIRTUAL[0].ser_write(&io_info, &dinfo), 0);
    }

    // 3994: mappings[0].virtual_select->extent.type = H5S_NO_CLASS.
    {
      H5S_t* vs0 = d52_ptr->shared->layout.storage.u.virt.list[0]
                       .source_dset.virtual_select;
      const H5S_class_t orig_et = vs0->extent.type;
      vs0->extent.type = H5S_NO_CLASS;
      absl::Cleanup r = [&]() { vs0->extent.type = orig_et; };
      EXPECT_LT(H5D_LOPS_VIRTUAL[0].ser_read(&io_info, &dinfo), 0);
    }

    // Build tree cleanly, then fail H5S_SELECT_BOUNDS in ser_read (3394) and
    // ser_write (3640).
    ASSERT_GE(H5Dread(v52, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      buf.data()),
              0);
    ASSERT_NE(d52_ptr->shared->layout.storage.u.virt.tree, nullptr);
    const H5S_select_class_t* orig_sel = s52_ptr->select.type;
    s52_ptr->select.type = &fail_bounds_cls;
    absl::Cleanup r = [&]() { s52_ptr->select.type = orig_sel; };
    EXPECT_LT(H5D_LOPS_VIRTUAL[0].ser_read(&io_info, &dinfo), 0);
    EXPECT_LT(H5D_LOPS_VIRTUAL[0].ser_write(&io_info, &dinfo), 0);
  }
}

}  // namespace
}  // namespace hdf5
