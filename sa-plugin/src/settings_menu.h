// San AnSkateas' own page in GTA's pause menu: Options > SAN ANSKATEAS, with
// GTA's look (its fonts, highlight and Back) and options that change the
// moment they're picked. 1.0 US only.
//
// GTA's menu is a table of pages (aScreens, 0x8CE008, 0xE2 bytes each) of up
// to 12 entries. Mod Loader's "Mod Configuration" moves that table to new
// memory and patches the code's references to it, so the table is found
// through one of those references (the code at 0x579568 holds its start).
// All 44 of GTA's pages are in use, so ours borrows one only while it's on
// screen: Controller Setup's "reset to defaults?" question (25), which the
// game handles purely by its contents. Picking our entry puts our page in
// it; leaving puts the question back. Our Options entry goes
// under LANGUAGE when nothing else is there yet, otherwise last before Back,
// so another mod's entry is never moved. The menu's text lookups and its
// options handler are hooked at their call sites, each hook calling whatever
// was there before (Mod Loader patches some of the same calls).
#pragma once

#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

#include "common.h"
#include "CFont.h"
#include "CMenuManager.h"
#include "CSprite2d.h"
#include "CAudioEngine.h"

namespace settings_menu {

struct Option {
    const char* key;          // menu text key, 7 characters at most ("SKT_DIF")
    const char* label;        // shown, upper case
    int count;                // choices, or bars
    bool bars;                // shown as a GTA slider of `count` bars, else by name
    const char* const* names; // the choices' names when not bars
    int value;                // 0 .. count - 1
};

using Changed = void (*)(int option, int value);

#pragma pack(push, 1)
struct Item {
    int8_t action;
    char name[8];
    int8_t type;
    int8_t target;
    uint8_t pad;
    uint16_t x, y;
    uint8_t align;
    uint8_t pad2;
};
struct Page {
    char title[8];
    int8_t parent;
    int8_t start;
    Item items[12];
};
#pragma pack(pop)
static_assert(sizeof(Item) == 0x12 && sizeof(Page) == 0xE2, "GTA's menu table layout");

// GTA's values (gta-reversed MenuManager_Internal.h).
constexpr int8_t kActionNone = 0, kActionBack = 2, kActionMenu = 5;
constexpr int8_t kEntryEnter = 11, kEntryOption = 12;
constexpr uint8_t kAlignLeft = 1, kAlignCenter = 3;
constexpr int kOptionsPage = 33;
constexpr int kBorrowedPage = 25;      // SCREEN_CONTROLS_RESET
constexpr int8_t kFirstAction = 112;   // ours: past GTA's (~67) and Mod Loader's
constexpr uintptr_t kTableRef = 0x579568; // DrawStandardMenus: holds &aScreens[0]
constexpr uintptr_t kOptionsCall = 0x576FEF; // ProcessMenuOptions -> ProcessPCMenuOptions
constexpr uintptr_t kTextCalls[] = {0x579688, 0x579824, 0x579C38, 0x579C93, 0x579D73,
                                    0x579F13, 0x579FCA, 0x57A01F, 0x57A12F, 0x57A161}; // CText::Get in DrawStandardMenus
constexpr int kTextCallCount = sizeof(kTextCalls) / sizeof(kTextCalls[0]);
constexpr const char* kEntryKey = "SKT_OPT";
constexpr const char* kTitleKey = "SKT_TTL";

struct State {
    Option* options = nullptr;
    int count = 0;
    const char* title = "SAN ANSKATEAS";
    Changed changed = nullptr;
    int page = -1;            // the page ours borrows, once the table is found
    Page original{};          // what that page holds when it isn't ours
    bool haveOriginal = false;
    bool hooked = false;
    uintptr_t textPrev[kTextCallCount] = {};
    uintptr_t optionsPrev = 0;
    char log[160] = "";       // what happened, for the host's log
};
inline State g;

inline uintptr_t CallTarget(uintptr_t at) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(at);
    if (IsBadReadPtr(p, 5) || p[0] != 0xE8) return 0;
    return at + 5 + *reinterpret_cast<const int32_t*>(p + 1);
}

inline void WriteCall(uintptr_t at, const void* to) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(at), 5, PAGE_EXECUTE_READWRITE, &old);
    auto* p = reinterpret_cast<uint8_t*>(at);
    p[0] = 0xE8;
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - at - 5);
    VirtualProtect(reinterpret_cast<void*>(at), 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}

// The live page table, wherever it is now; null if it doesn't look like GTA's.
inline Page* Table() {
    if (IsBadReadPtr(reinterpret_cast<void*>(kTableRef), 4)) return nullptr;
    auto* pages = reinterpret_cast<Page*>(*reinterpret_cast<const uintptr_t*>(kTableRef));
    if (IsBadReadPtr(pages, sizeof(Page) * 44) || strncmp(pages[kOptionsPage].title, "FET_OPT", 8) != 0) return nullptr;
    return pages;
}

inline bool Named(const Item& item, const char* key) { return strncmp(item.name, key, 8) == 0; }

