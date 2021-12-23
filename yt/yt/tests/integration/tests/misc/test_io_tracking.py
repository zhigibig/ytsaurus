from yt_env_setup import YTEnvSetup

import yt_commands

from yt_commands import (
    alter_table, authors, create, get, read_journal, wait, read_table, write_journal, create_account,
    write_table, update_nodes_dynamic_config, get_singular_chunk_id, set_node_banned,
    sync_create_cells, create_dynamic_table, sync_mount_table, insert_rows, sync_unmount_table)

from yt_helpers import read_structured_log, write_log_barrier
from yt_driver_bindings import Driver

import yt.yson as yson

from copy import deepcopy

import random
import time
import pytest

##################################################################


class TestNodeIOTrackingBase(YTEnvSetup):
    def setup(self):
        update_nodes_dynamic_config({
            "data_node": {
                "io_tracker": {
                    "enable": True,
                    "enable_raw": True,
                    "period_quant": 10,
                    "aggregation_period": 5000,
                }
            }
        })

    def get_structured_log_path(self, node_id=0):
        return "{}/logs/node-{}.json.log".format(self.path_to_run, node_id)

    def get_node_address(self, node_id=0):
        return "localhost:" + str(self.Env.configs["node"][node_id]["rpc_port"])

    def _default_filter(self, event):
        return event.get("user@") != "scheduler"

    def write_log_barrier(self, *args, **kwargs):
        return write_log_barrier(*args, **kwargs)

    def read_events(self, from_barrier=None, to_barrier=None, node_id=0, filter=lambda _: True):
        # NB. We need to filter out the IO generated by internal cluster processes. For example, scheduler
        # sometimes writes to //sys/scheduler/event_log. Such events are also logged in the data node and
        # may lead to unexpected test failures.
        real_filter = lambda event: self._default_filter(event) and filter(event)
        raw_events = read_structured_log(
            self.get_structured_log_path(node_id), from_barrier, to_barrier,
            category_filter={"IORaw"}, row_filter=real_filter)
        aggregate_events = read_structured_log(
            self.get_structured_log_path(node_id), from_barrier, to_barrier,
            category_filter={"IOAggregate"}, row_filter=real_filter)
        return raw_events, aggregate_events

    def wait_for_events(self, raw_count=None, aggregate_count=None, from_barrier=None, node_id=0,
                        filter=lambda _: True, check_event_count=True):
        def is_ready():
            to_barrier = self.write_log_barrier(self.get_node_address(node_id))
            raw_events, aggregate_events = self.read_events(from_barrier, to_barrier, node_id, filter)
            return (raw_count is None or raw_count <= len(raw_events)) and \
                   (aggregate_count is None or aggregate_count <= len(aggregate_events))

        wait(is_ready)
        to_barrier = self.write_log_barrier(self.get_node_address(node_id=node_id))
        raw_events, aggregate_events = self.read_events(from_barrier, to_barrier, node_id, filter)
        if check_event_count:
            assert raw_count is None or raw_count == len(raw_events)
            assert aggregate_count is None or aggregate_count == len(aggregate_events)
        return raw_events, aggregate_events

    def generate_large_data(self, row_len=50000, row_count=5):
        rnd = random.Random(42)
        # NB. The values are chosen in such a way so they cannot be compressed or deduplicated.
        large_data = [{
            "id": i,
            "data": bytes(bytearray([rnd.randint(0, 255) for _ in range(row_len)]))
        } for i in range(row_count)]
        large_data_size = row_count * row_len
        return large_data, large_data_size

    def generate_large_journal(self, row_len=50000, row_count=5):
        rnd = random.Random(42)
        # NB. The values are chosen in such a way so they cannot be compressed or deduplicated.
        large_journal = [{"data": bytes(bytearray([rnd.randint(0, 255) for _ in range(row_len)]))} for _ in range(row_count)]
        large_journal_size = row_count * row_len
        return large_journal, large_journal_size

##################################################################


