// TM Vibrant Shaders setup.
//
// Run from the extracted release zip. It copies the files that sit next to it - it carries no
// plugin inside itself - in one of two ways:
//   ModLoader:   "TM Vibrant Shaders" folder -> %LOCALAPPDATA%\TMLoader\database\TmForever\products
//   Game folder: TMVibrantShaders.dll -> <TrackMania>\d3d9.dll (no ModLoader needed)
// Running it again offers an update or a clean uninstall. Settings and presets in
// Documents\TrackMania\TMVS are never touched.
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

#include "tm_shaders_version.h"

namespace {

const wchar_t* const kProduct = L"TM Vibrant Shaders";
const wchar_t* const kLoaderUrl = L"https://tomashu.dev/software/tmloader/";
const wchar_t* const kRegistryKey = L"Software\\TM Vibrant Shaders";
const std::wstring kTitle = std::wstring(L"TM Vibrant Shaders ") + TM_SHADERS_VERSION;

enum ExitCode { kOk = 0, kFailed = 1, kNoLoader = 2, kGameRunning = 3, kFilesMissing = 4, kCancelled = 5 };

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

std::wstring sourceDll() {
    return sourceProduct() + L"\\" + TM_SHADERS_VERSION + L"\\TMVibrantShaders.dll";
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
        found = _wcsicmp(entry.szExeFile, L"TmForever.exe") == 0;
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

// Our d3d9.dll is recognised by its version resource, so a ReShade/DXVK d3d9.dll is never deleted.
bool isOurDll(const std::wstring& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return false;
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return false;
    wchar_t* name = nullptr;
    UINT length = 0;
    return VerQueryValueW(data.data(), L"\\StringFileInfo\\040904b0\\ProductName", reinterpret_cast<void**>(&name), &length) &&
           name && wcscmp(name, kProduct) == 0;
}

// --- Finding TrackMania ------------------------------------------------------

std::wstring regString(HKEY root, const std::wstring& key, const wchar_t* value, REGSAM view = 0) {
    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | view, nullptr, buffer, &size) != ERROR_SUCCESS) return L"";
    return buffer;
}

void addGameDir(std::vector<std::wstring>& dirs, std::wstring dir) {
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.empty() || !exists(dir + L"\\TmForever.exe")) return;
    for (const auto& known : dirs) {
        if (_wcsicmp(known.c_str(), dir.c_str()) == 0) return;
    }
    dirs.push_back(dir);
}

std::vector<std::wstring> findGameDirs() {
    std::vector<std::wstring> dirs;
    // A previous game-folder install.
    addGameDir(dirs, regString(HKEY_CURRENT_USER, kRegistryKey, L"GameFolder"));
    // Installers (Nadeo's own is Inno Setup) register an uninstall entry with the location.
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
                if (display.find(L"trackmania") == std::wstring::npos && display.find(L"tmnations") == std::wstring::npos) continue;
                addGameDir(dirs, regString(root, key, L"InstallLocation", flag));
            }
            RegCloseKey(uninstall);
        }
    }
    // Steam.
    const std::wstring steam = regString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (!steam.empty()) {
        for (const wchar_t* game : {L"TrackMania Nations Forever", L"TrackMania United", L"TrackMania United Forever"}) {
            addGameDir(dirs, steam + L"\\steamapps\\common\\" + game);
        }
    }
    // Usual places.
    const std::wstring programs = knownFolder(FOLDERID_ProgramFilesX86);
    for (const wchar_t* folder : {L"\\TmNationsForever", L"\\TmUnitedForever", L"\\Nadeo\\TmNationsForever", L"\\Nadeo\\TmUnitedForever",
                                  L"\\TrackMania Nations Forever", L"\\TrackMania United Forever"}) {
        addGameDir(dirs, programs + folder);
    }
    return dirs;
}

// United Forever sets its window title in Nadeo.ini; Nations Forever has none.
std::wstring gameName(const std::wstring& dir) {
    wchar_t title[128] = {};
    GetPrivateProfileStringW(L"TmForever", L"WindowTitle", L"", title, 128, (dir + L"\\Nadeo.ini").c_str());
    if (title[0]) return title;
    std::wstring lower = dir;
    for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
    return lower.find(L"united") != std::wstring::npos ? L"TrackMania United Forever" : L"TrackMania Nations Forever";
}

std::wstring pickGameDir(const std::wstring& suggestion) {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return result;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Select your TrackMania folder (the one with TmForever.exe)");
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

// --- The two installs ----------------------------------------------------------

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
    const std::wstring target = dir + L"\\d3d9.dll";
    const std::wstring backup = dir + L"\\d3d9.dll.tmvs-backup";
    if (exists(target) && !isOurDll(target) && !MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING)) return kFailed;
    if (!CopyFileW(sourceDll().c_str(), target.c_str(), FALSE)) return kFailed;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"GameFolder", 0, REG_SZ, reinterpret_cast<const BYTE*>(dir.c_str()),
                       static_cast<DWORD>((dir.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    return kOk;
}

int gameRemove(const std::wstring& dir) {
    const std::wstring target = dir + L"\\d3d9.dll";
    const std::wstring backup = dir + L"\\d3d9.dll.tmvs-backup";
    if (exists(target) && isOurDll(target) && !DeleteFileW(target.c_str())) return kFailed;
    if (exists(backup) && !exists(target)) MoveFileW(backup.c_str(), target.c_str()); // give back what was there before
    return kOk;
}

bool gameInstalled(const std::wstring& dir) {
    return exists(dir + L"\\d3d9.dll") && isOurDll(dir + L"\\d3d9.dll");
}

// Program Files needs admin rights: run this setup again elevated for just that step.
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
    if (!ShellExecuteExW(&info) || !info.hProcess) return kCancelled; // UAC declined
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = kFailed;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return static_cast<int>(code);
}

