# Stubs for pyspark.sql.utils (Python 3)
#
# NOTE: This dynamically typed stub was automatically generated by stubgen.

from typing import Any, Optional

class CapturedException(Exception):
    desc: Any = ...
    stackTrace: Any = ...
    cause: Any = ...
    def __init__(self, desc: Any, stackTrace: Any, cause: Optional[Any] = ...) -> None: ...

class AnalysisException(CapturedException): ...
class ParseException(CapturedException): ...
class IllegalArgumentException(CapturedException): ...
class StreamingQueryException(CapturedException): ...
class QueryExecutionException(CapturedException): ...
class UnknownException(CapturedException): ...

def convert_exception(e: Any): ...
def capture_sql_exception(f: Any): ...
def install_exception_handler() -> None: ...
def toJArray(gateway: Any, jtype: Any, arr: Any): ...
def require_minimum_pandas_version() -> None: ...
def require_minimum_pyarrow_version() -> None: ...
def require_test_compiled() -> None: ...

class ForeachBatchFunction:
    sql_ctx: Any = ...
    func: Any = ...
    def __init__(self, sql_ctx: Any, func: Any) -> None: ...
    error: Any = ...
    def call(self, jdf: Any, batch_id: Any) -> None: ...
    class Java:
        implements: Any = ...

def to_str(value: Any): ...
