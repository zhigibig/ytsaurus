#pragma once

#include "public.h"

#include <yt/yt/ytlib/chunk_client/public.h>

#include <yt/yt/core/ytree/yson_struct.h>

#include <yt/yt/core/compression/public.h>

#include <yt/yt/library/erasure/public.h>

namespace NYT::NCypressServer {

////////////////////////////////////////////////////////////////////////////////

class TCypressManagerConfig
    : public NYTree::TYsonStruct
{
public:
    int DefaultFileReplicationFactor;

    int DefaultTableReplicationFactor;

    NErasure::ECodec DefaultJournalErasureCodec;
    int DefaultJournalReplicationFactor;
    int DefaultJournalReadQuorum;
    int DefaultJournalWriteQuorum;

    NErasure::ECodec DefaultHunkStorageErasureCodec;
    int DefaultHunkStorageReplicationFactor;
    int DefaultHunkStorageReadQuorum;
    int DefaultHunkStorageWriteQuorum;

    REGISTER_YSON_STRUCT(TCypressManagerConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TCypressManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TDynamicCypressManagerConfig
    : public NYTree::TYsonStruct
{
public:
    //! Period between Cypress access statistics commits.
    TDuration StatisticsFlushPeriod;

    //! Maximum number of children map and list nodes are allowed to contain.
    int MaxNodeChildCount;

    //! Maximum allowed length of string nodes.
    int MaxStringNodeLength;

    //! Maximum allowed size of custom attributes for objects (transactions, Cypress nodes etc).
    //! This limit concerns the binary YSON representation of attributes.
    int MaxAttributeSize;

    //! Maximum allowed length of keys in map nodes.
    int MaxMapNodeKeyLength;

    TDuration ExpirationCheckPeriod;
    int MaxExpiredNodesRemovalsPerCommit;
    TDuration ExpirationBackoffTime;

    NCompression::ECodec TreeSerializationCodec;

    // COMPAT(ignat)
    //! Forbids performing set inside Cypress.
    bool ForbidSetCommand;

    TDuration RecursiveResourceUsageCacheExpirationTimeout;

    double DefaultExternalCellBias;

    TDuration PortalSynchronizationPeriod;

    // COMPAT(kvk1920)
    bool EnablePortalSynchronization;

    // COMPAT(kvk1920)
    bool EnableRevisionChangingForBuiltinAttributes;

    bool EnableSymlinkCyclicityCheck;

    // COMPAT(shakurov)
    bool AllowCrossShardDynamicTableCopying;

    REGISTER_YSON_STRUCT(TDynamicCypressManagerConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TDynamicCypressManagerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCypressServer
