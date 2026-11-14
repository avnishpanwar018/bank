#include "bank/money.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace bank {

Money Money::from_decimal(std::string_view value) {
    if (value.empty()) {
        throw std::invalid_argument("money value cannot be empty");
    }

    bool negative = false;
    std::size_t start = 0;
    if (value.front() == '-' || value.front() == '+') {
        negative = value.front() == '-';
        start = 1;
    }
    if (start == value.size()) {
        throw std::invalid_argument("money value has no digits");
    }

    const std::size_t dot = value.find('.', start);
    const std::size_t whole_end = dot == std::string_view::npos ? value.size() : dot;
    if (whole_end == start) {
        throw std::invalid_argument("money value has no whole units");
    }

    std::uint64_t whole = 0;
    const auto whole_result = std::from_chars(value.data() + start,
                                              value.data() + whole_end, whole);
    if (whole_result.ec != std::errc{} || whole_result.ptr != value.data() + whole_end) {
        throw std::invalid_argument("money whole units are invalid");
    }

    std::uint64_t cents = 0;
    if (dot != std::string_view::npos) {
        const std::size_t fractional_digits = value.size() - dot - 1;
        if (fractional_digits > 2 || fractional_digits == 0) {
            throw std::invalid_argument("money must have zero, one, or two fractional digits");
        }
        for (std::size_t index = dot + 1; index < value.size(); ++index) {
            if (value[index] < '0' || value[index] > '9') {
                throw std::invalid_argument("money fractional units are invalid");
            }
            cents = cents * 10 + static_cast<std::uint64_t>(value[index] - '0');
        }
        if (fractional_digits == 1) {
            cents *= 10;
        }
    }

    const std::uint64_t limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                                + (negative ? 1U : 0U);
    if (whole > (limit - cents) / 100U) {
        throw std::out_of_range("money value is outside int64 minor-unit range");
    }
    const std::uint64_t magnitude = whole * 100U + cents;
    if (negative && magnitude == limit) {
        return from_minor_units(std::numeric_limits<std::int64_t>::min());
    }
    const auto signed_magnitude = static_cast<std::int64_t>(magnitude);
    return from_minor_units(negative ? -signed_magnitude : signed_magnitude);
}

std::string Money::to_decimal() const {
    const bool negative = minor_units_ < 0;
    const std::uint64_t magnitude = negative
        ? static_cast<std::uint64_t>(-(minor_units_ + 1)) + 1U
        : static_cast<std::uint64_t>(minor_units_);
    const std::uint64_t whole = magnitude / 100U;
    const std::uint64_t cents = magnitude % 100U;
    std::string result = (negative ? "-" : "") + std::to_string(whole) + ".";
    if (cents < 10U) {
        result.push_back('0');
    }
    result += std::to_string(cents);
    return result;
}

}  // namespace bank
