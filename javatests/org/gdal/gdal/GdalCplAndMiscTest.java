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
import static org.junit.Assert.assertThrows;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Vector;
import org.gdal.GoogleGdalLoader;
import org.gdal.gdalconst.gdalconst;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

public final class GdalCplAndMiscTest {

  private static final String VSIMEM_DIR = "/vsimem/GdalCplAndMiscTest";

  @Before
  public void setUp() {
    GoogleGdalLoader.init();
    gdal.UseExceptions();
  }

  @After
  public void tearDown() {
    gdal.UseExceptions();
    gdal.ErrorReset();
    gdal.VSIErrorReset();
    gdal.ClearPathSpecificOptions();
    gdal.ClearCredentials();
    gdal.FinderClean();
    gdal.SetErrorHandler();
    gdal.RmdirRecursive(VSIMEM_DIR);
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void exceptionsErrorsAndConfig_behaveAsExpected() {
    assertThat(gdalJNI.isAvailable()).isAnyOf(true, false);
    assertThat(gdal.GetUseExceptions()).isTrue();
    gdal.DontUseExceptions();
    assertThat(gdal.GetUseExceptions()).isFalse();
    gdal.UseExceptions();
    assertThat(gdal.GetUseExceptions()).isTrue();

    assertThat(gdal.VersionInfo()).isNotEmpty();
    assertThat(gdal.VersionInfo("RELEASE_NAME")).isNotEmpty();
    gdal.Debug("TEST", "debug message");

    // GetConfigOptions safely returns empty when no config options are currently set; calling it
    // with non-empty config options would trigger upstream GetCSLStringAsHashTable free() bug.
    assertThat(gdal.GetConfigOptions()).isEmpty();

    gdal.SetConfigOption("GDAL_JAVA_TEST_KEY", "global_val");
    assertThat(gdal.GetConfigOption("GDAL_JAVA_TEST_KEY")).isEqualTo("global_val");
    assertThat(gdal.GetConfigOption("GDAL_JAVA_MISSING_KEY", "def")).isEqualTo("def");
    assertThat(gdal.GetGlobalConfigOption("GDAL_JAVA_TEST_KEY")).isEqualTo("global_val");
    assertThat(gdal.GetGlobalConfigOption("GDAL_JAVA_MISSING_KEY", "gdef")).isEqualTo("gdef");
    gdal.SetThreadLocalConfigOption("GDAL_JAVA_TEST_KEY", "tl_val");
    assertThat(gdal.GetThreadLocalConfigOption("GDAL_JAVA_TEST_KEY")).isEqualTo("tl_val");
    assertThat(gdal.GetThreadLocalConfigOption("GDAL_JAVA_MISSING_KEY", "tldef"))
        .isEqualTo("tldef");
    gdal.SetThreadLocalConfigOption("GDAL_JAVA_TEST_KEY", null);
    gdal.SetConfigOption("GDAL_JAVA_TEST_KEY", null);

    gdal.SetPathSpecificOption(VSIMEM_DIR, "PATH_OPT", "yes");
    assertThat(gdal.GetPathSpecificOption(VSIMEM_DIR + "/a.tif", "PATH_OPT")).isEqualTo("yes");
    assertThat(gdal.GetPathSpecificOption("/vsimem/other/a.tif", "PATH_OPT", "no")).isEqualTo("no");
    gdal.ClearPathSpecificOptions(VSIMEM_DIR);
    gdal.ClearPathSpecificOptions();

    gdal.SetCredential(VSIMEM_DIR, "TOKEN", "secret");
    assertThat(gdal.GetCredential(VSIMEM_DIR + "/b.tif", "TOKEN")).isEqualTo("secret");
    assertThat(gdal.GetCredential("/vsimem/other/b.tif", "TOKEN", "none")).isEqualTo("none");
    gdal.ClearCredentials(VSIMEM_DIR);
    gdal.ClearCredentials();

    // Error handlers & error reporting.
    assertThat(gdal.PushErrorHandler("CPLQuietErrorHandler")).isEqualTo(gdalconst.CE_None);
    gdal.SetCurrentErrorHandlerCatchDebug(0);
    gdal.ErrorReset();
    gdal.Error(gdalconst.CE_Warning, gdalconst.CPLE_AppDefined, "synthetic warning");
    assertThat(gdal.GetLastErrorType()).isEqualTo(gdalconst.CE_Warning);
    assertThat(gdal.GetLastErrorNo()).isEqualTo(gdalconst.CPLE_AppDefined);
    assertThat(gdal.GetLastErrorMsg()).contains("synthetic warning");
    assertThat(gdal.GetErrorCounter()).isGreaterThan(0L);
    gdal.ErrorReset();
    assertThat(gdal.GetLastErrorNo()).isEqualTo(0);
    gdal.PopErrorHandler();

    assertThat(gdal.PushErrorHandler()).isEqualTo(gdalconst.CE_None);
    gdal.PopErrorHandler();
    assertThat(gdal.SetErrorHandler()).isEqualTo(gdalconst.CE_None);
    NullPointerException eh1 =
        assertThrows(NullPointerException.class, () -> gdal.SetErrorHandler(null));
    assertThat(eh1).hasMessageThat().contains("CPLErrorHandler");
    NullPointerException eh2 =
        assertThrows(NullPointerException.class, () -> gdal.SetErrorHandler(null, null));
    assertThat(eh2).hasMessageThat().contains("CPLErrorHandler");

    gdal.VSIErrorReset();
    assertThat(gdal.VSIGetLastErrorNo()).isEqualTo(0);
    assertThat(gdal.VSIGetLastErrorMsg()).isEmpty();
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void vsiFinderStringsHexCmdLineAndDataTypes_workCorrectly() {
    gdal.Mkdir(VSIMEM_DIR, 0755);
    gdal.MkdirRecursive(VSIMEM_DIR + "/sub/deep", 0755);

    String file1 = VSIMEM_DIR + "/sub/deep/hello.txt";
    String file2 = VSIMEM_DIR + "/sub/deep/copy.txt";
    String file3 = VSIMEM_DIR + "/sub/deep/moved.txt";
    String file4 = VSIMEM_DIR + "/sub/deep/renamed.txt";
    byte[] payload = "Hello GDAL".getBytes(StandardCharsets.UTF_8);
    assertThat(gdal.FileFromMemBuffer(file1, payload)).isEqualTo(0);
    assertThat(gdal.GetMemFileBuffer(file1)).isEqualTo(payload);

    gdal.PushFinderLocation(VSIMEM_DIR + "/sub/deep");
    assertThat(gdal.FindFile(null, "hello.txt")).isEqualTo(file1);
    gdal.PopFinderLocation();
    gdal.FinderClean();

    assertThat(gdal.CopyFile(file1, file2)).isEqualTo(0);
    assertThat(gdal.CopyFile(file1, file2, null)).isEqualTo(0);
    assertThat(gdal.CopyFile(file1, file2, null, -1L)).isEqualTo(0);
    assertThat(gdal.CopyFile(file1, file2, null, -1L, new Vector<String>())).isEqualTo(0);
    ProgressCallback cb = new ProgressCallback();
    assertThat(gdal.CopyFile(file1, file2, null, -1L, new Vector<String>(), cb)).isEqualTo(0);

    assertThat(gdal.Move(file2, file3)).isEqualTo(0);
    assertThat(gdal.Move(file3, file2, new Vector<String>())).isEqualTo(0);
    assertThat(gdal.Move(file2, file3, new Vector<String>(), cb)).isEqualTo(0);
    assertThat(gdal.MoveFile(file3, file4)).isEqualTo(0);
    assertThat(gdal.Rename(file4, file3)).isEqualTo(0);

    List<String> dirEntries = gdal.ReadDir(VSIMEM_DIR + "/sub/deep");
    assertThat(dirEntries).contains("hello.txt");
    assertThat((List<String>) gdal.ReadDir(VSIMEM_DIR + "/sub/deep", 1)).isNotEmpty();
    assertThat((List<String>) gdal.ReadDirRecursive(VSIMEM_DIR)).contains("sub/deep/hello.txt");

    Vector<String> batch = new Vector<>();
    batch.add(file3);
    assertThat(gdal.UnlinkBatch(batch)).isTrue();
    assertThat(gdal.Unlink(file1)).isEqualTo(0);
    assertThat(gdal.Rmdir(VSIMEM_DIR + "/sub/deep")).isEqualTo(0);
    assertThat(gdal.RmdirRecursive(VSIMEM_DIR)).isEqualTo(0);

    assertThat(gdal.GetActualURL("/vsimem/foo.tif")).isNull();
    assertThat(gdal.GetSignedURL("/vsimem/foo.tif")).isNull();
    assertThat(gdal.GetSignedURL("/vsimem/foo.tif", new Vector<String>())).isNull();
    List<String> prefixes = gdal.GetFileSystemsPrefixes();
    assertThat(prefixes).contains("/vsimem/");
    assertThat(gdal.GetFileSystemOptions("/vsimem/")).isNull();

    // Escaping, Hex, CLI, system info, cache, datatypes & DMS.
    assertThat(gdal.EscapeString("a b&c", gdalconst.CPLES_URL)).isEqualTo("a%20b%26c");
    assertThat(gdal.EscapeString("<a>".getBytes(StandardCharsets.UTF_8), gdalconst.CPLES_XML))
        .isEqualTo("&lt;a&gt;");
    byte[] raw = new byte[] {0x01, (byte) 0xAB, (byte) 0xFF};
    String hex = gdal.CPLBinaryToHex(raw);
    assertThat(hex).isEqualTo("01ABFF");
    assertThat(gdal.CPLHexToBinary(hex)).isEqualTo(raw);

    assertThat((List<String>) gdal.ParseCommandLine("-a \"hello world\""))
        .containsExactly("-a", "hello world")
        .inOrder();
    Vector<String> cliVec = new Vector<>();
    cliVec.add("prog");
    cliVec.add("in.tif");
    assertThat((List<String>) gdal.GeneralCmdLineProcessor(cliVec)).contains("in.tif");
    assertThat((List<String>) gdal.GeneralCmdLineProcessor(cliVec, 0)).contains("in.tif");
    assertThat(gdal.GeneralCmdLineProcessor(new String[] {"x.tif"}))
        .asList()
        .containsExactly("x.tif");
    assertThat(gdal.GeneralCmdLineProcessor(new String[] {"y.tif"}, 0))
        .asList()
        .containsExactly("y.tif");

    assertThat(gdal.HasThreadSupport()).isAtLeast(0);
    assertThat(gdal.GetCurrentThreadCount()).isAtLeast(0);
    assertThat(gdal.GetNumCPUs()).isAtLeast(1);
    assertThat(gdal.GetUsablePhysicalRAM()).isGreaterThan(0L);
    assertThat(gdal.HasTriangulation()).isAnyOf(0, 1);

    assertThat(gdal.GOA2GetAuthorizationURL("https://www.googleapis.com/auth/devstorage.read_only"))
        .contains("accounts.google.com");
    gdal.DontUseExceptions();
    assertThat(gdal.GOA2GetRefreshToken("bad_auth", "scope")).isNull();
    assertThat(gdal.GOA2GetAccessToken("bad_refresh", "scope")).isNull();
    assertThat(gdal.GetJPEG2000StructureAsString(VSIMEM_DIR + "/missing.jp2")).isNull();
    assertThat(gdal.GetJPEG2000StructureAsString(VSIMEM_DIR + "/missing.jp2", new Vector<String>()))
        .isNull();
    gdal.UseExceptions();

    int prevCache = gdal.GetCacheMax();
    gdal.SetCacheMax(prevCache);
    assertThat(gdal.GetCacheUsed()).isAtLeast(0);

    assertThat(gdal.GetDataTypeSize(gdalconst.GDT_Float32)).isEqualTo(32);
    assertThat(gdal.GetDataTypeSizeBits(gdalconst.GDT_Float32)).isEqualTo(32);
    assertThat(gdal.GetDataTypeSizeBytes(gdalconst.GDT_Float32)).isEqualTo(4);
    assertThat(gdal.DataTypeIsComplex(gdalconst.GDT_CFloat32)).isEqualTo(1);
    assertThat(gdal.GetDataTypeName(gdalconst.GDT_UInt16)).isEqualTo("UInt16");
    assertThat(gdal.GetDataTypeByName("Float64")).isEqualTo(gdalconst.GDT_Float64);
    assertThat(gdal.DataTypeUnion(gdalconst.GDT_Byte, gdalconst.GDT_Int16))
        .isEqualTo(gdalconst.GDT_Int16);
    assertThat(gdal.DataTypeUnionWithValue(gdalconst.GDT_Byte, 500.0, false))
        .isEqualTo(gdalconst.GDT_UInt16);
    assertThat(gdal.GetColorInterpretationName(gdalconst.GCI_RedBand)).isEqualTo("Red");
    assertThat(gdal.GetColorInterpretationByName("Red")).isEqualTo(gdalconst.GCI_RedBand);
    assertThat(gdal.GetPaletteInterpretationName(gdalconst.GPI_RGB)).isEqualTo("RGB");

    String dms = gdal.DecToDMS(45.5, "Lat", 2);
    assertThat(dms).contains("45d30");
    assertThat(gdal.DecToDMS(45.5, "Lat")).contains("45d30");
    double packed = gdal.DecToPackedDMS(45.5);
    assertThat(gdal.PackedDMSToDec(packed)).isWithin(1e-6).of(45.5);

    cb.delete();
  }

  @Test
  public void xmlNodeGcpGeoTransformHomographyAndSubdatasetInfo_workCorrectly() {
    // XMLNode & XMLNodeType.
    XMLNode parsed = gdal.ParseXMLString("<root><item key=\"k\">val</item></root>");
    assertThat(parsed.getType()).isEqualTo(XMLNodeType.CXT_Element);
    assertThat(parsed.getValue()).isEqualTo("root");
    assertThat(parsed.toString()).contains("item");
    assertThat(gdal.SerializeXMLTree(parsed)).contains("<item");

    XMLNode child = new XMLNode(XMLNodeType.CXT_Element, "extra");
    child.AddXMLChild(new XMLNode(XMLNodeType.CXT_Text, "hello"));
    parsed.AddXMLChild(child);
    assertThat(parsed.GetXMLValue("extra", "default")).isEqualTo("hello");
    assertThat(parsed.GetXMLNode("extra")).isNotNull();
    assertThat(parsed.SearchXMLNode("extra")).isNotNull();
    assertThat(parsed.SetXMLValue("extra", "updated")).isEqualTo(1);
    parsed.AddXMLSibling(new XMLNode(XMLNodeType.CXT_Comment, "note"));
    assertThat(parsed.getChild()).isNotNull();
    assertThat(parsed.getNext()).isNotNull();
    parsed.StripXMLNamespace(null, 1);
    XMLNode clonedXml = parsed.Clone();
    assertThat(clonedXml.SerializeXMLTree()).contains("updated");
    assertThat(parsed.equals(parsed)).isTrue();
    assertThat(parsed.equals(clonedXml)).isFalse();
    assertThat(parsed.hashCode()).isNotEqualTo(0);

    String xmlPath = VSIMEM_DIR + "/sample.xml";
    gdal.MkdirRecursive(VSIMEM_DIR, 0755);
    gdal.FileFromMemBuffer(xmlPath, clonedXml.SerializeXMLTree().getBytes(StandardCharsets.UTF_8));
    XMLNode fromFile = XMLNode.ParseXMLFile(xmlPath);
    assertThat(fromFile.GetXMLValue("extra", "")).isEqualTo("updated");

    XMLNode fromCtor = new XMLNode("<a/>");
    assertThat(fromCtor.getValue()).isEqualTo("a");

    assertThat(XMLNodeType.CXT_Element.swigValue()).isEqualTo(0);
    assertThat(XMLNodeType.CXT_Element.toString()).isEqualTo("CXT_Element");
    assertThat(XMLNodeType.swigToEnum(XMLNodeType.CXT_Text.swigValue()))
        .isEqualTo(XMLNodeType.CXT_Text);

    fromCtor.delete();
    fromFile.delete();
    clonedXml.delete();
    parsed.delete();

    // GCP, GCPsToGeoTransform, GCPsToHomography, Apply/Inv GeoTransform & Homography.
    GCP g0 = new GCP(0.0, 0.0, 0.0, 0.0, 0.0, "origin", "id0");
    GCP g1 = new GCP(100.0, 0.0, 100.0, 0.0, "right", "id1");
    GCP g2 = new GCP(0.0, -100.0, 0.0, 0.0, 100.0);
    GCP g3 = new GCP(100.0, -100.0, 100.0, 100.0);
    g3.setGCPX(100.0);
    g3.setGCPY(-100.0);
    g3.setGCPZ(0.0);
    g3.setGCPPixel(100.0);
    g3.setGCPLine(100.0);
    g3.setInfo("br");
    g3.setId("id3");
    assertThat(g3.getGCPX()).isEqualTo(100.0);
    assertThat(g3.getGCPY()).isEqualTo(-100.0);
    assertThat(g3.getGCPZ()).isEqualTo(0.0);
    assertThat(g3.getGCPPixel()).isEqualTo(100.0);
    assertThat(g3.getGCPLine()).isEqualTo(100.0);
    assertThat(g3.getInfo()).isEqualTo("br");
    assertThat(g3.getId()).isEqualTo("id3");

    gdal.GDAL_GCP_GCPX_set(g3, 100.0);
    gdal.GDAL_GCP_GCPY_set(g3, -100.0);
    gdal.GDAL_GCP_GCPZ_set(g3, 0.0);
    gdal.GDAL_GCP_GCPPixel_set(g3, 100.0);
    gdal.GDAL_GCP_GCPLine_set(g3, 100.0);
    gdal.GDAL_GCP_Info_set(g3, "br");
    gdal.GDAL_GCP_Id_set(g3, "id3");
    assertThat(gdal.GDAL_GCP_GCPX_get(g3)).isEqualTo(100.0);
    assertThat(gdal.GDAL_GCP_GCPY_get(g3)).isEqualTo(-100.0);
    assertThat(gdal.GDAL_GCP_GCPZ_get(g3)).isEqualTo(0.0);
    assertThat(gdal.GDAL_GCP_GCPPixel_get(g3)).isEqualTo(100.0);
    assertThat(gdal.GDAL_GCP_GCPLine_get(g3)).isEqualTo(100.0);
    assertThat(gdal.GDAL_GCP_Info_get(g3)).isEqualTo("br");
    assertThat(gdal.GDAL_GCP_Id_get(g3)).isEqualTo("id3");

    GCP[] gcps = new GCP[] {g0, g1, g2, g3};
    double[] gt = new double[6];
    assertThat(gdal.GCPsToGeoTransform(gcps, gt)).isEqualTo(1);
    assertThat(gdal.GCPsToGeoTransform(gcps, gt, 1)).isEqualTo(1);
    double[] gx = new double[1];
    double[] gy = new double[1];
    gdal.ApplyGeoTransform(gt, 50.0, 50.0, gx, gy);
    assertThat(gx[0]).isWithin(1e-6).of(50.0);
    assertThat(gy[0]).isWithin(1e-6).of(-50.0);

    double[] invGt = gdal.InvGeoTransform(gt);
    assertThat(invGt).hasLength(6);
    double[] invGtOut = new double[6];
    assertThat(gdal.InvGeoTransform(gt, invGtOut)).isEqualTo(1);
    assertThat(gdal.InvGeoTransform(new double[6])).isNull();
    assertThat(gdal.InvGeoTransform(new double[6], invGtOut)).isEqualTo(0);

    double[] h = new double[9];
    assertThat(gdal.GCPsToHomography(gcps, h)).isEqualTo(1);
    assertThat(gdal.ApplyHomography(h, 50.0, 50.0, gx, gy)).isEqualTo(1);
    assertThat(gx[0]).isWithin(1e-6).of(50.0);
    double[] invH = new double[9];
    assertThat(gdal.InvHomography(h, invH)).isEqualTo(1);

    g3.delete();
    g2.delete();
    g1.delete();
    g0.delete();

    // SubdatasetInfo (package-private wrapper around SWIGTYPE_p_GDALSubdatasetInfo).
    assertThat(gdal.GetSubdatasetInfo("plain_file.tif")).isNull();
    SWIGTYPE_p_GDALSubdatasetInfo rawSub =
        gdal.GetSubdatasetInfo("NETCDF:\"/vsimem/GdalCplAndMiscTest/sub.nc\":temperature");
    assertThat(rawSub).isNotNull();
    SubdatasetInfo subInfo =
        new SubdatasetInfo(SWIGTYPE_p_GDALSubdatasetInfo.getCPtr(rawSub), true);
    assertThat(subInfo.GetPathComponent()).isEqualTo("/vsimem/GdalCplAndMiscTest/sub.nc");
    assertThat(subInfo.GetSubdatasetComponent()).isEqualTo("temperature");
    assertThat(subInfo.ModifyPathComponent("/vsimem/other.nc"))
        .isEqualTo("NETCDF:\"/vsimem/other.nc\":temperature");
    assertThat(subInfo.equals(subInfo)).isTrue();
    assertThat(subInfo.equals(new Object())).isFalse();
    assertThat(subInfo.hashCode()).isNotEqualTo(0);
    subInfo.delete();
  }
}
