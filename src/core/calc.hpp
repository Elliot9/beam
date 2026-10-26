// Inline calculator: "12*(3+4)", "2^10", "sqrt(2)", "0xff + 1", "5!".
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace beam {

// Pure evaluation; nullopt on syntax error, unknown identifier or non-finite result.
std::optional<double> EvaluateExpression(std::wstring_view input);

// Heuristic gate so plain words / numbers ("7-zip", "2048") are not treated as math.
bool LooksLikeMath(std::wstring_view input);

// LooksLikeMath + EvaluateExpression.
std::optional<double> TryCalculate(std::wstring_view input);

std::wstring FormatNumber(double v);         // "1234.5", "1.5e+20"
std::wstring FormatNumberGrouped(double v);  // "1,234.5" (display only)
std::wstring FormatProgrammer(double v);     // "0xFF · 0b11111111" for integers, else empty

}  // namespace beam
