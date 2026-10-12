// TM Vibrant Shaders setup.
//
// Run from the extracted release zip. It copies the files that sit next to it - it carries no
// plugin inside itself - for:
//   ModLoader:   "TM Vibrant Shaders" folder -> %LOCALAPPDATA%\TMLoader\database\TmForever\products
//   TM Forever:  TMVibrantShaders.dll -> <TrackMania>\d3d9.dll (no ModLoader needed)
//   ManiaPlanet: d3d11.dll -> <ManiaPlanet>\d3d11.dll
// Running it again offers an update or a clean uninstall. Settings and presets in
// Documents\TrackMania\TMVS and Documents\ManiaPlanet\TMVS are never touched.
//
// Command line: /S install for the ModLoader silently, /U uninstall everything silently.
// Internal (elevated helper): /game-install "<dir>", /game-remove "<dir>".
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <algorithm>

#include "tm_shaders_version.h"

namespace {

const wchar_t* const kProduct = L"TM Vibrant Shaders";
const wchar_t* const kLoaderUrl = L"https://tomashu.dev/software/tmloader/";
const wchar_t* const kRegistryKey = L"Software\\TM Vibrant Shaders";
const std::wstring kTitle = std::wstring(L"TM Vibrant Shaders ") + TM_SHADERS_VERSION;

enum ExitCode { kOk = 0, kFailed = 1, kNoLoader = 2, kGameRunning = 3, kFilesMissing = 4, kCancelled = 5 };

enum class GameKind { Forever, ManiaPlanet };

struct FoundGame {
    GameKind kind;
    std::wstring dir;
    std::wstring name;
};

// --- Small helpers -----------------------------------------------------------

bool exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    wchar_t* path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) result = path;
    CoTaskMemFree(path);
    return result;
}

std::wstring exeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

std::wstring loaderDir() {
    return knownFolder(FOLDERID_LocalAppData) + L"\\TMLoader";
}

std::wstring productsDir() {
    return loaderDir() + L"\\database\\TmForever\\products";
}

std::wstring sourceProduct() {
    return exeDir() + L"\\" + kProduct;
}

std::wstring sourceForeverDll() {
    std::wstring direct = exeDir() + L"\\TMVibrantShaders.dll";
    if (exists(direct)) return direct;
    return sourceProduct() + L"\\" + TM_SHADERS_VERSION + L"\\TMVibrantShaders.dll";
}

std::wstring sourceMpDll() {
    std::wstring direct = exeDir() + L"\\d3d11.dll";
    if (exists(direct)) return direct;
    std::wstring inSub = exeDir() + L"\\ManiaPlanet\\d3d11.dll";
    if (exists(inSub)) return inSub;
    std::wstring inProduct = sourceProduct() + L"\\" + TM_SHADERS_VERSION + L"\\d3d11.dll";
    if (exists(inProduct)) return inProduct;
    return direct;
}

bool hasForeverSource() {
    return exists(sourceForeverDll()) || exists(sourceProduct());
}

bool hasMpSource() {
    return exists(sourceMpDll());
}

int message(const std::wstring& text, UINT flags) {
    return MessageBoxW(nullptr, text.c_str(), kTitle.c_str(), flags | MB_SETFOREGROUND);
}

bool gameRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"TmForever.exe") == 0 ||
            _wcsicmp(entry.szExeFile, L"ManiaPlanet.exe") == 0 ||
            _wcsicmp(entry.szExeFile, L"ManiaPlanet32.exe") == 0) {
            found = true;
        }
    }
    CloseHandle(snapshot);
    return found;
}

bool shellFileOp(UINT op, const std::wstring& from, const std::wstring& to = L"") {
    // SHFileOperation wants double-null-terminated lists.
    std::wstring src = from + L'\0';
    std::wstring dst = to + L'\0';
    SHFILEOPSTRUCTW sh{};
    sh.wFunc = op;
    sh.pFrom = src.c_str();
    sh.pTo = to.empty() ? nullptr : dst.c_str();
    sh.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMMKDIR;
    return SHFileOperationW(&sh) == 0 && !sh.fAnyOperationsAborted;
}

