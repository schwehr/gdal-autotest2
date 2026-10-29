// Copyright 2026 Google Inc. All Rights Reserved.
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

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdaljp2metadata.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace gdal {
namespace {

static const GByte kMsiUuid[16] = {0xb1, 0x4b, 0xf8, 0xbd, 0x08, 0x3d,
                                   0x4b, 0x43, 0xa5, 0xae, 0x8c, 0xd7,
                                   0xd5, 0xa6, 0xce, 0x03};
static const GByte kXmpUuid[16] = {0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9,
                                   0x42, 0xE8, 0x9C, 0x71, 0x99, 0x94,
                                   0x91, 0xE3, 0xAF, 0xAC};

void AppendU16BE(std::vector<GByte>& buf, GUInt16 val) {
  buf.push_back(static_cast<GByte>((val >> 8) & 0xFF));
  buf.push_back(static_cast<GByte>(val & 0xFF));
}

void AppendU32BE(std::vector<GByte>& buf, GUInt32 val) {
  buf.push_back(static_cast<GByte>((val >> 24) & 0xFF));
  buf.push_back(static_cast<GByte>((val >> 16) & 0xFF));
  buf.push_back(static_cast<GByte>((val >> 8) & 0xFF));
  buf.push_back(static_cast<GByte>(val & 0xFF));
}

void AppendMarker(std::vector<GByte>& cs, GByte code,
                  const std::vector<GByte>& payload) {
  cs.push_back(0xFF);
  cs.push_back(code);
  GUInt16 len = static_cast<GUInt16>(payload.size() + 2);
  AppendU16BE(cs, len);
  cs.insert(cs.end(), payload.begin(), payload.end());
}

void WriteBoxToFile(VSILFILE* fp, GDALJP2Box* box) {
  GByte* data = box->GetWritableBoxData();
  VSIFWriteL(data, 1, static_cast<size_t>(box->GetDataLength() + 8), fp);
  CPLFree(data);
}

std::vector<GByte> BuildComprehensiveCodestream(GByte transformation,
                                                const std::string& com_str) {
  std::vector<GByte> cs;
  // SOC (0xFF4F)
  cs.push_back(0xFF);
  cs.push_back(0x4F);

  // Reserved marker 0xFF30 (no length)
  cs.push_back(0xFF);
  cs.push_back(0x30);

  // CAP (0xFF50): Pcap with bit (31 - 14) set (i+1 == 15) and bit 31 set (i ==
  // 0)
  {
    std::vector<GByte> p;
    GUInt32 pcap = (1U << 31) | (1U << (31 - 14));
    AppendU32BE(p, pcap);
    AppendU16BE(p, 0);               // Scap_P1
    AppendU16BE(p, (2U << 14) | 1);  // Scap_P15
    p.push_back(0x99);               // RemainingBytes
    AppendMarker(cs, 0x50, p);
  }

  // SIZ (0xFF51)
  {
    std::vector<GByte> p;
    AppendU16BE(p, 16384);  // Rsiz = HTJ2K
    AppendU32BE(p, 100);    // Xsiz
    AppendU32BE(p, 100);    // Ysiz
    AppendU32BE(p, 0);      // XOsiz
    AppendU32BE(p, 0);      // YOsiz
    AppendU32BE(p, 100);    // XTsiz
    AppendU32BE(p, 100);    // YTsiz
    AppendU32BE(p, 0);      // XTOSiz
    AppendU32BE(p, 0);      // YTOSiz
    AppendU16BE(p, 2);      // Csiz = 2 components
    // Component 0: unsigned 8-bit
    p.push_back(7);
    p.push_back(1);
    p.push_back(1);
    // Component 1: signed 8-bit
    p.push_back(0x80 | 7);
    p.push_back(1);
    p.push_back(1);
    p.push_back(0x00);  // RemainingBytes
    AppendMarker(cs, 0x51, p);
  }

  // COD (0xFF52)
  {
    std::vector<GByte> p;
    p.push_back(0x07);            // Scod: precincts + SOP + EPH
    p.push_back(0);               // SGcod_Progress: LRCP
    AppendU16BE(p, 1);            // SGcod_NumLayers
    p.push_back(1);               // SGcod_MCT
    p.push_back(1);               // SPcod_NumDecompositions
    p.push_back(4);               // SPcod_xcb_minus_2
    p.push_back(4);               // SPcod_ycb_minus_2
    p.push_back(0xFF);            // SPcod_cbstyle (all flags)
    p.push_back(transformation);  // SPcod_transformation (0 or 1)
    p.push_back(0x77);            // SPcod_Precincts0
    AppendMarker(cs, 0x52, p);
  }

  // COC (0xFF53)
  {
    std::vector<GByte> p;
    p.push_back(0);               // Ccoc (1 byte since Csiz < 257)
    p.push_back(0x01);            // Scoc: user precincts
    p.push_back(1);               // SPcoc_NumDecompositions
    p.push_back(4);               // SPcoc_xcb_minus_2
    p.push_back(4);               // SPcoc_ycb_minus_2
    p.push_back(0x00);            // SPcoc_cbstyle
    p.push_back(transformation);  // SPcoc_transformation
    p.push_back(0x66);            // SPcoc_Precincts0
    AppendMarker(cs, 0x53, p);
  }

  // TLM (0xFF55): ST=1, SP=0 and ST=2, SP=1
  {
    std::vector<GByte> p;
    p.push_back(0);       // Ztlm
    p.push_back(1 << 4);  // Stlm: ST=1, SP=0
    p.push_back(0);       // Ttlm0
    AppendU16BE(p, 100);  // Ptlm0
    p.push_back(0xAA);    // RemainingBytes
    AppendMarker(cs, 0x55, p);
  }
  {
    std::vector<GByte> p;
    p.push_back(1);                    // Ztlm
    p.push_back((2 << 4) | (1 << 6));  // Stlm: ST=2, SP=1
    AppendU16BE(p, 0);                 // Ttlm0
    AppendU32BE(p, 200);               // Ptlm0
    AppendMarker(cs, 0x55, p);
  }

  // PLM (0xFF57)
  AppendMarker(cs, 0x57, {0});

  // PLT (0xFF58)
  {
    std::vector<GByte> p;
    p.push_back(0);     // Zplt
    p.push_back(0x81);  // continuation bit
    p.push_back(0x02);  // end of packet length
    AppendMarker(cs, 0x58, p);
  }

  // CPF (0xFF59)
  {
    std::vector<GByte> p;
    AppendU16BE(p, 1);
    AppendMarker(cs, 0x59, p);
  }

  // QCD (0xFF5C): reversible (0) and scalar expounded (2)
  {
    std::vector<GByte> p;
    p.push_back(0);  // Sqcd = 0 (no quantization)
    p.push_back(8 << 3);
    AppendMarker(cs, 0x5C, p);
  }
  {
    std::vector<GByte> p;
    p.push_back(2 | (1 << 5));  // Sqcd = 2 (scalar expounded)
    AppendU16BE(p, (8 << 11) | 100);
    AppendMarker(cs, 0x5C, p);
  }

  // QCC (0xFF5D): reversible (0) and scalar expounded (2)
  {
    std::vector<GByte> p;
    p.push_back(0);  // Cqcc
    p.push_back(0);  // Sqcc = 0
    p.push_back(8 << 3);
    AppendMarker(cs, 0x5D, p);
  }
  {
    std::vector<GByte> p;
    p.push_back(0);             // Cqcc
    p.push_back(2 | (1 << 5));  // Sqcc = 2
    AppendU16BE(p, (8 << 11) | 100);
    AppendMarker(cs, 0x5D, p);
  }

  // RGN (0xFF5E), PPM (0xFF60), PPT (0xFF61), CRG (0xFF63)
  AppendMarker(cs, 0x5E, {0, 0, 5});
  AppendMarker(cs, 0x60, {0});
  AppendMarker(cs, 0x61, {0});
  AppendMarker(cs, 0x63, {0, 0, 0, 0});

  // POC (0xFF5F)
  {
    std::vector<GByte> p;
    p.push_back(0);     // RSpoc0
    p.push_back(0);     // CSpoc0
    AppendU16BE(p, 1);  // LYEpoc0
    p.push_back(2);     // REpoc0
    p.push_back(2);     // CEpoc0
    p.push_back(1);     // Ppoc0 (RLCP)
    p.push_back(0x99);  // RemainingBytes
    AppendMarker(cs, 0x5F, p);
  }

  // COM (0xFF64)
  if (!com_str.empty()) {
    std::vector<GByte> p;
    AppendU16BE(p, 1);  // Rcom = 1 (LATIN1)
    p.insert(p.end(), com_str.begin(), com_str.end());
    AppendMarker(cs, 0x64, p);
  }

  // SOT (0xFF90)
  {
    std::vector<GByte> p;
    AppendU16BE(p, 0);  // Isot
    AppendU32BE(p, 0);  // Psot = 0 (until EOC)
    p.push_back(0);     // TPsot
    p.push_back(1);     // TNsot
    p.push_back(0x77);  // RemainingBytes
    AppendMarker(cs, 0x90, p);
  }

  // SOD (0xFF93) + 4 dummy bitstream bytes + EOC (0xFFD9)
  cs.push_back(0xFF);
  cs.push_back(0x93);
  cs.push_back(0x01);
  cs.push_back(0x02);
  cs.push_back(0x03);
  cs.push_back(0x04);
  cs.push_back(0xFF);
  cs.push_back(0xD9);

  return cs;
}

TEST(GdalJp2StructureTest, DumpJP2BoxesAndCodestreamStructure) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. Non-existent and non-JP2 file error cases.
  EXPECT_EQ(nullptr,
            GDALGetJPEG2000Structure("/vsimem/no_such_file.jp2", nullptr));
  VSILFILE* bad_fp = VSIFOpenL("/vsimem/not_jp2.bin", "wb+");
  VSIFWriteL("0123456789abcdef", 1, 16, bad_fp);
  EXPECT_EQ(nullptr,
            GDALGetJPEG2000Structure("/vsimem/not_jp2.bin", bad_fp, nullptr));
  VSIFCloseL(bad_fp);
  VSIUnlink("/vsimem/not_jp2.bin");

  // 2. Build a full JP2 file containing all box types:
  //    jP, ftyp, rreq, jp2h(ihdr, bpcc, colr, pclr, cmap, cdef, res(resc,
  //    resd)), uuid(GeoTIFF), uuid(XMP), xml(XML), xml(text), jp2c(codestream).
  const char* jp2_path = "/vsimem/full_struct.jp2";
  VSILFILE* fp = VSIFOpenL(jp2_path, "wb+");
  ASSERT_NE(nullptr, fp);

  // Signature box 'jP  '
  GDALJP2Box box_jp;
  box_jp.SetType("jP  ");
  const GByte jp_sig[4] = {0x0D, 0x0A, 0x87, 0x0A};
  box_jp.SetWritableData(4, jp_sig);
  WriteBoxToFile(fp, &box_jp);

  // ftyp box (BR, MinV, CL0, CL1, + 1 remaining byte)
  GDALJP2Box box_ftyp;
  box_ftyp.SetType("ftyp");
  const GByte ftyp_data[] = {'j', 'p', '2', ' ', 0,   0,   0,   0,   'j',
                             'p', '2', ' ', 'j', 'p', 'x', ' ', 0x99};
  box_ftyp.SetWritableData(sizeof(ftyp_data), ftyp_data);
  WriteBoxToFile(fp, &box_ftyp);

  // rreq box (ML=1, FUAM, DCM, NSF=7 with standard values, NVF=1 with 16B UUID)
  GDALJP2Box box_rreq;
  box_rreq.SetType("rreq");
  std::vector<GByte> rreq_data;
  rreq_data.push_back(1);     // ML = 1
  rreq_data.push_back(0xFF);  // FUAM
  rreq_data.push_back(0x80);  // DCM
  AppendU16BE(rreq_data, 7);  // NSF = 7
  for (GUInt16 sf : {1, 2, 3, 4, 5, 35, 67}) {
    AppendU16BE(rreq_data, sf);
    rreq_data.push_back(0x01);  // SM
  }
  AppendU16BE(rreq_data, 1);  // NVF = 1
  rreq_data.insert(rreq_data.end(), kMsiUuid, kMsiUuid + 16);
  rreq_data.push_back(0x01);  // VM0
  rreq_data.push_back(0xEE);  // RemainingBytes
  box_rreq.SetWritableData(static_cast<int>(rreq_data.size()),
                           rreq_data.data());
  WriteBoxToFile(fp, &box_rreq);

  // ihdr box
  GDALJP2Box box_ihdr;
  box_ihdr.SetType("ihdr");
  std::vector<GByte> ihdr_data;
  AppendU32BE(ihdr_data, 100);  // HEIGHT
  AppendU32BE(ihdr_data, 100);  // WIDTH
  AppendU16BE(ihdr_data, 2);    // NC
  ihdr_data.push_back(255);     // BPC = 255 (varies)
  ihdr_data.push_back(7);       // C
  ihdr_data.push_back(0);       // UnkC
  ihdr_data.push_back(0);       // IPR
  ihdr_data.push_back(0x11);    // RemainingBytes
  box_ihdr.SetWritableData(static_cast<int>(ihdr_data.size()),
                           ihdr_data.data());

  // bpcc box
  GDALJP2Box box_bpcc;
  box_bpcc.SetType("bpcc");
  const GByte bpcc_data[] = {7, static_cast<GByte>(0x80 | 7)};
  box_bpcc.SetWritableData(sizeof(bpcc_data), bpcc_data);

  // colr box (METH=1, PREC=0, APPROX=0, EnumCS=16 sRGB, + 1 extra byte)
  GDALJP2Box box_colr;
  box_colr.SetType("colr");
  const GByte colr_data[] = {1, 0, 0, 0, 0, 0, 16, 0xFF};
  box_colr.SetWritableData(sizeof(colr_data), colr_data);

  // pclr box (NE=2, NPC=2, B0=7, B1=7, C_0_0..C_1_1, + 1 extra byte)
  GDALJP2Box box_pclr;
  box_pclr.SetType("pclr");
  const GByte pclr_data[] = {0, 2, 2, 7, 7, 10, 20, 30, 40, 0xFF};
  box_pclr.SetWritableData(sizeof(pclr_data), pclr_data);

  // cmap box (2 entries + 1 extra byte)
  GDALJP2Box box_cmap;
  box_cmap.SetType("cmap");
  const GByte cmap_data[] = {0, 0, 0, 0, 0, 1, 1, 1, 0xFF};
  box_cmap.SetWritableData(sizeof(cmap_data), cmap_data);

  // cdef box (N=4: Typ=0, 1, 2, 65535 + 1 extra byte)
  GDALJP2Box box_cdef;
  box_cdef.SetType("cdef");
  std::vector<GByte> cdef_data;
  AppendU16BE(cdef_data, 4);
  for (GUInt16 typ : {0, 1, 2, 65535}) {
    AppendU16BE(cdef_data, typ);
    AppendU16BE(cdef_data, typ);
    AppendU16BE(cdef_data, typ);
  }
  cdef_data.push_back(0xFF);
  box_cdef.SetWritableData(static_cast<int>(cdef_data.size()),
                           cdef_data.data());

  // res superbox with resc (exact 10 bytes) and resd (11 bytes ->
  // RemainingBytes)
  GDALJP2Box box_resc;
  box_resc.SetType("resc");
  const GByte resc_data[10] = {0, 10, 0, 1, 0, 20, 0, 1, 2, 2};
  box_resc.SetWritableData(10, resc_data);

  GDALJP2Box box_resd;
  box_resd.SetType("resd");
  const GByte resd_data[11] = {0, 10, 0, 0, 0, 20, 0, 0, 2, 2, 0xFF};
  box_resd.SetWritableData(11, resd_data);

  const GDALJP2Box* res_kids[2] = {&box_resc, &box_resd};
  std::unique_ptr<GDALJP2Box> box_res(
      GDALJP2Box::CreateSuperBox("res ", 2, res_kids));

  const GDALJP2Box* jp2h_kids[] = {&box_ihdr,    &box_bpcc, &box_colr,
                                   &box_pclr,    &box_cmap, &box_cdef,
                                   box_res.get()};
  std::unique_ptr<GDALJP2Box> box_jp2h(
      GDALJP2Box::CreateSuperBox("jp2h", 7, jp2h_kids));
  WriteBoxToFile(fp, box_jp2h.get());

  // GeoTIFF UUID box (1-band so DumpGeoTIFFBox converts to VRT)
  GDALJP2Metadata md;
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  md.SetSpatialRef(&srs);
  md.SetGeoTransform(GDALGeoTransform(0.0, 1.0, 0.0, 0.0, 0.0, -1.0));
  std::unique_ptr<GDALJP2Box> box_gtiff(md.CreateJP2GeoTIFF());
  if (box_gtiff) WriteBoxToFile(fp, box_gtiff.get());

  // XMP UUID box
  const char* xmp_str = "<x:xmpmeta/>";
  std::unique_ptr<GDALJP2Box> box_xmp(
      GDALJP2Box::CreateUUIDBox(kXmpUuid, static_cast<int>(strlen(xmp_str) + 1),
                                reinterpret_cast<const GByte*>(xmp_str)));
  WriteBoxToFile(fp, box_xmp.get());

  // XML box with valid XML and XML box with plain text
  GDALJP2Box box_xml1;
  box_xml1.SetType("xml ");
  const char* xml1_str = "<doc><item>val</item></doc>";
  box_xml1.SetWritableData(static_cast<int>(strlen(xml1_str) + 1),
                           reinterpret_cast<const GByte*>(xml1_str));
  WriteBoxToFile(fp, &box_xml1);

  GDALJP2Box box_xml2;
  box_xml2.SetType("xml ");
  const char* xml2_str = "plain text box content";
  box_xml2.SetWritableData(static_cast<int>(strlen(xml2_str) + 1),
                           reinterpret_cast<const GByte*>(xml2_str));
  WriteBoxToFile(fp, &box_xml2);

  // jp2c codestream box
  auto cs = BuildComprehensiveCodestream(
      1, "Created by OpenJPEG version 2.5 - LOSSLESS settings used");
  GDALJP2Box box_jp2c;
  box_jp2c.SetType("jp2c");
  box_jp2c.SetWritableData(static_cast<int>(cs.size()), cs.data());
  WriteBoxToFile(fp, &box_jp2c);
  VSIFCloseL(fp);

  // Dump structure with ALL=YES.
  const char* const all_opts[] = {"ALL=YES", nullptr};
  CPLXMLTreeCloser tree_all(GDALGetJPEG2000Structure(jp2_path, all_opts));
  ASSERT_NE(nullptr, tree_all.get());

  // Dump structure with MAX_LINES=5 to hit the line limit truncation path.
  const char* const max_lines_opts[] = {"ALL=YES", "MAX_LINES=5", nullptr};
  CPLXMLTreeCloser tree_trunc(
      GDALGetJPEG2000Structure(jp2_path, max_lines_opts));
  ASSERT_NE(nullptr, tree_trunc.get());

  // Test GDALGetJPEG2000Reversibility on the JP2 file.
  VSILFILE* fp_rev = VSIFOpenL(jp2_path, "rb");
  EXPECT_STREQ("LOSSLESS", GDALGetJPEG2000Reversibility(jp2_path, fp_rev));
  VSIFCloseL(fp_rev);
  VSIUnlink(jp2_path);

  CPLPopErrorHandler();
}

TEST(GdalJp2StructureTest, RawCodestreamAndReversibilityVariants) {
  CPLPushErrorHandler(CPLQuietErrorHandler);

  struct RevCase {
    GByte transformation;
    std::string com;
    const char* expected;
  };
  const RevCase cases[] = {
      {0, "", "LOSSY"},
      {1, "", "LOSSLESS (possibly)"},
      {1, "Created by OpenJPEG - LOSSY settings used", "LOSSY"},
      {1,
       "Kdu-Layer-Info: log_2{Delta-D(squared-error)/Delta-L(bytes)}, "
       "L(bytes)\n-192.0, 100",
       "LOSSLESS"},
      {1,
       "Kdu-Layer-Info: log_2{Delta-D(squared-error)/Delta-L(bytes)}, "
       "L(bytes)\n-10.0, 100",
       "LOSSY"},
      {1,
       "Kdu-Layer-Info: log_2{Delta-D(MSE)/[2^16*Delta-L(bytes)]}, "
       "L(bytes)\n-256.0, 100",
       "LOSSLESS"},
      {1,
       "Kdu-Layer-Info: log_2{Delta-D(MSE)/[2^16*Delta-L(bytes)]}, "
       "L(bytes)\n-10.0, 100",
       "LOSSY"},
  };

  for (const auto& c : cases) {
    auto cs = BuildComprehensiveCodestream(c.transformation, c.com);
    const char* j2k_path = "/vsimem/test_rev.j2k";
    VSILFILE* fp = VSIFOpenL(j2k_path, "wb+");
    VSIFWriteL(cs.data(), 1, cs.size(), fp);
    EXPECT_STREQ(c.expected, GDALGetJPEG2000Reversibility(j2k_path, fp));

    const char* const cs_opts[] = {"CODESTREAM=YES", nullptr};
    CPLXMLTreeCloser xml(GDALGetJPEG2000Structure(j2k_path, fp, cs_opts));
    EXPECT_NE(nullptr, xml.get());

    VSIFCloseL(fp);
    VSIUnlink(j2k_path);
  }

  // Test truncated/invalid codestream markers (non-0xFF, unknown marker, EOF).
  {
    std::vector<GByte> bad_cs = {0xFF, 0x4F,               // SOC
                                 0xFF, 0x51, 0x00, 0x02,   // SIZ (truncated)
                                 0xFF, 0x77, 0x00, 0x02,   // Unknown marker
                                 0x12, 0x34,               // Not a marker
                                 0x00, 0x00, 0x00, 0x00};  // Pad to >=16 bytes
    const char* bad_path = "/vsimem/bad_cs.j2k";
    VSILFILE* fp = VSIFOpenL(bad_path, "wb+");
    VSIFWriteL(bad_cs.data(), 1, bad_cs.size(), fp);
    VSIFSeekL(fp, 0, SEEK_SET);
    const char* const cs_opts[] = {"ALL=YES", nullptr};
    CPLXMLTreeCloser xml(GDALGetJPEG2000Structure(bad_path, fp, cs_opts));
    EXPECT_NE(nullptr, xml.get());
    VSIFCloseL(fp);
    VSIUnlink(bad_path);
  }

  CPLPopErrorHandler();
}

}  // namespace
}  // namespace gdal
