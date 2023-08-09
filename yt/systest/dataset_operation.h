#pragma once

#include <yt/systest/dataset.h>
#include <yt/systest/operation.h>

namespace NYT::NTest {

std::unique_ptr<IMultiMapper> CreateRandomMap(
    std::mt19937& randomEngine, int seed, const TStoredDataset& info);

std::unique_ptr<IMultiMapper> GenerateMultipleColumns(const TTable& table, int RowMultipler, int seed);

}  // namespace NYT::NTest