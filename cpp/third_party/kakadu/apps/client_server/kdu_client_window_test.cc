// Unit tests for
// third_party/kakadu/vendor/apps/client_server/kdu_client_window.cpp.

#include "third_party/kakadu/vendor/apps/compressed_io/kdu_client_window.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jp2.h"
#include "third_party/kakadu/vendor/apps/compressed_io/kdu_cache.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_supp {
namespace {

using ::kdu_core::kdu_coords;
using ::kdu_core::kdu_dims;
using ::kdu_core::kdu_long;
using ::kdu_core::kdu_uint32;

constexpr int kIntMax = std::numeric_limits<int>::max();

class GlobalErrorHandler final : public kdu_core::kdu_thread_safe_message {
 public:
  ~GlobalErrorHandler() noexcept override = default;
  void put_text(const char*) override {}
  void flush(bool end_of_message = false) override {
    kdu_core::kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw KDU_ERROR_EXCEPTION;
    }
  }
};

class NoopWarningHandler final : public kdu_core::kdu_message {
 public:
  ~NoopWarningHandler() noexcept override = default;
  void put_text(const char*) override {}
};

class KakaduEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* err = new GlobalErrorHandler();
    static auto* warn = new NoopWarningHandler();
    kdu_core::kdu_customize_errors(err);
    kdu_core::kdu_customize_warnings(warn);
  }
};

const ::testing::Environment* const kKakaduEnv =
    ::testing::AddGlobalTestEnvironment(new KakaduEnvironment());

// Populates `prefs.csf_sensitivities` (whose ownership passes to
// `kdu_window_prefs`, released via `delete[]` inside `init()`/`~dtor`). Direct
// allocation is used for multi-row tables because `parse_prefs` currently
// advances `data += max_sensitivities_per_row` instead of
// `2 + max_sensitivities_per_row` (kdu_client_window.cpp:265).
template <std::size_t N>
void SetCsfTable(kdu_window_prefs& prefs, int num_angles, int max_per_angle,
                 const std::array<float, N>& vals) {
  ASSERT_EQ(vals.size(),
            static_cast<std::size_t>(num_angles * (2 + max_per_angle)));
  delete[] prefs.csf_sensitivities;
  prefs.csf_sensitivities = nullptr;
  prefs.num_csf_angles = num_angles;
  prefs.max_sensitivities_per_csf_angle = max_per_angle;
  float* buf = new float[vals.size()];
  std::copy(vals.begin(), vals.end(), buf);
  prefs.csf_sensitivities = buf;
}

constexpr kdu_uint32 FourCC(char a, char b, char c, char d) {
  return (static_cast<kdu_uint32>(static_cast<unsigned char>(a)) << 24) |
         (static_cast<kdu_uint32>(static_cast<unsigned char>(b)) << 16) |
         (static_cast<kdu_uint32>(static_cast<unsigned char>(c)) << 8) |
         static_cast<kdu_uint32>(static_cast<unsigned char>(d));
}

kdu_dims MakeDims(int x, int y, int w, int h) {
  kdu_dims dims;
  dims.pos = kdu_coords(x, y);
  dims.size = kdu_coords(w, h);
  return dims;
}

class FakeWincontextMappings : public kdu_wincontext_mappings {
 public:
  int get_num_members(int base_context_idx, int rep_idx,
                      const int remapping_ids[]) override {
    last_base = base_context_idx;
    last_rep = rep_idx;
    last_remap[0] = remapping_ids[0];
    last_remap[1] = remapping_ids[1];
    return 2;
  }

  int get_codestream(int base_context_idx, int rep_idx,
                     const int /*remapping_ids*/[], int member_idx) override {
    return base_context_idx * 10 + rep_idx + member_idx;
  }

  const int* get_components(int /*base_context_idx*/, int /*rep_idx*/,
                            const int /*remapping_ids*/[], int member_idx,
                            int& num_components) override {
    num_components = (member_idx == 0) ? 3 : 1;
    return comps_.data();
  }

  bool perform_remapping(int /*base_context_idx*/, int /*rep_idx*/,
                         const int /*remapping_ids*/[], int member_idx,
                         kdu_coords& resolution, kdu_dims& region) override {
    if (member_idx != 0) return false;
    resolution.x *= 2;
    resolution.y *= 2;
    region.pos.x *= 2;
    region.pos.y *= 2;
    return true;
  }

  int last_base = -1;
  int last_rep = -1;
  std::array<int, 2> last_remap = {-1, -1};

 private:
  std::array<int, 3> comps_ = {0, 1, 2};
};

TEST(KduClientWindowTest, SampledRangeAndTypeCodeHelpers) {
  // 1. kdu_sampled_range constructors, predicates, getters, and setters.
  kdu_sampled_range empty_range;
  EXPECT_TRUE(empty_range.is_empty());
  EXPECT_TRUE(!empty_range);
  EXPECT_EQ(empty_range.get_from(), 0);
  EXPECT_EQ(empty_range.get_to(), -1);
  EXPECT_EQ(empty_range.get_step(), 1);
  EXPECT_EQ(empty_range.get_remapping_id(0), -1);
  EXPECT_EQ(empty_range.get_remapping_id(1), -1);
  EXPECT_EQ(empty_range.get_remapping_id(-1), 0);
  EXPECT_EQ(empty_range.get_remapping_id(2), 0);
  EXPECT_EQ(empty_range.get_context_type(), KDU_JPIP_CONTEXT_NONE);

  kdu_range_set dummy_expansion;
  kdu_sampled_range single_range(7);
  EXPECT_FALSE(single_range.is_empty());
  EXPECT_FALSE(!single_range);
  EXPECT_EQ(single_range.get_from(), 7);
  EXPECT_EQ(single_range.get_to(), 7);

  kdu_sampled_range span_range(2, 10, 3);
  span_range.set_context_type(KDU_JPIP_CONTEXT_JPXL);
  span_range.set_remapping_id(0, 4);
  span_range.set_remapping_id(1, 5);
  span_range.set_remapping_id(-1, 99);  // Ignored out of range
  span_range.set_remapping_id(2, 99);   // Ignored out of range
  span_range.expansion = &dummy_expansion;
  EXPECT_EQ(span_range.get_context_type(), KDU_JPIP_CONTEXT_JPXL);
  EXPECT_EQ(span_range.get_remapping_id(0), 4);
  EXPECT_EQ(span_range.get_remapping_id(1), 5);

  kdu_sampled_range copied(span_range);
  EXPECT_EQ(copied.expansion, nullptr);
  EXPECT_EQ(copied.get_from(), 2);
  copied.set_from(1);
  copied.set_to(9);
  copied.set_step(2);
  EXPECT_EQ(copied.get_from(), 1);
  EXPECT_EQ(copied.get_to(), 9);
  EXPECT_EQ(copied.get_step(), 2);

  // 2. kdu_write_type_code and kdu_parse_type_code.
  std::array<char, 20> buf{};
  EXPECT_STREQ(kdu_write_type_code(FourCC('x', 'm', 'l', ' '), buf.data()),
               "xml_");
  EXPECT_STREQ(
      kdu_write_type_code(FourCC('A', '9', '\x01', '\xff'), buf.data()),
      "A9\\001\\377");

  int num_chars = 0;
  EXPECT_EQ(kdu_parse_type_code("xml_tail", num_chars),
            FourCC('x', 'm', 'l', ' '));
  EXPECT_EQ(num_chars, 4);
  EXPECT_EQ(kdu_parse_type_code("A9\\001\\377", num_chars),
            FourCC('A', '9', '\x01', '\xff'));
  EXPECT_EQ(num_chars, 10);
  EXPECT_EQ(kdu_parse_type_code("ab", num_chars), FourCC('a', 'b', '\0', '\0'));
  EXPECT_EQ(num_chars, 2);

  // 3. kdu_window_context & kdu_wincontext_mappings.
  kdu_window_context empty_ctx;
  EXPECT_FALSE(empty_ctx.exists());
  EXPECT_TRUE(!empty_ctx);

  FakeWincontextMappings mappings;
  kdu_window_context ctx(&mappings, 3, 2);
  EXPECT_TRUE(ctx.exists());
  EXPECT_FALSE(!ctx);
  const std::array<int, 2> remaps = {7, 8};
  EXPECT_EQ(ctx.get_num_members(remaps.data()), 2);
  EXPECT_EQ(mappings.last_base, 3);
  EXPECT_EQ(mappings.last_rep, 2);
  EXPECT_EQ(mappings.last_remap[0], 7);
  EXPECT_EQ(ctx.get_codestream(remaps.data(), 1), 33);
  int num_comps = 0;
  const int* comps = ctx.get_components(remaps.data(), 0, num_comps);
  ASSERT_NE(comps, nullptr);
  EXPECT_EQ(num_comps, 3);
  kdu_coords res(100, 80);
  kdu_dims reg = MakeDims(5, 6, 10, 12);
  EXPECT_TRUE(ctx.perform_remapping(remaps.data(), 0, res, reg));
  EXPECT_EQ(res.x, 200);
  EXPECT_EQ(reg.pos.x, 10);
  EXPECT_FALSE(ctx.perform_remapping(remaps.data(), 1, res, reg));
}

