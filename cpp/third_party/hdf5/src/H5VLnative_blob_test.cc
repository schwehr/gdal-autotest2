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
// Tests native VOL connector blob callbacks (`H5VL__native_blob_put`,
// `H5VL__native_blob_get`, `H5VL__native_blob_specific`) and public
// variable-length / reference callers (third_party/hdf5/src/H5VLnative_blob.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLnative_blob.c

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5VLNATIVE_BLOB_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5VLnative_private.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5VLNATIVE_BLOB_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5VLNATIVE_BLOB_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  ScopedApiContext() { pushed_ = (H5CX_push(&node_) >= 0); }
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

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object(file_id));
}

TEST(H5VLNativeBlobTest, PublicVlenSequenceAndObjectReferences) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedSpace scalar_space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar_space.valid());

  // Variable-length sequence triggers blob put/get through the VOL layer.
  ScopedType vlen_type(H5Tvlen_create(H5T_NATIVE_INT));
  ASSERT_TRUE(vlen_type.valid());
  ScopedDataset vlen_dset(H5Dcreate2(file.get(), "vlen_ds", vlen_type.get(),
                                     scalar_space.get(), H5P_DEFAULT,
                                     H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(vlen_dset.valid());

  std::array<int32_t, 4> payload = {10, 20, 30, 40};
  hvl_t wval{};
  wval.len = payload.size();
  wval.p = payload.data();
  ASSERT_GE(H5Dwrite(vlen_dset.get(), vlen_type.get(), H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, &wval),
            0);

  hvl_t rval{};
  ASSERT_GE(H5Dread(vlen_dset.get(), vlen_type.get(), H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, &rval),
            0);
  absl::Cleanup reclaim_vlen([&] {
    H5Treclaim(vlen_type.get(), scalar_space.get(), H5P_DEFAULT, &rval);
  });
  ASSERT_EQ(rval.len, payload.size());
  const auto* read_ints = static_cast<const int32_t*>(rval.p);
  for (size_t i = 0; i < payload.size(); ++i) {
    EXPECT_EQ(read_ints[i], payload[i]);
  }

  // Standard region references also store blobs in global heap on write.
  const std::array<hsize_t, 1> dims = {4};
  ScopedSpace arr_space(H5Screate_simple(1, dims.data(), nullptr));
  ASSERT_TRUE(arr_space.valid());
  const std::array<hsize_t, 1> start = {1};
  const std::array<hsize_t, 1> count = {2};
  ASSERT_GE(H5Sselect_hyperslab(arr_space.get(), H5S_SELECT_SET, start.data(),
                                nullptr, count.data(), nullptr),
            0);

  H5R_ref_t wref{};
  ASSERT_GE(
      H5Rcreate_region(file.get(), "vlen_ds", arr_space.get(), H5P_DEFAULT, &wref),
      0);
  absl::Cleanup destroy_wref([&] { H5Rdestroy(&wref); });

  ScopedDataset ref_dset(H5Dcreate2(file.get(), "ref_ds", H5T_STD_REF,
                                    scalar_space.get(), H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ref_dset.valid());
  ASSERT_GE(H5Dwrite(ref_dset.get(), H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     &wref),
            0);

  H5R_ref_t rref{};
  ASSERT_GE(
      H5Dread(ref_dset.get(), H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT, &rref),
      0);
  absl::Cleanup destroy_rref([&] { H5Rdestroy(&rref); });
  EXPECT_EQ(H5Rget_type(&rref), H5R_DATASET_REGION2);
}

TEST(H5VLNativeBlobTest, DirectPutGetSpecificAndErrorBranches) {
  TempFile tmp_file("blob_direct.h5");
  {
    ScopedFile f(H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                           H5P_DEFAULT));
    ASSERT_TRUE(f.valid());
    H5F_t* hf = UnwrapFile(f.get());
    ASSERT_NE(hf, nullptr);

    ScopedApiContext api_ctx;

    // 1. Put and get a non-empty blob.
    std::array<uint8_t, H5VL_MAX_BLOB_ID_SIZE> blob_id{};
    const std::array<uint8_t, 6> data = {1, 2, 3, 4, 5, 6};
    ASSERT_GE(H5VL__native_blob_put(hf, data.data(), data.size(),
                                    blob_id.data(), nullptr),
              0);

    H5VL_blob_specific_args_t spec_args{};
    bool is_null = true;
    spec_args.op_type = H5VL_BLOB_ISNULL;
    spec_args.args.is_null.isnull = &is_null;
    ASSERT_GE(H5VL__native_blob_specific(hf, blob_id.data(), &spec_args), 0);
    EXPECT_FALSE(is_null);

    std::array<uint8_t, 6> read_back{};
    ASSERT_GE(H5VL__native_blob_get(hf, blob_id.data(), read_back.data(),
                                    read_back.size(), nullptr),
              0);
    EXPECT_EQ(read_back, data);

    // 2. Get with mismatched expected size triggers H5E_BADSIZE.
    {
      ScopedErrorSilence silence;
      std::array<uint8_t, 4> wrong_size_buf{};
      EXPECT_LT(H5VL__native_blob_get(hf, blob_id.data(), wrong_size_buf.data(),
                                      wrong_size_buf.size(), nullptr),
                0);
    }

    // 3. Delete valid blob via H5VL_BLOB_DELETE.
    spec_args = {};
    spec_args.op_type = H5VL_BLOB_DELETE;
    ASSERT_GE(H5VL__native_blob_specific(hf, blob_id.data(), &spec_args), 0);

    // 4. Zero-size put and nil blob checks.
    std::array<uint8_t, H5VL_MAX_BLOB_ID_SIZE> zero_blob_id{};
    ASSERT_GE(
        H5VL__native_blob_put(hf, nullptr, 0, zero_blob_id.data(), nullptr),
        0);

    std::array<uint8_t, H5VL_MAX_BLOB_ID_SIZE> nil_blob_id{};
    std::memset(nil_blob_id.data(), 0xff, nil_blob_id.size());
    spec_args = {};
    spec_args.op_type = H5VL_BLOB_SETNULL;
    ASSERT_GE(H5VL__native_blob_specific(hf, nil_blob_id.data(), &spec_args),
              0);

    is_null = false;
    spec_args = {};
    spec_args.op_type = H5VL_BLOB_ISNULL;
    spec_args.args.is_null.isnull = &is_null;
    ASSERT_GE(H5VL__native_blob_specific(hf, nil_blob_id.data(), &spec_args),
              0);
    EXPECT_TRUE(is_null);

    // Get and Delete on nil blob (addr == 0) are safe no-ops.
    std::array<uint8_t, 4> unused_buf{};
    EXPECT_GE(H5VL__native_blob_get(hf, nil_blob_id.data(), unused_buf.data(),
                                    unused_buf.size(), nullptr),
              0);
    spec_args = {};
    spec_args.op_type = H5VL_BLOB_DELETE;
    EXPECT_GE(H5VL__native_blob_specific(hf, nil_blob_id.data(), &spec_args),
              0);

    // 5. Bogus non-zero blob ID fails H5HG_get_obj_size and H5HG_remove.
    std::array<uint8_t, H5VL_MAX_BLOB_ID_SIZE> bogus_blob_id{};
    std::memset(bogus_blob_id.data(), 0x5a, bogus_blob_id.size());
    {
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5VL__native_blob_get(hf, bogus_blob_id.data(), unused_buf.data(),
                                unused_buf.size(), nullptr),
          0);
      spec_args = {};
      spec_args.op_type = H5VL_BLOB_DELETE;
      EXPECT_LT(
          H5VL__native_blob_specific(hf, bogus_blob_id.data(), &spec_args), 0);

      // 6. Unknown specific op_type.
      spec_args = {};
      spec_args.op_type = static_cast<H5VL_blob_specific_t>(999);
      EXPECT_LT(H5VL__native_blob_specific(hf, nil_blob_id.data(), &spec_args),
                0);
    }
  }

  // Reopen read-only to test H5HG_insert failure path in H5VL__native_blob_put.
  {
    ScopedFile ro_f(H5Fopen(tmp_file.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(ro_f.valid());
    H5F_t* hf = UnwrapFile(ro_f.get());
    ASSERT_NE(hf, nullptr);

    ScopedApiContext api_ctx;
    ScopedErrorSilence silence;
    std::array<uint8_t, H5VL_MAX_BLOB_ID_SIZE> ro_blob_id{};
    const std::array<uint8_t, 4> payload = {9, 8, 7, 6};
    EXPECT_LT(H5VL__native_blob_put(hf, payload.data(), payload.size(),
                                    ro_blob_id.data(), nullptr),
              0);
  }
}

}  // namespace
}  // namespace hdf5
