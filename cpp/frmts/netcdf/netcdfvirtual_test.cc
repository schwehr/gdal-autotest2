// Copyright 2026 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/frmts/netcdf/netcdfvirtual.h"

#include <cstddef>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>

#include "netcdf.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/netcdf/netcdfdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"

namespace autotest2 {
namespace {

using ::nccfdriver::INVALID_DIM_ID;
using ::nccfdriver::INVALID_VAR_ID;
using ::nccfdriver::netCDFVDimension;
using ::nccfdriver::netCDFVID;
using ::nccfdriver::netCDFVVariable;
using ::nccfdriver::SG_Exception_BadMapping;
using ::nccfdriver::SG_Exception_DupName;
using ::nccfdriver::SG_Exception_NVOOB;
using ::nccfdriver::SG_Exception_VWrite_Failure;

// All files are created with NC_DISKLESS (and without NC_PERSIST), so nothing
// is written to disk; the TempDir() path only keeps the name hermetic.
std::string TempNcPath(const char* name) {
  return (std::filesystem::path(::testing::TempDir()) / name).string();
}

TEST(NetCDFVTest, DimensionBasics) {
  netCDFVDimension dim("test_dim", 10, 0);
  EXPECT_TRUE(dim.isValid());
  EXPECT_EQ("test_dim", dim.getName());
  EXPECT_EQ(10, dim.getLen());
  EXPECT_EQ(INVALID_DIM_ID, dim.getRealID());
}

TEST(NetCDFVTest, VariableBasics) {
  int dims[2] = {0, 1};
  netCDFVVariable var("test_var", NC_INT, 2, dims);
  EXPECT_TRUE(var.isValid());
  EXPECT_EQ("test_var", var.getName());
  EXPECT_EQ(NC_INT, var.getType());
  EXPECT_EQ(2, var.getDimCount());
  ASSERT_NE(nullptr, var.getDimIds());
  EXPECT_EQ(0, var.getDimIds()[0]);
  EXPECT_EQ(1, var.getDimIds()[1]);
  EXPECT_EQ(INVALID_VAR_ID, var.getRealID());
}

TEST(NetCDFVTest, DirectModeOperations) {
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(TempNcPath("mem_direct.nc").c_str(),
                                NC_CLOBBER | NC_DISKLESS, &ncid));

  netCDFDataset ds;
  netCDFVID vid(&ds, ncid);

  // Define dim in direct mode
  int dimid = vid.nc_def_vdim("extra_dim", 4);
  EXPECT_GE(dimid, 0);

  // Failure: duplicate dim name in netCDF
  EXPECT_THROW(vid.nc_def_vdim("extra_dim", 4), SG_Exception_VWrite_Failure);

  // Define var in direct mode
  int dimids[1] = {dimid};
  int varid = vid.nc_def_vvar("extra_data", NC_INT, 1, dimids);
  EXPECT_GE(varid, 0);

  // Failure: duplicate var name
  EXPECT_THROW(vid.nc_def_vvar("extra_data", NC_INT, 1, dimids),
               SG_Exception_VWrite_Failure);

  // Put attributes in direct mode
  vid.nc_put_vatt_text(varid, "units", "meters");
  int ival = 100;
  vid.nc_put_vatt_int(varid, "valid_max", &ival);
  double dval = 3.14159;
  vid.nc_put_vatt_double(varid, "scale_factor", &dval);
  float fval = 1.5f;
  vid.nc_put_vatt_float(varid, "add_offset", &fval);
  signed char bval = 1;
  vid.nc_put_vatt_byte(varid, "flag", &bval);

