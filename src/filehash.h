#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

// SHA-256 of a whole file (Windows' CNG), for telling the one supported community exe of each old
// game from every other build (src/engine.cpp's legacy games, the setup). Header only: the setup
// and the mod use the same code.
namespace tmshaders {
namespace filehash {

struct Sha256 {
    unsigned char bytes[32] = {};
    bool operator==(const Sha256& other) const { return memcmp(bytes, other.bytes, 32) == 0; }
    bool operator!=(const Sha256& other) const { return !(*this == other); }
};

// From 64 hex digits (a constant in the code); all zero on a malformed text.
inline Sha256 fromHex(const char* hex) {
    Sha256 out;
    for (int i = 0; i < 32; i++) {
        unsigned value = 0;
        if (sscanf_s(hex + i * 2, "%2x", &value) != 1) return Sha256();
        out.bytes[i] = static_cast<unsigned char>(value);
    }
    return out;
}

inline void toHex(const Sha256& hash, char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", hash.bytes[i]);
}

// False when the file can't be read (or is larger than `limit` bytes).
inline bool ofFile(const wchar_t* path, Sha256& out, unsigned long long limit = 64ull << 20) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && static_cast<unsigned long long>(size.QuadPart) <= limit;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ok = ok && BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0;
    ok = ok && BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    std::vector<unsigned char> buffer(ok ? 1u << 20 : 0);
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;
        ok = BCryptHashData(hash, buffer.data(), read, 0) == 0;
    }
    ok = ok && BCryptFinishHash(hash, out.bytes, 32, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok;
}

} // namespace filehash
} // namespace tmshaders
