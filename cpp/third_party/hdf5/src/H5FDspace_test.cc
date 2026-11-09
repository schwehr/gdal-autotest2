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
// Tests VFD space allocation, deallocation, and EOA extension helpers
// (H5FD__alloc_real, H5FD_alloc, H5FD__free_real, H5FD_free, H5FD_try_extend,
// and static H5FD__extend) in third_party/hdf5/src/H5FDspace.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDspace.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FDSPACE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5FD_FRIEND
#include "third_party/hdf5/src/H5FDpkg.h"

#define H5F_FRIEND
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#pragma clang diagnostic pop

#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"

#ifdef H5FDSPACE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FDSPACE_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t dxpl_id = H5P_DATASET_XFER_DEFAULT) {
    H5open();
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5CX_set_dxpl(dxpl_id);
    }
  }
  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool pushed() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

struct MockVfdState {
  bool fail_query = false;
  unsigned long query_flags = 0;  // NOLINT(runtime/int)
  bool fail_alloc = false;
  haddr_t alloc_override = HADDR_UNDEF;
  bool fail_free = false;
  bool fail_get_eoa = false;
  bool fail_set_eoa = false;
  haddr_t eoa = 0;

  int query_calls = 0;
  int alloc_calls = 0;
  hsize_t last_alloc_size = 0;
  hid_t last_alloc_dxpl = H5I_INVALID_HID;
  int free_calls = 0;
  haddr_t last_free_addr = HADDR_UNDEF;
  hsize_t last_free_size = 0;
  hid_t last_free_dxpl = H5I_INVALID_HID;
  int get_eoa_calls = 0;
  int set_eoa_calls = 0;
};

struct MockVfdFile {
  H5FD_t pub;
  MockVfdState* state;
};

herr_t MockQuery(const H5FD_t* f, unsigned long* flags) {  // NOLINT
  const auto* mf = reinterpret_cast<const MockVfdFile*>(f);
  ++mf->state->query_calls;
  if (mf->state->fail_query) {
    return -1;
  }
  if (flags != nullptr) {
    *flags = mf->state->query_flags;
  }
  return 0;
}

haddr_t MockAlloc(H5FD_t* f, H5FD_mem_t /*type*/, hid_t dxpl_id, hsize_t size) {
  auto* mf = reinterpret_cast<MockVfdFile*>(f);
  ++mf->state->alloc_calls;
  mf->state->last_alloc_size = size;
  mf->state->last_alloc_dxpl = dxpl_id;
  if (mf->state->fail_alloc) {
    return HADDR_UNDEF;
  }
  const haddr_t addr = H5_addr_defined(mf->state->alloc_override)
                           ? mf->state->alloc_override
                           : mf->state->eoa;
  mf->state->eoa = addr + size;
  return addr;
}

herr_t MockFree(H5FD_t* f, H5FD_mem_t /*type*/, hid_t dxpl_id, haddr_t addr,
                hsize_t size) {
  auto* mf = reinterpret_cast<MockVfdFile*>(f);
  ++mf->state->free_calls;
  mf->state->last_free_addr = addr;
  mf->state->last_free_size = size;
  mf->state->last_free_dxpl = dxpl_id;
  return mf->state->fail_free ? -1 : 0;
}

haddr_t MockGetEoa(const H5FD_t* f, H5FD_mem_t /*type*/) {
  const auto* mf = reinterpret_cast<const MockVfdFile*>(f);
  ++mf->state->get_eoa_calls;
  if (mf->state->fail_get_eoa) {
    return HADDR_UNDEF;
  }
  return mf->state->eoa;
}

herr_t MockSetEoa(H5FD_t* f, H5FD_mem_t /*type*/, haddr_t addr) {
  auto* mf = reinterpret_cast<MockVfdFile*>(f);
  ++mf->state->set_eoa_calls;
  if (mf->state->fail_set_eoa) {
    return -1;
  }
  mf->state->eoa = addr;
  return 0;
}

