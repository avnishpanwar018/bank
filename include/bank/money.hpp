#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bank {

class Money {
public:
    constexpr Money() noexcept = default;

    static constexpr Money from_minor_units(std::int64_t minor_units) noexcept {
        return Money(minor_units);
    }

    static Money from_decimal(std::string_view value);

    [[nodiscard]] constexpr std::int64_t minor_units() const noexcept {
        return minor_units_;
    }

    [[nodiscard]] constexpr bool is_positive() const noexcept {
        return minor_units_ > 0;
    }

    [[nodiscard]] std::string to_decimal() const;

    friend constexpr bool operator==(Money, Money) noexcept = default;
    friend constexpr bool operator!=(Money lhs, Money rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    explicit constexpr Money(std::int64_t minor_units) noexcept
        : minor_units_(minor_units) {}

    std::int64_t minor_units_ = 0;
};

}  // namespace bank
