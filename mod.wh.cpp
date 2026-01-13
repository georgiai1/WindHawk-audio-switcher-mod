// ==WindhawkMod==
// @id              audio-scroll-switcher
// @name            Audio Output Device Switcher
// @description     Ctrl+Scroll on the taskbar to switch audio output devices
// @version         1.0
// @author          You
// @github          https://github.com/you
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lpropsys -lcomctl32 -lgdi32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Audio Output Device Switcher

Switch between audio output devices by holding **Ctrl** and scrolling the mouse
wheel over the taskbar.

## Usage
1. Compile the mod (Ctrl+B)
2. Enable the mod
3. Hold Ctrl and scroll over the taskbar to switch audio devices
4. A popup shows which device is now active
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- showNotification: true
  $name: Show notification
  $description: Show a popup notification when switching devices
- notificationDuration: 1500
  $name: Notification duration (ms)
  $description: How long to show the notification (in milliseconds)
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <vector>
#include <string>
#include <unordered_set>

// PKEY_Device_FriendlyName
static const PROPERTYKEY PKEY_Device_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } },
    14
};

// IPolicyConfig interface (undocumented but widely used)
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

const CLSID CLSID_CPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}
};

const IID IID_IPolicyConfig = {
    0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}
};

// Settings
struct {
    bool showNotification;
    int notificationDuration;
} g_settings;

// Globals
HWND g_hTaskbarWnd = nullptr;
DWORD g_dwTaskbarThreadId = 0;
std::unordered_set<HWND> g_secondaryTaskbarWindows;
HWND g_popupWnd = nullptr;
std::wstring g_popupText;
const wchar_t* POPUP_CLASS_NAME = L"AudioSwitcherPopup_WH";
bool g_popupClassRegistered = false;

UINT g_subclassRegisteredMsg = RegisterWindowMessage(
    L"Windhawk_SetWindowSubclassFromAnyThread_audio-scroll-switcher");

struct AudioDevice {
    std::wstring id;
    std::wstring name;
};

// ============================================================================
// Audio Device Functions
// ============================================================================

std::vector<AudioDevice> GetAudioOutputDevices() {
    std::vector<AudioDevice> devices;
    IMMDeviceEnumerator* pEnumerator = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);

    if (FAILED(hr) || !pEnumerator) {
        Wh_Log(L"Failed to create device enumerator: 0x%08X", hr);
        return devices;
    }

    IMMDeviceCollection* pCollection = nullptr;
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr) || !pCollection) {
        Wh_Log(L"Failed to enumerate endpoints: 0x%08X", hr);
        pEnumerator->Release();
        return devices;
    }

    UINT count = 0;
    pCollection->GetCount(&count);
    Wh_Log(L"Found %u audio devices", count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice* pDevice = nullptr;
        if (SUCCEEDED(pCollection->Item(i, &pDevice)) && pDevice) {
            LPWSTR deviceId = nullptr;
            if (SUCCEEDED(pDevice->GetId(&deviceId)) && deviceId) {
                AudioDevice device;
                device.id = deviceId;
                CoTaskMemFree(deviceId);

                IPropertyStore* pProps = nullptr;
                if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) &&
                        varName.vt == VT_LPWSTR && varName.pwszVal) {
                        device.name = varName.pwszVal;
                    } else {
                        device.name = L"Unknown Device";
                    }
                    PropVariantClear(&varName);
                    pProps->Release();
                }
                devices.push_back(device);
                Wh_Log(L"  Device %u: %s", i, device.name.c_str());
            }
            pDevice->Release();
        }
    }

    pCollection->Release();
    pEnumerator->Release();
    return devices;
}

std::wstring GetDefaultDeviceId() {
    std::wstring deviceId;
    IMMDeviceEnumerator* pEnumerator = nullptr;

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator)) || !pEnumerator) {
        return deviceId;
    }

    IMMDevice* pDevice = nullptr;
    if (SUCCEEDED(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice)) && pDevice) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(pDevice->GetId(&id)) && id) {
            deviceId = id;
            CoTaskMemFree(id);
        }
        pDevice->Release();
    }
    pEnumerator->Release();
    return deviceId;
}

