// Unit tests for third_party/kakadu/vendor/coresys/messaging/messaging.cpp
// and third_party/kakadu/vendor/coresys/common/kdu_messaging.h.

#include <cstddef>
#include <string>
#include <thread>  // NOLINT
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_elementary.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_core {
namespace {

class ThrowingErrorHandler final : public kdu_thread_safe_message {
 public:
  void put_text(const char* text) override { current_ += text; }
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      kdu_thread_safe_message::flush(true);
      current_.clear();
      throw KDU_ERROR_EXCEPTION;
    }
    kdu_thread_safe_message::flush(false);
  }

 private:
  std::string current_;
};

class SilentWarningHandler final : public kdu_thread_safe_message {};

class NonLockingSpyMessage final : public kdu_message {
 public:
  void start_message() override { ++start_calls_; }
  void put_text(const char* text) override { text_ += text; }
  void put_text(const kdu_uint16* /*text*/) override { ++unicode_calls_; }
  void flush(bool /*end_of_message*/ = false) override { ++flush_calls_; }

  int start_calls() const { return start_calls_; }
  int flush_calls() const { return flush_calls_; }
  int unicode_calls() const { return unicode_calls_; }
  const std::string& text() const { return text_; }

 private:
  int start_calls_ = 0;
  int flush_calls_ = 0;
  int unicode_calls_ = 0;
  std::string text_;
};

class CapturingMessageHandler final : public kdu_thread_safe_message {
 public:
  explicit CapturingMessageHandler(bool throw_on_end = false)
      : throw_on_end_(throw_on_end) {}

  void put_text(const char* text) override { current_ += text; }

  void put_text(const kdu_uint16* text) override {
    ++unicode_calls_;
    for (; *text != 0; ++text) {
      current_.push_back(static_cast<char>(*text & 0x7F));
    }
  }

  void flush(bool end_of_message = false) override {
    ++flush_calls_;
    if (end_of_message) {
      messages_.push_back(current_);
      last_message_ = std::move(current_);
      current_.clear();
      kdu_thread_safe_message::flush(true);
      if (throw_on_end_) {
        throw KDU_ERROR_EXCEPTION;
      }
      return;
    }
    kdu_thread_safe_message::flush(false);
  }

  const std::string& last_message() const { return last_message_; }
  const std::vector<std::string>& messages() const { return messages_; }
  int flush_calls() const { return flush_calls_; }
  int unicode_calls() const { return unicode_calls_; }

 private:
  bool throw_on_end_;
  std::string current_;
  std::string last_message_;
  std::vector<std::string> messages_;
  int flush_calls_ = 0;
  int unicode_calls_ = 0;
};

class AutoPopCapturingQueue final : public kdu_message_queue {
 public:
  const char* pop_message() override {
    const char* msg = kdu_message_queue::pop_message();
    if (msg != nullptr) {
      popped_.emplace_back(msg);
    }
    return msg;
  }

  const std::vector<std::string>& popped() const { return popped_; }

 private:
  std::vector<std::string> popped_;
};

kdu_message* GlobalErrorHandler() {
  static auto* const handler = new ThrowingErrorHandler();
  return handler;
}

kdu_message* GlobalWarningHandler() {
  static auto* const handler = new SilentWarningHandler();
  return handler;
}

class MessagingTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    kdu_customize_errors(GlobalErrorHandler());
    kdu_customize_warnings(GlobalWarningHandler());
  }
};

const ::testing::Environment* const kMessagingEnv =
    ::testing::AddGlobalTestEnvironment(new MessagingTestEnvironment());

TEST(MessagingTest, BaseKduMessageVirtualsAndStreamOperators) {
  // Base kdu_message is concrete and safely discards output by default.
  kdu_message base;
  const kdu_uint16 u16[] = {'O', 'K', 0};
  base.start_message();
  base.put_text("ignored");
  base.put_text(u16);
  base.flush(true);

  CapturingMessageHandler cap;
  cap.start_message();
  cap << "str:" << 'X' << " int:" << -42 << " uint:" << 99U
      << " short:" << static_cast<short>(-7)
      << " ushort:" << static_cast<unsigned short>(15U);
  EXPECT_FALSE(cap.set_hex_mode(true));
  cap << " hex:" << 0x2a << " uhex:" << 0xbeefU;
  EXPECT_TRUE(cap.set_hex_mode(false));
  cap << " i64z:" << static_cast<kdu_int64>(0)
      << " i64:" << static_cast<kdu_int64>(-1234567890123LL)
      << " u64z:" << static_cast<kdu_uint64>(0)
      << " u64:" << static_cast<kdu_uint64>(9876543210123ULL) << " flt:" << 1.5F
      << " dbl:" << -2.25;
  cap.flush(false);
  EXPECT_EQ(cap.flush_calls(), 1);
  cap.flush(true);

  ASSERT_EQ(cap.messages().size(), 1U);
  const std::string& out = cap.last_message();
  EXPECT_NE(out.find("str:X int:-42 uint:99 short:-7 ushort:15"),
            std::string::npos);
  EXPECT_NE(out.find("hex:2a uhex:beef"), std::string::npos);
  EXPECT_NE(out.find("i64z:0 i64:-1,234,567,890,123"), std::string::npos);
  EXPECT_NE(out.find("u64z:0 u64:9,876,543,210,123"), std::string::npos);
  EXPECT_NE(out.find("flt:1.500000 dbl:-2.250000"), std::string::npos);
}

TEST(MessagingTest, ThreadSafeMessageSerializesConcurrentMessages) {
  CapturingMessageHandler cap;
  // flush(false) must keep the mutex locked while building one message.
  cap.start_message();
  cap << "single-";
  cap.flush(false);
  cap << "thread";
  cap.flush(true);
  EXPECT_EQ(cap.last_message(), "single-thread");

  constexpr int kThreads = 4;
  constexpr int kMsgsPerThread = 12;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&cap, t]() {
      for (int m = 0; m < kMsgsPerThread; ++m) {
        cap.start_message();
        cap << "[" << t << ":" << m << "]";
        cap.flush(true);
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }

  ASSERT_EQ(cap.messages().size(),
            static_cast<size_t>(1 + kThreads * kMsgsPerThread));
  for (size_t i = 1; i < cap.messages().size(); ++i) {
    const std::string& msg = cap.messages()[i];
    ASSERT_FALSE(msg.empty());
    EXPECT_EQ(msg.front(), '[');
    EXPECT_EQ(msg.back(), ']');
    EXPECT_EQ(msg.find('[', 1), std::string::npos);
  }
}

