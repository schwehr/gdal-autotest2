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
// Unit tests for GDALMDReaderALOS (gcore/mdreader/reader_alos.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/mdreader/reader_alos.h
//   https://github.com/OSGeo/gdal/blob/master/gcore/mdreader/reader_alos.cpp

#include "third_party/gdal/gcore/mdreader/reader_alos.h"

#include <format>  // NOLINT
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_mdreader.h"
#include "third_party/gdal/gcore/gdal_multidomainmetadata.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

// Helper deriving from GDALMDReaderBase to invoke the protected virtual
// GetAcquisitionTimeFromString method directly on a GDALMDReaderALOS instance
// via standard C++ pointer-to-protected-base-member dispatch.
struct MdReaderBaseProtectedAccessor : public GDALMDReaderBase {
  static GIntBig CallGetAcquisitionTimeFromString(GDALMDReaderBase& reader,
                                                  const char* date_time) {
    return (reader.*
            (&MdReaderBaseProtectedAccessor::GetAcquisitionTimeFromString))(
        date_time);
  }
};

// Builds a synthetic 1026-byte ALOS RPC text line matching the fixed-width
// field layout in GDALMDReaderALOS::LoadRPCTxtFile:
//   10 scalar fields (6+5+8+9+5+6+5+8+9+5 = 66 bytes) followed by
//   4 coefficient arrays x 20 items x 12 bytes = 960 bytes (total = 1026).
std::string BuildAlosRpcLine() {
  std::string line;
  line.reserve(1026);
  // 10 scalar fields (66 bytes total):
  line += "+01234";     // LINE_OFF (6)
  line += "+0567";      // SAMP_OFF (5)
  line += "+35.1250";   // LAT_OFF (8)
  line += "+139.7500";  // LONG_OFF (9)
  line += "+0150";      // HEIGHT_OFF (5)
  line += "+02000";     // LINE_SCALE (6)
  line += "+0300";      // SAMP_SCALE (5)
  line += "+01.5000";   // LAT_SCALE (8)
  line += "+002.2500";  // LONG_SCALE (9)
  line += "+0500";      // HEIGHT_SCALE (5)

  // 4 arrays (LINE_NUM, LINE_DEN, SAMP_NUM, SAMP_DEN) x 20 coeffs x 12 bytes:
  for (int arr = 0; arr < 4; ++arr) {
    for (int idx = 0; idx < 20; ++idx) {
      // Exact 12-character scientific notation token: "+A.II000E-01"
      line += std::format("+{}.{:02d}000E-01", arr + 1, idx + 1);
    }
  }
  return line;
}

TEST(ReaderAlosTest, SidecarFileDiscoveryAndHasRequiredFiles) {
  absl::Cleanup suite_cleanup = [] {
    VSIRmdirRecursive("/vsimem/reader_alos_test");
  };

  // 1. Short basename (< 3 chars, skips both >= 6 and >= 3 checks) with no
  // sidecar files:
  {
    static constexpr char kDir[] = "/vsimem/reader_alos_test/short_empty";
    absl::Cleanup cleanup = [] { VSIRmdirRecursive(kDir); };
    const std::string tif_path = std::string(kDir) + "/ab.tif";
    VsiMemTempWrapper tif_file(tif_path, "dummy");

    GDALMDReaderALOS reader(tif_path.c_str(), nullptr);
    EXPECT_FALSE(reader.HasRequiredFiles());
    StringListPtr files(reader.GetMetadataFiles(), CSLDestroy);
    EXPECT_EQ(nullptr, files.get());

    GDALMDReaderManager mgr;
    EXPECT_EQ(nullptr, mgr.GetReader(tif_path.c_str(), nullptr, MDR_ALOS));
  }

  // 2. Lowercase summary.txt alone satisfies HasRequiredFiles():
  {
    static constexpr char kDir[] = "/vsimem/reader_alos_test/summary_lower";
    absl::Cleanup cleanup = [] { VSIRmdirRecursive(kDir); };
    const std::string tif_path =
        std::string(kDir) + "/IMG-01-ALPSMW247392950-O1B2R_U.tif";
    const std::string imd_path = std::string(kDir) + "/summary.txt";
    VsiMemTempWrapper tif_file(tif_path, "dummy");
    VsiMemTempWrapper imd_file(imd_path, "Lbi_Satellite=\"ALOS\"\n");

    GDALMDReaderALOS reader(tif_path.c_str(), nullptr);
    EXPECT_TRUE(reader.HasRequiredFiles());
    StringListPtr files(reader.GetMetadataFiles(), CSLDestroy);
    EXPECT_THAT(CslToVector(files.get()), ElementsAre(imd_path));

    GDALMDReaderManager mgr;
    GDALMDReaderBase* managed =
        mgr.GetReader(tif_path.c_str(), nullptr, MDR_ALOS);
    ASSERT_NE(nullptr, managed);
    EXPECT_STREQ(
        "ALOS", CSLFetchNameValue(managed->GetMetadataDomain(MD_DOMAIN_DEFAULT),
                                  MD_NAME_MDTYPE));
  }

  // 3. Uppercase SUMMARY.TXT fallback + 6-char prefix uppercase HDR/RPC (.TXT):
  {
    static constexpr char kDir[] = "/vsimem/reader_alos_test/upper_6char";
    absl::Cleanup cleanup = [] { VSIRmdirRecursive(kDir); };
    const std::string tif_path =
        std::string(kDir) + "/IMG-02-ALPSMW247392950-O1B2R_U.tif";
    const std::string imd_path = std::string(kDir) + "/SUMMARY.TXT";
    const std::string hdr_path =
        std::string(kDir) + "/HDR-ALPSMW247392950-O1B2R_U.TXT";
    const std::string rpc_path =
        std::string(kDir) + "/RPC-ALPSMW247392950-O1B2R_U.TXT";
    VsiMemTempWrapper tif_file(tif_path, "dummy");
    VsiMemTempWrapper imd_file(imd_path, "Lbi_Satellite=\"ALOS\"\n");
    VsiMemTempWrapper hdr_file(hdr_path, "Hdr_Key=1\n");
    VsiMemTempWrapper rpc_file(rpc_path, "rpc");

    GDALMDReaderALOS reader(tif_path.c_str(), nullptr);
    EXPECT_TRUE(reader.HasRequiredFiles());
    StringListPtr files(reader.GetMetadataFiles(), CSLDestroy);
    EXPECT_THAT(CslToVector(files.get()),
                ElementsAre(imd_path, hdr_path, rpc_path));
  }

  // 4. 6-char prefix lowercase HDR/RPC (.txt) without summary.txt:
  //    Verify HDR-only is false, RPC-only is false, and HDR+RPC is true
  //    (taking precedence over 3-char sidecars when both exist).
  {
    static constexpr char kDir[] = "/vsimem/reader_alos_test/lower_6char";
    absl::Cleanup cleanup = [] { VSIRmdirRecursive(kDir); };
    const std::string tif_path = std::string(kDir) + "/IMG-01-SCENE1.tif";
    const std::string hdr_path = std::string(kDir) + "/HDR-SCENE1.txt";
    const std::string rpc_path = std::string(kDir) + "/RPC-SCENE1.txt";
    VsiMemTempWrapper tif_file(tif_path, "dummy");

    {
      VsiMemTempWrapper hdr_only(hdr_path, "Hdr_Key=1\n");
      GDALMDReaderALOS reader_hdr_only(tif_path.c_str(), nullptr);
      EXPECT_FALSE(reader_hdr_only.HasRequiredFiles());
      StringListPtr files(reader_hdr_only.GetMetadataFiles(), CSLDestroy);
      EXPECT_THAT(CslToVector(files.get()), ElementsAre(hdr_path));
    }
    {
      VsiMemTempWrapper rpc_only(rpc_path, "rpc");
      GDALMDReaderALOS reader_rpc_only(tif_path.c_str(), nullptr);
      EXPECT_FALSE(reader_rpc_only.HasRequiredFiles());
      StringListPtr files(reader_rpc_only.GetMetadataFiles(), CSLDestroy);
      EXPECT_THAT(CslToVector(files.get()), ElementsAre(rpc_path));
    }
    {
      VsiMemTempWrapper hdr_file(hdr_path, "Hdr_Key=1\n");
      VsiMemTempWrapper rpc_file(rpc_path, "rpc");
      VsiMemTempWrapper hdr_3char(std::string(kDir) + "/HDR-01-SCENE1.txt",
                                  "Hdr_Key=3char\n");
      VsiMemTempWrapper rpc_3char(std::string(kDir) + "/RPC-01-SCENE1.txt",
                                  "rpc_3char");
      GDALMDReaderALOS reader_both(tif_path.c_str(), nullptr);
      EXPECT_TRUE(reader_both.HasRequiredFiles());
      StringListPtr files(reader_both.GetMetadataFiles(), CSLDestroy);
      EXPECT_THAT(CslToVector(files.get()), ElementsAre(hdr_path, rpc_path));
    }
  }

  // 5. 3-char prefix strip (whole-image naming IMG-ssssssss.tif):
  //    Test both lowercase (.txt) when basename >= 6 and uppercase (.TXT) when
  //    3 <= basename.size() < 6.
  {
    static constexpr char kDir[] = "/vsimem/reader_alos_test/prefix_3char";
    absl::Cleanup cleanup = [] { VSIRmdirRecursive(kDir); };

    const std::string tif_lower = std::string(kDir) + "/IMG-SCENE2.tif";
    const std::string hdr_lower = std::string(kDir) + "/HDR-SCENE2.txt";
    const std::string rpc_lower = std::string(kDir) + "/RPC-SCENE2.txt";
    VsiMemTempWrapper tif1(tif_lower, "dummy");
    VsiMemTempWrapper hdr1(hdr_lower, "Hdr_Key=2\n");
    VsiMemTempWrapper rpc1(rpc_lower, "rpc");

    GDALMDReaderALOS reader_lower(tif_lower.c_str(), nullptr);
    EXPECT_TRUE(reader_lower.HasRequiredFiles());
    StringListPtr files_lower(reader_lower.GetMetadataFiles(), CSLDestroy);
    EXPECT_THAT(CslToVector(files_lower.get()),
                ElementsAre(hdr_lower, rpc_lower));

    // Basename "IMGs" has length 4 (>= 3 and < 6), exercising the uppercase
    // 3-char fallback with < 6 basename length:
    const std::string tif_upper = std::string(kDir) + "/IMGs.tif";
    const std::string hdr_upper = std::string(kDir) + "/HDRs.TXT";
    const std::string rpc_upper = std::string(kDir) + "/RPCs.TXT";
    VsiMemTempWrapper tif2(tif_upper, "dummy");
    VsiMemTempWrapper hdr2(hdr_upper, "Hdr_Key=3\n");
    VsiMemTempWrapper rpc2(rpc_upper, "rpc");

    GDALMDReaderALOS reader_upper(tif_upper.c_str(), nullptr);
    EXPECT_TRUE(reader_upper.HasRequiredFiles());
    StringListPtr files_upper(reader_upper.GetMetadataFiles(), CSLDestroy);
    EXPECT_THAT(CslToVector(files_upper.get()),
                ElementsAre(hdr_upper, rpc_upper));
  }

  // 6. Non-null papszSiblingFiles discovery (case-insensitive match with
  //    in-place filename casing rewrite, and authoritative rejection when
  //    omitted from sibling list):
  {
    static constexpr char kDir[] = "/vsimem/reader_alos_test/sibling_list";
    absl::Cleanup cleanup = [] { VSIRmdirRecursive(kDir); };
    const std::string tif_path = std::string(kDir) + "/IMG-01-SCENE3.tif";
    const std::string imd_path = std::string(kDir) + "/Summary.TXT";
    const std::string hdr_path = std::string(kDir) + "/Hdr-SCENE3.Txt";
    const std::string rpc_path = std::string(kDir) + "/Rpc-SCENE3.Txt";
    VsiMemTempWrapper tif_file(tif_path, "dummy");
    VsiMemTempWrapper imd_file(imd_path, "Lbi_Satellite=\"ALOS\"\n");
    VsiMemTempWrapper hdr_file(hdr_path, "Hdr_Key=1\n");
    VsiMemTempWrapper rpc_file(rpc_path, "rpc");

    CPLStringList siblings;
    siblings.AddString("Summary.TXT");
    siblings.AddString("Hdr-SCENE3.Txt");
    siblings.AddString("Rpc-SCENE3.Txt");
    GDALMDReaderALOS reader_with_siblings(tif_path.c_str(), siblings.List());
    EXPECT_TRUE(reader_with_siblings.HasRequiredFiles());
    StringListPtr files(reader_with_siblings.GetMetadataFiles(), CSLDestroy);
    EXPECT_THAT(CslToVector(files.get()),
                ElementsAre(imd_path, hdr_path, rpc_path));

    CPLStringList empty_siblings;
    empty_siblings.AddString("unrelated.txt");
    GDALMDReaderALOS reader_omitted(tif_path.c_str(), empty_siblings.List());
    EXPECT_FALSE(reader_omitted.HasRequiredFiles());
  }
}

