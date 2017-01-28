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

"""Test handling of a driver that does not exist.

TODO(schwehr): Check exception handling and enable/disable.
TODO(schwehr): Seems like this belongs in gcore.
TODO(schwehr): Add code to gdal to see which available driver name is closest
  to the request driver for when a requested name does not match.
"""

import unittest

from autotest2.gdrivers import gdrivers_util


class BadNameTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    parent = super(BadNameTest, self)
    self.assertRaises(AssertionError, parent.setUp, 'bad driver', '.foo')

  def testTriggerDriverException(self):
    """Trigger the initial check for a driver in super setUp to fail."""
    pass


if __name__ == '__main__':
  unittest.main()
