# Stubs for pyspark.ml.param (Python 3.5)
#
# NOTE: This dynamically typed stub was automatically generated by stubgen.

from typing import overload
from typing import Any, Callable, Dict, List, Optional, TypeVar
from pyspark.ml.util import Identifiable
from pyspark.ml.linalg import DenseVector, Matrix

U = TypeVar('U')
ParamMap = Dict[Param, Any]

class Param:
    parent = ...  # type: str
    name = ...  # type: str
    doc = ...  # type: str
    typeConverter = ...  # type: Any
    def __init__(self, parent: Param, name: str, doc: str, typeConverter: Optional[Callable[[Any], Any]] = ...) -> None: ...
    def __hash__(self) -> int: ...
    def __eq__(self, other: Any) -> bool: ...

class TypeConverters:
    @staticmethod
    def identity(U) -> U: ...
    @staticmethod
    def toList(value: Any) -> List: ...
    @staticmethod
    def toListFloat(value: Any) -> List[float]: ...
    @staticmethod
    def toListInt(value: Any) -> List[int]: ...
    @staticmethod
    def toListString(value: Any) -> List[str]: ...
    @staticmethod
    def toVector(value: Any) -> DenseVector: ...
    @staticmethod
    def toMatrix(value: Any) -> Matrix: ...
    @staticmethod
    def toFloat(value: Any) -> float: ...
    @staticmethod
    def toInt(value: Any) -> int: ...
    @staticmethod
    def toString(value: Any) -> str: ...
    @staticmethod
    def toBoolean(value: Any) -> bool: ...

class Params(Identifiable):
    __metaclass__ = ...  # type: Any
    def __init__(self) -> None: ...
    @property
    def params(self) -> List[Param]: ...
    def explainParam(self, param: str) -> str: ...
    def explainParams(self) -> str: ...
    def getParam(self, paramName: str) -> Param: ...
    @overload
    def isSet(self, param: str) -> bool: ...
    @overload
    def isSet(self, param: Param) -> bool: ...
    @overload
    def hasDefault(self, param: str) -> bool: ...
    @overload
    def hasDefault(self, param: Param) -> bool: ...
    @overload
    def isDefined(self, param: str) -> bool: ...
    @overload
    def isDefined(self, param: Param) -> bool: ...
    def hasParam(self, paramName: str) -> Param: ...
    @overload
    def getOrDefault(self, param: str) -> Any: ...
    @overload
    def getOrDefault(self, param: Param) -> Any: ...
    def extractParamMap(self, extra: Optional[ParamMap] = ...) -> ParamMap: ...
    def copy(self, extra: Optional[ParamMap] = ...) -> Params: ...
    def set(self, param: Param, value: Any) -> None: ...
