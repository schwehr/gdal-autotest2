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
// Unit tests for gcore/gdalpython.cpp.
//
// Build & runtime caveats in google3:
// - libpython is not linked in google3, so Python initialization and C-API
//   calls are tested via dynamic-linker interposition (dlopen/dlclose/dlsym
//   resolved through dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34"|"2.2.5"|"2.17"))
//   outside sanitizer builds, and direct GDALPy function-pointer stubs under
//   ASan/MSan/TSan (whose runtimes interpose dlsym).
// - GDALPythonInitialize() is exercised using a temporary #!/bin/sh version
//   script and a two-level symlink chain prepended to PATH in
//   ::testing::TempDir().
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalpython.cpp

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer)
#define GDAL_PYTHON_TEST_USE_DIRECT_STUBS 1
#endif
#elif defined(ADDRESS_SANITIZER) || defined(MEMORY_SANITIZER) || \
    defined(THREAD_SANITIZER)
#define GDAL_PYTHON_TEST_USE_DIRECT_STUBS 1
#endif

#if !defined(GDAL_PYTHON_TEST_USE_DIRECT_STUBS)
#include <dlfcn.h>
#endif

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdalpython.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

GDALPy::PyObject g_dummy_obj = {1, nullptr};
GDALPy::PyObject g_bytes_obj = {1, nullptr};
GDALPy::PyObject g_tuple_obj = {1, nullptr};
GDALPy::PyObject g_mod_obj = {1, nullptr};
GDALPy::PyObject g_func_obj = {1, nullptr};
GDALPy::PyObject g_res_obj = {1, nullptr};
GDALPy::PyObject g_err_obj = {1, nullptr};

bool g_py_err_occurred = false;
bool g_py_call_fail = false;
bool g_py_compile_fail = false;
bool g_py_unicode_fail = false;

void Stub_Py_IncRef(GDALPy::PyObject*) {}
void Stub_Py_DecRef(GDALPy::PyObject*) {}

GDALPy::PyObject* Stub_PyErr_Occurred() {
  return g_py_err_occurred ? &g_err_obj : nullptr;
}

void Stub_PyErr_Print() {}

void Stub_PyErr_Fetch(GDALPy::PyObject** p_type, GDALPy::PyObject** p_val,
                      GDALPy::PyObject** p_tb) {
  g_py_err_occurred = false;
  *p_type = &g_dummy_obj;
  *p_val = &g_dummy_obj;
  *p_tb = &g_dummy_obj;
}

void Stub_PyErr_Clear() { g_py_err_occurred = false; }

GDALPy::PyObject* Stub_Py_CompileStringExFlags(const char*, const char*, int,
                                               void*, int) {
  return g_py_compile_fail ? nullptr : &g_dummy_obj;
}

GDALPy::PyObject* Stub_PyImport_ExecCodeModule(const char*, GDALPy::PyObject*) {
  return &g_mod_obj;
}

GDALPy::PyObject* Stub_PyObject_GetAttrString(GDALPy::PyObject*, const char*) {
  return &g_func_obj;
}

GDALPy::PyObject* Stub_PyTuple_New(size_t) { return &g_tuple_obj; }

int Stub_PyTuple_SetItem(GDALPy::PyObject*, size_t, GDALPy::PyObject*) {
  return 0;
}

GDALPy::PyObject* Stub_PyObject_Call(GDALPy::PyObject*, GDALPy::PyObject*,
                                     GDALPy::PyObject*) {
  if (g_py_call_fail) {
    g_py_err_occurred = true;
    return nullptr;
  }
  return &g_res_obj;
}

GDALPy::PyObject* Stub_PyUnicode_AsUTF8String(GDALPy::PyObject*) {
  return g_py_unicode_fail ? nullptr : &g_bytes_obj;
}

const char* Stub_PyBytes_AsString(GDALPy::PyObject* p) {
  return p != nullptr ? "stub_py_str" : nullptr;
}

#if !defined(GDAL_PYTHON_TEST_USE_DIRECT_STUBS)

#define NO_SANITIZE \
  __attribute__((no_sanitize("address", "memory", "thread", "undefined")))

using DlopenFn = void* (*)(const char*, int);
using DlcloseFn = int (*)(void*);
using DlsymFn = void* (*)(void*, const char*);

