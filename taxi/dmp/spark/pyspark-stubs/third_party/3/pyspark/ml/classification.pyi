# Stubs for pyspark.ml.classification (Python 3.5)
#

from typing import Any, Dict, List, Optional, TypeVar

from pyspark.ml._typing import ParamMap
from pyspark.ml.base import Estimator, Model, Transformer
from pyspark.ml.linalg import Matrix, Vector
from pyspark.ml.param.shared import *
from pyspark.ml.regression import DecisionTreeModel,  DecisionTreeParams, DecisionTreeRegressionModel, GBTParams, HasVarianceImpurity, RandomForestParams, TreeEnsembleModel
from pyspark.ml.util import *
from pyspark.ml.wrapper import JavaEstimator, JavaModel
from pyspark.ml.wrapper import JavaWrapper
from pyspark.sql.dataframe import DataFrame

P = TypeVar("P")
M = TypeVar("M", bound=Transformer)

class JavaClassificationModel(JavaPredictionModel):
    @property
    def numClasses(self) -> int: ...

class LinearSVC(JavaEstimator[LinearSVCModel], HasFeaturesCol, HasLabelCol, HasPredictionCol, HasMaxIter, HasRegParam, HasTol, HasRawPredictionCol, HasFitIntercept, HasStandardization, HasThreshold, HasWeightCol, HasAggregationDepth, JavaMLWritable, JavaMLReadable):
    def __init__(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., maxIter: int = ..., regParam: float = ..., tol: float = ..., rawPredictionCol: str = ..., fitIntercept: bool = ..., standardization: bool = ..., threshold: float = ..., weightCol: Optional[str] = ..., aggregationDepth: int = ...) -> None: ...
    def setParams(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., maxIter: int = ..., regParam: float = ..., tol: float = ..., rawPredictionCol: str = ..., fitIntercept: bool = ..., standardization: bool = ..., threshold: float = ..., weightCol: Optional[str] = ..., aggregationDepth: int = ...) -> 'LinearSVC': ...

class LinearSVCModel(JavaModel, JavaClassificationModel, JavaMLWritable, JavaMLReadable):
    @property
    def coefficients(self) -> Vector: ...
    @property
    def intercept(self) -> float: ...

class LogisticRegression(JavaEstimator[LogisticRegressionModel], HasFeaturesCol, HasLabelCol, HasPredictionCol, HasMaxIter, HasRegParam, HasTol, HasProbabilityCol, HasRawPredictionCol, HasElasticNetParam, HasFitIntercept, HasStandardization, HasThresholds, HasWeightCol, HasAggregationDepth, JavaMLWritable, JavaMLReadable):
    threshold = ...  # type: Param
    family = ...  # type: Param
    lowerBoundsOnCoefficients = ...  # type: Param
    upperBoundsOnCoefficients = ...  # type: Param
    lowerBoundsOnIntercepts = ...  # type: Param
    upperBoundsOnIntercepts = ...  # type: Param
    def __init__(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., maxIter: int = ..., regParam: float = ..., elasticNetParam: float = ..., tol: float = ..., fitIntercept: bool = ..., threshold: float = ..., thresholds: Optional[List[float]] = ..., probabilityCol: str = ..., rawPredictionCol: str = ..., standardization: bool = ..., weightCol: Optional[str] = ..., aggregationDepth: int = ..., family: str = ..., lowerBoundsOnCoefficients: Optional[Matrix] = ..., upperBoundsOnCoefficients: Optional[Matrix] = ..., lowerBoundsOnIntercepts: Optional[Vector] = ..., upperBoundsOnIntercepts: Optional[Vector] = ...) -> None: ...
    def setParams(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., maxIter: int = ..., regParam: float = ..., elasticNetParam: float = ..., tol: float = ..., fitIntercept: bool = ..., threshold: float = ..., thresholds: Optional[List[float]] = ..., probabilityCol: str = ..., rawPredictionCol: str = ..., standardization: bool = ..., weightCol: Optional[str] = ..., aggregationDepth: int = ..., family: str = ..., lowerBoundsOnCoefficients: Optional[Matrix] = ..., upperBoundsOnCoefficients: Optional[Matrix] = ..., lowerBoundsOnIntercepts: Optional[Vector] = ..., upperBoundsOnIntercepts: Optional[Vector] = ...) -> 'LogisticRegression': ...
    def setThreshold(self, value: float) -> 'LogisticRegression': ...
    def getThreshold(self) -> float: ...
    def setThresholds(self, value: List[float]) -> 'LogisticRegression': ...
    def getThresholds(self) -> List[float]: ...
    def setFamily(self, value: str) -> 'LogisticRegression': ...
    def getFamily(self) -> str: ...
    def setLowerBoundsOnCoefficients(self, value: Matrix) -> 'LogisticRegression': ...
    def getLowerBoundsOnCoefficients(self) -> Matrix : ...
    def setUpperBoundsOnCoefficients(self, value: Matrix) -> 'LogisticRegression': ...
    def getUpperBoundsOnCoefficients(self) -> Matrix: ...
    def setLowerBoundsOnIntercepts(self, value: Vector) -> 'LogisticRegression': ...
    def getLowerBoundsOnIntercepts(self) -> Vector: ...
    def setUpperBoundsOnIntercepts(self, value: Vector) -> 'LogisticRegression': ...
    def getUpperBoundsOnIntercepts(self) -> Vector: ...

class LogisticRegressionModel(JavaModel, JavaClassificationModel, JavaMLWritable, JavaMLReadable, HasTrainingSummary):
    @property
    def coefficients(self) -> Vector: ...
    @property
    def intercept(self) -> float: ...
    @property
    def coefficientMatrix(self) -> Matrix: ...
    @property
    def interceptVector(self) -> Vector: ...
    @property
    def summary(self) -> LogisticRegressionTrainingSummary: ...
    @property
    def hasSummary(self) -> bool: ...
    def evaluate(self, dataset: DataFrame) -> LogisticRegressionSummary: ...

class LogisticRegressionSummary(JavaWrapper):
    @property
    def predictions(self) -> DataFrame: ...
    @property
    def probabilityCol(self) -> str: ...
    @property
    def predictionCol(self) -> str: ...
    @property
    def labelCol(self) -> str: ...
    @property
    def featuresCol(self) -> str: ...
    @property
    def labels(self) -> List[float]: ...
    @property
    def truePositiveRateByLabel(self) -> List[float]: ...
    @property
    def falsePositiveRateByLabel(self) -> List[float]: ...
    @property
    def precisionByLabel(self) -> List[float]: ...
    @property
    def recallByLabel(self) -> List[float]: ...
    def fMeasureByLabel(self, beta: float = ...) -> List[float]: ...
    @property
    def accuracy(self) -> float: ...
    @property
    def weightedTruePositiveRate(self) -> float: ...
    @property
    def weightedFalsePositiveRate(self) -> float: ...
    @property
    def weightedRecall(self) -> float: ...
    @property
    def weightedPrecision(self) -> float: ...
    def weightedFMeasure(self, beta: float = ...) -> float: ...

class LogisticRegressionTrainingSummary(LogisticRegressionSummary):
    @property
    def objectiveHistory(self) -> List[float]: ...
    @property
    def totalIterations(self) -> int: ...

class BinaryLogisticRegressionSummary(LogisticRegressionSummary):
    @property
    def roc(self) -> DataFrame: ...
    @property
    def areaUnderROC(self) -> float: ...
    @property
    def pr(self) -> DataFrame: ...
    @property
    def fMeasureByThreshold(self) -> DataFrame: ...
    @property
    def precisionByThreshold(self) -> DataFrame: ...
    @property
    def recallByThreshold(self) -> DataFrame: ...

class BinaryLogisticRegressionTrainingSummary(BinaryLogisticRegressionSummary, LogisticRegressionTrainingSummary): ...

class TreeClassifierParams:
    supportedImpurities = ...  # type: List[str]
    impurity = ...  # type: Param
    def __init__(self) -> None: ...
    def getImpurity(self)  -> str: ...

class DecisionTreeClassifier(JavaEstimator[DecisionTreeClassificationModel], HasFeaturesCol, HasLabelCol, HasWeightCol, HasPredictionCol, HasProbabilityCol, HasRawPredictionCol, DecisionTreeParams, TreeClassifierParams, HasCheckpointInterval, HasSeed, JavaMLWritable, JavaMLReadable):
    def __init__(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., probabilityCol: str = ..., rawPredictionCol: str = ..., maxDepth: int = ..., maxBins: int = ..., minInstancesPerNode: int = ..., minInfoGain: float = ..., maxMemoryInMB: int = ..., cacheNodeIds: bool = ..., checkpointInterval: int = ..., impurity: str = ..., seed: Optional[int] = ..., weightCol: Optional[str] = ..., leafCol: str = ...) -> None: ...
    def setParams(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., probabilityCol: str = ..., rawPredictionCol: str = ..., maxDepth: int = ..., maxBins: int = ..., minInstancesPerNode: int = ..., minInfoGain: float = ..., maxMemoryInMB: int = ..., cacheNodeIds: bool = ..., checkpointInterval: int = ..., impurity: str = ..., seed: Optional[int] = ..., weightCol: Optional[str] = ..., leafCol: str = ...) -> DecisionTreeClassifier: ...
    def setMaxDepth(self, value: int) -> DecisionTreeClassifier: ...
    def setMaxBins(self, value: int) -> DecisionTreeClassifier: ...
    def setMinInstancesPerNode(self, value: int) -> DecisionTreeClassifier: ...
    def setMinInfoGain(self, value: float) -> DecisionTreeClassifier: ...
    def setMaxMemoryInMB(self, value: int) -> DecisionTreeClassifier: ...
    def setCacheNodeIds(self, value: bool) -> DecisionTreeClassifier: ...
    def setImpurity(self, value: str) -> DecisionTreeClassifier: ...

