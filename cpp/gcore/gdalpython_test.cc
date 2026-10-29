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

#include "third_party/gdal/gcore/gdalpython.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"

namespace gdal {
namespace {

using GDALPy::Py_buffer;
using GDALPy::Py_ssize_t;
using GDALPy::PyGILState_STATE;
using GDALPy::PyModuleDef;
using GDALPy::PyObject;

static int g_dlsym_calls = 0;
static int g_dlopen_calls = 0;
static int g_py_is_initialized = 0;
static bool g_py_err_occurred = false;
static bool g_py_compile_fail = false;
static bool g_py_call_fail = false;
static bool g_py_with_traceback = false;
static PyObject g_dummy_obj = {1, nullptr};
static PyObject g_err_obj = {1, nullptr};

const char* Stub_Py_GetVersion() { return "3.11.0\r\n(mock)\n"; }
void Stub_Py_SetProgramName(const wchar_t*) {}
void Stub_Py_SetPythonHome(const wchar_t*) {}
int Stub_PyBuffer_FillInfo(Py_buffer*, PyObject*, void*, size_t, int, int) {
  return 0;
}
PyObject* Stub_PyMemoryView_FromBuffer(Py_buffer*) { return &g_dummy_obj; }
PyObject* Stub_PyObject_Type(PyObject*) { return &g_dummy_obj; }
int Stub_PyObject_IsInstance(PyObject*, PyObject*) { return 1; }
PyObject* Stub_PyTuple_New(size_t) { return &g_dummy_obj; }
PyObject* Stub_PyBool_FromLong(long) { return &g_dummy_obj; }
PyObject* Stub_PyLong_FromLong(long) { return &g_dummy_obj; }
long Stub_PyLong_AsLong(PyObject*) { return 0; }
PyObject* Stub_PyLong_FromLongLong(GIntBig) { return &g_dummy_obj; }
GIntBig Stub_PyLong_AsLongLong(PyObject*) { return 0; }
Py_ssize_t Stub_PyBytes_Size(PyObject*) { return 4; }
const char* Stub_PyBytes_AsString(PyObject*) { return "mock_str"; }
int* Stub_PyBytes_AsStringAndSize(PyObject*, char**, Py_ssize_t*) {
  return nullptr;
}
PyObject* Stub_PyBytes_FromObject(PyObject*) { return &g_dummy_obj; }
PyObject* Stub_PyBytes_FromStringAndSize(const void*, size_t) {
  return &g_dummy_obj;
}
PyObject* Stub_PyModule_Create2(PyModuleDef*, int) { return &g_dummy_obj; }
PyObject* Stub_PyUnicode_FromString(const char*) { return &g_dummy_obj; }
PyObject* Stub_PyUnicode_AsUTF8String(PyObject*) { return &g_dummy_obj; }
PyObject* Stub_PyFloat_FromDouble(double) { return &g_dummy_obj; }
double Stub_PyFloat_AsDouble(PyObject*) { return 0.0; }
PyObject* Stub_PyObject_Call(PyObject*, PyObject*, PyObject*) {
  if (g_py_call_fail) {
    g_py_err_occurred = true;
  } else {
    g_py_err_occurred = false;
  }
  return &g_dummy_obj;
}
PyObject* Stub_PyObject_GetIter(PyObject*) { return &g_dummy_obj; }
PyObject* Stub_PyIter_Next(PyObject*) { return nullptr; }
void Stub_Py_IncRef(PyObject*) {}
void Stub_Py_DecRef(PyObject*) {}
PyObject* Stub_PyErr_Occurred() {
  return g_py_err_occurred ? &g_err_obj : nullptr;
}
void Stub_PyErr_Print() { g_py_err_occurred = false; }
int Stub_Py_IsInitialized() { return g_py_is_initialized; }
void Stub_Py_InitializeEx(int) { g_py_is_initialized = 1; }
void Stub_PyEval_InitThreads() {}
void* Stub_PyEval_SaveThread() { return &g_dummy_obj; }
void Stub_PyEval_RestoreThread(void*) {}
void Stub_Py_Finalize() { g_py_is_initialized = 0; }
PyObject* Stub_Py_CompileStringExFlags(const char*, const char*, int, void*,
                                       int) {
  if (g_py_compile_fail) {
    g_py_err_occurred = true;
    return nullptr;
  }
  g_py_err_occurred = false;
  return &g_dummy_obj;
}
PyObject* Stub_PyImport_ExecCodeModule(const char*, PyObject*) {
  return &g_dummy_obj;
}
int Stub_PyObject_HasAttrString(PyObject*, const char*) { return 1; }
PyObject* Stub_PyObject_GetAttrString(PyObject*, const char*) {
  return &g_dummy_obj;
}
int Stub_PyObject_SetAttrString(PyObject*, const char*, PyObject*) { return 0; }
int Stub_PyTuple_SetItem(PyObject*, size_t, PyObject*) { return 0; }
void Stub_PyObject_Print(PyObject*, FILE*, int) {}
PyObject* Stub_PyImport_ImportModule(const char*) { return &g_dummy_obj; }
int Stub_PyCallable_Check(PyObject*) { return 1; }
PyObject* Stub_PyDict_New() { return &g_dummy_obj; }
int Stub_PyDict_SetItemString(PyObject*, const char*, PyObject*) { return 0; }
int Stub_PyDict_Next(PyObject*, size_t*, PyObject**, PyObject**) { return 0; }
PyObject* Stub_PyDict_GetItemString(PyObject*, const char*) {
  return &g_dummy_obj;
}
PyObject* Stub_PyList_New(Py_ssize_t) { return &g_dummy_obj; }
int Stub_PyList_SetItem(PyObject*, Py_ssize_t, PyObject*) { return 0; }
int Stub_PySequence_Check(PyObject*) { return 1; }
Py_ssize_t Stub_PySequence_Size(PyObject*) { return 0; }
PyObject* Stub_PySequence_GetItem(PyObject*, Py_ssize_t) {
  return &g_dummy_obj;
}
int Stub_PyArg_ParseTuple(PyObject*, const char*, ...) { return 1; }
PyGILState_STATE Stub_PyGILState_Ensure() { return 1; }
void Stub_PyGILState_Release(PyGILState_STATE) {}
void Stub_PyErr_Fetch(PyObject** poPyType, PyObject** poPyValue,
                      PyObject** poPyTraceback) {
  *poPyType = &g_dummy_obj;
  *poPyValue = &g_dummy_obj;
  *poPyTraceback = g_py_with_traceback ? &g_dummy_obj : nullptr;
  g_py_err_occurred = false;
}
void Stub_PyErr_Clear() { g_py_err_occurred = false; }

#define NO_SANITIZE __attribute__((no_sanitize("address", "memory")))

NO_SANITIZE static bool is_equal(const char* a, const char* b) {
  if (!a || !b) return a == b;
  while (*a && *b && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

NO_SANITIZE static bool contains(const char* haystack, const char* needle) {
  if (!haystack || !needle) return false;
  for (const char* h = haystack; *h; ++h) {
    const char* h_sub = h;
    const char* n = needle;
    while (*h_sub && *n && *h_sub == *n) {
      ++h_sub;
      ++n;
    }
    if (!*n) return true;
  }
  return false;
}

static void* g_py_handle = nullptr;
static void* g_self_handle = nullptr;
static bool g_py_opened = false;

NO_SANITIZE void* MyDlopen(const char* filename, int flags) {
  auto real_dlopen = reinterpret_cast<void* (*)(const char*, int)>(
      dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.2.5"));
  if (filename == nullptr) {
    gdal::g_dlopen_calls++;
    if (real_dlopen) {
      g_self_handle = real_dlopen(nullptr, flags);
      return g_self_handle;
    }
    return nullptr;
  }
  if (gdal::contains(filename, "libpython3.14.so.1.0")) {
    gdal::g_dlopen_calls++;
    g_py_opened = true;
    if (real_dlopen) {
      g_py_handle = real_dlopen(nullptr, flags);
      return g_py_handle;
    }
    return nullptr;
  }
  if (real_dlopen) {
    return real_dlopen(filename, flags);
  }
  return nullptr;
}

NO_SANITIZE int MyDlclose(void* handle) {
  auto real_dlclose = reinterpret_cast<int (*)(void*)>(
      dlvsym(RTLD_NEXT, "dlclose", "GLIBC_2.2.5"));
  if (real_dlclose) {
    return real_dlclose(handle);
  }
  return 0;
}

}  // namespace
}  // namespace gdal

extern "C" NO_SANITIZE void* dlopen(const char* filename, int flags) {
  return gdal::MyDlopen(filename, flags);
}

extern "C" NO_SANITIZE int dlclose(void* handle) {
  return gdal::MyDlclose(handle);
}

extern "C" NO_SANITIZE void* dlsym(void* handle, const char* symbol) {
  if (!gdal::g_py_opened || handle != gdal::g_py_handle) {
    if (gdal::is_equal(symbol, "dlopen")) {
      return reinterpret_cast<void*>(&gdal::MyDlopen);
    }
    if (gdal::is_equal(symbol, "dlclose")) {
      return reinterpret_cast<void*>(&gdal::MyDlclose);
    }
    if (!gdal::g_py_opened && handle == gdal::g_self_handle &&
        gdal::is_equal(symbol, "Py_SetProgramName")) {
      return nullptr;
    }
    auto real_dlsym = reinterpret_cast<void* (*)(void*, const char*)>(
        dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5"));
    if (real_dlsym) {
      return real_dlsym(handle, symbol);
    }
    return nullptr;
  }
  gdal::g_dlsym_calls++;
#define MATCH_SYM(name, fn) \
  if (gdal::is_equal(symbol, name)) return reinterpret_cast<void*>(&gdal::fn)
  MATCH_SYM("Py_GetVersion", Stub_Py_GetVersion);
  MATCH_SYM("Py_SetProgramName", Stub_Py_SetProgramName);
  MATCH_SYM("Py_SetPythonHome", Stub_Py_SetPythonHome);
  MATCH_SYM("PyBuffer_FillInfo", Stub_PyBuffer_FillInfo);
  MATCH_SYM("PyMemoryView_FromBuffer", Stub_PyMemoryView_FromBuffer);
  MATCH_SYM("PyObject_Type", Stub_PyObject_Type);
  MATCH_SYM("PyObject_IsInstance", Stub_PyObject_IsInstance);
  MATCH_SYM("PyTuple_New", Stub_PyTuple_New);
  MATCH_SYM("PyBool_FromLong", Stub_PyBool_FromLong);
  MATCH_SYM("PyLong_FromLong", Stub_PyLong_FromLong);
  MATCH_SYM("PyLong_AsLong", Stub_PyLong_AsLong);
  MATCH_SYM("PyLong_FromLongLong", Stub_PyLong_FromLongLong);
  MATCH_SYM("PyLong_AsLongLong", Stub_PyLong_AsLongLong);
  MATCH_SYM("PyBytes_Size", Stub_PyBytes_Size);
  MATCH_SYM("PyBytes_AsString", Stub_PyBytes_AsString);
  MATCH_SYM("PyBytes_AsStringAndSize", Stub_PyBytes_AsStringAndSize);
  MATCH_SYM("PyBytes_FromObject", Stub_PyBytes_FromObject);
  MATCH_SYM("PyBytes_FromStringAndSize", Stub_PyBytes_FromStringAndSize);
  MATCH_SYM("PyModule_Create2", Stub_PyModule_Create2);
  MATCH_SYM("PyUnicodeUCS4_FromString", Stub_PyUnicode_FromString);
  MATCH_SYM("PyUnicodeUCS4_AsUTF8String", Stub_PyUnicode_AsUTF8String);
  MATCH_SYM("PyFloat_FromDouble", Stub_PyFloat_FromDouble);
  MATCH_SYM("PyFloat_AsDouble", Stub_PyFloat_AsDouble);
  MATCH_SYM("PyObject_Call", Stub_PyObject_Call);
  MATCH_SYM("PyObject_GetIter", Stub_PyObject_GetIter);
  MATCH_SYM("PyIter_Next", Stub_PyIter_Next);
  MATCH_SYM("Py_IncRef", Stub_Py_IncRef);
  MATCH_SYM("Py_DecRef", Stub_Py_DecRef);
  MATCH_SYM("PyErr_Occurred", Stub_PyErr_Occurred);
  MATCH_SYM("PyErr_Print", Stub_PyErr_Print);
  MATCH_SYM("Py_IsInitialized", Stub_Py_IsInitialized);
  MATCH_SYM("Py_InitializeEx", Stub_Py_InitializeEx);
  MATCH_SYM("PyEval_InitThreads", Stub_PyEval_InitThreads);
  MATCH_SYM("PyEval_SaveThread", Stub_PyEval_SaveThread);
  MATCH_SYM("PyEval_RestoreThread", Stub_PyEval_RestoreThread);
  MATCH_SYM("Py_Finalize", Stub_Py_Finalize);
  MATCH_SYM("Py_CompileStringExFlags", Stub_Py_CompileStringExFlags);
  MATCH_SYM("PyImport_ExecCodeModule", Stub_PyImport_ExecCodeModule);
  MATCH_SYM("PyObject_HasAttrString", Stub_PyObject_HasAttrString);
  MATCH_SYM("PyObject_GetAttrString", Stub_PyObject_GetAttrString);
  MATCH_SYM("PyObject_SetAttrString", Stub_PyObject_SetAttrString);
  MATCH_SYM("PyTuple_SetItem", Stub_PyTuple_SetItem);
  MATCH_SYM("PyObject_Print", Stub_PyObject_Print);
  MATCH_SYM("PyImport_ImportModule", Stub_PyImport_ImportModule);
  MATCH_SYM("PyCallable_Check", Stub_PyCallable_Check);
  MATCH_SYM("PyDict_New", Stub_PyDict_New);
  MATCH_SYM("PyDict_SetItemString", Stub_PyDict_SetItemString);
  MATCH_SYM("PyDict_Next", Stub_PyDict_Next);
  MATCH_SYM("PyDict_GetItemString", Stub_PyDict_GetItemString);
  MATCH_SYM("PyList_New", Stub_PyList_New);
  MATCH_SYM("PyList_SetItem", Stub_PyList_SetItem);
  MATCH_SYM("PySequence_Check", Stub_PySequence_Check);
  MATCH_SYM("PySequence_Size", Stub_PySequence_Size);
  MATCH_SYM("PySequence_GetItem", Stub_PySequence_GetItem);
  MATCH_SYM("PyArg_ParseTuple", Stub_PyArg_ParseTuple);
  MATCH_SYM("PyGILState_Ensure", Stub_PyGILState_Ensure);
  MATCH_SYM("PyGILState_Release", Stub_PyGILState_Release);
  MATCH_SYM("PyErr_Fetch", Stub_PyErr_Fetch);
  MATCH_SYM("PyErr_Clear", Stub_PyErr_Clear);
#undef MATCH_SYM
  return nullptr;
}

namespace gdal {
namespace {

TEST(GdalPythonTest, LoadPythonApiAndHelpers) {
  // CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // Set up two directories in PATH:
  // - dir1 has chained symlinks python3 -> python3_mid -> not_py_script (which
  // is a shell script printing "3.99")
  //   so on iTry=0:
  //   1) python3 is a symlink to python3_mid (relative, another symlink) ->
  //   follows loop! 2) python3_mid is a symlink to not_py_script (does not
  //   start with "python", so osVersion stays empty!) 3) Because osVersion is
  //   empty, it falls through to the `if (osVersion.empty())` CPLSpawn branch!
  //   4) CPLSpawn runs python3 (which resolves to not_py_script) and prints
  //   "3.99\n", setting osVersion = "3.99"! 5) Then it tries
  //   dlopen("libpython3.99.so") and "libpython3.99m.so" (which return
  //   nullptr),
  //      and then falls through to the apszPythonSO loop where
  //      "libpython3.14.so.1.0" succeeds!
  std::string tmp_dir = testing::TempDir() + "/gdal_py_test_bin";
  mkdir(tmp_dir.c_str(), 0755);
  std::string script_path = tmp_dir + "/not_py_script";
  std::string py_mid = tmp_dir + "/python3_mid";
  std::string py3 = tmp_dir + "/python3";
  FILE* fp = fopen(script_path.c_str(), "w");
  ASSERT_NE(fp, nullptr);
  fprintf(fp, "#!/bin/sh\necho 3.99\n");
  fclose(fp);
  chmod(script_path.c_str(), 0755);

  unlink(py_mid.c_str());
  unlink(py3.c_str());
  EXPECT_EQ(symlink("not_py_script", py_mid.c_str()), 0);
  EXPECT_EQ(symlink("python3_mid", py3.c_str()), 0);

  const char* orig_path = getenv("PATH");
  std::string saved_path = orig_path ? orig_path : "";
  setenv("PATH", tmp_dir.c_str(), 1);

  EXPECT_TRUE(GDALPythonInitialize());
  // Calling again hits the nInit >= 0 and bIsInitialized == 1 fast paths
  EXPECT_TRUE(GDALPythonInitialize());

  unlink(py3.c_str());
  unlink(py_mid.c_str());
  unlink(script_path.c_str());
  rmdir(tmp_dir.c_str());
  setenv("PATH", saved_path.c_str(), 1);

  // Exercise GDAL_Py_CompileString wrapper
  ASSERT_NE(GDALPy::Py_CompileString, nullptr);
  EXPECT_NE(GDALPy::Py_CompileString("x = 1", "test.py", Py_file_input),
            nullptr);

  // Exercise GIL_Holder (both exclusive and non-exclusive)
  {
    GDALPy::GIL_Holder gil1(true);
    GDALPy::GIL_Holder gil2(false);
  }

  // Exercise GetString, GetPyExceptionString, and ErrOccurredEmitCPLError
  g_py_err_occurred = false;
  EXPECT_FALSE(GDALPy::ErrOccurredEmitCPLError());
  EXPECT_STREQ(GDALPy::GetString(&g_dummy_obj, true).c_str(), "mock_str");

  // GetString when PyErr_Occurred() is true (with bEmitError = false and true)
  g_py_err_occurred = true;
  EXPECT_STREQ(GDALPy::GetString(&g_dummy_obj, false).c_str(), "");
  g_py_err_occurred = true;
  EXPECT_STREQ(GDALPy::GetString(&g_dummy_obj, true).c_str(), "");

  // ErrOccurredEmitCPLError with traceback = false and true
  g_py_err_occurred = true;
  g_py_with_traceback = false;
  EXPECT_TRUE(GDALPy::ErrOccurredEmitCPLError());

  g_py_err_occurred = true;
  g_py_with_traceback = true;
  EXPECT_TRUE(GDALPy::ErrOccurredEmitCPLError());

  // GetPyExceptionString when PyObject_Call fails
  g_py_call_fail = true;
  EXPECT_THAT(GDALPy::GetPyExceptionString().c_str(),
              testing::HasSubstr("exception occurred"));
  g_py_call_fail = false;

  // GetPyExceptionString when Py_CompileString fails
  g_py_compile_fail = true;
  EXPECT_THAT(GDALPy::GetPyExceptionString().c_str(),
              testing::HasSubstr("exception occurred"));
  g_py_compile_fail = false;

  // Finalize Python
  GDALPythonFinalize();
  // Second call is a no-op
  GDALPythonFinalize();
}

}  // namespace
}  // namespace gdal