int g_fake_lib = 0;
bool g_py_is_initialized = false;

int Stub_Py_IsInitialized() { return g_py_is_initialized ? 1 : 0; }
void Stub_Py_InitializeEx(int) {}
void Stub_PyEval_InitThreads() {}
void* Stub_PyEval_SaveThread() { return &g_dummy_obj; }
void Stub_PyEval_RestoreThread(void*) {}
void Stub_Py_Finalize() {}
const char* Stub_Py_GetVersion() { return "3.12.0\r\n(stub)"; }
void Stub_Py_SetProgramName(const wchar_t*) {}
void Stub_Py_SetPythonHome(const wchar_t*) {}
GDALPy::PyGILState_STATE Stub_PyGILState_Ensure() { return 0; }
void Stub_PyGILState_Release(GDALPy::PyGILState_STATE) {}

int Stub_PyBuffer_FillInfo(GDALPy::Py_buffer*, GDALPy::PyObject*, void*, size_t,
                           int, int) {
  return 0;
}

GDALPy::PyObject* Stub_PyMemoryView_FromBuffer(GDALPy::Py_buffer*) {
  return &g_dummy_obj;
}

GDALPy::PyObject* Stub_PyObject_Type(GDALPy::PyObject*) { return &g_dummy_obj; }

int Stub_PyObject_IsInstance(GDALPy::PyObject*, GDALPy::PyObject*) { return 1; }

GDALPy::PyObject* Stub_PyBool_FromLong(long) {  // NOLINT(runtime/int)
  return &g_dummy_obj;
}

GDALPy::PyObject* Stub_PyLong_FromLong(long) {  // NOLINT(runtime/int)
  return &g_dummy_obj;
}

long Stub_PyLong_AsLong(GDALPy::PyObject*) {  // NOLINT(runtime/int)
  return 0;
}

GDALPy::PyObject* Stub_PyLong_FromLongLong(GIntBig) { return &g_dummy_obj; }
GIntBig Stub_PyLong_AsLongLong(GDALPy::PyObject*) { return 0; }
GDALPy::Py_ssize_t Stub_PyBytes_Size(GDALPy::PyObject*) { return 0; }

int* Stub_PyBytes_AsStringAndSize(GDALPy::PyObject*, char**,
                                  GDALPy::Py_ssize_t*) {
  return nullptr;
}

GDALPy::PyObject* Stub_PyBytes_FromObject(GDALPy::PyObject*) {
  return &g_bytes_obj;
}

GDALPy::PyObject* Stub_PyBytes_FromStringAndSize(const void*, size_t) {
  return &g_bytes_obj;
}

GDALPy::PyObject* Stub_PyModule_Create2(GDALPy::PyModuleDef*, int) {
  return &g_mod_obj;
}

GDALPy::PyObject* Stub_PyUnicode_FromString(const char*) {
  return &g_dummy_obj;
}

GDALPy::PyObject* Stub_PyFloat_FromDouble(double) { return &g_dummy_obj; }
double Stub_PyFloat_AsDouble(GDALPy::PyObject*) { return 0.0; }
GDALPy::PyObject* Stub_PyObject_GetIter(GDALPy::PyObject*) {
  return &g_dummy_obj;
}

GDALPy::PyObject* Stub_PyIter_Next(GDALPy::PyObject*) { return nullptr; }
int Stub_PyObject_HasAttrString(GDALPy::PyObject*, const char*) { return 1; }
int Stub_PyObject_SetAttrString(GDALPy::PyObject*, const char*,
                                GDALPy::PyObject*) {
  return 0;
}

void Stub_PyObject_Print(GDALPy::PyObject*, FILE*, int) {}
GDALPy::PyObject* Stub_PyImport_ImportModule(const char*) { return &g_mod_obj; }
int Stub_PyCallable_Check(GDALPy::PyObject*) { return 1; }
GDALPy::PyObject* Stub_PyDict_New() { return &g_dummy_obj; }

int Stub_PyDict_SetItemString(GDALPy::PyObject*, const char*,
                              GDALPy::PyObject*) {
  return 0;
}

int Stub_PyDict_Next(GDALPy::PyObject*, size_t*, GDALPy::PyObject**,
                     GDALPy::PyObject**) {
  return 0;
}

GDALPy::PyObject* Stub_PyDict_GetItemString(GDALPy::PyObject*, const char*) {
  return &g_dummy_obj;
}

GDALPy::PyObject* Stub_PyList_New(GDALPy::Py_ssize_t) { return &g_dummy_obj; }
int Stub_PyList_SetItem(GDALPy::PyObject*, GDALPy::Py_ssize_t,
                        GDALPy::PyObject*) {
  return 0;
}

int Stub_PyArg_ParseTuple(GDALPy::PyObject*, const char*, ...) { return 1; }
int Stub_PySequence_Check(GDALPy::PyObject*) { return 1; }
GDALPy::Py_ssize_t Stub_PySequence_Size(GDALPy::PyObject*) { return 0; }
GDALPy::PyObject* Stub_PySequence_GetItem(GDALPy::PyObject*,
                                          GDALPy::Py_ssize_t) {
  return nullptr;
}

NO_SANITIZE bool IsEqual(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) {
    return a == b;
  }
  while (*a != '\0' && *b != '\0') {
    if (*a != *b) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == *b;
}

NO_SANITIZE bool Contains(const char* haystack, const char* needle) {
  if (haystack == nullptr || needle == nullptr) {
    return false;
  }
  if (*needle == '\0') {
    return true;
  }
  for (const char* p = haystack; *p != '\0'; ++p) {
    const char* h = p;
    const char* n = needle;
    while (*h != '\0' && *n != '\0' && *h == *n) {
      ++h;
      ++n;
    }
    if (*n == '\0') {
      return true;
    }
  }
  return false;
}

NO_SANITIZE DlsymFn GetRealDlsym() {
  static DlsymFn real_dlsym = []() NO_SANITIZE -> DlsymFn {
    void* sym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (sym == nullptr) {
      sym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    }
    if (sym == nullptr) {
      sym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
    }
    return reinterpret_cast<DlsymFn>(sym);
  }();
  return real_dlsym;
}

NO_SANITIZE DlopenFn GetRealDlopen() {
  static DlopenFn real_dlopen = []() NO_SANITIZE -> DlopenFn {
    DlsymFn real_dlsym = GetRealDlsym();
    if (real_dlsym == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<DlopenFn>(real_dlsym(RTLD_NEXT, "dlopen"));
  }();
  return real_dlopen;
}

NO_SANITIZE DlcloseFn GetRealDlclose() {
  static DlcloseFn real_dlclose = []() NO_SANITIZE -> DlcloseFn {
    DlsymFn real_dlsym = GetRealDlsym();
    if (real_dlsym == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<DlcloseFn>(real_dlsym(RTLD_NEXT, "dlclose"));
  }();
  return real_dlclose;
}

NO_SANITIZE void* LookupStubSymbol(const char* symbol) {
  if (IsEqual(symbol, "Py_CompileString")) return nullptr;
  if (IsEqual(symbol, "Py_CompileStringExFlags")) {
    return reinterpret_cast<void*>(Stub_Py_CompileStringExFlags);
  }
  if (IsEqual(symbol, "Py_IsInitialized")) {
    return reinterpret_cast<void*>(Stub_Py_IsInitialized);
  }
  if (IsEqual(symbol, "Py_InitializeEx")) {
    return reinterpret_cast<void*>(Stub_Py_InitializeEx);
  }
  if (IsEqual(symbol, "PyEval_InitThreads")) {
    return reinterpret_cast<void*>(Stub_PyEval_InitThreads);
  }
  if (IsEqual(symbol, "PyEval_SaveThread")) {
    return reinterpret_cast<void*>(Stub_PyEval_SaveThread);
  }
  if (IsEqual(symbol, "PyEval_RestoreThread")) {
    return reinterpret_cast<void*>(Stub_PyEval_RestoreThread);
  }
  if (IsEqual(symbol, "Py_Finalize")) {
    return reinterpret_cast<void*>(Stub_Py_Finalize);
  }
  if (IsEqual(symbol, "Py_GetVersion")) {
    return reinterpret_cast<void*>(Stub_Py_GetVersion);
  }
  if (IsEqual(symbol, "Py_SetProgramName")) {
    return reinterpret_cast<void*>(Stub_Py_SetProgramName);
  }
  if (IsEqual(symbol, "Py_SetPythonHome")) {
    return reinterpret_cast<void*>(Stub_Py_SetPythonHome);
  }
  if (IsEqual(symbol, "PyGILState_Ensure")) {
    return reinterpret_cast<void*>(Stub_PyGILState_Ensure);
  }
  if (IsEqual(symbol, "PyGILState_Release")) {
    return reinterpret_cast<void*>(Stub_PyGILState_Release);
  }
  if (IsEqual(symbol, "PyBuffer_FillInfo")) {
    return reinterpret_cast<void*>(Stub_PyBuffer_FillInfo);
  }
  if (IsEqual(symbol, "PyMemoryView_FromBuffer")) {
    return reinterpret_cast<void*>(Stub_PyMemoryView_FromBuffer);
  }
  if (IsEqual(symbol, "PyObject_Type")) {
    return reinterpret_cast<void*>(Stub_PyObject_Type);
  }
  if (IsEqual(symbol, "PyObject_IsInstance")) {
    return reinterpret_cast<void*>(Stub_PyObject_IsInstance);
  }
  if (IsEqual(symbol, "PyTuple_New")) {
    return reinterpret_cast<void*>(Stub_PyTuple_New);
  }
  if (IsEqual(symbol, "PyBool_FromLong")) {
    return reinterpret_cast<void*>(Stub_PyBool_FromLong);
  }
  if (IsEqual(symbol, "PyLong_FromLong")) {
    return reinterpret_cast<void*>(Stub_PyLong_FromLong);
  }
  if (IsEqual(symbol, "PyLong_AsLong")) {
    return reinterpret_cast<void*>(Stub_PyLong_AsLong);
  }
  if (IsEqual(symbol, "PyLong_FromLongLong")) {
    return reinterpret_cast<void*>(Stub_PyLong_FromLongLong);
  }
  if (IsEqual(symbol, "PyLong_AsLongLong")) {
    return reinterpret_cast<void*>(Stub_PyLong_AsLongLong);
  }
  if (IsEqual(symbol, "PyBytes_Size")) {
    return reinterpret_cast<void*>(Stub_PyBytes_Size);
  }
  if (IsEqual(symbol, "PyBytes_AsString")) {
    return reinterpret_cast<void*>(Stub_PyBytes_AsString);
  }
  if (IsEqual(symbol, "PyBytes_AsStringAndSize")) {
    return reinterpret_cast<void*>(Stub_PyBytes_AsStringAndSize);
  }
  if (IsEqual(symbol, "PyBytes_FromObject")) {
    return reinterpret_cast<void*>(Stub_PyBytes_FromObject);
  }
  if (IsEqual(symbol, "PyBytes_FromStringAndSize")) {
    return reinterpret_cast<void*>(Stub_PyBytes_FromStringAndSize);
  }
  if (IsEqual(symbol, "PyModule_Create2")) {
    return reinterpret_cast<void*>(Stub_PyModule_Create2);
  }
  if (IsEqual(symbol, "PyUnicodeUCS4_FromString")) {
    return reinterpret_cast<void*>(Stub_PyUnicode_FromString);
  }
  if (IsEqual(symbol, "PyUnicodeUCS4_AsUTF8String")) {
    return reinterpret_cast<void*>(Stub_PyUnicode_AsUTF8String);
  }
  if (IsEqual(symbol, "PyFloat_FromDouble")) {
    return reinterpret_cast<void*>(Stub_PyFloat_FromDouble);
  }
  if (IsEqual(symbol, "PyFloat_AsDouble")) {
    return reinterpret_cast<void*>(Stub_PyFloat_AsDouble);
  }
  if (IsEqual(symbol, "PyObject_Call")) {
    return reinterpret_cast<void*>(Stub_PyObject_Call);
  }
  if (IsEqual(symbol, "PyObject_GetIter")) {
    return reinterpret_cast<void*>(Stub_PyObject_GetIter);
  }
  if (IsEqual(symbol, "PyIter_Next")) {
    return reinterpret_cast<void*>(Stub_PyIter_Next);
  }
  if (IsEqual(symbol, "Py_IncRef")) {
    return reinterpret_cast<void*>(Stub_Py_IncRef);
  }
  if (IsEqual(symbol, "Py_DecRef")) {
    return reinterpret_cast<void*>(Stub_Py_DecRef);
  }
  if (IsEqual(symbol, "PyErr_Occurred")) {
    return reinterpret_cast<void*>(Stub_PyErr_Occurred);
  }
  if (IsEqual(symbol, "PyErr_Print")) {
    return reinterpret_cast<void*>(Stub_PyErr_Print);
  }
  if (IsEqual(symbol, "PyImport_ExecCodeModule")) {
    return reinterpret_cast<void*>(Stub_PyImport_ExecCodeModule);
  }
  if (IsEqual(symbol, "PyObject_HasAttrString")) {
    return reinterpret_cast<void*>(Stub_PyObject_HasAttrString);
  }
  if (IsEqual(symbol, "PyObject_GetAttrString")) {
    return reinterpret_cast<void*>(Stub_PyObject_GetAttrString);
  }
  if (IsEqual(symbol, "PyObject_SetAttrString")) {
    return reinterpret_cast<void*>(Stub_PyObject_SetAttrString);
  }
  if (IsEqual(symbol, "PyTuple_SetItem")) {
    return reinterpret_cast<void*>(Stub_PyTuple_SetItem);
  }
  if (IsEqual(symbol, "PyObject_Print")) {
    return reinterpret_cast<void*>(Stub_PyObject_Print);
  }
  if (IsEqual(symbol, "PyImport_ImportModule")) {
    return reinterpret_cast<void*>(Stub_PyImport_ImportModule);
  }
  if (IsEqual(symbol, "PyCallable_Check")) {
    return reinterpret_cast<void*>(Stub_PyCallable_Check);
  }
  if (IsEqual(symbol, "PyDict_New")) {
    return reinterpret_cast<void*>(Stub_PyDict_New);
  }
  if (IsEqual(symbol, "PyDict_SetItemString")) {
    return reinterpret_cast<void*>(Stub_PyDict_SetItemString);
  }
  if (IsEqual(symbol, "PyDict_Next")) {
    return reinterpret_cast<void*>(Stub_PyDict_Next);
  }
  if (IsEqual(symbol, "PyDict_GetItemString")) {
    return reinterpret_cast<void*>(Stub_PyDict_GetItemString);
  }
  if (IsEqual(symbol, "PyList_New")) {
    return reinterpret_cast<void*>(Stub_PyList_New);
  }
  if (IsEqual(symbol, "PyList_SetItem")) {
    return reinterpret_cast<void*>(Stub_PyList_SetItem);
  }
  if (IsEqual(symbol, "PyArg_ParseTuple")) {
    return reinterpret_cast<void*>(Stub_PyArg_ParseTuple);
  }
  if (IsEqual(symbol, "PySequence_Check")) {
    return reinterpret_cast<void*>(Stub_PySequence_Check);
  }
  if (IsEqual(symbol, "PySequence_Size")) {
    return reinterpret_cast<void*>(Stub_PySequence_Size);
  }
  if (IsEqual(symbol, "PySequence_GetItem")) {
    return reinterpret_cast<void*>(Stub_PySequence_GetItem);
  }
  if (IsEqual(symbol, "PyErr_Fetch")) {
    return reinterpret_cast<void*>(Stub_PyErr_Fetch);
  }
  if (IsEqual(symbol, "PyErr_Clear")) {
    return reinterpret_cast<void*>(Stub_PyErr_Clear);
  }
  return nullptr;
}

#else  // defined(GDAL_PYTHON_TEST_USE_DIRECT_STUBS)

GDALPy::PyObject* Stub_Py_CompileString(const char* str, const char* filename,
                                        int start) {
  return Stub_Py_CompileStringExFlags(str, filename, start, nullptr, 0);
}

#endif  // !defined(GDAL_PYTHON_TEST_USE_DIRECT_STUBS)

TEST(GdalPythonTest, LoadPythonApiAndHelpers) {
  g_py_err_occurred = false;
  g_py_call_fail = false;
  g_py_compile_fail = false;
  g_py_unicode_fail = false;

#if !defined(GDAL_PYTHON_TEST_USE_DIRECT_STUBS)
  (void)GetRealDlopen();
  (void)GetRealDlclose();
  (void)GetRealDlsym();

  const std::filesystem::path tmp_dir =
      std::filesystem::path(::testing::TempDir()) / "gdal_py_test_bin";
  const char* old_path = std::getenv("PATH");
  const bool had_path = (old_path != nullptr);
  const std::string saved_path = had_path ? old_path : "";
  absl::Cleanup restore_env = [&]() {
    if (had_path) {
      setenv("PATH", saved_path.c_str(), 1);
    } else {
      unsetenv("PATH");
    }
    std::error_code ec;
    std::filesystem::remove_all(tmp_dir, ec);
  };

  std::error_code ec;
  std::filesystem::create_directories(tmp_dir, ec);
  const std::filesystem::path script_path = tmp_dir / "not_py_script";
  // Emit "3.99" so GDALPythonInitialize() probes libpython3.99.so(.1.0) and
  // libpython3.99m.so(.1.0), misses them in InterceptDlopen, and enters the
  // apszPythonSO fallback loop succeeding on libpython3.8.so.
  {
    std::FILE* fp = std::fopen(script_path.c_str(), "w");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp]() { std::fclose(fp); };
    std::fputs("#!/bin/sh\necho 3.99\n", fp);
  }
  std::filesystem::permissions(script_path, std::filesystem::perms::owner_all,
                               ec);
  const std::filesystem::path link_mid = tmp_dir / "python3_mid";
  const std::filesystem::path link_top = tmp_dir / "python3";
  std::filesystem::create_symlink(script_path, link_mid, ec);
  std::filesystem::create_symlink(link_mid, link_top, ec);

  std::string new_path = tmp_dir.string();
  if (!saved_path.empty()) {
    new_path += ":" + saved_path;
  }
  setenv("PATH", new_path.c_str(), 1);

  absl::Cleanup finalize_py = []() { GDALPythonFinalize(); };

  g_py_is_initialized = false;
  EXPECT_TRUE(GDALPythonInitialize());
  g_py_is_initialized = true;
  EXPECT_TRUE(GDALPythonInitialize());
  g_py_is_initialized = false;

  ASSERT_NE(nullptr, GDALPy::Py_CompileString);
  EXPECT_NE(nullptr,
            GDALPy::Py_CompileString("x = 1", "test.py", Py_file_input));

  {
    GDALPy::GIL_Holder exclusive(true);
    GDALPy::GIL_Holder non_exclusive(false);
  }
#else   // defined(GDAL_PYTHON_TEST_USE_DIRECT_STUBS)
  {
    CPLConfigOptionSetter py_so_setter("PYTHONSO", "/nonexistent/libpython.so",
                                       /*bSetOnlyIfUndefined=*/false);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(GDALPythonInitialize());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  GDALPy::Py_CompileString = Stub_Py_CompileString;
  GDALPy::PyUnicode_AsUTF8String = Stub_PyUnicode_AsUTF8String;
  GDALPy::PyBytes_AsString = Stub_PyBytes_AsString;
  GDALPy::Py_DecRef = Stub_Py_DecRef;
  GDALPy::Py_IncRef = Stub_Py_IncRef;
  GDALPy::PyErr_Occurred = Stub_PyErr_Occurred;
  GDALPy::PyErr_Print = Stub_PyErr_Print;
  GDALPy::PyErr_Fetch = Stub_PyErr_Fetch;
  GDALPy::PyErr_Clear = Stub_PyErr_Clear;
  GDALPy::PyImport_ExecCodeModule = Stub_PyImport_ExecCodeModule;
  GDALPy::PyObject_GetAttrString = Stub_PyObject_GetAttrString;
  GDALPy::PyTuple_New = Stub_PyTuple_New;
  GDALPy::PyTuple_SetItem = Stub_PyTuple_SetItem;
  GDALPy::PyObject_Call = Stub_PyObject_Call;

  absl::Cleanup reset_py_ptrs = []() {
    GDALPy::Py_CompileString = nullptr;
    GDALPy::PyUnicode_AsUTF8String = nullptr;
    GDALPy::PyBytes_AsString = nullptr;
    GDALPy::Py_DecRef = nullptr;
    GDALPy::Py_IncRef = nullptr;
    GDALPy::PyErr_Occurred = nullptr;
    GDALPy::PyErr_Print = nullptr;
    GDALPy::PyErr_Fetch = nullptr;
    GDALPy::PyErr_Clear = nullptr;
    GDALPy::PyImport_ExecCodeModule = nullptr;
    GDALPy::PyObject_GetAttrString = nullptr;
    GDALPy::PyTuple_New = nullptr;
    GDALPy::PyTuple_SetItem = nullptr;
    GDALPy::PyObject_Call = nullptr;
  };

  ASSERT_NE(nullptr, GDALPy::Py_CompileString);
  EXPECT_NE(nullptr,
            GDALPy::Py_CompileString("x = 1", "test.py", Py_file_input));
#endif  // !defined(GDAL_PYTHON_TEST_USE_DIRECT_STUBS)

  EXPECT_FALSE(GDALPy::ErrOccurredEmitCPLError());
  EXPECT_EQ("stub_py_str", GDALPy::GetString(&g_dummy_obj, true));

  g_py_unicode_fail = true;
  EXPECT_EQ("", GDALPy::GetString(&g_dummy_obj, true));
  g_py_unicode_fail = false;

  g_py_err_occurred = true;
  EXPECT_EQ("", GDALPy::GetString(&g_dummy_obj, false));
  g_py_err_occurred = true;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ("", GDALPy::GetString(&g_dummy_obj, true));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_STREQ("stub_py_str", CPLGetLastErrorMsg());
    CPLErrorReset();
  }

  g_py_err_occurred = true;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_TRUE(GDALPy::ErrOccurredEmitCPLError());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_STREQ("stub_py_str", CPLGetLastErrorMsg());
    CPLErrorReset();
  }

  g_py_err_occurred = true;
  g_py_call_fail = true;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_TRUE(GDALPy::ErrOccurredEmitCPLError());
    EXPECT_STREQ("An exception occurred in exception formatting code...",
                 CPLGetLastErrorMsg());
    CPLErrorReset();
  }
  g_py_call_fail = false;
  g_py_err_occurred = false;

  g_py_compile_fail = true;
  EXPECT_EQ("An exception occurred in exception formatting code...",
            GDALPy::GetPyExceptionString());
  g_py_compile_fail = false;
}

