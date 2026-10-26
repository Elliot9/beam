#include "core/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace beam {
namespace {

class JsonParser {
public:
    explicit JsonParser(std::string_view s) : s_(s) {}

    std::optional<Json> Run(std::string* error) {
        Json value;
        SkipTrivia();
        if (!ParseValue(value, 0)) return Fail(error);
        SkipTrivia();
        if (pos_ != s_.size()) {
            error_ = "unexpected trailing characters";
            return Fail(error);
        }
        return value;
    }

private:
    std::optional<Json> Fail(std::string* error) {
        if (error) {
            size_t line = 1, col = 1;
            for (size_t i = 0; i < pos_ && i < s_.size(); ++i) {
                if (s_[i] == '\n') {
                    ++line;
                    col = 1;
                } else {
                    ++col;
                }
            }
            *error = "line " + std::to_string(line) + ", column " + std::to_string(col) + ": " +
                     (error_.empty() ? std::string("invalid JSON") : error_);
        }
        return std::nullopt;
    }

    bool SkipTrivia() {
        for (;;) {
            while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n'))
                ++pos_;
            if (pos_ + 1 < s_.size() && s_[pos_] == '/' && s_[pos_ + 1] == '/') {
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
                continue;
            }
            if (pos_ + 1 < s_.size() && s_[pos_] == '/' && s_[pos_ + 1] == '*') {
                const size_t end = s_.find("*/", pos_ + 2);
                if (end == std::string_view::npos) {
                    error_ = "unterminated comment";
                    pos_ = s_.size();
                    return false;
                }
                pos_ = end + 2;
                continue;
            }
            return true;
        }
    }

    bool ParseValue(Json& out, int depth) {
        if (depth > 128) {
            error_ = "nesting too deep";
            return false;
        }
        if (pos_ >= s_.size()) {
            error_ = "unexpected end of input";
            return false;
        }
        const char c = s_[pos_];
        if (c == '{') return ParseObject(out, depth);
        if (c == '[') return ParseArray(out, depth);
        if (c == '"') {
            std::string str;
            if (!ParseString(str)) return false;
            out = Json(std::move(str));
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
        if (s_.substr(pos_, 4) == "true") {
            pos_ += 4;
            out = Json(true);
            return true;
        }
        if (s_.substr(pos_, 5) == "false") {
            pos_ += 5;
            out = Json(false);
            return true;
        }
        if (s_.substr(pos_, 4) == "null") {
            pos_ += 4;
            out = Json();
            return true;
        }
        error_ = std::string("unexpected character '") + c + "'";
        return false;
    }

    bool ParseObject(Json& out, int depth) {
        out = Json::MakeObject();
        ++pos_;  // {
        for (;;) {
            if (!SkipTrivia()) return false;
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (pos_ >= s_.size() || s_[pos_] != '"') {
                error_ = "expected a quoted property name";
                return false;
            }
            std::string key;
            if (!ParseString(key)) return false;
            if (!SkipTrivia()) return false;
            if (pos_ >= s_.size() || s_[pos_] != ':') {
                error_ = "expected ':'";
                return false;
            }
            ++pos_;
            if (!SkipTrivia()) return false;
            Json value;
            if (!ParseValue(value, depth + 1)) return false;
            out.Set(key, std::move(value));
            if (!SkipTrivia()) return false;
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            error_ = "expected ',' or '}'";
            return false;
        }
    }

    bool ParseArray(Json& out, int depth) {
        out = Json::MakeArray();
        ++pos_;  // [
        for (;;) {
            if (!SkipTrivia()) return false;
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            Json value;
            if (!ParseValue(value, depth + 1)) return false;
            out.Push(std::move(value));
            if (!SkipTrivia()) return false;
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            error_ = "expected ',' or ']'";
            return false;
        }
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseHex4(uint32_t& out) {
        if (pos_ + 4 > s_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_ + i];
            out <<= 4;
            if (h >= '0' && h <= '9') out |= h - '0';
            else if (h >= 'a' && h <= 'f') out |= 10 + (h - 'a');
            else if (h >= 'A' && h <= 'F') out |= 10 + (h - 'A');
            else return false;
        }
        pos_ += 4;
        return true;
    }

    bool ParseString(std::string& out) {
        ++pos_;  // opening quote
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) {
                error_ = "control character in string (use \\n, \\t, ...)";
                --pos_;
                return false;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= s_.size()) break;
            const char e = s_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!ParseHex4(cp)) {
                        error_ = "invalid \\u escape";
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' &&
                        s_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        uint32_t lo = 0;
                        if (!ParseHex4(lo)) {
                            error_ = "invalid \\u escape";
                            return false;
                        }
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            AppendUtf8(out, 0xFFFD);
                            cp = lo;
                        }
                    }
                    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    error_ = std::string("invalid escape '\\") + e + "' (in Windows paths use \\\\)";
                    --pos_;
                    return false;
            }
        }
        error_ = "unterminated string";
        return false;
    }

    bool ParseNumber(Json& out) {
        const size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' || s_[pos_] == 'e' ||
                                    s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-')) {
            ++pos_;
        }
        const std::string text(s_.substr(start, pos_ - start));
        char* end = nullptr;
        const double v = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || *end != '\0' || !std::isfinite(v)) {
            error_ = "invalid number";
            pos_ = start;
            return false;
        }
        out = Json(v);
        return true;
    }

    std::string_view s_;
    size_t pos_ = 0;
    std::string error_;
};

void DumpString(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

}  // namespace

Json Json::MakeArray() {
    Json j;
    j.type_ = Type::Array;
    return j;
}

Json Json::MakeObject() {
    Json j;
    j.type_ = Type::Object;
    return j;
}

const std::string& Json::AsString() const {
    static const std::string kEmpty;
    return IsString() ? string_ : kEmpty;
}

const Json* Json::Find(std::string_view key) const {
    if (!IsObject()) return nullptr;
    for (const auto& [k, v] : object_)
        if (k == key) return &v;
    return nullptr;
}

Json& Json::Set(std::string_view key, Json value) {
    if (!IsObject()) {
        *this = MakeObject();
    }
    for (auto& [k, v] : object_) {
        if (k == key) {
            v = std::move(value);
            return v;
        }
    }
    object_.emplace_back(std::string(key), std::move(value));
    return object_.back().second;
}

void Json::Push(Json value) {
    if (!IsArray()) *this = MakeArray();
    array_.push_back(std::move(value));
}

std::optional<Json> Json::Parse(std::string_view text, std::string* error) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);  // UTF-8 BOM (older Notepad)
    }
    return JsonParser(text).Run(error);
}

std::string Json::Dump(int indent) const {
    std::string out;
    DumpTo(out, indent, 0);
    out.push_back('\n');
    return out;
}

void Json::DumpTo(std::string& out, int indent, int level) const {
    auto newline = [&](int lvl) {
        if (indent < 0) return;  // compact
        out.push_back('\n');
        out.append(static_cast<size_t>(indent * lvl), ' ');
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            char buf[64];
            if (std::floor(number_) == number_ && std::fabs(number_) < 1e15) {
                std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(number_));
            } else {
                std::snprintf(buf, sizeof buf, "%.17g", number_);
            }
            out += buf;
            break;
        }
        case Type::String: DumpString(out, string_); break;
        case Type::Array:
            if (array_.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (size_t i = 0; i < array_.size(); ++i) {
                if (i) out.push_back(',');
                newline(level + 1);
                array_[i].DumpTo(out, indent, level + 1);
            }
            newline(level);
            out.push_back(']');
            break;
        case Type::Object:
            if (object_.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (size_t i = 0; i < object_.size(); ++i) {
                if (i) out.push_back(',');
                newline(level + 1);
                DumpString(out, object_[i].first);
                out += indent < 0 ? ":" : ": ";
                object_[i].second.DumpTo(out, indent, level + 1);
            }
            newline(level);
            out.push_back('}');
            break;
    }
}

}  // namespace beam
