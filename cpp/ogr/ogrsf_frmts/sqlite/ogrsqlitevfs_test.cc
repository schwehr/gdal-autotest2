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
// Tests for SQLite VFS implementation (ogrsqlitevfs.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/sqlite/ogrsqlitevfs.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogrsqlitevfs.h"

#include <array>
#include <cstring>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogr_sqlite.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/sqlite/sqlite3.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/sqlite_vfs_test";

class OGRSQLiteVFSTest : public ::testing::Test {
 protected:
  void SetUp() override { VSIMkdir(kTestDir, 0777); }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }
};

static void DummyNotify(void* pUserData, const char* pszFilename,
                        VSILFILE* /*fp*/) {
  int* pCount = static_cast<int*>(pUserData);
  if (pCount != nullptr && pszFilename != nullptr) {
    (*pCount)++;
  }
}

static void FreeVFS(sqlite3_vfs* pVFS) {
  if (pVFS != nullptr) {
    sqlite3_vfs_unregister(pVFS);
    CPLFree(pVFS->pAppData);
    CPLFree(pVFS);
  }
}

TEST_F(OGRSQLiteVFSTest, VFSLifecycleAndNotifyCallback) {
  int nNotifyCount = 0;
  sqlite3_vfs* pVFS = OGRSQLiteCreateVFS(DummyNotify, &nNotifyCount);
  ASSERT_NE(nullptr, pVFS);
  absl::Cleanup vfs_cleaner = [pVFS] { FreeVFS(pVFS); };

  ASSERT_EQ(SQLITE_OK, sqlite3_vfs_register(pVFS, 0));

  std::string osDbPath = std::string(kTestDir) + "/notify.db";
  sqlite3* hDB = nullptr;
  ASSERT_EQ(
      SQLITE_OK,
      sqlite3_open_v2(osDbPath.c_str(), &hDB,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, pVFS->zName));
  EXPECT_GT(nNotifyCount, 0);

  EXPECT_EQ(SQLITE_OK,
            sqlite3_exec(hDB, "CREATE TABLE t(x);", nullptr, nullptr, nullptr));
  EXPECT_EQ(SQLITE_OK, sqlite3_close(hDB));
}

TEST_F(OGRSQLiteVFSTest, DirectVFSMethods) {
  sqlite3_vfs* pVFS = OGRSQLiteCreateVFS(nullptr, nullptr);
  ASSERT_NE(nullptr, pVFS);
  absl::Cleanup vfs_cleaner = [pVFS] { FreeVFS(pVFS); };

  // 1. xFullPathname
  char szOut[1024];
  EXPECT_EQ(SQLITE_OK,
            pVFS->xFullPathname(pVFS, "/vsimem/foo.db", sizeof(szOut), szOut));
  EXPECT_STREQ("/vsimem/foo.db", szOut);

  // Relative pathname branch
  EXPECT_EQ(SQLITE_OK,
            pVFS->xFullPathname(pVFS, "rel.db", sizeof(szOut), szOut));

  // Buffer too small triggers SQLITE_CANTOPEN
  char szTiny[2];
  EXPECT_EQ(SQLITE_CANTOPEN, pVFS->xFullPathname(pVFS, "/vsimem/foo.db",
                                                 sizeof(szTiny), szTiny));

  // 2. xAccess
  std::string osFile = std::string(kTestDir) + "/acc.db";
  int nRes = 0;
  EXPECT_EQ(SQLITE_OK,
            pVFS->xAccess(pVFS, osFile.c_str(), SQLITE_ACCESS_EXISTS, &nRes));
  EXPECT_EQ(0, nRes);

  // Special vsicurl / vsitar / vsizip paths return 0 immediately
  EXPECT_EQ(SQLITE_OK, pVFS->xAccess(pVFS, "/vsicurl/http://foo/bar-journal",
                                     SQLITE_ACCESS_EXISTS, &nRes));
  EXPECT_EQ(0, nRes);
  EXPECT_EQ(SQLITE_OK, pVFS->xAccess(pVFS, "/vsitar/foo.tar/bar-wal",
                                     SQLITE_ACCESS_EXISTS, &nRes));
  EXPECT_EQ(0, nRes);

  // 3. xDlOpen, xDlError, xDlSym
  EXPECT_EQ(nullptr, pVFS->xDlOpen(pVFS, "libfoo.so"));
  pVFS->xDlError(pVFS, sizeof(szOut), szOut);
  EXPECT_EQ(nullptr, pVFS->xDlSym(pVFS, nullptr, "sym"));

  // 4. xRandomness, xSleep, xCurrentTime, xCurrentTimeInt64, xGetLastError
  char szBuf[16];
  EXPECT_GT(pVFS->xRandomness(pVFS, sizeof(szBuf), szBuf), 0);
  EXPECT_GE(pVFS->xSleep(pVFS, 10), 0);
  double dfTime = 0.0;
  EXPECT_EQ(SQLITE_OK, pVFS->xCurrentTime(pVFS, &dfTime));
  EXPECT_GT(dfTime, 0.0);
  sqlite3_int64 nTime64 = 0;
  EXPECT_EQ(SQLITE_OK, pVFS->xCurrentTimeInt64(pVFS, &nTime64));
  EXPECT_GT(nTime64, 0);
  EXPECT_GE(pVFS->xGetLastError(pVFS, sizeof(szOut), szOut), 0);
}