bool canWrite(const std::wstring& dir) {
    const std::wstring probe = dir + L"\\tmvs_write_test.tmp";
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

// Our d3d9.dll and d3d11.dll are recognised by their version resource (ProductName "TM Vibrant Shaders"),
// or fallback marker so ReShade / other mods are never deleted.
bool isOurDll(const std::wstring& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size) {
        std::vector<BYTE> data(size);
        if (GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
            wchar_t* name = nullptr;
            UINT length = 0;
            if (VerQueryValueW(data.data(), L"\\StringFileInfo\\040904b0\\ProductName", reinterpret_cast<void**>(&name), &length) &&
                name && wcscmp(name, kProduct) == 0) {
                return true;
            }
        }
    }
    // Fallback: check for ASCII marker string "TmvsMpStats" inside binary
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD readBytes = 0;
        char buf[8192] = {};
        bool foundMarker = false;
        for (int i = 0; i < 2000; i++) {
            if (!ReadFile(hFile, buf, sizeof(buf), &readBytes, nullptr) || readBytes == 0) break;
            if (std::string(buf, readBytes).find("TmvsMpStats") != std::string::npos) {
                foundMarker = true;
                break;
            }
        }
        CloseHandle(hFile);
        if (foundMarker) return true;
    }
    return false;
}

bool isMpGame(const std::wstring& dir) {
    return exists(dir + L"\\ManiaPlanet.exe");
}

// --- Finding games -----------------------------------------------------------

std::wstring regString(HKEY root, const std::wstring& key, const wchar_t* value, REGSAM view = 0) {
    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | view, nullptr, buffer, &size) != ERROR_SUCCESS) return L"";
    return buffer;
}

std::wstring gameName(const std::wstring& dir) {
    if (isMpGame(dir)) return L"TrackMania 2 (ManiaPlanet)";
    wchar_t title[128] = {};
    GetPrivateProfileStringW(L"TmForever", L"WindowTitle", L"", title, 128, (dir + L"\\Nadeo.ini").c_str());
    if (title[0]) return title;
    std::wstring lower = dir;
    for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
    return lower.find(L"united") != std::wstring::npos ? L"TrackMania United Forever" : L"TrackMania Nations Forever";
}

void addGameDir(std::vector<FoundGame>& dirs, std::wstring dir) {
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.empty()) return;

    GameKind kind;
    if (exists(dir + L"\\ManiaPlanet.exe")) {
        kind = GameKind::ManiaPlanet;
    } else if (exists(dir + L"\\TmForever.exe")) {
        kind = GameKind::Forever;
    } else {
        return;
    }

    for (const auto& known : dirs) {
        if (_wcsicmp(known.dir.c_str(), dir.c_str()) == 0) return;
    }
    dirs.push_back({kind, dir, gameName(dir)});
}