TEST(ReaderAlosTest, LoadMetadataCombinationsAndImageryDomain) {
  absl::Cleanup suite_cleanup = [] {
    VSIRmdirRecursive("/vsimem/reader_alos_test");
  };

  // 1. IMD + HDR merge (HDR overrides IMD on shared keys via CSLMerge),
  //    Lbi_Satellite + Lbi_Sensor, CloudCover < 99 (scaled by 10), and
  //    Img_SceneCenterDateTime taking precedence over Lbi_ObservationDate:
  {
    static constexpr char kDir[] = "/vsimem/reader_alos_test/meta_full";
    absl::Cleanup cleanup = [] { VSIRmdirRecursive(kDir); };
    const std::string tif_path = std::string(kDir) + "/IMG-01-SCENE.tif";
    const std::string imd_path = std::string(kDir) + "/summary.txt";
    const std::string hdr_path = std::string(kDir) + "/HDR-SCENE.txt";
    VsiMemTempWrapper tif_file(tif_path, "dummy");
    VsiMemTempWrapper imd_file(
        imd_path,
        "Lbi_Satellite=\"ALOS\"\n"
        "Lbi_Sensor=\"PRISM\"\n"
        "Shared_Key=\"FromIMD\"\n"
        "Img_CloudQuantityOfAllImage=3\n"
        "Img_SceneCenterDateTime=\"20080715 14:25:36.123\"\n"
        "Lbi_ObservationDate=\"20091020\"\n");
    VsiMemTempWrapper hdr_file(hdr_path,
                               "Shared_Key=\"FromHDR\"\n"
                               "Hdr_Extra=\"FromHDR\"\n");

    GDALMDReaderALOS reader(tif_path.c_str(), nullptr);
    // First call triggers LoadMetadata(); second call exercises the
    // m_bIsMetadataLoad early return without duplicating entries.
    char** default_md = reader.GetMetadataDomain(MD_DOMAIN_DEFAULT);
    ASSERT_NE(nullptr, default_md);
    EXPECT_STREQ("ALOS", CSLFetchNameValue(default_md, MD_NAME_MDTYPE));

    GDALMultiDomainMetadata mdmd;
    EXPECT_TRUE(reader.FillMetadata(&mdmd));
    EXPECT_THAT(CslToVector(reader.GetMetadataDomain(MD_DOMAIN_DEFAULT)),
                ElementsAre("METADATATYPE=ALOS"));
    EXPECT_EQ(3, CSLCount(reader.GetMetadataDomain(MD_DOMAIN_IMAGERY)));
    EXPECT_STREQ("ALOS",
                 mdmd.GetMetadataItem(MD_NAME_MDTYPE, MD_DOMAIN_DEFAULT));
    // Pins upstream behavior: raw IMD domain retains surrounding quotes from
    // CSLLoad, whereas IMAGERY domain strips quotes via CPLStripQuotes.
    EXPECT_STREQ("\"ALOS\"",
                 mdmd.GetMetadataItem("Lbi_Satellite", MD_DOMAIN_IMD));
    EXPECT_STREQ("\"FromHDR\"",
                 mdmd.GetMetadataItem("Shared_Key", MD_DOMAIN_IMD));
    EXPECT_STREQ("\"FromHDR\"",
                 mdmd.GetMetadataItem("Hdr_Extra", MD_DOMAIN_IMD));
    EXPECT_STREQ("ALOS PRISM",
                 mdmd.GetMetadataItem(MD_NAME_SATELLITE, MD_DOMAIN_IMAGERY));
    EXPECT_STREQ("30",
                 mdmd.GetMetadataItem(MD_NAME_CLOUDCOVER, MD_DOMAIN_IMAGERY));
    EXPECT_STREQ("2008-07-15 14:25:36",
                 mdmd.GetMetadataItem(MD_NAME_ACQDATETIME, MD_DOMAIN_IMAGERY));
  }

  // 2. Empty summary.txt + non-empty HDR (nullptr == m_papszIMDMD fallback),
  //    Lbi_Satellite only (no Lbi_Sensor), CloudCover >= 99 ->
  //    MD_CLOUDCOVER_NA, and Lbi_ObservationDate fallback (no
  //    Img_SceneCenterDateTime):
  {
    static constexpr char kDir[] = "/vsimem/reader_alos_test/meta_hdr_only";
    absl::Cleanup cleanup = [] { VSIRmdirRecursive(kDir); };
    const std::string tif_path = std::string(kDir) + "/IMG-01-SCENE.tif";
    const std::string imd_path = std::string(kDir) + "/summary.txt";
    const std::string hdr_path = std::string(kDir) + "/HDR-SCENE.txt";
    VsiMemTempWrapper tif_file(tif_path, "dummy");
    VsiMemTempWrapper imd_file(imd_path, "");
    VsiMemTempWrapper hdr_file(hdr_path,
                               "Lbi_Satellite=\"ALOS-1\"\n"
                               "Img_CloudQuantityOfAllImage=99\n"
                               "Lbi_ObservationDate=\"20091020\"\n");

    GDALMDReaderALOS reader(tif_path.c_str(), nullptr);
    char** imagery_md = reader.GetMetadataDomain(MD_DOMAIN_IMAGERY);
    ASSERT_NE(nullptr, imagery_md);
    EXPECT_STREQ("ALOS-1", CSLFetchNameValue(imagery_md, MD_NAME_SATELLITE));
    EXPECT_STREQ(MD_CLOUDCOVER_NA,
                 CSLFetchNameValue(imagery_md, MD_NAME_CLOUDCOVER));
    EXPECT_STREQ("2009-10-20 00:00:00",
                 CSLFetchNameValue(imagery_md, MD_NAME_ACQDATETIME));
  }

  // 3. Lbi_Sensor only (no Lbi_Satellite), CloudCover == 98 ("980" boundary),
  //    and malformed Img_SceneCenterDateTime + direct
  //    GetAcquisitionTimeFromString(nullptr):
  {
    static constexpr char kDir[] = "/vsimem/reader_alos_test/meta_sensor_only";
    absl::Cleanup cleanup = [] { VSIRmdirRecursive(kDir); };
    const std::string tif_path = std::string(kDir) + "/IMG-01-SCENE.tif";
    const std::string imd_path = std::string(kDir) + "/summary.txt";
    VsiMemTempWrapper tif_file(tif_path, "dummy");
    VsiMemTempWrapper imd_file(imd_path,
                               "Lbi_Sensor=\"AVNIR-2\"\n"
                               "Img_CloudQuantityOfAllImage=98\n"
                               "Img_SceneCenterDateTime=\"invalid-date\"\n");

    GDALMDReaderALOS reader(tif_path.c_str(), nullptr);
    char** imagery_md = reader.GetMetadataDomain(MD_DOMAIN_IMAGERY);
    ASSERT_NE(nullptr, imagery_md);
    EXPECT_STREQ("AVNIR-2", CSLFetchNameValue(imagery_md, MD_NAME_SATELLITE));
    EXPECT_STREQ("980", CSLFetchNameValue(imagery_md, MD_NAME_CLOUDCOVER));
    EXPECT_STREQ("1970-01-01 00:00:00",
                 CSLFetchNameValue(imagery_md, MD_NAME_ACQDATETIME));

    // Verify protected GetAcquisitionTimeFromString handles nullptr, invalid,
    // and valid date strings directly:
    EXPECT_EQ(0,
              MdReaderBaseProtectedAccessor::CallGetAcquisitionTimeFromString(
                  reader, nullptr));
    EXPECT_EQ(0,
              MdReaderBaseProtectedAccessor::CallGetAcquisitionTimeFromString(
                  reader, "20080715"));
    EXPECT_EQ(1216131936,
              MdReaderBaseProtectedAccessor::CallGetAcquisitionTimeFromString(
                  reader, "20080715 14:25:36.000"));
  }

  // 4. Empty metadata fields (neither satellite/sensor, nor cloud, nor dates),
  //    plus quoted Img_CloudQuantityOfAllImage upstream quirk:
  {
    static constexpr char kDir[] = "/vsimem/reader_alos_test/meta_minimal";
    absl::Cleanup cleanup = [] { VSIRmdirRecursive(kDir); };
    const std::string tif_path = std::string(kDir) + "/IMG-01-SCENE.tif";
    const std::string imd_path = std::string(kDir) + "/summary.txt";
    VsiMemTempWrapper tif_file(tif_path, "dummy");
    {
      VsiMemTempWrapper imd_file(imd_path, "Unrelated_Key=\"Val\"\n");
      GDALMDReaderALOS reader(tif_path.c_str(), nullptr);
      EXPECT_EQ(nullptr, reader.GetMetadataDomain(MD_DOMAIN_IMAGERY));
      EXPECT_EQ(nullptr, reader.GetMetadataDomain(MD_DOMAIN_RPC));
    }
    {
      // Pins upstream quirk: LoadMetadata() calls atoi(pszCloudCover) without
      // CPLStripQuotes, so a quoted value "\"3\"" parses as 0 ("0").
      VsiMemTempWrapper imd_quoted(imd_path,
                                   "Img_CloudQuantityOfAllImage=\"3\"\n");
      GDALMDReaderALOS reader_quoted(tif_path.c_str(), nullptr);
      EXPECT_STREQ("0", CSLFetchNameValue(
                            reader_quoted.GetMetadataDomain(MD_DOMAIN_IMAGERY),
                            MD_NAME_CLOUDCOVER));
    }
  }
}

