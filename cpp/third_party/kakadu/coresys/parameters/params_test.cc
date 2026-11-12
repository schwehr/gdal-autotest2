// Copyright 2026 Google LLC
// Tests for third_party/kakadu/vendor/coresys/parameters/params.cpp via
// Kakadu's public kdu_params hierarchy and kdu_output interface.

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ios>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_core {
namespace {

class ThrowingErrorHandler final : public kdu_thread_safe_message {
 public:
  ~ThrowingErrorHandler() noexcept override = default;

  void put_text(const char* text) override { pending_ += text; }

  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      last_message_ = std::move(pending_);
      pending_.clear();
      if (!quiet_) {
        std::fprintf(stderr, "KDU_ERROR: %s\n", last_message_.c_str());
      }
      kdu_thread_safe_message::flush(end_of_message);
      throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
    }
    kdu_thread_safe_message::flush(end_of_message);
  }

  const std::string& last_message() const { return last_message_; }
  void set_quiet(bool quiet) { quiet_ = quiet; }
  void Clear() {
    pending_.clear();
    last_message_.clear();
    quiet_ = false;
  }

 private:
  std::string pending_;
  std::string last_message_;
  bool quiet_ = false;
};

class CapturingWarningHandler final : public kdu_thread_safe_message {
 public:
  ~CapturingWarningHandler() noexcept override = default;

  void put_text(const char* text) override { text_ += text; }

  const std::string& text() const { return text_; }
  void Clear() { text_.clear(); }

 private:
  std::string text_;
};

ThrowingErrorHandler& GlobalErrorHandler() {
  static auto* handler = new ThrowingErrorHandler();
  return *handler;
}

CapturingWarningHandler& GlobalWarningHandler() {
  static auto* handler = new CapturingWarningHandler();
  return *handler;
}

class ParamsTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    kdu_customize_errors(&GlobalErrorHandler());
    kdu_customize_warnings(&GlobalWarningHandler());
  }
};

const ::testing::Environment* const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new ParamsTestEnvironment());

class StringMessage final : public kdu_message {
 public:
  void put_text(const char* text) override { text_ += text; }
  void put_text(const kdu_uint16* /*text*/) override {}
  const std::string& text() const { return text_; }

 private:
  std::string text_;
};

class VectorOutput final : public kdu_output {
 public:
  ~VectorOutput() override { Flush(); }

  void Flush() {
    if (next_buf > buffer) {
      flush_buf();
    }
  }

  const std::vector<kdu_byte>& bytes() const { return data_; }
  void Clear() {
    next_buf = buffer;
    data_.clear();
  }

 protected:
  void flush_buf() override {
    data_.insert(data_.end(), buffer, next_buf);
    next_buf = buffer;
  }

 private:
  std::vector<kdu_byte> data_;
};

struct RawMarkerSegment {
  kdu_uint16 code = 0;
  std::vector<kdu_byte> body;
};

// Splits a stream of concatenated JPEG 2000 marker segments emitted by
// `kdu_params::generate_marker_segments` into (marker_code, body_after_Lmar).
std::vector<RawMarkerSegment> SplitMarkerSegments(
    const std::vector<kdu_byte>& stream) {
  std::vector<RawMarkerSegment> out;
  size_t pos = 0;
  while (pos + 4 <= stream.size()) {
    const kdu_uint16 code =
        static_cast<kdu_uint16>((stream[pos] << 8) | stream[pos + 1]);
    const int lmar = (stream[pos + 2] << 8) | stream[pos + 3];
    if (lmar < 2 || pos + 2 + static_cast<size_t>(lmar) > stream.size()) {
      ADD_FAILURE() << "Malformed marker length " << lmar << " at offset "
                    << pos;
      break;
    }
    RawMarkerSegment seg;
    seg.code = code;
    const size_t body_len = static_cast<size_t>(lmar - 2);
    seg.body.assign(
        stream.begin() + static_cast<std::ptrdiff_t>(pos + 4),
        stream.begin() + static_cast<std::ptrdiff_t>(pos + 4 + body_len));
    out.push_back(std::move(seg));
    pos += 2 + static_cast<size_t>(lmar);
  }
  EXPECT_EQ(pos, stream.size());
  return out;
}

template <typename ParamT>
void LinkCluster(kdu_params* head, int total_tiles, int num_comps) {
  auto child = std::make_unique<ParamT>();
  child->link(head, -1, -1, total_tiles, num_comps);
  child.release();
}

// Constructs the full 15-cluster parameter network mirroring codestream.cpp.
std::unique_ptr<siz_params> BuildFullCluster(int total_tiles, int num_comps,
                                             int nlt_comps,
                                             bool include_enc = true) {
  auto siz = std::make_unique<siz_params>();
  LinkCluster<mct_params>(siz.get(), total_tiles, 0);
  LinkCluster<mcc_params>(siz.get(), total_tiles, 0);
  LinkCluster<mco_params>(siz.get(), total_tiles, 0);
  LinkCluster<atk_params>(siz.get(), total_tiles, 0);
  if (include_enc) {
    LinkCluster<enc_params>(siz.get(), total_tiles, num_comps);
  }
  LinkCluster<cod_params>(siz.get(), total_tiles, num_comps);
  LinkCluster<dfs_params>(siz.get(), 0, 0);
  LinkCluster<ads_params>(siz.get(), total_tiles, 0);
  LinkCluster<qcd_params>(siz.get(), total_tiles, num_comps);
  LinkCluster<rgn_params>(siz.get(), total_tiles, num_comps);
  LinkCluster<poc_params>(siz.get(), total_tiles, 0);
  LinkCluster<org_params>(siz.get(), total_tiles, 0);
  LinkCluster<crg_params>(siz.get(), 0, 0);
  if (nlt_comps > 0) {
    LinkCluster<nlt_params>(siz.get(), total_tiles, nlt_comps);
  }
  return siz;
}

void RoundTripMarkers(siz_params* src, siz_params* dst, int total_tiles) {
  for (int tile = -1; tile < total_tiles; ++tile) {
    for (int tpart = 0; tpart < (tile < 0 ? 1 : 2); ++tpart) {
      int dry_flags = 0;
      const int dry_bytes =
          src->generate_marker_segments(nullptr, tile, tpart, dry_flags);
      VectorOutput out;
      int class_flags = 0;
      const int total_bytes =
          src->generate_marker_segments(&out, tile, tpart, class_flags);
      out.Flush();
      EXPECT_EQ(dry_flags, class_flags);
      EXPECT_EQ(static_cast<int>(out.bytes().size()), dry_bytes);
      int prf_cpf_bytes = 0;
      for (auto& seg : SplitMarkerSegments(out.bytes())) {
        if (seg.code == KDU_PRF || seg.code == KDU_CPF) {
          // siz_params::write_marker_segment(out != nullptr) omits prf_length
          // + cpf_length from its return value at params.cpp:5360, whereas the
          // dry-run path (out == nullptr at params.cpp:5141) includes them.
          prf_cpf_bytes += 4 + static_cast<int>(seg.body.size());
        }
        kdu_byte* body_ptr = seg.body.empty() ? nullptr : seg.body.data();
        const int parsed_flag = dst->translate_marker_segment(
            seg.code, static_cast<int>(seg.body.size()), body_ptr, tile, tpart);
        if (tpart == 0 || (seg.code != KDU_NLT && seg.code != KDU_MCO)) {
          EXPECT_NE(0, parsed_flag)
              << "tile=" << tile << " tpart=" << tpart << " code=0x" << std::hex
              << seg.code << std::dec << " len=" << seg.body.size();
        } else {
          // nlt_params and mco_params write_marker_segment emit NLT/MCO even
          // for tpart > 0, whereas their read_marker_segment rejects tpart > 0.
          EXPECT_EQ(0, parsed_flag);
        }
      }
      EXPECT_EQ(static_cast<int>(out.bytes().size()),
                total_bytes + prf_cpf_bytes);
    }
  }
}

class ParamsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GlobalErrorHandler().Clear();
    GlobalWarningHandler().Clear();
  }
};

TEST_F(ParamsTest, OutputBufferPutFillAndWrite) {
  VectorOutput out;
  std::vector<kdu_byte> ext_buf(16);
  out.set_external_buffer(ext_buf.data(), ext_buf.size());
  EXPECT_EQ(1, out.put(static_cast<kdu_byte>(0xAB)));
  EXPECT_EQ(2, out.put(static_cast<kdu_uint16>(0x1234)));
  EXPECT_EQ(4, out.put(static_cast<kdu_uint32>(0x89ABCDEFU)));
  EXPECT_EQ(4, out.put(1.5F));

  kdu_byte chunk[24];
  std::memset(chunk, 0x5A, sizeof(chunk));
  out.write(chunk, sizeof(chunk));
  out.fill_bytes(20, 0x7E);
  out.fill_pairs(15, 0xCA, 0xFE);
  // Switch back to internal buffer after pending data triggers flush_buf.
  out.put(static_cast<kdu_byte>(0x42));
  out.set_external_buffer(nullptr, 0);
  out.fill_pairs(10, 0xDE, 0xAD);
  out.Flush();

  EXPECT_EQ(1 + 2 + 4 + 4 + 24 + 20 + 30 + 1 + 20, out.bytes().size());
  EXPECT_EQ(0xAB, out.bytes()[0]);
  EXPECT_EQ(0x12, out.bytes()[1]);
  EXPECT_EQ(0x34, out.bytes()[2]);
  EXPECT_EQ(0x89, out.bytes()[3]);
}

TEST_F(ParamsTest, BaseClusterNavigationRelationsInstancesAndDestroy) {
  std::unique_ptr<siz_params> siz =
      BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/3, /*nlt_comps=*/3);

  EXPECT_STREQ("SIZ", siz->identify_cluster());
  EXPECT_EQ(siz.get(), siz->access_cluster(static_cast<const char*>(nullptr)));
  EXPECT_EQ(siz.get(), siz->access_cluster(SIZ_params));
  EXPECT_EQ(siz.get(), siz->access_cluster(0));
  EXPECT_EQ(nullptr, siz->access_cluster("NO_SUCH_CLUSTER"));
  EXPECT_EQ(nullptr, siz->access_cluster(99));

  kdu_params* cod = siz->access_cluster(COD_params);
  ASSERT_NE(cod, nullptr);
  EXPECT_EQ(cod, siz->access_cluster("COD"));
  EXPECT_EQ(2, cod->get_num_tiles());
  EXPECT_EQ(3, cod->get_num_comps());
  EXPECT_EQ(0, cod->get_instance());

  // Check read-only vs mutable relation access and dependency propagation.
  EXPECT_EQ(cod, cod->access_relation(1, 2, 0, /*read_only=*/true));
  EXPECT_EQ(nullptr, cod->access_unique(1, 2, 0));
  EXPECT_EQ(nullptr, cod->access_relation(2, 0, 0, /*read_only=*/false));
  EXPECT_EQ(nullptr, cod->access_relation(0, 3, 0, /*read_only=*/false));
  EXPECT_EQ(nullptr, cod->access_relation(0, 0, 1, /*read_only=*/false));

  kdu_params* cod_c1 = cod->access_relation(-1, 1, 0, /*read_only=*/false);
  kdu_params* cod_t0 = cod->access_relation(0, -1, 0, /*read_only=*/false);
  ASSERT_NE(cod_c1, nullptr);
  ASSERT_NE(cod_t0, nullptr);
  EXPECT_NE(cod, cod_c1);
  EXPECT_NE(cod, cod_t0);
  EXPECT_EQ(cod_c1, cod->access_unique(-1, 1, 0));
  // Unique (0, 1) must exist because both comp-head (-1,1) and tile-head
  // (0,-1) exist.
  EXPECT_NE(nullptr, cod->access_unique(0, 1, 0));

  // QCD depends on COD, so accessing unique COD (1,2) also materializes QCD.
  kdu_params* cod_t1c2 = cod->access_relation(1, 2, 0, /*read_only=*/false);
  ASSERT_NE(cod_t1c2, nullptr);
  kdu_params* qcd = siz->access_cluster(QCD_params);
  ASSERT_NE(qcd, nullptr);
  EXPECT_NE(nullptr, qcd->access_unique(1, 2, 0));

  // Multiple instances: POC allows instances; treat_instances_like_components
  // on ATK / MCT / MCC / ADS.
  kdu_params* poc = siz->access_cluster(POC_params);
  ASSERT_NE(poc, nullptr);
  EXPECT_EQ(nullptr, poc->new_instance());  // Negative tile index disallowed.
  kdu_params* poc_t0 = poc->access_relation(0, -1, 0, /*read_only=*/false);
  ASSERT_NE(poc_t0, nullptr);
  kdu_params* poc_t0_i1 = poc_t0->new_instance();
  ASSERT_NE(poc_t0_i1, nullptr);
  EXPECT_EQ(1, poc_t0_i1->get_instance());
  EXPECT_EQ(poc_t0_i1, poc_t0->access_next_inst());

  kdu_params* atk = siz->access_cluster(ATK_params);
  ASSERT_NE(atk, nullptr);
  kdu_params* atk_i5 = atk->access_relation(-1, -1, 5, /*read_only=*/false);
  ASSERT_NE(atk_i5, nullptr);
  EXPECT_EQ(5, atk_i5->get_instance());
  EXPECT_EQ(atk_i5, atk->access_next_inst());
  EXPECT_EQ(atk_i5, atk->access_relation(-1, -1, 5, /*read_only=*/true));

  // Deleting a specific tile-comp object updates refs to NULL.
  kdu_params* qcd_t1c2 = qcd->access_unique(1, 2, 0);
  ASSERT_NE(qcd_t1c2, nullptr);
  qcd_t1c2->destroy();
  EXPECT_EQ(nullptr, qcd->access_relation(1, 2, 0, /*read_only=*/true));

  // Delete a specific tile head (tile 1) cleanly, then a comp head (-1, 1).
  kdu_params* cod_t1_head = cod->access_unique(1, -1, 0);
  ASSERT_NE(cod_t1_head, nullptr);
  cod_t1_head->destroy();
  EXPECT_EQ(nullptr, cod->access_relation(1, -1, 0, /*read_only=*/true));
  cod->access_unique(-1, 1, 0)->destroy();
  EXPECT_EQ(nullptr, cod->access_relation(-1, 1, 0, /*read_only=*/true));
}

TEST_F(ParamsTest, SetGetCompareParseStringTextualizeAndDescribe) {
  std::unique_ptr<siz_params> siz =
      BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/2, /*nlt_comps=*/2);

  // Use parse_string with brace omission, multi-record, tile/comp specs, and
  // instance spec.
  EXPECT_TRUE(siz->parse_string("Sdims={32,24},{16,12}"));
  EXPECT_TRUE(siz->parse_string("Clayers=2"));
  EXPECT_TRUE(siz->parse_string("Cycc:T1=no"));
  EXPECT_TRUE(siz->parse_string("Clevels:C1=3"));
  EXPECT_TRUE(siz->parse_string("Corder:T1=PCRL"));
  EXPECT_TRUE(siz->parse_string("Cmodes=RESTART|CAUSAL"));
  EXPECT_TRUE(siz->parse_string("Kkernels:I2=I5X3"));
  EXPECT_FALSE(siz->parse_string("UnknownAttr=123"));

  // parse_string(str, tile_idx) filters by tile.
  EXPECT_TRUE(siz->parse_string("Cuse_sop:T0=yes", 0));
  EXPECT_FALSE(siz->parse_string("Cuse_sop:T0=yes", 1));
  EXPECT_FALSE(siz->parse_string("Cuse_sop=yes", 0));
  EXPECT_TRUE(siz->parse_string("Cuse_eph=yes", -1));

  // find_string lookups.
  const char* attr_name = nullptr;
  char q_str[] = "Qstep:T1C1=0.05";
  kdu_params* q_target = siz->find_string(q_str, attr_name);
  ASSERT_NE(q_target, nullptr);
  EXPECT_STREQ("Qstep", attr_name);

  // Set/Get/Compare across int, bool, float, derived, and inheritance flags.
  kdu_params* cod = siz->access_cluster(COD_params);
  kdu_params* cod_c1 = cod->access_relation(-1, 1, 0, false);
  kdu_params* cod_t1c1 = cod->access_relation(1, 1, 0, false);
  cod->set(Clevels, 0, 0, 5);  // Clear/overwrite parsed? Only allowed on set.
  cod->set_derived(Clevels);
  int ival = 0;
  bool bval = false;
  float fval = 0.0F;
  EXPECT_TRUE(cod_c1->get(Clevels, 0, 0, ival, /*allow_inherit=*/true, true,
                          /*allow_derived=*/true));
  EXPECT_EQ(3, ival);
  EXPECT_FALSE(cod_t1c1->get(Clevels, 0, 0, ival, /*allow_inherit=*/false));
  EXPECT_TRUE(cod->compare(Clevels, 0, 0, 5));
  EXPECT_TRUE(cod->compare(Cuse_eph, 0, 0, true));

  kdu_params* crg = siz->access_cluster(CRG_params);
  crg->set(CRGoffset, 0, 0, 0.25);
  crg->set(CRGoffset, 0, 1, 0.75);
  crg->set_derived(CRGoffset);
  // preserve_derived keeps derived mark intact.
  crg->set(CRGoffset, 0, 0, 0.50, /*preserve_derived=*/true);
  EXPECT_TRUE(crg->get(CRGoffset, 0, 0, fval));
  EXPECT_NEAR(0.50F, fval, 1e-6F);
  EXPECT_TRUE(crg->compare(CRGoffset, 0, 1, 0.75F));
  // Extrapolation on CRGoffset (CAN_EXTRAPOLATE).
  EXPECT_TRUE(crg->get(CRGoffset, 2, 0, fval, true, /*allow_extend=*/true));
  EXPECT_NEAR(0.50F, fval, 1e-6F);
  EXPECT_FALSE(crg->get(CRGoffset, 2, 0, fval, true, /*allow_extend=*/false));

  EXPECT_EQ(2, siz->count_records(Sdims));
  EXPECT_TRUE(siz->truncate_records(Sdims, 1));
  EXPECT_EQ(1, siz->count_records(Sdims));
  EXPECT_FALSE(siz->truncate_records(Sdims, 1));

  // delete_unparsed_attribute keeps parsed records intact, deletes unparsed.
  cod->set(Cprecincts, 0, 0, 64);
  cod->set(Cprecincts, 0, 1, 64);
  cod->delete_unparsed_attribute(Cprecincts);
  EXPECT_EQ(0, cod->count_records(Cprecincts));
  cod->delete_unparsed_attribute(Clayers);  // Clayers was parsed.
  EXPECT_EQ(1, cod->count_records(Clayers));

  // Textualization & description (with & without comments, skip_derived).
  StringMessage txt_skip;
  siz->textualize_attributes(txt_skip, /*skip_derived=*/true);
  EXPECT_EQ(std::string::npos, txt_skip.text().find("Clevels=5"));
  StringMessage txt_all;
  siz->textualize_attributes(txt_all, /*min_tile=*/-1, /*max_tile=*/1,
                             /*skip_derived=*/false);
  EXPECT_NE(std::string::npos, txt_all.text().find("Clayers=2"));

  StringMessage desc_all;
  siz->describe_attributes(desc_all, /*include_comments=*/true);
  EXPECT_NE(std::string::npos, desc_all.text().find("Sdims"));
  StringMessage desc_one;
  cod->describe_attribute(Corder, desc_one, /*include_comments=*/false);
  EXPECT_NE(std::string::npos, desc_one.text().find("Corder"));

  // Verify error handling on invalid accesses (must catch kdu_exception).
  GlobalErrorHandler().set_quiet(true);
  EXPECT_THROW(siz->parse_string("Corder:T1:C0=PCRL"), kdu_exception);
  EXPECT_THROW(cod->get("NoSuchAttr", 0, 0, ival), kdu_exception);
  EXPECT_THROW(cod->set("NoSuchAttr", 0, 0, 1), kdu_exception);
  EXPECT_THROW(cod->set(Clevels, 0, 0, 1.5), kdu_exception);
  EXPECT_THROW(cod->set(Cycc, 0, 0, 2), kdu_exception);
  EXPECT_THROW(crg->set(CRGoffset, 0, 0, 1), kdu_exception);
  EXPECT_THROW(siz->parse_string("Clayers= 2"), kdu_exception);
  EXPECT_THROW(siz->parse_string("Clayers=2"), kdu_exception);  // Parsed twice.
  EXPECT_THROW(siz->describe_attribute("BadAttr", desc_one), kdu_exception);
  EXPECT_THROW(cod_c1->set(Corder, 0, 0, Corder_LRCP), kdu_exception);
  EXPECT_THROW(cod->get(Cycc, 0, 0, fval), kdu_exception);
  EXPECT_THROW(cod->get(CRGoffset, 0, 0, bval), kdu_exception);
  GlobalErrorHandler().set_quiet(false);
}

TEST_F(ParamsTest, SizProfilesCapabilitiesAndMarkerRoundTrip) {
  // Part-2 codestream with non-zero origin, CBD (Mcomponents), and NLT markers.
  std::unique_ptr<siz_params> src =
      BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/3);
  ASSERT_TRUE(src->parse_string("Scomponents=3"));
  ASSERT_TRUE(src->parse_string("Sdims={128,96},{64,48},{64,48}"));
  ASSERT_TRUE(src->parse_string("Sorigin={4,2}"));
  ASSERT_TRUE(src->parse_string("Stiles={128,96}"));
  ASSERT_TRUE(src->parse_string("Stile_origin={0,0}"));
  ASSERT_TRUE(src->parse_string("Sprecision=10"));
  ASSERT_TRUE(src->parse_string("Ssigned=yes,no,no"));
  ASSERT_TRUE(src->parse_string("Sprofile=PART2"));
  ASSERT_TRUE(src->parse_string("Sextensions=MCT|NLT"));
  ASSERT_TRUE(src->parse_string("Mcomponents=3"));
  ASSERT_TRUE(src->parse_string("Mprecision=10"));
  ASSERT_TRUE(src->parse_string("Msigned=yes,yes,no"));
  ASSERT_TRUE(src->parse_string("Ncomponents=3"));
  ASSERT_TRUE(src->parse_string("Nprecision=10"));
  ASSERT_TRUE(src->parse_string("Nsigned=yes,yes,no"));
  src->finalize_all(/*after_reading=*/false);

  int conf_profile = 0;
  EXPECT_TRUE(src->get(Sconf_profile, 0, 0, conf_profile));

  // Write markers out (SIZ, CAP, PRF, CBD, etc.) and parse into empty dst.
  std::unique_ptr<siz_params> dst =
      BuildFullCluster(/*total_tiles=*/0, /*num_comps=*/0, /*nlt_comps=*/0);
  VectorOutput out;
  int flags = 0;
  const int nbytes = src->generate_marker_segments(&out, -1, 0, flags);
  out.Flush();
  ASSERT_GT(nbytes, 0);
  auto segs = SplitMarkerSegments(out.bytes());
  ASSERT_FALSE(segs.empty());
  // First translate SIZ so we know num_comps for a full cluster.
  EXPECT_EQ(KDU_ParamsClass_FIXED,
            dst->translate_marker_segment(segs[0].code,
                                          static_cast<int>(segs[0].body.size()),
                                          segs[0].body.data(), -1, 0));
  int comps = 0;
  ASSERT_TRUE(dst->get(Scomponents, 0, 0, comps));
  EXPECT_EQ(3, comps);

  // Also test IMF + Scpf_profile + Seprofile=IMFHT on a compliant single-tile
  // unsigned 10-bit 4:4:4 codestream.
  std::unique_ptr<siz_params> imf =
      BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/3);
  ASSERT_TRUE(imf->parse_string("Scomponents=3"));
  ASSERT_TRUE(imf->parse_string("Sdims={128,128},{128,128},{128,128}"));
  ASSERT_TRUE(imf->parse_string("Sprecision=10"));
  ASSERT_TRUE(imf->parse_string("Ssigned=no"));
  ASSERT_TRUE(imf->parse_string("Corder=PCRL"));
  ASSERT_TRUE(imf->parse_string("Sprofile=IMF"));
  ASSERT_TRUE(imf->parse_string("Simf={3,1,rev}"));
  ASSERT_TRUE(imf->parse_string("Seprofile=IMFHT"));
  imf->finalize_all(/*after_reading=*/false);
  EXPECT_TRUE(imf->get(Sconf_profile, 0, 0, conf_profile));
  EXPECT_EQ(Sprofile_IMF, conf_profile);
}

