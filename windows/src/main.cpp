// Google Chrome Profile Tabs for Windows: which window the strip sits on, what a click on a tab does,
// where a profile's window goes.
#include "common.h"
#include "strip.h"

#include <objbase.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <random>

extern bool g_logToFile;

static const UINT kRefresh = WM_APP + 10;
static const UINT_PTR kHeartbeat = 1, kRoom = 2, kPending = 3;

class App {
public:
    bool start(HINSTANCE instance);

private:
    Target target_;
    ProfileStore* profiles_ = nullptr;
    Windows* windows_ = nullptr;
    Settings* settings_ = nullptr;
    Strip strip_;
    HWND control_ = nullptr;
    IVirtualDesktopManager* desktops_ = nullptr;

    HWND attached_ = nullptr;
    std::wstring attachedDir_;
    // The window just switched to: Windows reports the new foreground window a moment later, and
    // without this the strip would jump back for an instant.
    HWND sticky_ = nullptr;
    ULONGLONG stickyUntil_ = 0;
    // A profile whose new window was asked for: when it shows up it goes where the current one is.
    bool pending_ = false;
    std::wstring pendingDir_;
    RECT pendingFrame_{};
    bool pendingHasFrame_ = false;
    std::set<HWND> pendingKnown_;
    ULONGLONG pendingDeadline_ = 0;
    HWND pendingForeground_ = nullptr;  // who had the keyboard when the window was asked for

    bool dragging_ = false;
    HWND draggedWindow_ = nullptr;
    // The window that had the keyboard before the strip took it for a menu or renaming.
    HWND previousForeground_ = nullptr;
    ULONGLONG lastMotion_ = 0;
    bool refreshQueued_ = false;
    bool hotkeysOn_ = false;
    HWINEVENTHOOK systemHook_ = nullptr, objectHook_ = nullptr, cloakHook_ = nullptr;
    DWORD hookedPid_ = 0;
    std::vector<Profile> visibleTabs_;

    static App* self_;
    static LRESULT CALLBACK controlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static void CALLBACK winEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void onWinEvent(DWORD event, HWND hwnd);

    void heartbeat();
    void queueRefresh();
    void refresh();
    void hookBrowser(DWORD pid);
    void setHotkeys(bool on);
    void showTabs(const std::set<std::wstring>& open);
    std::wstring titleFor(const std::wstring& dir) const;
    void layout();
    void follow();
    void scheduleRoom();
    void makeRoom();
    void select(const std::wstring& dir);
    void open(const std::wstring& dir, const RECT* frame);
    void resolvePending(const std::vector<BrowserWindow>& windows);
    void stopPending();
    void beginRename(const std::wstring& dir);
    void rename(const std::wstring& dir, const std::wstring& text);
    void editingEnded();
    void toggleNamesHidden();
    void contextMenu(const std::wstring& dir, POINT point);
    void testCommand(const std::wstring& command);
    void dumpState();
};

App* App::self_ = nullptr;

// ---------------------------------------------------------------- startup

bool App::start(HINSTANCE instance) {
    self_ = this;
    if (!Target::resolve(target_)) {
        MessageBoxW(nullptr, L"Google Chrome Profile Tabs shows Google Chrome profiles. Install Chrome and open Google Chrome Profile Tabs again.",
                    L"Google Chrome not found", MB_ICONINFORMATION);
        return false;
    }
    g_logToFile = target_.isTest;
    logLine(L"launched for %ls, user data %ls", target_.exe.c_str(), target_.userDataDir.c_str());
    profiles_ = new ProfileStore(target_);
    windows_ = new Windows(target_, *profiles_);
    settings_ = new Settings(target_.isTest);
    if (!target_.isTest && !settings_->startupOffered()) {
        // Start at login is turned on once, on the first run; after that it is the user's call.
        Settings::setStartsAtLogin(true);
        settings_->setStartupOffered(true);
    }
    CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&desktops_));

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = controlProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = target_.isTest ? L"GoogleChromeProfileTabsTestControl" : L"GoogleChromeProfileTabsControl";
    RegisterClassExW(&windowClass);
    control_ = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!control_ || !strip_.create(instance)) return false;

    strip_.onSelect = [this](const std::wstring& dir) { select(dir); };
    strip_.onReorder = [this](const std::vector<std::wstring>& dirs) {
        settings_->reorder(dirs, profiles_->profiles());
        refresh();
    };
    strip_.onRequestRename = [this](const std::wstring& dir) { beginRename(dir); };
    strip_.onRename = [this](const std::wstring& dir, const std::wstring& text) { rename(dir, text); };
    strip_.onEditingEnded = [this] { editingEnded(); };
    strip_.onTogglePrivacy = [this] { toggleNamesHidden(); };
    strip_.onContextMenu = [this](const std::wstring& dir, POINT point) { contextMenu(dir, point); };
    strip_.onEnvironmentChanged = [this] { queueRefresh(); };

    // Foreground changes, window drags and minimizing anywhere in the system.
    systemHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr, winEvent, 0, 0,
                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(control_, kHeartbeat, 1000, nullptr);
    profiles_->reloadIfChanged();
    refresh();
    return true;
}

