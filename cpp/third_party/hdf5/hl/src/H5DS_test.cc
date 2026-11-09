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
// Tests HDF5 High-Level Dimension Scale APIs (third_party/hdf5/hl/src/H5DS.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/src/H5DS.c
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/test/test_ds.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/base/config.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5_test_util.h"
#include "third_party/hdf5/hl/src/H5DSpublic.h"
#include "third_party/hdf5/hl/src/H5LTpublic.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::StrEq;

// Helpers to create datasets concisely inside in-memory files.
hid_t Create2dIntDataset(hid_t loc_id, const char* name, hsize_t rows,
                         hsize_t cols) {
  const std::array<hsize_t, 2> dims = {rows, cols};
  ScopedDataspace space(H5Screate_simple(2, dims.data(), nullptr));
  if (!space.valid()) {
    return H5I_INVALID_HID;
  }
  return H5Dcreate2(loc_id, name, H5T_NATIVE_INT32, space, H5P_DEFAULT,
                    H5P_DEFAULT, H5P_DEFAULT);
}

hid_t Create1dDoubleDataset(hid_t loc_id, const char* name, hsize_t len) {
  ScopedDataspace space(H5Screate_simple(1, &len, nullptr));
  if (!space.valid()) {
    return H5I_INVALID_HID;
  }
  return H5Dcreate2(loc_id, name, H5T_NATIVE_DOUBLE, space, H5P_DEFAULT,
                    H5P_DEFAULT, H5P_DEFAULT);
}

struct IterateLog {
  std::vector<std::string> names;
  std::vector<unsigned> dims;
  int stop_after = -1;
  herr_t stop_code = 0;
};

herr_t RecordScaleVisitor(hid_t /*dset*/, unsigned dim, hid_t scale,
                          void* visitor_data) {
  auto* log = static_cast<IterateLog*>(visitor_data);
  std::array<char, 64> buf = {};
  const ssize_t len = H5DSget_scale_name(scale, buf.data(), buf.size());
  if (len >= 0) {
    log->names.emplace_back(buf.data());
  } else {
    log->names.emplace_back("<unnamed>");
  }
  log->dims.push_back(dim);
  if (log->stop_after >= 0 &&
      static_cast<int>(log->names.size()) >= log->stop_after) {
    return log->stop_code;
  }
  return 0;
}

TEST(H5dsTest, WithNewRefReportsFalseOnNativeCoreFile) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  bool with_new_ref = true;
  ASSERT_GE(H5DSwith_new_ref(file, &with_new_ref), 0);
  EXPECT_FALSE(with_new_ref);

  ScopedDataset dset(Create1dDoubleDataset(file, "dset", 4));
  ASSERT_GE(dset.get(), 0);
  with_new_ref = true;
  ASSERT_GE(H5DSwith_new_ref(dset, &with_new_ref), 0);
  EXPECT_FALSE(with_new_ref);

  ScopedErrorSilence silence;
  EXPECT_LT(H5DSwith_new_ref(file, nullptr), 0);
  EXPECT_LT(H5DSwith_new_ref(H5I_INVALID_HID, &with_new_ref), 0);
}

TEST(H5dsTest, SetScaleIsScaleAndGetScaleName) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);
  // Upstream H5DSis_scale() leaks `aid` and `atid` on non-conforming CLASS
  // attributes because its `out:` handler only closes them when `is_ds < 0`,
  // keeping the Core file open unless swept before `file` closes.
  const absl::Cleanup sweep_leaked = [] {
    SweepOpenH5Ids(std::array{H5I_ATTR, H5I_DATATYPE});
  };

  // 1. Mark a named dimension scale and verify predicate + name queries.
  ScopedDataset named_scale(Create1dDoubleDataset(file, "named_scale", 5));
  ASSERT_GE(named_scale.get(), 0);
  EXPECT_EQ(0, H5DSis_scale(named_scale));

  ASSERT_GE(H5DSset_scale(named_scale, "time_seconds"), 0);
  EXPECT_EQ(1, H5DSis_scale(named_scale));

  // Query scale name size with nullptr buffer, full copy, and truncation.
  const ssize_t full_len = H5DSget_scale_name(named_scale, nullptr, 0);
  ASSERT_EQ(12, full_len);

  std::array<char, 32> name_buf = {};
  name_buf.fill('X');
  EXPECT_EQ(12,
            H5DSget_scale_name(named_scale, name_buf.data(), name_buf.size()));
  EXPECT_THAT(name_buf.data(), StrEq("time_seconds"));

  std::array<char, 5> trunc_buf = {};
  trunc_buf.fill('X');
  EXPECT_EQ(
      12, H5DSget_scale_name(named_scale, trunc_buf.data(), trunc_buf.size()));
  EXPECT_THAT(trunc_buf.data(), StrEq("time"));

  // 2. Unnamed dimension scale returns 0 from H5DSget_scale_name.
  ScopedDataset unnamed_scale(Create1dDoubleDataset(file, "unnamed_scale", 3));
  ASSERT_GE(unnamed_scale.get(), 0);
  ASSERT_GE(H5DSset_scale(unnamed_scale, nullptr), 0);
  EXPECT_EQ(1, H5DSis_scale(unnamed_scale));
  EXPECT_EQ(
      0, H5DSget_scale_name(unnamed_scale, name_buf.data(), name_buf.size()));

  // 3. A dataset that already has DIMENSION_LIST cannot be turned into a
  //    scale.
  ScopedDataset data(Create2dIntDataset(file, "data", 3, 5));
  ASSERT_GE(data.get(), 0);
  ASSERT_GE(H5DSattach_scale(data, named_scale, 1), 0);
  EXPECT_LT(H5DSset_scale(data, "invalid"), 0);

  // 4. CLASS validation branches inside H5DSis_scale.
  ScopedDataset nonstr_class(Create1dDoubleDataset(file, "nonstr_class", 2));
  ASSERT_GE(nonstr_class.get(), 0);
  constexpr int32_t kIntClass = 1;
  ASSERT_GE(H5LTset_attribute_int(file, "nonstr_class", "CLASS", &kIntClass, 1),
            0);
  EXPECT_EQ(0, H5DSis_scale(nonstr_class));

  ScopedDataset other_str(Create1dDoubleDataset(file, "other_str", 2));
  ASSERT_GE(other_str.get(), 0);
  ASSERT_GE(H5LTset_attribute_string(file, "other_str", "CLASS", "OTHER"), 0);
  EXPECT_EQ(0, H5DSis_scale(other_str));

  ScopedDataset nullpad_class(Create1dDoubleDataset(file, "nullpad_class", 2));
  ASSERT_GE(nullpad_class.get(), 0);
  ASSERT_GE(
      WriteNullpadScalarStringAttr(nullpad_class, "CLASS", "DIMENSION_SCALE"),
      0);
  EXPECT_EQ(0, H5DSis_scale(nullpad_class));

  // 5. Invalid object / non-scale error checks.
  ScopedErrorSilence silence;
  EXPECT_LT(H5DSis_scale(file), 0);
  EXPECT_LT(H5DSis_scale(H5I_INVALID_HID), 0);
  EXPECT_LT(H5DSset_scale(file, "bad"), 0);
  EXPECT_LT(H5DSset_scale(H5I_INVALID_HID, "bad"), 0);
  EXPECT_LT(H5DSget_scale_name(file, name_buf.data(), name_buf.size()), 0);
  EXPECT_LT(H5DSget_scale_name(data, name_buf.data(), name_buf.size()), 0);
  EXPECT_LT(
      H5DSget_scale_name(H5I_INVALID_HID, name_buf.data(), name_buf.size()), 0);
}

TEST(H5dsTest, AttachDetachIsAttachedAndScalarDatasetLegacyRefs) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  // 1. Create datasets and scales (keep s2 unmarked initially).
  ScopedDataset d1(Create2dIntDataset(file, "d1", 3, 4));
  ScopedDataset d2(Create2dIntDataset(file, "d2", 3, 4));
  ScopedDataset s1(Create1dDoubleDataset(file, "s1", 3));
  ScopedDataset s2(Create1dDoubleDataset(file, "s2", 3));
  ASSERT_GE(d1.get(), 0);
  ASSERT_GE(d2.get(), 0);
  ASSERT_GE(s1.get(), 0);
  ASSERT_GE(s2.get(), 0);

  ASSERT_GE(H5DSset_scale(s1, "rows_a"), 0);
  EXPECT_EQ(0, H5DSget_num_scales(d1, 0));
  EXPECT_EQ(0, H5DSis_attached(d1, s1, 0));

  // 2. Attach s1 to d1 dim 0 (initial), then unmarked s2 to d1 dim 0
  //    (extends existing VL entry and marks CLASS on s2 automatically).
  ASSERT_GE(H5DSattach_scale(d1, s1, 0), 0);
  EXPECT_EQ(1, H5DSis_attached(d1, s1, 0));
  EXPECT_EQ(0, H5DSis_attached(d1, s1, 1));
  EXPECT_EQ(1, H5DSget_num_scales(d1, 0));

  ASSERT_GE(H5DSattach_scale(d1, s2, 0), 0);
  EXPECT_EQ(1, H5DSis_scale(s2));
  EXPECT_EQ(2, H5DSget_num_scales(d1, 0));
  EXPECT_EQ(1, H5DSis_attached(d1, s2, 0));

  // 3. Attach s1 to d1 dim 1 (DIMENSION_LIST already exists, dim 1 was empty;
  //    s1 REFERENCE_LIST already exists so REFERENCE_LIST extends).
  ASSERT_GE(H5DSattach_scale(d1, s1, 1), 0);
  EXPECT_EQ(1, H5DSget_num_scales(d1, 1));
  EXPECT_EQ(1, H5DSis_attached(d1, s1, 1));

  // 4. Attach s1 to d2 dim 1 and to a scalar dataset (rank == 0 -> treated as
  //    1).
  ASSERT_GE(H5DSattach_scale(d2, s1, 1), 0);
  EXPECT_EQ(1, H5DSis_attached(d2, s1, 1));

  ScopedDataspace scalar_space(H5Screate(H5S_SCALAR));
  ASSERT_GE(scalar_space.get(), 0);
  ScopedDataset scalar_dset(H5Dcreate2(file, "scalar_dset", H5T_NATIVE_INT32,
                                       scalar_space, H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT));
  ScopedDataset s_scalar(Create1dDoubleDataset(file, "s_scalar", 2));
  ASSERT_GE(scalar_dset.get(), 0);
  ASSERT_GE(s_scalar.get(), 0);
  ASSERT_GE(H5DSset_scale(s_scalar, "scalar_scale"), 0);
  ASSERT_GE(H5DSattach_scale(scalar_dset, s_scalar, 0), 0);
  EXPECT_EQ(1, H5Aexists(scalar_dset, DIMENSION_LIST));

  // 5. Detach scales and verify attribute deletion once lists empty.
  ASSERT_GE(H5DSdetach_scale(d1, s1, 0), 0);
  EXPECT_EQ(0, H5DSis_attached(d1, s1, 0));
  EXPECT_EQ(1, H5DSis_attached(d1, s2, 0));
  EXPECT_EQ(1, H5DSget_num_scales(d1, 0));

  ASSERT_GE(H5DSdetach_scale(d1, s2, 0), 0);
  EXPECT_EQ(0, H5DSis_attached(d1, s2, 0));
  EXPECT_EQ(0, H5DSget_num_scales(d1, 0));
  // s2 has no remaining attached datasets -> REFERENCE_LIST deleted.
  EXPECT_EQ(0, H5Aexists(s2, REFERENCE_LIST));

  // Detach d1 dim 1 -> DIMENSION_LIST has no remaining scales so it is deleted.
  ASSERT_GE(H5DSdetach_scale(d1, s1, 1), 0);
  EXPECT_EQ(0, H5Aexists(d1, DIMENSION_LIST));

  // Detach remaining dataset d2 from s1 until REFERENCE_LIST is deleted.
  ASSERT_GE(H5DSdetach_scale(d2, s1, 1), 0);
  EXPECT_EQ(0, H5Aexists(s1, REFERENCE_LIST));

  // 6. Duplicate attach does not duplicate DIMENSION_LIST, though upstream
  //    H5DS.c still appends to REFERENCE_LIST when found_ds == 1.
  ASSERT_GE(H5DSattach_scale(d1, s1, 0), 0);
  ASSERT_GE(H5DSattach_scale(d1, s1, 0), 0);
  EXPECT_EQ(1, H5DSget_num_scales(d1, 0));
}