TEST_F(ParamsTest, FullCodestreamClusterRoundTripAndXforms) {
  std::unique_ptr<siz_params> src =
      BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/3, /*nlt_comps=*/3);
  ASSERT_TRUE(src->parse_string("Scomponents=3"));
  ASSERT_TRUE(src->parse_string("Sdims={128,128},{128,128},{128,128}"));
  ASSERT_TRUE(src->parse_string("Stiles={64,128}"));
  ASSERT_TRUE(src->parse_string("Sprecision=8"));
  ASSERT_TRUE(src->parse_string("Ssigned=no"));
  ASSERT_TRUE(src->parse_string("Clayers=2"));
  ASSERT_TRUE(src->parse_string("Corder=RPCL"));
  ASSERT_TRUE(src->parse_string("Clevels=3"));
  ASSERT_TRUE(src->parse_string("Cprecincts={64,64},{32,32}"));
  ASSERT_TRUE(src->parse_string("Cblk={32,32}"));
  ASSERT_TRUE(src->parse_string("Creversible=no"));
  ASSERT_TRUE(src->parse_string("Cycc=yes"));
  ASSERT_TRUE(src->parse_string("Cuse_sop=yes"));
  ASSERT_TRUE(src->parse_string("Cuse_eph=yes"));
  ASSERT_TRUE(src->parse_string("Calign_blk_last={yes,no}"));
  ASSERT_TRUE(
      src->parse_string("Cmodes=BYPASS|RESET|RESTART|CAUSAL|ERTERM|SEGMARK"));
  ASSERT_TRUE(src->parse_string("Ctype=Y,Cb,Cr"));
  ASSERT_TRUE(src->parse_string("Qfactor=75"));
  ASSERT_TRUE(src->parse_string("Qguard=2"));
  ASSERT_TRUE(src->parse_string("Qderived=no"));
  ASSERT_TRUE(src->parse_string("Rshift:T0C1=4"));
  ASSERT_TRUE(src->parse_string("Porder:T0={0,0,2,3,3,LRCP},{0,0,2,4,3,CPRL}"));
  ASSERT_TRUE(src->parse_string("CRGoffset={0.25,0.5},{0.125,0.75},{0.0,0.0}"));
  ASSERT_TRUE(src->parse_string("ORGgen_tlm=4"));
  ASSERT_TRUE(src->parse_string("ORGgen_plt=yes"));
  ASSERT_TRUE(src->parse_string("ORGtparts=R|L"));
  ASSERT_TRUE(src->parse_string("ORGtlm_style={short,long}"));
  ASSERT_TRUE(src->parse_string("Mcomponents=3"));
  ASSERT_TRUE(src->parse_string("Mprecision=8"));
  ASSERT_TRUE(src->parse_string("Msigned=no"));
  ASSERT_TRUE(src->parse_string("Mmatrix_size:I1=9"));
  ASSERT_TRUE(src->parse_string("Mmatrix_coeffs:I1=1,0,0,0,1,0,0,0,1"));
  ASSERT_TRUE(src->parse_string("Mvector_size:I1=3"));
  ASSERT_TRUE(src->parse_string("Mvector_coeffs:I1=0.5,1.0,1.5"));
  ASSERT_TRUE(src->parse_string("Mstage_inputs:I1={0,2}"));
  ASSERT_TRUE(src->parse_string("Mstage_outputs:I1={0,2}"));
  ASSERT_TRUE(src->parse_string("Mstage_collections:I1={3,3}"));
  ASSERT_TRUE(src->parse_string("Mstage_xforms:I1={MATRIX,1,1,0,0}"));
  ASSERT_TRUE(src->parse_string("Mnum_stages=1"));
  ASSERT_TRUE(src->parse_string("Mstages=1"));
  ASSERT_TRUE(src->parse_string("Ncomponents=3"));
  ASSERT_TRUE(src->parse_string("Nprecision=8"));
  ASSERT_TRUE(src->parse_string("Nsigned=no"));
  ASSERT_TRUE(src->parse_string("NLTmake:C0={GAMMA,2.4,0.055,64}"));
  ASSERT_TRUE(src->parse_string("NLType:C1=UMAG"));
  ASSERT_TRUE(src->parse_string("NLTmake:C2={GAMMA,1.8,0.0,32}"));

  src->finalize_all(/*after_reading=*/false);
  EXPECT_TRUE(src->any_changes());
  EXPECT_TRUE(src->check_typical_tile(1));
  EXPECT_FALSE(src->check_typical_tile(0));
  EXPECT_TRUE(src->check_typical_tile(0, "RGN:POC"));

  // Roundtrip generated marker segments into a fresh target cluster.
  std::unique_ptr<siz_params> dst =
      BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/3, /*nlt_comps=*/3,
                       /*include_enc=*/false);
  RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/2);
  dst->finalize_all(/*after_reading=*/true);

  int layers = 0;
  ASSERT_TRUE(dst->access_cluster(COD_params)->get(Clayers, 0, 0, layers));
  EXPECT_EQ(2, layers);

  // Exercise copy_all with transpose + discard_levels + skip_components.
  std::unique_ptr<siz_params> xform =
      BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/3, /*nlt_comps=*/3);
  xform->copy_all(src.get(), /*skip_components=*/0, /*discard_levels=*/1,
                  /*transpose=*/true, /*vflip=*/false, /*hflip=*/false);
  xform->finalize_all();

  int xs = 0, ys = 0;
  ASSERT_TRUE(xform->get(Sdims, 0, 0, xs));
  ASSERT_TRUE(xform->get(Sdims, 0, 1, ys));
  EXPECT_EQ(64, xs);
  EXPECT_EQ(64, ys);

  // Flipped copy_with_xforms produces negative tile origins guarded by
  // b/341121728.
  std::unique_ptr<siz_params> xform_flip =
      BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/3, /*nlt_comps=*/3);
  xform_flip->copy_all(src.get(), /*skip_components=*/0, /*discard_levels=*/1,
                       /*transpose=*/true, /*vflip=*/true, /*hflip=*/true);
  GlobalErrorHandler().set_quiet(true);
  EXPECT_THROW(xform_flip->finalize_all(), kdu_exception);
  GlobalErrorHandler().set_quiet(false);

  // Also test clear_marks(false), any_changes(), clear_marks(true), and
  // per-tile restart finalization.
  dst->clear_marks(/*for_reading=*/true);
  RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/2);
  dst->finalize_all(-1, /*after_reading=*/true);
  dst->finalize_all(0, /*after_reading=*/true);
  src->clear_marks(/*for_reading=*/false);
  EXPECT_FALSE(src->any_changes());
}

TEST_F(ParamsTest, ExtendedWaveletDfsAdsAtkAndNltHelpers) {
  // Static COD decomposition utilities.
  EXPECT_TRUE(cod_params::is_valid_decomp_terminator(0x03));  // B(-:-:-)
  kdu_int16 bands[49];
  const int count = cod_params::expand_decomp_bands(0x03, bands);
  EXPECT_EQ(4, count);
  int mh = 0, mv = 0;
  cod_params::get_max_decomp_levels(0x03, mh, mv);
  EXPECT_EQ(1, mh);
  EXPECT_EQ(1, mv);
  char dbuf[32];
  cod_params::textualize_decomp(dbuf, cod_params::transpose_decomp(0x01));
  EXPECT_STREQ("V(-)", dbuf);

  // ATK static & custom kernels + DFS + ADS marker roundtrip.
  EXPECT_FALSE(atk_params::is_predef_kernel_symmetric(Kkernels_I1X1));
  EXPECT_FALSE(atk_params::is_predef_kernel_symmetric(Kkernels_I2X2));
  EXPECT_TRUE(atk_params::is_predef_kernel_symmetric(Kkernels_I5X3));

  std::unique_ptr<siz_params> src =
      BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/2, /*nlt_comps=*/2);
  ASSERT_TRUE(src->parse_string("Scomponents=2"));
  ASSERT_TRUE(src->parse_string("Sdims={128,128},{128,128}"));
  ASSERT_TRUE(src->parse_string("Sprecision=8"));
  ASSERT_TRUE(src->parse_string("Ssigned=no"));
  ASSERT_TRUE(src->parse_string("Clevels=3"));
  ASSERT_TRUE(src->parse_string("Ckernels:C0=ATK"));
  ASSERT_TRUE(src->parse_string("Catk:C0=2"));
  ASSERT_TRUE(src->parse_string("Kkernels:I2=I5X3"));
  ASSERT_TRUE(src->parse_string("Cdecomp:C0=B(-:-:-),V(HH-),H(B)"));
  ASSERT_TRUE(src->parse_string("Ckernels:C1=ATK"));
  ASSERT_TRUE(src->parse_string("Catk:C1=3"));
  ASSERT_TRUE(src->parse_string("Creversible:C1=no"));
  ASSERT_TRUE(src->parse_string("Kreversible:I3=no"));
  ASSERT_TRUE(src->parse_string("Ksymmetric:I3=no"));
  ASSERT_TRUE(src->parse_string("Kextension:I3=CON"));
  ASSERT_TRUE(src->parse_string("Ksteps:I3={1,0,0,0},{1,-1,0,0}"));
  ASSERT_TRUE(src->parse_string("Kcoeffs:I3=-0.5,0.25"));
  ASSERT_TRUE(src->parse_string("DSdfs:I1=B,V,H"));
  src->finalize_all();

  std::unique_ptr<siz_params> dst =
      BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/2, /*nlt_comps=*/2,
                       /*include_enc=*/false);
  RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/1);
  dst->finalize_all(/*after_reading=*/true);

  // NLT gamma static helpers.
  float gamma[5] = {};
  nlt_params::make_gamma_params(2.4F, 0.055F, gamma);
  float lut[4] = {0.0F, 0.01F, 0.5F, 1.0F};
  nlt_params::apply_fwd_gamma(lut, 4, gamma);
  nlt_params::apply_rev_gamma(lut, 4, gamma);
  EXPECT_NEAR(0.0F, lut[0], 1e-4F);
  EXPECT_NEAR(0.01F, lut[1], 1e-4F);
  EXPECT_NEAR(0.5F, lut[2], 1e-4F);
  EXPECT_NEAR(1.0F, lut[3], 1e-4F);
}

struct ScopedQuietErrors {
  ScopedQuietErrors() { GlobalErrorHandler().set_quiet(true); }
  ~ScopedQuietErrors() { GlobalErrorHandler().set_quiet(false); }
  ScopedQuietErrors(const ScopedQuietErrors&) = delete;
  ScopedQuietErrors& operator=(const ScopedQuietErrors&) = delete;
};

TEST_F(ParamsTest, QcdFix16DerivedWeightedAndPart15MagB) {
  try {
    // Case 1: Qfix16=FREE with explicit Qweights, Cweight, Clev_weights,
    // Cband_weights and Qstep.
    {
      std::unique_ptr<siz_params> src =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
      ASSERT_TRUE(src->parse_string("Sdims={128,128}"));
      ASSERT_TRUE(src->parse_string("Scomponents=3"));
      ASSERT_TRUE(src->parse_string("Sprecision=8"));
      ASSERT_TRUE(src->parse_string("Ssigned=no"));
      ASSERT_TRUE(src->parse_string("Cycc=yes"));
      ASSERT_TRUE(src->parse_string("Clevels=3"));
      ASSERT_TRUE(src->parse_string("Ckernels=W9X7"));
      ASSERT_TRUE(src->parse_string("Qstep=0.012"));
      ASSERT_TRUE(src->parse_string("Qweights=1.7321,1.8051,1.5734"));
      ASSERT_TRUE(src->parse_string("Cweight:C0=1.2"));
      ASSERT_TRUE(src->parse_string("Clev_weights:C0=1.0,0.8,0.6"));
      ASSERT_TRUE(
          src->parse_string("Cband_weights:C0=0.5,0.4,0.3,0.6,0.5,0.4"));
      ASSERT_TRUE(src->parse_string("Qfix16=FREE"));
      src->finalize_all();

      std::unique_ptr<siz_params> dst =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
      RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/1);
      float qabs = 0.0F;
      EXPECT_TRUE(dst->access_cluster(QCD_params)->get(Qabs_steps, 0, 0, qabs));
      EXPECT_GT(qabs, 0.0F);
    }

    // Case 2: Qfix16=LIMIT forcing k_max = guard_bits + eps > 15 clamp,
    // paired with HT modes so adjust_absolute_steps_finding_magB runs.
    {
      std::unique_ptr<siz_params> src =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(src->parse_string("Sdims={128,128}"));
      ASSERT_TRUE(src->parse_string("Scomponents=1"));
      ASSERT_TRUE(src->parse_string("Sprecision=12"));
      ASSERT_TRUE(src->parse_string("Ssigned=no"));
      ASSERT_TRUE(src->parse_string("Scap=P15"));
      ASSERT_TRUE(src->parse_string("SCP15_caps=MIX"));
      ASSERT_TRUE(src->parse_string("Cmodes=HT"));
      ASSERT_TRUE(src->parse_string("Clevels=3"));
      ASSERT_TRUE(src->parse_string("Qguard=6"));
      ASSERT_TRUE(src->parse_string("Qstep=0.0001"));
      ASSERT_TRUE(src->parse_string("Qfix16=LIMIT"));
      src->finalize_all();

      int magb = 0;
      EXPECT_TRUE(src->get(SCP15_magb, 0, 0, magb));
      EXPECT_GT(magb, 0);

      std::unique_ptr<siz_params> dst =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/1);
    }

    // Case 3: Qfactor across subsampled/interlaced Ctypes and low/mid/high Q,
    // including Qfactor + Qfix16 warning branch.
    for (float qfactor : {20.0F, 85.0F, 100.0F}) {
      std::unique_ptr<siz_params> src =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/4, /*nlt_comps=*/0);
      ASSERT_TRUE(src->parse_string("Ssize={128,128}"));
      ASSERT_TRUE(src->parse_string("Scomponents=4"));
      ASSERT_TRUE(src->parse_string("Ssampling={1,1},{2,1},{1,2},{2,2}"));
      ASSERT_TRUE(src->parse_string("Sprecision=10"));
      ASSERT_TRUE(src->parse_string("Ssigned=no"));
      ASSERT_TRUE(src->parse_string("Cycc=no"));
      ASSERT_TRUE(src->parse_string("Ctype=IY,ICb,ICr,N"));
      ASSERT_TRUE(src->parse_string("Clevels=4"));
      if (qfactor == 85.0F) {
        GlobalWarningHandler().Clear();
        ASSERT_TRUE(src->parse_string("Qfix16=FREE"));
      }
      std::string qf_str = "Qfactor=" + std::to_string(qfactor);
      ASSERT_TRUE(src->parse_string(qf_str.c_str()));
      src->finalize_all();
      if (qfactor == 85.0F) {
        EXPECT_FALSE(GlobalWarningHandler().text().empty());
      }
      float derived_qstep = 0.0F;
      EXPECT_TRUE(
          src->access_cluster(ENC_params)->get(Qstep, 0, 0, derived_qstep));
      EXPECT_GT(derived_qstep, 0.0F);
    }

    // Case 4: Qderived=yes with Part-15 HT and Part-2 asymmetric H/V Cdecomp to
    // cover find_derived_step_magB across both mu thresholds and odd N_L.
    for (const char* qstep_str : {"Qstep=0.01", "Qstep=0.015"}) {
      std::unique_ptr<siz_params> src =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(src->parse_string("Sdims={128,128}"));
      ASSERT_TRUE(src->parse_string("Scomponents=1"));
      ASSERT_TRUE(src->parse_string("Sprecision=8"));
      ASSERT_TRUE(src->parse_string("Ssigned=no"));
      ASSERT_TRUE(src->parse_string("Sprofile=PART2"));
      ASSERT_TRUE(src->parse_string("Scap=P2|P15"));
      ASSERT_TRUE(src->parse_string("Cmodes=HT"));
      ASSERT_TRUE(src->parse_string("Clevels=3"));
      ASSERT_TRUE(src->parse_string("Cdecomp=H(-),V(-),B(-:-:-)"));
      ASSERT_TRUE(src->parse_string("Qderived=yes"));
      ASSERT_TRUE(src->parse_string(qstep_str));
      src->finalize_all();

      std::unique_ptr<siz_params> dst =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/1);
      bool is_derived = false;
      EXPECT_TRUE(
          dst->access_cluster(QCD_params)->get(Qderived, 0, 0, is_derived));
      EXPECT_TRUE(is_derived);
    }

    // Case 5: Reversible heterogeneous component precisions triggering QCC
    // generation and guard-bit range checks + explicit Cband_weights +
    // Qfparams.
    {
      std::unique_ptr<siz_params> src =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
      ASSERT_TRUE(src->parse_string("Sdims={128,128}"));
      ASSERT_TRUE(src->parse_string("Scomponents=3"));
      ASSERT_TRUE(src->parse_string("Sprecision=16,12,8"));
      ASSERT_TRUE(src->parse_string("Ssigned=no"));
      ASSERT_TRUE(src->parse_string("Cycc=no"));
      ASSERT_TRUE(src->parse_string("Creversible=yes"));
      ASSERT_TRUE(src->parse_string("Ckernels=W5X3"));
      ASSERT_TRUE(src->parse_string("Clevels=5"));
      ASSERT_TRUE(src->parse_string("Qguard=1"));
      src->finalize_all();

      std::unique_ptr<siz_params> dst =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
      RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/1);
      int range0 = 0;
      EXPECT_TRUE(
          dst->access_cluster(QCD_params)->get(Qabs_ranges, 0, 0, range0));
      EXPECT_GT(range0, 0);
    }

    // Case 6: Error paths in enc_params::finalize and qcd_params markers.
    {
      ScopedQuietErrors quiet;
      auto make_err_cluster = []() {
        auto c = BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/2,
                                  /*nlt_comps=*/0);
        c->parse_string("Sdims={64,64}");
        c->parse_string("Scomponents=2");
        c->parse_string("Sprecision=8");
        c->parse_string("Ssigned=no");
        return c;
      };

      {
        auto c = make_err_cluster();
        ASSERT_TRUE(c->parse_string("Qfactor=0"));
        EXPECT_THROW(c->finalize_all(), kdu_exception);
      }
      {
        auto c = make_err_cluster();
        ASSERT_TRUE(c->parse_string("Qfactor=80"));
        ASSERT_TRUE(c->parse_string("Qstep=0.05"));
        EXPECT_THROW(c->finalize_all(), kdu_exception);
      }
      {
        auto c = make_err_cluster();
        ASSERT_TRUE(c->parse_string("Ctype=Cb,Cr"));
        EXPECT_THROW(c->finalize_all(), kdu_exception);
      }
      {
        auto c = make_err_cluster();
        ASSERT_TRUE(c->parse_string("Qweights=1.0,-1.0"));
        EXPECT_THROW(c->finalize_all(), kdu_exception);
      }
      {
        auto c = make_err_cluster();
        ASSERT_TRUE(c->parse_string("Qfparams={0.04,80,0.5,75}"));
        EXPECT_THROW(c->finalize_all(), kdu_exception);
      }
      {
        auto c = make_err_cluster();
        ASSERT_TRUE(c->parse_string("Qguard=9"));
        c->finalize_all();
        VectorOutput out;
        EXPECT_THROW(c->generate_marker_segments(&out, -1, 0), kdu_exception);
      }
      {
        auto c = make_err_cluster();
        c->finalize_all();
        kdu_params* qcd = c->access_cluster(QCD_params);
        // Undefined style byte (3) in QCD marker segment.
        kdu_byte bad_style[4] = {0x43, 0x08, 0x00, 0x00};
        EXPECT_THROW(
            qcd->translate_marker_segment(KDU_QCD, 4, bad_style, -1, 0),
            kdu_exception);
        // Too small QCD marker segment (0 payload bytes).
        EXPECT_THROW(
            qcd->translate_marker_segment(KDU_QCD, 0, bad_style, -1, 0),
            kdu_exception);
        // Unconsumed trailing byte in irreversible QCD marker segment.
        kdu_byte extra_byte[4] = {0x42, 0x40, 0x00, 0xFF};
        EXPECT_THROW(
            qcd->translate_marker_segment(KDU_QCD, 4, extra_byte, -1, 0),
            kdu_exception);
      }
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

