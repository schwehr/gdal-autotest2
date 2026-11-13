// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package org.gdal.osr;

import static com.google.common.truth.Truth.assertThat;
import static org.junit.Assert.assertThrows;

import java.util.List;
import java.util.Vector;
import org.gdal.GoogleGdalLoader;
import org.gdal.gdal.gdal;
import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public final class OsrModuleAndTransformTest {

  @Rule public final TemporaryFolder tempFolder = new TemporaryFolder();

  @SuppressWarnings("JdkObsolete") // SWIG GetPROJSearchPaths/GetPROJAuxDbPaths return Vector.
  private Vector<?> savedProjSearchPaths;

  @SuppressWarnings("JdkObsolete")
  private Vector<?> savedAuxDbPaths;

  private boolean savedProjNetwork;

  @Before
  public void setUp() {
    GoogleGdalLoader.init();
    osr.UseExceptions();
    savedProjSearchPaths = osr.GetPROJSearchPaths();
    savedAuxDbPaths = osr.GetPROJAuxDbPaths();
    savedProjNetwork = osr.GetPROJEnableNetwork();
  }

  @After
  public void tearDown() {
    osr.UseExceptions();
    if (savedProjSearchPaths != null) {
      osr.SetPROJSearchPaths(savedProjSearchPaths);
    }
    if (savedAuxDbPaths != null) {
      osr.SetPROJAuxDbPaths(savedAuxDbPaths);
    }
    osr.SetPROJEnableNetwork(savedProjNetwork);
    gdal.ErrorReset();
  }

  @Test
  public void exceptionMode_toggleUpdatesGetUseExceptions() {
    assertThat(osr.GetUseExceptions()).isTrue();
    osr.DontUseExceptions();
    assertThat(osr.GetUseExceptions()).isFalse();
    osr.UseExceptions();
    assertThat(osr.GetUseExceptions()).isTrue();
  }

  @Test
  public void moduleHelpersAndConstants_exposeProjAndAuthorityInfo() {
    assertThat(osrJNI.isAvailable()).isAnyOf(true, false);
    assertThat(osrConstants.SRS_WKT_WGS84_LAT_LONG).contains("WGS 84");
    assertThat(osrConstants.SRS_PT_TRANSVERSE_MERCATOR).isEqualTo("Transverse_Mercator");
    assertThat(osrConstants.OAMS_TRADITIONAL_GIS_ORDER).isEqualTo(0);
    assertThat(osrConstants.OAO_North).isEqualTo(1);

    assertThat(osr.GetPROJVersionMajor()).isAtLeast(6);
    assertThat(osr.GetPROJVersionMinor()).isAtLeast(0);
    assertThat(osr.GetPROJVersionMicro()).isAtLeast(0);

    @SuppressWarnings({"unchecked", "JdkObsolete"})
    List<String> authorities = osr.GetAuthorityListFromDatabase();
    assertThat(authorities).contains("EPSG");
  }

  @Test
  public void getWellKnownGeogCSAndUserInputAsWkt_bothOverloadsWorkAndHandleErrors() {
    String wkt0 = osr.GetWellKnownGeogCSAsWKT("WGS84");
    assertThat(wkt0).contains("WGS 84");

    String[] wktOut = new String[1];
    assertThat(osr.GetWellKnownGeogCSAsWKT("NAD83", wktOut)).isEqualTo(0);
    assertThat(wktOut[0]).contains("NAD83");

    String userWkt = osr.GetUserInputAsWKT("EPSG:4326");
    assertThat(userWkt).contains("4326");

    String[] userWktOut = new String[1];
    assertThat(osr.GetUserInputAsWKT("EPSG:3857", userWktOut)).isEqualTo(0);
    assertThat(userWktOut[0]).contains("3857");

    osr.DontUseExceptions();
    assertThat(osr.GetWellKnownGeogCSAsWKT("NO_SUCH_CS", new String[1])).isNotEqualTo(0);
    assertThat(osr.GetUserInputAsWKT("EPSG:99999999", new String[1])).isNotEqualTo(0);

    assertThat(osr.GetWellKnownGeogCSAsWKT("NO_SUCH_CS")).isNull();
    assertThat(osr.GetUserInputAsWKT("EPSG:99999999")).isNull();

    osr.UseExceptions();
    RuntimeException thrownWellKnown =
        assertThrows(
            RuntimeException.class, () -> osr.GetWellKnownGeogCSAsWKT("NO_SUCH_CS", new String[1]));
    assertThat(thrownWellKnown).isNotNull();
    RuntimeException thrownUserInput =
        assertThrows(
            RuntimeException.class, () -> osr.GetUserInputAsWKT("EPSG:99999999", new String[1]));
    assertThat(thrownUserInput).isNotNull();
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void projPathsAndNetwork_roundTrip() throws Exception {
    String customDir = tempFolder.newFolder("proj").getAbsolutePath();
    osr.SetPROJSearchPath(customDir);
    List<String> paths = osr.GetPROJSearchPaths();
    assertThat(paths).containsExactly(customDir);

    Vector<String> customPaths = new Vector<>();
    customPaths.add(customDir);
    osr.SetPROJSearchPaths(customPaths);
    assertThat((List<String>) osr.GetPROJSearchPaths()).containsExactly(customDir);

    osr.SetPROJAuxDbPath(customDir + "/aux1.db");
    assertThat((List<String>) osr.GetPROJAuxDbPaths()).containsExactly(customDir + "/aux1.db");

    Vector<String> auxPaths = new Vector<>();
    auxPaths.add(customDir + "/aux2.db");
    osr.SetPROJAuxDbPaths(auxPaths);
    assertThat((List<String>) osr.GetPROJAuxDbPaths()).containsExactly(customDir + "/aux2.db");

    osr.SetPROJEnableNetwork(false);
    assertThat(osr.GetPROJEnableNetwork()).isFalse();
  }

  @Test
  public void areaOfUse_exposesBoundsAndIdentityMethods() {
    AreaOfUse area = new AreaOfUse(-10.0, 20.0, 30.0, 40.0, "Test Region");
    assertThat(area.getWest_lon_degree()).isEqualTo(-10.0);
    assertThat(area.getSouth_lat_degree()).isEqualTo(20.0);
    assertThat(area.getEast_lon_degree()).isEqualTo(30.0);
    assertThat(area.getNorth_lat_degree()).isEqualTo(40.0);
    assertThat(area.getName()).isEqualTo("Test Region");

    assertThat(osr.OSRAreaOfUse_west_lon_degree_get(area)).isEqualTo(-10.0);
    assertThat(osr.OSRAreaOfUse_south_lat_degree_get(area)).isEqualTo(20.0);
    assertThat(osr.OSRAreaOfUse_east_lon_degree_get(area)).isEqualTo(30.0);
    assertThat(osr.OSRAreaOfUse_north_lat_degree_get(area)).isEqualTo(40.0);
    assertThat(osr.OSRAreaOfUse_name_get(area)).isEqualTo("Test Region");

    assertThat(area.equals(area)).isTrue();
    assertThat(area.equals(new Object())).isFalse();
    assertThat(area.hashCode()).isNotEqualTo(0);
    area.delete();
  }

  @Test
  public void coordinateTransformationOptions_setsAllFieldsAndEquals() {
    CoordinateTransformationOptions opts = new CoordinateTransformationOptions();
    assertThat(opts.SetAreaOfInterest(-125.0, 24.0, -66.0, 50.0)).isTrue();
    assertThat(opts.SetOperation("+proj=noop", false)).isTrue();
    assertThat(opts.SetOperation("+proj=noop")).isTrue();
    assertThat(opts.SetDesiredAccuracy(5.0)).isTrue();
    assertThat(opts.SetBallparkAllowed(true)).isTrue();
    assertThat(opts.SetOnlyBest(false)).isTrue();
    assertThat(opts.equals(opts)).isTrue();
    assertThat(opts.equals(new Object())).isFalse();
    assertThat(opts.hashCode()).isNotEqualTo(0);
    opts.delete();
  }

  @Test
  public void coordinateTransformation_allTransformOverloadsAndInverse() {
    SpatialReference wgs84 = new SpatialReference();
    wgs84.ImportFromEPSG(4326);
    wgs84.SetAxisMappingStrategy(osrConstants.OAMS_TRADITIONAL_GIS_ORDER);

    SpatialReference webMercator = new SpatialReference();
    webMercator.ImportFromEPSG(3857);
    webMercator.SetAxisMappingStrategy(osrConstants.OAMS_TRADITIONAL_GIS_ORDER);

    CoordinateTransformationOptions options = new CoordinateTransformationOptions();
    options.SetAreaOfInterest(-180.0, -85.0, 180.0, 85.0);

    CoordinateTransformation ctDirect = new CoordinateTransformation(wgs84, webMercator);
    CoordinateTransformation ctWithOpts = new CoordinateTransformation(wgs84, webMercator, options);
    CoordinateTransformation ctFactory2 =
        CoordinateTransformation.CreateCoordinateTransformation(wgs84, webMercator);
    CoordinateTransformation ctFactory3 =
        CoordinateTransformation.CreateCoordinateTransformation(wgs84, webMercator, options);
    CoordinateTransformation osrFactory2 = osr.CreateCoordinateTransformation(wgs84, webMercator);
    CoordinateTransformation osrFactory3 =
        osr.CreateCoordinateTransformation(wgs84, webMercator, options);

    assertThat(ctFactory2).isNotNull();
    assertThat(ctFactory3).isNotNull();
    assertThat(osrFactory2).isNotNull();
    assertThat(osrFactory3).isNotNull();

    double[] p2 = ctDirect.TransformPoint(0.0, 0.0);
    assertThat(p2[0]).isWithin(1e-6).of(0.0);
    assertThat(p2[1]).isWithin(1e-6).of(0.0);

    double[] p3 = ctDirect.TransformPoint(1.0, 2.0, 100.0);
    assertThat(p3[0]).isWithin(1.0).of(111319.49);
    assertThat(p3[2]).isWithin(1e-6).of(100.0);

    double[] inout3 = new double[] {1.0, 2.0, 10.0};
    ctDirect.TransformPoint(inout3);
    assertThat(inout3[0]).isWithin(1.0).of(111319.49);

    double[] out2 = new double[3];
    ctDirect.TransformPoint(out2, -122.0, 37.0);
    assertThat(out2[0]).isLessThan(-1.3e7);
    IndexOutOfBoundsException out2Thrown =
        assertThrows(
            IndexOutOfBoundsException.class,
            () -> ctDirect.TransformPoint(new double[2], -122.0, 37.0));
    assertThat(out2Thrown).isNotNull();

    double[] out3 = new double[3];
    ctDirect.TransformPoint(out3, -122.0, 37.0, 50.0);
    assertThat(out3[2]).isWithin(1e-6).of(50.0);

    double[] out4 = new double[4];
    ctWithOpts.TransformPoint(out4, -122.0, 37.0, 50.0, 2020.0);
    assertThat(out4[3]).isWithin(1e-6).of(2020.0);

    double[] withErrOut = new double[4];
    assertThat(ctDirect.TransformPointWithErrorCode(withErrOut, -122.0, 37.0, 0.0, 0.0))
        .isEqualTo(0);

    double[][] pts2d = new double[][] {{0.0, 0.0}, {1.0, 1.0}};
    ctDirect.TransformPoints(pts2d);
    assertThat(pts2d[1][0]).isWithin(1.0).of(111319.49);

    double[][] pts3d = new double[][] {{0.0, 0.0, 5.0}, {-1.0, -1.0, 15.0}};
    int[] errors = ctDirect.TransformPointsWithErrorCodes(pts3d);
    assertThat(errors).isEqualTo(new int[] {0, 0});
    assertThat(pts3d[0][2]).isWithin(1e-6).of(5.0);

    double[] bounds = new double[4];
    ctDirect.TransformBounds(bounds, -1.0, -1.0, 1.0, 1.0, 5);
    assertThat(bounds[0]).isLessThan(0.0);
    assertThat(bounds[2]).isGreaterThan(0.0);

    CoordinateTransformation inv = ctDirect.GetInverse();
    assertThat(inv).isNotNull();
    double[] back = inv.TransformPoint(p3[0], p3[1], p3[2]);
    assertThat(back[0]).isWithin(1e-6).of(1.0);
    assertThat(back[1]).isWithin(1e-6).of(2.0);

    inv.delete();
    osrFactory3.delete();
    osrFactory2.delete();
    ctFactory3.delete();
    ctFactory2.delete();
    ctWithOpts.delete();
    ctDirect.delete();
    options.delete();
    webMercator.delete();
    wgs84.delete();
  }

  @Test
  public void coordinateTransformation_invalidOrErrorInputs_behavePerExceptionMode() {
    SpatialReference empty1 = new SpatialReference();
    SpatialReference empty2 = new SpatialReference();
    SpatialReference wgs84 = new SpatialReference();
    wgs84.ImportFromEPSG(4326);

    osr.UseExceptions();
    RuntimeException thrown2 =
        assertThrows(
            RuntimeException.class,
            () -> CoordinateTransformation.CreateCoordinateTransformation(empty1, empty2));
    assertThat(thrown2).hasMessageThat().isNotEmpty();

    CoordinateTransformationOptions opts = new CoordinateTransformationOptions();
    RuntimeException thrown3 =
        assertThrows(
            RuntimeException.class,
            () -> CoordinateTransformation.CreateCoordinateTransformation(empty1, empty2, opts));
    assertThat(thrown3).hasMessageThat().isNotEmpty();

    osr.DontUseExceptions();
    assertThat(CoordinateTransformation.CreateCoordinateTransformation(empty1, empty2)).isNull();
    assertThat(CoordinateTransformation.CreateCoordinateTransformation(empty1, empty2, opts))
        .isNull();

    osr.UseExceptions();
    CoordinateTransformation ct = new CoordinateTransformation(wgs84, wgs84);
    double[][] invalidPts = new double[][] {{1.0}};
    IllegalArgumentException dimThrown =
        assertThrows(IllegalArgumentException.class, () -> ct.TransformPoints(invalidPts));
    assertThat(dimThrown).hasMessageThat().contains("wrong array dimensions");

    double[][] nullElem = new double[][] {null};
    NullPointerException npeThrown =
        assertThrows(NullPointerException.class, () -> ct.TransformPoints(nullElem));
    assertThat(npeThrown).hasMessageThat().contains("null object");

    assertThrows(
        IllegalArgumentException.class, () -> ct.TransformPointsWithErrorCodes(invalidPts));
    assertThrows(NullPointerException.class, () -> ct.TransformPointsWithErrorCodes(nullElem));
    assertThrows(
        IndexOutOfBoundsException.class,
        () -> ct.TransformPointWithErrorCode(new double[1], 0.0, 0.0, 0.0, 0.0));

    ct.delete();
    opts.delete();
    wgs84.delete();
    empty2.delete();
    empty1.delete();
  }
}
