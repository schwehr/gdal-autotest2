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
// Tests VSI virtual filesystem TIFF I/O wrapper, child/reopen handles, buffered
// writes, and cached byte-range reads (tifvsi.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/tifvsi.cpp

#include "third_party/gdal/frmts/gtiff/tifvsi.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

// Minimal in-memory VSIVirtualHandle that can inject Seek and Write failures.
class ControllableHandle final : public VSIVirtualHandle {
 public:
  bool fail_seek = false;
  bool fail_write = false;
  std::vector<uint8_t> data;
  vsi_l_offset pos = 0;

  int Seek(vsi_l_offset nOffset, int nWhence) override {
    if (fail_seek) return -1;
    if (nWhence == SEEK_SET) {
      pos = nOffset;
    } else if (nWhence == SEEK_CUR) {
      pos += nOffset;
    } else if (nWhence == SEEK_END) {
      pos = data.size() + nOffset;
    }
    return 0;
  }

  vsi_l_offset Tell() override { return pos; }

  size_t Read(void* pBuffer, size_t nSize, size_t nCount) override {
    const size_t total = nSize * nCount;
    if (pos >= data.size()) return 0;
    const size_t avail = std::min<size_t>(total, data.size() - pos);
    std::memcpy(pBuffer, data.data() + pos, avail);
    pos += avail;
    return nSize != 0 ? (avail / nSize) : 0;
  }

  size_t Write(const void* pBuffer, size_t nSize, size_t nCount) override {
    if (fail_write) return 0;
    const size_t total = nSize * nCount;
    if (pos + total > data.size()) {
      data.resize(pos + total, 0);
    }
    std::memcpy(data.data() + pos, pBuffer, total);
    pos += total;
    return nCount;
  }

  void ClearErr() override {}
  int Error() override { return 0; }
  int Eof() override { return pos >= data.size() ? 1 : 0; }
  int Flush() override { return 0; }
  int Close() override { return 0; }
};

VSILFILE* ToVSILFILE(ControllableHandle* h) { return h; }

void CreateMinimalTiff(const char* filename) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(drv->Create(filename, 8, 8, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  const std::vector<uint8_t> buf(64, 42);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 8, 8, const_cast<uint8_t*>(buf.data()),
                         8, 8, GDT_Byte, 0, 0, nullptr));
}

class TifVsiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    CPLErrorReset();
  }

  void TearDown() override { CPLErrorReset(); }
};

// Tests VSI_TIFFOpen, VSI_TIFFOpenChild, VSI_TIFFReOpen across all mode
// combinations ("r", "rDO", "r+", "r+D") and mmap mode on /vsimem/.
TEST_F(TifVsiTest, OpenChildReOpenAndMmap) {
  constexpr char kFilename[] = "/vsimem/tifvsi_modes.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };
  ASSERT_NO_FATAL_FAILURE(CreateMinimalTiff(kFilename));

  constexpr const char* kModes[] = {"r", "rDO", "r+", "r+D"};
  for (const char* mode : kModes) {
    SCOPED_TRACE(mode);
    VSILFILE* fp = VSIFOpenL(kFilename, "rb+");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp]() { EXPECT_EQ(0, VSIFCloseL(fp)); };

    TIFF* parent = VSI_TIFFOpen(kFilename, mode, fp);
    ASSERT_NE(nullptr, parent);
    absl::Cleanup close_parent = [&parent]() {
      if (parent != nullptr) TIFFClose(parent);
    };

    // 1. Open child handle sharing underlying VSILFILE, switch active handles,
    // and close child while child is active.
    {
      TIFF* child = VSI_TIFFOpenChild(parent);
      ASSERT_NE(nullptr, child);
      absl::Cleanup close_child = [child]() { TIFFClose(child); };
      uint32_t width = 0;
      EXPECT_EQ(1, TIFFGetField(child, TIFFTAG_IMAGEWIDTH, &width));
      EXPECT_EQ(8u, width);
    }

    // 2. ReOpen parent handle in the same mode (replaces parent on success).
    TIFF* reopened = VSI_TIFFReOpen(parent);
    if (reopened != nullptr) parent = nullptr;
    ASSERT_NE(nullptr, reopened);
    absl::Cleanup close_reopened = [reopened]() { TIFFClose(reopened); };
    uint32_t width = 0;
    EXPECT_EQ(1, TIFFGetField(reopened, TIFFTAG_IMAGEWIDTH, &width));
    EXPECT_EQ(8u, width);

    EXPECT_EQ(fp, VSI_TIFFGetVSILFile(TIFFClientdata(reopened)));
  }

  // 3. Test GTIFF_USE_MMAP=YES on /vsimem/ read-only handle (_tiffMapProc).
  {
    CPLConfigOptionSetter mmap_opt("GTIFF_USE_MMAP", "YES",
                                   /*bSetOnlyIfUndefined=*/false);
    VSILFILE* fp = VSIFOpenL(kFilename, "rb");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp]() { EXPECT_EQ(0, VSIFCloseL(fp)); };
    TIFF* tif = VSI_TIFFOpen(kFilename, "r", fp);
    ASSERT_NE(nullptr, tif);
    absl::Cleanup close_tif = [tif]() { TIFFClose(tif); };
    uint32_t width = 0;
    EXPECT_EQ(1, TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width));
    EXPECT_EQ(8u, width);
  }
}

