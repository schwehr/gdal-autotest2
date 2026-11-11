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
// Forward declarations for C CLI entry points compiled with -Dmain=... in
// //third_party/netcdf/BUILD (*_cli_for_test targets).

#ifndef THIRD_PARTY_NETCDF_GOOGLE_UTIL_CLI_MAINS_H_
#define THIRD_PARTY_NETCDF_GOOGLE_UTIL_CLI_MAINS_H_

extern "C" {
int echon_cli_main(int argc, char** argv);
int nccopy_cli_main(int argc, char** argv);
int ncdump_cli_main(int argc, char** argv);
int ncfilteravail_cli_main(int argc, char** argv);
int ncgen_cli_main(int argc, char** argv);
int nchdf5version_cli_main(int argc, char** argv);
int ncpathcvt_cli_main(int argc, char** argv);
int ncvalidator_cli_main(int argc, char** argv);
int printfqn_cli_main(int argc, char** argv);
}  // extern "C"

#endif  // THIRD_PARTY_NETCDF_GOOGLE_UTIL_CLI_MAINS_H_
