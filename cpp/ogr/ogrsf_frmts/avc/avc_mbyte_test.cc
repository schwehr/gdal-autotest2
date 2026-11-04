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
// Tests for Arc/Info vector coverage multibyte conversions (avc_mbyte.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/avc/avc_mbyte.cpp

// clang-format off
#include "third_party/gdal/port/cpl_port.h"  // IWYU pragma: keep
#include "third_party/gdal/ogr/ogrsf_frmts/avc/avc_mbyte.h"
// clang-format on

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/ogr/ogrsf_frmts/avc/avc.h"

namespace autotest2 {
namespace {

TEST(AvcMbyteTest, AllocAndFreeAndCodePage) {
  // On non-Windows platforms (like Linux google3), AVCGetDBCSCodePage returns
  // 0.
  EXPECT_EQ(0, AVCGetDBCSCodePage());

  AVCDBCSInfo* info = AVCAllocDBCSInfo();
  ASSERT_NE(nullptr, info);
  EXPECT_EQ(0, info->nDBCSCodePage);
  EXPECT_EQ(AVC_CODE_UNKNOWN, info->nDBCSEncoding);
  EXPECT_EQ(nullptr, info->pszDBCSBuf);
  EXPECT_EQ(0, info->nDBCSBufSize);

  AVCFreeDBCSInfo(info);
  // Freeing null pointer should be safe.
  AVCFreeDBCSInfo(nullptr);
}

TEST(AvcMbyteTest, DetectEncodingEdgeCases) {
  // 1. Null info or null string.
  EXPECT_TRUE(AVCE00DetectEncoding(nullptr, nullptr));

  AVCDBCSInfo* info = AVCAllocDBCSInfo();
  ASSERT_NE(nullptr, info);
  absl::Cleanup info_cleaner = [info] { AVCFreeDBCSInfo(info); };

  // 2. Codepage == 0 (single byte) returns TRUE immediately without scanning.
  info->nDBCSCodePage = 0;
  EXPECT_TRUE(
      AVCE00DetectEncoding(info, reinterpret_cast<const GByte*>("abc")));

  // 3. Encoding already known returns TRUE immediately.
  info->nDBCSCodePage = AVC_DBCS_JAPANESE;
  info->nDBCSEncoding = AVC_CODE_JAP_EUC;
  EXPECT_TRUE(
      AVCE00DetectEncoding(info, reinterpret_cast<const GByte*>("abc")));

  // 4. Unsupported codepage returns TRUE and keeps encoding UNKNOWN.
  info->nDBCSCodePage = 9999;
  info->nDBCSEncoding = AVC_CODE_UNKNOWN;
  EXPECT_TRUE(
      AVCE00DetectEncoding(info, reinterpret_cast<const GByte*>("abc")));
  EXPECT_EQ(AVC_CODE_UNKNOWN, info->nDBCSEncoding);

  // 5. Japanese codepage with pure ASCII returns FALSE (needs more input).
  info->nDBCSCodePage = AVC_DBCS_JAPANESE;
  info->nDBCSEncoding = AVC_CODE_UNKNOWN;
  EXPECT_FALSE(
      AVCE00DetectEncoding(info, reinterpret_cast<const GByte*>("Pure ASCII")));
  EXPECT_EQ(AVC_CODE_UNKNOWN, info->nDBCSEncoding);

  // 6. Null line with UNKNOWN encoding returns FALSE.
  EXPECT_FALSE(AVCE00DetectEncoding(info, nullptr));

  // 7. Shift-JIS leader byte (0x81..0x9F).
  info->nDBCSEncoding = AVC_CODE_UNKNOWN;
  const GByte sjis1_bytes[] = {0x81, 0x40, 0x00};
  EXPECT_TRUE(AVCE00DetectEncoding(info, sjis1_bytes));
  EXPECT_EQ(AVC_CODE_JAP_SHIFTJIS, info->nDBCSEncoding);

  // 8. Katakana followed by byte <= 0xA0 -> Shift-JIS.
  info->nDBCSEncoding = AVC_CODE_UNKNOWN;
  const GByte kana_sjis_bytes[] = {0xA1, 0x80, 0x00};
  EXPECT_TRUE(AVCE00DetectEncoding(info, kana_sjis_bytes));
  EXPECT_EQ(AVC_CODE_JAP_SHIFTJIS, info->nDBCSEncoding);

  // 9. EUC-1 byte (0xF0..0xFE) -> EUC.
  info->nDBCSEncoding = AVC_CODE_UNKNOWN;
  const GByte euc1_bytes[] = {0xF0, 0xA1, 0x00};
  EXPECT_TRUE(AVCE00DetectEncoding(info, euc1_bytes));
  EXPECT_EQ(AVC_CODE_JAP_EUC, info->nDBCSEncoding);

  // 10. Non-ASCII byte ending string (increment reaches '\0').
  info->nDBCSEncoding = AVC_CODE_UNKNOWN;
  const GByte truncated_byte[] = {0xE0, 0x00};
  EXPECT_FALSE(AVCE00DetectEncoding(info, truncated_byte));
  EXPECT_EQ(AVC_CODE_UNKNOWN, info->nDBCSEncoding);

  // 11. Second byte matches Shift-JIS 2 ([0x40, 0x7E] or [0x80, 0xA0]).
  info->nDBCSEncoding = AVC_CODE_UNKNOWN;
  const GByte sjis2_bytes_a[] = {0xE0, 0x40, 0x00};
  EXPECT_TRUE(AVCE00DetectEncoding(info, sjis2_bytes_a));
  EXPECT_EQ(AVC_CODE_JAP_SHIFTJIS, info->nDBCSEncoding);

  info->nDBCSEncoding = AVC_CODE_UNKNOWN;
  const GByte sjis2_bytes_b[] = {0xE0, 0x85, 0x00};
  EXPECT_TRUE(AVCE00DetectEncoding(info, sjis2_bytes_b));
  EXPECT_EQ(AVC_CODE_JAP_SHIFTJIS, info->nDBCSEncoding);

  // 12. Second byte matches EUC 2 ([0xFD, 0xFE]).
  info->nDBCSEncoding = AVC_CODE_UNKNOWN;
  const GByte euc2_bytes[] = {0xE0, 0xFD, 0x00};
  EXPECT_TRUE(AVCE00DetectEncoding(info, euc2_bytes));
  EXPECT_EQ(AVC_CODE_JAP_EUC, info->nDBCSEncoding);

  // 13. Ambiguous two bytes not matching any specific check (e.g. 0xE0, 0xA1).
  info->nDBCSEncoding = AVC_CODE_UNKNOWN;
  const GByte ambig_bytes[] = {0xE0, 0xA1, 0x00};
  EXPECT_FALSE(AVCE00DetectEncoding(info, ambig_bytes));
  EXPECT_EQ(AVC_CODE_UNKNOWN, info->nDBCSEncoding);
}

TEST(AvcMbyteTest, Convert2ArcDBCS) {
  // 1. Null parameters or single-byte codepage return original pointer.
  const GByte sample[] = {'h', 'e', 'l', 'l', 'o', '\0'};
  EXPECT_EQ(sample, AVCE00Convert2ArcDBCS(nullptr, sample, 10));
  EXPECT_EQ(nullptr, AVCE00Convert2ArcDBCS(nullptr, nullptr, 10));

  AVCDBCSInfo* info = AVCAllocDBCSInfo();
  ASSERT_NE(nullptr, info);
  absl::Cleanup info_cleaner = [info] { AVCFreeDBCSInfo(info); };

  info->nDBCSCodePage = 0;
  EXPECT_EQ(sample, AVCE00Convert2ArcDBCS(info, sample, 10));

  // 2. All-ASCII with Japanese codepage returns original pointer.
  info->nDBCSCodePage = AVC_DBCS_JAPANESE;
  EXPECT_EQ(sample, AVCE00Convert2ArcDBCS(info, sample, 10));

  // 3. EUC encoding preserves EUC pair and ASCII characters.
  info->nDBCSEncoding = AVC_CODE_JAP_EUC;
  const GByte euc_input[] = {0xA4, 0xA2, 'A', 0x00};
  const GByte* euc_res = AVCE00Convert2ArcDBCS(info, euc_input, 10);
  ASSERT_NE(nullptr, euc_res);
  EXPECT_EQ(0xA4, euc_res[0]);
  EXPECT_EQ(0xA2, euc_res[1]);
  EXPECT_EQ('A', euc_res[2]);
  EXPECT_EQ('\0', euc_res[3]);

  // 4. Katakana char prefixes with 0x8E.
  info->nDBCSEncoding = AVC_CODE_JAP_SHIFTJIS;
  const GByte kana_input[] = {0xB1, 0x00};
  const GByte* kana_res = AVCE00Convert2ArcDBCS(info, kana_input, 10);
  ASSERT_NE(nullptr, kana_res);
  EXPECT_EQ(0x8E, kana_res[0]);
  EXPECT_EQ(0xB1, kana_res[1]);
  EXPECT_EQ('\0', kana_res[2]);

  // 5. Shift-JIS pair conversion (leader <= 0x9F and leader > 0x9F).
  // 5a. leader <= 0x9F, trailer <= 0x7F
  const GByte sjis_a[] = {0x82, 0x60, 0x00};
  const GByte* res_a = AVCE00Convert2ArcDBCS(info, sjis_a, 10);
  ASSERT_NE(nullptr, res_a);
  EXPECT_EQ(0xA3, res_a[0]);
  EXPECT_EQ(0xC1, res_a[1]);

  // 5b. leader <= 0x9F, trailer >= 0x9E
  const GByte sjis_b[] = {0x82, 0x9F, 0x00};
  const GByte* res_b = AVCE00Convert2ArcDBCS(info, sjis_b, 10);
  ASSERT_NE(nullptr, res_b);
  EXPECT_EQ(0xA4, res_b[0]);
  EXPECT_EQ(0xA1, res_b[1]);

  // 5c. leader <= 0x9F, 0x7F < trailer < 0x9E
  const GByte sjis_c[] = {0x82, 0x85, 0x00};
  const GByte* res_c = AVCE00Convert2ArcDBCS(info, sjis_c, 10);
  ASSERT_NE(nullptr, res_c);
  EXPECT_EQ(0xA3, res_c[0]);
  EXPECT_EQ(0xE5, res_c[1]);

  // 5d. leader > 0x9F, trailer >= 0x9E
  const GByte sjis_d[] = {0xE0, 0xA0, 0x00};
  const GByte* res_d = AVCE00Convert2ArcDBCS(info, sjis_d, 10);
  ASSERT_NE(nullptr, res_d);
  EXPECT_EQ(0xE0, res_d[0]);
  EXPECT_EQ(0xA2, res_d[1]);

  // 5e. Truncated non-ASCII at end (line 420 else branch).
  const GByte truncated[] = {'A', 0x81, 0x00};
  const GByte* res_trunc = AVCE00Convert2ArcDBCS(info, truncated, 10);
  ASSERT_NE(nullptr, res_trunc);
  EXPECT_EQ('A', res_trunc[0]);
  EXPECT_EQ(0x81, res_trunc[1]);

  // 6. Autodetect encoding inside AVCE00Convert2ArcDBCS when initially UNKNOWN.
  info->nDBCSEncoding = AVC_CODE_UNKNOWN;
  const GByte auto_sjis[] = {0x81, 0x40, 0x00};
  const GByte* auto_res = AVCE00Convert2ArcDBCS(info, auto_sjis, 10);
  ASSERT_NE(nullptr, auto_res);
  EXPECT_EQ(AVC_CODE_JAP_SHIFTJIS, info->nDBCSEncoding);

  // 7. MaxOutputLen limiting output (needs non-ASCII so bAllAscii is false).
  // Single Katakana 0xB1 becomes 2 bytes (0x8E, 0xB1). With limit 2, loop exits
  // before reading subsequent 'A', 'B'.
  const GByte limited_input[] = {static_cast<GByte>(0xB1), 'A', 'B', 0x00};
  const GByte* limited_res = AVCE00Convert2ArcDBCS(info, limited_input, 2);
  ASSERT_NE(nullptr, limited_res);
  EXPECT_EQ(0x8E, limited_res[0]);
  EXPECT_EQ(0xB1, limited_res[1]);
  EXPECT_EQ('\0', limited_res[2]);
}

TEST(AvcMbyteTest, ConvertFromArcDBCS) {
  // 1. Null parameters or single-byte codepage return original pointer.
  const GByte sample[] = {'w', 'o', 'r', 'l', 'd', '\0'};
  EXPECT_EQ(sample, AVCE00ConvertFromArcDBCS(nullptr, sample, 10));
  EXPECT_EQ(nullptr, AVCE00ConvertFromArcDBCS(nullptr, nullptr, 10));

  AVCDBCSInfo* info = AVCAllocDBCSInfo();
  ASSERT_NE(nullptr, info);
  absl::Cleanup info_cleaner = [info] { AVCFreeDBCSInfo(info); };

  info->nDBCSCodePage = 0;
  EXPECT_EQ(sample, AVCE00ConvertFromArcDBCS(info, sample, 10));

  // 2. All-ASCII with Japanese codepage returns original pointer.
  info->nDBCSCodePage = AVC_DBCS_JAPANESE;
  EXPECT_EQ(sample, AVCE00ConvertFromArcDBCS(info, sample, 10));

  // 3. Unsupported codepage with non-ASCII returns original pointer (default:
  // branch).
  info->nDBCSCodePage = 1234;
  const GByte non_ascii[] = {0xA1, 0xA1, 0x00};
  EXPECT_EQ(non_ascii, AVCE00ConvertFromArcDBCS(info, non_ascii, 10));

  // 4. Japanese codepage:
  info->nDBCSCodePage = AVC_DBCS_JAPANESE;

  // 4a. 0x8E Katakana flush (0x8E followed by byte).
  const GByte arc_kana[] = {'A', 0x8E, 0xB1, 'B', 0x00};
  const GByte* kana_out = AVCE00ConvertFromArcDBCS(info, arc_kana, 10);
  ASSERT_NE(nullptr, kana_out);
  EXPECT_EQ('A', kana_out[0]);
  EXPECT_EQ(0xB1, kana_out[1]);
  EXPECT_EQ('B', kana_out[2]);
  EXPECT_EQ('\0', kana_out[3]);

  // 4b. EUC pair to Shift-JIS conversion:
  // Odd leader with trailer < 0x7F
  const GByte euc_odd_low[] = {0xA1, 0xA1, 0x00};
  const GByte* sjis_odd_low = AVCE00ConvertFromArcDBCS(info, euc_odd_low, 10);
  ASSERT_NE(nullptr, sjis_odd_low);
  EXPECT_EQ(0x81, sjis_odd_low[0]);
  EXPECT_EQ(0x40, sjis_odd_low[1]);

  // Odd leader with trailer >= 0x7F
  const GByte euc_odd_high[] = {0xA1, 0xFE, 0x00};
  const GByte* sjis_odd_high = AVCE00ConvertFromArcDBCS(info, euc_odd_high, 10);
  ASSERT_NE(nullptr, sjis_odd_high);
  EXPECT_EQ(0x81, sjis_odd_high[0]);
  EXPECT_EQ(0x9E, sjis_odd_high[1]);

  // Even leader
  const GByte euc_even[] = {0xA2, 0xA1, 0x00};
  const GByte* sjis_even = AVCE00ConvertFromArcDBCS(info, euc_even, 10);
  ASSERT_NE(nullptr, sjis_even);
  EXPECT_EQ(0x81, sjis_even[0]);
  EXPECT_EQ(0x9F, sjis_even[1]);

  // High leader (leader > 0x9F after adjustment)
  const GByte euc_high[] = {0xDF, 0xA1, 0x00};
  const GByte* sjis_high = AVCE00ConvertFromArcDBCS(info, euc_high, 10);
  ASSERT_NE(nullptr, sjis_high);
  EXPECT_EQ(0xE0, sjis_high[0]);
  EXPECT_EQ(0x40, sjis_high[1]);

  // 4c. Truncated byte at end (line 489 else branch).
  const GByte trunc_arc[] = {'A', 0xA1, 0x00};
  const GByte* trunc_out = AVCE00ConvertFromArcDBCS(info, trunc_arc, 10);
  ASSERT_NE(nullptr, trunc_out);
  EXPECT_EQ('A', trunc_out[0]);
  EXPECT_EQ(0xA1, trunc_out[1]);
  EXPECT_EQ('\0', trunc_out[2]);

  // 4d. Truncated 0x8E at end.
  const GByte trunc_8e[] = {'A', 0x8E, 0x00};
  const GByte* trunc_8e_out = AVCE00ConvertFromArcDBCS(info, trunc_8e, 10);
  ASSERT_NE(nullptr, trunc_8e_out);
  EXPECT_EQ('A', trunc_8e_out[0]);
  EXPECT_EQ(0x8E, trunc_8e_out[1]);
  EXPECT_EQ('\0', trunc_8e_out[2]);
}

}  // namespace
}  // namespace autotest2