// Tests buffered writes, active-handle switching flush, >BUFFER_SIZE chunking,
// VSI_TIFFSeek, VSI_TIFFWrite, and _tiffSizeProc.
TEST_F(TifVsiTest, BufferedWriteSeekAndSize) {
  auto backing = std::make_unique<ControllableHandle>();
  VSILFILE* fp = ToVSILFILE(backing.get());

  // 1. Name does not start with "/vsimem/" so InitializeWriteBuffer allocates
  // the 64KB write buffer while all I/O stays in our in-memory vector.
  std::vector<uint8_t> small_data(100, 0xAA);
  {
    TIFF* parent = VSI_TIFFOpen("in_memory_buffered.tif", "w+", fp);
    ASSERT_NE(nullptr, parent);
    absl::Cleanup close_parent = [parent]() { TIFFClose(parent); };

    ASSERT_EQ(1, TIFFSetField(parent, TIFFTAG_IMAGEWIDTH, 4));
    ASSERT_EQ(1, TIFFSetField(parent, TIFFTAG_IMAGELENGTH, 4));
    ASSERT_EQ(1, TIFFSetField(parent, TIFFTAG_BITSPERSAMPLE, 8));
    ASSERT_EQ(1, TIFFSetField(parent, TIFFTAG_SAMPLESPERPIXEL, 1));
    ASSERT_EQ(1, TIFFSetField(parent, TIFFTAG_ROWSPERSTRIP, 4));
    ASSERT_EQ(
        1, TIFFSetField(parent, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK));
    uint8_t strip[16] = {1, 2, 3, 4};
    ASSERT_EQ(16, TIFFWriteEncodedStrip(parent, 0, strip, sizeof(strip)));
    ASSERT_EQ(1, TIFFWriteDirectory(parent));

    // Seek to end twice: first sets bAtEndOfFile=true, second takes fast path.
    const toff_t end1 = VSI_TIFFSeek(parent, 0, SEEK_END);
    EXPECT_GT(end1, 0u);
    const toff_t end2 = VSI_TIFFSeek(parent, 0, SEEK_END);
    EXPECT_EQ(end1, end2);

    // Small buffered write at EOF (accumulates in abyWriteBuffer) & size check.
    EXPECT_EQ(1, VSI_TIFFWrite(parent, small_data.data(), small_data.size()));
    EXPECT_EQ(end1 + 100, TIFFGetSizeProc(parent)(TIFFClientdata(parent)));

    // Open child while parent has unflushed buffered data -> SetActiveGTH
    // flushes parent's write buffer.
    TIFF* child = VSI_TIFFOpenChild(parent);
    ASSERT_NE(nullptr, child);
    absl::Cleanup close_child = [child]() { TIFFClose(child); };

    // Seek to start on parent -> switches active handle back to parent and
    // checks _tiffSizeProc when bAtEndOfFile is false.
    EXPECT_EQ(0u, VSI_TIFFSeek(parent, 0, SEEK_SET));
    EXPECT_GE(TIFFGetSizeProc(parent)(TIFFClientdata(parent)), end1 + 100);

    // Seek to EOF and write > 64KB (150KB) to exercise the chunked flush loop.
    EXPECT_GT(VSI_TIFFSeek(parent, 0, SEEK_END), 0u);
    const std::vector<uint8_t> large_data(150000, 0x55);
    EXPECT_EQ(1, VSI_TIFFWrite(parent, large_data.data(), large_data.size()));
    EXPECT_EQ(1, VSI_TIFFFlushBufferedWrite(TIFFClientdata(parent)));
  }

  // 2. Unbuffered write at EOF (name starting with /vsimem/).
  {
    constexpr char kMemName[] = "/vsimem/tifvsi_unbuffered.tif";
    absl::Cleanup cleanup = [&]() { VSIUnlink(kMemName); };
    VSILFILE* mem_fp = VSIFOpenL(kMemName, "wb+");
    ASSERT_NE(nullptr, mem_fp);
    absl::Cleanup close_mem_fp = [mem_fp]() {
      EXPECT_EQ(0, VSIFCloseL(mem_fp));
    };
    TIFF* mem_tif = VSI_TIFFOpen(kMemName, "w+", mem_fp);
    ASSERT_NE(nullptr, mem_tif);
    absl::Cleanup close_mem_tif = [mem_tif]() { TIFFClose(mem_tif); };
    EXPECT_EQ(8u, VSI_TIFFSeek(mem_tif, 0, SEEK_END));
    EXPECT_EQ(1, VSI_TIFFWrite(mem_tif, small_data.data(), small_data.size()));
  }
}