TEST(KduClientWindowTest, RangeSetMergingSortingContainmentAndExpansion) {
  // 1. Merging and sorting branches in kdu_range_set::add.
  kdu_range_set rs;
  EXPECT_TRUE(rs.is_empty());
  EXPECT_TRUE(!rs);
  EXPECT_TRUE(rs.get_range(0).is_empty());
  EXPECT_EQ(rs.access_range(0), nullptr);
  EXPECT_EQ(rs.access_range(-1), nullptr);

  // Ignored invalid/empty ranges.
  kdu_sampled_range invalid;
  invalid.from = 5;
  invalid.to = 3;
  rs.add(invalid);
  invalid.from = 2;
  invalid.to = 4;
  invalid.step = 0;
  rs.add(invalid);
  EXPECT_TRUE(rs.is_empty());

  // Tail trimming when (to - from) is not divisible by step.
  rs.add(kdu_sampled_range(2, 9, 3));  // {2, 5, 8}
  ASSERT_EQ(rs.get_num_ranges(), 1);
  EXPECT_EQ(rs.get_range(0).to, 8);

  // Contiguous extend forward & backward.
  rs.init();
  rs.add(kdu_sampled_range(4, 8, 2));    // {4, 6, 8}
  rs.add(kdu_sampled_range(10, 12, 2));  // extend end -> {4..12:2}
  rs.add(kdu_sampled_range(0, 2, 2));    // extend start -> {0..12:2}
  rs.add(kdu_sampled_range(4, 8, 2));    // contained inside -> unchanged
  ASSERT_EQ(rs.get_num_ranges(), 1);
  EXPECT_EQ(rs.get_range(0).from, 0);
  EXPECT_EQ(rs.get_range(0).to, 12);

  // Overlapping ranges with step 1 and gap filling collapsing 3->1 range.
  rs.init();
  rs.add(10, 12);
  rs.add(0, 2);  // bubble-sorted before [10..12]
  rs.add(4, 8);  // sorted in between
  EXPECT_EQ(rs.get_num_ranges(), 3);
  rs.add(3);  // bridges [0..2] and [4..8] -> [0..8] + [10..12]
  rs.add(9);  // bridges [0..8] and [10..12] -> [0..12]
  EXPECT_EQ(rs.get_num_ranges(), 1);
  EXPECT_EQ(rs.get_range(0).from, 0);
  EXPECT_EQ(rs.get_range(0).to, 12);

  // Single-element extension of a subsampled range.
  rs.init();
  rs.add(kdu_sampled_range(4, 8, 2));
  rs.add(2);   // extends start -> {2,4,6,8}
  rs.add(10);  // extends end -> {2..10:2}
  EXPECT_EQ(rs.get_num_ranges(), 1);
  EXPECT_EQ(rs.get_range(0).from, 2);
  EXPECT_EQ(rs.get_range(0).to, 10);
  EXPECT_EQ(rs.get_range(0).step, 2);

  // Non-common step and misaligned offset do not merge.
  rs.add(kdu_sampled_range(1, 9, 2));
  EXPECT_EQ(rs.get_num_ranges(), 2);

  // Context type and remapping IDs prevent merging.
  rs.init();
  kdu_sampled_range c1(0, 2);
  c1.context_type = KDU_JPIP_CONTEXT_JPXL;
  c1.remapping_ids[0] = 1;
  c1.remapping_ids[1] = 0;
  rs.add(c1);
  kdu_sampled_range c2(3, 5);
  c2.context_type = KDU_JPIP_CONTEXT_JPXL;
  c2.remapping_ids[0] = 1;
  c2.remapping_ids[1] = 1;
  rs.add(c2);
  EXPECT_EQ(rs.get_num_ranges(), 2);
  c2.remapping_ids[1] = 0;
  rs.add(c2);  // matches c1 -> merges with c1
  EXPECT_EQ(rs.get_num_ranges(), 2);

  // 2. Reallocation past 10 ranges, allow_merging=false, copy_from, operator=.
  kdu_range_set many;
  for (int i = 0; i < 15; ++i) {
    many.add(kdu_sampled_range(i * 2, i * 2 + 1), /*allow_merging=*/false);
  }
  EXPECT_EQ(many.get_num_ranges(), 15);
  kdu_range_set copy_rs;
  copy_rs = many;
  EXPECT_TRUE(copy_rs == many);
  EXPECT_FALSE(copy_rs != many);

  // 3. containment, empty_set_defaults_to_zero, translated context, test,
  // expand.
  kdu_range_set empty_rs;
  kdu_range_set zero_rs;
  zero_rs.add(0);
  kdu_range_set one_rs;
  one_rs.add(1);
  EXPECT_FALSE(empty_rs.contains(zero_rs, false));
  EXPECT_TRUE(empty_rs.contains(zero_rs, true));
  EXPECT_TRUE(zero_rs.contains(empty_rs, true));
  EXPECT_FALSE(empty_rs.contains(one_rs, true));
  EXPECT_FALSE(one_rs.contains(empty_rs, true));

  kdu_range_set translated_only;
  kdu_sampled_range tr(5, 9);
  tr.context_type = KDU_JPIP_CONTEXT_TRANSLATED;
  translated_only.add(tr);
  EXPECT_TRUE(empty_rs.contains(translated_only, false));

  kdu_range_set even;
  even.add(kdu_sampled_range(0, 12, 2));
  kdu_range_set sub_even;
  sub_even.add(kdu_sampled_range(2, 10, 2));
  kdu_range_set stride4;
  stride4.add(kdu_sampled_range(2, 10, 4));
  kdu_range_set stride3;
  stride3.add(kdu_sampled_range(2, 8, 3));
  EXPECT_TRUE(even.contains(sub_even));
  EXPECT_FALSE(even.contains(stride4));
  EXPECT_FALSE(even.contains(stride3));
  EXPECT_TRUE(even.test(4));
  EXPECT_FALSE(even.test(5));

  kdu_range_set ovlp;
  ovlp.add(kdu_sampled_range(2, 6), false);
  ovlp.add(kdu_sampled_range(4, 9), false);
  std::array<int, 16> expanded{};
  const int n_exp = ovlp.expand(expanded.data(), 3, 7);
  EXPECT_EQ(n_exp, 5);
  for (int i = 0; i < n_exp; ++i) {
    EXPECT_GE(expanded[i], 3);
    EXPECT_LE(expanded[i], 7);
  }
}

