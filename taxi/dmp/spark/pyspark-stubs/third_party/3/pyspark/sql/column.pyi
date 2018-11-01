# Stubs for pyspark.sql.column (Python 3.5)
#

from typing import overload
from typing import Any, Union
from pyspark.sql._typing import Literal, DecimalLiteral, DateTimeLiteral
from pyspark.sql.types import *
from pyspark.sql.window import WindowSpec

class Column:
    def __init__(self, jc) -> None: ...
    def __neg__(self) -> Column: ...
    def __add__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...
    def __sub__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...
    def __mul__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...
    def __div__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...
    def __truediv__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...
    def __mod__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...
    def __radd__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...  # type: ignore
    def __rsub__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...
    def __rmul__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...  # type: ignore
    def __rdiv__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...
    def __rtruediv__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...
    def __rmod__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...  # type: ignore
    def __pow__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...
    def __rpow__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...
    def __eq__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...  # type: ignore
    def __ne__(self, other: Any) -> Column: ...  # type: ignore
    def __lt__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...  # type: ignore
    def __le__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...  # type: ignore
    def __ge__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...  # type: ignore
    def __gt__(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...  # type: ignore
    def eqNullSafe(self, other: Union[Column, Literal, DecimalLiteral]) -> Column: ...
    def __and__(self, other: Column) -> Column: ...
    def __or__(self, other: Column) -> Column: ...
    def __invert__(self) -> Column: ...
    def __rand__(self, other: Column) -> Column: ...
    def __ror__(self, other: Column) -> Column: ...
    def __contains__(self, other: Any) -> Column: ...
    def __getitem__(self, other: Any) -> Column: ...
    def bitwiseOR(self, other: Union[Column, int]) -> Column: ...
    def bitwiseAND(self, other: Union[Column, int]) -> Column: ...
    def bitwiseXOR(self, other: Union[Column, int]) -> Column: ...
    def getItem(self, key: Any) -> Column: ...
    def getField(self, name: Any) -> Column: ...
    def __getattr__(self, item: Any) -> Column: ...
    def __iter__(self) -> None: ...
    def rlike(self, item: str) -> Column: ...
    def like(self, item: str) -> Column: ...
    def startswith(self, item: Union[str, Column]) -> Column: ...
    def endswith(self, item: Union[str, Column]) -> Column: ...
    @overload
    def substr(self, startPos: int, length: int) -> Column: ...
    @overload
    def substr(self, startPos: Column, length: Column) -> Column: ...
    def __getslice__(self, startPos: int, length: int) -> Column: ...
    def isin(self, *cols: Any) -> Column: ...
    def asc(self) -> Column: ...
    def desc(self) -> Column: ...
    def isNull(self) -> Column: ...
    def isNotNull(self) -> Column: ...
    def alias(self, *alias: str, **kwargs: Any) -> Column: ...
    def name(self, *alias: str) -> Column: ...
    def cast(self, dataType: Union[DataType, str]) -> Column: ...
    def astype(self, dataType: Union[DataType, str]) -> Column: ...
    def between(self, lowerBound, upperBound) -> Column: ...
    def when(self, condition: Column, value: Any) -> Column: ...
    def otherwise(self, value: Any) -> Column: ...
    def over(self, window: WindowSpec) -> Column: ...
    def __nonzero__(self) -> None: ...
    def __bool__(self) -> None: ...
