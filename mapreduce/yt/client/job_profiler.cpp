#include "job_profiler.h"

#include <yt/yt/library/ytprof/cpu_profiler.h>
#include <yt/yt/library/ytprof/external_pprof.h>
#include <yt/yt/library/ytprof/profile.h>
#include <yt/yt/library/ytprof/symbolize.h>

#include <mapreduce/yt/interface/logging/logger.h>
#include <mapreduce/yt/interface/logging/yt_log.h>

#include <library/cpp/yson/node/node_io.h>

#include <util/system/env.h>
#include <util/system/file.h>
#include <util/system/shellcommand.h>

namespace NYT {

using namespace NYTProf;

////////////////////////////////////////////////////////////////////////////////

static void RunSubprocess(const std::vector<TString>& cmd)
{
    auto command = cmd[0];
    auto args = TList<TString>(cmd.begin() + 1, cmd.end());

    TShellCommand(command, args)
        .Run()
        .Wait();
}

////////////////////////////////////////////////////////////////////////////////

class TJobProfiler
    : public IJobProfiler
{
public:
    TJobProfiler()
    {
        try {
            InitializeProfiler();
        } catch (const std::exception& ex) {
            YT_LOG_ERROR("Failed to initialize job profiler: %v",
                ex.what());
        }
    }

    void Start() override
    {
        if (CpuProfiler_) {
            CpuProfiler_->Start();
        }
    }

    void Stop() override
    {
        if (CpuProfiler_) {
            CpuProfiler_->Stop();

            auto profile = CpuProfiler_->ReadProfile();
            SymbolizeAndWriteProfile(&profile);
        }
    }

private:
    std::unique_ptr<TCpuProfiler> CpuProfiler_;

    bool RunExternalSymbolizer_ = false;

    void InitializeProfiler()
    {
        auto profilerSpecYson = GetEnv("YT_JOB_PROFILER_SPEC");
        if (!profilerSpecYson) {
            return;
        }

        auto profilerSpec = NodeFromYsonString(profilerSpecYson);
        if (profilerSpec["type"] == "cpu") {
            auto samplingFrequency = profilerSpec["sampling_frequency"].AsInt64();
            CpuProfiler_ = std::make_unique<TCpuProfiler>(TCpuProfilerOptions{
                .SamplingFrequency = static_cast<int>(samplingFrequency),
            });
        }

        if (profilerSpec["run_external_symbolizer"] == true) {
            RunExternalSymbolizer_ = true;
        }
    }

    void SymbolizeAndWriteProfile(NYTProf::NProto::Profile* profile)
    {
        Symbolize(profile, /*filesOnly*/ true);
        AddBuildInfo(profile, TBuildInfo::GetDefault());

        if (RunExternalSymbolizer_) {
            SymbolizeByExternalPProf(profile, TSymbolizationOptions{
                .RunTool = RunSubprocess,
            });
        }

        auto serializedProfile = SerializeProfile(*profile);

        constexpr int ProfileFileDescriptor = 8;
        TFile profileFile(ProfileFileDescriptor);
        profileFile.Write(serializedProfile.data(), serializedProfile.size());
        profileFile.FlushData();
    }
};

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IJobProfiler> CreateJobProfiler()
{
    return std::make_unique<TJobProfiler>();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
