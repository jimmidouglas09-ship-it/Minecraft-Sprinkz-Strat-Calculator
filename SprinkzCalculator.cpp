#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <gdiplus.h>
#include <memory>
#include <string>
#include <sstream>
#include <fstream>
#include <commctrl.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")

using namespace Gdiplus;
using namespace std;

// Constants
const int WM_HOTKEY_PRESSED = WM_USER + 1;
const int HOTKEY_ID = 1;
const wchar_t* CONFIG_FILE = L"chunk_finder_config.txt";

// Control IDs for options window
#define IDC_HOTKEY_EDIT         3001
#define IDC_MODIFIER_COMBO      3002
#define IDC_KEY_COMBO           3003
#define IDC_SAVE_BTN           3004
#define IDC_DEFAULTS_BTN       3005
#define IDC_CLOSE_BTN          3006

struct Vec3 {
    int x, y, z;
};

struct Config {
    UINT hotkeyVK = VK_F8;          // Default F8
    UINT hotkeyMod = MOD_NOREPEAT;   // No modifier by default
    bool overlayVisible = true;
    int overlayX = -1;               // -1 means use default position
    int overlayY = -1;
};

// Global variables for options window
static HWND g_hOptionsWnd = NULL;
static bool g_isCapturingKey = false;
static WNDPROC g_originalEditProc = NULL;

class ChunkCoordinateFinder {
private:
    HWND overlayWindow;
    HWND settingsWindow;
    HWND minecraftWindow;
    HINSTANCE hInstance;
    ULONG_PTR gdiplusToken;

    Vec3 lastCoordinates;
    Vec3 nearestChunkCoord;
    bool coordinatesFound;
    Config config;
    bool isDragging;
    POINT dragOffset;

    static ChunkCoordinateFinder* instance;

public:
    ChunkCoordinateFinder(HINSTANCE hInst) : hInstance(hInst) {
        instance = this;

        // Initialize GDI+
        GdiplusStartupInput gdiplusStartupInput;
        GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

        overlayWindow = nullptr;
        settingsWindow = nullptr;
        minecraftWindow = nullptr;
        coordinatesFound = false;
        lastCoordinates = { 0, 0, 0 };
        nearestChunkCoord = { 0, 0, 0 };
        isDragging = false;

        LoadConfig();
    }

    ~ChunkCoordinateFinder() {
        SaveConfig();
        UnregisterHotKey(overlayWindow, HOTKEY_ID);
        GdiplusShutdown(gdiplusToken);
    }

    void LoadConfig() {
        ifstream file(CONFIG_FILE);
        if (file.is_open()) {
            file >> config.hotkeyVK >> config.hotkeyMod >> config.overlayVisible >> config.overlayX >> config.overlayY;
            file.close();
        }
    }

    void SaveConfig() {
        ofstream file(CONFIG_FILE);
        if (file.is_open()) {
            RECT rect;
            GetWindowRect(overlayWindow, &rect);
            config.overlayX = rect.left;
            config.overlayY = rect.top;

            file << config.hotkeyVK << " " << config.hotkeyMod << " " << config.overlayVisible
                << " " << config.overlayX << " " << config.overlayY;
            file.close();
        }
    }

    // Static method to get key name from virtual key code
    static wstring GetKeyName(UINT vkCode) {
        wchar_t keyName[256] = L"Unknown";

        // Special cases for common keys
        switch (vkCode) {
        case VK_F1: return L"F1";
        case VK_F2: return L"F2";
        case VK_F3: return L"F3";
        case VK_F4: return L"F4";
        case VK_F5: return L"F5";
        case VK_F6: return L"F6";
        case VK_F7: return L"F7";
        case VK_F8: return L"F8";
        case VK_F9: return L"F9";
        case VK_F10: return L"F10";
        case VK_F11: return L"F11";
        case VK_F12: return L"F12";
        case VK_SPACE: return L"Space";
        case VK_RETURN: return L"Enter";
        case VK_ESCAPE: return L"Escape";
        case VK_TAB: return L"Tab";
        case VK_BACK: return L"Backspace";
        case VK_DELETE: return L"Delete";
        case VK_INSERT: return L"Insert";
        case VK_HOME: return L"Home";
        case VK_END: return L"End";
        case VK_PRIOR: return L"Page Up";
        case VK_NEXT: return L"Page Down";
        default:
            // Use system function for other keys
            UINT scanCode = MapVirtualKeyW(vkCode, MAPVK_VK_TO_VSC);
            if (GetKeyNameTextW(scanCode << 16, keyName, 256)) {
                return wstring(keyName);
            }
            return L"Unknown";
        }
    }

