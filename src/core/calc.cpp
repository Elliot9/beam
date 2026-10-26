#include "core/calc.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/text.hpp"

namespace beam {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kE = 2.71828182845904523536;

bool IsOpenParen(wchar_t c) { return c == L'(' || c == 0xFF08; }
bool IsCloseParen(wchar_t c) { return c == L')' || c == 0xFF09; }
bool IsComma(wchar_t c) { return c == L',' || c == 0xFF0C; }
bool IsPlus(wchar_t c) { return c == L'+' || c == 0xFF0B; }
bool IsMinus(wchar_t c) { return c == L'-' || c == 0x2212 || c == 0xFF0D; }
bool IsTimes(wchar_t c) { return c == L'*' || c == 0x00D7 || c == 0xFF0A; }
bool IsDivide(wchar_t c) { return c == L'/' || c == 0x00F7 || c == 0xFF0F; }
bool IsIdentStart(wchar_t c) { return IsAsciiAlpha(c) || c == L'_' || c == 0x03C0; }  // π
bool IsIdentChar(wchar_t c) { return IsIdentStart(c) || IsAsciiDigit(c); }

bool CallFunction(const std::wstring& n, const std::vector<double>& a, double& v) {
    using Fn = double (*)(double);
    auto one = [&](Fn f) {
        if (a.size() != 1) return false;
        v = f(a[0]);
        return true;
    };
    auto two = [&](double (*f)(double, double)) {
        if (a.size() != 2) return false;
        v = f(a[0], a[1]);
        return true;
    };
    if (n == L"sqrt") return one([](double x) { return std::sqrt(x); });
    if (n == L"cbrt") return one([](double x) { return std::cbrt(x); });
    if (n == L"abs") return one([](double x) { return std::fabs(x); });
    if (n == L"sin") return one([](double x) { return std::sin(x); });
    if (n == L"cos") return one([](double x) { return std::cos(x); });
    if (n == L"tan") return one([](double x) { return std::tan(x); });
    if (n == L"asin") return one([](double x) { return std::asin(x); });
    if (n == L"acos") return one([](double x) { return std::acos(x); });
    if (n == L"atan") return one([](double x) { return std::atan(x); });
    if (n == L"sinh") return one([](double x) { return std::sinh(x); });
    if (n == L"cosh") return one([](double x) { return std::cosh(x); });
    if (n == L"tanh") return one([](double x) { return std::tanh(x); });
    if (n == L"exp") return one([](double x) { return std::exp(x); });
    if (n == L"ln") return one([](double x) { return std::log(x); });
    if (n == L"log2") return one([](double x) { return std::log2(x); });
    if (n == L"log10") return one([](double x) { return std::log10(x); });
    if (n == L"floor") return one([](double x) { return std::floor(x); });
    if (n == L"ceil") return one([](double x) { return std::ceil(x); });
    if (n == L"deg") return one([](double x) { return x * 180.0 / kPi; });
    if (n == L"rad") return one([](double x) { return x * kPi / 180.0; });
    if (n == L"log") {
        if (a.size() == 1) {
            v = std::log10(a[0]);
            return true;
        }
        if (a.size() == 2) {
            v = std::log(a[0]) / std::log(a[1]);
            return true;
        }
        return false;
    }
    if (n == L"round") {
        if (a.size() == 1) {
            v = std::round(a[0]);
            return true;
        }
        if (a.size() == 2) {
            const double m = std::pow(10.0, std::floor(a[1]));
            v = std::round(a[0] * m) / m;
            return true;
        }
        return false;
    }
    if (n == L"min" || n == L"max") {
        if (a.empty()) return false;
        v = a[0];
        for (double x : a) v = (n == L"min") ? std::fmin(v, x) : std::fmax(v, x);
        return true;
    }
    if (n == L"pow") return two([](double x, double y) { return std::pow(x, y); });
    if (n == L"hypot") return two([](double x, double y) { return std::hypot(x, y); });
    if (n == L"mod") {
        if (a.size() != 2 || a[1] == 0) return false;
        v = std::fmod(a[0], a[1]);
        return true;
    }
    return false;
}

class Parser {
public:
    explicit Parser(std::wstring_view s) : s_(s) {}

    std::optional<double> Run() {
        double v = 0;
        if (!Expr(v)) return std::nullopt;
        SkipWs();
        if (pos_ != s_.size() || !std::isfinite(v)) return std::nullopt;
        return v;
    }

private:
    struct DepthGuard {
        int& depth;
        bool ok;
        explicit DepthGuard(int& d) : depth(d), ok(++d <= 64) {}
        ~DepthGuard() { --depth; }
    };

    wchar_t Cur() const { return pos_ < s_.size() ? s_[pos_] : L'\0'; }
    wchar_t At(size_t k) const { return pos_ + k < s_.size() ? s_[pos_ + k] : L'\0'; }
    void SkipWs() {
        while (pos_ < s_.size() && IsSpace(s_[pos_])) ++pos_;
    }
    bool NextOperandAfter(size_t k) const {
        size_t i = pos_ + k;
        while (i < s_.size() && IsSpace(s_[i])) ++i;
        return i < s_.size() && (IsAsciiDigit(s_[i]) || s_[i] == L'.' || IsOpenParen(s_[i]));
    }

    bool Expr(double& v) {
        DepthGuard g(depth_);
        if (!g.ok || !Term(v)) return false;
        for (;;) {
            SkipWs();
            const wchar_t c = Cur();
            double r = 0;
            if (IsPlus(c)) {
                ++pos_;
                if (!Term(r)) return false;
                v += r;
            } else if (IsMinus(c)) {
                ++pos_;
                if (!Term(r)) return false;
                v -= r;
            } else {
                return true;
            }
        }
    }

    bool Term(double& v) {
        if (!Unary(v)) return false;
        for (;;) {
            SkipWs();
            const wchar_t c = Cur();
            double r = 0;
            if (IsTimes(c) && At(1) != L'*') {
                ++pos_;
                if (!Unary(r)) return false;
                v *= r;
            } else if (IsDivide(c)) {
                ++pos_;
                if (!Unary(r)) return false;
                v /= r;
            } else if (c == L'%' || c == 0xFF05) {
                ++pos_;
                if (!Unary(r) || r == 0) return false;
                v = std::fmod(v, r);
            } else if ((c == L'x' || c == L'X') && NextOperandAfter(1)) {  // 1920x1080
                ++pos_;
                if (!Unary(r)) return false;
                v *= r;
            } else if (IsOpenParen(c) || IsIdentStart(c)) {  // implicit: 2pi, 3(4+1)
                if (!Unary(r)) return false;
                v *= r;
            } else {
                return true;
            }
        }
    }

    bool Unary(double& v) {
        DepthGuard g(depth_);
        if (!g.ok) return false;
        SkipWs();
        const wchar_t c = Cur();
        if (IsMinus(c)) {
            ++pos_;
            if (!Unary(v)) return false;
            v = -v;
            return true;
        }
        if (IsPlus(c)) {
            ++pos_;
            return Unary(v);
        }
        return Power(v);
    }

    bool Power(double& base) {
        if (!Postfix(base)) return false;
        SkipWs();
        const bool caret = Cur() == L'^';
        const bool stars = Cur() == L'*' && At(1) == L'*';
        if (caret || stars) {
            pos_ += caret ? 1 : 2;
            double exponent = 0;
            if (!Unary(exponent)) return false;  // right associative, allows 2^-1
            base = std::pow(base, exponent);
        }
        return true;
    }

    bool Postfix(double& v) {
        if (!Primary(v)) return false;
        for (;;) {
            SkipWs();
            if (Cur() != L'!') return true;
            ++pos_;
            if (v < 0 || v > 170 || std::floor(v) != v) return false;
            v = std::tgamma(v + 1);
        }
    }

    bool Primary(double& v) {
        SkipWs();
        const wchar_t c = Cur();
        if (IsOpenParen(c)) {
            ++pos_;
            if (!Expr(v)) return false;
            SkipWs();
            if (!IsCloseParen(Cur())) return false;
            ++pos_;
            return true;
        }
        if (IsAsciiDigit(c) || c == L'.') return Number(v);
        if (IsIdentStart(c)) return Identifier(v);
        return false;
    }

    bool Number(double& v) {
        const wchar_t p = static_cast<wchar_t>(At(1) | 0x20);
        if (Cur() == L'0' && (p == L'x' || p == L'b' || p == L'o')) {
            const int radix = p == L'x' ? 16 : (p == L'b' ? 2 : 8);
            size_t i = pos_ + 2;
            const size_t start = i;
            double acc = 0;
            bool any = false;
            for (; i < s_.size(); ++i) {
                const wchar_t d = s_[i];
                if (d == L'_' && any) continue;
                int dv = -1;
                if (IsAsciiDigit(d)) dv = d - L'0';
                else if (d >= L'a' && d <= L'f') dv = 10 + (d - L'a');
                else if (d >= L'A' && d <= L'F') dv = 10 + (d - L'A');
                if (dv < 0 || dv >= radix) break;
                acc = acc * radix + dv;
                any = true;
            }
            if (!any || i == start) return false;
            pos_ = i;
            v = acc;
            return true;
        }

        size_t i = pos_;
        std::string buf;
        bool digits = false;
        while (i < s_.size() && (IsAsciiDigit(s_[i]) || (s_[i] == L'_' && digits))) {
            if (s_[i] != L'_') {
                buf.push_back(static_cast<char>(s_[i]));
                digits = true;
            }
            ++i;
        }
        if (i < s_.size() && s_[i] == L'.') {
            buf.push_back('.');
            ++i;
            while (i < s_.size() && IsAsciiDigit(s_[i])) {
                buf.push_back(static_cast<char>(s_[i]));
                digits = true;
                ++i;
            }
        }
        if (!digits) return false;
        if (i < s_.size() && (s_[i] == L'e' || s_[i] == L'E')) {
            size_t j = i + 1;
            std::string exponent = "e";
            if (j < s_.size() && (s_[j] == L'+' || s_[j] == L'-')) exponent.push_back(static_cast<char>(s_[j++]));
            if (j < s_.size() && IsAsciiDigit(s_[j])) {
                while (j < s_.size() && IsAsciiDigit(s_[j])) exponent.push_back(static_cast<char>(s_[j++]));
                buf += exponent;
                i = j;
            }
        }
        v = std::strtod(buf.c_str(), nullptr);
        pos_ = i;
        return true;
    }

