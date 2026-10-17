#include "inifile.h"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tmshaders {
namespace {

// The rules below were taken from what WritePrivateProfileStringW does (Windows 11), case by case;
// tools/initest.cpp checks them against it.

bool blank(wchar_t c) {
    return c == L' ' || c == L'\t';
}

std::wstring trim(const std::wstring& text, size_t from = 0, size_t to = std::wstring::npos) {
    if (to > text.size()) to = text.size();
    while (from < to && blank(text[from])) from++;
    while (to > from && blank(text[to - 1])) to--;
    return text.substr(from, to - from);
}

// Section and key names compare without case, ASCII letters only (Windows matches "a" with "A" but
// not an umlaut with its capital).
bool sameName(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'A' && x <= L'Z') x = static_cast<wchar_t>(x - L'A' + L'a');
        if (y >= L'A' && y <= L'Z') y = static_cast<wchar_t>(y - L'A' + L'a');
        if (x != y) return false;
    }
    return true;
}

enum class Kind { Other, Section, Key };

// What a line is: "[name" up to "]" (the rest ignored, the "]" may be missing), ";" a comment,
// "name=value" a key; leading blanks don't count.
Kind classify(const std::wstring& text, std::wstring* name, size_t* equals) {
    size_t p = 0;
    while (p < text.size() && blank(text[p])) p++;
    if (p == text.size()) return Kind::Other;
    if (text[p] == L'[') {
        const size_t close = text.find(L']', p + 1);
        *name = trim(text, p + 1, close);
        return Kind::Section;
    }
    if (text[p] == L';') return Kind::Other;
    const size_t eq = text.find(L'=', p);
    if (eq == std::wstring::npos) return Kind::Other;
    *name = trim(text, p, eq);
    *equals = eq;
    return Kind::Key;
}

} // namespace

bool IniFile::load(const std::wstring& path) {
    m_lines.clear();
    m_unicode = false;
    m_ok = true;
    m_error = 0;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        m_error = GetLastError();
        // Not there yet: WritePrivateProfileString starts a new (ANSI) file.
        m_ok = m_error == ERROR_FILE_NOT_FOUND || m_error == ERROR_PATH_NOT_FOUND;
        return m_ok;
    }
    std::string raw;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > (64 << 20)) {
        m_error = GetLastError();
        CloseHandle(file);
        m_ok = false;
        return false;
    }
    raw.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    if (!raw.empty() && (!ReadFile(file, &raw[0], static_cast<DWORD>(raw.size()), &got, nullptr) || got != raw.size())) {
        m_error = GetLastError();
        CloseHandle(file);
        m_ok = false;
        return false;
    }
    CloseHandle(file);

    std::wstring text;
    if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE) {
        m_unicode = true;
        text.resize((raw.size() - 2) / 2);
        if (!text.empty()) memcpy(&text[0], raw.data() + 2, text.size() * sizeof(wchar_t));
    } else {
        // ANSI: one wchar_t per byte keeps every byte as it was; only new names and values are
        // converted (in the code page, like Windows does).
        text.resize(raw.size());
        for (size_t i = 0; i < raw.size(); i++) text[i] = static_cast<unsigned char>(raw[i]);
    }
    // Windows drops NULs at the end of the file.
    while (!text.empty() && text.back() == L'\0') text.pop_back();

    // Lines end with CR LF, LF or a lone CR.
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find_first_of(L"\r\n", pos);
        Line line;
        if (end == std::wstring::npos) {
            line.text = text.substr(pos);
            pos = text.size();
        } else {
            line.text = text.substr(pos, end - pos);
            const size_t breakLength = text[end] == L'\r' && end + 1 < text.size() && text[end + 1] == L'\n' ? 2 : 1;
            line.end = text.substr(end, breakLength);
            pos = end + breakLength;
        }
        m_lines.push_back(std::move(line));
    }
    return true;
}

std::wstring IniFile::encode(const wchar_t* text) const {
    if (m_unicode) return text;
    std::wstring out;
    const int n = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return out;
    std::string bytes(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, text, -1, &bytes[0], n, nullptr, nullptr);
    bytes.resize(static_cast<size_t>(n - 1));
    out.resize(bytes.size());
    for (size_t i = 0; i < bytes.size(); i++) out[i] = static_cast<unsigned char>(bytes[i]);
    return out;
}

void IniFile::set(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    if (!section || !key || !value) return;
    const std::wstring wantSection = trim(encode(section));
    const std::wstring wantKey = trim(encode(key));
    const std::wstring newValue = encode(value);
    const Line added{wantKey + L"=" + newValue, L"\r\n"};

    size_t at = 0;
    bool found = false;
    std::wstring name;
    size_t equals = 0;
    for (; at < m_lines.size(); at++) {
        if (classify(m_lines[at].text, &name, &equals) == Kind::Section && sameName(name, wantSection)) {
            found = true;
            break;
        }
    }
    if (found) {
        // The first key of that name gets the new value (its name, the blanks around it and those
        // after the old value stay); a new key goes after the section's last key, or right under its
        // header.
        size_t insertAfter = at;
        for (size_t i = at + 1; i < m_lines.size(); i++) {
            const Kind kind = classify(m_lines[i].text, &name, &equals);
            if (kind == Kind::Section) break;
            if (kind != Kind::Key) continue;
            if (sameName(name, wantKey)) {
                const std::wstring& text = m_lines[i].text;
                size_t valueEnd = text.size();
                while (valueEnd > equals + 1 && blank(text[valueEnd - 1])) valueEnd--;
                m_lines[i].text = text.substr(0, equals + 1) + newValue + text.substr(valueEnd);
                insertAfter = SIZE_MAX;
                break;
            }
            insertAfter = i;
        }
        if (insertAfter != SIZE_MAX) {
            if (m_lines[insertAfter].end.empty()) m_lines[insertAfter].end = L"\r\n";
            m_lines.insert(m_lines.begin() + static_cast<std::ptrdiff_t>(insertAfter) + 1, added);
        }
    } else {
        // A new section goes at the end.
        if (!m_lines.empty() && m_lines.back().end.empty()) m_lines.back().end = L"\r\n";
        m_lines.push_back(Line{L"[" + wantSection + L"]", L"\r\n"});
        m_lines.push_back(added);
    }
    // Windows ends the file with a line break after every write (a lone CR at the end gets CR LF).
    if (!m_lines.empty() && m_lines.back().end.find(L'\n') == std::wstring::npos) m_lines.back().end += L"\r\n";
}

std::string IniFile::bytes() const {
    std::wstring text;
    for (const Line& line : m_lines) {
        text += line.text;
        text += line.end;
    }
    std::string out;
    if (m_unicode) {
        out.resize(2 + text.size() * sizeof(wchar_t));
        out[0] = static_cast<char>(0xFF);
        out[1] = static_cast<char>(0xFE);
        if (!text.empty()) memcpy(&out[2], text.data(), text.size() * sizeof(wchar_t));
    } else {
        out.resize(text.size());
        for (size_t i = 0; i < text.size(); i++) out[i] = static_cast<char>(text[i]);
    }
    return out;
}

bool IniFile::save(const std::wstring& path) {
    if (!m_ok) return false; // never replace a file that couldn't be read
    const std::string data = bytes();
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        m_error = GetLastError();
        return false;
    }
    DWORD wrote = 0;
    const bool written = data.empty() || (WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr) &&
                                          wrote == data.size());
    if (!written) m_error = GetLastError();
    CloseHandle(file);
    if (written && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    if (written) m_error = GetLastError();
    DeleteFileW(temp.c_str());
    return false;
}

} // namespace tmshaders
