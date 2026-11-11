// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for third_party/geos/src/util/Interrupt.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
// - geos::util::Interrupt uses process-global non-atomic state and is tested
//   single-threaded with RAII restoration; thread-local isolation is tested
//   separately via geos::util::CurrentThreadInterrupt.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/util/Interrupt.cpp

#include "third_party/geos/include/geos/util/Interrupt.h"

#include <string_view>
#include <thread>  // NOLINT

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::util::CurrentThreadInterrupt;
using geos::util::GEOSException;
using geos::util::Interrupt;

constexpr std::string_view kInterruptedWhat =
    "InterruptedException: Interrupted!";

int g_plain_callback_calls = 0;
int g_requesting_callback_calls = 0;

void PlainInterruptCallback() { ++g_plain_callback_calls; }

void RequestingInterruptCallback() {
  ++g_requesting_callback_calls;
  Interrupt::request();
}

struct ThreadCallbackState {
  int calls = 0;
  int return_value = 0;
};

int CheckThreadCallback(void* data) {
  auto* state = static_cast<ThreadCallbackState*>(data);
  ++state->calls;
  return state->return_value;
}

int SecondaryThreadCallback(void* data) {
  auto* state = static_cast<ThreadCallbackState*>(data);
  ++state->calls;
  return state->return_value;
}

template <typename Fn>
void ExpectInterruptedException(Fn&& fn) {
  try {
    fn();
    FAIL() << "Expected GEOSException(InterruptedException)";
  } catch (const GEOSException& e) {
    EXPECT_EQ(std::string_view(e.what()), kInterruptedWhat);
  }
}

TEST(InterruptTest, RequestCheckAndCancelTogglePendingState) {
  absl::Cleanup cleanup([] { Interrupt::cancel(); });
  Interrupt::cancel();
  EXPECT_FALSE(Interrupt::check());

  Interrupt::request();
  EXPECT_TRUE(Interrupt::check());

  Interrupt::request();
  EXPECT_TRUE(Interrupt::check());

  Interrupt::cancel();
  EXPECT_FALSE(Interrupt::check());

  Interrupt::cancel();
  EXPECT_FALSE(Interrupt::check());
}

TEST(InterruptTest, ProcessNoOpWithoutCallbackOrRequest) {
  Interrupt::Callback* prev_cb = Interrupt::registerCallback(nullptr);
  absl::Cleanup cleanup([prev_cb] {
    Interrupt::registerCallback(prev_cb);
    Interrupt::cancel();
  });
  Interrupt::cancel();

  EXPECT_NO_THROW(Interrupt::process());
  EXPECT_FALSE(Interrupt::check());
}

TEST(InterruptTest, ProcessThrowsAndClearsPendingRequest) {
  Interrupt::Callback* prev_cb = Interrupt::registerCallback(nullptr);
  absl::Cleanup cleanup([prev_cb] {
    Interrupt::registerCallback(prev_cb);
    Interrupt::cancel();
  });
  Interrupt::request();
  ASSERT_TRUE(Interrupt::check());

  ExpectInterruptedException([] { Interrupt::process(); });
  EXPECT_FALSE(Interrupt::check());
  EXPECT_NO_THROW(Interrupt::process());
}

TEST(InterruptTest, InterruptThrowsDirectlyAndResetsRequest) {
  absl::Cleanup cleanup([] { Interrupt::cancel(); });

  Interrupt::cancel();
  ExpectInterruptedException([] { Interrupt::interrupt(); });
  EXPECT_FALSE(Interrupt::check());

  Interrupt::request();
  ASSERT_TRUE(Interrupt::check());
  ExpectInterruptedException([] { Interrupt::interrupt(); });
  EXPECT_FALSE(Interrupt::check());
}

TEST(InterruptTest, RegisterCallbackReturnsPreviousAndInvokesOnProcess) {
  g_plain_callback_calls = 0;
  g_requesting_callback_calls = 0;

  Interrupt::Callback* prev_cb =
      Interrupt::registerCallback(&PlainInterruptCallback);
  absl::Cleanup cleanup([prev_cb] {
    Interrupt::registerCallback(prev_cb);
    Interrupt::cancel();
  });
  EXPECT_EQ(prev_cb, nullptr);
  Interrupt::cancel();

  EXPECT_NO_THROW(Interrupt::process());
  EXPECT_EQ(g_plain_callback_calls, 1);
  EXPECT_FALSE(Interrupt::check());

  EXPECT_EQ(Interrupt::registerCallback(&RequestingInterruptCallback),
            &PlainInterruptCallback);
  ExpectInterruptedException([] { Interrupt::process(); });
  EXPECT_EQ(g_requesting_callback_calls, 1);
  EXPECT_EQ(g_plain_callback_calls, 1);
  EXPECT_FALSE(Interrupt::check());

  EXPECT_EQ(Interrupt::registerCallback(nullptr), &RequestingInterruptCallback);
  EXPECT_NO_THROW(Interrupt::process());
  EXPECT_EQ(g_requesting_callback_calls, 1);
}

