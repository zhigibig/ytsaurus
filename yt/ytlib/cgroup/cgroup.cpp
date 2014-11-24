#include "stdafx.h"
#include "private.h"
#include "cgroup.h"

#include <core/ytree/fluent.h>
#include <core/ytree/serialize.h>
#include <core/ytree/convert.h>
#include <core/ytree/tree_builder.h>

#include <core/misc/fs.h>
#include <core/misc/error.h>
#include <core/misc/process.h>
#include <core/misc/string.h>

#include <util/system/fs.h>
#include <util/string/split.h>
#include <util/string/strip.h>
#include <util/folder/path.h>
#include <util/system/execpath.h>
#include <util/system/yield.h>

#ifdef _linux_
  #include <unistd.h>
  #include <sys/eventfd.h>
#endif

namespace NYT {
namespace NCGroup {

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = CGroupLogger;
static const char* CGroupRootPath = "/sys/fs/cgroup";
static const int InvalidFd = -1;

////////////////////////////////////////////////////////////////////////////////

namespace {

Stroka GetParentFor(const Stroka& type)
{
#ifdef _linux_
    auto rawData = TFileInput("/proc/self/cgroup").ReadAll();
    auto result = ParseCurrentProcessCGroups(TStringBuf(rawData.data(), rawData.size()));
    return result[type];
#else
    return "_parent_";
#endif
}

yvector<Stroka> ReadAllValues(const Stroka& fileName)
{
    auto raw = TFileInput(fileName).ReadAll();
    LOG_DEBUG("File %Qv contains: %v", fileName, raw);

    yvector<Stroka> values;
    Split(raw.data(), " \n", values);
    return values;
}

#ifdef _linux_

TDuration FromJiffies(i64 jiffies)
{
    static long ticksPerSecond = sysconf(_SC_CLK_TCK);
    return TDuration::MicroSeconds(1000 * 1000 * jiffies / ticksPerSecond);
}

#endif

} // namespace

////////////////////////////////////////////////////////////////////////////////

TEvent::TEvent(int eventFd, int fd)
    : EventFd_(eventFd)
    , Fd_(fd)
    , Fired_(false)
{ }

TEvent::TEvent()
    : TEvent(InvalidFd, InvalidFd)
{ }

TEvent::TEvent(TEvent&& other)
    : TEvent()
{
    Swap(other);
}

TEvent::~TEvent()
{
    Destroy();
}

TEvent& TEvent::operator=(TEvent&& other)
{
    if (this == &other) {
        return *this;
    }
    Destroy();
    Swap(other);
    return *this;
}

bool TEvent::Fired()
{
    YCHECK(EventFd_ != InvalidFd);

    if (Fired_) {
        return true;
    }

    auto bytesRead = ::read(EventFd_, &LastValue_, sizeof(LastValue_));

    if (bytesRead < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return false;
        }
        THROW_ERROR_EXCEPTION() << TError::FromSystem();
    }
    YCHECK(bytesRead == sizeof(LastValue_));
    Fired_ = true;
    return true;
}

void TEvent::Clear()
{
    Fired_ = false;
}

void TEvent::Destroy()
{
    Clear();
    if (EventFd_ != InvalidFd) {
        ::close(EventFd_);
    }
    EventFd_ = InvalidFd;

    if (Fd_ != InvalidFd) {
        ::close(Fd_);
    }
    Fd_ = InvalidFd;
}

i64 TEvent::GetLastValue() const
{
    return LastValue_;
}

void TEvent::Swap(TEvent& other)
{
    std::swap(EventFd_, other.EventFd_);
    std::swap(Fd_, other.Fd_);
    std::swap(Fired_, other.Fired_);
}

////////////////////////////////////////////////////////////////////////////////

std::vector<Stroka> GetSupportedCGroups()
{
    std::vector<Stroka> result;
    result.push_back("cpuacct");
    result.push_back("blkio");
    result.push_back("memory");
    result.push_back("freezer");
    return result;
}

void ApplyActionToAllChlidren(const TFsPath& path, const TCallback<void(const TFsPath&)> action)
{
    if (path.Exists()) {
        yvector<TFsPath> children;
        path.List(children);
        for (const auto& child : children) {
            if (child.IsDirectory()) {
                ApplyActionToAllChlidren(child, action);
                action.Run(child);
            }
        }
    }
}

void RemoveDir(const TFsPath& path)
{
    path.DeleteIfExists();
}

void RemoveAllSubcgroups(const Stroka& path)
{
    ApplyActionToAllChlidren(TFsPath(path), BIND(RemoveDir));
}

// The caller must be sure that it has root privileges.
void RunKiller(const Stroka& processGroupPath)
{
#ifdef _linux_
    LOG_INFO("Kill %Qv processes", processGroupPath);

    TNonOwningCGroup group(processGroupPath);
    auto pids = group.GetTasks();
    if (pids.empty())
        return;

    auto throwError = [=] (const TError& error) {
        THROW_ERROR_EXCEPTION(
            "Failed to kill processes from %Qv",
            processGroupPath) << error;
    };

    TProcess process(GetExecPath());
    process.AddArgument("--killer");
    process.AddArgument("--process-group-path");
    process.AddArgument(processGroupPath);

    // We are forking here in order not to give the root privileges to the parent process ever,
    // because we cannot know what other threads are doing.
    auto error = process.Spawn();
    if (!error.IsOK()) {
        throwError(error);
    }

    error = process.Wait();
    if (!error.IsOK()) {
        throwError(error);
    }
#endif
}

void KillProcessGroupImpl(const TFsPath& processGroupPath)
{
#ifdef _linux_
    TNonOwningCGroup group(processGroupPath);
    auto pids = group.GetTasks();

    while (!pids.empty()) {
        LOG_DEBUG("Killing processes (PIDs: [%v])",
            JoinToString(pids));

        for (int pid : pids) {
            auto result = kill(pid, 9);
            if (result == -1) {
                YCHECK(errno == ESRCH);
            }
        }

        ThreadYield();
        pids = group.GetTasks();
    }
#endif
}

void KillProcessGroup(const Stroka& processGroupPath)
{
#ifdef _linux_
    LOG_DEBUG("Killing processes from %v cgroup",
        processGroupPath);

    YCHECK(setuid(0) == 0);
    ApplyActionToAllChlidren(TFsPath(processGroupPath), BIND(KillProcessGroupImpl));
#endif
}

////////////////////////////////////////////////////////////////////////////////

TNonOwningCGroup::TNonOwningCGroup()
    : FullPath_()
{ }


TNonOwningCGroup::TNonOwningCGroup(const Stroka& fullPath)
    : FullPath_(fullPath)
{ }

TNonOwningCGroup::TNonOwningCGroup(const Stroka& type, const Stroka& name)
    : FullPath_(NFS::CombinePaths(NFS::CombinePaths(NFS::CombinePaths(
        CGroupRootPath,
        type),
        GetParentFor(type)),
        name))
{ }

TNonOwningCGroup::TNonOwningCGroup(TNonOwningCGroup&& other)
    : FullPath_(std::move(other.FullPath_))
{ }

// This method SHOULD work fine in forked process
// So we cannot use out logging|profiling framework
void TNonOwningCGroup::AddTask(int pid)
{
    YCHECK(!IsNull());
#ifdef _linux_
    auto path = NFS::CombinePaths(FullPath_, "tasks");
    TFileOutput output(TFile(path, OpenMode::ForAppend));
    output << pid;
#endif
}


// This method SHOULD work fine in forked process
// So we cannot use out logging|profiling framework
void TNonOwningCGroup::AddCurrentTask()
{
    YCHECK(!IsNull());
#ifdef _linux_
    auto pid = getpid();
    AddTask(pid);
#endif
}

Stroka TNonOwningCGroup::Get(const Stroka& name) const
{
    YCHECK(!IsNull());
    Stroka result;
#ifdef _linux_
    const auto path = NFS::CombinePaths(FullPath_, name);
    result = TBufferedFileInput(path).ReadLine();
#endif
    return result;
}

void TNonOwningCGroup::Set(const Stroka& name, const Stroka& value) const
{
    YCHECK(!IsNull());
#ifdef _linux_
    auto path = NFS::CombinePaths(FullPath_, name);
    TFileOutput output(TFile(path, OpenMode::WrOnly));
    output << value;
#endif
}

void TNonOwningCGroup::Append(const Stroka& name, const Stroka& value) const
{
    YCHECK(!IsNull());
#ifdef _linux_
    auto path = NFS::CombinePaths(FullPath_, name);
    TFileOutput output(TFile(path, OpenMode::ForAppend));
    output << value;
#endif
}

bool TNonOwningCGroup::IsNull() const
{
    return FullPath_.Empty();
}

std::vector<int> TNonOwningCGroup::GetTasks() const
{
    std::vector<int> results;
    if (!IsNull()) {
#ifdef _linux_
        auto values = ReadAllValues(NFS::CombinePaths(FullPath_, "tasks"));
        for (const auto& value : values) {
            int pid = FromString<int>(value);
            results.push_back(pid);
        }
#endif
    }
    return results;
}

const Stroka& TNonOwningCGroup::GetFullPath() const
{
    return FullPath_;
}

void TNonOwningCGroup::EnsureExistance()
{
    LOG_INFO("Creating cgroup %Qv", FullPath_);

    YCHECK(!IsNull());

#ifdef _linux_
    NFS::ForcePath(FullPath_, 0755);
#endif
}

////////////////////////////////////////////////////////////////////////////////

TCGroup::TCGroup(const Stroka& type, const Stroka& name)
    : TNonOwningCGroup(type, name)
    , Created_(false)
{ }

TCGroup::TCGroup(TCGroup&& other)
    : TNonOwningCGroup(std::move(other))
    , Created_(other.Created_)
{
    other.Created_ = false;
}

TCGroup::~TCGroup()
{
    if (Created_) {
        try {
            Destroy();
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Unable to destroy cgroup %Qv", FullPath_);
        }
    }
}

void TCGroup::Create()
{
    EnsureExistance();
    Created_ = true;
}

void TCGroup::Destroy()
{
    LOG_INFO("Destroying cgroup %Qv", FullPath_);

#ifdef _linux_
    YCHECK(Created_);

    NFS::Remove(FullPath_);
    Created_ = false;
#endif
}

bool TCGroup::IsCreated() const
{
    return Created_;
}

////////////////////////////////////////////////////////////////////////////////

TCpuAccounting::TStatistics::TStatistics()
    : UserTime(0)
    , SystemTime(0)
{ }

TCpuAccounting::TCpuAccounting(const Stroka& name)
    : TCGroup("cpuacct", name)
{ }

TCpuAccounting::TStatistics TCpuAccounting::GetStatistics()
{
    TCpuAccounting::TStatistics result;
#ifdef _linux_
    auto path = NFS::CombinePaths(GetFullPath(), "cpuacct.stat");
    auto values = ReadAllValues(path);
    if (values.size() != 4) {
        THROW_ERROR_EXCEPTION("Unable to parse %Qv: expected 4 values, got %v",
            path,
            values.size());
    }

    Stroka type[2];
    i64 jiffies[2];

    for (int i = 0; i < 2; ++i) {
        type[i] = values[2 * i];
        jiffies[i] = FromString<i64>(values[2 * i + 1]);
    }

    for (int i = 0; i < 2; ++i) {
        if (type[i] == "user") {
            result.UserTime = FromJiffies(jiffies[i]);
        } else if (type[i] == "system") {
            result.SystemTime = FromJiffies(jiffies[i]);
        }
    }
#endif
    return result;
}

void ToProto(NProto::TCpuAccountingStatistics* protoStats, const TCpuAccounting::TStatistics& stats)
{
    protoStats->set_user_time(stats.UserTime.MilliSeconds());
    protoStats->set_system_time(stats.SystemTime.MilliSeconds());
}

void Serialize(const TCpuAccounting::TStatistics& statistics, NYson::IYsonConsumer* consumer)
{
    NYTree::BuildYsonFluently(consumer)
        .BeginMap()
            .Item("user").Value(static_cast<i64>(statistics.UserTime.MilliSeconds()))
            .Item("system").Value(static_cast<i64>(statistics.SystemTime.MilliSeconds()))
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

TBlockIO::TStatistics::TStatistics()
    : TotalSectors(0)
    , BytesRead(0)
    , BytesWritten(0)
{ }

TBlockIO::TBlockIO(const Stroka& name)
    : TCGroup("blkio", name)
{ }

// For more information about format of data
// read https://www.kernel.org/doc/Documentation/cgroups/blkio-controller.txt

TBlockIO::TStatistics TBlockIO::GetStatistics()
{
    TBlockIO::TStatistics result;
#ifdef _linux_
    {
        auto path = NFS::CombinePaths(GetFullPath(), "blkio.io_service_bytes");
        auto values = ReadAllValues(path);

        result.BytesRead = result.BytesWritten = 0;
        int lineNumber = 0;
        while (3 * lineNumber + 2 < values.size()) {
            const Stroka& deviceId = values[3 * lineNumber];
            const Stroka& type = values[3 * lineNumber + 1];
            auto bytes = FromString<i64>(values[3 * lineNumber + 2]);

            if (!deviceId.has_prefix("8:")) {
                THROW_ERROR_EXCEPTION("Unable to parse %Qv: %v should start with \"8:\"", path, deviceId);
            }

            if (type == "Read") {
                result.BytesRead += bytes;
            } else if (type == "Write") {
                result.BytesWritten += bytes;
            } else {
                if (type != "Sync" && type != "Async" && type != "Total") {
                    THROW_ERROR_EXCEPTION("Unable to parse %v: unexpected stat type %v", path, type);
                }
            }
            ++lineNumber;
        }
    }
    {
        auto path = NFS::CombinePaths(GetFullPath(), "blkio.sectors");
        auto values = ReadAllValues(path);

        result.TotalSectors = 0;
        int lineNumber = 0;
        while (2 * lineNumber < values.size()) {
            const Stroka& deviceId = values[2 * lineNumber];
            auto sectors = FromString<i64>(values[2 * lineNumber + 1]);

            if (!deviceId.has_prefix("8:")) {
                THROW_ERROR_EXCEPTION("Unable to parse %Qv: %v should start with \"8:\"", path, deviceId);
            }

            result.TotalSectors += sectors;
            ++lineNumber;
        }
    }
#endif
    return result;
}

std::vector<TBlockIO::TStatisticsItem> TBlockIO::GetIOServiceBytes()
{
    return GetDetailedStatistics("blkio.io_service_bytes");
}

std::vector<TBlockIO::TStatisticsItem> TBlockIO::GetIOServiced()
{
    return GetDetailedStatistics("blkio.io_serviced");
}

std::vector<TBlockIO::TStatisticsItem> TBlockIO::GetDetailedStatistics(const char* filename)
{
    std::vector<TBlockIO::TStatisticsItem> result;
#ifdef _linux_
    auto path = NFS::CombinePaths(GetFullPath(), filename);
    auto values = ReadAllValues(path);

    int lineNumber = 0;
    while (3 * lineNumber + 2 < values.size()) {
        TStatisticsItem item;
        item.DeviceId = values[3 * lineNumber];
        item.Type = values[3 * lineNumber + 1];
        item.Value = FromString<i64>(values[3 * lineNumber + 2]);

        if (!item.DeviceId.has_prefix("8:")) {
            THROW_ERROR_EXCEPTION("Unable to parse %Qv: %v should start with \"8:\"", path, item.DeviceId);
        }

        if (item.Type == "Read" || item.Type == "Write") {
            result.push_back(item);
        }
        ++lineNumber;
    }
#endif
    return result;
}

void TBlockIO::ThrottleOperations(const Stroka& deviceId, i64 operations)
{
    auto value = Format("%v %v", deviceId, operations);
    Append("blkio.throttle.read_iops_device", value);
    Append("blkio.throttle.write_iops_device", value);
}

void ToProto(NProto::TBlockIOStatistics* protoStats, const TBlockIO::TStatistics& stats)
{
    protoStats->set_total_sectors(stats.TotalSectors);
    protoStats->set_bytes_read(stats.BytesRead);
    protoStats->set_bytes_written(stats.BytesWritten);
}

void Serialize(const TBlockIO::TStatistics& statistics, NYson::IYsonConsumer* consumer)
{
    NYTree::BuildYsonFluently(consumer)
        .BeginMap()
            .Item("bytes_read").Value(statistics.BytesRead)
            .Item("bytes_written").Value(statistics.BytesWritten)
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

TMemory::TMemory(const Stroka& name)
    : TCGroup("memory", name)
{ }

TMemory::TMemory(TMemory&& other)
    : TCGroup(std::move(other))
{ }


TMemory::TStatistics TMemory::GetStatistics()
{
    TMemory::TStatistics result;
#ifdef _linux_
    {
        auto values = ReadAllValues(NFS::CombinePaths(FullPath_, "memory.stat"));
        int lineNumber = 0;
        while (2 * lineNumber + 1 < values.size()) {
            const auto& type = values[2 * lineNumber];
            const auto value = FromString<i64>(values[2 * lineNumber + 1]);
            if (type == "rss") {
                result.Rss = value;
            }
            if (type == "mapped_file") {
                result.MappedFile = value;
            }
            ++lineNumber;
        }
    }
#endif
    return result;
}

i64 TMemory::GetUsageInBytes() const
{
    return FromString<i64>(Get("memory.usage_in_bytes"));
}

i64 TMemory::GetMaxUsageInBytes() const
{
    return FromString<i64>(Get("memory.max_usage_in_bytes"));
}

void TMemory::SetLimitInBytes(i64 bytes) const
{
    Set("memory.limit_in_bytes", ToString(bytes));
}

bool TMemory::IsHierarchyEnabled() const
{
#ifdef _linux_
    auto isHierarchyEnabled = FromString<int>(Get("memory.use_hierarchy"));
    YCHECK((isHierarchyEnabled == 0) || (isHierarchyEnabled == 1));

    return (isHierarchyEnabled == 1);
#else
    return false;
#endif
}

void TMemory::EnableHierarchy() const
{
    Set("memory.use_hierarchy", "1");
}

bool TMemory::IsOomEnabled() const
{
#ifdef _linux_
    const auto path = NFS::CombinePaths(GetFullPath(), "memory.oom_control");
    auto values = ReadAllValues(path);
    if (values.size() != 4) {
        THROW_ERROR_EXCEPTION("Unable to parse %Qv: expected 4 values, got %v", path, values.size());
    }
    for (int i = 0; i < 2; ++i) {
        if (values[2 * i] == "oom_kill_disable") {
            const auto& isDisabled = values[2 * i + 1];
            if (isDisabled == "0") {
                return true;
            } else if (isDisabled == "1") {
                return false;
            } else {
                THROW_ERROR_EXCEPTION("Unexpected value for oom_kill_disable. Expected '0' or '1'. Got: %Qv",
                    isDisabled);
            }
        }
    }
    THROW_ERROR_EXCEPTION("Unable to find 'oom_kill_disable' in %Qv", path);
#else
    return false;
#endif
}

void TMemory::DisableOom() const
{
    // This parameter should be call `memory.disable_oom_control`.
    // 1 means `disable`.
    Set("memory.oom_control", "1");
}

TEvent TMemory::GetOomEvent() const
{
#ifdef _linux_
    auto fileName = NFS::CombinePaths(GetFullPath(), "memory.oom_control");
    auto fd = ::open(~fileName, O_WRONLY | O_CLOEXEC);

    auto eventFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    auto data = ToString(eventFd) + ' ' + ToString(fd);

    Set("cgroup.event_control", data);

    return TEvent(eventFd, fd);
#else
    return TEvent();
#endif
}

void TMemory::ForceEmpty() const
{
    Set("memory.force_empty", "0");
}

int TMemory::GetFailCount() const
{
    return FromString<int>(Get("memory.failcnt"));
}

TMemory::TStatistics::TStatistics()
    : Rss(0)
    , MappedFile(0)
{ }

////////////////////////////////////////////////////////////////////////////////

TFreezer::TFreezer(const Stroka& name)
    : TCGroup("freezer", name)
{ }

Stroka TFreezer::GetState() const
{
    return Get("freezer.state");
}

void TFreezer::Freeze() const
{
    Set("freezer.state", "FROZEN");
}

void TFreezer::UnFreeze() const
{
    Set("freezer.state", "THAWED");
}

////////////////////////////////////////////////////////////////////////////////

std::map<Stroka, Stroka> ParseCurrentProcessCGroups(TStringBuf str)
{
    std::map<Stroka, Stroka> result;

    yvector<Stroka> values;
    Split(str.data(), ":\n", values);
    for (size_t i = 0; i + 2 < values.size(); i += 3) {
        FromString<int>(values[i]);

        const Stroka& subsystemsSet = values[i + 1];
        const Stroka& name = values[i + 2];

        yvector<Stroka> subsystems;
        Split(subsystemsSet.data(), ",", subsystems);
        for (const auto& subsystem : subsystems) {
            if (!subsystem.has_prefix("name=")) {
                int start = 0;
                if (name.has_prefix("/")) {
                    start = 1;
                }
                result[subsystem] = name.substr(start);
            }
        }
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCGroup
} // namespace NYT
