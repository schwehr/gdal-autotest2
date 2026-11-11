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

// Unit tests for //third_party/netcdf/libdispatch/nclistmgr.c.
//
// Exercises open-file NC list allocation, insertion, lookup by ncid and path,
// NC3 vs NC4 group-bit validation, slot movement, iteration, deletion, and
// real dataset integration via ephemeral NC_DISKLESS handles.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/nclistmgr.c

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

extern "C" {
#include "third_party/netcdf/include/ncdispatch.h"
}  // extern "C"

namespace netcdf {
namespace {

class ScopedNcListEntry {
 public:
  ScopedNcListEntry(const NC_Dispatch* dispatch, char* path) {
    nc_.dispatch = dispatch;
    nc_.path = path;
  }

  ~ScopedNcListEntry() { remove(); }

  ScopedNcListEntry(const ScopedNcListEntry&) = delete;
  ScopedNcListEntry& operator=(const ScopedNcListEntry&) = delete;

  int add() {
    const int err = add_to_NCList(&nc_);
    if (err == NC_NOERR) {
      added_ = true;
    }
    return err;
  }

  void remove() {
    if (added_) {
      del_from_NCList(&nc_);
      added_ = false;
    }
  }

  NC* get() { return &nc_; }
  int ext_ncid() const { return nc_.ext_ncid; }

 private:
  NC nc_{};
  bool added_ = false;
};

TEST(NclistmgrTest, EmptyListOperationsAreSafe) {
  ASSERT_EQ(0, count_NCList());

  free_NCList();
  EXPECT_EQ(nullptr, find_in_NCList(1 << 16));
  EXPECT_EQ(nullptr, find_in_NCList_by_name("nonexistent.nc"));

  NC dummy{};
  dummy.ext_ncid = (1 << 16);
  del_from_NCList(&dummy);
  EXPECT_EQ(NC_EINVAL, move_in_NCList(&dummy, 2));

  EXPECT_EQ(NC_ERANGE, iterate_NCList(-1, nullptr));
  EXPECT_EQ(NC_ERANGE, iterate_NCList(0x10000, nullptr));
  EXPECT_EQ(NC_NOERR, iterate_NCList(0, nullptr));
  EXPECT_EQ(0, count_NCList());
}

TEST(NclistmgrTest, AddLookupMoveIterateAndDeleteEntries) {
  ASSERT_EQ(0, count_NCList());

  NC_Dispatch nc3_dispatch{};
  nc3_dispatch.model = NC_FORMATX_NC3;
  NC_Dispatch nc4_dispatch{};
  nc4_dispatch.model = NC_FORMATX_NC_HDF5;

  char path1[] = "first_dataset.nc";
  char path2[] = "second_dataset.nc";
  char path3[] = "third_dataset.nc";

  ScopedNcListEntry e1(&nc3_dispatch, path1);
  ScopedNcListEntry e2(&nc4_dispatch, path2);
  ScopedNcListEntry e3(nullptr, path3);

  ASSERT_EQ(NC_NOERR, e1.add());
  EXPECT_EQ(1 << 16, e1.ext_ncid());
  EXPECT_EQ(1, count_NCList());

  // free_NCList() is a no-op while files remain in the list.
  free_NCList();
  EXPECT_EQ(e1.get(), find_in_NCList(e1.ext_ncid()));

  ASSERT_EQ(NC_NOERR, e2.add());
  EXPECT_EQ(2 << 16, e2.ext_ncid());
  ASSERT_EQ(NC_NOERR, e3.add());
  EXPECT_EQ(3 << 16, e3.ext_ncid());
  EXPECT_EQ(3, count_NCList());

  // Classic (NC_FORMATX_NC3) rejects non-zero group bits; NC4 allows them.
  EXPECT_EQ(e1.get(), find_in_NCList(e1.ext_ncid()));
  EXPECT_EQ(nullptr, find_in_NCList(e1.ext_ncid() | 1));
  EXPECT_EQ(e2.get(), find_in_NCList(e2.ext_ncid()));
  EXPECT_EQ(e2.get(), find_in_NCList(e2.ext_ncid() | 5));
  EXPECT_EQ(e3.get(), find_in_NCList(e3.ext_ncid() | 3));
  EXPECT_EQ(nullptr, find_in_NCList(99 << 16));

  // Delete slot 1 so name lookup scans across NULL and non-matching slots.
  e1.remove();
  EXPECT_EQ(2, count_NCList());
  EXPECT_EQ(e3.get(), find_in_NCList_by_name("third_dataset.nc"));
  EXPECT_EQ(nullptr, find_in_NCList_by_name("missing_dataset.nc"));

  // Re-adding e1 reuses the freed slot 1.
  ASSERT_EQ(NC_NOERR, e1.add());
  EXPECT_EQ(1 << 16, e1.ext_ncid());
  EXPECT_EQ(3, count_NCList());

  // Moving to an occupied slot fails with NC_EINVAL; moving to a free slot
  // relocates the entry and clears the old slot.
  EXPECT_EQ(NC_EINVAL, move_in_NCList(e1.get(), 2));
  ASSERT_EQ(NC_NOERR, move_in_NCList(e1.get(), 42));
  EXPECT_EQ(42 << 16, e1.ext_ncid());
  EXPECT_EQ(nullptr, find_in_NCList(1 << 16));
  EXPECT_EQ(e1.get(), find_in_NCList(42 << 16));

  // Iteration validates bounds and retrieves entries.
  NC* iterated = e1.get();
  EXPECT_EQ(NC_ERANGE, iterate_NCList(-1, &iterated));
  EXPECT_EQ(NC_ERANGE, iterate_NCList(0x10000, &iterated));
  EXPECT_EQ(NC_NOERR, iterate_NCList(0, &iterated));
  EXPECT_EQ(nullptr, iterated);
  EXPECT_EQ(NC_NOERR, iterate_NCList(42, &iterated));
  EXPECT_EQ(e1.get(), iterated);
  EXPECT_EQ(NC_NOERR, iterate_NCList(42, nullptr));

  // Deleting with ncid==0 or mismatched NC* leaves count unchanged.
  NC invalid_zero{};
  invalid_zero.ext_ncid = 0;
  del_from_NCList(&invalid_zero);
  NC mismatched{};
  mismatched.ext_ncid = e2.ext_ncid();
  del_from_NCList(&mismatched);
  EXPECT_EQ(3, count_NCList());

  e1.remove();
  e2.remove();
  e3.remove();
  EXPECT_EQ(0, count_NCList());
}

TEST(NclistmgrTest, DisklessClassicAndNetcdf4DatasetsRegisterAndUnregister) {
  ASSERT_EQ(0, count_NCList());
  {
    ScopedNcFile nc3;
    ASSERT_EQ(NC_NOERR,
              nc_create("nclistmgr_classic.nc", NC_DISKLESS, nc3.ptr()));
    EXPECT_EQ(1, count_NCList());

    ScopedNcFile nc4;
    ASSERT_EQ(NC_NOERR, nc_create("nclistmgr_nc4.nc", NC_NETCDF4 | NC_DISKLESS,
                                  nc4.ptr()));
    EXPECT_EQ(2, count_NCList());

    int subgrpid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc4.get(), "subgrp", &subgrpid));

    EXPECT_NE(nullptr, find_in_NCList(nc3.get()));
    EXPECT_EQ(nullptr, find_in_NCList(nc3.get() | 1));
    EXPECT_NE(nullptr, find_in_NCList(nc4.get()));
    EXPECT_EQ(find_in_NCList(nc4.get()), find_in_NCList(subgrpid));
    EXPECT_EQ(find_in_NCList(nc3.get()),
              find_in_NCList_by_name("nclistmgr_classic.nc"));

    ASSERT_EQ(NC_NOERR, nc3.close());
    EXPECT_EQ(1, count_NCList());
    ASSERT_EQ(NC_NOERR, nc4.close());
  }
  EXPECT_EQ(0, count_NCList());
}

}  // namespace
}  // namespace netcdf
