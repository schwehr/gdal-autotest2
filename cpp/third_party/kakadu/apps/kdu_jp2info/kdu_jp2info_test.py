"""Tests for third_party kakadu kdu_jp2info binary."""

import os
import subprocess

from absl import flags

from google3.pyglib import resources
from google3.testing.pybase import googletest

FLAGS = flags.FLAGS


class KduJp2infoTest(googletest.TestCase):

  def setUp(self):
    super().setUp()
    resource_dir = resources.GetARootDirWithAllResources()
    base_dir = os.path.join(resource_dir, 'google3/third_party/kakadu')
    self.bin = os.path.join(base_dir, 'vendor/kdu_jp2info')
    self.testdata_dir = os.path.join(
        base_dir, 'google/tests/apps/kdu_jp2info/testdata'
    )

  def testNoArgsHelp(self):
    cmd = [self.bin]
    try:
      result = subprocess.check_output(
          cmd, stderr=subprocess.STDOUT, encoding='utf8')
    except subprocess.CalledProcessError as e:
      result = e.output

    self.assertIn('print a brief usage statement', result)
    self.assertIn('hex <max len>', result)
    self.assertNotIn('Must be a JP2-family file', result)

  def testUsage(self):
    cmd = [self.bin, '-usage']
    try:
      result = subprocess.check_output(
          cmd, stderr=subprocess.STDOUT, encoding='utf8')
    except subprocess.CalledProcessError as e:
      result = e.output

    self.assertIn('Must be a JP2-family file', result)

  def testByteImage(self):
    test_file = os.path.join(self.testdata_dir, 'byte_without_geotransform.jp2')
    cmd = [self.bin, '-i', test_file]
    result = subprocess.check_output(
        cmd, stderr=subprocess.STDOUT, encoding='utf8')
    self.assertIn('<width> 20 </width>', result)


# TODO(b/309473610): Add more tests

if __name__ == '__main__':
  googletest.main()
