// main.cpp
// Win32 + DirectShow enumeration (Unicode) + OpenCV capture (CAP_DSHOW)
// Compile with C++17, link with ole32.lib and OpenCV (.\vcpkg install opencv4:x64-windows )

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <atomic>
#include <thread>

#include <dshow.h>   // DirectShow interfaces
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")

#include <opencv2/opencv.hpp>

namespace fs = std::filesystem;

const wchar_t CLASS_NAME[] = L"DSEnumClass";
const int ID_BTN_OPEN = 201;
const int ID_BTN_CLOSE = 202;
const int ID_COMBO = 301;
const int ID_CHECK_SAVE = 302;
const int ID_TIMER_PREVIEW = 401;
const int ID_TIMER_SAVE = 402;

HINSTANCE g_hInst = nullptr;
HWND g_hCombo = nullptr;
cv::VideoCapture g_cap;
cv::Mat g_frame;
std::atomic<bool> g_running{ false };
std::vector<std::wstring> g_devNames; // friendly names shown to user
std::string g_outDir = "captures";

std::string timestampFilename() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#if defined(_MSC_VER)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return ss.str();
}

// Enumerate video capture devices via DirectShow and return friendly names (Unicode)
std::vector<std::wstring> EnumerateVideoDevices() {
    std::vector<std::wstring> result;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool coInit = SUCCEEDED(hr);
    ICreateDevEnum* pDevEnum = nullptr;
    IEnumMoniker* pEnum = nullptr;

    hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pDevEnum));
    if (FAILED(hr) || !pDevEnum) {
        if (coInit) CoUninitialize();
        return result;
    }

    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    if (hr == S_OK && pEnum) {
        IMoniker* pMoniker = nullptr;
        while (pEnum->Next(1, &pMoniker, NULL) == S_OK) {
            IPropertyBag* pPropBag = nullptr;
            hr = pMoniker->BindToStorage(0, 0, IID_PPV_ARGS(&pPropBag));
            if (SUCCEEDED(hr) && pPropBag) {
                VARIANT varName;
                VariantInit(&varName);
                // "FriendlyName" property holds the display name (BSTR, can contain Chinese)
                hr = pPropBag->Read(L"FriendlyName", &varName, 0);
                if (SUCCEEDED(hr) && varName.vt == VT_BSTR) {
                    // convert BSTR (wide) to std::wstring
                    std::wstring name((wchar_t*)varName.bstrVal);
                    result.push_back(name);
                }
                else {
                    result.push_back(L"Unknown Device");
                }
                VariantClear(&varName);
                pPropBag->Release();
            }
            else {
                result.push_back(L"Unknown Device");
            }
            pMoniker->Release();
        }
        pEnum->Release();
    }
    pDevEnum->Release();
    if (coInit) CoUninitialize();
    return result;
}

// Convert cv::Mat (BGR or GRAY) to HBITMAP (32bpp BGRA) for fast painting
HBITMAP MatToHBITMAP(const cv::Mat& mat) {
    if (mat.empty()) return nullptr;
    cv::Mat tmp;
    if (mat.channels() == 3) cv::cvtColor(mat, tmp, cv::COLOR_BGR2BGRA);
    else if (mat.channels() == 1) cv::cvtColor(mat, tmp, cv::COLOR_GRAY2BGRA);
    else tmp = mat;

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = tmp.cols;
    bmi.bmiHeader.biHeight = -tmp.rows; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = GetDC(NULL);
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!hbm || !bits) return nullptr;

    std::memcpy(bits, tmp.data, (size_t)tmp.total() * tmp.elemSize());
    return hbm;
}

