#ifndef THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_VSIMEM_H_
#define THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_VSIMEM_H_

#include <string>
#include "logging.h"
#include "port/cpl_vsi.h"

namespace autotest2 {

// Writes a buffer to an in-memory filesystem and deletes the file when the
// instance goes out of scope.
class VsiMemTempWrapper {
 public:
  VsiMemTempWrapper(const string &filename, const string &data)
      : filename_(filename) {
    VSILFILE *file = VSIFOpenL(filename.c_str(), "wb");
    CHECK_NE(nullptr, file);
    CHECK_EQ(1, VSIFWriteL(data.c_str(), data.length(), 1, file));
    CHECK_EQ(0, VSIFCloseL(file));
  }
  ~VsiMemTempWrapper() { CHECK_EQ(0, VSIUnlink(filename_.c_str())); }

 private:
  string filename_;
};

// Writes a buffer to an in-memory filesystem and deletes the file when the
// instance goes out of scope.
class VsiMemMaybeTempWrapper {
 public:
  VsiMemMaybeTempWrapper(const string &filename, const string &data, bool do_it)
      : filename_(filename), do_it_(do_it) {
    if (!do_it_) return;
    VSILFILE *file = VSIFOpenL(filename.c_str(), "wb");
    CHECK_NE(nullptr, file);
    CHECK_EQ(1, VSIFWriteL(data.c_str(), data.length(), 1, file));
    CHECK_EQ(0, VSIFCloseL(file));
  }
  ~VsiMemMaybeTempWrapper() {
    if (!do_it_) return;
    CHECK_EQ(0, VSIUnlink(filename_.c_str()));
  }

 private:
  string filename_;
  bool do_it_;
};

}  // namespace autotest2

#endif  // THIRD_PARTY_GDAL_AUTOTEST2_CPP_UTIL_VSIMEM_H_