TEST_F(ParamsTest, NltLutLogIgammaSmagAndMarkerRoundTrip) {
  try {
    // Multi-component NLT setup covering SMAG, UMAG, explicit GAMMA, NLTmake
    // LOG (16-bit LUT), and explicit 32-bit LUT (b/339492431), plus tile
    // overrides.
    {
      std::unique_ptr<siz_params> src =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/5, /*nlt_comps=*/5);
      ASSERT_TRUE(src->parse_string("Ssize={64,64}"));
      ASSERT_TRUE(src->parse_string("Ssampling={1,1}"));
      ASSERT_TRUE(src->parse_string("Stiles={32,64}"));
      ASSERT_TRUE(src->parse_string("Scomponents=5"));
      ASSERT_TRUE(src->parse_string("Sprecision=10,8,12,16,8"));
      ASSERT_TRUE(src->parse_string("Ssigned=yes,no,no,no,no"));
      ASSERT_TRUE(src->parse_string("Cycc=no"));
      ASSERT_TRUE(src->parse_string("Sprofile=PART2"));
      ASSERT_TRUE(src->parse_string("Sextensions=NLT"));
      // Comp 0: SMAG globally, overridden to NONE in T0 and matching SMAG in
      // T1.
      ASSERT_TRUE(src->parse_string("NLType:C0=SMAG"));
      ASSERT_TRUE(src->parse_string("NLType:T0C0=NONE"));
      ASSERT_TRUE(src->parse_string("NLType:T1C0=SMAG"));
      // Comp 1: UMAG on unsigned samples.
      ASSERT_TRUE(src->parse_string("NLType:C1=UMAG"));
      // Comp 2: Explicit GAMMA.
      ASSERT_TRUE(src->parse_string("NLType:C2=GAMMA"));
      ASSERT_TRUE(
          src->parse_string("NLTgamma:C2={2.4,12.92,0.04045,1.055,0.055}"));
      // Comp 3: NLTmake LOG -> builds LUT.
      ASSERT_TRUE(src->parse_string("NLTmake:C3={LOG,0.1,0.0,16}"));
      // Comp 4: Explicit 31-bit LUT for write+read round-trip (avoids
      // params.cpp:9558 upstream shift-by-32 UBSan bug on write when lut_P=32).
      ASSERT_TRUE(src->parse_string("NLType:C4=LUT"));
      ASSERT_TRUE(src->parse_string("NLTlut:C4={0.0,1.0,4,31}"));
      ASSERT_TRUE(src->parse_string("NLTdata:C4=0.0,0.25,0.75,1.0"));
      src->finalize_all();

      std::unique_ptr<siz_params> dst =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/5, /*nlt_comps=*/5);
      RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/2);

      // Feed raw 32-bit KDU_NLT marker segment directly to verify b/339492431
      // read path with lut_P=32 without invoking write_marker_segment.
      kdu_byte nlt32[31] = {0x00, 0x04, 0x07, 0x02, 0x00, 0x03, 0x00, 0x00,
                            0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 32,   0x00,
                            0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0,
                            0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
      EXPECT_EQ(dst->translate_marker_segment(KDU_NLT, 31, nlt32, -1, 0),
                KDU_ParamsClass_CODING);

      int nl0 = -1, nl1 = -1, nl2 = -1, nl3 = -1, nl4 = -1;
      kdu_params* dst_nlt = dst->access_cluster(NLT_params);
      ASSERT_TRUE(dst_nlt->access_relation(-1, 0)->get(NLType, 0, 0, nl0));
      ASSERT_TRUE(dst_nlt->access_relation(-1, 1)->get(NLType, 0, 0, nl1));
      ASSERT_TRUE(dst_nlt->access_relation(-1, 2)->get(NLType, 0, 0, nl2));
      ASSERT_TRUE(dst_nlt->access_relation(-1, 3)->get(NLType, 0, 0, nl3));
      ASSERT_TRUE(dst_nlt->access_relation(-1, 4)->get(NLType, 0, 0, nl4));
      EXPECT_EQ(NLType_SMAG, nl0);
      EXPECT_EQ(NLType_UMAG, nl1);
      EXPECT_EQ(NLType_GAMMA, nl2);
      EXPECT_EQ(NLType_LUT, nl3);
      EXPECT_EQ(NLType_LUT, nl4);

      // copy_all with NLType_LUT currently hits params.cpp:9381 bug where
      // nlt_params::copy_with_xforms reads NLTdata from `this` instead of
      // source, so finalize_all() rejects missing NLTdata.
      ScopedQuietErrors quiet;
      std::unique_ptr<siz_params> copied =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/5, /*nlt_comps=*/5);
      copied->copy_all(src.get());
      EXPECT_THROW(copied->finalize_all(), kdu_exception);
    }

    // Additional LUT widths (P=8, P=12) and NLTmake variants (GAMMA+IGAMMA,
    // ILOG, single NLTmake GAMMA with N=0).
    for (const char* spec : {
             "NLType=LUT NLTlut={0.1,0.9,3,8} NLTdata=0.0,0.5,1.0",
             "NLType=LUT NLTlut={0.05,0.95,3,12} NLTdata=0.1,0.5,0.9",
             "NLTmake={GAMMA,2.4,0.055,32},{IGAMMA,2.4,0.055,32}",
             "NLTmake={ILOG,0.1,0.0,16}",
             "NLTmake={GAMMA,2.2,0.099,0}",
         }) {
      std::unique_ptr<siz_params> src =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/1);
      ASSERT_TRUE(src->parse_string("Sdims={64,64}"));
      ASSERT_TRUE(src->parse_string("Scomponents=1"));
      ASSERT_TRUE(src->parse_string("Sprecision=8"));
      ASSERT_TRUE(src->parse_string("Ssigned=no"));
      ASSERT_TRUE(src->parse_string("Sprofile=PART2"));
      ASSERT_TRUE(src->parse_string("Sextensions=NLT"));
      std::string full_spec(spec);
      size_t sp = full_spec.find(' ');
      if (sp == std::string::npos) {
        ASSERT_TRUE(src->parse_string(full_spec.c_str()));
      } else {
        ASSERT_TRUE(src->parse_string(full_spec.substr(0, sp).c_str()));
        std::string rest = full_spec.substr(sp + 1);
        size_t sp2 = rest.find(' ');
        if (sp2 == std::string::npos) {
          ASSERT_TRUE(src->parse_string(rest.c_str()));
        } else {
          ASSERT_TRUE(src->parse_string(rest.substr(0, sp2).c_str()));
          ASSERT_TRUE(src->parse_string(rest.substr(sp2 + 1).c_str()));
        }
      }
      src->finalize_all();

      std::unique_ptr<siz_params> dst =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/1);
      RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/1);
    }

    // Trigger internal apply_fwd_log / apply_rev_log (including Bval clamping
    // and signed positive/negative linear & log branches) via NLTmake on signed
    // and unsigned components.
    for (bool is_signed : {false, true}) {
      for (const char* make_spec : {
               "NLTmake={LOG,0.0,0.0,16}",
               "NLTmake={LOG,2.0,0.0,16}",
               "NLTmake={ILOG,0.0,0.0,16}",
               "NLTmake={ILOG,2.0,0.0,16}",
           }) {
        std::unique_ptr<siz_params> src = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/1);
        ASSERT_TRUE(src->parse_string("Sdims={32,32}"));
        ASSERT_TRUE(src->parse_string("Scomponents=1"));
        ASSERT_TRUE(src->parse_string("Sprecision=8"));
        ASSERT_TRUE(
            src->parse_string(is_signed ? "Ssigned=yes" : "Ssigned=no"));
        ASSERT_TRUE(src->parse_string("Sprofile=PART2"));
        ASSERT_TRUE(src->parse_string("Sextensions=NLT"));
        ASSERT_TRUE(src->parse_string(make_spec));
        src->finalize_all();
      }
    }
    {
      float gamma[5] = {};
      nlt_params::make_gamma_params(0.5F, 300.0F, gamma);
      nlt_params::make_gamma_params(2.4F, 0.055F, gamma);
      float signed_lut[4] = {-1.0F, -0.01F, 0.01F, 1.0F};
      nlt_params::apply_fwd_gamma(signed_lut, 4, gamma);
      nlt_params::apply_rev_gamma(signed_lut, 4, gamma);
      EXPECT_NEAR(-1.0F, signed_lut[0], 1e-4F);
      EXPECT_NEAR(1.0F, signed_lut[3], 1e-4F);
    }

    // NLT warning and error branches.
    {
      ScopedQuietErrors quiet;
      std::unique_ptr<siz_params> c =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/1);
      ASSERT_TRUE(c->parse_string("Sdims={32,32}"));
      ASSERT_TRUE(c->parse_string("Scomponents=1"));
      ASSERT_TRUE(c->parse_string("Sprecision=8"));
      ASSERT_TRUE(c->parse_string("Ssigned=no"));
      ASSERT_TRUE(c->parse_string("NLType=SMAG"));
      EXPECT_THROW(c->finalize_all(), kdu_exception);

      c = BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/1);
      ASSERT_TRUE(c->parse_string("Sdims={32,32}"));
      ASSERT_TRUE(c->parse_string("Scomponents=1"));
      ASSERT_TRUE(c->parse_string("Sprecision=8"));
      ASSERT_TRUE(c->parse_string("Ssigned=yes"));
      ASSERT_TRUE(c->parse_string("NLType=UMAG"));
      EXPECT_THROW(c->finalize_all(), kdu_exception);

      c = BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/1);
      ASSERT_TRUE(c->parse_string("Sdims={32,32}"));
      ASSERT_TRUE(c->parse_string("Scomponents=1"));
      ASSERT_TRUE(c->parse_string("Sprecision=8"));
      ASSERT_TRUE(c->parse_string("Ssigned=no"));
      ASSERT_TRUE(c->parse_string("Sprofile=PART2"));
      ASSERT_TRUE(c->parse_string("NLType=GAMMA"));
      ASSERT_TRUE(c->parse_string("NLTgamma={300.0,1.0,0.0,1.0,0.0}"));
      EXPECT_THROW(c->finalize_all(), kdu_exception);

      c = BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/1);
      ASSERT_TRUE(c->parse_string("Sdims={32,32}"));
      ASSERT_TRUE(c->parse_string("Scomponents=1"));
      ASSERT_TRUE(c->parse_string("Sprecision=8"));
      ASSERT_TRUE(c->parse_string("Ssigned=no"));
      c->finalize_all();
      kdu_params* nlt = c->access_cluster(NLT_params);
      // Missing Sextensions_NLT emits a warning and ignores the NLT marker.
      GlobalWarningHandler().Clear();
      kdu_byte global_none[4] = {0xFF, 0xFF, 0x07, 0x00};
      EXPECT_FALSE(
          nlt->translate_marker_segment(KDU_NLT, 4, global_none, -1, 0));
      EXPECT_FALSE(GlobalWarningHandler().text().empty());

      // Invalid bit depth (40 > 38) throws.
      kdu_byte bad_prec[4] = {0xFF, 0xFF, 39, 0x00};
      EXPECT_THROW(nlt->translate_marker_segment(KDU_NLT, 4, bad_prec, -1, 0),
                   kdu_exception);

      // Invalid transform type with Sextensions=NLT enabled.
      ASSERT_TRUE(c->parse_string("Sprofile=PART2"));
      ASSERT_TRUE(c->parse_string("Sextensions=NLT"));
      kdu_byte bad_type[4] = {0xFF, 0xFF, 0x07, 0x07};
      EXPECT_THROW(nlt->translate_marker_segment(KDU_NLT, 4, bad_type, -1, 0),
                   kdu_exception);

      // Truncated NLT marker throws.
      kdu_byte too_short[3] = {0xFF, 0xFF, 0x07};
      EXPECT_THROW(nlt->translate_marker_segment(KDU_NLT, 3, too_short, -1, 0),
                   kdu_exception);
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

TEST_F(ParamsTest, BaseCopyFromParseErrorsAndHelpers) {
  try {
    // Multi-tile cluster exercising copy_from, textualize_attributes,
    // describe_attributes, truncate_records, delete_unparsed_attribute.
    std::unique_ptr<siz_params> src =
        BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/3, /*nlt_comps=*/0);
    ASSERT_TRUE(src->parse_string("Ssize={48,48}"));
    ASSERT_TRUE(src->parse_string("Ssampling={1,1}"));
    ASSERT_TRUE(src->parse_string("Stiles={24,48}"));
    ASSERT_TRUE(src->parse_string("Scomponents=3"));
    ASSERT_TRUE(src->parse_string("Sprecision=8"));
    ASSERT_TRUE(src->parse_string("Ssigned=no"));
    ASSERT_TRUE(src->parse_string("Cycc=no"));
    ASSERT_TRUE(src->parse_string("Clevels=1"));
    ASSERT_TRUE(src->parse_string("Cuse_precincts=yes"));
    ASSERT_TRUE(src->parse_string("Cprecincts={32,32}"));
    ASSERT_TRUE(src->parse_string("Qstep=0.05"));
    ASSERT_TRUE(src->parse_string("Qstep:T1=0.025"));
    src->finalize_all();

    StringMessage dumped;
    src->textualize_attributes(dumped, 0, 1, /*skip_derived=*/true);
    EXPECT_NE(dumped.text().find("Qstep"), std::string::npos);

    StringMessage described_with_comments;
    src->describe_attributes(described_with_comments,
                             /*include_comments=*/true);
    EXPECT_NE(described_with_comments.text().find("Ssize"), std::string::npos);
    StringMessage described_brief;
    src->describe_attributes(described_brief, /*include_comments=*/false);
    EXPECT_LT(described_brief.text().size(),
              described_with_comments.text().size());

    // Direct copy_from calls on cluster heads (single-tile and all-tiles).
    std::unique_ptr<siz_params> dst =
        BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/3, /*nlt_comps=*/0);
    dst->copy_from(src.get(), /*source_tile=*/-1, /*target_tile=*/-1,
                   /*instance=*/-1, /*skip_components=*/0, /*discard_levels=*/0,
                   false, false, false, /*force_copy=*/true);
    dst->copy_from(src.get(), /*source_tile=*/1, /*target_tile=*/1);
    dst->copy_from(src.get(), /*source_tile=*/99, /*target_tile=*/0);

    // truncate_records and delete_unparsed_attribute.
    kdu_params* dst_qcd = dst->access_cluster(QCD_params);
    EXPECT_TRUE(dst_qcd->truncate_records(Qabs_steps, 1));
    EXPECT_FALSE(dst_qcd->truncate_records(Qabs_steps, 5));
    dst->access_cluster(ENC_params)->set(Cweight, 0, 0, 1.5F);
    dst->access_cluster(ENC_params)->delete_unparsed_attribute(Cweight);

    // Geometric flip without finalizing SIZ drives generate_marker_segments
    // into negative coordinate adjustment and find_lcm(). Finalize non-SIZ
    // clusters so COD/QCD can still emit marker segments.
    std::unique_ptr<siz_params> flipped =
        BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/3, /*nlt_comps=*/0);
    flipped->copy_all(src.get(), /*skip_components=*/0, /*discard_levels=*/0,
                      /*transpose=*/true, /*vflip=*/true, /*hflip=*/true);
    for (const char* cl : {COD_params, QCD_params, ATK_params, DFS_params,
                           ADS_params, ENC_params, RGN_params, POC_params,
                           CRG_params, MCT_params, MCC_params, MCO_params}) {
      if (kdu_params* cluster = flipped->access_cluster(cl)) {
        cluster->finalize();
      }
    }
    VectorOutput flipped_out;
    flipped->generate_marker_segments(&flipped_out, -1, 0);
    flipped_out.Flush();
    EXPECT_GT(flipped_out.bytes().size(), 0U);

    // synthesize_canvas_size (Sdims given without Ssize or Ssampling).
    {
      std::unique_ptr<siz_params> synth =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/2, /*nlt_comps=*/0);
      ASSERT_TRUE(synth->parse_string("Scomponents=2"));
      ASSERT_TRUE(synth->parse_string("Sorigin={1,2}"));
      ASSERT_TRUE(synth->parse_string("Sdims={97,97},{31,31}"));
      ASSERT_TRUE(synth->parse_string("Sprecision=8"));
      ASSERT_TRUE(synth->parse_string("Ssigned=no"));
      synth->finalize_all();
      int sx = 0, sy = 0;
      EXPECT_TRUE(synth->get(Ssize, 0, 0, sy));
      EXPECT_TRUE(synth->get(Ssize, 0, 1, sx));
      EXPECT_GT(sy, 0);
      EXPECT_GT(sx, 0);
    }

    // Error paths: invalid enum/flag strings (hits display_options), bad
    // copy_from cluster/non-head calls, bad attribute names, impossible Sdims.
    {
      ScopedQuietErrors quiet;
      EXPECT_THROW(src->parse_string("Corder=INVALID_ORDER"), kdu_exception);
      EXPECT_NE(GlobalErrorHandler().last_message().find("LRCP"),
                std::string::npos);
      EXPECT_THROW(src->parse_string("Cmodes=INVALID_MODE"), kdu_exception);
      EXPECT_NE(GlobalErrorHandler().last_message().find("BYPASS"),
                std::string::npos);

      kdu_params* dst_cod = dst->access_cluster(COD_params);
      EXPECT_THROW(dst->copy_from(dst_cod, -1, -1), kdu_exception);
      ASSERT_EQ(dst->access_relation(0, -1), nullptr);
      kdu_params* dst_cod_tile0 = dst_cod->access_relation(0, -1);
      ASSERT_NE(dst_cod_tile0, nullptr);
      EXPECT_THROW(dst_cod_tile0->copy_from(src.get(), 0, 0), kdu_exception);
      EXPECT_THROW(dst->truncate_records("NoSuchAttr", 0), kdu_exception);
      EXPECT_THROW(dst->delete_unparsed_attribute("NoSuchAttr"), kdu_exception);

      std::unique_ptr<siz_params> bad_synth =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/2, /*nlt_comps=*/0);
      ASSERT_TRUE(bad_synth->parse_string("Scomponents=2"));
      ASSERT_TRUE(bad_synth->parse_string("Sdims={10,10},{10,10}"));
      ASSERT_TRUE(bad_synth->parse_string("Ssampling={1,1},{3,3}"));
      ASSERT_TRUE(bad_synth->parse_string("Sprecision=8"));
      ASSERT_TRUE(bad_synth->parse_string("Ssigned=no"));
      EXPECT_THROW(bad_synth->finalize_all(), kdu_exception);
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

void AppendBe16(std::vector<kdu_byte>* buf, kdu_uint16 val) {
  buf->push_back(static_cast<kdu_byte>((val >> 8) & 0xFF));
  buf->push_back(static_cast<kdu_byte>(val & 0xFF));
}

void AppendBe32(std::vector<kdu_byte>* buf, kdu_uint32 val) {
  buf->push_back(static_cast<kdu_byte>((val >> 24) & 0xFF));
  buf->push_back(static_cast<kdu_byte>((val >> 16) & 0xFF));
  buf->push_back(static_cast<kdu_byte>((val >> 8) & 0xFF));
  buf->push_back(static_cast<kdu_byte>(val & 0xFF));
}

bool ParseLines(kdu_params* p, const char* spec) {
  std::string s(spec);
  size_t pos = 0;
  while (pos < s.size()) {
    size_t next = s.find('\n', pos);
    std::string line =
        (next == std::string::npos) ? s.substr(pos) : s.substr(pos, next - pos);
    if (!line.empty() && !p->parse_string(line.c_str())) {
      return false;
    }
    if (next == std::string::npos) break;
    pos = next + 1;
  }
  return true;
}

TEST_F(ParamsTest, SizProfilesPrfCpfCapRoundTripAndErrors) {
  try {
    struct ProfileCase {
      const char* name;
      const char* spec;
    };
    const ProfileCase kCases[] = {
        {"PROFILE0",
         "Sprofile=PROFILE0\nSsize={256,256}\nSsampling={1,1}\nScomponents=3\n"
         "Sprecision=8\nSsigned=no\nCycc=yes\nCblk={32,32}\n"
         "Cmodes=RESTART\nClevels=3"},
        {"PROFILE1",
         "Sprofile=PROFILE1\nSsize={256,256}\nSsampling={1,1}\nScomponents=3\n"
         "Sprecision=8\nSsigned=no\nCycc=yes\nCblk={64,64}\nClevels=3"},
        {"CINEMA2K",
         "Sprofile=CINEMA2K\nSsize={1080,2048}\nSsampling={1,1}\n"
         "Scomponents=3\nSprecision=12\nSsigned=no\nCycc=yes\nClevels=5\n"
         "Corder=CPRL\nCblk={32,32}\nCreversible=no\nCkernels=W9X7\n"
         "Cuse_precincts=yes\nCprecincts={256,256}\nORGgen_tlm=1\n"
         "ORGtparts=C\nClayers=1"},
        {"CINEMA4K",
         "Sprofile=CINEMA4K\nSsize={2160,4096}\nSsampling={1,1}\n"
         "Scomponents=3\nSprecision=12\nSsigned=no\nCycc=yes\nClevels=6\n"
         "Corder=CPRL\nCblk={32,32}\nCreversible=no\nCkernels=W9X7\n"
         "Cuse_precincts=yes\nCprecincts={256,256}\nORGgen_tlm=1\n"
         "ORGtparts=C\nClayers=1"},
        {"CINEMA2S",
         "Sprofile=CINEMA2S\nSsize={1080,2048}\nSsampling={1,1}\n"
         "Scomponents=3\nSprecision=12\nSsigned=no\nCycc=yes\nClevels=4\n"
         "Corder=CPRL\nCblk={32,32}\nCreversible=no\nCkernels=W9X7\n"
         "Cuse_precincts=yes\nCprecincts={256,256}\nORGgen_tlm=1\n"
         "ORGtparts=C\nORGgen_plt=yes"},
        {"CINEMA4S",
         "Sprofile=CINEMA4S\nSsize={2160,4096}\nSsampling={1,1}\n"
         "Scomponents=3\nSprecision=12\nSsigned=no\nCycc=yes\nCorder=CPRL\n"
         "Cblk={32,32}\nCreversible=no\nCkernels=W9X7\nCuse_precincts=yes\n"
         "Cprecincts={256,256}\nORGgen_tlm=1\nORGtparts=C\nORGgen_plt=yes"},
        {"CINEMASS",
         "Sprofile=CINEMASS\nSsize={512,512}\nSsampling={1,1}\n"
         "Scomponents=3\nSprecision=12\nSsigned=no\nCycc=yes\nCorder=CPRL\n"
         "Cblk={32,32}\nCuse_precincts=yes\nCprecincts={256,256}\n"
         "ORGgen_tlm=1\nORGtparts=C\nORGgen_plt=yes\nCuse_sop=yes\n"
         "Cuse_eph=yes"},
        {"BROADCAST_S",
         "Sprofile=BROADCAST\nSbroadcast={6,single,irrev}\n"
         "Ssize={1080,1920}\nSsampling={1,1}\nScomponents=3\n"
         "Sprecision=10\nSsigned=no\nCycc=yes\nClevels=5\nCorder=CPRL\n"
         "Cblk={64,64}\nCreversible=no\nCkernels=W9X7\nCuse_precincts=yes\n"
         "Cprecincts={256,256}\nORGgen_tlm=1\nORGtparts=C\nClayers=1"},
        {"BROADCAST_M_REV",
         "Sprofile=BROADCAST\nSbroadcast={6,multi,rev}\n"
         "Ssize={1080,1920}\nSsampling={1,1}\nScomponents=3\n"
         "Sprecision=10\nSsigned=no\nCycc=yes\nClevels=5\nCorder=CPRL\n"
         "Cblk={64,64}\nCreversible=yes\nCkernels=W5X3\nCuse_precincts=yes\n"
         "Cprecincts={256,256}\nORGgen_tlm=1\nORGtparts=C\nClayers=1"},
        {"IMF_4K_MTILE_REV",
         "Sprofile=IMF\nSimf={4,1,rev}\nSsize={2000,3000}\nSsampling={1,1}\n"
         "Stiles={1024,1024}\nScomponents=3\nSprecision=10\nSsigned=no\n"
         "Cycc=yes\nCorder=CPRL\nCblk={32,32}\nCreversible=yes\n"
         "Ckernels=W5X3\nCuse_precincts=yes\nCprecincts={256,256}\n"
         "ORGgen_tlm=1\nORGtparts=C\nClayers=1"},
        {"IMFHT_EPROFILE",
         "Seprofile=IMFHT\nSsize={2160,3840}\nSsampling={1,1}\n"
         "Scomponents=3\nSprecision=10\nSsigned=no\nCycc=yes\nCorder=PCRL\n"
         "Cblk={64,64}\nCmodes=HT\nCreversible=no\nCkernels=W9X7\n"
         "Cuse_precincts=yes\nCprecincts={256,256}\nORGgen_tlm=1\n"
         "ORGtparts=C\nClayers=1"},
        {"HT_WITH_IMF_CPF",
         "Scpf_profile=IMF\nSimf={4,1,irrev}\n"
         "Ssize={1080,1920}\nSsampling={1,1}\nScomponents=3\n"
         "Sprecision=12\nSsigned=no\nCycc=yes\nCmodes=HT\nCorder=CPRL\n"
         "Cblk={64,64}\nCuse_precincts=yes\nCprecincts={256,256}\n"
         "ORGgen_tlm=1\nORGtparts=C\nClayers=1"},
        {"HT_WITH_LARGE_CPF",
         "Scpf_num=5\nSsize={256,256}\nSsampling={1,1}\nScomponents=3\n"
         "Sprecision=12\nSsigned=no\nCycc=yes\nCmodes=HT\nClevels=3"},
    };
    for (const ProfileCase& tc : kCases) {
      SCOPED_TRACE(tc.name);
      try {
        std::unique_ptr<siz_params> src = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(src.get(), tc.spec));
        src->finalize_all();
        std::unique_ptr<siz_params> dst = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
        RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/1);
      } catch (kdu_exception exc) {
        ADD_FAILURE() << tc.name << ": Unexpected kdu_exception(" << exc
                      << "): " << GlobalErrorHandler().last_message();
      }
    }

    // Crafted CAP, PRF, and CPF marker segments covering warning/error paths.
    std::unique_ptr<siz_params> p =
        BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
    ASSERT_TRUE(p->parse_string("Ssize={64,64}"));
    ASSERT_TRUE(p->parse_string("Ssampling={1,1}"));
    ASSERT_TRUE(p->parse_string("Scomponents=3"));
    ASSERT_TRUE(p->parse_string("Sprecision=8"));
    ASSERT_TRUE(p->parse_string("Ssigned=no"));
    ASSERT_TRUE(p->parse_string("Scap=P2|P15|P17"));
    {
      ScopedQuietErrors quiet;
      // CAP with P2 (unknown caps), P15 (RGN=0 -> clamp, Bval > 27 -> B=37,
      // unrecognized bit 0x0020), and P17 (unrecognized caps).
      std::vector<kdu_byte> cap;
      AppendBe32(&cap, Scap_P2 | Scap_P15 | Scap_P17);
      AppendBe16(&cap, 0x0001);
      AppendBe16(&cap, 24 | 0x0020);  // Bval=47 clamped to 37 + unknown flag
      AppendBe16(&cap, 0x0001);
      EXPECT_NE(0, p->translate_marker_segment(KDU_CAP, cap.size(), cap.data(),
                                               -1, 0));

      // Too small CAP throws.
      std::vector<kdu_byte> short_cap = {0x40, 0x00, 0x00, 0x00};
      EXPECT_THROW(p->translate_marker_segment(KDU_CAP, short_cap.size(),
                                               short_cap.data(), -1, 0),
                   kdu_exception);

      // PRF with trailing word==0, >32-bit, and short warnings.
      std::vector<kdu_byte> prf_zero = {0x00, 0x00};
      p->translate_marker_segment(KDU_PRF, prf_zero.size(), prf_zero.data(), -1,
                                  0);
      std::vector<kdu_byte> prf_big = {0x00, 0x01, 0x00, 0x00, 0x00, 0x01};
      p->translate_marker_segment(KDU_PRF, prf_big.size(), prf_big.data(), -1,
                                  0);
      std::vector<kdu_byte> prf_short = {0x00};
      p->translate_marker_segment(KDU_PRF, prf_short.size(), prf_short.data(),
                                  -1, 0);
      // PRF on Part-1 without CAP bit warns.
      std::unique_ptr<siz_params> p1 =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(p1->parse_string("Ssize={64,64}"));
      ASSERT_TRUE(p1->parse_string("Ssampling={1,1}"));
      ASSERT_TRUE(p1->parse_string("Scomponents=1"));
      ASSERT_TRUE(p1->parse_string("Sprecision=8"));
      ASSERT_TRUE(p1->parse_string("Ssigned=no"));
      std::vector<kdu_byte> prf_ok = {0x00, 0x01};
      p1->translate_marker_segment(KDU_PRF, prf_ok.size(), prf_ok.data(), -1,
                                   0);
      // CPF ignored when Scap_P15 not set.
      p1->translate_marker_segment(KDU_CPF, prf_ok.size(), prf_ok.data(), -1,
                                   0);

      // CPF with 0 -> 4095, 4095 (invalid), trailing zero, >32-bit, short.
      std::vector<kdu_byte> cpf_unconstrained = {0x00,
                                                 0x01};  // cpfnum=0 -> 4095
      p->translate_marker_segment(KDU_CPF, cpf_unconstrained.size(),
                                  cpf_unconstrained.data(), -1, 0);
      std::vector<kdu_byte> cpf_bad4095 = {0x10,
                                           0x00};  // cpfnum_plus1=4096 -> 4095
      p->translate_marker_segment(KDU_CPF, cpf_bad4095.size(),
                                  cpf_bad4095.data(), -1, 0);
      p->translate_marker_segment(KDU_CPF, prf_zero.size(), prf_zero.data(), -1,
                                  0);
      std::vector<kdu_byte> cpf_huge(10, 0x01);  // cpfbits >= 64
      p->translate_marker_segment(KDU_CPF, cpf_huge.size(), cpf_huge.data(), -1,
                                  0);
      EXPECT_THROW(p->translate_marker_segment(KDU_CPF, prf_short.size(),
                                               prf_short.data(), -1, 0),
                   kdu_exception);

      // Sprf_num / Scpf_num / Sbroadcast / Simf conflict warnings and errors.
      std::unique_ptr<siz_params> bcst =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(bcst.get(),
                             "Ssize={64,64}\nSsampling={1,1}\n"
                             "Scomponents=1\nSprecision=8\n"
                             "Ssigned=no\nSprofile=PROFILE0\n"
                             "Sprf_num=257\n"
                             "Sbroadcast={6,single,rev}"));
      bcst->finalize_all();
      std::unique_ptr<siz_params> bad_bcst =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(bad_bcst.get(),
                             "Ssize={64,64}\nSsampling={1,1}\n"
                             "Scomponents=1\nSprecision=8\n"
                             "Ssigned=no\n"
                             "Sbroadcast={12,single,irrev}"));
      EXPECT_THROW(bad_bcst->finalize_all(), kdu_exception);

      std::unique_ptr<siz_params> bad_imf =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(bad_imf.get(),
                             "Ssize={64,64}\nSsampling={1,1}\n"
                             "Scomponents=1\nSprecision=8\n"
                             "Ssigned=no\nSimf={3,3,irrev}"));
      EXPECT_THROW(bad_imf->finalize_all(), kdu_exception);

      std::unique_ptr<siz_params> bad_cin4k =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          bad_cin4k.get(),
          "Sprofile=CINEMA4K\nSsize={4096,2160}\nSsampling={1,1}\n"
          "Scomponents=3\nSprecision=12\nSsigned=no\nCycc=yes\nClevels=6\n"
          "Corder=CPRL\nCblk={32,32}\nCreversible=no\nCkernels=W9X7\n"
          "Cuse_precincts=yes\nCprecincts={256,256}\nORGgen_tlm=1\n"
          "ORGtparts=C\nClayers=1\nPorder={0,0,1,1,1,LRCP}"));
      EXPECT_THROW(bad_cin4k->finalize_all(), kdu_exception);
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

