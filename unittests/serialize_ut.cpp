#include "framework.h"

#include <yt/core/misc/serialize.h>

#include <yt/core/ytree/convert.h>

#include <array>

namespace NYT {
namespace NYTree {
namespace {

using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

Stroka RemoveSpaces(const Stroka& str)
{
    Stroka res = str;
    while (true) {
        size_t pos = res.find(" ");
        if (pos == Stroka::npos) {
            break;
        }
        res.replace(pos, 1, "");
    }
    return res;
}

TEST(TYTreeSerializationTest, All)
{
    TYsonString canonicalYson(
        "<\"acl\"={\"execute\"=[\"*\";];};>"
        "{\"mode\"=755;\"path\"=\"/home/sandello\";}"
    );
    auto root = ConvertToNode(canonicalYson);
    auto deserializedYson = ConvertToYsonString(root, NYson::EYsonFormat::Text);
    EXPECT_EQ(RemoveSpaces(canonicalYson.Data()), deserializedYson.Data());
}

TEST(TCustomTypeSerializationTest, TInstant)
{
    TInstant value = TInstant::MilliSeconds(100500);
    auto yson = ConvertToYsonString(value);
    auto deserializedValue = ConvertTo<TInstant>(yson);
    EXPECT_EQ(value, deserializedValue);
}

TEST(TCustomTypeSerializationTest, TNullable)
{
    {
        TNullable<int> value(10);
        auto yson = ConvertToYsonString(value);
        EXPECT_EQ(10, ConvertTo<TNullable<int>>(yson));
    }
    {
        TNullable<int> value = Null;
        auto yson = ConvertToYsonString(value);
        EXPECT_EQ(Stroka("#"), yson.Data());
        EXPECT_EQ(value, ConvertTo<TNullable<int>>(yson));
    }
}

TEST(TSerializationTest, PackRefs)
{
    std::vector<TSharedRef> refs;
    refs.push_back(TSharedRef::FromString("abc"));
    refs.push_back(TSharedRef::FromString("12"));

    TSharedRef packed = PackRefs(refs);
    std::vector<TSharedRef> unpacked;
    UnpackRefs(packed, &unpacked);

    EXPECT_EQ(unpacked.size(), 2);
    EXPECT_EQ(ToString(unpacked[0]), "abc");
    EXPECT_EQ(ToString(unpacked[1]), "12");
}

TEST(TSerializationTest, Map)
{
    std::map<Stroka, size_t> original{{"First", 12U}, {"Second", 7883U}, {"Third", 7U}};
    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<std::decay<decltype(original)>::type>(yson);
    EXPECT_EQ(original, deserialized);
}

TEST(TSerializationTest, Set)
{
    std::set<Stroka> original{"First", "Second", "Third"};
    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<std::decay<decltype(original)>::type>(yson);
    EXPECT_EQ(original, deserialized);
}

TEST(TSerializationTest, MultiSet)
{
    std::multiset<Stroka> original{"First", "Second", "Third", "Second", "Third", "Third"};
    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<std::decay<decltype(original)>::type>(yson);
    EXPECT_EQ(original, deserialized);
}

TEST(TSerializationTest, MultiMap)
{
    std::multimap<Stroka, size_t> original{{"First", 12U}, {"Second", 7883U}, {"Third", 7U}};
    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<std::decay<decltype(original)>::type>(yson);
    EXPECT_EQ(original, deserialized);
}

TEST(TSerializationTest, MultiMapErrorDuplicateKey)
{
    std::multimap<Stroka, size_t> original{{"First", 12U}, {"Second", 7883U}, {"First", 2U}, {"Second", 3U}};
    auto yson = ConvertToYsonString(original);
    EXPECT_THROW(ConvertTo<std::decay<decltype(original)>::type>(yson), std::exception);
}

TEST(TSerializationTest, UnorderedMap)
{
    std::unordered_map<Stroka, size_t> original{{"First", 12U}, {"Second", 7883U}, {"Third", 7U}};
    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<std::decay<decltype(original)>::type>(yson);
    EXPECT_EQ(original, deserialized);
}

TEST(TSerializationTest, UnorderedSet)
{
    const std::unordered_set<Stroka> original{"First", "Second", "Third"};
    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<std::decay<decltype(original)>::type>(yson);
    EXPECT_EQ(original, deserialized);
}

TEST(TSerializationTest, UnorderedMultiSet)
{
    const std::unordered_multiset<Stroka> original{"First", "Second", "Third", "Second", "Third", "Third"};
    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<std::decay<decltype(original)>::type>(yson);
    EXPECT_EQ(original, deserialized);
}

TEST(TSerializationTest, UnorderedMultiMap)
{
    const std::unordered_multimap<Stroka, size_t> original{{"First", 12U}, {"Second", 7883U}, {"Third", 7U}};
    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<std::decay<decltype(original)>::type>(yson);
    EXPECT_EQ(original, deserialized);
}

TEST(TSerializationTest, UnorderedMultiMapErrorDuplicateKey)
{
    const std::unordered_multimap<Stroka, size_t> original{{"Second", 7883U}, {"Third", 7U}, {"Second", 7U}};
    auto yson = ConvertToYsonString(original);
    EXPECT_THROW(ConvertTo<std::decay<decltype(original)>::type>(yson), std::exception);
}

TEST(TSerializationTest, Vector)
{
    const std::vector<Stroka> original{"First", "Second", "Third"};
    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<std::decay<decltype(original)>::type>(yson);
    EXPECT_EQ(original, deserialized);
}

TEST(TSerializationTest, Pair)
{
    auto original = std::make_pair<size_t, Stroka>(1U, "Second");
    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<std::decay<decltype(original)>::type>(yson);
    EXPECT_EQ(original, deserialized);
}

TEST(TSerializationTest, Atomic)
{
    std::atomic<size_t> original(42U);
    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<size_t>(yson);
    EXPECT_EQ(original, deserialized);
}

TEST(TSerializationTest, Array)
{
    std::array<Stroka, 4> original{{"One", "Two", "3", "4"}};
    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<std::decay<decltype(original)>::type>(yson);
    EXPECT_EQ(original, deserialized);
}

TEST(TSerializationTest, Tuple)
{
    auto original = std::make_tuple<int, Stroka, size_t>(43, "Stroka", 343U);
    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<std::decay<decltype(original)>::type>(yson);
    EXPECT_EQ(original, deserialized);
}

TEST(TSerializationTest, VectorOfTuple)
{
    std::vector<std::tuple<int, Stroka, size_t>> original{
        std::make_tuple<int, Stroka, size_t>(43, "First", 343U),
        std::make_tuple<int, Stroka, size_t>(0, "Second", 7U),
        std::make_tuple<int, Stroka, size_t>(2323, "Third", 9U)
    };

    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<std::decay<decltype(original)>::type>(yson);
    EXPECT_EQ(original, deserialized);
}

TEST(TSerializationTest, MapOnArray)
{
    std::map<Stroka, std::array<size_t, 3>> original{
        {"1", {{2112U, 4343U, 5445U}}},
        {"22", {{54654U, 93U, 5U}}},
        {"333", {{7U, 93U, 9U}}},
        {"rel", {{233U, 9763U, 0U}}}
    };

    auto yson = ConvertToYsonString(original);
    auto deserialized = ConvertTo<std::decay<decltype(original)>::type>(yson);
    EXPECT_EQ(original, deserialized);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYTree
} // namespace NYT
