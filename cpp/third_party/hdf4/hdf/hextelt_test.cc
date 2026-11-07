// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

#ifdef FAIL
#undef FAIL
#endif
#ifdef SUCCEED
#undef SUCCEED
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
// clang-format off
#include "third_party/hdf4/hdf/src/hdf_priv.h"
#include "third_party/hdf4/hdf/src/hfile_priv.h"
#include "third_party/hdf4/hdf/src/hcomp.h"
#include "third_party/hdf4/hdf/src/hproto.h"
// clang-format on

namespace {

uint16_t MakeSpecialTag(uint16_t tag) {
  return static_cast<uint16_t>((~tag & 0x8000) ? (tag | 0x4000) : DFTAG_NULL);
}

std::string MakeTestSubdir(const std::string& name) {
  const std::filesystem::path dir =
      std::filesystem::path(::testing::TempDir()) / name;
  std::filesystem::create_directories(dir);
  return dir.string();
}

struct TestExtInfo {
  int attached;
  int32 extern_offset;
  int32 length;
  int32 length_file_name;
  int32 para_extfile_id;
  hdf_file_t file_external;
  char* extern_file_name;
  int file_open;
};

// -----------------------------------------------------------------------------
// HXcreate Argument Validation Tests
// -----------------------------------------------------------------------------

TEST(HExtEltTest, HXcreateArgValidation) {
  const std::string dir = MakeTestSubdir("hxcreate_argval");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "test.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "ext.dat").string();

  // Invalid file_id
  EXPECT_EQ(FAIL, HXcreate(-1, 1000, 1, ext_path.c_str(), 0, 0));

  // Open file in read-only mode -> DFACC_WRITE is denied
  const int32_t fid_create = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid_create);
  EXPECT_NE(FAIL, Hclose(fid_create));

  const int32_t fid_ro = Hopen(hdf_path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid_ro);
  EXPECT_EQ(FAIL, HXcreate(fid_ro, 1000, 1, ext_path.c_str(), 0, 0));
  EXPECT_NE(FAIL, Hclose(fid_ro));

  // Open file in RDWR mode
  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_RDWR, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid] { Hclose(fid); };

  // Null external filename
  EXPECT_EQ(FAIL, HXcreate(fid, 1000, 1, nullptr, 0, 0));

  // Negative offset
  EXPECT_EQ(FAIL, HXcreate(fid, 1000, 1, ext_path.c_str(), -1, 0));

  // Tag is already special tag
  EXPECT_EQ(FAIL,
            HXcreate(fid, MakeSpecialTag(1000), 1, ext_path.c_str(), 0, 0));

  // Relative path when extcreatedir is null hits lines 1251-1252
  EXPECT_EQ(FAIL, HXcreate(fid, 1000, 2, "rel_no_createdir.dat", 0, 0));
}

// -----------------------------------------------------------------------------
// HXcreate New Element & Full Lifecycle (Write, Seek, Read, Inquire)
// -----------------------------------------------------------------------------

TEST(HExtEltTest, CreateNewExternalElementAndIO) {
  const std::string dir = MakeTestSubdir("hextelt_new_io");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "main.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "ext_data.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1000;
  const uint16_t ref = 1;

  // Create external element with offset 0 and start_len 0
  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 0);
  ASSERT_NE(FAIL, aid);

  // Inquire about the newly created external element
  int32_t inq_fid = 0, inq_len = 0, inq_off = 0, inq_pos = 0;
  uint16_t inq_tag = 0, inq_ref = 0;
  int16_t inq_acc = 0, inq_sp = 0;
  EXPECT_NE(FAIL, Hinquire(aid, &inq_fid, &inq_tag, &inq_ref, &inq_len,
                           &inq_off, &inq_pos, &inq_acc, &inq_sp));
  EXPECT_EQ(fid, inq_fid);
  EXPECT_EQ(MakeSpecialTag(tag), inq_tag);
  EXPECT_EQ(ref, inq_ref);
  EXPECT_EQ(0, inq_len);
  EXPECT_EQ(0, inq_pos);
  EXPECT_EQ(SPECIAL_EXT, inq_sp);

  // Hinquire with all null pointers
  EXPECT_NE(FAIL, Hinquire(aid, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr));

  // Write data to external element
  const char payload[] = "Hello External World!";
  const int32_t payload_len = static_cast<int32_t>(sizeof(payload));
  EXPECT_EQ(payload_len, Hwrite(aid, payload_len, payload));

  // Seek tests
  // Negative offset -> FAIL
  EXPECT_EQ(FAIL, Hseek(aid, -5, DF_START));

  // Seek DF_START
  EXPECT_NE(FAIL, Hseek(aid, 6, DF_START));

  // Seek DF_CURRENT
  EXPECT_NE(FAIL, Hseek(aid, 2, DF_CURRENT));

  // Seek DF_END
  EXPECT_NE(FAIL, Hseek(aid, -7, DF_END));

  // Seek back to start and read
  EXPECT_NE(FAIL, Hseek(aid, 0, DF_START));
  char read_buf[64] = {0};
  EXPECT_EQ(payload_len, Hread(aid, payload_len, read_buf));
  EXPECT_STREQ(payload, read_buf);

  // Negative read length -> FAIL
  EXPECT_EQ(FAIL, Hread(aid, -1, read_buf));

  // Read with length 0 -> reads to end of element
  EXPECT_NE(FAIL, Hseek(aid, 6, DF_START));
  std::memset(read_buf, 0, sizeof(read_buf));
  EXPECT_EQ(payload_len - 6, Hread(aid, 0, read_buf));
  EXPECT_STREQ("External World!", read_buf);

  // Read beyond end -> clamped to remaining length
  EXPECT_NE(FAIL, Hseek(aid, 10, DF_START));
  std::memset(read_buf, 0, sizeof(read_buf));
  EXPECT_EQ(payload_len - 10, Hread(aid, 1000, read_buf));
  EXPECT_STREQ("rnal World!", read_buf);

  // Negative write length -> FAIL
  EXPECT_EQ(FAIL, Hwrite(aid, -1, payload));

  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));

  // Reopen HDF file and read back external element using Hstartread and
  // Hgetelement
  const int32_t fid_read = Hopen(hdf_path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid_read);
  absl::Cleanup read_closer = [fid_read] { Hclose(fid_read); };

  const int32_t aid_read = Hstartread(fid_read, tag, ref);
  ASSERT_NE(FAIL, aid_read);
  std::memset(read_buf, 0, sizeof(read_buf));
  EXPECT_EQ(payload_len, Hread(aid_read, payload_len, read_buf));
  EXPECT_STREQ(payload, read_buf);
  EXPECT_NE(FAIL, Hendaccess(aid_read));

  // Test Hgetelement
  std::memset(read_buf, 0, sizeof(read_buf));
  EXPECT_EQ(payload_len, Hgetelement(fid_read, tag, ref,
                                     reinterpret_cast<uint8_t*>(read_buf)));
  EXPECT_STREQ(payload, read_buf);
}

// -----------------------------------------------------------------------------
// Promotion of Existing Regular Element to External Element
// -----------------------------------------------------------------------------

TEST(HExtEltTest, PromoteRegularElementToExternal) {
  const std::string dir = MakeTestSubdir("hextelt_promote_reg");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "promote.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "promoted.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1002;
  const uint16_t ref = 1;
  const char orig_data[] = "Original data stored internally in HDF file";
  const int32_t orig_len = static_cast<int32_t>(sizeof(orig_data));

  // 1. Write ordinary data element inside HDF file
  EXPECT_NE(FAIL,
            Hputelement(fid, tag, ref,
                        reinterpret_cast<const uint8_t*>(orig_data), orig_len));

  // 2. Promote to external element at offset 16 in ext_path
  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 16, 0);
  ASSERT_NE(FAIL, aid);

  // Verify external element length matches original data
  int32_t inq_len = 0;
  EXPECT_NE(FAIL, Hinquire(aid, nullptr, nullptr, nullptr, &inq_len, nullptr,
                           nullptr, nullptr, nullptr));
  EXPECT_EQ(orig_len, inq_len);

  // Read data through external AID
  char buf[128] = {0};
  EXPECT_EQ(orig_len, Hread(aid, orig_len, buf));
  EXPECT_STREQ(orig_data, buf);

  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));

  // Re-open and verify
  const int32_t fid_read = Hopen(hdf_path.c_str(), DFACC_RDWR, 0);
  ASSERT_NE(FAIL, fid_read);
  absl::Cleanup closer = [fid_read] { Hclose(fid_read); };

  std::memset(buf, 0, sizeof(buf));
  EXPECT_EQ(orig_len,
            Hgetelement(fid_read, tag, ref, reinterpret_cast<uint8_t*>(buf)));
  EXPECT_STREQ(orig_data, buf);

  // Append/write more data into the external element via Hstartwrite
  const int32_t aid_write = Hstartwrite(fid_read, tag, ref, orig_len + 10);
  ASSERT_NE(FAIL, aid_write);
  EXPECT_NE(FAIL, Hseek(aid_write, orig_len, DF_START));
  const char extra[] = " extra123";
  EXPECT_EQ(sizeof(extra), Hwrite(aid_write, sizeof(extra), extra));
  EXPECT_NE(FAIL, Hendaccess(aid_write));
}

