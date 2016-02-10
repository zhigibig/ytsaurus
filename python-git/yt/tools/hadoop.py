from yt.wrapper.common import generate_uuid, run_with_retries
from yt.wrapper.http import get_retriable_errors
import yt.packages.requests as requests
from yt.json import loads_as_bytes

import os
import json
import time
from subprocess import check_output

class HiveError(Exception):
    pass

class Hive(object):
    def __init__(self, airflow_name, hcatalog_host, hdfs_host, hive_importer_library, java_path=""):
        self.airflow_name = airflow_name
        self.hcatalog_host = hcatalog_host
        self.hdfs_host = hdfs_host
        self.hive_importer_library = hive_importer_library
        self.java_path = java_path

    def get_table_config_and_files(self, database, table):
        hive_cmd = "exec=set hive.ddl.output.format=json; use {0}; desc extended {1};".format(database, table)
        templeton_url = 'http://{0}/templeton/v1/ddl?user.name=none'.format(self.hcatalog_host)
        hive_response = json.loads(check_output(["curl", "--silent", "--show-error", "-d", hive_cmd, "-X", "POST", templeton_url]))

        config = hive_response["stdout"].strip()
        location = json.loads(config)["tableInfo"]["sd"]["location"]
        relative_path = location.lstrip("hdfs://").split("/", 1)[1]

        webhdfs_url = "http://{0}/webhdfs/v1/{1}?op=LISTSTATUS&user.name=none".format(self.hdfs_host, relative_path)
        list_response = json.loads(check_output(["curl", "--silent", "--show-error", webhdfs_url]))

        if "FileStatuses" not in list_response:
            raise HiveError("Incorrect response: " + str(list_response))
        return config, [os.path.join(relative_path, filename["pathSuffix"]) for filename in list_response["FileStatuses"]["FileStatus"]]

    def get_read_command(self, read_config):
        return """
set -uxe

while true; do
    set +e
    read -r table;
    result="$?"
    set -e
    if [ "$result" != "0" ]; then break; fi;

    {jar} -J-Xmx1024m xf ./{hive_importer_library} libhadoop.so libsnappy.so.1 >&2;
    curl --silent --show-error "http://{hdfs_host}/webhdfs/v1/${{table}}?op=OPEN&user.name=none" >output;
    LANG=en_US.UTF-8 {java} -Xmx1024m -Dhadoop.root.logger=INFO -Djava.library.path=./ -jar ./{hive_importer_library} -file output -config '{read_config}';
done
"""\
            .format(java=os.path.join(self.java_path, "java"),
                    jar=os.path.join(self.java_path, "jar"),
                    hive_importer_library=os.path.basename(self.hive_importer_library),
                    hdfs_host=self.hdfs_host,
                    read_config=read_config)

class AirflowError(Exception):
    pass

class Airflow(object):
    def __init__(self, address, dag_registered_wait_time):
        self._address = address
        self._headers = {
            "Content-Type": "application/json"
        }
        # Special sleep to ensure that dag is registered at airflow.
        self._dag_registered_wait_time = dag_registered_wait_time
        self.message_queue = None

    def add_task(self, task_type, source_cluster, source_path, destination_cluster, destination_path, owner):
        if task_type == "distcp":
            pattern = "{cluster}{path}"
        elif task_type == "hivecp":
            pattern = "{cluster}.{path}"
        elif task_type == "hbasecp":
            pattern = "{cluster}/{path}"
        else:
            raise AirflowError("Unsupported airflow task type: {0}".format(task_type))

        src = pattern.format(cluster=source_cluster, path=source_path)
        if task_type == "hbasecp":
            dst = destination_cluster
        else:
            dst = pattern.format(cluster=destination_cluster, path=destination_path)

        task_id = generate_uuid()
        data = {
            "owner": owner,
            "operation_type": task_type,
            "src": src,
            "dst": dst,
            "dag_id": task_id
        }

        submit_url = "http://{0}/admin/airflow/submit".format(self._address)
        # XXX(asaitgalin): Retries will be added when HDPDEV-279 is done.
        response = requests.post(submit_url, data=json.dumps(data), headers=self._headers)
        response.raise_for_status()

        # XXX(asaitgalin): not using "id" instead of "_id" here because this requires
        # web interface improvements. See YT-3415 for details.
        if self.message_queue is not None:
            self.message_queue.put({"type": "operation_started", "operation": {"_id": task_id, "cluster_name": self._name}})

        time.sleep(self._dag_registered_wait_time)

        return task_id

    def get_task_info(self, task_id):
        graph_url = "http://{0}/admin/airflow/graph".format(self._address)
        params = {
            "dag_id": task_id,
            "no_render": "true"
        }
        response = self._make_get_request(graph_url, params=params)
        if response.status_code == 400:
            raise AirflowError("Task {0} not found".format(task_id))

        info = loads_as_bytes(response.content)
        return info.values()[0] if info else {}

    def get_task_log(self, task_id):
        info = self.get_task_info(task_id)
        if not info:
            return ""

        log_url = "http://{0}/admin/airflow/log".format(self._address)
        params = {
            "dag_id": task_id,
            "task_id": info["task_id"],
            "execution_date": info["execution_date"],
            "no_render": "true"
        }
        response = self._make_get_request(log_url, params=params)
        if response.status_code == 400:
            raise AirflowError("Task {0} not found".format(task_id))

        return response.content

    @staticmethod
    def is_task_unsuccessfully_finished(state):
        return state in ["shutdown", "failed", "upstream_failed"]

    @staticmethod
    def is_task_finished(state):
        return state in ["success", "shutdown", "failed", "skipped", "upstream_failed"]

    def _make_get_request(self, url, params):
        def make_request():
            response = requests.get(url, params=params, headers=self._headers)
            return response

        return run_with_retries(make_request, exceptions=get_retriable_errors())

class Hdfs(object):
    def __init__(self, airflow_name):
        self.airflow_name = airflow_name

class HBase(object):
    def __init__(self, airflow_name):
        self.airflow_name = airflow_name