// ---------------------------------------------------------------- events

LRESULT CALLBACK App::controlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    return self_ && self_->control_ == hwnd ? self_->handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT App::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_TIMER:
            if (wParam == kHeartbeat) heartbeat();
            if (wParam == kRoom) {
                KillTimer(control_, kRoom);
                makeRoom();
            }
            if (wParam == kPending) refresh();
            return 0;
        case kRefresh:
            refreshQueued_ = false;
            refresh();
            return 0;
        case WM_HOTKEY: {
            int number = static_cast<int>(wParam);
            if (number >= 1 && number <= static_cast<int>(visibleTabs_.size())) select(visibleTabs_[number - 1].dir);
            return 0;
        }
        case WM_COPYDATA: {
            auto* data = reinterpret_cast<COPYDATASTRUCT*>(lParam);
            if (target_.isTest && data->lpData) {
                testCommand(std::wstring(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t)));
            }
            return TRUE;
        }
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            queueRefresh();
            return 0;
    }
    return DefWindowProcW(control_, message, wParam, lParam);
}

void CALLBACK App::winEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
    if (!self_ || !hwnd || object != OBJID_WINDOW || child != CHILDID_SELF) return;
    self_->onWinEvent(event, hwnd);
}

void App::onWinEvent(DWORD event, HWND hwnd) {
    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        // The strip follows its window on every move, right as the window moves.
        if (hwnd == attached_) {
            lastMotion_ = GetTickCount64();
            follow();
            if (!dragging_) scheduleRoom();
        }
        return;
    }
    if (event == EVENT_SYSTEM_MOVESIZESTART && hwnd == attached_) {
        dragging_ = true;
        draggedWindow_ = hwnd;
        return;
    }
    if (event == EVENT_SYSTEM_MOVESIZEEND && hwnd == draggedWindow_) {
        dragging_ = false;
        draggedWindow_ = nullptr;
        layout();
        return;
    }
    if (event == EVENT_OBJECT_NAMECHANGE && !pending_) return;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return;
    queueRefresh();
}

void App::queueRefresh() {
    if (refreshQueued_) return;
    refreshQueued_ = true;
    PostMessageW(control_, kRefresh, 0, 0);
}

void App::heartbeat() {
    if (profiles_->reloadIfChanged()) windows_->forgetProfiles();
    refresh();
}

// Window events of the browser process only: there are plenty of them, and the strip needs just
// Chrome's.
void App::hookBrowser(DWORD pid) {
    if (pid == hookedPid_) return;
    if (objectHook_) UnhookWinEvent(objectHook_);
    if (cloakHook_) UnhookWinEvent(cloakHook_);
    objectHook_ = cloakHook_ = nullptr;
    hookedPid_ = pid;
    if (!pid) return;
    objectHook_ = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_NAMECHANGE, nullptr, winEvent, pid, 0, WINEVENT_OUTOFCONTEXT);
    cloakHook_ = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, winEvent, pid, 0, WINEVENT_OUTOFCONTEXT);
}

// Alt+1...9 switch to that tab while a Chrome window is in front, and are left to other programs
// otherwise. Not Ctrl: on Windows Chrome itself switches its tabs with Ctrl+1...9.
void App::setHotkeys(bool on) {
    if (on == hotkeysOn_) return;
    hotkeysOn_ = on;
    for (int number = 1; number <= 9; number++) {
        if (on) RegisterHotKey(control_, number, MOD_ALT | MOD_NOREPEAT, '0' + number);
        else UnregisterHotKey(control_, number);
    }
}

// ---------------------------------------------------------------- windows

