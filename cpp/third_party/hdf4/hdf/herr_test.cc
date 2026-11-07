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
// Tests HDF error stack and reporting routines (hdf/src/herr.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/herr.c

#include <array>
#include <cstdio>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

// GoogleTest's gtest.h defines SUCCEED() and FAIL() as function-like macros.
// Undefine them before including HDF4 headers so hdf.h can define its constants
// SUCCEED (0) and FAIL (-1) without macro redefinition errors under no_modules.
#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/herr.h"

namespace hdf4 {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

TEST(HerrTest, HEstringReturnsKnownAndUnknownMessages) {
  struct TestCase {
    hdf_err_code_t error_code;
    std::string_view expected_str;
  };
  constexpr TestCase kCases[] = {
      {.error_code = DFE_NONE, .expected_str = "No error"},
      {.error_code = DFE_FNF, .expected_str = "File not found"},
      {.error_code = DFE_DENIED, .expected_str = "Access to file denied"},
      {.error_code = DFE_ALROPEN, .expected_str = "File already open"},
      {.error_code = DFE_TOOMANY,
       .expected_str = "Too Many AID's or files open"},
      {.error_code = DFE_BADNAME, .expected_str = "Bad file name on open"},
      {.error_code = DFE_BADACC, .expected_str = "Bad file access mode"},
      {.error_code = DFE_BADOPEN, .expected_str = "Error opening file"},
      {.error_code = DFE_NOTOPEN,
       .expected_str = "File can't be closed; It isn't open"},
      {.error_code = DFE_CANTCLOSE, .expected_str = "Unable to close file"},
      {.error_code = DFE_READERROR, .expected_str = "Read error"},
      {.error_code = DFE_WRITEERROR, .expected_str = "Write error"},
      {.error_code = DFE_SEEKERROR,
       .expected_str = "Error performing seek operation"},
      {.error_code = DFE_RDONLY,
       .expected_str = "Attempt to write to read-only HDF file"},
      {.error_code = DFE_BADSEEK,
       .expected_str = "Attempt to seek past end of element"},
      {.error_code = DFE_INVFILE,
       .expected_str =
           "File is not supported, must be either hdf, cdf, netcdf"},
      {.error_code = DFE_PUTELEM,
       .expected_str = "Hputelement failed in some way"},
      {.error_code = DFE_GETELEM,
       .expected_str = "Hgetelement failed in some way"},
      {.error_code = DFE_CANTLINK,
       .expected_str = "Can't initialize link information"},
      {.error_code = DFE_CANTSYNC,
       .expected_str = "Cannot synchronize memory with file"},
      {.error_code = DFE_BADGROUP,
       .expected_str = "Error from DFdiread in opening a group"},
      {.error_code = DFE_GROUPSETUP,
       .expected_str = "Error from DFdisetup in opening a group"},
      {.error_code = DFE_PUTGROUP,
       .expected_str = "Error when putting a tag/ref into a group"},
      {.error_code = DFE_GROUPWRITE,
       .expected_str = "Error when writing out a group"},
      {.error_code = DFE_DFNULL, .expected_str = "DF has a null pointer"},
      {.error_code = DFE_ILLTYPE,
       .expected_str = "Internal error: DF has an illegal type"},
      {.error_code = DFE_BADDDLIST,
       .expected_str = "Internal error: The DD list is non-existent"},
      {.error_code = DFE_NOTDFFILE, .expected_str = "This is not an HDF file"},
      {.error_code = DFE_SEEDTWICE,
       .expected_str = "Internal error: The DD list is already seeded"},
      {.error_code = DFE_NOSUCHTAG,
       .expected_str = "No such tag in the file: search failed"},
      {.error_code = DFE_NOFREEDD,
       .expected_str = "There are no free DD's left"},
      {.error_code = DFE_BADTAG, .expected_str = "Illegal WILDCARD tag"},
      {.error_code = DFE_BADREF, .expected_str = "Illegal WILDCARD reference"},
      {.error_code = DFE_NOMATCH,
       .expected_str = "No (more) DDs which match specified tag/ref"},
      {.error_code = DFE_NOTINSET,
       .expected_str = "Set contained unknown tag: ignored"},
      {.error_code = DFE_BADOFFSET, .expected_str = "Illegal offset specified"},
      {.error_code = DFE_CORRUPT, .expected_str = "File is corrupted"},
      {.error_code = DFE_NOREF,
       .expected_str = "No more reference numbers are available"},
      {.error_code = DFE_DUPDD, .expected_str = "Tag/ref is already used"},
      {.error_code = DFE_CANTMOD,
       .expected_str = "Old element does not exist, cannot modify"},
      {.error_code = DFE_DIFFFILES,
       .expected_str = "Attempt to merge objects in different files"},
      {.error_code = DFE_BADAID, .expected_str = "Unable to create a new AID"},
      {.error_code = DFE_OPENAID,
       .expected_str = "There are still active AIDs"},
      {.error_code = DFE_CANTFLUSH,
       .expected_str = "Cannot flush the changed DD back to the file"},
      {.error_code = DFE_CANTUPDATE,
       .expected_str = "Cannot update the DD block"},
      {.error_code = DFE_CANTHASH,
       .expected_str = "Cannot add a DD to the hash table"},
      {.error_code = DFE_CANTDELDD,
       .expected_str = "Cannot delete a DD in the file"},
      {.error_code = DFE_CANTDELHASH,
       .expected_str = "Cannot delete a DD from the hash table"},
      {.error_code = DFE_CANTACCESS,
       .expected_str = "Cannot access specified tag/ref"},
      {.error_code = DFE_CANTENDACCESS,
       .expected_str = "Cannot end access to data element"},
      {.error_code = DFE_TABLEFULL, .expected_str = "Access table is full"},
      {.error_code = DFE_NOTINTABLE,
       .expected_str = "Cannot find element in table"},
      {.error_code = DFE_UNSUPPORTED,
       .expected_str = "Feature not currently supported"},
      {.error_code = DFE_NOSPACE,
       .expected_str = "Internal error: Out of space"},
      {.error_code = DFE_BADCALL, .expected_str = "Calls in wrong order"},
      {.error_code = DFE_BADPTR, .expected_str = "NULL ptr argument"},
      {.error_code = DFE_BADLEN, .expected_str = "Invalid length specified"},
      {.error_code = DFE_NOTENOUGH,
       .expected_str = "Space provided insufficient for size of data"},
      {.error_code = DFE_NOVALS, .expected_str = "Values not available"},
      {.error_code = DFE_ARGS, .expected_str = "Invalid arguments to routine"},
      {.error_code = DFE_INTERNAL, .expected_str = "HDF Internal error"},
      {.error_code = DFE_NORESET, .expected_str = "Can not reset this value"},
      {.error_code = DFE_EXCEEDMAX,
       .expected_str = "Value exceeds max allowed"},
      {.error_code = DFE_GENAPP,
       .expected_str = "Generic application-level error"},
      {.error_code = DFE_UNINIT,
       .expected_str = "Interface was not initialized correctly"},
      {.error_code = DFE_CANTINIT,
       .expected_str = "Can't initialize an interface we depend on"},
      {.error_code = DFE_CANTSHUTDOWN,
       .expected_str = "Can't shut down an interface we depend on"},
      {.error_code = DFE_BADDIM,
       .expected_str = "Negative or zero dimensions specified"},
      {.error_code = DFE_BADFP,
       .expected_str = "File contained an illegal floating point number"},
      {.error_code = DFE_BADDATATYPE,
       .expected_str = "Unknown or unavailable data type specified"},
      {.error_code = DFE_BADMCTYPE,
       .expected_str = "Unknown or unavailable machine type specified"},
      {.error_code = DFE_BADNUMTYPE,
       .expected_str = "Unknown or unavailable number type specified"},
      {.error_code = DFE_BADORDER,
       .expected_str = "Unknown or illegal array order specified"},
      {.error_code = DFE_RANGE,
       .expected_str = "Improper range for attempted access"},
      {.error_code = DFE_BADCONV,
       .expected_str = "Don't know how to convert data type"},
      {.error_code = DFE_BADTYPE,
       .expected_str = "Incompatible type specified"},
      {.error_code = DFE_BADDIMNAME,
       .expected_str = "Dimension name not valid or already taken"},
      {.error_code = DFE_NOVGREP,
       .expected_str = "No Vgroup representation for SDS and dim"},
      {.error_code = DFE_BADSCHEME,
       .expected_str = "Unknown compression scheme specified"},
      {.error_code = DFE_BADMODEL,
       .expected_str = "Invalid compression model specified"},
      {.error_code = DFE_BADCODER,
       .expected_str = "Invalid compression coder specified"},
      {.error_code = DFE_MODEL,
       .expected_str = "Error in modeling layer of compression"},
      {.error_code = DFE_CODER,
       .expected_str = "Error in encoding layer of compression"},
      {.error_code = DFE_CINIT,
       .expected_str = "Error in encoding initialization"},
      {.error_code = DFE_CDECODE,
       .expected_str = "Error in decoding compressed data"},
      {.error_code = DFE_CENCODE,
       .expected_str = "Error in encoding compressed data"},
      {.error_code = DFE_CTERM,
       .expected_str = "Error in encoding termination"},
      {.error_code = DFE_CSEEK,
       .expected_str = "Error seeking in encoded dataset"},
      {.error_code = DFE_MINIT,
       .expected_str = "Error in modeling initialization"},
      {.error_code = DFE_COMPINFO,
       .expected_str = "Invalid compression header"},
      {.error_code = DFE_CANTCOMP, .expected_str = "Can't compress an object"},
      {.error_code = DFE_CANTDECOMP,
       .expected_str = "Can't de-compress an object"},
      {.error_code = DFE_NOENCODER, .expected_str = "Encoder not available"},
      {.error_code = DFE_NOSZLIB, .expected_str = "SZIP library not available"},
      {.error_code = DFE_COMPVERSION,
       .expected_str = "Z_VERSION_ERROR (-6) returned from zlib"},
      {.error_code = DFE_READCOMP,
       .expected_str = "Error in reading compressed data"},
      {.error_code = DFE_NODIM,
       .expected_str = "No dimension record associated with image or data set"},
      {.error_code = DFE_BADRIG, .expected_str = "Error processing a RIG"},
      {.error_code = DFE_RINOTFOUND, .expected_str = "Can't find raster image"},
      {.error_code = DFE_BADATTR, .expected_str = "Bad Attribute"},
      {.error_code = DFE_LUTNOTFOUND,
       .expected_str = "No palette information for RIG"},
      {.error_code = DFE_GRNOTFOUND, .expected_str = "Can't find specified GR"},
      {.error_code = DFE_BADTABLE, .expected_str = "The nsdg table is wrong"},
      {.error_code = DFE_BADSDG, .expected_str = "Error processing an sdg"},
      {.error_code = DFE_BADNDG, .expected_str = "Error processing an ndg"},
      {.error_code = DFE_VGSIZE,
       .expected_str = "No more elements will fit in this VGroup"},
      {.error_code = DFE_VTAB, .expected_str = "Element is not in VSet tables"},
      {.error_code = DFE_CANTADDELEM,
       .expected_str = "Cannot add tag/ref to VGroup"},
      {.error_code = DFE_BADVGNAME, .expected_str = "Cannot set VGroup name"},
      {.error_code = DFE_BADVGCLASS, .expected_str = "Cannot set VGroup class"},
      {.error_code = DFE_BADFIELDS,
       .expected_str = "Unable to parse fields string correctly"},
      {.error_code = DFE_NOVS,
       .expected_str = "Could not find specified VS or VG in file"},
      {.error_code = DFE_SYMSIZE, .expected_str = "Too many symbols in table"},
      {.error_code = DFE_BADATTACH,
       .expected_str = "Cannot write to a previously attached VData"},
      {.error_code = DFE_BADVSNAME, .expected_str = "Cannot set VData name"},
      {.error_code = DFE_BADVSCLASS, .expected_str = "Cannot set VData class"},
      {.error_code = DFE_VSWRITE, .expected_str = "Error writing to VData"},
      {.error_code = DFE_VSREAD, .expected_str = "Error reading from VData"},
      {.error_code = DFE_BADVH, .expected_str = "Error in VData Header"},
      {.error_code = DFE_FIELDSSET,
       .expected_str = "Fields already set for vdata"},
      {.error_code = DFE_VSCANTCREATE, .expected_str = "Cannot create VData"},
      {.error_code = DFE_VGCANTCREATE, .expected_str = "Cannot create VGroup"},
      {.error_code = DFE_CANTATTACH,
       .expected_str = "Cannot attach to a VData"},
      {.error_code = DFE_CANTDETACH,
       .expected_str = "Cannot detach a VData with access 'w'"},
      {.error_code = DFE_XDRERROR,
       .expected_str = "Error from XDR and/or CDF level"},
      {.error_code = DFE_BITREAD, .expected_str = "There was a bit-read error"},
      {.error_code = DFE_BITWRITE,
       .expected_str = "There was a bit-write error"},
      {.error_code = DFE_BITSEEK, .expected_str = "There was a bit-seek error"},
      {.error_code = DFE_TBBTINS,
       .expected_str = "Failed to insert element into tree"},
      {.error_code = DFE_BVNEW,
       .expected_str = "Failed to create a bit-vector"},
      {.error_code = DFE_BVSET,
       .expected_str = "Failed when setting a bit in a bit-vector"},
      {.error_code = DFE_BVGET,
       .expected_str = "Failed when getting a bit in a bit-vector"},
      {.error_code = DFE_BVFIND,
       .expected_str = "Failed when finding a bit in a bit-vector"},
      {.error_code = DFE_CANTSETATTR,
       .expected_str = "Cannot set an attribute"},
      {.error_code = DFE_CANTGETATTR,
       .expected_str = "Cannot find or get an attribute"},
      {.error_code = DFE_ANAPIERROR,
       .expected_str = "Failed in annotation interface"},
  };

  for (const auto& [code, expected] : kCases) {
    EXPECT_STREQ(expected.data(), HEstring(code));
  }

  // Unknown error code returns the default message.
  EXPECT_STREQ("Unknown error", HEstring(static_cast<hdf_err_code_t>(9999)));
}

TEST(HerrTest, HEclearOnEmptyStackDoesNotCrash) {
  HEclear();
  EXPECT_EQ(DFE_NONE, HEvalue(1));
}

TEST(HerrTest, HEpushAndHEvalueOrder) {
  HEclear();
  absl::Cleanup clean_stack = [] { HEclear(); };

  HEpush(DFE_FNF, "FuncA", "fileA.c", 10);
  HEpush(DFE_DENIED, "FuncB", "fileB.c", 20);
  HEpush(DFE_READERROR, "FuncC", "fileC.c", 30);

  // HEvalue(1) is the most recent error (FuncC / DFE_READERROR).
  EXPECT_EQ(DFE_READERROR, HEvalue(1));
  EXPECT_EQ(DFE_DENIED, HEvalue(2));
  EXPECT_EQ(DFE_FNF, HEvalue(3));

  // Invalid levels return DFE_NONE.
  EXPECT_EQ(DFE_NONE, HEvalue(0));
  EXPECT_EQ(DFE_NONE, HEvalue(-1));
  EXPECT_EQ(DFE_NONE, HEvalue(4));
}

TEST(HerrTest, HEpushDiscardsWhenStackIsFull) {
  HEclear();
  absl::Cleanup clean_stack = [] { HEclear(); };

  // ERR_STACK_SZ is 10. Push 12 errors.
  for (int i = 1; i <= 12; ++i) {
    HEpush(static_cast<hdf_err_code_t>(DFE_FNF + i), "LoopFunc", "loop.c", i);
  }

  // Only the first 10 were stored, 11 and 12 were discarded.
  // The top (most recent accepted) error is item 10: DFE_FNF + 10.
  EXPECT_EQ(static_cast<hdf_err_code_t>(DFE_FNF + 10), HEvalue(1));
  // The bottom error is item 1: DFE_FNF + 1 at level 10.
  EXPECT_EQ(static_cast<hdf_err_code_t>(DFE_FNF + 1), HEvalue(10));
  // Beyond stack depth 10 returns DFE_NONE.
  EXPECT_EQ(DFE_NONE, HEvalue(11));
}

TEST(HerrTest, HEreportAddsDescription) {
  HEclear();
  absl::Cleanup clean_stack = [] { HEclear(); };

  // HEreport when stack is empty is a no-op.
  HEreport("Orphan report: %d", 123);
  EXPECT_EQ(DFE_NONE, HEvalue(1));

  // Push an error and report a formatted description.
  HEpush(DFE_BADNAME, "OpenFunc", "open.c", 42);
  HEreport("Cannot open file: %s (code=%d)", "missing.hdf", 404);

  // Overwrite existing description on the same top error.
  HEreport("Revised cannot open file: %s", "missing.hdf");

  FILE* stream = std::tmpfile();
  ASSERT_NE(stream, nullptr);
  absl::Cleanup close_stream = [stream] { std::fclose(stream); };

  HEprint(stream, 0);
  std::rewind(stream);

  std::array<char, 1024> buffer{};
  const size_t bytes_read =
      std::fread(buffer.data(), 1, buffer.size() - 1, stream);
  buffer[bytes_read] = '\0';
  const std::string_view output(buffer.data(), bytes_read);

  EXPECT_THAT(output, HasSubstr("HDF error: (5) <Bad file name on open>"));
  EXPECT_THAT(output, HasSubstr("Detected in OpenFunc() [open.c line 42]"));
  EXPECT_THAT(output, HasSubstr("Revised cannot open file: missing.hdf"));
}

TEST(HerrTest, HEprintDifferentLevels) {
  HEclear();
  absl::Cleanup clean_stack = [] { HEclear(); };

  HEpush(DFE_FNF, "Func1", "file1.c", 100);
  HEpush(DFE_DENIED, "Func2", "file2.c", 200);

  // 1. Print level 1.
  // Note: In herr.c, HEprint uses print_levels as the array index upper bound:
  // for (print_levels--; print_levels >= 0; print_levels--). When print_levels
  // is 1, the loop runs only for index 0 (the oldest error, Func1).
  {
    FILE* stream = std::tmpfile();
    ASSERT_NE(stream, nullptr);
    absl::Cleanup close_stream = [stream] { std::fclose(stream); };

    HEprint(stream, 1);
    std::rewind(stream);

    std::array<char, 1024> buffer{};
    const size_t bytes_read =
        std::fread(buffer.data(), 1, buffer.size() - 1, stream);
    const std::string_view output(buffer.data(), bytes_read);

    EXPECT_THAT(output, HasSubstr("Func1"));
    EXPECT_THAT(output, Not(HasSubstr("Func2")));
  }

  // 2. Print level > error_top prints all.
  {
    FILE* stream = std::tmpfile();
    ASSERT_NE(stream, nullptr);
    absl::Cleanup close_stream = [stream] { std::fclose(stream); };

    HEprint(stream, 999);
    std::rewind(stream);

    std::array<char, 1024> buffer{};
    const size_t bytes_read =
        std::fread(buffer.data(), 1, buffer.size() - 1, stream);
    const std::string_view output(buffer.data(), bytes_read);

    EXPECT_THAT(output, HasSubstr("Func2"));
    EXPECT_THAT(output, HasSubstr("Func1"));
  }
}

TEST(HerrTest, HEclearFreesDescriptions) {
  HEclear();
  HEpush(DFE_INTERNAL, "FailFunc", "fail.c", 1);
  HEreport("Description string that needs to be freed");
  HEpush(DFE_CORRUPT, "CorruptFunc", "corrupt.c", 2);
  HEreport("Another description");

  EXPECT_EQ(DFE_CORRUPT, HEvalue(1));
  HEclear();
  EXPECT_EQ(DFE_NONE, HEvalue(1));
}

TEST(HerrTest, HEshutdownCleansUpAndAllowsReinitialization) {
  // Populate error stack.
  HEclear();
  HEpush(DFE_FNF, "InitFunc", "init.c", 5);
  EXPECT_EQ(DFE_FNF, HEvalue(1));

  // Shutdown frees error_stack.
  EXPECT_EQ(SUCCEED, HEshutdown());
  EXPECT_EQ(DFE_NONE, HEvalue(1));

  // Idempotent shutdown when error_stack is already null.
  EXPECT_EQ(SUCCEED, HEshutdown());

  // Subsequent HEpush reallocates error_stack cleanly.
  HEpush(DFE_READERROR, "RestartFunc", "restart.c", 10);
  EXPECT_EQ(DFE_READERROR, HEvalue(1));
  EXPECT_EQ(SUCCEED, HEshutdown());
}

}  // namespace
}  // namespace hdf4
