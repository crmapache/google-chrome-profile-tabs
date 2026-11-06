// Minimal JSON reader for Chrome's "Local State" file: objects, arrays, strings, numbers, literals.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    int64_t integer = 0;
    std::string text;  // UTF-8
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    const Value* get(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& member : members) {
            if (member.first == key) return &member.second;
        }
        return nullptr;
    }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Number; }
    bool isTrue() const { return type == Type::Bool && boolean; }
};

class Parser {
public:
    explicit Parser(const std::string& source) : s_(source) {}

    bool parse(Value& out) {
        skip();
        if (!value(out, 0)) return false;
        skip();
        return pos_ == s_.size();
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    void skip() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) pos_++;
    }

    bool literal(const char* word) {
        size_t length = strlen(word);
        if (s_.compare(pos_, length, word) != 0) return false;
        pos_ += length;
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > 200 || pos_ >= s_.size()) return false;
        char c = s_[pos_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out.type = Value::Type::String;
            return string(out.text);
        }
        if (literal("true")) { out.type = Value::Type::Bool; out.boolean = true; return true; }
        if (literal("false")) { out.type = Value::Type::Bool; out.boolean = false; return true; }
        if (literal("null")) { out.type = Value::Type::Null; return true; }
        return numberValue(out);
    }

    bool object(Value& out, int depth) {
        out.type = Value::Type::Object;
        pos_++;
        skip();
        if (pos_ < s_.size() && s_[pos_] == '}') { pos_++; return true; }
        while (true) {
            skip();
            std::string key;
            if (pos_ >= s_.size() || s_[pos_] != '"' || !string(key)) return false;
            skip();
            if (pos_ >= s_.size() || s_[pos_] != ':') return false;
            pos_++;
            skip();
            Value item;
            if (!value(item, depth + 1)) return false;
            out.members.emplace_back(std::move(key), std::move(item));
            skip();
            if (pos_ < s_.size() && s_[pos_] == ',') { pos_++; continue; }
            if (pos_ < s_.size() && s_[pos_] == '}') { pos_++; return true; }
            return false;
        }
    }

    bool array(Value& out, int depth) {
        out.type = Value::Type::Array;
        pos_++;
        skip();
        if (pos_ < s_.size() && s_[pos_] == ']') { pos_++; return true; }
        while (true) {
            skip();
            Value item;
            if (!value(item, depth + 1)) return false;
            out.items.push_back(std::move(item));
            skip();
            if (pos_ < s_.size() && s_[pos_] == ',') { pos_++; continue; }
            if (pos_ < s_.size() && s_[pos_] == ']') { pos_++; return true; }
            return false;
        }
    }

    static void appendUtf8(std::string& out, uint32_t code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool hex4(uint32_t& code) {
        if (pos_ + 4 > s_.size()) return false;
        code = 0;
        for (int i = 0; i < 4; i++) {
            char c = s_[pos_++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= c - '0';
            else if (c >= 'a' && c <= 'f') code |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') code |= c - 'A' + 10;
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        pos_++;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (pos_ >= s_.size()) return false;
            char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t code;
                    if (!hex4(code)) return false;
                    if (code >= 0xD800 && code <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        uint32_t low;
                        if (!hex4(low)) return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    appendUtf8(out, code);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool numberValue(Value& out) {
        size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) pos_++;
        while (pos_ < s_.size() && (isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '-' || s_[pos_] == '+')) pos_++;
        if (pos_ == start) return false;
        std::string token = s_.substr(start, pos_ - start);
        out.type = Value::Type::Number;
        out.number = strtod(token.c_str(), nullptr);
        out.integer = strtoll(token.c_str(), nullptr, 10);
        return true;
    }
};

}  // namespace json