// -----------------------------------------------------------------------------
// Promotion of Linked-Block Element to External Element
// -----------------------------------------------------------------------------

TEST(HExtEltTest, PromoteLinkedBlockToExternal) {
  const std::string dir = MakeTestSubdir("hextelt_promote_linked");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "linked.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "ext_from_linked.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1003;
  const uint16_t ref = 1;

  // 1. Create linked block element
  const int32_t link_aid = HLcreate(fid, tag, ref, 32, 2);
  ASSERT_NE(FAIL, link_aid);
  const char linked_data[] = "Linked block data across multiple blocks";
  const int32_t linked_len = static_cast<int32_t>(sizeof(linked_data));
  EXPECT_EQ(linked_len, Hwrite(link_aid, linked_len, linked_data));
  EXPECT_NE(FAIL, Hendaccess(link_aid));

  // 2. Promote linked-block element to external element
  const int32_t ext_aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 0);
  ASSERT_NE(FAIL, ext_aid);

  int32_t inq_len = 0;
  EXPECT_NE(FAIL, Hinquire(ext_aid, nullptr, nullptr, nullptr, &inq_len,
                           nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(linked_len, inq_len);

  char buf[128] = {0};
  EXPECT_EQ(linked_len, Hread(ext_aid, linked_len, buf));
  EXPECT_STREQ(linked_data, buf);

  EXPECT_NE(FAIL, Hendaccess(ext_aid));
  EXPECT_NE(FAIL, Hclose(fid));
}

// -----------------------------------------------------------------------------
// Promotion of Existing External Element to Another External Element
// -----------------------------------------------------------------------------

TEST(HExtEltTest, PromoteExternalToNewExternal) {
  const std::string dir = MakeTestSubdir("hextelt_promote_ext");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "ext2ext.hdf").string();
  const std::string ext1_path =
      (std::filesystem::path(dir) / "ext1.dat").string();
  const std::string ext2_path =
      (std::filesystem::path(dir) / "ext2.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1004;
  const uint16_t ref = 1;

  // 1. Create first external element
  const int32_t aid1 = HXcreate(fid, tag, ref, ext1_path.c_str(), 0, 0);
  ASSERT_NE(FAIL, aid1);
  const char msg[] = "Migrating from ext1 to ext2";
  const int32_t msg_len = static_cast<int32_t>(sizeof(msg));
  EXPECT_EQ(msg_len, Hwrite(aid1, msg_len, msg));
  EXPECT_NE(FAIL, Hendaccess(aid1));

  // 2. Re-create / promote the same tag/ref pointing to ext2
  const int32_t aid2 = HXcreate(fid, tag, ref, ext2_path.c_str(), 8, 0);
  ASSERT_NE(FAIL, aid2);

  char buf[128] = {0};
  EXPECT_EQ(msg_len, Hread(aid2, msg_len, buf));
  EXPECT_STREQ(msg, buf);

  EXPECT_NE(FAIL, Hendaccess(aid2));
  EXPECT_NE(FAIL, Hclose(fid));
}

// -----------------------------------------------------------------------------
// Rejection of Compressed Special Element in HXcreate
// -----------------------------------------------------------------------------

TEST(HExtEltTest, RejectCompressedElementPromotion) {
  const std::string dir = MakeTestSubdir("hextelt_comp_reject");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "comp.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "comp_ext.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid] { Hclose(fid); };

  const uint16_t tag = 1005;
  const uint16_t ref = 1;

  model_info minfo;
  std::memset(&minfo, 0, sizeof(minfo));
  comp_info cinfo;
  std::memset(&cinfo, 0, sizeof(cinfo));

  const int32_t aid =
      HCcreate(fid, tag, ref, COMP_MODEL_STDIO, &minfo, COMP_CODE_NONE, &cinfo);
  ASSERT_NE(FAIL, aid);
  const char data[] = "Compressed payload";
  EXPECT_EQ(sizeof(data), Hwrite(aid, sizeof(data), data));
  EXPECT_NE(FAIL, Hendaccess(aid));

  // HXcreate must reject compressed element with DFE_CANTMOD
  EXPECT_EQ(FAIL, HXcreate(fid, tag, ref, ext_path.c_str(), 0, 0));
}

// -----------------------------------------------------------------------------
// HDget_special_info and HDreset_special_info Tests (HXPinfo and HXPreset)
// -----------------------------------------------------------------------------

TEST(HExtEltTest, GetAndResetSpecialInfo) {
  const std::string dir = MakeTestSubdir("hextelt_reset_info");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "reset.hdf").string();
  const std::string ext1_path =
      (std::filesystem::path(dir) / "orig_file.dat").string();
  const std::string ext2_path =
      (std::filesystem::path(dir) / "new_file.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1006;
  const uint16_t ref = 1;

  const int32_t aid = HXcreate(fid, tag, ref, ext1_path.c_str(), 10, 0);
  ASSERT_NE(FAIL, aid);

  const char data[] = "Payload for reset special info";
  const int32_t data_len = static_cast<int32_t>(sizeof(data));
  EXPECT_EQ(data_len, Hwrite(aid, data_len, data));

  // Query special info
  sp_info_block_t sp_info;
  std::memset(&sp_info, 0, sizeof(sp_info));
  EXPECT_NE(FAIL, HDget_special_info(aid, &sp_info));
  EXPECT_EQ(SPECIAL_EXT, sp_info.key);
  EXPECT_EQ(10, sp_info.offset);
  EXPECT_EQ(data_len, sp_info.length);
  EXPECT_STREQ(ext1_path.c_str(), sp_info.path);

  // Copy file content from ext1 to ext2 so reset works
  std::filesystem::copy_file(ext1_path, ext2_path);

  // Reset special info with new path
  sp_info.path = const_cast<char*>(ext2_path.c_str());
  sp_info.offset = 20;
  EXPECT_NE(FAIL, HDset_special_info(aid, &sp_info));

  // Query again to verify updated path and offset
  sp_info_block_t sp_info2;
  std::memset(&sp_info2, 0, sizeof(sp_info2));
  EXPECT_NE(FAIL, HDget_special_info(aid, &sp_info2));
  EXPECT_EQ(20, sp_info2.offset);
  EXPECT_STREQ(ext2_path.c_str(), sp_info2.path);

  // Test error cases in HXPinfo and HXPreset
  auto* access_rec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_NE(nullptr, access_rec);

  // Special mismatch in HXPinfo
  const int16_t orig_special = access_rec->special;
  access_rec->special = SPECIAL_LINKED;
  EXPECT_EQ(FAIL, HXPinfo(access_rec, &sp_info));
  access_rec->special = orig_special;

  // Key mismatch in HXPreset
  sp_info.key = SPECIAL_LINKED;
  EXPECT_EQ(FAIL, HXPreset(access_rec, &sp_info));
  sp_info.key = SPECIAL_EXT;

  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));
}

// -----------------------------------------------------------------------------
// HXsetcreatedir and HXsetdir Tests
// -----------------------------------------------------------------------------