bool SetDefaultAudioDevice(const std::wstring& deviceId) {
    IPolicyConfig* pPolicyConfig = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL,
        IID_IPolicyConfig, (void**)&pPolicyConfig);

    if (FAILED(hr) || !pPolicyConfig) {
        Wh_Log(L"Failed to create PolicyConfig: 0x%08X", hr);
        return false;
    }

    hr = pPolicyConfig->SetDefaultEndpoint(deviceId.c_str(), eConsole);
    if (SUCCEEDED(hr)) {
        pPolicyConfig->SetDefaultEndpoint(deviceId.c_str(), eMultimedia);
        pPolicyConfig->SetDefaultEndpoint(deviceId.c_str(), eCommunications);
    }
    pPolicyConfig->Release();

    return SUCCEEDED(hr);
}

// ============================================================================
// Notification Popup
// ============================================================================

LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            // Dark background
            HBRUSH bgBrush = CreateSolidBrush(RGB(30, 30, 30));
            FillRect(hdc, &rc, bgBrush);
            DeleteObject(bgBrush);

            // Border
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(100, 100, 100));
            HPEN oldPen = (HPEN)SelectObject(hdc, pen);
            HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, 0, 0, rc.right, rc.bottom);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(pen);

            // Text
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkMode(hdc, TRANSPARENT);

            HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(hdc, hFont);

            DrawTextW(hdc, g_popupText.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, oldFont);
            DeleteObject(hFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            KillTimer(hwnd, 1);
            DestroyWindow(hwnd);
            g_popupWnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RegisterPopupClass() {
    if (g_popupClassRegistered) return;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = PopupWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = POPUP_CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    if (RegisterClassExW(&wc)) {
        g_popupClassRegistered = true;
        Wh_Log(L"Popup class registered");
    }
}

void ShowNotification(const std::wstring& deviceName) {
    Wh_Log(L"Switched to: %s", deviceName.c_str());

    if (!g_settings.showNotification) return;

    RegisterPopupClass();

    if (g_popupWnd) {
        DestroyWindow(g_popupWnd);
        g_popupWnd = nullptr;
    }

    g_popupText = L"Audio: " + deviceName;

    // Get work area (excludes taskbar)
    RECT workArea;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);

    int popupWidth = 350;
    int popupHeight = 45;
    int x = (workArea.right - workArea.left - popupWidth) / 2 + workArea.left;
    int y = workArea.bottom - popupHeight - 10;

    g_popupWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        POPUP_CLASS_NAME, L"", WS_POPUP,
        x, y, popupWidth, popupHeight,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr
    );

    if (g_popupWnd) {
        SetLayeredWindowAttributes(g_popupWnd, 0, 230, LWA_ALPHA);
        ShowWindow(g_popupWnd, SW_SHOWNOACTIVATE);
        UpdateWindow(g_popupWnd);
        SetTimer(g_popupWnd, 1, g_settings.notificationDuration, nullptr);
        Wh_Log(L"Notification shown");
    } else {
        Wh_Log(L"Failed to create popup: %u", GetLastError());
    }
}

// ============================================================================
// Audio Switching Logic
// ============================================================================

void SwitchAudioDevice(int direction) {
    Wh_Log(L"SwitchAudioDevice called with direction=%d", direction);

    auto devices = GetAudioOutputDevices();
    if (devices.size() < 2) {
        Wh_Log(L"Not enough devices to switch (found %zu)", devices.size());
        return;
    }

    std::wstring currentId = GetDefaultDeviceId();
    int currentIndex = 0;

    for (size_t i = 0; i < devices.size(); i++) {
        if (devices[i].id == currentId) {
            currentIndex = (int)i;
            break;
        }
    }

    int nextIndex = (currentIndex + direction + (int)devices.size()) % (int)devices.size();

    Wh_Log(L"Switching from index %d to %d", currentIndex, nextIndex);

    if (SetDefaultAudioDevice(devices[nextIndex].id)) {
        ShowNotification(devices[nextIndex].name);
    } else {
        Wh_Log(L"Failed to set default audio device");
    }
}

// ============================================================================
// Taskbar Subclassing
// ============================================================================

bool IsTaskbarWindow(HWND hWnd) {
    WCHAR szClassName[32];
    if (!GetClassName(hWnd, szClassName, ARRAYSIZE(szClassName))) {
        return false;
    }
    return _wcsicmp(szClassName, L"Shell_TrayWnd") == 0 ||
           _wcsicmp(szClassName, L"Shell_SecondaryTrayWnd") == 0;
}

bool OnMouseWheel(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    // Check if Ctrl is held
    bool ctrlHeld = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    if (!ctrlHeld) {
        return false;
    }

    // Make sure we're over the taskbar
    POINT pt;
    pt.x = GET_X_LPARAM(lParam);
    pt.y = GET_Y_LPARAM(lParam);

    RECT rc;
    if (!GetWindowRect(hWnd, &rc) || !PtInRect(&rc, pt)) {
        return false;
    }

    int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    int direction = (delta > 0) ? -1 : 1;  // Up = previous, Down = next

    Wh_Log(L"Ctrl+Scroll detected! delta=%d, direction=%d", delta, direction);

    SwitchAudioDevice(direction);
    return true;
}

LRESULT CALLBACK TaskbarWindowSubclassProc(
    HWND hWnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR uIdSubclass,
    DWORD_PTR dwRefData
) {
    if (uMsg == WM_NCDESTROY || (uMsg == g_subclassRegisteredMsg && !wParam)) {
        RemoveWindowSubclass(hWnd, TaskbarWindowSubclassProc, 0);
    }

    switch (uMsg) {
        case WM_MOUSEWHEEL:
            if (OnMouseWheel(hWnd, wParam, lParam)) {
                return 0;  // Consume the message
            }
            break;

        case WM_NCDESTROY:
            if (hWnd != g_hTaskbarWnd) {
                g_secondaryTaskbarWindows.erase(hWnd);
            }
            break;
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// For Windows 11 XAML islands
WNDPROC InputSiteWindowProc_Original = nullptr;

LRESULT CALLBACK InputSiteWindowProc_Hook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_POINTERWHEEL) {
        HWND hRootWnd = GetAncestor(hWnd, GA_ROOT);
        if (IsTaskbarWindow(hRootWnd) && OnMouseWheel(hRootWnd, wParam, lParam)) {
            return 0;
        }
    }
    return InputSiteWindowProc_Original(hWnd, uMsg, wParam, lParam);
}

BOOL SetWindowSubclassFromAnyThread(HWND hWnd, SUBCLASSPROC pfnSubclass, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    struct SET_WINDOW_SUBCLASS_PARAM {
        SUBCLASSPROC pfnSubclass;
        UINT_PTR uIdSubclass;
        DWORD_PTR dwRefData;
        BOOL result;
    };

    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (dwThreadId == 0) {
        return FALSE;
    }

    if (dwThreadId == GetCurrentThreadId()) {
        return SetWindowSubclass(hWnd, pfnSubclass, uIdSubclass, dwRefData);
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION) {
                const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
                if (cwp->message == g_subclassRegisteredMsg && cwp->wParam) {
                    auto* param = (SET_WINDOW_SUBCLASS_PARAM*)cwp->lParam;
                    param->result = SetWindowSubclass(
                        cwp->hwnd,
                        param->pfnSubclass,
                        param->uIdSubclass,
                        param->dwRefData
                    );
                }
            }
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr,
        dwThreadId
    );

    if (!hook) {
        return FALSE;
    }

    SET_WINDOW_SUBCLASS_PARAM param;
    param.pfnSubclass = pfnSubclass;
    param.uIdSubclass = uIdSubclass;
    param.dwRefData = dwRefData;
    param.result = FALSE;

    SendMessage(hWnd, g_subclassRegisteredMsg, TRUE, (LPARAM)&param);
    UnhookWindowsHookEx(hook);

    return param.result;
}

void SubclassTaskbarWindow(HWND hWnd) {
    if (SetWindowSubclassFromAnyThread(hWnd, TaskbarWindowSubclassProc, 0, 0)) {
        Wh_Log(L"Subclassed taskbar window: %p", hWnd);
    } else {
        Wh_Log(L"Failed to subclass taskbar window: %p", hWnd);
    }
}

void UnsubclassTaskbarWindow(HWND hWnd) {
    SendMessage(hWnd, g_subclassRegisteredMsg, FALSE, 0);
}

bool g_inputSiteProcHooked = false;

void HandleIdentifiedInputSiteWindow(HWND hWnd) {
    if (!g_dwTaskbarThreadId || GetWindowThreadProcessId(hWnd, nullptr) != g_dwTaskbarThreadId) {
        return;
    }

    HWND hParentWnd = GetParent(hWnd);
    WCHAR szClassName[64];
    if (!hParentWnd || !GetClassName(hParentWnd, szClassName, ARRAYSIZE(szClassName)) ||
        _wcsicmp(szClassName, L"Windows.UI.Composition.DesktopWindowContentBridge") != 0) {
        return;
    }

    hParentWnd = GetParent(hParentWnd);
    if (!hParentWnd || !IsTaskbarWindow(hParentWnd)) {
        return;
    }

    auto wndProc = (WNDPROC)GetWindowLongPtr(hWnd, GWLP_WNDPROC);
    Wh_SetFunctionHook((void*)wndProc, (void*)InputSiteWindowProc_Hook, (void**)&InputSiteWindowProc_Original);
    Wh_ApplyHookOperations();

    Wh_Log(L"Hooked InputSite wndproc: %p", wndProc);
    g_inputSiteProcHooked = true;
}

void HandleIdentifiedTaskbarWindow(HWND hWnd) {
    g_hTaskbarWnd = hWnd;
    g_dwTaskbarThreadId = GetWindowThreadProcessId(hWnd, nullptr);

    Wh_Log(L"Found taskbar window: %p (thread %u)", hWnd, g_dwTaskbarThreadId);

    SubclassTaskbarWindow(hWnd);

    for (HWND hSecondaryWnd : g_secondaryTaskbarWindows) {
        SubclassTaskbarWindow(hSecondaryWnd);
    }

    // Hook Windows 11 XAML input site
    if (!g_inputSiteProcHooked) {
        HWND hXamlIslandWnd = FindWindowEx(
            hWnd, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr);
        if (hXamlIslandWnd) {
            HWND hInputSiteWnd = FindWindowEx(
                hXamlIslandWnd, nullptr, L"Windows.UI.Input.InputSite.WindowClass", nullptr);
            if (hInputSiteWnd) {
                HandleIdentifiedInputSiteWindow(hInputSiteWnd);
            }
        }
    }
}

void HandleIdentifiedSecondaryTaskbarWindow(HWND hWnd) {
    if (!g_dwTaskbarThreadId || GetWindowThreadProcessId(hWnd, nullptr) != g_dwTaskbarThreadId) {
        return;
    }

    g_secondaryTaskbarWindows.insert(hWnd);
    SubclassTaskbarWindow(hWnd);
    Wh_Log(L"Found secondary taskbar: %p", hWnd);
}

HWND FindCurrentProcessTaskbarWindows(std::unordered_set<HWND>* secondaryTaskbarWindows) {
    struct ENUM_WINDOWS_PARAM {
        HWND* hWnd;
        std::unordered_set<HWND>* secondaryTaskbarWindows;
    };

    HWND hWnd = nullptr;
    ENUM_WINDOWS_PARAM param = {&hWnd, secondaryTaskbarWindows};

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            auto& param = *(ENUM_WINDOWS_PARAM*)lParam;

            DWORD dwProcessId = 0;
            if (!GetWindowThreadProcessId(hWnd, &dwProcessId) ||
                dwProcessId != GetCurrentProcessId()) {
                return TRUE;
            }

            WCHAR szClassName[32];
            if (GetClassName(hWnd, szClassName, ARRAYSIZE(szClassName)) == 0) {
                return TRUE;
            }

            if (_wcsicmp(szClassName, L"Shell_TrayWnd") == 0) {
                *param.hWnd = hWnd;
            } else if (_wcsicmp(szClassName, L"Shell_SecondaryTrayWnd") == 0) {
                param.secondaryTaskbarWindows->insert(hWnd);
            }

            return TRUE;
        },
        (LPARAM)&param
    );

    return hWnd;
}

