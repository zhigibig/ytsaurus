#!/usr/bin/python

import yt.wrapper.config as config
import yt.wrapper as yt
from yt.wrapper import Record, YtError, record_to_line, line_to_record, Table
from yt.common import flatten

from yt.environment import YTEnv

import os
import logging
import random
import string
import subprocess
from itertools import imap, izip, starmap, chain
from functools import partial

import unittest

LOCATION = os.path.dirname(os.path.abspath(__file__))
def abspath(path):
    return os.path.join(LOCATION, path)

TEST_DIR = "//home/tests"

LOCATION = os.path.dirname(os.path.abspath(__file__))

class YtTest(YTEnv):
    NUM_MASTERS = 1
    NUM_NODES = 5
    START_SCHEDULER = True
    START_PROXY = True

    @classmethod
    def setUpClass(cls):
        if os.path.exists("test.log"):
            os.remove("test.log")
        logging.basicConfig(level=logging.WARNING)

        ports = {
            "master": 18001,
            "node": 17001,
            "scheduler": 18101,
            "proxy": 18080}
        # (TODO): remake this strange stuff.
        cls.env = cls()
        cls.env.set_environment("tests/sandbox", "tests/sandbox/pids.txt", ports)

        config.PROXY = "localhost:%d" % ports["proxy"]

    @classmethod
    def tearDownClass(cls):
        cls.env.clear_environment()

    def setUp(self):
        os.environ["PATH"] = ".:" + os.environ["PATH"]
        if not yt.exists(TEST_DIR):
            yt.set(TEST_DIR, {})

        config.WAIT_TIMEOUT = 0.2
        config.DEFAULT_STRATEGY = yt.WaitStrategy(print_progress=False)

    def tearDown(self):
        yt.remove(TEST_DIR)

    def read_records(self, table, format=None):
        return filter(None,
            map(partial(line_to_record, format=format),
                yt.read_table(table, format)))

    def temp_records(self):
        columns = [string.digits, reversed(string.ascii_lowercase[:10]), string.ascii_uppercase[:10]]
        return map(record_to_line, starmap(Record, imap(flatten, reduce(izip, columns))))


    def create_temp_table(self):
        table = TEST_DIR + "/temp"
        yt.write_table(table, self.temp_records())
        return table

    def dsv_records(self):
        return map(
            partial(record_to_line, format=yt.DsvFormat()),
                [{"a": 12,  "b": "ignat"},
                           {"b": "max",  "c": 17.5},
                 {"a": "x", "b": "name", "c": 0.5}])

    def create_dsv_table(self):
        table = TEST_DIR + "/dsv"
        yt.write_table(table, self.dsv_records(), format=yt.DsvFormat())
        return table

    def run_capitilize_b(self, src, dst):
        yt.run_map("PYTHONPATH=. ./capitilize_b.py", src, dst,
                   files=map(abspath, ["../config.py", "../common.py", "../record.py", "../format.py", "capitilize_b.py"]),
                   format=yt.DsvFormat())


    def random_string(self, length):
        char_set = string.ascii_uppercase + string.digits
        return "".join(random.sample(char_set, length))

    def test_common_operations(self):
        self.assertTrue(yt.exists("/"))
        self.assertTrue(yt.exists("//sys"))
        self.assertFalse(yt.exists("//\"sys\""))

        self.assertFalse(yt.exists('//%s/%s' %
                                (self.random_string(10), self.random_string(10))))

        random_strA = self.random_string(10)
        random_strB = self.random_string(10)


        half_path = '%s/"%s"' % (TEST_DIR, random_strA)
        full_path = '%s/"%s"/"%s"' % (TEST_DIR, random_strA, random_strB)
        self.assertRaises(YtError, lambda: yt.set(full_path, {}))
        self.assertEqual(yt.set(half_path, {}), None)
        self.assertEqual(yt.set(full_path, {}), None)
        self.assertTrue(yt.exists(full_path))
        self.assertEqual(yt.get(full_path), {})
        self.assertEqual(yt.remove(half_path), None)
        self.assertRaises(YtError, lambda: yt.remove(full_path))

    def test_read_write(self):
        table = TEST_DIR + "/temp"
        if yt.exists(table):
            yt.remove(table)
        yt.create_table(table)

        records = map(record_to_line, [Record("x", "y", "z"), Record("key", "subkey", "value")])
        yt.write_table(table, records)
        self.assertEqual(sorted(yt.read_table(table)), sorted(records))

        # check rewrite case
        yt.write_table(Table(table, append=True), records)
        self.assertEqual(sorted(yt.read_table(table)), sorted(records + records))

    def test_huge_table(self):
        POWER = 3
        records = \
            imap(record_to_line,
                 (Record(str(i), str(i * i), "long long string with strange symbols #*@*&^$#%@(#!@:L|L|KL..,,.~`")
                 for i in xrange(10 ** POWER)))
        table = TEST_DIR + "/temp"
        yt.write_table(table, records)
        self.assertEqual(yt.records_count(table), 10 ** POWER)

        records_count = 0
        for rec in yt.read_table(table):
            records_count += 1
        self.assertEqual(records_count, 10 ** POWER)

    def test_copy_move(self):
        table = self.create_temp_table()
        other_table = TEST_DIR + "/temp_other"
        yt.create_table(other_table)

        yt.copy_table(table, other_table)
        self.assertEqual(sorted(self.temp_records()),
                         sorted(yt.read_table(other_table)))

        yt.copy_table(table, other_table)
        self.assertEqual(sorted(self.temp_records()),
                         sorted(yt.read_table(other_table)))

        yt.copy_table(table, Table(other_table, append=True))
        self.assertEqual(sorted(list(self.temp_records()) + list(self.temp_records())),
                         sorted(yt.read_table(other_table)))

        yt.move_table(table, other_table)
        self.assertFalse(yt.exists(table))
        self.assertEqual(list(yt.read_table(other_table)),
                         sorted(list(self.temp_records())))

        self.assertRaises(YtError, lambda: yt.copy_table(table, table))

    def test_sort(self):
        table = self.create_temp_table()
        files_count = len(list(yt.list(TEST_DIR)))
        yt.sort_table(table)
        self.assertEqual(len(list(yt.list(TEST_DIR))), files_count)
        self.assertEqual(self.read_records(table)[0].key, "0")
        self.assertEqual(sorted(list(self.temp_records())),
                         list(yt.read_table(table)))
        self.assertTrue(yt.is_sorted(table))

        yt.sort_table(table, sort_by=["subkey"])
        self.assertEqual(self.read_records(table)[0].subkey, "a")

        unexisting_table = TEST_DIR + "/unexisting"
        yt.sort_table(unexisting_table)
        self.assertFalse(yt.exists(unexisting_table))

    def test_attributes(self):
        table = self.create_temp_table()
        self.assertEqual(yt.records_count(table), 10)
        self.assertFalse(yt.is_sorted(table))

        yt.set_attribute(table, "my_attribute", {})
        yt.set_attribute(table, "my_attribute/000", 10)
        self.assertEqual(yt.get_attribute(table, "my_attribute/000"), 10)
        #self.assertEqual(yt.list_attributes(table, "my_attribute"), ["000"])

    def test_operations(self):
        table = self.create_temp_table()
        other_table = TEST_DIR + "/temp_other"
        yt.run_map("PYTHONPATH=. ./my_op.py",
                   table, other_table,
                   files=map(abspath, ["my_op.py", "helpers.py"]))
        self.assertEqual(2 * yt.records_count(table), yt.records_count(other_table))

        yt.sort_table(table)
        yt.run_reduce("./cpp_bin",
                      table, other_table,
                      files=abspath("cpp_bin"))

    def test_abort_operation(self):
        strategy = yt.AsyncStrategy()
        table = self.create_temp_table()
        other_table = TEST_DIR + "/temp_other"
        yt.run_map("PYTHONPATH=. ./my_op.py 10.0",
                   table, other_table,
                   files=map(abspath, ["my_op.py", "helpers.py"]),
                   strategy=strategy)

        operation = strategy.operations[-1]
        yt.abort_operation(operation)
        self.assertEqual(yt.get_operation_state(operation), "aborted")


    def test_dsv(self):
        table = self.create_dsv_table()
        other_table = TEST_DIR + "/dsv_capital"
        self.run_capitilize_b(table, other_table)

        recs = self.read_records(other_table, format=yt.DsvFormat())
        self.assertEqual(
            sorted([rec["b"] for rec in recs]),
            ["IGNAT", "MAX", "NAME"])
        self.assertEqual(
            sorted([rec["c"] for rec in recs if "c" in rec]),
            ["0.5", "17.5"])

    def test_many_output_tables(self):
        table = self.create_temp_table()
        other_table = TEST_DIR + "/temp_other"
        another_table = TEST_DIR + "/temp_another"
        more_another_table = TEST_DIR + "/temp_more_another"
        yt.copy_table(table, another_table)

        yt.run_map("PYTHONPATH=. ./many_output.py",
                   table,
                   [other_table, Table(another_table, append=True), more_another_table],
                   files=abspath("many_output.py"))
        self.assertEqual(yt.records_count(other_table), 1)
        self.assertEqual(yt.records_count(another_table), 11)
        self.assertEqual(yt.records_count(more_another_table), 1)

    def test_range_operations(self):
        table = self.create_dsv_table()
        other_table = TEST_DIR + "/dsv_capital"

        self.run_capitilize_b(Table(table, columns=["b"]), other_table)
        recs = self.read_records(other_table, format=yt.DsvFormat())

        self.assertEqual(
            sorted([rec["b"] for rec in recs]),
            ["IGNAT", "MAX", "NAME"])
        self.assertEqual(
            sorted([rec["c"] for rec in recs if "c" in rec]),
            [])

        yt.sort_table(table, sort_by=["b", "c"])
        self.assertEqual(
            self.read_records(Table(table, lower_key="a", upper_key="n", columns=["b"]),
                              format=yt.DsvFormat()),
            [{"b": "ignat"}, {"b": "max"}])

        self.assertEqual(
            self.read_records(Table(table, columns=["c"]),
                              format=yt.DsvFormat()),
            [{"c": "17.5"}, {"c": "0.5"}])

        self.assertEqual(
            self.read_records(Table(table, columns=["b"], end_index=2),
                              format=yt.DsvFormat()),
            [{"b": "ignat"}, {"b": "max"}])

        self.assertEqual(
            self.read_records(table + '{b}[:#2]', format=yt.DsvFormat()),
            [{"b": "ignat"}, {"b": "max"}])

    def test_merge(self):
        table = self.create_temp_table()
        other_table = TEST_DIR + "/temp_other"
        another_table = TEST_DIR + "/temp_another"
        yt.sort_table(table)
        self.assertTrue(yt.is_sorted(table))
        yt.merge_tables(table, other_table, mode="sorted")
        self.assertTrue(yt.is_sorted(other_table))
        yt.merge_tables([table, other_table], another_table, mode="sorted")
        self.assertTrue(yt.is_sorted(another_table))
        self.assertEqual(yt.records_count(another_table), 20)

    def test_digit_names(self):
        table = TEST_DIR + '/123'
        yt.write_table(table, self.temp_records())
        yt.sort_table(table)
        self.assertEqual(self.read_records(table)[0].key, "0")

    def test_empty_input_tables(self):
        table = self.create_temp_table()
        other_table = TEST_DIR + "/temp_other"
        another_table = TEST_DIR + "/temp_another"
        yt.run_map("PYTHONPATH=. ./my_op.py",
                   [table, other_table], another_table,
                   files=map(abspath, ["my_op.py", "helpers.py"]))
        self.assertFalse(yt.exists(other_table))

    def test_file_operations(self):
        dest = []
        for i in xrange(2):
            self.assertTrue(yt.smart_upload_file(abspath("my_op.py")).find("/my_op.py") != -1)

        for d in dest:
            self.assertEqual(list(yt.download_file(dest)),
                             open(abspath("my_op.py")).readlines())

        dest = TEST_DIR+"/file_dir/some_file"
        yt.smart_upload_file(abspath("my_op.py"), destination=dest)
        self.assertEqual(yt.get_attribute(dest, "file_name"), "some_file")

    def test_map_reduce_operation(self):
        input = TEST_DIR + "/input"
        output = TEST_DIR + "/output"
        yt.write_table(input,
            [
                "\1a\t\t\n",
                "\1b\t\t\n",
                "\1c\t\t\n",
                "a b\tc\t\n",
                "c c\tc\tc c a\n"
            ])
        yt.run_map_reduce("./split.py", "./collect.py", input, output,
                          map_files=abspath("split.py"), reduce_files=abspath("collect.py"))
        self.assertEqual(
            sorted(list(yt.read_table(output))),
            sorted(["a\t\t2\n", "b\t\t1\n", "c\t\t6\n"]))

    def test_python_operations(self):
        def func(rec):
            yield rec.strip() + "aaaaaaaaaa\n"

        table = self.create_temp_table()
        other_table = TEST_DIR + "/temp_other"
        yt.run_map(func, table, other_table)

        self.assertEqual(
            sorted(list(yt.read_table(other_table))),
            sorted(list(chain(*imap(func, self.temp_records())))))

    def test_empty_output_table_deletion(self):
        table = self.create_temp_table()
        other_table = TEST_DIR + "/temp_other"
        yt.run_map("cat 1>&2 2>/dev/null", table, other_table)
        self.assertFalse(yt.exists(other_table))

    def test_reformatting(self):
        def reformat(rec):
            values = rec.strip().split("\t", 2)
            yield "\t".join("=".join([k, v]) for k, v in zip(["k", "s", "v"], values))
        table = self.create_temp_table()
        other_table = TEST_DIR + "/temp_other"
        yt.run_map(reformat, table, other_table, output_format=yt.DsvFormat())
        self.assertTrue(yt.exists(other_table))
        self.assertEqual(
            sorted(
                map(lambda str: str.split("=", 1)[0],
                    yt.read_table(other_table, format=yt.DsvFormat())\
                        .next().strip().split("\t"))),
            ["k", "s", "v"])

    def test_table_ranges_with_exists(self):
        table = self.create_temp_table()
        self.assertTrue(yt.exists(table))
        self.assertTrue(yt.exists(table + "/@"))
        self.assertTrue(yt.exists(table + "/@compression_ratio"))
        self.assertTrue(len(yt.list_attributes(table)) > 1)
        self.assertTrue(len(yt.get_attribute(table, "channels")) == 0)

    def test_mapreduce_binary(self):
        yt.mkdir("//statbox")
        yt.create_table("//statbox/table")
        proc = subprocess.Popen(
            "YT_PROXY=%s %s" %
                (config.PROXY,
                 os.path.join(LOCATION, "../test_mapreduce.sh")),
            shell=True)
        proc.communicate()
        self.assertEqual(proc.returncode, 0)

    def test_inplace_operations(self):
        table = self.create_temp_table()

        yt.run_map("cat", table, table)
        self.assertEqual(yt.records_count(table), 10)

        yt.run_reduce("cat", table, table)
        self.assertEqual(yt.records_count(table), 10)

    def test_sort_of_sorted_tables(self):
        table = self.create_temp_table()
        other_table = TEST_DIR + "/temp_other"
        result_table = TEST_DIR + "/result"

        yt.sort_table(table)
        yt.copy_table(table, other_table)
        self.assertTrue(yt.is_sorted(other_table))

        yt.sort_table([table, other_table], result_table)
        self.assertTrue(yt.is_sorted(result_table))
        self.assertEqual(yt.records_count(result_table), 20)
    
    def test_sort_of_one_sorted_table(self):
        table = self.create_temp_table()
        other_table = TEST_DIR + "/temp_other"

        yt.sort_table(table)
        self.assertTrue(yt.is_sorted(table))

        yt.sort_table([table], other_table)
        self.assertTrue(yt.is_sorted(other_table))
        self.assertEqual(yt.records_count(other_table), 10)


if __name__ == "__main__":
    unittest.main()


