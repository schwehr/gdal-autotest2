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
// Tests HDF5 High-Level Packet Table APIs (third_party/hdf5/hl/src/H5PT.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/src/H5PT.c
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/test/test_packet.c

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5_test_util.h"
#include "third_party/hdf5/hl/src/H5PTpublic.h"
#include "third_party/hdf5/hl/src/hdf5_hl.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ScopedPacketTable = ScopedHid<H5PTclose>;

TEST(H5ptTest, OpenInitializesPacketTableTypeWhenNoTablesAreOpen) {
  // Ensure the packet-table ID type starts uninitialized when a table is
  // created, closed completely (bringing H5PT_ptable_count to 0), and then
  // reopened via H5PTopen().
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  {
    ScopedPacketTable created(
        H5PTcreate(file, "pt_reopen_first", H5T_NATIVE_INT32, 4, H5P_DEFAULT));
    ASSERT_GE(created.get(), 0);
    constexpr int32_t kValue = 42;
    ASSERT_GE(H5PTappend(created, 1, &kValue), 0);
  }

  ScopedPacketTable opened(H5PTopen(file, "pt_reopen_first"));
  ASSERT_GE(opened.get(), 0);
  int32_t read_back = 0;
  ASSERT_GE(H5PTread_packets(opened, 0, 1, &read_back), 0);
  EXPECT_EQ(42, read_back);
}

TEST(H5ptTest, FixedLengthLifecycleAppendAndIndexedReads) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  // 1. Create an uncompressed table with H5P_DEFAULT and a compressed table
  //    with a custom DCPL so both plist paths stay covered.
  ScopedPacketTable table_default(
      H5PTcreate(file, "pt_default", H5T_NATIVE_INT32, 4, H5P_DEFAULT));
  ASSERT_GE(table_default.get(), 0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(dcpl.get(), 0);
  ASSERT_GE(H5Pset_deflate(dcpl, 4), 0);
  ScopedPacketTable table_custom(
      H5PTcreate(file, "pt_custom", H5T_NATIVE_INT32, 4, dcpl));
  ASSERT_GE(table_custom.get(), 0);

  EXPECT_GE(H5PTis_valid(table_default), 0);
  EXPECT_EQ(0, H5PTis_varlen(table_default));

  // Borrowed handles from H5PTget_dataset/H5PTget_type must not be closed.
  const hid_t backend_dset = H5PTget_dataset(table_default);
  const hid_t backend_type = H5PTget_type(table_default);
  EXPECT_GE(backend_dset, 0);
  EXPECT_GE(backend_type, 0);
  EXPECT_EQ(sizeof(int32_t), H5Tget_size(backend_type));

  // 2. Append 0 records (early no-op), then append 5 packets and verify count.
  constexpr std::array<int32_t, 5> kPackets = {10, 20, 30, 40, 50};
  EXPECT_GE(H5PTappend(table_default, 0, nullptr), 0);
  ASSERT_GE(H5PTappend(table_default, kPackets.size(), kPackets.data()), 0);

  hsize_t num_packets = 0;
  EXPECT_GE(H5PTget_num_packets(table_default, &num_packets), 0);
  EXPECT_EQ(5u, num_packets);
  EXPECT_GE(H5PTget_num_packets(table_default, nullptr), 0);

  // 3. Reopen the table in parallel with table_custom and test sequential +
  //    random-access packet reads.
  table_default.reset();
  ScopedPacketTable reopened(H5PTopen(file, "pt_default"));
  ASSERT_GE(reopened.get(), 0);

  hsize_t index = 999;
  EXPECT_GE(H5PTget_index(reopened, &index), 0);
  EXPECT_EQ(0u, index);
  EXPECT_GE(H5PTget_index(reopened, nullptr), 0);

  std::array<int32_t, 2> next_buf = {-999, -999};
  EXPECT_GE(H5PTget_next(reopened, 0, next_buf.data()), 0);
  ASSERT_GE(H5PTget_next(reopened, 2, next_buf.data()), 0);
  EXPECT_THAT(next_buf, ElementsAre(10, 20));

  EXPECT_GE(H5PTget_index(reopened, &index), 0);
  EXPECT_EQ(2u, index);

  ASSERT_GE(H5PTset_index(reopened, 3), 0);
  ASSERT_GE(H5PTget_next(reopened, 2, next_buf.data()), 0);
  EXPECT_THAT(next_buf, ElementsAre(40, 50));

  ASSERT_GE(H5PTcreate_index(reopened), 0);
  EXPECT_GE(H5PTget_index(reopened, &index), 0);
  EXPECT_EQ(0u, index);

  std::array<int32_t, 3> slice_buf = {-999, -999, -999};
  EXPECT_GE(H5PTread_packets(reopened, 0, 0, slice_buf.data()), 0);
  ASSERT_GE(H5PTread_packets(reopened, 1, 3, slice_buf.data()), 0);
  EXPECT_THAT(slice_buf, ElementsAre(20, 30, 40));

  // Closing one table while another remains open exercises H5PT_ptable_count
  // > 1; closing the second brings H5PT_ptable_count back to 0.
  reopened.reset();
  table_custom.reset();
}

