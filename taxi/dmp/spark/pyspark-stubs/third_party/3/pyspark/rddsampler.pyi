# Stubs for pyspark.rddsampler (Python 3.5)
#

from typing import Any, Dict, Hashable, Iterator, Optional, Tuple, TypeVar

T = TypeVar("T")
U = TypeVar("U")
K = TypeVar("K")
V = TypeVar("V")

class RDDSamplerBase:
    def __init__(self, withReplacement: bool, seed: Optional[int] = ...) -> None: ...
    def initRandomGenerator(self, split: int) -> None: ...
    def getUniformSample(self) -> float: ...
    def getPoissonSample(self, mean: float) -> int: ...
    def func(self, split: int, iterator: Iterator[Any]) -> Iterator[Any]: ...

class RDDSampler(RDDSamplerBase):
    def __init__(
        self, withReplacement: bool, fraction: float, seed: Optional[int] = ...
    ) -> None: ...
    def func(self, split: int, iterator: Iterator[T]) -> Iterator[T]: ...

class RDDRangeSampler(RDDSamplerBase):
    def __init__(
        self, lowerBound: T, upperBound: T, seed: Optional[Any] = ...
    ) -> None: ...
    def func(self, split: int, iterator: Iterator[T]) -> Iterator[T]: ...

class RDDStratifiedSampler(RDDSamplerBase):
    def __init__(
        self,
        withReplacement: bool,
        fractions: Dict[K, float],
        seed: Optional[int] = ...,
    ) -> None: ...
    def func(
        self, split: int, iterator: Iterator[Tuple[K, V]]
    ) -> Iterator[Tuple[K, V]]: ...
