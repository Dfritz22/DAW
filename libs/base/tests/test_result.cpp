#include "base/result.h"

#include <gtest/gtest.h>

#include <string>

using daw::base::Err;
using daw::base::Result;

namespace {

TEST(Result, HoldsValue) {
    Result<int, std::string> r = 42;
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value(), 42);
}

TEST(Result, HoldsError) {
    Result<int, std::string> r = Err{std::string{"nope"}};
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), "nope");
}

TEST(Result, ValueOrFallback) {
    Result<int, std::string> err = Err{std::string{"x"}};
    EXPECT_EQ(err.value_or(7), 7);

    Result<int, std::string> ok = 3;
    EXPECT_EQ(ok.value_or(7), 3);
}

TEST(Result, MoveOnlyPayload) {
    Result<std::unique_ptr<int>, std::string> r = std::make_unique<int>(9);
    ASSERT_TRUE(r.has_value());
    auto p = std::move(r).value();
    ASSERT_TRUE(p);
    EXPECT_EQ(*p, 9);
}

TEST(Result, VoidResultOk) {
    Result<void, std::string> r;
    EXPECT_TRUE(r.has_value());
}

TEST(Result, VoidResultErr) {
    Result<void, std::string> r = Err{std::string{"bad"}};
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), "bad");
}

} // namespace
