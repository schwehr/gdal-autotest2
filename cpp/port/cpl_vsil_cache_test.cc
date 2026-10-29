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
// Tests for GDAL's /vsicached? and VSICreateCachedFile caching IO layer.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil_cache.cpp

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace {

// A customizable base handle to test forwarding methods and error conditions
// in VSICachedFile.
class FakeCacheBaseHandle : public VSIVirtualHandle {
 public:
  explicit FakeCacheBaseHandle(std::string data) : data_(std::move(data)) {}

  int Seek(vsi_l_offset nOffset, int nWhence) override {
    if (fail_seek_) return -1;
    if (nWhence == SEEK_SET) {
      offset_ = nOffset;
    } else if (nWhence == SEEK_CUR) {
      offset_ += nOffset;
    } else if (nWhence == SEEK_END) {
      offset_ = report_zero_size_ ? 0 : (data_.size() + nOffset);
    }
    return 0;
  }

  vsi_l_offset Tell() override { return offset_; }

  size_t Read(void* pBuffer, size_t nSize, size_t nMemb) override {
    size_t to_read = nSize * nMemb;
    if (to_read == 0 || offset_ >= data_.size()) return 0;
    if (short_read_with_error_) {
      err_ = true;
      size_t n = std::min<size_t>(to_read / 2, data_.size() - offset_);
      memcpy(pBuffer, data_.data() + offset_, n);
      offset_ += n;
      return n / nSize;
    }
    size_t avail = static_cast<size_t>(data_.size() - offset_);
    size_t n = std::min(to_read, avail);
    memcpy(pBuffer, data_.data() + offset_, n);
    offset_ += n;
    return n / nSize;
  }

  size_t Write(const void* /*pBuffer*/, size_t /*nSize*/,
               size_t /*nMemb*/) override {
    return 0;
  }

  void AdviseRead(int nRanges, const vsi_l_offset* /*panOffsets*/,
                  const size_t* /*panSizes*/) override {
    advise_read_ranges_ = nRanges;
  }

  void* GetNativeFileDescriptor() override {
    return reinterpret_cast<void*>(0x1234);
  }

  bool HasPRead() const override { return true; }

  size_t PRead(void* pBuffer, size_t nSize,
               vsi_l_offset nOffset) const override {
    if (nOffset >= data_.size()) return 0;
    size_t n = std::min<size_t>(nSize, data_.size() - nOffset);
    memcpy(pBuffer, data_.data() + nOffset, n);
    return n;
  }

  void ClearErr() override { err_ = false; }
  int Eof() override { return offset_ >= data_.size() ? 1 : 0; }
  int Error() override { return err_ ? 1 : 0; }
  int Flush() override { return 0; }
  int Close() override { return 0; }

  std::string data_;
  vsi_l_offset offset_ = 0;
  bool fail_seek_ = false;
  bool short_read_with_error_ = false;
  bool report_zero_size_ = false;
  bool err_ = false;
  int advise_read_ranges_ = 0;
};

TEST(CplVsilCacheTest, CachedFilesystemHandlerAnalyzeFilenameAndOperations) {
  auto* handler = VSIFileManager::GetHandler("/vsicached?");
  ASSERT_NE(nullptr, handler);

  // Direct calls with non-/vsicached? prefix fail.
  EXPECT_EQ(nullptr, handler->Open("/not_cached", "rb", false, nullptr));
  VSIStatBufL stat_buf;
  EXPECT_EQ(-1, handler->Stat("/not_cached", &stat_buf, 0));
  EXPECT_EQ(nullptr, handler->ReadDirEx("/not_cached", 0));

  const std::string mem_file = "/vsimem/cache_dir/test.bin";
  ASSERT_EQ(0, VSIMkdir("/vsimem/cache_dir", 0755));
  absl::Cleanup rm_dir = [] { VSIRmdirRecursive("/vsimem/cache_dir"); };
  autotest2::VsiMemTempWrapper wrapper(mem_file, "0123456789abcdefghij");

  // Missing 'file' option, unsupported option, and invalid chunk_size /
  // cache_size values.
  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, VSIFOpenL("/vsicached?chunk_size=1024", "rb"));
    EXPECT_EQ(nullptr,
              VSIFOpenL("/vsicached?file=/vsimem/x&chunk_size=abc", "rb"));
    EXPECT_EQ(nullptr,
              VSIFOpenL("/vsicached?file=/vsimem/x&chunk_size=2048MB", "rb"));
    EXPECT_EQ(
        nullptr,
        VSIFOpenL("/vsicached?file=/vsimem/x&chunk_size=2000000KB", "rb"));
    EXPECT_EQ(nullptr,
              VSIFOpenL("/vsicached?file=/vsimem/x&chunk_size=10GB", "rb"));
    EXPECT_EQ(nullptr,
              VSIFOpenL("/vsicached?file=/vsimem/x&cache_size=xyz", "rb"));
    // Write access mode fails (test both bSetError=true and false).
    EXPECT_EQ(nullptr,
              VSIFOpenExL(absl::StrCat("/vsicached?file=", mem_file).c_str(),
                          "wb", TRUE));
    EXPECT_EQ(nullptr,
              VSIFOpenExL(absl::StrCat("/vsicached?file=", mem_file).c_str(),
                          "wb", FALSE));
    // Non-existent underlying file fails.
    EXPECT_EQ(nullptr,
              VSIFOpenL("/vsicached?file=/vsimem/does_not_exist.bin", "rb"));
  }

  // Valid open with KB and MB suffixes and an unknown option warning.
  std::string valid_url = absl::StrCat(
      "/vsicached?file=", mem_file, "&chunk_size=4KB&cache_size=1MB&foo=bar");
  VSILFILE* fp = nullptr;
  {
    WithQuietHandler quiet;
    fp = VSIFOpenL(valid_url.c_str(), "rb");
  }
  ASSERT_NE(nullptr, fp);
  char buf[32] = {};
  EXPECT_EQ(20, VSIFReadL(buf, 1, 20, fp));
  EXPECT_EQ(0, memcmp(buf, "0123456789abcdefghij", 20));
  EXPECT_EQ(0, VSIFCloseL(fp));

  // Stat and ReadDirEx via /vsicached?.
  ASSERT_EQ(0, VSIStatL(absl::StrCat("/vsicached?file=", mem_file).c_str(),
                        &stat_buf));
  EXPECT_EQ(20, stat_buf.st_size);

  CPLStringList dir_list(VSIReadDirEx("/vsicached?file=/vsimem/cache_dir", 0));
  EXPECT_GE(dir_list.size(), 1);
}

TEST(CplVsilCacheTest, GetCacheMaxConfigOptionsAndLoadBlocksPaths) {
  std::string content(4096, '\0');
  for (size_t i = 0; i < content.size(); ++i) {
    content[i] = static_cast<char>(i % 251);
  }

  // Test invalid VSI_CACHE_SIZE config option fallback.
  {
    CPLConfigOptionSetter bad_size("VSI_CACHE_SIZE", "invalid_size", false);
    WithQuietHandler quiet;
    auto* base = new FakeCacheBaseHandle(content);
    std::unique_ptr<VSIVirtualHandle> cached(
        VSICreateCachedFile(base, /*nChunkSize=*/64, /*nCacheSize=*/0));
    ASSERT_NE(nullptr, cached);
  }

  // Create a cached file with small chunk size (64 bytes) and small cache size
  // (128 bytes = 2 blocks) to exercise multi-block loading, splitting large
  // requests (> 20 * chunk_size), and cache eviction during a single Read()!
  auto* base = new FakeCacheBaseHandle(content);
  std::unique_ptr<VSIVirtualHandle> cached(
      VSICreateCachedFile(base, /*nChunkSize=*/64, /*nCacheSize=*/128));

  EXPECT_EQ(0, cached->Tell());
  EXPECT_EQ(0, cached->Eof());
  EXPECT_EQ(0, cached->Error());
  EXPECT_EQ(0, cached->Flush());
  EXPECT_EQ(0, cached->Write("abc", 1, 3));

  // Forwarding methods: AdviseRead, GetNativeFileDescriptor, HasPRead, PRead,
  // ReadMultiRange.
  vsi_l_offset off = 10;
  size_t sz = 20;
  cached->AdviseRead(1, &off, &sz);
  EXPECT_EQ(1, base->advise_read_ranges_);
  EXPECT_EQ(reinterpret_cast<void*>(0x1234), cached->GetNativeFileDescriptor());
  EXPECT_TRUE(cached->HasPRead());

  char pread_buf[20] = {};
  EXPECT_EQ(20, cached->PRead(pread_buf, 20, 10));
  EXPECT_EQ(0, memcmp(pread_buf, content.data() + 10, 20));

  void* range_ptr = pread_buf;
  EXPECT_EQ(0, cached->ReadMultiRange(1, &range_ptr, &off, &sz));

  // Zero-byte Read returns 0.
  std::vector<char> buf(3000, 0);
  EXPECT_EQ(0, cached->Read(buf.data(), 0, 10));
  EXPECT_EQ(0, cached->Read(buf.data(), 1, 0));

  // Single-block read (loads block 0).
  EXPECT_EQ(32, cached->Read(buf.data(), 1, 32));
  EXPECT_EQ(32, cached->Tell());
  EXPECT_EQ(0, memcmp(buf.data(), content.data(), 32));

  // Multi-block read that is not block-aligned: at offset 96 a 1340-byte read
  // spans blocks 1..22 (22 * 64 = 1408 > 1340) and exceeds 20 * m_nChunkSize
  // (1280), which exercises the request-splitting and eviction paths.
  EXPECT_EQ(0, cached->Seek(96, SEEK_SET));
  EXPECT_EQ(1340, cached->Read(buf.data(), 1, 1340));
  EXPECT_EQ(96 + 1340, cached->Tell());
  EXPECT_EQ(0, memcmp(buf.data(), content.data() + 96, 1340));

  // Seek with SEEK_CUR and SEEK_END.
  EXPECT_EQ(0, cached->Seek(10, SEEK_CUR));
  EXPECT_EQ(96 + 1350, cached->Tell());
  EXPECT_EQ(0, cached->Seek(0, SEEK_END));
  EXPECT_EQ(content.size(), cached->Tell());

  // Reading at or past m_nFileSize returns 0 and sets EOF.
  EXPECT_EQ(0, cached->Read(buf.data(), 1, 10));
  EXPECT_NE(0, cached->Eof());
  cached->ClearErr();
  EXPECT_EQ(0, cached->Eof());

  // Reading across EOF (requests 100 bytes from content.size() - 30).
  EXPECT_EQ(0, cached->Seek(content.size() - 30, SEEK_SET));
  EXPECT_EQ(30, cached->Read(buf.data(), 1, 100));
  EXPECT_NE(0, cached->Eof());
  cached->ClearErr();

  // Test error and seek failure paths on base handle.
  base->fail_seek_ = true;
  EXPECT_EQ(0, cached->Seek(2000, SEEK_SET));
  // Single block seek failure:
  EXPECT_EQ(0, cached->Read(buf.data(), 1, 10));
  // Multi-block seek failure:
  EXPECT_EQ(0, cached->Read(buf.data(), 1, 200));
  base->fail_seek_ = false;

  // Short read with error on single block and multi-block:
  base->short_read_with_error_ = true;
  EXPECT_EQ(0, cached->Seek(2500, SEEK_SET));
  EXPECT_GT(cached->Read(buf.data(), 1, 10), 0);
  EXPECT_NE(0, cached->Error());
  cached->ClearErr();

  EXPECT_EQ(0, cached->Seek(3200, SEEK_SET));
  EXPECT_GT(cached->Read(buf.data(), 1, 300), 0);
  EXPECT_NE(0, cached->Error());
  cached->ClearErr();

  // Also test when base reports 0 initial file size (e.g. streaming handle)
  // and later hits actual EOF on LoadBlocks.
  auto* zero_size_base = new FakeCacheBaseHandle("short");
  zero_size_base->report_zero_size_ = true;
  std::unique_ptr<VSIVirtualHandle> zero_cached(
      VSICreateCachedFile(zero_size_base, 64, 128));
  EXPECT_EQ(5, zero_cached->Read(buf.data(), 1, 200));
  EXPECT_NE(0, zero_cached->Eof());
}

}  // namespace
