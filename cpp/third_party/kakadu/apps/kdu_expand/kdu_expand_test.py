"""Tests for third_party kakadu kdu_expand binary."""

import os
import subprocess

from absl import flags

from google3.pyglib import resources
from google3.testing.pybase import googletest

FLAGS = flags.FLAGS


class KduExpandTest(googletest.TestCase):

  def setUp(self):
    super().setUp()
    self.bin = os.path.join(
        resources.GetARootDirWithAllResources(),
        'google3/third_party/kakadu/vendor/kdu_expand',
    )

  def testNoArgsHelp(self):
    cmd = [self.bin]
    try:
      result = subprocess.check_output(
          cmd, stderr=subprocess.STDOUT, encoding='utf8')
    except subprocess.CalledProcessError as e:
      result = e.output

    self.assertIn('print a brief usage statement', result)

    # Make sure something from kdu_compress is not in kdu_expand
    self.assertNotIn('Ssampling=', result)

    self.assertNotIn('accepted image file formats', result)

  def testUsage(self):
    cmd = [self.bin, '-usage']
    try:
      result = subprocess.check_output(
          cmd, stderr=subprocess.STDOUT, encoding='utf8')
    except subprocess.CalledProcessError as e:
      result = e.output

    self.assertIn('accepted image file formats', result)


# TODO(b/309473610): Add more tests

if __name__ == '__main__':
  googletest.main()
