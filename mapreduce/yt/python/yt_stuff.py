from functools import wraps
import logging
import os
import shutil
import sys
import tempfile
import time
import uuid
import signal
import fcntl

import yt_version

import pytest
import yatest.common

import devtools.swag.daemon
import devtools.swag.ports

_YT_ARCHIVE_NAME = "yt/packages/yt.tar" # comes by FROM_SANDBOX
_YT_THOR_ARCHIVE_NAME = "yt/packages/yt_thor.tar" # comes by FROM_SANDBOX
_YT_PREFIX = "//"
_YT_MAX_START_RETRIES = 3

class YtConfig(object):
    def __init__(self, **kwargs):
        self.fqdn = kwargs.get("fqdn", "localhost")
        self.yt_id = kwargs.get("yt_id")

        self.proxy_port = kwargs.get("proxy_port")
        self.node_count = kwargs.get("node_count")

        self.node_config = kwargs.get("node_config")

        config_with_disabled_retries = tempfile.NamedTemporaryFile(delete=False)
        config_with_disabled_retries.write("""
            {
                "bus_server" = {
                    "bind_retry_count" = 1;
                };
            }"""
        )
        config_with_disabled_retries.close()

        self.node_config = kwargs.get("node_config") or config_with_disabled_retries.name
        self.scheduler_config = kwargs.get("scheduler_config") or config_with_disabled_retries.name
        self.master_config = kwargs.get("master_config") or config_with_disabled_retries.name

        proxy_config_with_disabled_retries = tempfile.NamedTemporaryFile(delete=False)
        proxy_config_with_disabled_retries.write("""
            {
                "bind_retry_count": 1
            }
            """
        )
        proxy_config_with_disabled_retries.close()

        self.proxy_config = kwargs.get("proxy_config") or proxy_config_with_disabled_retries.name

        self.yt_path = kwargs.get("yt_path")

        self.save_all_logs = kwargs.get("save_all_logs")
        self.enable_debug_log = kwargs.get("enable_debug_log")
        self.yt_work_dir = kwargs.get("yt_work_dir")
        self.keep_yt_work_dir = kwargs.get("keep_yt_work_dir")
        self.ram_drive_path = kwargs.get("ram_drive_path")
        self.local_cypress_dir = kwargs.get("local_cypress_dir")

        self.wait_tablet_cell_initialization = kwargs.get("wait_tablet_cell_initialization")
        self.operations_memory_limit = kwargs.get("operations_memory_limit") or (25 * 1024 * 1024 * 1024)


class YtStuff(object):
    def __init__(self, config=None):
        self.config = config or YtConfig()

        self.yt_id = self.config.yt_id or str(uuid.uuid4())
        self.yt_proxy_port = None

        self._prepare_logger()
        self._prepare_files()
        self._prepare_env()
        self._import_wrapper()

    def _prepare_logger(self):
        self.logger = logging.getLogger()

    def _log(self, *args, **kwargs):
        self.logger.debug(*args, **kwargs)

    def _timing(method):
        @wraps(method)
        def wrap(self, *args, **kwargs):
            start_time = time.time()
            ret = method(self, *args, **kwargs)
            finish_time = time.time()
            self._log("%s time: %f", method.__name__, finish_time - start_time)
            return ret
        return wrap

    @_timing
    def _extract_tar(self, tgz, where):
        #import tarfile
        #tarfile.open(tgz).extractall(path=where)
        import subprocess
        subprocess.check_output(['tar', '-xf', tgz], cwd=where, stderr=subprocess.STDOUT)

    @_timing
    def _pack_tar(self, archive_path, file_path):
        import subprocess
        subprocess.check_output(['tar', '-cvzf', archive_path, file_path], stderr=subprocess.STDOUT)

    def _prepare_files(self):
        build_path = yatest.common.runtime.build_path()
        work_path = yatest.common.runtime.work_path()

        self.tmpfs_path = self.config.ram_drive_path or yatest.common.get_param("ram_drive_path")
        if self.tmpfs_path:
            self.tmpfs_path = tempfile.mkdtemp(prefix="yt_", dir=self.tmpfs_path)

        # Folders
        self.yt_path = tempfile.mkdtemp(dir=work_path, prefix="yt_") if self.config.yt_path is None else self.config.yt_path
        self.yt_bins_path = os.path.join(self.yt_path, "bin")
        if yt_version.YT_VERSION == '17_5':
            self.yt_python_path = os.path.join(self.yt_path, "python")
        self.yt_node_path = os.path.join(self.yt_path, "node")
        self.yt_node_bin_path = os.path.join(self.yt_node_path, "bin")
        self.yt_node_modules_path = os.path.join(self.yt_path, "node_modules")
        self.yt_thor_path = os.path.join(self.yt_path, "yt-thor")
        # Binaries
        self.mapreduce_yt_path = [yatest.common.python_path(), os.path.join(self.yt_bins_path, "mapreduce-yt")]
        self.yt_local_path = [yatest.common.python_path(), os.path.join(self.yt_bins_path, "yt_local")]

        yt_archive_path = os.path.join(build_path, _YT_ARCHIVE_NAME if yt_version.YT_VERSION == "17_5" else _YT_THOR_ARCHIVE_NAME)
        self._extract_tar(yt_archive_path, self.yt_path)
        self._replace_binaries()

        user_yt_work_dir_base =  self.config.yt_work_dir or yatest.common.get_param("yt_work_dir")
        if user_yt_work_dir_base:
            self.yt_work_dir = os.path.join(user_yt_work_dir_base, "yt_wd")
        else:
            self.yt_work_dir = yatest.common.output_path("yt_wd")

        if not os.path.isdir(self.yt_work_dir):
            os.mkdir(self.yt_work_dir)

        self.yt_wrapper_log_path = os.path.join(self.yt_work_dir, "yt_wrapper_%s.log" % self.yt_id)

        # Create files for yt_local stdout/stderr. We can't just open them in 'w' mode, because
        # devtools.swag.daemon.run_daemon reads from them. So we create files and open them in 'r+' mode.
        yt_local_out_path = os.path.join(self.yt_work_dir, "yt_local_%s.out" % self.yt_id)
        yt_local_err_path = os.path.join(self.yt_work_dir, "yt_local_%s.err" % self.yt_id)
        open(yt_local_out_path, 'a').close()
        open(yt_local_err_path, 'a').close()
        self.yt_local_out = open(yt_local_out_path, "r+")
        self.yt_local_err = open(yt_local_err_path, "r+")
        self.is_running = False

    def _replace_binaries(self):
        version = yt_version.YT_VERSION
        if version == '17_5':
            return

        for path in (self.yt_bins_path, self.yt_node_bin_path):
            if not os.path.exists(path):
                os.makedirs(path)

        yt2_arcadia_path = yatest.common.binary_path('yt/packages/contrib/python/yt/bin/yt/yt')
        shutil.copy(yt2_arcadia_path, os.path.join(self.yt_bins_path, 'yt2'))

        self.mapreduce_yt_path = [yatest.common.binary_path('yt/packages/contrib/python/yt/bin/mapreduce-yt/mapreduce-yt')]
        self.yt_local_path = [yatest.common.binary_path('yt/packages/contrib/python/yt_local/bin/local/yt_local')]
        self.yt_env_watcher_dir_path = yatest.common.binary_path('yt/packages/contrib/python/yt_local/bin/watcher')

        yt_server_arcadia_path = yatest.common.binary_path('yt/packages/yt/{}/yt/server/ytserver_program/ytserver'.format(version))
        yt_server_custom_path = yatest.common.get_param("yt_ytserver_path")
        shutil.copy(yt_server_custom_path or yt_server_arcadia_path, os.path.join(self.yt_bins_path, 'ytserver'))

        yt_node_arcadia_path = yatest.common.binary_path('yt/packages/yt/{}/yt/nodejs/targets/bin/ytnode'.format(version))
        shutil.copy(yt_node_arcadia_path, os.path.join(self.yt_node_bin_path, 'nodejs'))

        node_modules_archive_path = yatest.common.binary_path('yt/packages/yt/{}/yt/node_modules/resource.tar.gz'.format(version))
        self._extract_tar(node_modules_archive_path, self.yt_path)

        yt_node_path = yatest.common.binary_path('yt/packages/yt/{}/yt/nodejs/targets/package'.format(version))
        shutil.copytree(yt_node_path, os.path.join(self.yt_node_modules_path, 'yt'))

    def _prepare_env(self):
        self.env = {}
        self.env["PATH"] = ":".join([
                self.yt_bins_path,
                self.yt_env_watcher_dir_path,
                self.yt_node_path,
                self.yt_node_bin_path,
            ])
        self.env["NODE_MODULES"] = self.yt_node_modules_path
        self.env["NODE_PATH"] = ":".join([
                self.yt_node_path,
                self.yt_node_modules_path,
            ])
        if yt_version.YT_VERSION == '17_5':
            self.env["PYTHONPATH"] = self.yt_python_path
        self.env["YT_LOCAL_THOR_PATH"] = self.yt_thor_path
        self.env["YT_ENABLE_VERBOSE_LOGGING"] = "1"
        self.env["YT_LOG_LEVEL"] = "DEBUG"

    def _import_wrapper(self):
        if yt_version.YT_VERSION == '17_5':
            sys.path.insert(0, self.yt_python_path)

        import yt.wrapper
        import yt.logger

        yt.logger.LOGGER.setLevel(logging.DEBUG)
        handler = logging.FileHandler(self.yt_wrapper_log_path)
        handler.setFormatter(yt.logger.BASIC_FORMATTER)
        handler.setLevel(logging.DEBUG)
        yt.logger.LOGGER.handlers = [handler]

        self.yt_wrapper = yt.wrapper
        self.yt_wrapper.config["prefix"] = _YT_PREFIX
        self.yt_wrapper.config["pickling"]["python_binary"] = yatest.common.python_path()

        self.yt_client = yt.wrapper.YtClient()
        self.yt_client.config["prefix"] = _YT_PREFIX
        self.yt_client.config["pickling"]["python_binary"] = yatest.common.python_path()

    def _start_local_yt(self):
        self._log("Try to start local YT with id=%s", self.yt_id)
        try:
            # Prepare arguments.
            args = [
                "start",
                "--sync",
                "--id", self.yt_id,
                "--path", self.yt_work_dir,
                "--fqdn", self.config.fqdn,
                "--operations-memory-limit", str(self.config.operations_memory_limit),
            ]

            if self.config.wait_tablet_cell_initialization:
                args += ["--wait-tablet-cell-initialization"]

            if self.config.proxy_port is not None:
                self.yt_proxy_port = self.config.proxy_port
                args += ["--proxy-port", str(self.config.proxy_port)]

            enable_debug_log = self.config.enable_debug_log or yatest.common.get_param("yt_enable_debug_logging")
            # Temporary hack: we want to analyse problems mr_apps tests.
            if "quality/mr_apps/" in yatest.common.work_path():
                enable_debug_log = True;
            if enable_debug_log:
                args += ["--enable-debug-logging"]
            if self.tmpfs_path:
                args += ["--tmpfs-path", self.tmpfs_path]
            if self.config.node_config:
                args += ["--node-config", self.config.node_config]
            if self.config.node_count:
                args += ["--node-count", str(self.config.node_count)]
            if self.config.scheduler_config:
                args += ["--scheduler-config", self.config.scheduler_config]
            if self.config.proxy_config:
                args += ["--proxy-config", self.config.proxy_config]

            local_cypress_dir = self.config.local_cypress_dir or yatest.common.get_param("yt_local_cypress_dir")
            if local_cypress_dir:
                args += ["--local-cypress-dir", local_cypress_dir]

            cmd = self.yt_local_path + list(args)
            self._log(" ".join([os.path.basename(cmd[0])] + cmd[1:]))

            special_file = os.path.join(self.yt_work_dir, self.yt_id, "started")

            if os.path.lexists(special_file):
                # It may be start after suspend
                os.remove(special_file)

            yt_daemon = devtools.swag.daemon.run_daemon(
                cmd,
                env=self.env,
                cwd=self.yt_work_dir,
                stdout=self.yt_local_out,
                stderr=self.yt_local_err,
            )
            # Wait until special file will appear. It means that yt_local had been started. See YT-4425 for details.
            MAX_WAIT_TIME, SLEEP_TIME = 60, 0.1 # in seconds
            NUM_TRIES = int(MAX_WAIT_TIME / SLEEP_TIME)
            for i in xrange(NUM_TRIES):
                if os.path.lexists(special_file):
                    break
                if not yt_daemon.is_alive():
                    self._log("yt_local process failed.")
                    break
                time.sleep(SLEEP_TIME)
            else:
                self._log("Can't find 'started' file for %d seconds.", MAX_WAIT_TIME)
                yt_daemon.stop()
                return False
            if self.config.proxy_port is None:
                info_yson_file = os.path.join(self.yt_work_dir, self.yt_id, "info.yson")
                import yt.yson
                with open(info_yson_file) as f:
                    info = yt.yson.load(f)
                self.yt_proxy_port = int(info["proxy"]["address"].split(":")[1])
        except Exception, e:
            self._log("Failed to start local YT:\n%s", str(e))
            for pid in self.get_pids():
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass
            return False
        self.yt_wrapper.config["proxy"]["url"] = self.get_server()
        self.yt_wrapper.config["proxy"]["enable_proxy_discovery"] = False
        self.yt_client.config["proxy"]["url"] = self.get_server()
        self.yt_client.config["proxy"]["enable_proxy_discovery"] = False
        self.env["YT_PROXY"] = self.get_server()
        self._log("Local YT was started with id=%s", self.yt_id)
        return True

    def get_pids(self):
        pids_file = os.path.join(self.yt_work_dir, self.yt_id, "pids.txt")
        pids = []
        if os.path.exists(pids_file):
            with open(pids_file) as f:
                for line in f.readlines():
                    try:
                        pids.append(int(line))
                    except ValueError:
                        pass
        return pids

    def get_yt_wrapper(self):
        return self.yt_wrapper

    def get_yt_client(self):
        return self.yt_client

    def get_server(self):
        return "localhost:%d" % self.yt_proxy_port

    def get_env(self):
        return self.env

    # Dear user! Please, look at run_mapreduce_yt() method!
    # Do you really want to use get_mapreduce_yt() directly?
    # If yes, please don't forget to use yatest.common.python_path() and to set environment
    # (right, like in run_mapreduce_yt() method).
    def get_mapreduce_yt(self):
        return self.mapreduce_yt_path

    def run_mapreduce_yt(self, cmd, env=None, *args, **kwargs):
        if not env:
            env = {}

        env.update(self.env)
        cmd = self.mapreduce_yt_path + cmd

        return yatest.common.execute(cmd, env=env, *args, **kwargs)

    def get_yt_cli_binary(self):
        return os.path.join(self.yt_bins_path, 'yt2')

    @_timing
    def start_local_yt(self):
        max_retries = int(yatest.common.get_param(
            "yt_start_max_tries",
            default=os.environ.get("YT_STUFF_MAX_START_RETRIES", _YT_MAX_START_RETRIES)
        ))
        for i in xrange(max_retries):
            self._log("Start local YT, attempt %d.", i)
            if self._start_local_yt():
                self.is_running = True
                break
            else:
                dirname = os.path.join(self.yt_work_dir, self.yt_id)
                if os.path.exists(dirname):
                    dir_i = i
                    while True:
                        failed_dirname = "%s_FAILED_try_%d" % (dirname, dir_i)
                        if os.path.exists(failed_dirname):
                            dir_i += 1
                        else:
                            os.rename(dirname, failed_dirname)
                            break
                MAX_WAIT_TIME = 60
                FAIL_PENALTY = 5
                time_to_sleep = min(i * FAIL_PENALTY, MAX_WAIT_TIME)
                time.sleep(time_to_sleep)
        else:
            self._save_logs(save_yt_all=True)
            raise Exception("Can't start local YT with id %s for %d attempts." % (self.yt_id, max_retries))


    def suspend_local_yt(self):
        try:
            cmd = self.yt_local_path + [
                "stop", os.path.join(self.yt_work_dir, self.yt_id),
            ]
            self._log(" ".join([os.path.basename(cmd[0])] + cmd[1:]))
            yatest.common.process.execute(
                cmd,
                env=self.env,
                cwd=self.yt_work_dir,
                stdout=self.yt_local_out,
                stderr=self.yt_local_err,
            )
            self.is_running = False
        except Exception, e:
            self._log("Errors while stopping local YT:\n%s", str(e))
            self._save_logs(save_yt_all=True)
            raise

    @_timing
    def stop_local_yt(self):
        if self.is_running:
            self.suspend_local_yt()
            with open(os.path.join(self.yt_work_dir, self.yt_id, "locked_file")) as lock_file:
                fcntl.flock(lock_file, fcntl.LOCK_EX)
                fcntl.flock(lock_file, fcntl.LOCK_UN)

        self._save_logs(save_yt_all=self.config.save_all_logs or yatest.common.get_param("yt_save_all_data"))

        if not self.config.keep_yt_work_dir:
            shutil.rmtree(self.yt_work_dir, ignore_errors=True)

    @_timing
    def _save_logs(self, save_yt_all=None):
        output_path = yatest.common.output_path()

        self._log("Logs saved in %s", output_path)

        common_interface_log = yatest.common.work_path("mr-client.log")
        if os.path.exists(common_interface_log):
            p = os.path.join(output_path, "mr-client.log")
            shutil.copyfile(common_interface_log, p)

        def _ignore(path, names):
            IGNORE_DIRS_ALWAYS = ["ui", "pipes"]
            IGNORE_DIRS = ["chunk_store", "chunk_cache", "changelogs", "snapshots"]
            ignored = set()
            for name in names:
                full_path = os.path.join(path, name)
                if os.path.islink(full_path):
                    ignored.add(name)
                elif os.path.isdir(full_path):
                    should_ignore = False
                    should_ignore |= name in IGNORE_DIRS_ALWAYS
                    should_ignore |= not save_yt_all and name in IGNORE_DIRS
                    if should_ignore:
                        ignored.add(name)
            return ignored

        yt_output_dir = os.path.join(output_path, "yt_logs_%s" % self.yt_id)
        shutil.copytree(src=self.yt_work_dir, dst=yt_output_dir, ignore=_ignore)
        os.system("chmod -R 0775 " + yt_output_dir)

        # Pack huge files, because ya.test cuts them.
        FILE_SIZE_LIMIT = 2 * 1024 * 1024 # See https://a.yandex-team.ru/arc/trunk/arcadia/devtools/ya/test/node/run_test.py?rev=2316309#L30
        for root, dirs, files in os.walk(yt_output_dir):
            for file in files:
                file_path = os.path.join(root, file)
                if os.path.getsize(file_path) >= FILE_SIZE_LIMIT:
                    archive_path = "%s.tgz" % file_path
                    self._pack_tar(archive_path=archive_path, file_path=file_path)
                    os.remove(file_path)

        cores_dir = os.path.join(yt_output_dir, "cores")
        if not os.path.isdir(cores_dir):
            os.mkdir(cores_dir)

        for pid in self.get_pids():
            core_file = yatest.common.cores.recover_core_dump_file(
                os.path.join(self.yt_bins_path, 'ytserver'),
                self.yt_work_dir,
                pid
            )
            if core_file:
                shutil.copy(core_file, cores_dir)


@pytest.fixture(scope="module")
def yt_stuff(request):
    try:
        yt_config = request.getfuncargvalue("yt_config")
    except Exception:
        yt_config = None
    yt = YtStuff(yt_config)
    yt.start_local_yt()
    request.addfinalizer(yt.stop_local_yt)
    return yt
