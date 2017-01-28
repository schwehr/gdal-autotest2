#!/usr/bin/env python
# Copyright 2017 Google Inc. All Rights Reserved.
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

"""Tests for osr_util.py."""

import os
import unittest

from gdal.autotest2.osr import osr_util


class OsrUtilTest(unittest.TestCase):

  def setUp(self):
    super(OsrUtilTest, self).setUp()

  def testHaveProj4(self):
    # The world without Proj.4 is just not worth dealing with.
    self.assertTrue(osr_util.HaveProj4())


if __name__ == '__main__':
  unittest.main()