  // Direct mode writes straight to the file: read the attributes back.
  {
    char units[16] = {};
    ASSERT_EQ(NC_NOERR, nc_get_att_text(ncid, varid, "units", units));
    EXPECT_STREQ("meters", units);
    int ival_read = 0;
    ASSERT_EQ(NC_NOERR, nc_get_att_int(ncid, varid, "valid_max", &ival_read));
    EXPECT_EQ(100, ival_read);
    double dval_read = 0;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_double(ncid, varid, "scale_factor", &dval_read));
    EXPECT_DOUBLE_EQ(3.14159, dval_read);
    float fval_read = 0;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_float(ncid, varid, "add_offset", &fval_read));
    EXPECT_FLOAT_EQ(1.5f, fval_read);
    signed char bval_read = 0;
    ASSERT_EQ(NC_NOERR, nc_get_att_schar(ncid, varid, "flag", &bval_read));
    EXPECT_EQ(1, bval_read);
  }

  // Errors on invalid varid in direct mode
  EXPECT_THROW(vid.nc_put_vatt_text(9999, "bad", "bad"),
               SG_Exception_VWrite_Failure);
  EXPECT_THROW(vid.nc_put_vatt_int(9999, "bad", &ival),
               SG_Exception_VWrite_Failure);
  EXPECT_THROW(vid.nc_put_vatt_double(9999, "bad", &dval),
               SG_Exception_VWrite_Failure);
  EXPECT_THROW(vid.nc_put_vatt_float(9999, "bad", &fval),
               SG_Exception_VWrite_Failure);
  EXPECT_THROW(vid.nc_put_vatt_byte(9999, "bad", &bval),
               SG_Exception_VWrite_Failure);

  // Define a char variable for text writing
  int char_varid = vid.nc_def_vvar("char_data", NC_CHAR, 1, dimids);

  // Switch to data mode
  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  size_t index[1] = {0};
  const char single_char = 'A';
  vid.nc_put_vvar1_text(char_varid, index, &single_char);

  size_t start[1] = {1};
  size_t count[1] = {2};
  const char str_data[] = "BC";
  vid.nc_put_vvara_text(char_varid, start, count, str_data);

  {
    char c = 0;
    ASSERT_EQ(NC_NOERR, nc_get_var1_text(ncid, char_varid, index, &c));
    EXPECT_EQ('A', c);
    char read_back[3] = {};
    ASSERT_EQ(NC_NOERR,
              nc_get_vara_text(ncid, char_varid, start, count, read_back));
    EXPECT_STREQ("BC", read_back);
  }

  // Failure cases on bad index
  size_t bad_index[1] = {9999};
  EXPECT_THROW(vid.nc_put_vvar1_text(char_varid, bad_index, &single_char),
               SG_Exception_VWrite_Failure);
  EXPECT_THROW(vid.nc_put_vvara_text(char_varid, bad_index, count, str_data),
               SG_Exception_VWrite_Failure);

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
}