// Rebuilds the picture: which windows belong to which profiles, which window the strip sits on.
void App::refresh() {
    std::vector<BrowserWindow> windows = windows_->list();
    hookBrowser(windows_->browserPid());
    resolvePending(windows);

    HWND foreground = GetForegroundWindow();
    // Only browser windows: DevTools and installed web apps keep their own Alt shortcuts.
    bool browserInFront = std::any_of(windows.begin(), windows.end(), [&](const BrowserWindow& window) { return window.hwnd == foreground; });
    setHotkeys(browserInFront && !strip_.isEditing());
    if (draggedWindow_ && draggedWindow_ != attached_ && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        dragging_ = false;
        draggedWindow_ = nullptr;
    }

    auto usable = [](const BrowserWindow& window) { return !window.minimized && !isCloaked(window.hwnd); };
    auto find = [&](HWND hwnd) -> const BrowserWindow* {
        for (const auto& window : windows) {
            if (window.hwnd == hwnd) return &window;
        }
        return nullptr;
    };
    const BrowserWindow* chosen = nullptr;
    if (sticky_ && GetTickCount64() < stickyUntil_) chosen = find(sticky_);
    if (!chosen) {
        const BrowserWindow* front = find(foreground);
        if (front && usable(*front)) chosen = front;
    }
    if (!chosen) {
        const BrowserWindow* current = find(attached_);
        if (current && usable(*current)) chosen = current;
    }
    if (!chosen) {
        for (const auto& window : windows) {
            if (usable(window)) {
                chosen = &window;
                break;
            }
        }
    }
    attached_ = chosen ? chosen->hwnd : nullptr;
    attachedDir_ = chosen ? chosen->dir : L"";

    std::set<std::wstring> open;
    for (const auto& window : windows) open.insert(window.dir);
    showTabs(open);
    layout();
}

void App::showTabs(const std::set<std::wstring>& open) {
    auto hidden = settings_->hidden();
    bool namesHidden = settings_->namesHidden();
    visibleTabs_.clear();
    std::vector<Tab> tabs;
    for (const auto& profile : settings_->arrange(profiles_->profiles())) {
        if (hidden.count(profile.dir)) continue;
        visibleTabs_.push_back(profile);
        Tab tab;
        tab.key = profile.dir;
        tab.title = titleFor(profile.dir);
        tab.color = profile.color;
        // The account photo gives a person away as well as a name, so it goes too while names are hidden.
        tab.picture = namesHidden ? L"" : profile.picture;
        tab.isOpen = open.count(profile.dir) > 0;
        tab.signature = profile.signature + L"|" + tab.title + L"|" + (tab.isOpen ? L"1" : L"0") + L"|" + tab.picture;
        tabs.push_back(tab);
    }
    strip_.show(tabs, attachedDir_, namesHidden);
}

// What the tab says: the user's own name or Chrome's, and gibberish made of it while names are hidden.
std::wstring App::titleFor(const std::wstring& dir) const {
    auto names = settings_->customNames();
    const Profile* profile = profiles_->find(dir);
    std::wstring shown = names.count(dir) ? names[dir] : (profile ? profile->name : dir);
    return settings_->namesHidden() ? gibberish(shown, settings_->scrambleSeed()) : shown;
}

void App::layout() {
    if (!attached_ || !IsWindowVisible(attached_) || IsIconic(attached_) || isCloaked(attached_) || isFullscreen(attached_)) {
        if (!strip_.isEditing()) strip_.hide();
        return;
    }
    // The strip lives on its window's virtual desktop.
    if (desktops_) {
        GUID desktop{}, ours{};
        if (SUCCEEDED(desktops_->GetWindowDesktopId(attached_, &desktop)) && SUCCEEDED(desktops_->GetWindowDesktopId(strip_.hwnd(), &ours)) &&
            !IsEqualGUID(desktop, ours) && !IsEqualGUID(desktop, GUID{})) {
            desktops_->MoveWindowToDesktop(strip_.hwnd(), desktop);
        }
    }
    strip_.place(visibleFrame(attached_), attached_, GetDpiForWindow(attached_), false);
    scheduleRoom();
}

void App::follow() {
    if (!attached_ || IsIconic(attached_) || isFullscreen(attached_)) {
        strip_.hide();
        return;
    }
    strip_.place(visibleFrame(attached_), attached_, GetDpiForWindow(attached_), dragging_);
}

void App::scheduleRoom() { SetTimer(control_, kRoom, 350, nullptr); }