TEST_F(ParamsTest, MctMccMcoTransformRoundTripAndErrors) {
  try {
    std::unique_ptr<siz_params> src =
        BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/4, /*nlt_comps=*/0);
    ASSERT_TRUE(ParseLines(
        src.get(),
        "Ssize={64,64}\nSsampling={1,1}\nScomponents=4\n"
        "Sprecision=8\nSsigned=no\n"
        "Mcomponents=4\nMprecision=10,8,8,8\nMsigned=yes,no,no,no\n"
        "Mnum_stages=3\nMstages=1,2,3\n"
        "Mstages:T0=1,2,3\n"
        "Mstage_inputs:I1={0,3}\nMstage_outputs:I1={0,3}\n"
        "Mstage_collections:I1={4,4}\nMstage_xforms:I1={DEP,1,2,0,0}\n"
        "Mtriang_size:I1=6\nMtriang_coeffs:I1=1,2,3,4,5,6\n"
        "Mvector_size:I2=4\nMvector_coeffs:I2=1,2,3,4\n"
        "Mstage_inputs:I2={0,3}\nMstage_outputs:I2={0,3}\n"
        "Mstage_collections:I2={4,4}\nMstage_xforms:I2={MATRIX,3,4,0,0}\n"
        "Mmatrix_size:I3=16\n"
        "Mmatrix_coeffs:I3=0.5,0.25,0,0,0,0.5,0,0,0,0,0.5,0,0,0,0,0.5\n"
        "Mvector_size:I4=4\nMvector_coeffs:I4=0.1,0.2,0.3,0.4\n"
        "Mstage_inputs:I3={0,3}\nMstage_outputs:I3={0,3}\n"
        "Mstage_collections:I3={4,4}\nMstage_xforms:I3={DWT,0,0,2,0}\n"
        "Cycc=no\nClevels=2"));
    src->finalize_all();

    std::unique_ptr<siz_params> dst =
        BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/4, /*nlt_comps=*/0);
    RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/2);
    for (int tile = -1; tile < 2; ++tile) {
      for (const char* cl : {MCT_params, MCC_params, MCO_params}) {
        dst->access_cluster(cl)->access_relation(tile, -1, 0)->finalize(true);
      }
    }

    // mco_params::copy_with_xforms preserves stages when component count is
    // unchanged. Note: calling copy_all with non-empty MCO and
    // skip_components > 0 (or new_components != old_components) hits a vendor
    // bug at params.cpp:8943 (`mcc = access_unique(...)` instead of
    // `mcc->access_unique(...)`), causing line 8971 to return nullptr and line
    // 8972 to dereference it.
    std::unique_ptr<siz_params> copied =
        BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/4, /*nlt_comps=*/0);
    copied->copy_all(src.get(), /*skip_components=*/0, /*discard_levels=*/0,
                     /*transpose=*/false, /*vflip=*/false, /*hflip=*/false);
    int copied_stages = 0;
    EXPECT_TRUE(copied->access_cluster(MCO_params)
                    ->get(Mnum_stages, 0, 0, copied_stages));
    EXPECT_EQ(3, copied_stages);

    // Malformed MCT / MCC / MCO marker segments & finalize() error paths.
    {
      ScopedQuietErrors quiet;
      // Direct MCT parsing: 16-bit int (coeff_type=0) & 64-bit double (3).
      kdu_params* mct5 =
          src->access_cluster(MCT_params)->access_relation(-1, -1, 5);
      std::vector<kdu_byte> mct16;
      AppendBe16(&mct16, 0);                         // Zmct=0
      AppendBe16(&mct16, 5 | (1 << 8) | (0 << 10));  // inst=5, matrix, int16
      AppendBe16(&mct16, 0);                         // Ymct=0
      AppendBe16(&mct16, 0xFFFE);                    // -2
      AppendBe16(&mct16, 3);
      EXPECT_NE(0, mct5->translate_marker_segment(KDU_MCT, mct16.size(),
                                                  mct16.data(), -1, 0));

      kdu_params* mct6 =
          src->access_cluster(MCT_params)->access_relation(-1, -1, 6);
      std::vector<kdu_byte> mct64;
      AppendBe16(&mct64, 0);
      AppendBe16(&mct64, 6 | (2 << 8) | (3 << 10));  // inst=6, offset, float64
      AppendBe16(&mct64, 0);
      AppendBe32(&mct64, 0x3FE00000U);  // 0.5 in IEEE 754 binary64
      AppendBe32(&mct64, 0x00000000U);
      EXPECT_NE(0, mct6->translate_marker_segment(KDU_MCT, mct64.size(),
                                                  mct64.data(), -1, 0));
      // Repeating Zmct=0 on mct6 throws.
      EXPECT_THROW(mct6->translate_marker_segment(KDU_MCT, mct64.size(),
                                                  mct64.data(), -1, 0),
                   kdu_exception);

      // Trailing unconsumed byte & short MCT.
      std::vector<kdu_byte> mct_extra = mct16;
      mct_extra.push_back(0xAA);
      EXPECT_THROW(mct5->translate_marker_segment(KDU_MCT, mct_extra.size(),
                                                  mct_extra.data(), -1, 0),
                   kdu_exception);
      std::vector<kdu_byte> mct_short = {0x00, 0x00, 0x01, 0x05, 0x00};
      EXPECT_THROW(mct5->translate_marker_segment(KDU_MCT, mct_short.size(),
                                                  mct_short.data(), -1, 0),
                   kdu_exception);

      // MCC marker error paths (b/336206504 short, z_mcc != 0, trailing bytes).
      kdu_params* mcc5 =
          src->access_cluster(MCC_params)->access_relation(-1, -1, 5);
      std::vector<kdu_byte> mcc_short = {0x00, 0x00, 0x05, 0x00};
      EXPECT_THROW(mcc5->translate_marker_segment(KDU_MCC, mcc_short.size(),
                                                  mcc_short.data(), -1, 0),
                   kdu_exception);
      std::vector<kdu_byte> mcc_nz = {0x00, 0x01, 0x05, 0x00, 0x00};
      EXPECT_THROW(mcc5->translate_marker_segment(KDU_MCC, mcc_nz.size(),
                                                  mcc_nz.data(), -1, 0),
                   kdu_exception);

      // MCO marker error paths.
      kdu_params* mco = src->access_cluster(MCO_params);
      std::vector<kdu_byte> mco_extra = {0x01, 0x01, 0xFF};
      EXPECT_THROW(mco->translate_marker_segment(KDU_MCO, mco_extra.size(),
                                                 mco_extra.data(), -1, 0),
                   kdu_exception);
      std::vector<kdu_byte> mco_short = {0x02, 0x01};
      EXPECT_THROW(mco->translate_marker_segment(KDU_MCO, mco_short.size(),
                                                 mco_short.data(), -1, 0),
                   kdu_exception);
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

TEST_F(ParamsTest, AtkPredefCustomKernelAndMarkerErrors) {
  try {
    // Predefined ATK kernels (:I2..:I6), custom pattern-matched kernels
    // (:I7..:I11), and custom >16-coeff asymmetric reversible kernel (:I12)
    // with DC gain = 1.
    std::unique_ptr<siz_params> src =
        BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/1, /*nlt_comps=*/0);
    ASSERT_TRUE(ParseLines(
        src.get(),
        "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\n"
        "Sprecision=8\nSsigned=no\n"
        "Stiles={32,64}\nCkernels=ATK\nCatk=7\nClevels=2\n"
        "Kkernels:I2=I1X1\nKkernels:I3=R1X1\nKkernels:I4=I2X2\n"
        "Kkernels:I5=R2X2\nKkernels:I6=I5X3\n"
        "Kreversible:I7=no\nKextension:I7=SYM\n"
        "Ksteps:I7={2,0,0,0},{2,-1,0,0}\n"
        "Kcoeffs:I7=-0.5,-0.5,0.25,0.25\n"
        "Kreversible:I8=yes\nKextension:I8=CON\n"
        "Ksteps:I8={1,0,0,0},{1,0,1,1}\nKcoeffs:I8=-1.0,0.5\n"
        "Kreversible:I9=no\nKextension:I9=CON\n"
        "Ksteps:I9={1,0,0,0},{1,0,0,0}\nKcoeffs:I9=-1.0,0.5\n"
        "Kreversible:I10=yes\nKsteps:I10={1,0,0,0}\nKcoeffs:I10=0.0\n"
        "Kreversible:I11=no\nKsteps:I11={1,0,0,0}\nKcoeffs:I11=0.0\n"
        "Kreversible:I12=yes\n"
        "Ksteps:I12={9,-4,0,0},{9,-4,1,1}\n"
        "Kcoeffs:I12=1,-1,0,0,0,0,0,0,0,0.5,-0.5,0,0,0,0,0,0,0\n"
        "Kreversible:T0I7=no\nKextension:T0I7=SYM\n"
        "Ksteps:T0I7={2,0,0,0},{2,-1,0,0}\n"
        "Kcoeffs:T0I7=-0.5,-0.5,0.25,0.25"));
    src->finalize_all();

    int k7 = -1, k8 = -1, k9 = -1, k10 = -1, k11 = -1;
    kdu_params* atk = src->access_cluster(ATK_params);
    EXPECT_TRUE(atk->access_relation(-1, -1, 7)->get(Kkernels, 0, 0, k7));
    EXPECT_EQ(Kkernels_I5X3, k7);
    EXPECT_TRUE(atk->access_relation(-1, -1, 8)->get(Kkernels, 0, 0, k8));
    EXPECT_EQ(Kkernels_R2X2, k8);
    EXPECT_TRUE(atk->access_relation(-1, -1, 9)->get(Kkernels, 0, 0, k9));
    EXPECT_EQ(Kkernels_I2X2, k9);
    EXPECT_TRUE(atk->access_relation(-1, -1, 10)->get(Kkernels, 0, 0, k10));
    EXPECT_EQ(Kkernels_R1X1, k10);
    EXPECT_TRUE(atk->access_relation(-1, -1, 11)->get(Kkernels, 0, 0, k11));
    EXPECT_EQ(Kkernels_I1X1, k11);

    std::unique_ptr<siz_params> dst =
        BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/1, /*nlt_comps=*/0);
    RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/2);

    // copy_with_xforms on unfinalized ATK source triggers
    // source->finalize(false), and simultaneous vflip+hflip reverses asymmetric
    // lifting coefficients.
    std::unique_ptr<siz_params> unfinal =
        BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
    ASSERT_TRUE(ParseLines(unfinal.get(),
                           "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\n"
                           "Sprecision=8\nSsigned=no\n"
                           "Kreversible:I2=yes\n"
                           "Ksteps:I2={3,-1,0,0},{3,-1,1,1}\n"
                           "Kcoeffs:I2=1,0,-1,0.5,0,-0.5"));
    static_cast<kdu_params*>(unfinal.get())
        ->finalize(false);  // Finalize SIZ only; leave ATK unfinalized.
    std::unique_ptr<siz_params> both_flipped =
        BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
    both_flipped->copy_all(unfinal.get(), 0, 0, false, /*vflip=*/true,
                           /*hflip=*/true);

    // Raw ATK marker variants (8-bit int coeffs, float64 coeffs) & errors.
    {
      ScopedQuietErrors quiet;
      // Single flip with asymmetric ATK throws.
      std::unique_ptr<siz_params> one_flip =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      EXPECT_THROW(
          one_flip->copy_all(both_flipped.get(), 0, 0, false, true, false),
          kdu_exception);

      // 8-bit integer reversible ATK (bytes_per_coeff=1, inst=13) with
      // m_last=0.
      kdu_params* a13 =
          src->access_cluster(ATK_params)->access_relation(-1, -1, 13);
      std::vector<kdu_byte> atk8;
      AppendBe16(&atk8,
                 13 | (0 << 8) | (1 << 12));  // rev=1, coeff=1B, m_last=0
      atk8.push_back(1);                      // num_steps=1
      atk8.push_back(0xFE);                   // Ns=-2
      atk8.push_back(1);                      // Ds=1
      atk8.push_back(1);                      // Rs=1
      atk8.push_back(1);                      // Ls=1
      atk8.push_back(0xFF);                   // coeff=-1
      EXPECT_NE(0, a13->translate_marker_segment(KDU_ATK, atk8.size(),
                                                 atk8.data(), -1, 0));

      // float64 irreversible ATK (bytes_per_coeff=8, inst=14).
      kdu_params* a14 =
          src->access_cluster(ATK_params)->access_relation(-1, -1, 14);
      std::vector<kdu_byte> atk64;
      AppendBe16(&atk64, 14 | (3 << 8));               // irrev, coeff=8B
      for (int i = 0; i < 8; ++i) atk64.push_back(0);  // K (ignored)
      atk64.push_back(1);                              // num_steps=1
      atk64.push_back(0);                              // Ns=0
      atk64.push_back(1);                              // Ls=1
      AppendBe32(&atk64, 0x3FE00000U);                 // 0.5
      AppendBe32(&atk64, 0x00000000U);
      EXPECT_NE(0, a14->translate_marker_segment(KDU_ATK, atk64.size(),
                                                 atk64.data(), -1, 0));

      // b/336182469 Ds > 32, rev+float, sym+CON, trailing bytes, short ATK.
      std::vector<kdu_byte> atk_ds = atk8;
      atk_ds[4] = 40;  // Ds=40 > 32
      EXPECT_THROW(a13->translate_marker_segment(KDU_ATK, atk_ds.size(),
                                                 atk_ds.data(), -1, 0),
                   kdu_exception);
      std::vector<kdu_byte> atk_sym_con;
      AppendBe16(&atk_sym_con, 13 | (1 << 11));  // sym=1, ext=CON(0)
      EXPECT_THROW(a13->translate_marker_segment(KDU_ATK, atk_sym_con.size(),
                                                 atk_sym_con.data(), -1, 0),
                   kdu_exception);
      std::vector<kdu_byte> atk_rev_flt;
      AppendBe16(&atk_rev_flt, 13 | (1 << 12) | (2 << 8));
      EXPECT_THROW(a13->translate_marker_segment(KDU_ATK, atk_rev_flt.size(),
                                                 atk_rev_flt.data(), -1, 0),
                   kdu_exception);
      std::vector<kdu_byte> atk_extra = atk8;
      atk_extra.push_back(0x00);
      EXPECT_THROW(a13->translate_marker_segment(KDU_ATK, atk_extra.size(),
                                                 atk_extra.data(), -1, 0),
                   kdu_exception);
      EXPECT_THROW(
          a13->translate_marker_segment(KDU_ATK, 3, atk8.data(), -1, 0),
          kdu_exception);

      // atk_params::finalize error cases (missing Kreversible, bad Ds, mismatch
      // coeffs, gain != 1).
      for (const char* bad_spec : {
               "Ksteps:I2={1,0,0,0}\nKcoeffs:I2=0",
               "Kreversible:I2=yes\nKsteps:I2={1,0,25,0}\nKcoeffs:I2=0",
               "Kreversible:I2=no\nKsteps:I2={1,0,1,0}\nKcoeffs:I2=0",
               "Kreversible:I2=no\nKsteps:I2={2,0,0,0}\nKcoeffs:I2=0",
               "Kreversible:I2=no\nKsteps:I2={1,0,0,0}\nKcoeffs:I2=0,1",
               "Kreversible:I2=no\nKsymmetric:I2=yes\nKsteps:I2={1,0,0,0}"
               "\nKcoeffs:I2=1",
               "Kreversible:I2=yes\nKsteps:I2={1,0,0,0},{1,0,1,1}\nKcoeffs:I2="
               "2,1",
           }) {
        SCOPED_TRACE(bad_spec);
        std::unique_ptr<siz_params> bad = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(bad.get(),
                               "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\n"
                               "Sprecision=8\nSsigned=no"));
        ASSERT_TRUE(ParseLines(bad.get(), bad_spec));
        EXPECT_THROW(bad->finalize_all(), kdu_exception);
      }
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

