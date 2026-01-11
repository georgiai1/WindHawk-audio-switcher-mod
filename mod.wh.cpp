// ==WindhawkMod==
// @id              audio-scroll-switcher
// @name            Audio Output Device Switcher
// @description     Ctrl+Scroll on the volume icon to switch audio devices
// @version         1.0
// @author          You
// @github          https://github.com/you
// @include         explorer.exe
// @compilerOptions -lole32 -loleaut32 -lpropsys -lgdi32 -luser32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Audio Output Device Switcher

Switch between audio output devices by holding **Ctrl** and scrolling the mouse
wheel over the volume/sound icon in the system tray.

## Usage
1. Compile the mod with Ctrl+B
2. Hover over the volume icon in the system tray
3. Hold Ctrl and scroll up/down to switch audio devices
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

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <vector>
#include <string>

// Define PKEY_Device_FriendlyName manually
static const PROPERTYKEY PKEY_Device_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } },
    14
};

// IPolicyConfig interface
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

// Global variables
HHOOK g_mouseHook = nullptr;
HWND g_popupWnd = nullptr;
std::wstring g_popupText;
const wchar_t* POPUP_CLASS_NAME = L"AudioSwitcherPopup";

struct AudioDevice {
    std::wstring id;
    std::wstring name;
};

std::vector<AudioDevice> GetAudioOutputDevices() {
    std::vector<AudioDevice> devices;
    IMMDeviceEnumerator* pEnumerator = nullptr;

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator))) return devices;

    IMMDeviceCollection* pCollection = nullptr;
    if (FAILED(pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection))) {
        pEnumerator->Release();
        return devices;
    }

    UINT count = 0;
    pCollection->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice* pDevice = nullptr;
        if (SUCCEEDED(pCollection->Item(i, &pDevice))) {
            LPWSTR deviceId = nullptr;
            if (SUCCEEDED(pDevice->GetId(&deviceId))) {
                AudioDevice device;
                device.id = deviceId;
                CoTaskMemFree(deviceId);

                IPropertyStore* pProps = nullptr;
                if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR) {
                        device.name = varName.pwszVal;
                    } else {
                        device.name = L"Unknown Device";
                    }
                    PropVariantClear(&varName);
                    pProps->Release();
                }
                devices.push_back(device);
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
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator))) return deviceId;

    IMMDevice* pDevice = nullptr;
    if (SUCCEEDED(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(pDevice->GetId(&id))) {
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

    if (FAILED(hr)) {
        Wh_Log(L"Failed to create PolicyConfig: 0x%08X", hr);
        return false;
    }

    hr = pPolicyConfig->SetDefaultEndpoint(deviceId.c_str(), eConsole);
    pPolicyConfig->SetDefaultEndpoint(deviceId.c_str(), eMultimedia);
    pPolicyConfig->SetDefaultEndpoint(deviceId.c_str(), eCommunications);
    pPolicyConfig->Release();

    return SUCCEEDED(hr);
}

LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            HBRUSH bgBrush = CreateSolidBrush(RGB(40, 40, 40));
            FillRect(hdc, &rc, bgBrush);
            DeleteObject(bgBrush);

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
    static bool registered = false;
    if (registered) return;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = PopupWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = POPUP_CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    registered = true;
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

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int popupWidth = 320;
    int popupHeight = 40;
    int x = (screenWidth - popupWidth) / 2;
    int y = screenHeight - 120;

    g_popupWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        POPUP_CLASS_NAME, L"", WS_POPUP,
        x, y, popupWidth, popupHeight,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr
    );

    if (g_popupWnd) {
        SetLayeredWindowAttributes(g_popupWnd, 0, 240, LWA_ALPHA);
        ShowWindow(g_popupWnd, SW_SHOWNOACTIVATE);
        UpdateWindow(g_popupWnd);
        SetTimer(g_popupWnd, 1, g_settings.notificationDuration, nullptr);
    }
}

void SwitchAudioDevice(int direction) {
    auto devices = GetAudioOutputDevices();
    if (devices.size() < 2) {
        Wh_Log(L"Not enough devices to switch");
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

    Wh_Log(L"Switching from %d to %d", currentIndex, nextIndex);

    if (SetDefaultAudioDevice(devices[nextIndex].id)) {
        ShowNotification(devices[nextIndex].name);
    }
}

// Check if mouse is over the volume icon area (system tray notification area)
bool IsOverVolumeIcon(POINT pt) {
    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTray) return false;

    HWND hNotify = FindWindowExW(hTray, nullptr, L"TrayNotifyWnd", nullptr);
    if (!hNotify) return false;

    // Windows 10/11 system tray area
    HWND hSysPager = FindWindowExW(hNotify, nullptr, L"SysPager", nullptr);
    if (hSysPager) {
        HWND hToolbar = FindWindowExW(hSysPager, nullptr, L"ToolbarWindow32", nullptr);
        if (hToolbar) {
            RECT rc;
            GetWindowRect(hToolbar, &rc);
            if (PtInRect(&rc, pt)) return true;
        }
    }

    // Windows 11 corner icons (where volume icon lives)
    HWND hCorner = FindWindowExW(hNotify, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr);
    if (hCorner) {
        RECT rc;
        GetWindowRect(hCorner, &rc);
        if (PtInRect(&rc, pt)) return true;
    }

    // Main notification area
    RECT rcNotify;
    GetWindowRect(hNotify, &rcNotify);
    if (PtInRect(&rcNotify, pt)) return true;

    return false;
}

LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_MOUSEWHEEL) {
        MSLLHOOKSTRUCT* pMouse = (MSLLHOOKSTRUCT*)lParam;

        // Check if Ctrl is held and mouse is over volume icon
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && IsOverVolumeIcon(pMouse->pt)) {
            short delta = HIWORD(pMouse->mouseData);
            int direction = (delta > 0) ? -1 : 1;

            Wh_Log(L"Ctrl+Scroll on volume icon, delta=%d", delta);
            SwitchAudioDevice(direction);

            return 1; // Block the scroll
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

void LoadSettings() {
    g_settings.showNotification = Wh_GetIntSetting(L"showNotification");
    g_settings.notificationDuration = Wh_GetIntSetting(L"notificationDuration");
    if (g_settings.notificationDuration == 0) g_settings.notificationDuration = 1500;
}

BOOL Wh_ModInit() {
    Wh_Log(L"Audio Device Switcher initializing...");

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadSettings();

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandle(nullptr), 0);
    if (!g_mouseHook) {
        Wh_Log(L"Failed to install mouse hook: %d", GetLastError());
        return FALSE;
    }

    auto devices = GetAudioOutputDevices();
    Wh_Log(L"Found %d audio devices:", (int)devices.size());
    for (const auto& dev : devices) {
        Wh_Log(L"  - %s", dev.name.c_str());
    }

    Wh_Log(L"Ready! Hold Ctrl and scroll over the volume icon to switch devices.");
    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L"Uninitializing...");

    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }

    if (g_popupWnd) {
        DestroyWindow(g_popupWnd);
        g_popupWnd = nullptr;
    }

    CoUninitialize();
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"Settings changed");
    LoadSettings();
}
