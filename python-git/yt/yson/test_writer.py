# -*- coding: utf-8 -*-

from __future__ import absolute_import

import unittest

import yt.yson.writer
from yt.yson import YsonUint64, YsonInt64, YsonEntity

try:
    import yt_yson_bindings
except ImportError:
    yt_yson_bindings = None


class YsonWriterTestBase(object):
    @staticmethod
    def dumps(*args, **kws):
        raise NotImplementedError

    def test_slash(self):
        self.assertEqual(
            self.dumps({"key": "1\\"}, yson_format="text"),
            '{"key"="1\\\\";}',
        )

    def test_boolean(self):
        self.assertEqual(self.dumps(False, boolean_as_string=True), '"false"')
        self.assertEqual(self.dumps(True, boolean_as_string=True), '"true"')
        self.assertEqual(self.dumps(False, boolean_as_string=False), "%false")
        self.assertEqual(self.dumps(True, boolean_as_string=False), "%true")

    def test_long_integers(self):
        value = 2 ** 63
        self.assertEqual('%su' % str(value), self.dumps(value))

        value = 2 ** 63 - 1
        self.assertEqual('%s' % str(value), self.dumps(value))
        self.assertEqual('%su' % str(value), self.dumps(YsonUint64(value)))

        value = -2 ** 63
        self.assertEqual('%s' % str(value), self.dumps(value))

        self.assertRaises(Exception, lambda: self.dumps(2 ** 64))
        self.assertRaises(Exception, lambda: self.dumps(-2 ** 63 - 1))
        self.assertRaises(Exception, lambda: self.dumps(YsonUint64(-2 ** 63)))
        self.assertRaises(Exception, lambda: self.dumps(YsonInt64(2 ** 63 + 1)))

    def test_list_fragment_text(self):
        self.assertEqual(
            '"a";\n"b";\n"c";\n42;\n',
            self.dumps(
                ["a", "b", "c", 42],
                yson_format="text",
                yson_type="list_fragment"
            )
        )

    def test_map_fragment_text(self):
        self.assertEqual(
            '"a"="b";\n"c"="d";\n',
            self.dumps(
                {"a": "b", "c": "d"},
                yson_format="text",
                yson_type="map_fragment"
            )
        )

    def test_list_fragment_pretty(self):
        self.assertEqual(
            '"a";\n"b";\n"c";\n42;\n',
            self.dumps(
                ["a", "b", "c", 42],
                yson_format="pretty",
                yson_type="list_fragment"
            )
        )

    def test_map_fragment_pretty(self):
        self.assertEqual(
            '"a" = "b";\n"c" = "d";\n',
            self.dumps(
                {"a": "b", "c": "d"},
                yson_format="pretty",
                yson_type="map_fragment"
            )
        )

    def test_invalid_attributes(self):
        obj = YsonEntity()

        obj.attributes = None
        self.assertEqual("#", self.dumps(obj))

        obj.attributes = []
        self.assertRaises(Exception, lambda: self.dumps(obj))


class TestWriterDefault(unittest.TestCase, YsonWriterTestBase):
    @staticmethod
    def dumps(*args, **kws):
        return yt.yson.dumps(*args, **kws)


class TestWriterPython(unittest.TestCase, YsonWriterTestBase):
    @staticmethod
    def dumps(*args, **kws):
        return yt.yson.writer.dumps(*args, **kws)


if yt_yson_bindings:
    class TestWriterBindings(unittest.TestCase, YsonWriterTestBase):
        @staticmethod
        def dumps(*args, **kws):
            return yt_yson_bindings.dumps(*args, **kws)
