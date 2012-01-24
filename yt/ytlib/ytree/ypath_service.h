#pragma once

#include "common.h"
#include "ytree_fwd.h"
#include "yson_consumer.h"

#include <ytlib/misc/property.h>
#include <ytlib/rpc/service.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(EYPathErrorCode,
    ((ResolveError)(1))
);

////////////////////////////////////////////////////////////////////////////////

//! Represents an abstract way of handling YPath requests.
/*!
 *  To handle a given YPath request one must first resolve the target.
 *  
 *  We start with some root service and call #Resolve. The latter either replies "here", in which case
 *  the resolution is finished, or "there", in which case a new candidate target is provided.
 *  At each resolution step the current path may be altered by specifying a new one
 *  as a part of the result.
 *  
 *  Once the request is resolved, #Invoke is called for the target service.
 */
struct IYPathService
    : virtual TRefCounted
{
    typedef TIntrusivePtr<IYPathService> TPtr;

    class TResolveResult
    {
        DEFINE_BYVAL_RO_PROPERTY(IYPathService::TPtr, Service);
        DEFINE_BYVAL_RO_PROPERTY(TYPath, Path);

    public:
        //! Creates a result indicating that resolution is finished.
        static TResolveResult Here(const TYPath& path)
        {
            TResolveResult result;
            result.Path_ = path;
            return result;
        }

        //! Creates a result indicating that resolution must proceed.
        static TResolveResult There(IYPathService* service, const TYPath& path)
        {
            YASSERT(service);

            TResolveResult result;
            result.Service_ = service;
            result.Path_ = path;
            return result;
        }

        //! Returns true iff the resolution is finished.
        bool IsHere() const
        {
            return !Service_;
        }
    };

    //! Resolves the given path by either returning "here" or "there" result.
    virtual TResolveResult Resolve(const TYPath& path, const Stroka& verb) = 0;

    //! Executes a given request.
    virtual void Invoke(NRpc::IServiceContext* context) = 0;

    //! Called for the target service and
    //! returns the logging category that will be used by RPC infrastructure
    //! to log various details about verb invocation (e.g. request and response infos).
    virtual Stroka GetLoggingCategory() const = 0;

    //! Called for the target service and
    //! returns true if the request may mutate target's state.
    /*!
     *  There are at least two scenarios when this call makes sense:
     *  - Checking if we need to log the request to be able to replay it during recovery.
     *  - Checking if the request modifies a mapped YSON file, so we need
     *    to write it back one the processing is finished.
     */
    virtual bool IsWriteRequest(NRpc::IServiceContext* context) const = 0;

    //! Creates a YPath service from a YSON producer.
    /*!
     *  Constructs an ephemeral tree from #producer and returns its root.
     */
    static IYPathService::TPtr FromProducer(TYsonProducer* producer);
};

typedef IFunc<NYTree::IYPathService::TPtr> TYPathServiceProvider;

////////////////////////////////////////////////////////////////////////////////

struct IYPathProcessor
    : public virtual TRefCounted
{
    typedef TIntrusivePtr<IYPathProcessor> TPtr;

    virtual void Resolve(
        const TYPath& path,
        const Stroka& verb,
        IYPathService::TPtr* suffixService,
        TYPath* suffixPath) = 0;

    virtual void Execute(
        IYPathService* service,
        NRpc::IServiceContext* context) = 0;
};

IYPathProcessor::TPtr CreateDefaultProcessor(IYPathService* rootService);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
