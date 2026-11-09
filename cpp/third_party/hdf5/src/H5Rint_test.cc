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
// Tests internal H5R package routines: lifecycle, location ID attachment and
// file reopening, equality, deep copy, token/region/name helpers, modern
// encode/decode, global-heap helpers, and legacy compat encode/decode
// (third_party/hdf5/src/H5Rint.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Rint.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

// `:hdf5` compiles with -DNDEBUG, which omits `#ifndef NDEBUG` fields from
// internal cache/metadata structs such as H5C_cache_entry_t inside H5F_t.
// Define NDEBUG around internal headers so struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5RINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5R_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5HGprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Rpkg.h"
#include "third_party/hdf5/src/H5Sprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5RINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5RINT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// RAII wrapper for pushing/popping an HDF5 API context and restoring tag state.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t dxpl_id = H5P_DEFAULT,
                            haddr_t tag = H5AC__GLOBALHEAP_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      if (dxpl_id != H5P_DEFAULT) {
        H5CX_set_dxpl(dxpl_id);
      }
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

// Move-only RAII guard for H5R_ref_priv_t that calls H5R__destroy() on exit.
class ScopedPrivRef {
 public:
  ScopedPrivRef() = default;
  ~ScopedPrivRef() { reset(); }

  ScopedPrivRef(const ScopedPrivRef&) = delete;
  ScopedPrivRef& operator=(const ScopedPrivRef&) = delete;

  void reset() {
    if (active_) {
      H5R__destroy(&ref_);
      active_ = false;
    }
    std::memset(&ref_, 0, sizeof(ref_));
  }

  H5R_ref_priv_t* get() {
    active_ = true;
    return &ref_;
  }
  const H5R_ref_priv_t* cget() const { return &ref_; }
  void disarm() { active_ = false; }

 private:
  H5R_ref_priv_t ref_{};
  bool active_ = false;
};

H5O_token_t MakeToken(uint8_t seed) {
  H5O_token_t tok{};
  for (size_t i = 0; i < sizeof(haddr_t); ++i) {
    tok.__data[i] = static_cast<uint8_t>(seed + i);
  }
  return tok;
}

