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
    // A collapsed profile's icon, narrowing down to the smaller width when many profiles are collapsed.
    static constexpr float kIconMax = 30;
    static constexpr float kIconMin = 20;
    // The buttons of a tab: their size, the gap between them and their distance from the tab's right edge.
    static constexpr float kTabButton = 18;
    static constexpr float kTabButtonGap = 2;
    static constexpr float kTabButtonInset = 6;
    // A narrower tab gets no buttons, they would leave no room for the name. Its menu has the same commands.
    static constexpr float kTabButtonsMinWidth = 90;

    std::function<void(const std::wstring&)> onSelect;
    std::function<void(const std::wstring&)> onClose;     // the tab's close button
    std::function<void(const std::wstring&)> onCollapse;  // the tab's collapse button
    std::function<void(const std::wstring&)> onExpand;    // a click on a collapsed profile's icon
    std::function<void(const std::wstring&)> onRequestRename;
    std::function<void(const std::vector<std::wstring>&)> onReorder;
    std::function<void(const std::wstring&, const std::wstring&)> onRename;
    std::function<void()> onEditingEnded;
    std::function<void()> onTogglePrivacy;
    // Right click: the profile folder of the tab or icon under the mouse (empty elsewhere) and the screen point.
    std::function<void(const std::wstring&, POINT)> onContextMenu;
    // Display or system settings changed: broadcasts reach top-level windows like the strip only.
    std::function<void()> onEnvironmentChanged;

    bool create(HINSTANCE instance);
    HWND hwnd() const { return hwnd_; }
    // The tabs in the row, and the collapsed profiles shown as icons at its right end.
    void show(const std::vector<Tab>& tabs, const std::vector<Tab>& icons, const std::wstring& active, bool namesHidden);
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
    // For checks: the mouse over the strip at this distance in DIPs from its left edge, or off it if negative.
    void hoverAt(float x);

private:
    // What is under the mouse. The index is a tab position for tabs and their buttons, an icon position
    // for icons.
    enum class HoverKind { None, Tab, Close, Collapse, Icon, Privacy };
    struct Hit {
        HoverKind kind = HoverKind::None;
        int index = -1;
        bool operator==(const Hit& other) const { return kind == other.kind && index == other.index; }
        bool operator!=(const Hit& other) const { return !(*this == other); }
    };
    struct TabButton {
        HoverKind kind;
        D2D1_RECT_F rect;
    };

    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HWND tooltip_ = nullptr;
    UINT_PTR toolId_ = 0;  // the tooltip's tool for the element under the mouse, 0 if there is none
    UINT_PTR lastToolId_ = 0;
    std::wstring toolText_;
    RECT toolRect_{};
    // After a click the tooltip waits until the mouse goes to something else, like everywhere in Windows.
    bool tooltipMuted_ = false;
    std::wstring editingKey_;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    bool interactive_ = false;
    bool dark_ = true;

    std::vector<Tab> tabs_;
    std::vector<Tab> icons_;
    std::wstring active_;
    bool namesHidden_ = false;
    Hit hover_;
    bool trackingLeave_ = false;
    int pressIndex_ = -1;
    float pressX_ = 0;
    Hit pressed_;     // a button or an icon held down: it acts if the mouse is let go over it
    Hit firstClick_;  // what got the first click of a double click
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
    IDWriteTextFormat* smallGlyph_ = nullptr;
    IDWriteInlineObject* ellipsis_ = nullptr;
    std::map<std::wstring, ID2D1Bitmap*> pictures_;

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK editProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);

    float scale() const;
    float width() const;
    float iconWidth() const;
    float tabsWidth() const;
    float slot() const;
    float snap(float value) const;
    float edge(int position) const;
    D2D1_RECT_F slotRect(int position) const;
    D2D1_RECT_F iconRect(int index) const;
    D2D1_RECT_F buttonRect() const;
    int indexAt(float x, float y) const;
    int iconAt(float x) const;
    std::vector<TabButton> tabButtons(int index, D2D1_RECT_F rect) const;
    int hoveredTab() const;
    int buttonsTab() const;
    Hit hitAt(D2D1_POINT_2F point) const;
    std::vector<int> dropOrder() const;
    D2D1_POINT_2F dips(LPARAM lParam) const;

    void paint();
    void drawContent();
    void discardTarget();
    void drawTab(int index, D2D1_RECT_F rect, bool lifted);
    void drawAvatar(const Tab& tab, D2D1_RECT_F rect, float alpha);
    void drawIcons(bool afterActive);
    void drawPrivacyButton(bool nextToActive);
    ID2D1Bitmap* picture(const std::wstring& path);
    void setHover(Hit hit);
    void updateTooltip();
    void relayToTooltip(UINT message, WPARAM wParam, LPARAM lParam);
    void act(Hit control);
    void positionEditor();
    void endEditing(bool commit);
    void finishPress(D2D1_POINT_2F point);
    void dragTo(float x);
    D2D1_POINT_2F cursorInDips() const;
};