TEST(HExtEltTest, CreatedirAndSetdir) {
  const std::string dir = MakeTestSubdir("hextelt_dirs");
  absl::Cleanup dir_cleaner = [&dir] {
    HXsetcreatedir(nullptr);
    HXsetdir(nullptr);
    HXPshutdown();
    std::filesystem::remove_all(dir);
  };

  const std::string create_subdir =
      (std::filesystem::path(dir) / "create_dir").string();
  const std::string locate_subdir =
      (std::filesystem::path(dir) / "locate_dir").string();
  std::filesystem::create_directories(create_subdir);
  std::filesystem::create_directories(locate_subdir);

  const std::string hdf_path =
      (std::filesystem::path(dir) / "dirs.hdf").string();

  // Test HXsetcreatedir
  EXPECT_EQ(SUCCEED, HXsetcreatedir(create_subdir.c_str()));

  // Test HXsetdir
  EXPECT_EQ(SUCCEED, HXsetdir(nullptr));
  EXPECT_EQ(SUCCEED, HXsetdir(locate_subdir.c_str()));
  // Calling same directory again -> branch where strings match
  EXPECT_EQ(SUCCEED, HXsetdir(locate_subdir.c_str()));

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1007;
  const uint16_t ref = 1;

  // When relative filename is given, HXcreate prepends extcreatedir
  const char rel_name[] = "rel_ext_file.dat";
  const int32_t aid = HXcreate(fid, tag, ref, rel_name, 0, 0);
  ASSERT_NE(FAIL, aid);

  const char data[] = "Created via HXsetcreatedir!";
  EXPECT_EQ(sizeof(data), Hwrite(aid, sizeof(data), data));
  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));

  // Verify file was created inside create_subdir
  const std::filesystem::path created_file =
      std::filesystem::path(create_subdir) / rel_name;
  EXPECT_TRUE(std::filesystem::exists(created_file));

  // Now move file to locate_subdir
  const std::filesystem::path located_file =
      std::filesystem::path(locate_subdir) / rel_name;
  std::filesystem::rename(created_file, located_file);

  // Set search path with multiple directories separated by '|'
  const std::string search_path =
      (std::filesystem::path(dir) / "bogus_dir").string() + "|" + locate_subdir;
  EXPECT_EQ(SUCCEED, HXsetdir(search_path.c_str()));

  // Reopen HDF file and read the external element found via HXsetdir
  const int32_t fid_read = Hopen(hdf_path.c_str(), DFACC_RDWR, 0);
  ASSERT_NE(FAIL, fid_read);
  absl::Cleanup closer = [fid_read] { Hclose(fid_read); };

  const int32_t aid_read = Hstartread(fid_read, tag, ref);
  ASSERT_NE(FAIL, aid_read);
  char buf[64] = {0};
  EXPECT_EQ(sizeof(data), Hread(aid_read, sizeof(data), buf));
  EXPECT_STREQ(data, buf);
  EXPECT_NE(FAIL, Hendaccess(aid_read));

  // Write while open to test directory changed logic in write mode
  const int32_t aid_write = Hstartwrite(fid_read, tag, ref, sizeof(data));
  ASSERT_NE(FAIL, aid_write);
  EXPECT_EQ(SUCCEED, HXsetdir(locate_subdir.c_str()));
  EXPECT_EQ(sizeof(data), Hwrite(aid_write, sizeof(data), data));
  EXPECT_NE(FAIL, Hendaccess(aid_write));

  // Reset directories
  EXPECT_EQ(SUCCEED, HXsetcreatedir(nullptr));
  EXPECT_EQ(SUCCEED, HXsetdir(nullptr));
}

// -----------------------------------------------------------------------------
// Environment Variables HDFEXTCREATEDIR and HDFEXTDIR Tests
// -----------------------------------------------------------------------------

TEST(HExtEltTest, EnvironmentVariables) {
  const std::string dir = MakeTestSubdir("hextelt_env");
  absl::Cleanup dir_cleaner = [&dir] {
    unsetenv("HDFEXTCREATEDIR");
    unsetenv("HDFEXTDIR");
    HXPshutdown();
    std::filesystem::remove_all(dir);
  };

  const std::string env_create =
      (std::filesystem::path(dir) / "env_create").string();
  const std::string env_dir =
      (std::filesystem::path(dir) / "env_search").string();
  std::filesystem::create_directories(env_create);
  std::filesystem::create_directories(env_dir);

  setenv("HDFEXTCREATEDIR", env_create.c_str(), 1);
  setenv("HDFEXTDIR", env_dir.c_str(), 1);

  // Reset hextelt state so environment variables are re-read
  HXPshutdown();

  const std::string hdf_path =
      (std::filesystem::path(dir) / "env.hdf").string();
  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1008;
  const uint16_t ref = 1;
  const char rel_fname[] = "env_file.dat";

  const int32_t aid = HXcreate(fid, tag, ref, rel_fname, 0, 0);
  ASSERT_NE(FAIL, aid);
  const char msg[] = "Env created external data";
  EXPECT_EQ(sizeof(msg), Hwrite(aid, sizeof(msg), msg));
  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));

  // Verify created file in env_create
  const std::filesystem::path created =
      std::filesystem::path(env_create) / rel_fname;
  EXPECT_TRUE(std::filesystem::exists(created));

  // Move to env_dir
  const std::filesystem::path moved =
      std::filesystem::path(env_dir) / rel_fname;
  std::filesystem::rename(created, moved);

  // Read back via HDFEXTDIR search
  const int32_t fid_read = Hopen(hdf_path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid_read);
  absl::Cleanup closer = [fid_read] { Hclose(fid_read); };

  char buf[64] = {0};
  EXPECT_EQ(sizeof(msg),
            Hgetelement(fid_read, tag, ref, reinterpret_cast<uint8_t*>(buf)));
  EXPECT_STREQ(msg, buf);
}

// -----------------------------------------------------------------------------
// Direct HXP Function Error Coverage
// -----------------------------------------------------------------------------

TEST(HExtEltTest, HXPDirectFunctionErrors) {
  const std::string dir = MakeTestSubdir("hextelt_hxp_direct");
  absl::Cleanup dir_cleaner = [&dir] {
    HXPshutdown();
    std::filesystem::remove_all(dir);
  };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "direct.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "direct.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1010;
  const uint16_t ref = 1;

  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 0);
  ASSERT_NE(FAIL, aid);

  auto* access_rec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_NE(nullptr, access_rec);

  // Test HXPsetaccesstype with DFACC_SERIAL
  access_rec->access_type = DFACC_SERIAL;
  EXPECT_EQ(SUCCEED, HXPsetaccesstype(access_rec));

  // Test HXPsetaccesstype with invalid access_type
  access_rec->access_type = 999;
  EXPECT_EQ(FAIL, HXPsetaccesstype(access_rec));
  access_rec->access_type = DFACC_SERIAL;

  // HXPendaccess argument validation
  EXPECT_EQ(FAIL, HXPendaccess(nullptr));

  accrec_t* rec = HIget_access_rec();
  ASSERT_NE(nullptr, rec);
  rec->file_id = -1;
  // HXPendaccess releases rec on failure via HIrelease_accrec_node
  EXPECT_EQ(FAIL, HXPendaccess(rec));

  // HXPsetaccesstype argument validation
  EXPECT_EQ(FAIL, HXPsetaccesstype(nullptr));

  accrec_t* rec2 = HIget_access_rec();
  ASSERT_NE(nullptr, rec2);
  rec2->special_info = nullptr;
  EXPECT_EQ(FAIL, HXPsetaccesstype(rec2));
  HIrelease_accrec_node(rec2);

  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));

  // HXPshutdown
  EXPECT_EQ(SUCCEED, HXPshutdown());
}

TEST(HExtEltTest, SimultaneousAccessToExternalElement) {
  const std::string dir = MakeTestSubdir("hextelt_simultaneous");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "simul.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "simul.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1011;
  const uint16_t ref = 1;

  const int32_t aid_create = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 0);
  ASSERT_NE(FAIL, aid_create);
  const char data[] = "Simultaneous AID data!";
  EXPECT_EQ(sizeof(data), Hwrite(aid_create, sizeof(data), data));
  EXPECT_NE(FAIL, Hendaccess(aid_create));
  EXPECT_NE(FAIL, Hclose(fid));

  // Reopen file
  const int32_t fid_read = Hopen(hdf_path.c_str(), DFACC_RDWR, 0);
  ASSERT_NE(FAIL, fid_read);
  absl::Cleanup closer = [fid_read] { Hclose(fid_read); };

  // First AID opens external element
  const int32_t aid1 = Hstartread(fid_read, tag, ref);
  ASSERT_NE(FAIL, aid1);

  // Second AID opens SAME external element -> hits HIgetspinfo attached++
  // (lines 471-477)
  const int32_t aid2 = Hstartread(fid_read, tag, ref);
  ASSERT_NE(FAIL, aid2);

  char buf1[64] = {0}, buf2[64] = {0};
  EXPECT_EQ(sizeof(data), Hread(aid1, sizeof(data), buf1));
  EXPECT_EQ(sizeof(data), Hread(aid2, sizeof(data), buf2));
  EXPECT_STREQ(data, buf1);
  EXPECT_STREQ(data, buf2);

  // End access to aid1 (attached decreases but > 0)
  EXPECT_NE(FAIL, Hendaccess(aid1));

  // End access to aid2 (attached reaches 0, closes and frees info)
  EXPECT_NE(FAIL, Hendaccess(aid2));
}

TEST(HExtEltTest, MissingExternalFileErrors) {
  const std::string dir = MakeTestSubdir("hextelt_missing_file");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "missing.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "ext_deleted.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1012;
  const uint16_t ref = 1;

  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 100);
  ASSERT_NE(FAIL, aid);
  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));

  // Delete external file from disk
  std::filesystem::remove(ext_path);

  // Reopen HDF file and attempt to read missing external file -> HXPread hits
  // lines 654-668
  const int32_t fid_read = Hopen(hdf_path.c_str(), DFACC_RDWR, 0);
  ASSERT_NE(FAIL, fid_read);
  absl::Cleanup closer = [fid_read] { Hclose(fid_read); };

  const int32_t aid_read = Hstartread(fid_read, tag, ref);
  ASSERT_NE(FAIL, aid_read);
  char buf[64] = {0};
  EXPECT_EQ(FAIL, Hread(aid_read, sizeof(buf), buf));
  EXPECT_NE(FAIL, Hendaccess(aid_read));

  // Attempt to write to missing external file -> HXPwrite hits lines 732-746
  const int32_t aid_write = Hstartwrite(fid_read, tag, ref, 64);
  ASSERT_NE(FAIL, aid_write);
  EXPECT_EQ(FAIL, Hwrite(aid_write, 5, "hello"));
  EXPECT_NE(FAIL, Hendaccess(aid_write));
}

TEST(HExtEltTest, AbsolutePathRelocatedViaSetdirAndEnv) {
  const std::string dir = MakeTestSubdir("hextelt_relocate");
  absl::Cleanup dir_cleaner = [&dir] {
    HXsetdir(nullptr);
    unsetenv("HDFEXTDIR");
    HXPshutdown();
    std::filesystem::remove_all(dir);
  };

  const std::string old_dir =
      (std::filesystem::path(dir) / "old_location").string();
  const std::string new_dir =
      (std::filesystem::path(dir) / "new_location").string();
  std::filesystem::create_directories(old_dir);
  std::filesystem::create_directories(new_dir);

  const std::string hdf_path =
      (std::filesystem::path(dir) / "reloc.hdf").string();
  const std::string old_ext_path =
      (std::filesystem::path(old_dir) / "file.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1013;
  const uint16_t ref = 1;

  // Create external element with absolute path old_ext_path
  const int32_t aid = HXcreate(fid, tag, ref, old_ext_path.c_str(), 0, 0);
  ASSERT_NE(FAIL, aid);
  const char data[] = "Relocated external file data!";
  EXPECT_EQ(sizeof(data), Hwrite(aid, sizeof(data), data));
  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));

  // Move file.dat to new_dir (so old_ext_path no longer exists)
  const std::string new_ext_path =
      (std::filesystem::path(new_dir) / "file.dat").string();
  std::filesystem::rename(old_ext_path, new_ext_path);

  // 1. Without HXsetdir or HDFEXTDIR: read fails with DFE_FNF (lines 1255-1257)
  const int32_t fid_read1 = Hopen(hdf_path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid_read1);
  const int32_t aid_fail = Hstartread(fid_read1, tag, ref);
  ASSERT_NE(FAIL, aid_fail);
  char buf[64] = {0};
  EXPECT_EQ(FAIL, Hread(aid_fail, sizeof(buf), buf));
  EXPECT_NE(FAIL, Hendaccess(aid_fail));
  EXPECT_NE(FAIL, Hclose(fid_read1));

  // 2. With HXsetdir: old absolute path is stripped, found in new_dir! (lines
  // 1258-1295)
  EXPECT_EQ(SUCCEED, HXsetdir(new_dir.c_str()));
  const int32_t fid_read2 = Hopen(hdf_path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid_read2);
  const int32_t aid_success1 = Hstartread(fid_read2, tag, ref);
  ASSERT_NE(FAIL, aid_success1);
  std::memset(buf, 0, sizeof(buf));
  EXPECT_EQ(sizeof(data), Hread(aid_success1, sizeof(data), buf));
  EXPECT_STREQ(data, buf);
  EXPECT_NE(FAIL, Hendaccess(aid_success1));
  EXPECT_NE(FAIL, Hclose(fid_read2));

  // 3. With HDFEXTDIR: reset HXsetdir, use HDFEXTDIR env var! (lines 1298-1326)
  EXPECT_EQ(SUCCEED, HXsetdir(nullptr));
  setenv("HDFEXTDIR", new_dir.c_str(), 1);
  HXPshutdown();

  const int32_t fid_read3 = Hopen(hdf_path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid_read3);
  const int32_t aid_success2 = Hstartread(fid_read3, tag, ref);
  ASSERT_NE(FAIL, aid_success2);
  std::memset(buf, 0, sizeof(buf));
  EXPECT_EQ(sizeof(data), Hread(aid_success2, sizeof(data), buf));
  EXPECT_STREQ(data, buf);
  EXPECT_NE(FAIL, Hendaccess(aid_success2));
  EXPECT_NE(FAIL, Hclose(fid_read3));
}

TEST(HExtEltTest, PromoteEmptyRegularElement) {
  const std::string dir = MakeTestSubdir("hextelt_empty_reg");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "empty.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "empty_ext.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid] { Hclose(fid); };

  const uint16_t tag = 1014;
  const uint16_t ref = 1;

  // Create empty regular element (len 0)
  const int32_t aid_reg = Hstartwrite(fid, tag, ref, 0);
  ASSERT_NE(FAIL, aid_reg);
  EXPECT_NE(FAIL, Hendaccess(aid_reg));

  // Promote empty regular element to external element with start_len = 50 (hits
  // line 296)
  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 50);
  ASSERT_NE(FAIL, aid);

  int32_t inq_len = 0;
  EXPECT_NE(FAIL, Hinquire(aid, nullptr, nullptr, nullptr, &inq_len, nullptr,
                           nullptr, nullptr, nullptr));
  EXPECT_EQ(50, inq_len);

  EXPECT_NE(FAIL, Hendaccess(aid));
}

TEST(HExtEltTest, HXPwriteFallbackToWritable) {
  const std::string dir = MakeTestSubdir("hextelt_write_fallback");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "fallback.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "fallback.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1015;
  const uint16_t ref = 1;

  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 0);
  ASSERT_NE(FAIL, aid);
  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));

  // Reopen in RDWR mode
  const int32_t fid_rdwr = Hopen(hdf_path.c_str(), DFACC_RDWR, 0);
  ASSERT_NE(FAIL, fid_rdwr);
  absl::Cleanup closer = [fid_rdwr] { Hclose(fid_rdwr); };

  // Open external element with Hstartread and perform a read so
  // info->file_external is opened in read-only mode.
  const int32_t aid_read = Hstartread(fid_rdwr, tag, ref);
  ASSERT_NE(FAIL, aid_read);
  absl::Cleanup closer_read = [aid_read] { Hendaccess(aid_read); };

  char rdbuf[4] = {0};
  EXPECT_NE(FAIL, Hread(aid_read, 1, rdbuf));

  // Open a second AID for the same element with Hstartwrite, which shares
  // special_info via HIgetspinfo().
  const int32_t aid_write = Hstartwrite(fid_rdwr, tag, ref, 0);
  ASSERT_NE(FAIL, aid_write);
  absl::Cleanup closer_write = [aid_write] { Hendaccess(aid_write); };

  // Call Hwrite on aid_write: HI_WRITE fails on the read-only file_external,
  // triggering fallback lines 758-769 where it reopens with DFACC_WRITE and
  // succeeds!
  const char data[] = "Fallback write succeeds!";
  EXPECT_EQ(sizeof(data), Hwrite(aid_write, sizeof(data), data));
}

TEST(HExtEltTest, HXPwriteReadOnlyFileFailure) {
  const std::string dir = MakeTestSubdir("hextelt_ro_file");
  absl::Cleanup dir_cleaner = [&dir] {
    // Restore write permissions before cleanup
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      std::filesystem::permissions(entry.path(), std::filesystem::perms::all);
    }
    std::filesystem::remove_all(dir);
  };

  const std::string hdf_path = (std::filesystem::path(dir) / "ro.hdf").string();
  const std::string ext_path = (std::filesystem::path(dir) / "ro.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1016;
  const uint16_t ref = 1;

  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 0);
  ASSERT_NE(FAIL, aid);
  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));

  // Make the external file read-only on filesystem (perms: owner_read only)
  std::filesystem::permissions(ext_path, std::filesystem::perms::owner_read);

  const int32_t fid_rdwr = Hopen(hdf_path.c_str(), DFACC_RDWR, 0);
  ASSERT_NE(FAIL, fid_rdwr);
  absl::Cleanup closer = [fid_rdwr] { Hclose(fid_rdwr); };

  const int32_t aid_write = Hstartwrite(fid_rdwr, tag, ref, 32);
  ASSERT_NE(FAIL, aid_write);

  // Writing must fail with DFE_DENIED (lines 761-764)
  EXPECT_EQ(FAIL, Hwrite(aid_write, 5, "hello"));

  EXPECT_NE(FAIL, Hendaccess(aid_write));
}

