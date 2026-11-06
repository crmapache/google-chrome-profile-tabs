// Chrome on Windows: where it lives, its profiles, its windows and which profile each belongs to.
#include "common.h"
#include "json.h"

#include <dwmapi.h>
#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

// ---------------------------------------------------------------- strings and logging

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    return out;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring lower(std::wstring text) {
    CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

bool g_logToFile = false;

void logLine(const wchar_t* format, ...) {
    wchar_t buffer[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf(buffer, 2047, format, args);
    va_end(args);
    buffer[2047] = 0;
    OutputDebugStringW(buffer);
    OutputDebugStringW(L"\n");
    if (!g_logToFile) return;
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat(path, L"gcpt.log");
    if (FILE* file = _wfopen(path, L"a, ccs=UTF-8")) {
        SYSTEMTIME now;
        GetLocalTime(&now);
        fwprintf(file, L"%02d:%02d:%02d.%03d %ls\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, buffer);
        fclose(file);
    }
}

static std::wstring environment(const wchar_t* name) {
    wchar_t buffer[32768];
    DWORD length = GetEnvironmentVariableW(name, buffer, 32768);
    return length && length < 32768 ? std::wstring(buffer, length) : std::wstring();
}

static bool fileExists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring registryString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buffer[MAX_PATH * 2];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return buffer;
}

// ---------------------------------------------------------------- target

bool Target::resolve(Target& out) {
    std::wstring testExe = environment(L"GOOGLE_CHROME_PROFILE_TABS_BROWSER");
    std::wstring testData = environment(L"GOOGLE_CHROME_PROFILE_TABS_USER_DATA");
    if (!testExe.empty() && !testData.empty()) {
        out.exe = testExe;
        out.userDataDir = testData;
        out.defaultUserData = false;
        out.isTest = true;
        return fileExists(testExe);
    }
    const wchar_t* appPaths = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe";
    std::vector<std::wstring> candidates = {
        registryString(HKEY_CURRENT_USER, appPaths, nullptr),
        registryString(HKEY_LOCAL_MACHINE, appPaths, nullptr),
        environment(L"ProgramFiles") + L"\\Google\\Chrome\\Application\\chrome.exe",
        environment(L"ProgramFiles(x86)") + L"\\Google\\Chrome\\Application\\chrome.exe",
        environment(L"LOCALAPPDATA") + L"\\Google\\Chrome\\Application\\chrome.exe",
    };
    for (const auto& candidate : candidates) {
        if (!candidate.empty() && fileExists(candidate)) {
            out.exe = candidate;
            break;
        }
    }
    out.userDataDir = environment(L"LOCALAPPDATA") + L"\\Google\\Chrome\\User Data";
    out.defaultUserData = true;
    out.isTest = false;
    return !out.exe.empty();
}

// ---------------------------------------------------------------- profiles

static uint32_t fallbackColor(const std::wstring& name) {
    static const uint32_t palette[] = {0x8AB4F8, 0x81C995, 0xFDD663, 0xF28B82, 0xC58AF9, 0x78D9EC, 0xFCAD70, 0xFF8BCB};
    uint32_t hash = 5381;
    for (wchar_t c : name) hash = hash * 33 + static_cast<uint32_t>(c);
    return palette[hash % 8];
}

static bool readFile(const std::wstring& path, std::string& out) {
    // Chrome keeps the file open; share everything so the read never gets in its way.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (64LL << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
    }
    CloseHandle(file);
    return ok;
}

bool ProfileStore::reloadIfChanged() {
    std::wstring path = target_.userDataDir + L"\\Local State";
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) return false;
    if (CompareFileTime(&attributes.ftLastWriteTime, &stamp_) == 0) return false;
    std::string text;
    json::Value root;
    // A half-written file is skipped: the previous list stays until the next write.
    if (!readFile(path, text) || !json::Parser(text).parse(root)) return false;
    const json::Value* section = root.get("profile");
    const json::Value* cache = section ? section->get("info_cache") : nullptr;
    if (!cache || cache->type != json::Value::Type::Object) return false;
    stamp_ = attributes.ftLastWriteTime;

    std::vector<std::string> order;
    if (const json::Value* list = section->get("profiles_order")) {
        for (const auto& item : list->items) {
            if (item.isString()) order.push_back(item.text);
        }
    }
    std::vector<std::pair<int, Profile>> found;
    for (const auto& member : cache->members) {
        const json::Value& entry = member.second;
        auto text = [&](const char* key) {
            const json::Value* value = entry.get(key);
            return value && value->isString() ? widen(value->text) : std::wstring();
        };
        auto flag = [&](const char* key) {
            const json::Value* value = entry.get(key);
            return value && value->isTrue();
        };
        if (flag("is_ephemeral") || flag("is_omitted_from_profile_list")) continue;
        Profile profile;
        profile.dir = widen(member.first);
        std::wstring local = text("name"), given = text("gaia_given_name"), full = text("gaia_name");
        if (local.empty()) local = profile.dir;
        // A profile without a name of its own is called after the Google account, as in Chrome's menu.
        profile.name = local;
        if (flag("is_using_default_name")) {
            if (!given.empty()) profile.name = given;
            else if (!full.empty()) profile.name = full;
        }
        const json::Value* seed = entry.get("profile_color_seed");
        if (!seed || !seed->isNumber()) seed = entry.get("default_avatar_fill_color");
        profile.color = seed && seed->isNumber() ? static_cast<uint32_t>(seed->integer) & 0xFFFFFF : fallbackColor(profile.name);
        std::wstring pictureStamp;
        std::wstring picture = text("gaia_picture_file_name");
        if (!picture.empty()) {
            std::wstring file = target_.userDataDir + L"\\" + profile.dir + L"\\" + picture;
            WIN32_FILE_ATTRIBUTE_DATA pictureAttributes;
            if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &pictureAttributes)) {
                profile.picture = file;
                pictureStamp = std::to_wstring(pictureAttributes.ftLastWriteTime.dwLowDateTime);
            }
        }
        profile.signature = profile.dir + L"|" + profile.name + L"|" + std::to_wstring(profile.color) + L"|" + pictureStamp;
        auto position = std::find(order.begin(), order.end(), member.first);
        found.emplace_back(position == order.end() ? 1 << 20 : static_cast<int>(position - order.begin()), std::move(profile));
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second.dir < b.second.dir;
    });
    std::vector<Profile> fresh;
    for (auto& item : found) fresh.push_back(std::move(item.second));

    bool changed = fresh.size() != profiles_.size();
    for (size_t i = 0; !changed && i < fresh.size(); i++) changed = fresh[i].signature != profiles_[i].signature;
    profiles_ = std::move(fresh);
    return changed;
}

