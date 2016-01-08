#include "ypath_service.h"
#include "convert.h"
#include "ephemeral_node_factory.h"
#include "tree_builder.h"
#include "ypath_client.h"
#include "ypath_detail.h"

#include <yt/core/rpc/dispatcher.h>

#include <yt/core/yson/async_consumer.h>
#include <yt/core/yson/attribute_consumer.h>
#include <yt/core/yson/writer.h>

#include <yt/core/concurrency/scheduler.h>
#include <yt/core/concurrency/periodic_executor.h>

namespace NYT {
namespace NYTree {

using namespace NYson;
using namespace NRpc;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

TAttributeFilter TAttributeFilter::All(EAttributeFilterMode::All, std::vector<Stroka>());
TAttributeFilter TAttributeFilter::None(EAttributeFilterMode::None, std::vector<Stroka>());

TAttributeFilter::TAttributeFilter()
    : Mode(EAttributeFilterMode::None)
{ }

TAttributeFilter::TAttributeFilter(
    EAttributeFilterMode mode,
    const std::vector<Stroka>& keys)
    : Mode(mode)
    , Keys(keys)
{ }

TAttributeFilter::TAttributeFilter(EAttributeFilterMode mode)
    : Mode(mode)
{ }

void ToProto(NProto::TAttributeFilter* protoFilter, const TAttributeFilter& filter)
{
    protoFilter->set_mode(static_cast<int>(filter.Mode));
    for (const auto& key : filter.Keys) {
        protoFilter->add_keys(key);
    }
}

void FromProto(TAttributeFilter* filter, const NProto::TAttributeFilter& protoFilter)
{
    *filter = TAttributeFilter(
        EAttributeFilterMode(protoFilter.mode()),
        NYT::FromProto<Stroka>(protoFilter.keys()));
}

////////////////////////////////////////////////////////////////////////////////

class TFromProducerYPathService
    : public TYPathServiceBase
    , public TSupportsGet
{
public:
    explicit TFromProducerYPathService(TYsonProducer producer)
        : Producer_(producer)
    { }

    virtual TResolveResult Resolve(
        const TYPath& path,
        IServiceContextPtr context) override
    {
        // Try to handle root get requests without constructing ephemeral YTree.
        if (path.empty() && context->GetMethod() == "Get") {
            return TResolveResult::Here(path);
        } else {
            auto node = BuildNodeFromProducer();
            return TResolveResult::There(node, path);
        }
    }

private:
    const TYsonProducer Producer_;


    virtual bool DoInvoke(IServiceContextPtr context) override
    {
        DISPATCH_YPATH_SERVICE_METHOD(Get);
        return TYPathServiceBase::DoInvoke(context);
    }

    virtual void GetSelf(TReqGet* request, TRspGet* response, TCtxGetPtr context) override
    {
        bool ignoreOpaque = request->ignore_opaque();
        auto mode = EAttributeFilterMode(request->attribute_filter().mode());
        if (!ignoreOpaque || mode != EAttributeFilterMode::All)  {
            // Execute fallback.
            auto node = BuildNodeFromProducer();
            ExecuteVerb(node, IServiceContextPtr(context));
            return;
        }

        Stroka result;
        TStringOutput stream(result);
        TYsonWriter writer(&stream, EYsonFormat::Binary, EYsonType::Node, true);
        Producer_.Run(&writer);

        response->set_value(result);
        context->Reply();
    }

    virtual void GetRecursive(const TYPath& /*path*/, TReqGet* /*request*/, TRspGet* /*response*/, TCtxGetPtr /*context*/) override
    {
        YUNREACHABLE();
    }

    virtual void GetAttribute(const TYPath& /*path*/, TReqGet* /*request*/, TRspGet* /*response*/, TCtxGetPtr /*context*/) override
    {
        YUNREACHABLE();
    }


    INodePtr BuildNodeFromProducer()
    {
        return ConvertTo<INodePtr>(Producer_);
    }

};

IYPathServicePtr IYPathService::FromProducer(TYsonProducer producer)
{
    return New<TFromProducerYPathService>(producer);
}

////////////////////////////////////////////////////////////////////////////////

class TViaYPathService
    : public TYPathServiceBase
{
public:
    TViaYPathService(
        IYPathServicePtr underlyingService,
        IInvokerPtr invoker)
        : UnderlyingService_(underlyingService)
        , Invoker_(invoker)
    { }

    virtual TResolveResult Resolve(
        const TYPath& path,
        IServiceContextPtr /*context*/) override
    {
        return TResolveResult::Here(path);
    }

private:
    const IYPathServicePtr UnderlyingService_;
    const IInvokerPtr Invoker_;


    virtual bool DoInvoke(IServiceContextPtr context) override
    {
        Invoker_->Invoke(BIND([=, this_ = MakeStrong(this)] () {
            ExecuteVerb(UnderlyingService_, context);
        }));
        return true;
    }
};

IYPathServicePtr IYPathService::Via(IInvokerPtr invoker)
{
    return New<TViaYPathService>(this, invoker);
}

////////////////////////////////////////////////////////////////////////////////

class TCachedYPathService
    : public TYPathServiceBase
{
public:
    TCachedYPathService(
        IYPathServicePtr underlyingService,
        TDuration updatePeriod)
        : UnderlyingService_(std::move(underlyingService))
        , PeriodicExecutor_(New<TPeriodicExecutor>(
            GetWorkerInvoker(),
            BIND(&TCachedYPathService::RebuildCache, MakeWeak(this)),
            updatePeriod))
    {
        YCHECK(UnderlyingService_);
        PeriodicExecutor_->Start();
    }
    
    virtual TResolveResult Resolve(const TYPath& path, IServiceContextPtr /*context*/) override
    {
        return TResolveResult::Here(path);
    }

private:
    const IYPathServicePtr UnderlyingService_;
    const TPeriodicExecutorPtr PeriodicExecutor_;

    TSpinLock SpinLock_;
    TErrorOr<INodePtr> CachedTreeOrError_;


    virtual bool DoInvoke(IServiceContextPtr context) override
    {
        GetWorkerInvoker()->Invoke(BIND([=, this_ = MakeStrong(this)] () {
            try {
                auto cachedTreeOrError = GetCachedTree();
                auto cachedTree = cachedTreeOrError.ValueOrThrow();
                ExecuteVerb(cachedTree, context);
            } catch (const std::exception& ex) {
                context->Reply(ex);
            }
        }));
        return true;
    }

    void RebuildCache()
    {
        try {
            auto asyncYson = AsyncYPathGet(
                UnderlyingService_,
                TYPath(),
                TAttributeFilter::All,
                true);

            auto yson = WaitFor(asyncYson)
                .ValueOrThrow();

            auto node = ConvertToNode(yson);

            SetCachedTree(node);
        } catch (const std::exception& ex) {
            SetCachedTree(TError(ex));
        }
    }


    TErrorOr<INodePtr> GetCachedTree()
    {
        TGuard<TSpinLock> guard(SpinLock_);
        return CachedTreeOrError_;
    }

    void SetCachedTree(const TErrorOr<INodePtr>& cachedTreeOrError)
    {
        TGuard<TSpinLock> guard(SpinLock_);
        CachedTreeOrError_ = cachedTreeOrError;
    }


    static IInvokerPtr GetWorkerInvoker()
    {
        return NRpc::TDispatcher::Get()->GetInvoker();
    }
};

IYPathServicePtr IYPathService::Cached(TDuration updatePeriod)
{
    return updatePeriod == TDuration::Zero()
        ? MakeStrong(this)
        : New<TCachedYPathService>(this, updatePeriod);
}

////////////////////////////////////////////////////////////////////////////////

void IYPathService::WriteAttributes(
    IAsyncYsonConsumer* consumer,
    const TAttributeFilter& filter,
    bool sortKeys)
{
    if (filter.Mode == EAttributeFilterMode::None)
        return;

    if (filter.Mode == EAttributeFilterMode::MatchingOnly && filter.Keys.empty())
        return;

    TAttributeFragmentConsumer attributesConsumer(consumer);
    WriteAttributesFragment(&attributesConsumer, filter, sortKeys);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
