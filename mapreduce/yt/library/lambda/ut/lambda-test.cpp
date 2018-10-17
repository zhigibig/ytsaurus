#include <mapreduce/yt/library/lambda/yt_lambda.h>
#include <mapreduce/yt/library/lambda/ut/dispersion.pb.h>

#include <mapreduce/yt/tests/yt_unittest_lib/yt_unittest_lib.h>
#include <library/unittest/registar.h>

using namespace NYT;
using namespace NYT::NTesting;

static void CreateTable(IClientPtr client, TString tableName, const TVector<TNode>& table) {
    auto writer = client->CreateTableWriter<TNode>(tableName);
    for (auto& elem : table) {
        writer->AddRow(elem);
    }
    writer->Finish();
}

static void CompareTable(IClientPtr client, TString tableName, const TVector<TNode>& expected) {
    auto reader = client->CreateTableReader<TNode>(tableName);
    TVector<TNode> table;
    for (; reader->IsValid(); reader->Next()) {
        table.push_back(reader->GetRow());
    }
    UNIT_ASSERT_VALUES_EQUAL(expected, table);
}

const TVector<TNode> InputTableData = {
    TNode()("Key", "first")("Val", 1u),
    TNode()("Key", "second")("Val", 20u),
    TNode()("Key", "third")("Val", 300u),
    TNode()("Key", "first")("Val", 4000u),
};

// Comparison with this works nicely only because (integer)/2.f is quite round
// in binary representation of float. Also note that sigma({N, 1}) == (N - 1)/2.
TVector<TNode> ExpectedOutputStatistics = {
    TNode()("key", "first")("mean", 2000.5)("sigma", 1999.5),
    TNode()("key", "second")("mean", 20.)("sigma", 0.),
    TNode()("key", "third")("mean", 300.)("sigma", 0.),
};

TVector<TNode> ExpectedOutputNF = {
    TNode()("key", "first")("count", 2u)("sum", 4001.)("sum_squared", 16000001.),
    TNode()("key", "second")("count", 1u)("sum", 20.)("sum_squared", 400.),
    TNode()("key", "third")("count", 1u)("sum", 300.)("sum_squared", 90000.),
};

