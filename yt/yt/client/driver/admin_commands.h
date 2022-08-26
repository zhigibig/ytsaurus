#pragma once

#include "command.h"

namespace NYT::NDriver {

////////////////////////////////////////////////////////////////////////////////

class TBuildSnapshotCommand
    : public TTypedCommand<NApi::TBuildSnapshotOptions>
{
public:
    TBuildSnapshotCommand();

private:
    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TBuildMasterSnapshotsCommand
    : public TTypedCommand<NApi::TBuildMasterSnapshotsOptions>
{
public:
    TBuildMasterSnapshotsCommand();

private:
    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TSwitchLeaderCommand
    : public TTypedCommand<NApi::TSwitchLeaderOptions>
{
public:
    TSwitchLeaderCommand();

private:
    NHydra::TCellId CellId_;
    TString NewLeaderAddress_;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class THealExecNodeCommand
    : public TTypedCommand<NApi::THealExecNodeOptions>
{
public:
    THealExecNodeCommand();

private:
    TString Address_;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TSuspendCoordinatorCommand
    : public TTypedCommand<NApi::TSuspendCoordinatorOptions>
{
public:
    TSuspendCoordinatorCommand();

private:
    NObjectClient::TCellId CoordinatorCellId_;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TResumeCoordinatorCommand
    : public TTypedCommand<NApi::TResumeCoordinatorOptions>
{
public:
    TResumeCoordinatorCommand();

private:
    NObjectClient::TCellId CoordinatorCellId_;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TMigrateReplicationCardsCommand
    : public TTypedCommand<NApi::TMigrateReplicationCardsOptions>
{
public:
    TMigrateReplicationCardsCommand();

private:
    NObjectClient::TCellId ChaosCellId_;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TSuspendTabletCellsCommand
    : public TTypedCommand<NApi::TSuspendTabletCellsOptions>
{
public:
    TSuspendTabletCellsCommand();

private:
    std::vector<NObjectClient::TCellId> CellIds_;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

class TResumeTabletCellsCommand
    : public TTypedCommand<NApi::TResumeTabletCellsOptions>
{
public:
    TResumeTabletCellsCommand();

private:
    std::vector<NObjectClient::TCellId> CellIds_;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDriver
