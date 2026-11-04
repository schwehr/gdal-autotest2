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
// Tests for Arc/Info vector coverage miscellaneous utilities (avc_misc.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/avc/avc_misc.cpp

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/avc/avc.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(AvcMiscTest, ComputeRecSizeAllTypesAndErrors) {
  // Test all supported field types.
  AVCFieldInfo fields[9];
  std::memset(fields, 0, sizeof(fields));

  // 1. DATE: type1 = 1 (10), size = 8 -> 8 chars
  fields[0].nType1 = 1;
  fields[0].nSize = 8;
  // 2. CHAR: type1 = 2 (20), size = 16 -> 16 chars
  fields[1].nType1 = 2;
  fields[1].nSize = 16;
  // 3. FIXINT: type1 = 3 (30), size = 5 -> 5 chars
  fields[2].nType1 = 3;
  fields[2].nSize = 5;
  // 4. BININT 4 bytes: type1 = 5 (50), size = 4 -> 11 chars
  fields[3].nType1 = 5;
  fields[3].nSize = 4;
  // 5. BININT 2 bytes: type1 = 5 (50), size = 2 -> 6 chars
  fields[4].nType1 = 5;
  fields[4].nSize = 2;
  // 6. FIXNUM type 40 with size > 8, bMapType40ToDouble = TRUE -> 24 chars
  fields[5].nType1 = 4;
  fields[5].nSize = 10;
  // 7. FIXNUM type 40 with size <= 8 (or bMapType40ToDouble = FALSE) -> 14
  // chars
  fields[6].nType1 = 4;
  fields[6].nSize = 7;
  // 8. BINFLOAT 4 bytes: type1 = 6 (60), size = 4 -> 14 chars
  fields[7].nType1 = 6;
  fields[7].nSize = 4;
  // 9. BINFLOAT 8 bytes: type1 = 6 (60), size = 8 -> 24 chars
  fields[8].nType1 = 6;
  fields[8].nSize = 8;

  constexpr int kExpectedTotal = 8 + 16 + 5 + 11 + 6 + 24 + 14 + 14 + 24;
  EXPECT_EQ(kExpectedTotal,
            _AVCE00ComputeRecSize(9, fields, /*bMapType40ToDouble=*/TRUE));

  // When bMapType40ToDouble is FALSE, field 5 (size 10) uses 14 chars instead
  // of 24.
  constexpr int kExpectedWithoutMap = 8 + 16 + 5 + 11 + 6 + 14 + 14 + 14 + 24;
  EXPECT_EQ(kExpectedWithoutMap,
            _AVCE00ComputeRecSize(9, fields, /*bMapType40ToDouble=*/FALSE));

  // Error handling: unsupported field type.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    AVCFieldInfo bad_field;
    std::memset(&bad_field, 0, sizeof(bad_field));
    bad_field.nType1 = 5;  // BININT but invalid size 8
    bad_field.nSize = 8;

    EXPECT_EQ(-1, _AVCE00ComputeRecSize(1, &bad_field, TRUE));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unsupported field type"));
  }
}

TEST(AvcMiscTest, DupAndDestroyTableDefAndFields) {
  // Null safety.
  EXPECT_EQ(nullptr, _AVCDupTableDef(nullptr));
  _AVCDestroyTableDef(nullptr);
  _AVCDestroyTableFields(nullptr, nullptr);

  // Allocate a table definition with fields.
  AVCTableDef src_def;
  std::memset(&src_def, 0, sizeof(src_def));
  std::snprintf(src_def.szTableName, sizeof(src_def.szTableName), "%s",
                "TEST.AAT");
  src_def.numFields = 5;

  AVCFieldInfo field_infos[5];
  std::memset(field_infos, 0, sizeof(field_infos));
  // DATE
  field_infos[0].nType1 = 1;
  field_infos[0].nSize = 8;
  // CHAR
  field_infos[1].nType1 = 2;
  field_infos[1].nSize = 12;
  // FIXINT
  field_infos[2].nType1 = 3;
  field_infos[2].nSize = 6;
  // FIXNUM
  field_infos[3].nType1 = 4;
  field_infos[3].nSize = 10;
  // BININT
  field_infos[4].nType1 = 5;
  field_infos[4].nSize = 4;

  src_def.pasFieldDef = field_infos;

  AVCTableDef* dup_def = _AVCDupTableDef(&src_def);
  ASSERT_NE(nullptr, dup_def);
  absl::Cleanup def_cleaner = [dup_def] { _AVCDestroyTableDef(dup_def); };
  EXPECT_STREQ("TEST.AAT", dup_def->szTableName);
  EXPECT_EQ(5, dup_def->numFields);
  ASSERT_NE(nullptr, dup_def->pasFieldDef);
  EXPECT_EQ(1, dup_def->pasFieldDef[0].nType1);
  EXPECT_EQ(8, dup_def->pasFieldDef[0].nSize);

  // Allocate AVCField array and populate string buffers.
  AVCField* fields = static_cast<AVCField*>(CPLCalloc(5, sizeof(AVCField)));
  fields[0].pszStr = reinterpret_cast<GByte*>(CPLStrdup("20260101"));
  fields[1].pszStr = reinterpret_cast<GByte*>(CPLStrdup("HelloWorld"));
  fields[2].pszStr = reinterpret_cast<GByte*>(CPLStrdup("12345"));
  fields[3].pszStr = reinterpret_cast<GByte*>(CPLStrdup("123.45"));
  fields[4].nInt32 = 42;

  _AVCDestroyTableFields(dup_def, fields);
}

// Custom handler to test uppercase filename adjustments.
class UpperFilesystemHandler : public VSIFilesystemHandler {
 public:
  VSIVirtualHandleUniquePtr Open(const char* /*pszFilename*/,
                                 const char* /*pszAccess*/, bool /*bSetError*/,
                                 CSLConstList /*papszOptions*/) override {
    return nullptr;
  }
  int Stat(const char* pszFilename, VSIStatBufL* pStatBuf,
           int /*nFlags*/) override {
    std::memset(pStatBuf, 0, sizeof(*pStatBuf));
    if (std::strcmp(pszFilename, "/VSIMEM_UPPER/FILE.ADF") == 0) {
      return 0;
    }
    return -1;
  }
};

TEST(AvcMiscTest, AdjustCaseSensitiveFilenameAndFileExists) {
  const std::string dir = "/vsimem/avc_misc_test";
  ASSERT_EQ(0, VSIMkdir(dir.c_str(), 0777));
  absl::Cleanup dir_cleaner = [&dir] { VSIRmdirRecursive(dir.c_str()); };

  // 1. Exact match already valid.
  const std::string exact_file = dir + "/exact.adf";
  VsiMemTempWrapper wrapper1(exact_file, "data");
  char fname_exact[256];
  std::snprintf(fname_exact, sizeof(fname_exact), "%s", exact_file.c_str());
  EXPECT_STREQ(exact_file.c_str(), AVCAdjustCaseSensitiveFilename(fname_exact));

  // 2. Backslash and lowercase match.
  const std::string lower_file = dir + "/lower.adf";
  VsiMemTempWrapper wrapper2(lower_file, "data");
  char fname_lower[256];
  std::snprintf(fname_lower, sizeof(fname_lower), "%s\\LOWER.ADF", dir.c_str());
  EXPECT_STREQ(lower_file.c_str(), AVCAdjustCaseSensitiveFilename(fname_lower));

  // 3. Uppercase match via custom VSI handler.
  auto upper_handler = std::make_unique<UpperFilesystemHandler>();
  VSIFileManager::InstallHandler("/VSIMEM_UPPER/", upper_handler.get());
  absl::Cleanup upper_cleaner = [&upper_handler] {
    VSIFileManager::RemoveHandler("/VSIMEM_UPPER/");
    upper_handler.reset();
  };

  char fname_upper[256];
  std::snprintf(fname_upper, sizeof(fname_upper), "/vsimem_upper/File.Adf");
  EXPECT_STREQ("/VSIMEM_UPPER/FILE.ADF",
               AVCAdjustCaseSensitiveFilename(fname_upper));

  // 4. Multi-level sub-directory search and case adjustment.
  const std::string sub_dir = dir + "/SubDir";
  ASSERT_EQ(0, VSIMkdir(sub_dir.c_str(), 0777));
  const std::string mixed_file = sub_dir + "/MixedCase.adf";
  VsiMemTempWrapper wrapper3(mixed_file, "data");

  char fname_mixed[256];
  std::snprintf(fname_mixed, sizeof(fname_mixed), "%s\\subdir/mixedcase.ADF",
                dir.c_str());
  EXPECT_STREQ(mixed_file.c_str(), AVCAdjustCaseSensitiveFilename(fname_mixed));

  // 5. Non-existent path with missing subcomponents (returns unchanged).
  char fname_missing[256];
  std::snprintf(fname_missing, sizeof(fname_missing), "%s/MissingDir/file.adf",
                dir.c_str());
  EXPECT_STREQ((dir + "/MissingDir/file.adf").c_str(),
               AVCAdjustCaseSensitiveFilename(fname_missing));

  // 6. Relative path where iTmpPtr == 0.
  // Note: relative paths with iTmpPtr == 0 branch into VSIReadDir("") on the
  // CWD.
  char fname_rel[256];
  std::snprintf(fname_rel, sizeof(fname_rel), "nonexistent_rel_dir/file.adf");
  EXPECT_STREQ("nonexistent_rel_dir/file.adf",
               AVCAdjustCaseSensitiveFilename(fname_rel));

  // 7. AVCFileExists returns TRUE for existing files and FALSE for missing.
  EXPECT_TRUE(AVCFileExists(dir.c_str(), "/exact.adf"));
  EXPECT_TRUE(AVCFileExists(dir.c_str(), "/EXACT.ADF"));
  EXPECT_FALSE(AVCFileExists(dir.c_str(), "/not_found.adf"));
}

TEST(AvcMiscTest, PrintRealValueAllFormatsAndSigns) {
  char buf[128];

  // 1. AVC_FORMAT_DBF_FLOAT: positive and negative.
  std::memset(buf, 0, sizeof(buf));
  int len = AVCPrintRealValue(buf, sizeof(buf), AVC_FORMAT_DBF_FLOAT,
                              AVCFileTABLE, 123.456);
  EXPECT_EQ(13, len);
  EXPECT_EQ(13, std::strlen(buf));
  EXPECT_EQ(' ', buf[0]);

  std::memset(buf, 0, sizeof(buf));
  len = AVCPrintRealValue(buf, sizeof(buf), AVC_FORMAT_DBF_FLOAT, AVCFileTABLE,
                          -123.456);
  EXPECT_EQ(13, len);
  EXPECT_EQ(13, std::strlen(buf));
  EXPECT_EQ('-', buf[0]);

  // 2. AVC_DOUBLE_PREC with AVCFileTABLE: positive and negative.
  std::memset(buf, 0, sizeof(buf));
  len = AVCPrintRealValue(buf, sizeof(buf), AVC_DOUBLE_PREC, AVCFileTABLE,
                          3.141592653589793);
  EXPECT_EQ(24, len);
  EXPECT_EQ(24, std::strlen(buf));
  EXPECT_EQ(' ', buf[0]);

  std::memset(buf, 0, sizeof(buf));
  len = AVCPrintRealValue(buf, sizeof(buf), AVC_DOUBLE_PREC, AVCFileTABLE,
                          -3.141592653589793);
  EXPECT_EQ(24, len);
  EXPECT_EQ(24, std::strlen(buf));
  EXPECT_EQ('-', buf[0]);

  // 3. AVC_DOUBLE_PREC with non-TABLE (e.g. AVCFileARC).
  std::memset(buf, 0, sizeof(buf));
  len = AVCPrintRealValue(buf, sizeof(buf), AVC_DOUBLE_PREC, AVCFileARC,
                          12345.6789);
  EXPECT_EQ(21, len);
  EXPECT_EQ(21, std::strlen(buf));
  EXPECT_EQ(' ', buf[0]);

  std::memset(buf, 0, sizeof(buf));
  len = AVCPrintRealValue(buf, sizeof(buf), AVC_DOUBLE_PREC, AVCFileARC,
                          -12345.6789);
  EXPECT_EQ(21, len);
  EXPECT_EQ(21, std::strlen(buf));
  EXPECT_EQ('-', buf[0]);

  // 4. Single precision (AVC_SINGLE_PREC).
  std::memset(buf, 0, sizeof(buf));
  len = AVCPrintRealValue(buf, sizeof(buf), AVC_SINGLE_PREC, AVCFileARC, 42.5);
  EXPECT_EQ(14, len);
  EXPECT_EQ(14, std::strlen(buf));
  EXPECT_EQ(' ', buf[0]);

  std::memset(buf, 0, sizeof(buf));
  len = AVCPrintRealValue(buf, sizeof(buf), AVC_SINGLE_PREC, AVCFileARC, -42.5);
  EXPECT_EQ(14, len);
  EXPECT_EQ(14, std::strlen(buf));
  EXPECT_EQ('-', buf[0]);

  // 5. Append to existing non-empty buffer.
  std::snprintf(buf, sizeof(buf), "PREFIX:");
  len = AVCPrintRealValue(buf, sizeof(buf), AVC_SINGLE_PREC, AVCFileARC, 1.0);
  EXPECT_EQ(14, len);
  EXPECT_EQ(7 + 14, std::strlen(buf));
}

}  // namespace
}  // namespace autotest2