TEST(KduClientWindowTest, WindowMetareqParsingAndContainment) {
  // 1. is_empty across all kdu_window fields.
  kdu_window win;
  EXPECT_TRUE(win.is_empty());
  win.set_resolution(kdu_coords(64, 64));
  EXPECT_FALSE(win.is_empty());
  win.init();
  win.set_region(MakeDims(0, 0, 8, 8));
  EXPECT_FALSE(win.is_empty());
  win.init();
  win.access_components()->add(0);
  EXPECT_FALSE(win.is_empty());
  win.init();
  win.access_codestreams()->add(0);
  EXPECT_FALSE(win.is_empty());
  win.init();
  kdu_sampled_range jpxl_rng(0);
  jpxl_rng.context_type = KDU_JPIP_CONTEXT_JPXL;
  win.access_contexts()->add(jpxl_rng);
  EXPECT_FALSE(win.is_empty());
  win.init();
  win.set_max_layers(2);
  EXPECT_FALSE(win.is_empty());
  win.init();
  win.add_metareq(FourCC('x', 'm', 'l', ' '));
  EXPECT_FALSE(win.is_empty());

  // 2. Context expansion, copy_from(copy_expansions=true/false), copy_metareq.
  win.init();
  win.set_resolution(kdu_coords(100, 100));
  win.set_round_direction(-1);
  win.set_region(MakeDims(10, 10, 20, 20));
  win.access_components()->add(0, 2);
  win.access_codestreams()->add(0, 1);
  win.access_contexts()->add(jpxl_rng);
  kdu_range_set* exp0 = win.create_context_expansion(0);
  ASSERT_NE(exp0, nullptr);
  exp0->add(0, 2);
  ASSERT_EQ(win.access_context_expansion(0), exp0);
  EXPECT_EQ(win.access_context_expansion(9), nullptr);
  win.set_max_layers(3);
  win.set_metadata_only(false);
  win.add_metareq(FourCC('x', 'm', 'l', ' '), 0, true, 128, false, 0, 2);
  ASSERT_NE(win.get_metareq(0), nullptr);
  EXPECT_EQ(win.get_metareq(0)->get_qualifier(), KDU_MRQ_DEFAULT);
  EXPECT_TRUE(win.get_metareq(0)->get_priority());
  EXPECT_EQ(win.get_metareq(0)->get_byte_limit(), 128);
  EXPECT_EQ(win.get_metareq(0)->get_max_depth(), 2);
  EXPECT_EQ(win.get_metareq(0)->get_root_bin_id(), 0);
  EXPECT_FALSE(win.get_metareq(0)->get_recurse());

  kdu_window win_copy_exp;
  win_copy_exp.copy_from(win, /*copy_expansions=*/true);
  ASSERT_NE(win_copy_exp.access_context_expansion(0), nullptr);
  EXPECT_TRUE(*win_copy_exp.access_context_expansion(0) == *exp0);
  EXPECT_TRUE(win.equals(win_copy_exp));
  EXPECT_TRUE(win.contains(win_copy_exp));
  EXPECT_TRUE(win.imagery_equals(win_copy_exp));
  EXPECT_TRUE(win.imagery_contains(win_copy_exp));

  kdu_window win_assign;
  win_assign = win;
  EXPECT_EQ(win_assign.access_context_expansion(0), nullptr);
  EXPECT_TRUE(win.equals(win_assign));

  // Sub-window containment checks.
  kdu_window sub;
  sub.copy_from(win, false);
  sub.set_region(MakeDims(12, 12, 10, 10));
  sub.access_components()->init();
  sub.access_components()->add(1);
  sub.set_max_layers(2);
  EXPECT_TRUE(win.contains(sub));
  EXPECT_FALSE(sub.contains(win));
  EXPECT_FALSE(win.equals(sub));
  sub.set_round_direction(1);
  EXPECT_FALSE(win.imagery_contains(sub));
  sub.set_round_direction(-1);
  EXPECT_TRUE(win.imagery_contains(sub));

  sub.copy_from(win, false);
  sub.set_metadata_only(true);
  EXPECT_TRUE(win.imagery_contains(sub));
  EXPECT_FALSE(sub.imagery_contains(win));
  EXPECT_FALSE(win.imagery_equals(sub));

  // 3. parse_context valid & malformed expressions.
  kdu_window pwin;
  pwin.init();
  const char* rem = pwin.parse_context("jpxl<0-4:2>[s1i2]=0-5:1,7,next");
  ASSERT_NE(rem, nullptr);
  EXPECT_EQ(std::string_view(rem), "next");
  ASSERT_EQ(pwin.contexts.get_num_ranges(), 1);
  EXPECT_EQ(pwin.contexts.get_range(0).context_type, KDU_JPIP_CONTEXT_JPXL);
  EXPECT_EQ(pwin.contexts.get_range(0).remapping_ids[0], 1);
  EXPECT_EQ(pwin.contexts.get_range(0).remapping_ids[1], 2);
  ASSERT_NE(pwin.access_context_expansion(0), nullptr);
  EXPECT_TRUE(pwin.access_context_expansion(0)->test(7));

  rem = pwin.parse_context("jpxl<3>;rest");
  ASSERT_NE(rem, nullptr);
  EXPECT_EQ(std::string_view(rem), ";rest");

  rem = pwin.parse_context("jpxl<1->");
  ASSERT_NE(rem, nullptr);
  EXPECT_EQ(*rem, '\0');

  rem = pwin.parse_context("mj2t<1-3+now>[track]=0-1;tail");
  ASSERT_NE(rem, nullptr);
  EXPECT_EQ(std::string_view(rem), ";tail");

  rem = pwin.parse_context("mj2t<0-+now>[movie]");
  EXPECT_EQ(*rem, '\0');

  rem = pwin.parse_context("mj2t<2>");
  EXPECT_EQ(*rem, '\0');

  // Unrecognized context skipped up to '='.
  rem = pwin.parse_context("custom<1-2>[foo]=3;more");
  EXPECT_EQ(std::string_view(rem), "=3;more");

  constexpr auto kBadContexts = std::to_array<const char*>({
      "jpxl",
      "jpxl<bad>",
      "jpxl<1:>",
      "jpxl<1",
      "jpxl<1>[bad]",
      "jpxl<1>[s]",
      "jpxl<1>[s1]",
      "mj2t<1>[bad]",
  });
  for (const char* bad : kBadContexts) {
    kdu_window err_w;
    err_w.init();
    const char* err_p = err_w.parse_context(bad);
    EXPECT_NE(err_p, nullptr);
    EXPECT_TRUE(err_w.contexts.is_empty());
  }

  // 4. add_metareq redundancy elimination and parse_metareq.
  kdu_window mwin;
  mwin.init();
  mwin.add_metareq(FourCC('x', 'm', 'l', ' '), KDU_MRQ_GLOBAL, false, 100);
  mwin.add_metareq(FourCC('x', 'm', 'l', ' '), KDU_MRQ_GLOBAL, false,
                   50);  // Subsumed
  EXPECT_EQ(mwin.get_metareq(1), nullptr);
  mwin.add_metareq(FourCC('x', 'm', 'l', ' '), KDU_MRQ_DEFAULT, true, 500, true,
                   2, 4);
  mwin.add_metareq(0, KDU_MRQ_ALL, true, kIntMax, false, 0, kIntMax);
  mwin.copy_metareq_from(win);
  EXPECT_NE(mwin.get_metareq(0), nullptr);
  mwin.init_metareq();
  EXPECT_EQ(mwin.get_metareq(0), nullptr);

  mwin.init();
  EXPECT_EQ(mwin.parse_metareq("[*/sga!,jp2h:10,ftyp:r/w!]R5D2,[xml_/g]!!"),
            nullptr);
  EXPECT_TRUE(mwin.get_metadata_only());
  EXPECT_TRUE(mwin.have_metareq_all);
  EXPECT_TRUE(mwin.have_metareq_global);
  EXPECT_TRUE(mwin.have_metareq_stream);
  EXPECT_TRUE(mwin.have_metareq_window);
  ASSERT_NE(mwin.get_metareq(0), nullptr);
  EXPECT_EQ(mwin.get_metareq(0)->get_box_type(), FourCC('x', 'm', 'l', ' '));
  EXPECT_EQ(mwin.get_metareq(0)->get_next(), mwin.get_metareq(1));
  EXPECT_EQ(mwin.get_metareq(0)->get_qualifier(), KDU_MRQ_GLOBAL);

  constexpr auto kBadMetareqs = std::to_array<const char*>({
      "xml_",
      "[xml_",
      "[xml_]Rx",
      "[xml_]R1Dx",
      "[xml_:abc]",
      "[xml_/]",
      "[xml_?]",
      "[]",
      "[xml_]!!extra",
  });
  for (const char* bad : kBadMetareqs) {
    kdu_window err_m;
    err_m.init();
    EXPECT_NE(err_m.parse_metareq(bad), nullptr) << bad;
  }
}

