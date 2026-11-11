// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for libtiff error-handling dispatch (tif_error.c).
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_error.c

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/temp_file.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::SizeIs;

enum class HandlerKind {
  kBasic,
  kExt,
  kExtR,
};

struct CapturedError {
  HandlerKind kind;
  const void* tif_ptr = nullptr;
  thandle_t handle = nullptr;
  const void* user_data = nullptr;
  bool module_is_null = false;
  std::string module;
  std::string message;
};

struct GlobalCaptureState {
  std::vector<CapturedError> errors;
};

// Points to a stack-owned GlobalCaptureState while ScopedCaptureBinding is
// alive; trivial pointer avoids global non-trivial destructors.
GlobalCaptureState* g_capture_state = nullptr;

class ScopedCaptureBinding {
 public:
  explicit ScopedCaptureBinding(GlobalCaptureState* state)
      : prev_(g_capture_state) {
    g_capture_state = state;
  }

  ~ScopedCaptureBinding() { g_capture_state = prev_; }

  ScopedCaptureBinding(const ScopedCaptureBinding&) = delete;
  ScopedCaptureBinding& operator=(const ScopedCaptureBinding&) = delete;

 private:
  GlobalCaptureState* prev_;
};

std::string FormatVa(const char* fmt, va_list ap) {
  if (fmt == nullptr) return {};
  va_list copy;
  va_copy(copy, ap);
  const int len = std::vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  if (len < 0) return {};
  std::string out(static_cast<size_t>(len) + 1, '\0');
  std::vsnprintf(out.data(), out.size(), fmt, ap);
  out.resize(static_cast<size_t>(len));
  return out;
}

void RecordBasicError(const char* module, const char* fmt, va_list ap) {
  if (g_capture_state == nullptr) return;
  g_capture_state->errors.push_back(CapturedError{
      .kind = HandlerKind::kBasic,
      .module_is_null = (module == nullptr),
      .module = module != nullptr ? module : "",
      .message = FormatVa(fmt, ap),
  });
}

void SecondaryBasicError(const char* /*module*/, const char* /*fmt*/,
                         va_list /*ap*/) {}

void RecordExtError(thandle_t fd, const char* module, const char* fmt,
                    va_list ap) {
  if (g_capture_state == nullptr) return;
  g_capture_state->errors.push_back(CapturedError{
      .kind = HandlerKind::kExt,
      .handle = fd,
      .module_is_null = (module == nullptr),
      .module = module != nullptr ? module : "",
      .message = FormatVa(fmt, ap),
  });
}

void SecondaryExtError(thandle_t /*fd*/, const char* /*module*/,
                       const char* /*fmt*/, va_list /*ap*/) {}

struct ExtRContext {
  GlobalCaptureState* state = nullptr;
  int return_value = 1;
};

int RecordExtRError(TIFF* tif, void* user_data, const char* module,
                    const char* fmt, va_list ap) {
  auto* ctx = static_cast<ExtRContext*>(user_data);
  if (ctx == nullptr) return 1;
  if (ctx->state != nullptr) {
    ctx->state->errors.push_back(CapturedError{
        .kind = HandlerKind::kExtR,
        .tif_ptr = tif,
        .user_data = user_data,
        .module_is_null = (module == nullptr),
        .module = module != nullptr ? module : "",
        .message = FormatVa(fmt, ap),
    });
  }
  return ctx->return_value;
}

int RecordExtRNullUserDataStop(TIFF* tif, void* user_data, const char* module,
                               const char* fmt, va_list ap) {
  if (g_capture_state != nullptr) {
    g_capture_state->errors.push_back(CapturedError{
        .kind = HandlerKind::kExtR,
        .tif_ptr = tif,
        .user_data = user_data,
        .module_is_null = (module == nullptr),
        .module = module != nullptr ? module : "",
        .message = FormatVa(fmt, ap),
    });
  }
  return 1;
}

TEST(TifErrorTest, DefaultGlobalHandlersInitialStateAndRestore) {
  TIFFErrorHandler initial_basic = TIFFSetErrorHandler(nullptr);
  absl::Cleanup restore_basic = [initial_basic] {
    TIFFSetErrorHandler(initial_basic);
  };
  TIFFErrorHandlerExt initial_ext = TIFFSetErrorHandlerExt(nullptr);
  absl::Cleanup restore_ext = [initial_ext] {
    TIFFSetErrorHandlerExt(initial_ext);
  };

  EXPECT_NE(initial_basic, nullptr);
  EXPECT_EQ(initial_ext, nullptr);
  EXPECT_EQ(_TIFFerrorHandler, nullptr);
  EXPECT_EQ(_TIFFerrorHandlerExt, nullptr);
}

TEST(TifErrorTest, SetErrorHandlerReturnsPreviousAndUpdatesGlobal) {
  ScopedTiffErrorHandlers global_guard(nullptr, nullptr);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);

  EXPECT_EQ(TIFFSetErrorHandler(&RecordBasicError), nullptr);
  EXPECT_EQ(_TIFFerrorHandler, &RecordBasicError);

  EXPECT_EQ(TIFFSetErrorHandler(&SecondaryBasicError), &RecordBasicError);
  EXPECT_EQ(_TIFFerrorHandler, &SecondaryBasicError);

  EXPECT_EQ(TIFFSetErrorHandler(nullptr), &SecondaryBasicError);
  EXPECT_EQ(_TIFFerrorHandler, nullptr);
}

TEST(TifErrorTest, SetErrorHandlerExtReturnsPreviousAndUpdatesGlobal) {
  ScopedTiffErrorHandlers global_guard(nullptr, nullptr);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);

  EXPECT_EQ(TIFFSetErrorHandlerExt(&RecordExtError), nullptr);
  EXPECT_EQ(_TIFFerrorHandlerExt, &RecordExtError);

  EXPECT_EQ(TIFFSetErrorHandlerExt(&SecondaryExtError), &RecordExtError);
  EXPECT_EQ(_TIFFerrorHandlerExt, &SecondaryExtError);

  EXPECT_EQ(TIFFSetErrorHandlerExt(nullptr), &SecondaryExtError);
  EXPECT_EQ(_TIFFerrorHandlerExt, nullptr);
}

TEST(TifErrorTest, TiffErrorInvokesBasicHandlerOnlyWhenExtIsNull) {
  GlobalCaptureState state;
  ScopedCaptureBinding bind(&state);
  ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);

  TIFFError("BasicMod", "code=%d tag=%s", 42, "ImageWidth");

  ASSERT_THAT(state.errors, SizeIs(1));
  EXPECT_EQ(state.errors[0].kind, HandlerKind::kBasic);
  EXPECT_FALSE(state.errors[0].module_is_null);
  EXPECT_EQ(state.errors[0].module, "BasicMod");
  EXPECT_EQ(state.errors[0].message, "code=42 tag=ImageWidth");
}

TEST(TifErrorTest, TiffErrorInvokesExtHandlerWithNullHandleWhenBasicIsNull) {
  GlobalCaptureState state;
  ScopedCaptureBinding bind(&state);
  ScopedTiffErrorHandlers global_guard(nullptr, nullptr);
  ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);

  TIFFError("ExtOnlyMod", "val=%u", 77U);

  ASSERT_THAT(state.errors, SizeIs(1));
  EXPECT_EQ(state.errors[0].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[0].handle, nullptr);
  EXPECT_FALSE(state.errors[0].module_is_null);
  EXPECT_EQ(state.errors[0].module, "ExtOnlyMod");
  EXPECT_EQ(state.errors[0].message, "val=77");
}

TEST(TifErrorTest, TiffErrorInvokesBothHandlersWithFreshVaListsAndNullModule) {
  GlobalCaptureState state;
  ScopedCaptureBinding bind(&state);
  ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
  ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);

  TIFFError("DualMod", "a=%s b=%d c=%u d=%llu", "alpha", -5, 12U,
            9876543210ULL);
  TIFFError(nullptr, "null_mod=%d", 9);

  ASSERT_THAT(state.errors, SizeIs(4));
  EXPECT_EQ(state.errors[0].kind, HandlerKind::kBasic);
  EXPECT_EQ(state.errors[0].module, "DualMod");
  EXPECT_EQ(state.errors[0].message, "a=alpha b=-5 c=12 d=9876543210");

  EXPECT_EQ(state.errors[1].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[1].handle, nullptr);
  EXPECT_EQ(state.errors[1].module, "DualMod");
  EXPECT_EQ(state.errors[1].message, "a=alpha b=-5 c=12 d=9876543210");

  EXPECT_EQ(state.errors[2].kind, HandlerKind::kBasic);
  EXPECT_TRUE(state.errors[2].module_is_null);
  EXPECT_EQ(state.errors[2].message, "null_mod=9");

  EXPECT_EQ(state.errors[3].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[3].handle, nullptr);
  EXPECT_TRUE(state.errors[3].module_is_null);
  EXPECT_EQ(state.errors[3].message, "null_mod=9");
}

TEST(TifErrorTest, TiffErrorIsNoOpWhenBothHandlersAreNull) {
  GlobalCaptureState state;
  ScopedCaptureBinding bind(&state);
  ScopedTiffErrorHandlers global_guard(nullptr, nullptr);
  ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);

  TIFFError("Silent", "unused %d", 1);

  EXPECT_THAT(state.errors, IsEmpty());
}