TEST(H5ptTest, DeprecatedCreateFlUncompressedAndCompressed) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  ScopedPacketTable uncompressed(
      H5PTcreate_fl(file, "fl_plain", H5T_NATIVE_DOUBLE, 8, -1));
  ASSERT_GE(uncompressed.get(), 0);

  ScopedPacketTable compressed(
      H5PTcreate_fl(file, "fl_deflate", H5T_NATIVE_DOUBLE, 8, 5));
  ASSERT_GE(compressed.get(), 0);

  constexpr std::array<double, 3> kValues = {1.25, 2.5, 3.75};
  ASSERT_GE(H5PTappend(compressed, kValues.size(), kValues.data()), 0);

  std::array<double, 3> read_back = {0.0, 0.0, 0.0};
  ASSERT_GE(H5PTread_packets(compressed, 0, read_back.size(), read_back.data()),
            0);
  EXPECT_THAT(read_back, ElementsAre(1.25, 2.5, 3.75));
}

TEST(H5ptTest, VariableLengthTableAppendReadAndReclaim) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  ScopedDatatype vlen_tid(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_GE(vlen_tid.get(), 0);

  ScopedPacketTable vlen_table(
      H5PTcreate(file, "pt_vlen", vlen_tid, 4, H5P_DEFAULT));
  ASSERT_GE(vlen_table.get(), 0);
  EXPECT_EQ(1, H5PTis_varlen(vlen_table));

  std::array<int32_t, 2> rec0 = {100, 101};
  std::array<int32_t, 3> rec1 = {200, 201, 202};
  std::array<hvl_t, 2> write_recs = {{
      {.len = rec0.size(), .p = rec0.data()},
      {.len = rec1.size(), .p = rec1.data()},
  }};
  ASSERT_GE(H5PTappend(vlen_table, write_recs.size(), write_recs.data()), 0);

  std::array<hvl_t, 2> read_recs = {};
  ASSERT_GE(H5PTget_next(vlen_table, read_recs.size(), read_recs.data()), 0);
  ASSERT_EQ(2u, read_recs[0].len);
  ASSERT_EQ(3u, read_recs[1].len);

  const auto* p0 = static_cast<const int32_t*>(read_recs[0].p);
  const auto* p1 = static_cast<const int32_t*>(read_recs[1].p);
  EXPECT_THAT(std::vector<int32_t>(p0, p0 + 2), ElementsAre(100, 101));
  EXPECT_THAT(std::vector<int32_t>(p1, p1 + 3), ElementsAre(200, 201, 202));
  EXPECT_GE(H5PTfree_vlen_buff(vlen_table, read_recs.size(), read_recs.data()),
            0);
}

