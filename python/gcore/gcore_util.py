# Copyright 2014 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Support for tests in gcore.

This supplements the helper code in gdrivers/gdrivers_util.py.
"""

import contextlib
import errno
import os
import shutil
import tempfile

from osgeo import gdal

import gflags as flags
import logging

FLAGS = flags.FLAGS


def IsGdal2():
  return (
      int(gdal.VersionInfo()) >= 2000000 and
      int(gdal.VersionInfo()) < 3000000)


def GetTestFilePath(filename):
  return os.path.join(
      FLAGS.test_srcdir,
      'autotest2/gcore/testdata',
             os.path.split(os.path.abspath(__file__))[0],
             'testdata'
      filename
      )


@contextlib.contextmanager
def ErrorHandler(error_name):
  handler = gdal.PushErrorHandler(error_name)
  try:
    yield handler
  finally:
    gdal.PopErrorHandler()


@contextlib.contextmanager
def TestTemporaryDirectory(keep=False, prefix=None):
  tempdir_base = gdal.GetConfigOption('TMPDIR', tempfile.gettempdir())
  prefix = prefix or 'GdalAutotest2'
  tempdir = tempfile.mkdtemp(prefix+'-', dir=tempdir_base)
  try:
    yield tempdir
  finally:
    if not keep:
      shutil.rmtree(tempdir)


@contextlib.contextmanager
def GdalUnlinkWhenDone(filepath):
  try:
    yield
  finally:
    gdal.Unlink(filepath)


def SetupTestEnv():
  """Test in a known environment.

  If GDAL_PAM_PROXY_DIR is not set, GDAL wants to write auxiliary files next
  to the original file.  That does not work in some testing environments.
  """
  gdal_tmpdir = os.path.join(FLAGS.test_tmpdir, 'gdal_tmp')
  gdal_pamdir = os.path.join(FLAGS.test_tmpdir, 'pam_tmp')

  for path in gdal_tmpdir, gdal_pamdir:
    try:
      os.mkdir(path)
    except OSError as exception:
      if exception.errno != errno.EEXIST:
        raise

  gdal.SetConfigOption('TMPDIR', gdal_tmpdir)
  gdal.SetConfigOption('CPL_TMPDIR', gdal_tmpdir)
  gdal.SetConfigOption('GDAL_PAM_PROXY_DIR', gdal_pamdir)
