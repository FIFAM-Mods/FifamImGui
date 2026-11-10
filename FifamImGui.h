#pragma once
// ImGui structures are shared with FifamImGui.dll, so they must have the same layout as there (default /Zp8).
// Plugins built with another struct member alignment (e.g. /Zp4) must also compile the imgui .cpp files
// with the default alignment (per-file "Struct Member Alignment: Default" in the project).
#pragma pack(push, 8)
#include "imgui.h"
#pragma pack(pop)
#include "plugin.h"
#include <Windows.h>

namespace FifamImGuiDetail {

inline HMODULE GetModule() {
    static HMODULE hMod = []() -> HMODULE {
        std::wstring p = FM::GameDirPath(L"plugins\\FifamImGui.dll");
        return LoadLibraryW(p.c_str());
    }();
    return hMod;
}

inline FARPROC GetProc(const char *name) {
    HMODULE h = GetModule();
    if (!h) return nullptr;
    return GetProcAddress(h, name);
}

}

inline ImGuiContext *ImGuiGetContext() {
    using Fn = ImGuiContext * (*)();
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiGetContext"));
    return fn ? fn() : nullptr;
}

inline void ImGuiSyncContext() {
    ImGui::SetCurrentContext(ImGuiGetContext());
}

#define IMGUI_SYNC ImGuiSyncContext();

inline void ImGuiOnCreate(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiOnCreate"));
    if (fn) fn(callback);
}

inline void ImGuiOnDestroy(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiOnDestroy"));
    if (fn) fn(callback);
}

inline void ImGuiOnUpdate(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiOnUpdate"));
    if (fn) fn(callback);
}

inline void ImGuiOnRender(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiOnRender"));
    if (fn) fn(callback);
}

inline void ImGuiOnLost(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiOnLost"));
    if (fn) fn(callback);
}

inline void ImGuiOnReset(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiOnReset"));
    if (fn) fn(callback);
}

inline void ImGuiRemoveOnCreate(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiRemoveOnCreate"));
    if (fn) fn(callback);
}

inline void ImGuiRemoveOnDestroy(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiRemoveOnDestroy"));
    if (fn) fn(callback);
}

inline void ImGuiRemoveOnUpdate(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiRemoveOnUpdate"));
    if (fn) fn(callback);
}

inline void ImGuiRemoveOnRender(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiRemoveOnRender"));
    if (fn) fn(callback);
}

inline void ImGuiRemoveOnLost(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiRemoveOnLost"));
    if (fn) fn(callback);
}

inline void ImGuiRemoveOnReset(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiRemoveOnReset"));
    if (fn) fn(callback);
}

inline const char *ImGuiGetHostVersion() {
    using Fn = const char *(*)();
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiGetVersion"));
    return fn ? fn() : nullptr;
}

// By default the game cursor is replaced by the ImGui cursor only while the mouse is over an ImGui window.
// This makes ImGui draw the cursor everywhere and hides the game cursor completely.
inline void ImGuiEnableMouseCursor() {
    using Fn = void (*)();
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiEnableMouseCursor"));
    if (fn) fn();
}

// True while FifamImGui hides the game cursor (the mouse is over an ImGui window, or ImGuiEnableMouseCursor() was called)
inline bool ImGuiIsGameCursorHidden() {
    using Fn = bool (*)();
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiIsGameCursorHidden"));
    return fn ? fn() : false;
}

// Returns false if this plugin can't share the ImGui context with FifamImGui.dll (don't call ImGui functions then).
// Call it after IMGUI_SYNC, e.g. in the OnCreate callback.
inline bool ImGuiCheckVersion() {
    const char *hostVersion = ImGuiGetHostVersion();
    if (!hostVersion || strcmp(hostVersion, IMGUI_VERSION) != 0) {
        MessageBoxA(nullptr,
            "FifamImGui version mismatch: your plugin was built against a "
            "different Dear ImGui version than FifamImGui.dll. Rebuild against "
            "the matching imgui headers.",
            "FifamImGui", MB_ICONERROR);
        return false;
    }
    using Fn = void (*)(size_t *, size_t *, size_t *, size_t *, size_t *, size_t *);
    static Fn fn = reinterpret_cast<Fn>(FifamImGuiDetail::GetProc("ExpImGuiGetDataLayout"));
    if (fn) {
        size_t io = 0, style = 0, vec2 = 0, vec4 = 0, vert = 0, idx = 0;
        fn(&io, &style, &vec2, &vec4, &vert, &idx);
        // First compare with this file's view of the structures, then with the plugin's compiled imgui code
        bool ok = io == sizeof(ImGuiIO) && style == sizeof(ImGuiStyle) && vec2 == sizeof(ImVec2) && vec4 == sizeof(ImVec4)
            && vert == sizeof(ImDrawVert) && idx == sizeof(ImDrawIdx)
            && ImGui::DebugCheckVersionAndDataLayout(hostVersion, io, style, vec2, vec4, vert, idx);
        if (!ok) {
            MessageBoxA(nullptr,
                "FifamImGui struct layout mismatch: your plugin compiles Dear ImGui with different "
                "settings than FifamImGui.dll (most likely Struct Member Alignment). Compile the imgui "
                ".cpp files with the default alignment.",
                "FifamImGui", MB_ICONERROR);
            return false;
        }
    }
    return true;
}

inline bool ImGuiEnsureLoaded(bool showErrorIfMissing = true) {
    HMODULE h = FifamImGuiDetail::GetModule();
    if (!h) {
        if (showErrorIfMissing) {
            MessageBoxA(nullptr,
                "FifamImGui.dll was not found in the plugins folder.\n\n"
                "This plugin requires FifamImGui.dll to display its UI. "
                "Please install it alongside your other .asi plugins.",
                "Missing dependency: FifamImGui.dll",
                MB_ICONERROR);
        }
        return false;
    }
    return true;
}
