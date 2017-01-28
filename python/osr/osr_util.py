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

"""Utilities for the osr test suite."""

from osgeo import gdal
from osgeo import osr
import logging


def HaveProj4():
  """Test if GDAL was built with PROJ.4."""

  utm_srs = osr.SpatialReference()
  utm_srs.SetUTM(11)
  utm_srs.SetWellKnownGeogCS('WGS84')

  ll_srs = osr.SpatialReference()
  ll_srs.SetWellKnownGeogCS('WGS84')

  # TODO(schwehr): Switch to a context manager for handler.
  gdal.PushErrorHandler('CPLQuietErrorHandler')
  try:
    ct = osr.CoordinateTransformation(ll_srs, utm_srs)
    if 'Unable to load PROJ.4' in gdal.GetLastErrorMsg():
      return False
  except ValueError as err:
    # TODO(schwehr): Why was a ValueError being specifically being trapped?
    logging.warn('Trapped ValueError: %s', err)
    return False
  finally:
    gdal.PopErrorHandler()

  if not ct or not ct.this:
    return False

  return True
