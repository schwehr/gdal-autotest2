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
// Tests deprecated HDF5 1.8/1.10 object APIs (H5Oopen_by_addr, H5Oget_info1/2,
// H5Oget_info_by_name1/2, H5Oget_info_by_idx1/2, H5Ovisit1/2,
// H5Ovisit_by_name1/2) in third_party/hdf5/src/H5Odeprec.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Odeprec.c

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

constexpr H5VL_class_value_t kNonNativeVolValue = 329;
constexpr char kNonNativeVolName[] = "h5odeprec_nonnative_mock_vol";
constexpr unsigned kInvalidInfoFields = 0x8000U;

struct MockNonNativeFile {
  int dummy = 1;
};

void* MockFileCreate(const char* /*name*/, unsigned /*flags*/,
                     hid_t /*fcpl_id*/, hid_t /*fapl_id*/, hid_t /*dxpl_id*/,
                     void** req) {
  if (req != nullptr) {
    *req = nullptr;
  }
  return new MockNonNativeFile{};
}

herr_t MockFileClose(void* file, hid_t /*dxpl_id*/, void** req) {
  if (req != nullptr) {
    *req = nullptr;
  }
  delete static_cast<MockNonNativeFile*>(file);
  return 0;
}

H5VL_class_t g_non_native_cls{};
bool g_fail_get_conn_cls = false;

herr_t MockGetConnCls(void* /*obj*/, H5VL_get_conn_lvl_t /*lvl*/,
                      const H5VL_class_t** conn_cls) {
  if (g_fail_get_conn_cls) {
    return -1;
  }
  *conn_cls = &g_non_native_cls;
  return 0;
}

herr_t MockOptQuery(void* /*obj*/, H5VL_subclass_t /*subcls*/, int /*opt_type*/,
                    uint64_t* flags) {
  if (flags != nullptr) {
    *flags = 0;
  }
  return 0;
}

class ScopedNonNativeVol {
 public:
  ScopedNonNativeVol() {
    g_fail_get_conn_cls = false;
    g_non_native_cls = {};
    g_non_native_cls.version = H5VL_VERSION;
    g_non_native_cls.value = kNonNativeVolValue;
    g_non_native_cls.name = kNonNativeVolName;
    g_non_native_cls.file_cls.create = MockFileCreate;
    g_non_native_cls.file_cls.close = MockFileClose;
    g_non_native_cls.introspect_cls.get_conn_cls = MockGetConnCls;
    g_non_native_cls.introspect_cls.opt_query = MockOptQuery;
    vol_id_ = H5VLregister_connector(&g_non_native_cls, H5P_DEFAULT);
  }

  ~ScopedNonNativeVol() {
    if (vol_id_ >= 0) {
      H5VLunregister_connector(vol_id_);
    }
    g_fail_get_conn_cls = false;
  }

  ScopedNonNativeVol(const ScopedNonNativeVol&) = delete;
  ScopedNonNativeVol& operator=(const ScopedNonNativeVol&) = delete;

  hid_t id() const { return vol_id_; }

 private:
  hid_t vol_id_ = H5I_INVALID_HID;
};

struct VisitRecord {
  std::string name;
  H5O_type_t type = H5O_TYPE_UNKNOWN;
  haddr_t addr = HADDR_UNDEF;
  unsigned rc = 0;
  hsize_t num_attrs = 0;
  unsigned hdr_nmesgs = 0;
  hsize_t meta_obj_index = 0;
};

struct VisitContext {
  std::vector<VisitRecord> records;
  int stop_after = -1;
  herr_t stop_code = 0;
};

herr_t VisitCallback(hid_t /*obj*/, const char* name, const H5O_info1_t* info,
                     void* op_data) {
  auto* ctx = static_cast<VisitContext*>(op_data);
  VisitRecord rec;
  rec.name = name != nullptr ? name : "";
  rec.type = info->type;
  rec.addr = info->addr;
  rec.rc = info->rc;
  rec.num_attrs = info->num_attrs;
  rec.hdr_nmesgs = info->hdr.nmesgs;
  rec.meta_obj_index = info->meta_size.obj.index_size;
  ctx->records.push_back(rec);
  if (ctx->stop_after >= 0 &&
      static_cast<int>(ctx->records.size()) >= ctx->stop_after) {
    return ctx->stop_code;
  }
  return 0;
}

// Helper creating an in-memory file with tracked creation order group,
// dataset with attribute, committed datatype, and a hard link alias.
ScopedFile CreateSampleHierarchy(const char* name) {
  ScopedFile file = CreateInMemoryFile(name);
  if (!file.valid()) {
    return file;
  }

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  if (!gcpl.valid() ||
      H5Pset_link_creation_order(
          gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED) < 0) {
    return ScopedFile();
  }

  ScopedGroup sub(H5Gcreate2(file.get(), "/alpha", H5P_DEFAULT, gcpl.get(),
                             H5P_DEFAULT));
  if (!sub.valid()) {
    return ScopedFile();
  }

  const hsize_t dims[1] = {4};
  ScopedSpace space(H5Screate_simple(1, dims, nullptr));
  if (!space.valid()) {
    return ScopedFile();
  }
  ScopedDataset dset(H5Dcreate2(sub.get(), "dset", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  if (!dset.valid()) {
    return ScopedFile();
  }
  const int32_t data[4] = {10, 20, 30, 40};
  if (H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
               data) < 0) {
    return ScopedFile();
  }

  ScopedSpace aspace(H5Screate(H5S_SCALAR));
  if (!aspace.valid()) {
    return ScopedFile();
  }
  ScopedAttr attr(H5Acreate2(dset.get(), "tag", H5T_NATIVE_INT32, aspace.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  if (!attr.valid()) {
    return ScopedFile();
  }
  int32_t attr_val = 7;
  if (H5Awrite(attr.get(), H5T_NATIVE_INT32, &attr_val) < 0) {
    return ScopedFile();
  }

  ScopedType dtype(H5Tcopy(H5T_NATIVE_DOUBLE));
  if (!dtype.valid() ||
      H5Tcommit2(sub.get(), "dtype", dtype.get(), H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT) < 0) {
    return ScopedFile();
  }

  if (H5Lcreate_hard(sub.get(), "dset", file.get(), "/dset_alias", H5P_DEFAULT,
                     H5P_DEFAULT) < 0) {
    return ScopedFile();
  }

  return file;
}

TEST(H5OdeprecTest, OpenByAddrAndGetInfo1Variants) {
  ScopedFile file = CreateSampleHierarchy("odeprec_info1.h5");
  ASSERT_TRUE(file.valid());

  H5O_info1_t dset_info = {};
  ASSERT_GE(
      H5Oget_info_by_name1(file.get(), "/alpha/dset", &dset_info, H5P_DEFAULT),
      0);
  EXPECT_EQ(dset_info.type, H5O_TYPE_DATASET);
  EXPECT_EQ(dset_info.rc, 2U);
  EXPECT_EQ(dset_info.num_attrs, 1U);
  EXPECT_NE(dset_info.addr, HADDR_UNDEF);
  EXPECT_GT(dset_info.hdr.nmesgs, 0U);

  // H5Oopen_by_addr opens the dataset directly and reads its contents.
  ScopedObject opened_dset(H5Oopen_by_addr(file.get(), dset_info.addr));
  ASSERT_TRUE(opened_dset.valid());
  EXPECT_EQ(H5Iget_type(opened_dset.get()), H5I_DATASET);
  int32_t read_buf[4] = {};
  ASSERT_GE(H5Dread(opened_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, read_buf),
            0);
  EXPECT_THAT(read_buf, ElementsAre(10, 20, 30, 40));

  H5O_info1_t by_id = {};
  ASSERT_GE(H5Oget_info1(opened_dset.get(), &by_id), 0);
  EXPECT_EQ(by_id.addr, dset_info.addr);
  EXPECT_EQ(by_id.type, H5O_TYPE_DATASET);
  EXPECT_EQ(by_id.num_attrs, 1U);

  // Open group and committed datatype via H5Oopen_by_addr + get_info_by_idx1.
  H5O_info1_t grp_info = {};
  ASSERT_GE(H5Oget_info_by_idx1(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &grp_info, H5P_DEFAULT),
            0);
  EXPECT_EQ(grp_info.type, H5O_TYPE_GROUP);
  ScopedObject opened_grp(H5Oopen_by_addr(opened_dset.get(), grp_info.addr));
  ASSERT_TRUE(opened_grp.valid());
  EXPECT_EQ(H5Iget_type(opened_grp.get()), H5I_GROUP);

  H5O_info1_t dt_info = {};
  ASSERT_GE(H5Oget_info_by_idx1(file.get(), "/alpha", H5_INDEX_CRT_ORDER,
                                H5_ITER_DEC, 0, &dt_info, H5P_DEFAULT),
            0);
  EXPECT_EQ(dt_info.type, H5O_TYPE_NAMED_DATATYPE);
  ScopedObject opened_dt(H5Oopen_by_addr(file.get(), dt_info.addr));
  ASSERT_TRUE(opened_dt.valid());
  EXPECT_EQ(H5Iget_type(opened_dt.get()), H5I_DATATYPE);
}

TEST(H5OdeprecTest, GetInfo2RespectsFieldBitmasksAcrossAllEntryPoints) {
  ScopedFile file = CreateSampleHierarchy("odeprec_info2.h5");
  ASSERT_TRUE(file.valid());

  ScopedDataset dset(H5Dopen2(file.get(), "/alpha/dset", H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  // 1. fields == 0 leaves H5O_info1_t reset.
  H5O_info1_t zero_info = {};
  zero_info.num_attrs = 99;
  ASSERT_GE(H5Oget_info2(dset.get(), &zero_info, 0), 0);
  EXPECT_EQ(zero_info.type, H5O_TYPE_UNKNOWN);
  EXPECT_EQ(zero_info.addr, HADDR_UNDEF);
  EXPECT_EQ(zero_info.num_attrs, 0U);

  // 2. H5O_INFO_BASIC populates fileno/addr/type/rc without attr count or hdr.
  H5O_info1_t basic_info = {};
  ASSERT_GE(H5Oget_info2(dset.get(), &basic_info, H5O_INFO_BASIC), 0);
  EXPECT_EQ(basic_info.type, H5O_TYPE_DATASET);
  EXPECT_NE(basic_info.addr, HADDR_UNDEF);
  EXPECT_EQ(basic_info.rc, 2U);
  EXPECT_EQ(basic_info.num_attrs, 0U);
  EXPECT_EQ(basic_info.hdr.nmesgs, 0U);

  // 3. Individual native/data-model subsets via by_name2 and by_idx2.
  H5O_info1_t time_info = {};
  ASSERT_GE(H5Oget_info_by_name2(file.get(), "/alpha/dset", &time_info,
                                 H5O_INFO_TIME, H5P_DEFAULT),
            0);
  EXPECT_EQ(time_info.type, H5O_TYPE_UNKNOWN);
  EXPECT_GT(time_info.mtime, 0);

  H5O_info1_t nattr_info = {};
  ASSERT_GE(H5Oget_info_by_name2(file.get(), "/alpha/dset", &nattr_info,
                                 H5O_INFO_NUM_ATTRS, H5P_DEFAULT),
            0);
  EXPECT_EQ(nattr_info.num_attrs, 1U);

  H5O_info1_t hdr_info = {};
  ASSERT_GE(H5Oget_info_by_idx2(file.get(), "/alpha", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &hdr_info, H5O_INFO_HDR,
                                H5P_DEFAULT),
            0);
  EXPECT_EQ(hdr_info.type, H5O_TYPE_UNKNOWN);
  EXPECT_GT(hdr_info.hdr.nmesgs, 0U);

  H5O_info1_t meta_info = {};
  ASSERT_GE(H5Oget_info_by_idx2(file.get(), "/alpha", H5_INDEX_CRT_ORDER,
                                H5_ITER_INC, 1, &meta_info, H5O_INFO_META_SIZE,
                                H5P_DEFAULT),
            0);
  EXPECT_EQ(meta_info.type, H5O_TYPE_UNKNOWN);

  H5O_info1_t all_info = {};
  ASSERT_GE(H5Oget_info2(dset.get(), &all_info, H5O_INFO_ALL), 0);
  EXPECT_EQ(all_info.type, H5O_TYPE_DATASET);
  EXPECT_EQ(all_info.num_attrs, 1U);
  EXPECT_GT(all_info.hdr.nmesgs, 0U);
}

TEST(H5OdeprecTest, Visit1AndVisit2TraverseStopFailAndApplyFieldMasks) {
  ScopedFile file = CreateSampleHierarchy("odeprec_visit.h5");
  ASSERT_TRUE(file.valid());

  // Full H5Ovisit1 traverses each unique object once (hard link alias is
  // deduplicated).
  VisitContext ctx1;
  EXPECT_EQ(
      H5Ovisit1(file.get(), H5_INDEX_NAME, H5_ITER_INC, VisitCallback, &ctx1),
      0);
  EXPECT_EQ(ctx1.records.size(), 4U);  // ".", "alpha", "alpha/dset", "alpha/dtype"

  // Early-stop positive return and callback negative return in H5Ovisit1.
  VisitContext stop_ctx;
  stop_ctx.stop_after = 2;
  stop_ctx.stop_code = 42;
  EXPECT_EQ(H5Ovisit1(file.get(), H5_INDEX_NAME, H5_ITER_INC, VisitCallback,
                      &stop_ctx),
            42);
  EXPECT_EQ(stop_ctx.records.size(), 2U);

  VisitContext err_ctx;
  err_ctx.stop_after = 1;
  err_ctx.stop_code = -1;
  EXPECT_LT(H5Ovisit1(file.get(), H5_INDEX_NAME, H5_ITER_INC, VisitCallback,
                      &err_ctx),
            0);

  // H5Ovisit_by_name1 rooted at "/alpha".
  VisitContext ctx_name1;
  EXPECT_EQ(H5Ovisit_by_name1(file.get(), "/alpha", H5_INDEX_CRT_ORDER,
                              H5_ITER_INC, VisitCallback, &ctx_name1,
                              H5P_DEFAULT),
            0);
  EXPECT_EQ(ctx_name1.records.size(), 3U);

  // H5Ovisit2 across field combinations: 0, individual bits, and ALL.
  for (unsigned fields :
       {0U, H5O_INFO_BASIC, H5O_INFO_TIME, H5O_INFO_NUM_ATTRS, H5O_INFO_HDR,
        H5O_INFO_META_SIZE, H5O_INFO_ALL}) {
    VisitContext ctx2;
    EXPECT_EQ(H5Ovisit2(file.get(), H5_INDEX_NAME, H5_ITER_INC, VisitCallback,
                        &ctx2, fields),
              0);
    // H5O__visit_cb deduplicates hard-linked objects only when oinfo.rc > 1,
    // which requires H5O_INFO_BASIC in `fields`; otherwise "/dset_alias" is
    // visited separately alongside "/alpha/dset".
    const size_t expected_count = (fields & H5O_INFO_BASIC) ? 4U : 5U;
    EXPECT_EQ(ctx2.records.size(), expected_count);
    if (fields == 0U) {
      EXPECT_EQ(ctx2.records[0].type, H5O_TYPE_UNKNOWN);
      EXPECT_EQ(ctx2.records[0].addr, HADDR_UNDEF);
    }
    if (fields & H5O_INFO_BASIC) {
      EXPECT_NE(ctx2.records[0].addr, HADDR_UNDEF);
    }
    if (fields & H5O_INFO_HDR) {
      EXPECT_GT(ctx2.records[0].hdr_nmesgs, 0U);
    }
  }

  // H5Ovisit2 early stop & callback error + H5Ovisit_by_name2.
  VisitContext stop_ctx2;
  stop_ctx2.stop_after = 2;
  stop_ctx2.stop_code = 17;
  EXPECT_EQ(H5Ovisit2(file.get(), H5_INDEX_NAME, H5_ITER_INC, VisitCallback,
                      &stop_ctx2, H5O_INFO_ALL),
            17);

  VisitContext err_ctx2;
  err_ctx2.stop_after = 1;
  err_ctx2.stop_code = -5;
  EXPECT_LT(H5Ovisit2(file.get(), H5_INDEX_NAME, H5_ITER_INC, VisitCallback,
                      &err_ctx2, H5O_INFO_BASIC),
            0);

  VisitContext ctx_name2;
  EXPECT_EQ(H5Ovisit_by_name2(file.get(), "/alpha", H5_INDEX_NAME, H5_ITER_DEC,
                              VisitCallback, &ctx_name2,
                              H5O_INFO_BASIC | H5O_INFO_HDR, H5P_DEFAULT),
            0);
  EXPECT_EQ(ctx_name2.records.size(), 3U);

  VisitContext err_name2;
  err_name2.stop_after = 1;
  err_name2.stop_code = -3;
  EXPECT_LT(H5Ovisit_by_name2(file.get(), "/alpha", H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &err_name2, H5O_INFO_ALL,
                              H5P_DEFAULT),
            0);
}

TEST(H5OdeprecTest, ArgumentValidationAndObjectLookupErrorsFailCleanly) {
  ScopedFile file = CreateSampleHierarchy("odeprec_errors.h5");
  ASSERT_TRUE(file.valid());
  ScopedErrorSilence silence;

  ScopedPlist fapl_wrong = MakeCoreFapl();
  ASSERT_TRUE(fapl_wrong.valid());
  H5O_info1_t oinfo = {};
  VisitContext ctx;

  // H5Oopen_by_addr errors.
  EXPECT_LT(H5Oopen_by_addr(H5I_INVALID_HID, 0), 0);
  EXPECT_LT(H5Oopen_by_addr(file.get(), HADDR_UNDEF), 0);
  EXPECT_LT(H5Oopen_by_addr(file.get(), 0x888888ULL), 0);

  // H5Oget_info1 & H5Oget_info2 errors.
  EXPECT_LT(H5Oget_info1(file.get(), nullptr), 0);
  EXPECT_LT(H5Oget_info1(H5I_INVALID_HID, &oinfo), 0);
  EXPECT_LT(H5Oget_info2(file.get(), nullptr, H5O_INFO_ALL), 0);
  EXPECT_LT(H5Oget_info2(file.get(), &oinfo, kInvalidInfoFields), 0);
  EXPECT_LT(H5Oget_info2(H5I_INVALID_HID, &oinfo, H5O_INFO_ALL), 0);

  // H5Oget_info_by_name1 & H5Oget_info_by_name2 errors.
  EXPECT_LT(H5Oget_info_by_name1(file.get(), nullptr, &oinfo, H5P_DEFAULT), 0);
  EXPECT_LT(H5Oget_info_by_name1(file.get(), "", &oinfo, H5P_DEFAULT), 0);
  EXPECT_LT(H5Oget_info_by_name1(file.get(), "/alpha", nullptr, H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Oget_info_by_name1(file.get(), "/alpha", &oinfo, fapl_wrong.get()), 0);
  EXPECT_LT(
      H5Oget_info_by_name1(H5I_INVALID_HID, "/alpha", &oinfo, H5P_DEFAULT), 0);
  EXPECT_LT(
      H5Oget_info_by_name1(file.get(), "/no_such_obj", &oinfo, H5P_DEFAULT), 0);

  EXPECT_LT(H5Oget_info_by_name2(file.get(), nullptr, &oinfo, H5O_INFO_ALL,
                                 H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Oget_info_by_name2(file.get(), "", &oinfo, H5O_INFO_ALL, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Oget_info_by_name2(file.get(), "/alpha", nullptr, H5O_INFO_ALL,
                                 H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_name2(file.get(), "/alpha", &oinfo,
                                 kInvalidInfoFields, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_name2(file.get(), "/alpha", &oinfo, H5O_INFO_ALL,
                                 fapl_wrong.get()),
            0);
  EXPECT_LT(H5Oget_info_by_name2(H5I_INVALID_HID, "/alpha", &oinfo,
                                 H5O_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_name2(file.get(), "/no_such_obj", &oinfo,
                                 H5O_INFO_ALL, H5P_DEFAULT),
            0);

  // H5Oget_info_by_idx1 & H5Oget_info_by_idx2 errors.
  EXPECT_LT(H5Oget_info_by_idx1(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                                0, &oinfo, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx1(file.get(), "", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &oinfo, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx1(file.get(), "/", H5_INDEX_UNKNOWN, H5_ITER_INC,
                                0, &oinfo, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx1(file.get(), "/", H5_INDEX_N, H5_ITER_INC, 0,
                                &oinfo, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx1(file.get(), "/", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                                0, &oinfo, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx1(file.get(), "/", H5_INDEX_NAME, H5_ITER_N, 0,
                                &oinfo, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx1(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 0,
                                nullptr, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx1(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &oinfo, fapl_wrong.get()),
            0);
  EXPECT_LT(H5Oget_info_by_idx1(H5I_INVALID_HID, "/", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &oinfo, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx1(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC,
                                99, &oinfo, H5P_DEFAULT),
            0);

  EXPECT_LT(H5Oget_info_by_idx2(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                                0, &oinfo, H5O_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx2(file.get(), "", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &oinfo, H5O_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx2(file.get(), "/", H5_INDEX_UNKNOWN, H5_ITER_INC,
                                0, &oinfo, H5O_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx2(file.get(), "/", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                                0, &oinfo, H5O_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx2(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 0,
                                nullptr, H5O_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx2(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &oinfo, kInvalidInfoFields, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx2(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC, 0,
                                &oinfo, H5O_INFO_ALL, fapl_wrong.get()),
            0);
  EXPECT_LT(H5Oget_info_by_idx2(H5I_INVALID_HID, "/", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &oinfo, H5O_INFO_ALL,
                                H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx2(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC,
                                99, &oinfo, H5O_INFO_ALL, H5P_DEFAULT),
            0);

  // H5Ovisit1 & H5Ovisit2 errors.
  EXPECT_LT(
      H5Ovisit1(file.get(), H5_INDEX_UNKNOWN, H5_ITER_INC, VisitCallback, &ctx),
      0);
  EXPECT_LT(
      H5Ovisit1(file.get(), H5_INDEX_N, H5_ITER_INC, VisitCallback, &ctx), 0);
  EXPECT_LT(
      H5Ovisit1(file.get(), H5_INDEX_NAME, H5_ITER_UNKNOWN, VisitCallback, &ctx),
      0);
  EXPECT_LT(
      H5Ovisit1(file.get(), H5_INDEX_NAME, H5_ITER_N, VisitCallback, &ctx), 0);
  EXPECT_LT(H5Ovisit1(file.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr, &ctx),
            0);
  EXPECT_LT(H5Ovisit1(H5I_INVALID_HID, H5_INDEX_NAME, H5_ITER_INC,
                      VisitCallback, &ctx),
            0);

  EXPECT_LT(H5Ovisit2(file.get(), H5_INDEX_UNKNOWN, H5_ITER_INC, VisitCallback,
                      &ctx, H5O_INFO_ALL),
            0);
  EXPECT_LT(H5Ovisit2(file.get(), H5_INDEX_NAME, H5_ITER_UNKNOWN, VisitCallback,
                      &ctx, H5O_INFO_ALL),
            0);
  EXPECT_LT(H5Ovisit2(file.get(), H5_INDEX_NAME, H5_ITER_INC, nullptr, &ctx,
                      H5O_INFO_ALL),
            0);
  EXPECT_LT(H5Ovisit2(file.get(), H5_INDEX_NAME, H5_ITER_INC, VisitCallback,
                      &ctx, kInvalidInfoFields),
            0);
  EXPECT_LT(H5Ovisit2(H5I_INVALID_HID, H5_INDEX_NAME, H5_ITER_INC,
                      VisitCallback, &ctx, H5O_INFO_ALL),
            0);

  // H5Ovisit_by_name1 & H5Ovisit_by_name2 errors.
  EXPECT_LT(H5Ovisit_by_name1(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name1(file.get(), "", H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name1(file.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC,
                              VisitCallback, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name1(file.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                              VisitCallback, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name1(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              nullptr, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name1(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &ctx, fapl_wrong.get()),
            0);
  EXPECT_LT(H5Ovisit_by_name1(H5I_INVALID_HID, ".", H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name1(file.get(), "/no_such_obj", H5_INDEX_NAME,
                              H5_ITER_INC, VisitCallback, &ctx, H5P_DEFAULT),
            0);

  EXPECT_LT(H5Ovisit_by_name2(file.get(), nullptr, H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &ctx, H5O_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name2(file.get(), "", H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &ctx, H5O_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name2(file.get(), ".", H5_INDEX_UNKNOWN, H5_ITER_INC,
                              VisitCallback, &ctx, H5O_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name2(file.get(), ".", H5_INDEX_NAME, H5_ITER_UNKNOWN,
                              VisitCallback, &ctx, H5O_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name2(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              nullptr, &ctx, H5O_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name2(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &ctx, kInvalidInfoFields,
                              H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name2(file.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &ctx, H5O_INFO_ALL,
                              fapl_wrong.get()),
            0);
  EXPECT_LT(H5Ovisit_by_name2(H5I_INVALID_HID, ".", H5_INDEX_NAME, H5_ITER_INC,
                              VisitCallback, &ctx, H5O_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit_by_name2(file.get(), "/no_such_obj", H5_INDEX_NAME,
                              H5_ITER_INC, VisitCallback, &ctx, H5O_INFO_ALL,
                              H5P_DEFAULT),
            0);
}

TEST(H5OdeprecTest, NonNativeVolConnectorErrorsAreRejectedAcrossAllApis) {
  ScopedNonNativeVol vol;
  ASSERT_GE(vol.id(), 0);

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_vol(fapl.get(), vol.id(), nullptr), 0);

  ScopedFile non_native_file(
      H5Fcreate("non_native.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(non_native_file.valid());

  ScopedErrorSilence silence;
  H5O_info1_t oinfo = {};
  VisitContext ctx;

  // 1. Non-native VOL object (!is_native_vol_obj branch in all 11 APIs).
  g_fail_get_conn_cls = false;
  EXPECT_LT(H5Oopen_by_addr(non_native_file.get(), 0), 0);
  EXPECT_LT(H5Oget_info1(non_native_file.get(), &oinfo), 0);
  EXPECT_LT(H5Oget_info_by_name1(non_native_file.get(), "obj", &oinfo,
                                 H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx1(non_native_file.get(), "/", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &oinfo, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info2(non_native_file.get(), &oinfo, H5O_INFO_ALL), 0);
  EXPECT_LT(H5Oget_info_by_name2(non_native_file.get(), "obj", &oinfo,
                                 H5O_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx2(non_native_file.get(), "/", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &oinfo, H5O_INFO_ALL,
                                H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit1(non_native_file.get(), H5_INDEX_NAME, H5_ITER_INC,
                      VisitCallback, &ctx),
            0);
  EXPECT_LT(H5Ovisit_by_name1(non_native_file.get(), ".", H5_INDEX_NAME,
                              H5_ITER_INC, VisitCallback, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit2(non_native_file.get(), H5_INDEX_NAME, H5_ITER_INC,
                      VisitCallback, &ctx, H5O_INFO_ALL),
            0);
  EXPECT_LT(H5Ovisit_by_name2(non_native_file.get(), ".", H5_INDEX_NAME,
                              H5_ITER_INC, VisitCallback, &ctx, H5O_INFO_ALL,
                              H5P_DEFAULT),
            0);

  // 2. Failing introspect get_conn_cls (H5VL_object_is_native < 0 in all 11
  //    APIs).
  g_fail_get_conn_cls = true;
  EXPECT_LT(H5Oopen_by_addr(non_native_file.get(), 0), 0);
  EXPECT_LT(H5Oget_info1(non_native_file.get(), &oinfo), 0);
  EXPECT_LT(H5Oget_info_by_name1(non_native_file.get(), "obj", &oinfo,
                                 H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx1(non_native_file.get(), "/", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &oinfo, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info2(non_native_file.get(), &oinfo, H5O_INFO_ALL), 0);
  EXPECT_LT(H5Oget_info_by_name2(non_native_file.get(), "obj", &oinfo,
                                 H5O_INFO_ALL, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Oget_info_by_idx2(non_native_file.get(), "/", H5_INDEX_NAME,
                                H5_ITER_INC, 0, &oinfo, H5O_INFO_ALL,
                                H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit1(non_native_file.get(), H5_INDEX_NAME, H5_ITER_INC,
                      VisitCallback, &ctx),
            0);
  EXPECT_LT(H5Ovisit_by_name1(non_native_file.get(), ".", H5_INDEX_NAME,
                              H5_ITER_INC, VisitCallback, &ctx, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Ovisit2(non_native_file.get(), H5_INDEX_NAME, H5_ITER_INC,
                      VisitCallback, &ctx, H5O_INFO_ALL),
            0);
  EXPECT_LT(H5Ovisit_by_name2(non_native_file.get(), ".", H5_INDEX_NAME,
                              H5_ITER_INC, VisitCallback, &ctx, H5O_INFO_ALL,
                              H5P_DEFAULT),
            0);
}

}  // namespace
}  // namespace hdf5
