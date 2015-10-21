#pragma once

#include "public.h"

#include <ytlib/table_client/public.h>

#include <core/ytree/yson_serializable.h>

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

class TYsonFormatConfig
    : public NYTree::TYsonSerializable
{
public:
    NYson::EYsonFormat Format;
    bool BooleanAsString;

    TYsonFormatConfig()
    {
        RegisterParameter("format", Format)
            .Default(NYson::EYsonFormat::Binary);
        RegisterParameter("boolean_as_string", BooleanAsString)
            .Default(false);
    }
};

DEFINE_REFCOUNTED_TYPE(TYsonFormatConfig)

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EJsonFormat,
    (Text)
    (Pretty)
);

DEFINE_ENUM(EJsonAttributesMode,
    (Always)
    (Never)
    (OnDemand)
);

class TJsonFormatConfig
    : public NYTree::TYsonSerializable
{
public:
    EJsonFormat Format;
    EJsonAttributesMode AttributesMode;
    bool EncodeUtf8;
    i64 MemoryLimit;

    TNullable<int> StringLengthLimit;

    bool BooleanAsString;

    // Size of buffer used read out input stream in parser.
    // NB: in case of parsing long string yajl holds in memory whole string prefix and copy it on every parse call.
    // Therefore parsing long strings works faster with larger buffer.
    int BufferSize;

    TJsonFormatConfig()
    {
        RegisterParameter("format", Format)
            .Default(EJsonFormat::Text);
        RegisterParameter("attributes_mode", AttributesMode)
            .Default(EJsonAttributesMode::OnDemand);
        RegisterParameter("encode_utf8", EncodeUtf8)
            .Default(true);
        RegisterParameter("string_length_limit", StringLengthLimit)
            .Default();
        RegisterParameter("boolean_as_string", BooleanAsString)
            .Default(false);
        RegisterParameter("buffer_size", BufferSize)
            .Default(16 * 1024 * 1024);

        // NB: yajl can consume two times more memory than row size.
        MemoryLimit = 2 * NTableClient::MaxRowWeightLimit;
    }
};

DEFINE_REFCOUNTED_TYPE(TJsonFormatConfig)

///////////////////////////////////////////////////////////////////////////////////////////////
// Readers for Yamr and Dsv share lots of methods and functionality                          //
// and dependency diagram has the following shape:                                           //
//                                                                                           //
//                    TTableFormatConfigBase --------------------------.                     //
//                      /                 \                             \                    //
//                     /                   \                             \                   //
//       TYamrFormatConfigBase        TDsvFormatConfigBase                \                  //
//            /        \                   /            \                  \                 //
//           /          \                 /              \                  \                //
// TYamrFormatConfig   TYamredDsvFormatConfig   TDsvFormatConfig  TSchemafulDsvFormatConfig  //
//                                                                                           //
// All fields are declared in Base classes, all parameters are                               //
// registered in derived classes.                                                            //

class TTableFormatConfigBase 
    : public NYTree::TYsonSerializable 
{
public:
    char RecordSeparator;
    char FieldSeparator;
    
    // Escaping rules (EscapingSymbol is '\\')
    //  * '\0' ---> "\0"
    //  * '\n' ---> "\n"
    //  * '\t' ---> "\t"
    //  * 'X'  ---> "\X" if X not in ['\0', '\n', '\t']
    bool EnableEscaping;
    char EscapingSymbol;
    
    bool EnableTableIndex;
};

DEFINE_REFCOUNTED_TYPE(TTableFormatConfigBase);

////////////////////////////////////////////////////////////////////////////////

class TYamrFormatConfigBase
    : public virtual TTableFormatConfigBase
{
public:
    bool HasSubkey;
    bool Lenval;
};

DEFINE_REFCOUNTED_TYPE(TYamrFormatConfigBase);

////////////////////////////////////////////////////////////////////////////////

class TDsvFormatConfigBase
    : public virtual TTableFormatConfigBase
{
public:
    char KeyValueSeparator;

    // Only supported for tabular data
    TNullable<Stroka> LinePrefix;
};

DEFINE_REFCOUNTED_TYPE(TDsvFormatConfigBase);

////////////////////////////////////////////////////////////////////////////////

class TYamrFormatConfig
    : public TYamrFormatConfigBase
{
public:
    Stroka Key;
    Stroka Subkey;
    Stroka Value;

    TYamrFormatConfig()
    {
        RegisterParameter("has_subkey", HasSubkey)
            .Default(false);
        RegisterParameter("key", Key)
            .Default("key");
        RegisterParameter("subkey", Subkey)
            .Default("subkey");
        RegisterParameter("value", Value)
            .Default("value");
        RegisterParameter("lenval", Lenval)
            .Default(false);
        RegisterParameter("fs", FieldSeparator)
            .Default('\t');
        RegisterParameter("rs", RecordSeparator)
            .Default('\n');
        RegisterParameter("enable_table_index", EnableTableIndex)
            .Default(false);
        RegisterParameter("enable_escaping", EnableEscaping)
            .Default(false);
        RegisterParameter("escaping_symbol", EscapingSymbol)
            .Default('\\');
    }
};

DEFINE_REFCOUNTED_TYPE(TYamrFormatConfig)

////////////////////////////////////////////////////////////////////////////////

class TDsvFormatConfig
    : public TDsvFormatConfigBase
{
public:

    Stroka TableIndexColumn;

    TDsvFormatConfig()
    {
        RegisterParameter("record_separator", RecordSeparator)
            .Default('\n');
        RegisterParameter("key_value_separator", KeyValueSeparator)
            .Default('=');
        RegisterParameter("field_separator", FieldSeparator)
            .Default('\t');
        RegisterParameter("line_prefix", LinePrefix)
            .Default();
        RegisterParameter("enable_escaping", EnableEscaping)
            .Default(true);
        RegisterParameter("escaping_symbol", EscapingSymbol)
            .Default('\\');
        RegisterParameter("enable_table_index", EnableTableIndex)
            .Default(false);
        RegisterParameter("table_index_column", TableIndexColumn)
            .Default("@table_index")
            .NonEmpty();
    }
};

DEFINE_REFCOUNTED_TYPE(TDsvFormatConfig)

////////////////////////////////////////////////////////////////////////////////

class TYamredDsvFormatConfig
    : public TYamrFormatConfigBase
    , public TDsvFormatConfigBase
{
public:
    char YamrKeysSeparator;

    std::vector<Stroka> KeyColumnNames;
    std::vector<Stroka> SubkeyColumnNames;

    TYamredDsvFormatConfig()
    {
        RegisterParameter("record_separator", RecordSeparator)
            .Default('\n');
        RegisterParameter("key_value_separator", KeyValueSeparator)
            .Default('=');
        RegisterParameter("field_separator", FieldSeparator)
            .Default('\t');
        RegisterParameter("line_prefix", LinePrefix)
            .Default();
        RegisterParameter("enable_escaping", EnableEscaping)
            .Default(true);
        RegisterParameter("escaping_symbol", EscapingSymbol)
            .Default('\\');
        RegisterParameter("enable_table_index", EnableTableIndex)
            .Default(false);
        RegisterParameter("has_subkey", HasSubkey)
            .Default(false);
        RegisterParameter("lenval", Lenval)
            .Default(false);
        RegisterParameter("key_column_names", KeyColumnNames);
        RegisterParameter("subkey_column_names", SubkeyColumnNames)
            .Default();
        RegisterParameter("yamr_keys_separator", YamrKeysSeparator)
            .Default(' ');

        RegisterValidator([&] () {
            yhash_set<Stroka> names;

            for (const auto& name : KeyColumnNames) {
                if (!names.insert(name).second) {
                    THROW_ERROR_EXCEPTION("Duplicate column %Qv found in \"key_column_names\"",
                        name);
                }
            }

            for (const auto& name : SubkeyColumnNames) {
                if (!names.insert(name).second) {
                    THROW_ERROR_EXCEPTION("Duplicate column %Qv found in \"subkey_column_names\"",
                        name);
                }
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TYamredDsvFormatConfig)

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EMissingSchemafulDsvValueMode,
    (SkipRow)
    (Fail)
    (PrintSentinel)
);

class TSchemafulDsvFormatConfig
    : public TTableFormatConfigBase
{
public:
    TNullable<std::vector<Stroka>> Columns;

    EMissingSchemafulDsvValueMode MissingValueMode;
    Stroka MissingValueSentinel;


    const std::vector<Stroka>& GetColumnsOrThrow() const
    {
        if (!Columns) {
            THROW_ERROR_EXCEPTION("Missing \"columns\" attribute in schemaful DSV format");
        }
        return *Columns;
    }

    TSchemafulDsvFormatConfig()
    {
        RegisterParameter("record_separator", RecordSeparator)
            .Default('\n');
        RegisterParameter("field_separator", FieldSeparator)
            .Default('\t');

        RegisterParameter("enable_table_index", EnableTableIndex)
            .Default(false);

        RegisterParameter("enable_escaping", EnableEscaping)
            .Default(true);
        RegisterParameter("escaping_symbol", EscapingSymbol)
            .Default('\\');

        RegisterParameter("columns", Columns)
            .Default();

        RegisterParameter("missing_value_mode", MissingValueMode)
            .Default(EMissingSchemafulDsvValueMode::SkipRow);

        RegisterParameter("missing_value_sentinel", MissingValueSentinel)
            .Default("");

        RegisterValidator([&] () {
            if (Columns) {
                yhash_set<Stroka> names;
                for (const auto& name : *Columns) {
                    if (!names.insert(name).second) {
                        THROW_ERROR_EXCEPTION("Duplicate column name %Qv in schemaful DSV configuration",
                            name);
                    }
                }
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TSchemafulDsvFormatConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
