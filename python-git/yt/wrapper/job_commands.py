from .config import get_config
from .driver import make_request, make_formatted_request, _create_http_client_from_rpc
from .common import set_param
from .ypath import YPath

def list_jobs(operation_id,
              job_type=None, job_state=None, address=None, job_competition_id=None,
              sort_field=None, sort_order=None,
              limit=None, offset=None, with_stderr=None, with_spec=None, with_fail_context=None,
              include_cypress=None, include_runtime=None, include_archive=None,
              data_source=None, format=None, client=None):
    """List jobs of operation."""
    params = {"operation_id": operation_id}
    set_param(params, "job_type", job_type)
    set_param(params, "job_state", job_state)
    set_param(params, "address", address)
    set_param(params, "job_competition_id", job_competition_id)
    set_param(params, "sort_field", sort_field)
    set_param(params, "sort_order", sort_order)
    set_param(params, "limit", limit)
    set_param(params, "offset", offset)
    set_param(params, "with_stderr", with_stderr)
    set_param(params, "with_spec", with_spec)
    set_param(params, "with_fail_context", with_fail_context)
    set_param(params, "include_cypress", include_cypress)
    set_param(params, "include_runtime", include_runtime)
    set_param(params, "include_archive", include_archive)
    set_param(params, "data_source", data_source)

    timeout = get_config(client)["operation_info_commands_timeout"]

    return make_formatted_request(
        "list_jobs",
        params=params,
        format=format,
        client=client,
        timeout=timeout)

def get_job(operation_id, job_id, format=None, client=None):
    """Get job of operation.

    :param str operation_id: operation id.
    :param str job_id: job id.
    """
    params = {"operation_id": operation_id, "job_id": job_id}
    timeout = get_config(client)["operation_info_commands_timeout"]
    return make_formatted_request(
        "get_job",
        params=params,
        format=format,
        client=client,
        timeout=timeout)

def run_job_shell(job_id, timeout=None, command=None, client=None):
    """Runs interactive shell in the job sandbox.

    :param str job_id: job id.
    """
    from .job_shell import JobShell

    JobShell(job_id, interactive=True, timeout=timeout, client=client).run(command=command)

def get_job_stderr(operation_id, job_id, client=None):
    """Gets stderr of the specified job.

    :param str operation_id: operation id.
    :param str job_id: job id.
    """
    return make_request(
        "get_job_stderr",
        {"operation_id": operation_id, "job_id": job_id},
        return_content=False,
        client=client)

def get_job_input(job_id, client=None):
    """Get full input of the specified job.

    :param str job_id: job id.
    """
    if get_config(client)["backend"] == "rpc" and get_config(client).get("use_http_backend_for_streaming", True):
        client = _create_http_client_from_rpc(client, "get_job_input")
    return make_request(
        "get_job_input",
        params={"job_id": job_id},
        return_content=False,
        use_heavy_proxy=True,
        client=client)

def get_job_input_paths(job_id, client=None):
    """Get input paths of the specified job.

    :param str job_if: job id.
    :return: list of YPaths.
    """
    timeout = get_config(client)["operation_info_commands_timeout"]
    yson_paths = make_formatted_request(
        "get_job_input_paths",
        params={"job_id": job_id},
        format=None,
        timeout=timeout,
        client=client)
    return list(map(YPath, yson_paths))

def abort_job(job_id, interrupt_timeout=None, client=None):
    """Interrupts running job with preserved result.

    :param str job_id: job id.
    :param int interrupt_timeout: wait for interrupt before abort (in ms).
    """
    params = {"job_id": job_id}
    set_param(params, "interrupt_timeout", interrupt_timeout)
    return make_request("abort_job", params, client=client)

def dump_job_context(job_id, path, client=None):
    """Dumps job input context to specified path."""
    return make_request("dump_job_context", {"job_id": job_id, "path": path}, client=client)

def get_job_fail_context(operation_id, job_id, client=None):
    """Get fail context of the specified job.

    :param str operation_id: operation id.
    :param str job_id: job id.
    """
    if get_config(client)["backend"] == "rpc" and get_config(client).get("use_http_backend_for_streaming", True):
        client = _create_http_client_from_rpc(client, "get_job_input")
    return make_request(
        "get_job_fail_context",
        params={"operation_id": operation_id, "job_id": job_id},
        return_content=False,
        use_heavy_proxy=True,
        client=client)