inline void SetItem(Item& item, int8_t action, const char* key, int8_t type, int8_t target, uint16_t x, uint16_t y, uint8_t align) {
    memset(&item, 0, sizeof(item));
    item.action = action;
    strncpy_s(item.name, key, 7);
    item.type = type;
    item.target = target;
    item.x = x;
    item.y = y;
    item.align = align;
}

// Text for our keys (the menu gets every string through CText::Get).
inline const char* OurText(const char* key) {
    if (!key || strncmp(key, "SKT_", 4) != 0) return nullptr;
    if (!strcmp(key, kEntryKey) || !strcmp(key, kTitleKey)) return g.title;
    for (int i = 0; i < g.count; i++) {
        if (!strcmp(key, g.options[i].key)) return g.options[i].label;
    }
    return nullptr;
}

using GetText = const char*(__thiscall*)(void*, const char*);
template <int N>
const char* __fastcall TextHook(void* text, void*, const char* key) {
    if (const char* ours = OurText(key)) return ours;
    return reinterpret_cast<GetText>(g.textPrev[N])(text, key);
}
template <int... N>
constexpr auto TextHooks(std::integer_sequence<int, N...>) {
    using Hook = const char*(__fastcall*)(void*, void*, const char*);
    return std::array<Hook, sizeof...(N)>{&TextHook<N>...};
}

inline bool IsOurs(const Page& page) { return strncmp(page.title, kTitleKey, 8) == 0; }

// Our page, written into the borrowed one.
inline void FillPage(Page& ours) {
    memset(&ours, 0, sizeof(ours));
    strncpy_s(ours.title, kTitleKey, 7);
    ours.parent = kOptionsPage;
    ours.start = 0;
    for (int i = 0; i < g.count && i < 10; i++) { // like Audio Setup: left column, 30 apart
        SetItem(ours.items[i], static_cast<int8_t>(kFirstAction + i), g.options[i].key, kEntryOption, static_cast<int8_t>(g.page), 57,
                static_cast<uint16_t>(100 + 30 * i), kAlignLeft);
    }
    SetItem(ours.items[g.count], kActionBack, "FEDS_TB", kEntryEnter, kOptionsPage, 320, static_cast<uint16_t>(130 + 30 * g.count),
            kAlignCenter);
}

// Left/right (and Enter) on one of our entries; Enter on our Options entry
// puts our page in place just before the menu switches to it.
using PcOptions = bool(__thiscall*)(CMenuManager*, int8_t, bool);
inline bool __fastcall OptionsHook(CMenuManager* menu, void*, int8_t leftRight, bool accept) {
    Page* pages = Table();
    int entryIndex = menu->m_nCurrentMenuEntry;
    if (pages && g.page >= 0 && g.haveOriginal && entryIndex >= 0 && entryIndex < 12 && menu->m_nCurrentMenuPage == kOptionsPage &&
        Named(pages[kOptionsPage].items[entryIndex], kEntryKey)) {
        FillPage(pages[g.page]); // the game's own handling then switches to it
    }
    if (pages && g.page >= 0 && menu->m_nCurrentMenuPage == g.page && IsOurs(pages[g.page]) && entryIndex >= 0 && entryIndex < 12) {
        int which = pages[g.page].items[entryIndex].action - kFirstAction;
        if (which >= 0 && which < g.count) {
            Option& o = g.options[which];
            int before = o.value;
            if (o.bars) {
                o.value = std::min(std::max(o.value + leftRight, 0), o.count - 1); // Enter does nothing on a slider
            } else {
                int step = leftRight != 0 ? leftRight : (accept ? 1 : 0); // choices wrap, Enter steps on
                o.value = ((o.value + step) % o.count + o.count) % o.count;
            }
            if (o.value != before) {
                if (leftRight != 0) AudioEngine.ReportFrontendAudioEvent(AE_FRONTEND_HIGHLIGHT, 0.f, 1.f);
                if (g.changed) g.changed(which, o.value);
            }
            return true;
        }
    }
    return g.optionsPrev ? reinterpret_cast<PcOptions>(g.optionsPrev)(menu, leftRight, accept) : false;
}

inline void Note(const char* text) { strncpy_s(g.log, text, _TRUNCATE); }

// Puts our page and Options entry in the live table (again, if something
// rebuilt it). Cheap: call it every menu frame.
inline void EnsureTable() {
    Page* pages = Table();
    if (!pages) {
        Note("menu: GTA's page table wasn't found (not 1.0 US?); options only in SanAnskateas.ini");
        return;
    }
    Page& borrowed = pages[kBorrowedPage];
    if (!g.haveOriginal) {
        if (IsOurs(borrowed) || (!borrowed.title[0] && borrowed.items[0].action == kActionNone)) {
            Note("menu: the page to borrow isn't GTA's (another mod changed it)");
            return;
        }
        g.original = borrowed;
        g.haveOriginal = true;
        g.page = kBorrowedPage;
    }
    // Off our page: the borrowed page is GTA's again.
    if (IsOurs(borrowed) && FrontEndMenuManager.m_nCurrentMenuPage != g.page) borrowed = g.original;
    Page& options = pages[kOptionsPage];
    int n = 0, back = -1, language = -1, entry = -1;
    for (; n < 12 && options.items[n].action != kActionNone; n++) {
        if (Named(options.items[n], "FEDS_TB")) back = n;
        if (Named(options.items[n], "FEH_LAN")) language = n;
        if (Named(options.items[n], kEntryKey)) entry = n;
    }
    if (entry >= 0) {
        options.items[entry].target = static_cast<int8_t>(g.page);
        return;
    }
    if (back < 0 || n >= 12) {
        Note("menu: the Options page has no room for our entry");
        return;
    }
    // Under LANGUAGE if Back follows it (nothing of anyone else's to move),
    // otherwise last, just above Back.
    int at = language >= 0 && language + 1 == back ? language + 1 : back;
    // GTA spaces entries without a place itself, one row (30) under the one
    // before, and caches that place in the table: those moving down a row
    // are cleared so it spaces them again.
    bool spaced[12] = {};
    for (int i = 1; i < n; i++) {
        const Item& a = options.items[i - 1];
        const Item& b = options.items[i];
        spaced[i] = (b.x == 0 && b.y == 0) || (b.x == a.x && b.y == a.y + 30);
    }
    memmove(&options.items[at + 1], &options.items[at], sizeof(Item) * (n - at));
    SetItem(options.items[at], kActionMenu, kEntryKey, kEntryEnter, static_cast<int8_t>(g.page), 0, 0, kAlignCenter);
    for (int i = at; i < n; i++) {
        if (spaced[i]) options.items[i + 1].x = options.items[i + 1].y = 0;
    }
    Note(at == language + 1 ? "menu: Options > SAN ANSKATEAS added under LANGUAGE" : "menu: Options > SAN ANSKATEAS added above Back");
}

// Starts it: hooks the menu's calls once and places the page.
inline void Install(Option* options, int count, Changed changed) {
    g.options = options;
    g.count = count;
    g.changed = changed;
    if (!g.hooked) {
        g.hooked = true;
        static const auto hooks = TextHooks(std::make_integer_sequence<int, kTextCallCount>());
        for (int i = 0; i < kTextCallCount; i++) {
            if (uintptr_t prev = CallTarget(kTextCalls[i])) {
                g.textPrev[i] = prev;
                WriteCall(kTextCalls[i], reinterpret_cast<const void*>(hooks[i]));
            }
        }
        if (uintptr_t prev = CallTarget(kOptionsCall)) {
            g.optionsPrev = prev;
            WriteCall(kOptionsCall, reinterpret_cast<const void*>(&OptionsHook));
        }
    }
    EnsureTable();
}

// GTA's slider (CMenuManager::DisplaySlider) with our own number of bars:
// bars growing taller to the right, lit up to the value, with a shadow.
inline void Slider(CMenuManager& m, float x, float y, float length, int bars, int lit) {
    const CRGBA on(172, 203, 241, 255), off(74, 90, 107, 255), shadow(0, 0, 0, 200);
    float low = m.StretchY(4.f), high = m.StretchY(20.f), step = length / bars, width = step * 0.48f;
    float dx = m.StretchX(2.f), dy = m.StretchY(2.f);
    for (int i = 0; i < bars; i++) {
        float bx = x + i * step, h = (low * (bars - 1 - i) + high * i) / (bars - 1), top = y + high - h, bottom = y + high;
        CSprite2d::DrawRect(CRect(bx + dx, top + dy, bx + width + dx, bottom + dy), shadow);
        CSprite2d::DrawRect(CRect(bx, top, bx + width, bottom), i < lit ? on : off);
    }
}

// Our values over GTA's drawing of the page (the right column it fills in
// for its own options), after the menu is drawn.
inline void Draw(CMenuManager& m) {
    Page* pages = Table();
    if (!pages || g.page < 0 || !m.m_bMenuActive || m.m_nCurrentMenuPage != g.page || !IsOurs(pages[g.page])) return;
    const CRGBA selected(172, 203, 241, 255), normal(74, 90, 107, 255);
    float right = SCREEN_WIDTH - m.StretchX(40.f);
    for (int i = 0; i < g.count; i++) {
        const Item& item = pages[g.page].items[i];
        float y = m.StretchY(static_cast<float>(item.y));
        const Option& o = g.options[i];
        if (o.bars) {
            Slider(m, m.StretchX(500.f), m.StretchY(static_cast<float>(item.y) - 5.f), m.StretchX(100.f), o.count, o.value + 1);
            continue;
        }
        CFont::SetBackground(false, false);
        CFont::SetProportional(true);
        CFont::SetFontStyle(FONT_MENU);
        CFont::SetScale(m.StretchX(0.7f), m.StretchY(1.0f));
        CFont::SetEdge(1);
        CFont::SetDropColor(CRGBA(0, 0, 0, 255));
        CFont::SetOrientation(ALIGN_RIGHT);
        CFont::SetColor(i == m.m_nCurrentMenuEntry ? selected : normal);
        CFont::PrintString(right, y, o.names[o.value]);
    }
    CFont::DrawFonts();
}

} // namespace settings_menu
