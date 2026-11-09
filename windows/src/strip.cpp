// The strip window: Direct2D drawing, mouse, renaming in place, the buttons that close and collapse a
// profile, the icons of collapsed profiles and the button that hides the names.
#include "strip.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <cmath>

// Newer DWM attributes, missing from older SDK headers.
static const DWORD kCornerPreference = 33;  // DWMWA_WINDOW_CORNER_PREFERENCE
static const DWORD kBorderColor = 34;       // DWMWA_BORDER_COLOR
static const int kRoundCorners = 2;         // DWMWCP_ROUND
static const UINT kEndEdit = WM_APP + 1;
static const UINT_PTR kPressTimer = 1;

template <typename T>
static void release(T*& pointer) {
    if (pointer) pointer->Release();
    pointer = nullptr;
}

// ---------------------------------------------------------------- palette

// Colors taken from the JetBrains project tab row (dark theme), with a light pair to match.
struct Palette {
    uint32_t background, active, hover, separator, border, text, activeText;
};

static const Palette kDark{0x181819, 0x4A4B51, 0x26272B, 0x393B3E, 0x2B2D30, 0xC1C4CA, 0xF0F1F3};
static const Palette kLight{0xE9EAEE, 0xFFFFFF, 0xDCDEE3, 0xC9CCD2, 0xD3D5DB, 0x4E5157, 0x111214};

static D2D1_COLOR_F color(uint32_t rgb, float alpha = 1) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, alpha);
}

static COLORREF colorRef(uint32_t rgb) { return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

static bool systemIsDark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

// The profile color for the line under the selected tab: a dark color on the dark strip is lifted,
// a light one on the light strip is darkened, so the line stays readable.
static D2D1_COLOR_F accent(uint32_t rgb, bool dark) {
    float r = ((rgb >> 16) & 0xFF) / 255.f, g = ((rgb >> 8) & 0xFF) / 255.f, b = (rgb & 0xFF) / 255.f;
    float maximum = std::max({r, g, b}), minimum = std::min({r, g, b});
    float brightness = maximum, saturation = maximum > 0 ? (maximum - minimum) / maximum : 0, hue = 0;
    if (maximum != minimum) {
        float delta = maximum - minimum;
        if (maximum == r) hue = std::fmod((g - b) / delta, 6.f);
        else if (maximum == g) hue = (b - r) / delta + 2;
        else hue = (r - g) / delta + 4;
        hue /= 6;
        if (hue < 0) hue += 1;
    }
    brightness = dark ? std::max(brightness, 0.8f) : std::min(brightness, 0.62f);
    float h = hue * 6, c = brightness * saturation, x = c * (1 - std::fabs(std::fmod(h, 2.f) - 1)), m = brightness - c;
    float rr = 0, gg = 0, bb = 0;
    if (h < 1) { rr = c; gg = x; } else if (h < 2) { rr = x; gg = c; } else if (h < 3) { gg = c; bb = x; }
    else if (h < 4) { gg = x; bb = c; } else if (h < 5) { rr = x; bb = c; } else { rr = c; bb = x; }
    return D2D1::ColorF(rr + m, gg + m, bb + m, 1);
}

static bool isLight(uint32_t rgb) {
    return 0.299 * ((rgb >> 16) & 0xFF) + 0.587 * ((rgb >> 8) & 0xFF) + 0.114 * (rgb & 0xFF) > 153;
}

// ---------------------------------------------------------------- setup

bool Strip::create(HINSTANCE instance) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dwrite_));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    if (!d2d_ || !dwrite_) return false;

    IDWriteFontCollection* fonts = nullptr;
    dwrite_->GetSystemFontCollection(&fonts);
    auto family = [&](std::initializer_list<const wchar_t*> names) {
        for (const wchar_t* name : names) {
            UINT32 index;
            BOOL exists = FALSE;
            if (fonts && SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists) return name;
        }
        return *names.begin();
    };
    const wchar_t* text = family({L"Segoe UI Variable Text", L"Segoe UI"});
    const wchar_t* icons = family({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});
    release(fonts);
    dwrite_->CreateTextFormat(text, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"", &regular_);
    dwrite_->CreateTextFormat(text, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"", &semibold_);
    dwrite_->CreateTextFormat(text, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 9.5f, L"", &initial_);
    dwrite_->CreateTextFormat(icons, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13, L"", &glyph_);
    dwrite_->CreateTextFormat(icons, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10, L"", &smallGlyph_);
    for (IDWriteTextFormat* format : {regular_, semibold_, initial_, glyph_, smallGlyph_}) {
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    initial_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    glyph_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    smallGlyph_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    dwrite_->CreateEllipsisTrimmingSign(regular_, &ellipsis_);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"GoogleChromeProfileTabsStrip";
    RegisterClassExW(&windowClass);
    // A tool window stays out of the taskbar and Alt+Tab; no-activate keeps clicks from taking
    // the keyboard away from Chrome.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass.lpszClassName, L"Google Chrome Profile Tabs",
                            WS_POPUP | WS_CLIPCHILDREN, 0, 0, 400, 30, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    DwmSetWindowAttribute(hwnd_, kCornerPreference, &kRoundCorners, sizeof(kRoundCorners));
    // The tooltip of the icons and the tab buttons. Always-tip: the strip's program is never the active one.
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, instance, nullptr);
    refreshTheme();
    return true;
}

