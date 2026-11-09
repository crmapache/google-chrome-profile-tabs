// Settings in the registry, start at login, and the gibberish for hidden names.
#include "common.h"

#include <algorithm>
#include <cwctype>

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"GoogleChromeProfileTabs";

Settings::Settings(bool isTest) {
    // The test copy keeps its own settings so checks never shuffle the real tabs.
    key_ = isTest ? L"Software\\GoogleChromeProfileTabs\\Test" : L"Software\\GoogleChromeProfileTabs";
}

DWORD Settings::dword(const wchar_t* name) const {
    DWORD value = 0, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, key_.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value;
}

void Settings::setDword(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key_.c_str(), name, REG_DWORD, &value, sizeof(value));
}

std::vector<std::wstring> Settings::strings(const wchar_t* name) const {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key_.c_str(), name, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) return {};
    std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, key_.c_str(), name, RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(), &size) != ERROR_SUCCESS) return {};
    std::vector<std::wstring> values;
    for (const wchar_t* item = buffer.c_str(); *item; item += wcslen(item) + 1) values.emplace_back(item);
    return values;
}

void Settings::setStrings(const wchar_t* name, const std::vector<std::wstring>& values) {
    std::wstring block;
    for (const auto& value : values) {
        block += value;
        block += L'\0';
    }
    block += L'\0';
    RegSetKeyValueW(HKEY_CURRENT_USER, key_.c_str(), name, REG_MULTI_SZ, block.data(), static_cast<DWORD>(block.size() * sizeof(wchar_t)));
}

std::vector<std::wstring> Settings::order() const { return strings(L"Order"); }
void Settings::setOrder(const std::vector<std::wstring>& dirs) { setStrings(L"Order", dirs); }

std::set<std::wstring> Settings::hidden() const {
    auto values = strings(L"Hidden");
    return {values.begin(), values.end()};
}

void Settings::setHidden(const std::set<std::wstring>& dirs) { setStrings(L"Hidden", {dirs.begin(), dirs.end()}); }

std::set<std::wstring> Settings::collapsed() const {
    auto values = strings(L"Collapsed");
    return {values.begin(), values.end()};
}

void Settings::setCollapsed(const std::set<std::wstring>& dirs) { setStrings(L"Collapsed", {dirs.begin(), dirs.end()}); }

std::map<std::wstring, std::wstring> Settings::customNames() const {
    std::map<std::wstring, std::wstring> names;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (key_ + L"\\CustomNames").c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return names;
    for (DWORD index = 0;; index++) {
        wchar_t name[256], value[1024];
        DWORD nameLength = 256, valueSize = sizeof(value), type = 0;
        if (RegEnumValueW(key, index, name, &nameLength, nullptr, &type, reinterpret_cast<BYTE*>(value), &valueSize) != ERROR_SUCCESS) break;
        if (type == REG_SZ) names[name] = value;
    }
    RegCloseKey(key);
    return names;
}

void Settings::setCustomName(const std::wstring& dir, const std::wstring& name) {
    std::wstring path = key_ + L"\\CustomNames";
    if (name.empty()) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, path.c_str(), dir.c_str());
        return;
    }
    RegSetKeyValueW(HKEY_CURRENT_USER, path.c_str(), dir.c_str(), REG_SZ, name.c_str(), static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)));
}

bool Settings::namesHidden() const { return dword(L"NamesHidden") != 0; }
void Settings::setNamesHidden(bool hidden) { setDword(L"NamesHidden", hidden ? 1 : 0); }

uint64_t Settings::scrambleSeed() const {
    uint64_t value = 0;
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, key_.c_str(), L"ScrambleSeed", RRF_RT_REG_QWORD, nullptr, &value, &size);
    return value;
}

void Settings::setScrambleSeed(uint64_t seed) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key_.c_str(), L"ScrambleSeed", REG_QWORD, &seed, sizeof(seed));
}

bool Settings::startupOffered() const { return dword(L"StartupOffered") != 0; }
void Settings::setStartupOffered(bool offered) { setDword(L"StartupOffered", offered ? 1 : 0); }

// Saved order first; profiles it does not know yet go to the end in Chrome's order.
std::vector<Profile> Settings::arrange(const std::vector<Profile>& profiles) const {
    std::vector<Profile> arranged;
    auto saved = order();
    for (const auto& dir : saved) {
        for (const auto& profile : profiles) {
            if (profile.dir == dir) arranged.push_back(profile);
        }
    }
    for (const auto& profile : profiles) {
        if (std::find(saved.begin(), saved.end(), profile.dir) == saved.end()) arranged.push_back(profile);
    }
    return arranged;
}

// A new order of the tabs in the row. Hidden and collapsed profiles keep their places among the rest.
void Settings::reorder(const std::vector<std::wstring>& visible, const std::vector<Profile>& profiles) {
    std::set<std::wstring> shown(visible.begin(), visible.end());
    std::vector<std::wstring> next;
    size_t index = 0;
    for (const auto& profile : arrange(profiles)) {
        if (!shown.count(profile.dir) || index >= visible.size()) next.push_back(profile.dir);
        else next.push_back(visible[index++]);
    }
    setOrder(next);
}

static std::wstring executablePath() {
    wchar_t path[MAX_PATH * 2];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    return std::wstring(path, length);
}

bool Settings::startsAtLogin() {
    wchar_t value[MAX_PATH * 2];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return false;
    return lower(value) == lower(L"\"" + executablePath() + L"\"");
}

void Settings::setStartsAtLogin(bool on) {
    if (!on) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        return;
    }
    std::wstring command = L"\"" + executablePath() + L"\"";
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, command.c_str(), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
}

std::wstring gibberish(const std::wstring& text, uint64_t seed) {
    static const wchar_t consonants[] = L"bcdfghjklmnprstvz";
    static const wchar_t vowels[] = L"aeiou";
    uint64_t state = seed ^ 14695981039346656037ULL;
    for (wchar_t c : text) state = (state ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
    state |= 1;
    auto next = [&]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    bool vowel = next() % 2 == 0;
    std::wstring out;
    for (wchar_t c : text) {
        if (iswdigit(c)) {
            out += static_cast<wchar_t>(L'0' + next() % 10);
        } else if (iswalpha(c)) {
            const wchar_t* pool = vowel ? vowels : consonants;
            size_t size = vowel ? 5 : 17;
            vowel = !vowel;
            wchar_t letter = pool[next() % size];
            out += iswupper(c) ? static_cast<wchar_t>(towupper(letter)) : letter;
        } else {
            out += c;
        }
    }
    return out;
}