TEST(H5dsTest, IterateScalesResumeStopAndErrors) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  ScopedDataset dset(Create2dIntDataset(file, "data", 2, 3));
  ScopedDataset s1(Create1dDoubleDataset(file, "s1", 2));
  ScopedDataset s2(Create1dDoubleDataset(file, "s2", 2));
  ASSERT_GE(dset.get(), 0);
  ASSERT_GE(s1.get(), 0);
  ASSERT_GE(s2.get(), 0);

  ASSERT_GE(H5DSset_scale(s1, "first"), 0);
  ASSERT_GE(H5DSset_scale(s2, "second"), 0);

  // Iterating before any DIMENSION_LIST returns 0 cleanly without invoking fn.
  IterateLog empty_log;
  EXPECT_EQ(
      0, H5DSiterate_scales(dset, 0, nullptr, RecordScaleVisitor, &empty_log));
  EXPECT_TRUE(empty_log.names.empty());

  ASSERT_GE(H5DSattach_scale(dset, s1, 0), 0);
  ASSERT_GE(H5DSattach_scale(dset, s2, 0), 0);

  // Iterate all with idx == nullptr.
  IterateLog all_log;
  EXPECT_EQ(0,
            H5DSiterate_scales(dset, 0, nullptr, RecordScaleVisitor, &all_log));
  EXPECT_THAT(all_log.names, ElementsAre("first", "second"));
  EXPECT_THAT(all_log.dims, ElementsAre(0u, 0u));

  // Stop after 1st element (> 0), then resume from idx == 1.
  IterateLog stop_log{.stop_after = 1, .stop_code = 42};
  int idx = 0;
  EXPECT_EQ(42,
            H5DSiterate_scales(dset, 0, &idx, RecordScaleVisitor, &stop_log));
  EXPECT_EQ(0, idx);
  EXPECT_THAT(stop_log.names, ElementsAre("first"));

  idx = 1;
  IterateLog resume_log;
  EXPECT_EQ(0,
            H5DSiterate_scales(dset, 0, &idx, RecordScaleVisitor, &resume_log));
  EXPECT_EQ(1, idx);
  EXPECT_THAT(resume_log.names, ElementsAre("second"));

  // Negative return from visitor aborts and propagates value.
  IterateLog err_log{.stop_after = 1, .stop_code = -7};
  idx = 0;
  EXPECT_EQ(-7,
            H5DSiterate_scales(dset, 0, &idx, RecordScaleVisitor, &err_log));

  ScopedErrorSilence silence;
  int bad_idx = 2;
  EXPECT_LT(
      H5DSiterate_scales(dset, 0, &bad_idx, RecordScaleVisitor, &resume_log),
      0);
  EXPECT_LT(
      H5DSiterate_scales(dset, 2, nullptr, RecordScaleVisitor, &resume_log), 0);
  EXPECT_LT(
      H5DSiterate_scales(file, 0, nullptr, RecordScaleVisitor, &resume_log), 0);
  EXPECT_LT(H5DSiterate_scales(H5I_INVALID_HID, 0, nullptr, RecordScaleVisitor,
                               &resume_log),
            0);
}

TEST(H5dsTest, SetAndGetLabels) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  ScopedDataset dset(Create2dIntDataset(file, "data", 2, 3));
  ASSERT_GE(dset.get(), 0);

  std::array<char, 16> buf = {};
  buf.fill('Z');
  EXPECT_EQ(0, H5DSget_label(dset, 0, buf.data(), buf.size()));
  EXPECT_EQ('\0', buf[0]);
  EXPECT_EQ(0, H5DSget_label(dset, 0, nullptr, 0));

  // Set dim 1 first (creates DIMENSION_LABELS), then dim 0, then overwrite
  // dim 1.
  ASSERT_GE(H5DSset_label(dset, 1, "x_axis_meters"), 0);
  ASSERT_GE(H5DSset_label(dset, 0, "y_axis"), 0);
  EXPECT_EQ(6, H5DSget_label(dset, 0, buf.data(), buf.size()));
  EXPECT_THAT(buf.data(), StrEq("y_axis"));

  ASSERT_GE(H5DSset_label(dset, 1, "longitude"), 0);
  EXPECT_EQ(9, H5DSget_label(dset, 1, nullptr, 0));
  EXPECT_EQ(9, H5DSget_label(dset, 1, buf.data(), buf.size()));
  EXPECT_THAT(buf.data(), StrEq("longitude"));

  std::array<char, 5> trunc = {};
  trunc.fill('Z');
  EXPECT_EQ(9, H5DSget_label(dset, 1, trunc.data(), trunc.size()));
  EXPECT_THAT(trunc.data(), StrEq("long"));

  // Reading a dimension whose entry is still NULL returns 0.
  ScopedDataset dset2(Create2dIntDataset(file, "data2", 2, 2));
  ASSERT_GE(dset2.get(), 0);
  ASSERT_GE(H5DSset_label(dset2, 1, "only_dim1"), 0);
  EXPECT_EQ(0, H5DSget_label(dset2, 0, buf.data(), buf.size()));

  ScopedErrorSilence silence;
  EXPECT_LT(H5DSset_label(dset, 0, nullptr), 0);
  EXPECT_LT(H5DSset_label(dset, 2, "oob"), 0);
  EXPECT_LT(H5DSset_label(file, 0, "not_dset"), 0);
  EXPECT_LT(H5DSset_label(H5I_INVALID_HID, 0, "bad"), 0);
  EXPECT_LT(H5DSget_label(dset, 2, buf.data(), buf.size()), 0);
  EXPECT_LT(H5DSget_label(file, 0, buf.data(), buf.size()), 0);
  EXPECT_LT(H5DSget_label(H5I_INVALID_HID, 0, buf.data(), buf.size()), 0);
}

TEST(H5dsTest, ReservedDatasetsAsymmetricMetadataAndErrors) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);
  // Upstream H5DSis_scale() leaks `aid` and `atid` on non-conforming CLASS
  // attributes, keeping the Core file open unless swept before `file` closes.
  const absl::Cleanup sweep_leaked = [] {
    SweepOpenH5Ids(std::array{H5I_ATTR, H5I_DATATYPE});
  };

  ScopedDataset scale(Create1dDoubleDataset(file, "scale", 4));
  ASSERT_GE(scale.get(), 0);
  ASSERT_GE(H5DSset_scale(scale, "s"), 0);

  // Reserved HDF5 High-Level datasets (IMAGE, PALETTE, TABLE) are rejected.
  for (const char* reserved_class : {"IMAGE", "PALETTE", "TABLE"}) {
    const std::string name = std::string("res_") + reserved_class;
    ScopedDataset dset(Create1dDoubleDataset(file, name.c_str(), 4));
    ASSERT_GE(dset.get(), 0);
    ASSERT_GE(
        H5LTset_attribute_string(file, name.c_str(), "CLASS", reserved_class),
        0);
    ScopedErrorSilence silence;
    EXPECT_LT(H5DSattach_scale(dset, scale, 0), 0);
  }

  // Non-reserved custom CLASS string succeeds.
  ScopedDataset custom(Create1dDoubleDataset(file, "custom", 4));
  ASSERT_GE(custom.get(), 0);
  ASSERT_GE(H5LTset_attribute_string(file, "custom", "CLASS", "CUSTOM"), 0);
  ASSERT_GE(H5DSattach_scale(custom, scale, 0), 0);
  EXPECT_EQ(1, H5DSis_attached(custom, scale, 0));

  // Corrupt CLASS attributes fail H5DS_is_reserved cleanly.
  ScopedDataset bad_int_cls(Create1dDoubleDataset(file, "bad_int_cls", 4));
  ASSERT_GE(bad_int_cls.get(), 0);
  constexpr int32_t kZero = 0;
  ASSERT_GE(H5LTset_attribute_int(file, "bad_int_cls", "CLASS", &kZero, 1), 0);

  ScopedDataset bad_pad_cls(Create1dDoubleDataset(file, "bad_pad_cls", 4));
  ASSERT_GE(bad_pad_cls.get(), 0);
  ASSERT_GE(WriteNullpadScalarStringAttr(bad_pad_cls, "CLASS", "IMAGE"), 0);

  // Asymmetric DIMENSION_LIST / REFERENCE_LIST error paths on
  // detach/is_attached.
  ScopedDataset d_only_dim(Create1dDoubleDataset(file, "d_only_dim", 4));
  ScopedDataset d_other(Create1dDoubleDataset(file, "d_other", 4));
  ScopedDataset s_unused(Create1dDoubleDataset(file, "s_unused", 4));
  ASSERT_GE(d_only_dim.get(), 0);
  ASSERT_GE(d_other.get(), 0);
  ASSERT_GE(s_unused.get(), 0);
  ASSERT_GE(H5DSset_scale(s_unused, "unused"), 0);
  ASSERT_GE(H5DSattach_scale(d_only_dim, s_unused, 0), 0);
  // Remove REFERENCE_LIST manually to hit has_reflist == 0 and found_dset == 0.
  ScopedDataset d_other2(Create1dDoubleDataset(file, "d_other2", 4));
  ScopedDataset s_mismatch(Create1dDoubleDataset(file, "s_mismatch", 4));
  ASSERT_GE(d_other2.get(), 0);
  ASSERT_GE(s_mismatch.get(), 0);
  ASSERT_GE(H5DSattach_scale(d_other2, s_mismatch, 0), 0);

  // s_unused has d_only_dim attached at dim 0; d_other has s_unused on nothing.
  ASSERT_GE(H5DSattach_scale(d_other, scale, 0), 0);
  EXPECT_EQ(0, H5DSis_attached(d_other, s_unused, 0));

  ScopedErrorSilence silence;
  EXPECT_LT(H5DSattach_scale(bad_int_cls, scale, 0), 0);
  EXPECT_LT(H5DSattach_scale(bad_pad_cls, scale, 0), 0);
  EXPECT_LT(H5DSattach_scale(scale, s_unused, 0), 0);
  EXPECT_LT(H5DSattach_scale(custom, custom, 0), 0);
  EXPECT_LT(H5DSattach_scale(custom, file, 0), 0);
  EXPECT_LT(H5DSattach_scale(custom, H5I_INVALID_HID, 0), 0);
  EXPECT_LT(H5DSattach_scale(custom, s_unused, 5), 0);

  EXPECT_LT(H5DSdetach_scale(d_other, s_unused, 0), 0);
  EXPECT_LT(H5DSdetach_scale(d_other, s_mismatch, 0), 0);
  EXPECT_LT(H5DSdetach_scale(scale, s_unused, 0), 0);
  EXPECT_LT(H5DSdetach_scale(custom, custom, 0), 0);
  EXPECT_LT(H5DSdetach_scale(custom, s_unused, 5), 0);
  EXPECT_LT(H5DSdetach_scale(file, s_unused, 0), 0);
  EXPECT_LT(H5DSdetach_scale(custom, H5I_INVALID_HID, 0), 0);

  // Delete REFERENCE_LIST on s_unused -> detach fails at has_reflist == 0.
  ASSERT_GE(H5Adelete(s_unused, REFERENCE_LIST), 0);
  EXPECT_EQ(0, H5DSis_attached(d_only_dim, s_unused, 0));
  EXPECT_LT(H5DSdetach_scale(d_only_dim, s_unused, 0), 0);

  // Detach when DIMENSION_LIST is absent.
  EXPECT_LT(H5DSdetach_scale(bad_int_cls, s_unused, 0), 0);

  EXPECT_LT(H5DSis_attached(scale, s_unused, 0), 0);
  EXPECT_LT(H5DSis_attached(custom, custom, 0), 0);
  EXPECT_LT(H5DSis_attached(custom, file, 0), 0);
  EXPECT_LT(H5DSis_attached(custom, s_unused, 5), 0);
  EXPECT_LT(H5DSis_attached(H5I_INVALID_HID, s_unused, 0), 0);

  EXPECT_LT(H5DSget_num_scales(file, 0), 0);
  EXPECT_LT(H5DSget_num_scales(custom, 5), 0);
  EXPECT_LT(H5DSget_num_scales(H5I_INVALID_HID, 0), 0);
}

