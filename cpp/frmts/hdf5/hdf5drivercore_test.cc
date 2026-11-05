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

// Unit tests for HDF5 driver identification, subdataset info parsing, and
// common driver metadata registration.
// See also:
//   https://gdal.org/drivers/raster/hdf5.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/hdf5/hdf5drivercore.cpp

#include "third_party/gdal/frmts/hdf5/hdf5drivercore.h"

#include <cstddef>
#include <cstring>
#include <format>  // NOLINT
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/frmts/hdf5/hdf5_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdalsubdatasetinfo.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace autotest2 {
namespace {

class DummyDataset final : public GDALDataset {};

GDALDataset* FakeNetCdfOpen(GDALOpenInfo* info) {
  if (std::string_view(info->pszFilename).find("nc_recognized") !=
      std::string_view::npos) {
    return new DummyDataset();
  }
  return nullptr;
}

constexpr std::string_view kHdf5Signature("\211HDF\r\n\032\n", 8);

std::string MakeSuperblockPayload(size_t offset, size_t total_size = 256) {
  std::string buf(total_size, '\0');
  std::memcpy(buf.data() + offset, kHdf5Signature.data(),
              kHdf5Signature.size());
  return buf;
}

TEST(Hdf5DriverCoreTest, Hdf5DatasetIdentify) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;

  // 1. Subdataset prefix and missing-file checks.
  {
    GDALOpenInfo multdim_info("HDF5:/vsimem/dummy.h5://sub",
                              GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, HDF5DatasetIdentify(&multdim_info));

    GDALOpenInfo raster_sub_info("HDF5:/vsimem/dummy.h5://sub",
                                 GDAL_OF_RASTER | GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, HDF5DatasetIdentify(&raster_sub_info));

    GDALOpenInfo missing_info("/vsimem/does_not_exist_drivercore.h5",
                              GDAL_OF_RASTER | GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, HDF5DatasetIdentify(&missing_info));
  }

  // 2. Standard HDF5 signature at offset 0.
  ScopedVsimemFile sig0("sig0.h5");
  {
    VsiMemTempWrapper tmp(sig0.str(), MakeSuperblockPayload(0));

    GDALOpenInfo info(sig0.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, HDF5DatasetIdentify(&info));

    const char* allowed[] = {"HDF5", nullptr};
    GDALOpenInfo single_info(sig0.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
    single_info.papszAllowedDrivers = allowed;
    EXPECT_EQ(TRUE, HDF5DatasetIdentify(&single_info));
  }

  // 3. .bag extension: accepted when BAG driver is absent, deferred when
  // registered.
  ScopedVsimemFile bag_file("test_ident.bag");
  {
    VsiMemTempWrapper tmp(bag_file.str(), MakeSuperblockPayload(0));
    {
      GDALOpenInfo info(bag_file.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
      EXPECT_EQ(TRUE, HDF5DatasetIdentify(&info));
    }
    {
      auto bag_drv = std::make_unique<GDALDriver>();
      BAGDriverSetCommonMetadata(bag_drv.get());
      GDALRegisterDriver(bag_drv.get());
      auto deregister =
          absl::MakeCleanup([&] { GDALDeregisterDriver(bag_drv.get()); });
      GDALOpenInfo info(bag_file.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
      EXPECT_EQ(FALSE, HDF5DatasetIdentify(&info));
    }
  }

  // 4. .kea extension: accepted when KEA driver is absent, deferred when
  // registered.
  ScopedVsimemFile kea_file("test_ident.kea");
  {
    VsiMemTempWrapper tmp(kea_file.str(), MakeSuperblockPayload(0));
    {
      GDALOpenInfo info(kea_file.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
      EXPECT_EQ(TRUE, HDF5DatasetIdentify(&info));
    }
    {
      auto kea_drv = std::make_unique<GDALDriver>();
      kea_drv->SetDescription("KEA");
      GDALRegisterDriver(kea_drv.get());
      auto deregister =
          absl::MakeCleanup([&] { GDALDeregisterDriver(kea_drv.get()); });
      GDALOpenInfo info(kea_file.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
      EXPECT_EQ(FALSE, HDF5DatasetIdentify(&info));
    }
  }

  // 5. .nc / .nc4 extensions: deferred when netCDF recognizes the file,
  // retained by HDF5 when netCDF rejects it.
  ScopedVsimemFile real_nc("nc_recognized.nc");
  {
    VsiMemTempWrapper tmp(real_nc.str(), MakeSuperblockPayload(0));
    {
      GDALOpenInfo info(real_nc.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
      EXPECT_EQ(TRUE, HDF5DatasetIdentify(&info));
    }
    {
      auto nc_drv = std::make_unique<GDALDriver>();
      nc_drv->SetDescription("netCDF");
      nc_drv->SetMetadataItem(GDAL_DCAP_OPEN, "YES");
      nc_drv->pfnOpen = FakeNetCdfOpen;
      GDALRegisterDriver(nc_drv.get());
      auto deregister =
          absl::MakeCleanup([&] { GDALDeregisterDriver(nc_drv.get()); });
      GDALOpenInfo info(real_nc.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
      EXPECT_EQ(FALSE, HDF5DatasetIdentify(&info));
    }
  }

  ScopedVsimemFile dummy_nc("nc_unrecognized.nc4");
  {
    VsiMemTempWrapper tmp(dummy_nc.str(), MakeSuperblockPayload(0));

    auto nc_drv = std::make_unique<GDALDriver>();
    nc_drv->SetDescription("netCDF");
    nc_drv->SetMetadataItem(GDAL_DCAP_OPEN, "YES");
    nc_drv->pfnOpen = FakeNetCdfOpen;
    GDALRegisterDriver(nc_drv.get());
    auto deregister =
        absl::MakeCleanup([&] { GDALDeregisterDriver(nc_drv.get()); });
    GDALOpenInfo info(dummy_nc.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, HDF5DatasetIdentify(&info));
  }

  // 6. Signature at offset 512 and <HDF_UserBlock> header.
  ScopedVsimemFile sig512("sig512.h5");
  {
    VsiMemTempWrapper tmp(sig512.str(), MakeSuperblockPayload(512, 600));
    GDALOpenInfo info(sig512.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, HDF5DatasetIdentify(&info));
  }

  ScopedVsimemFile userblock("userblock.h5");
  {
    VsiMemTempWrapper tmp(userblock.str(), "<HDF_UserBlock>header_data");
    GDALOpenInfo info(userblock.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, HDF5DatasetIdentify(&info));
  }

  // 7. Signature at offset 2048 (.h5 vs .dat vs netCDF-recognized .nc) and
  // non-HDF5 .h5 EOF fallback.
  ScopedVsimemFile sig2048_h5("sig2048.h5");
  {
    VsiMemTempWrapper tmp(sig2048_h5.str(), MakeSuperblockPayload(2048, 2100));
    GDALOpenInfo info(sig2048_h5.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, HDF5DatasetIdentify(&info));
  }

  ScopedVsimemFile sig2048_dat("sig2048.dat");
  {
    VsiMemTempWrapper tmp(sig2048_dat.str(), MakeSuperblockPayload(2048, 2100));

    GDALOpenInfo plain_info(sig2048_dat.path(),
                            GDAL_OF_RASTER | GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, HDF5DatasetIdentify(&plain_info));

    const char* allowed[] = {"HDF5", nullptr};
    GDALOpenInfo allowed_info(sig2048_dat.path(),
                              GDAL_OF_RASTER | GDAL_OF_READONLY);
    allowed_info.papszAllowedDrivers = allowed;
    EXPECT_EQ(TRUE, HDF5DatasetIdentify(&allowed_info));
  }

  ScopedVsimemFile sig2048_nc("nc_recognized_2048.nc");
  {
    VsiMemTempWrapper tmp(sig2048_nc.str(), MakeSuperblockPayload(2048, 2100));

    auto nc_drv = std::make_unique<GDALDriver>();
    nc_drv->SetDescription("netCDF");
    nc_drv->SetMetadataItem(GDAL_DCAP_OPEN, "YES");
    nc_drv->pfnOpen = FakeNetCdfOpen;
    GDALRegisterDriver(nc_drv.get());
    auto deregister =
        absl::MakeCleanup([&] { GDALDeregisterDriver(nc_drv.get()); });
    GDALOpenInfo info(sig2048_nc.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, HDF5DatasetIdentify(&info));
  }

  ScopedVsimemFile non_hdf5("not_hdf5.h5");
  {
    VsiMemTempWrapper tmp(non_hdf5.str(), std::string(2200, 'Z'));
    GDALOpenInfo info(non_hdf5.path(), GDAL_OF_RASTER | GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, HDF5DatasetIdentify(&info));
  }
}

TEST(Hdf5DriverCoreTest, IdentifyS102S104S111AndBag) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;

  // 1. Subdataset prefix matches and missing-file rejections.
  {
    GDALOpenInfo s102_prefix("S102:/vsimem/foo.h5", GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, S102DatasetIdentify(&s102_prefix));

    GDALOpenInfo s104_prefix("S104:/vsimem/foo.h5", GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, S104DatasetIdentify(&s104_prefix));

    GDALOpenInfo s111_prefix("S111:/vsimem/foo.h5", GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, S111DatasetIdentify(&s111_prefix));

    GDALOpenInfo bag_prefix("BAG:/vsimem/foo.bag", GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, BAGDatasetIdentify(&bag_prefix));

    GDALOpenInfo missing("/vsimem/missing_sxx.h5", GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, S102DatasetIdentify(&missing));
    EXPECT_EQ(FALSE, S104DatasetIdentify(&missing));
    EXPECT_EQ(FALSE, S111DatasetIdentify(&missing));
    EXPECT_EQ(FALSE, BAGDatasetIdentify(&missing));
  }

  // 2. Plain HDF5 file: rejected by default, accepted when driver is forced.
  ScopedVsimemFile plain_hdf5("plain_hdf5.h5");
  {
    VsiMemTempWrapper tmp(plain_hdf5.str(), MakeSuperblockPayload(0, 512));

    GDALOpenInfo info(plain_hdf5.path(), GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, S102DatasetIdentify(&info));
    EXPECT_EQ(FALSE, S104DatasetIdentify(&info));
    EXPECT_EQ(FALSE, S111DatasetIdentify(&info));
    EXPECT_EQ(FALSE, BAGDatasetIdentify(&info));

    const char* s102_allowed[] = {"S102", nullptr};
    GDALOpenInfo s102_single(plain_hdf5.path(), GDAL_OF_READONLY);
    s102_single.papszAllowedDrivers = s102_allowed;
    EXPECT_EQ(TRUE, S102DatasetIdentify(&s102_single));

    const char* bag_allowed[] = {"BAG", nullptr};
    GDALOpenInfo bag_single(plain_hdf5.path(), GDAL_OF_READONLY);
    bag_single.papszAllowedDrivers = bag_allowed;
    EXPECT_EQ(TRUE, BAGDatasetIdentify(&bag_single));
  }

  // 3. S102 header with BathymetryCoverage before Group_F + GDAL_S102_IDENTIFY.
  ScopedVsimemFile s102_file("s102_sample.h5");
  {
    std::string bytes = MakeSuperblockPayload(0, 1024);
    constexpr std::string_view kMainGroup = "BathymetryCoverage";
    constexpr std::string_view kGroupF = "Group_F";
    std::memcpy(bytes.data() + 64, kMainGroup.data(), kMainGroup.size());
    std::memcpy(bytes.data() + 256, kGroupF.data(), kGroupF.size());
    VsiMemTempWrapper tmp(s102_file.str(), bytes);

    {
      GDALOpenInfo info(s102_file.path(), GDAL_OF_READONLY);
      EXPECT_EQ(TRUE, S102DatasetIdentify(&info));
    }

    CPLConfigOptionSetter disable_s102("GDAL_S102_IDENTIFY", "NO", false);
    GDALOpenInfo info_disabled(s102_file.path(), GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, S102DatasetIdentify(&info_disabled));
  }

  // 4. S104 header with Group_F before WaterLevel.
  ScopedVsimemFile s104_file("s104_sample.h5");
  {
    std::string bytes = MakeSuperblockPayload(0, 1024);
    constexpr std::string_view kGroupF = "Group_F";
    constexpr std::string_view kMainGroup = "WaterLevel";
    std::memcpy(bytes.data() + 64, kGroupF.data(), kGroupF.size());
    std::memcpy(bytes.data() + 256, kMainGroup.data(), kMainGroup.size());
    VsiMemTempWrapper tmp(s104_file.str(), bytes);

    GDALOpenInfo info(s104_file.path(), GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, S104DatasetIdentify(&info));
  }

  // 5. S111 header >= 4096 bytes triggering TryToIngest(4096).
  ScopedVsimemFile s111_file("s111_large_hdr.h5");
  {
    std::string bytes = MakeSuperblockPayload(0, 4096);
    constexpr std::string_view kProdSpec = "productSpecification";
    constexpr std::string_view kGroupF = "Group_F";
    constexpr std::string_view kMainGroup = "SurfaceCurrent";
    std::memcpy(bytes.data() + 512, kProdSpec.data(), kProdSpec.size());
    std::memcpy(bytes.data() + 2048, kGroupF.data(), kGroupF.size());
    std::memcpy(bytes.data() + 2200, kMainGroup.data(), kMainGroup.size());
    VsiMemTempWrapper tmp(s111_file.str(), bytes);

    GDALOpenInfo info(s111_file.path(), GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, S111DatasetIdentify(&info));
  }
}

TEST(Hdf5DriverCoreTest, SubdatasetInfoAndCommonMetadata) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;
  auto hdf5_drv = std::make_unique<GDALDriver>();
  HDF5DriverSetCommonMetadata(hdf5_drv.get());
  EXPECT_STREQ(HDF5_DRIVER_NAME, hdf5_drv->GetDescription());
  EXPECT_STREQ("YES", hdf5_drv->GetMetadataItem(GDAL_DCAP_MULTIDIM_RASTER));
  EXPECT_STREQ("YES", hdf5_drv->GetMetadataItem(GDAL_DMD_SUBDATASETS));
  ASSERT_NE(nullptr, hdf5_drv->pfnGetSubdatasetInfoFunc);

  // 1. Invalid HDF5 subdataset strings return nullptr.
  constexpr const char* const kInvalidSpecs[] = {
      "/vsimem/no_prefix.h5",
      "HDF5:",
      "HDF5:/vsimem/only_path.h5",
      "HDF5::/only_sub",
  };
  for (const char* spec : kInvalidSpecs) {
    std::unique_ptr<GDALSubdatasetInfo> info(
        hdf5_drv->pfnGetSubdatasetInfoFunc(spec));
    EXPECT_EQ(nullptr, info) << spec;
  }

  // 2. Valid HDF5 subdataset strings (local, quoted, Windows paths).
  struct ValidSubdatasetCase {
    const char* spec;
    std::string_view expected_path;
    std::string_view expected_sub;
  };
  constexpr ValidSubdatasetCase kValidCases[] = {
      {.spec = "HDF5:/vsimem/test.h5://group/dataset",
       .expected_path = "/vsimem/test.h5",
       .expected_sub = "//group/dataset"},
      {.spec = "hdf5:\"/vsimem/test.h5\"://sub:part1:part2",
       .expected_path = "/vsimem/test.h5",
       .expected_sub = "//sub:part1:part2"},
      {.spec = "HDF5:C:\\data\\file.h5://sub",
       .expected_path = "C:\\data\\file.h5",
       .expected_sub = "//sub"},
      {.spec = "HDF5:\"D:/data/file.h5\"://sub",
       .expected_path = "D:/data/file.h5",
       .expected_sub = "//sub"},
  };
  for (const ValidSubdatasetCase& tc : kValidCases) {
    std::unique_ptr<GDALSubdatasetInfo> info(
        hdf5_drv->pfnGetSubdatasetInfoFunc(tc.spec));
    ASSERT_NE(nullptr, info) << tc.spec;
    EXPECT_EQ(tc.expected_path, info->GetPathComponent());
    EXPECT_EQ(tc.expected_sub, info->GetSubdatasetComponent());
  }

  {
    std::unique_ptr<GDALSubdatasetInfo> info(hdf5_drv->pfnGetSubdatasetInfoFunc(
        "HDF5:/vsimem/test.h5://group/dataset"));
    ASSERT_NE(nullptr, info);
    EXPECT_EQ("HDF5:/vsimem/renamed.h5://group/dataset",
              info->ModifyPathComponent("/vsimem/renamed.h5"));
  }

  // 3. Virtual curl / streaming protocol prefixes.
  constexpr std::string_view kCurlPrefixes[] = {
      "/vsicurl/http",
      "/vsicurl/https",
      "/vsicurl_streaming/http",
      "/vsicurl_streaming/https",
  };
  for (std::string_view prefix : kCurlPrefixes) {
    const std::string full =
        std::format("HDF5:{}://example.com/a.h5://band", prefix);
    std::unique_ptr<GDALSubdatasetInfo> info(
        hdf5_drv->pfnGetSubdatasetInfoFunc(full.c_str()));
    ASSERT_NE(nullptr, info) << full;
    EXPECT_EQ(std::format("{}://example.com/a.h5", prefix),
              info->GetPathComponent());
    EXPECT_EQ("//band", info->GetSubdatasetComponent());
  }

  // 4. Remaining SetCommonMetadata functions and HDF5ImageDatasetIdentify.
  auto hdf5_img_drv = std::make_unique<GDALDriver>();
  HDF5ImageDriverSetCommonMetadata(hdf5_img_drv.get());
  EXPECT_STREQ(HDF5_IMAGE_DRIVER_NAME, hdf5_img_drv->GetDescription());
  EXPECT_STREQ("YES", hdf5_img_drv->GetMetadataItem(GDAL_DCAP_VIRTUALIO));
  {
    GDALOpenInfo match_info("hdf5:/vsimem/a.h5://sub", GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, HDF5ImageDatasetIdentify(&match_info));
    GDALOpenInfo no_match_info("/vsimem/a.h5", GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, HDF5ImageDatasetIdentify(&no_match_info));
  }

  auto bag_drv = std::make_unique<GDALDriver>();
  BAGDriverSetCommonMetadata(bag_drv.get());
  EXPECT_STREQ(BAG_DRIVER_NAME, bag_drv->GetDescription());
  EXPECT_STREQ("YES", bag_drv->GetMetadataItem(GDAL_DCAP_VECTOR));
  EXPECT_STREQ("YES", bag_drv->GetMetadataItem(GDAL_DCAP_CREATE));

  auto s102_drv = std::make_unique<GDALDriver>();
  S102DriverSetCommonMetadata(s102_drv.get());
  EXPECT_STREQ(S102_DRIVER_NAME, s102_drv->GetDescription());
  EXPECT_STREQ("YES", s102_drv->GetMetadataItem(GDAL_DMD_SUBDATASETS));

  auto s104_drv = std::make_unique<GDALDriver>();
  S104DriverSetCommonMetadata(s104_drv.get());
  EXPECT_STREQ(S104_DRIVER_NAME, s104_drv->GetDescription());

  auto s111_drv = std::make_unique<GDALDriver>();
  S111DriverSetCommonMetadata(s111_drv.get());
  EXPECT_STREQ(S111_DRIVER_NAME, s111_drv->GetDescription());
}

// TODO: b/335317901 - Cover Windows drive-letter backslash unquoting edge
// cases in HDF5DriverGetSubdatasetInfo.

}  // namespace
}  // namespace autotest2
