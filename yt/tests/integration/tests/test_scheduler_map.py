from yt_env_setup import YTEnvSetup, unix_only, patch_porto_env_only, wait, skip_if_porto, parametrize_external
from yt_commands import *

from yt.test_helpers import assert_items_equal, are_almost_equal

from flaky import flaky

import pytest
import random
import string
import time
##################################################################

porto_delta_node_config = {
    "exec_agent": {
        "slot_manager": {
            # <= 18.4
            "enforce_job_control": True,
            "job_environment": {
                # >= 19.2
                "type": "porto",
            },
        }
    }
}

##################################################################

class TestSchedulerMapCommands(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 16
    NUM_SCHEDULERS = 1
    USE_DYNAMIC_TABLES = True

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,
            "operations_update_period": 10,
            "running_jobs_update_period": 10,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "operations_update_period": 10,
            "map_operation_options": {
                "job_splitter": {
                    "min_job_time": 5000,
                    "min_total_data_size": 1024,
                    "update_period": 100,
                    "candidate_percentile": 0.8,
                    "max_jobs_per_split": 3,
                    "max_input_table_count": 5,
                },
            },
        }
    }

    def test_empty_table(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        map(in_="//tmp/t1", out="//tmp/t2", command="cat")

        assert read_table("//tmp/t2") == []

    def test_no_outputs(self):
        create("table", "//tmp/t1")
        write_table("//tmp/t1", [{"key": "value"}])
        op = map(in_="//tmp/t1", command="cat > /dev/null; echo stderr>&2")
        check_all_stderrs(op, "stderr\n", 1)

    def test_empty_range(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")

        original_data = [{"index": i} for i in xrange(10)]
        write_table("//tmp/t1", original_data)

        command = "cat"
        map(in_="<ranges=[{lower_limit={row_index=1}; upper_limit={row_index=1}}]>//tmp/t1", out="//tmp/t2", command=command)

        assert [] == read_table("//tmp/t2", verbose=False)

    @unix_only
    def test_one_chunk(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", {"a": "b"})
        op = map(
            dont_track=True,
            in_="//tmp/t1",
            out="//tmp/t2",
            command=r'cat; echo "{v1=\"$V1\"};{v2=\"$TMPDIR\"}"',
            spec={"mapper": {"environment": {"V1": "Some data", "TMPDIR": "$(SandboxPath)/mytmp"}},
                  "title": "MyTitle"})

        get(op.get_path() + "/@spec")
        op.track()

        res = read_table("//tmp/t2")
        assert len(res) == 3
        assert res[0] == {"a": "b"}
        assert res[1] == {"v1": "Some data"}
        assert res[2].has_key("v2")
        assert res[2]["v2"].endswith("/mytmp")
        assert res[2]["v2"].startswith("/")

    def test_big_input(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")

        count = 1000 * 1000
        original_data = [{"index": i} for i in xrange(count)]
        write_table("//tmp/t1", original_data)

        command = "cat"
        map(in_="//tmp/t1", out="//tmp/t2", command=command)

        new_data = read_table("//tmp/t2", verbose=False)
        assert sorted(row.items() for row in new_data) == [[("index", i)] for i in xrange(count)]

    def test_two_outputs_at_the_same_time(self):
        create("table", "//tmp/t_input")
        create("table", "//tmp/t_output1")
        create("table", "//tmp/t_output2")

        count = 1000
        original_data = [{"index": i} for i in xrange(count)]
        write_table("//tmp/t_input", original_data)

        file = "//tmp/some_file.txt"
        create("file", file)
        write_file(file, "{value=42};\n")

        command = 'bash -c "cat <&0 & sleep 0.1; cat some_file.txt >&4; wait;"'
        map(in_="//tmp/t_input",
            out=["//tmp/t_output1", "//tmp/t_output2"],
            command=command,
            file=[file],
            verbose=True)

        assert read_table("//tmp/t_output2") == [{"value": 42}]
        assert sorted([row.items() for row in read_table("//tmp/t_output1")]) == [[("index", i)] for i in xrange(count)]

    def test_write_two_outputs_consistently(self):
        create("table", "//tmp/t_input")
        create("table", "//tmp/t_output1")
        create("table", "//tmp/t_output2")

        count = 10000
        original_data = [{"index": i} for i in xrange(count)]
        write_table("//tmp/t_input", original_data)

        file1 = "//tmp/some_file.txt"
        create("file", file1)
        write_file(file1, "}}}}};\n")

        with pytest.raises(YtError):
            map(in_="//tmp/t_input",
                out=["//tmp/t_output1", "//tmp/t_output2"],
                command='cat some_file.txt >&4; cat >&4; echo "{value=42}"',
                file=[file1],
                verbose=True)

    @unix_only
    def test_in_equal_to_out(self):
        create("table", "//tmp/t1")
        write_table("//tmp/t1", {"foo": "bar"})

        map(in_="//tmp/t1", out="<append=true>//tmp/t1", command="cat")

        assert read_table("//tmp/t1") == [{"foo": "bar"}, {"foo": "bar"}]

    def test_input_row_count(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"key": i} for i in xrange(5)])

        sort(in_="//tmp/t1", out="//tmp/t1", sort_by="key")
        op = map(command="cat", in_="//tmp/t1[:1]", out="//tmp/t2")

        assert get("//tmp/t2/@row_count") == 1

        row_count = get(op.get_path() + "/@progress/job_statistics/data/input/row_count/$/completed/map/sum")
        assert row_count == 1

    def test_multiple_output_row_count(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")
        write_table("//tmp/t1", [{"key": i} for i in xrange(5)])

        op = map(command="cat; echo {hello=world} >&4", in_="//tmp/t1", out=["//tmp/t2", "//tmp/t3"])
        assert get("//tmp/t2/@row_count") == 5
        row_count = get(op.get_path() + "/@progress/job_statistics/data/output/0/row_count/$/completed/map/sum")
        assert row_count == 5
        row_count = get(op.get_path() + "/@progress/job_statistics/data/output/1/row_count/$/completed/map/sum")
        assert row_count == 1

    def test_codec_statistics(self):
        create("table", "//tmp/t1", attributes={"compression_codec": "lzma_9"})
        create("table", "//tmp/t2", attributes={"compression_codec": "lzma_1"})

        def random_string(n):
            return ''.join(random.choice(string.printable) for _ in xrange(n))

        write_table("//tmp/t1", [{str(i): random_string(1000)} for i in xrange(100)])  # so much to see non-zero decode cpu usage in release mode

        op = map(command="cat", in_="//tmp/t1", out="//tmp/t2")
        decode_time = get(op.get_path() + "/@progress/job_statistics/codec/cpu/decode/lzma_9/$/completed/map/sum")
        encode_time = get(op.get_path() + "/@progress/job_statistics/codec/cpu/encode/0/lzma_1/$/completed/map/sum")
        assert decode_time > 0
        assert encode_time > 0

    @unix_only
    def test_sorted_output(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        for i in xrange(2):
            write_table("<append=true>//tmp/t1", {"key": "foo", "value": "ninja"})

        command = """cat >/dev/null;
           if [ "$YT_JOB_INDEX" = "0" ]; then
               k1=0; k2=1;
           else
               k1=0; k2=0;
           fi
           echo "{key=$k1; value=one}; {key=$k2; value=two}"
        """

        map(in_="//tmp/t1",
            out="<sorted_by=[key];append=true>//tmp/t2",
            command=command,
            spec={"job_count": 2})

        assert get("//tmp/t2/@sorted")
        assert get("//tmp/t2/@sorted_by") == ["key"]
        assert read_table("//tmp/t2") == [{"key": 0, "value": "one"}, {"key": 0, "value": "two"},
                                          {"key": 0, "value": "one"}, {"key": 1, "value": "two"}]

    def test_sorted_output_overlap(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        for i in xrange(2):
            write_table("<append=true>//tmp/t1", {"key": "foo", "value": "ninja"})

        command = 'cat >/dev/null; echo "{key=1; value=one}; {key=2; value=two}"'

        with pytest.raises(YtError):
            map(in_="//tmp/t1",
                out="<sorted_by=[key]>//tmp/t2",
                command=command,
                spec={"job_count": 2})

    def test_sorted_output_job_failure(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        for i in xrange(2):
            write_table("<append=true>//tmp/t1", {"key": "foo", "value": "ninja"})

        command = "cat >/dev/null; echo \"{key=2; value=one}; {key=1; value=two}\""

        with pytest.raises(YtError):
            map(in_="//tmp/t1",
                out="<sorted_by=[key]>//tmp/t2",
                command=command,
                spec={"job_count": 2})

    @unix_only
    def test_job_count(self):
        create("table", "//tmp/t1")
        for i in xrange(5):
            write_table("<append=true>//tmp/t1", {"foo": "bar"})

        command = "cat > /dev/null; echo {hello=world}"

        def check(table_name, job_count, expected_num_records):
            create("table", table_name)
            map(in_="//tmp/t1",
                out=table_name,
                command=command,
                spec={"job_count": job_count})
            assert read_table(table_name) == [{"hello": "world"} for _ in range(expected_num_records)]

        check("//tmp/t2", 3, 3)
        check("//tmp/t3", 10, 5) # number of jobs cannot be more than number of rows.

    @unix_only
    def test_skewed_rows(self):
        create("table", "//tmp/t1")
        # 5 small rows
        write_table("<append=true>//tmp/t1", [{"foo": "bar"}] * 5)
        # and one large row
        write_table("<append=true>//tmp/t1", {"foo": "".join(["r"] * 1024)})

        create("table", "//tmp/t2")
        map(in_="//tmp/t1",
            out="//tmp/t2",
            command="cat > /dev/null; echo {hello=world}",
            spec={"job_count": 6})
        assert read_table("//tmp/t2") == [{"hello": "world"} for _ in xrange(6)]

    # We skip this one in porto because it requires a lot of interaction with porto
    # (since there are a lot of operations with large number of jobs).
    # There is completely nothing porto-specific here.
    @unix_only
    @skip_if_porto
    def test_job_per_row(self):
        create("table", "//tmp/input")

        job_count = 976
        original_data = [{"index": str(i)} for i in xrange(job_count)]
        write_table("//tmp/input", original_data)

        create("table", "//tmp/output", ignore_existing=True)

        for job_count in xrange(976, 950, -1):
            op = map(
                dont_track=True,
                in_="//tmp/input",
                out="//tmp/output",
                command="sleep 100",
                spec={"job_count": job_count})

            wait(lambda: len(op.get_running_jobs()) > 1)

            assert op.get_job_count("total") == job_count
            op.abort()

    @unix_only
    def run_many_output_tables(self, yamr_mode=False):
        output_tables = ["//tmp/t%d" % i for i in range(3)]

        create("table", "//tmp/t_in")
        for table_path in output_tables:
            create("table", table_path)

        write_table("//tmp/t_in", {"a": "b"})

        if yamr_mode:
            mapper = "cat  > /dev/null; echo {v = 0} >&3; echo {v = 1} >&4; echo {v = 2} >&5"
        else:
            mapper = "cat  > /dev/null; echo {v = 0} >&1; echo {v = 1} >&4; echo {v = 2} >&7"

        create("file", "//tmp/mapper.sh")
        write_file("//tmp/mapper.sh", mapper)

        map(in_="//tmp/t_in",
            out=output_tables,
            command="bash mapper.sh",
            file="//tmp/mapper.sh",
            spec={"mapper": {"use_yamr_descriptors": yamr_mode}})

        assert read_table(output_tables[0]) == [{"v": 0}]
        assert read_table(output_tables[1]) == [{"v": 1}]
        assert read_table(output_tables[2]) == [{"v": 2}]

    @unix_only
    def test_many_output_yt(self):
        self.run_many_output_tables()

    @unix_only
    def test_many_output_yamr(self):
        self.run_many_output_tables(True)

    @unix_only
    def test_output_tables_switch(self):
        output_tables = ["//tmp/t%d" % i for i in range(3)]

        create("table", "//tmp/t_in")
        for table_path in output_tables:
            create("table", table_path)

        write_table("//tmp/t_in", {"a": "b"})
        mapper = 'cat  > /dev/null; echo "<table_index=2>#;{v = 0};{v = 1};<table_index=0>#;{v = 2}"'

        create("file", "//tmp/mapper.sh")
        write_file("//tmp/mapper.sh", mapper)

        map(in_="//tmp/t_in",
            out=output_tables,
            command="bash mapper.sh",
            file="//tmp/mapper.sh")

        assert read_table(output_tables[0]) == [{"v": 2}]
        assert read_table(output_tables[1]) == []
        assert read_table(output_tables[2]) == [{"v": 0}, {"v": 1}]

    @unix_only
    def test_executable_mapper(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", {"foo": "bar"})

        mapper =  \
"""
#!/bin/bash
cat > /dev/null; echo {hello=world}
"""

        create("file", "//tmp/mapper.sh")
        write_file("//tmp/mapper.sh", mapper)

        set("//tmp/mapper.sh/@executable", True)

        create("table", "//tmp/t_out")
        map(in_="//tmp/t_in",
            out="//tmp/t_out",
            command="./mapper.sh",
            file="//tmp/mapper.sh")

        assert read_table("//tmp/t_out") == [{"hello": "world"}]

    @unix_only
    def test_table_index(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/out")

        write_table("//tmp/t1", {"key": "a", "value": "value"})
        write_table("//tmp/t2", {"key": "b", "value": "value"})

        mapper = \
"""
import sys
table_index = sys.stdin.readline().strip()
row = sys.stdin.readline().strip()
print row + table_index

table_index = sys.stdin.readline().strip()
row = sys.stdin.readline().strip()
print row + table_index
"""

        create("file", "//tmp/mapper.py")
        write_file("//tmp/mapper.py", mapper)

        map(in_=["//tmp/t1", "//tmp/t2"],
            out="//tmp/out",
            command="python mapper.py",
            file="//tmp/mapper.py",
            spec={"mapper": {"format": yson.loads("<enable_table_index=true>yamr")}})

        expected = [{"key": "a", "value": "value0"},
                    {"key": "b", "value": "value1"}]
        assert_items_equal(read_table("//tmp/out"), expected)

    @unix_only
    def test_range_index(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/out")

        for i in xrange(1, 3):
            write_table(
                "<append=true>//tmp/t_in",
                [
                    {"key": "%05d" % i, "value": "value"},
                ],
                sorted_by=["key", "value"])

        t_in = '<ranges=[{lower_limit={key=["00002"]};upper_limit={key=["00003"]}};{lower_limit={key=["00002"]};upper_limit={key=["00003"]}}]>//tmp/t_in'

        op = map(
            dont_track=True,
            in_=[t_in],
            out="//tmp/out",
            command="cat >& 2",
            spec={
                "job_io": {
                    "control_attributes": {
                        "enable_range_index": True,
                        "enable_row_index": True,
                    }
                },
                "mapper": {
                    "input_format": yson.loads("<format=text>yson"),
                    "output_format": "dsv",
                }
            })

        op.track()
        check_all_stderrs(op, '"range_index"=0;', 1, substring=True)
        check_all_stderrs(op, '"range_index"=1;', 1, substring=True)

    def test_insane_demand(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", {"cool": "stuff"})

        with pytest.raises(YtError):
            map(in_="//tmp/t_in", out="//tmp/t_out", command="cat",
                spec={"mapper": {"memory_limit": 1000000000000}})

    def test_check_input_fully_consumed(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", {"foo": "bar"})

        command = 'python -c "import os; os.read(0, 5);"'

        # If all jobs failed then operation is also failed
        with pytest.raises(YtError):
            map(in_="//tmp/t1",
                out="//tmp/t2",
                command=command,
                spec={"mapper": {"input_format": "dsv", "check_input_fully_consumed": True}})

        assert read_table("//tmp/t2") == []

    def test_check_input_not_fully_consumed(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")

        data = [{"foo": "bar"} for _ in xrange(10000)]
        write_table("//tmp/t1", data)

        map(
            in_="//tmp/t1",
            out="//tmp/t2",
            command="head -1",
            spec={"mapper": {"input_format": "dsv", "output_format": "dsv"}})

        assert read_table("//tmp/t2") == [{"foo": "bar"}]

    @flaky(max_runs=3)
    def test_live_preview(self):
        create_user("u")

        data = [{"foo": i} for i in range(5)]

        create("table", "//tmp/t1")
        write_table("//tmp/t1", data)

        create("table", "//tmp/t2")
        set("//tmp/t2/@acl", [make_ace("allow", "u", "write")])
        effective_acl = get("//tmp/t2/@effective_acl")

        schema = make_schema([{"name": "foo", "type": "int64", "required": False}], strict=True, unique_keys=False)
        alter_table("//tmp/t2", schema=schema)

        op = map(
            dont_track=True,
            command=with_breakpoint("cat && BREAKPOINT"),
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"data_size_per_job": 1})
        jobs = wait_breakpoint(job_count=2)

        operation_path = op.get_path()
        async_transaction_id = get(operation_path + "/@async_scheduler_transaction_id")
        assert exists(operation_path + "/output_0", tx=async_transaction_id)
        assert effective_acl == get(operation_path + "/output_0/@acl", tx=async_transaction_id)
        assert schema == normalize_schema(get(operation_path + "/output_0/@schema", tx=async_transaction_id))

        for job_id in jobs[:2]:
            release_breakpoint(job_id=job_id)

        wait(lambda : op.get_job_count("completed") >= 2)

        time.sleep(1)

        live_preview_data = read_table(operation_path + "/output_0", tx=async_transaction_id)
        assert len(live_preview_data) == 2
        assert all(record in data for record in live_preview_data)

        release_breakpoint()
        op.track()
        assert sorted(read_table("//tmp/t2")) == sorted(data)

    def test_new_live_preview_multiple_output_tables(self):
        create_user("u")

        data = [{"foo": i} for i in range(5)]

        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", data)

        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")

        op = map(
            dont_track=True,
            command=with_breakpoint('echo "{a=$YT_JOB_INDEX}" >&1; echo "{b=$YT_JOB_INDEX}" >&4; BREAKPOINT'),
            in_="//tmp/t_in",
            out=["//tmp/t_out1", "//tmp/t_out2"],
            spec={"data_size_per_job": 1})

        jobs = wait_breakpoint(job_count=2)
        operation_path = op.get_path()
        for job_id in jobs[:2]:
            release_breakpoint(job_id=job_id)

        wait(lambda : op.get_job_count("completed") == 2)
        wait(lambda: exists(operation_path + "/controller_orchid"))

        live_preview_data1 = read_table(operation_path + "/controller_orchid/data_flow_graph/vertices/map/live_previews/0")
        live_preview_data2 = read_table(operation_path + "/controller_orchid/data_flow_graph/vertices/map/live_previews/1")
        live_preview_data1 = [d["a"] for d in live_preview_data1]
        live_preview_data2 = [d["b"] for d in live_preview_data2]
        assert sorted(live_preview_data1) == sorted(live_preview_data2)
        assert len(live_preview_data1) == 2

        release_breakpoint()
        op.track()


    def test_row_sampling(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")

        count = 1000
        original_data = [{"index": i} for i in xrange(count)]
        write_table("//tmp/t1", original_data)

        command = "cat"
        sampling_rate = 0.5
        spec = {"job_io": {"table_reader": {"sampling_seed": 42, "sampling_rate": sampling_rate}}}

        map(in_="//tmp/t1", out="//tmp/t2", command=command, spec=spec)
        map(in_="//tmp/t1", out="//tmp/t3", command=command, spec=spec)

        new_data_t2 = read_table("//tmp/t2", verbose=False)
        new_data_t3 = read_table("//tmp/t3", verbose=False)

        assert sorted(row.items() for row in new_data_t2) == sorted(row.items() for row in new_data_t3)

        actual_rate = len(new_data_t2) * 1.0 / len(original_data)
        variation = sampling_rate * (1 - sampling_rate)
        assert sampling_rate - variation <= actual_rate <= sampling_rate + variation

    @pytest.mark.parametrize("ordered", [False, True])
    @unix_only
    def test_map_row_count_limit(self, ordered):
        create("table", "//tmp/input")
        for i in xrange(5):
            write_table("<append=true>//tmp/input", {"key": "%05d" % i, "value": "foo"})

        create("table", "//tmp/output")
        op = map(
            dont_track=True,
            in_="//tmp/input",
            out="<row_count_limit=3>//tmp/output",
            command=with_breakpoint("cat ; BREAKPOINT"),
            oredered=ordered,
            spec={
                "data_size_per_job": 1,
                "max_failed_job_count": 1
            })
        jobs = wait_breakpoint(job_count=5)
        assert len(jobs) == 5

        for job_id in jobs[:3]:
            release_breakpoint(job_id=job_id)

        op.track()
        assert len(read_table("//tmp/output")) == 3


    @unix_only
    def test_job_controller_orchid(self):
        create("table", "//tmp/input")
        for i in xrange(5):
            write_table("<append=true>//tmp/input", {"key": "%05d" % i, "value": "foo"})

        create("table", "//tmp/output")
        op = map(
            dont_track=True,
            in_="//tmp/input",
            out="//tmp/output",
            command=with_breakpoint("cat && BREAKPOINT"),
            spec={
                "data_size_per_job": 1,
                "max_failed_job_count": 1
            })
        wait_breakpoint(job_count=5)

        for n in get("//sys/cluster_nodes"):
            job_controller = get("//sys/cluster_nodes/{0}/orchid/job_controller/active_jobs/scheduler".format(n))
            for job_id, values in job_controller.items():
                assert "start_time" in values
                assert "operation_id" in values
                assert "statistics" in values
                assert "job_type" in values
                assert "duration" in values

        op.abort()

    @unix_only
    def test_map_row_count_limit_second_output(self):
        create("table", "//tmp/input")
        for i in xrange(5):
            write_table("<append=true>//tmp/input", {"key": "%05d" % i, "value": "foo"})

        create("table", "//tmp/out_1")
        create("table", "//tmp/out_2")
        op = map(
            dont_track=True,
            in_="//tmp/input",
            out=["//tmp/out_1", "<row_count_limit=3>//tmp/out_2"],
            command=with_breakpoint("cat >&4 ; BREAKPOINT"),
            spec={
                "data_size_per_job": 1,
                "max_failed_job_count": 1
            })

        jobs = wait_breakpoint(job_count=5)
        for job_id in jobs[:3]:
            release_breakpoint(job_id=job_id)

        op.track()
        assert len(read_table("//tmp/out_1")) == 0
        assert len(read_table("//tmp/out_2")) == 3

    def test_multiple_row_count_limit(self):
        create("table", "//tmp/input")

        create("table", "//tmp/output")
        with pytest.raises(YtError):
            map(in_="//tmp/input",
                out=["<row_count_limit=1>//tmp/out_1", "<row_count_limit=1>//tmp/out_2"],
                command="cat")

    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_schema_validation(self, optimize_for):
        schema = make_schema(
            [
                {"name": "key", "type": "int64", "required": False},
                {"name": "value", "type": "string", "required": False},
            ],
            strict=True,
            unique_keys=False,
        )
        create("table", "//tmp/input")
        create("table", "//tmp/output", attributes={
            "optimize_for": optimize_for,
            "schema": schema})
        create("table", "//tmp/output2")

        for i in xrange(10):
            write_table("<append=true>//tmp/input", {"key": i, "value": "foo"})

        map(in_="//tmp/input",
            out="//tmp/output",
            command="cat")

        assert get("//tmp/output/@schema_mode") == "strong"
        assert get("//tmp/output/@schema/@strict")
        assert normalize_schema(get("//tmp/output/@schema")) == schema
        assert_items_equal(read_table("//tmp/output"), [{"key": i, "value": "foo"} for i in xrange(10)])

        map(in_="//tmp/input",
            out="<schema=%s>//tmp/output2" % yson.dumps(schema),
            command="cat")

        assert get("//tmp/output2/@schema_mode") == "strong"
        assert get("//tmp/output2/@schema/@strict")
        assert normalize_schema(get("//tmp/output2/@schema")) == schema
        assert_items_equal(read_table("//tmp/output2"), [{"key": i, "value": "foo"} for i in xrange(10)])

        write_table("//tmp/input", {"key": "1", "value": "foo"})

        with pytest.raises(YtError):
            map(in_="//tmp/input",
                out="//tmp/output",
                command="cat")

    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_rename_columns_simple(seld, optimize_for):
        create("table", "//tmp/tin", attributes={
                "schema": [{"name": "a", "type": "int64"}],
                "optimize_for": optimize_for
            })
        create("table", "//tmp/tout")
        write_table("//tmp/tin", [{"a": 42}])

        map(in_="<rename_columns={a=b}>//tmp/tin",
            out="//tmp/tout",
            command="cat")
        assert read_table("//tmp/tout") == [{"b": 42}]

    def test_rename_columns_without_schema(self):
        create("table", "//tmp/tin")
        create("table", "//tmp/tout")
        write_table("//tmp/tin", [{"a": 42}])

        with pytest.raises(YtError):
            map(in_="<rename_columns={a=b}>//tmp/tin",
                out="//tmp/tout",
                command="cat")

    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_raname_columns_overlapping_names_in_schema(self, optimize_for):
        create("table", "//tmp/tin", attributes={
                "schema": [
                    {"name": "a", "type": "int64"},
                    {"name": "b", "type": "int64"}],
                "optimize_for": optimize_for
            })
        create("table", "//tmp/tout")
        write_table("//tmp/tin", [{"a": 42, "b": 34}])

        with pytest.raises(YtError):
            map(in_="<rename_columns={a=b}>//tmp/tin",
                out="//tmp/tout",
                command="cat")

    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_rename_columns_overlapping_names_in_chunk(self, optimize_for):
        create("table", "//tmp/tin", attributes={
                "optimize_for": optimize_for
                })
        create("table", "//tmp/tout")
        write_table("//tmp/tin", [{"a": 42, "b": 34}])

        # Set weak schema
        sort(in_="//tmp/tin",
             out="//tmp/tin",
             sort_by="a")

        with pytest.raises(YtError):
            map(in_="<rename_columns={a=b}>//tmp/tin",
                out="//tmp/tout",
                command="cat")

    def test_rename_columns_wrong_name(self):
        create("table", "//tmp/tin", attributes={
                "schema": [{"name": "a", "type": "int64"}],
            })
        create("table", "//tmp/tout")
        write_table("//tmp/tin", [{"a": 42}])
        with pytest.raises(YtError):
            map(in_="<rename_columns={a=\"$wrong_name\"}>//tmp/tin",
                out="//tmp/tout",
                command="cat")

        with pytest.raises(YtError):
            map(in_="<rename_columns={a=\"\"}>//tmp/tin",
                out="//tmp/tout",
                command="cat")

        with pytest.raises(YtError):
            map(in_="<rename_columns={a=" + "b" * 1000 + "}>//tmp/tin",
                out="//tmp/tout",
                command="cat")

    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_rename_columns_swap(self, optimize_for):
        create("table", "//tmp/tin", attributes={
                "schema": [{"name": "a", "type": "int64"},
                           {"name": "b", "type": "int64"}],
                "optimize_for": optimize_for
                })
        create("table", "//tmp/tout")
        write_table("//tmp/tin", [{"a": 42, "b": 34}])

        map(in_="<rename_columns={a=b;b=a}>//tmp/tin",
            out="//tmp/tout",
            command="cat")

        assert read_table("//tmp/tout") == [{"b": 42, "a": 34}]

    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_rename_columns_filter(self, optimize_for):
        create("table", "//tmp/tin", attributes={
                "schema": [{"name": "a", "type": "int64"},
                           {"name": "b", "type": "int64"}],
                "optimize_for": optimize_for
                })
        create("table", "//tmp/tout")
        write_table("//tmp/tin", [{"a": 42, "b": 34}])

        map(in_="<rename_columns={a=d}>//tmp/tin{d}",
            out="//tmp/tout",
            command="cat")

        assert read_table("//tmp/tout") == [{"d": 42}]

    @pytest.mark.parametrize("mode", ["unordered", "ordered"])
    def test_computed_columns(self, mode):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2",
            attributes={
                "schema": [
                    {"name": "k1", "type": "int64", "expression": "k2 * 2"},
                    {"name": "k2", "type": "int64"}]
            })

        write_table("//tmp/t1", [{"k2": i} for i in xrange(2)])

        map(
            mode=mode,
            in_="//tmp/t1",
            out="//tmp/t2",
            command="cat")

        assert get("//tmp/t2/@schema_mode") == "strong"
        assert read_table("//tmp/t2") == [{"k1": i * 2, "k2": i} for i in xrange(2)]


    def test_map_max_data_size_per_job(self):
        create("table", "//tmp/t_input")
        create("table", "//tmp/t_output")
        write_table("//tmp/t_input", {"foo": "bar"})

        op = map(
            dont_track=True,
            in_="//tmp/t_input",
            out="//tmp/t_output",
            command='cat',
            spec={
                "max_data_size_per_job": 1
            })

        with pytest.raises(YtError):
            op.track()

    def test_ordered_map_multiple_ranges(self):
        create("table", "//tmp/t_input")
        create("table", "//tmp/t_output")
        rows = [
            {
                "field": 1
            },
            {
                "field": 42
            },
            {
                "field": 63
            },
            {
                "field": 100500
            },
        ]
        write_table("<sorted_by=[field]>//tmp/t_input", rows)

        map(
            ordered=True,
            in_="//tmp/t_input[1,42,63,100500]",
            out="<sorted_by=[field]>//tmp/t_output",
            command='cat')

        assert read_table("//tmp/t_output") == rows
        

    @pytest.mark.parametrize("ordered", [False, True])
    def test_map_interrupt_job(self, ordered):
        create("table", "//tmp/in_1")
        write_table(
            "//tmp/in_1",
            [{"key": "%08d" % i, "value": "(t_1)", "data": "a" * (2 * 1024 * 1024)} for i in range(3)],
            table_writer={
                "block_size": 1024,
                "desired_chunk_size": 1024})

        output = "//tmp/output"
        job_type = "map"
        if ordered:
            output = "<sorted_by=[key]>" + output
            job_type = "ordered_map"
        create("table", output)

        op = map(
            ordered=ordered,
            dont_track=True,
            label="interrupt_job",
            in_="//tmp/in_1",
            out=output,
            command=with_breakpoint("""read; echo "${REPLY/(???)/(job)}"; echo "$REPLY" ; BREAKPOINT ; cat"""),
            spec={
                "mapper": {
                    "format": "dsv"
                },
                "max_failed_job_count": 1,
                "job_io": {
                    "buffer_row_count": 1,
                },
                "enable_job_splitting": False,
            })

        jobs = wait_breakpoint()
        interrupt_job(jobs[0])
        release_breakpoint()
        op.track()

        result = read_table("//tmp/output", verbose=False)
        for row in result:
            print "key:", row["key"], "value:", row["value"]
        assert len(result) == 5
        if not ordered:
            result.sort()
        row_index = 0
        job_indexes = []
        for row in result:
            assert row["key"] == "%08d" % row_index
            if row["value"] == "(job)":
                job_indexes.append(int(row["key"]))
            else:
                row_index += 1
        assert 0 < job_indexes[1] < 99999
        assert get(op.get_path() + "/@progress/job_statistics/data/input/row_count/$/completed/{}/sum".format(job_type)) == len(result) - 2

    # YT-6324: false job interrupt when it does not consume any input data.
    @pytest.mark.parametrize("ordered", [False, True])
    def test_map_no_consumption(self, ordered):
        create("table", "//tmp/in_1")
        write_table(
            "//tmp/in_1",
            [{"key": "%08d" % i, "value": "(t_1)", "data": "a" * (2 * 1024 * 1024)} for i in range(3)],
            table_writer={
                "block_size": 1024,
                "desired_chunk_size": 1024})

        output = "//tmp/output"
        if ordered:
            output = "<sorted_by=[key]>" + output
        create("table", output)

        op = map(
            ordered=ordered,
            dont_track=True,
            label="interrupt_job",
            in_="//tmp/in_1",
            out=output,
            command="true",
            spec={
                "mapper": {
                    "format": "dsv"
                },
                "max_failed_job_count": 1,
                "job_io": {
                    "buffer_row_count": 1,
                },
                "enable_job_splitting": False,
            })
        op.track()

        assert get(op.get_path() + "/@progress/jobs/completed/total") == 1
        assert get(op.get_path() + "/@progress/jobs/completed/non-interrupted") == 1

    def test_ordered_map_many_jobs(self):
        create("table", "//tmp/t_input")
        create("table", "//tmp/t_output")
        original_data = [{"index": i} for i in xrange(10)]
        for row in original_data:
            write_table("<append=true>//tmp/t_input", row)

        op = map(
            in_="//tmp/t_input",
            out="//tmp/t_output",
            command="cat; echo stderr 1>&2",
            ordered=True,
            spec={"data_size_per_job": 1})

        assert get(op.get_path() + "/jobs/@count") == 10
        assert read_table("//tmp/t_output") == original_data

    @pytest.mark.parametrize("with_output_schema", [False, True])
    def test_ordered_map_remains_sorted(self, with_output_schema):
        create("table", "//tmp/t_input", attributes={"schema":[{"name": "key", "sort_order": "ascending", "type": "int64"}]})
        create("table", "//tmp/t_output")
        original_data = [{"key": i} for i in xrange(1000)]
        for i in xrange(10):
            write_table("<append=true>//tmp/t_input", original_data[100*i:100*(i+1)])

        op = map(
            in_="//tmp/t_input",
            out="<sorted_by=[key]>//tmp/t_output" if with_output_schema else "//tmp/t_output",
            command="cat; sleep $((5 - $YT_JOB_INDEX)); echo stderr 1>&2",
            ordered=True,
            spec={"job_count": 5})

        jobs = get(op.get_path() + "/jobs/@count")

        assert jobs == 5
        if with_output_schema:
            assert get("//tmp/t_output/@sorted")
            assert get("//tmp/t_output/@sorted_by") == ["key"]
        assert read_table("//tmp/t_output") == original_data

    # This is a really strange case that was added after YT-7507.
    def test_ordered_map_job_count_consider_only_primary_size(self):
        create("table", "//tmp/t_input")
        create("table", "//tmp/t_output")
        for i in xrange(20):
            write_table("<append=true>//tmp/t_input", [{"a": "x" * 1024 * 1024}])

        op = map(
            in_="//tmp/t_input",
            out="//tmp/t_output",
            ordered=True,
            command="cat >/dev/null; echo stderr 1>&2",
            spec={"job_count":10, "consider_only_primary_size": True})

        jobs = get(op.get_path() + "/jobs/@count")
        assert jobs == 10

    @pytest.mark.parametrize("ordered", [False, True])
    def test_map_job_splitter(self, ordered):
        create("table", "//tmp/in_1")
        write_table(
            "//tmp/in_1",
            [{"key": "%08d" % i, "value": "(t_1)", "data": "a" * (1024 * 1024)} for i in range(20)])

        input_ = "//tmp/in_1"
        output = "//tmp/output"
        create("table", output)

        command = """
while read ROW; do
    if [ "$YT_JOB_INDEX" == 0 ]; then
        sleep 10
    else
        sleep 0.1
    fi
    echo "$ROW"
done
"""

        op = map(
            ordered=ordered,
            dont_track=True,
            label="split_job",
            in_=input_,
            out=output,
            command=command,
            spec={
                "mapper": {
                    "format": "dsv",
                },
                "data_size_per_job": 21 * 1024 * 1024,
                "max_failed_job_count": 1,
                "job_io": {
                    "buffer_row_count": 1,
                },
            })

        op.track()

        completed = get(op.get_path() + "/@progress/jobs/completed")
        interrupted = completed["interrupted"]
        assert completed["total"] >= 2
        assert interrupted["job_split"] >= 1
        expected = read_table("//tmp/in_1", verbose=False)
        for row in expected:
            del row["data"]
        got = read_table(output, verbose=False)
        for row in got:
            del row["data"]
        assert sorted(got) == sorted(expected)

    def test_job_splitter_max_input_table_count(self):
        create("table", "//tmp/in_1")
        write_table(
            "//tmp/in_1",
            [{"key": "%08d" % i, "value": "(t_1)", "data": "a" * (1024 * 1024)} for i in range(20)])

        input_ = "//tmp/in_1"
        output = "//tmp/output"
        create("table", output)

        op = map(
            in_=[input_] * 10,
            out=output,
            command="sleep 5; echo '{a=1}'",
            dont_track=True)
        time.sleep(2.0)
        assert len(get(op.get_path() + "/controller_orchid/job_splitter")) == 0
        op.track()

##################################################################

@patch_porto_env_only(TestSchedulerMapCommands)
class TestSchedulerMapCommandsPorto(YTEnvSetup):
    DELTA_NODE_CONFIG = porto_delta_node_config
    USE_PORTO_FOR_SERVERS = True

##################################################################

class TestSchedulerMapCommandsMulticell(TestSchedulerMapCommands):
    NUM_SECONDARY_MASTER_CELLS = 2

    def test_multicell_input_fetch(self):
        create("table", "//tmp/t1", attributes={"external_cell_tag": 1})
        write_table("//tmp/t1", [{"a": 1}])
        create("table", "//tmp/t2", attributes={"external_cell_tag": 2})
        write_table("//tmp/t2", [{"a": 2}])

        create("table", "//tmp/t_in", attributes={"external": False})
        merge(mode="ordered",
              in_=["//tmp/t1", "//tmp/t2"],
              out="//tmp/t_in")

        create("table", "//tmp/t_out")
        map(in_="//tmp/t_in",
            out="//tmp/t_out",
            command="cat")

        assert_items_equal(read_table("//tmp/t_out"), [{"a": 1}, {"a": 2}])

##################################################################

class TestJobSizeAdjuster(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 5
    NUM_SCHEDULERS = 1

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "map_operation_options": {
              "data_size_per_job": 1
            }
        }
    }

    @pytest.mark.skipif("True", reason="YT-8228")
    def test_map_job_size_adjuster_boost(self):
        create("table", "//tmp/t_input")
        original_data = [{"index": "%05d" % i} for i in xrange(31)]
        for row in original_data:
            write_table("<append=true>//tmp/t_input", row, verbose=False)

        create("table", "//tmp/t_output")

        op = map(
            in_="//tmp/t_input",
            out="//tmp/t_output",
            command="echo lines=`wc -l`",
            spec={
                "mapper": {"format": "dsv"},
                "resource_limits": {"user_slots": 1}
            })

        expected = [{"lines": str(2**i)} for i in xrange(5)]
        actual = read_table("//tmp/t_output")
        assert_items_equal(actual, expected)
        estimated = get(op.get_path() + "/@progress/estimated_input_data_size_histogram")
        histogram = get(op.get_path() + "/@progress/input_data_size_histogram")
        assert estimated == histogram
        assert histogram["max"]/histogram["min"] == 16
        assert histogram["count"][0] == 1
        assert sum(histogram["count"]) == 5

    def test_map_job_size_adjuster_max_limit(self):
        create("table", "//tmp/t_input")
        original_data = [{"index": "%05d" % i} for i in xrange(31)]
        for row in original_data:
            write_table("<append=true>//tmp/t_input", row, verbose=False)
        chunk_id = get_first_chunk_id("//tmp/t_input")
        chunk_size = get("#{0}/@data_weight".format(chunk_id))
        create("table", "//tmp/t_output")

        op = map(
            in_="//tmp/t_input",
            out="//tmp/t_output",
            command="echo lines=`wc -l`",
            spec={
                "mapper": {"format": "dsv"},
                "max_data_size_per_job": chunk_size * 4,
                "resource_limits": {"user_slots": 3}
            })

        for row in read_table("//tmp/t_output"):
            assert int(row["lines"]) < 5

    def test_map_unavailable_chunk(self):
        create("table", "//tmp/t_input", attributes={"replication_factor": 1})
        original_data = [{"index": "%05d" % i} for i in xrange(20)]
        write_table("<append=true>//tmp/t_input", original_data[0], verbose=False)
        chunk_id = get_singular_chunk_id("//tmp/t_input")

        chunk_size = get("#{0}/@uncompressed_data_size".format(chunk_id))
        replicas = get("#{0}/@stored_replicas".format(chunk_id))
        assert len(replicas) == 1
        replica_to_ban = str(replicas[0])  # str() is for attribute stripping.

        banned = False
        for node in ls("//sys/cluster_nodes"):
            if node == replica_to_ban:
                set("//sys/cluster_nodes/{0}/@banned".format(node), True)
                banned = True
        assert banned

        time.sleep(1)
        assert get("#{0}/@replication_status/default/lost".format(chunk_id))

        for row in original_data[1:]:
            write_table("<append=true>//tmp/t_input", row, verbose=False)
        chunk_ids = get("//tmp/t_input/@chunk_ids")
        assert len(chunk_ids) == len(original_data)

        create("table", "//tmp/t_output")
        op = map(dont_track=True,
                 command="sleep $YT_JOB_INDEX; cat",
                 in_="//tmp/t_input",
                 out="//tmp/t_output",
                 spec={
                     "data_size_per_job": chunk_size * 2
                 })

        while True:
            time.sleep(0.2)
            if op.get_job_count("completed") > 3:
                break

        unbanned = False
        for node in ls("//sys/cluster_nodes"):
            if node == replica_to_ban:
                set("//sys/cluster_nodes/{0}/@banned".format(node), False)
                unbanned = True
        assert unbanned

        op.track()
        assert op.get_state() == "completed"

##################################################################

class TestMapOnDynamicTables(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 16
    NUM_SCHEDULERS = 1
    USE_DYNAMIC_TABLES = True

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,
            "operations_update_period": 10,
            "running_jobs_update_period": 10,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "operations_update_period": 10,
            "map_operation_options": {
                "job_splitter": {
                    "min_job_time": 5000,
                    "min_total_data_size": 1024,
                    "update_period": 100,
                    "candidate_percentile": 0.8,
                    "max_jobs_per_split": 3,
                },
            },
        }
    }

    def _create_simple_dynamic_table(self, path, sort_order="ascending", **attributes):
        if "schema" not in attributes:
            attributes.update({"schema": [
                {"name": "key", "type": "int64", "sort_order": sort_order},
                {"name": "value", "type": "string"}]
            })
        create_dynamic_table(path, **attributes)

    @parametrize_external
    @pytest.mark.parametrize("optimize_for", ["lookup", "scan"])
    @pytest.mark.parametrize("sort_order", [None, "ascending"])
    @pytest.mark.parametrize("ordered", [False, True])
    def test_map_on_dynamic_table(self, external, ordered, sort_order, optimize_for):
        sync_create_cells(1)
        self._create_simple_dynamic_table("//tmp/t", sort_order=sort_order, optimize_for=optimize_for, external=external)
        set("//tmp/t/@min_compaction_store_count", 5)
        create("table", "//tmp/t_out")

        rows = [{"key": i, "value": str(i)} for i in range(10)]
        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows)
        sync_unmount_table("//tmp/t")

        map(
            in_="//tmp/t",
            out="//tmp/t_out",
            ordered=ordered,
            command="cat")

        assert_items_equal(read_table("//tmp/t_out"), rows)

        rows1 = [{"key": i, "value": str(i+1)} for i in range(3)]
        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows1)
        sync_unmount_table("//tmp/t")

        rows2 = [{"key": i, "value": str(i+2)} for i in range(2, 6)]
        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows2)
        sync_unmount_table("//tmp/t")

        rows3 = [{"key": i, "value": str(i+3)} for i in range(7, 8)]
        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows3)
        sync_unmount_table("//tmp/t")

        assert len(get("//tmp/t/@chunk_ids")) == 4

        def update(new):
            def update_row(row):
                if sort_order == "ascending":
                    for r in rows:
                        if r["key"] == row["key"]:
                            r["value"] = row["value"]
                            return
                rows.append(row)
            for row in new:
                update_row(row)

        update(rows1)
        update(rows2)
        update(rows3)

        map(
            in_="//tmp/t",
            out="//tmp/t_out",
            ordered=ordered,
            command="cat")

        assert_items_equal(read_table("//tmp/t_out"), rows)

    @parametrize_external
    @pytest.mark.parametrize("optimize_for", ["lookup", "scan"])
    def test_sorted_dynamic_table_as_user_file(self, external, optimize_for):
        sync_create_cells(1)
        self._create_simple_dynamic_table("//tmp/t", optimize_for=optimize_for, external=external)
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        rows = [{"key": i, "value": str(i)} for i in range(5)]
        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows)
        sync_unmount_table("//tmp/t")

        rows1 = [{"key": i, "value": str(i+1)} for i in range(3, 8)]
        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows1)
        sync_unmount_table("//tmp/t")

        write_table("//tmp/t_in", [{"a": "b"}])

        map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            file=["<format=<format=text>yson>//tmp/t"],
            command="cat t",
            spec={
                "mapper": {
                    "format": yson.loads("<format=text>yson")
                }
            })

        def update(new):
            def update_row(row):
                for r in rows:
                    if r["key"] == row["key"]:
                        r["value"] = row["value"]
                        return
                rows.append(row)
            for row in new:
                update_row(row)

        update(rows1)
        rows = sorted(rows, key=lambda r: r["key"])
        assert read_table("//tmp/t_out") == rows

    @parametrize_external
    def test_ordered_dynamic_table_as_user_file(self, external):
        sync_create_cells(1)
        self._create_simple_dynamic_table("//tmp/t", sort_order=None, external=external)
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        rows = [{"key": i, "value": str(i)} for i in range(5)]
        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows)
        sync_unmount_table("//tmp/t")

        rows1 = [{"key": i, "value": str(i+1)} for i in range(3, 8)]
        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows1)
        sync_unmount_table("//tmp/t")

        write_table("//tmp/t_in", [{"a": "b"}])

        map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            file=["<format=<format=text>yson>//tmp/t"],
            command="cat t",
            spec={
                "mapper": {
                    "format": yson.loads("<format=text>yson")
                }
            })

        assert read_table("//tmp/t_out") == rows + rows1

    @parametrize_external
    def test_dynamic_table_timestamp(self, external):
        sync_create_cells(1)
        self._create_simple_dynamic_table("//tmp/t", external=external)
        create("table", "//tmp/t_out")

        rows = [{"key": i, "value": str(i)} for i in range(2)]
        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows)

        time.sleep(1)
        ts = generate_timestamp()

        sync_flush_table("//tmp/t")
        insert_rows("//tmp/t", [{"key": i, "value": str(i+1)} for i in range(2)])
        sync_flush_table("//tmp/t")
        sync_compact_table("//tmp/t")

        map(
            in_="<timestamp=%s>//tmp/t" % ts,
            out="//tmp/t_out",
            command="cat")

        assert_items_equal(read_table("//tmp/t_out"), rows)

        with pytest.raises(YtError):
            map(
                in_="<timestamp=%s>//tmp/t" % MinTimestamp,
                out="//tmp/t_out",
                command="cat")

        insert_rows("//tmp/t", rows)

        with pytest.raises(YtError):
            map(
                in_="<timestamp=%s>//tmp/t" % generate_timestamp(),
                out="//tmp/t_out",
                command="cat")

    @parametrize_external
    @pytest.mark.parametrize("optimize_for", ["lookup", "scan"])
    def test_dynamic_table_input_data_statistics(self, external, optimize_for):
        sync_create_cells(1)
        self._create_simple_dynamic_table("//tmp/t", optimize_for=optimize_for, external=external)
        create("table", "//tmp/t_out")

        rows = [{"key": i, "value": str(i)} for i in range(2)]
        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows)
        sync_unmount_table("//tmp/t")

        op = map(
            in_="//tmp/t",
            out="//tmp/t_out",
            command="cat")

        statistics = get(op.get_path() + "/@progress/job_statistics")
        assert get_statistics(statistics, "data.input.chunk_count.$.completed.map.sum") == 1
        assert get_statistics(statistics, "data.input.row_count.$.completed.map.sum") == 2
        assert get_statistics(statistics, "data.input.uncompressed_data_size.$.completed.map.sum") > 0
        assert get_statistics(statistics, "data.input.compressed_data_size.$.completed.map.sum") > 0
        assert get_statistics(statistics, "data.input.data_weight.$.completed.map.sum") > 0

    @parametrize_external
    @pytest.mark.parametrize("optimize_for", ["lookup", "scan"])
    def test_dynamic_table_column_filter(self, optimize_for, external):
        sync_create_cells(1)
        create("table", "//tmp/t", attributes={
            "external": external,
            "optimize_for": optimize_for,
            "schema": make_schema([
                {"name": "k", "type": "int64", "sort_order": "ascending"},
                {"name": "u", "type": "int64"},
                {"name": "v", "type": "int64"}
            ],
            unique_keys=True)
        })
        create("table", "//tmp/t_out")

        row = {"k": 0, "u": 1, "v": 2}
        write_table("//tmp/t", [row])
        alter_table("//tmp/t", dynamic=True)

        def get_data_size(statistics):
            return {
                "uncompressed_data_size": get_statistics(statistics, "data.input.uncompressed_data_size.$.completed.map.sum"),
                "compressed_data_size": get_statistics(statistics, "data.input.compressed_data_size.$.completed.map.sum")
            }

        op = map(
            in_="//tmp/t",
            out="//tmp/t_out",
            command="cat")
        stat1 = get_data_size(get(op.get_path() + "/@progress/job_statistics"))
        assert read_table("//tmp/t_out") == [row]

        # FIXME(savrus) investigate test flapping
        print get("//tmp/t/@compression_statistics")

        for columns in (["k"], ["u"], ["v"], ["k", "u"], ["k", "v"], ["u", "v"]):
            op = map(
                in_="<columns=[{0}]>//tmp/t".format(";".join(columns)),
                out="//tmp/t_out",
                command="cat")
            stat2 = get_data_size(get(op.get_path() + "/@progress/job_statistics"))
            assert read_table("//tmp/t_out") == [{c: row[c] for c in columns}]

            if columns == ["u", "v"] or optimize_for == "lookup":
                assert stat1["uncompressed_data_size"] == stat2["uncompressed_data_size"]
                assert stat1["compressed_data_size"] == stat2["compressed_data_size"]
            else:
                assert stat1["uncompressed_data_size"] > stat2["uncompressed_data_size"]
                assert stat1["compressed_data_size"] > stat2["compressed_data_size"]

    @parametrize_external
    def test_output_to_dynamic_table_fails(self, external):
        create("table", "//tmp/t_input")
        self._create_simple_dynamic_table("//tmp/t_output", external=external)

        with pytest.raises(YtError):
            map(
                in_="//tmp/t_input",
                out="//tmp/t_output",
                command="cat")

    @pytest.mark.parametrize("optimize_for", ["lookup", "scan"])
    def test_rename_columns_dynamic_table_simple(self, optimize_for):
        sync_create_cells(1)
        self._create_simple_dynamic_table("//tmp/t", optimize_for=optimize_for)
        create("table", "//tmp/t_out")

        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", [{"key": 1, "value": str(2)}])
        sync_unmount_table("//tmp/t")

        op = map(
            in_="<rename_columns={key=first;value=second}>//tmp/t",
            out="//tmp/t_out",
            command="cat")

        assert read_table("//tmp/t_out") == [{"first": 1, "second": str(2)}]

