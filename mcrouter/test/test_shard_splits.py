# Copyright (c) 2014, Facebook, Inc.
#  All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import time

from mcrouter.test.McrouterTestCase import McrouterTestCase
from mcrouter.test.MCProcess import Memcached

class TestShardSplits(McrouterTestCase):
    config = './mcrouter/test/test_shard_splits.json'
    extra_args = []

    def setUp(self):
        for i in range(7):
            self.add_server(Memcached())

    def test_shard_splits_basic(self):
        mcrouter = self.add_mcrouter(
            self.config,
            extra_args=self.extra_args)

        # Test set, get, direct get (by split shard id)
        self.assertTrue(mcrouter.set('a:1:blah', 'value'))
        time.sleep(0.1)
        self.assertEqual(mcrouter.get('a:1:blah'), 'value')
        self.assertEqual(mcrouter.get('a:1aa:blah'), 'value')
        self.assertEqual(mcrouter.get('a:1ba:blah'), 'value')
        self.assertIsNone(mcrouter.get('a:1ca:blah'))

        self.assertTrue(mcrouter.set('b:1:blah', 'value2'))
        time.sleep(0.1)
        self.assertEqual(mcrouter.get('b:1:blah'), 'value2')
        self.assertEqual(mcrouter.get('b:1aa:blah'), 'value2')
        self.assertIsNone(mcrouter.get('b:1ba:blah'))

    def test_shard_splits_update(self):
        mcrouter = self.add_mcrouter(
            self.config,
            extra_args=self.extra_args)

        # set & delete with splits
        self.assertTrue(mcrouter.set('a:1:foo', 'value'))
        time.sleep(0.1)
        self.assertEqual(mcrouter.get('a:1:foo'), 'value')
        self.assertEqual(mcrouter.get('a:1aa:foo'), 'value')
        self.assertEqual(mcrouter.get('a:1ba:foo'), 'value')

        self.assertTrue(mcrouter.delete('a:1:foo'))
        time.sleep(0.1)
        self.assertIsNone(mcrouter.get('a:1:foo'))
        self.assertIsNone(mcrouter.get('a:1aa:foo'))
        self.assertIsNone(mcrouter.get('a:1ba:foo'))

        # No splits
        self.assertTrue(mcrouter.set('a:5:bar', 'value2'))
        self.assertEqual(mcrouter.get('a:5:bar'), 'value2')
        self.assertIsNone(mcrouter.get('a:5aa:bar'))

        self.assertTrue(mcrouter.delete('a:5:bar'))
        self.assertIsNone(mcrouter.get('a:5:bar'))

        # Arithmetic operations with splits
        self.assertTrue(mcrouter.set('a:1:counter', '5'))
        time.sleep(0.1)
        self.assertEqual(mcrouter.incr('a:1:counter'), 6)
        time.sleep(0.1)
        self.assertEqual(mcrouter.get('a:1:counter'), '6')
        self.assertEqual(mcrouter.get('a:1aa:counter'), '6')
        self.assertEqual(mcrouter.get('a:1ba:counter'), '6')
        self.assertIsNone(mcrouter.get('a:1ca:counter'))

        self.assertEqual(mcrouter.decr('a:1:counter', 3), 3)
        time.sleep(0.1)
        self.assertEqual(mcrouter.get('a:1:counter'), '3')
        self.assertEqual(mcrouter.get('a:1aa:counter'), '3')
        self.assertEqual(mcrouter.get('a:1ba:counter'), '3')
        self.assertIsNone(mcrouter.get('a:1ca:counter'))

        # Arithmetic operations without splits
        self.assertTrue(mcrouter.set('a:125:counter', '125'))
        self.assertEqual(mcrouter.incr('a:125:counter'), 126)
        self.assertEqual(mcrouter.get('a:125:counter'), '126')
        self.assertIsNone(mcrouter.get('a:125aa:counter'))

        self.assertEqual(mcrouter.decr('a:125:counter', 124), 2)
        self.assertEqual(mcrouter.get('a:125:counter'), '2')
        self.assertIsNone(mcrouter.get('a:125aa:counter'))