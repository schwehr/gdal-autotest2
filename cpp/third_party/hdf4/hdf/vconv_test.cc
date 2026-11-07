#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"
#include "third_party/hdf4/hdf/src/vg.h"
#include "third_party/hdf4/hdf/src/vg_priv.h"

namespace {

class VconvTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }

  void TearDown() override { HEclear(); }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }
};

TEST_F(VconvTest, MapFromOldTypes) {
  // 1: LOCAL_CHARTYPE -> DFNT_CHAR
  EXPECT_EQ(DFNT_CHAR, map_from_old_types(1));
  // 5: LOCAL_BYTETYPE -> DFNT_INT8
  EXPECT_EQ(DFNT_INT8, map_from_old_types(5));
  // 6: LOCAL_SHORTTYPE -> DFNT_INT16
  EXPECT_EQ(DFNT_INT16, map_from_old_types(6));
  // 2: LOCAL_INTTYPE -> DFNT_INT16
  EXPECT_EQ(DFNT_INT16, map_from_old_types(2));
  // 4: LOCAL_LONGTYPE -> DFNT_INT32
  EXPECT_EQ(DFNT_INT32, map_from_old_types(4));
  // 3: LOCAL_FLOATTYPE -> DFNT_FLOAT32
  EXPECT_EQ(DFNT_FLOAT32, map_from_old_types(3));
  // 7: LOCAL_DOUBLETYPE -> DFNT_FLOAT64
  EXPECT_EQ(DFNT_FLOAT64, map_from_old_types(7));

  // Unknown type passes through
  EXPECT_EQ(99, map_from_old_types(99));
}

TEST_F(VconvTest, NonExistentFileFails) {
  char bad_path[] = "/nonexistent/directory/bad_file.hdf";
  EXPECT_EQ(FAIL, vcheckcompat(bad_path));
  EXPECT_EQ(FAIL, vmakecompat(bad_path));
}

TEST_F(VconvTest, FreshFileIsAlreadyCompatible) {
  std::string path = GetTempFilePath("fresh_compat.hdf");
  std::filesystem::remove(path);

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  EXPECT_EQ(SUCCEED, Hclose(fid));

  // Empty file has no old tags, so vcheckcompat returns 1 (compatible).
  EXPECT_EQ(1, vcheckcompat(path.data()));

  EXPECT_EQ(1, vmakecompat(path.data()));

  std::filesystem::remove(path);
}