    bool Identifier(double& v) {
        const size_t start = pos_;
        while (pos_ < s_.size() && IsIdentChar(s_[pos_])) ++pos_;
        const std::wstring name = Fold(s_.substr(start, pos_ - start));
        SkipWs();
        if (IsOpenParen(Cur())) {
            ++pos_;
            std::vector<double> args;
            SkipWs();
            if (!IsCloseParen(Cur())) {
                for (;;) {
                    double a = 0;
                    if (!Expr(a)) return false;
                    args.push_back(a);
                    SkipWs();
                    if (IsComma(Cur())) {
                        ++pos_;
                        continue;
                    }
                    if (IsCloseParen(Cur())) break;
                    return false;
                }
            }
            ++pos_;  // ')'
            return CallFunction(name, args, v);
        }
        if (name == L"pi" || name == L"π") v = kPi;
        else if (name == L"e") v = kE;
        else if (name == L"tau") v = 2 * kPi;
        else if (name == L"phi") v = 1.6180339887498948482;
        else return false;
        return true;
    }

    std::wstring_view s_;
    size_t pos_ = 0;
    int depth_ = 0;
};

}  // namespace

std::optional<double> EvaluateExpression(std::wstring_view input) {
    const std::wstring s = Fold(Trim(input));  // also maps full-width digits/operators to ASCII
    if (s.empty()) return std::nullopt;
    return Parser(s).Run();
}

bool LooksLikeMath(std::wstring_view input) {
    const std::wstring s = Fold(Trim(input));
    if (s.empty()) return false;
    bool digit = false;
    bool op = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (IsAsciiDigit(c)) digit = true;
        if (IsPlus(c) || IsMinus(c) || IsTimes(c) || IsDivide(c) || IsOpenParen(c) || IsCloseParen(c) ||
            c == L'^' || c == L'%' || c == L'!') {
            op = true;
        }
        if (c == L'x' && i > 0 && i + 1 < s.size() && IsAsciiDigit(s[i - 1]) && IsAsciiDigit(s[i + 1])) op = true;
    }
    if (!digit) return false;
    if (op) return true;
    // A bare radix literal is a conversion request: "0xff", "0b1010".
    return s.size() > 2 && s[0] == L'0' && (s[1] == L'x' || s[1] == L'b' || s[1] == L'o');
}

std::optional<double> TryCalculate(std::wstring_view input) {
    if (!LooksLikeMath(input)) return std::nullopt;
    return EvaluateExpression(input);
}

std::wstring FormatNumber(double v) {
    if (v == 0 || !std::isfinite(v)) return L"0";
    char buf[96];
    const double av = std::fabs(v);
    if (av >= 1e15 || av < 1e-6) {
        std::snprintf(buf, sizeof buf, "%.10g", v);
        return Utf8ToWide(buf);
    }
    std::snprintf(buf, sizeof buf, "%.10f", v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return Utf8ToWide(s);
}

std::wstring FormatNumberGrouped(double v) {
    std::wstring s = FormatNumber(v);
    if (s.find(L'e') != std::wstring::npos) return s;
    const size_t sign = (!s.empty() && s[0] == L'-') ? 1 : 0;
    size_t end = s.find(L'.');
    if (end == std::wstring::npos) end = s.size();
    for (size_t i = end; i > sign + 3; i -= 3) s.insert(i - 3, 1, L',');
    return s;
}

std::wstring FormatProgrammer(double v) {
    if (!std::isfinite(v) || std::floor(v) != v || std::fabs(v) >= 9.0e18) return L"";
    const long long n = static_cast<long long>(v);
    char hex[48];
    std::snprintf(hex, sizeof hex, "0x%llX", static_cast<unsigned long long>(n));
    std::string out = hex;
    if (n >= 0 && n < 65536) {
        std::string bits;
        for (int i = 15; i >= 0; --i) bits.push_back(((n >> i) & 1) ? '1' : '0');
        const size_t first = bits.find('1');
        bits = first == std::string::npos ? "0" : bits.substr(first);
        out += "  \xC2\xB7  0b" + bits;
    }
    return Utf8ToWide(out);
}

}  // namespace beam
