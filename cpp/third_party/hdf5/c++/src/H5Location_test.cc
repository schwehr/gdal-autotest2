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
// Unit tests for third_party/hdf5/c++/src/H5Location.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5Location.cpp

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

herr_t CollectGroupMember(hid_t /*loc_id*/, const char* name, void* opdata) {
  auto* names = static_cast<std::vector<std::string>*>(opdata);
  names->emplace_back(name);
  return 0;
}

TEST(H5LocationTest, LinksGroupsDataSetsCommentsRefsAndQueries) {
  H5::H5File file = CreateInMemoryFile("loc_mem.h5");
  EXPECT_FALSE(file.getFileName().empty());
  file.flush(H5F_SCOPE_LOCAL);

  // Create groups via both LinkCreatPropList and size_hint overloads.
  H5::Group g_lcpl = file.createGroup("g_lcpl", H5::LinkCreatPropList::DEFAULT);
  H5::Group g_lcpl_s = file.createGroup(H5std_string("g_lcpl_s"),
                                        H5::LinkCreatPropList::DEFAULT);
  H5::Group g_hint0 = file.createGroup("g_hint0", static_cast<size_t>(0));
  H5::Group g_hint64 =
      file.createGroup(H5std_string("g_hint64"), static_cast<size_t>(64));

  EXPECT_TRUE(file.nameExists("g_lcpl"));
  EXPECT_TRUE(file.nameExists(H5std_string("g_lcpl_s")));
  EXPECT_FALSE(file.nameExists("no_such_link"));
  EXPECT_TRUE(file.exists("g_hint0"));
  EXPECT_TRUE(file.exists(H5std_string("g_hint64")));

  EXPECT_NE(file.openGroup("g_lcpl").getId(), H5I_INVALID_HID);
  EXPECT_NE(file.openGroup(H5std_string("g_hint0")).getId(), H5I_INVALID_HID);

  // Create/open datasets and a committed datatype.
  const hsize_t dim = 4;
  H5::DataSpace space(1, &dim);
  H5::DataSet ds =
      g_hint64.createDataSet("ds_c", H5::PredType::NATIVE_INT32, space);
  H5::DataSet ds_s = file.createDataSet(H5std_string("ds_s"),
                                        H5::PredType::NATIVE_INT32, space);
  EXPECT_NE(g_hint64.openDataSet("ds_c").getId(), H5I_INVALID_HID);
  EXPECT_NE(file.openDataSet(H5std_string("ds_s")).getId(), H5I_INVALID_HID);

  H5::IntType named_dt(H5::PredType::NATIVE_INT32);
  named_dt.commit(file, "z_named_dt");

  // Comments: all 4 setComment overloads, 3 getComment overloads,
  // removeComment.
  file.setComment("g_lcpl", "hello_comment");
  file.setComment(H5std_string("g_lcpl_s"), H5std_string("second_comment"));
  g_hint0.setComment("self_c");
  g_hint64.setComment(H5std_string("self_s"));

  EXPECT_EQ(file.getComment("g_lcpl"), "hello_comment");
  EXPECT_EQ(file.getComment(H5std_string("g_lcpl_s")), "second_comment");
  EXPECT_EQ(file.getComment("g_lcpl", 5), "hello");

  std::array<char, 8> cbuf{};
  EXPECT_EQ(file.getComment("g_lcpl", cbuf.size(), cbuf.data()), 13);
  EXPECT_STREQ(cbuf.data(), "hello_c");
  EXPECT_EQ(file.getComment("g_hint0"), "self_c");

  file.removeComment("g_lcpl");
  EXPECT_EQ(file.getComment("g_lcpl"), "");
  file.removeComment(H5std_string("g_lcpl_s"));
  EXPECT_EQ(file.getComment("g_lcpl_s"), "");

  // Links: soft, hard(Group), hard(hid_t), deprecated link(H5L_type_t), copy,
  // move, move(deprecated), unlink.
  file.link("/ds_s", "soft_c");
  file.link(H5std_string("/ds_s"), H5std_string("soft_s"));
  file.link("ds_s", g_lcpl, "hard_in_grp_c");
  file.link(H5std_string("ds_s"), g_lcpl, H5std_string("hard_in_grp_s"));
  file.link("ds_s", H5L_SAME_LOC, "hard_same_c");
  file.link(H5std_string("ds_s"), H5L_SAME_LOC, H5std_string("hard_same_s"));
  file.link(H5L_TYPE_HARD, "ds_s", "dep_hard");
  file.link(H5L_TYPE_SOFT, H5std_string("/ds_s"), H5std_string("dep_soft"));

  EXPECT_EQ(file.getLinkInfo("soft_c").type, H5L_TYPE_SOFT);
  EXPECT_EQ(file.getLinkInfo(H5std_string("soft_c")).type, H5L_TYPE_SOFT);
  EXPECT_EQ(file.getLinkval("soft_c"), "/ds_s");
  EXPECT_EQ(file.getLinkval(H5std_string("soft_s"), 6), "/ds_s");

  file.copyLink("soft_c", g_lcpl, "copied_grp_c");
  file.copyLink(H5std_string("soft_c"), g_lcpl, H5std_string("copied_grp_s"));
  file.copyLink("soft_c", "copied_same_c");
  file.copyLink(H5std_string("soft_c"), H5std_string("copied_same_s"));

  file.moveLink("copied_same_c", g_lcpl, "moved_grp_c");
  file.moveLink(H5std_string("copied_same_s"), g_lcpl,
                H5std_string("moved_grp_s"));
  file.moveLink("dep_hard", "renamed_hard_c");
  file.moveLink(H5std_string("dep_soft"), H5std_string("renamed_soft_s"));
  file.move("renamed_hard_c", "moved_old_c");
  file.move(H5std_string("renamed_soft_s"), H5std_string("moved_old_s"));

  file.unlink("moved_old_c");
  file.unlink(H5std_string("moved_old_s"));
  EXPECT_FALSE(file.nameExists("moved_old_c"));

  // Mount & unmount another in-memory file on a dedicated mount point.
  H5::Group mnt_pt = file.createGroup("mnt");
  H5::H5File child_file = CreateInMemoryFile("child_mem.h5");
  child_file.createGroup("inside_child");
  file.mount("mnt", child_file, H5::PropList::DEFAULT);
  EXPECT_TRUE(file.nameExists("/mnt/inside_child"));
  file.unmount("mnt");

  H5::Group mnt_pt2 = file.createGroup("mnt2");
  file.mount(H5std_string("mnt2"), child_file, H5::PropList::DEFAULT);
  EXPECT_TRUE(file.nameExists("/mnt2/inside_child"));
  file.unmount(H5std_string("mnt2"));
  mnt_pt.close();
  mnt_pt2.close();
  child_file.close();

  // References: object reference + dataset region reference, dereference,
  // getRefObjType, getObjType, getRegion.
  const std::array<int32_t, 4> vals = {1, 2, 3, 4};
  ds_s.write(vals.data(), H5::PredType::NATIVE_INT32);
  hobj_ref_t oref1{};
  hobj_ref_t oref2{};
  file.reference(&oref1, "/ds_s", H5R_OBJECT);
  file.reference(&oref2, H5std_string("/ds_s"), H5R_OBJECT);
  EXPECT_EQ(file.getRefObjType(&oref1), H5O_TYPE_DATASET);
  EXPECT_EQ(file.getObjType(&oref1), H5G_DATASET);

  H5::DataSpace sel_space = ds_s.getSpace();
  const hsize_t count = 2;
  const hsize_t start = 1;
  sel_space.selectHyperslab(H5S_SELECT_SET, &count, &start);
  hdset_reg_ref_t rref1{};
  hdset_reg_ref_t rref2{};
  file.reference(&rref1, "/ds_s", sel_space, H5R_DATASET_REGION);
  file.reference(&rref2, H5std_string("/ds_s"), sel_space, H5R_DATASET_REGION);
  EXPECT_EQ(file.getRegion(&rref1).getSelectNpoints(), 2);

  hobj_ref_t grp_ref{};
  file.reference(&grp_ref, "/g_lcpl", H5R_OBJECT);
  H5::Group deref_g;
  deref_g.dereference(file, &grp_ref, H5R_OBJECT);
  EXPECT_NE(deref_g.getId(), H5I_INVALID_HID);
  deref_g.dereference(file, &grp_ref, H5R_OBJECT, H5::PropList::DEFAULT);
  EXPECT_NE(deref_g.getId(), H5I_INVALID_HID);

  // Native & generic object info overloads (self, name, idx, H5G_stat_t).
  H5O_native_info_t ninfo{};
  g_lcpl.getNativeObjinfo(ninfo);
  file.getNativeObjinfo("g_lcpl", ninfo);
  file.getNativeObjinfo(H5std_string("g_lcpl"), ninfo);
  file.getNativeObjinfo(".", H5_INDEX_NAME, H5_ITER_INC, 0, ninfo);
  file.getNativeObjinfo(H5std_string("."), H5_INDEX_NAME, H5_ITER_INC, 0,
                        ninfo);

  H5O_info2_t oinfo{};
  g_lcpl.getObjinfo(oinfo);
  file.getObjinfo("g_lcpl", oinfo);
  file.getObjinfo(H5std_string("g_lcpl"), oinfo);
  file.getObjinfo(".", H5_INDEX_NAME, H5_ITER_INC, 0, oinfo);
  file.getObjinfo(H5std_string("."), H5_INDEX_NAME, H5_ITER_INC, 0, oinfo);

  H5G_stat_t stat{};
  file.getObjinfo("soft_c", false, stat);
  EXPECT_EQ(stat.type, H5G_LINK);
  file.getObjinfo(H5std_string("soft_c"), true, stat);
  EXPECT_EQ(stat.type, H5G_DATASET);
  file.getObjinfo("g_lcpl", stat);
  file.getObjinfo(H5std_string("g_lcpl"), stat);

  // childObjType, childObjVersion, getNumObjs, getObjnameByIdx,
  // getObjTypeByIdx, iterateElems.
  EXPECT_EQ(file.childObjType("g_lcpl"), H5O_TYPE_GROUP);
  EXPECT_EQ(file.childObjType(H5std_string("ds_s")), H5O_TYPE_DATASET);
  EXPECT_EQ(file.childObjType("z_named_dt"), H5O_TYPE_NAMED_DATATYPE);

  EXPECT_GE(file.getNumObjs(), 7u);
  const std::string idx0_name = file.getObjnameByIdx(0);
  EXPECT_FALSE(idx0_name.empty());
  std::array<char, 64> oname_buf{};
  EXPECT_GT(file.getObjnameByIdx(0, oname_buf.data(), oname_buf.size()), 0);
  H5std_string oname_str;
  EXPECT_GT(file.getObjnameByIdx(0, oname_str, 16), 0);

  // Locate indices for each object type in a clean subgroup to cover all
  // childObjType / getObjTypeByIdx switch branches cleanly.
  H5::Group typed_grp = file.createGroup("typed_grp");
  typed_grp.createGroup("a_grp");
  typed_grp.createDataSet("b_ds", H5::PredType::NATIVE_INT32, space).close();
  H5::IntType t_sub(H5::PredType::NATIVE_INT32);
  t_sub.commit(typed_grp, "c_dt");
  typed_grp.link("/ds_s", "d_soft");

  EXPECT_EQ(typed_grp.childObjType(static_cast<hsize_t>(0)), H5O_TYPE_GROUP);
  EXPECT_EQ(typed_grp.childObjType(static_cast<hsize_t>(1)), H5O_TYPE_DATASET);
  EXPECT_EQ(typed_grp.childObjType(static_cast<hsize_t>(2)),
            H5O_TYPE_NAMED_DATATYPE);

  const unsigned ver_c = file.childObjVersion("g_lcpl");
  const unsigned ver_s = file.childObjVersion(H5std_string("g_lcpl"));
  EXPECT_EQ(ver_c, ver_s);
  EXPECT_TRUE(ver_c == H5O_VERSION_1 || ver_c == H5O_VERSION_2);

  // Pre-initialize type_buf because H5Location.cpp:2102 reads type_name.
  std::array<char, 32> type_buf{};
  H5std_string type_str;
  EXPECT_EQ(typed_grp.getObjTypeByIdx(0), H5G_GROUP);
  EXPECT_EQ(typed_grp.getObjTypeByIdx(0, type_buf.data()), H5G_GROUP);
  EXPECT_EQ(typed_grp.getObjTypeByIdx(0, type_str), H5G_GROUP);
  EXPECT_EQ(type_str, "group");
  EXPECT_EQ(typed_grp.getObjTypeByIdx(1, type_str), H5G_DATASET);
  EXPECT_EQ(type_str, "dataset");
  EXPECT_EQ(typed_grp.getObjTypeByIdx(2, type_str), H5G_TYPE);
  EXPECT_EQ(type_str, "datatype");
  // H5Gget_objtype_by_idx resolves the soft link to its target dataset.
  EXPECT_EQ(typed_grp.getObjTypeByIdx(3, type_str), H5G_DATASET);
  EXPECT_EQ(type_str, "dataset");

  std::vector<std::string> iterated;
  int iter_idx = 0;
  EXPECT_EQ(
      file.iterateElems("typed_grp", &iter_idx, CollectGroupMember, &iterated),
      0);
  EXPECT_THAT(iterated, ElementsAre("a_grp", "b_ds", "c_dt", "d_soft"));
  iterated.clear();
  iter_idx = 0;
  EXPECT_EQ(file.iterateElems(H5std_string("typed_grp"), &iter_idx,
                              CollectGroupMember, &iterated),
            0);
  EXPECT_THAT(iterated, ElementsAre("a_grp", "b_ds", "c_dt", "d_soft"));
}

TEST(H5LocationTest, ErrorPathsAndLocationException) {
  H5::H5File file = CreateInMemoryFile("loc_err.h5");
  H5::Group grp = file.createGroup("grp");
  H5::DataSpace scalar(H5S_SCALAR);
  H5::DataSet ds = file.createDataSet("ds", H5::PredType::NATIVE_INT32, scalar);

  // Closed DataSet uses base H5Location::throwException -> LocationException.
  H5::DataSet closed_ds;
  ScopedQuietHdf5Errors quiet;

  EXPECT_THROW(closed_ds.nameExists("x"), H5::LocationException);
  EXPECT_THROW(closed_ds.flush(H5F_SCOPE_LOCAL), H5::LocationException);
  EXPECT_THROW(closed_ds.getFileName(), H5::LocationException);
  EXPECT_THROW(file.setComment("missing", "c"), H5::LocationException);
  EXPECT_THROW(closed_ds.setComment("c"), H5::LocationException);
  EXPECT_THROW(file.removeComment("missing"), H5::LocationException);

  std::array<char, 8> cbuf{};
  EXPECT_THROW(file.getComment("missing", cbuf.size(), cbuf.data()),
               H5::LocationException);
  EXPECT_THROW(file.getComment("missing"), H5::LocationException);

  hobj_ref_t oref{};
  EXPECT_THROW(file.reference(&oref, "missing", H5R_OBJECT),
               H5::ReferenceException);
  EXPECT_THROW(file.reference(&oref, H5std_string("missing"), H5R_OBJECT),
               H5::ReferenceException);
  EXPECT_THROW(file.reference(&oref, "missing", scalar, H5R_DATASET_REGION),
               H5::ReferenceException);
  EXPECT_THROW(file.reference(&oref, H5std_string("missing"), scalar,
                              H5R_DATASET_REGION),
               H5::ReferenceException);
  EXPECT_THROW(H5::Group().dereference(file, nullptr, H5R_OBJECT),
               H5::ReferenceException);
  EXPECT_THROW(file.getObjType(nullptr), H5::ReferenceException);
  EXPECT_THROW(file.getRefObjType(nullptr), H5::ReferenceException);
  EXPECT_THROW(file.getRegion(nullptr), H5::ReferenceException);

  EXPECT_THROW(closed_ds.createGroup("sub", H5::LinkCreatPropList::DEFAULT),
               H5::LocationException);
  EXPECT_THROW(closed_ds.createGroup("sub", static_cast<size_t>(64)),
               H5::LocationException);
  EXPECT_THROW(closed_ds.openGroup("sub"), H5::LocationException);
  EXPECT_THROW(closed_ds.createDataSet("d", H5::PredType::NATIVE_INT32, scalar),
               H5::LocationException);
  EXPECT_THROW(closed_ds.openDataSet("d"), H5::LocationException);

  EXPECT_THROW(closed_ds.link("/x", "s"), H5::LocationException);
  EXPECT_THROW(closed_ds.link("x", grp, "h"), H5::LocationException);
  EXPECT_THROW(closed_ds.link("x", H5L_SAME_LOC, "h"), H5::LocationException);
  EXPECT_THROW(file.link(H5L_TYPE_EXTERNAL, "a", "b"), H5::FileIException);
  EXPECT_THROW(file.link(H5L_TYPE_HARD, "missing", "h"), H5::FileIException);
  EXPECT_THROW(file.copyLink("missing", grp, "c"), H5::FileIException);
  EXPECT_THROW(file.copyLink("missing", "c"), H5::FileIException);
  EXPECT_THROW(file.moveLink("missing", grp, "m"), H5::FileIException);
  EXPECT_THROW(file.moveLink("missing", "m"), H5::FileIException);
  EXPECT_THROW(file.unlink("missing"), H5::FileIException);

  H5O_native_info_t ninfo{};
  EXPECT_THROW(closed_ds.getNativeObjinfo(ninfo), H5::LocationException);
  EXPECT_THROW(file.getNativeObjinfo("missing", ninfo), H5::FileIException);
  EXPECT_THROW(
      file.getNativeObjinfo(".", H5_INDEX_NAME, H5_ITER_INC, 99, ninfo),
      H5::FileIException);

  H5O_info2_t oinfo{};
  EXPECT_THROW(closed_ds.getObjinfo(oinfo), H5::LocationException);
  EXPECT_THROW(file.getObjinfo("missing", oinfo), H5::FileIException);
  EXPECT_THROW(file.getObjinfo(".", H5_INDEX_NAME, H5_ITER_INC, 99, oinfo),
               H5::FileIException);

  H5G_stat_t stat{};
  EXPECT_THROW(file.getObjinfo("missing", false, stat), H5::FileIException);
  EXPECT_THROW(file.getObjinfo("missing", stat), H5::FileIException);
  EXPECT_THROW(file.getLinkInfo("missing"), H5::FileIException);
  EXPECT_THROW(file.getLinkval("missing"), H5::FileIException);
  // Requesting link value of a hard link ("ds") fails H5Lget_val.
  EXPECT_THROW(file.getLinkval("ds", 8), H5::FileIException);

  EXPECT_THROW(file.mount("missing", file, H5::PropList::DEFAULT),
               H5::FileIException);
  EXPECT_THROW(file.unmount("missing"), H5::FileIException);

  int idx = 0;
  EXPECT_THROW(file.iterateElems("missing", &idx, CollectGroupMember, nullptr),
               H5::FileIException);
  EXPECT_THROW(closed_ds.getNumObjs(), H5::LocationException);
  EXPECT_THROW(file.getObjnameByIdx(99), H5::FileIException);
  EXPECT_THROW(file.getObjnameByIdx(99, cbuf.data(), cbuf.size()),
               H5::FileIException);
  // Note: getObjnameByIdx(idx, H5std_string&, size) allocates new char[size+1]
  // before delegating to the throwing char* overload and leaks on error.
  H5std_string sbuf;
  EXPECT_THROW(file.childObjType("missing"), H5::FileIException);
  EXPECT_THROW(file.childObjType(static_cast<hsize_t>(99)), H5::FileIException);
  EXPECT_THROW(file.childObjVersion("missing"), H5::FileIException);
  EXPECT_THROW(file.getObjTypeByIdx(99), H5::FileIException);
  EXPECT_THROW(file.getObjTypeByIdx(99, sbuf), H5::FileIException);
  EXPECT_THROW(closed_ds.throwException("fn", "msg"), H5::LocationException);
}

}  // namespace
}  // namespace hdf5
