package ru.yandex.spark.yt.utils

import org.apache.spark.sql.Row
import org.apache.spark.sql.catalyst.encoders.RowEncoder
import org.scalatest.{FlatSpec, Matchers}
import ru.yandex.spark.yt.LocalSpark

class DataFrameUtilsTest extends FlatSpec with Matchers with LocalSpark {

  behavior of "DataFrameUtilsTest"

  import DataFrameUtils._

  it should "get top from df" in {
    import spark.implicits._

    val df = Seq(
      ("1", "1", "1", "a"),
      ("1", "1", "2", "b"),
      ("1", "1", "3", "c"),
      ("1", "2", "1", "d"),
      ("1", "2", "2", "e"),
      ("2", "3", "1", "f")
    ).
      toDF("user_phone", "brand", "moscow_event_dttm", "target")

    val res = df.top(Seq("user_phone", "brand"), Seq("moscow_event_dttm"), 1)

    res.columns should contain theSameElementsAs df.columns
    res.select(df.columns.head, df.columns.tail:_*).collect() should contain theSameElementsAs Seq(
      Row("1", "1", "1", "a"),
      Row("1", "2", "1", "d"),
      Row("2", "3", "1", "f")
    )
  }

}