TEST(TifErrorTest, TiffErrorExtDispatchesHandleAcrossHandlerCombinations) {
  GlobalCaptureState state;
  ScopedCaptureBinding bind(&state);
  int sentinel_target = 123;
  const thandle_t sentinel = &sentinel_target;

  {
    ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);
    TIFFErrorExt(sentinel, "ExtBasicOnly", "x=%d", 1);
  }
  {
    ScopedTiffErrorHandlers global_guard(nullptr, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);
    TIFFErrorExt(sentinel, "ExtHandlerOnly", "x=%d", 2);
  }
  {
    ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);
    TIFFErrorExt(sentinel, "ExtBoth", "%s:%d", "ok", 3);
    TIFFErrorExt(nullptr, nullptr, "nulls");
  }
  {
    ScopedTiffErrorHandlers global_guard(nullptr, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);
    TIFFErrorExt(sentinel, "ExtNeither", "quiet");
  }

  ASSERT_THAT(state.errors, SizeIs(6));
  EXPECT_EQ(state.errors[0].kind, HandlerKind::kBasic);
  EXPECT_EQ(state.errors[0].module, "ExtBasicOnly");
  EXPECT_EQ(state.errors[0].message, "x=1");

  EXPECT_EQ(state.errors[1].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[1].handle, sentinel);
  EXPECT_EQ(state.errors[1].module, "ExtHandlerOnly");
  EXPECT_EQ(state.errors[1].message, "x=2");

  EXPECT_EQ(state.errors[2].kind, HandlerKind::kBasic);
  EXPECT_EQ(state.errors[2].module, "ExtBoth");
  EXPECT_EQ(state.errors[2].message, "ok:3");

  EXPECT_EQ(state.errors[3].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[3].handle, sentinel);
  EXPECT_EQ(state.errors[3].module, "ExtBoth");
  EXPECT_EQ(state.errors[3].message, "ok:3");

  EXPECT_EQ(state.errors[4].kind, HandlerKind::kBasic);
  EXPECT_TRUE(state.errors[4].module_is_null);
  EXPECT_EQ(state.errors[4].message, "nulls");

  EXPECT_EQ(state.errors[5].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[5].handle, nullptr);
  EXPECT_TRUE(state.errors[5].module_is_null);
  EXPECT_EQ(state.errors[5].message, "nulls");
}

TEST(TifErrorTest, TiffErrorEarlyNullOptsOrNullCallbackFallsThroughToGlobals) {
  GlobalCaptureState state;
  ScopedCaptureBinding bind(&state);
  int client_target = 456;
  const thandle_t clientdata = &client_target;

  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts.get(), nullptr);

  {
    ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);

    _TIFFErrorEarly(nullptr, clientdata, "EarlyNullOpts", "n=%d", 10);
    _TIFFErrorEarly(opts.get(), clientdata, "EarlyNoHandler", "s=%s", "hi");
  }
  {
    ScopedTiffErrorHandlers global_guard(nullptr, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);

    _TIFFErrorEarly(nullptr, clientdata, "EarlySilent", "ignored");
  }

  ASSERT_THAT(state.errors, SizeIs(4));
  EXPECT_EQ(state.errors[0].kind, HandlerKind::kBasic);
  EXPECT_EQ(state.errors[0].module, "EarlyNullOpts");
  EXPECT_EQ(state.errors[0].message, "n=10");

  EXPECT_EQ(state.errors[1].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[1].handle, clientdata);
  EXPECT_EQ(state.errors[1].module, "EarlyNullOpts");
  EXPECT_EQ(state.errors[1].message, "n=10");

  EXPECT_EQ(state.errors[2].kind, HandlerKind::kBasic);
  EXPECT_EQ(state.errors[2].module, "EarlyNoHandler");
  EXPECT_EQ(state.errors[2].message, "s=hi");

  EXPECT_EQ(state.errors[3].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[3].handle, clientdata);
  EXPECT_EQ(state.errors[3].module, "EarlyNoHandler");
  EXPECT_EQ(state.errors[3].message, "s=hi");
}

