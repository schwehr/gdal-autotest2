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
// Tests JPEG VSI data source and destination managers (vsidataio.cpp).
// Note: google3 builds without IPPJ_HUFF, so IPP Huffman paths are not
// compiled.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/jpeg/vsidataio.cpp

// vsidataio.h requires <cstdio> (for FILE* in jpeglib.h) and cpl_port.h (for
// CPL_C_START).
// clang-format off
#include <cstdio>
#include "third_party/gdal/port/cpl_port.h"  // IWYU pragma: keep
#include "third_party/gdal/frmts/jpeg/vsidataio.h"
// clang-format on

#include <algorithm>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

CPL_C_START
#include "third_party/libjpeg_turbo/src/jerror.h"
#include "third_party/libjpeg_turbo/src/jpeglib.h"
CPL_C_END

namespace autotest2 {
namespace {

// Minimal in-memory VSIVirtualHandle that can inject Write and Flush failures.
class ControllableHandle final : public VSIVirtualHandle {
 public:
  bool fail_write = false;
  bool fail_flush = false;
  std::vector<uint8_t> data;
  vsi_l_offset pos = 0;

  int Seek(vsi_l_offset nOffset, int nWhence) override {
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
    if (pos >= data.size() || total == 0) return 0;
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
  int Flush() override { return fail_flush ? -1 : 0; }
  int Close() override { return 0; }
};

struct JpegErrorState {
  struct jpeg_error_mgr pub;
  std::jmp_buf setjmp_buffer;
  int last_error_code = 0;
  int warning_count = 0;
  bool saw_eof_warning = false;
};

void TestErrorExit(j_common_ptr cinfo) {
  auto* state = reinterpret_cast<JpegErrorState*>(cinfo->err);
  state->last_error_code = cinfo->err->msg_code;
  std::longjmp(state->setjmp_buffer, 1);
}

void TestEmitMessage(j_common_ptr cinfo, int msg_level) {
  auto* state = reinterpret_cast<JpegErrorState*>(cinfo->err);
  if (msg_level < 0) {
    ++state->warning_count;
    if (cinfo->err->msg_code == JWRN_JPEG_EOF) {
      state->saw_eof_warning = true;
    }
  }
}

void InitErrorState(JpegErrorState* jerr) {
  std::memset(jerr, 0, sizeof(*jerr));
  jpeg_std_error(&jerr->pub);
  jerr->pub.error_exit = TestErrorExit;
  jerr->pub.emit_message = TestEmitMessage;
}

// Compresses an RGB or grayscale buffer using jpeg_vsiio_dest inside a stack
// frame free of non-trivial C++ destructors so longjmp on error_exit is safe.
bool RunCompress(j_compress_ptr cinfo, JpegErrorState* jerr, VSILFILE* outfile,
                 int width, int height, int components, const uint8_t* pixels) {
  if (setjmp(jerr->setjmp_buffer) != 0) {
    jpeg_abort_compress(cinfo);
    return false;
  }
  jpeg_vsiio_dest(cinfo, outfile);
  cinfo->image_width = width;
  cinfo->image_height = height;
  cinfo->input_components = components;
  cinfo->in_color_space = components == 1 ? JCS_GRAYSCALE : JCS_RGB;
  jpeg_set_defaults(cinfo);
  jpeg_set_quality(cinfo, 95, TRUE);
  jpeg_start_compress(cinfo, TRUE);

  const int row_stride = width * components;
  while (cinfo->next_scanline < cinfo->image_height) {
    JSAMPROW row =
        const_cast<JSAMPROW>(pixels + cinfo->next_scanline * row_stride);
    jpeg_write_scanlines(cinfo, &row, 1);
  }
  jpeg_finish_compress(cinfo);
  return true;
}

// Decompresses a stream using jpeg_vsiio_src inside a stack frame free of
// non-trivial C++ destructors so longjmp on error_exit is safe.
bool RunDecompress(j_decompress_ptr dinfo, JpegErrorState* jerr,
                   VSILFILE* infile, uint8_t* out_row_buf, int* out_width,
                   int* out_height, int* out_components) {
  if (setjmp(jerr->setjmp_buffer) != 0) {
    jpeg_abort_decompress(dinfo);
    return false;
  }
  jpeg_vsiio_src(dinfo, infile);
  jpeg_read_header(dinfo, TRUE);
  // Exercise skip_input_data with non-positive lengths (no-op branch).
  if (dinfo->src != nullptr && dinfo->src->skip_input_data != nullptr) {
    dinfo->src->skip_input_data(dinfo, 0);
    dinfo->src->skip_input_data(dinfo, -4);
  }
  jpeg_start_decompress(dinfo);
  *out_width = static_cast<int>(dinfo->output_width);
  *out_height = static_cast<int>(dinfo->output_height);
  *out_components = dinfo->output_components;

  while (dinfo->output_scanline < dinfo->output_height) {
    JSAMPROW row = out_row_buf;
    jpeg_read_scanlines(dinfo, &row, 1);
  }
  jpeg_finish_decompress(dinfo);
  return true;
}

std::vector<uint8_t> MakeHighEntropyRgb(int width, int height) {
  std::vector<uint8_t> pixels(width * height * 3);
  uint32_t state = 0x12345678u;
  for (size_t i = 0; i < pixels.size(); ++i) {
    state = state * 1664525u + 1013904223u;
    pixels[i] = static_cast<uint8_t>((state >> 24) ^ ((i * 73) & 0xFF));
  }
  return pixels;
}

TEST(VsiDataIoTest, CompressSmallMultiBufferAndReuseDestination) {
  JpegErrorState jerr;
  InitErrorState(&jerr);
  struct jpeg_compress_struct cinfo = {};
  cinfo.err = &jerr.pub;
  jpeg_create_compress(&cinfo);
  absl::Cleanup destroy_cinfo = [&cinfo] { jpeg_destroy_compress(&cinfo); };

  // 1. First compression (cinfo.dest == nullptr): small 4x4 grayscale image
  // (< 4096 bytes output), testing init_destination + term_destination flush.
  ControllableHandle small_out;
  const std::vector<uint8_t> small_pixels(4 * 4, 128);
  ASSERT_TRUE(
      RunCompress(&cinfo, &jerr, &small_out, 4, 4, 1, small_pixels.data()));
  ASSERT_GT(small_out.data.size(), 4u);
  EXPECT_LT(small_out.data.size(), 4096u);
  EXPECT_EQ(0xFF, small_out.data[0]);
  EXPECT_EQ(0xD8, small_out.data[1]);
  EXPECT_EQ(0xFF, small_out.data[small_out.data.size() - 2]);
  EXPECT_EQ(0xD9, small_out.data[small_out.data.size() - 1]);

  // 2. Second compression on same cinfo (cinfo.dest != nullptr reuse branch):
  // 128x128 high-entropy RGB image (> 16 KB compressed) exercises multiple
  // empty_output_buffer refills followed by term_destination.
  ControllableHandle large_out;
  const std::vector<uint8_t> large_pixels = MakeHighEntropyRgb(128, 128);
  ASSERT_TRUE(
      RunCompress(&cinfo, &jerr, &large_out, 128, 128, 3, large_pixels.data()));
  EXPECT_GT(large_out.data.size(), 16384u);
}

TEST(VsiDataIoTest, DecompressNormalSkipMultiBufferAndReuseSource) {
  // Prepare two compressed JPEGs: one small, one with a >9000-byte APP5 marker
  // after SOI so skip_input_data spans >= 3 fill_input_buffer refills (4096B).
  JpegErrorState comp_err;
  InitErrorState(&comp_err);
  struct jpeg_compress_struct cinfo = {};
  cinfo.err = &comp_err.pub;
  jpeg_create_compress(&cinfo);
  absl::Cleanup destroy_cinfo = [&cinfo] { jpeg_destroy_compress(&cinfo); };

  ControllableHandle small_jpg;
  const std::vector<uint8_t> small_pixels(4 * 4, 200);
  ASSERT_TRUE(
      RunCompress(&cinfo, &comp_err, &small_jpg, 4, 4, 1, small_pixels.data()));

  ControllableHandle body_jpg;
  const std::vector<uint8_t> rgb_pixels = MakeHighEntropyRgb(64, 64);
  ASSERT_TRUE(
      RunCompress(&cinfo, &comp_err, &body_jpg, 64, 64, 3, rgb_pixels.data()));

  // Inject FF E5 <len=9502> <9500 padding bytes> immediately after FF D8.
  ControllableHandle with_large_app5;
  constexpr uint16_t kSkipPayloadBytes = 9500;
  constexpr uint16_t kMarkerLenField = kSkipPayloadBytes + 2;
  with_large_app5.data.push_back(0xFF);
  with_large_app5.data.push_back(0xD8);
  with_large_app5.data.push_back(0xFF);
  with_large_app5.data.push_back(0xE5);
  with_large_app5.data.push_back(
      static_cast<uint8_t>((kMarkerLenField >> 8) & 0xFF));
  with_large_app5.data.push_back(static_cast<uint8_t>(kMarkerLenField & 0xFF));
  with_large_app5.data.insert(with_large_app5.data.end(), kSkipPayloadBytes,
                              0x55);
  with_large_app5.data.insert(with_large_app5.data.end(),
                              body_jpg.data.begin() + 2, body_jpg.data.end());

  JpegErrorState dec_err;
  InitErrorState(&dec_err);
  struct jpeg_decompress_struct dinfo = {};
  dinfo.err = &dec_err.pub;
  jpeg_create_decompress(&dinfo);
  absl::Cleanup destroy_dinfo = [&dinfo] { jpeg_destroy_decompress(&dinfo); };

  std::vector<uint8_t> row_buf(64 * 3);
  int w = 0;
  int h = 0;
  int comps = 0;

  // 1. First decompress (dinfo.src == nullptr): small single-buffer image.
  small_jpg.Seek(0, SEEK_SET);
  ASSERT_TRUE(RunDecompress(&dinfo, &dec_err, &small_jpg, row_buf.data(), &w,
                            &h, &comps));
  EXPECT_EQ(4, w);
  EXPECT_EQ(4, h);
  EXPECT_EQ(1, comps);

  // 2. Second decompress on same dinfo (dinfo.src != nullptr): multi-buffer
  // APP5 skip (> 2 * 4096 bytes) + multi-buffer scanline decode.
  with_large_app5.Seek(0, SEEK_SET);
  ASSERT_TRUE(RunDecompress(&dinfo, &dec_err, &with_large_app5, row_buf.data(),
                            &w, &h, &comps));
  EXPECT_EQ(64, w);
  EXPECT_EQ(64, h);
  EXPECT_EQ(3, comps);
  EXPECT_EQ(0, dec_err.warning_count);
}

TEST(VsiDataIoTest, DecompressEmptyInputTriggersInputEmptyError) {
  ControllableHandle empty_input;

  JpegErrorState dec_err;
  InitErrorState(&dec_err);
  struct jpeg_decompress_struct dinfo = {};
  dinfo.err = &dec_err.pub;
  jpeg_create_decompress(&dinfo);
  absl::Cleanup destroy_dinfo = [&dinfo] { jpeg_destroy_decompress(&dinfo); };

  std::vector<uint8_t> row_buf(16);
  int w = 0;
  int h = 0;
  int comps = 0;
  EXPECT_FALSE(RunDecompress(&dinfo, &dec_err, &empty_input, row_buf.data(), &w,
                             &h, &comps));
  EXPECT_EQ(JERR_INPUT_EMPTY, dec_err.last_error_code);
}

TEST(VsiDataIoTest, DecompressTruncatedInputInsertsFakeEoiAndEmitsWarning) {
  JpegErrorState comp_err;
  InitErrorState(&comp_err);
  struct jpeg_compress_struct cinfo = {};
  cinfo.err = &comp_err.pub;
  jpeg_create_compress(&cinfo);
  absl::Cleanup destroy_cinfo = [&cinfo] { jpeg_destroy_compress(&cinfo); };

  ControllableHandle full_jpg;
  const std::vector<uint8_t> pixels = MakeHighEntropyRgb(32, 32);
  ASSERT_TRUE(
      RunCompress(&cinfo, &comp_err, &full_jpg, 32, 32, 3, pixels.data()));
  ASSERT_GT(full_jpg.data.size(), 100u);

  // Truncate the last 64 bytes so fill_input_buffer hits EOF mid-image
  // (start_of_file == FALSE), emitting JWRN_JPEG_EOF and inserting fake EOI.
  full_jpg.data.resize(full_jpg.data.size() - 64);
  full_jpg.Seek(0, SEEK_SET);

  JpegErrorState dec_err;
  InitErrorState(&dec_err);
  struct jpeg_decompress_struct dinfo = {};
  dinfo.err = &dec_err.pub;
  jpeg_create_decompress(&dinfo);
  absl::Cleanup destroy_dinfo = [&dinfo] { jpeg_destroy_decompress(&dinfo); };

  std::vector<uint8_t> row_buf(32 * 3);
  int w = 0;
  int h = 0;
  int comps = 0;
  EXPECT_TRUE(RunDecompress(&dinfo, &dec_err, &full_jpg, row_buf.data(), &w, &h,
                            &comps));
  EXPECT_GT(dec_err.warning_count, 0);
  EXPECT_TRUE(dec_err.saw_eof_warning);
}

TEST(VsiDataIoTest, CompressWriteAndFlushErrors) {
  JpegErrorState jerr;
  InitErrorState(&jerr);
  struct jpeg_compress_struct cinfo = {};
  cinfo.err = &jerr.pub;
  jpeg_create_compress(&cinfo);
  absl::Cleanup destroy_cinfo = [&cinfo] { jpeg_destroy_compress(&cinfo); };

  // 1. Write error inside empty_output_buffer (> 4096 bytes output).
  ControllableHandle fail_empty_out;
  fail_empty_out.fail_write = true;
  const std::vector<uint8_t> large_pixels = MakeHighEntropyRgb(96, 96);
  jerr.last_error_code = 0;
  EXPECT_FALSE(RunCompress(&cinfo, &jerr, &fail_empty_out, 96, 96, 3,
                           large_pixels.data()));
  EXPECT_EQ(JERR_FILE_WRITE, jerr.last_error_code);

  // 2. Write error inside term_destination (< 4096 bytes output, buffered
  // until jpeg_finish_compress).
  ControllableHandle fail_term_write;
  fail_term_write.fail_write = true;
  const std::vector<uint8_t> tiny_pixels(2 * 2, 42);
  jerr.last_error_code = 0;
  EXPECT_FALSE(RunCompress(&cinfo, &jerr, &fail_term_write, 2, 2, 1,
                           tiny_pixels.data()));
  EXPECT_EQ(JERR_FILE_WRITE, jerr.last_error_code);

  // 3. Flush error inside term_destination (Write succeeds, Flush fails).
  ControllableHandle fail_term_flush;
  fail_term_flush.fail_flush = true;
  jerr.last_error_code = 0;
  EXPECT_FALSE(RunCompress(&cinfo, &jerr, &fail_term_flush, 2, 2, 1,
                           tiny_pixels.data()));
  EXPECT_EQ(JERR_FILE_WRITE, jerr.last_error_code);
}

}  // namespace
}  // namespace autotest2
