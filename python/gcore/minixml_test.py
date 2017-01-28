#!/usr/bin/env python
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
# Copyright (c) 2005, Frank Warmerdam <warmerdam@pobox.com>
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

"""Test GDALGCPsToGeoTransform function.

Rewrite of

http://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/minixml.py
"""

import unittest


from osgeo import gdal
import unittest
from autotest2.gdrivers import gdrivers_util


class MiniXml(unittest.TestCase):

  def testTopLevelOnly(self):
    tree = gdal.ParseXMLString('<TestDoc></TestDoc>')
    self.assertEqual(tree, [0, 'TestDoc'])

  def testWithText(self):
    tree = gdal.ParseXMLString('<TestDoc>abc123</TestDoc>')
    self.assertEqual(tree, [0, 'TestDoc', [1, 'abc123']])

  def testMiniXml01Simple(self):
    tree = gdal.ParseXMLString(
        '<TestDoc style="123"><sub1/><sub2>abc</sub2></TestDoc>'
    )
    self.assertEqual(
        tree,
        [0, 'TestDoc', [2, 'style', [1, '123']],
         [0, 'sub1'], [0, 'sub2', [1, 'abc']]]
    )
    self.assertEqual(tree[0], gdal.CXT_Element)
    node = tree[2]
    self.assertEqual(node[0], gdal.CXT_Attribute)

  def testMiniXml02Serialize(self):
    tree = [0, 'TestDoc', [2, 'style', [1, '123']],
            [0, 'sub1'], [0, 'sub2', [1, 'abc']]]
    expected_doc = ('\n'.join((
        '<TestDoc style="123">',
        '  <sub1 />',
        '  <sub2>abc</sub2>',
        '</TestDoc>\n'
    )))
    result_doc = gdal.SerializeXMLTree(tree)
    self.assertEqual(result_doc, expected_doc)

  def testMiniXml03DocTypeAndComments(self):
    xml = ('\n'.join((
        '<?xml version="1.0" encoding="ISO-8859-1"?>',
        '<!-- XML document to test reading complex DOCTYPE element. -->',
        '<!-- The XML document type contains or points to markup',
        '     declarations providing a grammar for a class of documents. -->',
        '<!DOCTYPE chapter [',
        '    <!ELEMENT chapter (title,para+)>',
        '    <!ELEMENT title (#PCDATA)>',
        '    <!ELEMENT para (#PCDATA)>',
        ']>',
        '<chapter><title>Chapter 1</title>',
        '  <para>1st paragraph</para>',
        '  <para>Second paragraph</para>',
        '  <para>3rd</para>',
        '  <para>4th</para>',
        '</chapter>'
    )))

    expected = [
        0,
        '',
        [0, '?xml',
         [2, 'version', [1, '1.0']],
         [2, 'encoding', [1, 'ISO-8859-1']]],
        [3, ' XML document to test reading complex DOCTYPE element. '],
        [3,
         ' The XML document type contains or points to markup\n'
         '     declarations providing a grammar for a class of documents. '],
        [4,
         '<!DOCTYPE chapter [\n    <!ELEMENT chapter (title,para+)>\n'
         '    <!ELEMENT title (#PCDATA)>\n    <!ELEMENT para (#PCDATA)>\n]>'],
        [0,
         'chapter',
         [0, 'title', [1, 'Chapter 1']],
         [0, 'para', [1, '1st paragraph']],
         [0, 'para', [1, 'Second paragraph']],
         [0, 'para', [1, '3rd']],
         [0, 'para', [1, '4th']]]
    ]
    self.assertEqual(gdal.ParseXMLString(xml), expected)

  def testMiniXml04Prolog(self):
    xml = '<?xml encoding="utf-8"?>\n<foo />\n'
    tree = gdal.ParseXMLString(xml)
    self.assertEqual(
        tree,
        [0, '', [0, '?xml', [2, 'encoding', [1, 'utf-8']]], [0, 'foo']]
    )
    self.assertEqual(gdal.SerializeXMLTree(tree), xml)


if __name__ == '__main__':
  unittest.main()
