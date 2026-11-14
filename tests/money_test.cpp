#include "bank/money.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace bank {

TEST(MoneyTest, ParsesAndFormatsMinorUnitsWithoutFloatingPoint) {
    EXPECT_EQ(Money::from_decimal("100000.00").minor_units(), 10000000);
    EXPECT_EQ(Money::from_decimal("12.3").to_decimal(), "12.30");
    EXPECT_EQ(Money::from_decimal("-0.01").minor_units(), -1);
    EXPECT_EQ(Money::from_decimal("-92233720368547758.08").minor_units(),
              std::numeric_limits<std::int64_t>::min());
}

TEST(MoneyTest, RejectsMoreThanTwoFractionalDigits) {
    EXPECT_THROW(Money::from_decimal("1.234"), std::invalid_argument);
}

}  // namespace bank
