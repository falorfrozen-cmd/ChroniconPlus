#include "runner.hpp"

#include <windows.h>

#include <cstring>
#include <format>
#include <map>
#include <unordered_map>

namespace cp::gm {
namespace {

struct Range {
    uintptr_t begin = 0, end = 0;
    bool Contains(uintptr_t a) const { return a >= begin && a < end; }
};

Range g_text, g_rdata, g_data;
std::unordered_map<std::string, void*> g_functions;
std::unordered_map<std::string, BuiltinFn> g_builtins;
CInstance** g_globalHolder = nullptr;

// YYRunnerInterface fields this file uses (indexes in the extension SDK's struct).
enum : int { RI_YYGetString = 16, RI_BOOL = 17, RI_REAL = 18, RI_INT32 = 21, RI_COPY = 25, RI_FREE = 27, RI_CreateString = 28 };
const char* (*g_YYGetString)(RValue* args, int index) = nullptr;
bool (*g_BOOL)(const RValue*) = nullptr;
double (*g_REAL)(const RValue*) = nullptr;
int32_t (*g_INT32)(const RValue*) = nullptr;
void (*g_COPY)(RValue* destination, const RValue* source) = nullptr;
void (*g_FREE)(RValue*) = nullptr;
void (*g_CreateString)(RValue* out, const char* text) = nullptr;
uintptr_t g_interfaceFill = 0;

int32_t Rel32(uintptr_t at) {
    int32_t value;
    std::memcpy(&value, reinterpret_cast<const void*>(at), 4);
    return value;
}

bool Bytes(uintptr_t at, const char* pattern, size_t length) {
    return std::memcmp(reinterpret_cast<const void*>(at), pattern, length) == 0;
}

bool ReadSections(std::string* why) {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        char name[9] = {};
        std::memcpy(name, section->Name, 8);
        const Range range{ base + section->VirtualAddress, base + section->VirtualAddress + section->Misc.VirtualSize };
        if (!std::strcmp(name, ".text")) g_text = range;
        else if (!std::strcmp(name, ".rdata")) g_rdata = range;
        else if (!std::strcmp(name, ".data")) g_data = range;
    }
    if (!g_text.begin || !g_rdata.begin || !g_data.begin) {
        *why = "the game exe has no .text / .rdata / .data section";
        return false;
    }
    return true;
}

// A NUL-terminated string inside .rdata, or nullptr.
const char* RdataString(uintptr_t at, size_t maximum) {
    if (!g_rdata.Contains(at)) return nullptr;
    const size_t room = (std::min)(maximum, g_rdata.end - at);
    const auto* text = reinterpret_cast<const char*>(at);
    return strnlen(text, room) < room ? text : nullptr;
}

// {const char* name, void* code, YYVAR* variable} entries in .data.
void ScanFunctionTable() {
    for (uintptr_t at = g_data.begin; at + 16 <= g_data.end; at += 8) {
        const auto name_at = *reinterpret_cast<const uintptr_t*>(at);
        const auto code_at = *reinterpret_cast<const uintptr_t*>(at + 8);
        if (!g_text.Contains(code_at)) continue;
        const char* name = RdataString(name_at, 256);
        if (!name || std::strncmp(name, "gml_", 4) != 0) continue;
        g_functions.emplace(name, reinterpret_cast<void*>(code_at));
    }
}

bool BuiltinName(const char* text) {
    if (!text || !((text[0] >= 'a' && text[0] <= 'z') || text[0] == '_')) return false;
    size_t length = 0;
    for (const char* c = text; *c; ++c, ++length)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '@')) return false;
    return length >= 3 && length <= 60;
}

