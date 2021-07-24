#pragma once

#include "public.h"

#include <yt/yt/client/api/config.h>

#include <yt/yt/client/chunk_client/config.h>

#include <yt/yt/client/table_client/config.h>

#include <yt/yt/client/transaction_client/config.h>

#include <yt/yt/core/rpc/retrying_channel.h>

#include <yt/yt/core/ytree/yson_serializable.h>

#include <yt/yt/core/misc/cache_config.h>

namespace NYT::NDriver {

////////////////////////////////////////////////////////////////////////////////

constexpr int ApiVersion3 = 3;
constexpr int ApiVersion4 = 4;

class TDriverConfig
    : public NYTree::TYsonSerializable
{
public:
    NApi::TFileReaderConfigPtr FileReader;
    NApi::TFileWriterConfigPtr FileWriter;
    NTableClient::TTableReaderConfigPtr TableReader;
    NTableClient::TTableWriterConfigPtr TableWriter;
    NApi::TJournalReaderConfigPtr JournalReader;
    NApi::TJournalWriterConfigPtr JournalWriter;
    NChunkClient::TFetcherConfigPtr Fetcher;
    int ApiVersion;

    i64 ReadBufferRowCount;
    i64 ReadBufferSize;
    i64 WriteBufferSize;

    TSlruCacheConfigPtr ClientCache;

    std::optional<TString> Token;

    TAsyncExpiringCacheConfigPtr ProxyDiscoveryCache;

    TDriverConfig()
    {
        RegisterParameter("file_reader", FileReader)
            .DefaultNew();
        RegisterParameter("file_writer", FileWriter)
            .DefaultNew();
        RegisterParameter("table_reader", TableReader)
            .DefaultNew();
        RegisterParameter("table_writer", TableWriter)
            .DefaultNew();
        RegisterParameter("journal_reader", JournalReader)
            .DefaultNew();
        RegisterParameter("journal_writer", JournalWriter)
            .DefaultNew();
        RegisterParameter("fetcher", Fetcher)
            .DefaultNew();

        RegisterParameter("read_buffer_row_count", ReadBufferRowCount)
            .Default((i64) 10000);
        RegisterParameter("read_buffer_size", ReadBufferSize)
            .Default((i64) 1 * 1024 * 1024);
        RegisterParameter("write_buffer_size", WriteBufferSize)
            .Default((i64) 1 * 1024 * 1024);

        RegisterParameter("client_cache", ClientCache)
            .DefaultNew(1024 * 1024);

        RegisterParameter("api_version", ApiVersion)
            .Default(ApiVersion3)
            .GreaterThanOrEqual(ApiVersion3)
            .LessThanOrEqual(ApiVersion4);

        RegisterParameter("token", Token)
            .Optional();

        RegisterParameter("proxy_discovery_cache", ProxyDiscoveryCache)
            .DefaultNew();

        RegisterPreprocessor([&] {
            ProxyDiscoveryCache->RefreshTime = TDuration::Seconds(15);
            ProxyDiscoveryCache->ExpireAfterSuccessfulUpdateTime = TDuration::Seconds(15);
            ProxyDiscoveryCache->ExpireAfterFailedUpdateTime = TDuration::Seconds(15);
        });

        RegisterPostprocessor([&] {
            if (ApiVersion != ApiVersion3 && ApiVersion != ApiVersion4) {
                THROW_ERROR_EXCEPTION("Unsupported API version %v",
                    ApiVersion);
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TDriverConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDriver

