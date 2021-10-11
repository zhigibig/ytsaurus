from yt_env_setup import (
    YTEnvSetup,
    Restarter,
    SCHEDULERS_SERVICE,
    CONTROLLER_AGENTS_SERVICE,
    is_asan_build,
)

from yt_commands import (
    authors, print_debug, wait, wait_breakpoint, with_breakpoint,
    ls, get, set, remove, exists, create_pool, create_pool_tree,
    create_data_center, create_rack, make_batch_request,
    execute_batch, get_batch_error,
    vanilla, run_test_vanilla, run_sleeping_vanilla,
    update_controller_agent_config)

from yt_scheduler_helpers import (
    scheduler_orchid_pool_path,
    scheduler_orchid_operation_path, scheduler_orchid_default_pool_tree_config_path,
    scheduler_orchid_path, scheduler_orchid_node_path)


from yt.test_helpers.profiler import Profiler
from yt.test_helpers import are_almost_equal

from yt.common import YtError


import pytest

import time


##################################################################


class TestSchedulingSegments(YTEnvSetup):
    NUM_TEST_PARTITIONS = 3
    NUM_MASTERS = 1
    NUM_NODES = 10
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,
            "fair_share_update_period": 100,
            "fair_share_profiling_period": 100,
            "scheduling_segments_manage_period": 100,
            "scheduling_segments_initialization_timeout": 100,
        }
    }

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "job_controller": {
                "resource_limits": {
                    "cpu": 10,
                    "user_slots": 10,
                },
                "gpu_manager": {"test_resource": True, "test_gpu_count": 8},
            },
            "controller_agent_connector": {"heartbeat_period": 100},  # 100 msec
            "scheduler_connector": {"heartbeat_period": 100},  # 100 msec
        },
        "job_proxy_heartbeat_period": 100,
    }

    SCHEDULING_SEGMENTS = [
        "default",
        "large_gpu",
    ]

    DATA_CENTER = "SAS"
    RACK = "SAS1"

    def _get_usage_ratio(self, op, tree="default"):
        return get(scheduler_orchid_operation_path(op, tree) + "/usage_ratio", default=0.0)

    def _get_fair_share_ratio(self, op, tree="default"):
        return get(scheduler_orchid_operation_path(op, tree) + "/fair_share_ratio", default=0.0)

    # NB(eshcherbin): This method always returns NO nodes for the default segment.
    def _get_nodes_for_segment_in_tree(self, segment, tree="default"):
        node_states = get("//sys/scheduler/segments_state/node_states", default={})
        return [node_state["address"] for _, node_state in node_states.iteritems()
                if node_state["segment"] == segment and node_state["tree"] == tree]

    @classmethod
    def setup_class(cls):
        if is_asan_build():
            pytest.skip("test suite has too high memory consumption for ASAN build")
        super(TestSchedulingSegments, cls).setup_class()

    def setup_method(self, method):
        super(TestSchedulingSegments, self).setup_method(method)
        # NB(eshcherbin): This is done to reset node segments.
        with Restarter(self.Env, SCHEDULERS_SERVICE):
            requests = [make_batch_request("remove", path="//sys/scheduler/segments_state", force=True)]
            for node in ls("//sys/cluster_nodes"):
                requests.append(make_batch_request(
                    "remove",
                    path="//sys/cluster_nodes/{}/@scheduling_segment".format(node),
                    force=True
                ))
            for response in execute_batch(requests):
                assert not get_batch_error(response)

        create_data_center(TestSchedulingSegments.DATA_CENTER)
        create_rack(TestSchedulingSegments.RACK)
        set("//sys/racks/" + TestSchedulingSegments.RACK + "/@data_center", TestSchedulingSegments.DATA_CENTER)
        for node in ls("//sys/cluster_nodes"):
            set("//sys/cluster_nodes/" + node + "/@rack", TestSchedulingSegments.RACK)
        for node in ls("//sys/cluster_nodes"):
            wait(lambda: get(scheduler_orchid_node_path(node) + "/data_center") == TestSchedulingSegments.DATA_CENTER)

        create_pool("cpu", wait_for_orchid=False)
        create_pool("small_gpu", wait_for_orchid=False)
        create_pool("large_gpu")
        set("//sys/pool_trees/default/@config/scheduling_segments", {
            "mode": "large_gpu",
            "unsatisfied_segments_rebalancing_timeout": 1000,
            "data_centers": [TestSchedulingSegments.DATA_CENTER],
        })
        set("//sys/pool_trees/default/@config/main_resource", "gpu")
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/scheduling_segments/mode") == "large_gpu")
        wait(
            lambda: get(
                scheduler_orchid_default_pool_tree_config_path()
                + "/scheduling_segments/unsatisfied_segments_rebalancing_timeout"
            )
            == 1000
        )
        # Not to let preemption abort the jobs instead of segments manager.
        set("//sys/pool_trees/default/@config/preemptive_scheduling_backoff", 0)
        set("//sys/pool_trees/default/@config/fair_share_starvation_timeout", 100)
        set("//sys/pool_trees/default/@config/fair_share_starvation_timeout_limit", 100)
        set("//sys/pool_trees/default/@config/fair_share_starvation_tolerance", 0.95)
        set("//sys/pool_trees/default/@config/max_unpreemptable_running_job_count", 80)
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/max_unpreemptable_running_job_count") == 80)

    @authors("eshcherbin")
    def test_large_gpu_segment_extended(self):
        blocking_op = run_sleeping_vanilla(
            job_count=80,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))

    @authors("eshcherbin")
    def test_default_segment_extended_gpu(self):
        blocking_op = run_sleeping_vanilla(
            job_count=10,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op = run_sleeping_vanilla(
            job_count=8,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))

    @pytest.mark.skip("There is no logic that reduces oversatisfied segments yet, "
                      "and operations with zero GPU demand do not change the default segment's fair resource amount")
    @authors("eshcherbin")
    def test_default_segment_extended_cpu(self):
        blocking_op = run_sleeping_vanilla(
            job_count=10,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op = run_sleeping_vanilla(job_count=10, spec={"pool": "cpu"}, task_patch={"cpu_limit": 1})
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))

    @pytest.mark.skip("There is no logic that reduces oversatisfied segments yet, "
                      "and operations with zero GPU demand do not change the default segment's fair resource amount")
    @authors("eshcherbin")
    def test_default_segment_extended_gpu_and_cpu(self):
        blocking_op = run_sleeping_vanilla(
            job_count=10,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op1 = run_sleeping_vanilla(job_count=10, spec={"pool": "cpu"}, task_patch={"cpu_limit": 1})
        op2 = run_sleeping_vanilla(
            job_count=8,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )

        wait(lambda: are_almost_equal(self._get_usage_ratio(op1.id), 0.1))
        wait(lambda: are_almost_equal(self._get_usage_ratio(op2.id), 0.1))

    @authors("eshcherbin")
    def test_satisfaction_margins(self):
        # Just to check that it works with no core dump.
        set("//sys/pool_trees/default/@config/scheduling_segments/satisfaction_margins", {"default": 1.0})

        set("//sys/pool_trees/default/large_gpu/@strong_guarantee_resources", {"gpu": 80})
        set("//sys/pool_trees/default/@config/scheduling_segments/satisfaction_margins",
            {
                "large_gpu": {
                    TestSchedulingSegments.DATA_CENTER: 8
                }
            })
        set("//sys/pool_trees/default/@config/job_interrupt_timeout", 30000)

        wait(lambda: are_almost_equal(
            get(scheduler_orchid_default_pool_tree_config_path() +
                "/scheduling_segments/satisfaction_margins/large_gpu/" +
                TestSchedulingSegments.DATA_CENTER, default=0),
            8))
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/job_interrupt_timeout") == 30000)

        filling_op = run_sleeping_vanilla(
            job_count=9,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(filling_op.id), 0.9))

        run_test_vanilla(
            """(trap "sleep 40; exit 0" SIGINT; sleep 1000)""",
            job_count=8,
            spec={"pool": "small_gpu"},
            task_patch={"interruption_signal": "SIGINT", "gpu_limit": 1, "enable_gpu_layers": False},
        )

        time.sleep(3)

        op = run_test_vanilla(
            "sleep 1",
            job_count=1,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: op.get_state() == "completed")

    @authors("eshcherbin")
    def test_rebalancing_heuristic(self):
        blocking_op1 = run_sleeping_vanilla(
            job_count=9,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op1.id), 0.9))

        # Need to spend some time to ensure the nodes where blocking_op1's jobs are running won't be moved.
        time.sleep(1.0)

        blocking_op2 = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op2.id), 0.1))

        def get_first_job_node(op):
            wait(lambda: len(op.get_running_jobs()) >= 1)
            jobs = op.get_running_jobs()
            job = jobs[list(jobs)[0]]
            return job["address"]

        expected_node = get_first_job_node(blocking_op2)
        wait(lambda: get(scheduler_orchid_node_path(expected_node) + "/scheduling_segment", default=None) == "large_gpu")

        new_op = run_sleeping_vanilla(
            job_count=8,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(new_op.id), 0.1))

        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op2.id), 0.0))
        actual_node = get_first_job_node(new_op)
        assert actual_node == expected_node
        wait(lambda: get(scheduler_orchid_node_path(expected_node) + "/scheduling_segment", default=None) == "default")

    @authors("eshcherbin")
    def test_rebalancing_heuristic_choose_node_with_preemptable_job(self):
        set("//sys/pool_trees/default/@config/cached_job_preemption_statuses_update_period", 1000)
        set("//sys/pool_trees/default/large_gpu/@strong_guarantee_resources", {"gpu": 72})
        set("//sys/pool_trees/default/small_gpu/@strong_guarantee_resources", {"gpu": 8})
        create_pool("guaranteed_large", parent_name="large_gpu", attributes={"strong_guarantee_resources": {"gpu": 72}})
        create_pool("research_large", parent_name="large_gpu")

        blocking_op1 = run_sleeping_vanilla(
            spec={"pool": "research_large"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op1.id), 0.1))

        # Need to spend some time to ensure the nodes where blocking_op1's jobs are running won't be moved.
        time.sleep(3.0)

        blocking_op2 = run_sleeping_vanilla(
            job_count=9,
            spec={"pool": "guaranteed_large"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op2.id), 0.9))

        def get_first_job_node(op):
            wait(lambda: len(op.get_running_jobs()) >= 1)
            jobs = op.get_running_jobs()
            job = jobs[list(jobs)[0]]
            return job["address"]

        expected_node = get_first_job_node(blocking_op1)
        wait(lambda: get(scheduler_orchid_node_path(expected_node) + "/scheduling_segment", default=None) == "large_gpu")

        timeout_attribute_path = "/scheduling_segments/unsatisfied_segments_rebalancing_timeout"
        set("//sys/pool_trees/default/@config" + timeout_attribute_path, 1000000000)
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + timeout_attribute_path) == 1000000000)

        new_op = run_sleeping_vanilla(
            job_count=8,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )

        wait(
            lambda: get(
                scheduler_orchid_node_path(expected_node) + "/running_job_statistics/preemptable_gpu_time", default=0.0
            )
            > 0.0
        )
        set("//sys/pool_trees/default/@config" + timeout_attribute_path, 1000)

        wait(lambda: are_almost_equal(self._get_usage_ratio(new_op.id), 0.1))

        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op1.id), 0.0))
        actual_node = get_first_job_node(new_op)
        assert actual_node == expected_node
        wait(lambda: get(scheduler_orchid_node_path(expected_node) + "/scheduling_segment", default=None) == "default")

    @authors("eshcherbin")
    def test_mixed_operation(self):
        op = vanilla(
            spec={
                "tasks": {
                    "small": {
                        "job_count": 1,
                        "command": "sleep 1000",
                        "gpu_limit": 1,
                        "enable_gpu_layers": False,
                    },
                    "large": {
                        "job_count": 1,
                        "command": "sleep 1000",
                        "gpu_limit": 8,
                        "enable_gpu_layers": False,
                    },
                }
            },
            track=False,
        )

        wait(
            lambda: get(
                scheduler_orchid_operation_path(op.id) + "/scheduling_segment",
                default="",
            )
            == "default"
        )

    @authors("eshcherbin")
    def test_specified_segment(self):
        small_but_large_op = run_sleeping_vanilla(
            spec={"pool": "small_gpu", "scheduling_segment": "large_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        large_but_small_op = run_sleeping_vanilla(
            spec={"pool": "large_gpu", "scheduling_segment": "default"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )

        wait(
            lambda: get(
                scheduler_orchid_operation_path(small_but_large_op.id) + "/scheduling_segment",
                default="",
            )
            == "large_gpu"
        )
        wait(
            lambda: get(
                scheduler_orchid_operation_path(large_but_small_op.id) + "/scheduling_segment",
                default="",
            )
            == "default"
        )

        with pytest.raises(YtError):
            run_sleeping_vanilla(
                spec={
                    "pool": "small_gpu",
                    "scheduling_segment": "my_cool_but_totally_invalid_segment",
                },
                task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
            )

    @authors("eshcherbin")
    def test_disabled(self):
        set("//sys/pool_trees/default/@config/scheduling_segments/mode", "disabled")
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/scheduling_segments/mode") == "disabled")

        blocking_op = run_sleeping_vanilla(
            job_count=80,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(op.id), 0.1))

        op_slot_index_path = scheduler_orchid_path() + "/scheduler/operations/{}/slot_index_per_pool_tree/default".format(op.id)
        wait(lambda: exists(op_slot_index_path))
        op_slot_index = get(op_slot_index_path)

        op_usage_ratio_sensor = Profiler\
            .at_scheduler(self.Env.create_native_client(), fixed_tags={"tree": "default", "pool": "large_gpu", "slot_index": str(op_slot_index)})\
            .gauge("scheduler/operations_by_slot/dominant_usage_share")

        for _ in range(30):
            time.sleep(0.1)
            assert op_usage_ratio_sensor.get(default=0, verbose=False) == 0

    @authors("eshcherbin")
    def test_rebalancing_timeout_changed(self):
        timeout_attribute_path = "/scheduling_segments/unsatisfied_segments_rebalancing_timeout"
        set("//sys/pool_trees/default/@config" + timeout_attribute_path, 1000000000)
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + timeout_attribute_path) == 1000000000)

        blocking_op = run_sleeping_vanilla(
            job_count=80,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(op.id), 0.1))

        op_slot_index_path = scheduler_orchid_path() + "/scheduler/operations/{}/slot_index_per_pool_tree/default".format(op.id)
        wait(lambda: exists(op_slot_index_path))
        op_slot_index = get(op_slot_index_path)

        op_usage_ratio_sensor = Profiler \
            .at_scheduler(self.Env.create_native_client(), fixed_tags={"tree": "default", "pool": "large_gpu", "slot_index": str(op_slot_index)}) \
            .gauge("scheduler/operations_by_slot/dominant_usage_share")

        for _ in range(30):
            time.sleep(0.1)
            assert op_usage_ratio_sensor.get(default=0, verbose=False) == 0

        set("//sys/pool_trees/default/@config" + timeout_attribute_path, 1000)
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))

    @authors("eshcherbin")
    def test_orchid(self):
        small_op = run_sleeping_vanilla(
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        large_op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )

        wait(
            lambda: get(
                scheduler_orchid_operation_path(small_op.id) + "/scheduling_segment",
                default="",
            )
            == "default"
        )
        wait(
            lambda: get(
                scheduler_orchid_operation_path(large_op.id) + "/scheduling_segment",
                default="",
            )
            == "large_gpu"
        )

    @authors("eshcherbin")
    def test_update_operation_segment_on_reconfiguration(self):
        set("//sys/pool_trees/default/@config/scheduling_segments/mode", "disabled")
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/scheduling_segments/mode") == "disabled")

        blocking_op = run_sleeping_vanilla(
            job_count=80,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(op.id), 0.1))
        wait(
            lambda: get(
                scheduler_orchid_operation_path(op.id) + "/scheduling_segment",
                default="",
            )
            == "default"
        )

        set("//sys/pool_trees/default/@config/scheduling_segments/mode", "large_gpu")
        wait(lambda: get(scheduler_orchid_operation_path(op.id) + "/scheduling_segment", default="") == "large_gpu")
        wait(lambda: get(scheduler_orchid_operation_path(blocking_op.id) + "/scheduling_segment", default="") == "default")
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))

        set("//sys/pool_trees/default/@config/scheduling_segments/mode", "disabled")
        wait(lambda: get(scheduler_orchid_operation_path(op.id) + "/scheduling_segment", default="") == "default")
        wait(lambda: get(scheduler_orchid_operation_path(blocking_op.id) + "/scheduling_segment", default="") == "default")

        time.sleep(3.0)
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))

    @authors("eshcherbin")
    def test_profiling(self):
        set("//sys/pool_trees/default/@config/scheduling_segments/unsatisfied_segments_rebalancing_timeout", 1000000000)
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/scheduling_segments/unsatisfied_segments_rebalancing_timeout") == 1000000000)

        profiler = Profiler.at_scheduler(self.Env.create_native_client())
        fair_resource_amount_default_sensor = profiler.gauge("scheduler/segments/fair_resource_amount", fixed_tags={"segment": "default"})
        current_resource_amount_default_sensor = profiler.gauge("scheduler/segments/current_resource_amount", fixed_tags={"segment": "default"})
        fair_resource_amount_large_sensor = profiler.gauge("scheduler/segments/fair_resource_amount", fixed_tags={"segment": "large_gpu"})
        current_resource_amount_large_sensor = profiler.gauge("scheduler/segments/current_resource_amount", fixed_tags={"segment": "large_gpu"})

        wait(lambda: fair_resource_amount_default_sensor.get() == 0)
        wait(lambda: fair_resource_amount_large_sensor.get() == 0)
        wait(lambda: current_resource_amount_default_sensor.get() == 80)
        wait(lambda: current_resource_amount_large_sensor.get() == 0)

        blocking_op = run_sleeping_vanilla(
            job_count=80,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        wait(lambda: fair_resource_amount_default_sensor.get() == 80)
        wait(lambda: fair_resource_amount_large_sensor.get() == 0)
        wait(lambda: current_resource_amount_default_sensor.get() == 80)
        wait(lambda: current_resource_amount_large_sensor.get() == 0)

        op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(op.id), 0.1))

        time.sleep(3.0)

        wait(lambda: fair_resource_amount_default_sensor.get() == 72)
        wait(lambda: fair_resource_amount_large_sensor.get() == 8)
        wait(lambda: current_resource_amount_default_sensor.get() == 80)
        wait(lambda: current_resource_amount_large_sensor.get() == 0)

        set("//sys/pool_trees/default/@config/scheduling_segments/unsatisfied_segments_rebalancing_timeout", 1000)

        wait(lambda: fair_resource_amount_default_sensor.get() == 72)
        wait(lambda: fair_resource_amount_large_sensor.get() == 8)
        wait(lambda: current_resource_amount_default_sensor.get() == 72)
        wait(lambda: current_resource_amount_large_sensor.get() == 8)

        op.abort()

        wait(lambda: fair_resource_amount_default_sensor.get() == 80)
        wait(lambda: fair_resource_amount_large_sensor.get() == 0)
        wait(lambda: current_resource_amount_default_sensor.get() == 80)
        wait(lambda: current_resource_amount_large_sensor.get() == 0)

    @authors("eshcherbin")
    def test_revive_operation_segments_from_scratch(self):
        small_op = run_sleeping_vanilla(
            job_count=80,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        large_op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(small_op.id), 0.9))
        wait(lambda: are_almost_equal(self._get_usage_ratio(large_op.id), 0.1))

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass

        wait(
            lambda: get(
                scheduler_orchid_operation_path(small_op.id) + "/scheduling_segment",
                default="",
            )
            == "default"
        )
        wait(
            lambda: get(
                scheduler_orchid_operation_path(large_op.id) + "/scheduling_segment",
                default="",
            )
            == "large_gpu"
        )

        wait(lambda: are_almost_equal(self._get_usage_ratio(small_op.id), 0.9))
        wait(lambda: are_almost_equal(self._get_usage_ratio(large_op.id), 0.1))

    @authors("eshcherbin")
    @pytest.mark.parametrize("service_to_restart", [SCHEDULERS_SERVICE, CONTROLLER_AGENTS_SERVICE])
    def test_revive_operation_segments_from_snapshot(self, service_to_restart):
        update_controller_agent_config("snapshot_period", 300)

        small_op = run_sleeping_vanilla(
            job_count=80,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        large_op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(small_op.id), 0.9))
        wait(lambda: are_almost_equal(self._get_usage_ratio(large_op.id), 0.1))

        small_op.wait_for_fresh_snapshot()
        large_op.wait_for_fresh_snapshot()

        wait(lambda: exists(small_op.get_path() + "/@initial_aggregated_min_needed_resources"))
        wait(lambda: exists(large_op.get_path() + "/@initial_aggregated_min_needed_resources"))

        with Restarter(self.Env, service_to_restart):
            print_debug(get(small_op.get_path() + "/@initial_aggregated_min_needed_resources"))
            print_debug(get(large_op.get_path() + "/@initial_aggregated_min_needed_resources"))

        small_op.ensure_running()
        large_op.ensure_running()

        wait(
            lambda: get(
                scheduler_orchid_operation_path(large_op.id) + "/scheduling_segment",
                default="",
            )
            == "large_gpu"
        )
        wait(
            lambda: get(
                scheduler_orchid_operation_path(small_op.id) + "/scheduling_segment",
                default="",
            )
            == "default"
        )

        wait(lambda: are_almost_equal(self._get_usage_ratio(small_op.id), 0.9))
        wait(lambda: are_almost_equal(self._get_usage_ratio(large_op.id), 0.1))

    @authors("eshcherbin")
    def test_persistent_segments_state(self):
        wait(lambda: exists("//sys/scheduler/segments_state/node_states"))
        for segment in TestSchedulingSegments.SCHEDULING_SEGMENTS:
            wait(lambda: len(self._get_nodes_for_segment_in_tree(segment)) == 0)

        blocking_op = run_sleeping_vanilla(job_count=80, spec={"pool": "small_gpu"}, task_patch={"gpu_limit": 1, "enable_gpu_layers": False})
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))
        run_sleeping_vanilla(spec={"pool": "large_gpu"}, task_patch={"gpu_limit": 8, "enable_gpu_layers": False})

        wait(lambda: len(self._get_nodes_for_segment_in_tree("large_gpu")) == 1)
        wait(lambda: len(self._get_nodes_for_segment_in_tree("default")) == 0)

        node_segment_orchid_path = scheduler_orchid_path() + "/scheduler/nodes/{}/scheduling_segment"
        large_gpu_segment_nodes = self._get_nodes_for_segment_in_tree("large_gpu")
        assert len(large_gpu_segment_nodes) == 1
        for node in ls("//sys/cluster_nodes"):
            expected_segment = "large_gpu" \
                if node in large_gpu_segment_nodes \
                else "default"
            wait(lambda: get(node_segment_orchid_path.format(node), default="") == expected_segment)

    @authors("eshcherbin")
    def test_persistent_segments_state_revive(self):
        update_controller_agent_config("snapshot_period", 300)

        wait(lambda: exists("//sys/scheduler/segments_state/node_states"))
        for segment in TestSchedulingSegments.SCHEDULING_SEGMENTS:
            wait(lambda: len(self._get_nodes_for_segment_in_tree(segment)) == 0)

        blocking_op = run_sleeping_vanilla(job_count=80, spec={"pool": "small_gpu"}, task_patch={"gpu_limit": 1, "enable_gpu_layers": False})
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))
        op = run_test_vanilla(
            with_breakpoint("BREAKPOINT"),
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False}
        )

        wait_breakpoint()

        wait(lambda: len(self._get_nodes_for_segment_in_tree("large_gpu")) == 1)
        wait(lambda: len(self._get_nodes_for_segment_in_tree("default")) == 0)

        large_gpu_segment_nodes = self._get_nodes_for_segment_in_tree("large_gpu")
        assert len(large_gpu_segment_nodes) == 1
        expected_node = large_gpu_segment_nodes[0]

        jobs = list(op.get_running_jobs())
        assert len(jobs) == 1
        expected_job = jobs[0]

        op.wait_for_fresh_snapshot()

        agent_to_incarnation = {}
        for agent in ls("//sys/controller_agents/instances"):
            agent_to_incarnation[agent] = get("//sys/controller_agents/instances/{}/orchid/controller_agent/incarnation_id".format(agent))

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            set("//sys/scheduler/config/scheduling_segments_initialization_timeout", 30000)

        node_segment_orchid_path = scheduler_orchid_path() + "/scheduler/nodes/{}/scheduling_segment"
        for node in ls("//sys/cluster_nodes"):
            expected_segment = "large_gpu" \
                if node == expected_node \
                else "default"
            wait(lambda: get(node_segment_orchid_path.format(node), default="") == expected_segment)

        # NB(eshcherbin): See: YT-14796.
        for agent, old_incarnation in agent_to_incarnation.items():
            wait(lambda: old_incarnation != get("//sys/controller_agents/instances/{}/orchid/controller_agent/incarnation_id".format(agent), default=None))

        wait(lambda: len(list(op.get_running_jobs())) == 1)
        jobs = list(op.get_running_jobs())
        assert len(jobs) == 1
        assert jobs[0] == expected_job

    @authors("eshcherbin")
    def test_node_changes_trees(self):
        set("//sys/pool_trees/default/@config/nodes_filter", "!other")
        create_pool_tree("other", config={"nodes_filter": "other", "main_resource": "gpu"})

        op = run_sleeping_vanilla(spec={"pool": "large_gpu"}, task_patch={"gpu_limit": 8, "enable_gpu_layers": False})
        wait(lambda: len(op.get_running_jobs()) == 1)

        wait(lambda: len(self._get_nodes_for_segment_in_tree("large_gpu", tree="default")) > 0)
        large_nodes = self._get_nodes_for_segment_in_tree("large_gpu", tree="default")
        assert len(large_nodes) == 1
        node = large_nodes[0]

        set("//sys/cluster_nodes/{}/@user_tags/end".format(node), "other")
        wait(lambda: get(scheduler_orchid_pool_path("<Root>", tree="other") + "/resource_limits/cpu") > 0)

        wait(lambda: get(scheduler_orchid_node_path(node) + "/scheduling_segment") == "default")
        wait(lambda: len(self._get_nodes_for_segment_in_tree("large_gpu", tree="other")) == 0)
        wait(lambda: len(self._get_nodes_for_segment_in_tree("large_gpu", tree="default")) == 1)

    @authors("eshcherbin")
    def test_manual_move_node_from_segment_and_back(self):
        node = list(ls("//sys/cluster_nodes"))[0]
        set("//sys/cluster_nodes/{}/@scheduling_segment".format(node), "large_gpu")
        wait(lambda: get(scheduler_orchid_path() + "/scheduler/nodes/{}/scheduling_segment".format(node), "") == "large_gpu")
        set("//sys/cluster_nodes/{}/@scheduling_segment".format(node), "default")
        wait(lambda: get(scheduler_orchid_path() + "/scheduler/nodes/{}/scheduling_segment".format(node), "") == "default")

    @authors("eshcherbin")
    def test_freeze_node_segment(self):
        set("//sys/pools/large_gpu/@strong_guarantee_resources", {"gpu": 80})

        blocking_op = run_sleeping_vanilla(
            job_count=10,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op = run_sleeping_vanilla(
            job_count=8,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        time.sleep(3.0)
        assert are_almost_equal(self._get_usage_ratio(op.id), 0.0)

        node = list(ls("//sys/cluster_nodes"))[0]
        wait(lambda: get(scheduler_orchid_path() + "/scheduler/nodes/{}/scheduling_segment".format(node), "") == "large_gpu")
        set("//sys/cluster_nodes/{}/@scheduling_segment".format(node), "default")
        wait(lambda: get(scheduler_orchid_path() + "/scheduler/nodes/{}/scheduling_segment".format(node), "") == "default")

        set("//sys/pools/large_gpu/@strong_guarantee_resources", {"gpu": 72})
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))

        op.complete()
        time.sleep(3.0)
        wait(lambda: get(scheduler_orchid_path() + "/scheduler/nodes/{}/scheduling_segment".format(node), "") == "default")
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 0.9))

        remove("//sys/cluster_nodes/{}/@scheduling_segment".format(node))
        wait(lambda: get(scheduler_orchid_path() + "/scheduler/nodes/{}/scheduling_segment".format(node), "") == "large_gpu")
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

    @authors("eshcherbin")
    def test_invalid_config(self):
        with pytest.raises(YtError):
            set("//sys/pool_trees/default/@config/scheduling_segments/data_centers", ["SAS", "VLA", ""])
        with pytest.raises(YtError):
            set("//sys/pool_trees/default/@config/scheduling_segments/data_centers", ["SAS", "VLA", ""])
        with pytest.raises(YtError):
            set("//sys/pool_trees/default/@config/scheduling_segments/satisfaction_margins", {"default": {"SAS": "-3.0"}})
        with pytest.raises(YtError):
            set("//sys/pool_trees/default/@config/scheduling_segments/satisfaction_margins", {"large_gpu": {"VLA": "-3.0"}})