TEST_F(ParamsTest, CodAdsDfsPocCrgEncXformsAndErrors) {
  try {
    // Complex packet decomposition (ADS + DFS + Cdecomp), BYPASS_E1|BYPASS_E2,
    // PRCL progression, ENC weighting attributes, and CRGoffset.
    std::unique_ptr<siz_params> src =
        BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/4, /*nlt_comps=*/0);
    ASSERT_TRUE(ParseLines(
        src.get(),
        "Ssize={128,128}\nSsampling={1,1}\nStiles={64,128}\nScomponents=4\n"
        "Sprecision=8\nSsigned=no\nCycc=no\nClevels=2\nCorder=PRCL\n"
        "Cmodes=BYPASS|BYPASS_E1|BYPASS_E2\nCalign_blk_last={yes,yes}\n"
        "Cblk={32,32}\n"
        "Cdecomp:C0=B(HHH:HHH:HHH),B(-:-:-)\n"
        "Cdecomp:C1=H(-),H(-)\nCdecomp:C2=V(-),V(-)\n"
        "Cdecomp:C3=H(-),H(-)\n"
        "Cdecomp:T0C0=B(V--:H--:V--),B(-:-:-)\n"
        "Cplex={2,FIX,0.5,4}\nContent_type=PHOTO|GRAPHICS\nCvis=0.1\n"
        "Cweight=1.2\nClev_weights=1.0,1.1\n"
        "Cband_weights=1.0,0.9,0.8\nCprog_weights=0.25,0.5\n"
        "Qfparams={2.0,60,0.5,80}\nQfactor=85\nCRGoffset={0.25,0.75}"));
    src->finalize_all();

    int dfs1 = 0, dfs2 = 0, ads1 = 0;
    kdu_params* cod = src->access_cluster(COD_params);
    EXPECT_TRUE(cod->access_relation(-1, 1)->get(Cdfs, 0, 0, dfs1));
    EXPECT_TRUE(cod->access_relation(-1, 2)->get(Cdfs, 0, 0, dfs2));
    EXPECT_GT(dfs1, 0);
    EXPECT_NE(dfs1, dfs2);
    EXPECT_TRUE(cod->access_relation(0, 0)->get(Cads, 0, 0, ads1));
    EXPECT_GT(ads1, 0);

    std::unique_ptr<siz_params> dst =
        BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/4, /*nlt_comps=*/0);
    RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/2);

    // Static decomp helpers.
    int decomp0 = 0;
    ASSERT_TRUE(cod->access_relation(-1, 0)->get(Cdecomp, 0, 0, decomp0));
    int hlev = 0, vlev = 0;
    cod_params::get_max_decomp_levels(decomp0, hlev, vlev);
    EXPECT_GE(hlev, 2);
    kdu_int16 bands[49] = {};
    EXPECT_GT(cod_params::expand_decomp_bands(decomp0, bands), 3);
    EXPECT_TRUE(cod_params::is_valid_decomp_terminator(3));
    EXPECT_FALSE(cod_params::is_valid_decomp_terminator(3 | (1 << 2)));

    // CRG + ENC copy_with_xforms with discard_levels=1, skip_components=2,
    // transpose=true, vflip=false, hflip=false.
    std::unique_ptr<siz_params> xform_dst =
        BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/2, /*nlt_comps=*/0);
    xform_dst->copy_all(src.get(), /*skip_components=*/2, /*discard_levels=*/1,
                        /*transpose=*/true, /*vflip=*/false, /*hflip=*/false);
    float crg_y = 0.0F, crg_x = 0.0F;
    EXPECT_TRUE(
        xform_dst->access_cluster(CRG_params)->get(CRGoffset, 0, 0, crg_y));
    EXPECT_TRUE(
        xform_dst->access_cluster(CRG_params)->get(CRGoffset, 0, 1, crg_x));
    EXPECT_NEAR(0.75F, crg_y, 1e-4F);
    EXPECT_NEAR(0.25F, crg_x, 1e-4F);

    // copy_all with missing cluster (ENC present only in source, then only in
    // target).
    std::unique_ptr<siz_params> no_enc =
        BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/2, /*nlt_comps=*/0,
                         /*include_enc=*/false);
    no_enc->copy_all(xform_dst.get());
    std::unique_ptr<siz_params> with_enc =
        BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/2, /*nlt_comps=*/0,
                         /*include_enc=*/true);
    with_enc->copy_all(no_enc.get());

    // 260-component cluster exercises 2-byte component IDs in POC and COC/QCC.
    std::unique_ptr<siz_params> wide_src =
        BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/260, /*nlt_comps=*/0);
    ASSERT_TRUE(ParseLines(
        wide_src.get(),
        "Ssize={32,32}\nSsampling={1,1}\nScomponents=260\nSprecision=8\n"
        "Ssigned=no\nStiles={16,32}\nCycc=no\nClevels=1\nClevels:C259=0\n"
        "Qstep=0.05\n"
        "Porder={0,0,1,1,259,LRCP}\n"
        "Porder:T0={0,0,1,1,260,CPRL}\n"
        "Porder:T1={0,0,1,1,259,LRCP}"));
    wide_src->finalize_all();
    std::unique_ptr<siz_params> wide_dst =
        BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/260, /*nlt_comps=*/0);
    RoundTripMarkers(wide_src.get(), wide_dst.get(), /*total_tiles=*/2);

    // POC & CRG error paths.
    {
      ScopedQuietErrors quiet;
      for (const char* bad_poc : {
               "Porder={33,0,1,34,1,LRCP}",
               "Porder={1,0,1,1,1,LRCP}",
               "Porder={0,260,1,1,261,LRCP}",
               "Porder={0,0,1,1,265,LRCP}",
               "Porder={0,0,70000,1,1,LRCP}",
           }) {
        std::unique_ptr<siz_params> bp = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/4, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(bp.get(),
                               "Ssize={32,32}\nSsampling={1,1}\nScomponents=4\n"
                               "Sprecision=8\nSsigned=no"));
        ASSERT_TRUE(bp->parse_string(bad_poc));
        bp->finalize_all();
        VectorOutput out;
        EXPECT_THROW(bp->generate_marker_segments(&out, -1, 0), kdu_exception);
      }

      std::unique_ptr<siz_params> bad_crg =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          bad_crg.get(),
          "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCRGoffset={1.5,0.5}"));
      bad_crg->finalize_all();
      VectorOutput out;
      EXPECT_THROW(bad_crg->generate_marker_segments(&out, -1, 0),
                   kdu_exception);

      // Tile-level DFS mismatch throws in find_suitable_dfs_idx.
      std::unique_ptr<siz_params> bad_dfs =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          bad_dfs.get(),
          "Ssize={64,64}\nSsampling={1,1}\nStiles={32,64}\nScomponents=1\n"
          "Sprecision=8\nSsigned=no\nClevels=1\nCdecomp:T0=H(-)"));
      EXPECT_THROW(bad_dfs->finalize_all(), kdu_exception);
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

class B2CompressedTarget final : public kdu_compressed_target {
 public:
  explicit B2CompressedTarget(std::vector<kdu_byte>& out) : out_(out) {}

  bool write(const kdu_byte* buf, int num_bytes) override {
    out_.insert(out_.end(), buf, buf + num_bytes);
    return true;
  }

 private:
  std::vector<kdu_byte>& out_;
};

class B2CompressedSource final : public kdu_compressed_source {
 public:
  explicit B2CompressedSource(const std::vector<kdu_byte>& data)
      : data_(data) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE;
  }

  int read(kdu_byte* buf, int num_bytes) override {
    if (pos_ >= data_.size() || num_bytes <= 0) return 0;
    const size_t avail = data_.size() - pos_;
    const size_t n = std::min(avail, static_cast<size_t>(num_bytes));
    std::memcpy(buf, data_.data() + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }

  bool seek(kdu_long offset) override {
    if (offset < 0 || static_cast<size_t>(offset) > data_.size()) return false;
    pos_ = static_cast<size_t>(offset);
    return true;
  }

  kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }

 private:
  const std::vector<kdu_byte>& data_;
  size_t pos_ = 0;
};

struct B2CodestreamGuard {
  kdu_codestream cs;
  ~B2CodestreamGuard() {
    if (cs.exists()) cs.destroy();
  }
};

TEST(ParamsBatchB2Test, KduCodestreamCmemClusterConstructionAndRoundTrip) {
  const char* stage = "setup";
  try {
    std::unique_ptr<siz_params> in_siz =
        BuildFullCluster(/*total_tiles=*/4, /*num_comps=*/3, /*nlt_comps=*/3);
    ASSERT_TRUE(ParseLines(
        in_siz.get(),
        "Ssize={32,32}\nSsampling={1,1}\nStiles={16,16}\nScomponents=3\n"
        "Sprecision=8\nSsigned=no\nMcomponents=3\nMprecision=8\nMsigned=no\n"
        "Creversible=no\nClevels=2\nCblk={16,16}\nCorder=LRCP\nClayers=1\n"
        "Cycc=no\nCRGoffset={0.25,0.5}\nRshift=30\nNLType=NONE\n"
        "Porder={0,0,1,3,3,LRCP}\nCatk=2\nKreversible:I2=no\n"
        "Ksymmetric:I2=yes\nKextension:I2=SYM\n"
        "Ksteps:I2={2,0,0,0},{2,-1,0,0}\n"
        "Kcoeffs:I2=-0.5,-0.5,0.25,0.25\n"
        "Cdfs=1\nDSdfs:I1=B,H\n"
        "Mmatrix_size:I1=9\nMmatrix_coeffs:I1=1,0,0,0,1,0,0,0,1\n"
        "Mstage_inputs:I1={0,2}\nMstage_outputs:I1={0,2}\n"
        "Mstage_collections:I1={3,3}\nMstage_xforms:I1={MATRIX,1,0,0,0}\n"
        "Mnum_stages=1\nMstages=1\nORGtparts=R\nORGgen_tlm=1"));
    stage = "in_siz->finalize_all";
    in_siz->finalize_all();

    std::vector<kdu_byte> codestream_bytes;
    B2CompressedTarget target(codestream_bytes);
    B2CodestreamGuard writer;
    stage = "writer.cs.create";
    writer.cs.create(in_siz.get(), &target);
    ASSERT_NE(writer.cs.access_siz(), nullptr);
    stage = "writer.cs.copy_all";
    writer.cs.access_siz()->copy_all(in_siz.get());
    EXPECT_TRUE(writer.cs.access_siz()->parse_string("Mvector_size:I2=3"));
    EXPECT_TRUE(
        writer.cs.access_siz()->parse_string("Mvector_coeffs:I2=0,0,0"));
    EXPECT_TRUE(writer.cs.access_siz()->parse_string("Rshift:T1C0=32"));
    writer.cs.access_siz()->finalize_all();

    auto push_all_tiles = [](kdu_codestream& cs, int num_comps) {
      kdu_dims tile_indices;
      cs.get_valid_tiles(tile_indices);
      for (int ty = tile_indices.pos.y;
           ty < tile_indices.pos.y + tile_indices.size.y; ++ty) {
        for (int tx = tile_indices.pos.x;
             tx < tile_indices.pos.x + tile_indices.size.x; ++tx) {
          kdu_coords tidx(tx, ty);
          kdu_tile tile = cs.open_tile(tidx, nullptr);
          kdu_multi_analysis engine;
          engine.create(cs, tile);
          std::vector<int> heights(num_comps, 0);
          std::vector<int> widths(num_comps, 0);
          std::vector<int> pushed(num_comps, 0);
          for (int c = 0; c < num_comps; ++c) {
            kdu_dims cdims;
            cs.get_tile_dims(tidx, c, cdims, /*want_output_comps=*/true);
            heights[c] = cdims.size.y;
            widths[c] = cdims.size.x;
          }
          std::vector<kdu_line_buf*> lines(num_comps, nullptr);
          while (true) {
            int best_p = 1;
            int best_h = 1;
            bool any_left = false;
            for (int c = 0; c < num_comps; ++c) {
              if (pushed[c] < heights[c]) {
                if (!any_left ||
                    static_cast<kdu_long>(pushed[c]) * best_h <
                        static_cast<kdu_long>(best_p) * heights[c]) {
                  best_p = pushed[c];
                  best_h = heights[c];
                  any_left = true;
                }
              }
            }
            if (!any_left) break;
            bool made_progress = false;
            for (int c = 0; c < num_comps; ++c) {
              if (pushed[c] < heights[c] &&
                  static_cast<kdu_long>(pushed[c]) * best_h ==
                      static_cast<kdu_long>(best_p) * heights[c]) {
                lines[c] = engine.exchange_line(c, nullptr);
                EXPECT_NE(lines[c], nullptr);
                if (lines[c] != nullptr) {
                  if (auto* b16 = lines[c]->get_buf16()) {
                    for (int x = 0; x < widths[c]; ++x) b16[x].ival = 0;
                  } else if (auto* b32 = lines[c]->get_buf32()) {
                    if (lines[c]->is_absolute()) {
                      for (int x = 0; x < widths[c]; ++x) b32[x].ival = 0;
                    } else {
                      for (int x = 0; x < widths[c]; ++x) b32[x].fval = 0.0F;
                    }
                  }
                }
              } else {
                lines[c] = nullptr;
              }
            }
            for (int c = 0; c < num_comps; ++c) {
              if (lines[c] != nullptr) {
                engine.exchange_line(c, lines[c]);
                ++pushed[c];
                made_progress = true;
              }
            }
            if (!made_progress) {
              ADD_FAILURE() << "No line progress in push_all_tiles";
              break;
            }
          }
          engine.destroy();
          tile.close();
        }
      }
    };

    stage = "open_tile";
    kdu_dims tile_indices;
    writer.cs.get_valid_tiles(tile_indices);
    EXPECT_EQ(tile_indices.area(), 4);
    push_all_tiles(writer.cs, /*num_comps=*/3);
    stage = "writer.cs.flush";
    kdu_long layer_bytes = 1LL << 20;
    writer.cs.flush(&layer_bytes, 1);
    writer.cs.destroy();
    ASSERT_GT(codestream_bytes.size(), 20U);

    // Read generated codestream back so read-side coremem paths also execute.
    B2CompressedSource source(codestream_bytes);
    B2CodestreamGuard reader;
    stage = "reader.cs.create";
    reader.cs.create(&source);
    int read_comps = 0;
    EXPECT_TRUE(reader.cs.access_siz()->get(Scomponents, 0, 0, read_comps));
    EXPECT_EQ(read_comps, 3);
    reader.cs.destroy();

    // Separate kdu_codestream pass with ADS (arbitrary decomposition packet
    // structure) so ads_params(cmem) is also exercised cleanly.
    stage = "ads codestream";
    std::unique_ptr<siz_params> ads_siz =
        BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
    ASSERT_TRUE(ParseLines(
        ads_siz.get(),
        "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
        "Ssigned=no\nCreversible=no\nClevels=2\n"
        "Cdecomp=B(V--:H--:V--),B(-:-:-)\n"
        "ORGtparts=R"));
    ads_siz->finalize_all();
    std::vector<kdu_byte> ads_bytes;
    B2CompressedTarget ads_target(ads_bytes);
    B2CodestreamGuard ads_writer;
    ads_writer.cs.create(ads_siz.get(), &ads_target);
    ads_writer.cs.access_siz()->copy_all(ads_siz.get());
    ads_writer.cs.access_siz()->finalize_all();
    push_all_tiles(ads_writer.cs, /*num_comps=*/1);
    kdu_long ads_layer_bytes = 1LL << 20;
    ads_writer.cs.flush(&ads_layer_bytes, 1);
    ads_writer.cs.destroy();
    ASSERT_GT(ads_bytes.size(), 10U);
    B2CompressedSource ads_source(ads_bytes);
    B2CodestreamGuard ads_reader;
    ads_reader.cs.create(&ads_source);
    EXPECT_TRUE(ads_reader.cs.exists());
    ads_reader.cs.destroy();
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc << ") at stage '" << stage
           << "': " << GlobalErrorHandler().last_message();
  }
}

TEST(ParamsBatchB2Test, CustomAtkKernelDerivedRangesWeightedStepsAndQcd) {
  try {
    // 1. Reversible custom ATK kernel with deep levels & Qguard=0 -> guard adj.
    {
      std::unique_ptr<siz_params> rev_atk =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          rev_atk.get(),
          "Ssize={128,128}\nSsampling={1,1}\nScomponents=1\nSprecision=12\n"
          "Ssigned=no\nCreversible=yes\nClevels=4\nQguard=0\nCatk=2\n"
          "Kreversible:I2=yes\nKextension:I2=CON\n"
          "Ksteps:I2={1,0,0,0},{1,0,1,1}\nKcoeffs:I2=-1.0,0.5"));
      rev_atk->finalize_all();
      kdu_params* qcd = rev_atk->access_cluster(QCD_params);
      ASSERT_NE(qcd, nullptr);
      int guard = 0;
      EXPECT_TRUE(qcd->get(Qguard, 0, 0, guard));
      EXPECT_GE(guard, 0);

      // Copy reversible Qabs_ranges with discarded resolution levels.
      std::unique_ptr<siz_params> dst =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      dst->copy_all(rev_atk.get(), /*skip_components=*/0,
                    /*discard_levels=*/1, false, false, false);
      int range_ll = 0;
      EXPECT_TRUE(
          dst->access_cluster(QCD_params)->get(Qabs_ranges, 0, 0, range_ll));
    }

    // 2. Irreversible custom ATK kernel with Clev_weights & Cband_weights.
    {
      std::unique_ptr<siz_params> w_atk =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          w_atk.get(),
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=2\nQstep=0.05\nQderived=no\n"
          "Cweight=1.25\nClev_weights=1.0,2.0\n"
          "Cband_weights=1.0,1.2,1.4,1.5,1.6,1.8\nCatk=3\n"
          "Kreversible:I3=no\nKsymmetric:I3=no\n"
          "Ksteps:I3={1,0,0,0},{1,-1,0,0}\nKcoeffs:I3=-0.5,0.25"));
      w_atk->finalize_all();
      float ll_step = 0.0F;
      EXPECT_TRUE(
          w_atk->access_cluster(QCD_params)->get(Qabs_steps, 0, 0, ll_step));
      EXPECT_GT(ll_step, 0.0F);
    }

    // 3. Explicit Qabs_steps with Qderived=yes vs Qderived=no & custom ATK.
    {
      std::unique_ptr<siz_params> der =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          der.get(),
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=2\nQderived=yes\n"
          "Qabs_steps=0.04\nCatk=7\nKreversible:I7=no\nKextension:I7=SYM\n"
          "Ksteps:I7={2,0,0,0},{2,-1,0,0}\n"
          "Kcoeffs:I7=-0.5,-0.5,0.25,0.25\nScap=P15\nCmodes=HT\nSCP15_magb=1"));
      der->finalize_all();
      float ll_step = 0.0F;
      EXPECT_TRUE(
          der->access_cluster(QCD_params)->get(Qabs_steps, 0, 0, ll_step));
      EXPECT_GT(ll_step, 0.0F);
    }

    // 4. Explicit Qabs_steps / Qabs_ranges (valid + invalid error branches).
    {
      std::unique_ptr<siz_params> expl =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          expl.get(),
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1\nQderived=no\n"
          "Qabs_steps=0.05,0.03,0.03,0.02"));
      expl->finalize_all();

      std::unique_ptr<siz_params> rev_expl =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          rev_expl.get(),
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=yes\nClevels=1\n"
          "Qabs_ranges=10,11,11,12"));
      rev_expl->finalize_all();

      ScopedQuietErrors quiet;
      // Mismatched Qabs_steps count throws.
      std::unique_ptr<siz_params> bad_steps =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          bad_steps.get(),
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1\nQderived=no\n"
          "Qabs_steps=0.05,0.03"));
      EXPECT_THROW(bad_steps->finalize_all(), kdu_exception);

      // Qderived=yes with reversible + Qabs_steps resets Qderived to false.
      std::unique_ptr<siz_params> rev_der =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          rev_der.get(),
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=yes\nClevels=1\nQderived=yes\n"
          "Qabs_steps=0.05"));
      rev_der->finalize_all();
      bool qderived = true;
      EXPECT_TRUE(
          rev_der->access_cluster(QCD_params)->get(Qderived, 0, 0, qderived));
      EXPECT_FALSE(qderived);

      // Incomplete Qabs_ranges count is recomputed when finalizing reversible
      // QCD.
      std::unique_ptr<siz_params> partial_ranges =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          partial_ranges.get(),
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=yes\nClevels=1\nQabs_ranges=10,11"));
      partial_ranges->finalize_all();
      int recomputed_range = 0;
      EXPECT_TRUE(partial_ranges->access_cluster(QCD_params)
                      ->get(Qabs_ranges, 3, 0, recomputed_range));
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

TEST(ParamsBatchB2Test, SizPrfCpfCapProfilesSncapAndOrgFinalize) {
  try {
    // 1. PRF (6-byte & 8-byte), CPF (8-byte), CAP (P2+P15+P17),
    // copy_with_xforms.
    const char* const kProfileSpecs[] = {
        // Sprofile=PART1_UNREC with 4095 < Sprf_num < 69631 -> 6-byte PRF.
        "Ssize={64,64}\nSsampling={1,1}\nScomponents=3\nSprecision=8\n"
        "Ssigned=no\nSprofile=PART1_UNREC\nSprf_num=5000\n"
        "Creversible=no\nClevels=1",
        // Large Sprf_num (>= 4095 + 65536) + Scpf_num + SCP15_caps -> 8-byte
        // PRF/CPF.
        "Ssize={64,64}\nSsampling={1,1}\nScomponents=3\nSprecision=8\n"
        "Ssigned=no\nSprofile=OTHER_UNREC\nSprf_num=70000\n"
        "Scap=P15\nCmodes=HT\nSCP15_magb=12\nSCP15_limb=16\n"
        "SCP15_caps=IRV|HET\nScpf_profile=PART1_UNREC\nScpf_num=70000\n"
        "Creversible=no\nClevels=1",
        // BROADCAST single-tile with SCP2_caps and xform copy.
        "Ssize={1080,1920}\nSsampling={1,1}\nScomponents=3\nSprecision=10\n"
        "Ssigned=no\nSprofile=BROADCAST\nSbroadcast={5,single,irrev}\n"
        "Creversible=no\nClevels=3\nCblk={64,64}\nORGtparts=C",
        // IMF reversible with SCP15 CPF + xform copy.
        "Ssize={1080,1920}\nSsampling={1,1}\nScomponents=3\nSprecision=10\n"
        "Ssigned=no\nScap=P15\nCmodes=HT\nSCP15_magb=14\nSCP15_limb=16\n"
        "Scpf_profile=IMF\nSimf={4,2,irrev}\nCreversible=no\nClevels=3\n"
        "Cblk={32,32}\nCprecincts={256,256},{128,128}\nORGtparts=C",
        // Seprofile=IMFHT omitting Corder / ORGtparts so finalize auto-fills.
        "Ssize={1080,1920}\nSsampling={1,1}\nScomponents=3\nSprecision=10\n"
        "Ssigned=no\nSeprofile=IMFHT\nScap=P15\nCreversible=no\nClevels=3\n"
        "Cblk={32,64}\nCprecincts={256,256},{128,128}\nORGgen_tlm=1",
    };

    for (const char* spec : kProfileSpecs) {
      SCOPED_TRACE(spec);
      std::unique_ptr<siz_params> src =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(src.get(), spec));
      src->finalize_all();

      VectorOutput out;
      int dry_flags = 0;
      int dry = src->generate_marker_segments(nullptr, -1, 0, dry_flags);
      int flags = 0;
      int written = src->generate_marker_segments(&out, -1, 0, flags);
      out.Flush();
      EXPECT_GE(dry, written);
      EXPECT_GT(out.bytes().size(), 0U);

      std::unique_ptr<siz_params> dst = BuildFullCluster(
          /*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0,
          /*include_enc=*/false);
      for (const RawMarkerSegment& seg : SplitMarkerSegments(out.bytes())) {
        kdu_byte* ptr =
            seg.body.empty() ? nullptr : const_cast<kdu_byte*>(seg.body.data());
        dst->translate_marker_segment(
            seg.code, static_cast<int>(seg.body.size()), ptr, -1, 0);
      }
      dst->finalize_all(true);

      // Copy into transformed siz cluster to cover broadcast/IMF/SCP15 xforms.
      std::unique_ptr<siz_params> xformed = BuildFullCluster(
          /*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0,
          /*include_enc=*/false);
      xformed->copy_all(src.get(), 0, 0, true, false, false);
    }

    // 2. Explicit Sallowed / Srequired / Smodes_allowed / Smodes_required /
    // Slevels_allowed passing case + Sncap / capability error branches.
    {
      std::unique_ptr<siz_params> req =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          req.get(),
          "Ssize={128,128}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nSallowed=EPH|SOP|RGN|PPM|PPT|POC|HET|CHET|RHET|PHET|"
          "LYRS|FRAG\nSrequired=EPH|SOP|TLM|PLT|CPRL|REV|P256|PSMALL|PgeCB|"
          "CB5X5|CBto6|CBsq56\nSmodes_allowed=BYPASS|CAUSAL\n"
          "Smodes_required=CAUSAL\nSlevels_allowed={1,3}\nCreversible=yes\n"
          "Clevels=2\nCorder=CPRL\nCblk={32,32}\n"
          "Cprecincts={256,256},{128,128}\nCuse_sop=yes\nCuse_eph=yes\n"
          "ORGgen_tlm=1\nORGgen_plt=yes"));
      req->finalize_all();
    }

    ScopedQuietErrors quiet;
    // Exercise profile requirement/capability adjustments, Sncap conflicts, and
    // clamping paths (SCP15_limb < 2, Slevels_allowed, Srequired,
    // Smodes_allowed).
    const char* const kCompatSpecs[] = {
        // Sncap=P2 with Part-2 feature.
        "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
        "Ssigned=no\nSncap=P2\nScap=P2\nCreversible=no\nClevels=1\n"
        "Cdecomp=B(H--:H--:H--)",
        // Sncap=P15 with HT mode.
        "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
        "Ssigned=no\nSncap=P15\nScap=P15\nCreversible=no\nClevels=1\n"
        "Cmodes=HT",
        // SCP15_limb < 2 clamps limit to 2.
        "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
        "Ssigned=no\nScap=P15\nCmodes=HT\nSCP15_limb=1\n"
        "Creversible=no\nClevels=1",
        // Slevels_allowed below Clevels adjusts Clevels or warns.
        "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
        "Ssigned=no\nSlevels_allowed={2,3}\nCreversible=no\nClevels=1",
        // Srequired=REV / IRV / CB5X5 / Smodes_allowed exercise cod_params.
        "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
        "Ssigned=no\nSrequired=REV\nCreversible=no\nClevels=1",
        "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
        "Ssigned=no\nSrequired=IRV\nCreversible=yes\nClevels=1",
        "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
        "Ssigned=no\nSrequired=CB5X5\nCreversible=no\nClevels=1\nCblk={64,64}",
        "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
        "Ssigned=no\nSmodes_allowed=BYPASS\nCmodes=CAUSAL\n"
        "Creversible=no\nClevels=1",
    };
    for (const char* compat_spec : kCompatSpecs) {
      SCOPED_TRACE(compat_spec);
      std::unique_ptr<siz_params> p =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(p.get(), compat_spec));
      try {
        p->finalize_all();
      } catch (kdu_exception) {
        // Sncap conflicts throw while adjustment branches succeed.
      }
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

