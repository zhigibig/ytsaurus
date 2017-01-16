#include "statistics.h"

#include <yt/ytlib/chunk_client/data_statistics.h>

#include <yt/core/misc/protobuf_helpers.h>

#include <yt/core/ypath/token.h>

#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/helpers.h>

#include <util/string/util.h>

namespace NYT {
namespace NJobTrackerClient {

using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NPhoenix;
using namespace NChunkClient::NProto;

////////////////////////////////////////////////////////////////////

TSummary::TSummary()
{
    Reset();
}

TSummary::TSummary(i64 sum, i64 count, i64 min, i64 max)
    : Sum_(sum)
    , Count_(count)
    , Min_(min)
    , Max_(max)
{ }

void TSummary::AddSample(i64 sample)
{
    Sum_ += sample;
    Count_ += 1;
    Min_ = std::min(Min_, sample);
    Max_ = std::max(Max_, sample);
}

void TSummary::Update(const TSummary& summary)
{
    Sum_ += summary.GetSum();
    Count_ += summary.GetCount();
    Min_ = std::min(Min_, summary.GetMin());
    Max_ = std::max(Max_, summary.GetMax());
}

void TSummary::Reset()
{
    Sum_ = 0;
    Count_ = 0;
    Min_ = std::numeric_limits<i64>::max();
    Max_ = std::numeric_limits<i64>::min();
}

void TSummary::Persist(NPhoenix::TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Sum_);
    Persist(context, Count_);
    Persist(context, Min_);
    Persist(context, Max_);
}

void Serialize(const TSummary& summary, NYson::IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("sum").Value(summary.GetSum())
            .Item("count").Value(summary.GetCount())
            .Item("min").Value(summary.GetMin())
            .Item("max").Value(summary.GetMax())
        .EndMap();
}

bool TSummary::operator ==(const TSummary& other) const
{
    return
        Sum_ == other.Sum_ &&
        Count_ == other.Count_ &&
        Min_ == other.Min_ &&
        Max_ == other.Max_;
}

////////////////////////////////////////////////////////////////////

TSummary& TStatistics::GetSummary(const NYPath::TYPath& path)
{
    auto result = Data_.insert(std::make_pair(path, TSummary()));
    auto it = result.first;
    if (result.second) {
        // This is a new statistic, check validity.
        if (it != Data_.begin()) {
            auto prev = std::prev(it);
            if (HasPrefix(it->first, prev->first)) {
                Data_.erase(it);
                THROW_ERROR_EXCEPTION(
                    "Incompatible statistic paths: old %v, new %v",
                    prev->first,
                    path);
            }
        }
        auto next = std::next(it);
        if (next != Data_.end()) {
            if (HasPrefix(next->first, it->first)) {
                Data_.erase(it);
                THROW_ERROR_EXCEPTION(
                    "Incompatible statistic paths: old %v, new %v",
                    next->first,
                    path);
            }
        }
    }

    return it->second;
}

void TStatistics::AddSample(const NYPath::TYPath& path, i64 sample)
{
    GetSummary(path).AddSample(sample);
}

void TStatistics::AddSample(const NYPath::TYPath& path, const INodePtr& sample)
{
    switch (sample->GetType()) {
        case ENodeType::Int64:
            AddSample(path, sample->AsInt64()->GetValue());
            break;

        case ENodeType::Uint64:
            AddSample(path, static_cast<i64>(sample->AsUint64()->GetValue()));
            break;

        case ENodeType::Map:
            for (auto& pair : sample->AsMap()->GetChildren()) {
                AddSample(path + "/" + ToYPathLiteral(pair.first), pair.second);
            }
            break;

        default:
            THROW_ERROR_EXCEPTION(
                "Invalid statistics type: expected map or integral type but found %v",
                ConvertToYsonString(sample, EYsonFormat::Text).GetData());
    }
}

void TStatistics::Update(const TStatistics& statistics)
{
    for (const auto& pair : statistics.Data()) {
        GetSummary(pair.first).Update(pair.second);
    }
}

void TStatistics::AddSuffixToNames(const Stroka& suffix)
{
    TSummaryMap newData;
    for (const auto& pair : Data_) {
        newData[pair.first + suffix] = pair.second;
    }

    std::swap(Data_, newData);
}

void TStatistics::Persist(NPhoenix::TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Data_);
}

void Serialize(const TStatistics& statistics, NYson::IYsonConsumer* consumer)
{
    auto root = GetEphemeralNodeFactory()->CreateMap();
    if (statistics.GetTimestamp()) {
        root->MutableAttributes()->Set("timestamp", *statistics.GetTimestamp());
    }
    for (const auto& pair : statistics.Data()) {
        ForceYPath(root, pair.first);
        auto value = ConvertToNode(pair.second);
        SetNodeByYPath(root, pair.first, std::move(value));
    }

    Serialize(*root, consumer);
}

// Helper function for GetNumericValue.
i64 GetSum(const TSummary& summary)
{
    return summary.GetSum();
}

i64 GetNumericValue(const TStatistics& statistics, const Stroka& path)
{
    auto value = FindNumericValue(statistics, path);
    if (!value) {
        THROW_ERROR_EXCEPTION("Statistics %v is not present",
            path);
    } else {
        return *value;
    }
}

TNullable<i64> FindNumericValue(const TStatistics& statistics, const Stroka& path)
{
    const auto& data = statistics.Data();
    auto iterator = data.lower_bound(path);
    if (iterator != data.end() && iterator->first != path && HasPrefix(iterator->first, path)) {
        THROW_ERROR_EXCEPTION("Invalid statistics type: can't get numeric value of %v since it is a map",
            path);
    } else if (iterator == data.end() || iterator->first != path) {
        return Null;
    } else {
        return iterator->second.GetSum();
    }
}


////////////////////////////////////////////////////////////////////

class TStatisticsBuildingConsumer
    : public TYsonConsumerBase
    , public IBuildingYsonConsumer<TStatistics>
{
public:
    virtual void OnStringScalar(const TStringBuf& value) override
    {
        if (!AtAttributes_) {
            THROW_ERROR_EXCEPTION("String scalars are not allowed for statistics");
        }
        Statistics_.SetTimestamp(ConvertTo<TInstant>(value));
    }

    virtual void OnInt64Scalar(i64 value) override
    {
        if (AtAttributes_) {
            THROW_ERROR_EXCEPTION("Timestamp should have string type");
        }
        AtSummaryMap_ = true;
        if (LastKey_ == "sum") {
            CurrentSummary_.Sum_ = value;
        } else if (LastKey_ == "count") {
            CurrentSummary_.Count_ = value;
        } else if (LastKey_ == "min") {
            CurrentSummary_.Min_ = value;
        } else if (LastKey_ == "max") {
            CurrentSummary_.Max_ = value;
        } else {
            THROW_ERROR_EXCEPTION("Invalid summary key for statistics")
                << TErrorAttribute("key", LastKey_);
        }
        ++FilledSummaryFields_;
    }

    virtual void OnUint64Scalar(ui64 value) override
    {
        THROW_ERROR_EXCEPTION("Uint64 scalars are not allowed for statistics");
    }

    virtual void OnDoubleScalar(double value) override
    {
        THROW_ERROR_EXCEPTION("Double scalars are not allowed for statistics");
    }

    virtual void OnBooleanScalar(bool value) override
    {
        THROW_ERROR_EXCEPTION("Boolean scalars are not allowed for statistics");
    }

    virtual void OnEntity() override
    {
        THROW_ERROR_EXCEPTION("Entities are not allowed for statistics");
    }

    virtual void OnBeginList() override
    {
        THROW_ERROR_EXCEPTION("Lists are not allowed for statistics");
    }

    virtual void OnListItem() override
    {
        THROW_ERROR_EXCEPTION("Lists are not allowed for statistics");
    }

    virtual void OnEndList() override
    {
        THROW_ERROR_EXCEPTION("Lists are not allowed for statistics");
    }

    virtual void OnBeginMap() override
    {
        // If we are here, we are either:
        // * at the root (then do nothing)
        // * at some directory (then the last key was the directory name)
        if (!LastKey_.empty()) {
            DirectoryNameLengths_.push_back(LastKey_.size());
            CurrentPath_.append('/');
            CurrentPath_.append(LastKey_);
            LastKey_.clear();
        } else {
            if (!CurrentPath_.empty()) {
                THROW_ERROR_EXCEPTION("Empty keys are not allowed for statistics");
            }
        }
    }

    virtual void OnKeyedItem(const TStringBuf& key) override
    {
        if (AtAttributes_) {
            if (key != "timestamp") {
                THROW_ERROR_EXCEPTION("Attributes other than \"timestamp\" are not allowed");
            }
        } else {
            LastKey_ = ToYPathLiteral(key);
        }
    }

    virtual void OnEndMap() override
    {
        if (AtSummaryMap_) {
            if (FilledSummaryFields_ != 4) {
                THROW_ERROR_EXCEPTION("All four summary fields should be filled for statistics");
            }
            Statistics_.Data_[CurrentPath_] = CurrentSummary_;
            FilledSummaryFields_ = 0;
            AtSummaryMap_ = false;
        }

        if (!CurrentPath_.empty()) {
            // We need to go to the parent.
            CurrentPath_.resize(CurrentPath_.size() - DirectoryNameLengths_.back() - 1);
            DirectoryNameLengths_.pop_back();
        }
    }

    virtual void OnBeginAttributes() override
    {
        if (!CurrentPath_.empty()) {
            THROW_ERROR_EXCEPTION("Attributes are not allowed for statistics");
        }
        AtAttributes_ = true;
    }

    virtual void OnEndAttributes() override
    {
        AtAttributes_ = false;
    }

    virtual TStatistics Finish() override
    {
        return Statistics_;
    }

private:
    TStatistics Statistics_;

    Stroka CurrentPath_;
    std::vector<int> DirectoryNameLengths_;

    TSummary CurrentSummary_;
    i64 FilledSummaryFields_ = 0;

    Stroka LastKey_;

    bool AtSummaryMap_ = false;
    bool AtAttributes_ = false;
};