TEST(KduClientWindowTest, WindowPrefsParsingWritingUpdateAndColourPriority) {
  // 1. set_pref, set_max_bandwidth, set_bandwidth_slice, and colour priority.
  kdu_window_prefs prefs;
  EXPECT_FALSE(prefs.set_pref(0));
  EXPECT_FALSE(prefs.set_pref(1 << 30));
  EXPECT_TRUE(prefs.set_pref(KDU_WINDOW_PREF_FULL, false));
  EXPECT_TRUE(prefs.set_pref(KDU_WINDOW_PREF_PROGRESSIVE, true));
  EXPECT_EQ(prefs.required & KDU_WINDOW_PREF_MASK, KDU_WINDOW_PREF_PROGRESSIVE);
  EXPECT_EQ(prefs.preferred & KDU_WINDOW_PREF_MASK, 0);
  EXPECT_TRUE(prefs.set_pref(KDU_CONCISENESS_PREF_CONCISE, false));
  EXPECT_TRUE(prefs.set_pref(KDU_CONCISENESS_PREF_LOOSE, true));
  EXPECT_TRUE(prefs.set_pref(KDU_PLACEHOLDER_PREF_EQUIV, false));
  EXPECT_TRUE(prefs.set_pref(KDU_CODESEQ_PREF_ILVD, false));
  prefs.set_max_bandwidth(2500000, true);
  prefs.set_bandwidth_slice(40, false);

  prefs.colour_meth_pref_limits[0] = 2;
  prefs.colour_meth_pref_limits[1] = 0;
  prefs.colour_meth_pref_limits[2] = 4;
  prefs.colour_meth_pref_limits[3] = 1;
  prefs.preferred |= KDU_COLOUR_METH_PREF;
  EXPECT_EQ(prefs.get_colour_description_priority(JP2_sRGB_SPACE, 1, 1), 0);
  EXPECT_EQ(prefs.get_colour_description_priority(JP2_sRGB_SPACE, 1, 3), 2);
  EXPECT_EQ(prefs.get_colour_description_priority(JP2_iccLUM_SPACE, 1, 0),
            3000);
  EXPECT_EQ(prefs.get_colour_description_priority(JP2_iccLUM_SPACE, 1, 2),
            2001);
  EXPECT_EQ(prefs.get_colour_description_priority(JP2_iccANY_SPACE, 2, 3), -1);
  EXPECT_EQ(prefs.get_colour_description_priority(JP2_iccANY_SPACE, 2, 5), 3);
  EXPECT_EQ(prefs.get_colour_description_priority(JP2_vendor_SPACE, 1, 0), 0);

  // 2. parse_prefs and write_prefs covering all tokens and suffixes.
  kdu_window_prefs parsed;
  ASSERT_EQ(
      parsed.parse_prefs(
          "fullwindow/r,loose,meta:orig/r,codeseq:sequential,mbw:5G/r,slice:16/"
          "r,color-enum:1;color-ricc:2;color-icc:9;color-vend,csf=density:1.5;"
          "angle:22.5;1000000;0.25/r"),
      nullptr);
  const int needed = parsed.write_prefs(nullptr);
  ASSERT_GT(needed, 0);
  std::string written(static_cast<std::size_t>(needed) + 1, '\0');
  EXPECT_EQ(parsed.write_prefs(written.data()), needed);
  // Note: "fullwindow/rloose" omits the separator comma before "loose" because
  // vendor kdu_window_prefs::write_prefs (kdu_client_window.cpp:1392-1408)
  // omits the leading `if (num_chars > 0) *(out++) = ',';` on conciseness.
  EXPECT_STREQ(
      written.c_str(),
      "fullwindow/rloose,meta:orig/r,codeseq:sequential,mbw:5G/r,slice:16/r,"
      "color-enum:1;color-ricc:2;color-icc:4;color-vend,"
      "csf=density:1.500000;angle:22.50000;1000000;0.250000/r");

  // Also cover T/M/K suffixes, other codeseq/placeholder variants, write_mask.
  kdu_window_prefs parsed2;
  ASSERT_EQ(parsed2.parse_prefs(
                "progressive,concise/r,meta:incr,codeseq:reverse-sequential,"
                "mbw:2T,slice:3"),
            nullptr);
  EXPECT_EQ(parsed2.max_bandwidth, 2000000000000LL);
  std::array<char, 256> mask_buf{};
  EXPECT_GT(parsed2.write_prefs(mask_buf.data(),
                                KDU_WINDOW_PREF_MASK | KDU_MAX_BANDWIDTH_PREF),
            0);
  EXPECT_STREQ(mask_buf.data(), "progressive,mbw:2T");

  kdu_window_prefs parsed3;
  ASSERT_EQ(parsed3.parse_prefs("mbw:5M"), nullptr);
  EXPECT_EQ(parsed3.write_prefs(mask_buf.data()), 6);
  EXPECT_STREQ(mask_buf.data(), "mbw:5M");
  ASSERT_EQ(parsed3.parse_prefs("mbw:7K"), nullptr);
  EXPECT_EQ(parsed3.write_prefs(mask_buf.data()), 6);
  EXPECT_STREQ(mask_buf.data(), "mbw:7K");

  // Two-row csf parsing (cleared immediately via init() to avoid MSan on the
  // known kdu_client_window.cpp:265 row-stride offset bug).
  ASSERT_EQ(parsed3.parse_prefs("csf=density:0.5;0.25,density:0.8;0.5"),
            nullptr);
  parsed3.init();

  // Invalid parse_prefs cases.
  constexpr auto kBadPrefs = std::to_array<const char*>({
      "fullwindow,progressive",
      "mbw:0",
      "slice:0",
      "color-unknown",
      "color-enum:0",
      "csf=bad",
      "csf=density:0.5;angle:bad",
      "csf=density:0.5;bad",
      "fullwindow;extra",
  });
  for (const char* bad : kBadPrefs) {
    kdu_window_prefs err_p;
    EXPECT_NE(err_p.parse_prefs(bad), nullptr) << bad;
  }

  // 3. update(), copy constructor, operator=, including multi-row CSF write.
  kdu_window_prefs base;
  base.set_pref(KDU_WINDOW_PREF_FULL, false);
  base.set_pref(KDU_CONCISENESS_PREF_CONCISE, false);
  base.set_pref(KDU_PLACEHOLDER_PREF_INCR, false);
  base.set_pref(KDU_CODESEQ_PREF_FWD, false);
  base.set_max_bandwidth(1000, false);
  base.set_bandwidth_slice(5, false);
  base.preferred |= KDU_COLOUR_METH_PREF | KDU_CONTRAST_SENSITIVITY_PREF;
  base.colour_meth_pref_limits[0] = 1;
  constexpr auto kMultiRowCsf =
      std::to_array<float>({1.0F, 0.0F, 0.5F, -1.0F, 2.0F, 45.0F, 0.8F, 0.2F});
  SetCsfTable(base, 2, 2, kMultiRowCsf);
  std::array<char, 256> csf_buf{};
  EXPECT_GT(base.write_prefs(csf_buf.data(), KDU_CONTRAST_SENSITIVITY_PREF), 0);

  kdu_window_prefs copy_p(base);
  EXPECT_EQ(base.update(copy_p), 0);  // Equal -> 0 delta

  copy_p.set_pref(KDU_WINDOW_PREF_PROGRESSIVE, true);
  copy_p.set_pref(KDU_CONCISENESS_PREF_LOOSE, true);
  copy_p.set_pref(KDU_PLACEHOLDER_PREF_ORIG, true);
  copy_p.set_pref(KDU_CODESEQ_PREF_BWD, false);
  copy_p.set_max_bandwidth(2000, false);
  copy_p.set_bandwidth_slice(10, false);
  copy_p.colour_meth_pref_limits[1] = 2;
  copy_p.csf_sensitivities[2] = 0.75F;
  copy_p.denied = KDU_WINDOW_PREF_FULL;
  const int updated = base.update(copy_p);
  EXPECT_NE(updated & KDU_WINDOW_PREF_MASK, 0);
  EXPECT_NE(updated & KDU_MAX_BANDWIDTH_PREF, 0);
  EXPECT_NE(updated & KDU_COLOUR_METH_PREF, 0);
  EXPECT_NE(updated & KDU_CONTRAST_SENSITIVITY_PREF, 0);
  EXPECT_EQ(base.denied & KDU_WINDOW_PREF_FULL, KDU_WINDOW_PREF_FULL);

  // Dimension change in csf_sensitivities.
  kdu_window_prefs smaller_csf;
  smaller_csf.preferred |= KDU_CONTRAST_SENSITIVITY_PREF;
  constexpr auto kSmallCsf = std::to_array<float>({1.5F, 0.0F, 0.4F});
  SetCsfTable(smaller_csf, 1, 1, kSmallCsf);
  EXPECT_NE(base.update(smaller_csf) & KDU_CONTRAST_SENSITIVITY_PREF, 0);

  kdu_window_prefs assigned;
  assigned = base;
  EXPECT_EQ(assigned.max_bandwidth, 2000);
}