const Profile* ProfileStore::find(const std::wstring& dir) const {
    for (const auto& profile : profiles_) {
        if (profile.dir == dir) return &profile;
    }
    return nullptr;
}

// ---------------------------------------------------------------- windows

static std::wstring processPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t buffer[MAX_PATH * 2];
    DWORD length = MAX_PATH * 2;
    std::wstring path;
    if (QueryFullProcessImageNameW(process, 0, buffer, &length)) path.assign(buffer, length);
    CloseHandle(process);
    return path;
}

bool Windows::isBrowserProcess(DWORD pid) {
    auto known = browserByPid_.find(pid);
    if (known != browserByPid_.end()) return known->second;
    bool browser = lower(processPath(pid)) == lower(target_.exe);
    browserByPid_[pid] = browser;
    return browser;
}

bool Windows::isBrowserWindow(HWND hwnd) {
    wchar_t name[64];
    if (!GetClassNameW(hwnd, name, 64) || wcscmp(name, L"Chrome_WidgetWin_1") != 0) return false;
    if (GetWindow(hwnd, GW_OWNER)) return false;
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE), extended = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    // Browser windows have a frame; Chrome's menus, bubbles and tooltips are popups without one.
    if (!(style & WS_THICKFRAME) || (extended & WS_EX_TOOLWINDOW)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return isBrowserProcess(pid);
}

static std::wstring appUserModelId(HWND hwnd) {
    std::wstring id;
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &value)) && value.vt == VT_LPWSTR && value.pwszVal) id = value.pwszVal;
        PropVariantClear(&value);
        store->Release();
    }
    return id;
}

// Chrome builds a profile's part of the taskbar id from the folder names, keeping letters, digits and dots.
static std::wstring sanitized(const std::wstring& text) {
    std::wstring out;
    for (wchar_t c : text) {
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.') out += c;
    }
    return out;
}

