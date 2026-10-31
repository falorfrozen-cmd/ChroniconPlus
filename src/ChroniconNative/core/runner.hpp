// The GameMaker runner as Chronicon (YYC x64) builds it, found at runtime without any third-party layer.
//
// Everything is located by name or by a code pattern when the core starts, never by a hard-coded address:
//   - scripts and object events: the YYC function table in .data, {const char* name, void* code, YYVAR*};
//   - built-in functions: the runner's Function_Add("name", F_Name, ...) registrations in .text
//     (lea rcx,[name] next to lea rdx,[function]);
//   - RValue helpers (YYCreateString, COPY_RValue, FREE_RValue, ...): the YYRunnerInterface the runner fills
//     on its stack for extensions (lea rcx,[function]; mov [rsp+60h+8*i],rcx ...), field order from the
//     GameMaker extension SDK;
//   - the global instance: the pointer every compiled script loads before reading a global variable.
// Init() refuses (with the reason) if any of these is not found, so a game update cannot make it guess.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace cp::gm {

enum Kind : uint32_t {
    KIND_REAL = 0, KIND_STRING = 1, KIND_ARRAY = 2, KIND_PTR = 3, KIND_UNDEFINED = 5, KIND_OBJECT = 6,
    KIND_INT32 = 7, KIND_INT64 = 10, KIND_BOOL = 13, KIND_REF = 15, KIND_UNSET = 0x0FFFFFF,
};

struct CInstance;  // opaque: only passed back to the runner

struct RValue {
    union {
        double real;
        int64_t i64;
        int32_t i32;
        void* ptr;
    };
    uint32_t flags;
    uint32_t kind;
};
static_assert(sizeof(RValue) == 16);

using ScriptFn = RValue& (*)(CInstance* self, CInstance* other, RValue& result, int argc, RValue** args);
using EventFn = void (*)(CInstance* self, CInstance* other);
using BuiltinFn = void (*)(RValue& result, CInstance* self, CInstance* other, int argc, RValue* args);

// An owned RValue: strings, arrays and objects are reference counted by the runner, so copies go through
// COPY_RValue and destruction through FREE_RValue.
class Value {
public:
    Value();
    Value(double number);
    Value(int number);
    Value(bool flag);
    Value(const char* text);
    Value(const std::string& text);
    explicit Value(CInstance* instance);  // the instance as a GML value (self)
    Value(const Value& other);
    Value(Value&& other) noexcept;
    Value& operator=(const Value& other);
    Value& operator=(Value&& other) noexcept;
    ~Value();

    static Value Copy(const RValue& raw);  // a counted copy of a runner value
    static Value Adopt(RValue& raw);       // takes over raw (left undefined)

    uint32_t Kind() const { return raw_.kind & 0xFFFFFF; }
    bool IsNumber() const;
    bool IsString() const { return Kind() == KIND_STRING; }
    bool IsUndefined() const { return Kind() == KIND_UNDEFINED || Kind() == KIND_UNSET; }
    double Number() const;
    int Int() const;
    bool Bool() const;
    std::string Text() const;      // the string, or "" for other kinds
    std::string Describe() const;  // for logs and replies

    RValue& Raw() { return raw_; }
    const RValue& Raw() const { return raw_; }

private:
    RValue raw_;
};

// Finds everything listed above. Must run before any other function here; false with the reason on failure.
bool Init(std::string* why);
std::string Summary();  // counts and addresses, for the log

void* FindFunction(std::string_view name);  // gml_Script_*, gml_Object_*, gml_GlobalScript_*
ScriptFn FindScript(std::string_view name);
BuiltinFn FindBuiltin(std::string_view name);
CInstance* Global();

// Built-in function with the global instance as self/other (as an extension call does).
Value Call(const char* builtin, std::initializer_list<Value> args = {});
Value CallAs(CInstance* self, const char* builtin, std::initializer_list<Value> args = {});
// Replaces what Destination (a runner-owned slot, e.g. a script result) holds with a counted copy of Source.
void Assign(RValue& Destination, const Value& Source);

// Script with the given self; a missing script returns undefined.
Value CallScript(ScriptFn script, CInstance* self, std::initializer_list<Value> args = {});

}  // namespace cp::gm
