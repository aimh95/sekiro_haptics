#include "sekiro_haptics/lab/Expression.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace sekiro_haptics::lab {
namespace {

/// A tree this big is already far past anything a person types, and the cap
/// is what makes evaluation cost provably bounded.
constexpr std::size_t kMaxNodes = 512;
constexpr std::size_t kMaxTextLength = 2000;

enum class NodeKind { Number, Name, Unary, Binary, Call };

} // namespace

struct Expression::Node {
    NodeKind kind = NodeKind::Number;
    double number = 0.0;
    std::string name;                 // Name, Call, and the operator for Unary/Binary
    std::vector<std::unique_ptr<Node>> children;
};

namespace {

using Node = Expression::Node;

struct Token {
    enum class Kind { Number, Name, Operator, LeftParen, RightParen, Comma, End } kind = Kind::End;
    double number = 0.0;
    std::string text;
    std::size_t position = 0;
};

std::vector<Token> Tokenize(const std::string& text) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        Token token;
        token.position = i;
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            std::size_t used = 0;
            token.kind = Token::Kind::Number;
            token.number = std::stod(text.substr(i), &used);
            i += used;
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const auto start = i;
            while (i < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) ++i;
            token.kind = Token::Kind::Name;
            token.text = text.substr(start, i - start);
        } else if (c == '(') { token.kind = Token::Kind::LeftParen; ++i; }
        else if (c == ')') { token.kind = Token::Kind::RightParen; ++i; }
        else if (c == ',') { token.kind = Token::Kind::Comma; ++i; }
        else {
            // Two-character comparisons first, so "<=" is not read as "<".
            static const char* kTwo[] = {"<=", ">=", "==", "!="};
            std::string two = text.substr(i, 2);
            bool matched = false;
            for (const char* candidate : kTwo)
                if (two == candidate) { matched = true; break; }
            if (matched) { token.kind = Token::Kind::Operator; token.text = two; i += 2; }
            else if (std::string("+-*/^<>").find(c) != std::string::npos) {
                token.kind = Token::Kind::Operator;
                token.text = std::string(1, c);
                ++i;
            } else {
                throw std::runtime_error("알 수 없는 문자 '" + std::string(1, c) + "'");
            }
        }
        tokens.push_back(std::move(token));
    }
    Token end;
    end.position = text.size();
    tokens.push_back(end);
    return tokens;
}

/// How many arguments each function takes. This map IS the allowed list.
const std::unordered_map<std::string, int>& Functions() {
    static const std::unordered_map<std::string, int> kFunctions = {
        {"sin", 1}, {"cos", 1}, {"tan", 1}, {"exp", 1}, {"log", 1}, {"sqrt", 1},
        {"abs", 1}, {"sign", 1}, {"floor", 1}, {"ceil", 1},
        {"min", 2}, {"max", 2}, {"pow", 2},
        {"clamp", 3},
        // Short-circuits. Without it, a piecewise formula such as
        // "0 before t0, cos(...)/t after" has to evaluate the division even
        // where it is not wanted -- which is exactly the division by zero the
        // branch existed to avoid.
        {"if", 3},
    };
    return kFunctions;
}

class Parser {
public:
    Parser(std::vector<Token> tokens, const std::vector<std::string>& allowedNames)
        : tokens_(std::move(tokens)), allowed_(allowedNames) {}

    std::unique_ptr<Node> ParseAll() {
        auto node = ParseComparison();
        Expect(Token::Kind::End, "식 뒤에 남은 내용이 있습니다");
        return node;
    }

    std::size_t Nodes() const { return nodes_; }

private:
    [[noreturn]] void Fail(const std::string& why) {
        std::ostringstream out;
        out << why << " (위치 " << Peek().position + 1 << ")";
        throw std::runtime_error(out.str());
    }

    const Token& Peek() const { return tokens_[index_]; }
    const Token& Take() { return tokens_[index_++]; }
    bool IsOperator(const char* text) const {
        return Peek().kind == Token::Kind::Operator && Peek().text == text;
    }
    void Expect(Token::Kind kind, const std::string& why) {
        if (Peek().kind != kind) Fail(why);
        ++index_;
    }

    std::unique_ptr<Node> Make() {
        if (++nodes_ > kMaxNodes) throw std::runtime_error("식이 너무 깁니다");
        return std::make_unique<Node>();
    }

