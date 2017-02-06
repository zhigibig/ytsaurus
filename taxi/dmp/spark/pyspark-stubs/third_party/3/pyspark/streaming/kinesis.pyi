# Stubs for pyspark.streaming.kinesis (Python 3.5)
#
# NOTE: This dynamically typed stub was automatically generated by stubgen.

from typing import Any, Optional

def utf8_decoder(s): ...

class KinesisUtils:
    @staticmethod
    def createStream(ssc, kinesisAppName, streamName, endpointUrl, regionName, initialPositionInStream, checkpointInterval, storageLevel: Any = ..., awsAccessKeyId: Optional[Any] = ..., awsSecretKey: Optional[Any] = ..., decoder: Any = ...): ...

class InitialPositionInStream:
    LATEST = ...  # type: Any
    TRIM_HORIZON = ...  # type: Any