TEST(ParamsBatchB2Test, CoreLinkDestroyGetSetParseTextualizeAdsDfsRgn) {
  try {
    // 1. Delete non-first cluster and non-first instance individually.
    {
      siz_params* s = siz_params::create();
      cod_params* c0 = cod_params::create();
      mct_params* m0 = mct_params::create();
      c0->link(s, -1, -1, 1, 1);
      m0->link(s, -1, -1, 1, 0);
      // Create a second instance and delete non-first instance + non-first
      // cluster individually before destroying root.
      ASSERT_TRUE(m0->parse_string("Mvector_size:I2=3"));
      kdu_params* m2 = m0->access_relation(-1, -1, 2);
      ASSERT_NE(m2, nullptr);
      m2->destroy();
      m0->destroy();
      c0->destroy();
      s->destroy();
    }

    // 2. link error paths + get/set wrong types/fields/enums/flags/comp checks.
    {
      struct ParamsDestroy {
        void operator()(kdu_params* obj) const {
          if (obj != nullptr) obj->destroy();
        }
      };
      ScopedQuietErrors quiet;
      std::unique_ptr<siz_params> p =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/2, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          p.get(),
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=2\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1"));

      // Diversity mismatch and valid multi-instance MCT link (instance 1).
      std::unique_ptr<mct_params, ParamsDestroy> bad_div_mct(
          mct_params::create());
      EXPECT_THROW(bad_div_mct->link(p.get(), -1, 0, 1, 1), kdu_exception);
      std::unique_ptr<mct_params, ParamsDestroy> inst1_mct(
          mct_params::create());
      inst1_mct->link(p.get(), -1, -1, 1, 0);
      // p now owns inst1_mct via its MCT cluster instance list.
      inst1_mct.release();

      kdu_params* cod = p->access_cluster(COD_params);
      kdu_params* cod_c0 = cod->access_relation(-1, 0);
      int ival = 0;
      bool bval = false;
      float fval = 0.0F;
      EXPECT_THROW(cod->get("NoSuchAttr", 0, 0, ival), kdu_exception);
      EXPECT_THROW(cod->get("NoSuchAttr", 0, 0, bval), kdu_exception);
      EXPECT_THROW(cod->get("NoSuchAttr", 0, 0, fval), kdu_exception);
      EXPECT_THROW(cod->get(Clevels, 0, 1, ival), kdu_exception);
      EXPECT_THROW(cod->get(Cycc, 0, 1, bval), kdu_exception);
      EXPECT_THROW(cod->get(Cweight, 0, 1, fval), kdu_exception);
      EXPECT_THROW(cod->get(Cweight, 0, 0, ival), kdu_exception);
      EXPECT_THROW(cod->get(Clevels, 0, 0, bval), kdu_exception);
      EXPECT_THROW(cod->get(Clevels, 0, 0, fval), kdu_exception);

      EXPECT_THROW(cod->set("NoSuchAttr", 0, 0, 1), kdu_exception);
      EXPECT_THROW(cod->set("NoSuchAttr", 0, 0, true), kdu_exception);
      EXPECT_THROW(cod->set("NoSuchAttr", 0, 0, 1.0, false), kdu_exception);
      EXPECT_THROW(cod->set(Corder, 0, 0, 999), kdu_exception);
      EXPECT_THROW(cod->set(Cmodes, 0, 0, 1 << 20), kdu_exception);
      EXPECT_THROW(cod->set(Clevels, 0, 0, true), kdu_exception);
      EXPECT_THROW(cod->set(Clevels, 0, 0, 1.0, false), kdu_exception);
      EXPECT_THROW(cod_c0->set(Cycc, 0, 0, true), kdu_exception);
      EXPECT_THROW(cod_c0->set(Corder, 0, 0, 0), kdu_exception);
      EXPECT_THROW(p->access_cluster(ENC_params)
                       ->access_relation(-1, 0)
                       ->set(Qfactor, 0, 0, 50.0, false),
                   kdu_exception);

      // parse_string error branches (bad suffixes, malformed
      // values/braces/enum).
      EXPECT_THROW(p->parse_string("Clevels:T99=1"), kdu_exception);
      EXPECT_THROW(p->parse_string("Clevels:C99=1"), kdu_exception);
      EXPECT_THROW(p->parse_string("Clevels:I2=1"), kdu_exception);
      EXPECT_THROW(p->parse_string("Clevels=abc"), kdu_exception);
      EXPECT_THROW(p->parse_string("Cycc=maybe"), kdu_exception);
      EXPECT_THROW(p->parse_string("Cweight=notfloat"), kdu_exception);
      EXPECT_THROW(p->parse_string("Corder=INVALID"), kdu_exception);
      EXPECT_THROW(p->parse_string("Cmodes=INVALID_FLAG"), kdu_exception);
      EXPECT_THROW(p->parse_string("Cblk={32,32"), kdu_exception);
      EXPECT_THROW(p->parse_string("Cblk={32}"), kdu_exception);
      EXPECT_THROW(p->parse_string("Cdecomp=INVALID"), kdu_exception);
    }

    // 3. textualize_attributes (both overloads) + cod/ads
    // custom_textualize_field
    // + valid/invalid terminal Cdecomp + DOads/DSads.
    {
      std::unique_ptr<siz_params> tx =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          tx.get(),
          "Ssize={64,64}\nSsampling={1,1}\nStiles={32,64}\nScomponents=1\n"
          "Sprecision=8\nSsigned=no\nCreversible=no\nClevels=2\n"
          "Cdecomp=B(V--:H--:V--),B(-:-:-)\nCorder:T1=RLCP"));
      tx->finalize_all();
      StringMessage siz_msg;
      tx->textualize_attributes(siz_msg, /*skip_derived=*/false);
      EXPECT_NE(siz_msg.text().find("Ssize="), std::string::npos);

      StringMessage cod_msg;
      tx->access_cluster(COD_params)
          ->textualize_attributes(cod_msg, /*skip_derived=*/false);
      EXPECT_NE(cod_msg.text().find("Cdecomp="), std::string::npos);

      kdu_params* ads_inst =
          tx->access_cluster(ADS_params)->access_relation(-1, -1, 1, false);
      ASSERT_NE(ads_inst, nullptr);
      {
        StringMessage ads_msg;
        ads_inst->textualize_attributes(ads_msg, /*skip_derived=*/false);
        EXPECT_FALSE(ads_msg.text().empty());
      }

      StringMessage all_msg;
      tx->textualize_attributes(all_msg, /*min_tile=*/-1, /*max_tile=*/1,
                                /*skip_derived=*/false);
      EXPECT_NE(all_msg.text().find("Ssize="), std::string::npos);
      EXPECT_NE(all_msg.text().find("Cdecomp="), std::string::npos);

      StringMessage tile_msg;
      tx->textualize_attributes(tile_msg, /*min_tile=*/0, /*max_tile=*/1,
                                /*skip_derived=*/true);
      EXPECT_NE(tile_msg.text().find("Corder:T1=RLCP"), std::string::npos);

      // Round-trip ADS marker segments generated from tx into a reader cluster.
      std::unique_ptr<siz_params> ads_reader = BuildFullCluster(
          /*total_tiles=*/2, /*num_comps=*/1, /*nlt_comps=*/0,
          /*include_enc=*/false);
      RoundTripMarkers(tx.get(), ads_reader.get(), /*total_tiles=*/2);
      int ads_idx_read = 0;
      EXPECT_TRUE(ads_reader->access_cluster(COD_params)
                      ->access_relation(0, 0)
                      ->get(Cads, 0, 0, ads_idx_read));
      EXPECT_GE(ads_idx_read, 1);

      // Invalid terminal Cdecomp & malformed ADS / DFS markers.
      ScopedQuietErrors quiet;
      std::unique_ptr<siz_params> bad_term =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          bad_term.get(),
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1\n"
          "Cdecomp=B(H--:V--:H--)"));
      EXPECT_THROW(bad_term->finalize_all(), kdu_exception);

      std::unique_ptr<siz_params> mp =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      kdu_byte ads_short[] = {1, 2};
      EXPECT_THROW(mp->translate_marker_segment(KDU_ADS, 2, ads_short, -1, 0),
                   kdu_exception);
      kdu_byte ads_long[] = {1, 1, 0x00, 1, 0x00, 0x99};
      EXPECT_THROW(mp->translate_marker_segment(KDU_ADS, 6, ads_long, -1, 0),
                   kdu_exception);
      kdu_byte dfs_short[] = {0, 1, 5};
      EXPECT_THROW(mp->translate_marker_segment(KDU_DFS, 3, dfs_short, -1, 0),
                   kdu_exception);
      kdu_byte dfs_long[] = {0, 1, 1, 0x00, 0x99};
      EXPECT_THROW(mp->translate_marker_segment(KDU_DFS, 5, dfs_long, -1, 0),
                   kdu_exception);
    }

    // 4. RGN match last_marked, Rshift > 37 warning, Rshift > 255 / disallowed.
    {
      std::unique_ptr<siz_params> rgn =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          rgn.get(),
          "Ssize={64,64}\nSsampling={1,1}\nStiles={32,64}\nScomponents=1\n"
          "Sprecision=8\nSsigned=no\nCreversible=no\nClevels=1\n"
          "Rshift=5\nRshift:T0C0=5\nRshift:T1C0=40"));
      ScopedQuietErrors quiet;
      rgn->finalize_all();
      VectorOutput out_main, out_t0;
      int flags = KDU_ParamsClass_RGN;
      rgn->generate_marker_segments(&out_main, -1, 0, flags);
      flags = KDU_ParamsClass_RGN;
      rgn->generate_marker_segments(&out_t0, 0, 0, flags);

      // Disallowed RGN throws on write_marker_segment.
      std::unique_ptr<siz_params> rgn_disallowed =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          rgn_disallowed.get(),
          "Ssize={32,32}\nSsampling={1,1}\nStiles={32,32}\nScomponents=1\n"
          "Sprecision=8\nSsigned=no\nCreversible=no\nClevels=1\n"
          "Sallowed=EPH\nRshift=5"));
      rgn_disallowed->finalize_all();
      VectorOutput out_bad;
      flags = KDU_ParamsClass_RGN;
      EXPECT_THROW(
          rgn_disallowed->generate_marker_segments(&out_bad, -1, 0, flags),
          kdu_exception);

      // Rshift > 255 throws on write_marker_segment.
      std::unique_ptr<siz_params> rgn_overflow =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          rgn_overflow.get(),
          "Ssize={32,32}\nSsampling={1,1}\nStiles={32,32}\nScomponents=1\n"
          "Sprecision=8\nSsigned=no\nCreversible=no\nClevels=1\n"
          "Sallowed=RGN\nRshift=300"));
      rgn_overflow->finalize_all();
      flags = KDU_ParamsClass_RGN;
      EXPECT_THROW(
          rgn_overflow->generate_marker_segments(&out_bad, -1, 0, flags),
          kdu_exception);
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

TEST(ParamsBatchB3Test, Fix16CustomAtkWeightedStepsAndQcdWarnings) {
  // 1. derive_weighted_steps with custom ATK kernel & Qfix16=LIMIT with custom
  // ATK kernel + small Qstep forcing k_max > 15 clamp on subbands and band 0.
  try {
    {
      std::unique_ptr<siz_params> wt_atk =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          wt_atk.get(),
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=12\n"
          "Ssigned=no\nCreversible=no\nClevels=2\nCdecomp=H(-),V(-)\n"
          "Catk=2\nKreversible:I2=no\nKsymmetric:I2=yes\nKextension:I2=SYM\n"
          "Ksteps:I2={2,0,0,0},{2,-1,0,0}\n"
          "Kcoeffs:I2=-0.5,-0.5,0.25,0.25\n"
          "Qweights=0.5\nCweight=0.75\nClev_weights=1.2,1.0\n"
          "Cband_weights=0.8,0.9,1.0"));
      wt_atk->finalize_all();
      float step0 = 0.0f;
      EXPECT_TRUE(
          wt_atk->access_cluster(QCD_params)->get(Qabs_steps, 0, 0, step0));
      EXPECT_GT(step0, 0.0f);
    }

    {
      std::unique_ptr<siz_params> fix_atk =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          fix_atk.get(),
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=12\n"
          "Ssigned=no\nCreversible=no\nClevels=2\nQguard=7\n"
          "Catk=2\nKreversible:I2=no\nKsymmetric:I2=yes\nKextension:I2=SYM\n"
          "Ksteps:I2={2,0,0,0},{2,-1,0,0}\n"
          "Kcoeffs:I2=-0.5,-0.5,0.25,0.25\n"
          "Qstep=0.00001\nQfix16=LIMIT"));
      fix_atk->finalize_all();
      float step0 = 0.0f;
      EXPECT_TRUE(
          fix_atk->access_cluster(QCD_params)->get(Qabs_steps, 0, 0, step0));
      EXPECT_GT(step0, 0.0f);
    }

    // 2. Qderived + Qweights / Qfactor error; Qstep + Qabs_steps warning.
    {
      ScopedQuietErrors quiet;
      std::unique_ptr<siz_params> bad_w =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          bad_w.get(),
          "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1\nQderived=yes\nQweights=1.0"));
      EXPECT_THROW(bad_w->finalize_all(), kdu_exception);

      std::unique_ptr<siz_params> bad_f =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          bad_f.get(),
          "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1\nQderived=yes\nQfactor=0.5"));
      EXPECT_THROW(bad_f->finalize_all(), kdu_exception);

      std::unique_ptr<siz_params> warn_abs =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          warn_abs.get(),
          "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1\nQstep=0.01\n"
          "Qabs_steps=0.05"));
      warn_abs->finalize_all();
    }

    // 3. Reversible low precision (<5) + 3 comps (RCT +1) + max_eps > 31 guard
    // adjustment, and IMFHT MagB calculation with Cycc=yes and Cycc=no.
    {
      ScopedQuietErrors quiet;
      std::unique_ptr<siz_params> low_prec =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          low_prec.get(),
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=3\nSprecision=3\n"
          "Ssigned=no\nCreversible=yes\nClevels=8\nQguard=1\nCycc=yes"));
      low_prec->finalize_all();

      for (const char* ycc : {"yes", "no"}) {
        std::unique_ptr<siz_params> imfht = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
        std::string spec =
            "Ssize={256,256}\nSsampling={1,1}\nScomponents=3\n"
            "Sprecision=10\nSsigned=no\nSeprofile=IMFHT\n"
            "Creversible=yes\nClevels=6\nCblk={64,64}\n"
            "Cprecincts={256,256}\nCorder=PCRL\nORGtparts=R\n"
            "ORGgen_tlm=1\nCycc=";
        spec += ycc;
        ASSERT_TRUE(ParseLines(imfht.get(), spec.c_str()));
        imfht->finalize_all();
        int magb = 0;
        EXPECT_TRUE(imfht->get(SCP15_magb, 0, 0, magb));
        EXPECT_GE(magb, 14);
      }
    }

    // 4. Multi-tile/comp QCD/QCC marker suppression, override & xform
    // transpose.
    {
      std::unique_ptr<siz_params> src =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/2, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          src.get(),
          "Ssize={64,64}\nSsampling={1,1}\nStiles={32,64}\nScomponents=2\n"
          "Sprecision=8\nSsigned=no\nCreversible=no\nClevels=2\n"
          "Cdecomp=H(-),V(-)\nQguard=2\nQguard:T0=3\nQguard:T1C1=2"));
      src->finalize_all();
      std::unique_ptr<siz_params> dst = BuildFullCluster(
          /*total_tiles=*/2, /*num_comps=*/2, /*nlt_comps=*/0,
          /*include_enc=*/false);
      RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/2);

      std::unique_ptr<siz_params> xformed =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/2, /*nlt_comps=*/0);
      xformed->copy_all(src.get(), /*skip_components=*/0, /*discard_levels=*/1,
                        /*transpose=*/true, /*vflip=*/false, /*hflip=*/false);
      xformed->finalize_all();
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

TEST(ParamsBatchB3Test, McoMccMctCopyWithXformsTransposeDecompAndNlt) {
  try {
    // 1. copy_with_xforms with skip_components > 0 across poc/crg/nlt.
    // Note: Kakadu's mco_params::copy_with_xforms() has an upstream null-ptr
    // bug when Mnum_stages > 0 and (skip_components > 0 || Scomponents
    // changes), so skip_components > 0 is exercised without multi-stage MCO
    // stages.
    {
      std::unique_ptr<siz_params> src =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/4, /*nlt_comps=*/4);
      ASSERT_TRUE(ParseLines(
          src.get(),
          "Ssize={64,64}\nSsampling={1,1}\nStiles={32,64}\nScomponents=4\n"
          "Sprecision=8\nSsigned=no\nCreversible=no\nClevels=1\nCycc=no\n"
          "CRGoffset={0.25,0.5},{0.5,0.25},{0.1,0.2},{0.3,0.4}\n"
          "Porder={0,0,1,2,4,LRCP}"));
      src->finalize_all();

      // Copy skipping 1 component and skipping 3 components.
      std::unique_ptr<siz_params> dst_skip1 =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/3, /*nlt_comps=*/3);
      dst_skip1->copy_all(src.get(), /*skip_components=*/1);
      dst_skip1->finalize_all();

      std::unique_ptr<siz_params> dst_skip3 =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/1, /*nlt_comps=*/1);
      dst_skip3->copy_all(src.get(), /*skip_components=*/3);
      dst_skip3->finalize_all();
    }

    // 2. mco_params::copy_with_xforms (skip=0) & write_marker_segment tile
    // match/mismatch + overflow error.
    {
      std::unique_ptr<siz_params> mco =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/3, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          mco.get(),
          "Ssize={64,64}\nSsampling={1,1}\nStiles={32,64}\nScomponents=3\n"
          "Sprecision=8\nSsigned=no\nMcomponents=3\nMprecision=8\nMsigned=no\n"
          "Creversible=no\nClevels=1\nCycc=no\n"
          "Mmatrix_size:I1=9\nMmatrix_coeffs:I1=1,0,0,0,1,0,0,0,1\n"
          "Mstage_inputs:I1={0,2}\nMstage_outputs:I1={0,2}\n"
          "Mstage_collections:I1={3,3}\nMstage_xforms:I1={MATRIX,1,0,0,0}\n"
          "Mmatrix_size:I2=9\nMmatrix_coeffs:I2=1,0,0,0,1,0,0,0,1\n"
          "Mstage_inputs:I2={0,2}\nMstage_outputs:I2={0,2}\n"
          "Mstage_collections:I2={3,3}\nMstage_xforms:I2={MATRIX,2,0,0,0}\n"
          "Mnum_stages=1\nMstages=1\nMnum_stages:T0=1\nMstages:T0=1\n"
          "Mnum_stages:T1=1\nMstages:T1=2"));
      mco->finalize_all();
      std::unique_ptr<siz_params> mco_copy =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/3, /*nlt_comps=*/0);
      mco_copy->copy_all(mco.get(), /*skip_components=*/0);
      mco_copy->finalize_all();
      std::unique_ptr<siz_params> mco_dst = BuildFullCluster(
          /*total_tiles=*/2, /*num_comps=*/3, /*nlt_comps=*/0,
          /*include_enc=*/false);
      RoundTripMarkers(mco.get(), mco_dst.get(), /*total_tiles=*/2);

      ScopedQuietErrors quiet;
      std::unique_ptr<siz_params> mco_bad =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(mco_bad->parse_string("Mnum_stages=256"));
      VectorOutput out;
      int flags = KDU_ParamsClass_CODING;
      EXPECT_THROW(mco_bad->access_cluster(MCO_params)
                       ->generate_marker_segments(&out, -1, 0, flags),
                   kdu_exception);
    }

    // 3. mcc_params::read_marker_segment b/336206504 & finalize errors.
    {
      ScopedQuietErrors quiet;
      std::unique_ptr<siz_params> mcc =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/2, /*nlt_comps=*/0);
      kdu_params* mcc_i1 =
          mcc->access_cluster(MCC_params)->access_relation(-1, -1, 1, false);
      ASSERT_NE(mcc_i1, nullptr);
      kdu_byte short_mcc[] = {0, 0, 1, 0};
      EXPECT_THROW(
          mcc_i1->translate_marker_segment(KDU_MCC, 4, short_mcc, -1, 0),
          kdu_exception);
      kdu_byte nz_mcc[] = {0, 1, 1, 0, 0};
      EXPECT_THROW(mcc_i1->translate_marker_segment(KDU_MCC, 5, nz_mcc, -1, 0),
                   kdu_exception);

      std::unique_ptr<siz_params> bad_in =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/2, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          bad_in.get(),
          "Ssize={32,32}\nSsampling={1,1}\nScomponents=2\nSprecision=8\n"
          "Ssigned=no\nMcomponents=2\nMprecision=8\nMsigned=no\n"
          "Mstage_inputs:I1={5,2}"));
      EXPECT_THROW(bad_in->finalize_all(), kdu_exception);

      std::unique_ptr<siz_params> bad_out =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/2, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          bad_out.get(),
          "Ssize={32,32}\nSsampling={1,1}\nScomponents=2\nSprecision=8\n"
          "Ssigned=no\nMcomponents=2\nMprecision=8\nMsigned=no\n"
          "Mstage_inputs:I1={0,1}\nMstage_outputs:I1={5,2}"));
      EXPECT_THROW(bad_out->finalize_all(), kdu_exception);
    }

    // 4. cod_params::transpose_decomp, is_valid_decomp_terminator, and
    // find_suitable_ads_idx reuse + mismatch across 3 components.
    {
      EXPECT_EQ(cod_params::transpose_decomp(0), 0);
      const int all_b = 3 | (3 << 2) | (3 << 12) | (3 << 22);
      EXPECT_NE(cod_params::transpose_decomp(all_b), 0);
      EXPECT_TRUE(cod_params::is_valid_decomp_terminator(0));
      EXPECT_FALSE(
          cod_params::is_valid_decomp_terminator(3 | (1 << 2) | (2 << 12)));
      EXPECT_FALSE(cod_params::is_valid_decomp_terminator(
          3 | (1 << 2) | (1 << 12) | (2 << 22)));
      EXPECT_FALSE(cod_params::is_valid_decomp_terminator((1 << 2) | (1 << 4)));
      EXPECT_TRUE(
          cod_params::is_valid_decomp_terminator((1 << 2) | (0x05 << 4)));
      EXPECT_FALSE(cod_params::is_valid_decomp_terminator((2 << 2) | (1 << 4)));
      EXPECT_TRUE(
          cod_params::is_valid_decomp_terminator((2 << 2) | (0x0C << 4)));
      EXPECT_FALSE(cod_params::is_valid_decomp_terminator((3 << 2) | (1 << 4)));
      EXPECT_TRUE(
          cod_params::is_valid_decomp_terminator((3 << 2) | (0xFF << 4)));

      std::unique_ptr<siz_params> ads =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          ads.get(),
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=3\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=3\n"
          "Cdecomp:T0C0=B(V--:H--:V--),B(H--:V--:H--),B(-:-:-)\n"
          "Cdecomp:T0C1=B(V--:H--:V--),B(H--:V--:H--),B(-:-:-)\n"
          "Cdecomp:T0C2=B(HHH:HHH:HHH),B(H--:V--:H--),B(-:-:-)"));
      ads->finalize_all();
      int ads0 = 0, ads1 = 0, ads2 = 0;
      kdu_params* cod = ads->access_cluster(COD_params);
      EXPECT_TRUE(cod->access_relation(0, 0)->get(Cads, 0, 0, ads0));
      EXPECT_TRUE(cod->access_relation(0, 1)->get(Cads, 0, 0, ads1));
      EXPECT_TRUE(cod->access_relation(0, 2)->get(Cads, 0, 0, ads2));
      EXPECT_EQ(ads0, ads1);
      EXPECT_NE(ads0, ads2);

      std::unique_ptr<siz_params> xform =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
      xform->copy_all(ads.get(), /*skip_components=*/0, /*discard_levels=*/0,
                      /*transpose=*/true);
      xform->finalize_all();
    }

    // 5. nlt_params::write_marker_segment LUT 12-bit / 24-bit & error checks.
    {
      std::unique_ptr<siz_params> nlt =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/2, /*nlt_comps=*/2);
      ASSERT_TRUE(ParseLines(
          nlt.get(),
          "Ssize={32,32}\nSsampling={1,1}\nStiles={16,32}\nScomponents=2\n"
          "Sprecision=8\nSsigned=no\nCreversible=no\nClevels=1\n"
          "NLType:C0=LUT\nNLTlut:C0={0.0,1.0,4,12}\n"
          "NLTdata:C0=0.0,0.25,0.75,1.0\n"
          "NLType:C1=LUT\nNLTlut:C1={0.0,1.0,4,24}\n"
          "NLTdata:C1=0.0,0.3,0.7,1.0\n"
          "NLType:T0C0=LUT\nNLTlut:T0C0={0.0,1.0,4,12}\n"
          "NLTdata:T0C0=0.0,0.25,0.75,1.0\n"
          "NLType:T1C0=LUT\nNLTlut:T1C0={0.0,1.0,4,12}\n"
          "NLTdata:T1C0=0.1,0.3,0.8,1.0"));
      nlt->finalize_all();
      std::unique_ptr<siz_params> nlt_dst = BuildFullCluster(
          /*total_tiles=*/2, /*num_comps=*/2, /*nlt_comps=*/2,
          /*include_enc=*/false);
      RoundTripMarkers(nlt.get(), nlt_dst.get(), /*total_tiles=*/2);

      ScopedQuietErrors quiet;
      auto check_nlt_err = [](const char* extra_lines) {
        std::unique_ptr<siz_params> p = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/1);
        EXPECT_TRUE(ParseLines(
            p.get(),
            "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
            "Nprecision=8\nNsigned=no"));
        EXPECT_TRUE(ParseLines(p.get(), extra_lines));
        VectorOutput out;
        int flags = KDU_ParamsClass_CODING;
        EXPECT_THROW(p->access_cluster(NLT_params)
                         ->generate_marker_segments(&out, -1, 0, flags),
                     kdu_exception);
      };
      {
        std::unique_ptr<siz_params> p_bad = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/1);
        EXPECT_THROW(p_bad->access_cluster(NLT_params)->set(NLType, 0, 0, 99),
                     kdu_exception);
      }
      check_nlt_err("Ssigned=no\nNLType=GAMMA\nNLTgamma={-1.0,0,0,0,0}");
      check_nlt_err("Ssigned=no\nNLType=LUT\nNLTlut={0.5,0.2,4,8}");
      check_nlt_err("Ssigned=no\nNLType=SMAG");
      std::unique_ptr<siz_params> p_umag = BuildFullCluster(
          /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/1);
      ASSERT_TRUE(ParseLines(
          p_umag.get(),
          "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Nprecision=8\nSsigned=yes\nNsigned=yes\nNLType=UMAG"));
      VectorOutput out_umag;
      int uflags = KDU_ParamsClass_CODING;
      EXPECT_THROW(p_umag->access_cluster(NLT_params)
                       ->generate_marker_segments(&out_umag, -1, 0, uflags),
                   kdu_exception);
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

TEST(ParamsBatchB3Test, SizFinalizeDimsProfilesCapsAndNegativeOriginMarkers) {
  try {
    // 1. Scomponents inference from Ssampling / Sdims & Ssize/Ssampling
    // inference.
    {
      std::unique_ptr<siz_params> from_subs =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          from_subs.get(),
          "Ssize={64,64}\nSsampling={1,1},{2,2},{2,2}\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1"));
      from_subs->finalize_all();
      int dim2 = 0;
      EXPECT_TRUE(from_subs->get(Sdims, 2, 0, dim2));
      EXPECT_EQ(dim2, 32);

      std::unique_ptr<siz_params> from_dims =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
      ASSERT_TRUE(
          ParseLines(from_dims.get(),
                     "Sdims={64,64},{32,32},{32,32}\n"
                     "Sprecision=8\nSsigned=no\nCreversible=no\nClevels=1"));
      from_dims->finalize_all();
      int sx = 0, sy = 0, sub2 = 0;
      EXPECT_TRUE(from_dims->get(Ssize, 0, 0, sy));
      EXPECT_TRUE(from_dims->get(Ssize, 0, 1, sx));
      EXPECT_TRUE(from_dims->get(Ssampling, 2, 0, sub2));
      EXPECT_EQ(sy, 64);
      EXPECT_EQ(sx, 64);
      EXPECT_EQ(sub2, 2);
    }

    // 2. siz_params::finalize error cases.
    {
      ScopedQuietErrors quiet;
      auto expect_fin_err = [](const char* spec) {
        std::unique_ptr<siz_params> p = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/2, /*nlt_comps=*/0);
        EXPECT_TRUE(ParseLines(p.get(), spec));
        EXPECT_THROW(p->finalize_all(), kdu_exception) << "spec:\n" << spec;
      };
      expect_fin_err("Ssize={64,64}\nSprecision=8\nSsigned=no");
      expect_fin_err(
          "Ssize={64,64}\nScomponents=1\nSsampling={0,1}\n"
          "Sprecision=8\nSsigned=no");
      expect_fin_err("Ssize={64,64}\nScomponents=1\nSprecision=8\nSsigned=no");
      expect_fin_err(
          "Scomponents=1\nSsampling={1,1}\nSprecision=8\nSsigned=no");
      expect_fin_err(
          "Sdims={600,600},{599,599}\n"
          "Sprecision=8\nSsigned=no");
      expect_fin_err(
          "Ssize={64,64}\nScomponents=2\nSsampling={1,1}\n"
          "Sdims={32,32},{32,32}\nSprecision=8\nSsigned=no");
      expect_fin_err(
          "Ssize={64,64}\nScomponents=1\nSsampling={1,1}\n"
          "Stiles={-16,16}\nSprecision=8\nSsigned=no");
      expect_fin_err(
          "Ssize={64,64}\nScomponents=1\nSsampling={1,1}\n"
          "Stile_origin={1000,2147483000}\nStiles={1000,1000}\n"
          "Sprecision=8\nSsigned=no");
      expect_fin_err(
          "Ssize={64,64}\nScomponents=1\nSsampling={1,1}\n"
          "Stile_origin={64,0}\nStiles={32,32}\nSprecision=8\nSsigned=no");
    }

    // 3. Profile round-trips via public Sprofile/Sbroadcast/Simf/Sprf_num/Scap
    // (exercises to_prfnum, from_prfnum, set_profiles, and SIZ/PRF/CPF
    // markers).
    {
      ScopedQuietErrors quiet;
      const char* profile_specs[] = {
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nSprofile=PROFILE0\nCprecincts={128,128}",
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nSprofile=PROFILE1",
          "Ssize={1080,1920}\nSsampling={1,1}\nScomponents=3\nSprecision=8\n"
          "Ssigned=no\nSbroadcast={4,single,irrev}\nCreversible=no\n"
          "ORGgen_tlm=1",
          "Ssize={1080,1920}\nSsampling={1,1}\n"
          "Scomponents=3\nSprecision=8\nSsigned=no\n"
          "Sbroadcast={4,multi,irrev}\nCreversible=no\nORGgen_tlm=1",
          "Ssize={1080,1920}\nSsampling={1,1}\n"
          "Scomponents=3\nSprecision=8\nSsigned=no\n"
          "Sbroadcast={4,multi,rev}\nCreversible=yes\nORGgen_tlm=1",
          "Ssize={1080,2048}\nSsampling={1,1}\nScomponents=3\nSprecision=10\n"
          "Ssigned=no\nSimf={5,2,irrev}\nCreversible=no",
          "Ssize={2160,4096}\nSsampling={1,1}\n"
          "Scomponents=3\nSprecision=10\nSsigned=no\nSimf={5,2,rev}\n"
          "Creversible=yes",
          "Ssize={4320,7680}\nSsampling={1,1}\n"
          "Scomponents=3\nSprecision=10\nSsigned=no\nSimf={5,2,irrev}\n"
          "Creversible=no",
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nSprf_num=48879",
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nSprf_num=48879\nScap=P2\nCdecomp=B(V--:H--:V--)",
      };
      for (const char* spec : profile_specs) {
        std::unique_ptr<siz_params> src = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
        try {
          if (ParseLines(src.get(), spec)) {
            src->finalize_all();
            std::unique_ptr<siz_params> dst = BuildFullCluster(
                /*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0,
                /*include_enc=*/false);
            RoundTripMarkers(src.get(), dst.get(), /*total_tiles=*/1);
            int prof = 0;
            EXPECT_TRUE(dst->get(Sprofile, 0, 0, prof));
          }
        } catch (kdu_exception) {
        }
      }
    }

    // 4. Profiles finalize & RoundTripMarkers (CINEMA*, IMF*, IMFHT, BROADCAST,
    // SCP15_magb ranges, and negative canvas origin translation on SIZ write).
    {
      ScopedQuietErrors quiet;
      const char* cinema_specs[] = {
          "Ssize={1080,2048}\nSsampling={1,1}\nScomponents=3\nSprecision=12\n"
          "Ssigned=no\nSprofile=CINEMA2K\nCreversible=no\nClayers=1",
          "Ssize={2160,4096}\nSsampling={1,1}\nScomponents=3\nSprecision=12\n"
          "Ssigned=no\nSprofile=CINEMA4K\nCreversible=no\nClayers=1",
          "Ssize={1080,2048}\nSsampling={1,1}\nScomponents=3\nSprecision=12\n"
          "Ssigned=no\nSprofile=CINEMA2S\nCreversible=no\nClayers=2",
          "Ssize={2160,4096}\nSsampling={1,1}\nScomponents=3\nSprecision=12\n"
          "Ssigned=no\nSprofile=CINEMA4S\nCreversible=no\nClayers=2",
          "Ssize={2160,4096}\nSsampling={1,1}\nScomponents=3\nSprecision=12\n"
          "Ssigned=no\nSprofile=CINEMASS\nCreversible=no\nClayers=2",
          "Ssize={3000,5000}\nSsampling={1,1}\n"
          "Scomponents=3\nSprecision=10\nSsigned=no\nSimf={4,2,rev}\n"
          "Creversible=yes",
          "Ssize={256,256}\nSsampling={1,1}\nScomponents=3\nSprecision=10\n"
          "Ssigned=no\nSeprofile=IMFHT\nCreversible=yes\nClevels=6\n"
          "Cblk={64,64}\nCprecincts={256,256}\nCorder=PCRL\nORGtparts=R\n"
          "ORGgen_tlm=1",
          "Ssize={256,256}\nSsampling={1,1}\nScomponents=3\nSprecision=10\n"
          "Ssigned=no\nORGtparts=C\nORGgen_plt=yes",
      };
      for (const char* spec : cinema_specs) {
        std::unique_ptr<siz_params> p = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(p.get(), spec));
        p->finalize_all();
        std::unique_ptr<siz_params> q = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0,
            /*include_enc=*/false);
        RoundTripMarkers(p.get(), q.get(), /*total_tiles=*/1);
      }

      // CAP SCP15_magb: 20, 32, and 50 (with and without RGN flag).
      for (int magb : {20, 32, 50}) {
        std::unique_ptr<siz_params> cap = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        std::string spec =
            "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
            "Ssigned=no\nCmodes=HT\nSCP15_magb=" +
            std::to_string(magb);
        if (magb == 50) spec += "\nSCP15_caps=RGN";
        ASSERT_TRUE(ParseLines(cap.get(), spec.c_str()));
        cap->finalize_all();
        std::unique_ptr<siz_params> cap_dst = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0,
            /*include_enc=*/false);
        RoundTripMarkers(cap.get(), cap_dst.get(), /*total_tiles=*/1);
      }

      // Negative canvas / tile origin shifting during SIZ write_marker_segment.
      std::unique_ptr<siz_params> neg =
          BuildFullCluster(/*total_tiles=*/4, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          neg.get(),
          "Ssize={28,28}\nSorigin={0,0}\nStiles={16,16}\n"
          "Ssampling={1,1}\nScomponents=1\nSprecision=8\nSsigned=no\n"
          "Creversible=no\nClevels=1\nCprecincts={8,8}\nCuse_precincts=yes"));
      neg->finalize_all();
      neg->set(Sorigin, 0, 0, -4);
      neg->set(Sorigin, 0, 1, -4);
      neg->set(Stile_origin, 0, 0, -4);
      neg->set(Stile_origin, 0, 1, -4);
      VectorOutput out_neg;
      int sflags = KDU_ParamsClass_FIXED;
      neg->generate_marker_segments(&out_neg, -1, 0, sflags);
      out_neg.Flush();
      EXPECT_FALSE(out_neg.bytes().empty());
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

TEST(ParamsBatchB3Test, KduParamsParseStringRecordsCopyFromLinkAndTypicalTile) {
  try {
    // 1. parse_string error cases and tile-filtered parse_string.
    {
      ScopedQuietErrors quiet;
      std::unique_ptr<siz_params> p =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/2, /*nlt_comps=*/0);
      EXPECT_THROW(p->parse_string("Sprecision=8 "), kdu_exception);
      EXPECT_THROW(p->parse_string("Sprecision"), kdu_exception);
      EXPECT_THROW(p->parse_string("Sprecision:Z0=8"), kdu_exception);
      EXPECT_THROW(p->parse_string("Sprecision=abc"), kdu_exception);
      EXPECT_THROW(p->parse_string("Scomponents=1,2"), kdu_exception);
      EXPECT_THROW(p->parse_string("Ssize={32,32"), kdu_exception);
      EXPECT_THROW(p->parse_string("Corder=INVALID_ORD"), kdu_exception);

      EXPECT_FALSE(p->parse_string("Corder:T1=RLCP", /*which_tile=*/0));
      EXPECT_TRUE(p->parse_string("Corder:T1=RLCP", /*which_tile=*/1));
    }

    // 2. count_records, truncate_records, find_string, set error checks.
    {
      ScopedQuietErrors quiet;
      std::unique_ptr<siz_params> p =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/2, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          p.get(),
          "Ssize={64,64}\nSsampling={1,1}\nStiles={32,64}\nScomponents=2\n"
          "Sprecision=8\nSsigned=no\nCprecincts={64,64},{32,32},{16,16}\n"
          "Creversible=no\nClevels=2\nQstep=0.02\nQstep:T0C1=0.03"));
      kdu_params* cod = p->access_cluster(COD_params);
      EXPECT_EQ(cod->count_records("Cprecincts", false, true), 3);
      EXPECT_EQ(
          cod->access_relation(0, 1)->count_records("Cprecincts", true, true),
          3);
      EXPECT_EQ(
          cod->access_relation(0, 1)->count_records("Cprecincts", false, true),
          0);
      EXPECT_THROW(cod->count_records("NoSuchAttr"), kdu_exception);

      EXPECT_THROW(cod->truncate_records("NoSuchAttr", 0), kdu_exception);
      EXPECT_TRUE(cod->truncate_records("Cprecincts", 1));
      EXPECT_EQ(cod->count_records("Cprecincts", false, true), 1);
      EXPECT_TRUE(cod->truncate_records("Cprecincts", -1));
      EXPECT_EQ(cod->count_records("Cprecincts", false, true), 0);

      p->finalize_all();
      kdu_params* qcd = p->access_cluster(QCD_params);
      EXPECT_EQ(qcd->count_records("Qabs_steps", true, false), 0);
      EXPECT_GT(qcd->count_records("Qabs_steps", true, true), 0);

      char str_t0c1[] = "Qstep:T0C1=0.05";
      const char* attr_name = nullptr;
      kdu_params* enc = p->access_cluster(ENC_params);
      kdu_params* found = enc->find_string(str_t0c1, attr_name);
      ASSERT_NE(found, nullptr);
      EXPECT_STREQ(attr_name, "Qstep");
      char str_qguard[] = "Qguard:T0C1=2";
      EXPECT_NE(qcd->find_string(str_qguard, attr_name), nullptr);
      EXPECT_STREQ(attr_name, "Qguard");
      char str_bad[] = "Qstep:Z0=0.05";
      EXPECT_EQ(enc->find_string(str_bad, attr_name), nullptr);

      // Invalid record/field indices and type mismatches in set().
      EXPECT_THROW(p->set("Ssize", -1, 0, 10), kdu_exception);
      EXPECT_THROW(p->set("Ssize", 0, 5, 10), kdu_exception);
      EXPECT_THROW(p->set("Sprecision", 0, 0, 1.5), kdu_exception);
      EXPECT_THROW(p->set("Sprecision", 0, 0, true), kdu_exception);
      EXPECT_THROW(cod->set("Creversible", 0, 5, true), kdu_exception);
      EXPECT_THROW(p->access_cluster(ENC_params)->set("Qstep", 0, 5, 1.0),
                   kdu_exception);
    }

    // 3. check_typical_tile, copy_all / link errors, textualize_attributes.
    {
      ScopedQuietErrors quiet;
      std::unique_ptr<siz_params> p =
          BuildFullCluster(/*total_tiles=*/2, /*num_comps=*/1, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          p.get(),
          "Ssize={64,64}\nSsampling={1,1}\nStiles={32,64}\nScomponents=1\n"
          "Sprecision=8\nSsigned=no\nCreversible=no\nClevels=1\nQguard:T0=3"));
      p->finalize_all();
      EXPECT_THROW(p->check_typical_tile(-1), kdu_exception);
      EXPECT_THROW(p->check_typical_tile(2), kdu_exception);
      EXPECT_FALSE(p->check_typical_tile(0));
      EXPECT_TRUE(p->check_typical_tile(0, "QCD:RGN"));
      EXPECT_TRUE(p->check_typical_tile(1));

      std::unique_ptr<siz_params> head_mismatch =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      EXPECT_THROW(p->copy_all(head_mismatch->access_cluster(COD_params)),
                   kdu_exception);
      EXPECT_THROW(p->access_cluster(COD_params)
                       ->access_relation(0, 0)
                       ->copy_all(head_mismatch->access_cluster(COD_params)),
                   kdu_exception);

      StringMessage msg;
      p->textualize_attributes(msg, /*min_tile=*/-1, /*max_tile=*/1,
                               /*skip_derived=*/true);
      EXPECT_FALSE(msg.text().empty());
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

