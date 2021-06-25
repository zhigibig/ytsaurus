#include "tag_registry.h"

#include <yt/yt/core/misc/assert.h>

#include <yt/yt/core/profiling/profile_manager.h>

namespace NYT::NProfiling {

////////////////////////////////////////////////////////////////////////////////

TTagIdList TTagRegistry::Encode(const TTagList& tags)
{
    TTagIdList ids;

    for (const auto& tag : tags) {
        if (auto it = TagByName_.find(tag); it != TagByName_.end()) {
            ids.push_back(it->second);
        } else {
            TagById_.push_back(tag);
            TagByName_[tag] = TagById_.size();
            ids.push_back(TagById_.size());
        }
    }

    return ids;
}

TTagId TTagRegistry::Encode(const TTag& tag)
{
    if (auto it = TagByName_.find(tag); it != TagByName_.end()) {
        return it->second;
    } else {
        TagById_.push_back(tag);
        TagByName_[tag] = TagById_.size();
        return TagById_.size();
    }
}

TTagIdList TTagRegistry::Encode(const TTagSet& tags)
{
    return Encode(tags.Tags());
}

SmallVector<std::optional<TTagId>, TypicalTagCount> TTagRegistry::TryEncode(const TTagList& tags) const
{
    SmallVector<std::optional<TTagId>, TypicalTagCount> ids;

    for (const auto& tag : tags) {
        if (auto it = TagByName_.find(tag); it != TagByName_.end()) {
            ids.push_back(it->second);
        } else {
            ids.push_back({});
        }
    }

    return ids;
}

const TTag& TTagRegistry::Decode(TTagId tagId) const
{
    if (tagId < 1 || static_cast<size_t>(tagId) > TagById_.size()) {
        THROW_ERROR_EXCEPTION("Invalid tag")
            << TErrorAttribute("tag_id", tagId);
    }

    return TagById_[tagId - 1];
}

int TTagRegistry::GetSize() const
{
    return TagById_.size();
}

THashMap<TString, int> TTagRegistry::TopByKey() const
{
    THashMap<TString, int> counts;
    for (const auto& [key, value] : TagById_) {
        counts[key]++;
    }
    return counts;
}

TTagIdList TTagRegistry::EncodeLegacy(const TTagIdList& tagIds)
{
    TTagIdList legacy;
    
    for (auto tag : tagIds) {
        if (auto it = LegacyTags_.find(tag); it != LegacyTags_.end()) {
            legacy.push_back(it->second);
            continue;
        }

        const auto& [key, value] = Decode(tag);
        auto legacyTagId = TProfileManager::Get()->RegisterTag(key, value);
        LegacyTags_[tag] = legacyTagId;
        legacy.push_back(legacyTagId);
    }

    return legacy;
}

void TTagRegistry::DumpTags(NProto::TSensorDump* dump)
{
    dump->add_tags();

    for (int i = 0; i < std::ssize(TagById_); i++) {
        auto tag = dump->add_tags();
        tag->set_key(TagById_[i].first);
        tag->set_value(TagById_[i].second);
    }
}

////////////////////////////////////////////////////////////////////////////////

void TTagWriter::WriteLabel(TTagId tag)
{
    if (static_cast<size_t>(tag) >= Cache_.size()) {
        Cache_.resize(tag + 1);
    }

    auto& translation = Cache_[tag];
    if (!translation) {
        const auto& tagStr = Registry_.Decode(tag);
        translation = Encoder_->PrepareLabel(tagStr.first, tagStr.second);
    }

    Encoder_->OnLabel(translation->first, translation->second);
}

const TTag& TTagWriter::Decode(TTagId tagId) const
{
    return Registry_.Decode(tagId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NProfiling
