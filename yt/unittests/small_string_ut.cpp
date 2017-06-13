//===- llvm/unittest/ADT/SmallStringTest.cpp ------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// TSmallString unit tests.
//
//===----------------------------------------------------------------------===//

#include "framework.h"

#include <yt/core/misc/small_string.h>
#include <yt/core/misc/small_vector.h>

#include <util/generic/strbuf.h>
#include <util/generic/string.h>

#include <climits>
#include <cstring>

#include <stdarg.h>

namespace NYT {
namespace {

// Test fixture class
class TSmallStringTest : public testing::Test {
protected:
  typedef TSmallString<40> StringType;

  StringType theString;

  void assertEmpty(StringType & v) {
    // Size tests
    EXPECT_EQ(0u, v.size());
    EXPECT_TRUE(v.empty());
    // Iterator tests
    EXPECT_TRUE(v.begin() == v.end());
  }
};

// New string test.
TEST_F(TSmallStringTest, EmptyStringTest) {
  SCOPED_TRACE("EmptyStringTest");
  assertEmpty(theString);
  EXPECT_TRUE(theString.rbegin() == theString.rend());
}

TEST_F(TSmallStringTest, AssignRepeated) {
  theString.assign(3, 'a');
  EXPECT_EQ(3u, theString.size());
  EXPECT_STREQ("aaa", theString.c_str());
}

TEST_F(TSmallStringTest, AssignIterPair) {
  TStringBuf abc = "abc";
  theString.assign(abc.begin(), abc.end());
  EXPECT_EQ(3u, theString.size());
  EXPECT_STREQ("abc", theString.c_str());
}

TEST_F(TSmallStringTest, AssignStringRef) {
  TStringBuf abc = "abc";
  theString.assign(abc);
  EXPECT_EQ(3u, theString.size());
  EXPECT_STREQ("abc", theString.c_str());
}

TEST_F(TSmallStringTest, AssignSmallVector) {
  TStringBuf abc = "abc";
  SmallVector<char, 10> abcVec(abc.begin(), abc.end());
  theString.assign(abcVec);
  EXPECT_EQ(3u, theString.size());
  EXPECT_STREQ("abc", theString.c_str());
}

TEST_F(TSmallStringTest, AppendIterPair) {
  TStringBuf abc = "abc";
  theString.append(abc.begin(), abc.end());
  theString.append(abc.begin(), abc.end());
  EXPECT_EQ(6u, theString.size());
  EXPECT_STREQ("abcabc", theString.c_str());
}

TEST_F(TSmallStringTest, AppendStringRef) {
  TStringBuf abc = "abc";
  theString.append(abc);
  theString.append(abc);
  EXPECT_EQ(6u, theString.size());
  EXPECT_STREQ("abcabc", theString.c_str());
}

TEST_F(TSmallStringTest, AppendSmallVector) {
  TStringBuf abc = "abc";
  SmallVector<char, 10> abcVec(abc.begin(), abc.end());
  theString.append(abcVec);
  theString.append(abcVec);
  EXPECT_EQ(6u, theString.size());
  EXPECT_STREQ("abcabc", theString.c_str());
}

TEST_F(TSmallStringTest, Substr) {
  theString = "hello";
  EXPECT_EQ("lo", theString.substr(3));
  EXPECT_EQ("", theString.substr(100));
  EXPECT_EQ("hello", theString.substr(0, 100));
  EXPECT_EQ("o", theString.substr(4, 10));
}


// slice is not supported
//TEST_F(TSmallStringTest, Slice) {
//  theString = "hello";
//  EXPECT_EQ("l", theString.slice(2, 3));
//  EXPECT_EQ("ell", theString.slice(1, 4));
//  EXPECT_EQ("llo", theString.slice(2, 100));
//  EXPECT_EQ("", theString.slice(2, 1));
//  EXPECT_EQ("", theString.slice(10, 20));
//}

TEST_F(TSmallStringTest, Find) {
  theString = "hello";
  EXPECT_EQ(2U, theString.find('l'));
  EXPECT_EQ(std::string::npos, theString.find('z'));
  EXPECT_EQ(std::string::npos, theString.find("helloworld"));
  EXPECT_EQ(0U, theString.find("hello"));
  EXPECT_EQ(1U, theString.find("ello"));
  EXPECT_EQ(std::string::npos, theString.find("zz"));
  EXPECT_EQ(2U, theString.find("ll", 2));
  EXPECT_EQ(std::string::npos, theString.find("ll", 3));
  EXPECT_EQ(0U, theString.find(""));

  EXPECT_EQ(3U, theString.rfind('l'));
  EXPECT_EQ(std::string::npos, theString.rfind('z'));
// rfind is not supported
//  EXPECT_EQ(std::string::npos, theString.rfind("helloworld"));
//  EXPECT_EQ(0U, theString.rfind("hello"));
//  EXPECT_EQ(1U, theString.rfind("ello"));
//  EXPECT_EQ(std::string::npos, theString.rfind("zz"));

  EXPECT_EQ(2U, theString.find_first_of('l'));
  EXPECT_EQ(1U, theString.find_first_of("el"));
  EXPECT_EQ(std::string::npos, theString.find_first_of("xyz"));

  EXPECT_EQ(1U, theString.find_first_not_of('h'));
  EXPECT_EQ(4U, theString.find_first_not_of("hel"));
  EXPECT_EQ(std::string::npos, theString.find_first_not_of("hello"));

  theString = "hellx xello hell ello world foo bar hello";
  EXPECT_EQ(36U, theString.find("hello"));
  EXPECT_EQ(28U, theString.find("foo"));
  EXPECT_EQ(12U, theString.find("hell", 2));
  EXPECT_EQ(0U, theString.find(""));
}

// count is not supported
//TEST_F(TSmallStringTest, Count) {
//  theString = "hello";
//  EXPECT_EQ(2U, theString.count('l'));
//  EXPECT_EQ(1U, theString.count('o'));
//  EXPECT_EQ(0U, theString.count('z'));
//  EXPECT_EQ(0U, theString.count("helloworld"));
//  EXPECT_EQ(1U, theString.count("hello"));
//  EXPECT_EQ(1U, theString.count("ello"));
//  EXPECT_EQ(0U, theString.count("zz"));
//}

TEST(TStringRefTest, Comparisons) {
  EXPECT_EQ(-1, TSmallString<10>("aab").compare("aad"));
  EXPECT_EQ( 0, TSmallString<10>("aab").compare("aab"));
  EXPECT_EQ( 1, TSmallString<10>("aab").compare("aaa"));
  EXPECT_EQ(-1, TSmallString<10>("aab").compare("aabb"));
  EXPECT_EQ( 1, TSmallString<10>("aab").compare("aa"));
  EXPECT_EQ( 1, TSmallString<10>("\xFF").compare("\1"));

// compare lower is not supported
//  EXPECT_EQ(-1, TSmallString<10>("AaB").compare_lower("aAd"));
//  EXPECT_EQ( 0, TSmallString<10>("AaB").compare_lower("aab"));
//  EXPECT_EQ( 1, TSmallString<10>("AaB").compare_lower("AAA"));
//  EXPECT_EQ(-1, TSmallString<10>("AaB").compare_lower("aaBb"));
//  EXPECT_EQ( 1, TSmallString<10>("AaB").compare_lower("aA"));
//  EXPECT_EQ( 1, TSmallString<10>("\xFF").compare_lower("\1"));

//  EXPECT_EQ(-1, TSmallString<10>("aab").compare_numeric("aad"));
//  EXPECT_EQ( 0, TSmallString<10>("aab").compare_numeric("aab"));
//  EXPECT_EQ( 1, TSmallString<10>("aab").compare_numeric("aaa"));
//  EXPECT_EQ(-1, TSmallString<10>("aab").compare_numeric("aabb"));
//  EXPECT_EQ( 1, TSmallString<10>("aab").compare_numeric("aa"));
//  EXPECT_EQ(-1, TSmallString<10>("1").compare_numeric("10"));
//  EXPECT_EQ( 0, TSmallString<10>("10").compare_numeric("10"));
//  EXPECT_EQ( 0, TSmallString<10>("10a").compare_numeric("10a"));
//  EXPECT_EQ( 1, TSmallString<10>("2").compare_numeric("1"));
//  EXPECT_EQ( 0, TSmallString<10>("llvm_v1i64_ty").compare_numeric("llvm_v1i64_ty"));
//  EXPECT_EQ( 1, TSmallString<10>("\xFF").compare_numeric("\1"));
//  EXPECT_EQ( 1, TSmallString<10>("V16").compare_numeric("V1_q0"));
//  EXPECT_EQ(-1, TSmallString<10>("V1_q0").compare_numeric("V16"));
//  EXPECT_EQ(-1, TSmallString<10>("V8_q0").compare_numeric("V16"));
//  EXPECT_EQ( 1, TSmallString<10>("V16").compare_numeric("V8_q0"));
//  EXPECT_EQ(-1, TSmallString<10>("V1_q0").compare_numeric("V8_q0"));
//  EXPECT_EQ( 1, TSmallString<10>("V8_q0").compare_numeric("V1_q0"));
}

} // namespace
} // namespace NYT