class DecisionTreeClassificationModel(DecisionTreeModel, JavaClassificationModel, JavaMLWritable, JavaMLReadable):
    @property
    def featureImportances(self) -> Vector: ...

class RandomForestClassifier(JavaEstimator[RandomForestClassificationModel], HasFeaturesCol, HasLabelCol, HasPredictionCol, HasSeed, HasRawPredictionCol, HasProbabilityCol, RandomForestParams, TreeClassifierParams, HasCheckpointInterval, JavaMLWritable, JavaMLReadable):
    def __init__(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., probabilityCol: str = ..., rawPredictionCol: str = ..., maxDepth: int = ..., maxBins: int = ..., minInstancesPerNode: int = ..., minInfoGain: float = ..., maxMemoryInMB: int = ..., cacheNodeIds: bool = ..., checkpointInterval: int = ..., impurity: str = ..., numTrees: int = ..., featureSubsetStrategy: str = ..., seed: Optional[int] = ..., subsamplingRate: float = ..., leafCol: str = ...) -> None: ...
    def setParams(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., probabilityCol: str = ..., rawPredictionCol: str = ..., maxDepth: int = ..., maxBins: int = ..., minInstancesPerNode: int = ..., minInfoGain: float = ..., maxMemoryInMB: int = ..., cacheNodeIds: bool = ..., checkpointInterval: int = ..., seed: Optional[int] = ..., impurity: str = ..., numTrees: int = ..., featureSubsetStrategy: str = ..., subsamplingRate: float = ..., leafCol: str = ...) -> RandomForestClassifier: ...
    def setFeatureSubsetStrategy(self, value: str) -> RandomForestClassifier: ...

class RandomForestClassificationModel(TreeEnsembleModel, JavaClassificationModel, JavaMLWritable, JavaMLReadable):
    @property
    def featureImportances(self) -> Vector: ...
    @property
    def trees(self) -> List[DecisionTreeClassificationModel]: ...

class GBTClassifierParams(GBTParams, HasVarianceImpurity):
    supportedLossTypes = ...  # type: List[str]
    lossType = ...  # type: Param
    def getLossType(self) -> str: ...

class GBTClassifier(JavaEstimator, HasFeaturesCol, HasLabelCol, HasPredictionCol, GBTClassifierParams, HasCheckpointInterval, HasSeed, JavaMLWritable, JavaMLReadable):
    def __init__(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., maxDepth: int = ..., maxBins: int = ..., minInstancesPerNode: int = ..., minInfoGain: float = ..., maxMemoryInMB: int = ..., cacheNodeIds: bool = ..., checkpointInterval: int = ..., lossType: str = ..., maxIter: int = ..., stepSize: float = ..., seed: Optional[int] = ..., subsamplingRate: float = ..., featureSubsetStrategy: str = ...,  validationTol: float = ..., validationIndicatorCol: Optional[str] = ...) -> None: ...
    def setParams(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., maxDepth: int = ..., maxBins: int = ..., minInstancesPerNode: int = ..., minInfoGain: float = ..., maxMemoryInMB: int = ..., cacheNodeIds: bool = ..., checkpointInterval: int = ..., lossType: str = ..., maxIter: int = ..., stepSize: float = ..., seed: Optional[int] = ..., subsamplingRate: float = ..., featureSubsetStrategy: str = ..., validationTol: float = ..., validationIndicatorCol: Optional[str] = ...) -> GBTClassifier: ...
    def setLossType(self, value: str) -> GBTClassifier: ...
    def setFeatureSubsetStrategy(self, value: str) -> GBTClassifier: ...
    def setValidationIndicatorCol(self, value: str) -> GBTClassifier: ...

class GBTClassificationModel(TreeEnsembleModel, JavaClassificationModel, JavaMLWritable, JavaMLReadable):
    @property
    def featureImportances(self) -> Vector: ...
    @property
    def trees(self) -> List[DecisionTreeRegressionModel]: ...
    def evaluateEachIteration(self, dataset: DataFrame) -> List[float]: ...