TEST(ParamsBatchB4Test, QcdReversibleInheritGuardSizErrorsAndClusterSweep) {
  try {
    // 1. qcd_params: reversible copy_with_xforms (Qabs_ranges), max_eps > 31
    // guard increment, HT Part-15 magB reversible loop, and inherited
    // Qabs_steps adjustment across tiles.
    {
      for (bool use_ht : {false, true}) {
        std::unique_ptr<siz_params> rev = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        std::string spec =
            "Ssize={64,64}\nSsampling={1,1}\nScomponents=1\nSprecision=35\n"
            "Ssigned=no\nCreversible=yes\nClevels=5\nQguard=1";
        if (use_ht) spec += "\nCmodes=HT\nSCP15_magb=1";
        ASSERT_TRUE(ParseLines(rev.get(), spec.c_str()));
        rev->finalize_all();
        int guard = 0;
        ASSERT_TRUE(rev->access_cluster(QCD_params)->get(Qguard, 0, 0, guard));
        EXPECT_GE(guard, 2);

        std::unique_ptr<siz_params> rev_copy = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        rev_copy->copy_all(rev.get(), /*skip_components=*/0,
                           /*discard_levels=*/1, /*transpose=*/true);
        rev_copy->finalize_all();
        int range0 = 0;
        EXPECT_TRUE(rev_copy->access_cluster(QCD_params)
                        ->get(Qabs_ranges, 0, 0, range0));
      }

      // Inherited Qabs_steps on tile 0 when tile 0 sets Qguard:T0 only.
      for (bool use_ht : {false, true}) {
        std::unique_ptr<siz_params> inh = BuildFullCluster(
            /*total_tiles=*/2, /*num_comps=*/1, /*nlt_comps=*/0);
        std::string spec =
            "Ssize={64,64}\nStiles={32,64}\nSsampling={1,1}\nScomponents=1\n"
            "Sprecision=8\nSsigned=no\nCreversible=no\nClevels=1\n"
            "Qabs_steps=0.01,0.005,0.005,0.0025\nQguard:T0=2";
        if (use_ht) spec += "\nCmodes=HT\nSCP15_magb=1";
        ASSERT_TRUE(ParseLines(inh.get(), spec.c_str()));
        inh->finalize_all();
        float step = 0.0F;
        EXPECT_TRUE(inh->access_cluster(QCD_params)
                        ->access_relation(0, 0)
                        ->get(Qabs_steps, 0, 0, step));
        EXPECT_GT(step, 0.0F);
      }
    }

    // 2. siz_params::finalize vertical synthesis/errors, default BROADCAST,
    // conflicting Scpf_num/Simf, set_profiles negative fallback, and SIZ I/O.
    {
      ScopedQuietErrors quiet;
      // Valid vertical & horizontal Ssize deduction when both Ssampling & Sdims
      // are supplied.
      std::unique_ptr<siz_params> ded =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/2, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          ded.get(),
          "Ssampling={1,1},{2,2}\nSdims={64,64},{32,32}\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1"));
      ded->finalize_all();
      int sy = 0, sx = 0;
      EXPECT_TRUE(ded->get(Ssize, 0, 0, sy));
      EXPECT_TRUE(ded->get(Ssize, 0, 1, sx));
      EXPECT_EQ(sy, 64);
      EXPECT_EQ(sx, 64);

      auto expect_fin_err = [](const char* spec) {
        std::unique_ptr<siz_params> p = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/2, /*nlt_comps=*/0);
        EXPECT_TRUE(ParseLines(p.get(), spec));
        EXPECT_THROW(p->finalize_all(), kdu_exception) << spec;
      };
      // Vertical mismatch (64 vs 20*2).
      expect_fin_err(
          "Ssampling={1,1},{2,2}\nSdims={64,64},{20,32}\n"
          "Sprecision=8\nSsigned=no");
      // Vertical overflow with sub_y=2 and large dim_y.
      expect_fin_err(
          "Ssampling={2,1},{2,1}\nSdims={1200000000,10}\n"
          "Sprecision=8\nSsigned=no");

      // Default Sprofile=BROADCAST without explicit Sbroadcast.
      std::unique_ptr<siz_params> bcst =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          bcst.get(),
          "Ssize={1080,1920}\nSsampling={1,1}\nScomponents=3\nSprecision=8\n"
          "Ssigned=no\nSprofile=BROADCAST\nCreversible=no\nORGgen_tlm=1"));
      bcst->finalize_all();
      int b_lev = 0;
      EXPECT_TRUE(bcst->get(Sbroadcast, 0, 0, b_lev));
      EXPECT_EQ(b_lev, 1);

      // Conflicting Scpf_num + Simf warning + synthetic SIZ/PRF/CPF read paths.
      std::unique_ptr<siz_params> cpf_warn =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/3, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          cpf_warn.get(),
          "Ssize={1080,2048}\nSsampling={1,1}\nScomponents=3\nSprecision=10\n"
          "Ssigned=no\nCmodes=HT\nScpf_num=1024\nSimf={5,2,rev}\n"
          "Creversible=no"));
      cpf_warn->finalize_all();

      // set_profiles fallback when Part-1 Rsiz (1=PROFILE0) has Scap != 0.
      for (kdu_uint32 scap : {static_cast<kdu_uint32>(Scap_P2),
                              static_cast<kdu_uint32>(1 << 22)}) {
        std::unique_ptr<siz_params> sp = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        // SIZ marker body: Rsiz=1 (2B), Xs=32, Ys=32, Xo=0, Yo=0, Xt=32,
        // Yt=32, Xto=0, Yto=0, Csiz=1 (2B), Ssiz=7, XRsiz=1, YRsiz=1 (39B).
        kdu_byte siz_buf[39] = {0x00, 0x01, 0x00, 0x00, 0x00, 32, 0x00, 0x00,
                                0x00, 32,   0x00, 0x00, 0x00, 0,  0x00, 0x00,
                                0x00, 0,    0x00, 0x00, 0x00, 32, 0x00, 0x00,
                                0x00, 32,   0x00, 0x00, 0x00, 0,  0x00, 0x00,
                                0x00, 0,    0x00, 0x01, 7,    1,  1};
        EXPECT_NE(sp->translate_marker_segment(KDU_SIZ, 39, siz_buf, -1, 0), 0);
        sp->set(Scap, 0, 0, static_cast<int>(scap));
        // Send PRF [0x0001] -> set_profiles with prfnum=1 and scap!=0 ->
        // PART2/OTHER.
        kdu_byte prf_buf[2] = {0x00, 0x01};
        sp->translate_marker_segment(KDU_PRF, 2, prf_buf, -1, 0);
        int prof = -1;
        EXPECT_TRUE(sp->get(Sprofile, 0, 0, prof));
      }

      // SIZ read error paths: too short, trailing byte, b/332033271 overflow.
      std::unique_ptr<siz_params> sr =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      kdu_byte short_siz[10] = {};
      EXPECT_THROW(sr->translate_marker_segment(KDU_SIZ, 10, short_siz, -1, 0),
                   kdu_exception);
      kdu_byte extra_siz[40] = {0x00, 0x00, 0x00, 0x00, 0x00, 32, 0x00, 0x00,
                                0x00, 32,   0x00, 0x00, 0x00, 0,  0x00, 0x00,
                                0x00, 0,    0x00, 0x00, 0x00, 32, 0x00, 0x00,
                                0x00, 32,   0x00, 0x00, 0x00, 0,  0x00, 0x00,
                                0x00, 0,    0x00, 0x01, 7,    1,  1,    0x99};
      EXPECT_THROW(sr->translate_marker_segment(KDU_SIZ, 40, extra_siz, -1, 0),
                   kdu_exception);
      kdu_byte ovf_siz[39] = {0x00, 0x00, 0x7f, 0xff, 0xff, 0xff, 0x7f, 0xff,
                              0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00,
                              0x00, 0x00, 0x00, 0x00, 0x00, 32,   0x00, 0x00,
                              0x00, 32,   0x00, 0x00, 0x00, 0,    0x00, 0x00,
                              0x00, 0,    0x00, 0x01, 7,    1,    1};
      EXPECT_THROW(sr->translate_marker_segment(KDU_SIZ, 39, ovf_siz, -1, 0),
                   kdu_exception);

      // SIZ write error paths: invalid precision and invalid subsampling.
      auto check_siz_write_err = [](const char* attr, int val) {
        std::unique_ptr<siz_params> sw = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        EXPECT_TRUE(
            ParseLines(sw.get(),
                       "Ssize={32,32}\nStiles={32,32}\nSsampling={1,1}\n"
                       "Scomponents=1\nSprecision=8\nSsigned=no"));
        sw->set(attr, 0, 0, val);
        VectorOutput out;
        int flags = KDU_ParamsClass_FIXED;
        EXPECT_THROW(sw->generate_marker_segments(&out, -1, 0, flags),
                     kdu_exception);
      };
      check_siz_write_err(Sprecision, 39);
      check_siz_write_err(Ssampling, 300);
    }

    // 3. cod_params write errors, deferred comp decomp, 4-comp YCC ENC weights,
    // partial cluster copy_from, finalized !changed mutations, RGN read errs.
    {
      ScopedQuietErrors quiet;
      auto check_cod_write_err = [](const char* spec, const char* bad_attr,
                                    int bad_val) {
        std::unique_ptr<siz_params> p = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        EXPECT_TRUE(ParseLines(p.get(), spec));
        kdu_params* cod = p->access_cluster(COD_params);
        if (bad_attr != nullptr) cod->set(bad_attr, 0, 0, bad_val);
        VectorOutput out;
        int flags = KDU_ParamsClass_CODING;
        EXPECT_THROW(cod->generate_marker_segments(&out, -1, 0, flags),
                     kdu_exception);
      };
      const char* base_cod =
          "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1";
      check_cod_write_err(base_cod, Clayers, 65536);
      check_cod_write_err(base_cod, Clevels, 35);
      check_cod_write_err(
          "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1\nCuse_precincts=yes",
          nullptr, 0);
      check_cod_write_err(
          "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1\nCuse_precincts=yes\n"
          "Cprecincts={32,32},{32,32}",
          Cprecincts, 12);
      check_cod_write_err(
          "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1\nCuse_precincts=yes\n"
          "Cprecincts={32,32},{32,32}",
          Cprecincts, 65536);

      // Deferred cluster-head decomposition structure when all comps are
      // unique, plus 4-component Cycc=yes ENC default Qweights.
      std::unique_ptr<siz_params> comp_cod =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/4, /*nlt_comps=*/0);
      ASSERT_TRUE(ParseLines(
          comp_cod.get(),
          "Ssize={64,64}\nSsampling={1,1}\nScomponents=4\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=2\nCdecomp=H(-)\nCycc=yes\n"
          "Qfactor=85\nClevels:C0=2\nClevels:C1=2\nClevels:C2=2\nClevels:C3="
          "2"));
      comp_cod->finalize_all();
      float w3 = 0.0F;
      EXPECT_TRUE(
          comp_cod->access_cluster(ENC_params)->get(Qweights, 3, 0, w3));
      EXPECT_NEAR(w3, 1.0F, 1e-5F);

      // parse_string bitfield '0' fallback + finalized !changed truncate/set +
      // partial cluster copy_from mismatch skipping missing clusters.
      std::unique_ptr<siz_params> min_src = std::make_unique<siz_params>();
      LinkCluster<cod_params>(min_src.get(), /*total_tiles=*/1,
                              /*num_comps=*/1);
      LinkCluster<qcd_params>(min_src.get(), /*total_tiles=*/1,
                              /*num_comps=*/1);
      ASSERT_TRUE(ParseLines(
          min_src.get(),
          "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nSextensions=0\nCreversible=no\nClevels=1\n"
          "Cprecincts={32,32},{16,16}"));
      min_src->finalize_all();
      min_src->clear_marks(/*for_reading=*/false);
      EXPECT_FALSE(min_src->any_changes());
      EXPECT_TRUE(min_src->access_cluster(COD_params)
                      ->truncate_records("Cprecincts", 1));
      EXPECT_TRUE(min_src->any_changes());
      min_src->finalize_all();
      min_src->clear_marks(/*for_reading=*/false);
      EXPECT_TRUE(min_src->access_cluster(COD_params)
                      ->truncate_records("Cprecincts", 0));

      std::unique_ptr<siz_params> full_dst =
          BuildFullCluster(/*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
      full_dst->copy_all(min_src.get());
      full_dst->finalize_all();

      // RGN read_marker_segment malformed / mismatch paths.
      kdu_params* rgn0 =
          full_dst->access_cluster(RGN_params)->access_relation(0, 0);
      ASSERT_NE(rgn0, nullptr);
      kdu_byte rgn_short[2] = {0, 0};
      EXPECT_THROW(rgn0->translate_marker_segment(KDU_RGN, 2, rgn_short, 0, 0),
                   kdu_exception);
      kdu_byte rgn_long[5] = {0, 0, 3, 0xFF, 0};
      EXPECT_THROW(rgn0->translate_marker_segment(KDU_RGN, 5, rgn_long, 0, 0),
                   kdu_exception);
      kdu_byte rgn_t1[3] = {0, 0, 2};
      EXPECT_EQ(rgn0->translate_marker_segment(KDU_RGN, 3, rgn_t1, 0, 1), 0);
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

TEST(ParamsBatchB5Test, ProfileCapsSizCodWriteErrorsAndEdgePaths) {
  try {
    // 1. PRF/CPF marker segments, set_profiles branches, Scpf_profile defaults,
    // Nprecision/Nsigned inheritance, and Sncap disallow clearing.
    {
      ScopedQuietErrors quiet;

      // PRF error/warning branches and profile < 0 fallback via finalize_all.
      for (int scap_val : {static_cast<int>(Scap_P2), 1 << 22}) {
        std::unique_ptr<siz_params> p = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(
            p.get(),
            "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
            "Ssigned=no\nSprf_num=1"));
        p->set(Scap, 0, 0, scap_val);
        p->finalize_all();
        int prof = -1;
        EXPECT_TRUE(p->get(Sprofile, 0, 0, prof));
      }

      {
        std::unique_ptr<siz_params> prf = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(
            prf.get(),
            "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
            "Ssigned=no\nScap=P15"));
        prf->finalize_all();
        kdu_byte prf_short[1] = {0x00};
        prf->translate_marker_segment(KDU_PRF, 1, prf_short, -1, 0);
        kdu_byte prf_zero[2] = {0x00, 0x00};
        prf->translate_marker_segment(KDU_PRF, 2, prf_zero, -1, 0);
        kdu_byte prf_big[6] = {0x00, 0x01, 0x00, 0x01, 0x00, 0x01};
        prf->translate_marker_segment(KDU_PRF, 6, prf_big, -1, 0);

        // CPF malformed / edge cases and conflicting BROADCAST/IMF profiles.
        std::unique_ptr<siz_params> no_p15 = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        kdu_byte cpf_ok[2] = {0x00, 0x02};
        no_p15->translate_marker_segment(KDU_CPF, 2, cpf_ok, -1, 0);

        kdu_byte cpf_short[1] = {0x00};
        EXPECT_THROW(
            prf->translate_marker_segment(KDU_CPF, 1, cpf_short, -1, 0),
            kdu_exception);
        kdu_byte cpf_zero[2] = {0x00, 0x00};
        prf->translate_marker_segment(KDU_CPF, 2, cpf_zero, -1, 0);
        kdu_byte cpf_huge[10] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
        prf->translate_marker_segment(KDU_CPF, 10, cpf_huge, -1, 0);
        kdu_byte cpf_4095[2] = {0x10, 0x00};
        prf->translate_marker_segment(KDU_CPF, 2, cpf_4095, -1, 0);
        kdu_byte cpf_one[2] = {0x00, 0x01};
        prf->translate_marker_segment(KDU_CPF, 2, cpf_one, -1, 0);

        // Compatible broadcast & IMF via KDU_CPF when base profile has neither,
        // then conflicting broadcast & IMF numbers via KDU_CPF.
        kdu_byte cpf_bcst[2] = {0x01, 0x02};  // cpfnum = 0x101 (BROADCAST L1)
        prf->translate_marker_segment(KDU_CPF, 2, cpf_bcst, -1, 0);
        kdu_byte cpf_imf[2] = {0x04, 0x01};  // cpfnum = 0x400 (IMF)
        prf->translate_marker_segment(KDU_CPF, 2, cpf_imf, -1, 0);

        std::unique_ptr<siz_params> bcst = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(
            bcst.get(),
            "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
            "Ssigned=no\nScap=P15\nSprf_num=257"));
        bcst->finalize_all();
        kdu_byte cpf_bcst_diff[2] = {0x01, 0x03};  // cpfnum = 0x102 (L2)
        bcst->translate_marker_segment(KDU_CPF, 2, cpf_bcst_diff, -1, 0);

        std::unique_ptr<siz_params> imf = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(
            imf.get(),
            "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
            "Ssigned=no\nScap=P15\nSprf_num=1024"));
        imf->finalize_all();
        kdu_byte cpf_imf_diff[2] = {0x04, 0x02};  // cpfnum = 0x401
        imf->translate_marker_segment(KDU_CPF, 2, cpf_imf_diff, -1, 0);
      }

      // Scpf_profile defaults for BROADCAST and IMF without Sbroadcast/Simf.
      {
        std::unique_ptr<siz_params> p_bcst = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(
            p_bcst.get(),
            "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
            "Ssigned=no\nCmodes=HT\nScpf_profile=BROADCAST\nCreversible=no"));
        p_bcst->finalize_all();
        int lv = 0;
        EXPECT_TRUE(p_bcst->get(Sbroadcast, 0, 0, lv));

        std::unique_ptr<siz_params> p_imf = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(
            p_imf.get(),
            "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
            "Ssigned=no\nCmodes=HT\nScpf_profile=IMF\nCreversible=no"));
        p_imf->finalize_all();
        EXPECT_TRUE(p_imf->get(Simf, 0, 0, lv));
      }

      // Nsigned/Nprecision inheritance -> Ssigned/Sprecision and
      // Msigned/Mprecision, plus Sncap clearing SCP2_caps / SCP17_caps.
      {
        std::unique_ptr<siz_params> inherit_s = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(inherit_s.get(),
                               "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\n"
                               "Nprecision=10\nNsigned=yes\nSncap=P2|P17"));
        inherit_s->set(SCP2_caps, 0, 0, SCP2_caps_EXTENDED_COD);
        inherit_s->set(SCP17_caps, 0, 0, 0);
        inherit_s->finalize_all();
        int prec = 0, is_sgn = 0;
        EXPECT_TRUE(inherit_s->get(Sprecision, 0, 0, prec));
        EXPECT_EQ(prec, 10);
        EXPECT_TRUE(inherit_s->get(Ssigned, 0, 0, is_sgn));
        EXPECT_EQ(is_sgn, 1);

        std::unique_ptr<siz_params> inherit_m = std::make_unique<siz_params>();
        LinkCluster<cod_params>(inherit_m.get(), /*total_tiles=*/1,
                                /*num_comps=*/1);
        ASSERT_TRUE(ParseLines(inherit_m.get(),
                               "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\n"
                               "Mcomponents=1\nNprecision=12\nNsigned=no"));
        inherit_m->finalize_all();
        EXPECT_TRUE(inherit_m->get(Mprecision, 0, 0, prec));
        EXPECT_EQ(prec, 12);
        EXPECT_TRUE(inherit_m->get(Msigned, 0, 0, is_sgn));
        EXPECT_EQ(is_sgn, 0);
      }
    }

    // 2. Finalize-first SIZ and COD write_marker_segment validation errors,
    // plus SCP17 illegal capability error and Sallowed HET/CHET rejections.
    {
      ScopedQuietErrors quiet;

      // SIZ write error when precision/sampling/signed missing on unfinalized
      // obj.
      {
        std::unique_ptr<siz_params> miss_siz = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(miss_siz.get(),
                               "Ssize={32,32}\nSorigin={0,0}\nStiles={32,32}\n"
                               "Stile_origin={0,0}\nScomponents=1"));
        miss_siz->set(Sdims, 0, 0, 32);
        miss_siz->set(Sdims, 0, 1, 32);
        VectorOutput out;
        int flags = KDU_ParamsClass_FIXED;
        EXPECT_THROW(miss_siz->generate_marker_segments(&out, -1, 0, flags),
                     kdu_exception);
      }

      // SIZ precision=39 and sampling=300 errors after finalize.
      for (const auto& bad : {std::pair<const char*, int>{Sprecision, 39},
                              std::pair<const char*, int>{Ssampling, 300}}) {
        std::unique_ptr<siz_params> sw = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        ASSERT_TRUE(
            ParseLines(sw.get(),
                       "Ssize={32,32}\nStiles={32,32}\nSsampling={1,1}\n"
                       "Scomponents=1\nSprecision=8\nSsigned=no"));
        sw->finalize_all();
        sw->set(bad.first, 0, 0, bad.second);
        VectorOutput out;
        int flags = KDU_ParamsClass_FIXED;
        EXPECT_THROW(sw->generate_marker_segments(&out, -1, 0, flags),
                     kdu_exception);
      }

      // COD write errors after finalize_all().
      auto check_cod_post_finalize_err = [](const char* spec,
                                            const char* bad_attr, int bad_val) {
        std::unique_ptr<siz_params> p = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        EXPECT_TRUE(ParseLines(p.get(), spec));
        p->finalize_all();
        kdu_params* cod = p->access_cluster(COD_params);
        if (bad_attr != nullptr) cod->set(bad_attr, 0, 0, bad_val);
        VectorOutput out;
        int flags = KDU_ParamsClass_CODING;
        EXPECT_THROW(cod->generate_marker_segments(&out, -1, 0, flags),
                     kdu_exception);
      };
      const char* valid_cod =
          "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1";
      check_cod_post_finalize_err(valid_cod, Clayers, 65536);
      check_cod_post_finalize_err(valid_cod, Clevels, 35);
      check_cod_post_finalize_err(valid_cod, Cuse_precincts, 1);
      const char* prec_cod =
          "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
          "Ssigned=no\nCreversible=no\nClevels=1\nCuse_precincts=yes\n"
          "Cprecincts={32,32},{32,32}";
      check_cod_post_finalize_err(prec_cod, Cprecincts, 12);
      check_cod_post_finalize_err(prec_cod, Cprecincts, 65536);

      // Sallowed HET/CHET rejections on tile COD and component COCs.
      {
        std::unique_ptr<siz_params> sallow = BuildFullCluster(
            /*total_tiles=*/2, /*num_comps=*/2, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(
            sallow.get(),
            "Ssize={64,32}\nStiles={32,32}\nSsampling={1,1}\n"
            "Scomponents=2\nSprecision=8\nSsigned=no\nCreversible=no\n"
            "Clevels=2\nClevels:T0=1\nClevels:C0=3\nCblk:C1={16,16}"));
        sallow->finalize_all();
        sallow->set(Sallowed, 0, 0, 0);
        kdu_params* cod = sallow->access_cluster(COD_params);
        VectorOutput out;
        int flags = KDU_ParamsClass_CODING;
        EXPECT_THROW(cod->generate_marker_segments(&out, 0, 0, flags),
                     kdu_exception);
        EXPECT_THROW(cod->access_relation(-1, 0)->generate_marker_segments(
                         &out, -1, 0, flags),
                     kdu_exception);
        EXPECT_THROW(cod->access_relation(-1, 1)->generate_marker_segments(
                         &out, -1, 0, flags),
                     kdu_exception);
      }
    }

    // 3. Deferred tile ADS resolution on cod_params::finalize(true),
    // qcd_params::copy_with_xforms (levels=0 reversible), magB=1 QCD
    // Qabs_steps, Qfactor+Qweights error, translate bad tile/comp, and unmarked
    // removal.
    {
      ScopedQuietErrors quiet;

      // Deferred ADS resolution when every component in tile 0 is unique.
      {
        std::unique_ptr<siz_params> ads_p = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/2, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(
            ads_p.get(),
            "Ssize={64,64}\nSsampling={1,1}\nScomponents=2\nSprecision=8\n"
            "Ssigned=no\nCreversible=yes\nClevels=2\nCdecomp=H(-)\n"
            "Clevels:T0C0=2\nClevels:T0C1=2"));
        ads_p->finalize_all();
        VectorOutput main_out, tile_out;
        int flags = KDU_ParamsClass_FIXED | KDU_ParamsClass_FUNCTIONAL;
        ads_p->generate_marker_segments(&main_out, -1, 0, flags);
        main_out.Flush();
        ads_p->generate_marker_segments(&tile_out, 0, 0, flags);
        tile_out.Flush();

        // Read back ADS/COD/COC markers on a fresh parameter tree (without
        // ENC).
        std::unique_ptr<siz_params> ads_rx = std::make_unique<siz_params>();
        LinkCluster<cod_params>(ads_rx.get(), /*total_tiles=*/1,
                                /*num_comps=*/2);
        LinkCluster<dfs_params>(ads_rx.get(), /*total_tiles=*/0,
                                /*num_comps=*/0);
        LinkCluster<ads_params>(ads_rx.get(), /*total_tiles=*/1,
                                /*num_comps=*/0);
        LinkCluster<qcd_params>(ads_rx.get(), /*total_tiles=*/1,
                                /*num_comps=*/2);
        auto feed_stream = [&](std::vector<kdu_byte> buf, int tile) {
          size_t pos = 0;
          while (pos + 4 <= buf.size()) {
            kdu_uint16 code =
                (static_cast<kdu_uint16>(buf[pos]) << 8) | buf[pos + 1];
            kdu_uint16 len =
                (static_cast<kdu_uint16>(buf[pos + 2]) << 8) | buf[pos + 3];
            ASSERT_LE(pos + 2 + len, buf.size());
            ads_rx->translate_marker_segment(code, len - 2, &buf[pos + 4], tile,
                                             0);
            pos += 2 + len;
          }
        };
        feed_stream(main_out.bytes(), -1);
        feed_stream(tile_out.bytes(), 0);
        ads_rx->finalize_all(/*after_reading=*/true);

        // Out-of-range component index on KDU_COC triggers error L3632.
        kdu_byte bad_coc[10] = {9, 0x00, 1, 2, 2, 0, 1, 0x77, 0, 0};
        EXPECT_THROW(
            ads_rx->translate_marker_segment(KDU_COC, 8, bad_coc, 0, 0),
            kdu_exception);

        // finalize_all(true) & finalize_all(which_tile, true) removing unmarked
        // records when changed == false. Keep `for_reading=false` so parsed
        // attributes retain `num_marked_records > 0` while extra attrs do not.
        ads_rx->set(Sextensions, 0, 0, 1);
        ads_rx->clear_marks(/*for_reading=*/false);
        ads_rx->finalize_all(/*after_reading=*/true);
        ads_rx->access_cluster(COD_params)
            ->access_relation(0, -1)
            ->set(Cprecincts, 0, 0, 32);
        ads_rx->clear_marks(/*for_reading=*/false);
        ads_rx->finalize_all(/*which_tile=*/0, /*after_reading=*/true);
      }

      // qcd_params::copy_with_xforms with source Clevels=0 + Creversible=yes.
      {
        std::unique_ptr<siz_params> rev0 = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(
            rev0.get(),
            "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
            "Ssigned=no\nCreversible=yes\nClevels=0\nQabs_ranges=10"));
        rev0->finalize_all();
        std::unique_ptr<siz_params> dst0 = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        dst0->copy_all(rev0.get());
        int rng = 0;
        EXPECT_TRUE(
            dst0->access_cluster(QCD_params)->get(Qabs_ranges, 0, 0, rng));
      }

      // SCP15_magb=1 with irreversible Qabs_steps ->
      // adjust_absolute_steps_finding_magB.
      {
        std::unique_ptr<siz_params> magb = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(
            magb.get(),
            "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
            "Ssigned=no\nCmodes=HT\nScap=P15\nSCP15_magb=1\n"
            "Creversible=no\nClevels=1\nQabs_steps=0.05,0.04,0.04,0.03"));
        magb->finalize_all();
        int final_magb = 0;
        EXPECT_TRUE(magb->get(SCP15_magb, 0, 0, final_magb));
        EXPECT_GE(final_magb, 1);
      }

      // Qfactor + Qweights > 0 vs Qweights <= 0 error in enc_params::finalize.
      {
        std::unique_ptr<siz_params> qf_ok = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(
            qf_ok.get(),
            "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
            "Ssigned=no\nCreversible=no\nClevels=1\nQfactor=85\nQweights=1.5"));
        qf_ok->finalize_all();

        std::unique_ptr<siz_params> qf_bad = BuildFullCluster(
            /*total_tiles=*/1, /*num_comps=*/1, /*nlt_comps=*/0);
        ASSERT_TRUE(ParseLines(
            qf_bad.get(),
            "Ssize={32,32}\nSsampling={1,1}\nScomponents=1\nSprecision=8\n"
            "Ssigned=no\nCreversible=no\nClevels=1\nQfactor=85\nQweights=0"));
        EXPECT_THROW(qf_bad->finalize_all(), kdu_exception);
      }
    }
  } catch (kdu_exception exc) {
    FAIL() << "Unexpected kdu_exception(" << exc
           << "): " << GlobalErrorHandler().last_message();
  }
}

}  // namespace
}  // namespace kdu_core
