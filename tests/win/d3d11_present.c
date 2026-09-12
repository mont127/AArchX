/*
 * Standalone DXGI swap-chain probe: create a visible window, a D3D11 device and
 * a swap chain for that HWND (the path Chromium's ANGLE takes in Steam's GPU
 * process), clear + Present a run of frames, and report every HRESULT and the
 * per-frame time.
 *
 * Under DXMT (MacNCheese Wine) the swap chain needs the window's Metal layer
 * from winemac.drv via an ExtEscape; a failure there prints "Failed to get
 * metal layer via MACDRV_ESCAPE_GET_SURFACE" on stderr.
 *
 *   x86_64-w64-mingw32-gcc -O1 -o d3d11_present.exe d3d11_present.c \
 *       -ld3d11 -ldxgi -ldxguid -luser32 -lgdi32
 *
 *   d3d11_present.exe [frames] [flip]        single process (window + swap chain)
 *   d3d11_present.exe xproc [frames] [flip]  window in this process, swap chain
 *                                            in a child process (Steam's shape:
 *                                            the browser owns the HWND, the GPU
 *                                            process presents into it)
 *   d3d11_present.exe client <hwnd> [frames] [flip]   (spawned by xproc)
 *
 * The no-D3D mode asks a simpler question - does a plain window keep the height
 * it is given - and the xproc mode grows the window after a few seconds the way
 * Steam's login window does (700x74 -> 700x440).
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <dxgi1_2.h>
#include <d3d11.h>

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

static void modpath(const char *name)
{
    char buf[MAX_PATH] = "";
    HMODULE m = GetModuleHandleA(name);
    if (m) GetModuleFileNameA(m, buf, sizeof buf);
    printf("  %-14s %p %s\n", name, m, buf);
}

static int render_on(HWND hwnd, int frames, int flip, int foreign);

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    int xproc = argc > 1 && !strcmp(argv[1], "xproc");
    if (argc > 1 && !strcmp(argv[1], "gdi")) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"gdi_resize";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        HWND h = CreateWindowExW(0, wc.lpszClassName, L"gdi_resize", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 400, NULL, NULL, wc.hInstance, NULL);
        RECT rc, wr; pump(); Sleep(500); pump();
        GetClientRect(h, &rc); GetWindowRect(h, &wr);
        printf("gdi: created client %ldx%ld window %ld,%ld-%ld,%ld\n", rc.right, rc.bottom, wr.left, wr.top, wr.right, wr.bottom);
        SetWindowPos(h, NULL, 0, 0, 700, 480, SWP_NOMOVE | SWP_NOZORDER);
        GetClientRect(h, &rc); printf("gdi: right after SetWindowPos client %ldx%ld\n", rc.right, rc.bottom);
        for (int i = 0; i < 6; i++) { pump(); Sleep(250); GetClientRect(h, &rc); GetWindowRect(h, &wr);
            printf("gdi: +%dms client %ldx%ld window %ld,%ld-%ld,%ld\n", (i + 1) * 250, rc.right, rc.bottom, wr.left, wr.top, wr.right, wr.bottom); }
        HWND pop = CreateWindowExW(0, wc.lpszClassName, L"popup", WS_POPUP | WS_VISIBLE, 300, 300, 700, 440, NULL, NULL, wc.hInstance, NULL);
        for (int i = 0; i < 4; i++) { pump(); Sleep(250); GetClientRect(pop, &rc); GetWindowRect(pop, &wr);
            printf("gdi: popup +%dms client %ldx%ld window %ld,%ld-%ld,%ld\n", (i + 1) * 250, rc.right, rc.bottom, wr.left, wr.top, wr.right, wr.bottom); }
        printf("gdi: screen %dx%d virtual %d,%d %dx%d\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
               GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "client")) {
        HWND h = (HWND)(ULONG_PTR)strtoull(argv[2], NULL, 16);
        return render_on(h, argc > 3 ? atoi(argv[3]) : 90, argc > 4 && !strcmp(argv[4], "flip"), 1);
    }
    int frames = argc > 1 + xproc ? atoi(argv[1 + xproc]) : 90;
    int flip = argc > 2 + xproc && !strcmp(argv[2 + xproc], "flip");
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"d3d11_present";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"d3d11_present", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                100, 100, 640, 400, NULL, NULL, wc.hInstance, NULL);
    RECT rc; GetClientRect(hwnd, &rc);
    printf("window %p client %ldx%ld visible=%d\n", hwnd, rc.right, rc.bottom, IsWindowVisible(hwnd));
    pump();
    if (xproc) {
        char cmd[512]; STARTUPINFOA si = {0}; PROCESS_INFORMATION pi = {0};
        si.cb = sizeof si;
        snprintf(cmd, sizeof cmd, "\"%s\" client %p %d %s", argv[0], hwnd, frames, flip ? "flip" : "discard");
        printf("parent: spawning %s\n", cmd);
        if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            printf("parent: CreateProcess failed %lu\n", GetLastError()); return 5;
        }
        DWORD t0 = GetTickCount(); int grown = 0;
        for (;;) {
            pump();
            if (!grown && GetTickCount() - t0 > 4000) {
                SetWindowPos(hwnd, NULL, 0, 0, 700, 480, SWP_NOMOVE | SWP_NOZORDER);
                GetClientRect(hwnd, &rc);
                printf("parent: grew window, client now %ldx%ld\n", rc.right, rc.bottom); grown = 1;
            }
            if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0) break;
            if (GetTickCount() - t0 > 60000) { printf("parent: child timeout, killing\n"); TerminateProcess(pi.hProcess, 9); break; }
        }
        DWORD code = 99; GetExitCodeProcess(pi.hProcess, &code);
        printf("parent: child exit %lu -> %s\n", code, code == 0 ? "OK" : "FAIL");
        DestroyWindow(hwnd);
        return code == 0 ? 0 : 1;
    }
    return render_on(hwnd, frames, flip, 0);
}

static int render_on(HWND hwnd, int frames, int flip, int foreign)
{
    RECT rc; GetClientRect(hwnd, &rc);
    if (foreign) printf("client: pid %lu hwnd %p client %ldx%ld owner-pid %lu\n", GetCurrentProcessId(), hwnd, rc.right, rc.bottom,
                        ({ DWORD p = 0; GetWindowThreadProcessId(hwnd, &p); p; }));
    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; IDXGISwapChain *sc = NULL;
    D3D_FEATURE_LEVEL fl = 0;
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   want, 4, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               want + 1, 3, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    printf("D3D11CreateDevice hr=%#lx fl=%#x\n", (unsigned long)hr, (unsigned)fl);
    modpath("d3d11.dll"); modpath("dxgi.dll"); modpath("winemetal.dll"); modpath("d3d11_dxmt.dll"); modpath("dxgi_dxmt.dll");
    if (FAILED(hr)) return 2;

    IDXGIDevice *dxdev = NULL; IDXGIAdapter *adapter = NULL; IDXGIFactory2 *fac2 = NULL;
    hr = ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxdev);
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxdev, &adapter);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&fac2);
    printf("IDXGIFactory2 hr=%#lx %p\n", (unsigned long)hr, fac2);
    if (fac2) {
        DXGI_SWAP_CHAIN_DESC1 d = {0};
        d.Width = rc.right; d.Height = rc.bottom; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
        d.BufferCount = 2; d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = flip ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL : DXGI_SWAP_EFFECT_DISCARD;
        IDXGISwapChain1 *sc1 = NULL;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(fac2, (IUnknown *)dev, hwnd, &d, NULL, NULL, &sc1);
        printf("CreateSwapChainForHwnd(%s) hr=%#lx %p\n", flip ? "flip" : "discard", (unsigned long)hr, sc1);
        sc = (IDXGISwapChain *)sc1;
    }
    if (!sc) {
        DXGI_SWAP_CHAIN_DESC sd = {0};
        sd.BufferDesc.Width = rc.right; sd.BufferDesc.Height = rc.bottom; sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
        sd.OutputWindow = hwnd; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        IDXGIFactory *fac = NULL;
        hr = CreateDXGIFactory(&IID_IDXGIFactory, (void **)&fac);
        if (SUCCEEDED(hr)) hr = IDXGIFactory_CreateSwapChain(fac, (IUnknown *)dev, &sd, &sc);
        printf("CreateSwapChain (legacy) hr=%#lx %p\n", (unsigned long)hr, sc);
    }
    if (!sc) { fflush(stdout); return 3; }

    ID3D11Texture2D *bb = NULL; ID3D11RenderTargetView *rtv = NULL;
    hr = IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    printf("backbuffer/RTV hr=%#lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 4;

    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    int bad = 0;
    for (int i = 0; i < frames; i++) {
        float col[4] = { (i % 60) / 60.0f, 0.3f, 1.0f - (i % 60) / 60.0f, 1.0f };
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, col);
        hr = IDXGISwapChain_Present(sc, 1, 0);
        if (FAILED(hr)) { bad++; printf("Present[%d] hr=%#lx\n", i, (unsigned long)hr); if (bad > 5) break; }
        pump();
        if ((i + 1) % 30 == 0) {
            QueryPerformanceCounter(&t1);
            printf("frame %d: %.1f ms total, last hr=%#lx\n", i + 1, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart, (unsigned long)hr);
            fflush(stdout);
        }
    }
    if (!foreign) { SetWindowPos(hwnd, NULL, 0, 0, 700, 480, SWP_NOMOVE | SWP_NOZORDER); pump(); }
    else { RECT r2; DWORD tw = GetTickCount(); do { Sleep(50); GetClientRect(hwnd, &r2); } while (r2.bottom == rc.bottom && GetTickCount() - tw < 8000); }
    GetClientRect(hwnd, &rc);
    ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(bb); rtv = NULL; bb = NULL;
    hr = IDXGISwapChain_ResizeBuffers(sc, 0, rc.right, rc.bottom, DXGI_FORMAT_UNKNOWN, 0);
    printf("ResizeBuffers %ldx%ld hr=%#lx\n", rc.right, rc.bottom, (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        hr = IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
        for (int i = 0; i < 30 && SUCCEEDED(hr); i++) {
            float col[4] = { 0.1f, 0.8f, 0.2f, 1.0f };
            ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, col);
            hr = IDXGISwapChain_Present(sc, 1, 0);
            pump();
        }
        printf("post-resize frames hr=%#lx\n", (unsigned long)hr);
    }
    QueryPerformanceCounter(&t1);
    printf("%s total %.1f ms bad=%d\n", bad ? "FAIL" : "OK", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart, bad);
    fflush(stdout);
    if (!foreign) DestroyWindow(hwnd);
    return bad ? 1 : 0;
}