// If the window reaches the top of the screen and the strip has no room, the window goes down by the
// strip's height and its bottom stays put. A maximized window becomes one that fills the work area
// under the strip. Nothing happens while the window is being dragged.
void App::makeRoom() {
    if (!attached_ || strip_.isEditing()) return;
    if (dragging_ || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || GetTickCount64() - lastMotion_ < 300) {
        scheduleRoom();
        return;
    }
    if (IsIconic(attached_) || isFullscreen(attached_) || isCloaked(attached_)) return;
    RECT frame = visibleFrame(attached_);
    RECT work = workAreaFor(frame);
    UINT dpi = GetDpiForWindow(attached_);
    LONG needed = work.top + Strip::reserve(dpi);
    RECT fixed = frame;
    if (IsZoomed(attached_)) {
        fixed = RECT{work.left, needed, work.right, work.bottom};
    } else if (frame.top < needed) {
        fixed.top = needed;
        fixed.bottom = std::min(frame.bottom, work.bottom);
    } else if (frame.top == needed && frame.bottom > work.bottom && frame.bottom - work.bottom <= Strip::reserve(dpi) + 1) {
        fixed.bottom = work.bottom;
    } else {
        return;
    }
    if (fixed.bottom - fixed.top < static_cast<LONG>(300 * dpi / 96)) return;
    setVisibleFrame(attached_, fixed);
    follow();
}

// ---------------------------------------------------------------- switching

// The profile's window takes the place of the current one and comes forward. No window: a new one opens.
void App::select(const std::wstring& dir) {
    logLine(L"select %ls", dir.c_str());
    if (attached_ && attachedDir_ == dir) {
        bringForward(attached_, true);
        return;
    }
    RECT frame{};
    bool hasFrame = attached_ != nullptr;
    if (hasFrame) frame = visibleFrame(attached_);
    std::vector<BrowserWindow> windows = windows_->list();
    const BrowserWindow* target = nullptr;
    for (const auto& window : windows) {
        if (window.dir == dir && !window.minimized && !isCloaked(window.hwnd)) { target = &window; break; }
    }
    for (const auto& window : windows) {
        if (!target && window.dir == dir && window.minimized && !isCloaked(window.hwnd)) { target = &window; break; }
    }
    // A window on another virtual desktop counts as absent: Windows would jump there, and the point of
    // a tab is to show the profile here.
    if (!target) {
        open(dir, hasFrame ? &frame : nullptr);
        return;
    }
    if (hasFrame) setVisibleFrame(target->hwnd, frame);
    bringForward(target->hwnd, true);
    sticky_ = target->hwnd;
    stickyUntil_ = GetTickCount64() + 1500;
    refresh();
}

void App::open(const std::wstring& dir, const RECT* frame) {
    pending_ = true;
    pendingDir_ = dir;
    pendingHasFrame_ = frame != nullptr;
    if (frame) pendingFrame_ = *frame;
    pendingKnown_.clear();
    for (const auto& window : windows_->list()) {
        if (window.dir == dir) pendingKnown_.insert(window.hwnd);
    }
    pendingDeadline_ = GetTickCount64() + 20000;
    pendingForeground_ = GetForegroundWindow();
    SetTimer(control_, kPending, 100, nullptr);
    if (!openProfile(target_, dir)) stopPending();
}

void App::resolvePending(const std::vector<BrowserWindow>& windows) {
    if (!pending_) return;
    for (const auto& window : windows) {
        if (window.dir != pendingDir_ || pendingKnown_.count(window.hwnd) || window.minimized) continue;
        stopPending();
        if (pendingHasFrame_) setVisibleFrame(window.hwnd, pendingFrame_);
        // Chrome may take a while to open the window. If the user went to another program meanwhile, or
        // is renaming a tab, the window is only put in place: taking the keyboard would be a theft.
        HWND foreground = GetForegroundWindow();
        bool untouched = foreground == pendingForeground_;
        bool withChrome = !foreground || std::any_of(windows.begin(), windows.end(), [&](const BrowserWindow& other) { return other.hwnd == foreground; });
        if ((untouched || withChrome) && !strip_.isEditing()) bringForward(window.hwnd, untouched);
        sticky_ = window.hwnd;
        stickyUntil_ = GetTickCount64() + 1500;
        return;
    }
    if (GetTickCount64() > pendingDeadline_) stopPending();
}

