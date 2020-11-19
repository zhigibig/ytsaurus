from decimal_helpers import *
from yt_env_setup import YTEnvSetup
from yt_commands import *

from decimal import Decimal
from random import shuffle

import pytest


def with_name(skiff_type, name=None):
    if name is not None:
        skiff_type["name"] = name
    return skiff_type


def skiff_simple(wire_type, name=None):
    return with_name(
        {
            "wire_type": wire_type,
        },
        name,
    )


def skiff_optional(inner, name=None):
    return with_name(
        {
            "wire_type": "variant8",
            "children": [
                {
                    "wire_type": "nothing",
                },
                inner,
            ],
        },
        name,
    )


def skiff_tuple(*children, **kwargs):
    return with_name(
        {
            "wire_type": "tuple",
            "children": children,
        },
        **kwargs
    )


def skiff_repeated_variant8(*children, **kwargs):
    return with_name(
        {
            "wire_type": "repeated_variant8",
            "children": children,
        },
        **kwargs
    )


def make_skiff_format(*table_skiff_schemas):
    format = yson.YsonString("skiff")
    format.attributes["table_skiff_schemas"] = table_skiff_schemas
    return format


class TestSkiffFormat(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    @authors("ermolovd")
    def test_id_map(self):
        create("table", "//tmp/t_in")
        write_table(
            "//tmp/t_in",
            [
                {
                    "int64_column": -42,
                    "uint64_column": yson.YsonUint64(25),
                    "double_column": 3.14,
                    "boolean_column": True,
                    "string32_column": "foo",
                    "yson32_column": [110, "xxx", {"foo": "bar"}],
                },
                {
                    "int64_column": -15,
                    "uint64_column": yson.YsonUint64(25),
                    "double_column": 2.7,
                    "boolean_column": False,
                    "string32_column": "qux",
                    "yson32_column": None,
                },
            ],
        )

        create("table", "//tmp/t_out")

        format = yson.YsonString("skiff")
        format.attributes["table_skiff_schemas"] = [
            {
                "wire_type": "tuple",
                "children": [
                    {
                        "wire_type": "int64",
                        "name": "int64_column",
                    },
                    {
                        "wire_type": "uint64",
                        "name": "uint64_column",
                    },
                    {
                        "wire_type": "double",
                        "name": "double_column",
                    },
                    {
                        "wire_type": "boolean",
                        "name": "boolean_column",
                    },
                    {
                        "wire_type": "string32",
                        "name": "string32_column",
                    },
                    {
                        "wire_type": "yson32",
                        "name": "yson32_column",
                    },
                ],
            }
        ]

        map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="cat",
            spec={"mapper": {"format": format}},
        )

        assert read_table("//tmp/t_in") == read_table("//tmp/t_out")

        format = yson.YsonString("skiff")
        format.attributes["table_skiff_schemas"] = [
            {
                "wire_type": "tuple",
                "children": [
                    {
                        "wire_type": "yson32",
                        "name": "int64_column",
                    },
                    {
                        "wire_type": "yson32",
                        "name": "uint64_column",
                    },
                    {
                        "wire_type": "yson32",
                        "name": "double_column",
                    },
                    {
                        "wire_type": "yson32",
                        "name": "boolean_column",
                    },
                    {
                        "wire_type": "yson32",
                        "name": "string32_column",
                    },
                    {
                        "wire_type": "yson32",
                        "name": "yson32_column",
                    },
                ],
            }
        ]

        map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="cat",
            spec={
                "mapper": {
                    "format": format,
                }
            },
        )

        assert read_table("//tmp/t_in") == read_table("//tmp/t_out")

        format = yson.YsonString("skiff")
        format.attributes["table_skiff_schemas"] = [
            {
                "wire_type": "tuple",
                "children": [
                    {
                        "wire_type": "yson32",
                        "name": "$other_columns",
                    },
                ],
            }
        ]

        map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="cat",
            spec={
                "mapper": {
                    "format": format,
                }
            },
        )

        assert read_table("//tmp/t_in") == read_table("//tmp/t_out")

    @authors("ermolovd")
    def test_id_map_complex_types(self):
        schema = [
            {
                "name": "list_of_strings",
                "type_v3": list_type("string"),
            },
            {
                "name": "optional_list_of_strings",
                "type_v3": optional_type(list_type("string")),
            },
            {
                "name": "optional_optional_boolean",
                "type_v3": optional_type(optional_type("bool")),
            },
            {
                "name": "struct",
                "type_v3": struct_type(
                    [
                        ("key", "string"),
                        (
                            "points",
                            list_type(
                                struct_type(
                                    [
                                        ("x", "int64"),
                                        ("y", "int64"),
                                    ]
                                )
                            ),
                        ),
                    ]
                ),
            },
        ]
        create("table", "//tmp/t_in", attributes={"schema": schema})
        write_table(
            "//tmp/t_in",
            [
                {
                    "list_of_strings": ["foo", "bar", "baz"],
                    "optional_list_of_strings": None,
                    "optional_optional_boolean": [False],
                    "struct": {
                        "key": "qux",
                        "points": [{"x": 1, "y": 4}, {"x": 5, "y": 4}],
                    },
                },
                {
                    "list_of_strings": ["a", "bc"],
                    "optional_list_of_strings": ["defg", "hi"],
                    "optional_optional_boolean": [None],
                    "struct": {"key": "lol", "points": []},
                },
            ],
        )

        create("table", "//tmp/t_out", attributes={"schema": schema})

        format = make_skiff_format(
            skiff_tuple(
                skiff_repeated_variant8(skiff_simple("string32"), name="list_of_strings"),
                skiff_optional(
                    skiff_repeated_variant8(skiff_simple("string32")),
                    name="optional_list_of_strings",
                ),
                skiff_optional(
                    skiff_optional(skiff_simple("boolean")),
                    name="optional_optional_boolean",
                ),
                skiff_tuple(
                    skiff_simple("string32", name="key"),
                    skiff_repeated_variant8(
                        skiff_tuple(
                            skiff_simple("int64", name="x"),
                            skiff_simple("int64", name="y"),
                        ),
                        name="points",
                    ),
                    name="struct",
                ),
            ),
        )

        map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="tee /dev/stderr",
            spec={
                "mapper": {
                    "format": format,
                }
            },
        )

        assert read_table("//tmp/t_in") == read_table("//tmp/t_out")

    @authors("ermolovd")
    def test_read_complex_types(self):
        schema = [
            make_column("list_of_strings", list_type("string")),
            make_column("optional_list_of_strings", optional_type(list_type("string"))),
            make_column("optional_optional_boolean", optional_type(optional_type("bool"))),
            make_column("struct", struct_type(
                [
                    ("key", "string"),
                    ("points", list_type(
                        struct_type([("x", "int64"), ("y", "int64")])
                    )),
                ]
            )),

        ]
        create("table", "//tmp/table1", attributes={"schema": schema})
        write_table(
            "//tmp/table1",
            [
                {
                    "list_of_strings": ["foo", "bar", "baz"],
                    "optional_list_of_strings": None,
                    "optional_optional_boolean": [False],
                    "struct": {
                        "key": "qux",
                        "points": [{"x": 1, "y": 4}, {"x": 5, "y": 4}],
                    },
                },
                {
                    "list_of_strings": ["a", "bc"],
                    "optional_list_of_strings": ["defg", "hi"],
                    "optional_optional_boolean": [None],
                    "struct": {"key": "lol", "points": []},
                },
            ],
        )

        format = make_skiff_format(
            skiff_tuple(
                skiff_repeated_variant8(skiff_simple("string32"), name="list_of_strings"),
                skiff_optional(
                    skiff_repeated_variant8(skiff_simple("string32")),
                    name="optional_list_of_strings",
                ),
                skiff_optional(
                    skiff_optional(skiff_simple("boolean")),
                    name="optional_optional_boolean",
                ),
                skiff_tuple(
                    skiff_simple("string32", name="key"),
                    skiff_repeated_variant8(
                        skiff_tuple(
                            skiff_simple("int64", name="x"),
                            skiff_simple("int64", name="y"),
                        ),
                        name="points",
                    ),
                    name="struct",
                ),
            )
        )
        skiff_dump = read_table("//tmp/table1", output_format=format)

        # Check that column name is not in our table dump.
        # It's simple check that read_table didn't return yson.
        assert schema[0]["name"] not in skiff_dump

        create("table", "//tmp/table2", attributes={"schema": schema})
        write_table("//tmp/table2", skiff_dump, is_raw=True, input_format=format)

        assert read_table("//tmp/table1") == read_table("//tmp/table2")

    @authors("ermolovd")
    def test_read_empty_columns(self):
        create("table", "//tmp/t_in")
        write_table(
            "//tmp/t_in",
            [
                {"foo_column": 1},
                {"foo_column": 2},
                {"foo_column": 3},
            ],
        )

        format = yson.YsonString("skiff")
        format.attributes["table_skiff_schemas"] = [{"wire_type": "tuple", "children": []}]

        read_data = read_table("//tmp/t_in{}", output_format=format)
        assert read_data == "\x00\x00\x00\x00\x00\x00"

        create("table", "//tmp/t_out")
        write_table("//tmp/t_out", read_data, input_format=format, is_raw=True)

        assert read_table("//tmp/t_out") == [{}, {}, {}]

    @authors("ermolovd")
    def test_override_intermediate_table_schema(self):
        schema = [
            {"name": "key", "type_v3": "string"},
            {"name": "value", "type_v3": list_type("int64")},
        ]

        create("table", "//tmp/t_in", attributes={"schema": schema})
        create("table", "//tmp/t_out_reducer", attributes={"schema": schema})
        create("table", "//tmp/t_out_mapper", attributes={"schema": schema})

        write_table(
            "//tmp/t_in",
            [
                {"key": "foo", "value": [1, 2, 3]},
                {"key": "bar", "value": []},
            ],
        )

        format = make_skiff_format(
            skiff_tuple(
                skiff_simple("string32", name="key"),
                skiff_repeated_variant8(skiff_simple("int64"), name="value"),
            )
        )

        mapper_output_format = yson.YsonString("skiff")
        mapper_output_format.attributes["override_intermediate_table_schema"] = schema
        mapper_output_format.attributes["table_skiff_schemas"] = [
            format.attributes["table_skiff_schemas"][0],
            format.attributes["table_skiff_schemas"][0],
        ]

        reducer_input_format = yson.YsonString("skiff")
        reducer_input_format.attributes["override_intermediate_table_schema"] = schema
        reducer_input_format.attributes["table_skiff_schemas"] = [
            format.attributes["table_skiff_schemas"][0],
        ]

        map_reduce(
            in_="//tmp/t_in",
            out=["//tmp/t_out_mapper", "//tmp/t_out_reducer"],
            sort_by=["key"],
            mapper_command=(
                "cat && echo -en '"
                "\\x00\\x00"  # table index
                "\\x03\\x00\\x00\\x00"
                "baz"  # baz string
                "\\x00"
                "\\x42\\x00\\x00\\x00\\x00\\x00\\x00\\x00"  # list item
                "\\xFF"
                "' >&4"
            ),
            reducer_command="cat",
            spec={
                "mapper_output_table_count": 1,
                "mapper": {
                    "input_format": format,
                    "output_format": mapper_output_format,
                },
                "reducer": {
                    "input_format": reducer_input_format,
                    "output_format": format,
                },
            },
        )
        assert [
            {"key": "bar", "value": []},
            {"key": "foo", "value": [1, 2, 3]},
        ] == list(sorted(read_table("//tmp/t_out_reducer"), key=lambda x: x["key"]))
        assert [{"key": "baz", "value": [0x42]}] == read_table("//tmp/t_out_mapper")

    @authors("levysotsky")
    def test_map_reduce_trivial_mapper_schematized_streams(self):
        first_schema = [
            {"name": "key1", "type_v3": "int64"},
            {"name": "key3", "type_v3": "int64"},
        ]
        second_schema = [
            {"name": "key1", "type_v3": "int64"},
            {"name": "key2", "type_v3": "int64"},
        ]
        output_schema = [
            {"name": "key1", "type_v3": "int64"},
            {"name": "key2", "type_v3": optional_type("int64")},
            {"name": "key3", "type_v3": optional_type("int64")},
            {"name": "table_index", "type_v3": "int64"},
        ]
        create("table", "//tmp/in1", attributes={"schema": first_schema})
        create("table", "//tmp/in2", attributes={"schema": second_schema})
        create("table", "//tmp/out", attributes={"schema": output_schema})

        row_count = 2
        first_rows = [
            {
                "key1": 2 * i,
                "key3": i,
            }
            for i in xrange(row_count)
        ]
        shuffle(first_rows)
        write_table("//tmp/in1", first_rows)
        second_rows = [
            {
                "key1": 2 * i + 1,
                "key2": i,
            }
            for i in xrange(row_count)
        ]
        shuffle(second_rows)
        write_table("//tmp/in2", second_rows)

        reducer = """
import sys, json, struct

def read(n):
    bufs = []
    left = n
    while left > 0:
        bufs.append(sys.stdin.read(left))
        left -= len(bufs[-1])
        if len(bufs[-1]) == 0:
            assert left == n or left == 0
            break
    return b"".join(bufs)

while True:
    table_index_buf = read(2)
    if not table_index_buf:
        break
    (table_index,) = struct.unpack("<H", table_index_buf)
    (one_key,) = struct.unpack("<q", read(8))
    (another_key,) = struct.unpack("<q", read(8))
    row = {"table_index": table_index}
    if table_index == 0:
        row["key1"] = one_key
        row["key3"] = another_key
    else:
        row["key1"] = one_key
        row["key2"] = another_key
    sys.stderr.write(json.dumps(row) + "\\n")
    sys.stdout.write(json.dumps(row) + "\\n")
"""
        create("file", "//tmp/reducer.py")
        write_file("//tmp/reducer.py", reducer)
        input_format = yson.YsonString("skiff")
        input_format.attributes["table_skiff_schemas"] = [
            skiff_tuple(
                skiff_simple("int64", name="key1"),
                skiff_simple("nothing", name="key2"),
                skiff_simple("int64", name="key3"),
            ),
            skiff_tuple(
                skiff_simple("int64", name="key1"),
                skiff_simple("int64", name="key2"),
                skiff_simple("nothing", name="key3"),
            ),
        ]
        map_reduce(
            in_=["//tmp/in1", "//tmp/in2"],
            out="//tmp/out",
            reducer_file=["//tmp/reducer.py"],
            reducer_command="python reducer.py",
            sort_by=["key1", "key2", "key3"],
            spec={
                "reduce_job_io": {
                    "control_attributes": {
                        "enable_table_index": True,
                    },
                },
                "reducer": {
                    "input_format": input_format,
                    "output_format": "json",
                },
                "max_failed_job_count": 1,
            },
        )
        result_rows = read_table("//tmp/out")
        expected_rows = []
        for r in first_rows:
            r.update({"table_index": 0, "key2": None})
            expected_rows.append(r)
        for r in second_rows:
            r.update({"table_index": 1, "key3": None})
            expected_rows.append(r)
        assert sorted(expected_rows) == sorted(result_rows)

    @authors("ermolovd")
    def test_read_write_empty_tuple(self):
        schema = [{"name": "column", "type_v3": struct_type([])}]
        create(
            "table",
            "//tmp/table",
            attributes={
                "schema": schema,
            },
        )

        format = make_skiff_format(skiff_tuple(skiff_tuple(name="column")))

        skiff_data = "\x00\x00" "\x00\x00"

        write_table("//tmp/table", skiff_data, is_raw=True, input_format=format)
        assert read_table("//tmp/table") == [{"column": {}}, {"column": {}}]
        read_data = read_table("//tmp/table", is_raw=True, output_format=format)
        assert skiff_data == read_data

    @authors("ermolovd")
    def test_read_missing_complex_optional(self):
        create("table", "//tmp/table")

        format = make_skiff_format(
            skiff_tuple(
                skiff_simple("int64", name="column"),
                skiff_optional(skiff_repeated_variant8(skiff_simple("int64")), name="missing_column"),
            )
        )

        write_table("//tmp/table", [{"column": 1}, {"column": 2, "missing_column": None}])
        read_data = read_table("//tmp/table", is_raw=True, output_format=format)
        assert read_data == (
            "\x00\x00"
            "\x01\x00\x00\x00"
            "\x00\x00\x00\x00"
            "\x00"
            "\x00\x00"
            "\x02\x00\x00\x00"
            "\x00\x00\x00\x00"
            "\x00"
        )

        write_table("//tmp/table", [{"column": 1}, {"column": 2, "missing_column": "GG"}])
        with raises_yt_error("Cannot represent nonnull value of column"):
            read_table("//tmp/table", is_raw=True, output_format=format)

    @authors("ermolovd")
    def test_incorrect_wire_type_skiff_decimal(self):
        schema = [{"name": "column", "type_v3": decimal_type(5, 2)}]
        create_table("//tmp/table", schema=schema, force=True)
        write_table("//tmp/table", [
            {"column": encode_decimal("3.14", 5, 2)},
            {"column": encode_decimal("2.71", 5, 2)},
        ])
        format = make_skiff_format(
            skiff_tuple(skiff_simple("int64", name="column"))
        )
        with raises_yt_error("Skiff type Int64 cannot represent"):
            read_table("//tmp/table", is_raw=True, output_format=format)

        with raises_yt_error("Skiff type Int64 cannot represent"):
            write_table("//tmp/table", "", is_raw=True, input_format=format)


@pytest.mark.parametrize("precision,binary_size,skiff_type", [
    (3, 4, "int32"),
    (15, 8, "int64"),
    (35, 16, "int128")
])
@authors("ermolovd")
class TestGoodSkiffDecimal(YTEnvSetup):
    @authors("ermolovd")
    def test_skiff_nonoptional_schema_nonoptional_skiff(self, precision, binary_size, skiff_type):
        schema = [{"name": "column", "type_v3": decimal_type(precision, 2)}]
        create_table("//tmp/table", schema=schema, force=True)
        format = make_skiff_format(
            skiff_tuple(skiff_simple(skiff_type, name="column"))
        )
        skiff_data = (
            "\x00\x00\x3a\x01" + "\x00" * (binary_size - 2)
            + "\x00\x00\xf1\xfe" + "\xff" * (binary_size - 2)
        )
        write_table(
            "//tmp/table",
            skiff_data,
            is_raw=True,
            input_format=format,
        )
        assert [decode_decimal(row["column"], precision, 2) for row in read_table("//tmp/table")] == [
            Decimal("3.14"),
            Decimal("-2.71"),
        ]

        assert skiff_data == read_table("//tmp/table", is_raw=True, output_format=format)

    @authors("ermolovd")
    def test_skiff_optional_schema_nonoptional_skiff(self, precision, binary_size, skiff_type):
        schema = [{"name": "column", "type_v3": optional_type(decimal_type(precision, 2))}]
        create_table("//tmp/table", schema=schema, force=True)

        format = make_skiff_format(
            skiff_tuple(skiff_simple(skiff_type, name="column"))
        )
        skiff_data = (
            "\x00\x00\x3a\x01" + "\x00" * (binary_size - 2)
            + "\x00\x00\xf1\xfe" + "\xff" * (binary_size - 2)
        )
        write_table(
            "//tmp/table",
            skiff_data,
            is_raw=True,
            input_format=format)
        assert [decode_decimal(row["column"], precision, 2) for row in read_table("//tmp/table")] == [
            Decimal("3.14"),
            Decimal("-2.71"),
        ]
        assert skiff_data == read_table("//tmp/table", is_raw=True, output_format=format)

    @authors("ermolovd")
    def test_skiff_optional_schema_optional_skiff(self, precision, binary_size, skiff_type):
        schema = [{"name": "column", "type_v3": optional_type(decimal_type(precision, 2))}]
        create_table("//tmp/table", schema=schema, force=True)

        format = make_skiff_format(
            skiff_tuple(skiff_optional(skiff_simple(skiff_type), name="column"))
        )
        skiff_data = (
            "\x00\x00\x01\x3a\x01" + "\x00" * (binary_size - 2)
            + "\x00\x00\x00"
            + "\x00\x00\x01\xf1\xfe" + "\xff" * (binary_size - 2)
        )
        write_table(
            "//tmp/table",
            skiff_data,
            is_raw=True,
            input_format=format)
        actual =[
            decode_decimal(row["column"], precision, 2) if row["column"] else None
            for row in read_table("//tmp/table")
        ]
        assert actual == [
            Decimal("3.14"),
            None,
            Decimal("-2.71"),
        ]
        assert skiff_data == read_table("//tmp/table", is_raw=True, output_format=format)

    @authors("ermolovd")
    def test_skiff_nonoptional_schema_optional_skiff(self, precision, binary_size, skiff_type):
        schema = [{"name": "column", "type_v3": decimal_type(precision, 2)}]
        create_table("//tmp/table", schema=schema, force=True)

        format=make_skiff_format(
            skiff_tuple(skiff_optional(skiff_simple(skiff_type), name="column"))
        )
        skiff_data = (
            "\x00\x00\x01\x3a\x01" + "\x00" * (binary_size - 2)
            + "\x00\x00\x01\xf1\xfe" + "\xff" * (binary_size - 2)
        )
        write_table(
            "//tmp/table",
            skiff_data,
            is_raw=True,
            input_format=format)
        assert [decode_decimal(row["column"], precision, 2) for row in read_table("//tmp/table")] == [
            Decimal("3.14"),
            Decimal("-2.71"),
        ]
        with raises_yt_error(SchemaViolation):
            write_table(
                "//tmp/table",
                "\x00\x00\x00",
                is_raw=True,
                input_format=format)

        assert skiff_data == read_table("//tmp/table", is_raw=True, output_format=format)

    @authors("ermolovd")
    def test_skiff_decimal_inside_composite(self, precision, binary_size, skiff_type):
        schema = [
            make_column(
                "column",
                struct_type([("decimal_field", decimal_type(precision, 2)), ("string_field", "utf8")]))
        ]
        create_table("//tmp/table", schema=schema, force=True)

        format=make_skiff_format(
            skiff_tuple(
                skiff_tuple(
                    skiff_simple(skiff_type, name="decimal_field"),
                    skiff_simple("string32", name="string_field"),
                    name="column",
                )
            )
        )
        skiff_data = (
            "\x00\x00\x3a\x01" + "\x00" * (binary_size - 2) + "\x02\x00\x00\x00" "pi"
            + "\x00\x00\xf1\xfe" + "\xff" * (binary_size - 2) + "\x07\x00\x00\x00" "minus e"
        )
        write_table(
            "//tmp/table",
            skiff_data,
            is_raw=True,
            input_format=format)
        assert [
            (decode_decimal(row["column"]["decimal_field"], precision, 2), row["column"]["string_field"])
            for row in read_table("//tmp/table")
        ] == [
            (Decimal("3.14"), "pi"),
            (Decimal("-2.71"), "minus e"),
        ]
        assert skiff_data == read_table("//tmp/table", is_raw=True, output_format=format)
