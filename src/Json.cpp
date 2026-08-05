#include "sekiro_haptics/Json.hpp"

#include <cctype>
#include <sstream>
#include <stdexcept>

namespace sekiro_haptics::json {

JsonValue::JsonValue() : value_(nullptr) {}

JsonValue JsonValue::MakeNull() {
    JsonValue v;
    v.value_ = nullptr;
    return v;
}

JsonValue JsonValue::MakeBool(bool value) {
    JsonValue v;
    v.value_ = value;
    return v;
}

JsonValue JsonValue::MakeNumber(double value) {
    JsonValue v;
    v.value_ = value;
    return v;
}

JsonValue JsonValue::MakeString(std::string value) {
    JsonValue v;
    v.value_ = std::move(value);
    return v;
}

JsonValue JsonValue::MakeArray(std::vector<JsonValue> values) {
    JsonValue v;
    v.value_ = std::move(values);
    return v;
}

JsonValue JsonValue::MakeObject(std::vector<std::pair<std::string, JsonValue>> members) {
    JsonValue v;
    v.value_ = std::move(members);
    return v;
}

JsonType JsonValue::Type() const {
    switch (value_.index()) {
        case 0:
            return JsonType::Null;
        case 1:
            return JsonType::Bool;
        case 2:
            return JsonType::Number;
        case 3:
            return JsonType::String;
        case 4:
            return JsonType::Array;
        case 5:
            return JsonType::Object;
    }
    return JsonType::Null;
}

bool JsonValue::IsNull() const {
    return Type() == JsonType::Null;
}
bool JsonValue::IsBool() const {
    return Type() == JsonType::Bool;
}
bool JsonValue::IsNumber() const {
    return Type() == JsonType::Number;
}
bool JsonValue::IsString() const {
    return Type() == JsonType::String;
}
bool JsonValue::IsArray() const {
    return Type() == JsonType::Array;
}
bool JsonValue::IsObject() const {
    return Type() == JsonType::Object;
}

bool JsonValue::AsBool() const {
    if (!IsBool()) {
        throw std::logic_error("JsonValue::AsBool: not a bool");
    }
    return std::get<bool>(value_);
}

double JsonValue::AsNumber() const {
    if (!IsNumber()) {
        throw std::logic_error("JsonValue::AsNumber: not a number");
    }
    return std::get<double>(value_);
}

const std::string& JsonValue::AsString() const {
    if (!IsString()) {
        throw std::logic_error("JsonValue::AsString: not a string");
    }
    return std::get<std::string>(value_);
}

const std::vector<JsonValue>& JsonValue::AsArray() const {
    if (!IsArray()) {
        throw std::logic_error("JsonValue::AsArray: not an array");
    }
    return std::get<std::vector<JsonValue>>(value_);
}

const std::vector<std::pair<std::string, JsonValue>>& JsonValue::AsObject() const {
    if (!IsObject()) {
        throw std::logic_error("JsonValue::AsObject: not an object");
    }
    return std::get<Object>(value_);
}

const JsonValue* JsonValue::Find(const std::string& key) const {
    if (!IsObject()) {
        return nullptr;
    }
    const auto& members = std::get<Object>(value_);
    for (const auto& member : members) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

std::string JsonValue::GetString(const std::string& key, const std::string& fallback) const {
    const JsonValue* member = Find(key);
    return (member != nullptr && member->IsString()) ? member->AsString() : fallback;
}

double JsonValue::GetNumber(const std::string& key, double fallback) const {
    const JsonValue* member = Find(key);
    return (member != nullptr && member->IsNumber()) ? member->AsNumber() : fallback;
}

bool JsonValue::GetBool(const std::string& key, bool fallback) const {
    const JsonValue* member = Find(key);
    return (member != nullptr && member->IsBool()) ? member->AsBool() : fallback;
}

namespace {

// Hand-rolled recursive-descent JSON parser. See Json.hpp for the rationale
// (no third-party dependency) and unsupported-feature list.
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    JsonParseResult Parse() {
        SkipWhitespace();
        JsonValue value;
        if (!ParseValue(value)) {
            return JsonParseResult{false, JsonValue::MakeNull(), error_};
        }
        SkipWhitespace();
        if (pos_ != text_.size()) {
            Fail("trailing content after JSON value");
            return JsonParseResult{false, JsonValue::MakeNull(), error_};
        }
        return JsonParseResult{true, std::move(value), {}};
    }

private:
    bool AtEnd() const {
        return pos_ >= text_.size();
    }

    char Peek() const {
        return text_[pos_];
    }

    void SkipWhitespace() {
        while (!AtEnd()) {
            char c = Peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    void Fail(const std::string& message) {
        std::ostringstream oss;
        oss << message << " at offset " << pos_;
        error_ = oss.str();
    }

    bool Expect(char c) {
        if (AtEnd() || Peek() != c) {
            Fail(std::string("expected '") + c + "'");
            return false;
        }
        ++pos_;
        return true;
    }

    bool ParseValue(JsonValue& out) {
        if (AtEnd()) {
            Fail("unexpected end of input");
            return false;
        }
        switch (Peek()) {
            case '{':
                return ParseObject(out);
            case '[':
                return ParseArray(out);
            case '"': {
                std::string s;
                if (!ParseStringLiteral(s)) {
                    return false;
                }
                out = JsonValue::MakeString(std::move(s));
                return true;
            }
            case 't':
            case 'f':
                return ParseBoolLiteral(out);
            case 'n':
                return ParseNullLiteral(out);
            default:
                if (Peek() == '-' || std::isdigit(static_cast<unsigned char>(Peek()))) {
                    return ParseNumberLiteral(out);
                }
                Fail(std::string("unexpected character '") + Peek() + "'");
                return false;
        }
    }

    bool ParseObject(JsonValue& out) {
        std::vector<std::pair<std::string, JsonValue>> members;
        if (!Expect('{')) {
            return false;
        }
        SkipWhitespace();
        if (!AtEnd() && Peek() == '}') {
            ++pos_;
            out = JsonValue::MakeObject(std::move(members));
            return true;
        }

        while (true) {
            SkipWhitespace();
            if (AtEnd() || Peek() != '"') {
                Fail("expected string key");
                return false;
            }
            std::string key;
            if (!ParseStringLiteral(key)) {
                return false;
            }
            SkipWhitespace();
            if (!Expect(':')) {
                return false;
            }
            SkipWhitespace();
            JsonValue memberValue;
            if (!ParseValue(memberValue)) {
                return false;
            }

            bool replaced = false;
            for (auto& member : members) {
                if (member.first == key) {
                    member.second = std::move(memberValue);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                members.emplace_back(std::move(key), std::move(memberValue));
            }

            SkipWhitespace();
            if (AtEnd()) {
                Fail("unterminated object");
                return false;
            }
            if (Peek() == ',') {
                ++pos_;
                continue;
            }
            if (Peek() == '}') {
                ++pos_;
                break;
            }
            Fail("expected ',' or '}'");
            return false;
        }

        out = JsonValue::MakeObject(std::move(members));
        return true;
    }

    bool ParseArray(JsonValue& out) {
        std::vector<JsonValue> values;
        if (!Expect('[')) {
            return false;
        }
        SkipWhitespace();
        if (!AtEnd() && Peek() == ']') {
            ++pos_;
            out = JsonValue::MakeArray(std::move(values));
            return true;
        }

        while (true) {
            SkipWhitespace();
            JsonValue element;
            if (!ParseValue(element)) {
                return false;
            }
            values.push_back(std::move(element));

            SkipWhitespace();
            if (AtEnd()) {
                Fail("unterminated array");
                return false;
            }
            if (Peek() == ',') {
                ++pos_;
                continue;
            }
            if (Peek() == ']') {
                ++pos_;
                break;
            }
            Fail("expected ',' or ']'");
            return false;
        }

        out = JsonValue::MakeArray(std::move(values));
        return true;
    }

    bool ParseStringLiteral(std::string& out) {
        if (!Expect('"')) {
            return false;
        }
        std::string result;
        while (true) {
            if (AtEnd()) {
                Fail("unterminated string");
                return false;
            }
            char c = text_[pos_++];
            if (c == '"') {
                break;
            }
            if (c == '\\') {
                if (AtEnd()) {
                    Fail("unterminated escape sequence");
                    return false;
                }
                char escaped = text_[pos_++];
                switch (escaped) {
                    case '"':
                        result.push_back('"');
                        break;
                    case '\\':
                        result.push_back('\\');
                        break;
                    case '/':
                        result.push_back('/');
                        break;
                    case 'b':
                        result.push_back('\b');
                        break;
                    case 'f':
                        result.push_back('\f');
                        break;
                    case 'n':
                        result.push_back('\n');
                        break;
                    case 'r':
                        result.push_back('\r');
                        break;
                    case 't':
                        result.push_back('\t');
                        break;
                    case 'u':
                        Fail("unsupported \\u escape");
                        return false;
                    default:
                        Fail(std::string("invalid escape character '") + escaped + "'");
                        return false;
                }
                continue;
            }
            result.push_back(c);
        }
        out = std::move(result);
        return true;
    }

    bool ParseBoolLiteral(JsonValue& out) {
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out = JsonValue::MakeBool(true);
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out = JsonValue::MakeBool(false);
            return true;
        }
        Fail("invalid literal");
        return false;
    }

    bool ParseNullLiteral(JsonValue& out) {
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            out = JsonValue::MakeNull();
            return true;
        }
        Fail("invalid literal");
        return false;
    }

    bool ParseNumberLiteral(JsonValue& out) {
        std::size_t start = pos_;
        if (!AtEnd() && Peek() == '-') {
            ++pos_;
        }
        if (AtEnd() || !std::isdigit(static_cast<unsigned char>(Peek()))) {
            Fail("invalid number");
            return false;
        }
        if (Peek() == '0') {
            ++pos_;
        } else {
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek()))) {
                ++pos_;
            }
        }
        if (!AtEnd() && Peek() == '.') {
            ++pos_;
            if (AtEnd() || !std::isdigit(static_cast<unsigned char>(Peek()))) {
                Fail("invalid number (expected digit after '.')");
                return false;
            }
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek()))) {
                ++pos_;
            }
        }
        if (!AtEnd() && (Peek() == 'e' || Peek() == 'E')) {
            ++pos_;
            if (!AtEnd() && (Peek() == '+' || Peek() == '-')) {
                ++pos_;
            }
            if (AtEnd() || !std::isdigit(static_cast<unsigned char>(Peek()))) {
                Fail("invalid number (expected digit in exponent)");
                return false;
            }
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek()))) {
                ++pos_;
            }
        }

        std::string token(text_.substr(start, pos_ - start));
        try {
            out = JsonValue::MakeNumber(std::stod(token));
        } catch (const std::exception&) {
            Fail("number out of range");
            return false;
        }
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::string error_;
};

} // namespace

JsonParseResult ParseJson(std::string_view text) {
    Parser parser(text);
    return parser.Parse();
}

} // namespace sekiro_haptics::json
