// Shared types and helpers of the Windows version.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------- strings and logging

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);
std::wstring lower(std::wstring text);
void logLine(const wchar_t* format, ...);

// ---------------------------------------------------------------- browser and profiles

// The browser the strip serves. Google Chrome by default; the environment variables
// GOOGLE_CHROME_PROFILE_TABS_BROWSER (path to chrome.exe) and GOOGLE_CHROME_PROFILE_TABS_USER_DATA
// (a user data folder) point it at a separate copy for testing without touching the real Chrome.
struct Target {
    std::wstring exe;          // full path of chrome.exe
    std::wstring userDataDir;  // the "User Data" folder
    bool defaultUserData = true;
    bool isTest = false;

    static bool resolve(Target& out);
};

struct Profile {
    std::wstring dir;      // folder inside the user data: "Default", "Profile 1"
    std::wstring name;     // what Chrome calls it in its profile menu
    uint32_t color = 0;    // 0xRRGGBB
    std::wstring picture;  // the Google account photo, if Chrome keeps one
    std::wstring signature;
};

// Reads the profile list from Chrome's "Local State" file.
class ProfileStore {
public:
    explicit ProfileStore(const Target& target) : target_(target) {}
    // Re-reads the file if Chrome wrote it since the last read. True when the list changed.
    bool reloadIfChanged();
    const std::vector<Profile>& profiles() const { return profiles_; }
    const Profile* find(const std::wstring& dir) const;

private:
    const Target& target_;
    std::vector<Profile> profiles_;
    FILETIME stamp_{};
};

// A browser window and the profile it belongs to.
struct BrowserWindow {
    HWND hwnd = nullptr;
    std::wstring dir;
    bool minimized = false;
};

// Finds browser windows and tells which profile each belongs to.
class Windows {
public:
    Windows(const Target& target, const ProfileStore& profiles) : target_(target), profiles_(profiles) {}
    // Browser windows in z-order, the topmost first.
    std::vector<BrowserWindow> list();
    // The browser process that owns the windows, 0 if there are none.
    DWORD browserPid() const { return browserPid_; }
    bool isBrowserProcess(DWORD pid);
    bool isBrowserWindow(HWND hwnd);
    void forgetProfiles() { profileByWindow_.clear(); }

private:
    const Target& target_;
    const ProfileStore& profiles_;
    std::map<DWORD, bool> browserByPid_;
    std::map<HWND, std::wstring> profileByWindow_;
    DWORD browserPid_ = 0;

    std::wstring profileOf(HWND hwnd);
};

// Opens a window of the profile: Chrome, if it runs, gets the command and opens the window itself.
bool openProfile(const Target& target, const std::wstring& dir);

// Window geometry. All rectangles are physical pixels in virtual screen coordinates.
RECT visibleFrame(HWND hwnd);           // without the invisible resize borders
RECT workAreaFor(const RECT& frame);    // the work area of the monitor showing most of it
bool isCloaked(HWND hwnd);              // on another virtual desktop
bool isFullscreen(HWND hwnd);
void setVisibleFrame(HWND hwnd, const RECT& frame);
// Brings the window forward with the keyboard. With force, right after the user's click or hotkey,
// Windows' refusal is overridden by joining the input queues for a moment.
void bringForward(HWND hwnd, bool force);

// ---------------------------------------------------------------- settings

// What the user set up: tab order, hidden profiles, own names. Kept in the registry under
// HKCU\Software\GoogleChromeProfileTabs (\Test for the test copy).
class Settings {
public:
    explicit Settings(bool isTest);
    std::vector<std::wstring> order() const;
    void setOrder(const std::vector<std::wstring>& dirs);
    std::set<std::wstring> hidden() const;
    void setHidden(const std::set<std::wstring>& dirs);
    // Profiles closed and folded into icons at the right end of the strip.
    std::set<std::wstring> collapsed() const;
    void setCollapsed(const std::set<std::wstring>& dirs);
    std::map<std::wstring, std::wstring> customNames() const;
    void setCustomName(const std::wstring& dir, const std::wstring& name);  // empty name removes it
    bool namesHidden() const;
    void setNamesHidden(bool hidden);
    uint64_t scrambleSeed() const;
    void setScrambleSeed(uint64_t seed);
    bool startupOffered() const;
    void setStartupOffered(bool offered);

    std::vector<Profile> arrange(const std::vector<Profile>& profiles) const;
    void reorder(const std::vector<std::wstring>& visible, const std::vector<Profile>& profiles);

    static bool startsAtLogin();
    static void setStartsAtLogin(bool on);

private:
    std::wstring key_;
    DWORD dword(const wchar_t* name) const;
    void setDword(const wchar_t* name, DWORD value);
    std::vector<std::wstring> strings(const wchar_t* name) const;
    void setStrings(const wchar_t* name, const std::vector<std::wstring>& values);
};

// Gibberish in place of a name: same length, spaces and case, alternating consonants and vowels so
// it looks like a word. The same name with the same seed always gives the same gibberish.
std::wstring gibberish(const std::wstring& text, uint64_t seed);