class TestDataNodeIOTracking(TestNodeIOTrackingBase):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    DELTA_MASTER_CONFIG = {
        "cypress_manager": {
            "default_table_replication_factor": 1,
            "default_journal_read_quorum": 1,
            "default_journal_write_quorum": 1,
            "default_journal_replication_factor": 1,
        }
    }

    @authors("gepardo")
    def test_simple_write(self):
        from_barrier = write_log_barrier(self.get_node_address())
        create("table", "//tmp/table")
        write_table("//tmp/table", [{"a": 1, "b": 2, "c": 3}])
        raw_events, aggregate_events = self.wait_for_events(raw_count=1, aggregate_count=1, from_barrier=from_barrier)

        assert raw_events[0]["data_node_method@"] == "FinishChunk"
        assert raw_events[0]["direction@"] == "write"
        for counter in ["byte_count", "io_count"]:
            assert raw_events[0][counter] > 0 and raw_events[0][counter] == aggregate_events[0][counter]

    @authors("gepardo")
    def test_two_chunks(self):
        from_barrier = write_log_barrier(self.get_node_address())
        create("table", "//tmp/table")
        write_table("//tmp/table", [{"number": 42, "good": True}])
        write_table("<append=%true>//tmp/table", [{"number": 43, "good": False}])
        raw_events, aggregate_events = self.wait_for_events(raw_count=2, aggregate_count=1, from_barrier=from_barrier)

        assert raw_events[0]["data_node_method@"] == "FinishChunk"
        assert raw_events[0]["direction@"] == "write"
        assert raw_events[1]["data_node_method@"] == "FinishChunk"
        assert raw_events[1]["direction@"] == "write"
        for counter in ["byte_count", "io_count"]:
            assert raw_events[0][counter] > 0
            assert raw_events[1][counter] > 0
            assert raw_events[0][counter] + raw_events[1][counter] == aggregate_events[0][counter]

    @authors("gepardo")
    def test_read_table(self):
        from_barrier = write_log_barrier(self.get_node_address())
        create("table", "//tmp/table")
        write_table("//tmp/table", [{"a": 1, "b": 2, "c": 3}])
        assert read_table("//tmp/table") == [{"a": 1, "b": 2, "c": 3}]
        raw_events, _ = self.wait_for_events(raw_count=2, from_barrier=from_barrier)

        assert raw_events[0]["data_node_method@"] == "FinishChunk"
        assert raw_events[0]["direction@"] == "write"
        assert raw_events[1]["data_node_method@"] == "GetBlockSet"
        assert raw_events[1]["direction@"] == "read"
        for counter in ["byte_count", "io_count"]:
            assert raw_events[0][counter] > 0
            assert raw_events[1][counter] > 0

    @authors("gepardo")
    def test_large_data(self):
        create("table", "//tmp/table")

        for i in range(10):
            large_data, large_data_size = self.generate_large_data()

            old_disk_space = get("//tmp/table/@resource_usage/disk_space")
            from_barrier = write_log_barrier(self.get_node_address())
            write_table("<append=%true>//tmp/table", large_data)
            _, aggregate_events = self.wait_for_events(aggregate_count=1, from_barrier=from_barrier)
            new_disk_space = get("//tmp/table/@resource_usage/disk_space")

            min_data_bound = 0.95 * large_data_size
            max_data_bound = 1.05 * (new_disk_space - old_disk_space)

            assert aggregate_events[0]["data_node_method@"] == "FinishChunk"
            assert aggregate_events[0]["direction@"] == "write"
            assert min_data_bound <= aggregate_events[0]["byte_count"] <= max_data_bound
            assert aggregate_events[0]["io_count"] > 0

            from_barrier = write_log_barrier(self.get_node_address())
            assert read_table("//tmp/table[#{}:]".format(i * len(large_data))) == large_data
            _, aggregate_events = self.wait_for_events(aggregate_count=1, from_barrier=from_barrier)

            assert aggregate_events[0]["data_node_method@"] == "GetBlockSet"
            assert aggregate_events[0]["direction@"] == "read"
            assert min_data_bound <= aggregate_events[0]["byte_count"] <= max_data_bound
            assert aggregate_events[0]["io_count"] > 0

    @authors("gepardo")
    def test_journal(self):
        data = [{"data":  str(i)} for i in range(20)]

        from_barrier = write_log_barrier(self.get_node_address())
        create("journal", "//tmp/journal")
        write_journal("//tmp/journal", data)
        raw_events, _ = self.wait_for_events(raw_count=1, from_barrier=from_barrier)

        assert raw_events[0]["data_node_method@"] == "FlushBlocks"
        assert raw_events[0]["direction@"] == "write"
        assert raw_events[0]["byte_count"] > 0
        assert raw_events[0]["io_count"] > 0

        from_barrier = write_log_barrier(self.get_node_address())
        assert read_journal("//tmp/journal") == data
        raw_events, _ = self.wait_for_events(raw_count=1, from_barrier=from_barrier)

        assert raw_events[0]["data_node_method@"] == "GetBlockRange"
        assert raw_events[0]["direction@"] == "read"
        assert raw_events[0]["byte_count"] > 0
        assert raw_events[0]["io_count"] > 0

    @authors("gepardo")
    def test_large_journal(self):
        create("journal", "//tmp/journal")

        for i in range(10):
            large_journal, large_journal_size = self.generate_large_journal(row_len=200000)
            min_data_bound = 0.9 * large_journal_size
            max_data_bound = 1.1 * large_journal_size

            from_barrier = write_log_barrier(self.get_node_address())
            write_journal("//tmp/journal", large_journal)
            raw_events, _ = self.wait_for_events(raw_count=1, from_barrier=from_barrier)

            assert raw_events[0]["data_node_method@"] == "FlushBlocks"
            assert raw_events[0]["direction@"] == "write"
            assert min_data_bound <= raw_events[0]["byte_count"] <= max_data_bound
            assert raw_events[0]["io_count"] > 0

            from_barrier = write_log_barrier(self.get_node_address())
            read_result = read_journal("//tmp/journal[#{}:#{}]".format(i * len(large_journal), (i + 1) * len(large_journal)))
            assert read_result == large_journal
            raw_events, _ = self.wait_for_events(raw_count=1, from_barrier=from_barrier)

            assert raw_events[0]["data_node_method@"] == "GetBlockRange"
            assert raw_events[0]["direction@"] == "read"
            assert min_data_bound <= raw_events[0]["byte_count"] <= max_data_bound
            assert raw_events[0]["io_count"] > 0