// Which profile a window belongs to. Chrome groups each profile's windows on the taskbar under its own
// AppUserModelID: the browser's id for the Default profile of the usual user data folder, and
// "<browser id>.<user data folder>.<profile folder>" for the others ("Chrome.UserData.Profile1").
// Web app windows carry "_crx_" in theirs and are left alone.
std::wstring Windows::profileOf(HWND hwnd) {
    auto known = profileByWindow_.find(hwnd);
    if (known != profileByWindow_.end()) return known->second;
    wchar_t title[16];
    if (GetWindowTextW(hwnd, title, 16) && wcsncmp(title, L"DevTools", 8) == 0) return {};
    std::wstring id = appUserModelId(hwnd);
    if (id.empty() || id.find(L"_crx_") != std::wstring::npos) return {};
    std::wstring data = target_.userDataDir;
    std::wstring dataName = sanitized(data.substr(data.find_last_of(L"\\/") + 1));
    std::wstring found;
    for (const auto& profile : profiles_.profiles()) {
        std::wstring tail = L"." + dataName + L"." + sanitized(profile.dir);
        if (id.size() > tail.size() && lower(id.substr(id.size() - tail.size())) == lower(tail)) {
            found = profile.dir;
            break;
        }
    }
    if (found.empty() && target_.defaultUserData && lower(id).find(L"." + lower(dataName) + L".") == std::wstring::npos &&
        profiles_.find(L"Default")) {
        found = L"Default";
    }
    if (!found.empty()) profileByWindow_[hwnd] = found;
    return found;
}

std::vector<BrowserWindow> Windows::list() {
    std::vector<HWND> all;
    EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
        reinterpret_cast<std::vector<HWND>*>(data)->push_back(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&all));
    std::vector<BrowserWindow> windows;
    std::set<HWND> alive;
    std::set<DWORD> processes;
    browserPid_ = 0;
    for (HWND hwnd : all) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        processes.insert(pid);
        if (!IsWindowVisible(hwnd) || !isBrowserWindow(hwnd)) continue;
        alive.insert(hwnd);
        std::wstring dir = profileOf(hwnd);
        if (dir.empty()) continue;
        if (!browserPid_) GetWindowThreadProcessId(hwnd, &browserPid_);
        windows.push_back({hwnd, dir, IsIconic(hwnd) != FALSE});
    }
    for (auto it = profileByWindow_.begin(); it != profileByWindow_.end();) {
        it = alive.count(it->first) ? std::next(it) : profileByWindow_.erase(it);
    }
    // Process ids get reused: only processes that own a window right now stay remembered.
    for (auto it = browserByPid_.begin(); it != browserByPid_.end();) {
        it = processes.count(it->first) ? std::next(it) : browserByPid_.erase(it);
    }
    return windows;
}

bool openProfile(const Target& target, const std::wstring& dir) {
    std::wstring command = L"\"" + target.exe + L"\" --profile-directory=\"" + dir + L"\"";
    if (!target.defaultUserData) command += L" --user-data-dir=\"" + target.userDataDir + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        logLine(L"cannot open profile %ls: error %lu", dir.c_str(), GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

// ---------------------------------------------------------------- geometry

RECT visibleFrame(HWND hwnd) {
    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) GetWindowRect(hwnd, &frame);
    return frame;
}

RECT workAreaFor(const RECT& frame) {
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST), &info);
    return info.rcWork;
}

bool isCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

bool isFullscreen(HWND hwnd) {
    if (IsZoomed(hwnd)) return false;
    RECT frame = visibleFrame(hwnd);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST), &info);
    return frame.left <= info.rcMonitor.left && frame.top <= info.rcMonitor.top && frame.right >= info.rcMonitor.right &&
           frame.bottom >= info.rcMonitor.bottom;
}

// Puts the visible part of the window exactly at the frame. A maximized or minimized window is
// restored first, without taking the focus; then the invisible resize borders are added back.
void setVisibleFrame(HWND hwnd, const RECT& frame) {
    if (IsZoomed(hwnd) || IsIconic(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    RECT window, visible = visibleFrame(hwnd);
    GetWindowRect(hwnd, &window);
    RECT target{
        frame.left - (visible.left - window.left),
        frame.top - (visible.top - window.top),
        frame.right + (window.right - visible.right),
        frame.bottom + (window.bottom - visible.bottom),
    };
    SetWindowPos(hwnd, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void bringForward(HWND hwnd, bool force) {
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    if (SetForegroundWindow(hwnd) || !force) return;
    HWND foreground = GetForegroundWindow();
    DWORD theirs = GetWindowThreadProcessId(foreground, nullptr), ours = GetCurrentThreadId();
    AttachThreadInput(ours, theirs, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    AttachThreadInput(ours, theirs, FALSE);
}
