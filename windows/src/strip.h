// The strip: a row of equal-width profile tabs above the Chrome window, like JetBrains project tabs.
#pragma once

#include "common.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

struct Tab {
    std::wstring key;  // profile folder: the tab is known by it whatever it says on it
    std::wstring title;
    uint32_t color = 0;
    std::wstring picture;
    bool isOpen = false;  // tabs of profiles without windows are paler
    std::wstring signature;
};

class Strip {
public:
    // Sizes in device independent pixels.
    static constexpr float kHeight = 30;
    static constexpr float kGap = 3;
    static constexpr float kButton = 34;

    std::function<void(const std::wstring&)> onSelect;
    std::function<void(const std::wstring&)> onRequestRename;
    std::function<void(const std::vector<std::wstring>&)> onReorder;
    std::function<void(const std::wstring&, const std::wstring&)> onRename;
    std::function<void()> onEditingEnded;
    std::function<void()> onTogglePrivacy;
    // Right click: the profile folder of the tab under the mouse (empty between tabs) and the screen point.
    std::function<void(const std::wstring&, POINT)> onContextMenu;
    // Display or system settings changed: broadcasts reach top-level windows like the strip only.
    std::function<void()> onEnvironmentChanged;

    bool create(HINSTANCE instance);
    HWND hwnd() const { return hwnd_; }
    void show(const std::vector<Tab>& tabs, const std::wstring& active, bool namesHidden);
    // Puts the strip above the browser window's visible frame and right above it in the z-order.
    // While the window is being dragged (live) the strip only moves.
    void place(const RECT& frame, HWND browser, UINT dpi, bool live);
    void hide();
    bool isShown() const { return IsWindowVisible(hwnd_) != FALSE; }
    // Room the strip needs above the browser window, in physical pixels.
    static int reserve(UINT dpi);
    static int heightPixels(UINT dpi);
    static int gapPixels(UINT dpi);

    void beginEditing(const std::wstring& key, const std::wstring& text);
    void cancelEditing() { endEditing(false); }
    bool isEditing() const { return edit_ != nullptr; }
    // Lets the strip take the keyboard for a menu or renaming; otherwise clicks never activate it.
    void setInteractive(bool interactive);
    void refreshTheme();
    bool snapshot(const std::wstring& path);

private:
    enum class HoverKind { None, Tab, Privacy };

    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    std::wstring editingKey_;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    bool interactive_ = false;
    bool dark_ = true;

    std::vector<Tab> tabs_;
    std::wstring active_;
    bool namesHidden_ = false;
    HoverKind hoverKind_ = HoverKind::None;
    int hoverIndex_ = -1;
    bool trackingLeave_ = false;
    int pressIndex_ = -1;
    float pressX_ = 0;
    bool pressedPrivacy_ = false;
    bool doubleClick_ = false;
    D2D1_POINT_2F lastPoint_{};
    bool dragging_ = false;
    int dragIndex_ = -1;
    float dragGrab_ = 0;
    float dragLeft_ = 0;

    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    IWICImagingFactory* wic_ = nullptr;
    ID2D1HwndRenderTarget* target_ = nullptr;
    ID2D1RenderTarget* canvas_ = nullptr;  // what is being drawn on: the window or a snapshot
    std::vector<ID2D1Bitmap*> snapshotBitmaps_;
    IDWriteTextFormat* regular_ = nullptr;
    IDWriteTextFormat* semibold_ = nullptr;
    IDWriteTextFormat* initial_ = nullptr;
    IDWriteTextFormat* glyph_ = nullptr;
    IDWriteInlineObject* ellipsis_ = nullptr;
    std::map<std::wstring, ID2D1Bitmap*> pictures_;

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK editProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);

    float scale() const;
    float width() const;
    float tabsWidth() const;
    float slot() const;
    float snap(float value) const;
    float edge(int position) const;
    D2D1_RECT_F slotRect(int position) const;
    D2D1_RECT_F buttonRect() const;
    int indexAt(float x, float y) const;
    std::vector<int> dropOrder() const;
    D2D1_POINT_2F dips(LPARAM lParam) const;

    void paint();
    void drawContent();
    void discardTarget();
    void drawTab(int index, D2D1_RECT_F rect, bool lifted);
    void drawAvatar(const Tab& tab, D2D1_RECT_F rect, float alpha);
    void drawPrivacyButton(bool nextToActive);
    ID2D1Bitmap* picture(const std::wstring& path);
    void setHover(HoverKind kind, int index);
    void positionEditor();
    void endEditing(bool commit);
    void finishPress(D2D1_POINT_2F point);
    void dragTo(float x);
    D2D1_POINT_2F cursorInDips() const;
};
