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

// Unit tests for HDF-EOS5 Grid and Swath StructMetadata ODL parsing.
// See also:
//   https://gdal.org/drivers/raster/hdf5.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/hdf5/hdf5eosparser.cpp

#include "third_party/gdal/frmts/hdf5/hdf5eosparser.h"

#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/hdf5/hdf5_test_util.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"

namespace autotest2 {
namespace {

TEST(Hdf5EosParserTest, InvalidAndEmptyFiles) {
  // 1. Root group without HDFEOS INFORMATION.
  {
    ScopedVsimemFile mem_file("no_hdfeos.h5");
    ScopedH5File file_id(CreateVsimemHdf5File(mem_file.path()));
    ASSERT_GE(file_id.get(), 0);

    ScopedH5Group other_grp(H5Gcreate(file_id, "OTHER_GROUP", 0));
    ASSERT_GE(other_grp.get(), 0);

    EXPECT_FALSE(HDF5EOSParser::HasHDFEOS(file_id));
    HDF5EOSParser parser;
    ScopedQuietHdf5Errors quiet;
    EXPECT_FALSE(parser.Parse(file_id));
  }

  // 2. Root group where "HDFEOS INFORMATION" is a dataset rather than a group.
  {
    ScopedVsimemFile mem_file("hdfeos_info_is_dset.h5");
    ScopedH5File file_id(CreateVsimemHdf5File(mem_file.path()));
    ASSERT_GE(file_id.get(), 0);

    ScopedH5Dataspace space(H5Screate(H5S_SCALAR));
    ASSERT_GE(space.get(), 0);
    ScopedH5Dataset dset(H5Dcreate(file_id, "HDFEOS INFORMATION",
                                   H5T_NATIVE_INT, space, H5P_DEFAULT));
    ASSERT_GE(dset.get(), 0);

    ScopedQuietHdf5Errors quiet;
    EXPECT_FALSE(HDF5EOSParser::HasHDFEOS(file_id));
  }

  // 3. Group HDFEOS INFORMATION exists, but StructMetadata.0 is missing.
  {
    ScopedVsimemFile mem_file("missing_struct_metadata.h5");
    ScopedH5File file_id(CreateVsimemHdf5File(mem_file.path()));
    ASSERT_GE(file_id.get(), 0);

    ScopedH5Group grp(H5Gcreate(file_id, "HDFEOS INFORMATION", 0));
    ASSERT_GE(grp.get(), 0);

    EXPECT_TRUE(HDF5EOSParser::HasHDFEOS(file_id));
    HDF5EOSParser parser;
    ScopedQuietHdf5Errors quiet;
    EXPECT_FALSE(parser.Parse(file_id));
  }

  // 4. StructMetadata.0 is not a scalar fixed-length string (e.g., 1D integer).
  {
    ScopedVsimemFile mem_file("bad_type_struct_metadata.h5");
    ScopedH5File file_id(CreateVsimemHdf5File(mem_file.path()));
    ASSERT_GE(file_id.get(), 0);

    ScopedH5Group grp(H5Gcreate(file_id, "HDFEOS INFORMATION", 0));
    ASSERT_GE(grp.get(), 0);
    const hsize_t dim = 1;
    ScopedH5Dataspace space(H5Screate_simple(1, &dim, nullptr));
    ASSERT_GE(space.get(), 0);
    ScopedH5Dataset dset(
        H5Dcreate(grp, "StructMetadata.0", H5T_NATIVE_INT, space, H5P_DEFAULT));
    ASSERT_GE(dset.get(), 0);

    EXPECT_TRUE(HDF5EOSParser::HasHDFEOS(file_id));
    HDF5EOSParser parser;
    ScopedQuietHdf5Errors quiet;
    EXPECT_FALSE(parser.Parse(file_id));
  }

  // 5. Malformed ODL in StructMetadata.0.
  {
    ScopedVsimemFile mem_file("malformed_odl.h5");
    ScopedH5File file_id(CreateVsimemHdf5File(mem_file.path()));
    ASSERT_GE(file_id.get(), 0);

    ASSERT_TRUE(WriteStructMetadata(file_id, "GROUP=UnclosedGroup\nFOO=BAR\n"));

    HDF5EOSParser parser;
    ScopedQuietHdf5Errors quiet;
    EXPECT_FALSE(parser.Parse(file_id));
  }

  // 6. Empty GridStructure and SwathStructure.
  {
    ScopedVsimemFile mem_file("empty_structures.h5");
    ScopedH5File file_id(CreateVsimemHdf5File(mem_file.path()));
    ASSERT_GE(file_id.get(), 0);

    ASSERT_TRUE(WriteStructMetadata(file_id,
                                    "GROUP=GridStructure\n"
                                    "END_GROUP=GridStructure\n"
                                    "GROUP=SwathStructure\n"
                                    "END_GROUP=SwathStructure\n"
                                    "END\n"));

    HDF5EOSParser parser;
    EXPECT_FALSE(parser.Parse(file_id));
    EXPECT_EQ(HDF5EOSParser::DataModel::INVALID, parser.GetDataModel());
  }
}

TEST(Hdf5EosParserTest, GridStructure) {
  constexpr std::string_view kGridMetadata = R"(
GROUP=GridStructure
  GROUP=GRID_1
    GridName="UTMGrid"
    XDim=20
    YDim=10
    UpperLeftPointMtrs=(500000.0,4500000.0)
    LowerRightMtrs=(520000.0,4490000.0)
    Projection="HE5_GCTP_UTM"
    ZoneCode=18
    SphereCode=12
    ProjParams=(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)
    GridOrigin="HE5_HDFE_GD_UL"
    GROUP=Dimension
      OBJECT=Dimension_1
        DimensionName="BandDim"
        Size=3
      END_OBJECT=Dimension_1
      OBJECT=Dimension_2
        DimensionName="YDim"
        Size=10
      END_OBJECT=Dimension_2
      OBJECT=Dimension_3
        DimensionName="XDim"
        Size=20
      END_OBJECT=Dimension_3
    END_GROUP=Dimension
    GROUP=DataField
      OBJECT=DataField_1
        DataFieldName="BandData"
        DimList=("BandDim","YDim","XDim")
      END_OBJECT=DataField_1
      OBJECT=DataField_2
        DataFieldName="BadRefField"
        DimList=("MissingDim","XDim")
      END_OBJECT=DataField_2
    END_GROUP=DataField
  END_GROUP=GRID_1
  GROUP=GRID_2
    GridName="GeoInferredGrid"
    XDim=8
    YDim=4
    UpperLeftPointMtrs=(-180000000.0,90000000.0)
    LowerRightMtrs=(180000000.0,-90000000.0)
    Projection="HE5_GCTP_GEO"
    SphereCode=12
    ProjParams=(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)
    GROUP=Dimension
    END_GROUP=Dimension
    GROUP=DataField
      OBJECT=DataField_1
        DataFieldName="SeaIce"
        DimList=("YDim","XDim")
      END_OBJECT=DataField_1
    END_GROUP=DataField
  END_GROUP=GRID_2
  GROUP=GRID_3
    GridName="InvalidGrid"
    Projection="HE5_GCTP_UNKNOWN"
    GridOrigin="HE5_HDFE_GD_LL"
    UpperLeftPointMtrs=(0.0,0.0)
    LowerRightMtrs=(1.0,1.0)
  END_GROUP=GRID_3
  GROUP=GRID_4
    GridName="ZeroDimsProjGrid"
    Projection="HE5_GCTP_UTM"
    ZoneCode=999
    SphereCode=12
    ProjParams=(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)
    UpperLeftPointMtrs=(-3850000.0,5850000.0)
    LowerRightMtrs=(3750000.0,-5350000.0)
  END_GROUP=GRID_4
END_GROUP=GridStructure
END
)";

  // 1. Write StructMetadata.0 and verify GRID data model detection.
  ScopedVsimemFile mem_file("grid.h5");
  ScopedH5File file_id(CreateVsimemHdf5File(mem_file.path()));
  ASSERT_GE(file_id.get(), 0);
  ASSERT_TRUE(WriteStructMetadata(file_id, kGridMetadata));

  ASSERT_TRUE(HDF5EOSParser::HasHDFEOS(file_id));
  HDF5EOSParser parser;
  ASSERT_TRUE(parser.Parse(file_id));
  EXPECT_EQ(HDF5EOSParser::DataModel::GRID, parser.GetDataModel());

  // 2. Verify UTMGrid metadata, GeoTransform, SRS, and 3D DataField lookup.
  HDF5EOSParser::GridMetadata gm;
  EXPECT_FALSE(parser.GetGridMetadata("NonExistentGrid", gm));

  ASSERT_TRUE(parser.GetGridMetadata("UTMGrid", gm));
  EXPECT_EQ("UTMGrid", gm.osGridName);
  EXPECT_EQ("HE5_GCTP_UTM", gm.osProjection);
  EXPECT_EQ(18, gm.nZone);
  ASSERT_EQ(3u, gm.aoDimensions.size());

  GDALGeoTransform gt;
  ASSERT_TRUE(gm.GetGeoTransform(gt));
  EXPECT_DOUBLE_EQ(500000.0, gt[0]);
  EXPECT_DOUBLE_EQ(1000.0, gt[1]);
  EXPECT_DOUBLE_EQ(0.0, gt[2]);
  EXPECT_DOUBLE_EQ(4500000.0, gt[3]);
  EXPECT_DOUBLE_EQ(0.0, gt[4]);
  EXPECT_DOUBLE_EQ(-1000.0, gt[5]);

