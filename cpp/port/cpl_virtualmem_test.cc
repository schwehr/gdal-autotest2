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
// Tests for the virtual memory helpers (CPLVirtualMem*).
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_virtualmem.cpp

#include "third_party/gdal/port/cpl_virtualmem.h"

#include <signal.h>
#include <ucontext.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

struct CacheStats {
  int cache_calls = 0;
  int uncache_calls = 0;
  int free_calls = 0;
  size_t last_uncache_size = 0;
};

void FillPageCallback(CPLVirtualMem* ctxt, size_t nOffset, void* pPageToFill,
                      size_t nToFill, void* pUserData) {
  auto* stats = static_cast<CacheStats*>(pUserData);
  if (stats != nullptr) {
    stats->cache_calls++;
  }
  memset(pPageToFill, static_cast<int>((nOffset / 4096) & 0x7f) + 1, nToFill);
}

void UnCachePageCallback(CPLVirtualMem* ctxt, size_t nOffset,
                         const void* pPageToBeEvicted, size_t nToBeEvicted,
                         void* pUserData) {
  auto* stats = static_cast<CacheStats*>(pUserData);
  if (stats != nullptr) {
    stats->uncache_calls++;
    stats->last_uncache_size = nToBeEvicted;
  }
}

void FreeUserDataCallback(void* pUserData) {
  auto* stats = static_cast<CacheStats*>(pUserData);
  if (stats != nullptr) {
    stats->free_calls++;
  }
}

TEST(CplVirtualMemTest, PageSizeAndFileMapBasic) {
  const size_t page_size = CPLGetPageSize();
  EXPECT_GT(page_size, 0u);
  EXPECT_EQ(TRUE, CPLIsVirtualMemFileMapAvailable());

  CPLVirtualMemFree(nullptr);
  CPLVirtualMemManagerTerminate();

  // Virtual file (/vsimem/) cannot be memory-mapped.
  {
    VsiMemTempWrapper vmem("/vsimem/not_real.bin", "hello");
    VSILFILE* vfp = VSIFOpenL("/vsimem/not_real.bin", "rb");
    ASSERT_NE(nullptr, vfp);
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, CPLVirtualMemFileMapNew(vfp, 0, 5, VIRTUALMEM_READONLY,
                                               nullptr, nullptr));
    EXPECT_EQ(0, VSIFCloseL(vfp));
  }

  // Real local file mapping.
  const std::string path =
      absl::StrCat(testing::TempDir(), "/vmem_filemap_test.bin");
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);
  std::string initial(page_size + 500, 'A');
  ASSERT_EQ(1u, VSIFWriteL(initial.data(), initial.size(), 1, fp));
  ASSERT_EQ(0, VSIFFlushL(fp));

  // Read-only mapping beyond file size fails.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr,
              CPLVirtualMemFileMapNew(fp, 0, initial.size() + 1000,
                                      VIRTUALMEM_READONLY, nullptr, nullptr));
  }

  // Read-write mapping beyond file size extends the file automatically.
  CacheStats stats;
  CPLVirtualMem* vmem = CPLVirtualMemFileMapNew(fp, 100, initial.size() + 1000,
                                                VIRTUALMEM_READWRITE,
                                                FreeUserDataCallback, &stats);
  ASSERT_NE(nullptr, vmem);
  EXPECT_EQ(TRUE, CPLVirtualMemIsFileMapping(vmem));
  EXPECT_EQ(VIRTUALMEM_READWRITE, CPLVirtualMemGetAccessMode(vmem));
  EXPECT_EQ(page_size, CPLVirtualMemGetPageSize(vmem));
  EXPECT_EQ(initial.size() + 1000, CPLVirtualMemGetSize(vmem));
  EXPECT_EQ(TRUE, CPLVirtualMemIsAccessThreadSafe(vmem));

  // No-ops on file mappings.
  CPLVirtualMemDeclareThread(vmem);
  CPLVirtualMemPin(vmem, CPLVirtualMemGetAddr(vmem), 100, FALSE);
  CPLVirtualMemUnDeclareThread(vmem);

  char* ptr = static_cast<char*>(CPLVirtualMemGetAddr(vmem));
  ASSERT_NE(nullptr, ptr);
  EXPECT_EQ('A', ptr[0]);
  ptr[0] = 'Z';

  // Derived mapping.  An out-of-range window is rejected (silently, without a
  // CPLError).
  EXPECT_EQ(nullptr, CPLVirtualMemDerivedNew(vmem, initial.size() + 500, 1000,
                                             nullptr, nullptr));
  CacheStats derived_stats;
  CPLVirtualMem* derived = CPLVirtualMemDerivedNew(
      vmem, 10, 200, FreeUserDataCallback, &derived_stats);
  ASSERT_NE(nullptr, derived);
  EXPECT_EQ(200u, CPLVirtualMemGetSize(derived));
  EXPECT_EQ(ptr + 10, CPLVirtualMemGetAddr(derived));

  // Free base first (decrement refcount), then derived (frees both).
  CPLVirtualMemFree(vmem);
  EXPECT_EQ(0, stats.free_calls);
  CPLVirtualMemFree(derived);
  EXPECT_EQ(1, derived_stats.free_calls);
  EXPECT_EQ(1, stats.free_calls);

  EXPECT_EQ(0, VSIFCloseL(fp));

  // Read-only file with read-write mapping beyond file size fails to extend.
  VSILFILE* fp_ro = VSIFOpenL(path.c_str(), "rb");
  ASSERT_NE(nullptr, fp_ro);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr,
              CPLVirtualMemFileMapNew(fp_ro, 0, initial.size() + 50000,
                                      VIRTUALMEM_READWRITE, nullptr, nullptr));
    // Also test mmap() failure when requesting PROT_WRITE on read-only fd.
    EXPECT_EQ(nullptr,
              CPLVirtualMemFileMapNew(fp_ro, 0, 100, VIRTUALMEM_READWRITE,
                                      nullptr, nullptr));
  }
  EXPECT_EQ(0, VSIFCloseL(fp_ro));
  VSIUnlink(path.c_str());
}

