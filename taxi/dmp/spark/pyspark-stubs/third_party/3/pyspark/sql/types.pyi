# Stubs for pyspark.sql.types (Python 3.5)
#

from typing import overload
from typing import Any, Callable, Dict, Iterator, List, Optional, Union, Tuple, TypeVar
import datetime

T = TypeVar('T')
U = TypeVar('U')

class DataType:
    def __hash__(self) -> int: ...
    def __eq__(self, other: Any) -> bool: ...
    def __ne__(self, other: Any) -> bool: ...
    @classmethod
    def typeName(cls) -> str: ...
    def simpleString(self) -> str: ...
    def jsonValue(self) -> Union[str, Dict[str, Any]]: ...
    def json(self) -> str: ...
    def needConversion(self) -> bool: ...
    def toInternal(self, obj: Any) -> Any: ...
    def fromInternal(self, obj: Any) -> Any: ...

class DataTypeSingleton(type):
    def __call__(cls): ...

class NullType(DataType):
    __metaclass__ = ...  # type: type

class AtomicType(DataType): ...
class NumericType(AtomicType): ...

class IntegralType(NumericType):
    __metaclass__ = ...  # type: type

class FractionalType(NumericType): ...

class StringType(AtomicType):
    __metaclass__ = ...  # type: type

class BinaryType(AtomicType):
    __metaclass__ = ...  # type: type

class BooleanType(AtomicType):
    __metaclass__ = ...  # type: type

class DateType(AtomicType):
    __metaclass__ = ...  # type: type
    EPOCH_ORDINAL = ...  # type: int
    def needConversion(self) -> bool: ...
    def toInternal(self, d: datetime.date) -> int: ...
    def fromInternal(self, v: int) -> datetime.date: ...

class TimestampType(AtomicType):
    __metaclass__ = ...  # type: type
    def needConversion(self) -> bool: ...
    def toInternal(self, dt: datetime.datetime) -> int: ...
    def fromInternal(self, ts: int) -> datetime.datetime: ...

class DecimalType(FractionalType):
    precision = ...  # type: int
    scale = ...  # type: int
    hasPrecisionInfo = ...  # type: bool
    def __init__(self, precision: int = ..., scale: int = ...) -> None: ...
    def simpleString(self) -> str: ...
    def jsonValue(self) -> str: ...

class DoubleType(FractionalType):
    __metaclass__ = ...  # type: type

class FloatType(FractionalType):
    __metaclass__ = ...  # type: type

class ByteType(IntegralType):
    def simpleString(self) -> str: ...

class IntegerType(IntegralType):
    def simpleString(self) -> str: ...

class LongType(IntegralType):
    def simpleString(self) -> str: ...

class ShortType(IntegralType):
    def simpleString(self) -> str: ...

class ArrayType(DataType):
    elementType = ...  # type: DataType
    containsNull = ...  # type: bool
    def __init__(self, elementType=DataType, containsNull: bool = ...) -> None: ...
    def simpleString(self): ...
    def jsonValue(self) -> Dict[str, Any]: ...
    @classmethod
    def fromJson(cls, json: Dict[str, Any]) -> 'ArrayType': ...
    def needConversion(self) -> bool: ...
    def toInternal(self, obj: List[Optional[T]]) -> List[Optional[T]]: ...
    def fromInternal(self, obj: List[Optional[T]]) -> List[Optional[T]]: ...

class MapType(DataType):
    keyType = ...  # type: DataType
    valueType = ...  # type: DataType
    valueContainsNull = ...  # type: bool
    def __init__(self, keyType: DataType, valueType: DataType, valueContainsNull: bool = ...) -> None: ...
    def simpleString(self) -> str: ...
    def jsonValue(self) -> Dict[str, Any]: ...
    @classmethod
    def fromJson(cls, json: Dict[str, Any]) -> MapType: ...
    def needConversion(self) -> bool: ...
    def toInternal(self, obj: Dict[T, Optional[U]]) -> Dict[T, Optional[U]]: ...
    def fromInternal(self, obj: Dict[T, Optional[U]]) -> Dict[T, Optional[U]]: ...

class StructField(DataType):
    name = ...  # type: str
    dataType = ...  # type: DataType
    nullable = ...  # type: bool
    metadata = ...  # type: Dict[str, Any]
    def __init__(self, name: str, dataType: DataType, nullable: bool = ..., metadata: Optional[Dict[str, Any]] = ...) -> None: ...
    def simpleString(self) -> str: ...
    def jsonValue(self) -> Dict[str, Any]: ...
    @classmethod
    def fromJson(cls, json: Dict[str, Any]) -> 'StructField': ...
    def needConversion(self) -> bool: ...
    def toInternal(self, obj: T) -> T: ...
    def fromInternal(self, obj: T) -> T: ...

class StructType(DataType):
    fields = ...  # type: List[StructField]
    names = ...  # type: List[str]
    def __init__(self, fields: Optional[Any] = ...) -> None: ...
    def add(self, field, data_type: Optional[Any] = ..., nullable: bool = ..., metadata: Optional[Any] = ...): ...
    def __iter__(self) -> Iterator[StructField]: ...
    def __len__(self) -> int: ...
    def __getitem__(self, key) -> StructField: ...
    def simpleString(self) -> str: ...
    def jsonValue(self) -> Dict[str, Any]: ...
    @classmethod
    def fromJson(cls, json) -> 'StructType': ...
    def fieldNames(self) -> List[str]: ...
    def needConversion(self) -> bool: ...
    def toInternal(self, obj: Tuple) -> Tuple: ...
    def fromInternal(self, obj: Tuple) -> Row: ...

class UserDefinedType(DataType):
    @classmethod
    def typeName(cls) -> str: ...
    @classmethod
    def sqlType(cls) -> DataType: ...
    @classmethod
    def module(cls) -> str: ...
    @classmethod
    def scalaUDT(cls) -> str: ...
    def needConversion(self) -> bool: ...
    def toInternal(self, obj: Any) -> Any: ...
    def fromInternal(self, obj: Any) -> Any: ...
    def serialize(self, obj: Any) -> Any: ...
    def deserialize(self, datum: Any) -> Any: ...
    def simpleString(self) -> str: ...
    def json(self) -> str: ...
    def jsonValue(self) -> Dict[str, Any]: ...
    @classmethod
    def fromJson(cls, json: Dict[str, Any]) -> UserDefinedType: ...
    def __eq__(self, other: Any) -> bool: ...

class Row(tuple):
    @overload
    def __new__(self, *args: str) -> 'Row': ...
    @overload
    def __new__(self, **kwargs: Any) -> 'Row': ...
    @overload
    def __init__(self, *args: str) -> None: ...
    @overload
    def __init__(self, **kwargs: Any) -> None: ...
    def asDict(self, recursive: bool = ...) -> Dict[str, Any]: ...
    def __contains__(self, item: Any) -> bool: ...
    def __call__(self, *args: Any) -> Row: ...
    def __getitem__(self, item: Any) -> Any: ...
    def __getattr__(self, item: str) -> Any: ...
    def __setattr__(self, key: Any, value: Any) -> None: ...
    def __reduce__(self) -> Tuple[Callable[[List[str], List[Any]], Row], Tuple[List[str], Tuple]]: ...

class DateConverter:
    def can_convert(self, obj: Any) -> bool: ...
    def convert(self, obj, gateway_client) -> Any: ...

class DatetimeConverter:
    def can_convert(self, obj) -> bool: ...
    def convert(self, obj, gateway_client) -> Any: ...