H5FD_class_t MakeMockClass(bool has_query, bool has_alloc, bool has_free,
                           bool has_get_eoa) {
  H5FD_class_t cls{};
  cls.version = H5FD_CLASS_VERSION;
  cls.value = H5_VFD_RESERVED + 1;
  cls.name = "mock_space";
  cls.maxaddr = H5FD_MAXADDR;
  cls.fc_degree = H5F_CLOSE_WEAK;
  cls.query = has_query ? &MockQuery : nullptr;
  cls.alloc = has_alloc ? &MockAlloc : nullptr;
  cls.free = has_free ? &MockFree : nullptr;
  cls.get_eoa = has_get_eoa ? &MockGetEoa : nullptr;
  cls.set_eoa = &MockSetEoa;
  return cls;
}

MockVfdFile MakeMockFile(const H5FD_class_t* cls, MockVfdState* state,
                         haddr_t maxaddr = 4096) {
  MockVfdFile mf{};
  mf.pub.cls = cls;
  mf.pub.maxaddr = maxaddr;
  mf.pub.base_addr = 0;
  mf.pub.threshold = 1;
  mf.pub.alignment = 1;
  mf.pub.paged_aggr = false;
  mf.state = state;
  return mf;
}

TEST(H5FDspaceTest, AllocRealExtendFallbackAndAlignmentPaths) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  const H5FD_class_t cls = MakeMockClass(
      /*has_query=*/false, /*has_alloc=*/false, /*has_free=*/false,
      /*has_get_eoa=*/true);

  // 1. Plain extension without alignment (alignment == 1).
  {
    MockVfdState state;
    state.eoa = 100;
    MockVfdFile mf = MakeMockFile(&cls, &state);
    haddr_t frag_addr = 999;
    hsize_t frag_size = 999;
    const haddr_t addr = H5FD__alloc_real(&mf.pub, H5FD_MEM_DRAW, 32,
                                          &frag_addr, &frag_size);
    EXPECT_EQ(addr, haddr_t{100});
    EXPECT_EQ(state.eoa, haddr_t{132});
    EXPECT_EQ(frag_addr, haddr_t{999});
    EXPECT_EQ(frag_size, hsize_t{999});
  }

  // 2. Alignment skipped when paged_aggr is true or size < threshold, and when
  //    EOA is already aligned.
  {
    MockVfdState state;
    state.eoa = 5;
    MockVfdFile mf = MakeMockFile(&cls, &state);
    mf.pub.alignment = 16;
    mf.pub.threshold = 8;

    mf.pub.paged_aggr = true;
    EXPECT_EQ(
        H5FD__alloc_real(&mf.pub, H5FD_MEM_DRAW, 16, nullptr, nullptr),
        haddr_t{5});
    EXPECT_EQ(state.eoa, haddr_t{21});

    mf.pub.paged_aggr = false;
    EXPECT_EQ(
        H5FD__alloc_real(&mf.pub, H5FD_MEM_DRAW, 4, nullptr, nullptr),
        haddr_t{21});
    EXPECT_EQ(state.eoa, haddr_t{25});

    state.eoa = 32;  // Already 16-byte aligned.
    haddr_t frag_addr = 777;
    hsize_t frag_size = 777;
    EXPECT_EQ(H5FD__alloc_real(&mf.pub, H5FD_MEM_DRAW, 16, &frag_addr,
                               &frag_size),
              haddr_t{32});
    EXPECT_EQ(state.eoa, haddr_t{48});
    EXPECT_EQ(frag_addr, haddr_t{777});
    EXPECT_EQ(frag_size, hsize_t{777});
  }

  // 3. Misaligned EOA with non-zero base_addr reports fragment and adjusts
  //    returned relative address, plus NULL frag pointers branch.
  {
    MockVfdState state;
    state.eoa = 105;  // 105 % 16 == 9 -> extra = 7 -> aligned at 112.
    MockVfdFile mf = MakeMockFile(&cls, &state);
    mf.pub.base_addr = 100;
    mf.pub.alignment = 16;
    mf.pub.threshold = 8;

    haddr_t frag_addr = 0;
    hsize_t frag_size = 0;
    const haddr_t addr = H5FD__alloc_real(&mf.pub, H5FD_MEM_DRAW, 10,
                                          &frag_addr, &frag_size);
    EXPECT_EQ(addr, haddr_t{12});       // 112 - base_addr(100)
    EXPECT_EQ(frag_addr, haddr_t{5});   // 105 - base_addr(100)
    EXPECT_EQ(frag_size, hsize_t{7});
    EXPECT_EQ(state.eoa, haddr_t{122});  // 105 + 10 + 7

    state.eoa = 105;
    EXPECT_EQ(
        H5FD__alloc_real(&mf.pub, H5FD_MEM_DRAW, 10, nullptr, nullptr),
        haddr_t{12});
    EXPECT_EQ(state.eoa, haddr_t{122});
  }

  // 4. H5FD__extend failure paths: get_eoa returns HADDR_UNDEF, eoa + size
  //    exceeds maxaddr, address overflow, and set_eoa failure.
  {
    ScopedErrorSilence silence;
    MockVfdState state;
    MockVfdFile mf = MakeMockFile(&cls, &state, /*maxaddr=*/256);

    state.fail_get_eoa = true;
    EXPECT_EQ(
        H5FD__alloc_real(&mf.pub, H5FD_MEM_DRAW, 16, nullptr, nullptr),
        HADDR_UNDEF);
    state.fail_get_eoa = false;

    state.eoa = 200;
    EXPECT_EQ(
        H5FD__alloc_real(&mf.pub, H5FD_MEM_DRAW, 64, nullptr, nullptr),
        HADDR_UNDEF);

    mf.pub.maxaddr = HADDR_UNDEF;
    state.eoa = 100;
    EXPECT_EQ(H5FD__alloc_real(&mf.pub, H5FD_MEM_DRAW, HADDR_UNDEF - 50,
                               nullptr, nullptr),
              HADDR_UNDEF);

    mf.pub.maxaddr = 1024;
    state.fail_set_eoa = true;
    EXPECT_EQ(
        H5FD__alloc_real(&mf.pub, H5FD_MEM_DRAW, 16, nullptr, nullptr),
        HADDR_UNDEF);
  }
}

