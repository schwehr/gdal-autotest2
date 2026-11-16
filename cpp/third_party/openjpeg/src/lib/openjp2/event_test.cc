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
//
// Tests internal OpenJPEG event manager and message dispatch utilities
// (src/lib/openjp2/event.c).
//
// Caveats:
// - OPJ_CODE_NOT_USED blocks in event.c are not compiled in google3.
// - OPJ_MSG_SIZE (512 bytes) is file-static in event.c, so truncation checks
//   mirror kMaxFormattedMessageLength = 511 and must be updated if upstream
//   changes OPJ_MSG_SIZE.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/event.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/event.h

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);
static_assert(sizeof(opj_event_mgr_t) > 0);
static_assert(EVT_ERROR == 1);
static_assert(EVT_WARNING == 2);
static_assert(EVT_INFO == 4);

// Mirrors OPJ_MSG_SIZE (512) in src/lib/openjp2/event.c: 511 payload characters
// plus the mandatory trailing NUL byte.
constexpr size_t kMaxFormattedMessageLength = 511;

struct EventLevelCase {
  OPJ_INT32 event_type;
  std::string_view name;
};

constexpr EventLevelCase kValidLevels[] = {
    {.event_type = EVT_ERROR, .name = "EVT_ERROR"},
    {.event_type = EVT_WARNING, .name = "EVT_WARNING"},
    {.event_type = EVT_INFO, .name = "EVT_INFO"},
};

struct TruncationCase {
  size_t input_length;
  size_t expected_length;
};

constexpr TruncationCase kTruncationCases[] = {
    {.input_length = 510, .expected_length = 510},
    {.input_length = 511, .expected_length = 511},
    {.input_length = 512, .expected_length = 511},
    {.input_length = 700, .expected_length = 511},
};

struct NullClientDataTrace {
  size_t call_count = 0;
  bool saw_null_client_data = false;
  std::array<char, 64> last_message{};
  size_t last_message_len = 0;
};

// Closure-less opj_msg_callback functions cannot capture lambdas for null
// client_data tests; reset via absl::Cleanup inside the test for order safety.
constinit NullClientDataTrace g_null_client_data_trace{};

void RecordWithNullableClientData(const char* msg, void* client_data) {
  ++g_null_client_data_trace.call_count;
  g_null_client_data_trace.saw_null_client_data = (client_data == nullptr);
  if (msg != nullptr) {
    const std::string_view sv(msg);
    const size_t copy_len =
        std::min(sv.size(), g_null_client_data_trace.last_message.size());
    std::copy_n(sv.data(), copy_len,
                g_null_client_data_trace.last_message.begin());
    g_null_client_data_trace.last_message_len = copy_len;
  }
}

opj_event_mgr_t MakeCapturedEventManager(EventCapture* error_capture,
                                         EventCapture* warning_capture,
                                         EventCapture* info_capture) {
  opj_event_mgr_t mgr{};
  if (error_capture != nullptr) {
    mgr.error_handler = &EventCapture::OnError;
    mgr.m_error_data = error_capture;
  }
  if (warning_capture != nullptr) {
    mgr.warning_handler = &EventCapture::OnWarning;
    mgr.m_warning_data = warning_capture;
  }
  if (info_capture != nullptr) {
    mgr.info_handler = &EventCapture::OnInfo;
    mgr.m_info_data = info_capture;
  }
  return mgr;
}

opj_event_mgr_t MakeSharedEventManager(EventCapture* capture) {
  return MakeEventManager(capture);
}

const std::vector<std::string>& MessagesForLevel(const EventCapture& capture,
                                                 OPJ_INT32 event_type) {
  switch (event_type) {
    case EVT_ERROR:
      return capture.errors;
    case EVT_WARNING:
      return capture.warnings;
    case EVT_INFO:
      return capture.infos;
    default:
      ADD_FAILURE() << "Unexpected event_type: " << event_type;
      return capture.infos;
  }
}

std::string MakeDeterministicPattern(size_t length) {
  std::string out(length, '\0');
  for (size_t i = 0; i < length; ++i) {
    out[i] = static_cast<char>('A' + (i % 26));
  }
  return out;
}