void CreateBuildingYsonConsumer(std::unique_ptr<IBuildingYsonConsumer<TStatistics>>* buildingConsumer, EYsonType ysonType)
{
    YCHECK(ysonType == EYsonType::Node);
    *buildingConsumer = std::make_unique<TStatisticsBuildingConsumer>();
}

////////////////////////////////////////////////////////////////////

const Stroka inputPrefix = "/data/input";
const Stroka outputPrefix = "/data/output";

TDataStatistics GetTotalInputDataStatistics(const TStatistics& jobStatistics)
{
    TDataStatistics result;
    for (auto iterator = jobStatistics.Data().upper_bound(inputPrefix);
         iterator != jobStatistics.Data().end() && HasPrefix(iterator->first, inputPrefix);
         ++iterator)
    {
        SetDataStatisticsField(result, TStringBuf(iterator->first.begin() + 1 + inputPrefix.size(), iterator->first.end()), iterator->second.GetSum());
    }

    return result;
}

yhash_map<int, TDataStatistics> GetOutputDataStatistics(const TStatistics& jobStatistics)
{
    yhash_map<int, TDataStatistics> result;
    for (auto iterator = jobStatistics.Data().upper_bound(outputPrefix);
         iterator != jobStatistics.Data().end() && HasPrefix(iterator->first, outputPrefix);
         ++iterator)
    {
        TStringBuf currentPath(iterator->first.begin() + outputPrefix.size() + 1, iterator->first.end());
        size_t slashPos = currentPath.find("/");
        if (slashPos == TStringBuf::npos) {
            // Looks like a malformed path in /data/output, let's skip it.
            continue;
        }
        int tableIndex = a2i(Stroka(currentPath.substr(0, slashPos)));
        SetDataStatisticsField(result[tableIndex], currentPath.substr(slashPos + 1), iterator->second.GetSum());
    }

    return result;
}

TDataStatistics GetTotalOutputDataStatistics(const TStatistics& jobStatistics)
{
    TDataStatistics result;
    for (const auto& pair : GetOutputDataStatistics(jobStatistics)) {
        result += pair.second;
    }
    return result;
}

////////////////////////////////////////////////////////////////////

TStatisticsConsumer::TStatisticsConsumer(TSampleHandler sampleHandler)
    : TreeBuilder_(CreateBuilderFromFactory(GetEphemeralNodeFactory()))
    , SampleHandler_(sampleHandler)
{ }

void TStatisticsConsumer::OnMyListItem()
{
    TreeBuilder_->BeginTree();
    Forward(TreeBuilder_.get(), BIND(&TStatisticsConsumer::ProcessSample, this), NYson::EYsonType::Node);
}

void TStatisticsConsumer::ProcessSample()
{
    auto node = TreeBuilder_->EndTree();
    SampleHandler_.Run(node);
}

////////////////////////////////////////////////////////////////////

} // namespace NJobTrackerClient
} // namespace NYT
