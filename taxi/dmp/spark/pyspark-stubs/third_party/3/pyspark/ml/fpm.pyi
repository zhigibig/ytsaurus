# Stubs for pyspark.ml.base (Python 3)
#

from typing import Optional

from pyspark.ml._typing import P
from pyspark.ml.util import *
from pyspark.ml.wrapper import JavaEstimator, JavaParams, JavaModel
from pyspark.ml.param.shared import *
from pyspark.sql.dataframe import DataFrame

class _FPGrowthParams(HasPredictionCol):
    itemsCol: Param[str]
    minSupport: Param[float]
    numPartitions: Param[int]
    minConfidence: Param[float]
    def getItemsCol(self) -> str: ...
    def getMinSupport(self) -> float: ...
    def getNumPartitions(self) -> int: ...
    def getMinConfidence(self) -> float: ...

class FPGrowthModel(JavaModel, _FPGrowthParams, JavaMLWritable, JavaMLReadable[FPGrowthModel]):
    def setItemsCol(self, value: str) -> FPGrowthModel: ...
    def setMinConfidence(self, value: float) -> FPGrowthModel: ...
    @property
    def freqItemsets(self) -> DataFrame: ...
    @property
    def associationRules(self) -> DataFrame: ...

class FPGrowth(JavaEstimator[FPGrowthModel], _FPGrowthParams, JavaMLWritable, JavaMLReadable[FPGrowth]):
    def __init__(self, *, minSupport: float = ..., minConfidence: float = ..., itemsCol: str = ..., predictionCol: str = ..., numPartitions: Optional[int] = ...) -> None: ...
    def setParams(self, *, minSupport: float = ..., minConfidence: float = ..., itemsCol: str = ..., predictionCol: str = ..., numPartitions: Optional[int] = ...) -> FPGrowth: ...
    def setItemsCol(self, value: str) -> FPGrowth: ...
    def setMinSupport(self, value: float) -> FPGrowth: ...
    def setNumPartitions(self, value: int) -> FPGrowth: ...
    def setMinConfidence(self, value: float) -> FPGrowth: ...

class PrefixSpan(JavaParams):
    minSupport: Param[float]
    maxPatternLength: Param[int]
    maxLocalProjDBSize: Param[int]
    sequenceCol: Param[str]
    def __init__(self, *, minSupport: float = ..., maxPatternLength: int = ..., maxLocalProjDBSize: int = ..., sequenceCol: str = ...) -> None: ...
    def setParams(self, *, minSupport: float = ..., maxPatternLength: int = ..., maxLocalProjDBSize: int = ..., sequenceCol: str = ...) -> PrefixSpan: ...
    def setMinSupport(self, value: float) -> PrefixSpan: ...
    def getMinSupport(self) -> float: ...
    def setMaxPatternLength(self, value: int) -> PrefixSpan: ...
    def getMaxPatternLength(self) -> int: ...
    def setMaxLocalProjDBSize(self, value: int) -> PrefixSpan: ...
    def getMaxLocalProjDBSize(self) -> int: ...
    def setSequenceCol(self, value: str) -> PrefixSpan: ...
    def getSequenceCol(self) -> str: ...
    def findFrequentSequentialPatterns(self, dataset: DataFrame) -> DataFrame: ...
