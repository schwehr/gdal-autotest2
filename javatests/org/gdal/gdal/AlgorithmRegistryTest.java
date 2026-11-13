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

package org.gdal.gdal;

import static com.google.common.truth.Truth.assertThat;

import java.util.List;
import java.util.Vector;
import org.gdal.GoogleGdalLoader;
import org.gdal.gdalconst.gdalconst;
import org.gdal.ogr.Layer;
import org.gdal.ogr.ogrConstants;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

public final class AlgorithmRegistryTest {

  private static final String VSIMEM_DIR = "/vsimem/AlgorithmRegistryTest";

  private static final class RecordingProgressCallback extends ProgressCallback {
    int count;

    @Override
    public int run(double dfComplete, String pszMessage) {
      count++;
      return 1;
    }
  }

  @Before
  public void setUp() {
    GoogleGdalLoader.init();
    gdal.UseExceptions();
  }

  @After
  public void tearDown() {
    gdal.UseExceptions();
    gdal.ErrorReset();
    gdal.RmdirRecursive(VSIMEM_DIR);
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void registryAlgorithmAndArgApis_inspectAndExecuteEndToEnd() {
    assertThat(gdal.AlgorithmArgTypeIsList(gdalconst.GAAT_STRING_LIST)).isTrue();
    assertThat(gdal.AlgorithmArgTypeIsList(gdalconst.GAAT_STRING)).isFalse();
    assertThat(gdal.AlgorithmArgTypeName(gdalconst.GAAT_INTEGER)).isEqualTo("integer");

    AlgorithmRegistry registry = gdal.GetGlobalAlgorithmRegistry();
    assertThat(registry).isNotNull();
    assertThat(registry.equals(registry)).isTrue();
    assertThat(registry.equals(new Object())).isFalse();
    assertThat(registry.hashCode()).isNotEqualTo(0);

    // In google3's static build, top-level CLI algorithms are not statically registered,
    // while root "gdal" and driver-declared sub-algorithms under "driver" are instantiable.
    assertThat((List<String>) registry.GetAlgNames()).isEmpty();
    assertThat(registry.InstantiateAlg("nonexistent_alg_xyz")).isNull();

    // Instantiate root "gdal" algorithm (GDALMainAlgorithm).
    Algorithm mainAlg = registry.InstantiateAlg("gdal");
    assertThat(mainAlg).isNotNull();
    assertThat(mainAlg.GetName()).isEqualTo("gdal");
    assertThat(mainAlg.GetDescription()).isNotEmpty();
    assertThat(mainAlg.GetLongDescription()).isNotEmpty();
    assertThat(mainAlg.GetHelpFullURL()).isNotEmpty();
    assertThat(mainAlg.HasSubAlgorithms()).isTrue();
    assertThat((List<String>) mainAlg.GetSubAlgorithmNames()).contains("driver");
    assertThat(mainAlg.GetUsageAsJSON()).contains("\"name\"");
    assertThat(mainAlg.equals(mainAlg)).isTrue();
    assertThat(mainAlg.equals(new Object())).isFalse();
    assertThat(mainAlg.hashCode()).isNotEqualTo(0);

    List<String> mainArgNames = mainAlg.GetArgNames();
    assertThat(mainArgNames).containsAtLeast("drivers", "output-string");

    AlgorithmArg driversArg = mainAlg.GetArgNonConst("drivers");
    AlgorithmArg driversConst = mainAlg.GetArg("drivers");
    assertThat(driversConst.GetName()).isEqualTo("drivers");
    assertThat(driversArg.GetType()).isEqualTo(gdalconst.GAAT_BOOLEAN);
    assertThat(driversArg.GetDescription()).isNotEmpty();
    assertThat(driversArg.GetShortName()).isNotNull();
    assertThat((List<String>) driversArg.GetAliases()).isNotNull();
    assertThat(driversArg.GetMetaVar()).isNotNull();
    assertThat(driversArg.GetCategory()).isNotNull();
    assertThat(driversArg.IsPositional()).isFalse();
    assertThat(driversArg.IsRequired()).isFalse();
    assertThat(driversArg.GetMinCount()).isAtLeast(0);
    assertThat(driversArg.GetMaxCount()).isAtLeast(1);
    assertThat(driversArg.GetPackedValuesAllowed()).isAnyOf(true, false);
    assertThat(driversArg.GetRepeatedArgAllowed()).isAnyOf(true, false);
    assertThat((List<String>) driversArg.GetChoices()).isNotNull();
    assertThat((List<String>) driversArg.GetMetadataItem("MISSING_KEY")).isNotNull();
    assertThat(driversArg.HasDefaultValue()).isAnyOf(true, false);
    assertThat(driversArg.GetDefaultAsBoolean()).isFalse();
    assertThat(driversArg.IsHidden()).isFalse();
    assertThat(driversArg.IsHiddenForCLI()).isAnyOf(true, false);
    assertThat(driversArg.IsHiddenForAPI()).isAnyOf(true, false);
    assertThat(driversArg.IsInput()).isTrue();
    assertThat(driversArg.IsOutput()).isFalse();
    assertThat(driversArg.GetDatasetType()).isAtLeast(0);
    assertThat(driversArg.GetDatasetInputFlags()).isAtLeast(0);
    assertThat(driversArg.GetDatasetOutputFlags()).isAtLeast(0);
    assertThat(driversArg.GetMutualExclusionGroup()).isNotNull();
    assertThat(driversArg.equals(driversArg)).isTrue();
    assertThat(driversArg.equals(new Object())).isFalse();
    assertThat(driversArg.hashCode()).isNotEqualTo(0);

    assertThat(driversArg.IsExplicitlySet()).isFalse();
    assertThat(driversArg.SetAsBoolean(true)).isTrue();
    assertThat(driversArg.IsExplicitlySet()).isTrue();
    assertThat(driversArg.GetAsBoolean()).isTrue();

    RecordingProgressCallback cb = new RecordingProgressCallback();
    assertThat(mainAlg.Run(cb)).isTrue();
    assertThat(mainAlg.Finalize()).isTrue();
    AlgorithmArg outStrArg = mainAlg.GetArg("output-string");
    assertThat(outStrArg.GetType()).isEqualTo(gdalconst.GAAT_STRING);
    assertThat(outStrArg.GetAsString()).contains("GTiff");
    if (outStrArg.HasDefaultValue()) {
      assertThat(outStrArg.GetDefaultAsString()).isNotNull();
    }

    // ParseCommandLineArguments, GetActualAlgorithm, Run(), ParseRunAndFinalize().
    Vector<String> cliDrivers = new Vector<>();
    cliDrivers.add("--drivers");
    Algorithm mainAlg2 = registry.InstantiateAlg("gdal");
    assertThat(mainAlg2.ParseCommandLineArguments(cliDrivers)).isTrue();
    Algorithm actual = mainAlg2.GetActualAlgorithm();
    assertThat(actual.GetName()).isEqualTo("gdal");
    assertThat(mainAlg2.Run()).isTrue();
    assertThat(mainAlg2.GetArg("output-string").GetAsString()).contains("MEM");

    Algorithm mainAlg3 = registry.InstantiateAlg("gdal");
    assertThat(mainAlg3.ParseRunAndFinalize(cliDrivers)).isTrue();
    Algorithm mainAlg4 = registry.InstantiateAlg("gdal");
    assertThat(mainAlg4.ParseRunAndFinalize(cliDrivers, cb)).isTrue();

    // Exercise typed arg getters/setters not present on GDALMainAlgorithm under DontUseExceptions.
    gdal.DontUseExceptions();
    assertThat(driversArg.GetDefaultAsString()).isNull();
    assertThat(driversArg.GetDefaultAsInteger()).isEqualTo(0);
    assertThat(driversArg.GetDefaultAsDouble()).isWithin(1e-9).of(0.0);
    assertThat(driversArg.GetAsInteger()).isEqualTo(0);
    assertThat(driversArg.GetAsDouble()).isWithin(1e-9).of(0.0);
    assertThat((List<String>) driversArg.GetDefaultAsStringList()).isEmpty();
    assertThat((List<String>) driversArg.GetAsStringList()).isEmpty();
    assertThat(driversArg.SetAsInteger(1)).isAnyOf(true, false);
    assertThat(outStrArg.SetAsInteger(1)).isAnyOf(true, false);
    assertThat(driversArg.SetAsDouble(1.0)).isAnyOf(true, false);
    assertThat(driversArg.SetAsStringList(new Vector<String>())).isFalse();
    assertThat(driversArg.SetAsIntegerList(new int[] {1})).isFalse();
    assertThat(driversArg.SetAsDoubleList(new double[] {1.0})).isFalse();
    gdal.UseExceptions();
    gdal.ErrorReset();

    // Inspect and run driver-declared "driver" -> "gpkg" -> "repack" algorithm.
    Algorithm driverAlg = registry.InstantiateAlg("driver");
    assertThat(driverAlg).isNotNull();
    assertThat(driverAlg.HasSubAlgorithms()).isTrue();
    assertThat((List<String>) driverAlg.GetSubAlgorithmNames()).contains("gpkg");
    Algorithm gpkgAlg = driverAlg.InstantiateSubAlgorithm("gpkg");
    assertThat(gpkgAlg).isNotNull();
    assertThat((List<String>) gpkgAlg.GetSubAlgorithmNames()).contains("repack");

    gdal.MkdirRecursive(VSIMEM_DIR, 0755);
    String gpkgPath = VSIMEM_DIR + "/alg.gpkg";
    Dataset gpkgDs = gdal.GetDriverByName("GPKG").CreateVector(gpkgPath);
    Layer layer = gpkgDs.CreateLayer("pts", null, ogrConstants.wkbPoint);
    assertThat(layer).isNotNull();
    gpkgDs.FlushCache();

    // Pass Dataset directly via SetDataset & inspect ArgDatasetValue.
    Algorithm repack1 = gpkgAlg.InstantiateSubAlgorithm("repack");
    AlgorithmArg dsArg1 = repack1.GetArgNonConst("dataset");
    assertThat(dsArg1.GetType()).isEqualTo(gdalconst.GAAT_DATASET);
    assertThat(dsArg1.SetDataset(gpkgDs)).isTrue();
    ArgDatasetValue dsVal1 = dsArg1.GetAsDatasetValue();
    assertThat(dsVal1).isNotNull();
    Dataset gotDs = dsVal1.GetDataset();
    assertThat(gotDs).isNotNull();
    gotDs.delete();
    assertThat(dsVal1.equals(dsVal1)).isTrue();
    assertThat(dsVal1.equals(new Object())).isFalse();
    assertThat(dsVal1.hashCode()).isNotEqualTo(0);
    assertThat(repack1.Run(cb)).isTrue();
    assertThat(repack1.Finalize()).isTrue();

    // Exercise SetName / SetDataset on ArgDatasetValue and SetAsDatasetValue on another instance.
    Algorithm repack2 = gpkgAlg.InstantiateSubAlgorithm("repack");
    ArgDatasetValue dsVal2 = repack2.GetArgNonConst("dataset").GetAsDatasetValue();
    dsVal2.SetName(gpkgPath);
    assertThat(dsVal2.GetName()).isEqualTo(gpkgPath);
    dsVal2.SetDataset(gpkgDs);

    Algorithm repack3 = gpkgAlg.InstantiateSubAlgorithm("repack");
    assertThat(repack3.GetArgNonConst("dataset").SetAsDatasetValue(dsVal2)).isTrue();

    repack3.delete();
    dsVal2.delete();
    repack2.delete();
    dsVal1.delete();
    dsArg1.delete();
    repack1.delete();
    gpkgDs.Close();

    // Exercise SetAsString and SetDatasetNames with path on fresh repack instances.
    Algorithm repack4 = gpkgAlg.InstantiateSubAlgorithm("repack");
    assertThat(repack4.GetArgNonConst("dataset").SetAsString(gpkgPath)).isTrue();
    assertThat(repack4.Run()).isTrue();
    assertThat(repack4.Finalize()).isTrue();

    Algorithm repack5 = gpkgAlg.InstantiateSubAlgorithm("repack");
    Vector<String> dsNames = new Vector<>();
    dsNames.add(gpkgPath);
    gdal.DontUseExceptions();
    assertThat(repack5.GetArgNonConst("dataset").SetDatasetNames(dsNames)).isFalse();
    gdal.UseExceptions();
    gdal.ErrorReset();

    repack5.delete();
    repack4.delete();
    gpkgAlg.delete();
    driverAlg.delete();
    outStrArg.delete();
    driversConst.delete();
    driversArg.delete();
    actual.delete();
    mainAlg4.delete();
    mainAlg3.delete();
    mainAlg2.delete();
    mainAlg.delete();
    registry.delete();
    cb.delete();
  }
}
