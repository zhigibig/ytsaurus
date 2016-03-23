#pragma once

#include "public.h"

#include <yt/core/yson/public.h>

#include <yt/core/ytree/attributes.h>

#include <yt/ytlib/table_client/schema.h>

#include <yt/ytlib/chunk_client/read_limit.h>

namespace NYT {
namespace NYPath {

////////////////////////////////////////////////////////////////////////////////

//! YPath string plus attributes.
class TRichYPath
{
public:
    TRichYPath();
    TRichYPath(const TRichYPath& other);
    TRichYPath(TRichYPath&& other);
    TRichYPath(const char* path);
    TRichYPath(const TYPath& path);
    TRichYPath(const TYPath& path, const NYTree::IAttributeDictionary& attributes);
    TRichYPath& operator = (const TRichYPath& other);

    static TRichYPath Parse(const Stroka& str);
    TRichYPath Normalize() const;

    const TYPath& GetPath() const;
    void SetPath(const TYPath& path);

    const NYTree::IAttributeDictionary& Attributes() const;
    NYTree::IAttributeDictionary& Attributes();

    void Save(TStreamSaveContext& context) const;
    void Load(TStreamLoadContext& context);

    // Attribute accessors.
    // "append"
    bool GetAppend() const;
    void SetAppend(bool value);

    // "teleport"
    bool GetTeleport() const;

    // "primary"
    bool GetPrimary() const;

    // "foreign"
    bool GetForeign() const;

    // "channel"
    NChunkClient::TChannel GetChannel() const;

    // "ranges"
    // COMPAT(ignat): also "lower_limit" and "upper_limit"
    std::vector<NChunkClient::TReadRange> GetRanges() const;
    void SetRanges(const std::vector<NChunkClient::TReadRange>& value);

    // "file_name"
    TNullable<Stroka> GetFileName() const;

    // "executable"
    TNullable<bool> GetExecutable() const;

    // "format"
    TNullable<NYson::TYsonString> GetFormat() const;

    // "schema"
    TNullable<NTableClient::TTableSchema> GetSchema() const;

    // "sorted_by"
    NTableClient::TKeyColumns GetSortedBy() const;
    void SetSortedBy(const NTableClient::TKeyColumns& value);

    // "row_count_limit"
    TNullable<i64> GetRowCountLimit() const;

private:
    TYPath Path_;
    std::unique_ptr<NYTree::IAttributeDictionary> Attributes_;
};

bool operator== (const TRichYPath& lhs, const TRichYPath& rhs);

////////////////////////////////////////////////////////////////////////////////

Stroka ToString(const TRichYPath& path);

std::vector<TRichYPath> Normalize(const std::vector<TRichYPath>& paths);

void InitializeFetchRequest(
    NChunkClient::NProto::TReqFetch* request,
    const TRichYPath& richPath);

void Serialize(const TRichYPath& richPath, NYson::IYsonConsumer* consumer);
void Deserialize(TRichYPath& richPath, NYTree::INodePtr node);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYPath
} // namespace NYT