TEST(H5RintTest, InitPackageCreateAndDestroyAllTypes) {
  ASSERT_GE(H5open(), 0);

  constexpr hsize_t kDim[1] = {6};
  ScopedSpace space_id(H5Screate_simple(1, kDim, nullptr));
  ASSERT_TRUE(space_id.valid());
  constexpr hsize_t kStart[1] = {1};
  constexpr hsize_t kCount[1] = {3};
  ASSERT_GE(H5Sselect_hyperslab(space_id, H5S_SELECT_SET, kStart, nullptr,
                                kCount, nullptr),
            0);
  H5S_t* space_ptr =
      static_cast<H5S_t*>(H5I_object_verify(space_id, H5I_DATASPACE));
  ASSERT_NE(space_ptr, nullptr);

  const H5O_token_t tok = MakeToken(0x10);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. Object ref.
  ScopedPrivRef obj_ref;
  ASSERT_GE(H5R__create_object(&tok, sizeof(haddr_t), obj_ref.get()), 0);
  EXPECT_EQ(H5R__get_type(obj_ref.cget()), H5R_OBJECT2);
  EXPECT_EQ(H5R__get_loc_id(obj_ref.cget()), H5I_INVALID_HID);
  obj_ref.reset();

  // 2. Region ref.
  ScopedPrivRef reg_ref;
  ASSERT_GE(
      H5R__create_region(&tok, sizeof(haddr_t), space_ptr, reg_ref.get()), 0);
  EXPECT_EQ(H5R__get_type(reg_ref.cget()), H5R_DATASET_REGION2);
  reg_ref.reset();

  // 3. Attr ref (valid and oversized name > H5R_MAX_STRING_LEN).
  ScopedPrivRef attr_ref;
  ASSERT_GE(H5R__create_attr(&tok, sizeof(haddr_t), "scale", attr_ref.get()),
            0);
  EXPECT_EQ(H5R__get_type(attr_ref.cget()), H5R_ATTR);
  attr_ref.reset();

  const std::string long_attr(65537, 'x');
  ScopedErrorSilence silence;
  ScopedPrivRef bad_attr;
  EXPECT_LT(
      H5R__create_attr(&tok, sizeof(haddr_t), long_attr.c_str(), bad_attr.get()),
      0);
  bad_attr.disarm();

  // 3b. Region ref failure cleanup when 64-bit coordinates exceed pre-v1.12
  //     bounds in H5CX (hits H5R__encode_region + H5R__create_region cleanup).
  {
    ScopedPlist old_fapl = MakeCoreFapl();
    ASSERT_TRUE(old_fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(old_fapl, H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
        0);
    ScopedFile old_file(
        H5Fcreate("old_bounds_int.h5", H5F_ACC_TRUNC, H5P_DEFAULT, old_fapl));
    ASSERT_TRUE(old_file.valid());
    H5VL_object_t* vol_old = H5VL_vol_object(old_file.get());
    ASSERT_NE(vol_old, nullptr);
    H5F_t* old_f = static_cast<H5F_t*>(H5VL_object_data(vol_old));
    ASSERT_NE(old_f, nullptr);

    constexpr hsize_t kBigDim[1] = {5000000000ULL};
    ScopedSpace big_sp_id(H5Screate_simple(1, kBigDim, nullptr));
    ASSERT_TRUE(big_sp_id.valid());
    constexpr hsize_t kLargeCoord[1] = {4500000000ULL};
    ASSERT_GE(
        H5Sselect_elements(big_sp_id, H5S_SELECT_SET, 1, kLargeCoord), 0);
    H5S_t* big_sp =
        static_cast<H5S_t*>(H5I_object_verify(big_sp_id, H5I_DATASPACE));
    ASSERT_NE(big_sp, nullptr);

    ScopedApiContext old_ctx;
    ASSERT_TRUE(old_ctx.valid());
    ASSERT_GE(H5CX_set_libver_bounds(old_f), 0);
    ScopedPrivRef bad_reg;
    EXPECT_LT(
        H5R__create_region(&tok, sizeof(haddr_t), big_sp, bad_reg.get()), 0);
    EXPECT_EQ(bad_reg.cget()->info.reg.space, nullptr);
    bad_reg.disarm();
  }

  // 4. Destroy legacy enum variants (no-op payloads) and invalid type values.
  H5R_ref_priv_t compat1{};
  compat1.type = H5R_OBJECT1;
  compat1.loc_id = H5I_INVALID_HID;
  EXPECT_GE(H5R__destroy(&compat1), 0);

  H5R_ref_priv_t compat2{};
  compat2.type = H5R_DATASET_REGION1;
  compat2.loc_id = H5I_INVALID_HID;
  EXPECT_GE(H5R__destroy(&compat2), 0);

  for (const int8_t invalid_type :
       {static_cast<int8_t>(H5R_BADTYPE), static_cast<int8_t>(H5R_MAXTYPE),
        static_cast<int8_t>(99)}) {
    H5R_ref_priv_t bad_ref{};
    bad_ref.type = invalid_type;
    bad_ref.loc_id = H5I_INVALID_HID;
    EXPECT_LT(H5R__destroy(&bad_ref), 0);
  }
}

TEST(H5RintTest, SetGetLocIdAndReopenFile) {
  ScopedFile file1 = CreateInMemoryFile("loc1.h5");
  ScopedFile file2 = CreateInMemoryFile("loc2.h5");
  ASSERT_TRUE(file1.valid());
  ASSERT_TRUE(file2.valid());

  const H5O_token_t tok = MakeToken(0x20);
  ScopedPrivRef ref;
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ASSERT_GE(H5R__create_object(&tok, sizeof(haddr_t), ref.get()), 0);

    // Attach with app_ref=true, overwrite with app_ref=false, then overwrite
    // again with app_ref=true so both decrement branches in H5R__set_loc_id
    // and H5R__destroy run.
    ASSERT_GE(H5R__set_loc_id(ref.get(), file1.get(), true, true), 0);
    EXPECT_EQ(H5R__get_loc_id(ref.cget()), file1.get());
    ASSERT_GE(H5R__set_loc_id(ref.get(), file2.get(), true, false), 0);
    EXPECT_EQ(H5R__get_loc_id(ref.cget()), file2.get());
    ASSERT_GE(H5R__set_loc_id(ref.get(), file1.get(), true, true), 0);
    ref.reset();

    // Also test inc_ref=true failure on bogus hid_t and app_ref=false destroy.
    ASSERT_GE(H5R__create_object(&tok, sizeof(haddr_t), ref.get()), 0);
    ASSERT_GE(H5R__set_loc_id(ref.get(), file1.get(), true, false), 0);
    ref.reset();

    ASSERT_GE(H5R__create_object(&tok, sizeof(haddr_t), ref.get()), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5R__set_loc_id(ref.get(), static_cast<hid_t>(999999), true, true),
          0);
    }
    ref.get()->loc_id = H5I_INVALID_HID;
    ref.reset();
  }

  // Test H5R__reopen_file() with a real OS file via TempFile.
  TempFile disk_file("reopen_target.h5");
  {
    ScopedFile f(H5Fcreate(disk_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                           H5P_DEFAULT));
    ASSERT_TRUE(f.valid());
  }

  // Encode an external reference and decode it so ref.info.obj.filename is
  // heap-allocated and loc_id is H5I_INVALID_HID.
  ScopedPrivRef src_ref;
  std::vector<uint8_t> enc_buf;
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ASSERT_GE(H5R__create_object(&tok, sizeof(haddr_t), src_ref.get()), 0);
    size_t enc_size = 0;
    ASSERT_GE(H5R__encode(disk_file.c_str(), src_ref.cget(), nullptr, &enc_size,
                          H5R_IS_EXTERNAL),
              0);
    enc_buf.resize(enc_size);
    ASSERT_GE(H5R__encode(disk_file.c_str(), src_ref.cget(), enc_buf.data(),
                          &enc_size, H5R_IS_EXTERNAL),
              0);
  }

  ScopedPrivRef ext_ref;
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    size_t dec_size = enc_buf.size();
    ASSERT_GE(H5R__decode(enc_buf.data(), &dec_size, ext_ref.get()), 0);
  }

  // Error cases for H5R__reopen_file before reopening succeeds.
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5R__reopen_file(ext_ref.get(), H5I_INVALID_HID),
                H5I_INVALID_HID);
      EXPECT_EQ(H5R__reopen_file(ext_ref.get(), dcpl.get()), H5I_INVALID_HID);
      ScopedPrivRef no_file_ref;
      ASSERT_GE(H5R__create_object(&tok, sizeof(haddr_t), no_file_ref.get()),
                0);
      EXPECT_EQ(H5R__reopen_file(no_file_ref.get(), H5P_DEFAULT),
                H5I_INVALID_HID);
    }

    const hid_t reopened = H5R__reopen_file(ext_ref.get(), H5P_DEFAULT);
    ASSERT_GE(reopened, 0);
    EXPECT_EQ(H5R__get_loc_id(ext_ref.cget()), reopened);
  }
  // ext_ref owns reopened with app_ref=true (inc_ref=false); H5R__destroy
  // closes it on exit.
  ext_ref.reset();
}