TEST(TifErrorTest, TiffErrorEarlyStopsGlobalChainWhenCallbackReturnsNonZero) {
  GlobalCaptureState state;
  ScopedCaptureBinding bind(&state);
  ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
  ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);

  int client_target = 789;
  const thandle_t clientdata = &client_target;

  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts.get(), nullptr);

  ExtRContext ctx{.state = &state, .return_value = 1};
  TIFFOpenOptionsSetErrorHandlerExtR(opts.get(), &RecordExtRError, &ctx);
  _TIFFErrorEarly(opts.get(), clientdata, "EarlyStop", "err=%d %s", 404, "bad");

  ctx.return_value = 42;
  _TIFFErrorEarly(opts.get(), clientdata, "EarlyStopNonOne", "k=%d", 2);

  TIFFOpenOptionsSetErrorHandlerExtR(opts.get(), &RecordExtRNullUserDataStop,
                                     nullptr);
  _TIFFErrorEarly(opts.get(), clientdata, "EarlyNullUserData", "ok");

  ASSERT_THAT(state.errors, SizeIs(3));
  EXPECT_EQ(state.errors[0].kind, HandlerKind::kExtR);
  EXPECT_EQ(state.errors[0].tif_ptr, nullptr);
  EXPECT_EQ(state.errors[0].user_data, &ctx);
  EXPECT_EQ(state.errors[0].module, "EarlyStop");
  EXPECT_EQ(state.errors[0].message, "err=404 bad");

  EXPECT_EQ(state.errors[1].kind, HandlerKind::kExtR);
  EXPECT_EQ(state.errors[1].tif_ptr, nullptr);
  EXPECT_EQ(state.errors[1].user_data, &ctx);
  EXPECT_EQ(state.errors[1].module, "EarlyStopNonOne");
  EXPECT_EQ(state.errors[1].message, "k=2");

  EXPECT_EQ(state.errors[2].kind, HandlerKind::kExtR);
  EXPECT_EQ(state.errors[2].tif_ptr, nullptr);
  EXPECT_EQ(state.errors[2].user_data, nullptr);
  EXPECT_EQ(state.errors[2].module, "EarlyNullUserData");
  EXPECT_EQ(state.errors[2].message, "ok");
}

TEST(TifErrorTest,
     TiffErrorEarlyChainsToGlobalHandlersWhenCallbackReturnsZero) {
  GlobalCaptureState state;
  ScopedCaptureBinding bind(&state);
  int client_target = 321;
  const thandle_t clientdata = &client_target;

  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts.get(), nullptr);
  ExtRContext ctx{.state = &state, .return_value = 0};
  TIFFOpenOptionsSetErrorHandlerExtR(opts.get(), &RecordExtRError, &ctx);

  {
    ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);
    _TIFFErrorEarly(opts.get(), clientdata, "EarlyChain", "p=%s q=%d", "x", 99);
    _TIFFErrorEarly(opts.get(), clientdata, nullptr, "early_null=%d", 4);
  }
  {
    ScopedTiffErrorHandlers global_guard(nullptr, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);
    _TIFFErrorEarly(opts.get(), clientdata, "EarlyChainExtOnly", "z=%d", 5);
  }
  {
    ScopedTiffErrorHandlers global_guard(nullptr, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);
    _TIFFErrorEarly(opts.get(), clientdata, "EarlyChainNone", "n=%d", 6);
  }

  ASSERT_THAT(state.errors, SizeIs(9));
  EXPECT_EQ(state.errors[0].kind, HandlerKind::kExtR);
  EXPECT_EQ(state.errors[0].tif_ptr, nullptr);
  EXPECT_EQ(state.errors[0].user_data, &ctx);
  EXPECT_EQ(state.errors[0].module, "EarlyChain");
  EXPECT_EQ(state.errors[0].message, "p=x q=99");

  EXPECT_EQ(state.errors[1].kind, HandlerKind::kBasic);
  EXPECT_EQ(state.errors[1].module, "EarlyChain");
  EXPECT_EQ(state.errors[1].message, "p=x q=99");

  EXPECT_EQ(state.errors[2].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[2].handle, clientdata);
  EXPECT_EQ(state.errors[2].module, "EarlyChain");
  EXPECT_EQ(state.errors[2].message, "p=x q=99");

  EXPECT_EQ(state.errors[3].kind, HandlerKind::kExtR);
  EXPECT_TRUE(state.errors[3].module_is_null);
  EXPECT_EQ(state.errors[3].message, "early_null=4");

  EXPECT_EQ(state.errors[4].kind, HandlerKind::kBasic);
  EXPECT_TRUE(state.errors[4].module_is_null);
  EXPECT_EQ(state.errors[4].message, "early_null=4");

  EXPECT_EQ(state.errors[5].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[5].handle, clientdata);
  EXPECT_TRUE(state.errors[5].module_is_null);
  EXPECT_EQ(state.errors[5].message, "early_null=4");

  EXPECT_EQ(state.errors[6].kind, HandlerKind::kExtR);
  EXPECT_EQ(state.errors[6].module, "EarlyChainExtOnly");
  EXPECT_EQ(state.errors[6].message, "z=5");

  EXPECT_EQ(state.errors[7].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[7].handle, clientdata);
  EXPECT_EQ(state.errors[7].module, "EarlyChainExtOnly");
  EXPECT_EQ(state.errors[7].message, "z=5");

  EXPECT_EQ(state.errors[8].kind, HandlerKind::kExtR);
  EXPECT_EQ(state.errors[8].module, "EarlyChainNone");
  EXPECT_EQ(state.errors[8].message, "n=6");
}