// CPLVirtualMemMsgToWorkerThread in third_party/gdal/port/cpl_virtualmem.cpp
// contains 4 bytes of struct padding between opType and hRequesterThread that
// are not initialized before writing sizeof(msg) to the worker pipe, triggering
// a MemorySanitizer use-of-uninitialized-value error.
#if !defined(MEMORY_SANITIZER)
TEST(CplVirtualMemTest, VmaSingleAndMultiThreadLRUAndCallbacks) {
  const size_t page_size = CPLGetPageSize();

  // Test non-power-of-two page_size_hint and nCacheSize == 0 / > nSize.
  CacheStats stats1;
  CPLVirtualMem* vmem1 = CPLVirtualMemNew(
      page_size * 5 + 123, /*nCacheSize=*/0, /*nPageSizeHint=*/page_size + 500,
      /*bSingleThreadUsage=*/TRUE, VIRTUALMEM_READWRITE, FillPageCallback,
      UnCachePageCallback, FreeUserDataCallback, &stats1);
  ASSERT_NE(nullptr, vmem1);
  EXPECT_EQ(FALSE, CPLVirtualMemIsFileMapping(vmem1));
  EXPECT_EQ(FALSE, CPLVirtualMemIsAccessThreadSafe(vmem1));
  EXPECT_GE(CPLVirtualMemGetPageSize(vmem1), page_size);

  // Create a second VMA simultaneously to test multiple registered VMAs and
  // unregistering from the middle of pasVirtualMem.
  CacheStats stats2;
  CPLVirtualMem* vmem2 = CPLVirtualMemNew(
      page_size * 4 + 100, /*nCacheSize=*/page_size * 10,
      /*nPageSizeHint=*/page_size, /*bSingleThreadUsage=*/FALSE,
      VIRTUALMEM_READWRITE, FillPageCallback, UnCachePageCallback,
      FreeUserDataCallback, &stats2);
  ASSERT_NE(nullptr, vmem2);
  EXPECT_EQ(TRUE, CPLVirtualMemIsAccessThreadSafe(vmem2));

  CPLVirtualMemDeclareThread(vmem2);

  // Pin pages in vmem2 (both read and write).
  char* addr2 = static_cast<char*>(CPLVirtualMemGetAddr(vmem2));
  CPLVirtualMemPin(vmem2, addr2, page_size, FALSE);
  CPLVirtualMemPin(vmem2, addr2 + page_size, page_size, TRUE);

  // Trigger page faults on vmem1 (single-threaded mode) across all pages to
  // force LRU eviction of dirty pages (including partial last page).
  volatile char* addr1 =
      static_cast<volatile char*>(CPLVirtualMemGetAddr(vmem1));
  const size_t vmem1_page = CPLVirtualMemGetPageSize(vmem1);
  for (size_t offset = 0; offset < CPLVirtualMemGetSize(vmem1);
       offset += vmem1_page) {
    // First read fault (maps as PROT_READ), then write fault (upgrades to RW).
    char c = addr1[offset];
    addr1[offset] = static_cast<char>(c + 1);
  }
  // Access first page again after eviction, directly with a store fault.
  addr1[0] = 42;
  EXPECT_EQ(42, addr1[0]);
  EXPECT_GT(stats1.cache_calls, 0);
  EXPECT_GT(stats1.uncache_calls, 0);

  // Also test fault on vmem2 (multi-threaded mremap path): read fault then
  // write fault on same page (hits iPage == ctxt->iLastPage upgrade to RW), and
  // read fault on two pages followed by write fault on first page (hits iPage
  // != iLastPage with TEST_BIT(pabitMappedPages)).
  volatile char* vaddr2 = static_cast<volatile char*>(addr2);
  char val_p2 = vaddr2[page_size * 2];
  vaddr2[page_size * 2] = static_cast<char>(val_p2 + 1);

  char val_p3 = vaddr2[page_size * 3];
  char val_p4 = vaddr2[page_size * 4];
  (void)val_p3;
  (void)val_p4;
  vaddr2[page_size * 3] = 99;
  EXPECT_EQ(99, vaddr2[page_size * 3]);

  // Also test rep movsb / movsw / movsq on unmapped pages in vmem2!
  CPLVirtualMem* vmem3 = CPLVirtualMemNew(
      page_size * 4, page_size * 4, page_size, FALSE, VIRTUALMEM_READWRITE,
      FillPageCallback, UnCachePageCallback, nullptr, nullptr);
  ASSERT_NE(nullptr, vmem3);
#if defined(__x86_64__)
  {
    char* addr3 = static_cast<char*>(CPLVirtualMemGetAddr(vmem3));
    // Trigger OP_MOVS_RSI_RDI with rsi == fault addr (load) on page 0.
    char dst_buf[16] = {};
    void* rsi_ptr = addr3;
    void* rdi_ptr = dst_buf;
    size_t cnt = 1;
    asm volatile("rep movsq"
                 : "+S"(rsi_ptr), "+D"(rdi_ptr), "+c"(cnt)
                 :
                 : "memory");
    EXPECT_NE(0, dst_buf[0]);

    // Trigger OP_MOVS_RSI_RDI with rdi == fault addr (store) on page 1.
    char src_buf[16] = {7, 7, 7, 7, 7, 7, 7, 7};
    rsi_ptr = src_buf;
    rdi_ptr = addr3 + page_size;
    cnt = 1;
    asm volatile("rep movsw"
                 : "+S"(rsi_ptr), "+D"(rdi_ptr), "+c"(cnt)
                 :
                 : "memory");
    EXPECT_EQ(7, addr3[page_size]);

    // Trigger rep movsb on page 2.
    rsi_ptr = src_buf;
    rdi_ptr = addr3 + page_size * 2;
    cnt = 1;
    asm volatile("rep movsb"
                 : "+S"(rsi_ptr), "+D"(rdi_ptr), "+c"(cnt)
                 :
                 : "memory");
    EXPECT_EQ(7, addr3[page_size * 2]);
  }
#endif

  CPLVirtualMemUnDeclareThread(vmem2);
  // Free vmem1 first (index 0 in pasVirtualMem while vmem2 and vmem3 follow).
  CPLVirtualMemFree(vmem1);
  CPLVirtualMemFree(vmem2);
  // Leave vmem3 alive when calling CPLVirtualMemManagerTerminate so it cleans
  // up remaining VMAs in its while loop!
  CPLVirtualMemManagerTerminate();
}