TEST(H5FDspaceTest, AllocRealDriverAllocAndQueryCallbacks) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ScopedApiContext ctx(dxpl.get());
  ASSERT_TRUE(ctx.pushed());

  const H5FD_class_t cls = MakeMockClass(
      /*has_query=*/true, /*has_alloc=*/true, /*has_free=*/true,
      /*has_get_eoa=*/true);

  // 1. Failing query callback fails allocation.
  {
    ScopedErrorSilence silence;
    MockVfdState state;
    state.fail_query = true;
    MockVfdFile mf = MakeMockFile(&cls, &state);
    EXPECT_EQ(
        H5FD__alloc_real(&mf.pub, H5FD_MEM_DRAW, 16, nullptr, nullptr),
        HADDR_UNDEF);
    EXPECT_EQ(state.query_calls, 1);
    EXPECT_EQ(state.alloc_calls, 0);
  }

  // 2. query without H5FD_FEAT_USE_ALLOC_SIZE passes size + extra to alloc.
  {
    MockVfdState state;
    state.eoa = 5;
    state.query_flags = 0;
    MockVfdFile mf = MakeMockFile(&cls, &state);
    mf.pub.alignment = 16;
    mf.pub.threshold = 8;

    haddr_t frag_addr = 0;
    hsize_t frag_size = 0;
    const haddr_t addr = H5FD__alloc_real(&mf.pub, H5FD_MEM_SUPER, 10,
                                          &frag_addr, &frag_size);
    EXPECT_EQ(addr, haddr_t{16});
    EXPECT_EQ(frag_addr, haddr_t{5});
    EXPECT_EQ(frag_size, hsize_t{11});
    EXPECT_EQ(state.last_alloc_size, hsize_t{21});
    EXPECT_EQ(state.last_alloc_dxpl, dxpl.get());
  }

  // 3. query with H5FD_FEAT_USE_ALLOC_SIZE passes orig size to alloc directly.
  {
    MockVfdState state;
    state.eoa = 5;
    state.query_flags = H5FD_FEAT_USE_ALLOC_SIZE;
    state.alloc_override = 32;  // Driver performs its own alignment.
    MockVfdFile mf = MakeMockFile(&cls, &state);
    mf.pub.base_addr = 8;
    mf.pub.alignment = 16;
    mf.pub.threshold = 8;

    const haddr_t addr =
        H5FD__alloc_real(&mf.pub, H5FD_MEM_DRAW, 10, nullptr, nullptr);
    EXPECT_EQ(addr, haddr_t{24});  // 32 - base_addr(8)
    EXPECT_EQ(state.last_alloc_size, hsize_t{10});
    EXPECT_EQ(state.last_alloc_dxpl, dxpl.get());
  }

  // 4. Driver alloc returning HADDR_UNDEF propagates failure.
  {
    ScopedErrorSilence silence;
    MockVfdState state;
    state.fail_alloc = true;
    MockVfdFile mf = MakeMockFile(&cls, &state);
    EXPECT_EQ(
        H5FD__alloc_real(&mf.pub, H5FD_MEM_DRAW, 16, nullptr, nullptr),
        HADDR_UNDEF);
  }
}