TEST(TifErrorTest, TiffErrorExtRWithNullTifDispatchesGlobalsWithNullHandle) {
  GlobalCaptureState state;
  ScopedCaptureBinding bind(&state);

  {
    ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);
    TIFFErrorExtR(nullptr, "NullTif", "v=%d %s", -1, "msg");
  }
  {
    ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);
    TIFFErrorExtR(nullptr, "NullTifBasic", "b=%d", 2);
  }
  {
    ScopedTiffErrorHandlers global_guard(nullptr, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);
    TIFFErrorExtR(nullptr, "NullTifExt", "e=%d", 3);
  }
  {
    ScopedTiffErrorHandlers global_guard(nullptr, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);
    TIFFErrorExtR(nullptr, "NullTifSilent", "none");
  }

  ASSERT_THAT(state.errors, SizeIs(4));
  EXPECT_EQ(state.errors[0].kind, HandlerKind::kBasic);
  EXPECT_EQ(state.errors[0].module, "NullTif");
  EXPECT_EQ(state.errors[0].message, "v=-1 msg");

  EXPECT_EQ(state.errors[1].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[1].handle, nullptr);
  EXPECT_EQ(state.errors[1].module, "NullTif");
  EXPECT_EQ(state.errors[1].message, "v=-1 msg");

  EXPECT_EQ(state.errors[2].kind, HandlerKind::kBasic);
  EXPECT_EQ(state.errors[2].module, "NullTifBasic");
  EXPECT_EQ(state.errors[2].message, "b=2");

  EXPECT_EQ(state.errors[3].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[3].handle, nullptr);
  EXPECT_EQ(state.errors[3].module, "NullTifExt");
  EXPECT_EQ(state.errors[3].message, "e=3");
}

TEST(TifErrorTest, TiffErrorExtRTifWithoutPerHandleCallbackUsesClientData) {
  GlobalCaptureState state;
  ScopedCaptureBinding bind(&state);
  MemoryTiffStream stream;
  UniqueTiffOptions empty_opts = MakeUniqueTiffOptions();
  ASSERT_NE(empty_opts.get(), nullptr);
  UniqueTiff tif = stream.Open("w", empty_opts.get(), "no_cb.tif");
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(tif->tif_errorhandler, nullptr);

  {
    ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);
    TIFFErrorExtR(tif.get(), "NoPerHandle", "n=%d", 7);
  }
  {
    ScopedTiffErrorHandlers global_guard(nullptr, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);
    TIFFErrorExtR(tif.get(), "NoPerHandleSilent", "quiet");
  }

  ASSERT_THAT(state.errors, SizeIs(2));
  EXPECT_EQ(state.errors[0].kind, HandlerKind::kBasic);
  EXPECT_EQ(state.errors[0].module, "NoPerHandle");
  EXPECT_EQ(state.errors[0].message, "n=7");

  EXPECT_EQ(state.errors[1].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[1].handle, static_cast<thandle_t>(&stream));
  EXPECT_EQ(state.errors[1].module, "NoPerHandle");
  EXPECT_EQ(state.errors[1].message, "n=7");
}

TEST(TifErrorTest, TiffErrorExtRStopAndChainBehaviorWithPerHandleCallback) {
  GlobalCaptureState state;
  ScopedCaptureBinding bind(&state);
  MemoryTiffStream stream;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts.get(), nullptr);

  ExtRContext ctx{.state = &state, .return_value = 1};
  TIFFOpenOptionsSetErrorHandlerExtR(opts.get(), &RecordExtRError, &ctx);
  UniqueTiff tif = stream.Open("w", opts.get(), "per_handle.tif");
  ASSERT_NE(tif, nullptr);

  {
    ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);

    TIFFErrorExtR(tif.get(), "StopMod", "k=%s v=%d", "one", 1);

    ctx.return_value = 0;
    TIFFErrorExtR(tif.get(), "ChainMod", "k=%s v=%d", "two", 2);
  }
  {
    ScopedTiffErrorHandlers global_guard(nullptr, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);

    ctx.return_value = 0;
    TIFFErrorExtR(tif.get(), nullptr, "ext_only=%d", 4);
  }
  {
    ScopedTiffErrorHandlers global_guard(nullptr, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(nullptr, nullptr);

    ctx.return_value = 0;
    TIFFErrorExtR(tif.get(), "ChainQuietGlobals", "v=%d", 3);
  }

  ASSERT_THAT(state.errors, SizeIs(7));
  EXPECT_EQ(state.errors[0].kind, HandlerKind::kExtR);
  EXPECT_EQ(state.errors[0].tif_ptr, tif.get());
  EXPECT_EQ(state.errors[0].user_data, &ctx);
  EXPECT_EQ(state.errors[0].module, "StopMod");
  EXPECT_EQ(state.errors[0].message, "k=one v=1");

  EXPECT_EQ(state.errors[1].kind, HandlerKind::kExtR);
  EXPECT_EQ(state.errors[1].tif_ptr, tif.get());
  EXPECT_EQ(state.errors[1].user_data, &ctx);
  EXPECT_EQ(state.errors[1].module, "ChainMod");
  EXPECT_EQ(state.errors[1].message, "k=two v=2");

  EXPECT_EQ(state.errors[2].kind, HandlerKind::kBasic);
  EXPECT_EQ(state.errors[2].module, "ChainMod");
  EXPECT_EQ(state.errors[2].message, "k=two v=2");

  EXPECT_EQ(state.errors[3].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[3].handle, static_cast<thandle_t>(&stream));
  EXPECT_EQ(state.errors[3].module, "ChainMod");
  EXPECT_EQ(state.errors[3].message, "k=two v=2");

  EXPECT_EQ(state.errors[4].kind, HandlerKind::kExtR);
  EXPECT_EQ(state.errors[4].tif_ptr, tif.get());
  EXPECT_TRUE(state.errors[4].module_is_null);
  EXPECT_EQ(state.errors[4].message, "ext_only=4");

  EXPECT_EQ(state.errors[5].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[5].handle, static_cast<thandle_t>(&stream));
  EXPECT_TRUE(state.errors[5].module_is_null);
  EXPECT_EQ(state.errors[5].message, "ext_only=4");

  EXPECT_EQ(state.errors[6].kind, HandlerKind::kExtR);
  EXPECT_EQ(state.errors[6].tif_ptr, tif.get());
  EXPECT_EQ(state.errors[6].module, "ChainQuietGlobals");
  EXPECT_EQ(state.errors[6].message, "v=3");

  // Verify null user_data with per-handle ExtR stops global propagation.
  state.errors.clear();
  MemoryTiffStream null_ud_stream;
  UniqueTiffOptions null_ud_opts = MakeUniqueTiffOptions();
  ASSERT_NE(null_ud_opts.get(), nullptr);
  TIFFOpenOptionsSetErrorHandlerExtR(null_ud_opts.get(),
                                     &RecordExtRNullUserDataStop, nullptr);
  UniqueTiff null_ud_tif =
      null_ud_stream.Open("w", null_ud_opts.get(), "null_ud.tif");
  ASSERT_NE(null_ud_tif, nullptr);
  {
    ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
    ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);
    TIFFErrorExtR(null_ud_tif.get(), "NullUdMod", "msg=%s", "stopped");
  }
  ASSERT_THAT(state.errors, SizeIs(1));
  EXPECT_EQ(state.errors[0].kind, HandlerKind::kExtR);
  EXPECT_EQ(state.errors[0].tif_ptr, null_ud_tif.get());
  EXPECT_EQ(state.errors[0].user_data, nullptr);
  EXPECT_EQ(state.errors[0].module, "NullUdMod");
  EXPECT_EQ(state.errors[0].message, "msg=stopped");
}

TEST(TifErrorTest, OpenFailuresRouteThroughEarlyAndExtRHandlers) {
  GlobalCaptureState state;
  ScopedCaptureBinding bind(&state);
  ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
  ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);

  TempFile missing("missing_tif_error_test.tif");

  // Unparameterized TIFFOpen routes open failure through _TIFFErrorEarly to
  // global basic and extended handlers with nullptr clientdata.
  {
    UniqueTiff tif(TIFFOpen(missing.c_str(), "r"));
    EXPECT_EQ(tif, nullptr);
  }
  ASSERT_THAT(state.errors, SizeIs(2));
  EXPECT_EQ(state.errors[0].kind, HandlerKind::kBasic);
  EXPECT_EQ(state.errors[0].module, "TIFFOpen");
  EXPECT_THAT(state.errors[0].message, HasSubstr("missing_tif_error_test.tif"));
  EXPECT_EQ(state.errors[1].kind, HandlerKind::kExt);
  EXPECT_EQ(state.errors[1].handle, nullptr);
  state.errors.clear();

  // Bad open mode + early allocation cap use DiagnosticRecorder (ExtR stop=1).
  {
    DiagnosticRecorder rec;
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts.get(), nullptr);
    rec.Install(opts.get());

    UniqueTiff bad_mode(TIFFOpenExt(missing.c_str(), "z", opts.get()));
    EXPECT_EQ(bad_mode, nullptr);
    const std::vector<DiagnosticRecord> mode_errs = rec.errors();
    ASSERT_THAT(mode_errs, SizeIs(1));
    EXPECT_EQ(mode_errs[0].tif, nullptr);
    EXPECT_EQ(mode_errs[0].module, "TIFFOpen");
    EXPECT_THAT(mode_errs[0].message, HasSubstr("Bad mode"));
    EXPECT_THAT(state.errors, IsEmpty());

    rec.Clear();
    TIFFOpenOptionsSetMaxSingleMemAlloc(opts.get(), 8);
    MemoryTiffStream stream;
    UniqueTiff oom_tif = stream.Open("w", opts.get(), "oom_open.tif");
    EXPECT_EQ(oom_tif, nullptr);
    const std::vector<DiagnosticRecord> oom_errs = rec.errors();
    ASSERT_THAT(oom_errs, SizeIs(1));
    EXPECT_EQ(oom_errs[0].tif, nullptr);
    EXPECT_EQ(oom_errs[0].module, "TIFFClientOpenExt");
    EXPECT_THAT(oom_errs[0].message, HasSubstr("beyond the 8 byte limit"));
    EXPECT_THAT(state.errors, IsEmpty());
  }

  // Invalid magic bytes emit TIFFErrorExtR during open after TIFF* exists,
  // chaining to global handlers when ExtR returns 0.
  {
    DiagnosticRecorder rec;
    rec.set_chain_to_global(true);
    UniqueTiffOptions opts = MakeUniqueTiffOptions();
    ASSERT_NE(opts.get(), nullptr);
    rec.Install(opts.get());

    MemoryTiffStream bad_stream(
        std::vector<uint8_t>{'B', 'A', 'D', 0, 0, 0, 0, 0});
    UniqueTiff bad_tif = bad_stream.Open("r", opts.get(), "corrupt.tif");
    EXPECT_EQ(bad_tif, nullptr);

    const std::vector<DiagnosticRecord> rec_errs = rec.errors();
    ASSERT_THAT(rec_errs, SizeIs(1));
    EXPECT_NE(rec_errs[0].tif, nullptr);
    EXPECT_EQ(rec_errs[0].module, "corrupt.tif");
    EXPECT_THAT(rec_errs[0].message, HasSubstr("bad magic number"));

    ASSERT_THAT(state.errors, SizeIs(2));
    EXPECT_EQ(state.errors[0].kind, HandlerKind::kBasic);
    EXPECT_EQ(state.errors[0].module, "corrupt.tif");
    EXPECT_THAT(state.errors[0].message, HasSubstr("bad magic number"));
    EXPECT_EQ(state.errors[1].kind, HandlerKind::kExt);
    EXPECT_EQ(state.errors[1].handle, static_cast<thandle_t>(&bad_stream));
    EXPECT_THAT(state.errors[1].message, HasSubstr("bad magic number"));
  }
}

