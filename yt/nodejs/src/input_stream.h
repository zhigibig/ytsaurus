#pragma once

#include "stream_base.h"

#include <deque>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

//! This class adheres to TInputStream interface as a C++ object and
//! simultaneously provides 'writable stream' interface stubs as a JS object
//! thus effectively acting as a bridge from JS to C++.
class TNodeJSInputStream
    : public TNodeJSStreamBase
    , public TInputStream
{
protected:
    TNodeJSInputStream(ui64 lowWatermark, ui64 highWatermark);
    ~TNodeJSInputStream() throw();

public:
    static v8::Persistent<v8::FunctionTemplate> ConstructorTemplate;
    static void Initialize(v8::Handle<v8::Object> target);
    static bool HasInstance(v8::Handle<v8::Value> value);

    // Synchronous JS API.
    static v8::Handle<v8::Value> New(const v8::Arguments& args);

    static v8::Handle<v8::Value> Push(const v8::Arguments& args);
    v8::Handle<v8::Value> DoPush(v8::Persistent<v8::Value> handle, char* data, size_t offset, size_t length);

    static v8::Handle<v8::Value> End(const v8::Arguments& args);
    v8::Handle<v8::Value> DoEnd();

    static v8::Handle<v8::Value> Destroy(const v8::Arguments& args);
    v8::Handle<v8::Value> DoDestroy();

    // Asynchronous JS API.
    static v8::Handle<v8::Value> Sweep(const v8::Arguments& args);
    static int AsyncSweep(eio_req* request);
    void EnqueueSweep();
    void DoSweep();

    static v8::Handle<v8::Value> Drain(const v8::Arguments& args);
    static int AsyncDrain(eio_req* request);
    void EnqueueDrain();
    void DoDrain();

protected:
    // C++ API.
    size_t DoRead(void* data, size_t length) override;

private:
    void DisposeHandles(std::deque<TInputPart*>* queue);
    void UpdateV8Properties();

private:
    TAtomic IsPushable;
    TAtomic IsReadable;

    TAtomic SweepRequestPending;
    TAtomic DrainRequestPending;

    TAtomic CurrentBufferSize;
    const ui64 LowWatermark;
    const ui64 HighWatermark;

    TMutex Mutex;
    TCondVar Conditional;
    std::deque<TInputPart*> ActiveQueue;
    std::deque<TInputPart*> InactiveQueue;

private:
    TNodeJSInputStream(const TNodeJSInputStream&);
    TNodeJSInputStream& operator=(const TNodeJSInputStream&);
};

inline void TNodeJSInputStream::EnqueueSweep()
{
    if (AtomicCas(&SweepRequestPending, 1, 0)) {
        // Post to V8 thread.
        AsyncRef(false);
        EIO_PUSH(TNodeJSInputStream::AsyncSweep, this);
    }
}

inline void TNodeJSInputStream::EnqueueDrain()
{
    if (AtomicCas(&DrainRequestPending, 1, 0)) {
        // Post to V8 thread.
        AsyncRef(false);
        EIO_PUSH(TNodeJSInputStream::AsyncDrain, this);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
