import logging
import os
import sys
from contextlib import contextmanager

from yt.wrapper import YtClient, get
from yt.wrapper.http_helpers import get_token

logger = logging.getLogger(__name__)

from .utils import default_token, default_discovery_dir, get_spark_master, set_conf, \
    SparkDiscovery, parse_memory, format_memory, base_spark_conf
from .conf import read_remote_conf, read_global_conf, spyt_jar_path, spyt_python_path, validate_versions_compatibility, \
    read_cluster_conf, SELF_VERSION
from .enabler import set_enablers, set_except_enablers, get_enablers_list


class Defaults(object):
    LOCAL_CONF_PATH = os.path.join(os.getenv("HOME"), "spyt.yaml") if os.getenv("HOME") else None


class SparkInfo(object):
    def __init__(self, spark):
        self.spark = spark
        pass

    def _repr_html_(self):
        yt_proxy = self.spark.conf.get("spark.hadoop.yt.proxy")
        yt_token = self.spark.conf.get("spark.hadoop.yt.token")
        client = YtClient(proxy=yt_proxy, token=yt_token)
        discovery_path = self.spark.conf.get("spark.yt.master.discoveryPath")
        discovery = SparkDiscovery(discovery_path=discovery_path)
        master_web_ui = SparkDiscovery.get(discovery.master_webui(), client=client)
        spyt_version = self.spark.conf.get("spark.yt.version")
        spyt_cluster_version = self.spark.conf.get("spark.yt.cluster.version")
        return """
            <div>
                <p><b>SPYT</b></p>
                
                <p><a href="{master_web_ui}">Master Web UI</a></p>
                <dl>
                  <dt>Yt Cluster</dt>
                    <dd><code>{yt_proxy}</code></dd>
                  <dt>SPYT Cluster version</dt>
                    <dd><code>{spyt_cluster_version}</code></dd>
                  <dt>SPYT Library version</dt>
                    <dd><code>{spyt_version}</code></dd>
                </dl>
                
                {sc_HTML}
            </div>
        """.format(
            yt_proxy=yt_proxy,
            master_web_ui=master_web_ui,
            spyt_cluster_version=spyt_cluster_version,
            spyt_version=spyt_version,
            sc_HTML=self.spark.sparkContext._repr_html_()
        )


def info(spark):
    return SparkInfo(spark)


@contextmanager
def spark_session(num_executors=None,
                  yt_proxy=None,
                  discovery_path=None,
                  app_name=None,
                  cores_per_executor=None,
                  executor_memory_per_core=None,
                  driver_memory=None,
                  dynamic_allocation=False,
                  spark_conf_args=None,
                  local_conf_path=Defaults.LOCAL_CONF_PATH,
                  spark_id=None,
                  client=None):
    spark_session_already_existed = _spark_session_exists()
    spark = connect(
        num_executors=num_executors,
        yt_proxy=yt_proxy,
        discovery_path=discovery_path,
        app_name=app_name,
        cores_per_executor=cores_per_executor,
        executor_memory_per_core=executor_memory_per_core,
        driver_memory=driver_memory,
        dynamic_allocation=dynamic_allocation,
        spark_conf_args=spark_conf_args,
        local_conf_path=local_conf_path,
        spark_id=spark_id,
        client=client
    )
    exception = None
    try:
        yield spark
    except Exception as e:
        exception = e
    finally:
        if spark_session_already_existed:
            _raise_first(exception)
        else:
            stop(spark, exception)


def get_spark_discovery(discovery_path, spark_id, conf):
    spark_id = spark_id or conf.get("spark_id")
    discovery_path = discovery_path or conf.get("discovery_path") or conf.get(
        "discovery_dir") or default_discovery_dir()
    return SparkDiscovery(discovery_path=discovery_path, spark_id=spark_id)


def create_yt_client(yt_proxy, conf):
    yt_proxy = yt_proxy or conf.get("yt_proxy", os.getenv("YT_PROXY"))
    yt_token = conf.get("yt_token") or default_token()
    return YtClient(proxy=yt_proxy, token=yt_token)


def create_yt_client_spark_conf(yt_proxy, spark_conf):
    yt_proxy = yt_proxy or spark_conf.get("spark.hadoop.yt.proxy") or os.getenv("SPARK_YT_PROXY")
    yt_token = spark_conf.get("spark.hadoop.yt.token") or os.getenv("SPARK_YT_TOKEN")
    return YtClient(proxy=yt_proxy, token=yt_token)


def _configure_client_mode(spark_conf,
                           discovery_path,
                           local_conf,
                           spark_id,
                           client=None):
    discovery = get_spark_discovery(discovery_path, spark_id, local_conf)
    master = get_spark_master(discovery, rest=False, yt_client=client)
    set_conf(spark_conf, base_spark_conf(client=client, discovery=discovery))
    spark_conf.set("spark.master", master)
    os.environ["SPARK_YT_TOKEN"] = get_token(client=client)
    spark_conf.set("spark.yt.master.discoveryPath", str(discovery.base_discovery_path))

    spyt_version = SELF_VERSION
    spark_cluster_version = spark_conf.get("spark.yt.cluster.version")
    validate_versions_compatibility(spyt_version, spark_cluster_version)
    spark_conf.set("spark.yt.version", spyt_version)
    spark_conf.set("spark.yt.jars", "yt:/{}".format(spyt_jar_path(spyt_version)))
    spark_conf.set("spark.yt.pyFiles", "yt:/{}".format(spyt_python_path(spyt_version)))


def _validate_resources(num_executors, cores_per_executor, executor_memory_per_core):
    if num_executors and (not isinstance(num_executors, int) or num_executors < 1):
        raise AssertionError("Invalid number of executors")

    if cores_per_executor and (not isinstance(cores_per_executor, int) or cores_per_executor < 1):
        raise AssertionError("Invalid number of cores per executor")

    if executor_memory_per_core and executor_memory_per_core < 1:
        raise AssertionError("Invalid amount of memory per core")


def _set_none_safe(conf, key, value):
    if value:
        conf.set(key, value)


class Environment(object):
    IS_CLUSTER_PYTHON_PATH = False

    @staticmethod
    def configure_python_path(python_cluster_paths):
        python_version = "%d.%d" % sys.version_info[:2]
        if python_version not in python_cluster_paths:
            raise RuntimeError("Python version {} is not supported".format(python_version))
        os.environ["PYSPARK_PYTHON"] = python_cluster_paths[python_version]
        Environment.IS_CLUSTER_PYTHON_PATH = True

    @staticmethod
    def unset_python_path():
        if Environment.IS_CLUSTER_PYTHON_PATH:
            del(os.environ["PYSPARK_PYTHON"])
            Environment.IS_CLUSTER_PYTHON_PATH = False


def _read_local_conf(conf_path):
    import yaml
    if conf_path and os.path.isfile(conf_path):
        with open(conf_path) as f:
            return yaml.load(f, Loader=yaml.BaseLoader)
    return {}


def _configure_resources(spark_conf, local_conf,
                         num_executors,
                         cores_per_executor,
                         executor_memory_per_core,
                         driver_memory,
                         dynamic_allocation):
    num_executors = num_executors or local_conf.get("num_executors")
    cores_per_executor = cores_per_executor or local_conf.get("cores_per_executor")
    executor_memory_per_core = parse_memory(executor_memory_per_core or local_conf.get("executor_memory_per_core"))
    _validate_resources(num_executors, cores_per_executor, executor_memory_per_core)
    driver_memory = parse_memory(driver_memory or local_conf.get("driver_memory"))
    executor_memory = executor_memory_per_core * cores_per_executor if executor_memory_per_core and cores_per_executor else None
    max_cores = num_executors * cores_per_executor if num_executors and cores_per_executor else None

    _set_none_safe(spark_conf, "spark.dynamicAllocation.enabled", dynamic_allocation)
    _set_none_safe(spark_conf, "spark.dynamicAllocation.executorIdleTimeout", "10m")
    _set_none_safe(spark_conf, "spark.dynamicAllocation.cachedExecutorIdleTimeout", "60m")
    _set_none_safe(spark_conf, "spark.cores.max", max_cores)
    _set_none_safe(spark_conf, "spark.dynamicAllocation.maxExecutors", num_executors)
    _set_none_safe(spark_conf, "spark.executor.cores", cores_per_executor)
    _set_none_safe(spark_conf, "spark.executor.memory", format_memory(executor_memory))
    _set_none_safe(spark_conf, "spark.driver.memory", format_memory(driver_memory))
    _set_none_safe(spark_conf, "spark.driver.maxResultSize", format_memory(driver_memory))


def _build_spark_conf(num_executors=None,
                      yt_proxy=None,
                      discovery_path=None,
                      app_name=None,
                      cores_per_executor=None,
                      executor_memory_per_core=None,
                      driver_memory=None,
                      dynamic_allocation=None,
                      spark_conf_args=None,
                      local_conf_path=None,
                      spark_id=None,
                      client=None):
    from pyspark import SparkConf

    is_client_mode = os.getenv("IS_SPARK_CLUSTER") is None
    local_conf = {}
    spark_conf = SparkConf()

    if is_client_mode:
        local_conf = _read_local_conf(local_conf_path)
        app_name = app_name or "PySpark for {}".format(os.getenv("USER"))
        client = client or create_yt_client(yt_proxy, local_conf)
        _configure_client_mode(spark_conf, discovery_path, local_conf, spark_id, client)
        spark_cluster_version = spark_conf.get("spark.yt.cluster.version")
        spark_cluster_conf_path = spark_conf.get("spark.yt.cluster.confPath")
    else:
        client = client or create_yt_client_spark_conf(yt_proxy, spark_conf)
        spark_cluster_version = os.getenv("SPARK_CLUSTER_VERSION")
        spark_cluster_conf_path = os.getenv("SPARK_YT_CLUSTER_CONFPATH")

    _set_none_safe(spark_conf, "spark.app.name", app_name)
    _configure_resources(spark_conf, local_conf,
                         num_executors, cores_per_executor, executor_memory_per_core,
                         driver_memory, dynamic_allocation)

    global_conf = read_global_conf(client=client)
    remote_conf = read_remote_conf(global_conf, spark_cluster_version, client=client)
    set_conf(spark_conf, remote_conf["spark_conf"])

    if is_client_mode:
        Environment.configure_python_path(remote_conf["python_cluster_paths"])

    spark_cluster_conf = read_cluster_conf(spark_cluster_conf_path, client=client).get("spark_conf") or {}
    enablers = get_enablers_list(spark_cluster_conf)
    set_enablers(spark_conf, spark_conf_args, spark_cluster_conf, enablers)
    set_except_enablers(spark_conf, spark_cluster_conf, enablers)
    set_except_enablers(spark_conf, spark_conf_args, enablers)

    return spark_conf


def connect(num_executors=5,
            yt_proxy=None,
            discovery_path=None,
            app_name=None,
            cores_per_executor=4,
            executor_memory_per_core="4G",
            driver_memory="1G",
            dynamic_allocation=True,
            spark_conf_args=None,
            local_conf_path=Defaults.LOCAL_CONF_PATH,
            spark_id=None,
            client=None):
    from pyspark.sql import SparkSession
    conf = _build_spark_conf(
        num_executors=num_executors,
        yt_proxy=yt_proxy,
        discovery_path=discovery_path,
        app_name=app_name,
        cores_per_executor=cores_per_executor,
        executor_memory_per_core=executor_memory_per_core,
        driver_memory=driver_memory,
        dynamic_allocation=dynamic_allocation,
        spark_conf_args=spark_conf_args,
        local_conf_path=local_conf_path,
        spark_id=spark_id,
        client=client
    )
    if _spark_session_exists():
        logger.warning("SparkSession already exists and will be reused. Some configurations may not be applied. "
                       "You can use spyt.stop(spark) method to close previous session")

    spark = SparkSession.builder.config(conf=conf).getOrCreate()

    logger.info("SPYT Cluster version: {}".format(spark.conf.get("spark.yt.cluster.version", default=None)))
    logger.info("SPYT library version: {}".format(spark.conf.get("spark.yt.version", default=None)))

    return spark


def stop(spark, exception=None):
    is_client_mode = spark.conf.get("spark.submit.deployMode") == "client"
    _try_with_safe_finally(
        lambda:  spark.stop(),
        lambda e1: _try_with_safe_finally(
            lambda: _shutdown_jvm(spark) if is_client_mode else _close_yt_client(spark),
            lambda e2: _try_with_safe_finally(
                lambda: Environment.unset_python_path(),
                lambda e3: _raise_first(exception, e1, e2, e3)
            )
        )
    )


def jvm_process_pid():
    from pyspark import SparkContext
    return SparkContext._gateway.proc.pid


def _close_yt_client(spark):
    spark._jvm.ru.yandex.spark.yt.fs.YtClientProvider.close()


def _try_with_safe_finally(try_func, finally_func):
    exception = None
    try:
        try_func()
    except Exception as e:
        logger.error("Unexpected error {}".format(e.message))
        exception = e
    finally:
        finally_func(exception)


def _raise_first(*exceptions):
    exception = None
    for e in exceptions:
        exception = exception or e
    if exception:
        raise exception


def _shutdown_jvm(spark):
    from pyspark import SparkContext
    from pyspark.sql import SparkSession
    from subprocess import Popen
    proc = SparkContext._gateway.proc
    if not isinstance(proc, Popen):
        logger.warning("SparkSession cannot be closed properly, please update yandex-spyt and Spark cluster")
        return
    proc.stdin.close()
    SparkContext._gateway.shutdown()
    SparkContext._gateway = None
    SparkContext._jvm = None
    spark._jvm = None
    SparkSession.builder._options = {}


def _spark_session_exists():
    from pyspark.sql import SparkSession
    return SparkSession._instantiatedSession is not None