Y_UNIT_TEST_SUITE(Lambda) {
    Y_UNIT_TEST(CopyIf) {
        // Note that constants need not to be captured
        static constexpr ui64 LIMIT = 100;

        auto client = CreateTestClient();
        CreateTable(client, "//testing/input", InputTableData);

        CopyIf<TNode>(client, "//testing/input",  "//testing/output",
            [](auto& row) { return row["Val"].AsUint64() < LIMIT; });

        TVector<TNode> expectedOutput = {
            TNode()("Key", "first")("Val", 1u),
            TNode()("Key", "second")("Val", 20u),
        };

        CompareTable(client, "//testing/output", expectedOutput);
    }

    Y_UNIT_TEST(TransformCopyIf) {
        static constexpr ui64 LIMIT = 1000;
        auto client = CreateTestClient();
        CreateTable(client, "//testing/input", InputTableData);

        TransformCopyIf<TNode, TNode>(client, "//testing/input",  "//testing/output",
            [](auto& src, auto& dst) {
                if (src["Val"].AsUint64() >= LIMIT)
                    return false;
                dst["Key1"] = src["Key"];
                dst["Key2"] = src["Key"].AsString() + "Stuff";
                dst["Val"] = src["Val"];
                return true;
            });

        TVector<TNode> expectedOutput = {
            TNode()("Key1", "first")("Key2", "firstStuff")("Val", 1u),
            TNode()("Key1", "second")("Key2", "secondStuff")("Val", 20u),
            TNode()("Key1", "third")("Key2", "thirdStuff")("Val", 300u),
        };

        CompareTable(client, "//testing/output", expectedOutput);
    }

    Y_UNIT_TEST(AdditiveMapReduceSorted) {
        auto client = CreateTestClient();
        CreateTable(client, "//testing/input", InputTableData);

        AdditiveMapReduceSorted<TNode, TNode>(client, "//testing/input",  "//testing/output",
            { "Key1", "Key2" },
            [](auto& src, auto& dst) {
                dst["Key1"] = src["Key"];
                dst["Key2"] = TString(src["Key"].AsString().back()) + src["Key"].AsString();
                dst["Val"] = src["Val"];
                return true;
            },
            [](auto& src, auto& dst) {
                dst["Val"] = dst["Val"].AsUint64() + src["Val"].AsUint64();
            });

        TVector<TNode> expectedOutput = {
            TNode()("Key1", "first")("Key2", "tfirst")("Val", 4001u),
            TNode()("Key1", "second")("Key2", "dsecond")("Val", 20u),
            TNode()("Key1", "third")("Key2", "dthird")("Val", 300u),
        };

        CompareTable(client, "//testing/output", expectedOutput);
    }

    // * We could decrate this structure inside the function that uses it.
    //   But that will result in quite an unreadable job title.
    // * TDispersionDataMsg could be used instead of this structure,
    //   but look how clean the code is without Get/Set stuff.
    struct TDispersionData {
        ui64 Count = 0;
        long double Sum = 0.;
        long double SumSquared = 0.;
    };

    Y_UNIT_TEST(MapReduceSorted) {
        auto client = CreateTestClient();
        CreateTable(client, "//testing/input", InputTableData);

        MapReduceSorted<TNode, TSimpleKeyValue, TDispersionData, TKeyStat>(
            client,
            "//testing/input",  "//testing/output",
            "key",
            [](auto& src, auto& dst) { // mapper
                dst.SetKey(src["Key"].AsString());
                dst.SetValue(src["Val"].AsUint64());
                return true;
            },
            [](auto& src, auto& dst) { // reducer
                double value = src.GetValue();
                dst.Count++;
                dst.Sum += value;
                dst.SumSquared += value * value;
            },
            [](auto& src, auto& dst) { // finalizer
                double mean = (double)src.Sum / src.Count;
                double dispersion = (double)src.SumSquared / src.Count - mean * mean;
                dst.SetMean(mean);
                dst.SetSigma(std::sqrt(dispersion));
            });

        CompareTable(client, "//testing/output", ExpectedOutputStatistics);
    }

    Y_UNIT_TEST(MapReduceCombinedSorted) {
        auto client = CreateTestClient();
        CreateTable(client, "//testing/input", InputTableData);

        MapReduceCombinedSorted<TNode, TSimpleKeyValue, TDispersionDataMsg, TKeyStat>(
            client,
            "//testing/input",  "//testing/output",
            "key",
            [](auto& src, auto& dst) { // mapper
                dst.SetKey(src["Key"].AsString());
                dst.SetValue(src["Val"].AsUint64());
                return true;
            },
            [](auto& src, auto& dst) { // combiner
                double value = src.GetValue();
                dst.SetCount(dst.GetCount() + 1);
                dst.SetSum(dst.GetSum() + value);
                dst.SetSumSquared(dst.GetSumSquared() + value * value);
            },
            [](auto& src, auto& dst) { // reducer
                dst.SetCount(src.GetCount() + dst.GetCount());
                dst.SetSum(src.GetSum() + dst.GetSum());
                dst.SetSumSquared(src.GetSumSquared() + dst.GetSumSquared());
            },
            [](auto& src, auto& dst) { // finalizer
                double mean = src.GetSum() / src.GetCount();
                double dispersion = src.GetSumSquared() / src.GetCount() - mean * mean;
                dst.SetMean(mean);
                dst.SetSigma(std::sqrt(dispersion));
            });

        CompareTable(client, "//testing/output", ExpectedOutputStatistics);
    }

    Y_UNIT_TEST(MapReduceSortedNoFinalizer) {
        auto client = CreateTestClient();
        CreateTable(client, "//testing/input", InputTableData);

        MapReduceSorted<TNode, TSimpleKeyValue, TDispersionDataMsg>(
            client,
            "//testing/input",  "//testing/output",
            "key",
            [](auto& src, auto& dst) { // mapper
                dst.SetKey(src["Key"].AsString());
                dst.SetValue(src["Val"].AsUint64());
                return true;
            },
            [](auto& src, auto& dst) { // reducer
                double value = src.GetValue();
                dst.SetCount(dst.GetCount() + 1);
                dst.SetSum(dst.GetSum() + value);
                dst.SetSumSquared(dst.GetSumSquared() + value * value);
            });

        CompareTable(client, "//testing/output", ExpectedOutputNF);
    }

    Y_UNIT_TEST(MapReduceCombinedSortedNoFinalizer) {
        auto client = CreateTestClient();
        CreateTable(client, "//testing/input", InputTableData);

        MapReduceCombinedSorted<TNode, TSimpleKeyValue, TDispersionDataMsg>(
            client,
            "//testing/input",  "//testing/output",
            "key",
            [](auto& src, auto& dst) { // mapper
                dst.SetKey(src["Key"].AsString());
                dst.SetValue(src["Val"].AsUint64());
                return true;
            },
            [](auto& src, auto& dst) { // combiner
                double value = src.GetValue();
                dst.SetCount(dst.GetCount() + 1);
                dst.SetSum(dst.GetSum() + value);
                dst.SetSumSquared(dst.GetSumSquared() + value * value);
            },
            [](auto& src, auto& dst) { // reducer
                dst.SetCount(src.GetCount() + dst.GetCount());
                dst.SetSum(src.GetSum() + dst.GetSum());
                dst.SetSumSquared(src.GetSumSquared() + dst.GetSumSquared());
            });

        CompareTable(client, "//testing/output", ExpectedOutputNF);
    }
}