void PaintPreview(HWND hwnd, HDC hdc) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    // top controls area
    const int topH = 50;
    RECT previewRc = rc;
    previewRc.top += topH;

    FillRect(hdc, &previewRc, (HBRUSH)(COLOR_WINDOW + 1));

    if (g_frame.empty()) return;

    int pw = previewRc.right - previewRc.left;
    int ph = previewRc.bottom - previewRc.top;
    if (pw <= 0 || ph <= 0) return;

    double fx = double(pw) / g_frame.cols;
    double fy = double(ph) / g_frame.rows;
    double f = std::min(fx, fy);
    int sw = int(g_frame.cols * f);
    int sh = int(g_frame.rows * f);

    cv::Mat resized;
    cv::resize(g_frame, resized, cv::Size(sw, sh));

    HBITMAP hbm = MatToHBITMAP(resized);
    if (!hbm) return;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP old = (HBITMAP)SelectObject(memDC, hbm);

    int x = previewRc.left + (pw - sw) / 2;
    int y = previewRc.top + (ph - sh) / 2;

    BitBlt(hdc, x, y, sw, sh, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, old);
    DeleteObject(hbm);
    DeleteDC(memDC);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        HWND hStartButton = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 10, 80, 22, hwnd, (HMENU)ID_BTN_OPEN, g_hInst, NULL);
        HWND hStopButton = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            100, 10, 80, 22, hwnd, (HMENU)ID_BTN_CLOSE, g_hInst, NULL);
        HWND hSaveLabel = CreateWindowW(L"STATIC", L"Save every second", WS_CHILD | WS_VISIBLE,
            520, 14, 130, 18, hwnd, NULL, g_hInst, NULL);
        HWND hSaveCheck = CreateWindowW(L"BUTTON", NULL, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            650, 10, 20, 20, hwnd, (HMENU)ID_CHECK_SAVE, g_hInst, NULL);

        g_hCombo = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            200, 10, 300, 200, hwnd, (HMENU)ID_COMBO, g_hInst, NULL);

        SendMessageW(hStartButton, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hStopButton, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hSaveLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hSaveCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(g_hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);

        // enumerate devices and fill combo
        g_devNames = EnumerateVideoDevices();
        SendMessageW(g_hCombo, CB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < g_devNames.size(); ++i) {
            SendMessageW(g_hCombo, CB_ADDSTRING, 0, (LPARAM)g_devNames[i].c_str());
        }
        if (!g_devNames.empty()) SendMessageW(g_hCombo, CB_SETCURSEL, 0, 0);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_BTN_OPEN) {
            int sel = (int)SendMessageW(g_hCombo, CB_GETCURSEL, 0, 0);
            if (sel == CB_ERR) sel = 0;
            // open using OpenCV with DirectShow backend; enumerate order matches g_devNames order
            if (g_cap.isOpened()) g_cap.release();
            // use index and CAP_DSHOW to open the corresponding DirectShow device
            if (!g_cap.open(sel, cv::CAP_DSHOW)) {
                MessageBoxW(hwnd, L"Failed to open camera (DirectShow).", L"Error", MB_ICONERROR);
            }
            else {
                g_running = true;
                // preview timer ~33ms (~30fps)
                SetTimer(hwnd, ID_TIMER_PREVIEW, 33, NULL);
                // save timer if checkbox checked
                if (IsDlgButtonChecked(hwnd, ID_CHECK_SAVE) == BST_CHECKED) {
                    SetTimer(hwnd, ID_TIMER_SAVE, 1000, NULL);
                }
                // ensure output dir
                try { if (!fs::exists(g_outDir)) fs::create_directories(g_outDir); }
                catch (...) {}
            }
        }
        else if (id == ID_BTN_CLOSE) {
            g_running = false;
            KillTimer(hwnd, ID_TIMER_PREVIEW);
            KillTimer(hwnd, ID_TIMER_SAVE);
            if (g_cap.isOpened()) g_cap.release();
            g_frame.release();
            InvalidateRect(hwnd, NULL, TRUE);
        }
        else if (id == ID_CHECK_SAVE) {
            if (g_running) {
                if (IsDlgButtonChecked(hwnd, ID_CHECK_SAVE) == BST_CHECKED) {
                    SetTimer(hwnd, ID_TIMER_SAVE, 1000, NULL);
                }
                else {
                    KillTimer(hwnd, ID_TIMER_SAVE);
                }
            }
        }
        break;
    }
    case WM_TIMER: {
        if (wParam == ID_TIMER_PREVIEW && g_running) {
            cv::Mat frame;
            if (g_cap.read(frame) && !frame.empty()) {
                g_frame = frame.clone();
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        else if (wParam == ID_TIMER_SAVE && g_running) {
            cv::Mat frame;
            if (g_cap.read(frame) && !frame.empty()) {
                std::string fname = g_outDir + "\\" + timestampFilename() + ".jpg";
                std::vector<int> p = { cv::IMWRITE_JPEG_QUALITY, 90 };
                cv::imwrite(fname, frame, p);
            }
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintPreview(hwnd, hdc);
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_DESTROY:
        g_running = false;
        KillTimer(hwnd, ID_TIMER_PREVIEW);
        KillTimer(hwnd, ID_TIMER_SAVE);
        if (g_cap.isOpened()) g_cap.release();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    g_hInst = hInstance;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc)) return 0;

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"MySecurityCam",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 666,
        NULL, NULL, hInstance, NULL);
    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}