// ============================================================================
// CreateWindowExW Hook (for dynamic taskbar creation)
// ============================================================================

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Original;

HWND WINAPI CreateWindowExW_Hook(
    DWORD dwExStyle,
    LPCWSTR lpClassName,
    LPCWSTR lpWindowName,
    DWORD dwStyle,
    int X,
    int Y,
    int nWidth,
    int nHeight,
    HWND hWndParent,
    HMENU hMenu,
    HINSTANCE hInstance,
    LPVOID lpParam
) {
    HWND hWnd = CreateWindowExW_Original(
        dwExStyle, lpClassName, lpWindowName, dwStyle,
        X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam
    );

    if (!hWnd) return hWnd;

    BOOL bTextualClassName = ((ULONG_PTR)lpClassName & ~(ULONG_PTR)0xffff) != 0;

    if (bTextualClassName) {
        if (_wcsicmp(lpClassName, L"Shell_TrayWnd") == 0) {
            Wh_Log(L"Taskbar window created: %p", hWnd);
            HandleIdentifiedTaskbarWindow(hWnd);
        } else if (_wcsicmp(lpClassName, L"Shell_SecondaryTrayWnd") == 0) {
            Wh_Log(L"Secondary taskbar window created: %p", hWnd);
            HandleIdentifiedSecondaryTaskbarWindow(hWnd);
        } else if (_wcsicmp(lpClassName, L"Windows.UI.Input.InputSite.WindowClass") == 0) {
            if (!g_inputSiteProcHooked) {
                HandleIdentifiedInputSiteWindow(hWnd);
            }
        }
    }

    return hWnd;
}

// ============================================================================
// Windhawk Mod Functions
// ============================================================================

void LoadSettings() {
    g_settings.showNotification = Wh_GetIntSetting(L"showNotification");
    g_settings.notificationDuration = Wh_GetIntSetting(L"notificationDuration");
    if (g_settings.notificationDuration <= 0) {
        g_settings.notificationDuration = 1500;
    }
    Wh_Log(L"Settings: showNotification=%d, duration=%d",
           g_settings.showNotification, g_settings.notificationDuration);
}

BOOL Wh_ModInit() {
    Wh_Log(L"=== Audio Output Device Switcher initializing ===");

    // Initialize COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        Wh_Log(L"Failed to initialize COM: 0x%08X", hr);
    }

    LoadSettings();

    // List available devices on startup
    auto devices = GetAudioOutputDevices();
    Wh_Log(L"Available audio devices: %zu", devices.size());

    // Hook CreateWindowExW for dynamic window creation
    Wh_SetFunctionHook((void*)CreateWindowExW, (void*)CreateWindowExW_Hook, (void**)&CreateWindowExW_Original);

    Wh_Log(L"Hooks installed, waiting for taskbar...");
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L"Wh_ModAfterInit - looking for existing taskbar windows...");

    // Find existing taskbar windows
    WNDCLASS wndclass;
    if (GetClassInfo(GetModuleHandle(nullptr), L"Shell_TrayWnd", &wndclass)) {
        HWND hWnd = FindCurrentProcessTaskbarWindows(&g_secondaryTaskbarWindows);
        if (hWnd) {
            HandleIdentifiedTaskbarWindow(hWnd);
        } else {
            Wh_Log(L"No taskbar window found yet");
        }
    }

    Wh_Log(L"=== Ready! Hold Ctrl and scroll over the taskbar to switch audio devices ===");
}

void Wh_ModUninit() {
    Wh_Log(L"=== Audio Output Device Switcher uninitializing ===");

    if (g_hTaskbarWnd) {
        UnsubclassTaskbarWindow(g_hTaskbarWnd);
        for (HWND hSecondaryWnd : g_secondaryTaskbarWindows) {
            UnsubclassTaskbarWindow(hSecondaryWnd);
        }
    }

    if (g_popupWnd) {
        DestroyWindow(g_popupWnd);
        g_popupWnd = nullptr;
    }

    if (g_popupClassRegistered) {
        UnregisterClassW(POPUP_CLASS_NAME, GetModuleHandle(nullptr));
        g_popupClassRegistered = false;
    }

    CoUninitialize();
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"Settings changed, reloading...");
    LoadSettings();
}
