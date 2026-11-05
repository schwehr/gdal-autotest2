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

#include <memory>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class FailingStatFSHandler final : public VSIFilesystemHandler {
 public:
  VSIVirtualHandleUniquePtr Open(const char* filename, const char* access,
                                 bool set_error,
                                 CSLConstList options) override {
    return VSIVirtualHandleUniquePtr(
        VSIFileManager::GetHandler("/vsimem/")
            ->Open("/vsimem/dummy.id", access, set_error, options));
  }
  int Stat(const char* filename, VSIStatBufL* stat_buf, int flags) override {
    return -1;
  }
};

class LargeStatFSHandler final : public VSIFilesystemHandler {
 public:
  VSIVirtualHandleUniquePtr Open(const char* filename, const char* access,
                                 bool set_error,
                                 CSLConstList options) override {
    return VSIVirtualHandleUniquePtr(
        VSIFileManager::GetHandler("/vsimem/")
            ->Open("/vsimem/dummy.id", access, set_error, options));
  }
  int Stat(const char* filename, VSIStatBufL* stat_buf, int flags) override {
    memset(stat_buf, 0, sizeof(*stat_buf));
    stat_buf->st_size = static_cast<vsi_l_offset>(INT_MAX / 4) + 100;
    return 0;
  }
};