void Strip::refreshTheme() {
    dark_ = systemIsDark();
    const Palette& palette = dark_ ? kDark : kLight;
    COLORREF border = colorRef(palette.border);
    DwmSetWindowAttribute(hwnd_, kBorderColor, &border, sizeof(border));
    if (tooltip_) SetWindowTheme(tooltip_, dark_ ? L"DarkMode_Explorer" : nullptr, nullptr);
    if (editBrush_) DeleteObject(editBrush_);
    editBrush_ = CreateSolidBrush(colorRef(palette.active));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int Strip::heightPixels(UINT dpi) { return static_cast<int>(std::lround(kHeight * dpi / 96.f)); }
int Strip::gapPixels(UINT dpi) { return static_cast<int>(std::lround(kGap * dpi / 96.f)); }

// Rounded the same way as the strip itself, so it never pokes a pixel above the work area.
int Strip::reserve(UINT dpi) { return heightPixels(dpi) + gapPixels(dpi); }

// ---------------------------------------------------------------- state from the controller

void Strip::show(const std::vector<Tab>& tabs, const std::vector<Tab>& icons, const std::wstring& active, bool namesHidden) {
    auto same = [](const std::vector<Tab>& a, const std::vector<Tab>& b, std::wstring Tab::*field) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (a[i].*field != b[i].*field) return false;
        }
        return true;
    };
    // The state comes every second and rarely changes: no redraw without a change.
    if (active == active_ && namesHidden == namesHidden_ && same(tabs, tabs_, &Tab::signature) && same(icons, icons_, &Tab::signature)) return;
    if (!same(tabs, tabs_, &Tab::key) || !same(icons, icons_, &Tab::key)) {
        dragging_ = false;
        pressIndex_ = -1;
        pressed_ = firstClick_ = Hit{};
    }
    tabs_ = tabs;
    icons_ = icons;
    active_ = active;
    namesHidden_ = namesHidden;
    // What is under the mouse may change with the tabs: a closed profile's tab loses its close button.
    if (hover_.kind != HoverKind::None) hover_ = hitAt(cursorInDips());
    // A field for a tab that is gone, or one that would show a real name while names are hidden, closes.
    if (edit_ && (namesHidden_ || std::none_of(tabs_.begin(), tabs_.end(), [&](const Tab& tab) { return tab.key == editingKey_; }))) {
        endEditing(false);
    }
    positionEditor();
    updateTooltip();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Strip::hoverAt(float x) {
    tooltipMuted_ = false;
    setHover(x < 0 ? Hit{} : hitAt(D2D1::Point2F(x, kHeight / 2)));
}

void Strip::place(const RECT& frame, HWND browser, UINT dpi, bool live) {
    int height = heightPixels(dpi);
    int gap = gapPixels(dpi);
    int x = frame.left, y = frame.top - gap - height, w = frame.right - frame.left;
    UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER;
    HWND after = HWND_TOP;
    if (live && isShown()) {
        // While the window is dragged the strip only moves; its place in the z-order is set when
        // the mouse is let go.
        flags |= SWP_NOZORDER;
    } else {
        // Right above the browser window: whatever covers Chrome covers the strip too.
        HWND above = GetWindow(browser, GW_HWNDPREV);
        while (above == hwnd_) above = GetWindow(above, GW_HWNDPREV);
        if (above && !(GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST)) after = above;
    }
    RECT current;
    GetWindowRect(hwnd_, &current);
    if (current.left == x && current.top == y && current.right - current.left == w && current.bottom - current.top == height) {
        flags |= SWP_NOMOVE | SWP_NOSIZE;
    } else if (tooltip_) {
        // A tooltip would be left hanging where the strip was.
        SendMessageW(tooltip_, TTM_POP, 0, 0);
    }
    SetWindowPos(hwnd_, after, x, y, w, height, flags);
}

void Strip::hide() {
    if (tooltip_) SendMessageW(tooltip_, TTM_POP, 0, 0);
    if (isShown()) ShowWindow(hwnd_, SW_HIDE);
}

void Strip::setInteractive(bool interactive) {
    interactive_ = interactive;
    LONG_PTR extended = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    extended = interactive ? (extended & ~WS_EX_NOACTIVATE) : (extended | WS_EX_NOACTIVATE);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, extended);
}

// ---------------------------------------------------------------- geometry, in DIPs

float Strip::scale() const { return GetDpiForWindow(hwnd_) / 96.f; }

float Strip::width() const {
    RECT client;
    GetClientRect(hwnd_, &client);
    return client.right / scale();
}

// Icons take at most half of the row while there are tabs.
float Strip::iconWidth() const {
    if (icons_.empty()) return kIconMax;
    float room = (width() - kButton) * (tabs_.empty() ? 1.f : 0.5f);
    return std::clamp(room / icons_.size(), kIconMin, kIconMax);
}

