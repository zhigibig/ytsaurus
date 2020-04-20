import argparse
import logging
import os
import re

from yt.wrapper import YPath
from yt.wrapper.cypress_commands import list as yt_list, create
from yt.wrapper.errors import YtHttpResponseError
from yt.wrapper.http_helpers import get_proxy_url, get_user_name
from yt.wrapper.operation_commands import get_operation_url

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(name)s - %(message)s')
logger = logging.getLogger(__name__)


class SparkCluster(object):
    def __init__(self, master_endpoint, master_web_ui_url, master_rest_endpoint, operation_id, shs_url,
                 spark_cluster_version):
        self.master_endpoint = master_endpoint
        self.master_web_ui_url = master_web_ui_url
        self.master_rest_endpoint = master_rest_endpoint
        self.operation_id = operation_id
        self.shs_url = shs_url
        self.spark_cluster_version = spark_cluster_version

    def operation_url(self, client=None):
        return get_operation_url(self.operation_id, client=client)


class SparkDiscovery(object):
    def __init__(self, discovery_path=None, spark_id=None):
        discovery_path = discovery_path or os.getenv("SPARK_YT_DISCOVERY_PATH")
        self.base_discovery_path = YPath(discovery_path)
        self.spark_id = spark_id

    @staticmethod
    def get(path, client=None):
        try:
            return yt_list(path, client=client)[0]
        except YtHttpResponseError as e:
            logging.warning("Failed to get path {}, message: {}".format(path, e.message))
            for inner in e.inner_errors:
                logging.warning("Failed to get path {}, inner message {}".format(path, inner["message"]))

    def create(self, client):
        create("map_node", self.discovery(), recursive=True, ignore_existing=True, client=client)
        create("map_node", self.event_log(), recursive=True, ignore_existing=True, client=client)

    def discovery(self):
        if self.spark_id:
            return self.base_discovery_path.join("instances").join(self.spark_id)
        else:
            return self.base_discovery_path.join("discovery")

    def operation(self):
        return self.discovery().join("operation")

    def logs(self):
        return self.base_discovery_path.join("logs")

    def event_log(self):
        return self.logs().join("event_log")

    def master_spark(self):
        return self.discovery().join("address") if self.spark_id else self.discovery().join("spark_address")

    def master_webui(self):
        return self.discovery().join("webui")

    def master_rest(self):
        return self.discovery().join("rest")

    def shs(self):
        return self.discovery().join("shs")

    def stderr(self):
        return self.logs().join("stderr")

    def spark_cluster_version(self):
        return self.discovery().join("version")


def parse_memory(memory):
    if isinstance(memory, int):
        return memory
    if memory is None:
        return None
    units = {"gb": 1024 * 1024 * 1024, "mb": 1024 * 1024, "kb": 1024, "bb": 1, "b": 1}
    m = re.match(r"(\d+)(.*)", memory)
    value = int(m.group(1))
    unit = m.group(2).lower().strip()
    if len(unit) <= 1:
        unit = unit + "b"
    return value * units[unit]


def format_memory(memory_bytes):
    if memory_bytes is None:
        return None
    units = {"gb": 1024 * 1024 * 1024, "mb": 1024 * 1024, "kb": 1024, "bb": 1, "b": 1}
    if memory_bytes % units["gb"] == 0:
        return "{}G".format(memory_bytes // units["gb"])
    if memory_bytes % units["mb"] == 0:
        return "{}M".format(memory_bytes // units["mb"])
    if memory_bytes % units["kb"] == 0:
        return "{}K".format(memory_bytes // units["kb"])
    return "{}B".format(memory_bytes)


def get_spark_master(discovery, rest, yt_client=None):
    master_path = discovery.master_rest() if rest else discovery.master_spark()
    master = SparkDiscovery.get(master_path, client=yt_client)
    return "spark://{0}".format(master)


def default_token():
    token = os.getenv("YT_TOKEN")
    if token is None:
        with open(os.path.join(os.getenv("HOME"), ".yt", "token")) as f:
            token = f.readline().strip()
    return token


def base_spark_conf(client, discovery):
    yt_proxy = get_proxy_url(required=True, client=client)
    yt_user = get_user_name(client=client)
    spark_cluster_version = SparkDiscovery.get(discovery.spark_cluster_version(), client=client)
    return {
        "spark.hadoop.yt.proxy": yt_proxy,
        "spark.hadoop.yt.user": yt_user,
        "spark.master.rest.enabled": "true",
        "spark.eventLog.dir": "yt:/{}".format(discovery.event_log()),
        "spark.yt.cluster.version": spark_cluster_version
    }


def set_conf(conf, dict_conf):
    if dict_conf is not None:
        for (key, value) in dict_conf.items():
            conf.set(key, value)


def default_discovery_dir():
    return os.getenv("SPARK_YT_DISCOVERY_DIR") or YPath("//home").join(os.getenv("USER")).join("spark-tmp")


def default_proxy():
    return os.getenv("YT_PROXY")


def get_default_arg_parser(**kwargs):
    parser = argparse.ArgumentParser(**kwargs)
    parser.add_argument("--id", required=False)
    parser.add_argument("--discovery-path", required=False)
    parser.add_argument("--discovery-dir", required=False)
    parser.add_argument("--proxy", required=False, default=default_proxy())
    return parser


def parse_args(parser=None, parser_arguments=None):
    parser_arguments = parser_arguments or {}
    parser = parser or get_default_arg_parser(**parser_arguments)
    args, unknown_args = parser.parse_known_args()
    args.discovery_path = args.discovery_path or args.discovery_dir or default_discovery_dir()
    return args, unknown_args