TEST(InterruptTest, CurrentThreadInterruptRegisterAndProcess) {
  ThreadCallbackState state1{.calls = 0, .return_value = 0};
  ThreadCallbackState state2{.calls = 0, .return_value = 0};

  CurrentThreadInterrupt::ThreadCallback* prev_cb =
      CurrentThreadInterrupt::registerCallback(&CheckThreadCallback, &state1);
  absl::Cleanup cleanup([prev_cb] {
    CurrentThreadInterrupt::registerCallback(prev_cb, nullptr);
  });
  EXPECT_EQ(prev_cb, nullptr);

  EXPECT_NO_THROW(CurrentThreadInterrupt::process());
  EXPECT_NO_THROW(CurrentThreadInterrupt::process());
  EXPECT_EQ(state1.calls, 2);

  EXPECT_EQ(CurrentThreadInterrupt::registerCallback(&SecondaryThreadCallback,
                                                     &state2),
            &CheckThreadCallback);
  EXPECT_NO_THROW(CurrentThreadInterrupt::process());
  EXPECT_EQ(state1.calls, 2);
  EXPECT_EQ(state2.calls, 1);

  EXPECT_EQ(CurrentThreadInterrupt::registerCallback(nullptr, nullptr),
            &SecondaryThreadCallback);
  EXPECT_NO_THROW(CurrentThreadInterrupt::process());
  EXPECT_EQ(state2.calls, 1);
}

TEST(InterruptTest, CurrentThreadInterruptThrowsWhenCallbackReturnsNonZero) {
  ThreadCallbackState state{.calls = 0, .return_value = 1};
  CurrentThreadInterrupt::ThreadCallback* prev_cb =
      CurrentThreadInterrupt::registerCallback(&CheckThreadCallback, &state);
  absl::Cleanup cleanup([prev_cb] {
    CurrentThreadInterrupt::registerCallback(prev_cb, nullptr);
  });

  ExpectInterruptedException([] { CurrentThreadInterrupt::process(); });
  EXPECT_EQ(state.calls, 1);

  state.return_value = -1;
  ExpectInterruptedException([] { CurrentThreadInterrupt::process(); });
  EXPECT_EQ(state.calls, 2);
}

TEST(InterruptTest, CurrentThreadInterruptDirectInterruptThrows) {
  ExpectInterruptedException([] { CurrentThreadInterrupt::interrupt(); });
}

TEST(InterruptTest, GeosCheckForInterruptsChecksGlobalBeforeCurrentThread) {
  ThreadCallbackState state{.calls = 0, .return_value = 0};
  Interrupt::Callback* prev_global = Interrupt::registerCallback(nullptr);
  CurrentThreadInterrupt::ThreadCallback* prev_thread =
      CurrentThreadInterrupt::registerCallback(&CheckThreadCallback, &state);
  absl::Cleanup cleanup([prev_global, prev_thread] {
    Interrupt::registerCallback(prev_global);
    Interrupt::cancel();
    CurrentThreadInterrupt::registerCallback(prev_thread, nullptr);
  });

  Interrupt::cancel();
  EXPECT_NO_THROW(GEOS_CHECK_FOR_INTERRUPTS());
  EXPECT_EQ(state.calls, 1);

  // Pending global interrupt short-circuits before CurrentThreadInterrupt.
  Interrupt::request();
  ExpectInterruptedException([] { GEOS_CHECK_FOR_INTERRUPTS(); });
  EXPECT_EQ(state.calls, 1);
  EXPECT_FALSE(Interrupt::check());

  // With global interrupt cleared, CurrentThreadInterrupt handles the check.
  state.return_value = 1;
  ExpectInterruptedException([] { GEOS_CHECK_FOR_INTERRUPTS(); });
  EXPECT_EQ(state.calls, 2);
}

TEST(InterruptTest, CurrentThreadInterruptCallbackIsThreadLocal) {
  ThreadCallbackState main_state{.calls = 0, .return_value = 1};
  CurrentThreadInterrupt::ThreadCallback* prev_main =
      CurrentThreadInterrupt::registerCallback(&CheckThreadCallback,
                                               &main_state);
  absl::Cleanup cleanup([prev_main] {
    CurrentThreadInterrupt::registerCallback(prev_main, nullptr);
  });

  CurrentThreadInterrupt::ThreadCallback* worker_initial_prev = nullptr;
  ThreadCallbackState worker_state{.calls = 0, .return_value = 0};
  bool worker_threw_when_requested = false;

  std::thread worker([&] {
    worker_initial_prev = CurrentThreadInterrupt::registerCallback(
        &SecondaryThreadCallback, &worker_state);
    absl::Cleanup worker_cleanup([worker_initial_prev] {
      CurrentThreadInterrupt::registerCallback(worker_initial_prev, nullptr);
    });

    CurrentThreadInterrupt::process();
    worker_state.return_value = 1;
    try {
      CurrentThreadInterrupt::process();
    } catch (const GEOSException& e) {
      worker_threw_when_requested = (e.what() == kInterruptedWhat);
    }
  });
  worker.join();

  EXPECT_EQ(worker_initial_prev, nullptr);
  EXPECT_EQ(worker_state.calls, 2);
  EXPECT_TRUE(worker_threw_when_requested);

  // Worker's thread-local callback and cleanup did not alter main thread state.
  EXPECT_EQ(main_state.calls, 0);
  ExpectInterruptedException([] { CurrentThreadInterrupt::process(); });
  EXPECT_EQ(main_state.calls, 1);
}

}  // namespace
}  // namespace geos_google
