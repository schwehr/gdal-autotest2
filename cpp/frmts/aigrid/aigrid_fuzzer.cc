// Copyright 2020 Google Inc. All Rights Reserved.
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

#include "security/fuzzing/blaze/proto_message_mutator.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/frmts/aigrid/aigrid_fuzzer.pb.h"
#include "autotest2/cpp/frmts/aigrid/aigrid_fuzzer.proto.h"
#include "autotest2/cpp/fuzzers/gdal.h"
#include "autotest2/cpp/util/error_handler.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_frmts.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_conv.h"
#include "port/cpl_error.h"
#include "port/cpl_port.h"
#include "port/cpl_string.h"
#include "port/cpl_vsi.h"

using third_party::gdal::autotest2::cpp::frmts::aigrid::Aigrid;

DEFINE_PROTO_FUZZER(const Aigrid &a) {
  constexpr char kFilenameAux[] = "/vsimem/a.aux";
  constexpr char kFilenameRrd[] = "/vsimem/a.rrd";

  autotest2::VsiMemMaybeTempWrapper aux(kFilenameAux, a.aux(), a.has_aux());
  autotest2::VsiMemMaybeTempWrapper rrd(kFilenameRrd, a.rrd(), a.has_rrd());

  constexpr char kFilename[] = "/vsimem/a";
  VSIMkdir(kFilename, 0755);

  constexpr char kFilenameClr[] = "/vsimem/a/a.clr";
  constexpr char kFilenameDbl[] = "/vsimem/a/dblbnd.adf";
  constexpr char kFilenameHdr[] = "/vsimem/a/hdr.adf";
  constexpr char kFilenameLog[] = "/vsimem/a/log";
  constexpr char kFilenameXml[] = "/vsimem/a/metadata.xml";
  constexpr char kFilenamePrj[] = "/vsimem/a/prj.adf";
  constexpr char kFilenameSta[] = "/vsimem/a/sta.adf";
  constexpr char kFilenameVat[] = "/vsimem/a/vat.adf";
  constexpr char kFilenameW01[] = "/vsimem/a/w001001_adf";
  constexpr char kFilenameW1x[] = "/vsimem/a/w001001x_adf";

  autotest2::VsiMemMaybeTempWrapper clr(kFilenameClr, a.clr(), a.has_clr());
  autotest2::VsiMemMaybeTempWrapper dbl(kFilenameDbl, a.dbl(), a.has_dbl());
  autotest2::VsiMemMaybeTempWrapper hdr(kFilenameHdr, a.hdr(), a.has_hdr());
  autotest2::VsiMemMaybeTempWrapper log(kFilenameLog, a.log(), a.has_log());
  autotest2::VsiMemMaybeTempWrapper xml(kFilenameXml, a.xml(), a.has_xml());
  autotest2::VsiMemMaybeTempWrapper prj(kFilenamePrj, a.prj(), a.has_prj());
  autotest2::VsiMemMaybeTempWrapper sta(kFilenameSta, a.sta(), a.has_sta());
  autotest2::VsiMemMaybeTempWrapper vat(kFilenameVat, a.vat(), a.has_vat());
  autotest2::VsiMemMaybeTempWrapper w01(kFilenameW01, a.w01(), a.has_w01());
  autotest2::VsiMemMaybeTempWrapper x1k(kFilenameW1x, a.w1x(), a.has_w1x());

  constexpr char kFilenameInf[] = "/vsimem/info";
  VSIMkdir(kFilenameInf, 0755);

  constexpr char kFilenameDat0[] = "/vsimem/info/arc0000.dat";
  constexpr char kFilenameNit0[] = "/vsimem/info/arc0000.nit";
  constexpr char kFilenameXml0[] = "/vsimem/info/arc0000.xml";
  constexpr char kFilenameDat1[] = "/vsimem/info/arc0001.dat";
  constexpr char kFilenameNit1[] = "/vsimem/info/arc0001.nit";
  constexpr char kFilenameXml1[] = "/vsimem/info/arc0001.xml";
  constexpr char kFilenameDat2[] = "/vsimem/info/arc0002.dat";
  constexpr char kFilenameNit2[] = "/vsimem/info/arc0002.nit";
  constexpr char kFilenameXml2[] = "/vsimem/info/arc0002.xml";
  constexpr char kFilenameA2r1[] = "/vsimem/info/arc0002r.001";
  constexpr char kFilenameDat3[] = "/vsimem/info/arc0003.dat";
  constexpr char kFilenameNit3[] = "/vsimem/info/arc0003.nit";
  constexpr char kFilenameXml3[] = "/vsimem/info/arc0003.xml";
  constexpr char kFilenameDir[] = "/vsimem/info/arc.dir";

  autotest2::VsiMemMaybeTempWrapper dat0(kFilenameDat0, a.dat0(), a.has_dat0());
  autotest2::VsiMemMaybeTempWrapper nit0(kFilenameNit0, a.nit0(), a.has_nit0());
  autotest2::VsiMemMaybeTempWrapper xml0(kFilenameXml0, a.xml0(), a.has_xml0());
  autotest2::VsiMemMaybeTempWrapper dat1(kFilenameDat1, a.dat1(), a.has_dat1());
  autotest2::VsiMemMaybeTempWrapper nit1(kFilenameNit1, a.nit1(), a.has_nit1());
  autotest2::VsiMemMaybeTempWrapper xml1(kFilenameXml1, a.xml1(), a.has_xml1());
  autotest2::VsiMemMaybeTempWrapper dat2(kFilenameDat2, a.dat2(), a.has_dat2());
  autotest2::VsiMemMaybeTempWrapper nit2(kFilenameNit2, a.nit2(), a.has_nit2());
  autotest2::VsiMemMaybeTempWrapper xml2(kFilenameXml2, a.xml2(), a.has_xml2());
  autotest2::VsiMemMaybeTempWrapper a2r1(kFilenameA2r1, a.a2r1(), a.has_a2r1());
  autotest2::VsiMemMaybeTempWrapper dat3(kFilenameDat3, a.dat3(), a.has_dat3());
  autotest2::VsiMemMaybeTempWrapper nit3(kFilenameNit3, a.nit3(), a.has_nit3());
  autotest2::VsiMemMaybeTempWrapper xml3(kFilenameXml3, a.xml3(), a.has_xml3());
  autotest2::VsiMemMaybeTempWrapper dir(kFilenameDir, a.dir(), a.has_dir());

  WithQuietHandler error_handler;

  GDALRegister_AIGrid();

  auto dataset = GDALOpen(kFilename, GA_ReadOnly);
  if (dataset == nullptr) return;
  autotest2::GDALFuzzOneInput(static_cast<GDALDataset *>(dataset));
  GDALClose(dataset);
}