// Function_Add("name", F_Name, argc, ...): lea rdx,[F_Name] with lea rcx,["name"] just before or after it.
void ScanBuiltins() {
    for (uintptr_t p = g_text.begin + 16; p + 32 < g_text.end; ++p) {
        if (!Bytes(p, "\x48\x8D\x15", 3)) continue;
        const uintptr_t function = p + 7 + Rel32(p + 3);
        if (!g_text.Contains(function)) continue;
        for (uintptr_t q = p - 14; q < p + 16; ++q) {
            if (q >= p - 6 && q < p + 7) continue;
            if (!Bytes(q, "\x48\x8D\x0D", 3)) continue;
            const char* name = RdataString(q + 7 + Rel32(q + 3), 64);
            if (BuiltinName(name)) g_builtins.emplace(name, reinterpret_cast<BuiltinFn>(function));
            break;
        }
    }
}

// The YYRunnerInterface fill: lea rcx,[fn]; mov [rsp+disp8],rcx (12 bytes per field, offsets rising by 8),
// continuing with mov [rbp+disp],rcx once the struct passes the rsp-addressable part.
bool ScanRunnerInterface(std::map<int, uintptr_t>* fields) {
    const char kLea[] = "\x48\x8D\x0D", kStoreRsp[] = "\x48\x89\x4C\x24";
    for (uintptr_t p = g_text.begin; p + 64 < g_text.end; ++p) {
        bool run = true;
        for (int k = 0; k < 4 && run; ++k)
            run = Bytes(p + 12 * k, kLea, 3) && Bytes(p + 12 * k + 7, kStoreRsp, 4) &&
                  *reinterpret_cast<const uint8_t*>(p + 12 * k + 11) == *reinterpret_cast<const uint8_t*>(p + 11) + 8 * k;
        if (!run) continue;

        const int base = *reinterpret_cast<const uint8_t*>(p + 11);
        int previous = -1;
        bool have_delta = false;
        int delta = 0;
        uintptr_t at = p;
        for (int n = 0; n < 200 && Bytes(at, kLea, 3); ++n) {
            const uintptr_t target = at + 7 + Rel32(at + 3);
            const uintptr_t store = at + 7;
            int offset = 0, length = 0;
            if (Bytes(store, kStoreRsp, 4)) {
                offset = *reinterpret_cast<const uint8_t*>(store + 4);
                length = 5;
            } else if (Bytes(store, "\x48\x89\x4D", 3) || Bytes(store, "\x48\x89\x8D", 3)) {
                const bool wide = *reinterpret_cast<const uint8_t*>(store + 2) == 0x8D;
                const int displacement = wide ? Rel32(store + 3) : *reinterpret_cast<const int8_t*>(store + 3);
                length = wide ? 7 : 4;
                if (!have_delta) {
                    delta = previous + 8 - displacement;
                    have_delta = true;
                }
                offset = displacement + delta;
            } else
                break;
            const int index = (offset - base) / 8;
            if ((offset - base) % 8 == 0 && index >= 0 && index < 120 && (previous < 0 || offset == previous + 8)) {
                (*fields)[index] = target;
                previous = offset;
            }
            at = store + length;
        }
        if (fields->size() > 30) {
            g_interfaceFill = p;
            return true;
        }
        fields->clear();
    }
    return false;
}

// mov rcx,[global]; mov rax,[rcx]; mov edx,[varid]; call [rax+8]: how every script reads a global variable.
CInstance** ScanGlobalHolder() {
    std::map<uintptr_t, int> votes;
    int seen = 0;
    for (uintptr_t p = g_text.begin; p + 32 < g_text.end && seen < 300; ++p) {
        if (!Bytes(p, "\x48\x8B\x0D", 3) || !Bytes(p + 7, "\x48\x8B\x01\x8B\x15", 5) || !Bytes(p + 16, "\xFF\x50\x08", 3))
            continue;
        ++votes[p + 7 + Rel32(p + 3)];
        ++seen;
    }
    uintptr_t best = 0;
    int best_votes = 0;
    for (auto [address, count] : votes)
        if (count > best_votes) best = address, best_votes = count;
    return g_data.Contains(best) && best_votes > seen / 2 ? reinterpret_cast<CInstance**>(best) : nullptr;
}

}  // namespace