TEST(ReaderAlosTest, LoadRPCTxtFileValidAndErrorPaths) {
  absl::Cleanup suite_cleanup = [] {
    VSIRmdirRecursive("/vsimem/reader_alos_test");
  };

  // 1. Empty RPC file returns nullptr without emitting an error:
  {
    static constexpr char kDir[] = "/vsimem/reader_alos_test/rpc_empty";
    absl::Cleanup cleanup = [] { VSIRmdirRecursive(kDir); };
    const std::string tif_path = std::string(kDir) + "/IMG-01-SCENE.tif";
    const std::string hdr_path = std::string(kDir) + "/HDR-SCENE.txt";
    const std::string rpc_path = std::string(kDir) + "/RPC-SCENE.txt";
    VsiMemTempWrapper tif_file(tif_path, "dummy");
    VsiMemTempWrapper hdr_file(hdr_path, "Lbi_Satellite=\"ALOS\"\n");
    VsiMemTempWrapper rpc_file(rpc_path, "");

    CPLErrorReset();
    GDALMDReaderALOS reader(tif_path.c_str(), nullptr);
    EXPECT_EQ(nullptr, reader.GetMetadataDomain(MD_DOMAIN_RPC));
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
  }

  // 2. Truncated RPC line (< 1026 bytes) reports CE_Failure / CPLE_AppDefined:
  {
    static constexpr char kDir[] = "/vsimem/reader_alos_test/rpc_truncated";
    absl::Cleanup cleanup = [] { VSIRmdirRecursive(kDir); };
    const std::string tif_path = std::string(kDir) + "/IMG-01-SCENE.tif";
    const std::string hdr_path = std::string(kDir) + "/HDR-SCENE.txt";
    const std::string rpc_path = std::string(kDir) + "/RPC-SCENE.txt";
    VsiMemTempWrapper tif_file(tif_path, "dummy");
    VsiMemTempWrapper hdr_file(hdr_path, "Lbi_Satellite=\"ALOS\"\n");
    // 1025 bytes is 1 byte short of the required 1026 bytes:
    VsiMemTempWrapper rpc_file(rpc_path, std::string(1025, '1'));

    GDALMDReaderALOS reader(tif_path.c_str(), nullptr);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, reader.GetMetadataDomain(MD_DOMAIN_RPC));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("has only 1025 bytes wherea 1026 are required"));
    CPLErrorReset();
  }

  // 3. Valid >= 1026-byte RPC file (with trailing whitespace and newline)
  //    parses all 10 scalar fields and 4x20 coeffs and round-trips through
  //    GDALExtractRPCInfoV2:
  {
    static constexpr char kDir[] = "/vsimem/reader_alos_test/rpc_valid";
    absl::Cleanup cleanup = [] { VSIRmdirRecursive(kDir); };
    const std::string tif_path = std::string(kDir) + "/IMG-01-SCENE.tif";
    const std::string hdr_path = std::string(kDir) + "/HDR-SCENE.txt";
    const std::string rpc_path = std::string(kDir) + "/RPC-SCENE.txt";
    const std::string rpc_line = BuildAlosRpcLine();
    ASSERT_EQ(1026u, rpc_line.size());

    VsiMemTempWrapper tif_file(tif_path, "dummy");
    VsiMemTempWrapper hdr_file(hdr_path, "Lbi_Satellite=\"ALOS\"\n");
    VsiMemTempWrapper rpc_file(rpc_path, rpc_line + "   \nextra_line\n");

    CPLErrorReset();
    GDALMDReaderALOS reader(tif_path.c_str(), nullptr);
    char** rpc_md = reader.GetMetadataDomain(MD_DOMAIN_RPC);
    ASSERT_NE(nullptr, rpc_md);
    EXPECT_EQ(CE_None, CPLGetLastErrorType());

    EXPECT_STREQ("+01234", CSLFetchNameValue(rpc_md, RPC_LINE_OFF));
    EXPECT_STREQ("+0567", CSLFetchNameValue(rpc_md, RPC_SAMP_OFF));
    EXPECT_STREQ("+35.1250", CSLFetchNameValue(rpc_md, RPC_LAT_OFF));
    EXPECT_STREQ("+139.7500", CSLFetchNameValue(rpc_md, RPC_LONG_OFF));
    EXPECT_STREQ("+0150", CSLFetchNameValue(rpc_md, RPC_HEIGHT_OFF));
    EXPECT_STREQ("+02000", CSLFetchNameValue(rpc_md, RPC_LINE_SCALE));
    EXPECT_STREQ("+0300", CSLFetchNameValue(rpc_md, RPC_SAMP_SCALE));
    EXPECT_STREQ("+01.5000", CSLFetchNameValue(rpc_md, RPC_LAT_SCALE));
    EXPECT_STREQ("+002.2500", CSLFetchNameValue(rpc_md, RPC_LONG_SCALE));
    EXPECT_STREQ("+0500", CSLFetchNameValue(rpc_md, RPC_HEIGHT_SCALE));

    GDALRPCInfoV2 rpc_info{};
    ASSERT_EQ(TRUE, GDALExtractRPCInfoV2(rpc_md, &rpc_info));
    EXPECT_DOUBLE_EQ(1234.0, rpc_info.dfLINE_OFF);
    EXPECT_DOUBLE_EQ(567.0, rpc_info.dfSAMP_OFF);
    EXPECT_DOUBLE_EQ(35.125, rpc_info.dfLAT_OFF);
    EXPECT_DOUBLE_EQ(139.75, rpc_info.dfLONG_OFF);
    EXPECT_DOUBLE_EQ(150.0, rpc_info.dfHEIGHT_OFF);
    EXPECT_DOUBLE_EQ(2000.0, rpc_info.dfLINE_SCALE);
    EXPECT_DOUBLE_EQ(300.0, rpc_info.dfSAMP_SCALE);
    EXPECT_DOUBLE_EQ(1.5, rpc_info.dfLAT_SCALE);
    EXPECT_DOUBLE_EQ(2.25, rpc_info.dfLONG_SCALE);
    EXPECT_DOUBLE_EQ(500.0, rpc_info.dfHEIGHT_SCALE);
    for (int idx = 0; idx < 20; ++idx) {
      EXPECT_NEAR((1.0 + (idx + 1) * 0.01) * 0.1,
                  rpc_info.adfLINE_NUM_COEFF[idx], 1e-12);
      EXPECT_NEAR((2.0 + (idx + 1) * 0.01) * 0.1,
                  rpc_info.adfLINE_DEN_COEFF[idx], 1e-12);
      EXPECT_NEAR((3.0 + (idx + 1) * 0.01) * 0.1,
                  rpc_info.adfSAMP_NUM_COEFF[idx], 1e-12);
      EXPECT_NEAR((4.0 + (idx + 1) * 0.01) * 0.1,
                  rpc_info.adfSAMP_DEN_COEFF[idx], 1e-12);
    }
  }
}

// TODO: b/335317901 - Add dedicated unit tests for the remaining 10 satellite
// metadata readers in gcore/mdreader/ (reader_digital_globe.cpp,
// reader_eros.cpp, reader_geo_eye.cpp, reader_kompsat.cpp, reader_landsat.cpp,
// reader_orb_view.cpp, reader_pleiades.cpp, reader_rapid_eye.cpp,
// reader_rdk1.cpp, and reader_spot.cpp).

}  // namespace
}  // namespace autotest2
