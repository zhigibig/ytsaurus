#include <yt/server/cell_proxy/bootstrap.h>
#include <yt/server/cell_proxy/config.h>

#include <yt/server/program/program.h>
#include <yt/server/program/program_config_mixin.h>
#include <yt/server/program/program_pdeathsig_mixin.h>

#include <yt/server/misc/configure_singletons.h>

#include <util/system/mlock.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TCellProxyProgram
    : public TYTProgram
    , public TProgramPdeathsigMixin
    , public TProgramConfigMixin<NCellProxy::TCellProxyConfig>
{
public:
    TCellProxyProgram()
        : TProgramPdeathsigMixin(Opts_)
        , TProgramConfigMixin(Opts_)
    { }

protected:
    virtual void DoRun(const NLastGetopt::TOptsParseResult& parseResult) override
    {
        TThread::CurrentThreadSetName("ProxyMain");

        ConfigureUids();
        ConfigureSignals();
        ConfigureCrashHandler();

        try {
            LockAllMemory(ELockAllMemoryFlag::LockCurrentMemory | ELockAllMemoryFlag::LockFutureMemory);
        } catch (const std::exception& ex) {
            OnError(Format("Failed to lock memory: %v", ex.what()));
        }

        if (HandlePdeathsigOptions()) {
            return;
        }

        if (HandleConfigOptions()) {
            return;
        }

        auto config = GetConfig();
        auto configNode = GetConfigNode();

        ConfigureServerSingletons(config);

        // TODO(babenko): This memory leak is intentional.
        // We should avoid destroying bootstrap since some of the subsystems
        // may be holding a reference to it and continue running some actions in background threads.
        auto* bootstrap = new NCellProxy::TBootstrap(std::move(config), std::move(configNode));
        bootstrap->Run();
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

int main(int argc, const char** argv)
{
    return NYT::TCellProxyProgram().Run(argc, argv);
}

