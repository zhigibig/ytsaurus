# Stubs for pyspark.ml.linalg (Python 3)

from typing import overload
from typing import Any, Dict, Iterable, List, Optional, Tuple, Union

from pyspark.ml import linalg as newlinalg
from pyspark.sql.types import StructType, UserDefinedType

from numpy import float64, ndarray  # type: ignore

class VectorUDT(UserDefinedType):
    @classmethod
    def sqlType(cls) -> StructType: ...
    @classmethod
    def module(cls) -> str: ...
    @classmethod
    def scalaUDT(cls) -> str: ...
    def serialize(
        self, obj: Vector
    ) -> Tuple[int, Optional[int], Optional[List[int]], List[float]]: ...
    def deserialize(self, datum: Any) -> Vector: ...
    def simpleString(self) -> str: ...

class MatrixUDT(UserDefinedType):
    @classmethod
    def sqlType(cls) -> StructType: ...
    @classmethod
    def module(cls) -> str: ...
    @classmethod
    def scalaUDT(cls) -> str: ...
    def serialize(
        self, obj
    ) -> Tuple[
        int, int, int, Optional[List[int]], Optional[List[int]], List[float], bool
    ]: ...
    def deserialize(self, datum: Any) -> Matrix: ...
    def simpleString(self) -> str: ...

class Vector:
    __UDT__: VectorUDT
    def toArray(self) -> ndarray: ...

class DenseVector(Vector):
    array: ndarray
    @overload
    def __init__(self, *elements: float) -> None: ...
    @overload
    def __init__(self, __arr: bytes) -> None: ...
    @overload
    def __init__(self, __arr: Iterable[float]) -> None: ...
    @staticmethod
    def parse(s) -> DenseVector: ...
    def __reduce__(self) -> Tuple[type, bytes]: ...
    def numNonzeros(self) -> int: ...
    def norm(self, p: Union[float, str]) -> float64: ...
    def dot(self, other: Iterable[float]) -> float64: ...
    def squared_distance(self, other: Iterable[float]) -> float64: ...
    def toArray(self) -> ndarray: ...
    @property
    def values(self) -> ndarray: ...
    def __getitem__(self, item: int) -> float64: ...
    def __len__(self) -> int: ...
    def __eq__(self, other: Any) -> bool: ...
    def __ne__(self, other: Any) -> bool: ...
    def __hash__(self) -> int: ...
    def __getattr__(self, item: str) -> Any: ...
    def __neg__(self) -> DenseVector: ...
    def __add__(self, other: Union[float, Iterable[float]]) -> DenseVector: ...
    def __sub__(self, other: Union[float, Iterable[float]]) -> DenseVector: ...
    def __mul__(self, other: Union[float, Iterable[float]]) -> DenseVector: ...
    def __div__(self, other: Union[float, Iterable[float]]) -> DenseVector: ...
    def __truediv__(self, other: Union[float, Iterable[float]]) -> DenseVector: ...
    def __mod__(self, other: Union[float, Iterable[float]]) -> DenseVector: ...
    def __radd__(self, other: Union[float, Iterable[float]]) -> DenseVector: ...
    def __rsub__(self, other: Union[float, Iterable[float]]) -> DenseVector: ...
    def __rmul__(self, other: Union[float, Iterable[float]]) -> DenseVector: ...
    def __rdiv__(self, other: Union[float, Iterable[float]]) -> DenseVector: ...
    def __rtruediv__(self, other: Union[float, Iterable[float]]) -> DenseVector: ...
    def __rmod__(self, other: Union[float, Iterable[float]]) -> DenseVector: ...

class SparseVector(Vector):
    size: int
    indices: ndarray
    values: ndarray
    @overload
    def __init__(self, size: int, *args: Tuple[int, float]) -> None: ...
    @overload
    def __init__(self, size: int, __indices: bytes, __values: bytes) -> None: ...
    @overload
    def __init__(
        self, size: int, __indices: Iterable[int], __values: Iterable[float]
    ) -> None: ...
    @overload
    def __init__(self, size: int, __pairs: Iterable[Tuple[int, float]]) -> None: ...
    @overload
    def __init__(self, size: int, __map: Dict[int, float]) -> None: ...
    def numNonzeros(self) -> int: ...
    def norm(self, p: Union[float, str]) -> float64: ...
    def __reduce__(self): ...
    @staticmethod
    def parse(s: str) -> SparseVector: ...
    def dot(self, other: Iterable[float]) -> float64: ...
    def squared_distance(self, other: Iterable[float]) -> float64: ...
    def toArray(self) -> ndarray: ...
    def __len__(self) -> int: ...
    def __eq__(self, other) -> bool: ...
    def __getitem__(self, index: int) -> float64: ...
    def __ne__(self, other) -> bool: ...
    def __hash__(self) -> int: ...

class Vectors:
    @overload
    @staticmethod
    def sparse(size: int, *args: Tuple[int, float]) -> SparseVector: ...
    @overload
    @staticmethod
    def sparse(size: int, __indices: bytes, __values: bytes) -> SparseVector: ...
    @overload
    @staticmethod
    def sparse(
        size: int, __indices: Iterable[int], __values: Iterable[float]
    ) -> SparseVector: ...
    @overload
    @staticmethod
    def sparse(size: int, __pairs: Iterable[Tuple[int, float]]) -> SparseVector: ...
    @overload
    @staticmethod
    def sparse(size: int, __map: Dict[int, float]) -> SparseVector: ...
    @overload
    @staticmethod
    def dense(self, *elements: float) -> DenseVector: ...
    @overload
    @staticmethod
    def dense(self, __arr: bytes) -> DenseVector: ...
    @overload
    @staticmethod
    def dense(self, __arr: Iterable[float]) -> DenseVector: ...
    @staticmethod
    def stringify(vector: Vector) -> str: ...
    @staticmethod
    def squared_distance(v1: Vector, v2: Vector) -> float64: ...
    @staticmethod
    def norm(vector: Vector, p: Union[float, str]) -> float64: ...
    @staticmethod
    def parse(s: str) -> Vector: ...
    @staticmethod
    def zeros(size: int) -> DenseVector: ...

class Matrix:
    __UDT__: MatrixUDT
    numRows: int
    numCols: int
    isTransposed: bool
    def __init__(
        self, numRows: int, numCols: int, isTransposed: bool = ...
    ) -> None: ...
    def toArray(self): ...

class DenseMatrix(Matrix):
    values: Any
    @overload
    def __init__(
        self, numRows: int, numCols: int, values: bytes, isTransposed: bool = ...
    ) -> None: ...
    @overload
    def __init__(
        self,
        numRows: int,
        numCols: int,
        values: Iterable[float],
        isTransposed: bool = ...,
    ) -> None: ...
    def __reduce__(self) -> Tuple[type, Tuple[int, int, bytes, int]]: ...
    def toArray(self) -> ndarray: ...
    def toSparse(self) -> SparseMatrix: ...
    def __getitem__(self, indices: Tuple[int, int]) -> float64: ...
    def __eq__(self, other) -> bool: ...

class SparseMatrix(Matrix):
    colPtrs: ndarray
    rowIndices: ndarray
    values: ndarray
    @overload
    def __init__(
        self,
        numRows: int,
        numCols: int,
        colPtrs: bytes,
        rowIndices: bytes,
        values: bytes,
        isTransposed: bool = ...,
    ) -> None: ...
    @overload
    def __init__(
        self,
        numRows: int,
        numCols: int,
        colPtrs: Iterable[int],
        rowIndices: Iterable[int],
        values: Iterable[float],
        isTransposed: bool = ...,
    ) -> None: ...
    def __reduce__(self) -> Tuple[type, Tuple[int, int, bytes, bytes, bytes, int]]: ...
    def __getitem__(self, indices: Tuple[int, int]) -> float64: ...
    def toArray(self) -> ndarray: ...
    def toDense(self) -> DenseMatrix: ...
    def __eq__(self, other) -> bool: ...

class Matrices:
    @overload
    @staticmethod
    def dense(
        numRows: int, numCols: int, values: bytes, isTransposed: bool = ...
    ) -> DenseMatrix: ...
    @overload
    @staticmethod
    def dense(
        numRows: int, numCols: int, values: Iterable[float], isTransposed: bool = ...
    ) -> DenseMatrix: ...
    @overload
    @staticmethod
    def sparse(
        numRows: int,
        numCols: int,
        colPtrs: bytes,
        rowIndices: bytes,
        values: bytes,
        isTransposed: bool = ...,
    ) -> SparseMatrix: ...
    @overload
    @staticmethod
    def sparse(
        numRows: int,
        numCols: int,
        colPtrs: Iterable[int],
        rowIndices: Iterable[int],
        values: Iterable[float],
        isTransposed: bool = ...,
    ) -> SparseMatrix: ...
