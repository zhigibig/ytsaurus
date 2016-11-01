from .common import EMPTY_GENERATOR, YtError, get_binary_std_stream

import inspect
import os
import sys
import types

class YtStandardStreamAccessError(YtError):
    pass

class StreamWrapper(object):
    ALLOWED_ATTRIBUTES = set(["fileno", "isatty", "tell", "encoding", "name", "mode"])

    def __init__(self, original_stream):
        self.original_stream = original_stream

    def __getattr__(self, attr):
        if attr in self.ALLOWED_ATTRIBUTES:
            return self.original_stream.__getattribute__(attr)
        raise YtStandardStreamAccessError("Stdin, stdout are inaccessible for Python operations"
                                          " without raw_io attribute")

class WrappedStreams(object):
    def __init__(self, wrap_stdin=True, wrap_stdout=True):
        self.wrap_stdin = wrap_stdin
        self.wrap_stdout = wrap_stdout

    def __enter__(self):
        self.stdin = sys.stdin
        self.stdout = sys.stdout
        if self.wrap_stdin:
            sys.stdin = StreamWrapper(self.stdin)
        if self.wrap_stdout:
            sys.stdout = StreamWrapper(self.stdout)
        return self

    def __exit__(self, exc_type, exc_value, exc_traceback):
       sys.stdin = self.stdin
       sys.stdout = self.stdout

    def get_original_stdin(self):
        return self.stdin

    def get_original_stdout(self):
        return self.stdout

class Context(object):
    def __init__(self):
        self.table_index = None
        self.row_index = None
        self.range_index = None

def convert_callable_to_generator(func):
    def generator(*args):
        result = func(*args)
        if isinstance(result, types.GeneratorType):
            return result
        elif result is not None:
            raise YtError('Non-yielding operation function should return generator or None.'
                          ' Did you mean "yield" instead of "return"?')
        return EMPTY_GENERATOR

    return generator

def extract_operation_methods(operation, context):
    if hasattr(operation, "start") and inspect.ismethod(operation.start):
        start = convert_callable_to_generator(operation.start)
    else:
        start = lambda: EMPTY_GENERATOR

    if hasattr(operation, "finish") and inspect.ismethod(operation.finish):
        finish = convert_callable_to_generator(operation.finish)
    else:
        finish = lambda: EMPTY_GENERATOR


    if context is not None:
        operation_func = lambda *args: operation(*args, context=context)
    else:
        operation_func = operation

    return start, convert_callable_to_generator(operation_func), finish

def extract_context(rows):
    context = Context()

    def generate_rows():
        for row in rows:
            context.table_index = getattr(rows, "table_index", None)
            context.row_index = getattr(rows, "row_index", None)
            context.range_index = getattr(rows, "range_index", None)
            yield row

    return generate_rows(), context


def check_job_environment_variables():
    for name in ["YT_OPERATION_ID", "YT_JOB_ID", "YT_JOB_INDEX"]:
        if name not in os.environ:
            sys.stderr.write("Warning! {0} is not set. If this job is not run "
                             "manually for testing purposes then this is a bug.\n".format(name))

def process_rows(operation_dump_filename, config_dump_filename, start_time):
    from itertools import chain, groupby, starmap
    try:
        from itertools import imap
    except ImportError:  # Python 3
        imap = map

    import time

    import yt.yson
    import yt.wrapper
    from yt.wrapper.format import YsonFormat, extract_key
    from yt.wrapper.pickling import Unpickler
    from yt.wrapper.mappings import FrozenDict

    def process_frozen_dict(rows):
        for row in rows:
            if type(row) == FrozenDict:
                yield row.as_dict()
            else:
                yield row

    yt.wrapper.config.config = \
        Unpickler(yt.wrapper.config.DEFAULT_PICKLING_FRAMEWORK).load(open(config_dump_filename, "rb"))

    yt.wrapper.py_runner_helpers.check_job_environment_variables()

    unpickler_name = yt.wrapper.config.config["pickling"]["framework"]
    unpickler = Unpickler(unpickler_name)
    if unpickler_name == "dill" and yt.wrapper.config.config["pickling"]["load_additional_dill_types"]:
        unpickler.load_types()

    operation, attributes, operation_type, input_format, output_format, group_by_keys, python_version = \
        unpickler.load(open(operation_dump_filename, "rb"))

    if yt.wrapper.config["pickling"]["enable_job_statistics"]:
        try:
            import yt.wrapper.user_statistics
            if start_time is not None:
                yt.wrapper.user_statistics.write_statistics({"python_job_preparation_time": int((time.time() - start_time) * 1000)})
        except ImportError:
            pass

    if yt.wrapper.config["pickling"]["check_python_version"] and yt.wrapper.common.get_python_version() != python_version:
        sys.stderr.write("Python version on cluster differs from local python version")
        sys.exit(1)

    if attributes.get("is_raw_io", False):
        operation()
        return

    raw = attributes.get("is_raw", False)

    if isinstance(input_format, YsonFormat) and yt.yson.TYPE != "BINARY":
        sys.stderr.write("YSON bindings not found. To resolve the problem "
                         "try to use JsonFormat format or install yandex-yt-python-yson package.")
        sys.exit(1)

    rows = input_format.load_rows(get_binary_std_stream(sys.stdin), raw=raw)

    context = None
    if attributes.get("with_context", False):
        rows, context = extract_context(rows)
    start, run, finish = yt.wrapper.py_runner_helpers.extract_operation_methods(operation, context)
    wrap_stdin = wrap_stdout = yt.wrapper.config["pickling"]["safe_stream_mode"]
    with yt.wrapper.py_runner_helpers.WrappedStreams(wrap_stdin, wrap_stdout) as streams:
        if attributes.get("is_aggregator", False):
            result = run(rows)
        else:
            if operation_type == "mapper" or raw:
                result = chain(
                    start(),
                    chain.from_iterable(imap(run, rows)),
                    finish())
            else:
                if attributes.get("is_reduce_aggregator"):
                    result = run(groupby(rows, lambda row: extract_key(row, group_by_keys)))
                else:
                    result = chain(
                        start(),
                        chain.from_iterable(
                            starmap(run,
                                groupby(rows, lambda row: extract_key(row, group_by_keys)))),
                        finish())

        result = process_frozen_dict(result)
        output_format.dump_rows(result, get_binary_std_stream(streams.get_original_stdout()), raw=raw)

    # Read out all input
    for row in rows:
        pass

