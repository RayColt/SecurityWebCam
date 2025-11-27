// main.cpp
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <opencv2/opencv.hpp>

namespace fs = std::filesystem;

const char CLASS_NAME[] = "WebcamWinClass";
const int ID_BTN_START = 101;
const int ID_BTN_STOP = 102;
const int ID_TIMER = 201;
const int PREVIEW_MARGIN = 10;

HINSTANCE g_hInst = nullptr;
cv::VideoCapture g_cap;
cv::Mat g_frame;
bool g_running = false;
HWND g_hwndPreview = nullptr;
std::string g_outDir = "captures";

// Helper: timestamp filename
std::string TimestampFilename() 
{
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

// Helper: convert cv::Mat (BGR) to HBITMAP for Win32 painting
HBITMAP MatToHBITMAP(const cv::Mat& mat) 
{
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

    // copy data (mat is BGRA)
    std::memcpy(bits, tmp.data, (size_t)tmp.total() * tmp.elemSize());
    return hbm;
}

// Draw preview into client area
void PaintPreview(HWND hwnd, HDC hdc) 
{
    RECT rc;
    GetClientRect(hwnd, &rc);

    if (g_frame.empty()) {
        // clear background
        FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
        return;
    }

    // compute preview area (leave space for buttons at top)
    RECT previewRc = rc;
    previewRc.top += 50;

    int pw = previewRc.right - previewRc.left - PREVIEW_MARGIN * 2;
    int ph = previewRc.bottom - previewRc.top - PREVIEW_MARGIN * 2;
    if (pw <= 0 || ph <= 0) return;

    // scale frame to fit preview while keeping aspect ratio
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

    int x = previewRc.left + (pw - sw) / 2 + PREVIEW_MARGIN;
    int y = previewRc.top + (ph - sh) / 2 + PREVIEW_MARGIN;

    BitBlt(hdc, x, y, sw, sh, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, old);
    DeleteObject(hbm);
    DeleteDC(memDC);
}

// Window procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) 
    {
    case WM_CREATE:
    {
        CreateWindow(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 10, 80, 28, hwnd, (HMENU)ID_BTN_START, g_hInst, NULL);
        CreateWindow(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            100, 10, 80, 28, hwnd, (HMENU)ID_BTN_STOP, g_hInst, NULL);
        g_hwndPreview = hwnd;
        break;
    }
    case WM_COMMAND: 
    {
        int id = LOWORD(wParam);
        if (id == ID_BTN_START) 
        {
            if (!g_running) {
                // ensure output dir
                try 
                {
                    if (!fs::exists(g_outDir)) fs::create_directories(g_outDir);
                }
                catch (...) {}
                if (!g_cap.isOpened()) 
                {
                    g_cap.open(0);
                }
                if (g_cap.isOpened()) 
                {
                    g_running = true;
                    SetTimer(hwnd, ID_TIMER, 1000, NULL); // 1 second timer
                }
                else 
                {
                    MessageBox(hwnd, L"Failed to open webcam.", L"Error", MB_ICONERROR);
                }
            }
        }
        else if (id == ID_BTN_STOP) 
        {
            if (g_running) 
            {
                g_running = false;
                KillTimer(hwnd, ID_TIMER);
                if (g_cap.isOpened()) g_cap.release();
            }
        }
        break;
    }
    case WM_TIMER: 
    {
        if (wParam == ID_TIMER && g_running) 
        {
            cv::Mat frame;
            if (g_cap.read(frame) && !frame.empty()) 
            {
                g_frame = frame.clone(); // update preview
                // save image
                std::string fname = g_outDir + "/" + TimestampFilename() + ".jpg";
                std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 90 };
                cv::imwrite(fname, frame, params);
            }
            InvalidateRect(hwnd, NULL, FALSE); // request repaint
        }
        break;
    }
    case WM_PAINT: 
    {
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
        if (g_running) {
            KillTimer(hwnd, ID_TIMER);
            g_running = false;
        }
        if (g_cap.isOpened()) g_cap.release();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) 
{
    g_hInst = hInstance;

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"MyCamClassName";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0,
        wc.lpszClassName,
        L"Webcam Capture - Win32 + OpenCV",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 600,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) 
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}