#if !defined(GDAL_PYTHON_TEST_USE_DIRECT_STUBS)
NO_SANITIZE void* InterceptDlopen(const char* filename, int flags) {
  if (Contains(filename, "3.99")) {
    return nullptr;
  }
  if (Contains(filename, "libpython3.") || Contains(filename, "libpython")) {
    return &g_fake_lib;
  }
  DlopenFn real_dlopen = GetRealDlopen();
  return real_dlopen != nullptr ? real_dlopen(filename, flags) : nullptr;
}

NO_SANITIZE int InterceptDlclose(void* handle) {
  if (handle == &g_fake_lib) {
    return 0;
  }
  DlcloseFn real_dlclose = GetRealDlclose();
  return real_dlclose != nullptr ? real_dlclose(handle) : -1;
}

NO_SANITIZE void* InterceptDlsym(void* handle, const char* symbol) {
  if (handle == &g_fake_lib && symbol != nullptr) {
    return LookupStubSymbol(symbol);
  }
  DlsymFn real_dlsym = GetRealDlsym();
  return real_dlsym != nullptr ? real_dlsym(handle, symbol) : nullptr;
}

extern "C" {

void* dlopen(const char* filename, int flags) NO_SANITIZE {
  return InterceptDlopen(filename, flags);
}

int dlclose(void* handle) NO_SANITIZE { return InterceptDlclose(handle); }

void* dlsym(void* handle, const char* symbol) NO_SANITIZE {
  return InterceptDlsym(handle, symbol);
}

}  // extern "C"

#undef NO_SANITIZE
#endif  // !defined(GDAL_PYTHON_TEST_USE_DIRECT_STUBS)

#ifdef GDAL_PYTHON_TEST_USE_DIRECT_STUBS
#undef GDAL_PYTHON_TEST_USE_DIRECT_STUBS
#endif

}  // namespace
}  // namespace autotest2

// TODO(schwehr): Cover GDALPyFunctionWrapper and Python object conversion
// helpers (GetLong, GetDouble, CreateDict, etc.) when additional callers exist.
