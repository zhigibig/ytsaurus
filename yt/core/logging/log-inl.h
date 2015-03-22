#ifndef LOG_INL_H_
#error "Direct inclusion of this file is not allowed, include log.h"
#endif
#undef LOG_H_

namespace NYT {
namespace NLogging {
namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

template <class... TArgs>
inline Stroka FormatLogMessage(const char* format, const TArgs&... args)
{
    return Format(format, args...);
}

template <class... TArgs>
inline Stroka FormatLogMessage(const TError& error, const char* format, const TArgs&... args)
{
    TStringBuilder builder;
    Format(&builder, format, args...);
    builder.AppendChar('\n');
    builder.AppendString(ToString(error));
    return builder.Flush();
}

template <class T>
inline Stroka FormatLogMessage(const T& obj)
{
    return ToString(obj);
}

template <class TLogger>
void LogEventImpl(
    TLogger& logger,
    const char* fileName,
    int line,
    const char* function,
    ELogLevel level,
    const Stroka& message)
{
    TLogEvent event;
    event.DateTime = TInstant::Now();
    event.Category = logger.GetCategory();
    event.Level = level;
    event.Message = message;
    event.FileName = fileName;
    event.Line = line;
    event.ThreadId = NConcurrency::GetCurrentThreadId();
    event.FiberId = NConcurrency::GetCurrentFiberId();
    event.TraceId = NTracing::GetCurrentTraceContext().GetTraceId();
    event.Function = function;
    logger.Write(std::move(event));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail
} // namespace NLogging
} // namespace NYT