##################################################################


class TestDataNodeErasureIOTracking(TestNodeIOTrackingBase):
    NUM_MASTERS = 1
    NUM_NODES = 6
    NUM_SCHEDULERS = 1

    def _check_data_read(self, from_barriers, method):
        was_data_read = False
        for node_id in range(self.NUM_NODES):
            raw_events, _ = self.read_events(from_barrier=from_barriers[node_id], node_id=node_id)
            if not raw_events:
                continue
            assert len(raw_events) == 1
            assert raw_events[0]["data_node_method@"] == method
            assert raw_events[0]["byte_count"] > 0
            assert raw_events[0]["io_count"] > 0
            was_data_read = True
        assert was_data_read

    @authors("gepardo")
    def test_erasure_blob_chunks(self):
        data = [{"a": i, "b": 2 * i, "c": 3 * i} for i in range(100)]

        from_barriers = [write_log_barrier(self.get_node_address(node_id)) for node_id in range(self.NUM_NODES)]
        create("table", "//tmp/table", attributes={"erasure_codec": "reed_solomon_3_3"})
        write_table("//tmp/table", data)

        for node_id in range(self.NUM_NODES):
            raw_events, _ = self.wait_for_events(raw_count=1, node_id=node_id, from_barrier=from_barriers[node_id])

            assert raw_events[0]["data_node_method@"] == "FinishChunk"
            assert raw_events[0]["byte_count"] > 0
            assert raw_events[0]["io_count"] > 0

        from_barriers = [write_log_barrier(self.get_node_address(node_id)) for node_id in range(self.NUM_NODES)]
        assert read_table("//tmp/table") == data
        time.sleep(1.0)
        self._check_data_read(from_barriers, "GetBlockSet")

    @authors("gepardo")
    def test_erasure_journal_chunks(self):
        data = [{"data": str(i)} for i in range(20)]

        from_barriers = [write_log_barrier(self.get_node_address(node_id)) for node_id in range(self.NUM_NODES)]
        create("journal", "//tmp/journal", attributes={
            "erasure_codec": "reed_solomon_3_3",
            "replication_factor": 1,
            "read_quorum": 6,
            "write_quorum": 6,
        })
        write_journal("//tmp/journal", data)

        for node_id in range(self.NUM_NODES):
            raw_events, _ = self.wait_for_events(raw_count=1, node_id=node_id, from_barrier=from_barriers[node_id])

            assert raw_events[0]["data_node_method@"] == "FlushBlocks"
            assert raw_events[0]["byte_count"] > 0
            assert raw_events[0]["io_count"] > 0

        from_barriers = [write_log_barrier(self.get_node_address(node_id)) for node_id in range(self.NUM_NODES)]
        assert read_journal("//tmp/journal") == data
        time.sleep(1.0)
        self._check_data_read(from_barriers, "GetBlockRange")