#if defined(__x86_64__)
// Helper that invokes the installed SIGSEGV handler with a synthetic ucontext_t
// pointing REG_RIP at `insn_bytes` and `si_addr` at a valid page in `vmem`.
// Because `si_addr` is inside `vmem`, the worker thread maps/handles the page
// and returns MAPPING_FOUND without chaining to oldact.
//
// This assumes the handler currently installed for SIGSEGV is the one
// CPLVirtualMemManagerInit() registered (sa_flags == SA_SIGINFO).  If some
// other component (sanitizer runtime, failure signal handler, death-test
// machinery) re-installs its own handler after CPLVirtualMemNew(), the checks
// below fail fast instead of calling a foreign handler with a fake context.
void InvokeSigsegvWithInstruction(CPLVirtualMem* vmem, size_t page_idx,
                                  const std::vector<GByte>& insn_bytes) {
  struct sigaction cur_act;
  ASSERT_EQ(0, sigaction(SIGSEGV, nullptr, &cur_act));
  ASSERT_NE(0, cur_act.sa_flags & SA_SIGINFO)
      << "SIGSEGV handler is not a sa_sigaction handler";
  const auto handler_bits = reinterpret_cast<uintptr_t>(cur_act.sa_sigaction);
  ASSERT_NE(reinterpret_cast<uintptr_t>(SIG_DFL), handler_bits);
  ASSERT_NE(reinterpret_cast<uintptr_t>(SIG_IGN), handler_bits);

  GByte rip_buf[8] = {};
  memcpy(rip_buf, insn_bytes.data(),
         std::min(sizeof(rip_buf), insn_bytes.size()));

  char* base = static_cast<char*>(CPLVirtualMemGetAddr(vmem));
  size_t page_size = CPLVirtualMemGetPageSize(vmem);

  siginfo_t info;
  memset(&info, 0, sizeof(info));
  info.si_signo = SIGSEGV;
  info.si_code = SEGV_ACCERR;
  info.si_addr = base + (page_idx * page_size);

  ucontext_t uctx;
  memset(&uctx, 0, sizeof(uctx));
  uctx.uc_mcontext.gregs[REG_RIP] = reinterpret_cast<greg_t>(rip_buf);
  uctx.uc_mcontext.gregs[REG_RSI] = reinterpret_cast<greg_t>(info.si_addr);
  uctx.uc_mcontext.gregs[REG_RDI] = reinterpret_cast<greg_t>(info.si_addr);

  cur_act.sa_sigaction(SIGSEGV, &info, &uctx);
}