TEST(EventTest, DefaultEventHandlerInitializesAndExecutesSilently) {
  EventCapture prior_capture;
  opj_event_mgr_t mgr = MakeSharedEventManager(&prior_capture);

  opj_set_default_event_handler(&mgr);

  EXPECT_EQ(nullptr, mgr.m_error_data);
  EXPECT_EQ(nullptr, mgr.m_warning_data);
  EXPECT_EQ(nullptr, mgr.m_info_data);
  ASSERT_NE(nullptr, mgr.error_handler);
  ASSERT_NE(nullptr, mgr.warning_handler);
  ASSERT_NE(nullptr, mgr.info_handler);
  EXPECT_NE(&EventCapture::OnError, mgr.error_handler);
  EXPECT_NE(&EventCapture::OnWarning, mgr.warning_handler);
  EXPECT_NE(&EventCapture::OnInfo, mgr.info_handler);
  EXPECT_EQ(mgr.error_handler, mgr.warning_handler);
  EXPECT_EQ(mgr.warning_handler, mgr.info_handler);

  // Invoking opj_event_msg for all 3 levels exercises opj_default_callback.
  EXPECT_EQ(OPJ_TRUE, opj_event_msg(&mgr, EVT_ERROR, "default error %d", 1));
  EXPECT_EQ(OPJ_TRUE,
            opj_event_msg(&mgr, EVT_WARNING, "default warning %s", "text"));
  EXPECT_EQ(OPJ_TRUE, opj_event_msg(&mgr, EVT_INFO, "default info"));
  EXPECT_EQ(OPJ_TRUE, opj_event_msg(&mgr, EVT_ERROR, nullptr));
  EXPECT_EQ(OPJ_FALSE, opj_event_msg(&mgr, 999, "unknown"));

  EXPECT_THAT(prior_capture.errors, IsEmpty());
  EXPECT_THAT(prior_capture.warnings, IsEmpty());
  EXPECT_THAT(prior_capture.infos, IsEmpty());
}

TEST(EventTest, CustomHandlersRouteDistinctUserDataAndFormatArguments) {
  EventCapture error_capture;
  EventCapture warning_capture;
  EventCapture info_capture;
  opj_event_mgr_t mgr =
      MakeCapturedEventManager(&error_capture, &warning_capture, &info_capture);

  EXPECT_EQ(OPJ_TRUE, opj_event_msg(&mgr, EVT_ERROR, "tile %d/%u: %s (0x%04x)",
                                    -3, 16u, "bad marker", 0xFF90u));
  EXPECT_EQ(OPJ_TRUE, opj_event_msg(&mgr, EVT_WARNING, "progress 100%% done"));
  EXPECT_EQ(OPJ_TRUE, opj_event_msg(&mgr, EVT_INFO, ""));
  EXPECT_EQ(OPJ_TRUE, opj_event_msg(&mgr, EVT_INFO, "second %s", "info"));

  EXPECT_THAT(error_capture.errors,
              ElementsAre("tile -3/16: bad marker (0xff90)"));
  EXPECT_THAT(error_capture.warnings, IsEmpty());
  EXPECT_THAT(error_capture.infos, IsEmpty());

  EXPECT_THAT(warning_capture.errors, IsEmpty());
  EXPECT_THAT(warning_capture.warnings, ElementsAre("progress 100% done"));
  EXPECT_THAT(warning_capture.infos, IsEmpty());

  EXPECT_THAT(info_capture.errors, IsEmpty());
  EXPECT_THAT(info_capture.warnings, IsEmpty());
  EXPECT_THAT(info_capture.infos, ElementsAre("", "second info"));
}

TEST(EventTest, CustomHandlersReceiveNullClientDataWhenConfigured) {
  g_null_client_data_trace = NullClientDataTrace{};
  absl::Cleanup reset_trace = [] {
    g_null_client_data_trace = NullClientDataTrace{};
  };

  opj_event_mgr_t mgr{};
  mgr.error_handler = &RecordWithNullableClientData;
  mgr.warning_handler = &RecordWithNullableClientData;
  mgr.info_handler = &RecordWithNullableClientData;

  size_t index = 0;
  for (const auto& level : kValidLevels) {
    SCOPED_TRACE(std::string(level.name));
    EXPECT_EQ(OPJ_TRUE,
              opj_event_msg(&mgr, level.event_type, "null-data-%zu", index));
    ++index;
    EXPECT_EQ(index, g_null_client_data_trace.call_count);
    EXPECT_TRUE(g_null_client_data_trace.saw_null_client_data);
    const std::string_view recorded(
        g_null_client_data_trace.last_message.data(),
        g_null_client_data_trace.last_message_len);
    EXPECT_EQ("null-data-" + std::to_string(index - 1), recorded);
  }
}

TEST(EventTest, NullEventManagerReturnsFalseForAllEventTypes) {
  constexpr OPJ_INT32 kEventTypes[] = {EVT_ERROR, EVT_WARNING, EVT_INFO, 0,
                                       999};
  for (OPJ_INT32 event_type : kEventTypes) {
    SCOPED_TRACE(event_type);
    EXPECT_EQ(OPJ_FALSE, opj_event_msg(nullptr, event_type, "ignored %d", 42));
    EXPECT_EQ(OPJ_FALSE, opj_event_msg(nullptr, event_type, nullptr));
  }
}

TEST(EventTest, NullHandlerForLevelReturnsFalseWithoutInvokingOtherLevels) {
  // Verify each level independently when its handler is null (with non-null
  // user data) while the other two levels remain registered.
  for (const auto& level : kValidLevels) {
    SCOPED_TRACE(std::string(level.name));
    EventCapture capture;
    opj_event_mgr_t mgr = MakeSharedEventManager(&capture);
    switch (level.event_type) {
      case EVT_ERROR:
        mgr.error_handler = nullptr;
        break;
      case EVT_WARNING:
        mgr.warning_handler = nullptr;
        break;
      case EVT_INFO:
        mgr.info_handler = nullptr;
        break;
      default:
        FAIL() << "Unexpected event_type: " << level.event_type;
    }

    EXPECT_EQ(OPJ_FALSE,
              opj_event_msg(&mgr, level.event_type, "dropped %d", 1));
    EXPECT_EQ(OPJ_FALSE, opj_event_msg(&mgr, level.event_type, nullptr));
    EXPECT_THAT(capture.errors, IsEmpty());
    EXPECT_THAT(capture.warnings, IsEmpty());
    EXPECT_THAT(capture.infos, IsEmpty());
  }

  // Zero-initialized manager with all handlers and user data null returns
  // OPJ_FALSE.
  opj_event_mgr_t empty_mgr{};
  for (const auto& level : kValidLevels) {
    EXPECT_EQ(OPJ_FALSE,
              opj_event_msg(&empty_mgr, level.event_type, "dropped"));
  }
}

TEST(EventTest, UnknownEventTypeReturnsFalseWithoutInvokingHandlers) {
  EventCapture capture;
  opj_event_mgr_t mgr = MakeSharedEventManager(&capture);

  // Includes bitflag combinations (3, 5, 6, 7) to confirm exact switch
  // dispatch.
  constexpr OPJ_INT32 kUnknownEventTypes[] = {
      0,
      3,
      5,
      6,
      7,
      -1,
      8,
      999,
      std::numeric_limits<OPJ_INT32>::min(),
      std::numeric_limits<OPJ_INT32>::max(),
  };

  for (OPJ_INT32 event_type : kUnknownEventTypes) {
    SCOPED_TRACE(event_type);
    EXPECT_EQ(OPJ_FALSE, opj_event_msg(&mgr, event_type, "unknown %d", 7));
    EXPECT_EQ(OPJ_FALSE, opj_event_msg(&mgr, event_type, nullptr));
  }

  EXPECT_THAT(capture.errors, IsEmpty());
  EXPECT_THAT(capture.warnings, IsEmpty());
  EXPECT_THAT(capture.infos, IsEmpty());
}

TEST(EventTest, NullFormatSkipsCallbackAndReturnsTrueWhenHandlerIsSet) {
  EventCapture capture;
  opj_event_mgr_t mgr = MakeSharedEventManager(&capture);

  for (const auto& level : kValidLevels) {
    SCOPED_TRACE(std::string(level.name));
    EXPECT_EQ(OPJ_TRUE, opj_event_msg(&mgr, level.event_type, nullptr));
  }

  EXPECT_THAT(capture.errors, IsEmpty());
  EXPECT_THAT(capture.warnings, IsEmpty());
  EXPECT_THAT(capture.infos, IsEmpty());

  // Subsequent formatted calls continue dispatching normally.
  EXPECT_EQ(OPJ_TRUE, opj_event_msg(&mgr, EVT_WARNING, "after null fmt"));
  EXPECT_THAT(capture.warnings, ElementsAre("after null fmt"));
}

TEST(EventTest, TruncatesFormattedMessagesExceedingOpjMsgSize) {
  for (const auto& level : kValidLevels) {
    SCOPED_TRACE(std::string(level.name));
    for (const auto& tc : kTruncationCases) {
      SCOPED_TRACE(tc.input_length);
      EventCapture capture;
      opj_event_mgr_t mgr = MakeSharedEventManager(&capture);
      const std::string payload = MakeDeterministicPattern(tc.input_length);

      ASSERT_EQ(OPJ_TRUE,
                opj_event_msg(&mgr, level.event_type, "%s", payload.c_str()));

      const auto& logged = MessagesForLevel(capture, level.event_type);
      ASSERT_EQ(1u, logged.size());
      EXPECT_EQ(tc.expected_length, logged[0].size());
      EXPECT_EQ(std::string_view(payload).substr(0, tc.expected_length),
                logged[0]);
    }
  }

  // Verify multi-argument formatted truncation also caps cleanly at 511 bytes.
  EventCapture capture;
  opj_event_mgr_t mgr = MakeSharedEventManager(&capture);
  const std::string payload = MakeDeterministicPattern(600);
  const std::string expected =
      ("prefix:" + payload).substr(0, kMaxFormattedMessageLength);
  ASSERT_EQ(OPJ_TRUE, opj_event_msg(&mgr, EVT_ERROR, "prefix:%s:tail:%d",
                                    payload.c_str(), 42));
  EXPECT_THAT(capture.errors, ElementsAre(expected));
}

}  // namespace
}  // namespace openjpeg2