TEST(H5ptTest, ErrorPathsAndInvalidArguments) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  ScopedPacketTable table(
      H5PTcreate(file, "pt_valid", H5T_NATIVE_INT32, 4, H5P_DEFAULT));
  ASSERT_GE(table.get(), 0);
  constexpr int32_t kVal = 7;
  ASSERT_GE(H5PTappend(table, 1, &kVal), 0);

  ScopedErrorSilence silence;

  // Create/open failures.
  EXPECT_LT(H5PTcreate(file, nullptr, H5T_NATIVE_INT32, 4, H5P_DEFAULT), 0);
  EXPECT_LT(H5PTcreate(file, "zero_chunk", H5T_NATIVE_INT32, 0, H5P_DEFAULT),
            0);
  EXPECT_LT(H5PTcreate(file, "pt_valid", H5T_NATIVE_INT32, 4, H5P_DEFAULT), 0);
  EXPECT_LT(
      H5PTcreate(H5I_INVALID_HID, "bad_loc", H5T_NATIVE_INT32, 4, H5P_DEFAULT),
      0);
  EXPECT_LT(H5PTcreate(file, "bad_type", H5I_INVALID_HID, 4, H5P_DEFAULT), 0);

  // Wrong property-list class causes H5Pset_chunk to fail inside H5PTcreate.
  ScopedPlist wrong_plist(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_GE(wrong_plist.get(), 0);
  EXPECT_LT(H5PTcreate(file, "wrong_plist", H5T_NATIVE_INT32, 4, wrong_plist),
            0);

  EXPECT_LT(H5PTcreate_fl(file, nullptr, H5T_NATIVE_INT32, 4, -1), 0);
  EXPECT_LT(H5PTcreate_fl(file, "fl_zero_chunk", H5T_NATIVE_INT32, 0, -1), 0);
  EXPECT_LT(H5PTcreate_fl(file, "pt_valid", H5T_NATIVE_INT32, 4, -1), 0);

  EXPECT_LT(H5PTopen(file, nullptr), 0);
  EXPECT_LT(H5PTopen(file, "does_not_exist"), 0);

  // Operations on out-of-range bounds or invalid packet table IDs.
  std::array<int32_t, 2> out_buf = {};
  hsize_t dummy = 0;
  EXPECT_LT(H5PTset_index(table, 1), 0);
  EXPECT_LT(H5PTread_packets(table, 1, 1, out_buf.data()), 0);
  EXPECT_LT(H5PTget_next(table, 2, out_buf.data()), 0);
  // H5TB_common_append_records extends the dataset before H5Dwrite rejects
  // nullptr `buf`.
  EXPECT_LT(H5PTappend(table, 1, nullptr), 0);
  EXPECT_LT(
      H5PTfree_vlen_buff(table, static_cast<size_t>(H5S_UNLIMITED), nullptr),
      0);
  EXPECT_LT(H5PTfree_vlen_buff(table, 1, nullptr), 0);

  EXPECT_LT(H5PTclose(H5I_INVALID_HID), 0);
  EXPECT_LT(H5PTappend(H5I_INVALID_HID, 1, &kVal), 0);
  EXPECT_LT(H5PTget_next(H5I_INVALID_HID, 1, out_buf.data()), 0);
  EXPECT_LT(H5PTread_packets(H5I_INVALID_HID, 0, 1, out_buf.data()), 0);
  EXPECT_LT(H5PTcreate_index(H5I_INVALID_HID), 0);
  EXPECT_LT(H5PTset_index(H5I_INVALID_HID, 0), 0);
  EXPECT_LT(H5PTget_index(H5I_INVALID_HID, &dummy), 0);
  EXPECT_LT(H5PTget_num_packets(H5I_INVALID_HID, &dummy), 0);
  EXPECT_LT(H5PTis_valid(H5I_INVALID_HID), 0);
  EXPECT_LT(H5PTis_varlen(H5I_INVALID_HID), 0);
  EXPECT_LT(H5PTfree_vlen_buff(H5I_INVALID_HID, 0, nullptr), 0);
  EXPECT_LT(H5PTget_dataset(H5I_INVALID_HID), 0);
  EXPECT_LT(H5PTget_type(H5I_INVALID_HID), 0);
}