    unique_ptr<Bitmap> BitmapFromHWND(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) return nullptr;

        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        RECT rc;
        GetWindowRect(hwnd, &rc);
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;

        if (width <= 0 || height <= 0) return nullptr;

        HDC hdc = GetDC(hwnd);
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP hBitmap = CreateCompatibleBitmap(hdc, width, height);
        HBITMAP hOldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);
        PrintWindow(hwnd, memDC, PW_RENDERFULLCONTENT);
        auto pBitmap = make_unique<Bitmap>(hBitmap, nullptr);
        SelectObject(memDC, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(memDC);
        ReleaseDC(hwnd, hdc);
        return pBitmap;
    }

    int GetShownCoordinates(HWND hwnd, Vec3* coordinates) {
        auto pBitmap = BitmapFromHWND(hwnd);
        if (!pBitmap) return 0;

        int width = pBitmap->GetWidth();
        int height = pBitmap->GetHeight();
        int searchWidth = max(width / 3, min(125, width));
        int searchHeight = height / 3;
        BitmapData bitmapData;
        Rect rect(0, 0, searchWidth, searchHeight);

        if (pBitmap->LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &bitmapData) != Ok) {
            return 0;
        }

        int startTextX = 0, startTextY = 0, streak = 0;
        int stride = bitmapData.Stride / sizeof(ARGB);
        ARGB* pixels = static_cast<ARGB*>(bitmapData.Scan0);

        for (int y = 30; y < searchHeight; y++) {
            for (int x = 8; x < searchWidth; x++) {
                if (pixels[y * stride + x] == 0xFFFFFFFF) {
                    if (!startTextX) { startTextX = x; startTextY = y; }
                    streak++;
                }
                else if (streak < 4) streak = 0;
                else if (streak >= 4) break;
            }
            if (streak >= 4) break;
        }

        if (streak < 4) {
            pBitmap->UnlockBits(&bitmapData);
            return 0;
        }

        int scale = streak / 4;
        startTextX += 44 * scale;
        int coords[3] = { 0, 0, 0 };
        int index = 0;
        bool isSigned = false;

        while (startTextX < searchWidth) {
            unsigned int columnMask = 0;
            for (int dy = 0; dy < 7; dy++) {
                columnMask <<= 1;
                if (pixels[(startTextY + dy * scale) * stride + startTextX] == 0xFFFFFFFF)
                    columnMask |= 1;
            }

            int digit = -1;
            switch (columnMask) {
            case 0b0111110: digit = 0; break;
            case 0b0000001: digit = 1; break;
            case 0b0100011: digit = 2; break;
            case 0b0100010: digit = 3; break;
            case 0b0001100: digit = 4; break;
            case 0b1110010: digit = 5; break;
            case 0b0011110: digit = 6; break;
            case 0b1100000: digit = 7; break;
            case 0b0110110: digit = 8; break;
            case 0b0110000: digit = 9; break;
            case 0b0001000: isSigned = true; break;
            case 0b0000011:
                if (isSigned) coords[index] *= -1;
                if (++index > 2) break;
                isSigned = false;
                break;
            default:
                if (index >= 2) break;
                if (isSigned) coords[index] *= -1;
                break;
            }
            if (digit != -1)
                coords[index] = coords[index] * 10 + digit;
            startTextX += 6 * scale;
        }

        if (isSigned && index <= 2) {
            coords[index] *= -1;
        }

        pBitmap->UnlockBits(&bitmapData);
        coordinates->x = coords[0];
        coordinates->y = coords[1];
        coordinates->z = coords[2];
        return 1;
    }

    void findMinecraftWindow() {
        minecraftWindow = FindWindowA("LWJGL", nullptr);
        if (!minecraftWindow) {
            minecraftWindow = FindWindowA(nullptr, "Minecraft");
        }
    }

    Vec3 calculateNearest4x4Coordinate(const Vec3& playerPos) {
        Vec3 nearest;

        auto roundToNearestChunkCoord = [](int coord) -> int {
            int chunkIndex = (coord + 8) / 16;
            return chunkIndex * 16 + 4;
            };

        nearest.x = roundToNearestChunkCoord(playerPos.x);
        nearest.y = playerPos.y;
        nearest.z = roundToNearestChunkCoord(playerPos.z);

        return nearest;
    }

    void updateCoordinates() {
        findMinecraftWindow();

        if (!minecraftWindow || !IsWindow(minecraftWindow)) {
            coordinatesFound = false;
            return;
        }

        Vec3 currentCoords;
        if (GetShownCoordinates(minecraftWindow, &currentCoords)) {
            lastCoordinates = currentCoords;
            nearestChunkCoord = calculateNearest4x4Coordinate(currentCoords);
            coordinatesFound = true;
            InvalidateRect(overlayWindow, nullptr, TRUE);
        }
        else {
            coordinatesFound = false;
        }
    }

    void updateHotkey() {
        UnregisterHotKey(overlayWindow, HOTKEY_ID);
        RegisterHotKey(overlayWindow, HOTKEY_ID, config.hotkeyMod, config.hotkeyVK);
    }

    wstring GetHotkeyString() {
        wstring result;
        if (config.hotkeyMod & MOD_CONTROL) result += L"Ctrl+";
        if (config.hotkeyMod & MOD_ALT) result += L"Alt+";
        if (config.hotkeyMod & MOD_SHIFT) result += L"Shift+";

        result += GetKeyName(config.hotkeyVK);
        return result;
    }

    // Custom edit control procedure for key capture
    static LRESULT CALLBACK EditKeyCapture(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            if (g_isCapturingKey) {
                int vkCode = (int)wParam;

                // Don't capture certain keys for UI navigation
                if (vkCode == VK_TAB || vkCode == VK_RETURN || vkCode == VK_ESCAPE) {
                    g_isCapturingKey = false;
                    SetWindowTextW(hwnd, L"Click to set key");
                    return CallWindowProc(g_originalEditProc, hwnd, msg, wParam, lParam);
                }

                // Set the key name
                wstring keyName = instance->GetKeyName(vkCode);
                SetWindowTextW(hwnd, keyName.c_str());

                // Update the config
                instance->config.hotkeyVK = vkCode;
                instance->updateHotkey();

                g_isCapturingKey = false;
                return 0; // Consume the key press
            }
        }
        else if (msg == WM_LBUTTONDOWN) {
            if (!g_isCapturingKey) {
                g_isCapturingKey = true;
                SetWindowTextW(hwnd, L"Press a key...");
                SetFocus(hwnd);
            }
        }
        else if (msg == WM_KILLFOCUS) {
            if (g_isCapturingKey) {
                g_isCapturingKey = false;
                wstring keyName = instance->GetKeyName(instance->config.hotkeyVK);
                SetWindowTextW(hwnd, keyName.c_str());
            }
        }

        return CallWindowProc(g_originalEditProc, hwnd, msg, wParam, lParam);
    }

    // Options window procedure
    static LRESULT CALLBACK OptionsWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            switch (wmId) {
            case IDC_MODIFIER_COMBO:
                if (HIWORD(wParam) == CBN_SELCHANGE) {
                    HWND hCombo = GetDlgItem(hwnd, IDC_MODIFIER_COMBO);
                    int sel = SendMessage(hCombo, CB_GETCURSEL, 0, 0);
                    switch (sel) {
                    case 0: instance->config.hotkeyMod = MOD_NOREPEAT; break;
                    case 1: instance->config.hotkeyMod = MOD_CONTROL | MOD_NOREPEAT; break;
                    case 2: instance->config.hotkeyMod = MOD_ALT | MOD_NOREPEAT; break;
                    case 3: instance->config.hotkeyMod = MOD_SHIFT | MOD_NOREPEAT; break;
                    }
                    instance->updateHotkey();
                }
                break;

            case IDC_SAVE_BTN:
                instance->SaveConfig();
                MessageBoxW(hwnd, L"Settings saved successfully!", L"Settings", MB_OK | MB_ICONINFORMATION);
                break;

            case IDC_DEFAULTS_BTN:
                instance->config.hotkeyVK = VK_F8;
                instance->config.hotkeyMod = MOD_NOREPEAT;
                instance->updateHotkey();
                instance->updateOptionsControls();
                break;

            case IDC_CLOSE_BTN:
                DestroyWindow(hwnd);
                break;
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g_hOptionsWnd = NULL;
            g_isCapturingKey = false; // Reset capture state
            break;
        }

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    void updateOptionsControls() {
        if (!g_hOptionsWnd) return;

        // Update hotkey edit
        wstring keyName = GetKeyName(config.hotkeyVK);
        SetWindowTextW(GetDlgItem(g_hOptionsWnd, IDC_HOTKEY_EDIT), keyName.c_str());

        // Update modifier combo
        HWND hCombo = GetDlgItem(g_hOptionsWnd, IDC_MODIFIER_COMBO);
        int sel = 0;
        if (config.hotkeyMod & MOD_CONTROL) sel = 1;
        else if (config.hotkeyMod & MOD_ALT) sel = 2;
        else if (config.hotkeyMod & MOD_SHIFT) sel = 3;
        SendMessage(hCombo, CB_SETCURSEL, sel, 0);
    }

    void createOptionsWindow() {
        // Reset global state
        g_hOptionsWnd = NULL;
        g_isCapturingKey = false;

        // Register window class
        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASSW wc = { 0 };
            wc.lpfnWndProc = OptionsWindowProc;
            wc.hInstance = hInstance;
            wc.lpszClassName = L"ChunkFinderOptionsWindow";
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            RegisterClassW(&wc);
            classRegistered = true;
        }

        // Calculate center position
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        int windowWidth = 400;
        int windowHeight = 250;
        int x = (screenWidth - windowWidth) / 2;
        int y = (screenHeight - windowHeight) / 2;

        // Create window at center of screen
        g_hOptionsWnd = CreateWindowExW(
            WS_EX_TOPMOST,  // Make it topmost to ensure visibility
            L"ChunkFinderOptionsWindow",
            L"Chunk Finder - Hotkey Settings",
            WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
            x, y, windowWidth, windowHeight,
            nullptr, nullptr, hInstance, nullptr
        );

        if (!g_hOptionsWnd) {
            MessageBoxW(nullptr, L"Failed to create options window!", L"Error", MB_OK | MB_ICONERROR);
            return;
        }

        int yPos = 20;

        // Title
        CreateWindowW(L"STATIC", L"Configure your hotkey settings:",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            20, yPos, 350, 20, g_hOptionsWnd, nullptr, hInstance, nullptr);
        yPos += 35;

        // Modifier label and combo
        CreateWindowW(L"STATIC", L"Modifier key:",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            20, yPos, 100, 20, g_hOptionsWnd, nullptr, hInstance, nullptr);
        HWND hModifierCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
            130, yPos - 2, 120, 100, g_hOptionsWnd, (HMENU)IDC_MODIFIER_COMBO, hInstance, nullptr);

        SendMessageW(hModifierCombo, CB_ADDSTRING, 0, (LPARAM)L"None");
        SendMessageW(hModifierCombo, CB_ADDSTRING, 0, (LPARAM)L"Ctrl");
        SendMessageW(hModifierCombo, CB_ADDSTRING, 0, (LPARAM)L"Alt");
        SendMessageW(hModifierCombo, CB_ADDSTRING, 0, (LPARAM)L"Shift");
        yPos += 35;

        // Key label and edit
        CreateWindowW(L"STATIC", L"Key:",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            20, yPos, 100, 20, g_hOptionsWnd, nullptr, hInstance, nullptr);
        HWND hKeyEdit = CreateWindowW(L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_READONLY | ES_CENTER,
            130, yPos - 2, 120, 24, g_hOptionsWnd, (HMENU)IDC_HOTKEY_EDIT, hInstance, nullptr);

        // Subclass the edit control
        g_originalEditProc = (WNDPROC)SetWindowLongPtrW(hKeyEdit, GWLP_WNDPROC, (LONG_PTR)EditKeyCapture);
        yPos += 35;

        // Instructions
        CreateWindowW(L"STATIC", L"Click on the key field and press a key to set it",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            20, yPos, 350, 20, g_hOptionsWnd, nullptr, hInstance, nullptr);
        yPos += 40;

        // Buttons
        CreateWindowW(L"BUTTON", L"Save Settings",
            WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            20, yPos, 100, 30, g_hOptionsWnd, (HMENU)IDC_SAVE_BTN, hInstance, nullptr);
        CreateWindowW(L"BUTTON", L"Defaults",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            140, yPos, 100, 30, g_hOptionsWnd, (HMENU)IDC_DEFAULTS_BTN, hInstance, nullptr);
        CreateWindowW(L"BUTTON", L"Close",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            260, yPos, 100, 30, g_hOptionsWnd, (HMENU)IDC_CLOSE_BTN, hInstance, nullptr);
    }

    void ShowOptionsWindow() {
        // Always create a new window instead of reusing
        if (g_hOptionsWnd && IsWindow(g_hOptionsWnd)) {
            DestroyWindow(g_hOptionsWnd);
            g_hOptionsWnd = NULL;
        }

        createOptionsWindow();

        if (g_hOptionsWnd) {
            // Force the window to be visible and on top
            ShowWindow(g_hOptionsWnd, SW_SHOW);
            UpdateWindow(g_hOptionsWnd);
            SetForegroundWindow(g_hOptionsWnd);
            BringWindowToTop(g_hOptionsWnd);
            SetActiveWindow(g_hOptionsWnd);

            // Update controls after showing
            updateOptionsControls();

            // Force a redraw
            InvalidateRect(g_hOptionsWnd, nullptr, TRUE);
        }
    }

    static LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        if (!instance) return DefWindowProc(hwnd, uMsg, wParam, lParam);
        return instance->HandleOverlayMessage(hwnd, uMsg, wParam, lParam);
    }

    LRESULT HandleOverlayMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        switch (uMsg) {
        case WM_HOTKEY:
            if (wParam == HOTKEY_ID) {
                updateCoordinates();
            }
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // Create memory DC for double buffering
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, clientRect.right, clientRect.bottom);
            HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

            // Background
            HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(memDC, &clientRect, bgBrush);
            DeleteObject(bgBrush);

            // Text
            SetTextColor(memDC, RGB(255, 255, 255));
            SetBkMode(memDC, TRANSPARENT);

            wstring text;
            if (coordinatesFound) {
                wstringstream ss;
                ss << L"Player: " << lastCoordinates.x << L", " << lastCoordinates.y << L", " << lastCoordinates.z << L"\n";
                ss << L"4x4: " << nearestChunkCoord.x << L", " << nearestChunkCoord.y << L", " << nearestChunkCoord.z << L"\n";
                int distX = abs(lastCoordinates.x - nearestChunkCoord.x);
                int distZ = abs(lastCoordinates.z - nearestChunkCoord.z);
                double totalDist = sqrt(distX * distX + distZ * distZ);
                ss << L"Dist: " << (int)totalDist << L" blocks";
                text = ss.str();
            }
            else {
                text = L"Press " + GetHotkeyString() + L" to read coords\nMake sure to be decently near to dig spot\nRight-click for settings";
            }

            RECT textRect = clientRect;
            textRect.left += 5;
            textRect.top += 5;
            DrawTextW(memDC, text.c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK);

            // Copy to main DC
            BitBlt(hdc, 0, 0, clientRect.right, clientRect.bottom, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBitmap);
            DeleteObject(memBitmap);
            DeleteDC(memDC);

            EndPaint(hwnd, &ps);
            break;
        }

        case WM_LBUTTONDOWN: {
            isDragging = true;
            POINT cursorPos;
            GetCursorPos(&cursorPos);
            RECT windowRect;
            GetWindowRect(hwnd, &windowRect);
            dragOffset.x = cursorPos.x - windowRect.left;
            dragOffset.y = cursorPos.y - windowRect.top;
            SetCapture(hwnd);
            break;
        }

        case WM_LBUTTONUP:
            if (isDragging) {
                isDragging = false;
                ReleaseCapture();
                SaveConfig();
            }
            break;

        case WM_MOUSEMOVE:
            if (isDragging) {
                POINT cursorPos;
                GetCursorPos(&cursorPos);
                SetWindowPos(hwnd, nullptr,
                    cursorPos.x - dragOffset.x,
                    cursorPos.y - dragOffset.y,
                    0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
            break;

        case WM_RBUTTONUP:
            ShowOptionsWindow();
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
        return 0;
    }

    bool CreateOverlay() {
        const wchar_t* className = L"ChunkFinderOverlay";

        WNDCLASSW wc = {};
        wc.lpfnWndProc = OverlayWindowProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = className;
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

        RegisterClassW(&wc);

        // Ensure saved position is on-screen
        RECT virtualScreen;
        virtualScreen.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        virtualScreen.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        virtualScreen.right = virtualScreen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        virtualScreen.bottom = virtualScreen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

        int x = config.overlayX;
        int y = config.overlayY;

        // Check if x, y are valid
        if (x < virtualScreen.left || x > virtualScreen.right - 100 ||
            y < virtualScreen.top || y > virtualScreen.bottom - 50) {
            // Use default position
            x = GetSystemMetrics(SM_CXSCREEN) - 220;
            y = 20;
        }

        overlayWindow = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_LAYERED,
            className,
            L"Chunk Finder",
            WS_POPUP | WS_VISIBLE,
            x, y, 200, 80,
            nullptr, nullptr, hInstance, nullptr);


        if (!overlayWindow) return false;

        SetLayeredWindowAttributes(overlayWindow, 0, 220, LWA_ALPHA);

        // Register hotkey
        RegisterHotKey(overlayWindow, HOTKEY_ID, config.hotkeyMod, config.hotkeyVK);

        return true;
    }

    void Run() {
        if (!CreateOverlay()) return;

        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
};

ChunkCoordinateFinder* ChunkCoordinateFinder::instance = nullptr;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    ChunkCoordinateFinder finder(hInstance);
    finder.Run();
    return 0;
}