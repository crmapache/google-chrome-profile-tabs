// The strip window: Direct2D drawing, mouse, renaming in place and the button that hides the names.
#include "strip.h"

#include <commctrl.h>
#include <dwmapi.h>
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
    for (IDWriteTextFormat* format : {regular_, semibold_, initial_, glyph_}) {
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    initial_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    glyph_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
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
    refreshTheme();
    return true;
}

void Strip::refreshTheme() {
    dark_ = systemIsDark();
    const Palette& palette = dark_ ? kDark : kLight;
    COLORREF border = colorRef(palette.border);
    DwmSetWindowAttribute(hwnd_, kBorderColor, &border, sizeof(border));
    if (editBrush_) DeleteObject(editBrush_);
    editBrush_ = CreateSolidBrush(colorRef(palette.active));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int Strip::heightPixels(UINT dpi) { return static_cast<int>(std::lround(kHeight * dpi / 96.f)); }
int Strip::gapPixels(UINT dpi) { return static_cast<int>(std::lround(kGap * dpi / 96.f)); }

// Rounded the same way as the strip itself, so it never pokes a pixel above the work area.
int Strip::reserve(UINT dpi) { return heightPixels(dpi) + gapPixels(dpi); }

// ---------------------------------------------------------------- state from the controller

void Strip::show(const std::vector<Tab>& tabs, const std::wstring& active, bool namesHidden) {
    // The state comes every second and rarely changes: no redraw without a change.
    bool same = active == active_ && namesHidden == namesHidden_ && tabs.size() == tabs_.size();
    for (size_t i = 0; same && i < tabs.size(); i++) same = tabs[i].signature == tabs_[i].signature;
    if (same) return;
    bool sameKeys = tabs.size() == tabs_.size();
    for (size_t i = 0; sameKeys && i < tabs.size(); i++) sameKeys = tabs[i].key == tabs_[i].key;
    if (dragging_ && !sameKeys) {
        dragging_ = false;
        pressIndex_ = -1;
    }
    tabs_ = tabs;
    active_ = active;
    namesHidden_ = namesHidden;
    if (hoverKind_ == HoverKind::Tab && hoverIndex_ >= static_cast<int>(tabs_.size())) hoverKind_ = HoverKind::None;
    // A field for a tab that is gone, or one that would show a real name while names are hidden, closes.
    if (edit_ && (namesHidden_ || std::none_of(tabs_.begin(), tabs_.end(), [&](const Tab& tab) { return tab.key == editingKey_; }))) {
        endEditing(false);
    }
    positionEditor();
    InvalidateRect(hwnd_, nullptr, FALSE);
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
    }
    SetWindowPos(hwnd_, after, x, y, w, height, flags);
}

void Strip::hide() {
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

float Strip::tabsWidth() const { return std::max(0.f, width() - kButton); }
float Strip::slot() const { return tabs_.empty() ? 0 : tabsWidth() / tabs_.size(); }

// Everything is laid out on screen pixels, not DIPs: when the window is resized the tabs and names
// move a pixel at a time instead of jumping, and edges do not shimmer.
float Strip::snap(float value) const { return std::round(value * scale()) / scale(); }

float Strip::edge(int position) const { return snap(position * slot()); }

D2D1_RECT_F Strip::slotRect(int position) const {
    return D2D1::RectF(edge(position), 0, edge(position + 1), kHeight);
}

D2D1_RECT_F Strip::buttonRect() const {
    return D2D1::RectF(edge(static_cast<int>(tabs_.size())), 0, width(), kHeight);
}

int Strip::indexAt(float x, float y) const {
    if (tabs_.empty() || x < 0 || y < 0 || x >= tabsWidth() || y > kHeight) return -1;
    return std::clamp(static_cast<int>(x / slot()), 0, static_cast<int>(tabs_.size()) - 1);
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
    drawPrivacyButton(!order.empty() && tabs_[order.back()].key == active_);
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
    } else if (hoverKind_ == HoverKind::Tab && hoverIndex_ == index && !dragging_) {
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

    float alpha = tab.isOpen || isActive ? 1.f : 0.45f;
    IDWriteTextFormat* format = isActive ? semibold_ : regular_;
    const float avatar = 16, spacing = 7;
    float available = std::max(0.f, (rect.right - rect.left) - 24 - avatar - spacing);
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

void Strip::drawPrivacyButton(bool nextToActive) {
    const Palette& palette = dark_ ? kDark : kLight;
    D2D1_RECT_F rect = buttonRect();
    ID2D1SolidColorBrush* brush = nullptr;
    canvas_->CreateSolidColorBrush(color(palette.hover), &brush);
    if (hoverKind_ == HoverKind::Privacy || pressedPrivacy_) canvas_->FillRectangle(rect, brush);
    if (!nextToActive && !tabs_.empty()) {
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

// The end of a press, wherever the button was let go: a click, a double click, a drop or the eye button.
void Strip::finishPress(D2D1_POINT_2F point) {
    KillTimer(hwnd_, kPressTimer);
    bool privacy = pressedPrivacy_, wasDragging = dragging_, twice = doubleClick_;
    int pressed = pressIndex_;
    std::vector<int> order = dropOrder();
    logLine(L"mouse up x=%.1f pressed=%d privacy=%d dragging=%d", point.x, pressed, privacy, wasDragging);
    pressedPrivacy_ = dragging_ = doubleClick_ = false;
    pressIndex_ = -1;
    // Released only now: losing the capture cancels a press, and this one is already taken.
    if (GetCapture() == hwnd_) ReleaseCapture();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (privacy) {
        if (point.x >= buttonRect().left && point.y >= 0 && point.y <= kHeight && onTogglePrivacy) onTogglePrivacy();
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

// ---------------------------------------------------------------- window messages

void Strip::setHover(HoverKind kind, int index) {
    if (kind == hoverKind_ && index == hoverIndex_) return;
    hoverKind_ = kind;
    hoverIndex_ = index;
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
            if ((pressIndex_ >= 0 || pressedPrivacy_) && !(wParam & MK_LBUTTON)) {
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
            D2D1_RECT_F button = buttonRect();
            if (point.x >= button.left && point.x <= button.right) setHover(HoverKind::Privacy, -1);
            else {
                int index = indexAt(point.x, point.y);
                setHover(index >= 0 ? HoverKind::Tab : HoverKind::None, index);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            trackingLeave_ = false;
            setHover(HoverKind::None, -1);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            SetCapture(hwnd_);
            D2D1_POINT_2F point = dips(lParam);
            D2D1_RECT_F button = buttonRect();
            doubleClick_ = message == WM_LBUTTONDBLCLK;
            logLine(L"mouse down x=%.1f dips, button at %.1f, double=%d", point.x, button.left, doubleClick_);
            if (point.x >= button.left) {
                lastPoint_ = point;
                pressedPrivacy_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            pressIndex_ = indexAt(point.x, point.y);
            pressX_ = point.x;
            lastPoint_ = point;
            if (pressIndex_ >= 0) SetTimer(hwnd_, kPressTimer, 15, nullptr);
            return 0;
        }
        case WM_LBUTTONUP:
            finishPress(dips(lParam));
            return 0;
        case WM_CAPTURECHANGED:
            // Losing the capture with the button still held (another window took it) cancels the press.
            if (reinterpret_cast<HWND>(lParam) != hwnd_ && (pressIndex_ >= 0 || pressedPrivacy_) && leftButtonDown()) {
                KillTimer(hwnd_, kPressTimer);
                dragging_ = pressedPrivacy_ = doubleClick_ = false;
                pressIndex_ = -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_RBUTTONUP: {
            D2D1_POINT_2F point = dips(lParam);
            int index = indexAt(point.x, point.y);
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd_, &screen);
            std::wstring key = index >= 0 ? tabs_[index].key : std::wstring();
            if (onContextMenu) onContextMenu(key, screen);
            return 0;
        }
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