void App::stopPending() {
    pending_ = false;
    KillTimer(control_, kPending);
}

// ---------------------------------------------------------------- names

void App::beginRename(const std::wstring& dir) {
    // While names are hidden the field would show the real one.
    if (settings_->namesHidden()) return;
    auto names = settings_->customNames();
    const Profile* profile = profiles_->find(dir);
    HWND foreground = GetForegroundWindow();
    if (foreground != strip_.hwnd()) previousForeground_ = foreground;
    setHotkeys(false);
    strip_.beginEditing(dir, names.count(dir) ? names[dir] : (profile ? profile->name : dir));
}

// An empty name, or Chrome's own, gives the tab Chrome's name back.
void App::rename(const std::wstring& dir, const std::wstring& text) {
    logLine(L"rename %ls to %ls", dir.c_str(), text.c_str());
    std::wstring trimmed = text;
    trimmed.erase(0, trimmed.find_first_not_of(L" \t"));
    trimmed.erase(trimmed.find_last_not_of(L" \t") + 1);
    const Profile* profile = profiles_->find(dir);
    settings_->setCustomName(dir, profile && trimmed == profile->name ? L"" : trimmed);
    refresh();
}

// After renaming, the keyboard goes back where it was, unless the user went to another program.
void App::editingEnded() {
    HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == strip_.hwnd()) {
        HWND back = previousForeground_ && IsWindow(previousForeground_) ? previousForeground_ : attached_;
        if (back) bringForward(back, false);
    }
    previousForeground_ = nullptr;
    refresh();
}

void App::toggleNamesHidden() {
    if (!settings_->namesHidden()) strip_.cancelEditing();
    logLine(L"names hidden: %d -> %d", settings_->namesHidden(), !settings_->namesHidden());
    if (!settings_->namesHidden()) settings_->setScrambleSeed(std::random_device{}() * 6364136223846793005ULL + std::random_device{}() + 1);
    settings_->setNamesHidden(!settings_->namesHidden());
    refresh();
}

// ---------------------------------------------------------------- menu

void App::contextMenu(const std::wstring& dir, POINT point) {
    enum { kRename = 1, kReset, kHide, kStartup, kQuit, kShowHidden = 100 };
    HMENU menu = CreatePopupMenu();
    bool namesHidden = settings_->namesHidden();
    auto names = settings_->customNames();
    if (!dir.empty()) {
        if (!namesHidden) {
            AppendMenuW(menu, MF_STRING, kRename, L"Rename…");
            const Profile* profile = profiles_->find(dir);
            if (names.count(dir) && profile) AppendMenuW(menu, MF_STRING, kReset, (L"Reset Name to “" + profile->name + L"”").c_str());
        }
        AppendMenuW(menu, MF_STRING, kHide, L"Hide Tab");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    auto hidden = settings_->hidden();
    std::vector<std::wstring> hiddenDirs;
    HMENU submenu = CreatePopupMenu();
    for (const auto& profile : settings_->arrange(profiles_->profiles())) {
        if (!hidden.count(profile.dir)) continue;
        AppendMenuW(submenu, MF_STRING, kShowHidden + hiddenDirs.size(), titleFor(profile.dir).c_str());
        hiddenDirs.push_back(profile.dir);
    }
    if (hiddenDirs.empty()) DestroyMenu(submenu);
    else AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), L"Show Hidden Profile");
    if (!target_.isTest) AppendMenuW(menu, MF_STRING | (Settings::startsAtLogin() ? MF_CHECKED : 0), kStartup, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kQuit, L"Quit Google Chrome Profile Tabs");

    // A menu closes properly on a click elsewhere only if its window is in front.
    HWND before = GetForegroundWindow();
    if (before != strip_.hwnd()) previousForeground_ = before;
    setHotkeys(false);
    strip_.setInteractive(true);
    SetForegroundWindow(strip_.hwnd());
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, point.x, point.y, strip_.hwnd(), nullptr);
    PostMessageW(strip_.hwnd(), WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (command != kRename) {
        strip_.setInteractive(false);
        // Esc or a pick: the keyboard goes back to the window that had it before the right click.
        HWND back = previousForeground_ && IsWindow(previousForeground_) ? previousForeground_ : attached_;
        if (GetForegroundWindow() == strip_.hwnd() && back) bringForward(back, false);
        previousForeground_ = nullptr;
    }
    switch (command) {
        case kRename: beginRename(dir); break;
        case kReset: rename(dir, L""); break;
        case kHide: {
            hidden.insert(dir);
            settings_->setHidden(hidden);
            refresh();
            break;
        }
        case kStartup: Settings::setStartsAtLogin(!Settings::startsAtLogin()); break;
        case kQuit: PostQuitMessage(0); break;
        default:
            if (command >= kShowHidden && command < kShowHidden + hiddenDirs.size()) {
                hidden.erase(hiddenDirs[command - kShowHidden]);
                settings_->setHidden(hidden);
                refresh();
            }
    }
    if (command == kRename && !strip_.isEditing()) strip_.setInteractive(false);
}