TEST(TifErrorTest, BigTiffDirectoryAndAllocLimitEmitErrors) {
  GlobalCaptureState state;
  ScopedCaptureBinding bind(&state);
  ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
  ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);

  // Zero-entry BigTIFF IFD triggers Google's tif_dirread.c patch, which calls
  // TIFFErrorExt(tif->tif_clientdata, ...) directly (bypassing ExtR), and then
  // TIFFReadDirectory emits "Failed to read directory" via TIFFErrorExtR.
  {
    RawTiffBuilder zero_builder(/*big_endian=*/true, /*big_tiff=*/true);
    zero_builder.AddIfd({});
    MemoryTiffStream zero_ifd_stream(zero_builder.Take());
    UniqueTiff zero_tif = zero_ifd_stream.Open("r", nullptr, "empty_ifd.tif");
    EXPECT_EQ(zero_tif, nullptr);
    const std::vector<DiagnosticRecord> zero_rec_errs =
        zero_ifd_stream.diagnostics().errors();
    ASSERT_THAT(zero_rec_errs, SizeIs(1));
    EXPECT_EQ(zero_rec_errs[0].module, "TIFFReadDirectory");
    EXPECT_THAT(zero_rec_errs[0].message,
                HasSubstr("Failed to read directory"));
    ASSERT_THAT(state.errors, SizeIs(2));
    EXPECT_EQ(state.errors[0].kind, HandlerKind::kBasic);
    EXPECT_EQ(state.errors[0].module, "TIFFFetchDirectory");
    EXPECT_THAT(state.errors[0].message,
                HasSubstr("Invalid zero directory count"));
    EXPECT_EQ(state.errors[1].kind, HandlerKind::kExt);
    EXPECT_EQ(state.errors[1].handle, static_cast<thandle_t>(&zero_ifd_stream));
    EXPECT_THAT(state.errors[1].message,
                HasSubstr("Invalid zero directory count"));
    state.errors.clear();
  }

  // Truncated BigTIFF IFD (dircount=1, missing entry payload) triggers
  // TIFFErrorExtR twice (TIFFFetchDirectory + TIFFReadDirectory), suppressed
  // from global handlers by DiagnosticRecorder.
  {
    RawTiffBuilder trunc_builder(/*big_endian=*/true, /*big_tiff=*/true);
    trunc_builder.AddIfd({trunc_builder.MakeLong(TIFFTAG_IMAGEWIDTH, 4)});
    std::vector<uint8_t> trunc_bytes = trunc_builder.Take();
    // BigTIFF header (16B) + dircount64 (8B) + partial entry (only 4 of 20B).
    trunc_bytes.resize(16 + 8 + 4);
    MemoryTiffStream trunc_stream(std::move(trunc_bytes));
    UniqueTiff trunc_tif = trunc_stream.Open("r", nullptr, "trunc_ifd.tif");
    EXPECT_EQ(trunc_tif, nullptr);
    const std::vector<DiagnosticRecord> trunc_errs =
        trunc_stream.diagnostics().errors();
    ASSERT_THAT(trunc_errs, SizeIs(2));
    EXPECT_NE(trunc_errs[0].tif, nullptr);
    EXPECT_EQ(trunc_errs[0].module, "TIFFFetchDirectory");
    EXPECT_THAT(trunc_errs[0].message,
                HasSubstr("Can not read TIFF directory"));
    EXPECT_NE(trunc_errs[1].tif, nullptr);
    EXPECT_EQ(trunc_errs[1].module, "TIFFReadDirectory");
    EXPECT_THAT(trunc_errs[1].message, HasSubstr("Failed to read directory"));
    EXPECT_THAT(state.errors, IsEmpty());
  }

  // Open a BigTIFF handle with non-zero cumulated limit so
  // ScopedCumulatedMemAllocRemaining can legally tighten the budget and emit
  // _TIFFEmitErrorAboveMaxCumulatedMemAlloc -> TIFFErrorExtR.
  DiagnosticRecorder rec;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts.get(), nullptr);
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), 1 << 24);
  rec.Install(opts.get());

  MemoryTiffStream write_stream;
  UniqueTiff tif = write_stream.Open("w8", opts.get(), "alloc_limit.tif");
  ASSERT_NE(tif, nullptr);

  {
    ScopedCumulatedMemAllocRemaining budget(tif.get(), 1);
    void* ptr = _TIFFmallocExt(tif.get(), 1024);
    absl::Cleanup free_if_unexpected = [ptr, raw = tif.get()] {
      if (ptr != nullptr) _TIFFfreeExt(raw, ptr);
    };
    EXPECT_EQ(ptr, nullptr);
  }

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, 4U), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, 2U), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE), 1);
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
            1);

  std::vector<uint8_t> scanline(4, 0);
  EXPECT_EQ(TIFFReadScanline(tif.get(), scanline.data(), 0, 0), -1);

  const std::vector<DiagnosticRecord> errs = rec.errors();
  ASSERT_THAT(errs, SizeIs(2));
  EXPECT_EQ(errs[0].tif, tif.get());
  EXPECT_THAT(errs[0].message, HasSubstr("cumulated byte limit"));
  EXPECT_EQ(errs[1].tif, tif.get());
  EXPECT_THAT(errs[1].message, HasSubstr("Invalid strip byte count"));
  EXPECT_THAT(state.errors, IsEmpty());
}

TEST(TifErrorTest, ValidRoundTripEmitsZeroErrors) {
  GlobalCaptureState state;
  ScopedCaptureBinding bind(&state);
  ScopedTiffErrorHandlers global_guard(&RecordBasicError, nullptr);
  ScopedTiffErrorHandlersExt ext_guard(&RecordExtError, nullptr);

  DiagnosticRecorder rec;
  rec.set_chain_to_global(true);
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  ASSERT_NE(opts.get(), nullptr);
  rec.Install(opts.get());

  MemoryTiffStream stream;
  {
    UniqueTiff writer = stream.Open("wl", opts.get(), "valid.tif");
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, 4U), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, 2U), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, uint16_t{8}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, uint16_t{1}),
              1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);

    std::vector<uint8_t> row0 = {10, 20, 30, 40};
    std::vector<uint8_t> row1 = {50, 60, 70, 80};
    EXPECT_EQ(TIFFWriteScanline(writer.get(), row0.data(), 0, 0), 1);
    EXPECT_EQ(TIFFWriteScanline(writer.get(), row1.data(), 1, 0), 1);
    EXPECT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  UniqueTiff reader = stream.Open("r", opts.get(), "valid.tif");
  ASSERT_NE(reader, nullptr);
  uint32_t width = 0;
  uint16_t bps = 0;
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_IMAGEWIDTH, &width), 1);
  EXPECT_EQ(TIFFGetField(reader.get(), TIFFTAG_BITSPERSAMPLE, &bps), 1);
  EXPECT_EQ(width, 4U);
  EXPECT_EQ(bps, 8);

  std::vector<uint8_t> read_row(4, 0);
  EXPECT_EQ(TIFFReadScanline(reader.get(), read_row.data(), 1, 0), 1);
  EXPECT_EQ(read_row, (std::vector<uint8_t>{50, 60, 70, 80}));

  EXPECT_EQ(rec.error_count(), 0U);
  EXPECT_THAT(state.errors, IsEmpty());
}

}  // namespace
}  // namespace libtiff
