// Copyright 2014 Google Inc. All Rights Reserved.
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

#include "autotest2/cpp/util/error_handler.h"

#include "log_severity.h"
#include "logging.h"
#include "port/cpl_error.h"

void CPLGoogleLogErrorHandler(CPLErr error_class,
                              int error_num,
                              const char *error_msg) {
  int log_level = base_logging::FATAL;
  switch (error_class) {
    case CE_None:  // Fall through and treat as INFO.
    case CE_Debug:
      log_level = base_logging::INFO;
      break;
    case CE_Warning:
      log_level = base_logging::WARNING;
      break;
    case CE_Failure:
      log_level = base_logging::ERROR;
      break;
    case CE_Fatal:
      log_level = base_logging::FATAL;
      break;
    default:
      LOG(ERROR) << "Unknown error class of " << error_class;
      log_level = base_logging::FATAL;
  }

  LOG(LEVEL(log_level))
      << "GDAL Error class: " << error_class
      << " Error num: " << error_num
      << " Error msg: '" << error_msg << "'";
}