  auto srs = gm.GetSRS();
  ASSERT_NE(nullptr, srs);
  EXPECT_TRUE(srs->IsProjected());

  HDF5EOSParser::GridDataFieldMetadata dfm;
  EXPECT_FALSE(parser.GetGridDataFieldMetadata(
      "//HDFEOS/GRIDS/UTMGrid/Data_Fields/BadRefField", dfm));
  ASSERT_TRUE(parser.GetGridDataFieldMetadata(
      "//HDFEOS/GRIDS/UTMGrid/Data_Fields/BandData", dfm));
  ASSERT_EQ(3u, dfm.aoDimensions.size());
  EXPECT_EQ("BandDim", dfm.aoDimensions[0].osName);
  EXPECT_EQ(3, dfm.aoDimensions[0].nSize);
  EXPECT_EQ("YDim", dfm.aoDimensions[1].osName);
  EXPECT_EQ(10, dfm.aoDimensions[1].nSize);
  EXPECT_EQ("XDim", dfm.aoDimensions[2].osName);
  EXPECT_EQ(20, dfm.aoDimensions[2].nSize);
  EXPECT_NE(nullptr, dfm.poGridMetadata);

  // 3. Verify geographic grid with packed DMS bounds and inferred XDim/YDim.
  ASSERT_TRUE(parser.GetGridMetadata("GeoInferredGrid", gm));
  ASSERT_TRUE(gm.GetGeoTransform(gt));
  EXPECT_DOUBLE_EQ(-180.0, gt[0]);
  EXPECT_DOUBLE_EQ(45.0, gt[1]);
  EXPECT_DOUBLE_EQ(90.0, gt[3]);
  EXPECT_DOUBLE_EQ(-45.0, gt[5]);
  auto geo_srs = gm.GetSRS();
  ASSERT_NE(nullptr, geo_srs);
  EXPECT_TRUE(geo_srs->IsGeographic());

  // 4. Verify invalid projection and zero-dimension error paths.
  ASSERT_TRUE(parser.GetGridMetadata("InvalidGrid", gm));
  EXPECT_EQ(-1, gm.nProjCode);
  EXPECT_FALSE(gm.GetGeoTransform(gt));
  EXPECT_NE(nullptr, gm.GetSRS());

  ASSERT_TRUE(parser.GetGridMetadata("ZeroDimsProjGrid", gm));
  EXPECT_FALSE(gm.GetGeoTransform(gt));
  EXPECT_EQ(nullptr, gm.GetSRS());
}

TEST(Hdf5EosParserTest, SwathStructure) {
  constexpr std::string_view kSwathMetadata = R"(
GROUP=SwathStructure
  GROUP=SWATH_1
    SwathName="MappedSwath"
    GROUP=Dimension
      OBJECT=Dimension_1
        DimensionName="GeoTrack"
        Size=5
      END_OBJECT=Dimension_1
      OBJECT=Dimension_2
        DimensionName="GeoXtrack"
        Size=4
      END_OBJECT=Dimension_2
      OBJECT=Dimension_3
        DimensionName="DataTrack"
        Size=10
      END_OBJECT=Dimension_3
      OBJECT=Dimension_4
        DimensionName="DataXtrack"
        Size=8
      END_OBJECT=Dimension_4
      OBJECT=Dimension_5
        DimensionName="BandDim"
        Size=2
      END_OBJECT=Dimension_5
    END_GROUP=Dimension
    GROUP=DimensionMap
      OBJECT=DimensionMap_1
        GeoDimension="GeoTrack"
        DataDimension="DataTrack"
        Offset=1
        Increment=2
      END_OBJECT=DimensionMap_1
      OBJECT=DimensionMap_2
        GeoDimension="GeoXtrack"
        DataDimension="DataXtrack"
        Offset=0
        Increment=2
      END_OBJECT=DimensionMap_2
    END_GROUP=DimensionMap
    GROUP=GeoField
      OBJECT=GeoField_1
        GeoFieldName="Latitude"
        DimList=("GeoTrack","GeoXtrack")
      END_OBJECT=GeoField_1
      OBJECT=GeoField_2
        GeoFieldName="Longitude"
        DimList=("GeoTrack","GeoXtrack")
      END_OBJECT=GeoField_2
      OBJECT=GeoField_3
        GeoFieldName="InvalidGeo"
        DimList=("MissingGeoDim","GeoXtrack")
      END_OBJECT=GeoField_3
    END_GROUP=GeoField
    GROUP=DataField
      OBJECT=DataField_1
        DataFieldName="Radiance"
        DimList=("BandDim","DataTrack","DataXtrack")
      END_OBJECT=DataField_1
      OBJECT=DataField_2
        DataFieldName="InvalidData"
        DimList=("MissingDataDim")
      END_OBJECT=DataField_2
    END_GROUP=DataField
  END_GROUP=SWATH_1
  GROUP=SWATH_2
    SwathName="ImplicitMapSwath"
    GROUP=Dimension
      OBJECT=Dimension_1
        DimensionName="Line"
        Size=6
      END_OBJECT=Dimension_1
      OBJECT=Dimension_2
        DimensionName="Sample"
        Size=8
      END_OBJECT=Dimension_2
    END_GROUP=Dimension
    GROUP=GeoField
      OBJECT=GeoField_1
        GeoFieldName="Latitude"
        DimList=("Line","Sample")
      END_OBJECT=GeoField_1
      OBJECT=GeoField_2
        GeoFieldName="Longitude"
        DimList=("Line","Sample")
      END_OBJECT=GeoField_2
    END_GROUP=GeoField
    GROUP=DataField
      OBJECT=DataField_1
        DataFieldName="Reflectance"
        DimList=("Line","Sample")
      END_OBJECT=DataField_1
    END_GROUP=DataField
  END_GROUP=SWATH_2
END_GROUP=SwathStructure
END
)";

  // 1. Write StructMetadata.0 and verify SWATH data model detection.
  ScopedVsimemFile mem_file("swath.h5");
  ScopedH5File file_id(CreateVsimemHdf5File(mem_file.path()));
  ASSERT_GE(file_id.get(), 0);
  ASSERT_TRUE(WriteStructMetadata(file_id, kSwathMetadata));

  HDF5EOSParser parser;
  ASSERT_TRUE(parser.Parse(file_id));
  EXPECT_EQ(HDF5EOSParser::DataModel::SWATH, parser.GetDataModel());

  // 2. Verify explicit DimensionMap offset/increment parsing.
  HDF5EOSParser::SwathMetadata sm;
  EXPECT_FALSE(parser.GetSwathMetadata("MissingSwath", sm));
  ASSERT_TRUE(parser.GetSwathMetadata("MappedSwath", sm));
  EXPECT_EQ("MappedSwath", sm.osSwathName);
  EXPECT_EQ(5u, sm.aoDimensions.size());

  HDF5EOSParser::SwathGeolocationFieldMetadata gfm;
  EXPECT_FALSE(parser.GetSwathGeolocationFieldMetadata(
      "//HDFEOS/SWATHS/MappedSwath/Geolocation_Fields/InvalidGeo", gfm));
  ASSERT_TRUE(parser.GetSwathGeolocationFieldMetadata(
      "//HDFEOS/SWATHS/MappedSwath/Geolocation_Fields/Latitude", gfm));
  EXPECT_EQ(2u, gfm.aoDimensions.size());

  HDF5EOSParser::SwathDataFieldMetadata sdm;
  EXPECT_FALSE(parser.GetSwathDataFieldMetadata(
      "//HDFEOS/SWATHS/MappedSwath/Data_Fields/InvalidData", sdm));
  ASSERT_TRUE(parser.GetSwathDataFieldMetadata(
      "//HDFEOS/SWATHS/MappedSwath/Data_Fields/Radiance", sdm));
  EXPECT_EQ(0, sdm.iOtherDim);
  EXPECT_EQ(1, sdm.iYDim);
  EXPECT_EQ(2, sdm.iXDim);
  EXPECT_EQ(1, sdm.nLineOffset);
  EXPECT_EQ(2, sdm.nLineStep);
  EXPECT_EQ(0, sdm.nPixelOffset);
  EXPECT_EQ(2, sdm.nPixelStep);
  EXPECT_EQ("//HDFEOS/SWATHS/MappedSwath/Geolocation_Fields/Longitude",
            sdm.osLongitudeSubdataset);
  EXPECT_EQ("//HDFEOS/SWATHS/MappedSwath/Geolocation_Fields/Latitude",
            sdm.osLatitudeSubdataset);

  // 3. Verify implicit 1:1 geolocation dimension map.
  ASSERT_TRUE(parser.GetSwathDataFieldMetadata(
      "//HDFEOS/SWATHS/ImplicitMapSwath/Data_Fields/Reflectance", sdm));
  EXPECT_EQ(0, sdm.iYDim);
  EXPECT_EQ(1, sdm.iXDim);
  EXPECT_EQ(0, sdm.nLineOffset);
  EXPECT_EQ(1, sdm.nLineStep);
  EXPECT_EQ(0, sdm.nPixelOffset);
  EXPECT_EQ(1, sdm.nPixelStep);
}

// TODO: b/335317901 - Cover multi-part StructMetadata.1+ concatenation in
// HDF5EOSParser::Parse.

}  // namespace
}  // namespace autotest2