##################################################################

class TestMapOnDynamicTablesMulticell(TestMapOnDynamicTables):
    NUM_SECONDARY_MASTER_CELLS = 2

##################################################################

@patch_porto_env_only(TestMapOnDynamicTables)
class TestMapOnDynamicTablesPorto(YTEnvSetup):
    DELTA_NODE_CONFIG = porto_delta_node_config
    USE_PORTO_FOR_SERVERS = True

##################################################################

class TestInputOutputFormats(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 16
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,
            "operations_update_period": 10,
            "running_jobs_update_period": 10,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "operations_update_period": 10,
            "map_operation_options": {
                "job_splitter": {
                    "min_job_time": 5000,
                    "min_total_data_size": 1024,
                    "update_period": 100,
                    "candidate_percentile": 0.8,
                    "max_jobs_per_split": 3,
                },
            },
        }
    }

    @unix_only
    def test_tskv_input_format(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", {"foo": "bar"})

        mapper = \
"""
import sys
input = sys.stdin.readline().strip('\\n').split('\\t')
assert input == ['tskv', 'foo=bar']
print '{hello=world}'

"""
        create("file", "//tmp/mapper.sh")
        write_file("//tmp/mapper.sh", mapper)

        create("table", "//tmp/t_out")
        map(in_="//tmp/t_in",
            out="//tmp/t_out",
            command="python mapper.sh",
            file="//tmp/mapper.sh",
            spec={"mapper": {"input_format": yson.loads("<line_prefix=tskv>dsv")}})

        assert read_table("//tmp/t_out") == [{"hello": "world"}]

    @unix_only
    def test_tskv_output_format(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", {"foo": "bar"})

        mapper = \
"""
import sys
input = sys.stdin.readline().strip('\\n')
assert input == '<"table_index"=0;>#;'
input = sys.stdin.readline().strip('\\n')
assert input == '{"foo"="bar";};'
print "tskv" + "\\t" + "hello=world"
"""
        create("file", "//tmp/mapper.sh")
        write_file("//tmp/mapper.sh", mapper)

        create("table", "//tmp/t_out")
        map(in_="//tmp/t_in",
            out="//tmp/t_out",
            command="python mapper.sh",
            file="//tmp/mapper.sh",
            spec={"mapper": {
                "enable_input_table_index": True,
                "input_format": yson.loads("<format=text>yson"),
                "output_format": yson.loads("<line_prefix=tskv>dsv")
            }})

        assert read_table("//tmp/t_out") == [{"hello": "world"}]

    @unix_only
    def test_yamr_output_format(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", {"foo": "bar"})

        mapper = \
"""
import sys
input = sys.stdin.readline().strip('\\n')
assert input == '{"foo"="bar";};'
print "key\\tsubkey\\tvalue"

"""
        create("file", "//tmp/mapper.py")
        write_file("//tmp/mapper.py", mapper)

        create("table", "//tmp/t_out")
        map(in_="//tmp/t_in",
            out="//tmp/t_out",
            command="python mapper.py",
            file="//tmp/mapper.py",
            spec={"mapper": {
                "input_format": yson.loads("<format=text>yson"),
                "output_format": yson.loads("<has_subkey=true>yamr")
            }})

        assert read_table("//tmp/t_out") == [{"key": "key", "subkey": "subkey", "value": "value"}]

    @unix_only
    def test_yamr_input_format(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", {"value": "value", "subkey": "subkey", "key": "key", "a": "another"})

        mapper = \
"""
import sys
input = sys.stdin.readline().strip('\\n').split('\\t')
assert input == ['key', 'subkey', 'value']
print '{hello=world}'

"""
        create("file", "//tmp/mapper.sh")
        write_file("//tmp/mapper.sh", mapper)

        create("table", "//tmp/t_out")
        map(in_="//tmp/t_in",
            out="//tmp/t_out",
            command="python mapper.sh",
            file="//tmp/mapper.sh",
            spec={"mapper": {"input_format": yson.loads("<has_subkey=true>yamr")}})

        assert read_table("//tmp/t_out") == [{"hello": "world"}]

    def test_type_conversion(self):
        create("table", "//tmp/s")
        write_table("//tmp/s", {"foo": "42"})

        create("table", "//tmp/t",
               attributes={
                   "schema": make_schema([
                       {"name": "int64", "type": "int64", "sort_order": "ascending"},
                       {"name": "uint64", "type": "uint64"},
                       {"name": "boolean", "type": "boolean"},
                       {"name": "double", "type": "double"},
                       {"name": "any", "type": "any"}],
                       strict=False)
               })

        row = '{int64=3u; uint64=42; boolean="false"; double=18; any={}; extra=qwe}'

        with pytest.raises(YtError):
            map(in_="//tmp/s",
                out="//tmp/t",
                command="echo '{0}'".format(row),
                spec={"max_failed_job_count": 1})

        yson_with_type_conversion = yson.loads("<enable_type_conversion=%true>yson")
        map(in_="//tmp/s",
            out="//tmp/t",
            command="echo '{0}'".format(row), format=yson_with_type_conversion,
            spec={"max_failed_job_count": 1, "mapper": {"output_format": yson_with_type_conversion}})

    #@unix_only
    def test_invalid_row_indices(self):
        create("table", "//tmp/t_in")
        write_table("<append=%true>//tmp/t_in", [{"a": i} for i in range(10)])
        write_table("<append=%true>//tmp/t_in", [{"a": i} for i in range(10, 20)])

        create("table", "//tmp/t_out")

        # None of this operations should fail.

        map(in_="//tmp/t_in[#18:#2]",
            out="//tmp/t_out",
            command="cat")

        map(in_="//tmp/t_in[#8:#2]",
            out="//tmp/t_out",
            command="cat")

        map(in_="//tmp/t_in[#18:#12]",
            out="//tmp/t_out",
            command="cat")

        map(in_="//tmp/t_in[#8:#8]",
            out="//tmp/t_out",
            command="cat")

        map(in_="//tmp/t_in[#10:#10]",
            out="//tmp/t_out",
            command="cat")

        map(in_="//tmp/t_in[#12:#12]",
            out="//tmp/t_out",
            command="cat")


##################################################################

class TestInputOutputFormatsMulticell(TestInputOutputFormats):
    NUM_SECONDARY_MASTER_CELLS = 2