// ---------------------------------------------------------------- test hooks

// The test copy takes commands over WM_COPYDATA instead of synthetic clicks on the screen:
// "select:<folder>", "rename:<folder>", "names", "dump", "snapshot".
void App::testCommand(const std::wstring& command) {
    logLine(L"test command %ls", command.c_str());
    auto argument = [&](const wchar_t* prefix) {
        size_t length = wcslen(prefix);
        return command.compare(0, length, prefix) == 0 ? command.substr(length) : std::wstring();
    };
    if (!argument(L"select:").empty()) select(argument(L"select:"));
    else if (!argument(L"rename:").empty()) beginRename(argument(L"rename:"));
    else if (command == L"names") toggleNamesHidden();
    else if (command == L"dump") dumpState();
    else if (command == L"snapshot") {
        wchar_t path[MAX_PATH];
        GetTempPathW(MAX_PATH, path);
        wcscat(path, L"gcpt-strip.png");
        logLine(L"snapshot %ls: %d", path, strip_.snapshot(path));
    }
}

void App::dumpState() {
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat(path, L"gcpt-state.txt");
    FILE* file = _wfopen(path, L"w, ccs=UTF-8");
    if (!file) return;
    RECT strip;
    GetWindowRect(strip_.hwnd(), &strip);
    HWND above = GetWindow(attached_, GW_HWNDPREV);
    fwprintf(file, L"strip visible=%d rect=%ld,%ld,%ld,%ld editing=%d directly-above-window=%d\n", strip_.isShown(), strip.left, strip.top,
             strip.right, strip.bottom, strip_.isEditing(), above == strip_.hwnd());
    if (attached_) {
        RECT frame = visibleFrame(attached_);
        fwprintf(file, L"attached hwnd=%p dir=%ls frame=%ld,%ld,%ld,%ld dpi=%u zoomed=%d\n", attached_, attachedDir_.c_str(), frame.left,
                 frame.top, frame.right, frame.bottom, GetDpiForWindow(attached_), IsZoomed(attached_));
    } else {
        fwprintf(file, L"attached none\n");
    }
    fwprintf(file, L"foreground=%p hotkeys=%d namesHidden=%d\n", GetForegroundWindow(), hotkeysOn_, settings_->namesHidden());
    for (const auto& window : windows_->list()) {
        RECT frame = visibleFrame(window.hwnd);
        fwprintf(file, L"window hwnd=%p dir=%ls minimized=%d frame=%ld,%ld,%ld,%ld\n", window.hwnd, window.dir.c_str(), window.minimized,
                 frame.left, frame.top, frame.right, frame.bottom);
    }
    for (const auto& profile : visibleTabs_) {
        fwprintf(file, L"tab dir=%ls title=%ls color=%06x picture=%d\n", profile.dir.c_str(), titleFor(profile.dir).c_str(), profile.color,
                 !profile.picture.empty());
    }
    fclose(file);
}

// ---------------------------------------------------------------- entry

// Dark context menus when Windows is in dark mode. uxtheme has no public call for it; these are the
// ordinals every app uses, looked up so that a Windows without them just keeps light menus.
static void allowDarkMenus() {
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    using SetPreferredAppMode = int(WINAPI*)(int);
    using FlushMenuThemes = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    if (setMode) setMode(1);  // follow the system
    if (flush) flush();
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool test = GetEnvironmentVariableW(L"GOOGLE_CHROME_PROFILE_TABS_BROWSER", nullptr, 0) > 0;
    HANDLE single = CreateMutexW(nullptr, TRUE, test ? L"Local\\GoogleChromeProfileTabsTest" : L"Local\\GoogleChromeProfileTabs");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    allowDarkMenus();
    App app;
    if (!app.start(instance)) return 1;
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    CloseHandle(single);
    return 0;
}
