import subprocess
import sys
import pytest

import yatest.common
from mapreduce.yt.python.yt_stuff import yt_stuff, YtConfig

BINARY_PATH = yatest.common.binary_path("mapreduce/yt/tests/rpc_proxy/test-rpc-proxy")

TESTS_LIST = sorted(subprocess.check_output([BINARY_PATH, "--list-verbose"]).split())

@pytest.fixture
def yt_config(request):
    return YtConfig(yt_version="19_2")

@pytest.mark.parametrize("test_name", TESTS_LIST)
def test(yt_config, yt_stuff, test_name):
    stderr_file_name = yatest.common.output_path(test_name + '.stderr')

    try:
        with open(stderr_file_name, 'w') as stderr_file:
            yatest.common.execute(
                [BINARY_PATH,  test_name],
                env={"YT_RPC_PROXY": yt_stuff.get_rpc_proxy()}, stderr=stderr_file)
    except:
        with open(stderr_file_name) as inf:
            sys.stderr.write(inf.read())
        raise
