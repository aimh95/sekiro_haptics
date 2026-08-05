#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sekiro_haptics::json {

/// The kind of value a JsonValue currently holds.
enum class JsonType {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

/// A minimal, self-contained JSON value tree.
///
/// This project hand-rolls JSON support rather than taking a third-party
/// dependency: every schema this repo needs (trace lines, preset/mapping
/// config) is flat objects, at most one array of objects, and at most one
/// level of nested object -- a recursive-descent parser handles that (and
/// arbitrary deeper nesting, for free) in a few hundred lines. See
/// docs/trace-format.md for the license/dependency rationale and the
/// documented fallback (nlohmann/json) if a future schema needs more than
/// this supports.
///
/// Not supported, by design: \u-escaped unicode, comments, trailing commas.
/// ParseJson() reports a clear error rather than mis-parsing these.
class JsonValue {
public:
    JsonValue(); // Null

    static JsonValue MakeNull();
    static JsonValue MakeBool(bool value);
    static JsonValue MakeNumber(double value);
    static JsonValue MakeString(std::string value);
    static JsonValue MakeArray(std::vector<JsonValue> values);
    /// `members` preserves insertion order; duplicate keys keep the last
    /// occurrence (matching a JSON object with a repeated key parsed
    /// top-to-bottom).
    static JsonValue MakeObject(std::vector<std::pair<std::string, JsonValue>> members);

    JsonType Type() const;
    bool IsNull() const;
    bool IsBool() const;
    bool IsNumber() const;
    bool IsString() const;
    bool IsArray() const;
    bool IsObject() const;

    /// Precondition: Type() matches. For code that has already checked the
    /// type (e.g. via Find()+Is*()); these throw std::logic_error on a type
    /// mismatch rather than returning a silently-wrong default.
    bool AsBool() const;
    double AsNumber() const;
    const std::string& AsString() const;
    const std::vector<JsonValue>& AsArray() const;
    /// All members in file/insertion order. Precondition: IsObject().
    const std::vector<std::pair<std::string, JsonValue>>& AsObject() const;

    /// Object member lookup. Returns nullptr if this value is not an object
    /// or has no member with that key -- never throws.
    const JsonValue* Find(const std::string& key) const;

    /// Convenience accessors for object members that tolerate a missing key
    /// or a type mismatch by returning `fallback` instead of throwing.
    std::string GetString(const std::string& key, const std::string& fallback = {}) const;
    double GetNumber(const std::string& key, double fallback = 0.0) const;
    bool GetBool(const std::string& key, bool fallback = false) const;

private:
    using Object = std::vector<std::pair<std::string, JsonValue>>;
    std::variant<std::nullptr_t, bool, double, std::string, std::vector<JsonValue>, Object> value_;
};

/// Result of ParseJson(): either a valid `value`, or `ok == false` with a
/// human-readable `error` describing what went wrong and at what byte
/// offset into `text`.
struct JsonParseResult {
    bool ok = false;
    JsonValue value;
    std::string error;
};

/// Parses a single JSON value from `text`. Whitespace-only content, or
/// content with trailing non-whitespace after the value, is an error.
JsonParseResult ParseJson(std::string_view text);

} // namespace sekiro_haptics::json
