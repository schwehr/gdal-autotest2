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

// Supplies global variables and helper functions normally defined in
// third_party/netcdf/ncgen/main.c so non-main ncgen/*.c unit tests link cleanly
// without defining a conflicting C `main()`.

#include "third_party/netcdf/google/util/ncgen_main_shim.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/netcdf/config.h"
#include "third_party/netcdf/ncgen/includes.h"

char* progname = "ncgen_test";
char* cdlname = NULL;

int nofill_flag = 0;
char* mainname = "main";
Language l_flag = L_UNDEFINED;
int syntax_only = 0;
int header_only = 0;

int k_flag = 0;
int format_attribute = 0;
int enhanced_flag = 0;
int cdf5_flag = 0;
int specials_flag = 0;
int usingclassic = 0;
int cmode_modifier = 0;
int diskless = 0;
int ncloglevel = -1;
int wholevarsize = 1024;

GlobalSpecialData globalspecials;
size_t zerosvector[NC_MAX_VAR_DIMS];
size_t onesvector[NC_MAX_VAR_DIMS];

char* binary_ext = ".nc";
struct Vlendata* vlendata = NULL;
char* netcdf_name = NULL;
char* datasetname = NULL;

struct Kvalues legalkinds[] = {
    {"classic", NC_FORMAT_CLASSIC, 0},
    {"nc3", NC_FORMAT_CLASSIC, 0},
    {"1", NC_FORMAT_CLASSIC, 0},
    {"64-bit offset", NC_FORMAT_64BIT_OFFSET, 0},
    {"nc6", NC_FORMAT_64BIT_OFFSET, 0},
    {"2", NC_FORMAT_64BIT_OFFSET, 0},
    {"64-bit-offset", NC_FORMAT_64BIT_OFFSET, 0},
    {"netCDF-4", NC_FORMAT_NETCDF4, 0},
    {"nc4", NC_FORMAT_NETCDF4, 0},
    {"3", NC_FORMAT_NETCDF4, 0},
    {"netCDF4", NC_FORMAT_NETCDF4, 0},
    {"hdf5", NC_FORMAT_NETCDF4, 0},
    {"enhanced", NC_FORMAT_NETCDF4, 0},
    {"netcdf-4", NC_FORMAT_NETCDF4, 0},
    {"netcdf4", NC_FORMAT_NETCDF4, 0},
    {"netCDF-4 classic model", NC_FORMAT_NETCDF4_CLASSIC, 0},
    {"nc7", NC_FORMAT_NETCDF4_CLASSIC, 0},
    {"4", NC_FORMAT_NETCDF4_CLASSIC, 0},
    {"netCDF-4-classic", NC_FORMAT_NETCDF4_CLASSIC, 0},
    {"netCDF-4_classic", NC_FORMAT_NETCDF4_CLASSIC, 0},
    {"netCDF4_classic", NC_FORMAT_NETCDF4_CLASSIC, 0},
    {"hdf5-nc3", NC_FORMAT_NETCDF4_CLASSIC, 0},
    {"enhanced-nc3", NC_FORMAT_NETCDF4_CLASSIC, 0},
    {"5", NC_FORMAT_64BIT_DATA, 0},
    {"64-bit-data", NC_FORMAT_64BIT_DATA, 0},
    {"64-bit data", NC_FORMAT_64BIT_DATA, 0},
    {"nc5", NC_FORMAT_64BIT_DATA, 0},
    {"cdf5", NC_FORMAT_64BIT_DATA, 0},
    {"cdf-5", NC_FORMAT_64BIT_DATA, 0},
    {NULL, 0, 0},
};

struct Languages {
  char* name;
  Language flag;
} legallanguages[] = {
    {"b", L_BINARY},         {"c", L_C},
    {"C", L_C},              {"f77", L_F77},
    {"fortran77", L_F77},    {"Fortran77", L_F77},
    {"j", L_JAVA},           {"java", L_JAVA},
    {NULL, L_UNDEFINED},
};

static sigjmp_buf* g_finalize_jmp = NULL;
static int* g_finalize_retcode = NULL;

void ncgen_shim_set_finalize_jmp(sigjmp_buf* env, int* out_retcode) {
  g_finalize_jmp = env;
  g_finalize_retcode = out_retcode;
}

void ncgen_shim_reset_globals(void) {
  progname = "ncgen_test";
  cdlname = NULL;
  nofill_flag = 0;
  mainname = "main";
  l_flag = L_UNDEFINED;
  syntax_only = 0;
  header_only = 0;
  k_flag = 0;
  format_attribute = 0;
  enhanced_flag = 0;
  cdf5_flag = 0;
  specials_flag = 0;
  usingclassic = 0;
  cmode_modifier = 0;
  diskless = 0;
  ncloglevel = -1;
  wholevarsize = 1024;
  memset(&globalspecials, 0, sizeof(globalspecials));
  binary_ext = ".nc";
  vlendata = NULL;
  netcdf_name = NULL;
  datasetname = NULL;
  init_netcdf();
}

void usage(void) { derror("Usage: %s", progname ? progname : "ncgen_test"); }

void init_netcdf(void) {
  int i;
  memset((void*)&nullconstant, 0, sizeof(NCConstant));
  fillconstant = nullconstant;
  fillconstant.nctype = NC_FILLVALUE;

  filldatalist = builddatalist(1);
  dlappend(filldatalist, &fillconstant);
  filldatalist->readonly = 1;

  codebuffer = bbNew();
  stmt = bbNew();
  error_count = 0;

  for (i = 0; i < NC_MAX_VAR_DIMS; i++) onesvector[i] = 1;
  memset(zerosvector, 0, sizeof(zerosvector));
}

void finalize_netcdf(int retcode) {
  if (g_finalize_jmp != NULL) {
    if (g_finalize_retcode != NULL) {
      *g_finalize_retcode = retcode;
    }
    siglongjmp(*g_finalize_jmp, 1);
  }
  nc_finalize();
  exit(retcode);
}