TEST(HExtEltTest, HXPreadSeekPastEnd) {
  const std::string dir = MakeTestSubdir("hextelt_seek_past_end");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "seek_past.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "seek_past.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1017;
  const uint16_t ref = 1;

  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 0);
  ASSERT_NE(FAIL, aid);
  const char data[] = "short";
  EXPECT_EQ(sizeof(data), Hwrite(aid, sizeof(data), data));

  // Seek past end of element (HXPseek allows any non-negative offset)
  EXPECT_NE(FAIL, Hseek(aid, 100, DF_START));

  // HXPread with length 0 calculates length = info->length - posn < 0 -> hits
  // line 645
  char buf[32] = {0};
  EXPECT_EQ(FAIL, Hread(aid, 0, buf));

  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));
}

TEST(HExtEltTest, HstartwriteOnReadOnlyFile) {
  const std::string dir = MakeTestSubdir("hextelt_ro_hdf");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "ro_file.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "ro_ext.dat").string();

  const int32_t fid_create = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid_create);
  const uint16_t tag = 1018;
  const uint16_t ref = 1;
  const int32_t aid = HXcreate(fid_create, tag, ref, ext_path.c_str(), 0, 10);
  ASSERT_NE(FAIL, aid);
  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid_create));

  // Open HDF file read-only (DFACC_READ)
  const int32_t fid_ro = Hopen(hdf_path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid_ro);
  absl::Cleanup closer = [fid_ro] { Hclose(fid_ro); };

  // Hstartwrite requests DFACC_WRITE, but file is DFACC_READ -> HXIstaccess
  // hits line 462
  EXPECT_EQ(FAIL, Hstartwrite(fid_ro, tag, ref, 20));
}

TEST(HExtEltTest, HXPFunctionErrorsWithInvalidIDs) {
  const std::string dir = MakeTestSubdir("hextelt_invalid_ids");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "inv.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "inv.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  const uint16_t tag = 1019;
  const uint16_t ref = 1;
  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 10);
  ASSERT_NE(FAIL, aid);

  auto* access_rec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_NE(nullptr, access_rec);

  // HXPinquire with invalid ddid (line 829)
  const atom_t orig_ddid = access_rec->ddid;
  access_rec->ddid = -1;
  uint16 t = 0, r = 0;
  EXPECT_EQ(FAIL, HXPinquire(access_rec, nullptr, &t, &r, nullptr, nullptr,
                             nullptr, nullptr, nullptr));
  access_rec->ddid = orig_ddid;

  // HXPreset with invalid file_id (line 1020)
  sp_info_block_t sp_info;
  std::memset(&sp_info, 0, sizeof(sp_info));
  sp_info.key = SPECIAL_EXT;
  sp_info.path = const_cast<char*>(ext_path.c_str());
  const int32_t orig_fid = access_rec->file_id;
  access_rec->file_id = -1;
  EXPECT_EQ(FAIL, HXPreset(access_rec, &sp_info));
  access_rec->file_id = orig_fid;

  // HXPendaccess with invalid ddid (line 887)
  access_rec->ddid = -1;
  EXPECT_EQ(FAIL, HXPendaccess(access_rec));
  // Note: HXPendaccess releases access_rec on failure, so remove from atom
  // group
  HAremove_atom(aid);
  EXPECT_NE(FAIL, HTPendaccess(orig_ddid));

  EXPECT_NE(FAIL, Hclose(fid));
}

TEST(HExtEltTest, PathLengthExceedingMaxPath) {
  const std::string long_dir(MAX_PATH_LEN + 10, 'a');
  const std::string almost_long_dir(MAX_PATH_LEN - 5, 'b');

  // HXsetcreatedir with path exceeding MAX_PATH_LEN during build
  EXPECT_EQ(SUCCEED, HXsetcreatedir(long_dir.c_str()));
  const std::string dir = MakeTestSubdir("hextelt_longpath");
  absl::Cleanup dir_cleaner = [&dir] {
    HXsetcreatedir(nullptr);
    HXsetdir(nullptr);
    unsetenv("HDFEXTCREATEDIR");
    unsetenv("HDFEXTDIR");
    HXPshutdown();
    std::filesystem::remove_all(dir);
  };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "long.hdf").string();
  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid] { Hclose(fid); };

  // HXcreate with relative path when extcreatedir is too long -> hits line 1230
  EXPECT_EQ(FAIL, HXcreate(fid, 1020, 1, "test.dat", 0, 0));

  EXPECT_EQ(SUCCEED, HXsetcreatedir(nullptr));

  // Create an external element and reset its name to a relative path
  const std::string ext_path =
      (std::filesystem::path(dir) / "normal.dat").string();
  const int32_t aid = HXcreate(fid, 1020, 2, ext_path.c_str(), 0, 10);
  ASSERT_NE(FAIL, aid);
  auto* access_rec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_NE(nullptr, access_rec);

  sp_info_block_t sp_info;
  std::memset(&sp_info, 0, sizeof(sp_info));
  sp_info.key = SPECIAL_EXT;
  sp_info.path = const_cast<char*>("normal_rel.dat");
  EXPECT_EQ(SUCCEED, HXPreset(access_rec, &sp_info));
  EXPECT_NE(FAIL, Hendaccess(aid));

  // 1. extdir component >= MAX_PATH_LEN -> hits line 1284
  EXPECT_EQ(SUCCEED, HXsetdir(long_dir.c_str()));
  const int32_t aid_read = Hstartread(fid, 1020, 2);
  ASSERT_NE(FAIL, aid_read);
  char buf[32];
  EXPECT_EQ(FAIL, Hread(aid_read, sizeof(buf), buf));
  EXPECT_NE(FAIL, Hendaccess(aid_read));

  // 2. extdir component < MAX_PATH_LEN but component + fname > MAX_PATH_LEN ->
  // hits line 1295
  EXPECT_EQ(SUCCEED, HXsetdir(almost_long_dir.c_str()));
  const int32_t aid_read2 = Hstartread(fid, 1020, 2);
  ASSERT_NE(FAIL, aid_read2);
  EXPECT_EQ(FAIL, Hread(aid_read2, sizeof(buf), buf));
  EXPECT_NE(FAIL, Hendaccess(aid_read2));
  EXPECT_EQ(SUCCEED, HXsetdir(nullptr));

  // 3. HDFEXTCREATEDIR + fname > MAX_PATH_LEN -> hits line 1241
  HXPshutdown();
  setenv("HDFEXTCREATEDIR", long_dir.c_str(), 1);
  EXPECT_EQ(FAIL, HXcreate(fid, 1020, 3, "rel.dat", 0, 0));
  unsetenv("HDFEXTCREATEDIR");

  // 4. HDFEXTDIR component >= MAX_PATH_LEN -> hits line 1314
  HXPshutdown();
  setenv("HDFEXTDIR", long_dir.c_str(), 1);
  const int32_t aid_read3 = Hstartread(fid, 1020, 2);
  ASSERT_NE(FAIL, aid_read3);
  EXPECT_EQ(FAIL, Hread(aid_read3, sizeof(buf), buf));
  EXPECT_NE(FAIL, Hendaccess(aid_read3));

  // 5. HDFEXTDIR component + fname > MAX_PATH_LEN -> hits line 1325
  HXPshutdown();
  setenv("HDFEXTDIR", almost_long_dir.c_str(), 1);
  const int32_t aid_read4 = Hstartread(fid, 1020, 2);
  ASSERT_NE(FAIL, aid_read4);
  EXPECT_EQ(FAIL, Hread(aid_read4, sizeof(buf), buf));
  EXPECT_NE(FAIL, Hendaccess(aid_read4));
  unsetenv("HDFEXTDIR");
  HXPshutdown();

  // 6. Missing absolute path when extdir IS set -> hits lines 1264-1266
  EXPECT_EQ(SUCCEED, HXsetdir(dir.c_str()));
  sp_info.path = const_cast<char*>("/missing_dir/normal_rel.dat");
  const int32_t aid5 = HXcreate(fid, 1020, 4, ext_path.c_str(), 0, 10);
  ASSERT_NE(FAIL, aid5);
  auto* access_rec5 = static_cast<accrec_t*>(HAatom_object(aid5));
  ASSERT_NE(nullptr, access_rec5);
  EXPECT_EQ(SUCCEED, HXPreset(access_rec5, &sp_info));
  EXPECT_NE(FAIL, Hendaccess(aid5));

  const int32_t aid_read5 = Hstartread(fid, 1020, 4);
  ASSERT_NE(FAIL, aid_read5);
  EXPECT_EQ(FAIL, Hread(aid_read5, sizeof(buf), buf));
  EXPECT_NE(FAIL, Hendaccess(aid_read5));
}

