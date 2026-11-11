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

// Unit tests for //third_party/netcdf/libdispatch/dinstance_intern.c.
//
// Exercises internal instance copy/reclaim, alignment calculation, and print
// entrypoints (NC_reclaim_data[_all], NC_copy_data[_all], NC_read_align,
// NC_type_alignment_internal, NC_print_data) across atomic, VLEN, opaque,
// enum, and deeply nested compound types.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dinstance_intern.c

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

extern "C" {
#include "third_party/netcdf/include/nc.h"
#include "third_party/netcdf/include/nc4internal.h"
#include "third_party/netcdf/include/ncoffsets.h"

int NC_print_data(NC_FILE_INFO_T* file, nc_type xtype, const void* memory,
                  size_t count);
}  // extern "C"

namespace netcdf {
namespace {

template <typename Fn>
std::string CaptureStderr(Fn&& fn) {
  std::fflush(stderr);
  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    return {};
  }
  int saved_stderr = dup(STDERR_FILENO);
  absl::Cleanup cleanup = [&]() {
    std::fflush(stderr);
    if (saved_stderr >= 0) {
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stderr);
      saved_stderr = -1;
    }
    if (pipe_fds[0] >= 0) {
      close(pipe_fds[0]);
      pipe_fds[0] = -1;
    }
    if (pipe_fds[1] >= 0) {
      close(pipe_fds[1]);
      pipe_fds[1] = -1;
    }
  };
  if (saved_stderr < 0 || dup2(pipe_fds[1], STDERR_FILENO) < 0) {
    return {};
  }
  close(pipe_fds[1]);
  pipe_fds[1] = -1;

  fn();
  std::fflush(stderr);

  dup2(saved_stderr, STDERR_FILENO);
  close(saved_stderr);
  saved_stderr = -1;

