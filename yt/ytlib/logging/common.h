#pragma once

#include <ytlib/misc/common.h>
#include <ytlib/misc/thread.h>

namespace NYT {
namespace NLog {

////////////////////////////////////////////////////////////////////////////////

// Any changes to this enum must be also propagated to FormatLevel.
DECLARE_ENUM(ELogLevel,
    (Minimum)
    (Trace)
    (Debug)
    (Info)
    (Warning)
    (Error)
    (Fatal)
    (Maximum)
);

struct TLogEvent
{
    static const i32 InvalidLine = -1;

    TLogEvent()
        : DateTime(TInstant::Now())
        , FileName(NULL)
        , Line(InvalidLine)
        , ThreadId(NThread::InvalidThreadId)
        , Function(NULL)
    { }

    TLogEvent(const Stroka& category, ELogLevel level, const Stroka& message)
        : Category(category)
        , Level(level)
        , Message(message)
        , DateTime(TInstant::Now())
        , FileName(NULL)
        , Line(InvalidLine)
        , ThreadId(NThread::InvalidThreadId)
        , Function(NULL)
    { }

    Stroka Category;
    ELogLevel Level;
    Stroka Message;
    TInstant DateTime;
    const char* FileName;
    i32 Line;
    ::NYT::NThread::TThreadId ThreadId;
    const char* Function;

};

struct ILogWriter;
typedef TIntrusivePtr<ILogWriter> ILogWriterPtr;

////////////////////////////////////////////////////////////////////////////////

} // namespace NLog
} // namespace NYT