// Tests VSI_TIFFSetCachedRanges, VSI_TIFFHasCachedRanges,
// VSI_TIFFGetCachedRange, and _tiffReadProc cache hit + miss paths.
TEST_F(TifVsiTest, CachedRangesReadHitAndMiss) {
  constexpr char kFilename[] = "/vsimem/tifvsi_cached.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kFilename); };
  ASSERT_NO_FATAL_FAILURE(CreateMinimalTiff(kFilename));

  vsi_l_offset file_len = 0;
  GByte* raw = VSIGetMemFileBuffer(kFilename, &file_len, FALSE);
  ASSERT_NE(nullptr, raw);
  ASSERT_GT(file_len, 32u);

  VSILFILE* fp = VSIFOpenL(kFilename, "rb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup close_fp = [fp]() { EXPECT_EQ(0, VSIFCloseL(fp)); };
  TIFF* tif = VSI_TIFFOpen(kFilename, "r", fp);
  ASSERT_NE(nullptr, tif);
  absl::Cleanup close_tif = [tif]() { TIFFClose(tif); };
  thandle_t th = TIFFClientdata(tif);

  EXPECT_EQ(0, VSI_TIFFHasCachedRanges(th));

  // Cache two non-contiguous ranges: [4, 12) and [20, file_len).
  std::vector<uint8_t> range0(raw + 4, raw + 12);
  std::vector<uint8_t> range1(raw + 20, raw + file_len);
  void* ptrs[2] = {range0.data(), range1.data()};
  const vsi_l_offset offsets[2] = {4, 20};
  const size_t sizes[2] = {range0.size(), range1.size()};

  VSI_TIFFSetCachedRanges(th, 2, ptrs, offsets, sizes);
  EXPECT_EQ(1, VSI_TIFFHasCachedRanges(th));

  // Before first range -> early break -> nullptr.
  EXPECT_EQ(nullptr, VSI_TIFFGetCachedRange(th, 0, 4));
  // Inside first range -> non-null.
  EXPECT_NE(nullptr, VSI_TIFFGetCachedRange(th, 6, 4));
  // Between ranges -> early break on range 1 -> nullptr.
  EXPECT_EQ(nullptr, VSI_TIFFGetCachedRange(th, 14, 4));
  // Inside second range -> non-null.
  EXPECT_NE(nullptr, VSI_TIFFGetCachedRange(th, 24, 8));
  // Past all ranges -> loop finishes -> nullptr.
  EXPECT_EQ(nullptr, VSI_TIFFGetCachedRange(th, file_len + 10, 4));

  // Trigger _tiffReadProc over cached range and uncached range.
  std::vector<uint8_t> strip(64, 0);
  EXPECT_EQ(64, TIFFReadEncodedStrip(tif, 0, strip.data(), strip.size()));
  EXPECT_EQ(42, strip[0]);

  // Clear cached ranges so we also hit nRanges == 0 branch.
  VSI_TIFFSetCachedRanges(th, 0, nullptr, nullptr, nullptr);
  EXPECT_EQ(0, VSI_TIFFHasCachedRanges(th));
}

// Tests error paths in open, seek, unbuffered write, buffered chunk write, and
// flush buffer.
TEST_F(TifVsiTest, FailurePaths) {
  WithQuietHandler quiet_errors;

  auto handle = std::make_unique<ControllableHandle>();
  VSILFILE* fp = ToVSILFILE(handle.get());

  // 1. VSI_TIFFOpen fails immediately if initial Seek(0, SEEK_SET) fails.
  handle->fail_seek = true;
  EXPECT_EQ(nullptr, VSI_TIFFOpen("fail_open.tif", "r", fp));
  handle->fail_seek = false;

  // 2. VSI_TIFFOpen_common fails when reading invalid header bytes (FreeGTH).
  handle->data = {0x00, 0x01, 0x02, 0x03};
  EXPECT_EQ(nullptr, VSI_TIFFOpen("bad_magic.tif", "r", fp));

  // 3. Valid buffered handle with injected seek & write failures.
  handle->data.clear();
  handle->pos = 0;
  TIFF* tif = VSI_TIFFOpen("buffered_err.tif", "w+", fp);
  ASSERT_NE(nullptr, tif);
  absl::Cleanup close_tif = [tif]() { TIFFClose(tif); };
  thandle_t th = TIFFClientdata(tif);

  // Force bAtEndOfFile = false, then fail both SEEK_END and SEEK_SET.
  EXPECT_EQ(0u, VSI_TIFFSeek(tif, 0, SEEK_SET));
  handle->fail_seek = true;
  EXPECT_EQ(static_cast<toff_t>(-1), VSI_TIFFSeek(tif, 0, SEEK_END));
  EXPECT_EQ(static_cast<toff_t>(-1), VSI_TIFFSeek(tif, 0, SEEK_SET));
  handle->fail_seek = false;

  // Unbuffered write failure when bAtEndOfFile == false.
  constexpr uint8_t kSmallBuf[16] = {};
  handle->fail_write = true;
  EXPECT_EQ(0, VSI_TIFFWrite(tif, kSmallBuf, sizeof(kSmallBuf)));
  handle->fail_write = false;

  // Buffered >64KB write failure when bAtEndOfFile == true.
  EXPECT_NE(static_cast<toff_t>(-1), VSI_TIFFSeek(tif, 0, SEEK_END));
  handle->fail_write = true;
  const std::vector<uint8_t> big_buf(100000, 0x11);
  EXPECT_EQ(0, VSI_TIFFWrite(tif, big_buf.data(), big_buf.size()));

  // Small buffered write succeeds into memory buffer, then flush fails.
  EXPECT_EQ(1, VSI_TIFFWrite(tif, kSmallBuf, sizeof(kSmallBuf)));
  EXPECT_EQ(0, VSI_TIFFFlushBufferedWrite(th));

  handle->fail_write = false;
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