TEST(KduClientWindowTest, WindowModelExplicitImplicitAndBlockQueries) {
  // 1. Empty state, init variants, stateless flag, and stream contexts.
  kdu_window_model model;
  EXPECT_FALSE(model.is_stateless());
  EXPECT_TRUE(model.is_empty());
  EXPECT_EQ(model.get_first_atomic_stream(), -1);

  model.init(true);
  EXPECT_TRUE(model.is_stateless());
  EXPECT_TRUE(model.is_empty());

  model.init(/*stateless=*/false, /*background_full=*/false,
             /*default_stream_idx=*/0);
  EXPECT_FALSE(model.is_stateless());
  EXPECT_TRUE(model.is_empty());

  // 2. Meta instructions (atomic + wildcard, additive + subtractive).
  model.add_instruction(KDU_META_DATABIN, 5, 0,
                        0);  // Non-subtractive 0 ignored
  EXPECT_TRUE(model.is_empty());
  model.add_instruction(KDU_META_DATABIN, 5, 0, 128);
  model.add_instruction(KDU_META_DATABIN, 6, KDU_WINDOW_MODEL_SUBTRACTIVE, -1);
  model.add_instruction(KDU_META_DATABIN, -1, 0, -1);  // All meta complete
  EXPECT_FALSE(model.is_empty());

  std::array<int, 2> buf{};
  kdu_long meta_id = 5;
  EXPECT_EQ(model.get_meta_instructions(meta_id, buf.data()), 1);
  // Complete wildcard (limit = -1) overrides the earlier 128-byte bound.
  EXPECT_EQ(buf[0], -1);
  meta_id = -1;  // Pops remaining atomic meta instruction (bin 6)
  EXPECT_EQ(model.get_meta_instructions(meta_id, buf.data()), 2);
  EXPECT_EQ(meta_id, 6);
  EXPECT_EQ(buf[1], 1);

  // 3. Header instructions (atomic, main & tile headers) in stateful mode.
  model.init(false, false, 0);
  model.add_instruction(KDU_MAIN_HEADER_DATABIN, 0, 0, 64);
  model.add_instruction(KDU_TILE_HEADER_DATABIN, 2, 0, 32);
  model.add_instruction(KDU_TILE_DATABIN, 0, 0, 10);
  model.set_codestream_context(0, 0);  // Reuse existing context

  EXPECT_EQ(model.get_first_atomic_stream(), 0);
  EXPECT_GT(model.get_header_instructions(0, -1, buf.data()), 0);
  EXPECT_EQ(buf[0], 64);
  int popped_tnum = -99;
  EXPECT_GT(model.get_header_instructions(0, &popped_tnum, buf.data()), 0);
  EXPECT_EQ(popped_tnum, 2);
  EXPECT_EQ(buf[0], 32);

  // 4. Precinct instructions (explicit + implicit, copy ctor, operator=,
  // append).
  kdu_window_model pm;
  pm.init(/*stateless=*/false, /*background_full=*/false,
          /*default_stream_idx=*/0);
  pm.add_instruction(KDU_PRECINCT_DATABIN, 10, KDU_WINDOW_MODEL_LAYERS, 3);
  pm.add_instruction(0, 0, 1, 1, 0, 0, 2, 3, KDU_WINDOW_MODEL_LAYERS, 4);
  pm.add_instruction(KDU_PRECINCT_DATABIN, 11, 0, kIntMax);

  kdu_window_model pm_copy(pm);
  kdu_window_model pm_assign;
  pm_assign = pm_copy;
  pm_assign.append(pm);

  int tnum = 0, cnum = 0, rnum = 0;
  kdu_long pnum = -1;
  EXPECT_GT(pm.get_precinct_instructions(0, tnum, cnum, rnum, pnum, buf.data()),
            0);
  EXPECT_EQ(tnum, -1);  // Explicit instruction popped first
  EXPECT_EQ(pnum, 10);
  EXPECT_EQ(buf[0], 2 * 3 + 1);

  EXPECT_GT(pm.get_precinct_instructions(0, tnum, cnum, rnum, pnum, buf.data()),
            0);
  EXPECT_EQ(tnum, 0);  // Implicit instruction popped (pmax > pmin => atomic)
  EXPECT_EQ(cnum, 1);
  EXPECT_EQ(rnum, 0);
  EXPECT_EQ(pnum, 2);

  EXPECT_GT(pm.get_precinct_instructions(0, tnum, cnum, rnum, pnum, buf.data()),
            0);
  EXPECT_EQ(tnum, -1);
  EXPECT_EQ(pnum, 11);

  // 5. Stateless background_full and get_precinct_block obliteration rules.
  kdu_window_model stateless_m;
  stateless_m.init(/*stateless=*/true, /*background_full=*/false, 0);
  // Stateless non-specific queries return 0.
  meta_id = -1;
  EXPECT_EQ(stateless_m.get_meta_instructions(meta_id, buf.data()), 0);
  EXPECT_EQ(stateless_m.get_header_instructions(0, &popped_tnum, buf.data()),
            0);
  EXPECT_EQ(stateless_m.get_precinct_instructions(0, tnum, cnum, rnum, pnum,
                                                  buf.data()),
            0);

  // Stateless multi-stream and all-codestream header instructions.
  stateless_m.set_codestream_context(1, 2);
  stateless_m.add_instruction(KDU_TILE_HEADER_DATABIN, 0,
                              KDU_WINDOW_MODEL_SUBTRACTIVE, kIntMax);
  stateless_m.set_codestream_context(-1, 0);
  stateless_m.add_instruction(KDU_MAIN_HEADER_DATABIN, -1, 0, -1);
  stateless_m.set_codestream_context(0, 0);
  EXPECT_GT(stateless_m.get_header_instructions(1, -1, buf.data()), 0);
  EXPECT_EQ(buf[0], -1);
  EXPECT_GT(stateless_m.get_header_instructions(1, 0, buf.data()), 0);

  // Additive layers vs subtractive bytes & additive bytes vs subtractive
  // layers.
  stateless_m.add_instruction(KDU_PRECINCT_DATABIN, 0, KDU_WINDOW_MODEL_LAYERS,
                              2);
  stateless_m.add_instruction(KDU_PRECINCT_DATABIN, 0,
                              KDU_WINDOW_MODEL_SUBTRACTIVE, 50);
  stateless_m.add_instruction(KDU_PRECINCT_DATABIN, 1, 0, 100);
  stateless_m.add_instruction(
      KDU_PRECINCT_DATABIN, 1,
      KDU_WINDOW_MODEL_LAYERS | KDU_WINDOW_MODEL_SUBTRACTIVE, 2);
  // Two subtractive instructions (bytes & layers) collapse to empty (2).
  stateless_m.add_instruction(KDU_PRECINCT_DATABIN, 2,
                              KDU_WINDOW_MODEL_SUBTRACTIVE, 40);
  stateless_m.add_instruction(
      KDU_PRECINCT_DATABIN, 2,
      KDU_WINDOW_MODEL_LAYERS | KDU_WINDOW_MODEL_SUBTRACTIVE, 3);
  // Two additive instructions keep latest; implicit 2D block overlaps
  // precinct 3.
  stateless_m.add_instruction(KDU_PRECINCT_DATABIN, 3, 0, 60);
  stateless_m.add_instruction(0, 1, 0, 0, 0, 0, 0, 1, KDU_WINDOW_MODEL_LAYERS,
                              4);
  // Exercises vendor union { int tmin; int databin_class; } quirk: tmin = -1
  // sets databin_class = -1 != KDU_PRECINCT_DATABIN, so get_precinct_block
  // skips it cleanly without modifying `blk`.
  stateless_m.add_instruction(-1, 0, -1, 0, -1, 0, -1, 0,
                              KDU_WINDOW_MODEL_LAYERS, -1);

  std::array<int, 8> blk{};
  EXPECT_TRUE(stateless_m.get_precinct_block(
      /*stream_idx=*/0, /*tnum=*/0, /*cnum=*/0, /*rnum=*/0, /*t_across=*/2,
      /*p_across=*/2, /*id_base=*/0, /*id_gap=*/1, MakeDims(0, 0, 2, 2),
      blk.data()));
  EXPECT_EQ(blk[0 * 2 + 1], 102);
  EXPECT_EQ(blk[1 * 2 + 1], 2);
  EXPECT_EQ(blk[2 * 2 + 1], 2);  // Collapsed empty

  // Exercise real implicit wildcard matching (cmin/rmin/pmin < 0 with tmin = 0
  // so databin_class == KDU_PRECINCT_DATABIN).
  kdu_window_model implicit_wild;
  implicit_wild.init(/*stateless=*/true, /*background_full=*/false, 0);
  implicit_wild.add_instruction(0, 0, -1, 0, -1, 0, -1, 0,
                                KDU_WINDOW_MODEL_LAYERS, -1);
  EXPECT_TRUE(implicit_wild.get_precinct_block(
      /*stream_idx=*/0, /*tnum=*/0, /*cnum=*/0, /*rnum=*/0, /*t_across=*/2,
      /*p_across=*/2, /*id_base=*/0, /*id_gap=*/1, MakeDims(0, 0, 2, 2),
      blk.data()));
  EXPECT_EQ(blk[0], -1);
  EXPECT_EQ(blk[1], 0);

  kdu_window_model bkgnd;
  bkgnd.init(/*stateless=*/true, /*background_full=*/true, 0);
  EXPECT_FALSE(bkgnd.is_empty());
  bkgnd.add_instruction(KDU_META_DATABIN, 0, KDU_WINDOW_MODEL_SUBTRACTIVE, 16);
  bkgnd.add_instruction(KDU_MAIN_HEADER_DATABIN, 0, 0, 32);
  bkgnd.add_instruction(KDU_PRECINCT_DATABIN, 0, KDU_WINDOW_MODEL_LAYERS, 1);
  bkgnd.add_instruction(KDU_PRECINCT_DATABIN, 1, 0, 4);
  meta_id = 0;
  EXPECT_EQ(bkgnd.get_meta_instructions(meta_id, buf.data()), 2);
  EXPECT_EQ(buf[0], 16);
  EXPECT_EQ(buf[1], 17);
  EXPECT_EQ(bkgnd.get_header_instructions(0, -1, buf.data()), 2);
  EXPECT_EQ(buf[0], 32);
  EXPECT_EQ(buf[1], 33);
  EXPECT_TRUE(bkgnd.get_precinct_block(0, 0, 0, 0, 1, 2, 0, 1,
                                       MakeDims(0, 0, 2, 1), blk.data()));
  EXPECT_EQ(blk[0], 0);
  EXPECT_EQ(blk[1], 2);
  EXPECT_EQ(blk[2], 8);
  EXPECT_EQ(blk[3], 10);
}

}  // namespace
}  // namespace kdu_supp