TEST(MitabIDFileTest, OpenErrorsAndEdgeCases) {
  TABIDFile id_file;

  // Unopened operations
  EXPECT_EQ(-1, id_file.GetObjPtr(1));
  EXPECT_EQ(-1, id_file.SetObjPtr(1, 100));

  // Invalid mode string
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, id_file.Open("/vsimem/id_test/invalid.id", "x"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Non-existent read
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, id_file.Open("/vsimem/id_test/nonexistent.id", "r"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Open lowercase .map with TABWrite (hits .map -> .id substitution)
  constexpr char kLowerMap[] = "/vsimem/id_test/lower.map";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/id_test"); };
  VSIMkdir("/vsimem/id_test", 0777);

  EXPECT_EQ(0, id_file.Open(kLowerMap, TABWrite));
  EXPECT_EQ(0, id_file.Close());
  // After Close(), m_poIDBlock is nullptr, but access mode was write, so
  // SyncToDisk returns 0.
  EXPECT_EQ(0, id_file.SyncToDisk());

  // Open existing file with TABReadWrite
  EXPECT_EQ(0, id_file.Open(kLowerMap, TABReadWrite));
  EXPECT_EQ(0, id_file.SetObjPtr(1, 512));
  EXPECT_EQ(0, id_file.Close());
}

TEST(MitabIDFileTest, WriteReadAndMultiBlock) {
  constexpr char kPath[] = "/vsimem/id_test/test.id";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/id_test"); };
  VSIMkdir("/vsimem/id_test", 0777);

  // Write mode
  {
    TABIDFile id_file;
    EXPECT_EQ(0, id_file.Open(kPath, "w"));
    // Open when already open
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(-1, id_file.Open(kPath, "w"));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    // Invalid nObjId <= 0
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(-1, id_file.SetObjPtr(0, 100));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    // Set first object
    EXPECT_EQ(0, id_file.SetObjPtr(1, 512));
    EXPECT_EQ(1, id_file.GetMaxObjId());

    // Set an object in a much later block (nObjId = 600 > 256 per 1024-byte
    // block)
    EXPECT_EQ(0, id_file.SetObjPtr(600, 2048));
    EXPECT_EQ(600, id_file.GetMaxObjId());

    // Update an earlier object in an already committed block
    EXPECT_EQ(0, id_file.SetObjPtr(2, 768));

    EXPECT_EQ(0, id_file.SyncToDisk());
    EXPECT_EQ(0, id_file.Close());
    EXPECT_EQ(0, id_file.Close());
  }

  // Read mode
  {
    TABIDFile id_file;
    // Test that passing .MAP extension automatically opens .id
    constexpr char kMapPath[] = "/vsimem/id_test/test.MAP";
    EXPECT_EQ(0, id_file.Open(kMapPath, "r"));
    EXPECT_EQ(600, id_file.GetMaxObjId());

    // Read back objects
    EXPECT_EQ(512, id_file.GetObjPtr(1));
    EXPECT_EQ(768, id_file.GetObjPtr(2));
    EXPECT_EQ(2048, id_file.GetObjPtr(600));

    // Non-set object within range returns 0
    EXPECT_EQ(0, id_file.GetObjPtr(300));

    // Invalid object IDs
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(-1, id_file.GetObjPtr(0));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

      CPLErrorReset();
      EXPECT_EQ(-1, id_file.GetObjPtr(601));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    // Mutating in read-only mode
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(-1, id_file.SetObjPtr(1, 999));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

      CPLErrorReset();
      EXPECT_EQ(-1, id_file.SyncToDisk());
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    EXPECT_EQ(0, id_file.Close());
  }
}

TEST(MitabIDFileTest, EmptyFileRead) {
  constexpr char kPath[] = "/vsimem/id_test/empty.id";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/id_test"); };
  VSIMkdir("/vsimem/id_test", 0777);

  VsiMemTempWrapper file(kPath, "");
  TABIDFile id_file;
  EXPECT_EQ(0, id_file.Open(kPath, "r"));
  EXPECT_EQ(0, id_file.GetMaxObjId());
  EXPECT_EQ(0, id_file.Close());
}

TEST(MitabIDFileTest, TruncatedFileGotoByteFailure) {
  constexpr char kPath[] = "/vsimem/id_test/trunc.id";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/id_test"); };
  VSIMkdir("/vsimem/id_test", 0777);

  // Write 2048 bytes (512 IDs)
  {
    TABIDFile id_file;
    EXPECT_EQ(0, id_file.Open(kPath, "w"));
    EXPECT_EQ(0, id_file.SetObjPtr(512, 1000));
    EXPECT_EQ(0, id_file.Close());
  }

  // Open for read, then truncate behind its back to 1024 bytes
  {
    TABIDFile id_file;
    EXPECT_EQ(0, id_file.Open(kPath, "r"));
    EXPECT_EQ(512, id_file.GetMaxObjId());

    // Truncate file on disk to 1024 bytes
    VSILFILE* fp = VSIFOpenL(kPath, "rb+");
    ASSERT_NE(nullptr, fp);
    EXPECT_EQ(0, VSIFTruncateL(fp, 1024));
    EXPECT_EQ(0, VSIFCloseL(fp));

    // Reading object in truncated block fails in GotoByteInFile (line 271)
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, id_file.GetObjPtr(300));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();

    EXPECT_EQ(0, id_file.Close());
  }
}

TEST(MitabIDFileTest, OpenStatFailureAndLargeSize) {
  constexpr char kDummyPath[] = "/vsimem/dummy.id";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/dummy.id"); };
  VsiMemTempWrapper dummy_file(kDummyPath, "");

  // Failing Stat (lines 153-156)
  {
    constexpr char kFailFsPrefix[] = "/vsifailstat/";
    FailingStatFSHandler handler;
    VSIFileManager::InstallHandler(kFailFsPrefix, &handler);
    absl::Cleanup remove_handler = [kFailFsPrefix] {
      VSIFileManager::RemoveHandler(kFailFsPrefix);
    };

    TABIDFile id_file;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, id_file.Open("/vsifailstat/test.id", "r"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Large Stat size > INT_MAX/4 and ReadFromFile failure on empty underlying
  // file (lines 161, 179-180)
  {
    constexpr char kLargeFsPrefix[] = "/vsilargestat/";
    LargeStatFSHandler handler;
    VSIFileManager::InstallHandler(kLargeFsPrefix, &handler);
    absl::Cleanup remove_handler = [kLargeFsPrefix] {
      VSIFileManager::RemoveHandler(kLargeFsPrefix);
    };

    TABIDFile id_file;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, id_file.Open("/vsilargestat/test.id", "r"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
