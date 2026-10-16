#include "gbufpatch.h"
#include <cctype>
#include <cstring>
#include <vector>

namespace tmshaders {
namespace gbufpatch {
namespace {

// --- Source text -------------------------------------------------------------
// The game's sources are plain HLSL: one PS_OUTPUT struct, a function that fills `Output` and
// returns it, preprocessor branches for its quality tweaks. The scan below works on tokens of
// the source with comments and preprocessor lines left out (positions stay those of the source),
// so neither commented-out debug lines nor #ifdef'd alternatives fool it. What it can't see
// (which #ifdef branch is active) the compiler decides: a patch that doesn't compile is dropped.

struct Token {
    size_t begin = 0, end = 0;
    std::string text;
};

bool identStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool identChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    const size_t n = s.size();
    size_t i = 0;
    bool lineStart = true;
    while (i < n) {
        const char c = s[i];
        if (c == '\n') {
            lineStart = true;
            i++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            const size_t close = s.find("*/", i + 2);
            i = close == std::string::npos ? n : close + 2;
            continue;
        }
        if (c == '#' && lineStart) {
            // A preprocessor line, with its continuations.
            while (i < n && s[i] != '\n') {
                if (s[i] == '\\' && i + 1 < n && (s[i + 1] == '\n' || (s[i + 1] == '\r' && i + 2 < n && s[i + 2] == '\n'))) {
                    i += s[i + 1] == '\r' ? 3 : 2;
                    continue;
                }
                i++;
            }
            continue;
        }
        lineStart = false;
        Token t;
        t.begin = i;
        if (identStart(c)) {
            while (i < n && identChar(s[i])) i++;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            while (i < n && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '.' ||
                             ((s[i] == '+' || s[i] == '-') && (s[i - 1] == 'e' || s[i - 1] == 'E')))) {
                i++;
            }
        } else {
            i++;
        }
        t.end = i;
        t.text = s.substr(t.begin, t.end - t.begin);
        out.push_back(std::move(t));
    }
    return out;
}

bool isIdent(const Token& t) {
    return !t.text.empty() && identStart(t.text[0]);
}

bool is(const std::vector<Token>& t, size_t i, const char* text) {
    return i < t.size() && t[i].text == text;
}

// Index of the brace matching the `{` at `open`, or npos.
size_t matchBrace(const std::vector<Token>& t, size_t open) {
    int depth = 0;
    for (size_t i = open; i < t.size(); i++) {
        if (t[i].text == "{") depth++;
        else if (t[i].text == "}" && --depth == 0) return i;
    }
    return std::string::npos;
}

bool isVector3(const std::string& type) {
    return type == "float3" || type == "half3" || type == "fpart3";
}

struct Field {
    std::string type, name, semantic;
};

// The fields of `struct <name> { ... }`: Type Name[ [n] ] : Semantic; Returns false when the
// body holds anything else (a macro could add outputs the scan can't see).
bool readStruct(const std::vector<Token>& t, size_t open, size_t close, std::vector<Field>& fields) {
    for (size_t i = open + 1; i < close;) {
        if (!isIdent(t[i]) || i + 1 >= close || !isIdent(t[i + 1])) return false;
        Field f;
        f.type = t[i].text;
        f.name = t[i + 1].text;
        i += 2;
        if (is(t, i, "[")) {
            while (i < close && t[i].text != "]") i++;
            i++;
        }
        if (!is(t, i, ":") || i + 1 >= close || !isIdent(t[i + 1]) || !is(t, i + 2, ";")) return false;
        f.semantic = t[i + 1].text;
        fields.push_back(f);
        i += 3;
    }
    return true;
}

// `struct <name> {`: the index of its `{`, how many there are in `count`.
size_t findStruct(const std::vector<Token>& t, const char* name, int& count) {
    size_t at = std::string::npos;
    count = 0;
    for (size_t i = 0; i + 2 < t.size(); i++) {
        if (t[i].text == "struct" && t[i + 1].text == name && t[i + 2].text == "{") {
            if (count++ == 0) at = i + 2;
        }
    }
    return at;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// The shader's own word for its world normal: its editor debug view "MDbg_NormalInW(0.5+0.5*X)"
// (or "X*0.5+0.5"). Empty if it has none or shows a constant there (the flat ground shaders
// show float3(0,1,0): they have no normal of their own).
bool isHalf(const std::vector<Token>& t, size_t i) {
    return i < t.size() && (t[i].text == "0.5" || t[i].text == "0.5f" || t[i].text == ".5" || t[i].text == ".5f");
}

std::string debugNormal(const std::vector<Token>& t) {
    for (size_t i = 0; i + 1 < t.size(); i++) {
        if (t[i].text != "MDbg_NormalInW" || t[i + 1].text != "(") continue;
        const size_t j = i + 2;
        // 0.5 + 0.5 * X )
        if (isHalf(t, j) && is(t, j + 1, "+") && isHalf(t, j + 2) && is(t, j + 3, "*") && j + 4 < t.size() && isIdent(t[j + 4]) && is(t, j + 5, ")")) {
            return t[j + 4].text;
        }
        // X * 0.5 + 0.5 )
        if (j < t.size() && isIdent(t[j]) && is(t, j + 1, "*") && isHalf(t, j + 2) && is(t, j + 3, "+") && isHalf(t, j + 4) && is(t, j + 5, ")")) {
            return t[j].text;
        }
    }
    return std::string();
}

bool declaredIn(const std::vector<Token>& t, size_t from, size_t to, const std::string& name) {
    for (size_t i = from; i + 1 < to; i++) {
        if (isVector3(t[i].text) && t[i + 1].text == name) return true;
    }
    return false;
}

bool memberUsedIn(const std::vector<Token>& t, size_t from, size_t to, const std::string& object, const std::string& member) {
    for (size_t i = from; i + 2 < to; i++) {
        if (t[i].text == object && t[i + 1].text == "." && t[i + 2].text == member) return true;
    }
    return false;
}

bool pixelProfile(const char* profile) {
    return profile && (!strcmp(profile, "ps_2_0") || !strcmp(profile, "ps_2_a") || !strcmp(profile, "ps_2_b") || !strcmp(profile, "ps_3_0"));
}

// --- Bytecode ---------------------------------------------------------------

constexpr DWORD kEnd = 0x0000FFFF;
constexpr DWORD kComment = 0xFFFE;
constexpr DWORD kDcl = 0x1F, kDef = 0x51, kDefI = 0x30, kDefB = 0x2F;
constexpr DWORD kColorOut = 8; // D3DSPR_COLOROUT

// Walks the instructions of a shader model 2/3 shader: fn(opcode, first param, param count).
template <typename Fn>
bool walk(const void* code, size_t bytes, Fn fn) {
    const DWORD* t = static_cast<const DWORD*>(code);
    const size_t n = bytes / 4;
    if (n < 2 || ((t[0] >> 8) & 0xFF) < 2) return false;
    for (size_t i = 1; i < n;) {
        const DWORD tok = t[i];
        if (tok == kEnd) return true;
        const DWORD op = tok & 0xFFFF;
        if (op == kComment) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const DWORD length = (tok >> 24) & 0x0F;
        if (i + 1 + length > n) return false;
        fn(op, t + i + 1, length);
        i += 1 + length;
    }
    return false;
}

struct Constant {
    std::string name;
    WORD set = 0, index = 0, count = 0;
};

bool constantTable(const void* code, size_t bytes, std::vector<Constant>& out) {
    const DWORD* t = static_cast<const DWORD*>(code);
    const size_t n = bytes / 4;
    for (size_t i = 1; i < n;) {
        const DWORD tok = t[i];
        if (tok == kEnd) break;
        if ((tok & 0xFFFF) != kComment) {
            if (((t[0] >> 8) & 0xFF) < 2) return false;
            i += 1 + ((tok >> 24) & 0x0F);
            continue;
        }
        const size_t words = (tok >> 16) & 0x7FFF;
        if (i + 1 + words > n) return false;
        if (words >= 8 && t[i + 1] == 0x42415443u) { // "CTAB"
            const char* base = reinterpret_cast<const char*>(t + i + 2);
            const size_t size = (words - 1) * 4;
            const DWORD* header = reinterpret_cast<const DWORD*>(base);
            const DWORD count = header[3], info = header[4];
            if (info > size || count > (size - info) / 20) return false;
            for (DWORD c = 0; c < count; c++) {
                const char* entry = base + info + c * 20;
                DWORD nameOffset = 0;
                memcpy(&nameOffset, entry, 4);
                Constant k;
                memcpy(&k.set, entry + 4, 2);
                memcpy(&k.index, entry + 6, 2);
                memcpy(&k.count, entry + 8, 2);
                if (nameOffset >= size) return false;
                const char* name = base + nameOffset;
                k.name.assign(name, strnlen(name, size - nameOffset));
                out.push_back(k);
            }
            return true;
        }
        i += 1 + words;
    }
    return false;
}

std::string firstLine(ID3DBlob* blob) {
    if (!blob || !blob->GetBufferPointer()) return "no message";
    std::string s(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
    const size_t end = s.find_first_of("\r\n");
    if (end != std::string::npos) s.resize(end);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    if (s.size() > 200) s.resize(200);
    return s;
}

void release(ID3DBlob*& blob) {
    if (blob) blob->Release();
    blob = nullptr;
}

} // namespace

Patch patchSource(const char* source, size_t length, const char* profile) {
    Patch p;
    if (!pixelProfile(profile)) {
        p.reason = "not a pixel shader of model 2 or 3";
        return p;
    }
    const std::string s(source, length);
    const std::vector<Token> t = tokenize(s);

    int outputs = 0;
    const size_t outOpen = findStruct(t, "PS_OUTPUT", outputs);
    if (outputs != 1) {
        p.reason = outputs ? "several PS_OUTPUT structs" : "no PS_OUTPUT struct";
        return p;
    }
    const size_t outClose = matchBrace(t, outOpen);
    std::vector<Field> outFields;
    if (outClose == std::string::npos || !readStruct(t, outOpen, outClose, outFields)) {
        p.reason = "PS_OUTPUT holds more than plain fields";
        return p;
    }
    bool color0 = false;
    for (const Field& f : outFields) {
        const std::string sem = upper(f.semantic);
        if (sem == "COLOR0" || sem == "COLOR") color0 = true;
        else if (sem.compare(0, 5, "COLOR") == 0 || sem.compare(0, 5, "DEPTH") == 0) {
            p.reason = "writes " + f.semantic + " already";
            return p;
        }
        if (f.name == "TmvsGbuf") {
            p.reason = "patched already";
            return p;
        }
    }
    if (!color0) {
        p.reason = "no COLOR0 output";
        return p;
    }

    // The one `PS_OUTPUT <name> =` (or `;`) and the function around it.
    size_t decl = std::string::npos;
    int decls = 0;
    for (size_t i = 0; i + 2 < t.size(); i++) {
        if (t[i].text == "PS_OUTPUT" && isIdent(t[i + 1]) && (t[i + 2].text == "=" || t[i + 2].text == ";") && (i == 0 || t[i - 1].text != "struct")) {
            if (decls++ == 0) decl = i;
        }
    }
    if (decls != 1) {
        p.reason = decls ? "several PS_OUTPUT variables" : "no PS_OUTPUT variable";
        return p;
    }
    const std::string output = t[decl + 1].text;
    size_t bodyOpen = std::string::npos, bodyClose = std::string::npos;
    std::string input;
    for (size_t i = 0; i + 2 < t.size() && i < decl; i++) {
        if (t[i].text != "PS_OUTPUT" || !isIdent(t[i + 1]) || t[i + 2].text != "(") continue;
        size_t close = i + 3;
        std::string param;
        while (close < t.size() && t[close].text != ")") {
            if (t[close].text == "PS_INPUT" && close + 1 < t.size() && isIdent(t[close + 1])) param = t[close + 1].text;
            close++;
        }
        if (!is(t, close + 1, "{")) continue;
        const size_t end = matchBrace(t, close + 1);
        if (end != std::string::npos && close + 1 < decl && decl < end) {
            bodyOpen = close + 1;
            bodyClose = end;
            input = param;
        }
    }
    if (bodyOpen == std::string::npos) {
        p.reason = "no function around the PS_OUTPUT variable";
        return p;
    }
    if (input.empty()) {
        p.reason = "the function takes no PS_INPUT";
        return p;
    }
    std::vector<size_t> returns;
    for (size_t i = decl; i + 2 < bodyClose; i++) {
        if (t[i].text == "return" && t[i + 1].text == output && t[i + 2].text == ";") returns.push_back(i);
    }
    if (returns.empty()) {
        p.reason = "no `return " + output + ";`";
        return p;
    }

    // The input fields: the eye vector (only its length is used: the distance from the eye).
    int inputs = 0;
    const size_t inOpen = findStruct(t, "PS_INPUT", inputs);
    std::vector<Field> inFields;
    if (inputs != 1 || matchBrace(t, inOpen) == std::string::npos || !readStruct(t, inOpen, matchBrace(t, inOpen), inFields)) {
        p.reason = "PS_INPUT not readable";
        return p;
    }
    // Eye to the surface (or back), world orientation: the vertex shaders build these from
    // GbxEyeInVisual. Not PosInWorld (absolute) nor PosToEyeV.
    const char* const kEyeFields[] = {"EyeToPos", "EyeToPosInW", "PosToEyeInW", "PosInW"};
    std::string eye;
    for (const char* name : kEyeFields) {
        for (const Field& f : inFields) {
            if (eye.empty() && f.name == name && isVector3(f.type) && memberUsedIn(t, bodyOpen, bodyClose, input, f.name)) {
                eye = f.name;
                p.eyeSemantic = f.semantic;
            }
        }
    }

    // The world normal: what its debug view shows as such, else the interpolated NormalInW.
    std::string normal = debugNormal(t);
    std::string normalExpr;
    if (!normal.empty() && declaredIn(t, bodyOpen, bodyClose, normal)) {
        normalExpr = normal;
    } else {
        normal.clear();
        for (const Field& f : inFields) {
            if (f.name == "NormalInW" && isVector3(f.type) && memberUsedIn(t, bodyOpen, bodyClose, input, f.name)) {
                normal = input + ".NormalInW";
                normalExpr = normal;
                p.normalSemantic = f.semantic;
            }
        }
    }
    if (normal.empty()) {
        p.reason = "no world normal";
        return p;
    }
    if (eye.empty()) {
        p.reason = "no eye vector";
        return p;
    }

    // Each `return Output;` becomes `{ Output.TmvsGbuf = ...; return Output; }` (a block: safe
    // after an `if` too), the field goes last into PS_OUTPUT. From the end, so earlier
    // positions stay valid.
    const std::string assign = output + ".TmvsGbuf = float4(normalize((float3)(" + normalExpr + ")), length((float3)" + input + "." + eye + ")); ";
    std::string out = s;
    for (size_t r = returns.size(); r-- > 0;) {
        const size_t at = t[returns[r]].begin, after = t[returns[r] + 2].end;
        out.insert(after, " }");
        out.insert(at, "{ " + assign);
    }
    out.insert(t[outClose].begin, "\tfloat4 TmvsGbuf : COLOR1;\n");
    p.patched = true;
    p.source.swap(out);
    p.reason = "normal " + normal + ", eye " + eye;
    return p;
}

size_t bytecodeSize(const DWORD* code) {
    if (!code || ((code[0] >> 8) & 0xFF) < 2) return 0;
    // A shader is a few thousand tokens; anything past 1 M is no shader.
    for (size_t i = 1; i < (1u << 20);) {
        const DWORD tok = code[i];
        if (tok == kEnd) return (i + 1) * 4;
        if ((tok & 0xFFFF) == kComment) i += 1 + ((tok >> 16) & 0x7FFF);
        else i += 1 + ((tok >> 24) & 0x0F);
    }
    return 0;
}

bool writesColorOutput(const void* code, size_t bytes, int index) {
    bool writes = false;
    walk(code, bytes, [&](DWORD op, const DWORD* params, DWORD count) {
        if (op == kDef || op == kDefI || op == kDefB) return; // literal values, no registers
        for (DWORD k = (op == kDcl ? 1 : 0); k < count; k++) {
            const DWORD p = params[k];
            const DWORD type = ((p >> 28) & 7) | ((p >> 8) & 0x18);
            if ((p & 0x80000000u) && type == kColorOut && static_cast<int>(p & 0x7FF) == index) writes = true;
        }
    });
    return writes;
}

bool sameConstants(const void* original, size_t originalBytes, const void* patched, size_t patchedBytes, std::string* why) {
    std::vector<Constant> a, b;
    if (!constantTable(original, originalBytes, a) || !constantTable(patched, patchedBytes, b)) {
        if (why) *why = "no constant table";
        return false;
    }
    for (const Constant& k : a) {
        bool found = false;
        for (const Constant& m : b) {
            if (m.name == k.name && m.set == k.set) {
                found = true;
                if (m.index != k.index || m.count != k.count) {
                    if (why) *why = k.name + " moved (" + std::to_string(k.index) + " -> " + std::to_string(m.index) + ")";
                    return false;
                }
            }
        }
        if (!found) {
            if (why) *why = k.name + " gone";
            return false;
        }
    }
    if (a.size() != b.size()) {
        if (why) *why = "constants added";
        return false;
    }
    return true;
}

int instructionCount(const void* code, size_t bytes) {
    int count = 0;
    walk(code, bytes, [&](DWORD op, const DWORD*, DWORD) {
        if (op != kDcl && op != kDef && op != kDefI && op != kDefB) count++;
    });
    return count;
}

HRESULT compile(CompileFn fn, bool patch, const char* source, UINT length, const Macro* defines, Include* include, const char* function,
                const char* profile, DWORD flags, ID3DBlob** shader, ID3DBlob** errors, void** constants, Outcome* outcome) {
    if (outcome) *outcome = Outcome();
    // The game's own compile, as it asked for it: what it gets unless the patch is sure.
    const HRESULT hr = fn(source, length, defines, include, function, profile, flags, shader, errors, constants);
    if (!patch || !pixelProfile(profile)) return hr;
    if (FAILED(hr) || !source || !shader || !*shader) {
        if (outcome) {
            outcome->kind = Outcome::Skipped;
            outcome->detail = "the game's own compile failed";
        }
        return hr;
    }
    const Patch p = patchSource(source, length ? length : static_cast<UINT>(strlen(source)), profile);
    if (!p.patched) {
        if (outcome) {
            outcome->kind = Outcome::Skipped;
            outcome->detail = p.reason;
        }
        return hr;
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* messages = nullptr;
    void* table = nullptr;
    const HRESULT phr = fn(p.source.c_str(), static_cast<UINT>(p.source.size()), defines, include, function, profile, flags, &code, &messages,
                           constants ? &table : nullptr);
    std::string why;
    if (FAILED(phr) || !code) {
        why = "compile: " + firstLine(messages);
    } else if (!writesColorOutput(code->GetBufferPointer(), code->GetBufferSize(), 1)) {
        why = "the result writes no COLOR1";
    } else {
        std::string moved;
        if (!sameConstants((*shader)->GetBufferPointer(), (*shader)->GetBufferSize(), code->GetBufferPointer(), code->GetBufferSize(), &moved)) {
            why = "constants: " + moved;
        }
    }
    if (!why.empty()) {
        release(code);
        release(messages);
        if (table) static_cast<IUnknown*>(table)->Release();
        if (outcome) {
            outcome->kind = Outcome::Failed;
            outcome->detail = p.reason + ": " + why;
        }
        return hr;
    }
    (*shader)->Release();
    *shader = code;
    if (errors) {
        if (*errors) (*errors)->Release();
        *errors = messages;
    } else {
        release(messages);
    }
    if (constants) {
        if (*constants) static_cast<IUnknown*>(*constants)->Release();
        *constants = table;
    }
    if (outcome) {
        outcome->kind = Outcome::Patched;
        outcome->detail = p.reason;
    }
    return phr;
}

} // namespace gbufpatch
} // namespace tmshaders
