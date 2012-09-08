import config
import logger
from common import require, YtError
from http import make_request
from tree_commands import get_attribute, exists, list
from file_commands import download_file

import os
from time import sleep

OPERATIONS_PATH = "//sys/operations"

class OperationState(object):
    def __init__(self, name):
        self.name = name

    def is_final(self):
        return self.name in ["aborted", "completed", "failed"]

    def is_failed(self):
        return self.name in ["aborted", "failed"]

    def is_running(self):
        return self.name == "running"

    def __eq__(self, other):
        return self.name == other

    def __repr__(self):
        return self.name

    def __str__(self):
        return self.name

def get_operation_state(operation):
    operation_path = os.path.join(OPERATIONS_PATH, operation)
    require(exists(operation_path),
            YtError("Operation %s doesn't exist" % operation))
    return OperationState(get_attribute(operation_path, "state"))

def get_operation_progress(operation):
    operation_path = os.path.join(OPERATIONS_PATH, operation)
    return get_attribute(operation_path, "progress/jobs")

def get_operation_stderr(operation):
    operation_path = os.path.join(OPERATIONS_PATH, operation)
    jobs = list(operation_path + "/jobs")
    stderr_paths = ("%s/jobs/%s/stderr" % (operation_path, job) for job in jobs)
    return "\n\n".join("".join(download_file(path))
                       for path in stderr_paths if exists(path))

def get_operation_result(operation):
    operation_path = os.path.join(OPERATIONS_PATH, operation)
    return get_attribute(operation_path, "result", check_errors=False)

def get_jobs_errors(operation):
    operation_path = os.path.join(OPERATIONS_PATH, operation)
    jobs = list(operation_path + "/jobs")
    jobs_paths = ("%s/jobs/%s" % (operation_path, job) for job in jobs)
    return "\n\n".join(repr(get_attribute(job, "error"))
                       for job in jobs_paths
                       if "error" in list(job + "/@"))

def abort_operation(operation):
    if not get_operation_state(operation).is_final():
        make_request("POST", "abort_op", {"operation_id": operation})

def wait_operation(operation, timeout=None, print_progress=True):
    if timeout is None: timeout = config.WAIT_TIMEOUT
    try:
        progress = None
        while True:
            state = get_operation_state(operation)
            if state.is_final():
                # TODO(ignat): Make some common logger
                return state
            if state.is_running() and print_progress:
                new_progress = get_operation_progress(operation)
                if new_progress != progress:
                    progress = new_progress
                    logger.error(
                        "jobs of operation %s: %s",
                        operation,
                        "\t".join(["=".join(map(str, [k, v])) for k, v in progress.iteritems()]))
            sleep(timeout)
    except KeyboardInterrupt:
        if config.KEYBOARD_ABORT:
            while True:
                try:
                    if get_operation_state(operation).is_final():
                        break
                    abort_operation(operation)
                    sleep(timeout)
                except KeyboardInterrupt:
                    pass
        raise
    except:
        raise

""" Waiting operation strategies """
class WaitStrategy(object):
    def __init__(self, files_to_delete=None, check_result=True, print_progress=True):
        self.check_result = check_result
        self.print_progress = print_progress

    def process_operation(self, type, operation, finalization=None):
        self.finalization = finalization if finalization is not None else lambda: None
        state = wait_operation(operation, print_progress=self.print_progress)
        if self.check_result and state.is_failed():
            operation_result = get_operation_result(operation)
            jobs_errors = get_jobs_errors(operation)
            stderr = get_operation_stderr(operation)
            # TODO: remove finalization when transactions would be buultin
            self.finalization()
            raise YtError(
                "Operation {0} failed!\n"
                "Operation result: {1}\n"
                "Job results: {2}\n"
                "Stderr: {3}\n".format(
                    operation,
                    operation_result,
                    jobs_errors,
                    stderr))
        self.finalization()
        if self.print_progress:
            logger.info("operation %s completed", operation)
        #return operation_result, jobs_errors, stderr

class AsyncStrategy(object):
    # TODO(improve this strategy)
    def __init__(self):
        self.operations = []

    def process_operation(self, type, operation, files_to_remove):
        self.operations.append(operation)

    def get_last_operation(self):
        return self.operations[-1]

config.DEFAULT_STRATEGY = WaitStrategy()
