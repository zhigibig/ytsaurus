#include <yt/core/test_framework/framework.h>

#include <yt/server/lib/hydra/changelog.h>
#include <yt/server/lib/hydra/config.h>
#include <yt/server/lib/hydra/format.h>
#include <yt/server/lib/hydra/async_file_changelog_index.h>

#include <yt/ytlib/chunk_client/io_engine.h>
#include <yt/ytlib/hydra/proto/hydra_manager.pb.h>

#include <yt/core/misc/fs.h>

#include <yt/core/profiling/timing.h>

#include <util/random/random.h>

#include <util/system/tempfile.h>

#include <array>

namespace NYT::NHydra {
namespace {

using namespace NHydra::NProto;

////////////////////////////////////////////////////////////////////////////////

class TAsyncFileChangelogIndexTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<std::tuple<
        NChunkClient::EIOEngineType,
        const char*>>
{
protected:
    NChunkClient::IIOEnginePtr IOEngine_;

public:
    virtual void SetUp() override
    {
        const auto& args = GetParam();
        const auto& type = std::get<0>(args);
        auto config = NYTree::ConvertTo<NYTree::INodePtr>(
            NYson::TYsonString(std::get<1>(args), NYson::EYsonType::Node));

        IOEngine_ = NChunkClient::CreateIOEngine(type, config);
    }
};

TEST_P(TAsyncFileChangelogIndexTest, Simple)
{
    TString IndexFileName = GenerateRandomFileName("TAsyncFileChangelogIndexTest.index");
    TAsyncFileChangelogIndex index(IOEngine_, IndexFileName, 4096, 16);

    index.Create();
    index.Append(0, 0, 1);
    index.Append(1, 1, 15);

    index.Append(2, 16, 1);

    index.FlushData().Get()
        .ThrowOnError();

    std::vector<int> appendSizes;
    int filePosition = 17;
    for (int i = 0; i < 1024; ++i) {
        appendSizes.push_back(16);
    }

    index.Append(3, filePosition, appendSizes);
    index.FlushData().Get()
        .ThrowOnError();

    for (auto i : appendSizes) {
        filePosition += i;
    }

    {
        TChangelogIndexRecord lowerBound{-1, -1};
        TChangelogIndexRecord upperBound{-1, -1};
        index.Search(&lowerBound, &upperBound, 0, 2);

        EXPECT_EQ(lowerBound.RecordId, 0); EXPECT_EQ(lowerBound.FilePosition, 0);
        EXPECT_EQ(upperBound.RecordId, 4); EXPECT_EQ(upperBound.FilePosition, 33);
    }


    {
        TChangelogIndexRecord lowerBound{-1, -1};
        TChangelogIndexRecord upperBound{-1, -1};
        index.Search(&lowerBound, &upperBound, 267, 2048);

        EXPECT_EQ(lowerBound.RecordId, 267); EXPECT_EQ(lowerBound.FilePosition, 4241);
        EXPECT_EQ(upperBound.RecordId, -1); EXPECT_EQ(upperBound.FilePosition, -1);
    }

    index.Close();

    TAsyncFileChangelogIndex index2(IOEngine_, IndexFileName, 4096, 16);
    index2.Read();
    index2.TruncateInvalidRecords(index2.Records().size());

    {
        TChangelogIndexRecord lowerBound{-1, -1};
        TChangelogIndexRecord upperBound{-1, -1};
        index2.Search(&lowerBound, &upperBound, 0, 2);

        EXPECT_EQ(lowerBound.RecordId, 0); EXPECT_EQ(lowerBound.FilePosition, 0);
        EXPECT_EQ(upperBound.RecordId, 4); EXPECT_EQ(upperBound.FilePosition, 33);
    }

    {
        TChangelogIndexRecord lowerBound{-1, -1};
        TChangelogIndexRecord upperBound{-1, -1};
        index2.Search(&lowerBound, &upperBound, 267, 2048);

        EXPECT_EQ(lowerBound.RecordId, 267); EXPECT_EQ(lowerBound.FilePosition, 4241);
        EXPECT_EQ(upperBound.RecordId, -1); EXPECT_EQ(upperBound.FilePosition, -1);
    }

    index2.Close();
}

INSTANTIATE_TEST_CASE_P(
    TAsyncFileChangelogIndexTest,
    TAsyncFileChangelogIndexTest,
    ::testing::Values(
        std::make_tuple(NChunkClient::EIOEngineType::ThreadPool, "{ }"),
        std::make_tuple(NChunkClient::EIOEngineType::ThreadPool, "{ use_direct_io = true; }"),
        std::make_tuple(NChunkClient::EIOEngineType::Aio, "{ }")
    )
);

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NHydra