// Where the icons start; the tabs share the room left of them.
float Strip::tabsWidth() const { return std::max(0.f, width() - kButton - icons_.size() * iconWidth()); }
float Strip::slot() const { return tabs_.empty() ? 0 : tabsWidth() / tabs_.size(); }

// Everything is laid out on screen pixels, not DIPs: when the window is resized the tabs and names
// move a pixel at a time instead of jumping, and edges do not shimmer.
float Strip::snap(float value) const { return std::round(value * scale()) / scale(); }

// The last edge is where the icons start: the tabs, the icons and the eye button meet on the same pixels.
float Strip::edge(int position) const {
    return position >= static_cast<int>(tabs_.size()) ? snap(tabsWidth()) : snap(position * slot());
}

D2D1_RECT_F Strip::slotRect(int position) const {
    return D2D1::RectF(edge(position), 0, edge(position + 1), kHeight);
}

D2D1_RECT_F Strip::iconRect(int index) const {
    float left = tabsWidth(), icon = iconWidth();
    return D2D1::RectF(snap(left + index * icon), 0, snap(left + (index + 1) * icon), kHeight);
}

D2D1_RECT_F Strip::buttonRect() const {
    return D2D1::RectF(iconRect(static_cast<int>(icons_.size())).left, 0, width(), kHeight);
}

int Strip::indexAt(float x, float y) const {
    if (tabs_.empty() || x < 0 || y < 0 || x >= edge(static_cast<int>(tabs_.size())) || y > kHeight) return -1;
    return std::clamp(static_cast<int>(x / slot()), 0, static_cast<int>(tabs_.size()) - 1);
}

int Strip::iconAt(float x) const {
    for (int index = 0; index < static_cast<int>(icons_.size()); index++) {
        D2D1_RECT_F rect = iconRect(index);
        if (x >= rect.left && x < rect.right) return index;
    }
    return -1;
}

// A tab's buttons from right to left. Close only for a profile with windows: there is nothing else to close.
std::vector<Strip::TabButton> Strip::tabButtons(int index, D2D1_RECT_F rect) const {
    std::vector<TabButton> buttons;
    if (rect.right - rect.left < kTabButtonsMinWidth) return buttons;
    float top = snap((rect.top + rect.bottom) / 2 - kTabButton / 2), right = rect.right - kTabButtonInset;
    for (HoverKind kind : {HoverKind::Close, HoverKind::Collapse}) {
        if (kind == HoverKind::Close && !tabs_[index].isOpen) continue;
        float left = snap(right - kTabButton);
        buttons.push_back({kind, D2D1::RectF(left, top, left + kTabButton, top + kTabButton)});
        right = left - kTabButtonGap;
    }
    return buttons;
}

int Strip::hoveredTab() const {
    bool onTab = hover_.kind == HoverKind::Tab || hover_.kind == HoverKind::Close || hover_.kind == HoverKind::Collapse;
    return onTab && hover_.index < static_cast<int>(tabs_.size()) ? hover_.index : -1;
}

// The tab showing its buttons: the one under the mouse, unless a tab is being dragged or renamed.
int Strip::buttonsTab() const {
    int index = hoveredTab();
    if (dragging_ || index < 0 || (edit_ && tabs_[index].key == editingKey_)) return -1;
    return index;
}

Strip::Hit Strip::hitAt(D2D1_POINT_2F point) const {
    if (point.x < 0 || point.y < 0 || point.x >= width() || point.y > kHeight) return {};
    if (point.x >= buttonRect().left) return {HoverKind::Privacy, -1};
    int icon = iconAt(point.x);
    if (icon >= 0) return {HoverKind::Icon, icon};
    int index = indexAt(point.x, point.y);
    if (index < 0) return {};
    // The mouse over a tab makes it show its buttons, so they count as soon as the mouse is on the tab.
    if (!dragging_ && !(edit_ && tabs_[index].key == editingKey_)) {
        for (const auto& button : tabButtons(index, slotRect(index))) {
            const D2D1_RECT_F& rect = button.rect;
            if (point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom) return {button.kind, index};
        }
    }
    return {HoverKind::Tab, index};
}

// The order of the tabs if the dragged one were dropped now.
std::vector<int> Strip::dropOrder() const {
    std::vector<int> order;
    for (int i = 0; i < static_cast<int>(tabs_.size()); i++) order.push_back(i);
    if (!dragging_) return order;
    int target = std::clamp(static_cast<int>((dragLeft_ + slot() / 2) / slot()), 0, static_cast<int>(tabs_.size()) - 1);
    order.erase(order.begin() + dragIndex_);
    order.insert(order.begin() + target, dragIndex_);
    return order;
}

D2D1_POINT_2F Strip::dips(LPARAM lParam) const {
    return D2D1::Point2F(GET_X_LPARAM(lParam) / scale(), GET_Y_LPARAM(lParam) / scale());
}

// ---------------------------------------------------------------- drawing

void Strip::discardTarget() {
    for (auto& item : pictures_) release(item.second);
    pictures_.clear();
    release(target_);
}

void Strip::paint() {
    PAINTSTRUCT paintStruct;
    BeginPaint(hwnd_, &paintStruct);
    RECT client;
    GetClientRect(hwnd_, &client);
    if (!target_) {
        float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
        D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties();
        properties.dpiX = properties.dpiY = dpi;
        d2d_->CreateHwndRenderTarget(properties, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(client.right, client.bottom)), &target_);
    }
    if (target_) {
        canvas_ = target_;
        target_->BeginDraw();
        drawContent();
        canvas_ = nullptr;
        if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
            discardTarget();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    EndPaint(hwnd_, &paintStruct);
}

void Strip::drawContent() {
    const Palette& palette = dark_ ? kDark : kLight;
    canvas_->Clear(color(palette.background));
    std::vector<int> order = dropOrder();
    for (int position = 0; position < static_cast<int>(order.size()); position++) {
        if (dragging_ && order[position] == dragIndex_) continue;
        drawTab(order[position], slotRect(position), false);
    }
    ID2D1SolidColorBrush* line = nullptr;
    canvas_->CreateSolidColorBrush(color(palette.separator), &line);
    for (int position = 1; position < static_cast<int>(order.size()); position++) {
        int left = order[position - 1], right = order[position];
        // No line next to the selected or the dragged tab: their fill already sets them apart.
        bool quiet = tabs_[left].key == active_ || tabs_[right].key == active_ || (dragging_ && (left == dragIndex_ || right == dragIndex_));
        if (!quiet) canvas_->FillRectangle(D2D1::RectF(edge(position), 7, edge(position) + 1 / scale(), kHeight - 7), line);
    }
    release(line);
    bool lastTabActive = !order.empty() && tabs_[order.back()].key == active_;
    drawIcons(lastTabActive);
    drawPrivacyButton(icons_.empty() ? lastTabActive : icons_.back().key == active_);
    if (dragging_) {
        float left = snap(dragLeft_);
        drawTab(dragIndex_, D2D1::RectF(left, 0, left + snap(slot()), kHeight), true);
    }
}