// Pass-through VOL connector delegating to H5VL_NATIVE for injecting
// dataset_cls.get failures during H5PTopen().
struct FaultVolState {
  bool close_space_on_create = false;
  bool close_dcpl_on_create = false;
  bool close_type_on_create = false;
  bool fail_get_type = false;
  bool bad_get_type = false;
  bool immutable_get_type = false;
  bool fail_get_space = false;
  bool bad_get_space = false;

  void Reset() { *this = FaultVolState{}; }
};

FaultVolState g_fault_vol_state;

void* FaultFileCreate(const char* name, unsigned flags, hid_t fcpl_id,
                      hid_t fapl_id, hid_t dxpl_id, void** req) {
  ScopedPlist under_fapl(H5Pcopy(fapl_id));
  if (!under_fapl.valid() || H5Pset_vol(under_fapl, H5VL_NATIVE, nullptr) < 0) {
    return nullptr;
  }
  return H5VLfile_create(name, flags, fcpl_id, under_fapl, dxpl_id, req);
}

herr_t FaultFileOptional(void* file, H5VL_optional_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLfile_optional(file, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultFileGet(void* file, H5VL_file_get_args_t* args, hid_t dxpl_id,
                    void** req) {
  return H5VLfile_get(file, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultFileClose(void* file, hid_t dxpl_id, void** req) {
  return H5VLfile_close(file, H5VL_NATIVE, dxpl_id, req);
}

void* FaultDatasetCreate(void* obj, const H5VL_loc_params_t* loc_params,
                         const char* name, hid_t lcpl_id, hid_t type_id,
                         hid_t space_id, hid_t dcpl_id, hid_t dapl_id,
                         hid_t dxpl_id, void** req) {
  void* dset =
      H5VLdataset_create(obj, loc_params, H5VL_NATIVE, name, lcpl_id, type_id,
                         space_id, dcpl_id, dapl_id, dxpl_id, req);
  if (dset != nullptr) {
    // Closing caller-owned space_id/dcpl_id/type_id here relies on
    // H5D__create_api_common not dereferencing them after the VOL callback
    // returns so H5PTcreate/H5PTcreate_fl cleanup failure paths trigger safely.
    if (g_fault_vol_state.close_space_on_create) {
      H5Sclose(space_id);
    }
    if (g_fault_vol_state.close_dcpl_on_create) {
      H5Pclose(dcpl_id);
    }
    if (g_fault_vol_state.close_type_on_create) {
      H5Tclose(type_id);
    }
  }
  return dset;
}

void* FaultDatasetOpen(void* obj, const H5VL_loc_params_t* loc_params,
                       const char* name, hid_t dapl_id, hid_t dxpl_id,
                       void** req) {
  return H5VLdataset_open(obj, loc_params, H5VL_NATIVE, name, dapl_id, dxpl_id,
                          req);
}

herr_t FaultDatasetGet(void* dset, H5VL_dataset_get_args_t* args, hid_t dxpl_id,
                       void** req) {
  if (args->op_type == H5VL_DATASET_GET_TYPE) {
    if (g_fault_vol_state.fail_get_type) {
      return -1;
    }
    if (g_fault_vol_state.bad_get_type) {
      args->args.get_type.type_id =
          H5P_DEFAULT;  // 0 is never a valid object ID
      return 0;
    }
    if (g_fault_vol_state.immutable_get_type) {
      args->args.get_type.type_id = H5T_NATIVE_INT32;
      return 0;
    }
  } else if (args->op_type == H5VL_DATASET_GET_SPACE) {
    if (g_fault_vol_state.fail_get_space) {
      return -1;
    }
    if (g_fault_vol_state.bad_get_space) {
      args->args.get_space.space_id = H5S_ALL;  // 0 is never a valid object ID
      return 0;
    }
  }
  return H5VLdataset_get(dset, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultDatasetOptional(void* obj, H5VL_optional_args_t* args,
                            hid_t dxpl_id, void** req) {
  return H5VLdataset_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t FaultDatasetClose(void* dset, hid_t dxpl_id, void** req) {
  return H5VLdataset_close(dset, H5VL_NATIVE, dxpl_id, req);
}

extern const H5VL_class_t kFaultVolClass;

herr_t FaultIntrospectGetConnCls(void* obj, H5VL_get_conn_lvl_t lvl,
                                 const H5VL_class_t** conn_cls) {
  if (lvl == H5VL_GET_CONN_LVL_CURR) {
    *conn_cls = &kFaultVolClass;
    return 0;
  }
  return H5VLintrospect_get_conn_cls(obj, H5VL_NATIVE, lvl, conn_cls);
}

herr_t FaultIntrospectGetCapFlags(const void* /*info*/, uint64_t* cap_flags) {
  return H5VLintrospect_get_cap_flags(nullptr, H5VL_NATIVE, cap_flags);
}

herr_t FaultIntrospectOptQuery(void* obj, H5VL_subclass_t cls, int opt_type,
                               uint64_t* flags) {
  return H5VLintrospect_opt_query(obj, H5VL_NATIVE, cls, opt_type, flags);
}

const H5VL_class_t kFaultVolClass = {
    .version = H5VL_VERSION,
    .value = static_cast<H5VL_class_value_t>(51003),
    .name = "h5pt_fault_vol",
    .conn_version = 1,
    .cap_flags = 0,
    .initialize = nullptr,
    .terminate = nullptr,
    .info_cls = {},
    .wrap_cls = {},
    .attr_cls = {},
    .dataset_cls =
        {
            .create = FaultDatasetCreate,
            .open = FaultDatasetOpen,
            .read = nullptr,
            .write = nullptr,
            .get = FaultDatasetGet,
            .specific = nullptr,
            .optional = FaultDatasetOptional,
            .close = FaultDatasetClose,
        },
    .datatype_cls = {},
    .file_cls =
        {
            .create = FaultFileCreate,
            .open = nullptr,
            .get = FaultFileGet,
            .specific = nullptr,
            .optional = FaultFileOptional,
            .close = FaultFileClose,
        },
    .group_cls = {},
    .link_cls = {},
    .object_cls = {},
    .introspect_cls =
        {
            .get_conn_cls = FaultIntrospectGetConnCls,
            .get_cap_flags = FaultIntrospectGetCapFlags,
            .opt_query = FaultIntrospectOptQuery,
        },
    .request_cls = {},
    .blob_cls = {},
    .token_cls = {},
    .optional = nullptr,
};

TEST(H5ptTest, OpenHandlesInternalDatasetFailuresCleanly) {
  g_fault_vol_state.Reset();
  const hid_t vol_id = H5VLregister_connector(&kFaultVolClass, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  const absl::Cleanup cleanup_vol = [vol_id] {
    g_fault_vol_state.Reset();
    H5VLunregister_connector(vol_id);
  };

  ScopedFile file = CreateCoreVolFile(vol_id, "h5pt_fault_vol.h5");
  ASSERT_GE(file.get(), 0);

  {
    ScopedPacketTable created(
        H5PTcreate(file, "pt_fault", H5T_NATIVE_INT32, 4, H5P_DEFAULT));
    ASSERT_GE(created.get(), 0);
  }

  ScopedErrorSilence silence;

  // Failing H5PTcreate after dataset creation via prematurely closed handles.
  g_fault_vol_state.Reset();
  g_fault_vol_state.close_space_on_create = true;
  EXPECT_LT(H5PTcreate(file, "pt_fail_space", H5T_NATIVE_INT32, 4, H5P_DEFAULT),
            0);

  g_fault_vol_state.Reset();
  g_fault_vol_state.close_dcpl_on_create = true;
  EXPECT_LT(H5PTcreate(file, "pt_fail_dcpl", H5T_NATIVE_INT32, 4, H5P_DEFAULT),
            0);

  {
    g_fault_vol_state.Reset();
    g_fault_vol_state.close_type_on_create = true;
    const hid_t transient_tid = H5Tcopy(H5T_NATIVE_INT32);
    ASSERT_GE(transient_tid, 0);
    const absl::Cleanup close_tid = [transient_tid] {
      if (H5Iis_valid(transient_tid) > 0) {
        H5Tclose(transient_tid);
      }
    };
    EXPECT_LT(H5PTcreate(file, "pt_fail_type", transient_tid, 4, H5P_DEFAULT),
              0);
  }

  // Same post-dataset cleanup failures in H5PTcreate_fl.
  g_fault_vol_state.Reset();
  g_fault_vol_state.close_space_on_create = true;
  EXPECT_LT(H5PTcreate_fl(file, "fl_fail_space", H5T_NATIVE_INT32, 4, -1), 0);

  g_fault_vol_state.Reset();
  g_fault_vol_state.close_dcpl_on_create = true;
  EXPECT_LT(H5PTcreate_fl(file, "fl_fail_dcpl", H5T_NATIVE_INT32, 4, -1), 0);

  {
    g_fault_vol_state.Reset();
    g_fault_vol_state.close_type_on_create = true;
    const hid_t transient_tid = H5Tcopy(H5T_NATIVE_INT32);
    ASSERT_GE(transient_tid, 0);
    const absl::Cleanup close_tid = [transient_tid] {
      if (H5Iis_valid(transient_tid) > 0) {
        H5Tclose(transient_tid);
      }
    };
    EXPECT_LT(H5PTcreate_fl(file, "fl_fail_type", transient_tid, 4, -1), 0);
  }

  // Failing H5PTopen during type/space inspection.
  g_fault_vol_state.Reset();
  g_fault_vol_state.fail_get_type = true;
  EXPECT_LT(H5PTopen(file, "pt_fault"), 0);

  g_fault_vol_state.Reset();
  g_fault_vol_state.bad_get_type = true;
  EXPECT_LT(H5PTopen(file, "pt_fault"), 0);

  g_fault_vol_state.Reset();
  g_fault_vol_state.immutable_get_type = true;
  EXPECT_LT(H5PTopen(file, "pt_fault"), 0);

  g_fault_vol_state.Reset();
  g_fault_vol_state.fail_get_space = true;
  EXPECT_LT(H5PTopen(file, "pt_fault"), 0);

  g_fault_vol_state.Reset();
  g_fault_vol_state.bad_get_space = true;
  EXPECT_LT(H5PTopen(file, "pt_fault"), 0);

  // Open and close one valid table so H5PT_ptable_count drops back to 0 and
  // H5PT_ptable_id_type (which failing H5PTopen may have registered) is freed;
  // also exercise H5PT_free_id via H5Iregister + H5Idec_ref.
  g_fault_vol_state.Reset();
  ScopedPacketTable cleanup_open(H5PTopen(file, "pt_fault"));
  ASSERT_GE(cleanup_open.get(), 0);

  void* dummy_obj = std::malloc(1);
  ASSERT_NE(dummy_obj, nullptr);
  const hid_t extra_id = H5Iregister(H5Iget_type(cleanup_open), dummy_obj);
  if (extra_id < 0) {
    std::free(dummy_obj);
    FAIL() << "Failed to register auxiliary packet-table ID";
  }
  EXPECT_EQ(0, H5Idec_ref(extra_id));
}

}  // namespace
}  // namespace hdf5

// TODO(schwehr): Remaining uncovered lines in H5PT.c are H5Iregister_type2
// initialization failure and mid-call malloc OOM guards.