TEST_F(OGRSQLiteVFSTest, FileOperationsAndIOMethods) {
  sqlite3_vfs* pVFS = OGRSQLiteCreateVFS(nullptr, nullptr);
  ASSERT_NE(nullptr, pVFS);
  absl::Cleanup vfs_cleaner = [pVFS] { FreeVFS(pVFS); };

  std::string osFile = std::string(kTestDir) + "/io.db";
  auto fileStorage = std::make_unique<char[]>(pVFS->szOsFile);
  sqlite3_file* pFile = reinterpret_cast<sqlite3_file*>(fileStorage.get());

  int nOutFlags = 0;
  // Open create
  ASSERT_EQ(SQLITE_OK, pVFS->xOpen(pVFS, osFile.c_str(), pFile,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                   &nOutFlags));

  // xWrite
  const char szData[] = "12345678";
  EXPECT_EQ(SQLITE_OK, pFile->pMethods->xWrite(pFile, szData, 8, 0));

  // xFileSize
  sqlite3_int64 nSize = 0;
  EXPECT_EQ(SQLITE_OK, pFile->pMethods->xFileSize(pFile, &nSize));
  EXPECT_EQ(8, nSize);

  // xRead full
  char szReadBuf[16] = {};
  EXPECT_EQ(SQLITE_OK, pFile->pMethods->xRead(pFile, szReadBuf, 8, 0));
  EXPECT_STREQ("12345678", szReadBuf);

  // xRead past EOF (short read)
  std::memset(szReadBuf, 0xFF, sizeof(szReadBuf));
  EXPECT_EQ(SQLITE_IOERR_SHORT_READ,
            pFile->pMethods->xRead(pFile, szReadBuf, 16, 0));
  EXPECT_EQ(0, szReadBuf[8]);  // Short read is zero-filled

  // xTruncate
  EXPECT_EQ(SQLITE_OK, pFile->pMethods->xTruncate(pFile, 4));
  EXPECT_EQ(SQLITE_OK, pFile->pMethods->xFileSize(pFile, &nSize));
  EXPECT_EQ(4, nSize);

  // xSync, locks, file control, sector size
  EXPECT_EQ(SQLITE_OK, pFile->pMethods->xSync(pFile, 0));
  EXPECT_EQ(SQLITE_OK, pFile->pMethods->xLock(pFile, SQLITE_LOCK_SHARED));
  int nResLock = 0;
  EXPECT_EQ(SQLITE_OK, pFile->pMethods->xCheckReservedLock(pFile, &nResLock));
  EXPECT_EQ(SQLITE_OK, pFile->pMethods->xUnlock(pFile, SQLITE_LOCK_NONE));
  EXPECT_EQ(SQLITE_NOTFOUND,
            pFile->pMethods->xFileControl(pFile, 9999, nullptr));
  EXPECT_EQ(0, pFile->pMethods->xSectorSize(pFile));
  EXPECT_GE(pFile->pMethods->xDeviceCharacteristics(pFile), 0);

  // xClose
  EXPECT_EQ(SQLITE_OK, pFile->pMethods->xClose(pFile));

  // Open read-only
  ASSERT_EQ(SQLITE_OK, pVFS->xOpen(pVFS, osFile.c_str(), pFile,
                                   SQLITE_OPEN_READONLY, &nOutFlags));
  // Write to read-only file fails with SQLITE_IOERR_WRITE
  EXPECT_EQ(SQLITE_IOERR_WRITE, pFile->pMethods->xWrite(pFile, szData, 4, 0));
  EXPECT_EQ(SQLITE_OK, pFile->pMethods->xClose(pFile));

  // Open read-write on existing file
  ASSERT_EQ(SQLITE_OK, pVFS->xOpen(pVFS, osFile.c_str(), pFile,
                                   SQLITE_OPEN_READWRITE, &nOutFlags));
  EXPECT_EQ(SQLITE_OK, pFile->pMethods->xClose(pFile));

  // Open with flags = 0 returns SQLITE_CANTOPEN without leaking
  EXPECT_EQ(SQLITE_CANTOPEN,
            pVFS->xOpen(pVFS, osFile.c_str(), pFile, 0, &nOutFlags));

  // Open non-existent read-only file fails
  EXPECT_EQ(SQLITE_CANTOPEN,
            pVFS->xOpen(pVFS, "/vsimem/sqlite_vfs_test/non_existent.db", pFile,
                        SQLITE_OPEN_READONLY, &nOutFlags));

  // Open with zName = nullptr (temp file)
  ASSERT_EQ(SQLITE_OK, pVFS->xOpen(pVFS, nullptr, pFile,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                       SQLITE_OPEN_DELETEONCLOSE,
                                   &nOutFlags));
  EXPECT_EQ(SQLITE_OK, pFile->pMethods->xClose(pFile));

  // Delete on close with named file
  std::string osDelFile = std::string(kTestDir) + "/del.db";
  ASSERT_EQ(SQLITE_OK, pVFS->xOpen(pVFS, osDelFile.c_str(), pFile,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                       SQLITE_OPEN_DELETEONCLOSE,
                                   &nOutFlags));
  EXPECT_EQ(SQLITE_OK, pFile->pMethods->xClose(pFile));
  VSIStatBufL sStat;
  EXPECT_NE(0, VSIStatL(osDelFile.c_str(), &sStat));

  // xDelete
  EXPECT_EQ(SQLITE_OK, pVFS->xDelete(pVFS, osFile.c_str(), 1));
}

// TODO: b/335317901 - Cover OGRSQLiteVFS xFileControl and
// xDeviceCharacteristics edge cases.

}  // namespace
}  // namespace autotest2