TEST(HExtEltTest, RelativeFileLookupWithoutDirs) {
  const std::string dir = MakeTestSubdir("hextelt_rel_lookup");
  absl::Cleanup dir_cleaner = [&dir] {
    HXsetcreatedir(nullptr);
    HXsetdir(nullptr);
    HXPshutdown();
    std::filesystem::remove_all(dir);
  };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "rel_lookup.hdf").string();
  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid] { Hclose(fid); };

  const uint16_t tag = 1021;
  const uint16_t ref = 1;

  // Use HXsetcreatedir so HXcreate creates the file inside TempDir, but records
  // a relative filename in the HDF element.
  EXPECT_EQ(SUCCEED, HXsetcreatedir(dir.c_str()));
  const char rel_file[] = "rel_external.dat";
  const int32_t aid = HXcreate(fid, tag, ref, rel_file, 0, 10);
  ASSERT_NE(FAIL, aid);
  EXPECT_EQ(10, Hwrite(aid, 10, "0123456789"));
  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));
  std::move(closer).Cancel();
  EXPECT_EQ(SUCCEED, HXsetcreatedir(nullptr));

  // Reopen without HXsetdir -> relative lookup fails in cwd (hits line 1344)
  const int32_t fid_read = Hopen(hdf_path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid_read);
  absl::Cleanup closer_read = [fid_read] { Hclose(fid_read); };

  const int32_t aid_read = Hstartread(fid_read, tag, ref);
  ASSERT_NE(FAIL, aid_read);
  char buf[32] = {0};
  EXPECT_EQ(FAIL, Hread(aid_read, sizeof(buf), buf));
  EXPECT_NE(FAIL, Hendaccess(aid_read));

  // Now set HXsetdir so the relative file is located in dir -> reading succeeds
  EXPECT_EQ(SUCCEED, HXsetdir(dir.c_str()));
  const int32_t aid_read2 = Hstartread(fid_read, tag, ref);
  ASSERT_NE(FAIL, aid_read2);
  EXPECT_EQ(10, Hread(aid_read2, 10, buf));
  EXPECT_STREQ("0123456789", buf);
  EXPECT_NE(FAIL, Hendaccess(aid_read2));
}

TEST(HExtEltTest, EnvironmentVariablesSupport) {
  const std::string base_dir = MakeTestSubdir("hextelt_env");
  const std::string create_dir =
      (std::filesystem::path(base_dir) / "create_dir").string();
  const std::string search_dir1 =
      (std::filesystem::path(base_dir) / "search_dir1").string();
  const std::string search_dir2 =
      (std::filesystem::path(base_dir) / "search_dir2").string();

  std::filesystem::create_directories(create_dir);
  std::filesystem::create_directories(search_dir1);
  std::filesystem::create_directories(search_dir2);

  HXPshutdown();

  setenv("HDFEXTCREATEDIR", create_dir.c_str(), 1);
  // HDF4 uses '|' (DIR_PATH_SEPC 124) as the directory list separator in
  // HDFEXTDIR
  const std::string search_path = search_dir1 + "|" + search_dir2;
  setenv("HDFEXTDIR", search_path.c_str(), 1);

  absl::Cleanup env_cleaner = [&base_dir] {
    unsetenv("HDFEXTCREATEDIR");
    unsetenv("HDFEXTDIR");
    HXPshutdown();
    std::filesystem::remove_all(base_dir);
  };

  const std::string hdf_path =
      (std::filesystem::path(base_dir) / "env.hdf").string();
  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid] { Hclose(fid); };

  const uint16_t tag = 1023;
  const uint16_t ref = 1;
  const char rel_file[] = "env_target.dat";

  // HXcreate with relative path uses HDFEXTCREATEDIR environment variable (hits
  // line 1242)
  const int32_t aid = HXcreate(fid, tag, ref, rel_file, 0, 10);
  ASSERT_NE(FAIL, aid);
  EXPECT_EQ(10, Hwrite(aid, 10, "0123456789"));
  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));
  std::move(closer).Cancel();

  // Verify the external file was created inside create_dir
  EXPECT_TRUE(
      std::filesystem::exists(std::filesystem::path(create_dir) / rel_file));

  // Move external file from create_dir to search_dir2 so the search in
  // HDFEXTDIR misses in search_dir1, hits DIR_PATH_SEPC, and finds it in
  // search_dir2 (hits lines 1307-1331)
  std::filesystem::rename(std::filesystem::path(create_dir) / rel_file,
                          std::filesystem::path(search_dir2) / rel_file);

  // Reset firstinvoked to reload HDFEXTDIR from environment
  HXPshutdown();
  setenv("HDFEXTDIR", search_path.c_str(), 1);

  const int32_t fid_read = Hopen(hdf_path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid_read);
  absl::Cleanup closer_read = [fid_read] { Hclose(fid_read); };

  const int32_t aid_read = Hstartread(fid_read, tag, ref);
  ASSERT_NE(FAIL, aid_read);
  char buf[32] = {0};
  EXPECT_EQ(10, Hread(aid_read, 10, buf));
  EXPECT_STREQ("0123456789", buf);
  EXPECT_NE(FAIL, Hendaccess(aid_read));
}

TEST(HExtEltTest, HXIstaccessFailurePaths) {
  const std::string dir = MakeTestSubdir("hextelt_istacc");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "istacc.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "istacc.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid] { Hclose(fid); };

  const uint16_t tag = 1024;
  const uint16_t ref = 1;

  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 10);
  ASSERT_NE(FAIL, aid);
  EXPECT_EQ(10, Hwrite(aid, 10, "0123456789"));
  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));
  std::move(closer).Cancel();

  // Reopen in READ-ONLY mode
  const int32_t fid_ro = Hopen(hdf_path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid_ro);
  absl::Cleanup closer_ro = [fid_ro] { Hclose(fid_ro); };

  const int32_t aid_ro = Hstartread(fid_ro, tag, ref);
  ASSERT_NE(FAIL, aid_ro);
  auto* access_rec = static_cast<accrec_t*>(HAatom_object(aid_ro));
  ASSERT_NE(nullptr, access_rec);

  // Directly exercise HXIstaccess error paths via
  // access_rec->special_func->stread:
  // 1. Invalid file_id hits line 462 & cleanup lines 517-520
  auto* bad_rec1 = HIget_access_rec();
  ASSERT_NE(nullptr, bad_rec1);
  bad_rec1->special_func = access_rec->special_func;
  bad_rec1->file_id = -1;
  EXPECT_EQ(FAIL, bad_rec1->special_func->stread(bad_rec1));
  HIrelease_accrec_node(bad_rec1);

  // 2. Invalid ddid hits line 471 & cleanup lines 517-520
  auto* bad_rec2 = HIget_access_rec();
  ASSERT_NE(nullptr, bad_rec2);
  bad_rec2->special_func = access_rec->special_func;
  bad_rec2->file_id = fid_ro;
  bad_rec2->ddid = -1;
  EXPECT_EQ(FAIL, bad_rec2->special_func->stread(bad_rec2));
  HIrelease_accrec_node(bad_rec2);

  EXPECT_NE(FAIL, Hendaccess(aid_ro));
}

