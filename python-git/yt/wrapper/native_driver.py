from config import get_config, get_option, set_option
from common import require, generate_int64
from errors import YtResponseError, YtError
from string_iter_io import StringIterIO
from response_stream import ResponseStream

import yt.logger as logger
import yt.yson as yson
try:
    import yt_driver_bindings
    from yt_driver_bindings import Request, Driver, BufferedStream
except ImportError:
    Driver = None

from cStringIO import StringIO

def read_config(path):
    driver_config = yson.load(open(path, "r"))
    if not hasattr(read_config, "logging_and_tracing_initialized"):
        if "logging" in driver_config:
            yt_driver_bindings.configure_logging(driver_config["logging"])
        if "tracing" in driver_config:
            yt_driver_bindings.configure_tracing(driver_config["tracing"])
        setattr(read_config, "logging_and_tracing_initialized", True)
    return driver_config["driver"]

def get_driver_instance(client):
    config = get_config(client)
    if config["driver_config"] is not None:
        driver_config = config["driver_config"]
    elif config["driver_config_path"] is not None:
        driver_config = read_config(config["driver_config_path"])
    else:
        raise YtError("Driver config is not specified")

    driver = get_option("_driver", client=client)
    if driver is None:
        if Driver is None:
            raise YtError("Driver class not found, install yt driver bindings.")
        set_option("_driver", Driver(driver_config), client=client)
        driver = get_option("_driver", client=client)
    return driver

def convert_to_stream(data):
    if data is None:
        return data
    elif hasattr(data, "read"):
        return data
    elif isinstance(data, str):
        return StringIO(data)
    elif isinstance(data, list):
        return StringIterIO(iter(data))
    else:
        return StringIterIO(data)

def make_request(command_name, params,
                 data=None,
                 return_content=True,
                 client=None):
    driver = get_driver_instance(client)

    require(command_name in driver.get_command_descriptors(),
            lambda: YtError("Command {0} is not supported".format(command_name)))

    description = driver.get_command_descriptor(command_name)

    input_stream = convert_to_stream(data)

    output_stream = None
    if description.output_type() != "Null":
        if "output_format" not in params and description.output_type() != "Binary":
            raise YtError("Inner error: output format is not specified for native driver command '{0}'".format(command_name))
        if return_content:
            output_stream = StringIO()
        else:
            output_stream = BufferedStream(size=get_config(client)["read_buffer_size"])

    request_id = generate_int64(get_option("_random_generator", client))

    logger.debug("Executing command %s with parameters %s and id %s", command_name, repr(params), hex(request_id)[2:])

    request = Request(
        command_name=command_name,
        parameters=params,
        input_stream=input_stream,
        output_stream=output_stream,
        user=get_config(client)["driver_user_name"])

    if get_config(client)["enable_passing_request_id_to_driver"]:
        request.id = request_id

    response = driver.execute(request)

    if return_content:
        response.wait()
        if not response.is_ok():
            raise YtResponseError(response.error())
        if output_stream is not None:
            return output_stream.getvalue()
    else:
        def process_error(request):
            if not response.is_ok():
                raise YtResponseError(response.error())

        if response.is_set() and not response.is_ok():
            raise YtResponseError(response.error())

        return ResponseStream(
            lambda: response,
            yt_driver_bindings.chunk_iter(output_stream, response, get_config(client)["read_buffer_size"]),
            lambda: None,
            process_error,
            lambda: response.response_parameters())