  std::string captured;
  char chunk[256];
  for (;;) {
    const ssize_t n = read(pipe_fds[0], chunk, sizeof(chunk));
    if (n > 0) {
      captured.append(chunk, static_cast<size_t>(n));
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  close(pipe_fds[0]);
  pipe_fds[0] = -1;
  return captured;
}

NC* GetInternalNc(int ncid) {
  NC* nc = nullptr;
  EXPECT_EQ(NC_NOERR, NC_check_id(ncid, &nc));
  return nc;
}

TEST(DinstanceInternTest, ReadAlignAndTypeAlignmentInternal) {
  // Exercise NC_read_align across alignment==0, aligned, and unaligned inputs.
  EXPECT_EQ(uintptr_t{100}, NC_read_align(100, 0));
  EXPECT_EQ(uintptr_t{0}, NC_read_align(0, 8));
  EXPECT_EQ(uintptr_t{16}, NC_read_align(16, 8));
  EXPECT_EQ(uintptr_t{24}, NC_read_align(19, 8));
  EXPECT_EQ(uintptr_t{4}, NC_read_align(1, 4));

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create_mem("dinstance_intern_align.nc", NC_NETCDF4, 0,
                                    nc.ptr()));
  NC* ncp = GetInternalNc(nc.get());
  ASSERT_NE(nullptr, ncp);
  NC_FILE_INFO_T* file = NC4_DATA(ncp);
  ASSERT_NE(nullptr, file);

  nc_type opq_type = NC_NAT;
  nc_type vlen_type = NC_NAT;
  nc_type enum_type = NC_NAT;
  nc_type empty_cmpd = NC_NAT;
  nc_type atomic_first_cmpd = NC_NAT;
  nc_type user_first_cmpd = NC_NAT;

  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc.get(), 4, "opq4", &opq_type));
  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc.get(), "vi", NC_INT, &vlen_type));
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_INT, "en", &enum_type));
  const int enum_zero = 0;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), enum_type, "ZERO", &enum_zero));

  struct EmptyPair {
    int32_t a;
  };
  struct AtomicFirst {
    double d;
    nc_vlen_t v;
  };
  struct UserFirst {
    nc_vlen_t v;
    int32_t tag;
  };
  ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(EmptyPair), "empty_c",
                                      &empty_cmpd));
  ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(AtomicFirst),
                                      "atomic_first", &atomic_first_cmpd));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), atomic_first_cmpd, "d",
                               NC_COMPOUND_OFFSET(AtomicFirst, d), NC_DOUBLE));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), atomic_first_cmpd, "v",
                               NC_COMPOUND_OFFSET(AtomicFirst, v), vlen_type));
  ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(UserFirst), "user_first",
                                      &user_first_cmpd));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), user_first_cmpd, "v",
                               NC_COMPOUND_OFFSET(UserFirst, v), vlen_type));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), user_first_cmpd, "tag",
                               NC_COMPOUND_OFFSET(UserFirst, tag), NC_INT));

  // Atomic path initializes static alignment table on first call.
  size_t align = 0;
  EXPECT_EQ(NC_NOERR,
            NC_type_alignment_internal(file, NC_DOUBLE, nullptr, &align));
  EXPECT_EQ(NC_NOERR,
            NC_type_alignment_internal(file, NC_STRING, nullptr, &align));

  // User types via utype==NULL lookup.
  align = 0;
  EXPECT_EQ(NC_NOERR,
            NC_type_alignment_internal(file, opq_type, nullptr, &align));
  EXPECT_GE(align, 1U);

  align = 0;
  EXPECT_EQ(NC_NOERR,
            NC_type_alignment_internal(file, vlen_type, nullptr, &align));
  EXPECT_EQ(alignof(nc_vlen_t), align);

  align = 0;
  EXPECT_EQ(NC_NOERR,
            NC_type_alignment_internal(file, enum_type, nullptr, &align));

  // Compound variants: empty -> NC_EINVAL, atomic-first, user-first.
  align = 0;
  EXPECT_EQ(NC_EINVAL,
            NC_type_alignment_internal(file, empty_cmpd, nullptr, &align));

  align = 0;
  EXPECT_EQ(NC_NOERR, NC_type_alignment_internal(file, atomic_first_cmpd,
                                                 nullptr, &align));

  align = 0;
  EXPECT_EQ(NC_NOERR,
            NC_type_alignment_internal(file, user_first_cmpd, nullptr, &align));
  EXPECT_EQ(alignof(nc_vlen_t), align);

  // Pre-looked-up utype != NULL and alignp == NULL branches.
  NC_TYPE_INFO_T* vlen_info = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_find_type(file, vlen_type, &vlen_info));
  ASSERT_NE(nullptr, vlen_info);
  EXPECT_EQ(NC_NOERR,
            NC_type_alignment_internal(file, vlen_type, vlen_info, nullptr));

  // Non-existent user typeid fails via nc4_find_type.
  align = 0;
  EXPECT_EQ(NC_EBADTYPID,
            NC_type_alignment_internal(file, 9999, nullptr, &align));
}