TEST(HExtEltTest, HXPreadSeekPastEndAndExtdirChanged) {
  const std::string dir = MakeTestSubdir("hextelt_read_edge");
  const std::string other_dir = MakeTestSubdir("hextelt_read_other");
  absl::Cleanup dir_cleaner = [&dir, &other_dir] {
    HXsetcreatedir(nullptr);
    HXsetdir(nullptr);
    HXPshutdown();
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(other_dir);
  };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "edge.hdf").string();
  const char rel_name[] = "rel_edge.dat";

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid] { Hclose(fid); };

  const uint16_t tag = 1025;
  const uint16_t ref = 1;
  EXPECT_EQ(SUCCEED, HXsetcreatedir(dir.c_str()));
  const int32_t aid = HXcreate(fid, tag, ref, rel_name, 0, 10);
  ASSERT_NE(FAIL, aid);
  EXPECT_EQ(10, Hwrite(aid, 10, "0123456789"));
  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));
  std::move(closer).Cancel();
  EXPECT_EQ(SUCCEED, HXsetcreatedir(nullptr));

  EXPECT_EQ(SUCCEED, HXsetdir(dir.c_str()));
  const int32_t fid_read = Hopen(hdf_path.c_str(), DFACC_RDWR, 0);
  ASSERT_NE(FAIL, fid_read);
  absl::Cleanup closer_read = [fid_read] { Hclose(fid_read); };

  const int32_t aid_read = Hstartread(fid_read, tag, ref);
  ASSERT_NE(FAIL, aid_read);
  absl::Cleanup closer_aid = [aid_read] { Hendaccess(aid_read); };

  auto* access_rec = static_cast<accrec_t*>(HAatom_object(aid_read));
  ASSERT_NE(nullptr, access_rec);

  // Read 1 byte so file_open is TRUE
  char buf[32];
  EXPECT_EQ(1, Hread(aid_read, 1, buf));

  // Call HXsetdir to point to other_dir (where rel_edge.dat does not exist)
  EXPECT_EQ(SUCCEED, HXsetdir(other_dir.c_str()));

  // Next Hread sees file_open && extdir_changed -> closes old file, searches
  // other_dir, and fails (hits lines 654-656 and 665-668)
  EXPECT_EQ(FAIL, Hread(aid_read, 1, buf));
  EXPECT_EQ(DFE_BADOPEN, HEvalue(1));

  // Restore valid directory
  EXPECT_EQ(SUCCEED, HXsetdir(dir.c_str()));

  // Now test HXPwrite extdir_changed branch: open aid for write
  const int32_t aid_write = Hstartwrite(fid_read, tag, ref, 0);
  ASSERT_NE(FAIL, aid_write);
  absl::Cleanup closer_write = [aid_write] { Hendaccess(aid_write); };

  // First write succeeds and opens file
  EXPECT_EQ(4, Hwrite(aid_write, 4, "test"));

  // Change extdir to other_dir (where rel_name does not exist)
  EXPECT_EQ(SUCCEED, HXsetdir(other_dir.c_str()));

  // Next write hits lines 732-734 (closes old file before reopening) and line
  // 746
  EXPECT_EQ(FAIL, Hwrite(aid_write, 4, "more"));
  EXPECT_EQ(DFE_BADOPEN, HEvalue(1));
}

TEST(HExtEltTest, HXPresetKeyCheckAndMissingAbsolute) {
  const std::string dir = MakeTestSubdir("hextelt_reset_key");
  absl::Cleanup dir_cleaner = [&dir] {
    HXsetdir(nullptr);
    HXPshutdown();
    std::filesystem::remove_all(dir);
  };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "reset.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "reset.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid] { Hclose(fid); };

  const uint16_t tag = 1026;
  const uint16_t ref = 1;
  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 10);
  ASSERT_NE(FAIL, aid);

  auto* access_rec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_NE(nullptr, access_rec);

  // HXPreset with key != SPECIAL_EXT hits line 1014
  sp_info_block_t sp_info;
  std::memset(&sp_info, 0, sizeof(sp_info));
  sp_info.key = SPECIAL_LINKED;
  sp_info.path = const_cast<char*>(ext_path.c_str());
  EXPECT_EQ(FAIL, HXPreset(access_rec, &sp_info));
  EXPECT_EQ(DFE_INTERNAL, HEvalue(1));

  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));
  std::move(closer).Cancel();

  // Create external element with nonexistent absolute path
  const int32_t fid2 = Hopen(hdf_path.c_str(), DFACC_RDWR, 0);
  ASSERT_NE(FAIL, fid2);
  absl::Cleanup closer2 = [fid2] { Hclose(fid2); };

  const uint16_t tag2 = 1027;
  const int32_t aid2 = HXcreate(fid2, tag2, ref, ext_path.c_str(), 0, 10);
  ASSERT_NE(FAIL, aid2);
  auto* access_rec2 = static_cast<accrec_t*>(HAatom_object(aid2));
  ASSERT_NE(nullptr, access_rec2);

  sp_info.key = SPECIAL_EXT;
  sp_info.path = const_cast<char*>("/nonexistent_absolute_dir_98765/file.dat");
  EXPECT_EQ(SUCCEED, HXPreset(access_rec2, &sp_info));
  EXPECT_NE(FAIL, Hendaccess(aid2));
  EXPECT_NE(FAIL, Hclose(fid2));
  std::move(closer2).Cancel();

  // Reopen and try reading when extdir is NULL -> hits lines 1262-1263
  // (DFE_FNF)
  EXPECT_EQ(SUCCEED, HXsetdir(nullptr));
  const int32_t fid_read = Hopen(hdf_path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid_read);
  absl::Cleanup closer_read = [fid_read] { Hclose(fid_read); };

  const int32_t aid_read = Hstartread(fid_read, tag2, ref);
  ASSERT_NE(FAIL, aid_read);
  char buf[32];
  EXPECT_EQ(FAIL, Hread(aid_read, sizeof(buf), buf));
  EXPECT_NE(FAIL, Hendaccess(aid_read));
}

TEST(HExtEltTest, ReadOnlyExternalFileErrors) {
  const std::string dir = MakeTestSubdir("hextelt_ro_ext");
  const std::string ext_path =
      (std::filesystem::path(dir) / "ro_ext.dat").string();
  absl::Cleanup dir_cleaner = [&dir, &ext_path] {
    std::filesystem::permissions(ext_path, std::filesystem::perms::all,
                                 std::filesystem::perm_options::replace);
    std::filesystem::remove_all(dir);
  };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "ro_ext.hdf").string();
  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid] { Hclose(fid); };

  const uint16_t tag = 1028;
  const uint16_t ref = 1;
  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 10);
  ASSERT_NE(FAIL, aid);
  EXPECT_EQ(10, Hwrite(aid, 10, "0123456789"));
  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));
  std::move(closer).Cancel();

  // Reopen in RDWR mode
  const int32_t fid_rw = Hopen(hdf_path.c_str(), DFACC_RDWR, 0);
  ASSERT_NE(FAIL, fid_rw);
  absl::Cleanup closer_rw = [fid_rw] { Hclose(fid_rw); };

  // 1. Open AID with Hstartread and perform a read so file_external is opened
  // read-only
  const int32_t aid_read = Hstartread(fid_rw, tag, ref);
  ASSERT_NE(FAIL, aid_read);
  absl::Cleanup closer_aid_read = [aid_read] { Hendaccess(aid_read); };
  char buf[32];
  EXPECT_EQ(1, Hread(aid_read, 1, buf));

  // Make external file on disk read-only
  std::filesystem::permissions(ext_path, std::filesystem::perms::owner_read,
                               std::filesystem::perm_options::replace);

  // Open second AID for write (shares special_info)
  const int32_t aid_write = Hstartwrite(fid_rw, tag, ref, 0);
  ASSERT_NE(FAIL, aid_write);
  absl::Cleanup closer_aid_write = [aid_write] { Hendaccess(aid_write); };

  // Hwrite fails on read-only file_external, and reopening with DFACC_WRITE
  // also fails
  // -> hits lines 762-764!
  EXPECT_EQ(FAIL, Hwrite(aid_write, 4, "fail"));
  EXPECT_EQ(DFE_DENIED, HEvalue(1));

  // 2. Make external file unreadable (perms::none)
  std::filesystem::permissions(ext_path, std::filesystem::perms::none,
                               std::filesystem::perm_options::replace);

  // Reopen file to close cached handles
  std::move(closer_aid_write).Cancel();
  Hendaccess(aid_write);
  std::move(closer_aid_read).Cancel();
  Hendaccess(aid_read);
  std::move(closer_rw).Cancel();
  Hclose(fid_rw);

  const int32_t fid_read2 = Hopen(hdf_path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid_read2);
  absl::Cleanup closer_read2 = [fid_read2] { Hclose(fid_read2); };

  const int32_t aid_read2 = Hstartread(fid_read2, tag, ref);
  ASSERT_NE(FAIL, aid_read2);
  // Hread calls HXIbuildfilename (stat succeeds), then HI_OPEN fails -> hits
  // lines 665-668!
  EXPECT_EQ(FAIL, Hread(aid_read2, 1, buf));
  EXPECT_EQ(DFE_BADOPEN, HEvalue(1));
  EXPECT_NE(FAIL, Hendaccess(aid_read2));
}

TEST(HExtEltTest, HXPsetaccesstypeCoverage) {
  const std::string dir = MakeTestSubdir("hextelt_acc_type");
  absl::Cleanup dir_cleaner = [&dir] {
    HXsetdir(nullptr);
    HXPshutdown();
    std::filesystem::remove_all(dir);
  };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "acctype.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "acctype.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid] { Hclose(fid); };

  const uint16_t tag = 1029;
  const uint16_t ref = 1;
  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 10);
  ASSERT_NE(FAIL, aid);

  auto* access_rec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_NE(nullptr, access_rec);

  // Null access_rec hits line 397
  EXPECT_EQ(FAIL, HXPsetaccesstype(nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // Null special_info hits line 400
  void* orig_info = access_rec->special_info;
  access_rec->special_info = nullptr;
  EXPECT_EQ(FAIL, HXPsetaccesstype(access_rec));
  EXPECT_EQ(DFE_NOSPACE, HEvalue(1));
  access_rec->special_info = orig_info;

  // Nonexistent relative path with extdir null hits line 404
  EXPECT_EQ(SUCCEED, HXsetdir(nullptr));
  sp_info_block_t sp_info;
  std::memset(&sp_info, 0, sizeof(sp_info));
  sp_info.key = SPECIAL_EXT;
  sp_info.path = const_cast<char*>("missing_rel_xyz123.dat");
  EXPECT_EQ(SUCCEED, HXPreset(access_rec, &sp_info));
  EXPECT_EQ(FAIL, HXPsetaccesstype(access_rec));
  EXPECT_EQ(DFE_BADOPEN, HEvalue(1));

  // Path that is a directory hits lines 411-414 (HI_OPEN/HI_CREATE fail)
  access_rec->access_type = DFACC_SERIAL;
  sp_info.path = const_cast<char*>(dir.c_str());
  EXPECT_EQ(SUCCEED, HXPreset(access_rec, &sp_info));
  EXPECT_EQ(FAIL, HXPsetaccesstype(access_rec));
  EXPECT_EQ(DFE_BADOPEN, HEvalue(1));

  // Restore valid path
  sp_info.path = const_cast<char*>(ext_path.c_str());
  EXPECT_EQ(SUCCEED, HXPreset(access_rec, &sp_info));
  EXPECT_NE(FAIL, Hendaccess(aid));
}

TEST(HExtEltTest, HXPwriteExtendWithInvalidDdid) {
  const std::string dir = MakeTestSubdir("hextelt_write_ext");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "extend.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "extend.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid] { Hclose(fid); };

  const uint16_t tag = 1030;
  const uint16_t ref = 1;
  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 10);
  ASSERT_NE(FAIL, aid);

  auto* access_rec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_NE(nullptr, access_rec);

  // Invalidate ddid so HTPinquire at line 780 fails when writing past length
  const atom_t orig_ddid = access_rec->ddid;
  access_rec->ddid = -1;
  const char more_data[] = "0123456789extra_past_length";
  EXPECT_EQ(FAIL, Hwrite(aid, sizeof(more_data), more_data));
  EXPECT_EQ(DFE_INTERNAL, HEvalue(1));
  access_rec->ddid = orig_ddid;

  // Negative length validation in read and write
  char sbuf[16];
  EXPECT_EQ(FAIL, access_rec->special_func->read(access_rec, -1, sbuf));
  EXPECT_EQ(DFE_RANGE, HEvalue(1));
  EXPECT_EQ(FAIL, access_rec->special_func->write(access_rec, -1, "x"));
  EXPECT_EQ(DFE_RANGE, HEvalue(1));

  // Null extern_file_name when reopening external file
  auto* info = static_cast<TestExtInfo*>(access_rec->special_info);
  char* saved_name = info->extern_file_name;
  info->extern_file_name = nullptr;
  info->file_open = FALSE;
  EXPECT_EQ(FAIL, Hread(aid, 1, sbuf));
  EXPECT_EQ(DFE_BADOPEN, HEvalue(1));
  EXPECT_EQ(FAIL, Hwrite(aid, 1, "x"));
  EXPECT_EQ(DFE_BADOPEN, HEvalue(1));
  EXPECT_EQ(FAIL, HXPsetaccesstype(access_rec));
  EXPECT_EQ(DFE_BADOPEN, HEvalue(1));
  info->extern_file_name = saved_name;

  EXPECT_NE(FAIL, Hendaccess(aid));
}

TEST(HExtEltTest, HXPresetErrors) {
  const std::string dir = MakeTestSubdir("hextelt_reset_err");
  absl::Cleanup dir_cleaner = [&dir] { std::filesystem::remove_all(dir); };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "reset_err.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "reset_err.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid] { Hclose(fid); };

  const uint16_t tag = 1032;
  const uint16_t ref = 1;
  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 10);
  ASSERT_NE(FAIL, aid);

  auto* access_rec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_NE(nullptr, access_rec);

  sp_info_block_t sp_info;
  std::memset(&sp_info, 0, sizeof(sp_info));
  sp_info.key = SPECIAL_EXT;
  sp_info.path = const_cast<char*>(ext_path.c_str());

  // Invalidate ddid -> HTPupdate at line 1059 fails -> hits line 1060
  const atom_t orig_ddid = access_rec->ddid;
  access_rec->ddid = -1;
  EXPECT_EQ(FAIL, HXPreset(access_rec, &sp_info));
  EXPECT_EQ(DFE_INTERNAL, HEvalue(1));
  access_rec->ddid = orig_ddid;

  // Negative extern_offset causes HI_SEEK to fail in Hread and Hwrite -> hits
  // line 676 and 754
  sp_info.offset = -100;
  EXPECT_EQ(SUCCEED, HXPreset(access_rec, &sp_info));
  char buf[32];
  EXPECT_EQ(FAIL, Hread(aid, 1, buf));
  EXPECT_EQ(DFE_SEEKERROR, HEvalue(1));
  EXPECT_EQ(FAIL, Hwrite(aid, 1, "x"));
  EXPECT_EQ(DFE_SEEKERROR, HEvalue(1));
  sp_info.offset = 0;
  EXPECT_EQ(SUCCEED, HXPreset(access_rec, &sp_info));

  EXPECT_NE(FAIL, Hendaccess(aid));
}

TEST(HExtEltTest, RelativeLookupInCwd) {
  const std::string dir = MakeTestSubdir("hextelt_cwd_lookup");
  absl::Cleanup dir_cleaner = [&dir] {
    HXsetdir(nullptr);
    HXPshutdown();
    std::filesystem::remove_all(dir);
  };

  const std::string hdf_path =
      (std::filesystem::path(dir) / "cwd.hdf").string();
  const std::string ext_path =
      (std::filesystem::path(dir) / "cwd.dat").string();

  const int32_t fid = Hopen(hdf_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup closer = [fid] { Hclose(fid); };

  const uint16_t tag = 1033;
  const uint16_t ref = 1;
  const int32_t aid = HXcreate(fid, tag, ref, ext_path.c_str(), 0, 10);
  ASSERT_NE(FAIL, aid);

  auto* access_rec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_NE(nullptr, access_rec);

  // Set external filename to "." which exists in cwd (stat succeeds at line
  // 1338), but HI_OPEN fails on a directory, hitting lines 1340-1341 and line
  // 668
  sp_info_block_t sp_info;
  std::memset(&sp_info, 0, sizeof(sp_info));
  sp_info.key = SPECIAL_EXT;
  sp_info.path = const_cast<char*>(".");
  EXPECT_EQ(SUCCEED, HXPreset(access_rec, &sp_info));
  EXPECT_NE(FAIL, Hendaccess(aid));
  EXPECT_NE(FAIL, Hclose(fid));
  std::move(closer).Cancel();

  EXPECT_EQ(SUCCEED, HXsetdir(nullptr));
  const int32_t fid_read = Hopen(hdf_path.c_str(), DFACC_READ, 0);
  ASSERT_NE(FAIL, fid_read);
  absl::Cleanup closer_read = [fid_read] { Hclose(fid_read); };

  const int32_t aid_read = Hstartread(fid_read, tag, ref);
  ASSERT_NE(FAIL, aid_read);
  char buf[32];
  EXPECT_EQ(FAIL, Hread(aid_read, 1, buf));
  EXPECT_NE(FAIL, Hendaccess(aid_read));
}

}  // namespace
