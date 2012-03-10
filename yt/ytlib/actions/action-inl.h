#ifndef ACTION_INL_H_
#error "Direct inclusion of this file is not allowed, include action.h"
#endif

#include "future.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class TResult>
struct TAsyncFuncTraits
{
    typedef TIntrusivePtr< TFuture<TResult> > TAsync;

    static void InnerThunk(
        TIntrusivePtr< IFunc<TResult> > func,
        TAsync result)
    {
        result->Set(func->Do());
    }

    static TAsync OuterThunk(
        TIntrusivePtr< IFunc<TResult> > func,
        TIntrusivePtr<IInvoker> invoker)
    {
        TAsync result = New< TFuture<TResult> >();
        invoker->Invoke(FromMethod(&InnerThunk, func, result));
        return result;
    }
};

template <class TResult>
struct TAsyncFuncTraits< TIntrusivePtr< TFuture<TResult> > >
{
    typedef TIntrusivePtr< TFuture<TResult> > TAsync;

    static void InnerThunk(
        TIntrusivePtr< IFunc<TAsync> > func,
        TAsync result)
    {
        func->Do()->Subscribe(FromMethod(
            &TFuture<TResult>::Set, result));
    }

    static TAsync OuterThunk(
        TIntrusivePtr< IFunc<TAsync> > func,
        TIntrusivePtr<IInvoker> invoker)
    {
        TAsync result = New< TFuture<TResult> >();
        invoker->Invoke(FromMethod(&InnerThunk, func, result));
        return result;
    }
};

template <class TResult>
TIntrusivePtr< IFunc <typename TAsyncTraits<TResult>::TAsync> >
IFunc<TResult>::AsyncVia(TIntrusivePtr<IInvoker> invoker)
{
    return FromMethod(
        &TAsyncFuncTraits<TResult>::OuterThunk,
        MakeStrong(this),
        invoker);
}

////////////////////////////////////////////////////////////////////////////////

template <class TParam>
IAction::TPtr IParamAction<TParam>::Bind(TParam param)
{
    return FromMethod(
        &IParamAction<TParam>::Do,
        MakeStrong(this),
        param);
}

template <class TParam>
void ParamActionViaThunk(
    TParam param,
    typename IParamAction<TParam>::TPtr paramAction,
    TIntrusivePtr< IInvoker > invoker)
{
    invoker->Invoke(paramAction->Bind(param));
}

template <class TParam>
typename IParamAction<TParam>::TPtr IParamAction<TParam>::Via(
    TIntrusivePtr< IInvoker > invoker)
{
    return FromMethod(
        &ParamActionViaThunk<TParam>,
        MakeStrong(this),
        invoker);
}

template <class TParam>
void ParamActionViaThunk(TParam param, IAction::TPtr action)
{
    UNUSED(param);
    action->Do();
}

////////////////////////////////////////////////////////////////////////////////

template <class TParam, class TResult>
struct TAsyncParamFuncTraits
{
    typedef TIntrusivePtr< TFuture<TResult> > TAsync;

    static void InnerThunk(
        TParam param,
        TIntrusivePtr< IParamFunc<TParam, TResult> > func,
        TAsync result)
    {
        result->Set(func->Do(param));
    }

    static TAsync OuterThunk(
        TParam param,
        TIntrusivePtr< IParamFunc<TParam, TResult> > func,
        TIntrusivePtr<IInvoker> invoker)
    {
        TAsync result = New< TFuture<TResult> >();
        invoker->Invoke(FromMethod(&InnerThunk, param, func, result));
        return result;
    }
};

template <class TParam, class TResult>
struct TAsyncParamFuncTraits< TParam, TIntrusivePtr< TFuture<TResult> > >
{
    typedef TIntrusivePtr< TFuture<TResult> > TAsync;

    static void InnerThunk(
        TParam param,
        TIntrusivePtr< IParamFunc<TParam, TAsync> > func,
        TAsync result)
    {
        func->Do(param)->Subscribe(FromMethod(
            &TFuture<TResult>::Set,
            result));
    }

    static TAsync OuterThunk(
        TParam param,
        TIntrusivePtr< IParamFunc<TParam, TAsync> > func,
        TIntrusivePtr<IInvoker> invoker)
    {
        TAsync result = New< TFuture<TResult> >();
        invoker->Invoke(FromMethod(&InnerThunk, param, func, result));
        return result;
    }
};

template <class TParam, class TResult>
TIntrusivePtr< IParamFunc<TParam, typename TAsyncTraits<TResult>::TAsync> >
IParamFunc<TParam, TResult>::AsyncVia(TIntrusivePtr<IInvoker> invoker)
{
    return FromMethod(
        &TAsyncParamFuncTraits<TParam, TResult>::OuterThunk,
        MakeStrong(this),
        invoker);
}

////////////////////////////////////////////////////////////////////////////////

template <class TParam>
typename IParamAction<TParam>::TPtr IAction::ToParamAction()
{
    return FromMethod(&ParamActionViaThunk<TParam>, MakeStrong(this));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
