#pragma once

#include <core/misc/common.h>

namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

class IConnection;
typedef TIntrusivePtr<IConnection> IConnectionPtr;

struct IDriver;
typedef TIntrusivePtr<IDriver> IDriverPtr;

struct TTableMountInfo;
typedef TIntrusivePtr<TTableMountInfo> TTableMountInfoPtr;

class TTableMountCache;
typedef TIntrusivePtr<TTableMountCache> TTableMountCachePtr;

class TTableMountCacheConfig;
typedef TIntrusivePtr<TTableMountCacheConfig> TTableMountCacheConfigPtr;

class TQueryCallbacksProvider;
typedef TIntrusivePtr<TQueryCallbacksProvider> TQueryCallbacksProviderPtr;

class TConnectionConfig;
typedef TIntrusivePtr<TConnectionConfig> TConnectionConfigPtr;

class TDriverConfig;
typedef TIntrusivePtr<TDriverConfig> TDriverConfigPtr;

struct TCommandDescriptor;

struct TDriverRequest;
struct TDriverResponse;

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
