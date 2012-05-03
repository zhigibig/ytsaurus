#include "arguments.h"
#include "preprocess.h"

#include <ytlib/ytree/tokenizer.h>

#include <build.h>

#include <ytlib/job_proxy/config.h>

namespace NYT {

using namespace NYTree;
using namespace NScheduler;

////////////////////////////////////////////////////////////////////////////////

TArgsParserBase::TArgsParserBase()
    : CmdLine("Command line", ' ', YT_VERSION)
    , ConfigArg("", "config", "configuration file", false, "", "file_name")
    , OutputFormatArg("", "format", "output format", false, TFormat(), "text, pretty, binary")
    , ConfigUpdatesArg("", "config_set", "set configuration value", false, "ypath=yson")
    , OptsArg("", "opts", "other options", false, "key=yson")
{
    CmdLine.add(ConfigArg);
    CmdLine.add(OptsArg);
    CmdLine.add(OutputFormatArg);
    CmdLine.add(ConfigUpdatesArg);
}

void TArgsParserBase::Parse(std::vector<std::string>& args)
{
    CmdLine.parse(args);
}

INodePtr TArgsParserBase::GetCommand()
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    builder->OnBeginMap();
    BuildCommand(~builder);
    builder->OnEndMap();
    return builder->EndTree();
}

Stroka TArgsParserBase::GetConfigName()
{
    return Stroka(ConfigArg.getValue());
}

TArgsParserBase::TFormat TArgsParserBase::GetOutputFormat()
{
    return OutputFormatArg.getValue();
}

void TArgsParserBase::ApplyConfigUpdates(IYPathServicePtr service)
{
    FOREACH (auto updateString, ConfigUpdatesArg.getValue()) {
        TTokenizer tokenizer(updateString);
        tokenizer.ParseNext();
        while (tokenizer.GetCurrentType() != ETokenType::Equals) {
            if (!tokenizer.ParseNext()) {
                ythrow yexception() << "Incorrect option";
            }
        }
        TStringBuf ypath = TStringBuf(updateString).Chop(tokenizer.GetCurrentInput().length());
        SyncYPathSet(service, TYPath(ypath), TYson(tokenizer.GetCurrentSuffix()));
    }
}

void TArgsParserBase::BuildOptions(IYsonConsumer* consumer)
{
    // TODO(babenko): think about a better way of doing this
    FOREACH (const auto& opts, OptsArg.getValue()) {
        TYson yson = Stroka("{") + Stroka(opts) + "}";
        auto items = DeserializeFromYson(yson)->AsMap();
        FOREACH (const auto& pair, items->GetChildren()) {
            consumer->OnKeyedItem(pair.first);
            VisitTree(pair.second, consumer, true);
        }
    }
}

void TArgsParserBase::BuildCommand(IYsonConsumer* consumer)
{ }

////////////////////////////////////////////////////////////////////////////////

TTransactedArgsParser::TTransactedArgsParser()
    : TxArg("", "tx", "set transaction id", false, "", "transaction_id")
{
    CmdLine.add(TxArg);
}

void TTransactedArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    BuildYsonMapFluently(consumer)
        .DoIf(TxArg.isSet(), [=] (TFluentMap fluent) {
            TYson txYson = TxArg.getValue();
            ValidateYson(txYson);
            fluent.Item("transaction_id").Node(txYson);
        });

    TArgsParserBase::BuildCommand(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TGetArgsParser::TGetArgsParser()
    : PathArg("path", "path to an object in Cypress that must be retrieved", true, "", "path")
{
    CmdLine.add(PathArg);
}

void TGetArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    auto path = PreprocessYPath(PathArg.getValue());

    BuildYsonMapFluently(consumer)
        .Item("path").Scalar(path);

    TTransactedArgsParser::BuildCommand(consumer);
    BuildOptions(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TSetArgsParser::TSetArgsParser()
    : PathArg("path", "path to an object in Cypress that must be set", true, "", "path")
    , ValueArg("value", "value to set", true, "", "yson")
{
    CmdLine.add(PathArg);
    CmdLine.add(ValueArg);
}

void TSetArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    auto path = PreprocessYPath(PathArg.getValue());

    BuildYsonMapFluently(consumer)
        .Item("path").Scalar(path)
        .Item("value").Node(ValueArg.getValue());

    TTransactedArgsParser::BuildCommand(consumer);
    BuildOptions(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TRemoveArgsParser::TRemoveArgsParser()
    : PathArg("path", "path to an object in Cypress that must be removed", true, "", "path")
{
    CmdLine.add(PathArg);
}

void TRemoveArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    auto path = PreprocessYPath(PathArg.getValue());

    BuildYsonMapFluently(consumer)
        .Item("path").Scalar(path);

    TTransactedArgsParser::BuildCommand(consumer);
    BuildOptions(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TListArgsParser::TListArgsParser()
    : PathArg("path", "path to a object in Cypress whose children must be listed", true, "", "path")
{
    CmdLine.add(PathArg);
}

void TListArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    auto path = PreprocessYPath(PathArg.getValue());

    BuildYsonMapFluently(consumer)
        .Item("path").Scalar(path);
 
    TTransactedArgsParser::BuildCommand(consumer);
    BuildOptions(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TCreateArgsParser::TCreateArgsParser()
    : TypeArg("type", "type of node", true, NObjectServer::EObjectType::Null, "object type")
    , PathArg("path", "path for a new object in Cypress", true, "", "ypath")
{
    CmdLine.add(TypeArg);
    CmdLine.add(PathArg);
}

void TCreateArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    auto path = PreprocessYPath(PathArg.getValue());

    BuildYsonMapFluently(consumer)
        .Item("path").Scalar(path)
        .Item("type").Scalar(TypeArg.getValue().ToString());

    TTransactedArgsParser::BuildCommand(consumer);
    BuildOptions(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TLockArgsParser::TLockArgsParser()
    : PathArg("path", "path to an object in Cypress that must be locked", true, "", "path")
    , ModeArg("", "mode", "lock mode", false, NCypress::ELockMode::Exclusive, "snapshot, shared, exclusive")
{
    CmdLine.add(PathArg);
    CmdLine.add(ModeArg);
}

void TLockArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    auto path = PreprocessYPath(PathArg.getValue());

    BuildYsonMapFluently(consumer)
        .Item("path").Scalar(path)
        .Item("mode").Scalar(ModeArg.getValue().ToString());

    TTransactedArgsParser::BuildCommand(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TStartTxArgsParser::TStartTxArgsParser()
{ }

void TStartTxArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    TArgsParserBase::BuildCommand(consumer);
    BuildOptions(consumer);
}

////////////////////////////////////////////////////////////////////////////////

void TCommitTxArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    TTransactedArgsParser::BuildCommand(consumer);
}

////////////////////////////////////////////////////////////////////////////////

void TAbortTxArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    TTransactedArgsParser::BuildCommand(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TReadArgsParser::TReadArgsParser()
    : PathArg("path", "path to a table in Cypress that must be read", true, "", "ypath")
{
    CmdLine.add(PathArg);
}

void TReadArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    auto path = PreprocessYPath(PathArg.getValue());

    BuildYsonMapFluently(consumer)
        .Item("do").Scalar("read")
        .Item("path").Scalar(path);

    TTransactedArgsParser::BuildCommand(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TWriteArgsParser::TWriteArgsParser()
    : PathArg("path", "path to a table in Cypress that must be written", true, "", "ypath")
    , ValueArg("value", "row(s) to write", false, "", "yson")
    , KeyColumnsArg("", "sorted", "key columns names (table must initially be empty, input data must be sorted)", false, "", "list_fragment")
{
    CmdLine.add(PathArg);
    CmdLine.add(ValueArg);
    CmdLine.add(KeyColumnsArg);
}

void TWriteArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    auto path = PreprocessYPath(PathArg.getValue());
    auto value = ValueArg.getValue();
    // TODO(babenko): refactor
    auto keyColumns = DeserializeFromYson< yvector<Stroka> >("[" + KeyColumnsArg.getValue() + "]");

    BuildYsonMapFluently(consumer)
        .Item("do").Scalar("write")
        .Item("path").Scalar(path)
        .DoIf(!keyColumns.empty(), [=] (TFluentMap fluent) {
            fluent.Item("sorted").Scalar(true);
            fluent.Item("key_columns").List(keyColumns);
        })
        .DoIf(!value.empty(), [=] (TFluentMap fluent) {
                fluent.Item("value").Node(value);
        });

    TTransactedArgsParser::BuildCommand(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TUploadArgsParser::TUploadArgsParser()
    : PathArg("path", "to a new file in Cypress that must be uploaded", true, "", "ypath")
{
    CmdLine.add(PathArg);
}

void TUploadArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    auto path = PreprocessYPath(PathArg.getValue());

    BuildYsonMapFluently(consumer)
        .Item("path").Scalar(path);

    TTransactedArgsParser::BuildCommand(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TDownloadArgsParser::TDownloadArgsParser()
    : PathArg("path", "path to a file in Cypress that must be downloaded", true, "", "ypath")
{
    CmdLine.add(PathArg);
}

void TDownloadArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    auto path = PreprocessYPath(PathArg.getValue());

    BuildYsonMapFluently(consumer)
        .Item("path").Scalar(path);

    TTransactedArgsParser::BuildCommand(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TMapArgsParser::TMapArgsParser()
    : InArg("", "in", "input tables", false, "ypath")
    , OutArg("", "out", "output tables", false, "ypath")
    , FilesArg("", "file", "additional files", false, "ypath")
    , MapperArg("", "mapper", "mapper shell command", true, "", "command")
{
    CmdLine.add(InArg);
    CmdLine.add(OutArg);
    CmdLine.add(FilesArg);
    CmdLine.add(MapperArg);
}

void TMapArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    auto input = PreprocessYPaths(InArg.getValue());
    auto output = PreprocessYPaths(OutArg.getValue());
    auto files = PreprocessYPaths(FilesArg.getValue());

    BuildYsonMapFluently(consumer)
        .Item("spec").BeginMap()
            .Item("mapper").Scalar(MapperArg.getValue())
            .Item("input_table_paths").List(input)
            .Item("output_table_paths").List(output)
            .Item("files").List(files)
            .Do(BIND(&TMapArgsParser::BuildOptions, Unretained(this)))
        .EndMap();

    TTransactedArgsParser::BuildCommand(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TMergeArgsParser::TMergeArgsParser()
    : InArg("", "in", "input tables", false, "ypath")
    , OutArg("", "out", "output table", false, "", "ypath")
    , ModeArg("", "mode", "merge mode", false, TMode(EMergeMode::Unordered), "unordered, ordered, sorted")
    , CombineArg("", "combine", "combine small output chunks into larger ones")
{
    CmdLine.add(InArg);
    CmdLine.add(OutArg);
    CmdLine.add(ModeArg);
    CmdLine.add(CombineArg);
}

void TMergeArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    auto input = PreprocessYPaths(InArg.getValue());
    auto output = PreprocessYPath(OutArg.getValue());

    BuildYsonMapFluently(consumer)
        .Item("spec").BeginMap()
            .Item("input_table_paths").List(input)
            .Item("output_table_path").Scalar(output)
            .Item("mode").Scalar(FormatEnum(ModeArg.getValue().Get()))
            .Item("combine_chunks").Scalar(CombineArg.getValue())
            .Do(BIND(&TMergeArgsParser::BuildOptions, Unretained(this)))
        .EndMap();

    TTransactedArgsParser::BuildCommand(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TSortArgsParser::TSortArgsParser()
    : InArg("", "in", "input tables", false, "ypath")
    , OutArg("", "out", "output table", false, "", "ypath")
    , KeyColumnsArg("", "key_columns", "key columns names", true, "", "list_fragment")
{
    CmdLine.add(InArg);
    CmdLine.add(OutArg);
    CmdLine.add(KeyColumnsArg);
}

void TSortArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    auto input = PreprocessYPaths(InArg.getValue());
    auto output = PreprocessYPath(OutArg.getValue());
    // TODO(babenko): refactor
    auto keyColumns = DeserializeFromYson< yvector<Stroka> >("[" + KeyColumnsArg.getValue() + "]");

    BuildYsonMapFluently(consumer)
        .Item("spec").BeginMap()
            .Item("input_table_paths").List(input)
            .Item("output_table_path").Scalar(output)
            .Item("key_columns").List(keyColumns)
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

TEraseArgsParser::TEraseArgsParser()
    : InArg("", "in", "input table", false, "", "ypath")
    , OutArg("", "out", "output table", false, "", "ypath")
    , CombineArg("", "combine", "combine small output chunks into larger ones")
{
    CmdLine.add(InArg);
    CmdLine.add(OutArg);
    CmdLine.add(CombineArg);
}

void TEraseArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    auto input = PreprocessYPath(InArg.getValue());
    auto output = PreprocessYPath(OutArg.getValue());

    BuildYsonMapFluently(consumer)
        .Item("spec").BeginMap()
            .Item("input_table_path").Scalar(input)
            .Item("output_table_path").Scalar(output)
            .Item("combine_chunks").Scalar(CombineArg.getValue())
            .Do(BIND(&TEraseArgsParser::BuildOptions, Unretained(this)))
        .EndMap();

    TTransactedArgsParser::BuildCommand(consumer);
}

////////////////////////////////////////////////////////////////////////////////

TAbortOpArgsParser::TAbortOpArgsParser()
    : OpArg("", "op", "id of an operation that must be aborted", true, "", "operation_id")
{
    CmdLine.add(OpArg);
}

void TAbortOpArgsParser::BuildCommand(IYsonConsumer* consumer)
{
    BuildYsonMapFluently(consumer)
        .Item("operation_id").Scalar(OpArg.getValue());

    TArgsParserBase::BuildCommand(consumer);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