TEST(DinstanceInternTest, AtomicNoOpErrorAndPrintPaths) {
  ScopedNcFile nc3;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dinstance_intern_nc3.nc", NC_CLOBBER, 0, nc3.ptr()));
  NC* nc3p = GetInternalNc(nc3.get());
  ASSERT_NE(nullptr, nc3p);

  int32_t i32 = 17;
  int32_t out_i32 = 0;
  EXPECT_EQ(NC_NOERR, NC_reclaim_data(nc3p, NC_INT, nullptr, 0));
  EXPECT_EQ(NC_NOERR, NC_reclaim_data(nc3p, NC_INT, &i32, 1));
  EXPECT_EQ(NC_NOERR, NC_copy_data(nc3p, NC_INT, nullptr, 0, &out_i32));
  EXPECT_EQ(NC_NOERR, NC_copy_data(nc3p, NC_INT, &i32, 0, nullptr));
  ASSERT_EQ(NC_NOERR, NC_copy_data(nc3p, NC_INT, &i32, 1, &out_i32));
  EXPECT_EQ(17, out_i32);

  void* copy_nc3 = nullptr;
  ASSERT_EQ(NC_NOERR, NC_copy_data_all(nc3p, NC_INT, &i32, 1, &copy_nc3));
  ASSERT_NE(nullptr, copy_nc3);
  EXPECT_EQ(17, *static_cast<int32_t*>(copy_nc3));
  EXPECT_EQ(NC_NOERR, NC_reclaim_data_all(nc3p, NC_INT, copy_nc3, 1));

  ScopedNcFile nc4;
  ASSERT_EQ(NC_NOERR, nc_create_mem("dinstance_intern_atom.nc", NC_NETCDF4, 0,
                                    nc4.ptr()));
  NC* nc4p = GetInternalNc(nc4.get());
  ASSERT_NE(nullptr, nc4p);
  NC_FILE_INFO_T* file = NC4_DATA(nc4p);
  ASSERT_NE(nullptr, file);

  // Invalid user type errors and NULL no-op paths.
  EXPECT_EQ(NC_NOERR, NC_reclaim_data(nc4p, NC_STRING, nullptr, 0));
  EXPECT_EQ(NC_EBADTYPID, NC_reclaim_data(nc4p, 9999, &i32, 1));
  EXPECT_EQ(NC_NOERR, NC_reclaim_data_all(nc4p, NC_INT, nullptr, 0));
  EXPECT_EQ(NC_EBADTYPID, NC_reclaim_data_all(nc4p, 9999, &i32, 1));
  EXPECT_EQ(NC_EBADTYPID, NC_copy_data(nc4p, 9999, &i32, 1, &out_i32));
  void* bad_all = nullptr;
  EXPECT_EQ(NC_EBADTYPID, NC_copy_data_all(nc4p, 9999, &i32, 1, &bad_all));

  // Fixed user type (opaque) and count==0 with NC_copy_data_all.
  nc_type opq_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc4.get(), 4, "opq4", &opq_type));
  void* empty_user_copy = &i32;
  EXPECT_EQ(NC_NOERR,
            NC_copy_data_all(nc4p, opq_type, nullptr, 0, &empty_user_copy));
  EXPECT_EQ(nullptr, empty_user_copy);
  EXPECT_EQ(NC_NOERR, NC_copy_data_all(nc4p, opq_type, nullptr, 0, nullptr));

  const std::array<unsigned char, 8> opq_src{1, 2, 3, 4, 5, 6, 7, 8};
  std::array<unsigned char, 8> opq_dst{};
  ASSERT_EQ(NC_NOERR,
            NC_copy_data(nc4p, opq_type, opq_src.data(), 2, opq_dst.data()));
  EXPECT_EQ(opq_src, opq_dst);
  EXPECT_EQ(NC_NOERR, NC_reclaim_data(nc4p, opq_type, opq_dst.data(), 2));

  // NC_STRING copy & reclaim with NULL and non-NULL slots.
  const std::array<const char*, 3> str_src{"one", nullptr, "three"};
  std::array<char*, 3> str_dst{};
  absl::Cleanup reclaim_str_dst = [&]() {
    EXPECT_EQ(NC_NOERR,
              NC_reclaim_data(nc4p, NC_STRING, str_dst.data(), str_dst.size()));
  };
  ASSERT_EQ(NC_NOERR, NC_copy_data(nc4p, NC_STRING, str_src.data(),
                                   str_src.size(), str_dst.data()));
  EXPECT_NE(str_src[0], str_dst[0]);
  EXPECT_STREQ("one", str_dst[0]);
  EXPECT_EQ(nullptr, str_dst[1]);
  EXPECT_STREQ("three", str_dst[2]);

  void* str_all = nullptr;
  absl::Cleanup reclaim_str_all = [&]() {
    if (str_all != nullptr) {
      void* p = str_all;
      str_all = nullptr;
      EXPECT_EQ(NC_NOERR, NC_reclaim_data_all(nc4p, NC_STRING, p, 3));
    }
  };
  ASSERT_EQ(NC_NOERR, NC_copy_data_all(nc4p, NC_STRING, str_src.data(),
                                       str_src.size(), &str_all));
  ASSERT_NE(nullptr, str_all);
  char* const* copied_strs = static_cast<char* const*>(str_all);
  EXPECT_STREQ("one", copied_strs[0]);
  EXPECT_EQ(nullptr, copied_strs[1]);
  EXPECT_STREQ("three", copied_strs[2]);

  // NC_print_data delegation to nc_print_data.
  const std::array<int32_t, 2> print_vals{12, 34};
  int print_rc = -1;
  const std::string printed = CaptureStderr([&]() {
    print_rc =
        NC_print_data(file, NC_INT, print_vals.data(), print_vals.size());
  });
  EXPECT_EQ(NC_NOERR, print_rc);
  EXPECT_EQ("12 34\n", printed);
  EXPECT_EQ(NC_EBADTYPE, NC_print_data(file, 9999, print_vals.data(), 1));
}