##################################################################


class TestMasterJobsIOTracking(TestNodeIOTrackingBase):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    def _wait_for_merge(self, table_path, merge_mode, account="tmp"):
        yt_commands.set("//sys/@config/chunk_manager/chunk_merger/enable", True)

        rows = read_table(table_path)
        assert get("{}/@resource_usage/chunk_count".format(table_path)) > 1

        yt_commands.set("{}/@chunk_merger_mode".format(table_path), merge_mode)
        yt_commands.set("//sys/accounts/{}/@merge_job_rate_limit".format(account), 10)
        yt_commands.set("//sys/accounts/{}/@chunk_merger_node_traversal_concurrency".format(account), 1)
        wait(lambda: get("{}/@resource_usage/chunk_count".format(table_path)) == 1)
        assert read_table(table_path) == rows

    @authors("gepardo")
    def test_replicate_chunk_writes(self):
        from_barriers = [write_log_barrier(self.get_node_address(node_id)) for node_id in range(self.NUM_NODES)]
        create("table", "//tmp/table", attributes={"replication_factor": self.NUM_NODES})
        write_table("//tmp/table", [{"a": 1, "b": 2, "c": 3}])

        has_replication_job = False
        for node_id in range(self.NUM_NODES):
            raw_events, _ = self.wait_for_events(
                raw_count=1, from_barrier=from_barriers[node_id], node_id=node_id,
                filter=lambda event: event.get("data_node_method@") == "FinishChunk")
            if "job_type@" not in raw_events[0]:
                continue
            has_replication_job = True
            assert raw_events[0]["job_type@"] == "ReplicateChunk"
            assert "job_id" in raw_events[0]
            assert raw_events[0]["byte_count"] > 0
            assert raw_events[0]["io_count"] > 0

        assert has_replication_job

    @authors("gepardo")
    def test_large_replicate(self):
        large_data, large_data_size = self.generate_large_data()

        from_barriers = [write_log_barrier(self.get_node_address(node_id)) for node_id in range(self.NUM_NODES)]
        create("table", "//tmp/table", attributes={"replication_factor": self.NUM_NODES})
        write_table("//tmp/table", large_data)

        disk_space = get("//tmp/table/@resource_usage/disk_space")
        min_data_bound = 0.95 * large_data_size
        max_data_bound = 1.05 * disk_space // self.NUM_NODES
        assert min_data_bound < max_data_bound

        events = []

        has_replication_write_job = False
        for node_id in range(self.NUM_NODES):
            raw_events, _ = self.wait_for_events(
                raw_count=1, from_barrier=from_barriers[node_id], node_id=node_id,
                filter=lambda event: event.get("data_node_method@") == "FinishChunk")
            event = raw_events[0]
            if "job_type@" not in event:
                continue
            has_replication_write_job = True
            events.append(event)
        assert has_replication_write_job

        time.sleep(3.0)

        has_replication_read_job = False
        for node_id in range(self.NUM_NODES):
            raw_events, _ = self.read_events(from_barrier=from_barriers[node_id], node_id=node_id)
            for event in raw_events:
                if "job_type@" in event and "data_node_method@" not in event:
                    has_replication_read_job = True
                    events.append(event)
                    break
        assert has_replication_read_job

        for event in events:
            assert event["job_type@"] == "ReplicateChunk"
            assert "job_id" in event
            assert min_data_bound <= event["byte_count"] <= max_data_bound

    @authors("gepardo")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_merge_chunks(self, merge_mode):
        from_barrier = write_log_barrier(self.get_node_address(node_id=0))

        create("table", "//tmp/table")
        write_table("<append=true>//tmp/table", {"name": "cheetah", "type": "cat"})
        write_table("<append=true>//tmp/table", {"name": "fox", "type": "dog"})
        write_table("<append=true>//tmp/table", {"name": "wolf", "type": "dog"})
        write_table("<append=true>//tmp/table", {"name": "tiger", "type": "cat"})

        self._wait_for_merge("//tmp/table", merge_mode)

        _, aggregate_events = self.wait_for_events(
            aggregate_count=1, from_barrier=from_barrier, node_id=0,
            filter=lambda event: event.get("job_type@") == "MergeChunks" and event.get("data_node_method@") == "FinishChunk")
        assert aggregate_events[0]["byte_count"] > 0
        assert aggregate_events[0]["io_count"] > 0

    @authors("gepardo")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_large_merge(self, merge_mode):
        large_data, large_data_size = self.generate_large_data()

        from_barrier = write_log_barrier(self.get_node_address(node_id=0))

        create("table", "//tmp/table")
        for row in large_data:
            write_table("<append=true>//tmp/table", row)

        disk_space = get("//tmp/table/@resource_usage/disk_space")
        min_data_bound = 0.95 * large_data_size
        max_data_bound = 1.05 * disk_space // self.NUM_NODES
        assert min_data_bound < max_data_bound

        self._wait_for_merge("//tmp/table", merge_mode)

        _, aggregate_events = self.wait_for_events(
            aggregate_count=1, from_barrier=from_barrier, node_id=0,
            filter=lambda event: event.get("job_type@") == "MergeChunks" and event.get("data_node_method@") == "FinishChunk")
        assert min_data_bound <= aggregate_events[0]["byte_count"] <= max_data_bound
        assert aggregate_events[0]["io_count"] > 0


class TestRepairMasterJobIOTracking(TestNodeIOTrackingBase):
    NUM_MASTERS = 1
    # We need six nodes to store chunks for reed_solomon_3_3 and one extra node to store repaired chunk.
    NUM_NODES = 7
    NUM_SCHEDULERS = 1

    @authors("gepardo")
    def test_repair_chunk(self):
        from_barriers = [write_log_barrier(self.get_node_address(node_id)) for node_id in range(self.NUM_NODES)]

        create("table", "//tmp/table", attributes={"erasure_codec": "reed_solomon_3_3"})
        write_table("//tmp/table", [{"a": i, "b": 2 * i, "c": 3 * i} for i in range(100)])

        chunk_id = get_singular_chunk_id("//tmp/table")
        replicas = get("#{0}/@stored_replicas".format(chunk_id))
        address_to_ban = str(replicas[3])
        set_node_banned(address_to_ban, True)
        time.sleep(3.0)

        read_result = read_table("//tmp/table",
                                 table_reader={
                                     "unavailable_chunk_strategy": "restore",
                                     "pass_count": 1,
                                     "retry_count": 1,
                                 })
        assert read_result == [{"a": i, "b": 2 * i, "c": 3 * i} for i in range(100)]

        replicas = set(map(str, replicas))
        new_replicas = set(map(str, get("#{0}/@stored_replicas".format(chunk_id))))

        has_repaired_replica = True
        for node_id in range(self.NUM_NODES):
            address = self.get_node_address(node_id)
            if address in new_replicas and address not in replicas:
                has_repaired_replica = True
                raw_events, _ = self.wait_for_events(
                    raw_count=1, from_barrier=from_barriers[node_id], node_id=node_id,
                    filter=lambda event: "job_type@" in event)
                assert len(raw_events) == 1
                assert "job_id" in raw_events[0]
                assert raw_events[0]["byte_count"] > 0
                assert raw_events[0]["io_count"] > 0

        assert has_repaired_replica

        set_node_banned(address_to_ban, False)

##################################################################


