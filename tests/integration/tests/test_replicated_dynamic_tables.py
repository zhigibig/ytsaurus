import pytest

from yt_env_setup import YTEnvSetup
from yt_commands import *
from time import sleep
from yt.yson import YsonEntity

##################################################################

class TestReplicatedDynamicTables(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 3
    NUM_SCHEDULERS = 0
    NUM_REMOTE_CLUSTERS = 1

    DELTA_NODE_CONFIG = {
        "cluster_connection": {
            # Disable cache
            "table_mount_cache": {
                "expire_after_successful_update_time": 0,
                "expire_after_failed_update_time": 0,
                "expire_after_access_time": 0,
                "refresh_time": 0
            }
        },
        "tablet_node": {
            "tablet_manager": {
                "replicator_soft_backoff_time": 100
            }
        }
    }

    SIMPLE_SCHEMA = [
        {"name": "key", "type": "int64", "sort_order": "ascending"},
        {"name": "value1", "type": "string"},
        {"name": "value2", "type": "int64"}
    ]

    AGGREGATE_SCHEMA = [
        {"name": "key", "type": "int64", "sort_order": "ascending"},
        {"name": "value1", "type": "string"},
        {"name": "value2", "type": "int64", "aggregate": "sum"}
    ]

    REPLICA_CLUSTER_NAME = "remote_0"

    
    def setup(self):
        self.replica_driver = get_driver(cluster=self.REPLICA_CLUSTER_NAME)


    def _get_table_attributes(self, schema):
        return {
            "dynamic": True,
            "schema": schema
        }
    
    def _create_replicated_table(self, path, schema=SIMPLE_SCHEMA, attributes={}, mount=True):
        attributes.update(self._get_table_attributes(schema))
        attributes["enable_replication_logging"] = True
        create("replicated_table", path, attributes=attributes)
        if mount:
            self.sync_mount_table(path)

    def _create_replica_table(self, path, schema=SIMPLE_SCHEMA, mount=True):
        create("table", path, attributes=self._get_table_attributes(schema), driver=self.replica_driver)
        if mount:
            self.sync_mount_table(path, driver=self.replica_driver)

    def _create_cells(self):
        self.sync_create_cells(1)
        self.sync_create_cells(1, driver=self.replica_driver)


    def test_replicated_table_must_be_dynamic(self):
        with pytest.raises(YtError): create("replicated_table", "//tmp/t")

    def test_simple(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")

        insert_rows("//tmp/t", [{"key": 1, "value1": "test"}])
        delete_rows("//tmp/t", [{"key": 2}])

    def test_add_replica_fail1(self):
        with pytest.raises(YtError): create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")

    def test_add_replica_fail2(self):
        create("table", "//tmp/t")
        with pytest.raises(YtError): create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")

    def test_add_remove_replica(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        assert exists("//tmp/t/@replicas/{0}".format(replica_id))
        attributes = get("#{0}/@".format(replica_id))
        assert attributes["type"] == "table_replica"
        assert attributes["state"] == "disabled"
        remove_table_replica(replica_id)
        assert not exists("#{0}/@".format(replica_id))

    def test_enable_disable_replica_unmounted(self):
        self._create_replicated_table("//tmp/t", mount=False)
        tablet_id = get("//tmp/t/@tablets/0/tablet_id")
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        attributes = get("#{0}/@".format(replica_id), attributes=["state", "tablets"])
        assert attributes["state"] == "disabled"
        assert len(attributes["tablets"]) == 1
        assert attributes["tablets"][tablet_id]["state"] == "none"

        enable_table_replica(replica_id)
        attributes = get("#{0}/@".format(replica_id), attributes=["state", "tablets"])
        assert attributes["state"] == "enabled"
        assert len(attributes["tablets"]) == 1
        assert attributes["tablets"][tablet_id]["state"] == "none"

        disable_table_replica(replica_id)
        attributes = get("#{0}/@".format(replica_id), attributes=["state", "tablets"])
        assert attributes["state"] == "disabled"
        assert len(attributes["tablets"]) == 1
        assert attributes["tablets"][tablet_id]["state"] == "none"

    def test_enable_disable_replica_mounted(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        assert get("#{0}/@state".format(replica_id)) == "disabled"

        enable_table_replica(replica_id)
        assert get("#{0}/@state".format(replica_id)) == "enabled"

        self.sync_disable_table_replica(replica_id)
        assert get("#{0}/@state".format(replica_id)) == "disabled"

    def test_passive_replication(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        self._create_replica_table("//tmp/r")

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        enable_table_replica(replica_id)
   
        insert_rows("//tmp/t", [{"key": 1, "value1": "test", "value2": 123}])
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test", "value2": 123}]

        insert_rows("//tmp/t", [{"key": 1, "value1": "new_test"}], update=True)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "new_test", "value2": 123}]

        insert_rows("//tmp/t", [{"key": 1, "value2": 456}], update=True)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "new_test", "value2": 456}]

        delete_rows("//tmp/t", [{"key": 1}])
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == []

    def test_disable_propagates_replication_row_index(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        self._create_replica_table("//tmp/r")

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        tablet_id = get("//tmp/t/@tablets/0/tablet_id")
        enable_table_replica(replica_id)
   
        assert get("#{0}/@tablets/{1}/current_replication_row_index".format(replica_id, tablet_id)) == 0

        insert_rows("//tmp/t", [{"key": 1, "value1": "test", "value2": 123}])
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test", "value2": 123}]

        self.sync_disable_table_replica(replica_id)

        assert get("#{0}/@tablets/{1}/current_replication_row_index".format(replica_id, tablet_id)) == 1

    def test_unmount_propagates_replication_row_index(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        self._create_replica_table("//tmp/r")

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        tablet_id = get("//tmp/t/@tablets/0/tablet_id")
        enable_table_replica(replica_id)
   
        assert get("#{0}/@tablets/{1}/current_replication_row_index".format(replica_id, tablet_id)) == 0

        insert_rows("//tmp/t", [{"key": 1, "value1": "test", "value2": 123}])
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test", "value2": 123}]

        self.sync_unmount_table("//tmp/t")

        assert get("#{0}/@tablets/{1}/current_replication_row_index".format(replica_id, tablet_id)) == 1

    @pytest.mark.parametrize("with_data", [False, True])
    def test_start_replication_timestamp(self, with_data):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        self._create_replica_table("//tmp/r")

        insert_rows("//tmp/t", [{"key": 1, "value1": "test"}])

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r", attributes={
            "start_replication_timestamp": generate_timestamp()
        })
        enable_table_replica(replica_id)

        if with_data:
            insert_rows("//tmp/t", [{"key": 2, "value1": "test"}])

        sleep(1.0)

        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == \
            ([{"key": 2, "value1": "test", "value2": YsonEntity()}] if with_data else \
            [])

    @pytest.mark.parametrize("ttl, chunk_count, trimmed_row_count", [
        (0, 1, 1),
        (60000, 2, 0)
    ])
    def test_replication_trim(self, ttl, chunk_count, trimmed_row_count):
        self._create_cells()
        self._create_replicated_table("//tmp/t", attributes={"min_replication_log_ttl": ttl})
        self.sync_mount_table("//tmp/t")

        self._create_replica_table("//tmp/r")

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        enable_table_replica(replica_id)

        insert_rows("//tmp/t", [{"key": 1, "value1": "test1"}])
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test1", "value2": YsonEntity()}]

        self.sync_unmount_table("//tmp/t")
        assert get("//tmp/t/@chunk_count") == 1
        assert get("//tmp/t/@tablets/0/flushed_row_count") == 1
        assert get("//tmp/t/@tablets/0/trimmed_row_count") == 0
        
        self.sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", [{"key": 2, "value1": "test2"}])
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test1", "value2": YsonEntity()}, {"key": 2, "value1": "test2", "value2": YsonEntity()}]
        self.sync_unmount_table("//tmp/t")

        assert get("//tmp/t/@chunk_count") == chunk_count
        assert get("//tmp/t/@tablets/0/flushed_row_count") == 2
        assert get("//tmp/t/@tablets/0/trimmed_row_count") == trimmed_row_count        

    def test_aggregate_replication(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t", schema=self.AGGREGATE_SCHEMA)
        self._create_replica_table("//tmp/r", schema=self.AGGREGATE_SCHEMA)

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        enable_table_replica(replica_id)
   
        insert_rows("//tmp/t", [{"key": 1, "value1": "test1"}])
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test1", "value2": YsonEntity()}]

        insert_rows("//tmp/t", [{"key": 1, "value1": "test2", "value2": 100}])
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test2", "value2": 100}]

        insert_rows("//tmp/t", [{"key": 1, "value2": 50}], aggregate=True, update=True)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test2", "value2": 150}]

    def test_replication_lag(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t", schema=self.AGGREGATE_SCHEMA)
        self._create_replica_table("//tmp/r", schema=self.AGGREGATE_SCHEMA)

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")

        assert get("#{0}/@replication_lag_time".format(replica_id)) == 0

        insert_rows("//tmp/t", [{"key": 1, "value1": "test1"}])
        sleep(1.0)
        get("#{0}/@replication_lag_time".format(replica_id)) > 1000000
        
        enable_table_replica(replica_id)
        sleep(1.0)
        assert get("#{0}/@replication_lag_time".format(replica_id)) == 0

    def test_replica_ops_require_exclusive_lock(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t", mount=False)
        
        tx1 = start_transaction()
        lock("//tmp/t", mode="exclusive", tx=tx1)
        with pytest.raises(YtError): create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        abort_transaction(tx1)

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        tx2 = start_transaction()
        lock("//tmp/t", mode="exclusive", tx=tx2)
        with pytest.raises(YtError): remove_table_replica(replica_id)

##################################################################

class TestReplicatedDynamicTablesMulticell(TestReplicatedDynamicTables):
    NUM_SECONDARY_MASTER_CELLS = 2