std::vector<FoundGame> findGameDirs() {
    std::vector<FoundGame> dirs;
    // Previous game-folder installs.
    addGameDir(dirs, regString(HKEY_CURRENT_USER, kRegistryKey, L"GameFolder"));
    addGameDir(dirs, regString(HKEY_CURRENT_USER, kRegistryKey, L"GameFolder_ManiaPlanet"));

    // Installers register an uninstall entry with the location.
    const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
    const REGSAM views[] = {KEY_WOW64_32KEY, KEY_WOW64_64KEY};
    for (HKEY root : roots) {
        for (REGSAM view : views) {
            HKEY uninstall = nullptr;
            if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | view, &uninstall) != ERROR_SUCCESS) {
                continue;
            }
            wchar_t name[256];
            for (DWORD i = 0;; i++) {
                DWORD length = 256;
                if (RegEnumKeyExW(uninstall, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                const std::wstring key = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + name;
                const REGSAM flag = view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : RRF_SUBKEY_WOW6464KEY;
                std::wstring display = regString(root, key, L"DisplayName", flag);
                for (auto& ch : display) ch = static_cast<wchar_t>(towlower(ch));
                if (display.find(L"trackmania") == std::wstring::npos &&
                    display.find(L"tmnations") == std::wstring::npos &&
                    display.find(L"maniaplanet") == std::wstring::npos) continue;
                addGameDir(dirs, regString(root, key, L"InstallLocation", flag));
            }
            RegCloseKey(uninstall);
        }
    }

    // Steam: check primary folder and all library folders from libraryfolders.vdf.
    const std::wstring steam = regString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (!steam.empty()) {
        std::vector<std::wstring> steamLibs = {steam};
        const std::wstring vdfPath = steam + L"\\steamapps\\libraryfolders.vdf";
        HANDLE vdfFile = CreateFileW(vdfPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (vdfFile != INVALID_HANDLE_VALUE) {
            DWORD fileSize = GetFileSize(vdfFile, nullptr);
            if (fileSize > 0 && fileSize < 1024 * 1024) {
                std::string vdf(fileSize, '\0');
                DWORD readBytes = 0;
                ReadFile(vdfFile, &vdf[0], fileSize, &readBytes, nullptr);
                size_t pos = 0;
                while ((pos = vdf.find("\"path\"", pos)) != std::string::npos) {
                    pos += 6;
                    size_t q1 = vdf.find('\"', pos);
                    if (q1 == std::string::npos) break;
                    size_t q2 = vdf.find('\"', q1 + 1);
                    if (q2 == std::string::npos) break;
                    std::string libStr = vdf.substr(q1 + 1, q2 - q1 - 1);
                    std::wstring libW;
                    for (size_t k = 0; k < libStr.size(); k++) {
                        if (libStr[k] == '\\' && k + 1 < libStr.size() && libStr[k + 1] == '\\') k++;
                        libW.push_back(static_cast<wchar_t>(libStr[k]));
                    }
                    if (!libW.empty()) steamLibs.push_back(libW);
                    pos = q2 + 1;
                }
            }
            CloseHandle(vdfFile);
        }

        for (const auto& lib : steamLibs) {
            for (const wchar_t* game : {L"TrackMania Nations Forever", L"TrackMania United", L"TrackMania United Forever", L"ManiaPlanet"}) {
                addGameDir(dirs, lib + L"\\steamapps\\common\\" + game);
            }
        }
    }

    // Usual program files locations.
    const std::wstring prog32 = knownFolder(FOLDERID_ProgramFilesX86);
    const std::wstring prog64 = knownFolder(FOLDERID_ProgramFiles);
    for (const wchar_t* folder : {L"\\TmNationsForever", L"\\TmUnitedForever", L"\\Nadeo\\TmNationsForever", L"\\Nadeo\\TmUnitedForever",
                                  L"\\TrackMania Nations Forever", L"\\TrackMania United Forever",
                                  L"\\ManiaPlanet", L"\\Ubisoft\\ManiaPlanet"}) {
        if (!prog32.empty()) addGameDir(dirs, prog32 + folder);
        if (!prog64.empty()) addGameDir(dirs, prog64 + folder);
    }
    return dirs;
}

std::wstring pickGameDir(const std::wstring& suggestion) {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return result;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Select your game folder (with TmForever.exe or ManiaPlanet.exe)");
    if (!suggestion.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(suggestion.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    if (SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) result = path;
            CoTaskMemFree(path);
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

// --- The installs ------------------------------------------------------------

bool installModLoader() {
    // Older versions stay next to the new one (the ModLoader lists every version folder).
    const std::wstring productDir = productsDir() + L"\\" + kProduct;
    const std::wstring versionTarget = productDir + L"\\" + TM_SHADERS_VERSION;
    const std::wstring srcVersion = sourceProduct() + L"\\" + TM_SHADERS_VERSION;
    const std::wstring srcDesc = sourceProduct() + L"\\description.yaml";
    const std::wstring targetDesc = productDir + L"\\description.yaml";

    SHCreateDirectoryExW(nullptr, versionTarget.c_str(), nullptr);
    if (exists(srcDesc)) CopyFileW(srcDesc.c_str(), targetDesc.c_str(), FALSE);
    return shellFileOp(FO_COPY, srcVersion, productDir);
}

bool removeModLoader() {
    const std::wstring target = productsDir() + L"\\" + kProduct;
    return !exists(target) || shellFileOp(FO_DELETE, target);
}

int gameInstall(const std::wstring& dir) {
    const bool isMp = isMpGame(dir);
    const std::wstring dllName = isMp ? L"\\d3d11.dll" : L"\\d3d9.dll";
    const std::wstring src = isMp ? sourceMpDll() : sourceForeverDll();
    if (!exists(src)) return kFilesMissing;

    const std::wstring target = dir + dllName;
    const std::wstring backup = dir + dllName + L".tmvs-backup";
    if (exists(target) && !isOurDll(target) && !MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING)) return kFailed;
    if (!CopyFileW(src.c_str(), target.c_str(), FALSE)) return kFailed;

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const wchar_t* valName = isMp ? L"GameFolder_ManiaPlanet" : L"GameFolder";
        RegSetValueExW(key, valName, 0, REG_SZ, reinterpret_cast<const BYTE*>(dir.c_str()),
                       static_cast<DWORD>((dir.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    return kOk;
}

int gameRemove(const std::wstring& dir) {
    const bool isMp = isMpGame(dir);
    const std::wstring dllName = isMp ? L"\\d3d11.dll" : L"\\d3d9.dll";
    const std::wstring target = dir + dllName;
    const std::wstring backup = dir + dllName + L".tmvs-backup";
    if (exists(target) && isOurDll(target) && !DeleteFileW(target.c_str())) return kFailed;
    if (exists(backup) && !exists(target)) MoveFileW(backup.c_str(), target.c_str());
    return kOk;
}

bool gameInstalled(const std::wstring& dir) {
    const bool isMp = isMpGame(dir);
    const std::wstring dllName = isMp ? L"\\d3d11.dll" : L"\\d3d9.dll";
    return exists(dir + dllName) && isOurDll(dir + dllName);
}

int runGameStep(const wchar_t* step, const std::wstring& dir) {
    if (canWrite(dir)) return wcscmp(step, L"/game-install") == 0 ? gameInstall(dir) : gameRemove(dir);
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::wstring args = std::wstring(step) + L" \"" + dir + L"\"";
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = self;
    info.lpParameters = args.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) return kCancelled;
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = kFailed;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return static_cast<int>(code);
}

// --- Interactive flows -------------------------------------------------------

void doModLoaderInstall() {
    if (!exists(loaderDir())) {
        if (message(L"The TrackMania ModLoader was not found.\n\nInstall it, start it once and run this setup again - "
                    L"or choose \"Install into the game folder\" instead.\n\nOpen the ModLoader download page?",
                    MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            ShellExecuteW(nullptr, L"open", kLoaderUrl, nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }
    if (!installModLoader()) {
        message(L"Could not copy the files to\n" + productsDir(), MB_OK | MB_ICONERROR);
        return;
    }
    message(L"Installed for the TrackMania ModLoader.\n\n"
            L"1. Open the TrackMania ModLoader.\n"
            L"2. Tick \"TM Vibrant Shaders\" in your profile.\n"
            L"3. Start the game.\n\n"
            L"In game: F8 opens the shader menu, F7 turns the shaders on/off.",
            MB_OK | MB_ICONINFORMATION);
}

void doGameInstall(std::wstring dir) {
    if (dir.empty()) {
        const std::vector<FoundGame> games = findGameDirs();
        dir = pickGameDir(games.empty() ? L"" : games.front().dir);
    }
    if (dir.empty()) return;

    const bool isMp = isMpGame(dir);
    const bool isForever = exists(dir + L"\\TmForever.exe");
    if (!isMp && !isForever) {
        message(L"Neither TmForever.exe nor ManiaPlanet.exe was found in\n" + dir + L"\n\nPlease select the TrackMania or ManiaPlanet folder itself.", MB_OK | MB_ICONWARNING);
        return;
    }

    const std::wstring dllName = isMp ? L"d3d11.dll" : L"d3d9.dll";
    const std::wstring existing = dir + L"\\" + dllName;
    if (exists(existing) && !isOurDll(existing) &&
        message(L"There already is a " + dllName + L" in this folder (ReShade or another mod?).\n\n"
                L"It will be kept as " + dllName + L".tmvs-backup and restored when you uninstall. Continue?",
                MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    const int result = runGameStep(L"/game-install", dir);
    if (result == kCancelled) return;
    if (result != kOk) {
        message(L"Could not copy " + dllName + L" to\n" + dir, MB_OK | MB_ICONERROR);
        return;
    }

    if (isMp) {
        message(L"Installed into TrackMania 2 (ManiaPlanet)\n" + dir + L"\n\nStart ManiaPlanet (64-bit) as usual.\n"
                L"In game: F8 opens the shader menu, F5 turns the shaders on/off.",
                MB_OK | MB_ICONINFORMATION);
    } else {
        message(L"Installed into " + gameName(dir) + L"\n" + dir + L"\n\nStart the game as usual (no ModLoader needed).\n"
                L"In game: F8 opens the shader menu, F7 turns the shaders on/off.",
                MB_OK | MB_ICONINFORMATION);
    }
}

void doUninstall() {
    bool ok = removeModLoader();
    for (const auto& g : findGameDirs()) {
        if (gameInstalled(g.dir)) {
            const int result = runGameStep(L"/game-remove", g.dir);
            ok &= result == kOk;
        }
    }
    if (ok) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegistryKey, L"GameFolder");
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegistryKey, L"GameFolder_ManiaPlanet");
        RegDeleteKeyW(HKEY_CURRENT_USER, kRegistryKey);
    }
    message(ok ? L"TM Vibrant Shaders was removed.\n\nYour settings and presets are kept in Documents\\TrackMania\\TMVS and Documents\\ManiaPlanet\\TMVS "
                 L"(delete those folders to remove them too)."
               : L"Some files could not be removed. Close the games and try again.",
            MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::wstring command = argc > 1 ? argv[1] : L"";
    const std::wstring argument = argc > 2 ? argv[2] : L"";
    LocalFree(argv);

    // Elevated helper steps.
    if (command == L"/game-install") return gameInstall(argument);
    if (command == L"/game-remove") return gameRemove(argument);

    if (!hasForeverSource() && !hasMpSource()) {
        if (command.empty()) {
            message(L"Please extract the whole zip first, then run the setup from the extracted folder.\n\n"
                    L"(Neither TMVibrantShaders.dll nor d3d11.dll was found next to this setup.)",
                    MB_OK | MB_ICONWARNING);
        }
        return kFilesMissing;
    }

    if (command == L"/S" || command == L"/s") {
        if (!exists(loaderDir())) return kNoLoader;
        if (gameRunning()) return kGameRunning;
        return installModLoader() ? kOk : kFailed;
    }
    if (command == L"/U" || command == L"/u") {
        if (gameRunning()) return kGameRunning;
        bool ok = removeModLoader();
        for (const auto& g : findGameDirs()) {
            if (gameInstalled(g.dir) && canWrite(g.dir)) ok &= gameRemove(g.dir) == kOk;
        }
        return ok ? kOk : kFailed;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    while (gameRunning()) {
        if (message(L"TrackMania or ManiaPlanet is running. Please close the game, then press Retry.", MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY) {
            return kGameRunning;
        }
    }

    // Current state, for dialog text: the ModLoader and games found.
    const bool loaderFound = exists(loaderDir());
    const bool inLoader = exists(productsDir() + L"\\" + kProduct);
    const std::vector<FoundGame> games = findGameDirs();
    bool inGame = false;
    std::wstring status = L"Real-time lighting, weather and skies for TrackMania Forever & TrackMania 2.\n\nInstalled: ";
    std::wstring installed;
    if (inLoader) installed = L"ModLoader";
    for (const auto& g : games) {
        if (!gameInstalled(g.dir)) continue;
        inGame = true;
        installed += (installed.empty() ? L"" : L", ") + g.name;
    }
    status += installed.empty() ? L"not yet" : installed;

    std::vector<TASKDIALOG_BUTTON> buttons;
    std::vector<std::wstring> buttonTexts;

    if (hasForeverSource()) {
        buttonTexts.push_back(std::wstring(L"Install for the TrackMania ModLoader\n") +
                              (loaderFound ? L"Recommended for TMNF/TMUF. Switch the mod on or off in the ModLoader."
                                           : L"The ModLoader was not found - this opens its download page."));
        buttons.push_back({101, buttonTexts.back().c_str()});
    }

    for (size_t i = 0; i < games.size(); i++) {
        const auto& g = games[i];
        const bool isMp = (g.kind == GameKind::ManiaPlanet);
        if (isMp && !hasMpSource()) continue;
        if (!isMp && !hasForeverSource()) continue;

        const std::wstring note = isMp ? L"Adds d3d11.dll to " + g.dir + L". F5 toggles shaders in game."
                                       : L"Adds d3d9.dll to " + g.dir + L". No ModLoader needed.";
        buttonTexts.push_back(L"Install into " + g.name + (gameInstalled(g.dir) ? L" (update)" : L"") + L"\n" + note);
        buttons.push_back({200 + static_cast<int>(i), buttonTexts.back().c_str()});
    }

    buttonTexts.push_back(games.empty() ? L"Install into a game folder\nChoose the folder with TmForever.exe or ManiaPlanet.exe yourself."
                                        : L"Install into another game folder\nChoose the folder with TmForever.exe or ManiaPlanet.exe yourself.");
    buttons.push_back({102, buttonTexts.back().c_str()});

    if (inLoader || inGame) {
        buttonTexts.push_back(L"Uninstall\nRemoves the mod from the ModLoader and every game folder.");
        buttons.push_back({103, buttonTexts.back().c_str()});
    }

    TASKDIALOGCONFIG config{sizeof(config)};
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = kTitle.c_str();
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"How do you want to install TM Vibrant Shaders?";
    config.pszContent = status.c_str();
    config.pButtons = buttons.data();
    config.cButtons = static_cast<UINT>(buttons.size());
    config.nDefaultButton = 101;
    config.pszFooter = L"Settings and presets live in Documents\\TrackMania\\TMVS and Documents\\ManiaPlanet\\TMVS.";
    config.pszFooterIcon = TD_INFORMATION_ICON;

    int choice = 0;
    if (FAILED(TaskDialogIndirect(&config, &choice, nullptr, nullptr))) return kFailed;
    switch (choice) {
        case 101: doModLoaderInstall(); break;
        case 102: doGameInstall(L""); break;
        case 103: doUninstall(); break;
        default:
            if (choice >= 200 && choice < 200 + static_cast<int>(games.size())) {
                doGameInstall(games[static_cast<size_t>(choice - 200)].dir);
            }
            break;
    }
    CoUninitialize();
    return kOk;
}