// --- Interactive flows -----------------------------------------------------------

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
    message(L"Installed for the ModLoader.\n\n"
            L"1. Open the TrackMania ModLoader.\n"
            L"2. Tick \"TM Vibrant Shaders\" in your profile.\n"
            L"3. Start the game.\n\n"
            L"In game: F8 opens the shader menu, F7 turns the shaders on/off.",
            MB_OK | MB_ICONINFORMATION);
}

// dir: one of the games found, or empty to pick a folder.
void doGameInstall(std::wstring dir) {
    if (dir.empty()) {
        const std::vector<std::wstring> dirs = findGameDirs();
        dir = pickGameDir(dirs.empty() ? L"" : dirs.front());
    }
    if (dir.empty()) return;
    if (!exists(dir + L"\\TmForever.exe")) {
        message(L"TmForever.exe is not in\n" + dir + L"\n\nPlease select the TrackMania folder itself.", MB_OK | MB_ICONWARNING);
        return;
    }
    const std::wstring existing = dir + L"\\d3d9.dll";
    if (exists(existing) && !isOurDll(existing) &&
        message(L"There already is a d3d9.dll in this folder (ReShade or another mod?).\n\n"
                L"It will be kept as d3d9.dll.tmvs-backup and restored when you uninstall. Continue?",
                MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    const int result = runGameStep(L"/game-install", dir);
    if (result == kCancelled) return;
    if (result != kOk) {
        message(L"Could not copy d3d9.dll to\n" + dir, MB_OK | MB_ICONERROR);
        return;
    }
    message(L"Installed into " + gameName(dir) + L"\n" + dir + L"\n\nJust start the game as usual (no ModLoader needed).\n"
            L"In game: F8 opens the shader menu, F7 turns the shaders on/off.",
            MB_OK | MB_ICONINFORMATION);
}

void doUninstall() {
    bool ok = removeModLoader();
    for (const auto& dir : findGameDirs()) {
        if (gameInstalled(dir)) {
            const int result = runGameStep(L"/game-remove", dir);
            ok &= result == kOk;
        }
    }
    if (ok) RegDeleteKeyW(HKEY_CURRENT_USER, kRegistryKey);
    message(ok ? L"TM Vibrant Shaders was removed.\n\nYour settings and presets are kept in Documents\\TrackMania\\TMVS "
                 L"(delete that folder to remove them too)."
               : L"Some files could not be removed. Close TrackMania and try again.",
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

    if (!exists(sourceDll())) {
        if (command.empty()) {
            message(L"Please extract the whole zip first, then run the setup from the extracted folder.\n\n"
                    L"(The folder \"TM Vibrant Shaders\" has to be next to this setup.)",
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
        for (const auto& dir : findGameDirs()) {
            if (gameInstalled(dir) && canWrite(dir)) ok &= gameRemove(dir) == kOk;
        }
        return ok ? kOk : kFailed;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    while (gameRunning()) {
        if (message(L"TrackMania is running. Please close the game, then press Retry.", MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY) {
            return kGameRunning;
        }
    }

    // Current state, for the dialog text: the ModLoader and every game found (Nations and
    // United Forever can both be installed; each gets its own button).
    const bool loaderFound = exists(loaderDir());
    const bool inLoader = exists(productsDir() + L"\\" + kProduct);
    const std::vector<std::wstring> games = findGameDirs();
    bool inGame = false;
    std::wstring status = L"Real-time lighting, weather and skies for TrackMania Nations & United Forever.\n\nInstalled: ";
    std::wstring installed;
    if (inLoader) installed = L"ModLoader";
    for (const auto& dir : games) {
        if (!gameInstalled(dir)) continue;
        inGame = true;
        installed += (installed.empty() ? L"" : L", ") + gameName(dir);
    }
    status += installed.empty() ? L"not yet" : installed;

    const std::wstring loaderButton = std::wstring(L"Install for the TrackMania ModLoader\n") +
                                      (loaderFound ? L"Recommended. Switch the mod on or off in the ModLoader."
                                                   : L"The ModLoader was not found - this opens its download page.");
    std::vector<std::wstring> gameButtons;
    for (const auto& dir : games) {
        gameButtons.push_back(L"Install into " + gameName(dir) + (gameInstalled(dir) ? L" (update)" : L"") +
                              L"\nAdds d3d9.dll to " + dir + L". No ModLoader needed.");
    }
    std::vector<TASKDIALOG_BUTTON> buttons;
    buttons.push_back({101, loaderButton.c_str()});
    for (size_t i = 0; i < games.size(); i++) buttons.push_back({200 + static_cast<int>(i), gameButtons[i].c_str()});
    buttons.push_back({102, games.empty() ? L"Install into the game folder (no ModLoader)\nAdds d3d9.dll next to TmForever.exe. Start TrackMania as usual."
                                          : L"Install into another game folder\nChoose the folder with TmForever.exe yourself."});
    if (inLoader || inGame) buttons.push_back({103, L"Uninstall\nRemoves the mod from the ModLoader and every game folder."});
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
    config.pszFooter = L"Settings and your presets live in Documents\\TrackMania\\TMVS.";
    config.pszFooterIcon = TD_INFORMATION_ICON;

    int choice = 0;
    if (FAILED(TaskDialogIndirect(&config, &choice, nullptr, nullptr))) return kFailed;
    switch (choice) {
        case 101: doModLoaderInstall(); break;
        case 102: doGameInstall(L""); break;
        case 103: doUninstall(); break;
        default:
            if (choice >= 200 && choice < 200 + static_cast<int>(games.size())) doGameInstall(games[static_cast<size_t>(choice - 200)]);
            break;
    }
    CoUninitialize();
    return kOk;
}
