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

import java.util.Vector;
import org.gdal.GoogleGdalLoader;
import org.gdal.gdal.gdal;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

public final class SpatialReferenceCoreTest {

  @Before
  public void setUp() {
    GoogleGdalLoader.init();
    osr.UseExceptions();
  }

  @After
  public void tearDown() {
    osr.UseExceptions();
    gdal.ErrorReset();
  }

  @Test
  public void constructorsCloneEqualsToString_roundTrip() {
    SpatialReference wgs84 = new SpatialReference(osrConstants.SRS_WKT_WGS84_LAT_LONG);
    assertThat(wgs84.IsGeographic()).isEqualTo(1);
    assertThat(wgs84.GetName()).contains("WGS 84");
    assertThat(wgs84.toString()).isEqualTo(wgs84.__str__());
    assertThat(wgs84.toString()).contains("GEOGCS");

    SpatialReference cloned = wgs84.Clone();
    Object javaCloned = wgs84.clone();
    assertThat(cloned.IsSame(wgs84)).isEqualTo(1);
    assertThat(((SpatialReference) javaCloned).IsSameGeogCS(wgs84)).isEqualTo(1);
    assertThat(wgs84.equals(wgs84)).isTrue();
    assertThat(wgs84.equals(cloned)).isFalse();
    assertThat(wgs84.equals(new Object())).isFalse();
    assertThat(wgs84.hashCode()).isNotEqualTo(0);

    @SuppressWarnings({"unchecked", "JdkObsolete"})
    Vector<String> opts = new Vector<>();
    opts.add("CRITERIA=EQUIVALENT");
    assertThat(wgs84.IsSame(cloned, opts)).isEqualTo(1);

    SpatialReference copied = new SpatialReference();
    assertThat(copied.CopyGeogCSFrom(wgs84)).isEqualTo(0);
    SpatialReference clonedGeog = wgs84.CloneGeogCS();
    assertThat(clonedGeog.IsSameGeogCS(copied)).isEqualTo(1);

    clonedGeog.delete();
    copied.delete();
    ((SpatialReference) javaCloned).delete();
    cloned.delete();
    wgs84.delete();
  }

  @Test
  public void crsTypesDynamicAndVertical_reportAccuratePredicates() {
    SpatialReference wgs84 = new SpatialReference();
    wgs84.ImportFromEPSG(4326);
    assertThat(wgs84.IsGeographic()).isEqualTo(1);
    assertThat(wgs84.IsDerivedGeographic()).isEqualTo(0);
    assertThat(wgs84.IsProjected()).isEqualTo(0);
    assertThat(wgs84.IsDerivedProjected()).isEqualTo(0);
    assertThat(wgs84.IsCompound()).isEqualTo(0);
    assertThat(wgs84.IsGeocentric()).isEqualTo(0);
    assertThat(wgs84.IsLocal()).isEqualTo(0);
    assertThat(wgs84.IsVertical()).isEqualTo(0);
    assertThat(wgs84.EPSGTreatsAsLatLong()).isEqualTo(1);
    assertThat(wgs84.EPSGTreatsAsNorthingEasting()).isEqualTo(0);
    assertThat(wgs84.GetCelestialBodyName()).isEqualTo("Earth");
    assertThat(wgs84.Validate()).isEqualTo(0);

    // Coordinate epoch & dynamic CRS predicates.
    SpatialReference dynamicSrs = new SpatialReference();
    dynamicSrs.ImportFromEPSG(9000); // ITRF2014
    assertThat(dynamicSrs.IsDynamic()).isTrue();
    assertThat(dynamicSrs.HasPointMotionOperation()).isFalse();
    dynamicSrs.SetCoordinateEpoch(2020.5);
    assertThat(dynamicSrs.GetCoordinateEpoch()).isWithin(1e-6).of(2020.5);

    SpatialReference local = new SpatialReference();
    assertThat(local.SetLocalCS("Local Grid")).isEqualTo(0);
    assertThat(local.IsLocal()).isEqualTo(1);

    SpatialReference geoc = new SpatialReference();
    assertThat(geoc.SetGeogCS("MyGeog", "WGS 84", "WGS 84", 6378137.0, 298.257223563)).isEqualTo(0);
    assertThat(geoc.SetGeocCS("MyGeocentric")).isEqualTo(0);
    assertThat(geoc.IsGeocentric()).isEqualTo(1);
    SpatialReference geocDefault = new SpatialReference();
    assertThat(geocDefault.SetGeocCS()).isEqualTo(0);

    SpatialReference vert = new SpatialReference();
    assertThat(vert.SetVertCS("EGM96", "EGM96 geoid", 2005)).isEqualTo(0);
    assertThat(vert.IsVertical()).isEqualTo(1);
    assertThat(vert.IsSameVertCS(vert)).isEqualTo(1);
    assertThat(new SpatialReference().SetVertCS("V", "VD")).isEqualTo(0);
    assertThat(new SpatialReference().SetVertCS("V")).isEqualTo(0);
    assertThat(new SpatialReference().SetVertCS()).isEqualTo(0);

    SpatialReference compound = new SpatialReference();
    assertThat(compound.SetCompoundCS("WGS84+EGM96", wgs84, vert)).isEqualTo(0);
    assertThat(compound.IsCompound()).isEqualTo(1);
    assertThat(compound.StripVertical()).isEqualTo(0);
    assertThat(compound.IsCompound()).isEqualTo(0);

    compound.delete();
    vert.delete();
    geocDefault.delete();
    geoc.delete();
    local.delete();
    dynamicSrs.delete();
    wgs84.delete();
  }

  @Test
  public void authoritiesAttributesUnitsAxesAndEllipsoid_roundTrip() {
    SpatialReference srs = new SpatialReference();
    assertThat(
            srs.SetGeogCS(
                "Custom Geog",
                "Custom Datum",
                "Custom Sphere",
                6378137.0,
                298.257223563,
                "Greenwich",
                0.0,
                "degree",
                0.0174532925199433))
        .isEqualTo(0);
    assertThat(srs.SetProjCS("Custom TM")).isEqualTo(0);
    assertThat(srs.SetProjection(osrConstants.SRS_PT_TRANSVERSE_MERCATOR)).isEqualTo(0);
    assertThat(srs.SetProjParm(osrConstants.SRS_PP_CENTRAL_MERIDIAN, 9.0)).isEqualTo(0);
    assertThat(srs.GetProjParm(osrConstants.SRS_PP_CENTRAL_MERIDIAN, -999.0))
        .isWithin(1e-9)
        .of(9.0);
    assertThat(srs.GetProjParm(osrConstants.SRS_PP_CENTRAL_MERIDIAN)).isWithin(1e-9).of(9.0);
    assertThat(srs.SetNormProjParm(osrConstants.SRS_PP_SCALE_FACTOR, 0.9996)).isEqualTo(0);
    assertThat(srs.GetNormProjParm(osrConstants.SRS_PP_SCALE_FACTOR, 1.0))
        .isWithin(1e-9)
        .of(0.9996);
    assertThat(srs.GetNormProjParm(osrConstants.SRS_PP_SCALE_FACTOR)).isWithin(1e-9).of(0.9996);

    assertThat(srs.SetLinearUnits("Kilometer", 1000.0)).isEqualTo(0);
    assertThat(srs.GetLinearUnits()).isWithin(1e-6).of(1000.0);
    assertThat(srs.GetLinearUnitsName()).isEqualTo("Kilometer");
    assertThat(srs.SetLinearUnitsAndUpdateParameters("Meter", 1.0)).isEqualTo(0);
    assertThat(srs.SetTargetLinearUnits("PROJCS", "Foot", 0.3048)).isEqualTo(0);
    assertThat(srs.GetTargetLinearUnits("PROJCS")).isWithin(1e-6).of(0.3048);
    assertThat(srs.SetAngularUnits("degree", 0.0174532925199433)).isEqualTo(0);
    assertThat(srs.GetAngularUnits()).isWithin(1e-9).of(0.0174532925199433);
    assertThat(srs.GetAngularUnitsName()).isEqualTo("degree");

    assertThat(srs.GetSemiMajor()).isWithin(1e-3).of(6378137.0);
    assertThat(srs.GetInvFlattening()).isWithin(1e-6).of(298.257223563);
    assertThat(srs.GetSemiMinor()).isWithin(1.0).of(6356752.314);

    assertThat(srs.SetAuthority("PROJCS", "CUSTOM", 42)).isEqualTo(0);
    assertThat(srs.GetAuthorityName("PROJCS")).isEqualTo("CUSTOM");
    assertThat(srs.GetAuthorityCode("PROJCS")).isEqualTo("42");
    assertThat(srs.SetAttrValue("PROJCS", "Renamed TM")).isEqualTo(0);
    assertThat(srs.GetAttrValue("PROJCS")).isEqualTo("Renamed TM");
    assertThat(srs.GetAttrValue("PROJCS", 0)).isEqualTo("Renamed TM");

    SpatialReference epsg32632 = new SpatialReference();
    epsg32632.ImportFromEPSG(32632);
    AreaOfUse area = epsg32632.GetAreaOfUse();
    assertThat(area).isNotNull();
    assertThat(area.getEast_lon_degree()).isGreaterThan(area.getWest_lon_degree());
    assertThat(epsg32632.GetAxesCount()).isEqualTo(2);
    assertThat(epsg32632.GetAxisName(null, 0)).isNotEmpty();
    assertThat(epsg32632.GetAxisOrientation(null, 0)).isEqualTo(osrConstants.OAO_East);

    epsg32632.SetAxisMappingStrategy(osrConstants.OAMS_AUTHORITY_COMPLIANT);
    assertThat(epsg32632.GetAxisMappingStrategy()).isEqualTo(osrConstants.OAMS_AUTHORITY_COMPLIANT);
    assertThat(epsg32632.SetDataAxisToSRSAxisMapping(new int[] {2, 1})).isEqualTo(0);

    // Also cover additional SetGeogCS / SetProjCS / SetFromUserInput overloads.
    SpatialReference s2 = new SpatialReference();
    assertThat(s2.SetGeogCS("G", "D", "E", 6378137.0, 298.25, "Greenwich", 0.0, "degree"))
        .isEqualTo(0);
    assertThat(s2.SetGeogCS("G", "D", "E", 6378137.0, 298.25, "Greenwich", 0.0)).isEqualTo(0);
    assertThat(s2.SetGeogCS("G", "D", "E", 6378137.0, 298.25, "Greenwich")).isEqualTo(0);
    assertThat(s2.SetProjCS()).isEqualTo(0);

    area.delete();
    s2.delete();
    epsg32632.delete();
    srs.delete();
  }

  @Test
  public void utmStatePlaneTowgs84AndTransformations_workCorrectly() {
    SpatialReference utm = new SpatialReference();
    assertThat(utm.SetWellKnownGeogCS("WGS84")).isEqualTo(0);
    assertThat(utm.SetUTM(10, 1)).isEqualTo(0);
    assertThat(utm.GetUTMZone()).isEqualTo(10);
    assertThat(utm.AutoIdentifyEPSG()).isEqualTo(0);
    assertThat(utm.GetAuthorityCode(null)).isEqualTo("32610");

    SpatialReference utmSouth = new SpatialReference();
    utmSouth.SetWellKnownGeogCS("WGS84");
    assertThat(utmSouth.SetUTM(18, 0)).isEqualTo(0);
    assertThat(utmSouth.GetUTMZone()).isEqualTo(-18);
    SpatialReference utmDefaultNorth = new SpatialReference();
    utmDefaultNorth.SetWellKnownGeogCS("WGS84");
    assertThat(utmDefaultNorth.SetUTM(15)).isEqualTo(0);
    assertThat(utmDefaultNorth.GetUTMZone()).isEqualTo(15);

    SpatialReference statePlane = new SpatialReference();
    assertThat(statePlane.SetStatePlane(403, 1, "Foot_US", 0.3048006096012192)).isEqualTo(0);
    assertThat(statePlane.SetStatePlane(403, 1, "Meter")).isEqualTo(0);
    assertThat(statePlane.SetStatePlane(403, 1)).isEqualTo(0);
    assertThat(statePlane.SetStatePlane(403)).isEqualTo(0);

    SpatialReference datumShift = new SpatialReference();
    datumShift.ImportFromEPSG(4326);
    assertThat(datumShift.HasTOWGS84()).isFalse();
    assertThat(datumShift.SetTOWGS84(1.0, 2.0, 3.0)).isEqualTo(0);
    assertThat(datumShift.HasTOWGS84()).isTrue();
    double[] params = datumShift.GetTOWGS84();
    assertThat(params).usingExactEquality().containsExactly(1.0, 2.0, 3.0, 0.0, 0.0, 0.0, 0.0);

    double[] out7 = new double[7];
    assertThat(datumShift.SetTOWGS84(1.0, 2.0, 3.0, 0.1, 0.2, 0.3, 0.4)).isEqualTo(0);
    assertThat(datumShift.GetTOWGS84(out7)).isEqualTo(0);
    assertThat(out7[6]).isWithin(1e-9).of(0.4);

    // IndexOutOfBoundsException if array length != 7.
    IndexOutOfBoundsException towgsThrown =
        assertThrows(IndexOutOfBoundsException.class, () -> datumShift.GetTOWGS84(new double[2]));
    assertThat(towgsThrown).isNotNull();

    SpatialReference osgb = new SpatialReference();
    osgb.ImportFromEPSG(4277);
    assertThat(osgb.AddGuessedTOWGS84()).isEqualTo(0);

    SpatialReference user = new SpatialReference();
    assertThat(user.SetFromUserInput("EPSG:4326")).isEqualTo(0);
    @SuppressWarnings({"unchecked", "JdkObsolete"})
    Vector<String> uiOpts = new Vector<>();
    uiOpts.add("ALLOW_NETWORK_ACCESS=NO");
    assertThat(user.SetFromUserInput("EPSG:3857", uiOpts)).isEqualTo(0);

    assertThat(user.PromoteTo3D("WGS84 3D")).isEqualTo(0);
    assertThat(user.PromoteTo3D()).isEqualTo(0);
    assertThat(user.DemoteTo2D("WGS84 2D")).isEqualTo(0);
    assertThat(user.DemoteTo2D()).isEqualTo(0);

    assertThat(utm.MorphToESRI()).isEqualTo(0);
    assertThat(utm.MorphFromESRI()).isEqualTo(0);

    SpatialReference mercator1sp = new SpatialReference();
    mercator1sp.SetWellKnownGeogCS("WGS84");
    mercator1sp.SetMercator(0.0, 0.0, 0.99, 0.0, 0.0);
    SpatialReference other = mercator1sp.ConvertToOtherProjection(osrConstants.SRS_PT_MERCATOR_2SP);
    assertThat(other).isNotNull();
    assertThat(other.ExportToWkt()).contains("Mercator_2SP");
    @SuppressWarnings({"unchecked", "JdkObsolete"})
    Vector<String> convOpts = new Vector<>();
    SpatialReference otherWithOpts =
        other.ConvertToOtherProjection(osrConstants.SRS_PT_MERCATOR_1SP, convOpts);
    assertThat(otherWithOpts).isNotNull();
    assertThat(otherWithOpts.ExportToWkt()).contains("Mercator_1SP");

    otherWithOpts.delete();
    other.delete();
    mercator1sp.delete();
    user.delete();
    osgb.delete();
    datumShift.delete();
    statePlane.delete();
    utmDefaultNorth.delete();
    utmSouth.delete();
    utm.delete();
  }
}