TEST(NetCDFVTest, VirtualModeOperationsAndMapping) {
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(TempNcPath("mem_virt.nc").c_str(),
                                NC_CLOBBER | NC_DISKLESS, &ncid));

  netCDFDataset ds;
  netCDFVID vid(&ds, ncid);
  vid.enableFullVirtualMode();

  // Define virtual dimensions
  int vdim0 = vid.nc_def_vdim("dim_x", 10);
  EXPECT_EQ(0, vdim0);
  EXPECT_THROW(vid.nc_def_vdim("dim_x", 10), SG_Exception_DupName);

  int vdim_deleted = vid.nc_def_vdim("dim_del", 20);
  EXPECT_EQ(1, vdim_deleted);
  vid.nc_resize_vdim(vdim0, 15);
  // Deleting virtual dim invalidates it
  vid.nc_del_vdim(vdim_deleted);

  // Enquiry for dimensions
  EXPECT_EQ(0, vid.nameToVirtualDID("dim_x"));
  EXPECT_THROW(vid.nameToVirtualDID("unknown_dim"), SG_Exception_BadMapping);
  EXPECT_EQ(15, vid.virtualDIDToDim(0).getLen());
  EXPECT_THROW(vid.virtualDIDToDim(-1), SG_Exception_NVOOB);
  EXPECT_THROW(vid.virtualDIDToDim(99), SG_Exception_NVOOB);

  // Define virtual variables
  int vdims[1] = {vdim0};
  int vvar0 = vid.nc_def_vvar("var_temp", NC_CHAR, 1, vdims);
  EXPECT_EQ(0, vvar0);
  EXPECT_TRUE(vid.virtualVarNameDefined("var_temp"));
  EXPECT_FALSE(vid.virtualVarNameDefined("non_existent"));
  EXPECT_THROW(vid.nc_def_vvar("var_temp", NC_CHAR, 1, vdims),
               SG_Exception_DupName);

  int vvar_deleted = vid.nc_def_vvar("var_del", NC_INT, 0, nullptr);
  EXPECT_EQ(1, vvar_deleted);
  // Deleting virtual var invalidates it
  vid.nc_del_vvar(vvar_deleted);

  // Enquiry for variables
  EXPECT_EQ(0, vid.nameToVirtualVID("var_temp"));
  EXPECT_THROW(vid.nameToVirtualVID("unknown_var"), SG_Exception_BadMapping);
  EXPECT_EQ("var_temp", vid.virtualVIDToVar(0).getName());
  EXPECT_THROW(vid.virtualVIDToVar(-1), SG_Exception_NVOOB);
  EXPECT_THROW(vid.virtualVIDToVar(99), SG_Exception_NVOOB);

  // Add virtual attributes
  vid.nc_put_vatt_text(vvar0, "units", "celsius");
  int ival = 10;
  vid.nc_put_vatt_int(vvar0, "max", &ival);
  double dval = 5.5;
  vid.nc_put_vatt_double(vvar0, "factor", &dval);
  float fval = 2.0f;
  vid.nc_put_vatt_float(vvar0, "gain", &fval);
  signed char bval = 3;
  vid.nc_put_vatt_byte(vvar0, "offset", &bval);

  // Attributes on invalid virtual varid
  EXPECT_THROW(vid.nc_put_vatt_text(-1, "bad", "bad"), SG_Exception_NVOOB);

  // Now map virtual objects to netCDF file (also tests skipping deleted dim and
  // var)
  vid.nc_vmap();

  // Check that real IDs are now set
  EXPECT_NE(INVALID_DIM_ID, vid.virtualDIDToDim(0).getRealID());
  EXPECT_NE(INVALID_VAR_ID, vid.virtualVIDToVar(0).getRealID());

  // The surviving dim/var now exist in the file with the resized length and
  // the queued attributes; the deleted ones were never created.
  {
    int real_dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(ncid, "dim_x", &real_dimid));
    EXPECT_EQ(vid.virtualDIDToDim(0).getRealID(), real_dimid);
    size_t dimlen = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_dimlen(ncid, real_dimid, &dimlen));
    EXPECT_EQ(15, dimlen);
    int unused_id = -1;
    EXPECT_EQ(NC_EBADDIM, nc_inq_dimid(ncid, "dim_del", &unused_id));

    int real_varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ncid, "var_temp", &real_varid));
    EXPECT_EQ(vid.virtualVIDToVar(0).getRealID(), real_varid);
    EXPECT_EQ(NC_ENOTVAR, nc_inq_varid(ncid, "var_del", &unused_id));

    char units[16] = {};
    ASSERT_EQ(NC_NOERR, nc_get_att_text(ncid, real_varid, "units", units));
    EXPECT_STREQ("celsius", units);
    int ival_read = 0;
    ASSERT_EQ(NC_NOERR, nc_get_att_int(ncid, real_varid, "max", &ival_read));
    EXPECT_EQ(10, ival_read);
    double dval_read = 0;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_double(ncid, real_varid, "factor", &dval_read));
    EXPECT_DOUBLE_EQ(5.5, dval_read);
    float fval_read = 0;
    ASSERT_EQ(NC_NOERR, nc_get_att_float(ncid, real_varid, "gain", &fval_read));
    EXPECT_FLOAT_EQ(2.0f, fval_read);
    signed char bval_read = 0;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_schar(ncid, real_varid, "offset", &bval_read));
    EXPECT_EQ(3, bval_read);
  }

  // nc_resize_vdim on already mapped dim should have no effect
  size_t old_len = vid.virtualDIDToDim(0).getLen();
  vid.nc_resize_vdim(0, 999);
  EXPECT_EQ(old_len, vid.virtualDIDToDim(0).getLen());

  // Switch netcdf to data mode
  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  // Test writing single text datum and array via virtual interface
  size_t idx[1] = {0};
  const char c = 'Z';
  vid.nc_put_vvar1_text(0, idx, &c);

  size_t start[1] = {1};
  size_t count[1] = {3};
  const char str[] = "ABC";
  vid.nc_put_vvara_text(0, start, count, str);

  {
    const int real_varid = vid.virtualVIDToVar(0).getRealID();
    char c_read = 0;
    ASSERT_EQ(NC_NOERR, nc_get_var1_text(ncid, real_varid, idx, &c_read));
    EXPECT_EQ('Z', c_read);
    char read_back[4] = {};
    ASSERT_EQ(NC_NOERR,
              nc_get_vara_text(ncid, real_varid, start, count, read_back));
    EXPECT_STREQ("ABC", read_back);
  }

  // Calling nc_put_vvar1_text, nc_put_vvara_text, or nc_put_vvar1_string on
  // deleted var returns early
  const char* str_val = "test";
  vid.nc_put_vvar1_text(vvar_deleted, idx, &c);
  vid.nc_put_vvara_text(vvar_deleted, start, count, str);
  vid.nc_put_vvar1_string(vvar_deleted, idx, &str_val);

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
}

TEST(NetCDFVTest, StringOperationsNC4) {
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(TempNcPath("mem_str.nc").c_str(),
                                NC_CLOBBER | NC_NETCDF4 | NC_DISKLESS, &ncid));

  netCDFDataset ds;
  netCDFVID vid(&ds, ncid);

  int dimid = vid.nc_def_vdim("dim_str", 2);
  int dimids[1] = {dimid};
  int varid = vid.nc_def_vvar("str_var", NC_STRING, 1, dimids);

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  size_t index[1] = {0};
  const char* str_val = "hello_world";
  vid.nc_put_vvar1_string(varid, index, &str_val);

  {
    char* read_back = nullptr;
    ASSERT_EQ(NC_NOERR, nc_get_var1_string(ncid, varid, index, &read_back));
    ASSERT_NE(nullptr, read_back);
    EXPECT_STREQ("hello_world", read_back);
    ASSERT_EQ(NC_NOERR, nc_free_string(1, &read_back));
  }

  // Failure in direct mode
  size_t bad_idx[1] = {9999};
  EXPECT_THROW(vid.nc_put_vvar1_string(varid, bad_idx, &str_val),
               SG_Exception_VWrite_Failure);

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
}

}  // namespace
}  // namespace autotest2
