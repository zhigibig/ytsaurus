#include "stdafx.h"
#include "monitoring_manager.h"

#include <ytlib/ytree/ephemeral_node_factory.h>
#include <ytlib/yson/writer.h>
#include <ytlib/ytree/tree_visitor.h>
#include <ytlib/ytree/ypath_proxy.h>
#include <ytlib/ytree/node.h>
#include <ytlib/ytree/convert.h>

#include <ytlib/actions/bind.h>

#include <ytlib/profiling/profiler.h>

namespace NYT {
namespace NMonitoring {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("Monitoring");
static NProfiling::TProfiler Profiler("/monitoring");

static const TDuration UpdatePeriod = TDuration::Seconds(3);

////////////////////////////////////////////////////////////////////////////////

TMonitoringManager::TMonitoringManager()
    : IsStarted(false)
    , ActionQueue(New<TActionQueue>("Monitoring"))
{ }

void TMonitoringManager::Register(const TYPath& path, TYsonProducer producer)
{
    TGuard<TSpinLock> guard(SpinLock);
    YCHECK(MonitoringMap.insert(std::make_pair(path, producer)).second);
}

void TMonitoringManager::Unregister(const TYPath& path)
{
    TGuard<TSpinLock> guard(SpinLock);
    YCHECK(MonitoringMap.erase(path) == 1);
}

INodePtr TMonitoringManager::GetRoot() const
{
    return Root;
}

void TMonitoringManager::Start()
{
    YASSERT(!IsStarted);

    // Create an empty root immediately to prevent GetRoot from returning NULL.
    Root = GetEphemeralNodeFactory()->CreateMap();

    PeriodicInvoker = New<TPeriodicInvoker>(
        ActionQueue->GetInvoker(),
        BIND(&TMonitoringManager::Update, MakeStrong(this)),
        UpdatePeriod);
    PeriodicInvoker->Start();

    IsStarted = true;
}

void TMonitoringManager::Stop()
{
    if (!IsStarted)
        return;

    IsStarted = false;
    PeriodicInvoker->Stop();
    Root.Reset();
}

void TMonitoringManager::Update()
{
    PROFILE_TIMING ("/update_time") {
        INodePtr newRoot = GetEphemeralNodeFactory()->CreateMap();

        FOREACH (const auto& pair, MonitoringMap) {
            TYsonString value = ConvertToYsonString(pair.second);
            SyncYPathSet(newRoot, pair.first, value);
        }

        if (IsStarted) {
            Root = newRoot;
        }
    }
}

void TMonitoringManager::Visit(NYson::IYsonConsumer* consumer)
{
    PROFILE_TIMING ("/visit_time") {
        VisitTree(GetRoot(), consumer);
    }
}

TYsonProducer TMonitoringManager::GetProducer()
{
    YASSERT(IsStarted);
    YASSERT(Root);

    return BIND(&TMonitoringManager::Visit, MakeStrong(this));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMonitoring
} // namespace NYT
