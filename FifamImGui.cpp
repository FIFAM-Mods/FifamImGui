#include "FifamImGui.h"
#include "GfxCoreHook.h"
#include "plugin.h"
#include <array>
#include <list>
#include <vector>
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx9.h"
#include <d3d9.h>

using namespace plugin;
using namespace std;

#define IMGUI_EXP extern "C" __declspec(dllexport)

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// FIFA Manager 13 (1.0.3.0) addresses.
// GfxCore.dll values are RVAs (use GfxCoreAddress()), Manager.exe values are absolute.
namespace Addr {
    // GfxCore.dll
    constexpr unsigned int CreateDeviceCalls[] = { 0x2D600F, 0x2D6240, 0x2D6371 };           // call EAGL::RenderContextExtension::CreateDirect3DDevice (0x2D59C0)
    constexpr unsigned int ResetDeviceCalls[] = { 0x2D6006, 0x2D6237, 0x2D6369, 0x2D66F5 }; // call EAGL::RenderContextExtension::ResetDirect3DDevice (0x2D55F0)
    constexpr unsigned int DestroyContextCall = 0x2D5E33;  // call EAGL::RenderContext::dtor (0x2D5380), releases the device
    constexpr unsigned int EndFrameVTableEntry = 0x5273F4; // EAGL::RenderContext vtable: EndFrame (0x2D69F0), calls Present
    constexpr unsigned int WndProcPush = 0x7086 + 4;       // CreateGameWindow: WndClass.lpfnWndProc = WndProc (0x6E00)
    constexpr unsigned int EaglWndProc = 0x64C668;         // EAGL window class lpfnWndProc = EAGL::RenderContext::WindowProc (0x2D3C20)
    constexpr unsigned int DeviceHWnd = 0xBEEE04;          // HWND the D3D device renders to (EAGL's own window in fullscreen)
    constexpr unsigned int D3DDevice = 0xBEF498;           // IDirect3DDevice9 *gD3D9Device
    constexpr unsigned int AppHWnd = 0x669064;             // HWND of the game window (CreateGameWindow)
    constexpr unsigned int WheelMessage = 0xD3F51C;        // UINT registered message the game posts for DirectInput wheel
    constexpr unsigned int MouseStatus = 0x655274;         // int gMouseStatus: 4 = mouse from window messages, else DirectInput (6 in game)
    constexpr unsigned int MouseState = 0xBF6110;          // MOUSESTATE (x, y, wheel, buttons) returned by MOUSE_getstate(6)
    constexpr unsigned int ActiveScreenSize = 0xBFA738;    // int EAGL_ActiveScreenWidth, EAGL_ActiveScreenHeight
    constexpr unsigned int MouseModePushes[] = { 0x2837BE + 1, 0x3BB9AD + 1 }; // push 6 before MOUSE_setstate(7 = access mode, 6)
    constexpr unsigned int GuiMouseStateCall = 0x17D34;    // call MOUSE_getstate(6) (0x307C90) when building the GUI input packet
    constexpr unsigned int MouseGetState = 0x307C90;       // int *MOUSE_getstate(int)
    constexpr unsigned int UpdateCursorPos = 0x3BB4D0;     // void CApp::UpdateCursorPos(CApp *), called by CApp::ProcessInput (and by UCP)
    // Manager.exe
    constexpr unsigned int RenderCursorCalls[] = { 0x4500B3, 0x451157 }; // call RenderCursor(&gCursor) (0x494A90)
}

enum eCallbackList {
    LIST_CREATE,
    LIST_DESTROY,
    LIST_UPDATE,
    LIST_RENDER,
    LIST_LOST,
    LIST_RESET,
    NUM_CALLBACK_LISTS
};

uintptr_t OrigCreateDevice[std::size(Addr::CreateDeviceCalls)] = {};
uintptr_t OrigResetDevice[std::size(Addr::ResetDeviceCalls)] = {};
uintptr_t OrigDestroyContext = 0;
uintptr_t OrigEndFrame = 0;
uintptr_t OrigWndProc = 0;
uintptr_t OrigEaglWndProc = 0;
uintptr_t OrigRenderCursor[std::size(Addr::RenderCursorCalls)] = {};
uintptr_t OrigGuiMouseState = 0;

struct GameMouseState {
    int x, y;
    int wheel;
    unsigned char buttons[4];
    int maxY, maxX;
};

static bool gShutDown = false;
static bool gDeviceObjectsValid = false;
static HWND gImGuiWindow = 0; // window the Win32 backend is attached to

// Cursor handling. The game hides the Windows cursor and draws its own one (RenderCursor in Manager.exe).
// While the mouse is over an ImGui window, the game cursor is hidden and ImGui draws its cursor instead
// (or, if the Windows cursor is visible, e.g. UCP's "Windows mouse pointer" option, ImGui sets its shape).
// ImGuiEnableMouseCursor() switches to "always": the game cursor is never drawn and ImGui's cursor is used everywhere.
enum eCursorMode {
    CURSOR_SWAP_OVER_IMGUI,
    CURSOR_IMGUI_ALWAYS
};
static eCursorMode gCursorMode = CURSOR_SWAP_OVER_IMGUI;
static unsigned char gHideGameCursor = 0; // read from the naked RenderCursor hooks
static bool gBlockGameMouse = false;      // ImGui wants the mouse: the game gets no mouse button presses
static bool gBlockGameKeyboard = false;   // ImGui wants the keyboard: the game gets no key presses

// In fullscreen the game moves its cursor with DirectInput (relative movement, no acceleration, clamped to the
// screen), so the Windows cursor position doesn't match the game cursor. ImGui then takes the position and the
// buttons from the game's mouse state instead of window messages.
bool GameUsesDirectInputMouse() {
    return *(int *)GfxCoreAddress(Addr::MouseStatus) != 4;
}

void AddGameMouseEvents(ImGuiIO &io) {
    auto ms = (GameMouseState *)GfxCoreAddress(Addr::MouseState);
    int screenW = *(int *)GfxCoreAddress(Addr::ActiveScreenSize);
    int screenH = *(int *)GfxCoreAddress(Addr::ActiveScreenSize + 4);
    float x = (float)ms->x, y = (float)ms->y;
    if (screenW > 0 && screenH > 0 && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
        x = x * io.DisplaySize.x / screenW;
        y = y * io.DisplaySize.y / screenH;
    }
    io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    io.AddMousePosEvent(x, y);
    for (int i = 0; i < 3; i++)
        io.AddMouseButtonEvent(i, ms->buttons[i] != 0);
}

static struct CallbackLists {
    array<list<void(*)()>, NUM_CALLBACK_LISTS> lists;

    ~CallbackLists() {
        gShutDown = true;
    }
} gCallbacks;

void ExecuteList(eCallbackList listId) {
    if (gShutDown)
        return;
    auto const &callbacks = gCallbacks.lists[listId];
    vector<void(*)()> snapshot(callbacks.begin(), callbacks.end());
    for (auto callback : snapshot)
        callback();
}

void AddCallback(eCallbackList listId, void (*callback)()) {
    if (gShutDown)
        return;
    gCallbacks.lists[listId].push_back(callback);
}

void RemoveCallback(eCallbackList listId, void (*callback)()) {
    if (gShutDown)
        return;
    gCallbacks.lists[listId].remove(callback);
}

IDirect3DDevice9 *GetDevice() {
    return *(IDirect3DDevice9 **)GfxCoreAddress(Addr::D3DDevice);
}

// The window that renders and receives input. In windowed mode EAGL uses the game window (CreateGameWindow),
// in fullscreen it creates its own window with EAGL::RenderContext::WindowProc.
HWND GetGameWindow(IDirect3DDevice9 *device) {
    HWND hWnd = *(HWND *)GfxCoreAddress(Addr::DeviceHWnd);
    if (!hWnd)
        hWnd = *(HWND *)GfxCoreAddress(Addr::AppHWnd);
    if (!hWnd && device) {
        D3DDEVICE_CREATION_PARAMETERS params = {};
        if (SUCCEEDED(device->GetCreationParameters(&params)))
            hWnd = params.hFocusWindow;
    }
    return hWnd;
}

bool IsWindowsCursorVisible() {
    CURSORINFO info = { sizeof(CURSORINFO) };
    return GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING);
}

// Called before ImGui::NewFrame(). Uses WantCaptureMouse from the previous frame, the same value WndProc uses.
void UpdateCursor() {
    ImGuiIO &io = ImGui::GetIO();
    bool imguiOwnsCursor = gCursorMode == CURSOR_IMGUI_ALWAYS || io.WantCaptureMouse;
    gHideGameCursor = imguiOwnsCursor;
    gBlockGameMouse = io.WantCaptureMouse;
    gBlockGameKeyboard = io.WantCaptureKeyboard;
    if (imguiOwnsCursor) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        io.MouseDrawCursor = !IsWindowsCursorVisible();
    }
    else {
        // Leave the cursor to the game (it sets its own one on WM_SETCURSOR)
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        io.MouseDrawCursor = false;
    }
}

bool IsMouseMessage(UINT msg) {
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)
        return true;
    UINT wheelMsg = *(UINT *)GfxCoreAddress(Addr::WheelMessage);
    return wheelMsg && msg == wheelMsg;
}

bool IsReleaseMessage(UINT msg) {
    // Always let the game see these, so a key or button pressed before ImGui took focus doesn't get stuck
    return msg == WM_KEYUP || msg == WM_SYSKEYUP || msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP
        || msg == WM_XBUTTONUP;
}

LRESULT ProcessWindowMessage(uintptr_t origWndProc, HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (gShutDown || !ImGui::GetCurrentContext() || hWnd != gImGuiWindow)
        return CallMethodAndReturnDynGlobal<LRESULT>(origWndProc, 0, hWnd, msg, wParam, lParam);
    LRESULT handled = 0;
    // With the DirectInput mouse, the mouse position and buttons come from the game (AddGameMouseEvents), only the wheel from here
    bool skipImGui = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL)
        && GameUsesDirectInputMouse();
    if (!skipImGui)
        handled = ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    if (msg == WM_SETCURSOR && handled)
        return TRUE; // ImGui has set the cursor shape, don't let the game replace it
    ImGuiIO &io = ImGui::GetIO();
    if (!IsReleaseMessage(msg)) {
        bool isKeyMsg = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST);
        if ((io.WantCaptureMouse && IsMouseMessage(msg)) || (io.WantCaptureKeyboard && isKeyMsg))
            return 0;
    }
    return CallMethodAndReturnDynGlobal<LRESULT>(origWndProc, 0, hWnd, msg, wParam, lParam);
}

LRESULT WINAPI GameWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return ProcessWindowMessage(OrigWndProc, hWnd, msg, wParam, lParam);
}

LRESULT WINAPI EaglWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return ProcessWindowMessage(OrigEaglWndProc, hWnd, msg, wParam, lParam);
}

// Switching between windowed and fullscreen can replace the window, so the Win32 backend follows the device window
void UpdateImGuiWindow(IDirect3DDevice9 *device) {
    HWND hWnd = GetGameWindow(device);
    if (hWnd && hWnd != gImGuiWindow) {
        if (gImGuiWindow)
            ImGui_ImplWin32_Shutdown();
        ImGui_ImplWin32_Init(hWnd);
        gImGuiWindow = hWnd;
    }
}

void InitImGui(IDirect3DDevice9 *device) {
    if (ImGui::GetCurrentContext())
        return;
    IMGUI_CHECKVERSION();
    ImGui::SetCurrentContext(ImGui::CreateContext());
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // Custom font: plugins\FifamImGui.ttf (size from plugins\FifamImGui.ini, [Font] Size=, default 14), else ImGui's
    // ProggyClean (Latin-1 only). The atlas is dynamic in 1.92, so any character the font has loads on first use.
    ImFont *font = nullptr;
    std::wstring fontPathW = FM::GameDirPath(L"plugins\\FifamImGui.ttf");
    if (GetFileAttributesW(fontPathW.c_str()) != INVALID_FILE_ATTRIBUTES) {
        int size = GetPrivateProfileIntW(L"Font", L"Size", 14, FM::GameDirPath(L"plugins\\FifamImGui.ini").c_str());
        char fontPath[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, fontPathW.c_str(), -1, fontPath, sizeof(fontPath), nullptr, nullptr);
        ImFontConfig cfg;
        cfg.OversampleH = 1; // pixel fonts (Terminus) stay sharp at their native sizes
        cfg.OversampleV = 1;
        cfg.PixelSnapH = true;
        font = io.Fonts->AddFontFromFileTTF(fontPath, float(size < 6 ? 14 : size), &cfg);
    }
    if (!font)
        io.Fonts->AddFontDefault();
    ImGui::StyleColorsDark();
    gImGuiWindow = 0;
    UpdateImGuiWindow(device);
    ImGui_ImplDX9_Init(device);
    gDeviceObjectsValid = true;
    ExecuteList(LIST_CREATE);
}

void ShutdownImGui() {
    if (!ImGui::GetCurrentContext())
        return;
    ExecuteList(LIST_DESTROY);
    ImGui_ImplDX9_Shutdown();
    if (gImGuiWindow)
        ImGui_ImplWin32_Shutdown();
    gImGuiWindow = 0;
    ImGui::DestroyContext();
    gBlockGameMouse = false;
    gBlockGameKeyboard = false;
    gDeviceObjectsValid = false;
    gHideGameCursor = false;
}

template<unsigned int ID>
HRESULT METHOD OnCreateDevice(void *extension, DUMMY_ARG, int skipCapsPostprocess) {
    HRESULT result = CallMethodAndReturnDynGlobal<HRESULT>(OrigCreateDevice[ID], extension, skipCapsPostprocess);
    IDirect3DDevice9 *device = GetDevice();
    if (SUCCEEDED(result) && device)
        InitImGui(device);
    return result;
}

template<unsigned int ID>
HRESULT METHOD OnResetDevice(void *extension) {
    // The game calls Reset again while the device stays lost, so only invalidate once
    if (ImGui::GetCurrentContext() && gDeviceObjectsValid) {
        ExecuteList(LIST_LOST);
        ImGui_ImplDX9_InvalidateDeviceObjects();
        gDeviceObjectsValid = false;
    }
    HRESULT result = CallMethodAndReturnDynGlobal<HRESULT>(OrigResetDevice[ID], extension);
    if (ImGui::GetCurrentContext() && !gDeviceObjectsValid && SUCCEEDED(result)) {
        ImGui_ImplDX9_CreateDeviceObjects();
        gDeviceObjectsValid = true;
        ExecuteList(LIST_RESET);
    }
    return result;
}

void METHOD OnDestroyContext(void *renderContext) {
    ShutdownImGui();
    CallMethodDynGlobal(OrigDestroyContext, renderContext);
}

void METHOD OnEndFrame(void *renderContext) {
    ExecuteList(LIST_UPDATE);
    IDirect3DDevice9 *device = GetDevice();
    if (ImGui::GetCurrentContext() && gDeviceObjectsValid && device && device->TestCooperativeLevel() == D3D_OK) {
        UpdateImGuiWindow(device);
        UpdateCursor();
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (GameUsesDirectInputMouse())
            AddGameMouseEvents(ImGui::GetIO()); // after ImGui_ImplWin32_NewFrame, which may add the Windows cursor position
        ImGui::NewFrame();
        ExecuteList(LIST_RENDER);
        ImGui::EndFrame();
        ImGui::Render();
        // EAGL ends the scene with each viewport, so there's normally no active scene here
        bool sceneStarted = SUCCEEDED(device->BeginScene());
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        if (sceneStarted)
            device->EndScene();
    }
    CallMethodDynGlobal(OrigEndFrame, renderContext);
}

// RenderCursor is __usercall (ecx = cursor, xmm1 = float) with no stack arguments, so these are naked
// to keep xmm1 intact when passing the call on.
// The game GUI reads the mouse buttons from here
int *__cdecl OnGetGuiMouseState(int type) {
    int *result = CallAndReturnDynGlobal<int *>(OrigGuiMouseState, type);
    if (result && type == 6 && gBlockGameMouse) {
        static GameMouseState masked;
        masked = *(GameMouseState *)result;
        memset(masked.buttons, 0, sizeof(masked.buttons));
        return (int *)&masked;
    }
    return result;
}

__declspec(naked) void OnRenderCursor0() {
    __asm {
        cmp byte ptr [gHideGameCursor], 0
        jne skip
        jmp dword ptr [OrigRenderCursor]
    skip:
        mov eax, ecx
        ret
    }
}

__declspec(naked) void OnRenderCursor1() {
    __asm {
        cmp byte ptr [gHideGameCursor], 0
        jne skip
        jmp dword ptr [OrigRenderCursor + 4]
    skip:
        mov eax, ecx
        ret
    }
}

IMGUI_EXP ImGuiContext *ExpImGuiGetContext() { return ImGui::GetCurrentContext(); }
IMGUI_EXP void ExpImGuiOnCreate(void (*callback)()) { AddCallback(LIST_CREATE, callback); }
IMGUI_EXP void ExpImGuiOnDestroy(void (*callback)()) { AddCallback(LIST_DESTROY, callback); }
IMGUI_EXP void ExpImGuiOnUpdate(void (*callback)()) { AddCallback(LIST_UPDATE, callback); }
IMGUI_EXP void ExpImGuiOnRender(void (*callback)()) { AddCallback(LIST_RENDER, callback); }
IMGUI_EXP void ExpImGuiOnLost(void (*callback)()) { AddCallback(LIST_LOST, callback); }
IMGUI_EXP void ExpImGuiOnReset(void (*callback)()) { AddCallback(LIST_RESET, callback); }
IMGUI_EXP void ExpImGuiRemoveOnCreate(void (*callback)()) { RemoveCallback(LIST_CREATE, callback); }
IMGUI_EXP void ExpImGuiRemoveOnDestroy(void (*callback)()) { RemoveCallback(LIST_DESTROY, callback); }
IMGUI_EXP void ExpImGuiRemoveOnUpdate(void (*callback)()) { RemoveCallback(LIST_UPDATE, callback); }
IMGUI_EXP void ExpImGuiRemoveOnRender(void (*callback)()) { RemoveCallback(LIST_RENDER, callback); }
IMGUI_EXP void ExpImGuiRemoveOnLost(void (*callback)()) { RemoveCallback(LIST_LOST, callback); }
IMGUI_EXP void ExpImGuiRemoveOnReset(void (*callback)()) { RemoveCallback(LIST_RESET, callback); }
IMGUI_EXP const char *ExpImGuiGetVersion() { return IMGUI_VERSION; }
IMGUI_EXP void ExpImGuiGetDataLayout(size_t *io, size_t *style, size_t *vec2, size_t *vec4, size_t *vert, size_t *idx) {
    *io = sizeof(ImGuiIO); *style = sizeof(ImGuiStyle); *vec2 = sizeof(ImVec2); *vec4 = sizeof(ImVec4);
    *vert = sizeof(ImDrawVert); *idx = sizeof(ImDrawIdx);
}
IMGUI_EXP void ExpImGuiEnableMouseCursor() { gCursorMode = CURSOR_IMGUI_ALWAYS; }
IMGUI_EXP bool ExpImGuiIsGameCursorHidden() { return gHideGameCursor != 0; }

// Replaces CApp::UpdateCursorPos, which gives the game GUI its cursor position.
// In fullscreen the original doesn't use the Windows cursor: it reads relative movement and moves the Windows cursor
// back to the window center every frame. Its "window cursor" variant measures CApp's window, which in fullscreen isn't
// the one on screen (EAGL creates its own). This takes the Windows cursor position relative to the window the game
// renders to, like UCP's ProcessMouse does in windowed mode.
void METHOD OnUpdateCursorPos(void *app) {
    HWND hWnd = GetGameWindow(GetDevice());
    float appX = 0.0f, appY = 0.0f;
    CallVirtualMethod<43>(app, &appX, &appY); // GetScreenResolution
    POINT point = {};
    RECT rect = {};
    float x = *raw_ptr<float>(app, 0xF4), y = *raw_ptr<float>(app, 0xF8);
    if (hWnd && GetCursorPos(&point) && ScreenToClient(hWnd, &point) && GetClientRect(hWnd, &rect)
        && rect.right > rect.left && rect.bottom > rect.top)
    {
        x = (float)((double)(point.x - rect.left) * appX / (double)(rect.right - rect.left));
        y = (float)((double)(point.y - rect.top) * appY / (double)(rect.bottom - rect.top));
        x = x < 0.0f ? 0.0f : (x < appX ? x : appX - 1.0f);
        y = y < 0.0f ? 0.0f : (y < appY ? y : appY - 1.0f);
    }
    *raw_ptr<float>(app, 0x100) = x - *raw_ptr<float>(app, 0xF4); // m_vecMouseMovement
    *raw_ptr<float>(app, 0x104) = y - *raw_ptr<float>(app, 0xF8);
    *raw_ptr<float>(app, 0xF4) = x;                                // m_vecMousePos
    *raw_ptr<float>(app, 0xF8) = y;
    int *mouseState = CallAndReturnDynGlobal<int *>(GfxCoreAddress(Addr::MouseGetState), 6);
    *raw_ptr<float>(app, 0xFC) = (float)mouseState[2];             // wheel
    *raw_ptr<float>(app, 0x108) = *raw_ptr<float>(app, 0xFC);
}

// The game GUI (Manager.exe CGuiServer input pump) and GfxCore read buttons and keys with GetKeyState/GetAsyncKeyState,
// which don't depend on the window messages we block, so these are masked too.
bool IsMouseButtonKey(int vKey) {
    return vKey == VK_LBUTTON || vKey == VK_RBUTTON || vKey == VK_MBUTTON || vKey == VK_XBUTTON1 || vKey == VK_XBUTTON2;
}

bool IsKeyBlocked(int vKey) {
    if (gShutDown)
        return false;
    return IsMouseButtonKey(vKey) ? gBlockGameMouse : gBlockGameKeyboard;
}

SHORT WINAPI OnGetKeyState(int vKey) {
    return IsKeyBlocked(vKey) ? 0 : GetKeyState(vKey);
}

SHORT WINAPI OnGetAsyncKeyState(int vKey) {
    return IsKeyBlocked(vKey) ? 0 : GetAsyncKeyState(vKey);
}

// Replaces an imported function in a module's import address table. Returns the number of patched entries.
unsigned int PatchImport(HMODULE module, void *original, void *replacement) {
    if (!module || !original)
        return 0;
    auto base = (unsigned char *)module;
    auto dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    auto nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    auto &iatDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    auto &impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    unsigned int count = 0;
    auto patchRange = [&](uintptr_t *begin, uintptr_t *end) {
        for (uintptr_t *p = begin; p < end; p++) {
            if (*p == (uintptr_t)original) {
                patch::SetPointer((uintptr_t)p, replacement);
                count++;
            }
        }
    };
    if (impDir.VirtualAddress && impDir.Size) {
        for (auto desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + impDir.VirtualAddress); desc->Name || desc->FirstThunk; desc++) {
            if (!desc->FirstThunk)
                continue;
            auto first = (uintptr_t *)(base + desc->FirstThunk);
            auto last = first;
            while (*last)
                last++;
            patchRange(first, last);
        }
    }
    if (!count && iatDir.VirtualAddress && iatDir.Size)
        patchRange((uintptr_t *)(base + iatDir.VirtualAddress), (uintptr_t *)(base + iatDir.VirtualAddress + iatDir.Size));
    if (!count && !impDir.Size && !iatDir.Size) {
        // Packed executable (Manager.exe) without import directories: the IAT is at the start of .rdata
        auto section = IMAGE_FIRST_SECTION(nt);
        for (unsigned int i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
            if (!strncmp((char const *)section->Name, ".rdata", 8)) {
                auto start = (uintptr_t *)(base + section->VirtualAddress);
                patchRange(start, start + min<DWORD>(section->Misc.VirtualSize, 0x2000) / sizeof(uintptr_t));
                break;
            }
        }
    }
    return count;
}

void InstallGfxCorePatches() {
    OrigCreateDevice[0] = patch::RedirectCall(GfxCoreAddress(Addr::CreateDeviceCalls[0]), OnCreateDevice<0>);
    OrigCreateDevice[1] = patch::RedirectCall(GfxCoreAddress(Addr::CreateDeviceCalls[1]), OnCreateDevice<1>);
    OrigCreateDevice[2] = patch::RedirectCall(GfxCoreAddress(Addr::CreateDeviceCalls[2]), OnCreateDevice<2>);
    OrigResetDevice[0] = patch::RedirectCall(GfxCoreAddress(Addr::ResetDeviceCalls[0]), OnResetDevice<0>);
    OrigResetDevice[1] = patch::RedirectCall(GfxCoreAddress(Addr::ResetDeviceCalls[1]), OnResetDevice<1>);
    OrigResetDevice[2] = patch::RedirectCall(GfxCoreAddress(Addr::ResetDeviceCalls[2]), OnResetDevice<2>);
    OrigResetDevice[3] = patch::RedirectCall(GfxCoreAddress(Addr::ResetDeviceCalls[3]), OnResetDevice<3>);
    OrigDestroyContext = patch::RedirectCall(GfxCoreAddress(Addr::DestroyContextCall), OnDestroyContext);
    OrigEndFrame = patch::SetPointer(GfxCoreAddress(Addr::EndFrameVTableEntry), OnEndFrame);
    OrigWndProc = patch::SetPointer(GfxCoreAddress(Addr::WndProcPush), GameWndProc);
    OrigEaglWndProc = patch::SetPointer(GfxCoreAddress(Addr::EaglWndProc), EaglWndProc);
    OrigGuiMouseState = patch::RedirectCall(GfxCoreAddress(Addr::GuiMouseStateCall), OnGetGuiMouseState);
    // Use the window-message mouse mode (4) instead of DirectInput (6), so the game cursor follows the Windows cursor
    // and ImGui and the game agree on the position. The DirectInput path above stays for the case someone switches back.
    for (auto addr : Addr::MouseModePushes) {
        if (patch::GetUChar(GfxCoreAddress(addr)) == 6)
            patch::SetUChar(GfxCoreAddress(addr), 4);
    }
    patch::RedirectJump(GfxCoreAddress(Addr::UpdateCursorPos), OnUpdateCursorPos);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    void *getKeyState = GetProcAddress(user32, "GetKeyState");
    void *getAsyncKeyState = GetProcAddress(user32, "GetAsyncKeyState");
    unsigned int patched[2][2] = {};
    HMODULE modules[2] = { GetModuleHandleA(NULL), (HMODULE)GfxCoreAddress(0) };
    for (unsigned int i = 0; i < 2; i++) {
        patched[i][0] = PatchImport(modules[i], getKeyState, OnGetKeyState);
        patched[i][1] = PatchImport(modules[i], getAsyncKeyState, OnGetAsyncKeyState);
    }
    FILE *log = _wfopen(FM::GameDirPath(L"plugins\\FifamImGui.log").c_str(), L"wt");
    if (log) {
        fprintf(log, "GetKeyState/GetAsyncKeyState patched: Manager.exe %u/%u, GfxCore.dll %u/%u\n",
            patched[0][0], patched[0][1], patched[1][0], patched[1][1]);
        fclose(log);
    }
    // Installed here (not at startup) so that we chain to UCP's RenderCursor call hooks, which are set at startup
    OrigRenderCursor[0] = patch::RedirectCall(Addr::RenderCursorCalls[0], OnRenderCursor0);
    OrigRenderCursor[1] = patch::RedirectCall(Addr::RenderCursorCalls[1], OnRenderCursor1);
}

class FifamImGui {
public:
    FifamImGui() {
        if (FM::GetAppVersion().id() == ID_FM_13_1030_RLD)
            InstallGfxCoreHook(InstallGfxCorePatches);
    }
} fifamImGui;
