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
#
# This is a complete rewrite of a file licensed as follows:
#
# Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

"""Support for testing jpeg2000 drivers.

Rewrite of deregister_all_jpeg2000_drivers_but:

http://trac.osgeo.org/gdal/browser/trunk/autotest/pymod/gdaltest.py#L1125
"""

import contextlib


from osgeo import gdal
import logging
from autotest2.gdrivers import gdrivers_util

JP2K_DRIVERS = (
    gdrivers_util.JPEG2000_DRIVER,
    gdrivers_util.JP2ECW_DRIVER,
    gdrivers_util.JP2KAK_DRIVER,
    gdrivers_util.JP2MRSID,
    gdrivers_util.JP2OPENJPEG
)


@contextlib.contextmanager
def DeregisterJpeg2000(skip=None):
  """Context manager to temporarily disable gdal jpeg 2000 drivers.

  Args:
    skip: list of drivers to keep registered within this context.

  Yields:
    List of driver names that were removed in this context.
  """
  drivers = list(JP2K_DRIVERS)
  if isinstance(skip, str) or isinstance(skip, unicode):
    skip = [skip]
  for driver_name in skip:
    drivers.remove(driver_name)

  unregistered = []
  for driver_name in drivers:
    drv = gdal.GetDriverByName(driver_name)
    if drv:
      logging.info('Deregistering %s', driver_name)
      try:
        drv.Deregister()
        unregistered.append(driver_name)
      # pylint:disable=broad-except
      except Exception as error:
        logging.error('Unable to deregister %s: %s', driver_name, error)

  yield unregistered

  for driver_name in unregistered:
    drv = gdal.GetDriverByName(driver_name)
    try:
      drv.Register()
    # pylint:disable=broad-except
    except Exception as error:
      logging.error('Unable to re-register %s: %s', driver_name, error)