TEST(DinstanceInternTest, VlenAllBaseCategoriesCopyAndReclaim) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dinstance_intern_vlen.nc", NC_NETCDF4, 0, nc.ptr()));
  NC* ncp = GetInternalNc(nc.get());
  ASSERT_NE(nullptr, ncp);

  struct FixedPair {
    int32_t x;
    double y;
  };
  struct VarsizedItem {
    nc_vlen_t vec;
    char* tag;
  };

  nc_type fixed_pair_type = NC_NAT;
  nc_type vlen_int_type = NC_NAT;
  nc_type vlen_str_type = NC_NAT;
  nc_type vlen_fixed_type = NC_NAT;
  nc_type varsized_item_type = NC_NAT;
  nc_type vlen_varsized_type = NC_NAT;

  ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(FixedPair), "fixed_pair",
                                      &fixed_pair_type));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), fixed_pair_type, "x",
                               NC_COMPOUND_OFFSET(FixedPair, x), NC_INT));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), fixed_pair_type, "y",
                               NC_COMPOUND_OFFSET(FixedPair, y), NC_DOUBLE));

  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc.get(), "v_int", NC_INT, &vlen_int_type));
  ASSERT_EQ(NC_NOERR,
            nc_def_vlen(nc.get(), "v_str", NC_STRING, &vlen_str_type));
  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc.get(), "v_fixed", fixed_pair_type,
                                  &vlen_fixed_type));

  ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(VarsizedItem),
                                      "varsized_item", &varsized_item_type));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc.get(), varsized_item_type, "vec",
                                         NC_COMPOUND_OFFSET(VarsizedItem, vec),
                                         vlen_int_type));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc.get(), varsized_item_type, "tag",
                                         NC_COMPOUND_OFFSET(VarsizedItem, tag),
                                         NC_STRING));
  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc.get(), "v_varsized", varsized_item_type,
                                  &vlen_varsized_type));

  // 1) VLEN of fixed atomic (NC_INT): includes empty (len==0) and populated.
  std::array<int32_t, 3> int_data{10, 20, 30};
  const std::array<nc_vlen_t, 2> src_vi{
      nc_vlen_t{0, nullptr},
      nc_vlen_t{int_data.size(), int_data.data()},
  };
  {
    std::array<nc_vlen_t, 2> dst_vi{};
    absl::Cleanup reclaim_vi = [&]() {
      EXPECT_EQ(NC_NOERR, NC_reclaim_data(ncp, vlen_int_type, dst_vi.data(),
                                          dst_vi.size()));
    };
    ASSERT_EQ(NC_NOERR, NC_copy_data(ncp, vlen_int_type, src_vi.data(),
                                     src_vi.size(), dst_vi.data()));
    EXPECT_EQ(0U, dst_vi[0].len);
    EXPECT_EQ(nullptr, dst_vi[0].p);
    ASSERT_EQ(3U, dst_vi[1].len);
    EXPECT_NE(int_data.data(), dst_vi[1].p);
    EXPECT_EQ(30, static_cast<const int32_t*>(dst_vi[1].p)[2]);
  }

  // 2) VLEN of NC_STRING: includes empty (len==0) and populated strings, plus
  //    reclaim of a NULL slot inside a manually allocated VLEN of strings.
  char s_a[] = "hello";
  char s_b[] = "world";
  std::array<char*, 2> str_elems{s_a, s_b};
  const std::array<nc_vlen_t, 2> src_vs{
      nc_vlen_t{0, nullptr},
      nc_vlen_t{str_elems.size(), str_elems.data()},
  };
  {
    std::array<nc_vlen_t, 2> dst_vs{};
    absl::Cleanup reclaim_vs = [&]() {
      EXPECT_EQ(NC_NOERR, NC_reclaim_data(ncp, vlen_str_type, dst_vs.data(),
                                          dst_vs.size()));
    };
    ASSERT_EQ(NC_NOERR, NC_copy_data(ncp, vlen_str_type, src_vs.data(),
                                     src_vs.size(), dst_vs.data()));
    EXPECT_EQ(0U, dst_vs[0].len);
    EXPECT_EQ(nullptr, dst_vs[0].p);
    ASSERT_EQ(2U, dst_vs[1].len);
    char* const* out_strs = static_cast<char* const*>(dst_vs[1].p);
    EXPECT_NE(s_a, out_strs[0]);
    EXPECT_STREQ("hello", out_strs[0]);
    EXPECT_STREQ("world", out_strs[1]);
  }
  {
    constexpr char kReclaimLiteral[] = "reclaim_me";
    char** manual_vec = static_cast<char**>(std::calloc(2, sizeof(char*)));
    ASSERT_NE(nullptr, manual_vec);
    manual_vec[0] = static_cast<char*>(std::malloc(sizeof(kReclaimLiteral)));
    ASSERT_NE(nullptr, manual_vec[0]);
    std::memcpy(manual_vec[0], kReclaimLiteral, sizeof(kReclaimLiteral));
    manual_vec[1] = nullptr;
    nc_vlen_t manual_vlen{2, manual_vec};
    EXPECT_EQ(NC_NOERR, NC_reclaim_data(ncp, vlen_str_type, &manual_vlen, 1));
  }

  // 3) VLEN of fixed user type (FixedPair).
  std::array<FixedPair, 2> pair_elems{FixedPair{1, 1.5}, FixedPair{2, 2.5}};
  const nc_vlen_t src_vfixed{pair_elems.size(), pair_elems.data()};
  {
    void* dst_vfixed = nullptr;
    absl::Cleanup reclaim_vf = [&]() {
      if (dst_vfixed != nullptr) {
        void* p = dst_vfixed;
        dst_vfixed = nullptr;
        EXPECT_EQ(NC_NOERR, NC_reclaim_data_all(ncp, vlen_fixed_type, p, 1));
      }
    };
    ASSERT_EQ(NC_NOERR, NC_copy_data_all(ncp, vlen_fixed_type, &src_vfixed, 1,
                                         &dst_vfixed));
    ASSERT_NE(nullptr, dst_vfixed);
    const nc_vlen_t* out_vf = static_cast<const nc_vlen_t*>(dst_vfixed);
    ASSERT_EQ(2U, out_vf->len);
    EXPECT_NE(pair_elems.data(), out_vf->p);
    const FixedPair* copied_pairs = static_cast<const FixedPair*>(out_vf->p);
    EXPECT_EQ(2, copied_pairs[1].x);
    EXPECT_DOUBLE_EQ(2.5, copied_pairs[1].y);
  }

  // 4) VLEN of variable-sized user type (VarsizedItem) -> recursive copy &
  // reclaim.
  std::array<int32_t, 2> sub0{100, 200};
  std::array<int32_t, 1> sub1{300};
  char t0[] = "item0";
  char t1[] = "item1";
  std::array<VarsizedItem, 2> items{
      VarsizedItem{nc_vlen_t{sub0.size(), sub0.data()}, t0},
      VarsizedItem{nc_vlen_t{sub1.size(), sub1.data()}, t1},
  };
  const nc_vlen_t src_vv{items.size(), items.data()};
  {
    void* dst_vv = nullptr;
    absl::Cleanup reclaim_vv = [&]() {
      if (dst_vv != nullptr) {
        void* p = dst_vv;
        dst_vv = nullptr;
        EXPECT_EQ(NC_NOERR, NC_reclaim_data_all(ncp, vlen_varsized_type, p, 1));
      }
    };
    ASSERT_EQ(NC_NOERR,
              NC_copy_data_all(ncp, vlen_varsized_type, &src_vv, 1, &dst_vv));
    ASSERT_NE(nullptr, dst_vv);
    const nc_vlen_t* out_vv = static_cast<const nc_vlen_t*>(dst_vv);
    ASSERT_EQ(2U, out_vv->len);
    const VarsizedItem* out_items = static_cast<const VarsizedItem*>(out_vv->p);
    EXPECT_NE(t0, out_items[0].tag);
    EXPECT_STREQ("item0", out_items[0].tag);
    EXPECT_NE(sub0.data(), out_items[0].vec.p);
    EXPECT_EQ(200, static_cast<const int32_t*>(out_items[0].vec.p)[1]);
    EXPECT_STREQ("item1", out_items[1].tag);
    EXPECT_EQ(300, static_cast<const int32_t*>(out_items[1].vec.p)[0]);
  }
}