bool Init(std::string* why) {
    if (!ReadSections(why)) return false;
    ScanFunctionTable();
    if (g_functions.size() < 1000) {
        *why = std::format("YYC function table not found ({} entries)", g_functions.size());
        return false;
    }
    ScanBuiltins();
    for (const char* needed : { "variable_global_get", "variable_instance_get", "instance_find", "draw_text_transformed",
                                "ds_map_find_value", "keyboard_check_pressed" })
        if (!g_builtins.contains(needed)) {
            *why = std::string("built-in function not found: ") + needed;
            return false;
        }

    std::map<int, uintptr_t> fields;
    if (!ScanRunnerInterface(&fields)) {
        *why = "YYRunnerInterface fill not found";
        return false;
    }
    for (int index : { RI_YYGetString, RI_BOOL, RI_REAL, RI_INT32, RI_COPY, RI_FREE, RI_CreateString })
        if (!fields.contains(index) || !g_text.Contains(fields[index])) {
            *why = std::format("YYRunnerInterface field {} missing", index);
            return false;
        }
    g_YYGetString = reinterpret_cast<decltype(g_YYGetString)>(fields[RI_YYGetString]);
    g_BOOL = reinterpret_cast<decltype(g_BOOL)>(fields[RI_BOOL]);
    g_REAL = reinterpret_cast<decltype(g_REAL)>(fields[RI_REAL]);
    g_INT32 = reinterpret_cast<decltype(g_INT32)>(fields[RI_INT32]);
    g_COPY = reinterpret_cast<decltype(g_COPY)>(fields[RI_COPY]);
    g_FREE = reinterpret_cast<decltype(g_FREE)>(fields[RI_FREE]);
    g_CreateString = reinterpret_cast<decltype(g_CreateString)>(fields[RI_CreateString]);

    g_globalHolder = ScanGlobalHolder();
    if (!g_globalHolder) {
        *why = "global instance pointer not found";
        return false;
    }
    return true;
}

std::string Summary() {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return std::format("{} gml functions, {} built-ins, runner interface at +0x{:X}, global holder at +0x{:X}, "
                       "YYCreateString +0x{:X}, FREE_RValue +0x{:X}",
                       g_functions.size(), g_builtins.size(), g_interfaceFill - base,
                       reinterpret_cast<uintptr_t>(g_globalHolder) - base,
                       reinterpret_cast<uintptr_t>(g_CreateString) - base, reinterpret_cast<uintptr_t>(g_FREE) - base);
}

void* FindFunction(std::string_view name) {
    auto found = g_functions.find(std::string(name));
    return found == g_functions.end() ? nullptr : found->second;
}

ScriptFn FindScript(std::string_view name) { return reinterpret_cast<ScriptFn>(FindFunction(name)); }

BuiltinFn FindBuiltin(std::string_view name) {
    auto found = g_builtins.find(std::string(name));
    return found == g_builtins.end() ? nullptr : found->second;
}

CInstance* Global() { return g_globalHolder ? *g_globalHolder : nullptr; }

// ---------------------------------------------------------------- Value

namespace {
bool Counted(uint32_t kind) { return kind < 32 && ((1u << kind) & 0x46u); }  // string, array, object
RValue Undefined() {
    RValue raw{};
    raw.kind = KIND_UNDEFINED;
    return raw;
}
}  // namespace

Value::Value() : raw_(Undefined()) {}
Value::Value(double number) : raw_{} { raw_.real = number, raw_.kind = KIND_REAL; }
Value::Value(int number) : raw_{} { raw_.real = number, raw_.kind = KIND_REAL; }
Value::Value(bool flag) : raw_{} { raw_.real = flag ? 1.0 : 0.0, raw_.kind = KIND_BOOL; }
Value::Value(const char* text) : raw_(Undefined()) { g_CreateString(&raw_, text ? text : ""); }
Value::Value(const std::string& text) : Value(text.c_str()) {}
Value::Value(CInstance* instance) : raw_{} { raw_.ptr = instance, raw_.kind = KIND_OBJECT; }