class NaiveBayes(JavaEstimator[NaiveBayesModel], HasFeaturesCol, HasLabelCol, HasPredictionCol, HasProbabilityCol, HasRawPredictionCol, HasThresholds, HasWeightCol, JavaMLWritable, JavaMLReadable):
    smoothing = ...  # type: Param
    modelType = ...  # type: Param
    def __init__(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., probabilityCol: str = ..., rawPredictionCol: str = ..., smoothing: float = ..., modelType: str = ..., thresholds: Optional[List[float]] = ..., weightCol: Optional[str] = ...) -> None: ...
    def setParams(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., probabilityCol: str = ..., rawPredictionCol: str = ..., smoothing: float = ..., modelType: str = ..., thresholds: Optional[List[float]] = ..., weightCol: Optional[str] = ...) -> 'NaiveBayes': ...
    def setSmoothing(self, value: float) -> 'NaiveBayes': ...
    def getSmoothing(self) -> float: ...
    def setModelType(self, value: str) -> 'NaiveBayes': ...
    def getModelType(self) -> str: ...

class NaiveBayesModel(JavaModel, JavaClassificationModel, JavaMLWritable, JavaMLReadable):
    @property
    def pi(self) -> Vector: ...
    @property
    def theta(self) -> Matrix: ...

class MultilayerPerceptronClassifier(JavaEstimator[MultilayerPerceptronClassificationModel], HasFeaturesCol, HasLabelCol, HasPredictionCol, HasMaxIter, HasTol, HasSeed, HasStepSize, HasSolver, JavaMLWritable, JavaMLReadable, HasProbabilityCol, HasRawPredictionCol):
    layers = ...  # type: Param
    blockSize = ...  # type: Param
    solver = ...  # type: Param
    initialWeights = ...  # type: Param
    def __init__(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., maxIter: int = ..., tol: float = ..., seed: Optional[int] = ..., layers: Optional[List[int]] = ..., blockSize: int = ..., stepSize: float = ..., solver: str = ..., initialWeights: Optional[Vector] = ..., probabilityCol: str = ..., rawPredictionCol: str = ...) -> None: ...
    def setParams(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., maxIter: int = ..., tol: float = ..., seed: Optional[int] = ..., layers: Optional[List[int]] = ..., blockSize: int = ..., stepSize: float = ..., solver: str = ..., initialWeights: Optional[Vector] = ..., probabilityCol: str = ..., rawPredictionCol: str = ...) -> 'MultilayerPerceptronClassifier': ...
    def setLayers(self, value: List[int]) -> 'MultilayerPerceptronClassifier': ...
    def getLayers(self) -> List[int]: ...
    def setBlockSize(self, value: int) -> 'MultilayerPerceptronClassifier': ...
    def getBlockSize(self) -> int: ...
    def setStepSize(self, value: float) -> 'MultilayerPerceptronClassifier': ...
    def getStepSize(self) -> float: ...
    def setInitialWeights(self, value: Vector) -> 'MultilayerPerceptronClassifier': ...
    def getInitialWeights(self) -> Vector: ...

class MultilayerPerceptronClassificationModel(JavaModel, JavaClassificationModel, JavaMLWritable, JavaMLReadable):
    @property
    def layers(self) -> List[int]: ...
    @property
    def weights(self) -> Vector: ...

class OneVsRestParams(HasFeaturesCol, HasLabelCol, HasWeightCol, HasPredictionCol, HasRawPredictionCol):
    classifier = ...  # type: Param
    def setClassifier(self, value: Estimator[M]) -> OneVsRestParams: ...
    def getClassifier(self) -> Estimator[M]: ...

class OneVsRest(Estimator[OneVsRestModel], OneVsRestParams, HasParallelism, JavaMLReadable, JavaMLWritable):
    def __init__(self, featuresCol: str = ..., labelCol: str = ..., predictionCol: str = ..., rawPredictionCol: str = ..., classifier: Optional[Estimator[M]] = ..., weightCol: Optional[str] = ..., parallelism: int = ...) -> None: ...
    def setParams(self, featuresCol: Optional[str] = ..., labelCol: Optional[str] = ..., predictionCol: Optional[str] = ..., rawPredictionCol: str = ..., classifier: Optional[Estimator[M]] = ..., weightCol: Optional[str] = ..., parallelism: int = ...) -> OneVsRest: ...
    def copy(self, extra: Optional[ParamMap] = ...) -> OneVsRest: ...

class OneVsRestModel(Model, OneVsRestParams, JavaMLReadable, JavaMLWritable):
    models = ...  # type: List[Transformer]
    def __init__(self, models: List[Transformer]) -> None: ...
    def copy(self, extra: Optional[ParamMap] = ...) -> 'OneVsRestModel': ...