TEST(MessagingTest, CustomizeTextBinPackingLookupAndErrorWarning) {
  static constexpr char kCtxPack1[] = "CtxBinPack";
  static constexpr char kCtxPack2[] = "CtxBinPack2";
  static constexpr char kCtxEmptyLead[] = "CtxEmptyLead";
  static constexpr char kCtxUnreg[] = "CtxUnregistered";
  static constexpr char kLead1[] = "Lead1:";
  static constexpr char kEmpty[] = "";
  static constexpr char kMultiText[] = "seg1\0seg2\0";
  static constexpr kdu_uint16 kU16Lead[] = {'U', ':', 0};
  static constexpr kdu_uint16 kU16Text[] = {'u', 't', 0};

  // Fill 1 context + 63 ASCII elements (total 64 bins in the first store),
  // then overwrite an existing ASCII entry to cover the update path.
  for (kdu_uint32 id = 0; id < 63; ++id) {
    kdu_customize_text(kCtxPack1, id, kLead1, kMultiText);
  }
  kdu_customize_text(kCtxPack1, 0, kLead1, kMultiText);

  // Registering an ASCII entry before Unicode avoids kd_text_register's
  // uninitialized context elt list when creating a brand-new context via the
  // Unicode overload. Calling twice covers new-store alloc + existing update.
  kdu_customize_text(kCtxPack1, 999, kU16Lead, kU16Text);
  kdu_customize_text(kCtxPack1, 999, kU16Lead, kU16Text);

  // Consume the remaining 63 bins in the second store, then open a new context
  // and another 63 IDs to hit new-store allocation for both contexts and elts.
  for (kdu_uint32 id = 1000; id < 1063; ++id) {
    kdu_customize_text(kCtxPack1, id, kEmpty, kMultiText);
  }
  kdu_customize_text(kCtxPack2, 1, kLead1, kMultiText);
  for (kdu_uint32 id = 2; id < 65; ++id) {
    kdu_customize_text(kCtxPack2, id, kEmpty, kMultiText);
  }
  kdu_customize_text(kCtxEmptyLead, 7, kEmpty, kMultiText);

  CapturingMessageHandler warn_cap;
  CapturingMessageHandler err_cap(/*throw_on_end=*/true);
  absl::Cleanup restore = [] {
    kdu_customize_warnings(GlobalWarningHandler());
    kdu_customize_errors(GlobalErrorHandler());
  };

  // NULL warning handler silently discards messages.
  kdu_customize_warnings(nullptr);
  {
    kdu_warning w;
    w << "discarded";
  }

  kdu_customize_warnings(&warn_cap);
  kdu_customize_errors(&err_cap);

  {
    kdu_warning w;
    w << "default";
    w.flush(true);  // Must forward flush(false) until destructor.
  }
  EXPECT_EQ(warn_cap.last_message(), "Kakadu Warning:\ndefault");

  {
    kdu_warning w("WarnLead:");
    w << " body";
  }
  EXPECT_EQ(warn_cap.last_message(), "WarnLead: body");

  {
    kdu_warning w("");
    w << "no_lead";
  }
  EXPECT_EQ(warn_cap.last_message(), "no_lead");

  // Unregistered (context, id) suppresses kdu_warning completely. Use a
  // non-locking kdu_message here because vendor kdu_warning(context, id)
  // calls handler->start_message() before nulling handler when unregistered,
  // skipping flush(true) on destruction.
  {
    NonLockingSpyMessage unreg_spy;
    kdu_customize_warnings(&unreg_spy);
    absl::Cleanup restore_warn_cap = [&warn_cap] {
      kdu_customize_warnings(&warn_cap);
    };
    const kdu_uint16 u16_ignored[] = {'?', 0};
    {
      kdu_warning w(kCtxUnreg, 42);
      w << "suppressed";
      w.put_text(u16_ignored);
      w.flush(false);
    }
    EXPECT_EQ(unreg_spy.start_calls(), 1);
    EXPECT_EQ(unreg_spy.flush_calls(), 0);
    EXPECT_EQ(unreg_spy.unicode_calls(), 0);
    EXPECT_TRUE(unreg_spy.text().empty());
  }

  // Registered (context, id) with non-empty lead_in and empty lead_in.
  {
    kdu_warning w(kCtxPack1, 0);
    w << "<#>" << "|" << "<#>" << "|" << "<#>";
    const kdu_uint16 u16_tail[] = {'!', 0};
    w.put_text(u16_tail);
  }
  EXPECT_EQ(warn_cap.last_message(), "Lead1:seg1|seg2|<#>!");
  EXPECT_EQ(warn_cap.unicode_calls(), 1);

  {
    kdu_warning w(kCtxEmptyLead, 7);
    w << "<#>";
  }
  EXPECT_EQ(warn_cap.last_message(), "seg1");

  kdu_print_warning("shortcut-warn");
  EXPECT_EQ(warn_cap.last_message(), "Kakadu Warning:\nshortcut-warn");

  // kdu_error constructors and throw behavior.
  EXPECT_THROW(
      {
        kdu_error e;
        e << "err-def";
        e.flush(true);
      },
      kdu_exception);
  EXPECT_EQ(err_cap.last_message(), "Kakadu Error:\nerr-def");

  EXPECT_THROW(
      {
        kdu_error e("ErrLead:");
        e << " x";
      },
      kdu_exception);
  EXPECT_EQ(err_cap.last_message(), "ErrLead: x");

  EXPECT_THROW(
      {
        kdu_error e("");
        e << "empty-lead";
      },
      kdu_exception);
  EXPECT_EQ(err_cap.last_message(), "empty-lead");

  EXPECT_THROW(
      {
        kdu_error e(kCtxUnreg, 42);
        e << "details";
      },
      kdu_exception);
  EXPECT_NE(err_cap.last_message().find("Untranslated error"),
            std::string::npos);

  // Known context with unregistered ID also falls back to untranslated error.
  EXPECT_THROW(
      {
        kdu_error e(kCtxPack1, 999999);
        e << "missing-id";
      },
      kdu_exception);
  EXPECT_NE(err_cap.last_message().find("Untranslated error"),
            std::string::npos);

  static constexpr kdu_uint16 kU16Err[] = {'?', 0};
  EXPECT_THROW(
      {
        kdu_error e(kCtxPack1, 0);
        e << "<#>" << "-" << "<#>" << "-" << "<#>";
        e.put_text(kU16Err);
      },
      kdu_exception);
  EXPECT_EQ(err_cap.last_message(), "Lead1:seg1-seg2-<#>?");

  EXPECT_THROW(
      {
        kdu_error e(kCtxEmptyLead, 7);
        e << "<#>";
      },
      kdu_exception);
  EXPECT_EQ(err_cap.last_message(), "seg1");

  EXPECT_THROW(kdu_print_error("shortcut-err"), kdu_exception);
  EXPECT_EQ(err_cap.last_message(), "Kakadu Error:\nshortcut-err");
}

TEST(MessagingTest, MessageQueueGrowthTruncationRingBufferAndAutoPop) {
  kdu_message_queue q;
  EXPECT_EQ(q.pop_message(), nullptr);
  q.flush(true);  // No-op when no message is active (active_msg == NULL).

  // Small (<10 bytes) then >10 bytes growth. Do not call pop_message() while
  // the message lock is held (between start_message() and flush(true)).
  q.start_message();
  q.put_text("hi");
  q.flush(false);
  q.put_text(" and longer text > 10 bytes");
  q.flush(true);
  const char* first = q.pop_message();
  ASSERT_NE(first, nullptr);
  EXPECT_STREQ(first, "hi and longer text > 10 bytes");
  EXPECT_EQ(q.pop_message(), nullptr);

  // Exact cap truncation at KDU_MAX_MESSAGE_BYTES (65536).
  q.start_message();
  std::string huge(70000, 'x');
  q.put_text(huge.c_str());
  q.put_text("extra");
  q.flush(true);
  const char* big = q.pop_message();
  ASSERT_NE(big, nullptr);
  EXPECT_EQ(std::string(big).size(), 65536U);

  // Mixed 1-, 2-, and 3-byte UTF-16 encoding plus truncation > 65536 bytes.
  q.start_message();
  const kdu_uint16 utf16_mixed[] = {'A', 0x03B1, 0x20AC, 'B', 'C',
                                    'D', 'E',    'F',    'G', 0};
  q.put_text(utf16_mixed);
  q.flush(true);
  const char* utf8 = q.pop_message();
  ASSERT_NE(utf8, nullptr);
  EXPECT_STREQ(utf8,
               "A\xCE\xB1\xE2\x82\xAC"
               "BCDEFG");

  q.start_message();
  q.put_text(
      "0123456789ab");  // Grow max_len above 10 before large UTF-16 call.
  std::vector<kdu_uint16> huge_u16(22000, 0x20AC);
  huge_u16.push_back(0);
  q.put_text(huge_u16.data());
  q.flush(true);
  const char* big_u8 = q.pop_message();
  ASSERT_NE(big_u8, nullptr);
  EXPECT_LE(std::string(big_u8).size(), 65536U);

  // 1-byte and 2-byte UTF-16 truncation at KDU_MAX_MESSAGE_BYTES (65536).
  q.start_message();
  std::vector<kdu_uint16> huge_u16_1b(66000, 'A');
  huge_u16_1b.push_back(0);
  q.put_text(huge_u16_1b.data());
  q.flush(true);
  const char* big_u8_1b = q.pop_message();
  ASSERT_NE(big_u8_1b, nullptr);
  EXPECT_EQ(std::string(big_u8_1b).size(), 65536U);

  q.start_message();
  std::vector<kdu_uint16> huge_u16_2b(33000, 0x03B1);
  huge_u16_2b.push_back(0);
  q.put_text(huge_u16_2b.data());
  q.flush(true);
  const char* big_u8_2b = q.pop_message();
  ASSERT_NE(big_u8_2b, nullptr);
  EXPECT_EQ(std::string(big_u8_2b).size(), 65536U);

  // max_queued_messages < 1 clamps to 1, dropping earlier messages.
  q.configure(/*max_queued_messages=*/0, /*auto_pop=*/false,
              /*throw_exceptions=*/false);
  q.start_message();
  q.put_text("first-dropped");
  q.flush(true);
  q.start_message();
  q.put_text("only-kept");
  q.flush(true);
  EXPECT_STREQ(q.pop_message(), "only-kept");
  EXPECT_EQ(q.pop_message(), nullptr);

  // Configure depth 2, overflow ring buffer, and verify destructor frees both
  // popped_msg and remaining queued elements.
  {
    kdu_message_queue scoped_q;
    scoped_q.configure(/*max_queued_messages=*/2, /*auto_pop=*/false,
                       /*throw_exceptions=*/false);
    scoped_q.start_message();
    scoped_q.put_text("m1");
    scoped_q.flush(true);
    scoped_q.start_message();
    scoped_q.put_text("m2");
    scoped_q.flush(true);
    scoped_q.start_message();
    scoped_q.put_text("m3");
    scoped_q.flush(true);
    EXPECT_STREQ(scoped_q.pop_message(), "m2");
  }

  // auto_pop + throw_exceptions via derived pop_message override.
  AutoPopCapturingQueue auto_q;
  auto_q.configure(/*max_queued_messages=*/4, /*auto_pop=*/true,
                   /*throw_exceptions=*/true, KDU_ERROR_EXCEPTION);
  auto_q.start_message();
  auto_q.put_text("auto-popped-err");
  EXPECT_THROW(auto_q.flush(true), kdu_exception);
  ASSERT_EQ(auto_q.popped().size(), 1U);
  EXPECT_EQ(auto_q.popped()[0], "auto-popped-err");
}