TEST_F(VconvTest, VicheckcompatAllBranches) {
  // 1. File with OLD tags only: vcheckcompat returns 0
  std::string path_old = GetTempFilePath("old_only.hdf");
  std::filesystem::remove(path_old);
  int32 fid = Hopen(path_old.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  uint8_t dummy = 0;
  ASSERT_NE(FAIL, Hputelement(fid, OLD_VGDESCTAG, 1, &dummy, 1));
  ASSERT_NE(FAIL, Hputelement(fid, OLD_VSDESCTAG, 2, &dummy, 1));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  EXPECT_EQ(0, vcheckcompat(path_old.data()));

  // 2. File with both OLD and NEW tags: vcheckcompat returns 1
  std::string path_both = GetTempFilePath("both_tags.hdf");
  std::filesystem::remove(path_both);
  fid = Hopen(path_both.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Hputelement(fid, OLD_VGDESCTAG, 1, &dummy, 1));
  ASSERT_NE(FAIL, Hputelement(fid, NEW_VGDESCTAG, 2, &dummy, 1));
  ASSERT_NE(FAIL, Hputelement(fid, NEW_VSDESCTAG, 3, &dummy, 1));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  EXPECT_EQ(1, vcheckcompat(path_both.data()));

  // 3. File with only NEW tags: vcheckcompat returns 1
  std::string path_new = GetTempFilePath("new_only.hdf");
  std::filesystem::remove(path_new);
  fid = Hopen(path_new.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  ASSERT_NE(FAIL, Hputelement(fid, NEW_VGDESCTAG, 1, &dummy, 1));
  EXPECT_EQ(SUCCEED, Hclose(fid));

  EXPECT_EQ(1, vcheckcompat(path_new.data()));

  std::filesystem::remove(path_old);
  std::filesystem::remove(path_both);
  std::filesystem::remove(path_new);
}

TEST_F(VconvTest, ConvertOldVGroupAndVData) {
  std::string path = GetTempFilePath("old_compat.hdf");
  std::filesystem::remove(path);

  int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);

  // 1. Build an OLD_VGDESCTAG descriptor buffer:
  // nvelt = 3 (includes OLD_VGDESCTAG, OLD_VSDESCTAG, and DFTAG_NULL to test
  // error branch)
  std::vector<uint8_t> vg_buf;
  auto append16 = [&](uint16_t val) {
    vg_buf.push_back((val >> 8) & 0xFF);
    vg_buf.push_back(val & 0xFF);
  };
  append16(3);  // nvelt = 3
  append16(OLD_VGDESCTAG);
  append16(OLD_VSDESCTAG);
  append16(DFTAG_NULL);  // Non-vset tag triggers DFE_NOTINSET branch in vconv.c
  append16(101);         // ref 1
  append16(102);         // ref 2
  append16(103);         // ref 3
  const char vg_name[] = "OldVGroup";
  vg_buf.insert(vg_buf.end(), vg_name, vg_name + sizeof(vg_name));

  ASSERT_NE(FAIL, Hputelement(fid, OLD_VGDESCTAG, 100, vg_buf.data(),
                              static_cast<int32>(vg_buf.size())));

  // 2. Build an OLD_VSDESCTAG descriptor buffer with 2 fields:
  // Field 0: valid LOCAL_LONGTYPE (4)
  // Field 1: invalid type (99) to exercise VSIZEOF out-of-bounds branch
  std::vector<uint8_t> vs_buf;
  auto append32 = [&](uint32_t val) {
    vs_buf.push_back((val >> 24) & 0xFF);
    vs_buf.push_back((val >> 16) & 0xFF);
    vs_buf.push_back((val >> 8) & 0xFF);
    vs_buf.push_back(val & 0xFF);
  };

  auto vs_append16 = [&](uint16_t val) {
    vs_buf.push_back((val >> 8) & 0xFF);
    vs_buf.push_back(val & 0xFF);
  };

  vs_append16(0);  // interlace
  append32(5);     // nvertices
  vs_append16(8);  // ivsize
  vs_append16(2);  // n = 2 fields

  // types:
  vs_append16(1);   // LOCAL_CHARTYPE (1 <= 3 to hit line 86 of VSIZEOF)
  vs_append16(99);  // invalid type (exercises VSIZEOF out-of-bounds)

  // isizes:
  vs_append16(4);
  vs_append16(4);

  // offsets:
  vs_append16(0);
  vs_append16(4);

  // orders:
  vs_append16(1);
  vs_append16(1);

  // field names:
  const char f0[] = "f0";
  vs_buf.insert(vs_buf.end(), f0, f0 + sizeof(f0));
  const char f1[] = "f1";
  vs_buf.insert(vs_buf.end(), f1, f1 + sizeof(f1));

  // vsname:
  const char vs_name[] = "OldVData";
  vs_buf.insert(vs_buf.end(), vs_name, vs_name + sizeof(vs_name));

  // Add dummy raw data element with OLD_VSDATATAG
  uint8_t raw_data[20] = {1, 2, 3, 4};
  ASSERT_NE(FAIL,
            Hputelement(fid, OLD_VSDATATAG, 200, raw_data, sizeof(raw_data)));

  // Write descriptor with OLD_VSDESCTAG
  ASSERT_NE(FAIL, Hputelement(fid, OLD_VSDESCTAG, 200, vs_buf.data(),
                              static_cast<int32>(vs_buf.size())));

  EXPECT_EQ(SUCCEED, Hclose(fid));

  // 3. Before conversion: file has old tags and no new tags -> vcheckcompat
  // returns 0.
  EXPECT_EQ(0, vcheckcompat(path.data()));

  // 4. Run conversion via vmakecompat -> returns 1.
  EXPECT_EQ(1, vmakecompat(path.data()));

  // 5. After conversion: file has both old and new tags -> vcheckcompat
  // returns 1.
  EXPECT_EQ(1, vcheckcompat(path.data()));

  std::filesystem::remove(path);
}

}  // namespace
