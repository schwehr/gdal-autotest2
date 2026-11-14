# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Helpers for GDAL new CLI ('gdal') Python autotest2 tests."""

from collections.abc import Iterator, Mapping, Sequence
import contextlib
import os
import subprocess
from typing import Any

from osgeo import gdal

from google3.pyglib import resources
from google3.third_party.gdal.autotest2.python.gcore import gcore_util

_UTILITIES_TESTDATA = (
    "google3/third_party/gdal/autotest2/python/utilities/testdata"
)
_GCORE_TESTDATA = "google3/third_party/gdal/autotest2/python/gcore/testdata"
_GDRIVERS_TESTDATA = (
    "google3/third_party/gdal/autotest2/python/gdrivers/testdata"
)
_OGR_TESTDATA = "google3/third_party/gdal/autotest2/python/ogr/testdata"


def setup_test_env() -> None:
  """Initializes GDAL environment inside setUpModule()."""
  gcore_util.SetupTestEnv()
  gdal.SetConfigOption("GDAL_PATH", os.path.dirname(get_gdal_path()))
  gdal.UseExceptions()


def get_gdal_path() -> str:
  """Returns the path to the Blaze-built `gdal` CLI binary."""
  return os.path.join(
      resources.GetARootDirWithAllResources(),
      "google3/third_party/gdal/apps/gdal",
  )


def run_gdal_cli(
    args: Sequence[str],
    env: Mapping[str, str] | None = None,
    stdin_text: str | None = None,
) -> tuple[str, str, int]:
  """Runs the `gdal` CLI binary and returns `(stdout, stderr, returncode)`."""
  proc_env = os.environ.copy()
  if env:
    proc_env.update(env)
  res = subprocess.run(
      [get_gdal_path(), *args],
      capture_output=True,
      text=True,
      input=stdin_text,
      env=proc_env,
      check=False,
  )
  return res.stdout, res.stderr, res.returncode


def run_gdal_cli_bytes(
    args: Sequence[str],
    env: Mapping[str, str] | None = None,
    stdin_bytes: bytes | None = None,
) -> tuple[bytes, bytes, int]:
  """Runs `gdal` CLI in binary mode and returns `(stdout, stderr, code)`."""
  proc_env = os.environ.copy()
  if env:
    proc_env.update(env)
  res = subprocess.run(
      [get_gdal_path(), *args],
      capture_output=True,
      input=stdin_bytes,
      env=proc_env,
      check=False,
  )
  return res.stdout, res.stderr, res.returncode


def get_testdata_path(upstream_rel_path: str) -> str:
  """Resolves an upstream `autotest/utilities` relative path to runfiles."""
  root = resources.GetARootDirWithAllResources()
  if upstream_rel_path.startswith("data/"):
    rel = upstream_rel_path[len("data/") :]
    return os.path.join(root, _UTILITIES_TESTDATA, rel)

  for prefix, sub, fallback in (
      ("../gcore/data/", "gcore", _GCORE_TESTDATA),
      ("../gdrivers/data/", "gdrivers", _GDRIVERS_TESTDATA),
      ("../ogr/data/", "ogr", _OGR_TESTDATA),
  ):
    if upstream_rel_path.startswith(prefix):
      rel = upstream_rel_path[len(prefix) :]
      local_cand = os.path.join(root, _UTILITIES_TESTDATA, sub, rel)
      if os.path.exists(local_cand):
        return local_cand
      return os.path.join(root, fallback, rel)

  return os.path.join(root, _UTILITIES_TESTDATA, upstream_rel_path)


def has_driver(name: str) -> bool:
  """Returns whether the given GDAL/OGR driver is built in."""
  return gdal.GetDriverByName(name) is not None


def has_curl() -> bool:
  """Returns whether libcurl/HTTP support is enabled."""
  return gdal.GetDriverByName("HTTP") is not None


def get_arg_dataset(alg: Any, name: str = "output") -> gdal.Dataset:
  """Returns the gdal.Dataset backing a GDALAlgorithm dataset argument."""
  return alg[name].GetDataset()


def get_arg_name(alg: Any, name: str = "output") -> str:
  """Returns the dataset name backing a GDALAlgorithm dataset argument."""
  return alg[name].GetName()


@contextlib.contextmanager
def vsimem_dir(name: str) -> Iterator[str]:
  """Creates and cleans up a `/vsimem/<name>` directory."""
  path = f"/vsimem/{name}"
  gdal.Mkdir(path, 0o755)
  try:
    yield path
  finally:
    gdal.RmdirRecursive(path)