TEST(H5RintTest, TokenRegionNamesEqualAndCopy) {
  ASSERT_GE(H5open(), 0);
  const H5O_token_t tok1 = MakeToken(0x01);
  const H5O_token_t tok2 = MakeToken(0x40);

  constexpr hsize_t kDim1[1] = {5};
  constexpr hsize_t kDim2[1] = {8};
  ScopedSpace sp1_id(H5Screate_simple(1, kDim1, nullptr));
  ScopedSpace sp2_id(H5Screate_simple(1, kDim2, nullptr));
  ASSERT_TRUE(sp1_id.valid());
  ASSERT_TRUE(sp2_id.valid());
  constexpr hsize_t kStart[1] = {1};
  constexpr hsize_t kCount[1] = {2};
  ASSERT_GE(H5Sselect_hyperslab(sp1_id, H5S_SELECT_SET, kStart, nullptr,
                                kCount, nullptr),
            0);
  H5S_t* sp1 = static_cast<H5S_t*>(H5I_object_verify(sp1_id, H5I_DATASPACE));
  H5S_t* sp2 = static_cast<H5S_t*>(H5I_object_verify(sp2_id, H5I_DATASPACE));
  ASSERT_NE(sp1, nullptr);
  ASSERT_NE(sp2, nullptr);

  ScopedSpace dst_sp_id(H5Screate_simple(1, kDim1, nullptr));
  ASSERT_TRUE(dst_sp_id.valid());
  H5S_t* dst_sp =
      static_cast<H5S_t*>(H5I_object_verify(dst_sp_id, H5I_DATASPACE));
  ASSERT_NE(dst_sp, nullptr);

  ScopedFile mem_file = CreateInMemoryFile("copy_loc.h5");
  ASSERT_TRUE(mem_file.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // H5R__set_obj_token / H5R__get_obj_token.
  ScopedPrivRef r1;
  ASSERT_GE(H5R__create_object(&tok1, sizeof(haddr_t), r1.get()), 0);
  H5O_token_t got_tok{};
  size_t got_sz = 0;
  ASSERT_GE(H5R__get_obj_token(r1.cget(), &got_tok, &got_sz), 0);
  EXPECT_EQ(got_sz, sizeof(haddr_t));
  EXPECT_EQ(std::memcmp(&got_tok, &tok1, sizeof(haddr_t)), 0);
  EXPECT_GE(H5R__get_obj_token(r1.cget(), nullptr, &got_sz), 0);

  H5R_ref_priv_t zero_tok_ref{};
  zero_tok_ref.token_size = 0;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5R__get_obj_token(&zero_tok_ref, &got_tok, &got_sz), 0);
  }

  // H5R__get_region.
  ScopedPrivRef reg1;
  ASSERT_GE(H5R__create_region(&tok1, sizeof(haddr_t), sp1, reg1.get()), 0);
  ASSERT_GE(H5R__get_region(reg1.cget(), dst_sp), 0);
  EXPECT_EQ(H5Sget_select_npoints(dst_sp_id), 2);

  // H5R__get_file_name & H5R__get_attr_name.
  {
    ScopedErrorSilence silence;
    std::array<char, 16> buf{};
    EXPECT_LT(H5R__get_file_name(r1.cget(), buf.data(), buf.size()), 0);
  }

  ScopedPrivRef attr1;
  ASSERT_GE(H5R__create_attr(&tok1, sizeof(haddr_t), "units", attr1.get()), 0);
  EXPECT_EQ(H5R__get_attr_name(attr1.cget(), nullptr, 0), 5);
  std::array<char, 8> abuf{};
  EXPECT_EQ(H5R__get_attr_name(attr1.cget(), abuf.data(), 4), 5);
  EXPECT_STREQ(abuf.data(), "uni");
  EXPECT_EQ(H5R__get_attr_name(attr1.cget(), abuf.data(), abuf.size()), 5);
  EXPECT_STREQ(abuf.data(), "units");

  // Encode/decode an external ref to exercise H5R__get_file_name + H5R__equal
  // filename comparisons + H5R__copy external vs local.
  auto make_external_obj = [](const H5O_token_t& t, const char* fname,
                              ScopedPrivRef& out) {
    ScopedPrivRef tmp;
    ASSERT_GE(H5R__create_object(&t, sizeof(haddr_t), tmp.get()), 0);
    size_t sz = 0;
    ASSERT_GE(H5R__encode(fname, tmp.cget(), nullptr, &sz, H5R_IS_EXTERNAL),
              0);
    std::vector<uint8_t> buf(sz);
    ASSERT_GE(
        H5R__encode(fname, tmp.cget(), buf.data(), &sz, H5R_IS_EXTERNAL), 0);
    size_t nbytes = buf.size();
    ASSERT_GE(H5R__decode(buf.data(), &nbytes, out.get()), 0);
  };

  ScopedPrivRef ext_a;
  ScopedPrivRef ext_b;
  make_external_obj(tok1, "alpha.h5", ext_a);
  make_external_obj(tok1, "beta.h5", ext_b);
  EXPECT_EQ(H5R__get_file_name(ext_a.cget(), nullptr, 0), 9);
  std::array<char, 16> fbuf{};
  EXPECT_EQ(H5R__get_file_name(ext_a.cget(), fbuf.data(), 4), 4);
  EXPECT_STREQ(fbuf.data(), "alp");
  EXPECT_EQ(H5R__get_file_name(ext_a.cget(), fbuf.data(), fbuf.size()), 9);
  EXPECT_STREQ(fbuf.data(), "alpha.h5");

  // H5R__equal across mismatches and matches.
  ScopedPrivRef r1_diff_tok;
  ScopedPrivRef r1_diff_sz;
  ScopedPrivRef reg2;
  ScopedPrivRef attr2;
  ASSERT_GE(H5R__create_object(&tok2, sizeof(haddr_t), r1_diff_tok.get()), 0);
  ASSERT_GE(H5R__create_object(&tok1, 4, r1_diff_sz.get()), 0);
  ASSERT_GE(H5R__create_region(&tok1, sizeof(haddr_t), sp2, reg2.get()), 0);
  ASSERT_GE(H5R__create_attr(&tok1, sizeof(haddr_t), "other", attr2.get()), 0);

  EXPECT_GT(H5R__equal(r1.cget(), r1.cget()), 0);
  EXPECT_GT(H5R__equal(reg1.cget(), reg1.cget()), 0);
  EXPECT_GT(H5R__equal(attr1.cget(), attr1.cget()), 0);
  EXPECT_GT(H5R__equal(ext_a.cget(), ext_a.cget()), 0);

  EXPECT_EQ(H5R__equal(r1.cget(), reg1.cget()), 0);
  EXPECT_EQ(H5R__equal(r1.cget(), r1_diff_sz.cget()), 0);
  EXPECT_EQ(H5R__equal(r1.cget(), r1_diff_tok.cget()), 0);
  EXPECT_EQ(H5R__equal(ext_a.cget(), r1.cget()), 0);
  EXPECT_EQ(H5R__equal(r1.cget(), ext_a.cget()), 0);
  EXPECT_EQ(H5R__equal(ext_a.cget(), ext_b.cget()), 0);
  EXPECT_EQ(H5R__equal(reg1.cget(), reg2.cget()), 0);
  EXPECT_EQ(H5R__equal(attr1.cget(), attr2.cget()), 0);

  // H5R__copy for external (filename strdup) and local (loc_id inc_ref) refs.
  ScopedPrivRef copy_ext;
  ASSERT_GE(H5R__copy(ext_a.cget(), copy_ext.get()), 0);
  EXPECT_GT(H5R__equal(ext_a.cget(), copy_ext.cget()), 0);

  ASSERT_GE(H5R__set_loc_id(reg1.get(), mem_file.get(), true, true), 0);
  ASSERT_GE(H5R__set_loc_id(attr1.get(), mem_file.get(), true, true), 0);

  ScopedPrivRef copy_reg;
  ScopedPrivRef copy_attr;
  ASSERT_GE(H5R__copy(reg1.cget(), copy_reg.get()), 0);
  ASSERT_GE(H5R__copy(attr1.cget(), copy_attr.get()), 0);
  EXPECT_GT(H5R__equal(reg1.cget(), copy_reg.cget()), 0);
  EXPECT_GT(H5R__equal(attr1.cget(), copy_attr.cget()), 0);

  // Error cases for H5R__equal and H5R__copy.
  {
    ScopedErrorSilence silence;
    H5R_ref_priv_t bad1{};
    bad1.type = H5R_OBJECT1;
    bad1.token_size = sizeof(haddr_t);
    H5R_ref_priv_t bad2{};
    bad2.type = H5R_OBJECT1;
    bad2.token_size = sizeof(haddr_t);
    EXPECT_LT(H5R__equal(&bad1, &bad2), 0);
    bad1.type = 99;
    bad2.type = 99;
    EXPECT_LT(H5R__equal(&bad1, &bad2), 0);

    H5R_ref_priv_t copy_dst{};
    for (const int8_t bad_t :
         {static_cast<int8_t>(H5R_OBJECT1), static_cast<int8_t>(H5R_BADTYPE),
          static_cast<int8_t>(H5R_MAXTYPE), static_cast<int8_t>(99)}) {
      bad1.type = bad_t;
      EXPECT_LT(H5R__copy(&bad1, &copy_dst), 0);
    }

    H5R_ref_priv_t bad_loc_src{};
    bad_loc_src.type = H5R_OBJECT2;
    bad_loc_src.token_size = sizeof(haddr_t);
    bad_loc_src.loc_id = static_cast<hid_t>(999999);
    EXPECT_LT(H5R__copy(&bad_loc_src, &copy_dst), 0);
  }
}

TEST(H5RintTest, EncodeAndDecodeModernReferencesAndErrorPaths) {
  ASSERT_GE(H5open(), 0);
  const H5O_token_t tok = MakeToken(0x33);

  constexpr hsize_t kDims[2] = {4, 5};
  ScopedSpace sp_id(H5Screate_simple(2, kDims, nullptr));
  ASSERT_TRUE(sp_id.valid());
  constexpr hsize_t kCoords[4] = {0, 1, 3, 4};
  ASSERT_GE(H5Sselect_elements(sp_id, H5S_SELECT_SET, 2, kCoords), 0);
  H5S_t* sp = static_cast<H5S_t*>(H5I_object_verify(sp_id, H5I_DATASPACE));
  ASSERT_NE(sp, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  ScopedPrivRef obj_ref;
  ScopedPrivRef reg_ref;
  ScopedPrivRef attr_ref;
  ASSERT_GE(H5R__create_object(&tok, sizeof(haddr_t), obj_ref.get()), 0);
  ASSERT_GE(H5R__create_region(&tok, sizeof(haddr_t), sp, reg_ref.get()), 0);
  ASSERT_GE(H5R__create_attr(&tok, sizeof(haddr_t), "meta", attr_ref.get()), 0);

  // Round-trip encode/decode across all 3 types, both internal and external,
  // plus partial/size-only encode calls.
  for (const H5R_ref_priv_t* src :
       {obj_ref.cget(), reg_ref.cget(), attr_ref.cget()}) {
    for (const unsigned flags : {0U, static_cast<unsigned>(H5R_IS_EXTERNAL)}) {
      const char* fname = (flags & H5R_IS_EXTERNAL) ? "ext.h5" : nullptr;
      size_t needed = 0;
      ASSERT_GE(H5R__encode(fname, src, nullptr, &needed, flags), 0);

      // Buffer too small (< header size and == header size).
      std::array<uint8_t, 2> tiny{};
      size_t tiny_sz = 1;
      ASSERT_GE(H5R__encode(fname, src, tiny.data(), &tiny_sz, flags), 0);
      EXPECT_EQ(tiny_sz, needed);
      tiny_sz = 2;
      ASSERT_GE(H5R__encode(fname, src, tiny.data(), &tiny_sz, flags), 0);
      EXPECT_EQ(tiny_sz, needed);

      std::vector<uint8_t> buf(needed);
      size_t nalloc = buf.size();
      ASSERT_GE(H5R__encode(fname, src, buf.data(), &nalloc, flags), 0);
      EXPECT_EQ(nalloc, needed);

      ScopedPrivRef decoded;
      size_t nbytes = buf.size();
      ASSERT_GE(H5R__decode(buf.data(), &nbytes, decoded.get()), 0);
      EXPECT_EQ(nbytes, needed);
      EXPECT_EQ(H5R__get_type(decoded.cget()), H5R__get_type(src));
      EXPECT_EQ(H5R__get_loc_id(decoded.cget()), H5I_INVALID_HID);
    }
  }

  // Encode error cases.
  {
    ScopedErrorSilence silence;
    size_t sz = 0;
    const std::string long_fname(65537, 'f');
    EXPECT_LT(H5R__encode(long_fname.c_str(), obj_ref.cget(), nullptr, &sz,
                          H5R_IS_EXTERNAL),
              0);

    for (const int8_t bad_t :
         {static_cast<int8_t>(H5R_OBJECT1), static_cast<int8_t>(H5R_MAXTYPE),
          static_cast<int8_t>(99)}) {
      H5R_ref_priv_t bad{};
      bad.type = bad_t;
      bad.token_size = sizeof(haddr_t);
      EXPECT_LT(H5R__encode(nullptr, &bad, nullptr, &sz, 0), 0);
    }
  }

  // Decode error cases (buffers remain sized cleanly for ASan).
  {
    ScopedErrorSilence silence;
    std::array<uint8_t, 64> buf{};
    H5R_ref_priv_t out{};

    // 1. Header too small.
    size_t n = 1;
    EXPECT_LT(H5R__decode(buf.data(), &n, &out), 0);

    // 2. Invalid reference type in header.
    buf[0] = static_cast<uint8_t>(H5R_BADTYPE);
    n = 16;
    EXPECT_LT(H5R__decode(buf.data(), &n, &out), 0);
    buf[0] = static_cast<uint8_t>(H5R_MAXTYPE);
    n = 16;
    EXPECT_LT(H5R__decode(buf.data(), &n, &out), 0);

    // 3. Truncated at token_size byte.
    buf[0] = static_cast<uint8_t>(H5R_OBJECT2);
    buf[1] = 0;
    n = 2;
    EXPECT_LT(H5R__decode(buf.data(), &n, &out), 0);

    // 4. Token size > sizeof(H5O_token_t).
    buf[2] = static_cast<uint8_t>(sizeof(H5O_token_t) + 1);
    n = 32;
    EXPECT_LT(H5R__decode(buf.data(), &n, &out), 0);

    // 5. External filename truncated before uint16 length header.
    buf[0] = static_cast<uint8_t>(H5R_OBJECT2);
    buf[1] = H5R_IS_EXTERNAL;
    buf[2] = 0;  // 0-byte token
    n = 4;       // leaves 1 byte (< sizeof(uint16_t)) for filename
    EXPECT_LT(H5R__decode(buf.data(), &n, &out), 0);

    // 6. Legacy H5R_OBJECT1 / unknown type after decoding external filename
    //    (hits decoded_filename cleanup in H5R__decode).
    std::memset(buf.data(), 0, buf.size());
    buf[0] = static_cast<uint8_t>(H5R_OBJECT1);
    buf[1] = H5R_IS_EXTERNAL;
    buf[2] = 0;
    buf[3] = 2;
    buf[4] = 0;  // length 2
    buf[5] = 'a';
    buf[6] = 'b';
    n = 7;
    EXPECT_LT(H5R__decode(buf.data(), &n, &out), 0);

    // 7. Attribute name truncated before uint16 length header.
    std::memset(buf.data(), 0, buf.size());
    buf[0] = static_cast<uint8_t>(H5R_ATTR);
    buf[1] = 0;
    buf[2] = 0;
    n = 4;  // leaves 1 byte (< sizeof(uint16_t))
    EXPECT_LT(H5R__decode(buf.data(), &n, &out), 0);

    // 8. Region decode: buffer < 8 bytes, buffer < declared selection size,
    //    invalid rank (> H5S_MAX_RANK), and corrupt selection payload with
    //    external filename cleanup.
    std::memset(buf.data(), 0, buf.size());
    buf[0] = static_cast<uint8_t>(H5R_DATASET_REGION2);
    buf[1] = 0;
    buf[2] = 0;
    n = 6;  // leaves 3 bytes (< 8)
    EXPECT_LT(H5R__decode(buf.data(), &n, &out), 0);

    // Declared selection size = 100 > remaining bytes.
    buf[3] = 100;
    n = 16;
    EXPECT_LT(H5R__decode(buf.data(), &n, &out), 0);

    // Declared selection size = 4, rank = 64 (> H5S_MAX_RANK).
    std::memset(buf.data(), 0, buf.size());
    buf[0] = static_cast<uint8_t>(H5R_DATASET_REGION2);
    buf[1] = 0;
    buf[2] = 0;
    buf[3] = 4;   // buf_size = 4 (+8 = 12)
    buf[7] = 64;  // rank = 64
    n = 20;
    EXPECT_LT(H5R__decode(buf.data(), &n, &out), 0);

    // Valid rank=1, external filename present, corrupt selection payload.
    std::memset(buf.data(), 0, buf.size());
    buf[0] = static_cast<uint8_t>(H5R_DATASET_REGION2);
    buf[1] = H5R_IS_EXTERNAL;
    buf[2] = 0;  // 0-byte token
    buf[3] = 1;
    buf[4] = 0;  // filename len = 1
    buf[5] = 'x';
    buf[6] = 8;   // buf_size = 8
    buf[10] = 1;  // rank = 1
    buf[14] = 0xff;  // invalid H5S selection type
    n = 24;
    EXPECT_LT(H5R__decode(buf.data(), &n, &out), 0);
  }
}

TEST(H5RintTest, GlobalHeapAndCompatTokenEncodeDecode) {
  ScopedFile file = CreateInMemoryFile("compat_int.h5");
  ASSERT_TRUE(file.valid());

  constexpr hsize_t kDim[1] = {5};
  ScopedSpace space_id(H5Screate_simple(1, kDim, nullptr));
  ASSERT_TRUE(space_id.valid());
  ScopedDataset dset(H5Dcreate2(file, "/ds", H5T_NATIVE_INT32, space_id,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  // Obtain real dataset token and legacy region reference via public API.
  H5O_info2_t oinfo{};
  ASSERT_GE(H5Oget_info3(dset, &oinfo, H5O_INFO_BASIC), 0);

  constexpr hsize_t kStart[1] = {1};
  constexpr hsize_t kCount[1] = {3};
  ASSERT_GE(H5Sselect_hyperslab(space_id, H5S_SELECT_SET, kStart, nullptr,
                                kCount, nullptr),
            0);
  hdset_reg_ref_t reg_ref{};
  ASSERT_GE(H5Rcreate(&reg_ref, file, "/ds", H5R_DATASET_REGION, space_id), 0);

  H5VL_object_t* vol_file = H5VL_vol_object(file.get());
  ASSERT_NE(vol_file, nullptr);
  H5F_t* f = static_cast<H5F_t*>(H5VL_object_data(vol_file));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // 1. H5R__encode_token_obj_compat / H5R__decode_token_obj_compat.
  size_t obj_sz = 0;
  ASSERT_GE(H5R__encode_token_obj_compat(&oinfo.token, sizeof(haddr_t), nullptr,
                                         &obj_sz),
            0);
  EXPECT_EQ(obj_sz, sizeof(haddr_t));
  std::vector<uint8_t> obj_buf(obj_sz);
  size_t too_small_obj = obj_sz - 1;
  ASSERT_GE(H5R__encode_token_obj_compat(&oinfo.token, sizeof(haddr_t),
                                         obj_buf.data(), &too_small_obj),
            0);
  ASSERT_GE(H5R__encode_token_obj_compat(&oinfo.token, sizeof(haddr_t),
                                         obj_buf.data(), &obj_sz),
            0);

  H5O_token_t dec_tok{};
  size_t dec_obj_sz = obj_buf.size();
  ASSERT_GE(H5R__decode_token_obj_compat(obj_buf.data(), &dec_obj_sz, &dec_tok,
                                         sizeof(haddr_t)),
            0);
  EXPECT_EQ(std::memcmp(&dec_tok, &oinfo.token, sizeof(haddr_t)), 0);

  {
    ScopedErrorSilence silence;
    size_t short_sz = sizeof(haddr_t) - 1;
    EXPECT_LT(H5R__decode_token_obj_compat(obj_buf.data(), &short_sz, &dec_tok,
                                           sizeof(haddr_t)),
              0);
  }

  // 2. H5R__encode_heap / H5R__decode_heap.
  size_t heap_id_sz = 0;
  ASSERT_GE(H5R__encode_heap(f, nullptr, &heap_id_sz, nullptr, 0), 0);
  std::vector<uint8_t> heap_id(heap_id_sz);
  const std::array<uint8_t, 5> payload = {11, 22, 33, 44, 55};
  size_t small_alloc = heap_id_sz - 1;
  ASSERT_GE(H5R__encode_heap(f, heap_id.data(), &small_alloc, payload.data(),
                             payload.size()),
            0);
  ASSERT_GE(H5R__encode_heap(f, heap_id.data(), &heap_id_sz, payload.data(),
                             payload.size()),
            0);

  unsigned char* out_data = nullptr;
  absl::Cleanup free_out_data = [&out_data] { H5MM_xfree(out_data); };
  size_t out_sz = 0;
  size_t dec_heap_sz = heap_id.size();
  ASSERT_GE(H5R__decode_heap(f, heap_id.data(), &dec_heap_sz, &out_data,
                             &out_sz),
            0);
  EXPECT_EQ(out_sz, payload.size());
  EXPECT_EQ(std::memcmp(out_data, payload.data(), payload.size()), 0);

  // Also test caller-provided buffer passed into H5R__decode_heap.
  std::vector<unsigned char> caller_buf(payload.size());
  unsigned char* caller_ptr = caller_buf.data();
  dec_heap_sz = heap_id.size();
  ASSERT_GE(H5R__decode_heap(f, heap_id.data(), &dec_heap_sz, &caller_ptr,
                             &out_sz),
            0);
  EXPECT_THAT(caller_buf, ElementsAre(11, 22, 33, 44, 55));

  {
    ScopedErrorSilence silence;
    size_t short_heap_sz = heap_id.size() - 1;
    unsigned char* err_ptr = nullptr;
    EXPECT_LT(
        H5R__decode_heap(f, heap_id.data(), &short_heap_sz, &err_ptr, &out_sz),
        0);

    std::vector<uint8_t> zero_heap(heap_id.size(), 0);
    dec_heap_sz = zero_heap.size();
    EXPECT_LT(H5R__decode_heap(f, zero_heap.data(), &dec_heap_sz, &err_ptr,
                               &out_sz),
              0);

    std::vector<uint8_t> undef_heap(heap_id.size(), 0xff);
    dec_heap_sz = undef_heap.size();
    EXPECT_LT(H5R__decode_heap(f, undef_heap.data(), &dec_heap_sz, &err_ptr,
                               &out_sz),
              0);

    std::vector<uint8_t> bad_idx_heap = heap_id;
    bad_idx_heap.back() ^= 0x55;
    dec_heap_sz = bad_idx_heap.size();
    EXPECT_LT(H5R__decode_heap(f, bad_idx_heap.data(), &dec_heap_sz, &err_ptr,
                               &out_sz),
              0);
  }

  // 3. H5R__decode_token_region_compat: valid with and without space_ptr.
  H5O_token_t reg_tok{};
  H5S_t* reg_space = nullptr;
  absl::Cleanup close_reg_space = [&reg_space] {
    if (reg_space != nullptr) {
      H5S_close(reg_space);
    }
  };
  size_t reg_nbytes = H5R_DSET_REG_REF_BUF_SIZE;
  ASSERT_GE(H5R__decode_token_region_compat(
                f, reg_ref.__data, &reg_nbytes, &reg_tok, sizeof(haddr_t),
                &reg_space),
            0);
  ASSERT_NE(reg_space, nullptr);
  EXPECT_EQ(std::memcmp(&reg_tok, &oinfo.token, sizeof(haddr_t)), 0);

  reg_nbytes = H5R_DSET_REG_REF_BUF_SIZE;
  ASSERT_GE(H5R__decode_token_region_compat(f, reg_ref.__data, &reg_nbytes,
                                            &reg_tok, sizeof(haddr_t), nullptr),
            0);

  // 4. H5R__decode_token_region_compat error paths:
  //    a) invalid heap ID
  //    b) heap item with bogus object token (H5S_read fails)
  //    c) heap item truncated at token_size only (p - 1 >= p_end)
  //    d) heap item with valid token + corrupt selection bytes
  {
    ScopedErrorSilence silence;
    H5S_t* err_space = nullptr;
    std::array<uint8_t, H5R_DSET_REG_REF_BUF_SIZE> zero_reg{};
    reg_nbytes = zero_reg.size();
    EXPECT_LT(H5R__decode_token_region_compat(f, zero_reg.data(), &reg_nbytes,
                                              &reg_tok, sizeof(haddr_t),
                                              &err_space),
              0);

    // (b) Bogus token in heap.
    std::vector<uint8_t> bogus_tok_item(sizeof(haddr_t) + 8, 0x7f);
    std::vector<uint8_t> bogus_heap_id(heap_id_sz);
    size_t sz = bogus_heap_id.size();
    ASSERT_GE(H5R__encode_heap(f, bogus_heap_id.data(), &sz,
                               bogus_tok_item.data(), bogus_tok_item.size()),
              0);
    reg_nbytes = bogus_heap_id.size();
    EXPECT_LT(H5R__decode_token_region_compat(f, bogus_heap_id.data(),
                                              &reg_nbytes, &reg_tok,
                                              sizeof(haddr_t), &err_space),
              0);

    // (c) Valid token with zero selection bytes.
    std::vector<uint8_t> trunc_item(sizeof(haddr_t));
    std::memcpy(trunc_item.data(), &oinfo.token, sizeof(haddr_t));
    std::vector<uint8_t> trunc_heap_id(heap_id_sz);
    sz = trunc_heap_id.size();
    ASSERT_GE(H5R__encode_heap(f, trunc_heap_id.data(), &sz, trunc_item.data(),
                               trunc_item.size()),
              0);
    reg_nbytes = trunc_heap_id.size();
    EXPECT_LT(H5R__decode_token_region_compat(f, trunc_heap_id.data(),
                                              &reg_nbytes, &reg_tok,
                                              sizeof(haddr_t), &err_space),
              0);

    // (d) Valid token + corrupt selection bytes.
    std::vector<uint8_t> bad_sel_item(sizeof(haddr_t) + 8, 0xff);
    std::memcpy(bad_sel_item.data(), &oinfo.token, sizeof(haddr_t));
    std::vector<uint8_t> bad_sel_heap_id(heap_id_sz);
    sz = bad_sel_heap_id.size();
    ASSERT_GE(H5R__encode_heap(f, bad_sel_heap_id.data(), &sz,
                               bad_sel_item.data(), bad_sel_item.size()),
              0);
    reg_nbytes = bad_sel_heap_id.size();
    EXPECT_LT(H5R__decode_token_region_compat(f, bad_sel_heap_id.data(),
                                              &reg_nbytes, &reg_tok,
                                              sizeof(haddr_t), &err_space),
              0);
  }

  // 5. H5R__encode_heap fails cleanly when writing to a read-only file.
  TempFile ro_tmp("ro_heap_int.h5");
  {
    ScopedFile wf(
        H5Fcreate(ro_tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(wf.valid());
  }
  {
    ScopedFile rf(H5Fopen(ro_tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(rf.valid());
    H5VL_object_t* ro_vol = H5VL_vol_object(rf.get());
    ASSERT_NE(ro_vol, nullptr);
    H5F_t* ro_f = static_cast<H5F_t*>(H5VL_object_data(ro_vol));
    ASSERT_NE(ro_f, nullptr);
    ScopedApiContext ro_ctx;
    ASSERT_TRUE(ro_ctx.valid());
    std::vector<uint8_t> ro_heap_id(heap_id_sz);
    size_t ro_sz = ro_heap_id.size();
    ScopedErrorSilence silence;
    EXPECT_LT(H5R__encode_heap(ro_f, ro_heap_id.data(), &ro_sz, payload.data(),
                               payload.size()),
              0);
  }
}

}  // namespace
}  // namespace hdf5