// Pass-through VOL connector that reports non-native identity when
// `report_native == false` (triggering H5DS `is_new_ref == true`) or native
// identity when `report_native == true` (`is_new_ref == false`), intercepts
// compound `REFERENCE_LIST` I/O to bypass upstream H5Tref.c alignment UB, and
// supports deterministic fault injection.
struct NdsListCompat {
  H5R_ref_t ref;
  unsigned int dim_idx;
};
static_assert(alignof(NdsListCompat) == alignof(H5R_ref_t));

struct StoredRefEntry {
  H5O_token_t target_token;
  unsigned int dim_idx;
};

using TokenKey = std::array<uint8_t, H5O_MAX_TOKEN_SIZE>;

struct DsFaultVolState {
  hid_t active_file_id = H5I_INVALID_HID;
  bool report_native = false;
  bool fail_introspect_get_conn_cls = false;
  bool in_internal_vol_op = false;
  int in_attr_specific = 0;

  int attr_create_calls = 0;
  int fail_attr_create_call = 0;
  int attr_open_calls = 0;
  int fail_attr_open_call = 0;
  int attr_get_type_calls = 0;
  int fail_attr_get_type_call = 0;
  int attr_get_space_calls = 0;
  int fail_attr_get_space_call = 0;
  int attr_read_calls = 0;
  int fail_attr_read_call = 0;
  int attr_write_calls = 0;
  int fail_attr_write_call = 0;
  int attr_close_calls = 0;
  int fail_attr_close_call = 0;
  int attr_exists_calls = 0;
  int fail_attr_exists_call = 0;
  bool fail_attr_exists = false;
  bool fail_attr_delete = false;
  int dset_get_space_calls = 0;
  int fail_dset_get_space_call = 0;
  bool fail_dset_get_space = false;
  int invalid_dset_get_space_call = 0;
  int invalid_attr_get_type_call = 0;
  int immutable_str_attr_get_type_call = 0;
  int invalid_attr_get_space_call = 0;
  int dset_close_calls = 0;
  int fail_dset_close_call = 0;
  int object_open_calls = 0;
  int fail_object_open_call = 0;
  int object_get_info_calls = 0;
  int fail_object_get_info_call = 0;
  int object_lookup_calls = 0;
  int fail_object_lookup_call = 0;
  int token_cmp_calls = 0;
  int fail_token_cmp_call = 0;

  // Keep raw native `void*` attribute pointers unwrapped (HDF5 casts
  // `H5VL_object_t::data` directly to `H5A_t*` inside
  // `H5F__get_objects_cb`), tracking `REFERENCE_LIST` handles in a side table.
  std::map<void*, TokenKey> open_ref_attrs;
  std::map<TokenKey, std::vector<StoredRefEntry>> ref_lists;
  std::map<TokenKey, size_t> ref_list_counts;

  void ResetFaults() {
    const hid_t saved_file = active_file_id;
    const bool saved_native = report_native;
    auto saved_open_refs = std::move(open_ref_attrs);
    auto saved_lists = std::move(ref_lists);
    auto saved_counts = std::move(ref_list_counts);
    *this = DsFaultVolState{};
    active_file_id = saved_file;
    report_native = saved_native;
    open_ref_attrs = std::move(saved_open_refs);
    ref_lists = std::move(saved_lists);
    ref_list_counts = std::move(saved_counts);
  }

  void Reset() { *this = DsFaultVolState{}; }
};

DsFaultVolState g_ds_vol_state;

class ScopedInternalVolOp {
 public:
  ScopedInternalVolOp() : prev_(g_ds_vol_state.in_internal_vol_op) {
    g_ds_vol_state.in_internal_vol_op = true;
  }
  ~ScopedInternalVolOp() { g_ds_vol_state.in_internal_vol_op = prev_; }

 private:
  bool prev_;
};

bool DsVolGetOwnerKey(void* obj, const H5VL_loc_params_t* loc_params,
                      hid_t dxpl_id, TokenKey* out_key) {
  H5O_info2_t oinfo = {};
  H5VL_object_get_args_t gargs = {};
  gargs.op_type = H5VL_OBJECT_GET_INFO;
  gargs.args.get_info.fields = H5O_INFO_BASIC;
  gargs.args.get_info.oinfo = &oinfo;
  if (H5VLobject_get(obj, loc_params, H5VL_NATIVE, &gargs, dxpl_id, nullptr) <
      0) {
    return false;
  }
  std::memcpy(out_key->data(), oinfo.token.__data, H5O_MAX_TOKEN_SIZE);
  return true;
}

void* DsVolFileCreate(const char* name, unsigned flags, hid_t fcpl_id,
                      hid_t fapl_id, hid_t dxpl_id, void** req) {
  ScopedPlist under_fapl(H5Pcopy(fapl_id));
  if (!under_fapl.valid() || H5Pset_vol(under_fapl, H5VL_NATIVE, nullptr) < 0) {
    return nullptr;
  }
  return H5VLfile_create(name, flags, fcpl_id, under_fapl, dxpl_id, req);
}

herr_t DsVolFileGet(void* file, H5VL_file_get_args_t* args, hid_t dxpl_id,
                    void** req) {
  return H5VLfile_get(file, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t DsVolFileSpecific(void* obj, H5VL_file_specific_args_t* args,
                         hid_t dxpl_id, void** req) {
  return H5VLfile_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t DsVolFileOptional(void* file, H5VL_optional_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLfile_optional(file, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t DsVolFileClose(void* file, hid_t dxpl_id, void** req) {
  return H5VLfile_close(file, H5VL_NATIVE, dxpl_id, req);
}

void* DsVolAttrCreate(void* obj, const H5VL_loc_params_t* loc_params,
                      const char* attr_name, hid_t type_id, hid_t space_id,
                      hid_t acpl_id, hid_t aapl_id, hid_t dxpl_id, void** req) {
  if (!g_ds_vol_state.in_internal_vol_op) {
    ++g_ds_vol_state.attr_create_calls;
    if (g_ds_vol_state.fail_attr_create_call ==
        g_ds_vol_state.attr_create_calls) {
      return nullptr;
    }
  }
  void* native_attr =
      H5VLattr_create(obj, loc_params, H5VL_NATIVE, attr_name, type_id,
                      space_id, acpl_id, aapl_id, dxpl_id, req);
  if (native_attr == nullptr) {
    return nullptr;
  }
  if (!g_ds_vol_state.report_native && attr_name != nullptr &&
      std::strcmp(attr_name, REFERENCE_LIST) == 0) {
    TokenKey key = {};
    if (DsVolGetOwnerKey(obj, loc_params, dxpl_id, &key)) {
      const hssize_t npoints = H5Sget_simple_extent_npoints(space_id);
      if (npoints >= 0) {
        g_ds_vol_state.ref_list_counts[key] = static_cast<size_t>(npoints);
        g_ds_vol_state.open_ref_attrs[native_attr] = key;
      }
    }
  }
  return native_attr;
}

void* DsVolAttrOpen(void* obj, const H5VL_loc_params_t* loc_params,
                    const char* attr_name, hid_t aapl_id, hid_t dxpl_id,
                    void** req) {
  if (!g_ds_vol_state.in_internal_vol_op) {
    ++g_ds_vol_state.attr_open_calls;
    if (g_ds_vol_state.fail_attr_open_call == g_ds_vol_state.attr_open_calls) {
      return nullptr;
    }
  }
  void* native_attr = H5VLattr_open(obj, loc_params, H5VL_NATIVE, attr_name,
                                    aapl_id, dxpl_id, req);
  if (native_attr == nullptr) {
    return nullptr;
  }
  if (!g_ds_vol_state.report_native && attr_name != nullptr &&
      std::strcmp(attr_name, REFERENCE_LIST) == 0) {
    TokenKey key = {};
    if (DsVolGetOwnerKey(obj, loc_params, dxpl_id, &key)) {
      g_ds_vol_state.open_ref_attrs[native_attr] = key;
    }
  }
  return native_attr;
}

herr_t DsVolAttrRead(void* attr, hid_t mem_type_id, void* buf, hid_t dxpl_id,
                     void** req) {
  if (!g_ds_vol_state.in_internal_vol_op) {
    ++g_ds_vol_state.attr_read_calls;
    if (g_ds_vol_state.fail_attr_read_call == g_ds_vol_state.attr_read_calls) {
      return -1;
    }
  }
  if (const auto ref_it = g_ds_vol_state.open_ref_attrs.find(attr);
      ref_it != g_ds_vol_state.open_ref_attrs.end()) {
    ScopedInternalVolOp guard;
    const auto it = g_ds_vol_state.ref_lists.find(ref_it->second);
    if (it == g_ds_vol_state.ref_lists.end() ||
        g_ds_vol_state.active_file_id < 0) {
      return -1;
    }
    const auto& entries = it->second;
    auto* dst = static_cast<NdsListCompat*>(buf);
    std::memset(dst, 0, entries.size() * sizeof(NdsListCompat));
    for (size_t k = 0; k < entries.size(); ++k) {
      ScopedDataset target(H5Oopen_by_token(g_ds_vol_state.active_file_id,
                                            entries[k].target_token));
      if (!target.valid() ||
          H5Rcreate_object(target.get(), ".", H5P_DEFAULT, &dst[k].ref) < 0) {
        return -1;
      }
      dst[k].dim_idx = entries[k].dim_idx;
    }
    return 0;
  }
  return H5VLattr_read(attr, H5VL_NATIVE, mem_type_id, buf, dxpl_id, req);
}

herr_t DsVolAttrWrite(void* attr, hid_t mem_type_id, const void* buf,
                      hid_t dxpl_id, void** req) {
  if (!g_ds_vol_state.in_internal_vol_op) {
    ++g_ds_vol_state.attr_write_calls;
    if (g_ds_vol_state.fail_attr_write_call ==
        g_ds_vol_state.attr_write_calls) {
      return -1;
    }
  }
  if (const auto ref_it = g_ds_vol_state.open_ref_attrs.find(attr);
      ref_it != g_ds_vol_state.open_ref_attrs.end()) {
    ScopedInternalVolOp guard;
    const auto count_it = g_ds_vol_state.ref_list_counts.find(ref_it->second);
    if (count_it == g_ds_vol_state.ref_list_counts.end()) {
      return -1;
    }
    const size_t npoints = count_it->second;
    const auto* src = static_cast<const NdsListCompat*>(buf);
    std::vector<StoredRefEntry> entries;
    entries.reserve(npoints);
    for (size_t k = 0; k < npoints; ++k) {
      H5R_ref_t ref_copy = src[k].ref;
      ScopedDataset target(H5Ropen_object(&ref_copy, H5P_DEFAULT, H5P_DEFAULT));
      if (!target.valid()) {
        return -1;
      }
      H5O_info2_t oi = {};
      if (H5Oget_info3(target.get(), &oi, H5O_INFO_BASIC) < 0) {
        return -1;
      }
      entries.push_back(StoredRefEntry{oi.token, src[k].dim_idx});
    }
    g_ds_vol_state.ref_lists[ref_it->second] = std::move(entries);
    return 0;
  }
  return H5VLattr_write(attr, H5VL_NATIVE, mem_type_id, buf, dxpl_id, req);
}

herr_t DsVolAttrGet(void* attr, H5VL_attr_get_args_t* args, hid_t dxpl_id,
                    void** req) {
  if (!g_ds_vol_state.in_internal_vol_op) {
    if (args->op_type == H5VL_ATTR_GET_TYPE) {
      ++g_ds_vol_state.attr_get_type_calls;
      if (g_ds_vol_state.fail_attr_get_type_call ==
          g_ds_vol_state.attr_get_type_calls) {
        return -1;
      }
      if (g_ds_vol_state.invalid_attr_get_type_call ==
          g_ds_vol_state.attr_get_type_calls) {
        // Return a non-datatype valid-ish small ID (>= 0) with no new native ID
        // so H5Aget_type succeeds and subsequent H5T* checks fail leak-free.
        args->args.get_type.type_id = H5P_DEFAULT;
        return 0;
      }
      if (g_ds_vol_state.immutable_str_attr_get_type_call ==
          g_ds_vol_state.attr_get_type_calls) {
        args->args.get_type.type_id = H5T_C_S1;
        return 0;
      }
    } else if (args->op_type == H5VL_ATTR_GET_SPACE) {
      ++g_ds_vol_state.attr_get_space_calls;
      if (g_ds_vol_state.fail_attr_get_space_call ==
          g_ds_vol_state.attr_get_space_calls) {
        return -1;
      }
      if (g_ds_vol_state.invalid_attr_get_space_call ==
          g_ds_vol_state.attr_get_space_calls) {
        args->args.get_space.space_id = H5P_DEFAULT;
        return 0;
      }
    }
  }
  return H5VLattr_get(attr, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t DsVolAttrSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                         H5VL_attr_specific_args_t* args, hid_t dxpl_id,
                         void** req) {
  if (!g_ds_vol_state.in_internal_vol_op) {
    if (args->op_type == H5VL_ATTR_EXISTS) {
      ++g_ds_vol_state.attr_exists_calls;
      if (g_ds_vol_state.fail_attr_exists ||
          g_ds_vol_state.fail_attr_exists_call ==
              g_ds_vol_state.attr_exists_calls) {
        return -1;
      }
    }
    if (args->op_type == H5VL_ATTR_DELETE && g_ds_vol_state.fail_attr_delete) {
      return -1;
    }
  }
  ++g_ds_vol_state.in_attr_specific;
  const herr_t status =
      H5VLattr_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
  --g_ds_vol_state.in_attr_specific;
  if (status >= 0 && !g_ds_vol_state.report_native &&
      args->op_type == H5VL_ATTR_DELETE && args->args.del.name != nullptr &&
      std::strcmp(args->args.del.name, REFERENCE_LIST) == 0) {
    TokenKey key = {};
    if (DsVolGetOwnerKey(obj, loc_params, dxpl_id, &key)) {
      g_ds_vol_state.ref_lists.erase(key);
      g_ds_vol_state.ref_list_counts.erase(key);
    }
  }
  return status;
}

herr_t DsVolAttrOptional(void* attr, H5VL_optional_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLattr_optional(attr, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t DsVolAttrClose(void* attr, hid_t dxpl_id, void** req) {
  if (!g_ds_vol_state.in_internal_vol_op) {
    ++g_ds_vol_state.attr_close_calls;
    if (g_ds_vol_state.fail_attr_close_call ==
        g_ds_vol_state.attr_close_calls) {
      return -1;
    }
  }
  g_ds_vol_state.open_ref_attrs.erase(attr);
  return H5VLattr_close(attr, H5VL_NATIVE, dxpl_id, req);
}

void* DsVolDatasetCreate(void* obj, const H5VL_loc_params_t* loc_params,
                         const char* name, hid_t lcpl_id, hid_t type_id,
                         hid_t space_id, hid_t dcpl_id, hid_t dapl_id,
                         hid_t dxpl_id, void** req) {
  return H5VLdataset_create(obj, loc_params, H5VL_NATIVE, name, lcpl_id,
                            type_id, space_id, dcpl_id, dapl_id, dxpl_id, req);
}

void* DsVolDatasetOpen(void* obj, const H5VL_loc_params_t* loc_params,
                       const char* name, hid_t dapl_id, hid_t dxpl_id,
                       void** req) {
  return H5VLdataset_open(obj, loc_params, H5VL_NATIVE, name, dapl_id, dxpl_id,
                          req);
}

herr_t DsVolDatasetRead(size_t count, void* dset[], hid_t mem_type_id[],
                        hid_t mem_space_id[], hid_t file_space_id[],
                        hid_t dxpl_id, void* buf[], void** req) {
  return H5VLdataset_read(count, dset, H5VL_NATIVE, mem_type_id, mem_space_id,
                          file_space_id, dxpl_id, buf, req);
}

herr_t DsVolDatasetWrite(size_t count, void* dset[], hid_t mem_type_id[],
                         hid_t mem_space_id[], hid_t file_space_id[],
                         hid_t dxpl_id, const void* buf[], void** req) {
  return H5VLdataset_write(count, dset, H5VL_NATIVE, mem_type_id, mem_space_id,
                           file_space_id, dxpl_id, buf, req);
}

herr_t DsVolDatasetGet(void* dset, H5VL_dataset_get_args_t* args, hid_t dxpl_id,
                       void** req) {
  if (!g_ds_vol_state.in_internal_vol_op &&
      args->op_type == H5VL_DATASET_GET_SPACE) {
    ++g_ds_vol_state.dset_get_space_calls;
    if (g_ds_vol_state.fail_dset_get_space ||
        g_ds_vol_state.fail_dset_get_space_call ==
            g_ds_vol_state.dset_get_space_calls) {
      return -1;
    }
    if (g_ds_vol_state.invalid_dset_get_space_call ==
        g_ds_vol_state.dset_get_space_calls) {
      args->args.get_space.space_id = H5P_DEFAULT;
      return 0;
    }
  }
  return H5VLdataset_get(dset, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t DsVolDatasetSpecific(void* obj, H5VL_dataset_specific_args_t* args,
                            hid_t dxpl_id, void** req) {
  return H5VLdataset_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t DsVolDatasetOptional(void* obj, H5VL_optional_args_t* args,
                            hid_t dxpl_id, void** req) {
  return H5VLdataset_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t DsVolDatasetClose(void* dset, hid_t dxpl_id, void** req) {
  // H5Dclose uses H5I_dec_app_ref_always_close, which removes the ID and frees
  // H5VL_object_t even if H5D__close_cb fails; close native storage first so
  // injected close faults never leak the underlying H5D_t.
  const herr_t status = H5VLdataset_close(dset, H5VL_NATIVE, dxpl_id, req);
  if (!g_ds_vol_state.in_internal_vol_op &&
      g_ds_vol_state.in_attr_specific == 0) {
    ++g_ds_vol_state.dset_close_calls;
    if (g_ds_vol_state.fail_dset_close_call ==
        g_ds_vol_state.dset_close_calls) {
      return -1;
    }
  }
  return status;
}

herr_t DsVolLinkGet(void* obj, const H5VL_loc_params_t* loc_params,
                    H5VL_link_get_args_t* args, hid_t dxpl_id, void** req) {
  return H5VLlink_get(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t DsVolLinkSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                         H5VL_link_specific_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLlink_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

void* DsVolObjectOpen(void* obj, const H5VL_loc_params_t* loc_params,
                      H5I_type_t* opened_type, hid_t dxpl_id, void** req) {
  if (!g_ds_vol_state.in_internal_vol_op) {
    ++g_ds_vol_state.object_open_calls;
    if (g_ds_vol_state.fail_object_open_call ==
        g_ds_vol_state.object_open_calls) {
      return nullptr;
    }
  }
  return H5VLobject_open(obj, loc_params, H5VL_NATIVE, opened_type, dxpl_id,
                         req);
}

herr_t DsVolObjectGet(void* obj, const H5VL_loc_params_t* loc_params,
                      H5VL_object_get_args_t* args, hid_t dxpl_id, void** req) {
  if (!g_ds_vol_state.in_internal_vol_op &&
      args->op_type == H5VL_OBJECT_GET_INFO) {
    ++g_ds_vol_state.object_get_info_calls;
    if (g_ds_vol_state.fail_object_get_info_call ==
        g_ds_vol_state.object_get_info_calls) {
      return -1;
    }
  }
  return H5VLobject_get(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t DsVolObjectSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                           H5VL_object_specific_args_t* args, hid_t dxpl_id,
                           void** req) {
  if (!g_ds_vol_state.in_internal_vol_op &&
      args->op_type == H5VL_OBJECT_LOOKUP) {
    ++g_ds_vol_state.object_lookup_calls;
    if (g_ds_vol_state.fail_object_lookup_call ==
        g_ds_vol_state.object_lookup_calls) {
      return -1;
    }
  }
  return H5VLobject_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t DsVolObjectOptional(void* obj, const H5VL_loc_params_t* loc_params,
                           H5VL_optional_args_t* args, hid_t dxpl_id,
                           void** req) {
  return H5VLobject_optional(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t DsVolBlobPut(void* obj, const void* buf, size_t size, void* blob_id,
                    void* ctx) {
  return H5VLblob_put(obj, H5VL_NATIVE, buf, size, blob_id, ctx);
}

herr_t DsVolBlobGet(void* obj, const void* blob_id, void* buf, size_t size,
                    void* ctx) {
  return H5VLblob_get(obj, H5VL_NATIVE, blob_id, buf, size, ctx);
}

herr_t DsVolBlobSpecific(void* obj, void* blob_id,
                         H5VL_blob_specific_args_t* args) {
  return H5VLblob_specific(obj, H5VL_NATIVE, blob_id, args);
}

herr_t DsVolTokenCmp(void* obj, const H5O_token_t* token1,
                     const H5O_token_t* token2, int* cmp_value) {
  if (!g_ds_vol_state.in_internal_vol_op) {
    ++g_ds_vol_state.token_cmp_calls;
    if (g_ds_vol_state.fail_token_cmp_call == g_ds_vol_state.token_cmp_calls) {
      return -1;
    }
  }
  return H5VLtoken_cmp(obj, H5VL_NATIVE, token1, token2, cmp_value);
}

herr_t DsVolTokenToStr(void* obj, H5I_type_t obj_type, const H5O_token_t* token,
                       char** token_str) {
  return H5VLtoken_to_str(obj, obj_type, H5VL_NATIVE, token, token_str);
}

herr_t DsVolTokenFromStr(void* obj, H5I_type_t obj_type, const char* token_str,
                         H5O_token_t* token) {
  return H5VLtoken_from_str(obj, obj_type, H5VL_NATIVE, token_str, token);
}

extern const H5VL_class_t kDsNonNativeVolClass;

herr_t DsVolIntrospectGetConnCls(void* obj, H5VL_get_conn_lvl_t lvl,
                                 const H5VL_class_t** conn_cls) {
  if (!g_ds_vol_state.in_internal_vol_op &&
      g_ds_vol_state.fail_introspect_get_conn_cls) {
    return -1;
  }
  if (g_ds_vol_state.report_native) {
    return H5VLintrospect_get_conn_cls(obj, H5VL_NATIVE, lvl, conn_cls);
  }
  // Report this connector for both CURR and TERM so H5VLobject_is_native sets
  // `*native = false` and H5DSwith_new_ref returns `true`.
  *conn_cls = &kDsNonNativeVolClass;
  return 0;
}

herr_t DsVolIntrospectGetCapFlags(const void* /*info*/, uint64_t* cap_flags) {
  return H5VLintrospect_get_cap_flags(nullptr, H5VL_NATIVE, cap_flags);
}

herr_t DsVolIntrospectOptQuery(void* obj, H5VL_subclass_t cls, int opt_type,
                               uint64_t* flags) {
  return H5VLintrospect_opt_query(obj, H5VL_NATIVE, cls, opt_type, flags);
}

const H5VL_class_t kDsNonNativeVolClass = {
    .version = H5VL_VERSION,
    .value = static_cast<H5VL_class_value_t>(51005),
    .name = "h5ds_nonnative_vol",
    .conn_version = 1,
    .cap_flags = 0,
    .initialize = nullptr,
    .terminate = nullptr,
    .info_cls = {},
    .wrap_cls = {},
    .attr_cls =
        {
            .create = DsVolAttrCreate,
            .open = DsVolAttrOpen,
            .read = DsVolAttrRead,
            .write = DsVolAttrWrite,
            .get = DsVolAttrGet,
            .specific = DsVolAttrSpecific,
            .optional = DsVolAttrOptional,
            .close = DsVolAttrClose,
        },
    .dataset_cls =
        {
            .create = DsVolDatasetCreate,
            .open = DsVolDatasetOpen,
            .read = DsVolDatasetRead,
            .write = DsVolDatasetWrite,
            .get = DsVolDatasetGet,
            .specific = DsVolDatasetSpecific,
            .optional = DsVolDatasetOptional,
            .close = DsVolDatasetClose,
        },
    .datatype_cls = {},
    .file_cls =
        {
            .create = DsVolFileCreate,
            .open = nullptr,
            .get = DsVolFileGet,
            .specific = DsVolFileSpecific,
            .optional = DsVolFileOptional,
            .close = DsVolFileClose,
        },
    .group_cls = {},
    .link_cls =
        {
            .create = nullptr,
            .copy = nullptr,
            .move = nullptr,
            .get = DsVolLinkGet,
            .specific = DsVolLinkSpecific,
            .optional = nullptr,
        },
    .object_cls =
        {
            .open = DsVolObjectOpen,
            .copy = nullptr,
            .get = DsVolObjectGet,
            .specific = DsVolObjectSpecific,
            .optional = DsVolObjectOptional,
        },
    .introspect_cls =
        {
            .get_conn_cls = DsVolIntrospectGetConnCls,
            .get_cap_flags = DsVolIntrospectGetCapFlags,
            .opt_query = DsVolIntrospectOptQuery,
        },
    .request_cls = {},
    .blob_cls =
        {
            .put = DsVolBlobPut,
            .get = DsVolBlobGet,
            .specific = DsVolBlobSpecific,
            .optional = nullptr,
        },
    .token_cls =
        {
            .cmp = DsVolTokenCmp,
            .to_str = DsVolTokenToStr,
            .from_str = DsVolTokenFromStr,
        },
    .optional = nullptr,
};

template <typename FieldT, typename Fn>
void ExpectDsFault(FieldT DsFaultVolState::* knob, FieldT value, Fn&& op) {
  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.*knob = value;
  EXPECT_LT(op(), 0);
}

void CleanupVolConnector(hid_t vol_id) {
  g_ds_vol_state.Reset();
  // Sweep any attribute/dataset/file IDs left open by injected close faults or
  // upstream H5DS.c leaks (e.g., H5DSis_scale non-conforming CLASS attributes
  // and H5DSattach_scale `tmp_id`) so ASan/LSan, Testify, and VOL
  // unregistration remain clean. `SweepOpenH5Ids()` collects IDs via
  // `H5Iiterate` under `ScopedErrorSilence` before closing them.
  SweepOpenH5Ids();
  H5VLunregister_connector(vol_id);
}

TEST(H5dsTest, NewRefLifecycleAndVolFaultPropagation) {
  g_ds_vol_state.Reset();
  const hid_t vol_id =
      H5VLregister_connector(&kDsNonNativeVolClass, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  const absl::Cleanup cleanup_vol = [vol_id] { CleanupVolConnector(vol_id); };

  // H5T_STD_REF attributes require at least 1.12 format support.
  ScopedFile file =
      CreateCoreVolFile(vol_id, "h5ds_new_ref.h5", /*latest_format=*/true);
  ASSERT_GE(file.get(), 0);
  g_ds_vol_state.active_file_id = file.get();

  bool with_new_ref = false;
  ASSERT_GE(H5DSwith_new_ref(file, &with_new_ref), 0);
  EXPECT_TRUE(with_new_ref);

  ScopedDataset d1(Create2dIntDataset(file, "d1", 3, 4));
  ScopedDataset d2(Create2dIntDataset(file, "d2", 3, 4));
  ScopedDataset s1(Create1dDoubleDataset(file, "s1", 3));
  ScopedDataset s2(Create1dDoubleDataset(file, "s2", 3));
  ASSERT_GE(d1.get(), 0);
  ASSERT_GE(d2.get(), 0);
  ASSERT_GE(s1.get(), 0);
  ASSERT_GE(s2.get(), 0);

  ASSERT_GE(H5DSset_scale(s1, "nr_s1"), 0);

  // Full new-reference attach/extend/iterate/is_attached lifecycle.
  ASSERT_GE(H5DSattach_scale(d1, s1, 0), 0);
  ASSERT_GE(H5DSattach_scale(d1, s2, 0), 0);
  EXPECT_EQ(2, H5DSget_num_scales(d1, 0));
  EXPECT_EQ(1, H5DSis_attached(d1, s1, 0));
  EXPECT_EQ(1, H5DSis_attached(d1, s2, 0));
  EXPECT_EQ(0, H5DSis_attached(d1, s1, 1));
  EXPECT_EQ(0, H5DSis_attached(d2, s1, 0));

  IterateLog iter_log;
  EXPECT_EQ(0,
            H5DSiterate_scales(d1, 0, nullptr, RecordScaleVisitor, &iter_log));
  // Implicitly promoted scale s2 has no NAME attribute -> H5DSget_scale_name
  // returns 0 and leaves an empty string.
  EXPECT_THAT(iter_log.names, ElementsAre("nr_s1", ""));

  int idx = 0;
  IterateLog stop_log{.stop_after = 1, .stop_code = 11};
  EXPECT_EQ(11, H5DSiterate_scales(d1, 0, &idx, RecordScaleVisitor, &stop_log));

#if !defined(ABSL_HAVE_ADDRESS_SANITIZER) && !defined(__SANITIZE_ADDRESS__)
  // TODO(schwehr): Re-enable under ASan once upstream H5DS.c fixes `tmp_id`
  // / `ndsbuf[j].ref` leaks in H5DSattach_scale (`has_reflist > 0`) and missing
  // `H5Treclaim` / `H5Rdestroy` calls in H5DSdetach_scale on `is_new_ref`
  // paths. Run full new-reference extend/detach/empty-cleanup under non-ASan
  // builds and coverage.
  ASSERT_GE(H5DSattach_scale(d1, s1, 1), 0);
  ASSERT_GE(H5DSattach_scale(d2, s1, 1), 0);
  EXPECT_EQ(1, H5DSis_attached(d1, s1, 1));
  EXPECT_EQ(1, H5DSis_attached(d2, s1, 1));
  EXPECT_EQ(0, H5DSis_attached(d2, s1, 0));

  ASSERT_GE(H5DSdetach_scale(d1, s1, 0), 0);
  EXPECT_EQ(0, H5DSis_attached(d1, s1, 0));
  EXPECT_EQ(1, H5DSis_attached(d1, s2, 0));
  ASSERT_GE(H5DSdetach_scale(d1, s2, 0), 0);
  ASSERT_GE(H5DSdetach_scale(d1, s1, 1), 0);
  ASSERT_GE(H5DSdetach_scale(d2, s1, 1), 0);
  EXPECT_EQ(0, H5Aexists(d1, DIMENSION_LIST));
  EXPECT_EQ(0, H5Aexists(s1, REFERENCE_LIST));

  // Re-attach cleanly and exercise found_ds == 1 destroy branch.
  ASSERT_GE(H5DSattach_scale(d1, s1, 0), 0);
  ASSERT_GE(H5DSattach_scale(d1, s1, 0), 0);
  ASSERT_GE(H5DSdetach_scale(d1, s1, 0), 0);
  // Leave d1 dim 0 = [s1, s2] for fault-injection checks below.
  ASSERT_GE(H5DSattach_scale(d1, s1, 0), 0);
  ASSERT_GE(H5DSattach_scale(d1, s2, 0), 0);
#endif  // !defined(ABSL_HAVE_ADDRESS_SANITIZER) &&
        // !defined(__SANITIZE_ADDRESS__)
  ASSERT_GE(H5DSset_label(d1, 0, "nr_label"), 0);

  // Use a separate pair to exercise new-ref detach DIMENSION_LIST update
  // followed by a leak-free failure before REFERENCE_LIST read.
  ScopedDataset d_det(Create2dIntDataset(file, "d_det", 2, 2));
  ScopedDataset s_det(Create1dDoubleDataset(file, "s_det", 2));
  ASSERT_GE(d_det.get(), 0);
  ASSERT_GE(s_det.get(), 0);
  ASSERT_GE(H5DSset_scale(s_det, "s_det"), 0);
  ASSERT_GE(H5DSattach_scale(d_det, s_det, 0), 0);
  {
    ScopedErrorSilence silence_det;
    g_ds_vol_state.ResetFaults();
    g_ds_vol_state.fail_attr_open_call = 2;
    EXPECT_LT(H5DSdetach_scale(d_det, s_det, 0), 0);
  }

  ScopedErrorSilence silence;

  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_introspect_get_conn_cls = true;
  EXPECT_LT(H5DSwith_new_ref(file, &with_new_ref), 0);
  EXPECT_LT(H5DSattach_scale(d2, s2, 0), 0);
  EXPECT_LT(H5DSdetach_scale(d1, s1, 0), 0);
  EXPECT_LT(H5DSis_attached(d1, s1, 0), 0);
  EXPECT_LT(H5DSiterate_scales(d1, 0, nullptr, RecordScaleVisitor, &iter_log),
            0);

  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_attr_exists = true;
  EXPECT_LT(H5DSset_scale(s2, "fail"), 0);
  EXPECT_LT(H5DSattach_scale(d2, s2, 0), 0);
  EXPECT_LT(H5DSdetach_scale(d1, s1, 0), 0);
  EXPECT_LT(H5DSis_attached(d1, s1, 0), 0);
  EXPECT_LT(H5DSiterate_scales(d1, 0, nullptr, RecordScaleVisitor, &iter_log),
            0);
  EXPECT_LT(H5DSset_label(d1, 0, "fail"), 0);
  std::array<char, 16> lbuf = {};
  EXPECT_LT(H5DSget_label(d1, 0, lbuf.data(), lbuf.size()), 0);
  EXPECT_LT(H5DSget_scale_name(s1, lbuf.data(), lbuf.size()), 0);
  EXPECT_LT(H5DSis_scale(s1), 0);
  EXPECT_LT(H5DSget_num_scales(d1, 0), 0);

  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_dset_get_space = true;
  EXPECT_LT(H5DSattach_scale(d2, s2, 0), 0);
  EXPECT_LT(H5DSdetach_scale(d1, s1, 0), 0);
  EXPECT_LT(H5DSis_attached(d1, s1, 0), 0);
  EXPECT_LT(H5DSiterate_scales(d1, 0, nullptr, RecordScaleVisitor, &iter_log),
            0);
  EXPECT_LT(H5DSset_label(d1, 0, "fail"), 0);
  EXPECT_LT(H5DSget_label(d1, 0, lbuf.data(), lbuf.size()), 0);
  EXPECT_LT(H5DSget_num_scales(d1, 0), 0);

  // Pre-malloc H5Aopen failures across callers.
  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_attr_open_call = 2;
  EXPECT_LT(H5DSis_attached(d1, s1, 0), 0);
  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_attr_open_call = 2;
  EXPECT_LT(H5DSattach_scale(d1, s1, 1), 0);
  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_attr_open_call = 2;
  EXPECT_LT(H5DSiterate_scales(d1, 0, nullptr, RecordScaleVisitor, &iter_log),
            0);
  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_attr_open_call = 1;
  EXPECT_LT(H5DSset_label(d1, 0, "fail"), 0);
  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_attr_open_call = 1;
  EXPECT_LT(H5DSget_label(d1, 0, lbuf.data(), lbuf.size()), 0);
  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_attr_open_call = 2;
  EXPECT_LT(H5DSget_scale_name(s1, lbuf.data(), lbuf.size()), 0);
  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_attr_open_call = 1;
  EXPECT_LT(H5DSis_scale(s1), 0);
  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_attr_open_call = 1;
  EXPECT_LT(H5DSget_num_scales(d1, 0), 0);

  // Object open / dereference / get_space / reclaim error propagation on
  // is_new_ref paths.
  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_object_open_call = 1;
  EXPECT_LT(H5DSiterate_scales(d1, 0, nullptr, RecordScaleVisitor, &iter_log),
            0);

  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_dset_close_call = 1;
  EXPECT_LT(H5DSiterate_scales(d1, 0, nullptr, RecordScaleVisitor, &iter_log),
            0);
  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_dset_close_call = 1;
  EXPECT_LT(H5DSiterate_scales(d1, 0, &idx, RecordScaleVisitor, &stop_log), 0);

  // New-ref H5DSattach_scale first H5Rcreate_object lookup failure.
  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_object_lookup_call = 1;
  EXPECT_LT(H5DSattach_scale(d2, s2, 0), 0);

  // New-ref H5DSis_attached REFERENCE_LIST H5Aread failure.
  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_attr_read_call = 2;
  EXPECT_LT(H5DSis_attached(d1, s1, 0), 0);

#if !defined(ABSL_HAVE_ADDRESS_SANITIZER) && !defined(__SANITIZE_ADDRESS__)
  // TODO(schwehr): Re-enable under ASan once upstream H5DS.c `out:` handlers
  // call `H5Rdestroy` / `H5Treclaim` on failed `is_new_ref`
  // attach/detach/is_attached cleanup; exercise them in non-ASan builds and
  // coverage runs.
  // A. H5DSattach_scale (`is_new_ref`): has_dimlist == 0 & has_reflist == 0
  //    REFERENCE_LIST write fault, plus has_dimlist > 0 and has_reflist > 0.
  {
    ScopedDataset d_nr_w(Create2dIntDataset(file, "d_nr_w", 2, 2));
    ScopedDataset s_nr_w(Create1dDoubleDataset(file, "s_nr_w", 2));
    ASSERT_GE(d_nr_w.get(), 0);
    ASSERT_GE(s_nr_w.get(), 0);
    ASSERT_GE(H5DSset_scale(s_nr_w, "s_nr_w"), 0);
    g_ds_vol_state.ResetFaults();
    g_ds_vol_state.fail_attr_write_call = 2;
    EXPECT_LT(H5DSattach_scale(d_nr_w, s_nr_w, 0), 0);
  }

  // B. H5DSdetach_scale (`is_new_ref`): DIMENSION_LIST & REFERENCE_LIST faults.
  ScopedDataset d_nd(Create2dIntDataset(file, "d_nd", 2, 2));
  ScopedDataset s_nd(Create1dDoubleDataset(file, "s_nd", 2));
  ASSERT_GE(d_nd.get(), 0);
  ASSERT_GE(s_nd.get(), 0);
  ASSERT_GE(H5DSset_scale(s_nd, "s_nd"), 0);
  g_ds_vol_state.ResetFaults();
  ASSERT_GE(H5DSattach_scale(d_nd, s_nd, 0), 0);
  for (int i = 0; i < 4; ++i) {
    g_ds_vol_state.ResetFaults();
    if (i == 0) g_ds_vol_state.fail_object_open_call = 1;
    if (i == 1) g_ds_vol_state.fail_object_get_info_call = 3;
    if (i == 2) g_ds_vol_state.fail_dset_close_call = 1;
    if (i == 3) g_ds_vol_state.fail_token_cmp_call = 2;
    EXPECT_LT(H5DSdetach_scale(d_nd, s_nd, 0), 0);
  }

  for (int i = 0; i < 8; ++i) {
    g_ds_vol_state.ResetFaults();
    const std::string dname = "d_ndl" + std::to_string(i);
    const std::string sname = "s_ndl" + std::to_string(i);
    ScopedDataset d_l(Create2dIntDataset(file, dname.c_str(), 2, 2));
    ScopedDataset s_l(Create1dDoubleDataset(file, sname.c_str(), 2));
    ScopedDataset s_keep(
        Create1dDoubleDataset(file, ("s_keep" + std::to_string(i)).c_str(), 2));
    ASSERT_GE(d_l.get(), 0);
    ASSERT_GE(s_l.get(), 0);
    ASSERT_GE(s_keep.get(), 0);
    ASSERT_GE(H5DSset_scale(s_l, sname.c_str()), 0);
    ASSERT_GE(H5DSset_scale(s_keep, "keep"), 0);
    ASSERT_GE(H5DSattach_scale(d_l, s_l, 0), 0);
    ASSERT_GE(H5DSattach_scale(d_l, s_keep, 1), 0);
    g_ds_vol_state.ResetFaults();
    if (i == 0) g_ds_vol_state.fail_attr_read_call = 2;
    if (i == 1) g_ds_vol_state.fail_object_open_call = 2;
    if (i == 2) g_ds_vol_state.fail_object_open_call = 3;
    if (i == 3) g_ds_vol_state.fail_object_get_info_call = 4;
    if (i == 4) g_ds_vol_state.fail_dset_close_call = 3;
    if (i == 5) g_ds_vol_state.fail_token_cmp_call = 3;
    if (i == 6) g_ds_vol_state.fail_attr_close_call = 2;
    if (i == 7) g_ds_vol_state.fail_attr_delete = true;
    EXPECT_LT(H5DSdetach_scale(d_l, s_l, 0), 0);
  }

  // Non-empty remaining REFERENCE_LIST (nelmts > 0) write/close faults.
  for (int i = 0; i < 3; ++i) {
    g_ds_vol_state.ResetFaults();
    ScopedDataset d_m1(
        Create2dIntDataset(file, ("d_m1_" + std::to_string(i)).c_str(), 2, 2));
    ScopedDataset d_m2(
        Create2dIntDataset(file, ("d_m2_" + std::to_string(i)).c_str(), 2, 2));
    ScopedDataset s_m(
        Create1dDoubleDataset(file, ("s_m_" + std::to_string(i)).c_str(), 2));
    ScopedDataset s_m_keep(
        Create1dDoubleDataset(file, ("s_mk_" + std::to_string(i)).c_str(), 2));
    ASSERT_GE(d_m1.get(), 0);
    ASSERT_GE(d_m2.get(), 0);
    ASSERT_GE(s_m.get(), 0);
    ASSERT_GE(s_m_keep.get(), 0);
    ASSERT_GE(H5DSset_scale(s_m, "sm"), 0);
    ASSERT_GE(H5DSset_scale(s_m_keep, "smk"), 0);
    ASSERT_GE(H5DSattach_scale(d_m1, s_m, 0), 0);
    ASSERT_GE(H5DSattach_scale(d_m1, s_m_keep, 1), 0);
    ASSERT_GE(H5DSattach_scale(d_m2, s_m, 0), 0);
    g_ds_vol_state.ResetFaults();
    if (i == 0) g_ds_vol_state.fail_attr_create_call = 1;
    if (i == 1) g_ds_vol_state.fail_attr_write_call = 2;
    if (i == 2) g_ds_vol_state.fail_attr_close_call = 3;
    EXPECT_LT(H5DSdetach_scale(d_m1, s_m, 0), 0);
  }

  // Exercise `found_dset == 0` when REFERENCE_LIST dim_idx does not match
  // the detached dimension.
  {
    ScopedDataset d_fd(Create2dIntDataset(file, "d_fd", 2, 2));
    ScopedDataset s_fd(Create1dDoubleDataset(file, "s_fd", 2));
    ASSERT_GE(d_fd.get(), 0);
    ASSERT_GE(s_fd.get(), 0);
    ASSERT_GE(H5DSset_scale(s_fd, "s_fd"), 0);
    ASSERT_GE(H5DSattach_scale(d_fd, s_fd, 0), 0);
    H5O_info2_t oinfo = {};
    ASSERT_GE(H5Oget_info3(s_fd, &oinfo, H5O_INFO_BASIC), 0);
    TokenKey key = {};
    std::memcpy(key.data(), oinfo.token.__data, H5O_MAX_TOKEN_SIZE);
    ASSERT_TRUE(g_ds_vol_state.ref_lists.contains(key));
    ASSERT_FALSE(g_ds_vol_state.ref_lists[key].empty());
    g_ds_vol_state.ref_lists[key][0].dim_idx = 1;
    g_ds_vol_state.ResetFaults();
    EXPECT_LT(H5DSdetach_scale(d_fd, s_fd, 0), 0);
  }

  // C. H5DSis_attached (`is_new_ref`): top-level and REFERENCE_LIST faults
  // (after H5Treclaim(buf), avoiding DIMENSION_LIST `out:` reclaim skip).
  for (int i = 0; i < 8; ++i) {
    g_ds_vol_state.ResetFaults();
    if (i == 0) g_ds_vol_state.fail_object_get_info_call = 1;
    if (i == 1) g_ds_vol_state.fail_object_get_info_call = 2;
    if (i == 2) g_ds_vol_state.fail_token_cmp_call = 1;
    if (i == 3) g_ds_vol_state.fail_object_open_call = 3;
    if (i == 4) g_ds_vol_state.fail_object_get_info_call = 7;
    if (i == 5) g_ds_vol_state.fail_object_get_info_call = 8;
    if (i == 6) g_ds_vol_state.fail_token_cmp_call = 4;
    if (i == 7) g_ds_vol_state.fail_dset_close_call = 3;
    EXPECT_LT(H5DSis_attached(d1, s1, 0), 0);
  }
#endif  // !defined(ABSL_HAVE_ADDRESS_SANITIZER) &&
        // !defined(__SANITIZE_ADDRESS__)
}

TEST(H5dsTest, NativeVolFaultPropagation) {
  g_ds_vol_state.Reset();
  g_ds_vol_state.report_native = true;
  const hid_t vol_id =
      H5VLregister_connector(&kDsNonNativeVolClass, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  const absl::Cleanup cleanup_vol = [vol_id] { CleanupVolConnector(vol_id); };

  ScopedFile file = CreateCoreVolFile(vol_id, "h5ds_native_fault.h5");
  ASSERT_GE(file.get(), 0);
  g_ds_vol_state.active_file_id = file.get();

  bool with_new_ref = true;
  ASSERT_GE(H5DSwith_new_ref(file, &with_new_ref), 0);
  EXPECT_FALSE(with_new_ref);

  ScopedDataset s1(Create1dDoubleDataset(file, "s1", 4));
  ScopedDataset s2(Create1dDoubleDataset(file, "s2", 4));
  ScopedDataset d1(Create2dIntDataset(file, "d1", 4, 4));
  ScopedDataset d2(Create2dIntDataset(file, "d2", 4, 4));
  ScopedDataset d_custom(Create2dIntDataset(file, "d_custom", 4, 4));
  ASSERT_GE(s1.get(), 0);
  ASSERT_GE(s2.get(), 0);
  ASSERT_GE(d1.get(), 0);
  ASSERT_GE(d2.get(), 0);
  ASSERT_GE(d_custom.get(), 0);

  ASSERT_GE(H5DSset_scale(s1, "scale_one"), 0);
  ASSERT_GE(H5DSset_scale(s2, "scale_two"), 0);
  ASSERT_GE(H5LTset_attribute_string(file, "d_custom", "CLASS", "CUSTOM"), 0);

  // Attach d1 dim 0 -> s1, s2; d2 dim 0 -> s1 (so s1 has REFERENCE_LIST).
  ASSERT_GE(H5DSattach_scale(d1, s1, 0), 0);
  ASSERT_GE(H5DSattach_scale(d1, s2, 0), 0);
  ASSERT_GE(H5DSattach_scale(d2, s1, 0), 0);
  ASSERT_GE(H5DSset_label(d1, 0, "lbl0"), 0);

  ScopedErrorSilence silence;
  std::array<char, 32> buf = {};
  IterateLog log;

  // 1. H5DSset_scale CLASS / NAME / DIMENSION_LIST errors.
  ScopedDataset s_new(Create1dDoubleDataset(file, "s_new", 4));
  ASSERT_GE(s_new.get(), 0);
  ExpectDsFault(&DsFaultVolState::fail_attr_exists_call, 1,
                [&] { return H5DSset_scale(s_new, "x"); });
  ExpectDsFault(&DsFaultVolState::fail_attr_write_call, 1,
                [&] { return H5DSset_scale(s_new, "x"); });
  ExpectDsFault(&DsFaultVolState::fail_attr_write_call, 2,
                [&] { return H5DSset_scale(s_new, "x"); });

  // 2. H5DS_is_reserved & CLASS validation fault paths.
  ExpectDsFault(&DsFaultVolState::fail_attr_exists_call, 3,
                [&] { return H5DSattach_scale(d_custom, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_open_call, 2,
                [&] { return H5DSattach_scale(d_custom, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_get_type_call, 2,
                [&] { return H5DSattach_scale(d_custom, s1, 0); });
  // CLASS="CUSTOM" is 7 bytes (!= 16), so H5DSis_scale skips H5Aread and
  // H5DS_is_reserved issues read call #1.
  ExpectDsFault(&DsFaultVolState::fail_attr_read_call, 1,
                [&] { return H5DSattach_scale(d_custom, s1, 0); });

  // 3. H5DSattach_scale param, H5Aexists, has_dimlist, and REFERENCE_LIST
  //    faults.
  EXPECT_LT(H5DSattach_scale(d2, d1, 0), 0);  // d1 already has DIMENSION_LIST.
  ExpectDsFault(&DsFaultVolState::fail_object_get_info_call, 1,
                [&] { return H5DSattach_scale(d2, s2, 0); });
  ExpectDsFault(&DsFaultVolState::fail_object_get_info_call, 2,
                [&] { return H5DSattach_scale(d2, s2, 0); });
  ExpectDsFault(&DsFaultVolState::fail_token_cmp_call, 1,
                [&] { return H5DSattach_scale(d2, s2, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_exists_call, 4,
                [&] { return H5DSattach_scale(d2, s2, 0); });

  // has_dimlist == 0: fail H5Acreate2 on DIMENSION_LIST.
  ScopedDataset d_fresh1(Create2dIntDataset(file, "d_fresh1", 4, 4));
  ASSERT_GE(d_fresh1.get(), 0);
  ExpectDsFault(&DsFaultVolState::fail_attr_create_call, 1,
                [&] { return H5DSattach_scale(d_fresh1, s1, 0); });

  // has_dimlist > 0: pre-malloc H5Aopen / get_type / get_space errors.
  ExpectDsFault(&DsFaultVolState::fail_attr_open_call, 1,
                [&] { return H5DSattach_scale(d1, s1, 1); });
  ExpectDsFault(&DsFaultVolState::fail_attr_get_type_call, 1,
                [&] { return H5DSattach_scale(d1, s1, 1); });
  ExpectDsFault(&DsFaultVolState::fail_attr_get_space_call, 1,
                [&] { return H5DSattach_scale(d1, s1, 1); });

  // has_reflist == 0: fail H5Acreate2, H5Awrite, H5Aclose on REFERENCE_LIST.
  ScopedDataset s_bare1(Create1dDoubleDataset(file, "s_bare1", 4));
  ScopedDataset s_bare2(Create1dDoubleDataset(file, "s_bare2", 4));
  ScopedDataset s_bare3(Create1dDoubleDataset(file, "s_bare3", 4));
  ScopedDataset d_att1(Create2dIntDataset(file, "d_att1", 4, 4));
  ScopedDataset d_att2(Create2dIntDataset(file, "d_att2", 4, 4));
  ScopedDataset d_att3(Create2dIntDataset(file, "d_att3", 4, 4));
  ASSERT_GE(s_bare1.get(), 0);
  ASSERT_GE(s_bare2.get(), 0);
  ASSERT_GE(s_bare3.get(), 0);
  ASSERT_GE(d_att1.get(), 0);
  ASSERT_GE(d_att2.get(), 0);
  ASSERT_GE(d_att3.get(), 0);

  ExpectDsFault(&DsFaultVolState::fail_attr_create_call, 2,
                [&] { return H5DSattach_scale(d_att1, s_bare1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_write_call, 2,
                [&] { return H5DSattach_scale(d_att2, s_bare2, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_close_call, 2,
                [&] { return H5DSattach_scale(d_att3, s_bare3, 0); });

  // Fail final H5DSis_scale / CLASS creation after REFERENCE_LIST write.
  ScopedDataset s_bare4(Create1dDoubleDataset(file, "s_bare4", 4));
  ScopedDataset s_bare5(Create1dDoubleDataset(file, "s_bare5", 4));
  ScopedDataset d_att4(Create2dIntDataset(file, "d_att4", 4, 4));
  ScopedDataset d_att5(Create2dIntDataset(file, "d_att5", 4, 4));
  ASSERT_GE(s_bare4.get(), 0);
  ASSERT_GE(s_bare5.get(), 0);
  ASSERT_GE(d_att4.get(), 0);
  ASSERT_GE(d_att5.get(), 0);
  ExpectDsFault(&DsFaultVolState::fail_attr_exists_call, 6,
                [&] { return H5DSattach_scale(d_att4, s_bare4, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_write_call, 3,
                [&] { return H5DSattach_scale(d_att5, s_bare5, 0); });

  // has_dimlist > 0, then has_reflist H5Aexists failure on d1 dim 1 -> s1.
  ExpectDsFault(&DsFaultVolState::fail_attr_exists_call, 5,
                [&] { return H5DSattach_scale(d1, s1, 1); });

  // has_dimlist == 0, has_reflist > 0 on s1: fail open, get_type, get_space,
  // read, close, delete, and recreated H5Aclose (all leak-free for old POD
  // refs).
  for (int i = 0; i < 7; ++i) {
    const std::string name = "d_hr_" + std::to_string(i);
    ScopedDataset d_hr(Create2dIntDataset(file, name.c_str(), 4, 4));
    ASSERT_GE(d_hr.get(), 0);
    g_ds_vol_state.ResetFaults();
    if (i == 0) g_ds_vol_state.fail_attr_open_call = 1;
    if (i == 1) g_ds_vol_state.fail_attr_get_type_call = 1;
    if (i == 2) g_ds_vol_state.fail_attr_get_space_call = 1;
    if (i == 3) g_ds_vol_state.fail_attr_read_call = 1;
    if (i == 4) g_ds_vol_state.fail_attr_close_call = 2;
    if (i == 5) g_ds_vol_state.fail_attr_delete = true;
    if (i == 6) g_ds_vol_state.fail_attr_close_call = 3;
    EXPECT_LT(H5DSattach_scale(d_hr, s1, 0), 0);
  }

  // Dedicated fresh pairs for has_dimlist > 0 read/close faults and
  // has_reflist > 0 post-H5Adelete create/write faults (isolating attribute
  // mutations from d1 and s1).
  {
    ScopedDataset d_dim_r(Create2dIntDataset(file, "d_dim_r", 2, 2));
    ScopedDataset s_dim_r(Create1dDoubleDataset(file, "s_dim_r", 2));
    ASSERT_GE(d_dim_r.get(), 0);
    ASSERT_GE(s_dim_r.get(), 0);
    ASSERT_GE(H5DSset_scale(s_dim_r, "s_dim_r"), 0);
    ASSERT_GE(H5DSattach_scale(d_dim_r, s_dim_r, 0), 0);
    ExpectDsFault(&DsFaultVolState::fail_attr_read_call, 1,
                  [&] { return H5DSattach_scale(d_dim_r, s_dim_r, 1); });

    ScopedDataset d_dim_c(Create2dIntDataset(file, "d_dim_c", 2, 2));
    ScopedDataset s_dim_c(Create1dDoubleDataset(file, "s_dim_c", 2));
    ASSERT_GE(d_dim_c.get(), 0);
    ASSERT_GE(s_dim_c.get(), 0);
    ASSERT_GE(H5DSset_scale(s_dim_c, "s_dim_c"), 0);
    ASSERT_GE(H5DSattach_scale(d_dim_c, s_dim_c, 0), 0);
    ExpectDsFault(&DsFaultVolState::fail_attr_close_call, 1,
                  [&] { return H5DSattach_scale(d_dim_c, s_dim_c, 1); });

    for (int i = 0; i < 2; ++i) {
      ScopedDataset d_b(Create2dIntDataset(
          file, ("d_hrb_" + std::to_string(i)).c_str(), 2, 2));
      ScopedDataset d_x(Create2dIntDataset(
          file, ("d_hrx_" + std::to_string(i)).c_str(), 2, 2));
      ScopedDataset s_x(Create1dDoubleDataset(
          file, ("s_hrx_" + std::to_string(i)).c_str(), 2));
      ASSERT_GE(d_b.get(), 0);
      ASSERT_GE(d_x.get(), 0);
      ASSERT_GE(s_x.get(), 0);
      ASSERT_GE(H5DSset_scale(s_x, "s_x"), 0);
      ASSERT_GE(H5DSattach_scale(d_b, s_x, 0), 0);
      g_ds_vol_state.ResetFaults();
      if (i == 0) g_ds_vol_state.fail_attr_create_call = 2;
      if (i == 1) g_ds_vol_state.fail_attr_write_call = 2;
      EXPECT_LT(H5DSattach_scale(d_x, s_x, 0), 0);
    }
  }

  // 4. H5DSdetach_scale fault propagation (legacy POD refs).
  ExpectDsFault(&DsFaultVolState::fail_object_get_info_call, 1,
                [&] { return H5DSdetach_scale(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_object_get_info_call, 2,
                [&] { return H5DSdetach_scale(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_token_cmp_call, 1,
                [&] { return H5DSdetach_scale(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_exists_call, 2,
                [&] { return H5DSdetach_scale(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_exists_call, 3,
                [&] { return H5DSdetach_scale(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_open_call, 1,
                [&] { return H5DSdetach_scale(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_get_type_call, 1,
                [&] { return H5DSdetach_scale(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_get_space_call, 1,
                [&] { return H5DSdetach_scale(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_object_open_call, 1,
                [&] { return H5DSdetach_scale(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_dset_close_call, 1,
                [&] { return H5DSdetach_scale(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_token_cmp_call, 2,
                [&] { return H5DSdetach_scale(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_write_call, 1,
                [&] { return H5DSdetach_scale(d1, s1, 0); });

  // H5Adelete and H5Aclose on DIMENSION_LIST faults.
  ExpectDsFault(&DsFaultVolState::fail_attr_delete, true,
                [&] { return H5DSdetach_scale(d2, s1, 0); });
  {
    ScopedDataset d_dc(Create2dIntDataset(file, "d_dc", 2, 2));
    ScopedDataset s_dc(Create1dDoubleDataset(file, "s_dc", 2));
    ASSERT_GE(d_dc.get(), 0);
    ASSERT_GE(s_dc.get(), 0);
    ASSERT_GE(H5DSset_scale(s_dc, "s_dc"), 0);
    ASSERT_GE(H5DSattach_scale(d_dc, s_dc, 0), 0);
    ExpectDsFault(&DsFaultVolState::fail_attr_close_call, 1,
                  [&] { return H5DSdetach_scale(d_dc, s_dc, 0); });
  }

  // Faults after DIMENSION_LIST updated cleanly, before dsbuf_w malloc.
  g_ds_vol_state.ResetFaults();
  ScopedDataset d_det1(Create2dIntDataset(file, "d_det1", 4, 4));
  ScopedDataset d_det2(Create2dIntDataset(file, "d_det2", 4, 4));
  ScopedDataset d_det3(Create2dIntDataset(file, "d_det3", 4, 4));
  ASSERT_GE(d_det1.get(), 0);
  ASSERT_GE(d_det2.get(), 0);
  ASSERT_GE(d_det3.get(), 0);
  ASSERT_GE(H5DSattach_scale(d_det1, s1, 0), 0);
  ASSERT_GE(H5DSattach_scale(d_det2, s1, 0), 0);
  ASSERT_GE(H5DSattach_scale(d_det3, s1, 0), 0);

  ExpectDsFault(&DsFaultVolState::fail_attr_get_type_call, 2,
                [&] { return H5DSdetach_scale(d_det1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_get_space_call, 2,
                [&] { return H5DSdetach_scale(d_det2, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_read_call, 2,
                [&] { return H5DSdetach_scale(d_det3, s1, 0); });

  // 5. H5DSis_attached faults (old POD refs, DIMENSION_LIST + REFERENCE_LIST).
  ExpectDsFault(&DsFaultVolState::fail_object_get_info_call, 1,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_object_get_info_call, 2,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_token_cmp_call, 1,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_exists_call, 2,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_open_call, 1,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_get_type_call, 1,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_get_space_call, 1,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_read_call, 1,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_close_call, 1,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_exists_call, 3,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_get_type_call, 2,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_get_space_call, 2,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_read_call, 2,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_object_open_call, 3,
                [&] { return H5DSis_attached(d1, s1, 0); });
  // Calls 1-2: top-level did/dsid; 3-6: DIMENSION_LIST [s1, s2];
  // calls 7-8: REFERENCE_LIST after H5Treclaim(buf), avoiding VL buf leak.
  ExpectDsFault(&DsFaultVolState::fail_object_get_info_call, 7,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_object_get_info_call, 8,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_dset_close_call, 3,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_token_cmp_call, 4,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_close_call, 2,
                [&] { return H5DSis_attached(d1, s1, 0); });

  // 6. H5DSiterate_scales faults (old POD refs).
  ExpectDsFault(&DsFaultVolState::fail_dset_get_space_call, 2, [&] {
    return H5DSiterate_scales(d1, 0, nullptr, RecordScaleVisitor, &log);
  });
  ExpectDsFault(&DsFaultVolState::fail_attr_exists_call, 2, [&] {
    return H5DSiterate_scales(d1, 0, nullptr, RecordScaleVisitor, &log);
  });
  ExpectDsFault(&DsFaultVolState::fail_attr_get_type_call, 2, [&] {
    return H5DSiterate_scales(d1, 0, nullptr, RecordScaleVisitor, &log);
  });
  ExpectDsFault(&DsFaultVolState::fail_attr_get_space_call, 2, [&] {
    return H5DSiterate_scales(d1, 0, nullptr, RecordScaleVisitor, &log);
  });
  ExpectDsFault(&DsFaultVolState::fail_attr_open_call, 2, [&] {
    return H5DSiterate_scales(d1, 0, nullptr, RecordScaleVisitor, &log);
  });
  ExpectDsFault(&DsFaultVolState::fail_object_open_call, 1, [&] {
    return H5DSiterate_scales(d1, 0, nullptr, RecordScaleVisitor, &log);
  });
  ExpectDsFault(&DsFaultVolState::fail_attr_close_call, 2, [&] {
    return H5DSiterate_scales(
        d1, 0, nullptr,
        +[](hid_t, unsigned, hid_t, void*) -> herr_t { return 0; }, nullptr);
  });

  // 7. H5DSset_label, H5DSget_label, H5DSget_scale_name, H5DSis_scale,
  //    H5DSget_num_scales faults.
  ScopedDataset d_lbl_new(Create2dIntDataset(file, "d_lbl_new", 2, 2));
  ASSERT_GE(d_lbl_new.get(), 0);
  ExpectDsFault(&DsFaultVolState::fail_attr_create_call, 1,
                [&] { return H5DSset_label(d_lbl_new, 0, "fail"); });
  ExpectDsFault(&DsFaultVolState::fail_attr_write_call, 1,
                [&] { return H5DSset_label(d_lbl_new, 0, "fail"); });

  ScopedDataset d_lbl_c(Create2dIntDataset(file, "d_lbl_c", 2, 2));
  ASSERT_GE(d_lbl_c.get(), 0);
  ExpectDsFault(&DsFaultVolState::fail_attr_close_call, 1,
                [&] { return H5DSset_label(d_lbl_c, 0, "fail"); });

  ScopedDataset d_lbl_1(Create2dIntDataset(file, "d_lbl_1", 2, 2));
  ASSERT_GE(d_lbl_1.get(), 0);
  ASSERT_GE(H5DSset_label(d_lbl_1, 0, "a"), 0);
  ExpectDsFault(&DsFaultVolState::fail_attr_close_call, 1,
                [&] { return H5DSset_label(d_lbl_1, 0, "b"); });

  ASSERT_GE(H5DSset_label(d1, 1, "lbl1"), 0);
  ExpectDsFault(&DsFaultVolState::fail_attr_get_type_call, 1,
                [&] { return H5DSset_label(d1, 0, "fail"); });
  ExpectDsFault(&DsFaultVolState::fail_attr_write_call, 1,
                [&] { return H5DSset_label(d1, 0, "fail"); });

  ExpectDsFault(&DsFaultVolState::fail_attr_get_type_call, 1,
                [&] { return H5DSget_label(d1, 0, buf.data(), buf.size()); });

  ExpectDsFault(&DsFaultVolState::fail_attr_exists_call, 2,
                [&] { return H5DSget_scale_name(s1, buf.data(), buf.size()); });
  ExpectDsFault(&DsFaultVolState::fail_attr_get_space_call, 1,
                [&] { return H5DSget_scale_name(s1, buf.data(), buf.size()); });
  // Call 1 = CLASS (16 bytes), Call 2 = NAME.
  ExpectDsFault(&DsFaultVolState::fail_attr_get_type_call, 2,
                [&] { return H5DSget_scale_name(s1, buf.data(), buf.size()); });
  ExpectDsFault(&DsFaultVolState::fail_attr_read_call, 2,
                [&] { return H5DSget_scale_name(s1, buf.data(), buf.size()); });
  ExpectDsFault(&DsFaultVolState::fail_attr_close_call, 2,
                [&] { return H5DSget_scale_name(s1, buf.data(), buf.size()); });

  ExpectDsFault(&DsFaultVolState::fail_attr_get_type_call, 1,
                [&] { return H5DSis_scale(s1); });
  ExpectDsFault(&DsFaultVolState::fail_attr_read_call, 1,
                [&] { return H5DSis_scale(s1); });
  g_ds_vol_state.ResetFaults();
  g_ds_vol_state.fail_attr_close_call = 1;
  // is_ds is set to 1 before H5Aclose(aid), so H5DSis_scale logs the close
  // error and still returns 1 rather than FAIL.
  EXPECT_EQ(1, H5DSis_scale(s1));

  ExpectDsFault(&DsFaultVolState::fail_attr_get_type_call, 1,
                [&] { return H5DSget_num_scales(d1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_get_space_call, 1,
                [&] { return H5DSget_num_scales(d1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_read_call, 1,
                [&] { return H5DSget_num_scales(d1, 0); });
  ExpectDsFault(&DsFaultVolState::fail_attr_close_call, 1,
                [&] { return H5DSget_num_scales(d1, 0); });

  // 8. H5Rcreate (H5VL_OBJECT_LOOKUP), null-string DIMENSION_LABELS error
  //    cleanup, and leak-free invalid/immutable H5S / H5T handle faults.
  ExpectDsFault(&DsFaultVolState::fail_object_lookup_call, 1,
                [&] { return H5DSattach_scale(d2, s2, 0); });
  ExpectDsFault(&DsFaultVolState::fail_object_lookup_call, 2,
                [&] { return H5DSattach_scale(d2, s2, 0); });

  {
    ScopedDataset d_null_lbl(Create2dIntDataset(file, "d_null_lbl", 2, 2));
    ASSERT_GE(d_null_lbl.get(), 0);
    constexpr hsize_t kLabelDims = 2;
    ScopedDataspace lspace(H5Screate_simple(1, &kLabelDims, nullptr));
    ScopedDatatype ltype(H5Tcopy(H5T_C_S1));
    ASSERT_GE(lspace.get(), 0);
    ASSERT_GE(ltype.get(), 0);
    ASSERT_GE(H5Tset_size(ltype, H5T_VARIABLE), 0);
    ScopedAttribute lattr(H5Acreate2(d_null_lbl, DIMENSION_LABELS, ltype,
                                     lspace, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_GE(lattr.get(), 0);
    const std::array<const char*, 2> null_lbls = {nullptr, nullptr};
    ASSERT_GE(H5Awrite(lattr, ltype, null_lbls.data()), 0);
    lattr.reset();
    ExpectDsFault(&DsFaultVolState::fail_attr_close_call, 1, [&] {
      return H5DSget_label(d_null_lbl, 0, buf.data(), buf.size());
    });
  }

  ExpectDsFault(&DsFaultVolState::invalid_dset_get_space_call, 1,
                [&] { return H5DSattach_scale(d2, s2, 0); });
  ExpectDsFault(&DsFaultVolState::invalid_dset_get_space_call, 1,
                [&] { return H5DSis_attached(d1, s1, 0); });
  ExpectDsFault(&DsFaultVolState::invalid_dset_get_space_call, 2, [&] {
    return H5DSiterate_scales(d1, 0, nullptr, RecordScaleVisitor, &log);
  });
  ExpectDsFault(&DsFaultVolState::invalid_dset_get_space_call, 1,
                [&] { return H5DSset_label(d1, 0, "x"); });
  ExpectDsFault(&DsFaultVolState::invalid_dset_get_space_call, 1,
                [&] { return H5DSget_label(d1, 0, buf.data(), buf.size()); });
  ExpectDsFault(&DsFaultVolState::invalid_dset_get_space_call, 1,
                [&] { return H5DSget_num_scales(d1, 0); });

  {
    ScopedDataset d_hr_t(Create2dIntDataset(file, "d_hr_t", 2, 2));
    ScopedDataset d_det_t(Create2dIntDataset(file, "d_det_t", 2, 2));
    ScopedDataset s_empty_nm(Create1dDoubleDataset(file, "s_empty_nm", 2));
    ASSERT_GE(d_hr_t.get(), 0);
    ASSERT_GE(d_det_t.get(), 0);
    ASSERT_GE(s_empty_nm.get(), 0);
    ASSERT_GE(H5DSattach_scale(d_det_t, s1, 0), 0);
    ASSERT_GE(H5DSset_scale(s_empty_nm, ""), 0);

    ExpectDsFault(&DsFaultVolState::invalid_attr_get_type_call, 1,
                  [&] { return H5DSattach_scale(d_hr_t, s1, 0); });
    ExpectDsFault(&DsFaultVolState::invalid_attr_get_type_call, 2,
                  [&] { return H5DSdetach_scale(d_det_t, s1, 0); });
    ExpectDsFault(&DsFaultVolState::invalid_attr_get_type_call, 2,
                  [&] { return H5DSis_attached(d1, s1, 0); });
    ExpectDsFault(&DsFaultVolState::invalid_attr_get_type_call, 2, [&] {
      return H5DSget_scale_name(s1, buf.data(), buf.size());
    });
    ExpectDsFault(&DsFaultVolState::invalid_attr_get_type_call, 1,
                  [&] { return H5DSis_scale(s1); });
    ExpectDsFault(&DsFaultVolState::immutable_str_attr_get_type_call, 2, [&] {
      return H5DSget_scale_name(s_empty_nm, buf.data(), buf.size());
    });
  }

  {
    ScopedDataset d_hr_s(Create2dIntDataset(file, "d_hr_s", 2, 2));
    ScopedDataset d_det_s(Create2dIntDataset(file, "d_det_s", 2, 2));
    ScopedDataset d_evl1(Create2dIntDataset(file, "d_evl1", 2, 2));
    ScopedDataset d_evl2(Create2dIntDataset(file, "d_evl2", 2, 2));
    ASSERT_GE(d_hr_s.get(), 0);
    ASSERT_GE(d_det_s.get(), 0);
    ASSERT_GE(d_evl1.get(), 0);
    ASSERT_GE(d_evl2.get(), 0);
    ASSERT_GE(H5DSattach_scale(d_det_s, s1, 0), 0);
    ASSERT_GE(H5DSattach_scale(d_evl2, s1, 0), 0);

    constexpr hsize_t kVlenDims = 2;
    ScopedDataspace vspace(H5Screate_simple(1, &kVlenDims, nullptr));
    ScopedDatatype vtype(H5Tvlen_create(H5T_STD_REF_OBJ));
    ASSERT_GE(vspace.get(), 0);
    ASSERT_GE(vtype.get(), 0);
    ScopedAttribute vattr(H5Acreate2(d_evl1, DIMENSION_LIST, vtype, vspace,
                                     H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_GE(vattr.get(), 0);
    const std::array<hvl_t, 2> empty_vl = {
        {hvl_t{0, nullptr}, hvl_t{0, nullptr}}};
    ASSERT_GE(H5Awrite(vattr, vtype, empty_vl.data()), 0);
    vattr.reset();

    ExpectDsFault(&DsFaultVolState::invalid_attr_get_space_call, 1,
                  [&] { return H5DSattach_scale(d_hr_s, s1, 0); });
    ExpectDsFault(&DsFaultVolState::invalid_attr_get_space_call, 2,
                  [&] { return H5DSdetach_scale(d_det_s, s1, 0); });
    ExpectDsFault(&DsFaultVolState::invalid_attr_get_space_call, 2,
                  [&] { return H5DSis_attached(d1, s1, 0); });
    ExpectDsFault(&DsFaultVolState::invalid_attr_get_space_call, 1, [&] {
      return H5DSget_scale_name(s1, buf.data(), buf.size());
    });

    ExpectDsFault(&DsFaultVolState::invalid_attr_get_space_call, 1,
                  [&] { return H5DSget_num_scales(d_evl1, 0); });
    ExpectDsFault(&DsFaultVolState::invalid_attr_get_space_call, 1,
                  [&] { return H5DSis_attached(d_evl1, s1, 0); });
    ExpectDsFault(&DsFaultVolState::invalid_attr_get_space_call, 2, [&] {
      return H5DSiterate_scales(
          d_evl1, 0, nullptr,
          +[](hid_t, unsigned, hid_t, void*) -> herr_t { return 0; }, nullptr);
    });
    ExpectDsFault(&DsFaultVolState::invalid_attr_get_space_call, 1,
                  [&] { return H5DSdetach_scale(d_evl2, s1, 0); });
  }
}

// TODO(schwehr): Remaining uncovered lines in H5DS.c (~9.4%) are defensive
// malloc/calloc OOM checks, HDF5 datatype/dataspace creation failures
// (`H5Screate`, `H5Tvlen_create`, `H5Tcopy`), and `H5DSis_scale`/
// `H5DS_is_reserved` secondary `out:` close branches whose trigger paths leak
// upstream HDF5 handles; under ASan builds, leaky `is_new_ref` cleanup paths
// are also skipped via the guards above.

}  // namespace
}  // namespace hdf5