TEST(CplVirtualMemTest, X86OpcodeDisassemblerCoverage) {
  const size_t page_size = CPLGetPageSize();
  // Create a VMA with 2 pages so we can alternate between page 0 and page 1
  // without ever exceeding nRetry == 100 on the same page.
  CPLVirtualMem* vmem = CPLVirtualMemNew(
      page_size * 4, page_size * 4, page_size, TRUE, VIRTUALMEM_READONLY,
      FillPageCallback, nullptr, nullptr, nullptr);
  ASSERT_NE(nullptr, vmem);

  const std::vector<std::vector<GByte>> opcodes = {
      // Top-level cases in CPLVirtualMemGetOpType:
      {0x00, 0x00},
      {0x02, 0x00},
      {0x08, 0x00},
      {0x0a, 0x00},
      {0x0f, 0xb6},
      {0x0f, 0x00},
      {0xc6, 0x00},
      {0x20, 0x00},
      {0x22, 0x00},
      {0x28, 0x00},
      {0x2a, 0x00},
      {0x30, 0x00},
      {0x32, 0x00},
      {0x38, 0x00},
      // 0x40 prefix:
      {0x40, 0x00},
      {0x40, 0x02},
      {0x40, 0x28},
      {0x40, 0x2a},
      {0x40, 0x3a},
      {0x40, 0x8a},
      {0x40, 0xff},
      // 0x41..0x47 prefix:
      {0x41, 0x00},
      {0x41, 0x02},
      {0x41, 0x0f, 0xb6},
      {0x41, 0x0f, 0x00},
      {0x41, 0x28},
      {0x41, 0x2a},
      {0x41, 0x38},
      {0x41, 0x80, 0x00},  // CPLVirtualMemGetOpTypeImm: 0x00..0x07
      {0x41, 0x80, 0x08},  // Imm: 0x08..0x0f
      {0x41, 0x80, 0x20},  // Imm: 0x20..0x27
      {0x41, 0x80, 0x28},  // Imm: 0x28..0x2f
      {0x41, 0x80, 0x30},  // Imm: 0x30..0x37
      {0x41, 0x80, 0x38},  // Imm: 0x38..0x3f
      {0x41, 0x88},
      {0x41, 0x8a},
      {0x41, 0xc6},
      {0x41, 0x84},
      {0x41, 0xf6, 0x00},
      {0x41, 0xf6, 0x10},
      {0x41, 0xff},
      // 0x48..0x4f prefix:
      {0x48, 0x01},
      {0x48, 0x03},
      {0x48, 0x09},
      {0x48, 0x0b},
      {0x48, 0x0f, 0xc3},
      {0x48, 0x0f, 0x00},
      {0x48, 0x21},
      {0x48, 0x23},
      {0x48, 0x29},
      {0x48, 0x2b},
      {0x48, 0x31},
      {0x48, 0x33},
      {0x48, 0x39},
      {0x48, 0x81, 0x40},
      {0x48, 0x85},
      {0x48, 0x89},
      {0x48, 0x8b},
      {0x48, 0xc7},
      {0x48, 0xf7, 0x00},
      {0x48, 0xf7, 0x10},
      {0x48, 0xff},
      // 0x66 prefix:
      {0x66, 0x01},
      {0x66, 0x03},
      {0x66, 0x0f, 0x2e},
      {0x66, 0x0f, 0x6f},
      {0x66, 0x0f, 0x7f},
      {0x66, 0x0f, 0xb6},
      {0x66, 0x0f, 0xe7},
      {0x66, 0x0f, 0x00},
      {0x66, 0x29},
      {0x66, 0x2b},
      {0x66, 0x39},
      // 0x66 + 0x41..0x47:
      {0x66, 0x41, 0x01},
      {0x66, 0x41, 0x03},
      {0x66, 0x41, 0x0f, 0x2e},
      {0x66, 0x41, 0x0f, 0x6f},
      {0x66, 0x41, 0x0f, 0x7f},
      {0x66, 0x41, 0x0f, 0xb6},
      {0x66, 0x41, 0x0f, 0xe7},
      {0x66, 0x41, 0x0f, 0x00},
      {0x66, 0x41, 0x29},
      {0x66, 0x41, 0x2b},
      {0x66, 0x41, 0x39},
      {0x66, 0x41, 0x81, 0x48},
      {0x66, 0x41, 0x85},
      {0x66, 0x41, 0x89},
      {0x66, 0x41, 0x8b},
      {0x66, 0x41, 0xc7},
      {0x66, 0x41, 0xf7, 0x00},
      {0x66, 0x41, 0xf7, 0x10},
      {0x66, 0x41, 0xff},
      // 0x66 cont:
      {0x66, 0x81, 0x60},
      {0x66, 0x85},
      {0x66, 0x89},
      {0x66, 0x8b},
      {0x66, 0xc7},
      {0x66, 0xf3, 0xa5},
      {0x66, 0xf3, 0x00},
      {0x66, 0xf7, 0x00},
      {0x66, 0xf7, 0x10},
      {0x66, 0xff},
      // 0x80..0xd9:
      {0x80, 0x68},
      {0x84, 0x00},
      {0x88, 0x00},
      {0x89, 0x00},
      {0x8a, 0x00},
      {0x8b, 0x00},
      {0xd9, 0x00},
      {0xd9, 0x18},
      {0xd9, 0x30},
      // 0xf2 prefix:
      {0xf2, 0x0f, 0x10},
      {0xf2, 0x0f, 0x11},
      {0xf2, 0x0f, 0x58},
      {0xf2, 0x0f, 0x59},
      {0xf2, 0x0f, 0x5c},
      {0xf2, 0x0f, 0x5e},
      {0xf2, 0x0f, 0x00},
      {0xf2, 0x41, 0x0f, 0x10},
      {0xf2, 0x41, 0x0f, 0x11},
      {0xf2, 0x41, 0x0f, 0x58},
      {0xf2, 0x41, 0x0f, 0x59},
      {0xf2, 0x41, 0x0f, 0x5c},
      {0xf2, 0x41, 0x0f, 0x5e},
      {0xf2, 0x41, 0x0f, 0x00},
      {0xf2, 0x41, 0x00},
      {0xf2, 0x00},
      // 0xf3 prefix:
      {0xf3, 0x0f, 0x10},
      {0xf3, 0x0f, 0x11},
      {0xf3, 0x0f, 0x6f},
      {0xf3, 0x0f, 0x7f},
      {0xf3, 0x0f, 0x00},
      {0xf3, 0x41, 0x0f, 0x10},
      {0xf3, 0x41, 0x0f, 0x11},
      {0xf3, 0x41, 0x0f, 0x6f},
      {0xf3, 0x41, 0x0f, 0x7f},
      {0xf3, 0x41, 0x0f, 0x00},
      {0xf3, 0x41, 0x00},
      {0xf3, 0x48, 0xa5},
      {0xf3, 0x48, 0x00},
      {0xf3, 0xa4},
      {0xf3, 0xa6},
      {0xf3, 0x00},
      // 0xf6/0xf7 and default:
      {0xf6, 0x00},
      {0xf6, 0x10},
      {0xff, 0x00},
  };

  for (size_t i = 0; i < opcodes.size(); ++i) {
    InvokeSigsegvWithInstruction(vmem, i % 2, opcodes[i]);
  }

  CPLVirtualMemFree(vmem);
  CPLVirtualMemManagerTerminate();
}
#endif
#endif  // !defined(MEMORY_SANITIZER)

}  // namespace
}  // namespace autotest2
