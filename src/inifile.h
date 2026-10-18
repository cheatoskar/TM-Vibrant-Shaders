#pragma once
#include <string>
#include <vector>

namespace tmshaders {

// An ini file edited in memory and written back in one go. Saving settings.ini used to take about
// 160 WritePrivateProfileString calls, each opening, parsing and rewriting the whole file: 125 ms,
// over a second on a fresh file, on the game's render thread. set() makes exactly the edit
// WritePrivateProfileStringW makes (tools/initest.cpp compares the bytes against Windows), so the
// GetPrivateProfile* reads, hand edits, comments and sections the mod doesn't know stay as before.
// An ANSI file stays ANSI, a UTF-16 one (byte order mark FF FE) stays UTF-16.
class IniFile {
public:
    // Reads the file; a missing file is an empty one. False if it exists but can't be read: then
    // save() refuses, so a file locked for a moment never loses what it holds.
    bool load(const std::wstring& path);
    // The edit WritePrivateProfileStringW(section, key, value, file) makes. value must not be null.
    void set(const wchar_t* section, const wchar_t* key, const wchar_t* value);
    // The file as it would be written.
    std::string bytes() const;
    // Writes <path>.tmp and moves it over path: the file is always the old or the new one, never half
    // written. False (and GetLastError's code in error()) if it can't, e.g. locked by another program.
    bool save(const std::wstring& path);
    unsigned long error() const { return m_error; }

private:
    struct Line {
        std::wstring text; // without the line break; in an ANSI file one wchar_t per byte
        std::wstring end;  // "\r\n", "\n", "\r" or "" (the last line without one)
    };
    std::wstring encode(const wchar_t* text) const;

    std::vector<Line> m_lines;
    bool m_unicode = false;
    bool m_ok = true;
    unsigned long m_error = 0;
};

} // namespace tmshaders
