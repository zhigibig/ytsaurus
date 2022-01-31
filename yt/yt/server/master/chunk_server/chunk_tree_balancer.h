#pragma once

#include "private.h"

#include <yt/yt/server/master/cell_master/public.h>

#include <yt/yt/server/master/object_server/public.h>

namespace NYT::NChunkServer {

////////////////////////////////////////////////////////////////////////////////

struct TChunkTreeBalancerSettings
{
    // NB: Changing these values will invalidate all changelogs!
    int MaxChunkTreeRank = 32;
    int MinChunkListSize = 1024;
    int MaxChunkListSize = 2048;
    double MinChunkListToChunkRatio = 0.01;
};

////////////////////////////////////////////////////////////////////////////////

struct IChunkTreeBalancerCallbacks
    : public virtual TRefCounted
{
    virtual void RefObject(NObjectServer::TObject* object) = 0;
    virtual void UnrefObject(NObjectServer::TObject* object) = 0;
    virtual void FlushObjectUnrefs() = 0;
    virtual int GetObjectRefCounter(NObjectServer::TObject* object) = 0;

    virtual void ScheduleRequisitionUpdate(TChunkTree* chunkTree) = 0;

    virtual TChunkList* CreateChunkList() = 0;
    virtual void ClearChunkList(TChunkList* chunkList) = 0;
    virtual void AttachToChunkList(
        TChunkList* chunkList,
        const std::vector<TChunkTree*>& children) = 0;
    virtual void AttachToChunkList(
        TChunkList* chunkList,
        TChunkTree* child) = 0;
    virtual void AttachToChunkList(
        TChunkList* chunkList,
        TChunkTree* const* childrenBegin,
        TChunkTree* const* childrenEnd) = 0;
};

DEFINE_REFCOUNTED_TYPE(IChunkTreeBalancerCallbacks)

////////////////////////////////////////////////////////////////////////////////

class TChunkTreeBalancer
{
public:
    explicit TChunkTreeBalancer(
        IChunkTreeBalancerCallbacksPtr callbacks,
        const TChunkTreeBalancerSettings& settings = TChunkTreeBalancerSettings());

    bool IsRebalanceNeeded(TChunkList* root);
    void Rebalance(TChunkList* root);

private:
    const IChunkTreeBalancerCallbacksPtr Callbacks_;
    const TChunkTreeBalancerSettings Settings_;

    void MergeChunkTrees(
        std::vector<TChunkTree*>* children,
        TChunkTree* child);

    void AppendChunkTree(
        std::vector<TChunkTree*>* children,
        TChunkTree* root);

    void AppendChild(
        std::vector<TChunkTree*>* children,
        TChunkTree* child);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
