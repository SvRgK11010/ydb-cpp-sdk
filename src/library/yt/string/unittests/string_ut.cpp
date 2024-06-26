#include <src/library/testing/gtest/gtest.h>

#include <src/library/yt/string/string.h>

namespace NYT {
namespace {

////////////////////////////////////////////////////////////////////////////////

struct TTestCase
{
    const char* UnderCase;
    const char* CamelCase;
};

static std::vector<TTestCase> TestCases {
    { "kenny", "Kenny" },
    { "south_park", "SouthPark" },
    { "a", "A" },
    { "a_b_c", "ABC" },
    { "reed_solomon_6_3", "ReedSolomon_6_3" },
    { "lrc_12_2_2", "Lrc_12_2_2" },
    { "0", "0" },
    { "0_1_2", "0_1_2" },
    { "int64", "Int64" }
};

////////////////////////////////////////////////////////////////////////////////

TEST(std::stringTest, UnderscoreCaseToCamelCase)
{
    for (const auto& testCase : TestCases) {
        auto result = UnderscoreCaseToCamelCase(testCase.UnderCase);
        EXPECT_STREQ(testCase.CamelCase, result.c_str())
            << "Original: \"" << testCase.UnderCase << '"';
    }
}

TEST(std::stringTest, CamelCaseToUnderscoreCase)
{
    for (const auto& testCase : TestCases) {
        auto result = CamelCaseToUnderscoreCase(testCase.CamelCase);
        EXPECT_STREQ(testCase.UnderCase, result.c_str())
            << "Original: \"" << testCase.CamelCase << '"';
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT

