#pragma once

#include "command.h"

#include <yt/ytlib/job_tracker_client/public.h>

#include <yt/ytlib/scheduler/public.h>

namespace NYT {
namespace NDriver {

//////////////////////////////////////////////////////////////////////////////

class TDumpJobContextCommand
    : public TTypedCommand<NApi::TDumpJobContextOptions>
{
public:
    TDumpJobContextCommand();

private:
    NJobTrackerClient::TJobId JobId;
    NYPath::TYPath Path;

    virtual void DoExecute(ICommandContextPtr context) override;
};

//////////////////////////////////////////////////////////////////////////////

class TGetJobInputCommand
    : public TTypedCommand<NApi::TGetJobInputOptions>
{
public:
    TGetJobInputCommand();

private:
    NJobTrackerClient::TOperationId OperationId;
    NJobTrackerClient::TJobId JobId;

    virtual void DoExecute(ICommandContextPtr context) override;
};

//////////////////////////////////////////////////////////////////////////////

class TGetJobStderrCommand
    : public TTypedCommand<NApi::TGetJobStderrOptions>
{
public:
    TGetJobStderrCommand();

private:
    NJobTrackerClient::TOperationId OperationId;
    NJobTrackerClient::TJobId JobId;

    virtual void DoExecute(ICommandContextPtr context) override;
};

//////////////////////////////////////////////////////////////////////////////

class TListJobsCommand
    : public TTypedCommand<NApi::TListJobsOptions>
{
public:
    TListJobsCommand();

private:
    NJobTrackerClient::TOperationId OperationId;

    virtual void DoExecute(ICommandContextPtr context) override;
};

//////////////////////////////////////////////////////////////////////////////

class TStraceJobCommand
    : public TTypedCommand<NApi::TStraceJobOptions>
{
public:
    TStraceJobCommand();

private:
    NJobTrackerClient::TJobId JobId;

    virtual void DoExecute(ICommandContextPtr context) override;
};

//////////////////////////////////////////////////////////////////////////////

class TSignalJobCommand
    : public TTypedCommand<NApi::TSignalJobOptions>
{
public:
    TSignalJobCommand();

private:
    NJobTrackerClient::TJobId JobId;
    TString SignalName;

    virtual void DoExecute(ICommandContextPtr context) override;
};

//////////////////////////////////////////////////////////////////////////////

class TAbandonJobCommand
    : public TTypedCommand<NApi::TAbandonJobOptions>
{
public:
    TAbandonJobCommand();

private:
    NJobTrackerClient::TJobId JobId;

    virtual void DoExecute(ICommandContextPtr context) override;
};

//////////////////////////////////////////////////////////////////////////////

class TPollJobShellCommand
    : public TTypedCommand<NApi::TPollJobShellOptions>
{
public:
    TPollJobShellCommand();

private:
    NJobTrackerClient::TJobId JobId;
    NYTree::INodePtr Parameters;

    virtual void OnLoaded() override;
    virtual void DoExecute(ICommandContextPtr context) override;
};

//////////////////////////////////////////////////////////////////////////////

class TAbortJobCommand
    : public TTypedCommand<NApi::TAbortJobOptions>
{
private:
    NJobTrackerClient::TJobId JobId;

public:
    TAbortJobCommand();

    virtual void DoExecute(ICommandContextPtr context) override;
};

//////////////////////////////////////////////////////////////////////////////

class TStartOperationCommandBase
    : public TTypedCommand<NApi::TStartOperationOptions>
{
public:
    TStartOperationCommandBase();

private:
    NYTree::INodePtr Spec;

    virtual NScheduler::EOperationType GetOperationType() const = 0;
    virtual void DoExecute(ICommandContextPtr context) override;
};

//////////////////////////////////////////////////////////////////////////////

class TMapCommand
    : public TStartOperationCommandBase
{
private:
    virtual NScheduler::EOperationType GetOperationType() const override;
};

//////////////////////////////////////////////////////////////////////////////

class TMergeCommand
    : public TStartOperationCommandBase
{
private:
    virtual NScheduler::EOperationType GetOperationType() const override;
};

//////////////////////////////////////////////////////////////////////////////

class TSortCommand
    : public TStartOperationCommandBase
{
private:
    virtual NScheduler::EOperationType GetOperationType() const override;
};

//////////////////////////////////////////////////////////////////////////////

class TEraseCommand
    : public TStartOperationCommandBase
{
private:
    virtual NScheduler::EOperationType GetOperationType() const override;
};

//////////////////////////////////////////////////////////////////////////////

class TReduceCommand
    : public TStartOperationCommandBase
{
private:
    virtual NScheduler::EOperationType GetOperationType() const override;
};

//////////////////////////////////////////////////////////////////////////////

class TJoinReduceCommand
    : public TStartOperationCommandBase
{
private:
    virtual NScheduler::EOperationType GetOperationType() const override;
};

//////////////////////////////////////////////////////////////////////////////

class TMapReduceCommand
    : public TStartOperationCommandBase
{
private:
    virtual NScheduler::EOperationType GetOperationType() const override;
};

//////////////////////////////////////////////////////////////////////////////

class TRemoteCopyCommand
    : public TStartOperationCommandBase
{
private:
    virtual NScheduler::EOperationType GetOperationType() const override;
};

//////////////////////////////////////////////////////////////////////////////

template <class TOptions>
class TSimpleOperationCommandBase
    : public virtual TTypedCommandBase<TOptions>
{
protected:
    NScheduler::TOperationId OperationId;

public:
    TSimpleOperationCommandBase()
    {
        this->RegisterParameter("operation_id", OperationId);
    }
};

//////////////////////////////////////////////////////////////////////////////

class TAbortOperationCommand
    : public TSimpleOperationCommandBase<NApi::TAbortOperationOptions>
{
public:
    TAbortOperationCommand();

private:
    virtual void DoExecute(ICommandContextPtr context) override;
};

//////////////////////////////////////////////////////////////////////////////

class TSuspendOperationCommand
    : public TSimpleOperationCommandBase<NApi::TSuspendOperationOptions>
{
public:
    TSuspendOperationCommand();

private:
    virtual void DoExecute(ICommandContextPtr context) override;
};

//////////////////////////////////////////////////////////////////////////////

class TResumeOperationCommand
    : public TSimpleOperationCommandBase<NApi::TResumeOperationOptions>
{
public:
    virtual void DoExecute(ICommandContextPtr context) override;
};

//////////////////////////////////////////////////////////////////////////////

class TCompleteOperationCommand
    : public TSimpleOperationCommandBase<NApi::TCompleteOperationOptions>
{
public:
    virtual void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT

