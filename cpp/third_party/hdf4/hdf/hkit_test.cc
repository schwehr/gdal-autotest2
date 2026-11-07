#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/atom_priv.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"
#include "third_party/hdf4/hdf/src/hfile_priv.h"

namespace {

class HkitTest : public ::testing::Test {
 protected:
  void SetUp() override {
    HEclear();
    HAinit_group(FIDGROUP, 64);
  }

  void TearDown() override {
    HAdestroy_group(FIDGROUP);
    HAshutdown();
    HEclear();
  }
};

TEST_F(HkitTest, C2FStrPadsWithSpaces) {
  char buf[32] = "hello";
  EXPECT_EQ(SUCCEED, HDc2fstr(buf, 10));
  EXPECT_EQ(0, std::memcmp(buf, "hello     ", 10));

  // When len <= strlen(str), string is unchanged.
  char buf2[32] = "hello";
  EXPECT_EQ(SUCCEED, HDc2fstr(buf2, 5));
  EXPECT_STREQ("hello", buf2);

  EXPECT_EQ(SUCCEED, HDc2fstr(buf2, 2));
  EXPECT_STREQ("hello", buf2);
}

TEST_F(HkitTest, F2CStringStripsTrailingBlanks) {
  char fstr1[] = "hello   ";
  char* cstr1 = HDf2cstring(fstr1, 8);
  ASSERT_NE(nullptr, cstr1);
  EXPECT_STREQ("hello", cstr1);
  std::free(cstr1);

  char fstr2[] = "world";
  char* cstr2 = HDf2cstring(fstr2, 5);
  ASSERT_NE(nullptr, cstr2);
  EXPECT_STREQ("world", cstr2);
  std::free(cstr2);

  char fstr3[] = "hello world  ";
  char* cstr3 = HDf2cstring(fstr3, 13);
  ASSERT_NE(nullptr, cstr3);
  EXPECT_STREQ("hello world", cstr3);
  std::free(cstr3);

  char fstr4[] = "    ";
  char* cstr4 = HDf2cstring(fstr4, 4);
  ASSERT_NE(nullptr, cstr4);
  EXPECT_STREQ("", cstr4);
  std::free(cstr4);

  char fstr5[] = "";
  char* cstr5 = HDf2cstring(fstr5, 0);
  ASSERT_NE(nullptr, cstr5);
  EXPECT_STREQ("", cstr5);
  std::free(cstr5);
}

TEST_F(HkitTest, PackFStringCopiesAndPads) {
  char dest[16] = {};

  char src1[] = "abc";
  EXPECT_EQ(SUCCEED, HDpackFstring(src1, dest, 6));
  EXPECT_EQ(0, std::memcmp(dest, "abc   ", 6));

  char src2[] = "abcdefgh";
  EXPECT_EQ(SUCCEED, HDpackFstring(src2, dest, 4));
  EXPECT_EQ(0, std::memcmp(dest, "abcd", 4));

  char src3[] = "";
  EXPECT_EQ(SUCCEED, HDpackFstring(src3, dest, 5));
  EXPECT_EQ(0, std::memcmp(dest, "     ", 5));

  EXPECT_EQ(SUCCEED, HDpackFstring(src1, dest, 0));
}

TEST_F(HkitTest, GetTagDesc) {
  EXPECT_STREQ("DFTAG_NULL", HDgettagdesc(DFTAG_NULL));
  EXPECT_STREQ("DFTAG_LINKED", HDgettagdesc(DFTAG_LINKED));
  EXPECT_STREQ("DFTAG_VERSION", HDgettagdesc(DFTAG_VERSION));
  EXPECT_STREQ("DFTAG_COMPRESSED", HDgettagdesc(DFTAG_COMPRESSED));
  EXPECT_STREQ("DFTAG_CHUNK", HDgettagdesc(DFTAG_CHUNK));
  EXPECT_STREQ("DFTAG_FID", HDgettagdesc(DFTAG_FID));
  EXPECT_STREQ("DFTAG_FD", HDgettagdesc(DFTAG_FD));
  EXPECT_STREQ("DFTAG_RLE", HDgettagdesc(DFTAG_RLE));
  EXPECT_STREQ("DFTAG_VG", HDgettagdesc(DFTAG_VG));
  EXPECT_STREQ("DFTAG_SDG", HDgettagdesc(DFTAG_SDG));
  EXPECT_STREQ("DFTAG_GREYJPEG5", HDgettagdesc(DFTAG_GREYJPEG5));

  EXPECT_EQ(nullptr, HDgettagdesc(0xFFFF));
}

TEST_F(HkitTest, GetTagsName) {
  char* name1 = HDgettagsname(DFTAG_NULL);
  ASSERT_NE(nullptr, name1);
  EXPECT_STREQ("No Data", name1);
  std::free(name1);

  char* name2 = HDgettagsname(DFTAG_RLE);
  ASSERT_NE(nullptr, name2);
  EXPECT_STREQ("Run Length Encoding", name2);
  std::free(name2);

  // Special tag
  uint16 special_null = MKSPECIALTAG(DFTAG_NULL);
  char* name3 = HDgettagsname(special_null);
  ASSERT_NE(nullptr, name3);
  EXPECT_STREQ("Special No Data", name3);
  std::free(name3);

  // Unknown normal tag
  EXPECT_EQ(nullptr, HDgettagsname(0x0FFF));

  // Unknown special tag returns "Special "
  uint16 unknown_special = 0x4FFF;
  char* name4 = HDgettagsname(unknown_special);
  ASSERT_NE(nullptr, name4);
  EXPECT_STREQ("Special ", name4);
  std::free(name4);
}

TEST_F(HkitTest, GetTagNum) {
  EXPECT_EQ(DFTAG_NULL, HDgettagnum("No Data"));
  EXPECT_EQ(DFTAG_LINKED, HDgettagnum("Linked Blocks Indicator"));
  EXPECT_EQ(DFTAG_RLE, HDgettagnum("Run Length Encoding"));
  EXPECT_EQ(FAIL, HDgettagnum("NonExistentTag"));
}

TEST_F(HkitTest, GetNTDesc) {
  auto check_nt = [](int32 nt, const char* expected) {
    char* desc = HDgetNTdesc(nt);
    ASSERT_NE(nullptr, desc) << "Failed for nt: " << nt;
    EXPECT_STREQ(expected, desc);
    std::free(desc);
  };

  check_nt(DFNT_NATIVE | DFNT_INT32, "native format 32-bit signed integer");
  check_nt(DFNT_CUSTOM | DFNT_FLOAT32, "custom format 32-bit floating point");
  check_nt(DFNT_LITEND | DFNT_UINT16,
           "little-endian format 16-bit unsigned integer");

  check_nt(DFNT_NONE, "number-type not set");
  check_nt(DFNT_FLOAT32, "32-bit floating point");
  check_nt(DFNT_FLOAT64, "64-bit floating point");
  check_nt(DFNT_FLOAT128, "128-bit floating point");
  check_nt(DFNT_INT8, "8-bit signed integer");
  check_nt(DFNT_UINT8, "8-bit unsigned integer");
  check_nt(DFNT_INT16, "16-bit signed integer");
  check_nt(DFNT_UINT16, "16-bit unsigned integer");
  check_nt(DFNT_INT32, "32-bit signed integer");
  check_nt(DFNT_UINT32, "32-bit unsigned integer");
  check_nt(DFNT_INT64, "64-bit signed integer");
  check_nt(DFNT_UINT64, "64-bit unsigned integer");
  check_nt(DFNT_INT128, "128-bit signed integer");
  check_nt(DFNT_UINT128, "128-bit unsigned integer");
  check_nt(DFNT_CHAR8, "8-bit signed char");
  check_nt(DFNT_UCHAR8, "8-bit unsigned char");
  check_nt(DFNT_CHAR16, "16-bit signed char");
  check_nt(DFNT_UCHAR16, "16-bit unsigned char");

  EXPECT_EQ(nullptr, HDgetNTdesc(0x0EEE));
}

TEST_F(HkitTest, Flush) {
  // Invalid FID
  EXPECT_EQ(FAIL, HDflush(-1));

  // Atom with refcount == 0 (BADFREC)
  filerec_t bad_rec = {};
  bad_rec.refcount = 0;
  int32 bad_fid = HAregister_atom(FIDGROUP, &bad_rec);
  ASSERT_NE(FAIL, bad_fid);
  EXPECT_EQ(FAIL, HDflush(bad_fid));
  EXPECT_NE(nullptr, HAremove_atom(bad_fid));

  // Valid FID with open file
  FILE* tmp = std::tmpfile();
  ASSERT_NE(nullptr, tmp);

  filerec_t rec = {};
  rec.refcount = 1;
  rec.file = tmp;
  int32 fid = HAregister_atom(FIDGROUP, &rec);
  ASSERT_NE(FAIL, fid);

  EXPECT_EQ(SUCCEED, HDflush(fid));

  EXPECT_NE(nullptr, HAremove_atom(fid));
  std::fclose(tmp);
}

TEST_F(HkitTest, FidToName) {
  // Invalid FID
  EXPECT_EQ(nullptr, HDfidtoname(-1));

  // Valid FID
  char path[] = "/dummy/path/test.hdf";
  filerec_t rec = {};
  rec.refcount = 1;
  rec.path = path;
  int32 fid = HAregister_atom(FIDGROUP, &rec);
  ASSERT_NE(FAIL, fid);

  EXPECT_STREQ("/dummy/path/test.hdf", HDfidtoname(fid));

  EXPECT_NE(nullptr, HAremove_atom(fid));
}

}  // namespace