##################################################################


class TestSchedulingSegmentsMultiDataCenter(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 10
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,
            "fair_share_update_period": 100,
            "fair_share_profiling_period": 100,
            "scheduling_segments_manage_period": 100,
            "scheduling_segments_initialization_timeout": 100,
            "operations_update_period": 100,
            "operation_hangup_check_period": 100,
        },
    }

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "job_controller": {
                "resource_limits": {
                    "cpu": 10,
                    "user_slots": 10,
                },
                "gpu_manager": {"test_resource": True, "test_gpu_count": 8},
            },
            "controller_agent_connector": {"heartbeat_period": 100},  # 100 msec
            "scheduler_connector": {"heartbeat_period": 100},  # 100 msec
        },
        "job_proxy_heartbeat_period": 100,
    }

    SCHEDULING_SEGMENTS = [
        "default",
        "large_gpu",
    ]

    DATA_CENTERS = ["SAS", "VLA"]
    RACKS = ["SAS1", "VLA1"]

    def _get_usage_ratio(self, op, tree="default"):
        return get(scheduler_orchid_operation_path(op, tree) + "/usage_ratio", default=0.0)

    def _get_fair_share_ratio(self, op, tree="default"):
        return get(scheduler_orchid_operation_path(op, tree) + "/fair_share_ratio", default=0.0)

    def _get_data_center(self, op, tree="default"):
        return get(scheduler_orchid_operation_path(op.id, tree) + "/scheduling_segment_data_center", default=None)

    @classmethod
    def setup_class(cls):
        if is_asan_build():
            pytest.skip("test suite has too high memory consumption for ASAN build")
        super(TestSchedulingSegmentsMultiDataCenter, cls).setup_class()

    def setup_method(self, method):
        super(TestSchedulingSegmentsMultiDataCenter, self).setup_method(method)
        # NB(eshcherbin): This is done to reset node segments.
        with Restarter(self.Env, SCHEDULERS_SERVICE):
            requests = [make_batch_request("remove", path="//sys/scheduler/segments_state", force=True)]
            for node in ls("//sys/cluster_nodes"):
                requests.append(make_batch_request(
                    "remove",
                    path="//sys/cluster_nodes/{}/@scheduling_segment".format(node),
                    force=True
                ))
            for response in execute_batch(requests):
                assert not get_batch_error(response)

        for dc, r in zip(TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS, TestSchedulingSegmentsMultiDataCenter.RACKS):
            create_data_center(dc)
            create_rack(r)
            set("//sys/racks/" + r + "/@data_center", dc)

        nodes = list(ls("//sys/cluster_nodes"))
        dc_count = len(TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS)
        for i, node in enumerate(nodes):
            set("//sys/cluster_nodes/" + node + "/@rack", TestSchedulingSegmentsMultiDataCenter.RACKS[i % dc_count])
        for i, node in enumerate(nodes):
            wait(lambda: get(scheduler_orchid_node_path(node) + "/data_center") == TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS[i % dc_count])

        create_pool("cpu", wait_for_orchid=False)
        create_pool("small_gpu", wait_for_orchid=False)
        create_pool("large_gpu")
        set("//sys/pool_trees/default/@config/scheduling_segments", {
            "mode": "large_gpu",
            "unsatisfied_segments_rebalancing_timeout": 1000,
            "data_centers": TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS,
        })
        set("//sys/pool_trees/default/@config/main_resource", "gpu")
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/scheduling_segments/mode") == "large_gpu")
        wait(
            lambda: get(
                scheduler_orchid_default_pool_tree_config_path()
                + "/scheduling_segments/unsatisfied_segments_rebalancing_timeout"
            ) == 1000
        )
        # Not to let preemption abort the jobs instead of segments manager.
        set("//sys/pool_trees/default/@config/preemptive_scheduling_backoff", 0)
        set("//sys/pool_trees/default/@config/fair_share_starvation_timeout", 100)
        set("//sys/pool_trees/default/@config/fair_share_starvation_timeout_limit", 100)
        set("//sys/pool_trees/default/@config/fair_share_starvation_tolerance", 0.95)
        set("//sys/pool_trees/default/@config/max_unpreemptable_running_job_count", 80)
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/max_unpreemptable_running_job_count") == 80)

    @authors("eshcherbin")
    def test_data_center_locality_for_large_multihost_operations(self):
        op = run_sleeping_vanilla(
            job_count=5,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )

        wait(lambda: self._get_data_center(op) in TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS)
        dc = self._get_data_center(op)

        wait(lambda: len(op.get_running_jobs()) == 5)
        jobs = op.get_running_jobs()
        for _, job in jobs.iteritems():
            assert get("//sys/cluster_nodes/" + job["address"] + "/@data_center", default="") == dc

    @authors("eshcherbin")
    def test_no_data_center_locality_for_small_multihost_operations(self):
        op = run_sleeping_vanilla(
            job_count=12,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 4, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 48.0 / 80.0))

    @authors("eshcherbin")
    def test_uniform_distribution_of_large_operations_to_data_centers_1(self):
        ops = []
        for i in range(10):
            op = run_sleeping_vanilla(
                spec={"pool": "large_gpu"},
                task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
            )
            ops.append(op)
            wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))

        dcs = [self._get_data_center(op_) for op_ in ops]
        assert dcs[0] != dcs[1]
        for i in range(2, 10):
            assert dcs[i] == dcs[i - 2]

    @authors("eshcherbin")
    def test_uniform_distribution_of_large_operations_to_data_centers_2(self):
        set("//sys/pools/large_gpu/@strong_guarantee_resources", {"gpu": 80})

        blocking_op = run_sleeping_vanilla(
            job_count=4,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 4, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 0.2))

        big_op = run_sleeping_vanilla(
            job_count=4,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(big_op.id), 0.4))
        big_dc = self._get_data_center(big_op)

        for i in range(4):
            op = run_sleeping_vanilla(
                spec={"pool": "large_gpu"},
                task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
            )
            wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))
            wait(lambda: big_dc != self._get_data_center(op))

        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 0.2))

    @authors("eshcherbin")
    def test_specified_data_center(self):
        big_op = run_sleeping_vanilla(
            job_count=4,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(big_op.id), 0.4))
        big_dc = self._get_data_center(big_op)

        op = run_sleeping_vanilla(
            spec={"pool": "large_gpu", "scheduling_segment_data_centers": [big_dc]},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))
        wait(lambda: big_dc == self._get_data_center(op))

    @authors("eshcherbin")
    def test_data_center_reconsideration(self):
        big_op = run_sleeping_vanilla(
            job_count=5,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(big_op.id), 0.5))
        big_dc = self._get_data_center(big_op)

        node_to_disappear = None
        for node in ls("//sys/cluster_nodes"):
            if get("//sys/cluster_nodes/" + node + "/@data_center") == big_dc:
                node_to_disappear = node
                break
        assert node_to_disappear is not None

        set("//sys/pool_trees/default/@config/nodes_filter", "!other")
        create_pool_tree("other", config={"nodes_filter": "other", "main_resource": "gpu"})
        set("//sys/cluster_nodes/" + node_to_disappear + "/@user_tags/end", "other")
        wait(lambda: are_almost_equal(self._get_usage_ratio(big_op.id), 4. / 9.))

        set("//sys/pool_trees/default/@config/scheduling_segments/data_center_reconsideration_timeout", 5000)
        wait(lambda: are_almost_equal(self._get_usage_ratio(big_op.id), 5. / 9.))
        wait(lambda: big_dc != self._get_data_center(big_op))
        wait(lambda: big_op.get_job_count("aborted") >= 5)

    @authors("eshcherbin")
    def test_rebalance_large_gpu_segment_nodes_between_data_centers(self):
        create_pool("large_gpu_other")
        set("//sys/pools/large_gpu/@strong_guarantee_resources", {"gpu": 40})
        set("//sys/pools/small_gpu/@strong_guarantee_resources", {"gpu": 40})

        blocking_op = run_sleeping_vanilla(
            job_count=10,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 4, "enable_gpu_layers": False},
        )

        big_op = run_sleeping_vanilla(
            job_count=3,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(big_op.id), 0.3))
        big_dc = self._get_data_center(big_op)

        opportunistic_op = run_sleeping_vanilla(
            job_count=2,
            spec={"pool": "large_gpu_other", "scheduling_segment_data_centers": [big_dc]},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(opportunistic_op.id), 0.2))

        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 0.5))

        op = run_sleeping_vanilla(
            job_count=2,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.2))
        wait(lambda: big_dc != self._get_data_center(op))

        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 0.5))

    @authors("eshcherbin")
    def test_revive_operation_data_center(self):
        update_controller_agent_config("snapshot_period", 300)

        ops = []
        for i in range(10):
            op = run_sleeping_vanilla(
                spec={"pool": "large_gpu"},
                task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
            )
            ops.append(op)

        dcs = []
        for op in ops:
            op_dc_path = op.get_path() + "/@runtime_parameters/scheduling_options_per_pool_tree/default/scheduling_segment_data_center"
            wait(lambda: exists(op_dc_path))
            dcs.append(get(op_dc_path))

        ops[-1].wait_for_fresh_snapshot()

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass

        for op, dc in zip(ops, dcs):
            wait(lambda: dc == self._get_data_center(op))

    @authors("eshcherbin")
    def test_profiling(self):
        set("//sys/pool_trees/default/@config/scheduling_segments/unsatisfied_segments_rebalancing_timeout", 1000000000)
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/scheduling_segments/unsatisfied_segments_rebalancing_timeout") == 1000000000)

        profiler = Profiler.at_scheduler(self.Env.create_native_client())

        for dc in TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS:
            wait(lambda: profiler.gauge("scheduler/segments/data_center_capacity", fixed_tags={"data_center": dc}).get() ==
                 8 * (TestSchedulingSegmentsMultiDataCenter.NUM_NODES / 2))

        fair_resource_amount_default_sensor = profiler.gauge("scheduler/segments/fair_resource_amount", fixed_tags={"segment": "default"})
        current_resource_amount_default_sensor = profiler.gauge("scheduler/segments/current_resource_amount", fixed_tags={"segment": "default"})
        fair_resource_amount_large_sensor = profiler.gauge("scheduler/segments/fair_resource_amount", fixed_tags={"segment": "large_gpu"})
        current_resource_amount_large_sensor = profiler.gauge("scheduler/segments/current_resource_amount", fixed_tags={"segment": "large_gpu"})

        wait(lambda: fair_resource_amount_default_sensor.get() == 0)
        wait(lambda: current_resource_amount_default_sensor.get() == 80)
        for dc in TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS:
            wait(lambda: fair_resource_amount_large_sensor.get(tags={"data_center": dc}) == 0)
            wait(lambda: current_resource_amount_large_sensor.get(tags={"data_center": dc}) == 0)

        blocking_op = run_sleeping_vanilla(
            job_count=20,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 4, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        wait(lambda: fair_resource_amount_default_sensor.get() == 80)
        wait(lambda: current_resource_amount_default_sensor.get() == 80)
        for dc in TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS:
            wait(lambda: fair_resource_amount_large_sensor.get(tags={"data_center": dc}) == 0)
            wait(lambda: current_resource_amount_large_sensor.get(tags={"data_center": dc}) == 0)

        op1 = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(op1.id), 0.1))
        wait(lambda: self._get_data_center(op1) in TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS)
        op1_dc = self._get_data_center(op1)

        time.sleep(3.0)

        wait(lambda: fair_resource_amount_default_sensor.get() == 72)
        wait(lambda: current_resource_amount_default_sensor.get() == 80)
        wait(lambda: fair_resource_amount_large_sensor.get(tags={"data_center": op1_dc}) == 8)
        wait(lambda: current_resource_amount_large_sensor.get(tags={"data_center": op1_dc}) == 0)

        set("//sys/pool_trees/default/@config/scheduling_segments/unsatisfied_segments_rebalancing_timeout", 1000)

        wait(lambda: fair_resource_amount_default_sensor.get() == 72)
        wait(lambda: current_resource_amount_default_sensor.get() == 72)
        wait(lambda: fair_resource_amount_large_sensor.get(tags={"data_center": op1_dc}) == 8)
        wait(lambda: current_resource_amount_large_sensor.get(tags={"data_center": op1_dc}) == 8)

        op2 = run_sleeping_vanilla(
            job_count=2,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(op2.id), 0.2))
        wait(lambda: self._get_data_center(op2) in TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS)
        op2_dc = self._get_data_center(op2)
        assert op1_dc != op2_dc

        wait(lambda: fair_resource_amount_default_sensor.get() == 56)
        wait(lambda: current_resource_amount_default_sensor.get() == 56)
        wait(lambda: fair_resource_amount_large_sensor.get(tags={"data_center": op1_dc}) == 8)
        wait(lambda: current_resource_amount_large_sensor.get(tags={"data_center": op1_dc}) == 8)
        wait(lambda: fair_resource_amount_large_sensor.get(tags={"data_center": op2_dc}) == 16)
        wait(lambda: current_resource_amount_large_sensor.get(tags={"data_center": op2_dc}) == 16)

    @authors("eshcherbin")
    def test_fail_large_gpu_operation_started_in_several_trees(self):
        node = list(ls("//sys/cluster_nodes"))[0]
        set("//sys/pool_trees/default/@config/nodes_filter", "!other")
        create_pool_tree("other", config={"nodes_filter": "other", "main_resource": "gpu"})
        set("//sys/cluster_nodes/" + node + "/@user_tags/end", "other")

        big_op = run_sleeping_vanilla(
            spec={"pool_trees": ["default", "other"]},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: big_op.get_state() == "failed")

        small_op = run_test_vanilla(
            "sleep 1",
            job_count=2,
            spec={"pool_trees": ["default", "other"]},
            task_patch={"gpu_limit": 4, "enable_gpu_layers": False},
        )
        small_op.track()

    @authors("eshcherbin")
    def test_fail_operations_with_custom_tag_filter(self):
        blocking_op = run_sleeping_vanilla(
            job_count=5,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 0.5))
        wait(lambda: self._get_data_center(blocking_op) in TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS)
        dc = self._get_data_center(blocking_op)
        other_dc = [dz for dz in TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS if dz != dc][0]

        op1 = run_sleeping_vanilla(
            spec={"pool": "large_gpu", "scheduling_tag_filter": dc},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        op1.wait_for_state("failed")

        op2 = run_sleeping_vanilla(
            spec={"pool": "large_gpu", "scheduling_tag_filter": other_dc},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(op2.id), 0.1))

        op3 = run_sleeping_vanilla(
            spec={"pool": "large_gpu", "scheduling_tag_filter": "{} & !{}".format(dc, other_dc)},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(op3.id), 0.1))
        time.sleep(1.0)
        op3.wait_for_state("running")
        wait(lambda: are_almost_equal(self._get_usage_ratio(op3.id), 0.0))

        op4 = run_sleeping_vanilla(
            spec={"pool": "large_gpu", "scheduling_tag_filter": dc},
            task_patch={"gpu_limit": 4, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(op4.id), 0.05))
        time.sleep(1.0)
        op4.wait_for_state("running")
        wait(lambda: are_almost_equal(self._get_usage_ratio(op4.id), 0.0))

    @authors("eshcherbin")
    def test_min_remaining_feasible_capacity_assignment_heuristic(self):
        set("//sys/pool_trees/default/@config/scheduling_segments/data_center_assignment_heuristic", "min_remaining_feasible_capacity")
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/scheduling_segments/data_center_assignment_heuristic") ==
             "min_remaining_feasible_capacity")

        op1 = run_sleeping_vanilla(
            job_count=1,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: self._get_data_center(op1) in TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS)

        op2 = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: self._get_data_center(op2) in TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS)

        op3 = run_sleeping_vanilla(
            job_count=4,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: self._get_data_center(op3) in TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS)

        op4 = run_sleeping_vanilla(
            job_count=3,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: self._get_data_center(op4) in TestSchedulingSegmentsMultiDataCenter.DATA_CENTERS)

        assert self._get_data_center(op1) == self._get_data_center(op2) == self._get_data_center(op4)
        assert self._get_data_center(op1) != self._get_data_center(op3)
