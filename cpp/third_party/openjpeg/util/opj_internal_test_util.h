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
// Shared C++20 RAII wrappers and event-manager helpers for internal OpenJPEG
// test targets. Include after test_util.h and opj_internal.h.

#ifndef THIRD_PARTY_OPENJPEG2_GOOGLE_UTIL_OPJ_INTERNAL_TEST_UTIL_H_
#define THIRD_PARTY_OPENJPEG2_GOOGLE_UTIL_OPJ_INTERNAL_TEST_UTIL_H_

#include <memory>

#include "third_party/openjpeg2/google/util/opj_internal.h"
#include "third_party/openjpeg2/google/util/test_util.h"

namespace openjpeg2 {

// Initializes an opj_event_mgr_t wired to `capture` (or silent defaults if
// nullptr).
inline opj_event_mgr_t MakeEventManager(EventCapture* capture = nullptr) {
  opj_event_mgr_t mgr{};
  if (capture != nullptr) {
    mgr.error_handler = &EventCapture::OnError;
    mgr.m_error_data = capture;
    mgr.warning_handler = &EventCapture::OnWarning;
    mgr.m_warning_data = capture;
    mgr.info_handler = &EventCapture::OnInfo;
    mgr.m_info_data = capture;
  } else {
    opj_set_default_event_handler(&mgr);
  }
  return mgr;
}

struct OpjThreadPoolDeleter {
  void operator()(opj_thread_pool_t* tp) const { opj_thread_pool_destroy(tp); }
};
using OpjThreadPoolPtr =
    std::unique_ptr<opj_thread_pool_t, OpjThreadPoolDeleter>;

struct OpjMutexDeleter {
  void operator()(opj_mutex_t* mutex) const { opj_mutex_destroy(mutex); }
};
using OpjMutexPtr = std::unique_ptr<opj_mutex_t, OpjMutexDeleter>;

struct OpjCondDeleter {
  void operator()(opj_cond_t* cond) const { opj_cond_destroy(cond); }
};
using OpjCondPtr = std::unique_ptr<opj_cond_t, OpjCondDeleter>;

struct OpjTcdDeleter {
  void operator()(opj_tcd_t* tcd) const { opj_tcd_destroy(tcd); }
};
using OpjTcdPtr = std::unique_ptr<opj_tcd_t, OpjTcdDeleter>;

struct OpjT1Deleter {
  void operator()(opj_t1_t* t1) const { opj_t1_destroy(t1); }
};
using OpjT1Ptr = std::unique_ptr<opj_t1_t, OpjT1Deleter>;

struct OpjBioDeleter {
  void operator()(opj_bio_t* bio) const { opj_bio_destroy(bio); }
};
using OpjBioPtr = std::unique_ptr<opj_bio_t, OpjBioDeleter>;

struct OpjTgtTreeDeleter {
  void operator()(opj_tgt_tree_t* tree) const { opj_tgt_destroy(tree); }
};
using OpjTgtTreePtr = std::unique_ptr<opj_tgt_tree_t, OpjTgtTreeDeleter>;

struct OpjMarkerInfoDeleter {
  void operator()(opj_tcd_marker_info_t* info) const {
    opj_tcd_marker_info_destroy(info);
  }
};
using OpjMarkerInfoPtr =
    std::unique_ptr<opj_tcd_marker_info_t, OpjMarkerInfoDeleter>;

struct OpjAlignedFreeDeleter {
  void operator()(void* ptr) const { opj_aligned_free(ptr); }
};
using OpjAlignedVoidPtr = std::unique_ptr<void, OpjAlignedFreeDeleter>;
template <typename T>
using OpjAlignedPtr = std::unique_ptr<T[], OpjAlignedFreeDeleter>;

struct OpjImageDataDeleter {
  void operator()(OPJ_INT32* ptr) const { opj_image_data_free(ptr); }
};
using OpjImageDataPtr = std::unique_ptr<OPJ_INT32[], OpjImageDataDeleter>;

}  // namespace openjpeg2

#endif  // THIRD_PARTY_OPENJPEG2_GOOGLE_UTIL_OPJ_INTERNAL_TEST_UTIL_H_