    std::unique_ptr<Node> ParseComparison() {
        auto left = ParseAdditive();
        static const char* kComparisons[] = {"<", ">", "<=", ">=", "==", "!="};
        for (const char* op : kComparisons) {
            if (!IsOperator(op)) continue;
            auto node = Make();
            node->kind = NodeKind::Binary;
            node->name = Take().text;
            node->children.push_back(std::move(left));
            node->children.push_back(ParseAdditive());
            return node;   // no chaining: "a < b < c" is a mistake, not a range
        }
        return left;
    }

    std::unique_ptr<Node> ParseAdditive() {
        auto left = ParseMultiplicative();
        while (IsOperator("+") || IsOperator("-")) {
            auto node = Make();
            node->kind = NodeKind::Binary;
            node->name = Take().text;
            node->children.push_back(std::move(left));
            node->children.push_back(ParseMultiplicative());
            left = std::move(node);
        }
        return left;
    }

    std::unique_ptr<Node> ParseMultiplicative() {
        auto left = ParseUnary();
        while (IsOperator("*") || IsOperator("/")) {
            auto node = Make();
            node->kind = NodeKind::Binary;
            node->name = Take().text;
            node->children.push_back(std::move(left));
            node->children.push_back(ParseUnary());
            left = std::move(node);
        }
        return left;
    }

    /// Unary minus binds LOOSER than `^`, so `-2^2` is -(2^2) = -4 rather
    /// than (-2)^2 = 4. That is what ordinary mathematical notation means and
    /// what every calculator a person is likely to have checked against does,
    /// so the other reading would be a silent wrong answer.
    std::unique_ptr<Node> ParseUnary() {
        if (IsOperator("+")) { Take(); return ParseUnary(); }
        if (IsOperator("-")) {
            auto node = Make();
            node->kind = NodeKind::Unary;
            node->name = Take().text;
            node->children.push_back(ParseUnary());
            return node;
        }
        return ParsePower();
    }

    std::unique_ptr<Node> ParsePower() {
        auto base = ParsePrimary();
        if (!IsOperator("^")) return base;
        auto node = Make();
        node->kind = NodeKind::Binary;
        node->name = Take().text;
        node->children.push_back(std::move(base));
        // Right associative, and the exponent goes through ParseUnary so
        // `2^-3` is a negative exponent rather than a parse error.
        node->children.push_back(ParseUnary());
        return node;
    }

    std::unique_ptr<Node> ParsePrimary() {
        if (Peek().kind == Token::Kind::Number) {
            auto node = Make();
            node->kind = NodeKind::Number;
            node->number = Take().number;
            return node;
        }
        if (Peek().kind == Token::Kind::LeftParen) {
            Take();
            auto node = ParseComparison();
            Expect(Token::Kind::RightParen, "닫는 괄호가 없습니다");
            return node;
        }
        if (Peek().kind != Token::Kind::Name) Fail("숫자나 이름이 와야 합니다");

        const auto name = Take().text;
        if (Peek().kind == Token::Kind::LeftParen) {
            const auto found = Functions().find(name);
            if (found == Functions().end())
                throw std::runtime_error("쓸 수 없는 함수 \"" + name + "\"");
            Take();
            auto node = Make();
            node->kind = NodeKind::Call;
            node->name = name;
            if (Peek().kind != Token::Kind::RightParen) {
                node->children.push_back(ParseComparison());
                while (Peek().kind == Token::Kind::Comma) {
                    Take();
                    node->children.push_back(ParseComparison());
                }
            }
            Expect(Token::Kind::RightParen, "닫는 괄호가 없습니다");
            if (static_cast<int>(node->children.size()) != found->second) {
                std::ostringstream out;
                out << name << "() 는 인자가 " << found->second << "개입니다";
                throw std::runtime_error(out.str());
            }
            return node;
        }

        if (name != "pi" && name != "e" &&
            std::find(allowed_.begin(), allowed_.end(), name) == allowed_.end())
            throw std::runtime_error("쓸 수 없는 이름 \"" + name + "\"");
        auto node = Make();
        node->kind = NodeKind::Name;
        node->name = name;
        return node;
    }

    std::vector<Token> tokens_;
    const std::vector<std::string>& allowed_;
    std::size_t index_ = 0;
    std::size_t nodes_ = 0;
};

double Lookup(const std::string& name, const std::vector<ExpressionBinding>& bindings) {
    if (name == "pi") return 3.14159265358979323846;
    if (name == "e") return 2.71828182845904523536;
    for (const auto& binding : bindings)
        if (binding.name == name) return binding.value;
    return 0.0;
}

double Eval(const Node& node, const std::vector<ExpressionBinding>& bindings,
            ExpressionDiagnostics& diagnostics) {
    switch (node.kind) {
    case NodeKind::Number:
        return node.number;
    case NodeKind::Name:
        return Lookup(node.name, bindings);
    case NodeKind::Unary:
        return -Eval(*node.children[0], bindings, diagnostics);
    case NodeKind::Binary: {
        const double a = Eval(*node.children[0], bindings, diagnostics);
        const auto& op = node.name;
        if (op == "+") return a + Eval(*node.children[1], bindings, diagnostics);
        if (op == "-") return a - Eval(*node.children[1], bindings, diagnostics);
        if (op == "*") return a * Eval(*node.children[1], bindings, diagnostics);
        if (op == "/") {
            const double b = Eval(*node.children[1], bindings, diagnostics);
            // Counted, not allowed through. An infinity here would propagate
            // into the sum and make every later measurement meaningless,
            // and the count is how the UI can say it happened.
            if (b == 0.0 || !std::isfinite(b)) { ++diagnostics.guardedDivisions; return 0.0; }
            return a / b;
        }
        if (op == "^") return std::pow(a, Eval(*node.children[1], bindings, diagnostics));
        const double b = Eval(*node.children[1], bindings, diagnostics);
        if (op == "<") return a < b ? 1.0 : 0.0;
        if (op == ">") return a > b ? 1.0 : 0.0;
        if (op == "<=") return a <= b ? 1.0 : 0.0;
        if (op == ">=") return a >= b ? 1.0 : 0.0;
        if (op == "==") return a == b ? 1.0 : 0.0;
        return a != b ? 1.0 : 0.0;
    }
    case NodeKind::Call: {
        const auto& name = node.name;
        // Short-circuit BEFORE evaluating the branches: that is the whole
        // reason this is a tree walk rather than a stack machine.
        if (name == "if") {
            const double condition = Eval(*node.children[0], bindings, diagnostics);
            const auto& taken = condition != 0.0 ? *node.children[1] : *node.children[2];
            return Eval(taken, bindings, diagnostics);
        }
        const double a = Eval(*node.children[0], bindings, diagnostics);
        if (name == "sin") return std::sin(a);
        if (name == "cos") return std::cos(a);
        if (name == "tan") return std::tan(a);
        if (name == "exp") return std::exp(a);
        if (name == "log") return a > 0.0 ? std::log(a) : (++diagnostics.nonFiniteResults, 0.0);
        if (name == "sqrt") return a >= 0.0 ? std::sqrt(a) : (++diagnostics.nonFiniteResults, 0.0);
        if (name == "abs") return std::fabs(a);
        if (name == "sign") return a > 0.0 ? 1.0 : (a < 0.0 ? -1.0 : 0.0);
        if (name == "floor") return std::floor(a);
        if (name == "ceil") return std::ceil(a);
        const double b = Eval(*node.children[1], bindings, diagnostics);
        if (name == "min") return std::fmin(a, b);
        if (name == "max") return std::fmax(a, b);
        if (name == "pow") return std::pow(a, b);
        const double c = Eval(*node.children[2], bindings, diagnostics);
        return std::fmin(std::fmax(a, b), c);   // clamp(x, lo, hi)
    }
    }
    return 0.0;
}

} // namespace

Expression::Expression() = default;
Expression::~Expression() = default;
Expression::Expression(Expression&&) noexcept = default;
Expression& Expression::operator=(Expression&&) noexcept = default;

std::string Expression::Parse(const std::string& text,
                              const std::vector<std::string>& allowedNames) {
    root_.reset();
    if (text.size() > kMaxTextLength) return "식이 너무 깁니다";
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return "식이 비어 있습니다";
    try {
        Parser parser(Tokenize(text), allowedNames);
        root_ = parser.ParseAll();
    } catch (const std::exception& e) {
        root_.reset();
        return e.what();
    } catch (...) {
        root_.reset();
        return "식을 해석할 수 없습니다";
    }
    return {};
}

bool Expression::Ok() const { return root_ != nullptr; }

double Expression::Evaluate(const std::vector<ExpressionBinding>& bindings,
                            ExpressionDiagnostics& diagnostics) const {
    if (!root_) return 0.0;
    const double value = Eval(*root_, bindings, diagnostics);
    if (!std::isfinite(value)) { ++diagnostics.nonFiniteResults; return 0.0; }
    return value;
}

std::vector<std::string> Expression::SupportedFunctions() {
    std::vector<std::string> names;
    names.reserve(Functions().size());
    for (const auto& [name, arity] : Functions()) names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace sekiro_haptics::lab