TEST(DinstanceInternTest, CompoundAllFieldCategoriesCopyAndReclaim) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dinstance_intern_cmpd.nc", NC_NETCDF4, 0, nc.ptr()));
  NC* ncp = GetInternalNc(nc.get());
  ASSERT_NE(nullptr, ncp);

  struct FixedPair {
    int32_t a;
    double b;
  };
  struct SubVarsized {
    nc_vlen_t seq;
    char* note;
  };
  struct MegaCompound {
    int32_t scalar_i;
    int16_t mat[2][2];
    char* title;
    char* tags[2];
    FixedPair fixed_scalar;
    FixedPair fixed_arr[2];
    nc_vlen_t v_scalar;
    SubVarsized var_arr[2];
  };

  nc_type fixed_pair_type = NC_NAT;
  nc_type vlen_int_type = NC_NAT;
  nc_type sub_var_type = NC_NAT;
  nc_type mega_type = NC_NAT;

  ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(FixedPair), "fixed_pair",
                                      &fixed_pair_type));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), fixed_pair_type, "a",
                               NC_COMPOUND_OFFSET(FixedPair, a), NC_INT));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), fixed_pair_type, "b",
                               NC_COMPOUND_OFFSET(FixedPair, b), NC_DOUBLE));

  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc.get(), "vi", NC_INT, &vlen_int_type));

  ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(SubVarsized),
                                      "sub_varsized", &sub_var_type));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc.get(), sub_var_type, "seq",
                                         NC_COMPOUND_OFFSET(SubVarsized, seq),
                                         vlen_int_type));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc.get(), sub_var_type, "note",
                                         NC_COMPOUND_OFFSET(SubVarsized, note),
                                         NC_STRING));

  ASSERT_EQ(NC_NOERR, nc_def_compound(nc.get(), sizeof(MegaCompound),
                                      "mega_compound", &mega_type));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(
                          nc.get(), mega_type, "scalar_i",
                          NC_COMPOUND_OFFSET(MegaCompound, scalar_i), NC_INT));
  const std::array<int, 2> mat_dims{2, 2};
  ASSERT_EQ(NC_NOERR,
            nc_insert_array_compound(nc.get(), mega_type, "mat",
                                     NC_COMPOUND_OFFSET(MegaCompound, mat),
                                     NC_SHORT, 2, mat_dims.data()));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(
                          nc.get(), mega_type, "title",
                          NC_COMPOUND_OFFSET(MegaCompound, title), NC_STRING));
  const std::array<int, 1> two_elems{2};
  ASSERT_EQ(NC_NOERR,
            nc_insert_array_compound(nc.get(), mega_type, "tags",
                                     NC_COMPOUND_OFFSET(MegaCompound, tags),
                                     NC_STRING, 1, two_elems.data()));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), mega_type, "fixed_scalar",
                               NC_COMPOUND_OFFSET(MegaCompound, fixed_scalar),
                               fixed_pair_type));
  ASSERT_EQ(NC_NOERR, nc_insert_array_compound(
                          nc.get(), mega_type, "fixed_arr",
                          NC_COMPOUND_OFFSET(MegaCompound, fixed_arr),
                          fixed_pair_type, 1, two_elems.data()));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), mega_type, "v_scalar",
                               NC_COMPOUND_OFFSET(MegaCompound, v_scalar),
                               vlen_int_type));
  ASSERT_EQ(NC_NOERR,
            nc_insert_array_compound(nc.get(), mega_type, "var_arr",
                                     NC_COMPOUND_OFFSET(MegaCompound, var_arr),
                                     sub_var_type, 1, two_elems.data()));

  std::array<int32_t, 2> d0{1, 2};
  std::array<int32_t, 2> d1{3, 4};
  std::array<int32_t, 1> d2{5};
  char title0[] = "first_rec";
  char title1[] = "second_rec";
  char tag0[] = "t0";
  char tag1[] = "t1";
  char note0[] = "n0";
  char note1[] = "n1";

  const std::array<MegaCompound, 2> src{
      MegaCompound{
          101,
          {{1, 2}, {3, 4}},
          title0,
          {tag0, nullptr},
          FixedPair{7, 7.5},
          {FixedPair{8, 8.5}, FixedPair{9, 9.5}},
          nc_vlen_t{d0.size(), d0.data()},
          {
              SubVarsized{nc_vlen_t{d1.size(), d1.data()}, note0},
              SubVarsized{nc_vlen_t{0, nullptr}, nullptr},
          },
      },
      MegaCompound{
          202,
          {{5, 6}, {7, 8}},
          title1,
          {nullptr, tag1},
          FixedPair{10, 10.5},
          {FixedPair{11, 11.5}, FixedPair{12, 12.5}},
          nc_vlen_t{0, nullptr},
          {
              SubVarsized{nc_vlen_t{d2.size(), d2.data()}, note1},
              SubVarsized{nc_vlen_t{0, nullptr}, note0},
          },
      },
  };

  void* copy_raw = nullptr;
  absl::Cleanup reclaim_copy = [&]() {
    if (copy_raw != nullptr) {
      void* p = copy_raw;
      copy_raw = nullptr;
      EXPECT_EQ(NC_NOERR, NC_reclaim_data_all(ncp, mega_type, p, src.size()));
    }
  };
  ASSERT_EQ(NC_NOERR, NC_copy_data_all(ncp, mega_type, src.data(), src.size(),
                                       &copy_raw));
  ASSERT_NE(nullptr, copy_raw);

  const MegaCompound* dst = static_cast<const MegaCompound*>(copy_raw);
  EXPECT_EQ(101, dst[0].scalar_i);
  EXPECT_EQ(4, dst[0].mat[1][1]);
  EXPECT_NE(title0, dst[0].title);
  EXPECT_STREQ("first_rec", dst[0].title);
  EXPECT_NE(tag0, dst[0].tags[0]);
  EXPECT_STREQ("t0", dst[0].tags[0]);
  EXPECT_EQ(nullptr, dst[0].tags[1]);
  EXPECT_EQ(7, dst[0].fixed_scalar.a);
  EXPECT_DOUBLE_EQ(9.5, dst[0].fixed_arr[1].b);
  ASSERT_EQ(2U, dst[0].v_scalar.len);
  EXPECT_NE(d0.data(), dst[0].v_scalar.p);
  EXPECT_EQ(2, static_cast<const int32_t*>(dst[0].v_scalar.p)[1]);
  EXPECT_STREQ("n0", dst[0].var_arr[0].note);
  EXPECT_EQ(4, static_cast<const int32_t*>(dst[0].var_arr[0].seq.p)[1]);
  EXPECT_EQ(0U, dst[0].var_arr[1].seq.len);
  EXPECT_EQ(nullptr, dst[0].var_arr[1].note);

  EXPECT_EQ(202, dst[1].scalar_i);
  EXPECT_EQ(nullptr, dst[1].tags[0]);
  EXPECT_STREQ("t1", dst[1].tags[1]);
  EXPECT_EQ(0U, dst[1].v_scalar.len);
  EXPECT_EQ(5, static_cast<const int32_t*>(dst[1].var_arr[0].seq.p)[0]);
  EXPECT_STREQ("n0", dst[1].var_arr[1].note);
}

}  // namespace
}  // namespace netcdf