// Draws the strip as it is now into a PNG. The test copy uses it to check the drawing on a machine
// whose screen nobody is looking at.
bool Strip::snapshot(const std::wstring& path) {
    RECT client;
    GetClientRect(hwnd_, &client);
    if (!wic_ || client.right <= 0 || client.bottom <= 0) return false;
    IWICBitmap* bitmap = nullptr;
    ID2D1RenderTarget* offscreen = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    bool ok = SUCCEEDED(wic_->CreateBitmap(client.right, client.bottom, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) &&
              SUCCEEDED(d2d_->CreateWicBitmapRenderTarget(bitmap, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi), &offscreen));
    if (ok) {
        canvas_ = offscreen;
        offscreen->BeginDraw();
        drawContent();
        ok = SUCCEEDED(offscreen->EndDraw());
        canvas_ = nullptr;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    ok = ok && SUCCEEDED(wic_->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
         SUCCEEDED(frame->SetSize(client.right, client.bottom)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
         SUCCEEDED(frame->WriteSource(bitmap, nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    release(frame);
    release(encoder);
    release(stream);
    for (auto* item : snapshotBitmaps_) release(item);
    snapshotBitmaps_.clear();
    release(offscreen);
    release(bitmap);
    return ok;
}

void Strip::drawTab(int index, D2D1_RECT_F rect, bool lifted) {
    const Tab& tab = tabs_[index];
    const Palette& palette = dark_ ? kDark : kLight;
    bool isActive = tab.key == active_;
    bool editing = edit_ && tab.key == editingKey_;
    ID2D1SolidColorBrush* brush = nullptr;
    canvas_->CreateSolidColorBrush(color(palette.active), &brush);
    if (isActive || lifted || editing) {
        canvas_->FillRectangle(rect, brush);
    } else if (hoveredTab() == index && !dragging_) {
        brush->SetColor(color(palette.hover));
        canvas_->FillRectangle(rect, brush);
    }
    if (isActive) {
        brush->SetColor(accent(tab.color, dark_));
        canvas_->FillRectangle(D2D1::RectF(rect.left, rect.bottom - 2, rect.right, rect.bottom), brush);
    }
    if (editing) {
        release(brush);
        return;
    }

    std::vector<TabButton> buttons = !lifted && index == buttonsTab() ? tabButtons(index, rect) : std::vector<TabButton>();
    // The name keeps clear of the buttons on both sides, so it stays centered when they show up.
    float margin = buttons.empty() ? 12 : rect.right - buttons.back().rect.left + 4;
    float alpha = tab.isOpen || isActive ? 1.f : 0.45f;
    IDWriteTextFormat* format = isActive ? semibold_ : regular_;
    const float avatar = 16, spacing = 7;
    float available = std::max(0.f, (rect.right - rect.left) - 2 * margin - avatar - spacing);
    IDWriteTextLayout* measure = nullptr;
    dwrite_->CreateTextLayout(tab.title.c_str(), static_cast<UINT32>(tab.title.size()), format, 10000, kHeight, &measure);
    DWRITE_TEXT_METRICS metrics{};
    if (measure) measure->GetMetrics(&metrics);
    release(measure);
    float textWidth = std::min(std::ceil(metrics.widthIncludingTrailingWhitespace), available);
    float x = snap((rect.left + rect.right) / 2 - (avatar + spacing + textWidth) / 2);
    float middle = (rect.top + rect.bottom) / 2;

    drawAvatar(tab, D2D1::RectF(x, snap(middle - avatar / 2), x + avatar, snap(middle - avatar / 2) + avatar), alpha);
    x += avatar + spacing;
    IDWriteTextLayout* layout = nullptr;
    dwrite_->CreateTextLayout(tab.title.c_str(), static_cast<UINT32>(tab.title.size()), format, std::max(1.f, textWidth), kHeight, &layout);
    if (layout) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trimming, ellipsis_);
        brush->SetColor(color(isActive ? palette.activeText : palette.text, alpha));
        canvas_->DrawTextLayout(D2D1::Point2F(x, 0), layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        release(layout);
    }
    uint32_t ink = isActive ? palette.activeText : palette.text;
    for (const auto& button : buttons) {
        Hit self{button.kind, index};
        bool pressed = pressed_ == self;
        if (pressed || hover_ == self) {
            brush->SetColor(color(ink, pressed ? 0.24f : 0.14f));
            canvas_->FillRoundedRectangle(D2D1::RoundedRect(button.rect, 4, 4), brush);
        }
        // ChromeClose and ChromeMinimize of Segoe Fluent Icons / MDL2 Assets.
        const wchar_t* glyph = button.kind == HoverKind::Close ? L"\xE8BB" : L"\xE921";
        brush->SetColor(color(ink));
        canvas_->DrawText(glyph, 1, smallGlyph_, button.rect, brush);
    }
    release(brush);
}

ID2D1Bitmap* Strip::picture(const std::wstring& path) {
    bool cached = canvas_ == target_;
    // Keyed by the file's time too: a new account photo under the same name shows up.
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes);
    std::wstring key = path + L"|" + std::to_wstring(attributes.ftLastWriteTime.dwLowDateTime);
    auto known = pictures_.find(key);
    if (cached && known != pictures_.end()) return known->second;
    ID2D1Bitmap* bitmap = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    if (wic_ && SUCCEEDED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic_->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        canvas_->CreateBitmapFromWicBitmap(converter, nullptr, &bitmap);
    }
    release(converter);
    release(frame);
    release(decoder);
    if (!bitmap) return nullptr;
    if (cached) pictures_[key] = bitmap;
    else snapshotBitmaps_.push_back(bitmap);
    return bitmap;
}

void Strip::drawAvatar(const Tab& tab, D2D1_RECT_F rect, float alpha) {
    D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2), (rect.right - rect.left) / 2,
                                        (rect.bottom - rect.top) / 2);
    ID2D1Bitmap* bitmap = tab.picture.empty() ? nullptr : picture(tab.picture);
    if (bitmap) {
        ID2D1BitmapBrush* brush = nullptr;
        D2D1_SIZE_F size = bitmap->GetSize();
        canvas_->CreateBitmapBrush(bitmap, &brush);
        if (brush) {
            brush->SetTransform(D2D1::Matrix3x2F::Scale((rect.right - rect.left) / size.width, (rect.bottom - rect.top) / size.height) *
                                D2D1::Matrix3x2F::Translation(rect.left, rect.top));
            brush->SetOpacity(alpha);
            canvas_->FillEllipse(circle, brush);
            release(brush);
            return;
        }
    }
    ID2D1SolidColorBrush* brush = nullptr;
    canvas_->CreateSolidColorBrush(color(tab.color, alpha), &brush);
    canvas_->FillEllipse(circle, brush);
    std::wstring letter = tab.title.empty() ? L"" : std::wstring(1, static_cast<wchar_t>(towupper(tab.title[0])));
    brush->SetColor(isLight(tab.color) ? D2D1::ColorF(0, 0, 0, 0.7f * alpha) : D2D1::ColorF(1, 1, 1, 0.92f * alpha));
    canvas_->DrawText(letter.c_str(), static_cast<UINT32>(letter.size()), initial_, rect, brush);
    release(brush);
}

// Collapsed profiles: the avatar alone, paler while the profile has no window, like a tab.
void Strip::drawIcons(bool afterActive) {
    if (icons_.empty()) return;
    const Palette& palette = dark_ ? kDark : kLight;
    ID2D1SolidColorBrush* brush = nullptr;
    canvas_->CreateSolidColorBrush(color(palette.active), &brush);
    for (int index = 0; index < static_cast<int>(icons_.size()); index++) {
        const Tab& icon = icons_[index];
        D2D1_RECT_F rect = iconRect(index);
        bool isActive = icon.key == active_;
        Hit self{HoverKind::Icon, index};
        bool lit = hover_ == self || pressed_ == self;
        if (isActive) {
            brush->SetColor(color(palette.active));
            canvas_->FillRectangle(rect, brush);
            brush->SetColor(accent(icon.color, dark_));
            canvas_->FillRectangle(D2D1::RectF(rect.left, rect.bottom - 2, rect.right, rect.bottom), brush);
        } else if (lit) {
            brush->SetColor(color(palette.hover));
            canvas_->FillRectangle(rect, brush);
        }
        const float avatar = 16;
        float x = snap((rect.left + rect.right) / 2 - avatar / 2), y = snap((rect.top + rect.bottom) / 2 - avatar / 2);
        drawAvatar(icon, D2D1::RectF(x, y, x + avatar, y + avatar), icon.isOpen || isActive || lit ? 1.f : 0.45f);
    }
    if (!afterActive && !tabs_.empty()) {
        float left = iconRect(0).left;
        brush->SetColor(color(palette.separator));
        canvas_->FillRectangle(D2D1::RectF(left, 7, left + 1 / scale(), kHeight - 7), brush);
    }
    release(brush);
}

void Strip::drawPrivacyButton(bool nextToActive) {
    const Palette& palette = dark_ ? kDark : kLight;
    D2D1_RECT_F rect = buttonRect();
    ID2D1SolidColorBrush* brush = nullptr;
    canvas_->CreateSolidColorBrush(color(palette.hover), &brush);
    if (hover_.kind == HoverKind::Privacy || pressed_.kind == HoverKind::Privacy) canvas_->FillRectangle(rect, brush);
    if (!nextToActive && !(tabs_.empty() && icons_.empty())) {
        brush->SetColor(color(palette.separator));
        canvas_->FillRectangle(D2D1::RectF(rect.left, 7, rect.left + 1 / scale(), kHeight - 7), brush);
    }
    // The eye, crossed out while the names are hidden. Glyphs of Segoe Fluent Icons / MDL2 Assets.
    const wchar_t* glyph = namesHidden_ ? L"" : L"";
    brush->SetColor(namesHidden_ ? color(palette.activeText) : color(palette.text, 0.65f));
    canvas_->DrawText(glyph, 1, glyph_, rect, brush);
    release(brush);
}

// ---------------------------------------------------------------- renaming

void Strip::beginEditing(const std::wstring& key, const std::wstring& text) {
    endEditing(false);
    if (std::none_of(tabs_.begin(), tabs_.end(), [&](const Tab& tab) { return tab.key == key; })) return;
    setInteractive(true);
    editingKey_ = key;
    edit_ = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | ES_CENTER | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    if (editFont_) DeleteObject(editFont_);
    editFont_ = CreateFontW(-static_cast<int>(std::lround(12 * scale())), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
    SetWindowSubclass(edit_, editProc, 1, reinterpret_cast<DWORD_PTR>(this));
    positionEditor();
    SetForegroundWindow(hwnd_);
    SetFocus(edit_);
    SendMessageW(edit_, EM_SETSEL, 0, -1);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Strip::positionEditor() {
    if (!edit_) return;
    auto found = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab& tab) { return tab.key == editingKey_; });
    if (found == tabs_.end()) return;
    D2D1_RECT_F rect = slotRect(static_cast<int>(found - tabs_.begin()));
    float s = scale(), height = 18;
    int left = static_cast<int>(std::lround((rect.left + 12) * s)), top = static_cast<int>(std::lround((kHeight - height) / 2 * s));
    int w = std::max(0, static_cast<int>(std::lround((rect.right - rect.left - 24) * s)));
    MoveWindow(edit_, left, top, w, static_cast<int>(std::lround(height * s)), TRUE);
}

// Enter saves, Esc cancels, clicking elsewhere saves.
LRESULT CALLBACK Strip::editProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    Strip* strip = reinterpret_cast<Strip*>(data);
    switch (message) {
        case WM_KEYDOWN:
            if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
                PostMessageW(strip->hwnd_, kEndEdit, wParam == VK_RETURN, 0);
                return 0;
            }
            break;
        case WM_CHAR:
            if (wParam == L'\r' || wParam == 27) return 0;  // no beep
            break;
        case WM_KILLFOCUS:
            PostMessageW(strip->hwnd_, kEndEdit, 1, 0);
            break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void Strip::endEditing(bool commit) {
    if (!edit_) return;
    // Forget the field first: taking it down ends the editing and would call back here.
    HWND edit = edit_;
    std::wstring key = editingKey_;
    edit_ = nullptr;
    editingKey_.clear();
    int length = GetWindowTextLengthW(edit);
    std::wstring text(length, L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    RemoveWindowSubclass(edit, editProc, 1);
    DestroyWindow(edit);
    setInteractive(false);
    if (commit && onRename) onRename(key, text);
    if (onEditingEnded) onEditingEnded();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------- pressing and dragging

// The left button as the user sees it: GetAsyncKeyState reads physical buttons, and swapped buttons
// put the user's "left" on the physical right one.
static bool leftButtonDown() {
    return GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000;
}

D2D1_POINT_2F Strip::cursorInDips() const {
    POINT cursor;
    GetCursorPos(&cursor);
    ScreenToClient(hwnd_, &cursor);
    return D2D1::Point2F(cursor.x / scale(), cursor.y / scale());
}

void Strip::dragTo(float x) {
    dragLeft_ = std::clamp(x - dragGrab_, 0.f, tabsWidth() - slot());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// The end of a press, wherever the button was let go: a click, a double click, a drop, a tab button, an
// icon or the eye button.
void Strip::finishPress(D2D1_POINT_2F point) {
    KillTimer(hwnd_, kPressTimer);
    Hit control = pressed_;
    bool wasDragging = dragging_, twice = doubleClick_;
    int pressed = pressIndex_;
    std::vector<int> order = dropOrder();
    logLine(L"mouse up x=%.1f pressed=%d control=%d dragging=%d", point.x, pressed, static_cast<int>(control.kind), wasDragging);
    pressed_ = Hit{};
    dragging_ = doubleClick_ = false;
    pressIndex_ = -1;
    // Released only now: losing the capture cancels a press, and this one is already taken.
    if (GetCapture() == hwnd_) ReleaseCapture();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (control.kind != HoverKind::None) {
        if (hitAt(point) == control) act(control);
        return;
    }
    if (wasDragging) {
        bool moved = false;
        for (size_t i = 0; i < order.size(); i++) moved |= order[i] != static_cast<int>(i);
        if (moved) {
            std::vector<Tab> reordered;
            std::vector<std::wstring> keys;
            for (int index : order) {
                reordered.push_back(tabs_[index]);
                keys.push_back(tabs_[index].key);
            }
            tabs_ = reordered;
            if (onReorder) onReorder(keys);
        }
        setHover(hitAt(point));
        return;
    }
    if (pressed < 0 || pressed >= static_cast<int>(tabs_.size()) || indexAt(point.x, point.y) != pressed) return;
    std::wstring key = tabs_[pressed].key;
    // The first click of a double click already switched the profile; the second renames.
    if (twice) {
        if (onRequestRename) onRequestRename(key);
    } else if (onSelect) {
        onSelect(key);
    }
}

// The key is copied: the callback may bring new tabs, and the one pressed goes with the old ones.
void Strip::act(Hit control) {
    std::wstring key;
    if (control.kind == HoverKind::Close || control.kind == HoverKind::Collapse) key = tabs_[control.index].key;
    if (control.kind == HoverKind::Icon) key = icons_[control.index].key;
    switch (control.kind) {
        case HoverKind::Privacy:
            if (onTogglePrivacy) onTogglePrivacy();
            break;
        case HoverKind::Close:
            if (onClose) onClose(key);
            break;
        case HoverKind::Collapse:
            if (onCollapse) onCollapse(key);
            break;
        case HoverKind::Icon:
            if (onExpand) onExpand(key);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------- tooltip

// The icons show the profile's name, the buttons what they do; tabs show their names already. One tool at
// a time, for the element under the mouse, with a new id each time: the tooltip control takes the move to
// the next element as a new tool and shows its text.
void Strip::updateTooltip() {
    if (!tooltip_) return;
    std::wstring text;
    D2D1_RECT_F rect{};
    if (!tooltipMuted_ && pressIndex_ < 0 && pressed_.kind == HoverKind::None) {
        if (hover_.kind == HoverKind::Icon) {
            text = icons_[hover_.index].title;
            rect = iconRect(hover_.index);
        } else if (hover_.kind == HoverKind::Close || hover_.kind == HoverKind::Collapse) {
            for (const auto& button : tabButtons(hover_.index, slotRect(hover_.index))) {
                if (button.kind == hover_.kind) rect = button.rect;
            }
            text = hover_.kind == HoverKind::Close     ? L"Close profile"
                   : tabs_[hover_.index].isOpen ? L"Close and collapse to icon"
                                                : L"Collapse to icon";
        }
    }
    float s = scale();
    RECT pixels{std::lround(rect.left * s), std::lround(rect.top * s), std::lround(rect.right * s), std::lround(rect.bottom * s)};
    // The same element with the same text keeps its tool, or a tooltip on screen would go away.
    if (toolId_ && text == toolText_ && EqualRect(&pixels, &toolRect_)) return;
    if (toolId_) {
        TTTOOLINFOW old{sizeof(old)};
        old.hwnd = hwnd_;
        old.uId = toolId_;
        SendMessageW(tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&old));
        toolId_ = 0;
    }
    if (text.empty()) return;
    toolText_ = text;
    toolRect_ = pixels;
    TTTOOLINFOW tool{sizeof(tool)};
    tool.hwnd = hwnd_;
    tool.uId = toolId_ = ++lastToolId_;
    tool.rect = pixels;
    tool.lpszText = toolText_.data();
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
}

// The tooltip control learns where the mouse is only from the messages passed on to it.
void Strip::relayToTooltip(UINT message, WPARAM wParam, LPARAM lParam) {
    if (!tooltip_) return;
    MSG msg{hwnd_, message, wParam, lParam, static_cast<DWORD>(GetMessageTime()), {}};
    GetCursorPos(&msg.pt);
    SendMessageW(tooltip_, TTM_RELAYEVENT, 0, reinterpret_cast<LPARAM>(&msg));
}

// ---------------------------------------------------------------- window messages

void Strip::setHover(Hit hit) {
    if (hit == hover_) return;
    hover_ = hit;
    updateTooltip();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK Strip::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        reinterpret_cast<Strip*>(create->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* strip = reinterpret_cast<Strip*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return strip ? strip->handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Strip::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT:
            paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (target_) target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            positionEditor();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED:
            // The controller sizes the strip itself; only the drawing follows the new density.
            discardTarget();
            positionEditor();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_SETTINGCHANGE:
            if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) refreshTheme();
            if (onEnvironmentChanged) onEnvironmentChanged();
            return 0;
        case WM_DISPLAYCHANGE:
            if (onEnvironmentChanged) onEnvironmentChanged();
            return 0;
        case WM_TIMER:
            // The strip never comes to the front, so Windows sends it the mouse only while the cursor is
            // over it. While the button is held, the cursor and the button are read directly: a drag goes
            // on outside the strip, and a release anywhere ends it.
            if (wParam == kPressTimer) {
                D2D1_POINT_2F point = cursorInDips();
                if (!leftButtonDown()) finishPress(point);
                else if (dragging_) dragTo(point.x);
            }
            return 0;
        case WM_MOUSEACTIVATE:
            return interactive_ ? MA_ACTIVATE : MA_NOACTIVATE;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && reinterpret_cast<HWND>(wParam) == hwnd_) {
                SetCursor(LoadCursorW(nullptr, dragging_ ? IDC_SIZEWE : IDC_HAND));
                return TRUE;
            }
            break;
        case WM_CTLCOLOREDIT: {
            const Palette& palette = dark_ ? kDark : kLight;
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, colorRef(palette.activeText));
            SetBkColor(dc, colorRef(palette.active));
            return reinterpret_cast<LRESULT>(editBrush_);
        }
        case kEndEdit:
            endEditing(wParam != 0);
            return 0;
        case WM_MOUSEMOVE: {
            if (!trackingLeave_) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
                trackingLeave_ = TrackMouseEvent(&track) != FALSE;
            }
            D2D1_POINT_2F point = dips(lParam);
            lastPoint_ = point;
            if ((pressIndex_ >= 0 || pressed_.kind != HoverKind::None) && !(wParam & MK_LBUTTON)) {
                // The release happened where the strip could not see it.
                finishPress(point);
                return 0;
            }
            if (pressIndex_ >= 0 && tabs_.size() > 1 && !edit_) {
                if (!dragging_ && std::fabs(point.x - pressX_) > 4) {
                    dragging_ = true;
                    dragIndex_ = pressIndex_;
                    dragGrab_ = pressX_ - pressIndex_ * slot();
                }
                if (dragging_) dragTo(point.x);
            }
            Hit hit = hitAt(point);
            if (hit != hover_) tooltipMuted_ = false;
            setHover(hit);
            relayToTooltip(message, wParam, lParam);
            return 0;
        }
        case WM_MOUSELEAVE:
            trackingLeave_ = false;
            setHover(Hit{});
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            D2D1_POINT_2F point = dips(lParam);
            Hit hit = hitAt(point);
            tooltipMuted_ = true;
            updateTooltip();
            logLine(L"mouse down x=%.1f dips, target %d/%d, double=%d", point.x, static_cast<int>(hit.kind), hit.index,
                    message == WM_LBUTTONDBLCLK);
            // The second click of a double click counts only on what got the first: the first may have
            // changed the row, and a tab may now be where an icon or a close button was.
            if (message == WM_LBUTTONDBLCLK && hit != firstClick_) return 0;
            if (message == WM_LBUTTONDOWN) firstClick_ = hit;
            SetCapture(hwnd_);
            doubleClick_ = message == WM_LBUTTONDBLCLK;
            lastPoint_ = point;
            if (hit.kind == HoverKind::Tab) {
                pressIndex_ = hit.index;
                pressX_ = point.x;
                SetTimer(hwnd_, kPressTimer, 15, nullptr);
            } else if (hit.kind != HoverKind::None) {
                pressed_ = hit;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP:
            finishPress(dips(lParam));
            return 0;
        case WM_CAPTURECHANGED:
            // Losing the capture with the button still held (another window took it) cancels the press.
            if (reinterpret_cast<HWND>(lParam) != hwnd_ && (pressIndex_ >= 0 || pressed_.kind != HoverKind::None) && leftButtonDown()) {
                KillTimer(hwnd_, kPressTimer);
                dragging_ = doubleClick_ = false;
                pressed_ = Hit{};
                pressIndex_ = -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_RBUTTONDOWN:
            tooltipMuted_ = true;
            updateTooltip();
            return 0;
        case WM_RBUTTONUP: {
            D2D1_POINT_2F point = dips(lParam);
            int index = indexAt(point.x, point.y), icon = iconAt(point.x);
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd_, &screen);
            std::wstring key = icon >= 0 ? icons_[icon].key : index >= 0 ? tabs_[index].key : std::wstring();
            if (onContextMenu) onContextMenu(key, screen);
            return 0;
        }
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