TEST(MessagingTest, MessageFormatterWrappingIndentsAndNullDest) {
  {
    kdu_message_formatter null_fmt(nullptr, 40);
    const kdu_uint16 u16[] = {'a', 0};
    null_fmt.start_message();
    null_fmt.put_text("ignored");
    null_fmt.put_text(u16);
    null_fmt.flush(true);
  }

  // Wide line_chars (> 40) to test clamping max_line > 200 and clamping indent.
  CapturingMessageHandler cap_wide;
  {
    kdu_message_formatter wide_fmt(&cap_wide, 250);
    wide_fmt.start_message();
    wide_fmt.put_text(
        "\t\t\t\t\t\t\t\t\t\t\tx\n");  // 11 tabs > max_indent (40).
    wide_fmt.set_master_indent(50);    // Clamped to 40.
    wide_fmt.put_text("indented\n");
    wide_fmt.set_master_indent(-5);  // Clamped back to 0.
    wide_fmt.put_text("unflushed-tail");
    wide_fmt.flush(true);
    EXPECT_EQ(cap_wide.flush_calls(), 2);
  }  // Destructor calls dest->flush(false).
  EXPECT_EQ(cap_wide.flush_calls(), 3);
  EXPECT_NE(cap_wide.last_message().find("unflushed-tail\n"),
            std::string::npos);

  // Narrow width (16 chars) for tab conversion, \f, wrapping, extra spaces,
  // long words, and mid-line set_master_indent auto-flush.
  CapturingMessageHandler cap_narrow;
  kdu_message_formatter fmt(&cap_narrow, 16);
  fmt.start_message();
  fmt.put_text("\tmid\ttab\fword wrap\n");
  fmt.put_text("0123456789abcdef   next\n");
  fmt.put_text("abcdefghijklmnopqrstuv\n");
  fmt.put_text("before-indent");
  fmt.set_master_indent(2);  // Auto-flushes partial line mid-paragraph.
  fmt.put_text("after\nnextline");
  fmt.flush(true);

  const std::string& formatted = cap_narrow.last_message();
  EXPECT_NE(formatted.find("    mid tab\fword\n"), std::string::npos);
  EXPECT_NE(formatted.find("0123456789abcdef\n"), std::string::npos);
  EXPECT_NE(formatted.find("next\n"), std::string::npos);
  EXPECT_NE(formatted.find("abcdefghijklmnop\n"), std::string::npos);
  EXPECT_NE(formatted.find("  after\n"), std::string::npos);
  EXPECT_NE(formatted.find("  nextline\n"), std::string::npos);
}

}  // namespace
}  // namespace kdu_core
