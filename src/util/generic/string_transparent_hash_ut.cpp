#include "string.h"
#include "vector.h"

#include <src/library/testing/unittest/registar.h>
#include <src/library/containers/absl_flat_hash/flat_hash_set.h>

#include <ydb-cpp-sdk/util/str_stl.h>

Y_UNIT_TEST_SUITE(StringHashFunctorTests) {
    Y_UNIT_TEST(TestTransparencyWithUnorderedSet) {
        // Using Abseil hash set because `std::unordered_set` is transparent only from C++20 (while
        // we stuck with C++17 right now).
        absl::flat_hash_set<std::string, THash<std::string>, TEqualTo<std::string>> s = {"foo"};
        // If either `THash` or `TEqualTo` is not transparent compilation will fail.
        UNIT_ASSERT_UNEQUAL(s.find(std::string_view("foo")), s.end());
        UNIT_ASSERT_EQUAL(s.find(std::string_view("bar")), s.end());
    }
}
