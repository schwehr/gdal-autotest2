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
// Unit tests for third_party/hdf5/c++/src/H5FaccProp.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5FaccProp.cpp

#include <cstddef>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {

template struct ConstantGetterBypass<ConstantGetterTag<H5::FileAccPropList>,
                                     &H5::FileAccPropList::getConstant>;

namespace {

TEST(H5FaccPropTest, DriversCoreFamilySplitAndLog) {
  H5::FileAccPropList fapl;
  EXPECT_EQ(fapl.fromClass(), "FileAccPropList");
  EXPECT_GE(fapl.getId(), 0);
  EXPECT_GE(H5::FileAccPropList::DEFAULT.getId(), 0);

  H5::FileAccPropList copied(fapl);
  EXPECT_EQ(copied.getId(), fapl.getId());

  H5::FileAccPropList from_id(fapl.getId());
  EXPECT_TRUE(from_id == fapl);

  fapl.setStdio();
  EXPECT_EQ(fapl.getDriver(), H5FD_STDIO);

  fapl.setSec2();
  EXPECT_EQ(fapl.getDriver(), H5FD_SEC2);

  fapl.setCore(65536, false);
  EXPECT_EQ(fapl.getDriver(), H5FD_CORE);
  size_t increment = 0;
  bool backing_store = true;
  fapl.getCore(increment, backing_store);
  EXPECT_EQ(increment, 65536u);
  EXPECT_FALSE(backing_store);

  H5::FileAccPropList memb_fapl;
  fapl.setFamily(1024 * 1024, memb_fapl);
  EXPECT_EQ(fapl.getDriver(), H5FD_FAMILY);
  fapl.setFamilyOffset(4096);
  EXPECT_EQ(fapl.getFamilyOffset(), 4096u);

  hsize_t out_memb_size = 0;
  H5::FileAccPropList out_memb_plist;
  fapl.getFamily(out_memb_size, out_memb_plist);
  EXPECT_EQ(out_memb_size, 1024u * 1024u);
  H5::FileAccPropList ret_memb = fapl.getFamily(out_memb_size);
  EXPECT_EQ(out_memb_size, 1024u * 1024u);
  EXPECT_GE(ret_memb.getId(), 0);

  H5::FileAccPropList split_fapl;
  H5::FileAccPropList meta_fapl;
  H5::FileAccPropList raw_fapl;
  split_fapl.setSplit(meta_fapl, raw_fapl, ".meta", ".raw");
  split_fapl.setSplit(meta_fapl, raw_fapl, H5std_string(".m"),
                      H5std_string(".r"));
  EXPECT_EQ(split_fapl.getDriver(), H5FD_MULTI);

  TempFile log_path1(".log");
  TempFile log_path2(".log2");
  H5::FileAccPropList log_fapl;
  log_fapl.setLog(log_path1.c_str(), H5FD_LOG_ALL, 1024);
  log_fapl.setLog(H5std_string(log_path2.string()), H5FD_LOG_LOC_READ, 512);
  EXPECT_EQ(log_fapl.getDriver(), H5FD_LOG);

  H5::FileAccPropList driver_fapl;
  driver_fapl.setDriver(H5FD_CORE, nullptr);
  EXPECT_EQ(driver_fapl.getDriver(), H5FD_CORE);

  H5::FileAccPropList multi_fapl;
  ASSERT_GE(H5Pset_fapl_multi(multi_fapl.getId(), nullptr, nullptr, nullptr,
                              nullptr, true),
            0);
  multi_fapl.setMultiType(H5FD_MEM_SUPER);
  EXPECT_EQ(multi_fapl.getMultiType(), H5FD_MEM_SUPER);
}

TEST(H5FaccPropTest, SieveCacheAlignmentLockingAndLibver) {
  H5::FileAccPropList fapl;

  fapl.setSieveBufSize(128 * 1024);
  EXPECT_EQ(fapl.getSieveBufSize(), 128u * 1024u);

  hsize_t block_size = 4096;
  fapl.setMetaBlockSize(block_size);
  EXPECT_EQ(fapl.getMetaBlockSize(), 4096u);

  fapl.setAlignment(1024, 512);
  hsize_t threshold = 0;
  hsize_t alignment = 0;
  fapl.getAlignment(threshold, alignment);
  EXPECT_EQ(threshold, 1024u);
  EXPECT_EQ(alignment, 512u);

  fapl.setCache(0, 521, 1024 * 1024, 0.75);
  int mdc_nelmts = -1;
  size_t rdcc_nelmts = 0;
  size_t rdcc_nbytes = 0;
  double rdcc_w0 = 0.0;
  fapl.getCache(mdc_nelmts, rdcc_nelmts, rdcc_nbytes, rdcc_w0);
  EXPECT_EQ(rdcc_nelmts, 521u);
  EXPECT_EQ(rdcc_nbytes, 1024u * 1024u);
  EXPECT_DOUBLE_EQ(rdcc_w0, 0.75);

  fapl.setFcloseDegree(H5F_CLOSE_STRONG);
  EXPECT_EQ(fapl.getFcloseDegree(), H5F_CLOSE_STRONG);

  fapl.setGcReferences(1);
  EXPECT_EQ(fapl.getGcReferences(), 1u);

  fapl.setFileLocking(true, false);
  bool use_locking = false;
  bool ignore_disabled = true;
  fapl.getFileLocking(use_locking, ignore_disabled);
  EXPECT_TRUE(use_locking);
  EXPECT_FALSE(ignore_disabled);

  fapl.setLibverBounds(H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  H5F_libver_t low = H5F_LIBVER_EARLIEST;
  H5F_libver_t high = H5F_LIBVER_EARLIEST;
  fapl.getLibverBounds(low, high);
  EXPECT_EQ(low, H5F_LIBVER_V18);
  EXPECT_EQ(high, H5F_LIBVER_LATEST);
}

TEST(H5FaccPropTest, ErrorPathsThrowPropListIException) {
  ScopedQuietHdf5Errors quiet;
  H5::FileAccPropList fapl;

  EXPECT_THROW(fapl.setDriver(H5I_INVALID_HID, nullptr),
               H5::PropListIException);
  EXPECT_THROW(fapl.setAlignment(0, 0), H5::PropListIException);
  EXPECT_THROW(fapl.setCache(0, 521, 1024, 2.0), H5::PropListIException);
  EXPECT_THROW(fapl.setLibverBounds(H5F_LIBVER_LATEST, H5F_LIBVER_V18),
               H5::PropListIException);

  fapl.close();
  size_t inc = 0;
  bool backing = false;
  hsize_t memb_size = 0;
  H5::FileAccPropList memb;
  hsize_t block_size = 2048;
  hsize_t threshold = 0;
  hsize_t alignment = 0;
  int mdc_nelmts = 0;
  size_t rdcc_nelmts = 0;
  size_t rdcc_nbytes = 0;
  double rdcc_w0 = 0.0;
  bool use_locking = false;
  bool ignore_disabled = false;
  H5F_libver_t low = H5F_LIBVER_EARLIEST;
  H5F_libver_t high = H5F_LIBVER_LATEST;

  EXPECT_THROW(fapl.setStdio(), H5::PropListIException);
  EXPECT_THROW(fapl.getDriver(), H5::PropListIException);
  EXPECT_THROW(fapl.setFamilyOffset(0), H5::PropListIException);
  EXPECT_THROW(fapl.getFamilyOffset(), H5::PropListIException);
  EXPECT_THROW(fapl.setCore(1024, false), H5::PropListIException);
  EXPECT_THROW(fapl.getCore(inc, backing), H5::PropListIException);
  EXPECT_THROW(fapl.setFamily(1024, memb), H5::PropListIException);
  EXPECT_THROW(fapl.getFamily(memb_size, memb), H5::PropListIException);
  EXPECT_THROW(fapl.getFamily(memb_size), H5::PropListIException);
  EXPECT_THROW(fapl.setSplit(memb, memb, ".m", ".r"), H5::PropListIException);
  EXPECT_THROW(fapl.getSieveBufSize(), H5::PropListIException);
  EXPECT_THROW(fapl.setSieveBufSize(1024), H5::PropListIException);
  EXPECT_THROW(fapl.setMetaBlockSize(block_size), H5::PropListIException);
  EXPECT_THROW(fapl.getMetaBlockSize(), H5::PropListIException);
  EXPECT_THROW(fapl.setLog("unused.log", H5FD_LOG_ALL, 1024),
               H5::PropListIException);
  EXPECT_THROW(fapl.setSec2(), H5::PropListIException);
  EXPECT_THROW(fapl.getAlignment(threshold, alignment), H5::PropListIException);
  EXPECT_THROW(fapl.setMultiType(H5FD_MEM_SUPER), H5::PropListIException);
  EXPECT_THROW(fapl.getMultiType(), H5::PropListIException);
  EXPECT_THROW(fapl.getCache(mdc_nelmts, rdcc_nelmts, rdcc_nbytes, rdcc_w0),
               H5::PropListIException);
  EXPECT_THROW(fapl.setFcloseDegree(H5F_CLOSE_DEFAULT), H5::PropListIException);
  EXPECT_THROW(fapl.getFcloseDegree(), H5::PropListIException);
  EXPECT_THROW(fapl.setGcReferences(0), H5::PropListIException);
  EXPECT_THROW(fapl.getGcReferences(), H5::PropListIException);
  EXPECT_THROW(fapl.setFileLocking(true, true), H5::PropListIException);
  EXPECT_THROW(fapl.getFileLocking(use_locking, ignore_disabled),
               H5::PropListIException);
  EXPECT_THROW(fapl.getLibverBounds(low, high), H5::PropListIException);
}

TEST(H5FaccPropTest, GetConstantDuplicateCallThrows) {
  ExpectDuplicateGetConstantThrows<H5::FileAccPropList>();
}

}  // namespace
}  // namespace hdf5
