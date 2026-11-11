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

// Unit tests for //third_party/netcdf/libdispatch/ncproplist.c.
//
// Exercises property list creation, addition of NCP_CONST, NCP_BYTES, string,
// and NCP_COMPLEX entries, key truncation at NCPROPSMAXKEY, dynamic
// reallocation, lookup, indexed iteration, cloning into uninitialized and
// pre-populated destinations, and complex type copy/reclaim error propagation.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/ncproplist.c

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

extern "C" {
#include "third_party/netcdf/include/ncproplist.h"
}  // extern "C"

namespace netcdf {
namespace {

struct NcProplistDeleter {
  void operator()(NCproplist* plist) const { ncproplistfree(plist); }
};

using ScopedNcProplist = std::unique_ptr<NCproplist, NcProplistDeleter>;

char* DupCString(std::string_view s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out != nullptr) {
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
  }
  return out;
}

struct ComplexBox {
  int payload = 0;
  bool fail_copy = false;
  bool fail_reclaim = false;
};

ComplexBox* MakeComplexBox(int payload, bool fail_copy = false,
                           bool fail_reclaim = false) {
  auto* box = static_cast<ComplexBox*>(std::malloc(sizeof(ComplexBox)));
  if (box != nullptr) {
    box->payload = payload;
    box->fail_copy = fail_copy;
    box->fail_reclaim = fail_reclaim;
  }
  return box;
}

int ComplexTypeOp(NCPtypeop op, NCPpair* input, NCPpair* output) {
  auto* src = reinterpret_cast<ComplexBox*>(input->value);
  switch (op) {
    case NCP_RECLAIM:
      if (src != nullptr && src->fail_reclaim) {
        return NC_EINVAL;
      }
      std::free(src);
      input->value = 0;
      return NC_NOERR;
    case NCP_COPY:
      if (src == nullptr) {
        output->value = 0;
        return NC_NOERR;
      }
      if (src->fail_copy) {
        return NC_ENOMEM;
      }
      auto* copy = static_cast<ComplexBox*>(std::malloc(sizeof(ComplexBox)));
      if (copy == nullptr) {
        return NC_ENOMEM;
      }
      *copy = *src;
      output->value = reinterpret_cast<uintptr_t>(copy);
      output->size = sizeof(ComplexBox);
      return NC_NOERR;
  }
  return NC_EINVAL;
}

TEST(NcproplistTest, NullSafeAndEmptyListBehavior) {
  EXPECT_EQ(0, ncproplistfree(nullptr));
  EXPECT_EQ(NC_NOERR, ncproplistadd(nullptr, "k", 1));
  EXPECT_EQ(NC_NOERR, ncproplistaddbytes(nullptr, "k", nullptr, 0));
  EXPECT_EQ(NC_NOERR, ncproplistaddstring(nullptr, "k", nullptr));
  EXPECT_EQ(NC_NOERR,
            ncproplistaddx(nullptr, "k", nullptr, 0, 0, ComplexTypeOp));
  EXPECT_EQ(NC_ENOOBJECT, ncproplistget(nullptr, "k", nullptr, nullptr));
  EXPECT_EQ(NC_NOERR, ncproplistith(nullptr, 0, nullptr, nullptr, nullptr));

  ScopedNcProplist empty(ncproplistnew());
  ASSERT_NE(nullptr, empty.get());
  EXPECT_EQ(0U, empty->count);
  EXPECT_GE(empty->alloc, 2U);

  EXPECT_EQ(NC_EINVAL, ncproplistclone(nullptr, empty.get()));
  EXPECT_EQ(NC_EINVAL, ncproplistclone(empty.get(), nullptr));
  EXPECT_EQ(NC_ENOOBJECT,
            ncproplistget(empty.get(), nullptr, nullptr, nullptr));

  uintptr_t val = 999;
  uintptr_t sz = 888;
  EXPECT_EQ(NC_ENOOBJECT, ncproplistget(empty.get(), "missing", &val, &sz));
  EXPECT_EQ(0U, val);
  EXPECT_EQ(0U, sz);

  EXPECT_EQ(NC_EINVAL,
            ncproplistith(empty.get(), 0, nullptr, nullptr, nullptr));

  // Cloning an empty list hits the nprops == 0 fast path in extendplist.
  ScopedNcProplist clone(ncproplistnew());
  ASSERT_NE(nullptr, clone.get());
  EXPECT_EQ(NC_NOERR, ncproplistclone(empty.get(), clone.get()));
  EXPECT_EQ(0U, clone->count);
}

TEST(NcproplistTest, AddConstBytesStringTruncateKeysAndExtend) {
  ScopedNcProplist plist(ncproplistnew());
  ASSERT_NE(nullptr, plist.get());

  // Keys longer than NCPROPSMAXKEY (31 chars) are truncated.
  const std::string long_key(40, 'k');
  const std::string expected_key(NCPROPSMAXKEY, 'k');
  ASSERT_EQ(NC_NOERR, ncproplistadd(plist.get(), long_key.c_str(), 12345));
  ASSERT_EQ(NC_NOERR,
            ncproplistaddstring(plist.get(), "str_key", DupCString("hello")));

  uintptr_t val = 0;
  uintptr_t sz = 99;
  EXPECT_EQ(NC_ENOOBJECT,
            ncproplistget(plist.get(), long_key.c_str(), &val, &sz));
  EXPECT_EQ(NC_NOERR,
            ncproplistget(plist.get(), expected_key.c_str(), &val, &sz));
  EXPECT_EQ(12345U, val);
  EXPECT_EQ(0U, sz);

  EXPECT_EQ(NC_NOERR, ncproplistget(plist.get(), "str_key", &val, nullptr));
  EXPECT_STREQ("hello", reinterpret_cast<const char*>(val));

  // Grow past MINPROPS (2) across add, addbytes (with long key), and null str.
  ASSERT_EQ(NC_NOERR, ncproplistadd(plist.get(), "const_grow", 77));
  const std::string long_bytes_key = std::string(35, 'b');
  ASSERT_EQ(NC_NOERR, ncproplistaddbytes(plist.get(), long_bytes_key.c_str(),
                                         DupCString("payload"), 8));
  ASSERT_EQ(NC_NOERR, ncproplistaddstring(plist.get(), "null_str", nullptr));
  EXPECT_EQ(5U, plist->count);

  char* ith_key = nullptr;
  uintptr_t ith_val = 0;
  uintptr_t ith_sz = 99;
  ASSERT_EQ(NC_NOERR,
            ncproplistith(plist.get(), 0, &ith_key, &ith_val, &ith_sz));
  EXPECT_EQ(expected_key, ith_key);
  EXPECT_EQ(12345U, ith_val);
  EXPECT_EQ(0U, ith_sz);

  ASSERT_EQ(NC_NOERR,
            ncproplistith(plist.get(), 2, &ith_key, &ith_val, nullptr));
  EXPECT_STREQ("const_grow", ith_key);
  EXPECT_EQ(77U, ith_val);

  ASSERT_EQ(NC_NOERR, ncproplistith(plist.get(), 3, nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_EINVAL,
            ncproplistith(plist.get(), 5, &ith_key, &ith_val, &ith_sz));
}

TEST(NcproplistTest, ComplexPropertiesAndCloningAcrossTargetStates) {
  ScopedNcProplist src(ncproplistnew());
  ASSERT_NE(nullptr, src.get());

  const std::string long_complex_key(40, 'x');
  ASSERT_EQ(NC_NOERR, ncproplistaddx(src.get(), long_complex_key.c_str(),
                                     MakeComplexBox(10), sizeof(ComplexBox),
                                     101, ComplexTypeOp));
  ASSERT_EQ(NC_NOERR, ncproplistaddx(src.get(), "c2", MakeComplexBox(20),
                                     sizeof(ComplexBox), 202, ComplexTypeOp));
  ASSERT_EQ(NC_NOERR, ncproplistaddx(src.get(), "c3", MakeComplexBox(30),
                                     sizeof(ComplexBox), 303, ComplexTypeOp));
  ASSERT_EQ(NC_NOERR, ncproplistaddx(src.get(), "c4", MakeComplexBox(40),
                                     sizeof(ComplexBox), 404, ComplexTypeOp));
  EXPECT_EQ(4U, src->count);

  uintptr_t val = 0;
  uintptr_t sz = 0;
  ASSERT_EQ(NC_NOERR, ncproplistget(src.get(), "c3", &val, &sz));
  EXPECT_EQ(sizeof(ComplexBox), sz);
  EXPECT_EQ(30, reinterpret_cast<const ComplexBox*>(val)->payload);

  // Clone into a fresh list whose initial alloc (2) must expand to 4.
  ScopedNcProplist grown_clone(ncproplistnew());
  ASSERT_NE(nullptr, grown_clone.get());
  ASSERT_EQ(NC_NOERR, ncproplistclone(src.get(), grown_clone.get()));
  EXPECT_EQ(4U, grown_clone->count);

  uintptr_t cloned_val = 0;
  uintptr_t cloned_sz = 0;
  ASSERT_EQ(NC_NOERR,
            ncproplistget(grown_clone.get(), "c3", &cloned_val, &cloned_sz));
  EXPECT_NE(val, cloned_val);
  EXPECT_EQ(sizeof(ComplexBox), cloned_sz);
  EXPECT_EQ(30, reinterpret_cast<const ComplexBox*>(cloned_val)->payload);

  // Build a mixed list (NCP_CONST, NCP_BYTES, NCP_COMPLEX) with initialized
  // calloc'd slots for the first two entries.
  ScopedNcProplist mixed(ncproplistnew());
  ASSERT_NE(nullptr, mixed.get());
  ASSERT_EQ(NC_NOERR, ncproplistadd(mixed.get(), "k_const", 555));
  ASSERT_EQ(NC_NOERR,
            ncproplistaddbytes(mixed.get(), "k_bytes", DupCString("abc"), 4));
  ASSERT_EQ(NC_NOERR,
            ncproplistaddx(mixed.get(), "k_complex", MakeComplexBox(77),
                           sizeof(ComplexBox), 707, ComplexTypeOp));

  // Clone into an uninitialized calloc'd NCproplist (alloc == 0).
  ScopedNcProplist cold_clone(
      static_cast<NCproplist*>(std::calloc(1, sizeof(NCproplist))));
  ASSERT_NE(nullptr, cold_clone.get());
  ASSERT_EQ(NC_NOERR, ncproplistclone(mixed.get(), cold_clone.get()));
  EXPECT_EQ(3U, cold_clone->count);

  // Clone a 1-entry list into the already-populated grown_clone (alloc >= 4)
  // to cover ncproplistclear inside ncproplistinit and already-sized extend.
  ScopedNcProplist single(ncproplistnew());
  ASSERT_NE(nullptr, single.get());
  ASSERT_EQ(NC_NOERR, ncproplistaddx(single.get(), "only", MakeComplexBox(99),
                                     sizeof(ComplexBox), 0, ComplexTypeOp));
  ASSERT_EQ(NC_NOERR, ncproplistclone(single.get(), grown_clone.get()));
  EXPECT_EQ(1U, grown_clone->count);
  ASSERT_EQ(NC_NOERR,
            ncproplistget(grown_clone.get(), "only", &cloned_val, &sz));
  EXPECT_EQ(99, reinterpret_cast<const ComplexBox*>(cloned_val)->payload);
}

TEST(NcproplistTest, ComplexCallbackFailuresPropagateFromCloneInitAndFree) {
  ScopedNcProplist src_bad_copy(ncproplistnew());
  ASSERT_NE(nullptr, src_bad_copy.get());
  ASSERT_EQ(NC_NOERR, ncproplistaddx(src_bad_copy.get(), "bad_copy",
                                     MakeComplexBox(1, /*fail_copy=*/true),
                                     sizeof(ComplexBox), 0, ComplexTypeOp));

  ScopedNcProplist dest(ncproplistnew());
  ASSERT_NE(nullptr, dest.get());
  EXPECT_EQ(NC_ENOMEM, ncproplistclone(src_bad_copy.get(), dest.get()));

  ScopedNcProplist raw_bad(ncproplistnew());
  ASSERT_NE(nullptr, raw_bad.get());
  std::unique_ptr<ComplexBox, decltype(&std::free)> owned_box(
      MakeComplexBox(2, /*fail_copy=*/false, /*fail_reclaim=*/true),
      &std::free);
  ASSERT_NE(nullptr, owned_box.get());
  ASSERT_EQ(NC_NOERR,
            ncproplistaddx(raw_bad.get(), "bad_reclaim", owned_box.get(),
                           sizeof(ComplexBox), 0, ComplexTypeOp));
  ComplexBox* bad_reclaim_box = owned_box.release();
  absl::Cleanup allow_reclaim = [bad_reclaim_box]() {
    bad_reclaim_box->fail_reclaim = false;
  };

  // Failing reclaim propagates out of ncproplistfree and ncproplistclone's
  // internal ncproplistinit clear step without freeing raw_bad yet.
  EXPECT_EQ(NC_EINVAL, ncproplistfree(raw_bad.get()));
  EXPECT_EQ(NC_EINVAL, ncproplistclone(dest.get(), raw_bad.get()));
}

}  // namespace
}  // namespace netcdf
