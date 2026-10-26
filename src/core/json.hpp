// Small JSON value with a lenient parser (comments and trailing commas are
// accepted, since settings.json is meant to be edited by hand).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace beam {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int n) : type_(Type::Number), number_(n) {}
    Json(int64_t n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Json(double n) : type_(Type::Number), number_(n) {}
    Json(const char* s) : type_(Type::String), string_(s) {}
    Json(std::string s) : type_(Type::String), string_(std::move(s)) {}

    static Json MakeArray();
    static Json MakeObject();

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool AsBool(bool fallback = false) const { return IsBool() ? bool_ : fallback; }
    double AsNumber(double fallback = 0) const { return IsNumber() ? number_ : fallback; }
    const std::string& AsString() const;

    const std::vector<Json>& Items() const { return array_; }
    const std::vector<std::pair<std::string, Json>>& Members() const { return object_; }

    const Json* Find(std::string_view key) const;
    Json& Set(std::string_view key, Json value);  // insert or replace, keeps order
    void Push(Json value);

    static std::optional<Json> Parse(std::string_view text, std::string* error = nullptr);
    std::string Dump(int indent = 2) const;  // indent < 0: compact, single line

private:
    void DumpTo(std::string& out, int indent, int level) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Json> array_;
    std::vector<std::pair<std::string, Json>> object_;
};

}  // namespace beam