TEST(H5FDspaceTest, FreeRealValidationDriverCallbackAndEoaShrink) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ScopedApiContext ctx(dxpl.get());
  ASSERT_TRUE(ctx.pushed());

  // 1. Address/size sanity checks and driver `free` callback.
  {
    const H5FD_class_t cls = MakeMockClass(
        /*has_query=*/false, /*has_alloc=*/false, /*has_free=*/true,
        /*has_get_eoa=*/true);
    MockVfdState state;
    MockVfdFile mf = MakeMockFile(&cls, &state, /*maxaddr=*/200);
    mf.pub.base_addr = 100;

    EXPECT_GE(H5FD__free_real(&mf.pub, H5FD_MEM_DRAW, 20, 30), 0);
    EXPECT_EQ(state.free_calls, 1);
    EXPECT_EQ(state.last_free_addr, haddr_t{120});
    EXPECT_EQ(state.last_free_size, hsize_t{30});
    EXPECT_EQ(state.last_free_dxpl, dxpl.get());

    ScopedErrorSilence silence;
    EXPECT_LT(H5FD__free_real(&mf.pub, H5FD_MEM_DRAW, HADDR_UNDEF, 10), 0);
    EXPECT_LT(H5FD__free_real(&mf.pub, H5FD_MEM_DRAW, 150, 10), 0);
    EXPECT_LT(H5FD__free_real(&mf.pub, H5FD_MEM_DRAW, 60, 50), 0);

    mf.pub.maxaddr = HADDR_UNDEF;
    EXPECT_LT(
        H5FD__free_real(&mf.pub, H5FD_MEM_DRAW, 20, HADDR_UNDEF - 50), 0);

    mf.pub.maxaddr = 500;
    state.fail_free = true;
    EXPECT_LT(H5FD__free_real(&mf.pub, H5FD_MEM_DRAW, 20, 30), 0);
  }

  // 2. No driver `free`, with `get_eoa`: shrinks EOA only for tail block.
  {
    const H5FD_class_t cls = MakeMockClass(
        /*has_query=*/false, /*has_alloc=*/false, /*has_free=*/false,
        /*has_get_eoa=*/true);
    MockVfdState state;
    state.eoa = 200;
    MockVfdFile mf = MakeMockFile(&cls, &state, /*maxaddr=*/500);
    mf.pub.base_addr = 100;

    // Non-tail block [110..150): EOA (200) unchanged.
    EXPECT_GE(H5FD__free_real(&mf.pub, H5FD_MEM_DRAW, 10, 40), 0);
    EXPECT_EQ(state.get_eoa_calls, 1);
    EXPECT_EQ(state.set_eoa_calls, 0);
    EXPECT_EQ(state.eoa, haddr_t{200});

    // Tail block [150..200): EOA shrinks to 150.
    EXPECT_GE(H5FD__free_real(&mf.pub, H5FD_MEM_DRAW, 50, 50), 0);
    EXPECT_EQ(state.set_eoa_calls, 1);
    EXPECT_EQ(state.eoa, haddr_t{150});

    // Failure in set_eoa during tail shrink returns FAIL.
    ScopedErrorSilence silence;
    state.fail_set_eoa = true;
    EXPECT_LT(H5FD__free_real(&mf.pub, H5FD_MEM_DRAW, 10, 40), 0);
  }

  // 3. No driver `free` and no `get_eoa`: silently succeeds.
  {
    const H5FD_class_t cls = MakeMockClass(
        /*has_query=*/false, /*has_alloc=*/false, /*has_free=*/false,
        /*has_get_eoa=*/false);
    MockVfdState state;
    MockVfdFile mf = MakeMockFile(&cls, &state, /*maxaddr=*/500);
    EXPECT_GE(H5FD__free_real(&mf.pub, H5FD_MEM_DRAW, 10, 40), 0);
  }
}

TEST(H5FDspaceTest, FileWrappersAllocFreeAndTryExtend) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  H5F_t* f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(f, nullptr);
  ASSERT_NE(f->shared, nullptr);
  ASSERT_NE(f->shared->lf, nullptr);
  ASSERT_NE(f->shared->sblock, nullptr);

  // 1. Real file wrapper round-trip: alloc, extend hit, non-tail extend miss,
  //    and free tail block.
  const haddr_t addr = H5FD_alloc(f->shared->lf, H5FD_MEM_DRAW, f, 64,
                                  nullptr, nullptr);
  ASSERT_NE(addr, HADDR_UNDEF);
  EXPECT_EQ(H5FD_try_extend(f->shared->lf, H5FD_MEM_DRAW, f, addr + 64, 32),
            1);
  EXPECT_EQ(H5FD_try_extend(f->shared->lf, H5FD_MEM_DRAW, f, addr, 16), 0);
  EXPECT_GE(H5FD_free(f->shared->lf, H5FD_MEM_DRAW, f, addr, 96), 0);

  // 2. Mock VFD failure paths in H5FD_alloc, H5FD_free, H5FD_try_extend,
  //    plus H5F_eoa_dirty(f) failure branches.
  const H5FD_class_t cls = MakeMockClass(
      /*has_query=*/false, /*has_alloc=*/false, /*has_free=*/false,
      /*has_get_eoa=*/true);
  MockVfdState state;
  state.eoa = 64;
  MockVfdFile mf = MakeMockFile(&cls, &state, /*maxaddr=*/256);
  mf.pub.base_addr = 16;

  {
    ScopedErrorSilence silence;

    // H5FD_alloc -> H5FD__alloc_real failure.
    EXPECT_EQ(H5FD_alloc(&mf.pub, H5FD_MEM_DRAW, f, 512, nullptr, nullptr),
              HADDR_UNDEF);

    // H5FD_free -> H5FD__free_real failure.
    EXPECT_LT(H5FD_free(&mf.pub, H5FD_MEM_DRAW, f, HADDR_UNDEF, 16), 0);

    // H5FD_try_extend -> get_eoa failure and H5FD__extend failure.
    state.fail_get_eoa = true;
    EXPECT_LT(H5FD_try_extend(&mf.pub, H5FD_MEM_DRAW, f, 48, 16), 0);
    state.fail_get_eoa = false;

    state.eoa = 200;
    EXPECT_LT(H5FD_try_extend(&mf.pub, H5FD_MEM_DRAW, f, 184, 128), 0);

    // Trigger H5F_eoa_dirty(f) < 0 by unpinning superblock temporarily.
    ASSERT_TRUE(f->shared->sblock->cache_info.is_pinned);
    ASSERT_FALSE(f->shared->sblock->cache_info.is_protected);
    const bool orig_pinned = f->shared->sblock->cache_info.is_pinned;
    f->shared->sblock->cache_info.is_pinned = false;
    absl::Cleanup restore_pin = [&] {
      f->shared->sblock->cache_info.is_pinned = orig_pinned;
    };

    state.eoa = 64;
    EXPECT_EQ(H5FD_alloc(&mf.pub, H5FD_MEM_DRAW, f, 16, nullptr, nullptr),
              HADDR_UNDEF);
    EXPECT_LT(H5FD_free(&mf.pub, H5FD_MEM_DRAW, f, 0, 16), 0);
    EXPECT_LT(H5FD_try_extend(&mf.pub, H5FD_MEM_DRAW, f, 64, 16), 0);
  }
}

}  // namespace
}  // namespace hdf5