class TestClientIOTracking(TestNodeIOTrackingBase):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1
    USE_DYNAMIC_TABLES = True

    DELTA_MASTER_CONFIG = {
        "cypress_manager": {
            "default_table_replication_factor": 1,
            "default_journal_read_quorum": 1,
            "default_journal_write_quorum": 1,
            "default_journal_replication_factor": 1,
        }
    }

    def _get_proxy_type(self):
        return "http"

    @authors("gepardo")
    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_write_static_table(self, optimize_for):
        data1, data_size = self.generate_large_data()
        data2, _ = self.generate_large_data()

        create_account("gepardo")
        create_account("some_other_account")

        create("table", "//tmp/table1", attributes={"optimize_for": optimize_for})
        yt_commands.set("//tmp/table1/@account", "gepardo")

        create("table", "//tmp/table2", attributes={"optimize_for": optimize_for})
        yt_commands.set("//tmp/table2/@account", "some_other_account")

        from_barrier = self.write_log_barrier(self.get_node_address(), "Barrier")
        write_table("//tmp/table1", data1)
        write_table("//tmp/table2", data2)
        raw_events, _ = self.wait_for_events(raw_count=2, from_barrier=from_barrier,
                                             filter=lambda event: event.get("data_node_method@") == "FinishChunk")

        raw_events.sort(key=lambda event: event["object_path"])
        event1, event2 = raw_events

        disk_space = get("//tmp/table1/@resource_usage/disk_space")
        min_data_bound = 0.95 * data_size
        max_data_bound = 1.05 * disk_space
        assert min_data_bound < max_data_bound

        assert min_data_bound <= event1["byte_count"] <= max_data_bound
        assert event1["io_count"] > 0
        assert event1["account@"] == "gepardo"
        assert event1["object_path"] == "//tmp/table1"
        assert event1["api_method@"] == "write_table"
        assert event1["proxy_type@"] == self._get_proxy_type()
        assert "object_id" in event1

        assert min_data_bound <= event2["byte_count"] <= max_data_bound
        assert event2["io_count"] > 0
        assert event2["account@"] == "some_other_account"
        assert event2["object_path"] == "//tmp/table2"
        assert event2["api_method@"] == "write_table"
        assert event2["proxy_type@"] == self._get_proxy_type()
        assert "object_id" in event2

    @authors("gepardo")
    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_read_static_table(self, optimize_for):
        data1, data_size = self.generate_large_data()
        data2, _ = self.generate_large_data()

        create_account("gepardo")
        create_account("some_other_account")

        create("table", "//tmp/table1", attributes={"optimize_for": optimize_for})
        create("table", "//tmp/table2", attributes={"optimize_for": optimize_for})
        write_table("//tmp/table1", data1)
        write_table("//tmp/table2", data2)
        yt_commands.set("//tmp/table1/@account", "gepardo")
        yt_commands.set("//tmp/table2/@account", "some_other_account")

        from_barrier = self.write_log_barrier(self.get_node_address(), "Barrier")
        assert read_table("//tmp/table2") == data2
        assert read_table("//tmp/table1") == data1
        raw_events, _ = self.wait_for_events(raw_count=2, from_barrier=from_barrier,
                                             filter=lambda event: event.get("data_node_method@") == "GetBlockSet")

        raw_events.sort(key=lambda event: event["object_path"])
        event1, event2 = raw_events

        disk_space = get("//tmp/table1/@resource_usage/disk_space")
        min_data_bound = 0.95 * data_size
        max_data_bound = 1.05 * disk_space
        assert min_data_bound < max_data_bound

        assert min_data_bound <= event1["byte_count"] <= max_data_bound
        assert event1["io_count"] > 0
        assert event1["account@"] == "gepardo"
        assert event1["object_path"] == "//tmp/table1"
        assert event1["api_method@"] == "read_table"
        assert event1["proxy_type@"] == self._get_proxy_type()
        assert "object_id" in event1

        assert min_data_bound <= event2["byte_count"] <= max_data_bound
        assert event2["io_count"] > 0
        assert event2["account@"] == "some_other_account"
        assert event2["object_path"] == "//tmp/table2"
        assert event2["api_method@"] == "read_table"
        assert event2["proxy_type@"] == self._get_proxy_type()
        assert "object_id" in event2

    @authors("gepardo")
    def test_read_static_table_range(self):
        data = [
            {"row": "first", "cat": "lion"},
            {"row": "second", "cat": "tiger"},
            {"row": "third", "cat": "panthera"},
            {"row": "fourth", "cat": "leopard"},
        ]

        create_account("gepardo")
        create("table", "//tmp/table")
        yt_commands.set("//tmp/table/@account", "gepardo")
        write_table("//tmp/table", data)

        from_barrier = self.write_log_barrier(self.get_node_address(), "Barrier")
        assert read_table("//tmp/table[#1:#3]") == data[1:3]
        raw_events, _ = self.wait_for_events(raw_count=1, from_barrier=from_barrier,
                                             filter=lambda event: event.get("data_node_method@") == "GetBlockSet")

        event = raw_events[0]
        assert event["byte_count"] > 0
        assert event["io_count"] > 0
        assert event["account@"] == "gepardo"
        assert event["object_path"] == "//tmp/table"
        assert event["api_method@"] == "read_table"
        assert event["proxy_type@"] == self._get_proxy_type()
        assert "object_id" in event

    @authors("gepardo")
    @pytest.mark.parametrize("sorted_table", [False, True])
    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_read_dynamic_table(self, sorted_table, optimize_for):
        schema = yson.YsonList([
            {"name": "key", "type": "int64"},
            {"name": "value", "type": "string"},
        ])
        if sorted_table:
            schema[0]["sort_order"] = "ascending"
            schema.attributes["unique_keys"] = True
        data = [
            {"key": 3, "value": "test"},
            {"key": 31, "value": "test read"},
            {"key": 314, "value": "test read dynamic"},
            {"key": 3141, "value": "test read dynamic table"},
        ]

        create_account("gepardo")
        yt_commands.set("//sys/accounts/gepardo/@resource_limits/tablet_count", 10)

        sync_create_cells(1)
        create_dynamic_table("//tmp/my_dyntable", schema=schema, optimize_for=optimize_for, account="gepardo")
        sync_mount_table("//tmp/my_dyntable")
        insert_rows("//tmp/my_dyntable", data)
        sync_unmount_table("//tmp/my_dyntable")

        from_barrier = self.write_log_barrier(self.get_node_address(), "Barrier")
        assert sorted(read_table("//tmp/my_dyntable")) == sorted(data)
        raw_events, _ = self.wait_for_events(raw_count=1, from_barrier=from_barrier, check_event_count=False,
                                             filter=lambda event: event.get("data_node_method@") == "GetBlockSet")

        for event in raw_events:
            assert event["byte_count"] > 0
            assert event["io_count"] > 0
            assert event["object_path"] == "//tmp/my_dyntable"
            assert event["api_method@"] == "read_table"
            assert event["proxy_type@"] == self._get_proxy_type()
            assert event["account@"] == "gepardo"
            assert "object_id" in event

    @authors("gepardo")
    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_read_dynamic_table_converted_from_static(self, optimize_for):
        schema = yson.YsonList([
            {"name": "key", "type": "int64", "sort_order": "ascending"},
            {"name": "value", "type": "string"},
        ])
        schema.attributes["strict"] = True
        schema.attributes["unique_keys"] = True
        data = [
            {"key": 3, "value": "test"},
            {"key": 31, "value": "test read"},
            {"key": 314, "value": "test read dynamic"},
            {"key": 3141, "value": "test read dynamic table"},
        ]

        create_account("gepardo")
        yt_commands.set("//sys/accounts/gepardo/@resource_limits/tablet_count", 10)

        sync_create_cells(1)
        create("table", "//tmp/table", attributes={
            "schema": schema,
            "account": "gepardo",
            "optimize_for": optimize_for,
        })
        write_table("//tmp/table", data)
        alter_table("//tmp/table", dynamic=True)

        from_barrier = self.write_log_barrier(self.get_node_address(), "Barrier")
        assert sorted(read_table("//tmp/table")) == sorted(data)
        raw_events, _ = self.wait_for_events(raw_count=1, from_barrier=from_barrier, check_event_count=False,
                                             filter=lambda event: event.get("data_node_method@") == "GetBlockSet")

        for event in raw_events:
            assert event["byte_count"] > 0
            assert event["io_count"] > 0
            assert event["object_path"] == "//tmp/table"
            assert event["api_method@"] == "read_table"
            assert event["proxy_type@"] == self._get_proxy_type()
            assert event["account@"] == "gepardo"
            assert "object_id" in event


class TestClientRpcProxyIOTracking(TestClientIOTracking):
    DRIVER_BACKEND = "rpc"
    ENABLE_HTTP_PROXY = True
    ENABLE_RPC_PROXY = True

    def write_log_barrier(self, *args, **kwargs):
        kwargs["driver"] = self.__native_driver
        return write_log_barrier(*args, **kwargs)

    def setup(self):
        super(TestClientIOTracking, self).setup()
        native_config = deepcopy(self.Env.configs["driver"])
        self.__native_driver = Driver(native_config)

    def _get_proxy_type(self):
        return "rpc"