Value::Value(const Value& other) : raw_(Undefined()) { g_COPY(&raw_, &other.raw_); }
Value::Value(Value&& other) noexcept : raw_(other.raw_) { other.raw_ = Undefined(); }

Value& Value::operator=(const Value& other) {
    if (this != &other) g_COPY(&raw_, &other.raw_);
    return *this;
}

Value& Value::operator=(Value&& other) noexcept {
    if (this != &other) {
        if (Counted(Kind())) g_FREE(&raw_);
        raw_ = other.raw_;
        other.raw_ = Undefined();
    }
    return *this;
}

Value::~Value() {
    if (Counted(Kind())) g_FREE(&raw_);
}

Value Value::Copy(const RValue& raw) {
    Value value;
    g_COPY(&value.raw_, &raw);
    return value;
}

Value Value::Adopt(RValue& raw) {
    Value value;
    value.raw_ = raw;
    raw = Undefined();
    return value;
}

bool Value::IsNumber() const {
    const uint32_t kind = Kind();
    return kind == KIND_REAL || kind == KIND_INT32 || kind == KIND_INT64 || kind == KIND_BOOL;
}

double Value::Number() const { return IsNumber() ? g_REAL(&raw_) : 0.0; }
int Value::Int() const { return IsNumber() ? g_INT32(&raw_) : 0; }
bool Value::Bool() const { return IsNumber() && g_BOOL(&raw_); }

std::string Value::Text() const {
    if (!IsString()) return {};
    const char* text = g_YYGetString(const_cast<RValue*>(&raw_), 0);
    return text ? text : "";
}

std::string Value::Describe() const {
    switch (Kind()) {
    case KIND_STRING: return "\"" + Text() + "\"";
    case KIND_ARRAY: return "<array>";
    case KIND_OBJECT: return "<object>";
    case KIND_REF: return "<ref>";
    case KIND_PTR: return "<ptr>";
    case KIND_UNDEFINED: return "<undefined>";
    case KIND_UNSET: return "<unset>";
    default:
        if (!IsNumber()) return std::format("<kind {}>", Kind());
        const double number = Number();
        if (number == static_cast<double>(static_cast<long long>(number))) return std::to_string(static_cast<long long>(number));
        std::string text = std::format("{:.4f}", number);
        while (text.back() == '0') text.pop_back();
        if (text.back() == '.') text.pop_back();
        return text;
    }
}

// ---------------------------------------------------------------- calls

Value CallAs(CInstance* self, const char* builtin, std::initializer_list<Value> args) {
    BuiltinFn function = FindBuiltin(builtin);
    if (!function) return Value();
    std::vector<RValue> raw;
    raw.reserve(args.size());
    for (const Value& argument : args) raw.push_back(argument.Raw());  // borrowed: args outlive the call
    RValue result = Undefined();
    function(result, self, self, static_cast<int>(raw.size()), raw.data());
    return Value::Adopt(result);
}

Value Call(const char* builtin, std::initializer_list<Value> args) { return CallAs(Global(), builtin, args); }

void Assign(RValue& Destination, const Value& Source) { g_COPY(&Destination, &Source.Raw()); }

Value CallScript(ScriptFn script, CInstance* self, std::initializer_list<Value> args) {
    if (!script || !self) return Value();
    std::vector<Value> owned(args);
    std::vector<RValue*> pointers;
    for (Value& argument : owned) pointers.push_back(&argument.Raw());
    RValue result = Undefined();
    RValue& returned = script(self, self, result, static_cast<int>(pointers.size()), pointers.data());
    if (&returned == &result) return Value::Adopt(result);
    Value copy = Value::Copy(returned);
    Value::Adopt(result);  // releases whatever the script left in our buffer
    return copy;
}

}  // namespace cp::gm
