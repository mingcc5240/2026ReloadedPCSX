#include <windows.h>
#include <process.h>
#include <stdlib.h>
#include <crtdbg.h>
#include <stdio.h>
#include <string.h>
#include "psxcommon.h"
#include "plugins.h"
#include "cdriso.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

static HWND g_hWnd = NULL;       // VB6 picScreen.hwnd
static HWND g_hGpuWnd = NULL;    // OpenGL 전용 CS_OWNDC 자식 창
static HWND g_hRootWnd = NULL;   // VB6 Form1.hwnd
static HANDLE g_hThread = NULL;
static volatile int g_Running = 0;
static int g_Pad1Opened = 0;
static int g_Pad2Opened = 0;
static int g_SpuOpened = 0;
static unsigned long g_VSyncCount = 0;
static char g_LogPath[MAX_PATH] = { 0 };
static const char* g_LastStep = "Init";

static void WriteLogLine(const char* text) {
    FILE* f;
    if (g_LogPath[0] == 0) return;
    f = fopen(g_LogPath, "a");
    if (f) {
        fprintf(f, "%s\n", text);
        fflush(f);
        fclose(f);
    }
}

static void TraceLog(const char* step) {
    g_LastStep = step;
    WriteLogLine(step);
}

static void OnInvalidParameter(const wchar_t* expr, const wchar_t* func,
    const wchar_t* file, unsigned int line, uintptr_t pReserved) {
    char msg[256];
    snprintf(msg, sizeof(msg), "CRT Invalid Parameter at %s", g_LastStep);
    WriteLogLine(msg);
}

static LONG CALLBACK VectoredCrashHandler(PEXCEPTION_POINTERS pEx) {
    DWORD code = pEx->ExceptionRecord->ExceptionCode;
    if (code == 0xE06D7363 || code == DBG_PRINTEXCEPTION_C || code == 0x406D1388) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION || code == STATUS_STACK_BUFFER_OVERRUN ||
        code == STATUS_HEAP_CORRUPTION || code == EXCEPTION_STACK_OVERFLOW) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Fatal Exception at %s\nCode: 0x%08lX (Addr: 0x%08X)",
            g_LastStep, code, (unsigned int)(uintptr_t)pEx->ExceptionRecord->ExceptionAddress);
        WriteLogLine(msg);
        MessageBoxA(g_hRootWnd, msg, "PCSXR Crash Guard", MB_ICONERROR);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// .cue 파일에서 실제 .bin 경로를 추출하고 존재 여부 검증
static int ResolveCueToBin(const char* inputPath, char* outPath, size_t outSize) {
    const char* ext = strrchr(inputPath, '.');
    FILE* f;
    char line[512];
    char binName[MAX_PATH] = { 0 };
    char candidate[MAX_PATH] = { 0 };

    strncpy(outPath, inputPath, outSize - 1);
    outPath[outSize - 1] = '\0';

    if (!ext || _stricmp(ext, ".cue") != 0) {
        return 0; // .bin/.iso 직접 선택 시 그대로 사용
    }

    f = fopen(inputPath, "rb");
    if (!f) return -1;

    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        // UTF-8 BOM 제거
        if ((unsigned char)p[0] == 0xEF &&
            (unsigned char)p[1] == 0xBB &&
            (unsigned char)p[2] == 0xBF) {
            p += 3;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (_strnicmp(p, "FILE", 4) == 0 && (p[4] == ' ' || p[4] == '\t')) {
            p += 4;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') {
                char* endQuote = strchr(p + 1, '"');
                if (endQuote) {
                    size_t len = (size_t)(endQuote - (p + 1));
                    if (len >= sizeof(binName)) len = sizeof(binName) - 1;
                    memcpy(binName, p + 1, len);
                    binName[len] = '\0';
                }
            }
            else {
                char* endTok = p;
                while (*endTok && *endTok != ' ' && *endTok != '\t' && *endTok != '\r' && *endTok != '\n') {
                    endTok++;
                }
                size_t len = (size_t)(endTok - p);
                if (len >= sizeof(binName)) len = sizeof(binName) - 1;
                memcpy(binName, p, len);
                binName[len] = '\0';
            }
            break;
        }
    }
    fclose(f);

    if (binName[0] == '\0') return -2;

    if (strchr(binName, '\\') || strchr(binName, '/')) {
        strncpy(candidate, binName, sizeof(candidate) - 1);
    }
    else {
        const char* slash = strrchr(inputPath, '\\');
        if (slash) {
            size_t dirLen = (size_t)(slash - inputPath + 1);
            if (dirLen >= sizeof(candidate)) dirLen = sizeof(candidate) - 1;
            memcpy(candidate, inputPath, dirLen);
            candidate[dirLen] = '\0';
            strncat(candidate, binName, sizeof(candidate) - strlen(candidate) - 1);
        }
        else {
            strncpy(candidate, binName, sizeof(candidate) - 1);
        }
    }

    if (GetFileAttributesA(candidate) == INVALID_FILE_ATTRIBUTES) {
        char msg[MAX_PATH + 128];
        snprintf(msg, sizeof(msg),
            "CUE 파일이 가리키는 BIN 파일을 찾을 수 없습니다:\n%s\n\n폴더 내 실제 .bin 파일명을 확인하세요.",
            candidate);
        WriteLogLine(msg);
        MessageBoxA(g_hRootWnd, msg, "PCSXR CUE Error", MB_ICONERROR);
        return -3;
    }

    strncpy(outPath, candidate, outSize - 1);
    outPath[outSize - 1] = '\0';
    {
        char logMsg[MAX_PATH + 64];
        snprintf(logMsg, sizeof(logMsg), "[CUE Resolved] -> %s", outPath);
        WriteLogLine(logMsg);
    }
    return 0;
}

static LRESULT CALLBACK GpuViewportWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(hwnd, NULL);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static HWND CreateOpenGLViewport(HWND parentHwnd) {
    WNDCLASSEXA wc;
    RECT rc;
    int w, h;
    HINSTANCE hInst = GetModuleHandleA(NULL);
    HWND hwnd;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = GpuViewportWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "PCSX_GL_VIEWPORT";
    RegisterClassExA(&wc);

    GetClientRect(parentHwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;

    hwnd = CreateWindowExA(
        0, "PCSX_GL_VIEWPORT", "",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, w, h,
        parentHwnd, NULL, hInst, NULL
    );
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

// --- OS별 DLL 로드/언로드 함수 ---
void* SysLoadLibrary(const char* lib) { return (void*)LoadLibraryA(lib); }
void* SysLoadSym(void* lib, const char* sym) { return (void*)GetProcAddress((HMODULE)lib, sym); }
const char* SysLibError(void) { return NULL; }
void SysCloseLibrary(void* lib) { if (lib) FreeLibrary((HMODULE)lib); }

int OpenPlugins(void) {
    RECT rc;
    int w, h;
    char vpLog[128];

    TraceLog("[Step 6] CDR_open");
    if (CDR_open() < 0) return -1;

    // GPU_open 직전에 부모 picScreen 크기에 맞춰 뷰포트 크기 강제 동기화
    GetClientRect(g_hWnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    SetWindowPos(g_hGpuWnd, HWND_TOP, 0, 0, w, h, SWP_SHOWWINDOW);

    snprintf(vpLog, sizeof(vpLog), "[Step 7] GPU_open (Viewport %dx%d)", w, h);
    TraceLog(vpLog);

    if (GPU_open((HWND*)g_hGpuWnd) < 0) {
        if (GPU_open(&g_hGpuWnd) < 0) {
            return -3;
        }
    }
    return 0;
}

void ClosePlugins(void) {
    GPU_close();
    CDR_close();
}

// --- libpcsxcore 필수 콜백 ---
int SysInit(void) {
    if (EmuInit() == -1) return -1;
    LoadMcds(Config.Mcd1, Config.Mcd2);
    return 0;
}
void SysReset(void) { EmuReset(); }
void SysPrintf(const char* fmt, ...) {}
void SysMessage(const char* fmt, ...) {}

void SysUpdate(void) {
    g_VSyncCount++;
    if (g_VSyncCount == 60) {
        WriteLogLine("[VSync] Frame 60 reached (CPU/GPU loop active)");
    }
    if (!g_Running) {
        ClosePlugins();
        _endthreadex(0);
    }
}

void SysClose(void) { EmuShutdown(); }
void SysRunGui(void) { g_Running = 0; }

// --- 에뮬레이션 실행 스레드 ---
static unsigned __stdcall EmuThreadProc(void* lpParam) {
    int ret = OpenPlugins();
    if (ret < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "OpenPlugins failed: %d", ret);
        MessageBoxA(g_hRootWnd, msg, "PCSXR", MB_ICONERROR);
        ClosePlugins();
        g_Running = 0;
        return 1;
    }

    TraceLog("[Step 8] SysReset");
    SysReset();

    TraceLog("[Step 9] CheckCdrom");
    CheckCdrom();

    TraceLog("[Step 10] LoadCdrom");
    if (LoadCdrom() == -1) {
        MessageBoxA(g_hRootWnd, "LoadCdrom failed (Check BIN image or BIOS)", "PCSXR", MB_ICONERROR);
        ClosePlugins();
        g_Running = 0;
        return 1;
    }

    TraceLog("[Step 11] psxCpu->Execute");
    psxCpu->Execute();

    ClosePlugins();
    return 0;
}

// --- VB6 호출용 익스포트 함수 ---
int __stdcall PSX_Init(HWND targetHwnd, const char* pluginsDir, const char* biosPath,
    const char* gpuDll, const char* spuDll, const char* cdrDll, const char* padDll) {
    char searchPath[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE hFind;
    FILE* f;

    _set_invalid_parameter_handler(OnInvalidParameter);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    AddVectoredExceptionHandler(1, VectoredCrashHandler);

    g_hWnd = targetHwnd;
    g_hRootWnd = GetAncestor(targetHwnd, GA_ROOT);
    if (!g_hRootWnd) g_hRootWnd = targetHwnd;

    SetWindowLongA(g_hWnd, GWL_STYLE,
        GetWindowLongA(g_hWnd, GWL_STYLE) | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
    if (g_hRootWnd) {
        SetWindowLongA(g_hRootWnd, GWL_STYLE,
            GetWindowLongA(g_hRootWnd, GWL_STYLE) | WS_CLIPCHILDREN);
    }

    if (!g_hGpuWnd) {
        g_hGpuWnd = CreateOpenGLViewport(g_hWnd);
    }

    GetModuleFileNameA(NULL, g_LogPath, MAX_PATH);
    {
        char* slash = strrchr(g_LogPath, '\\');
        if (slash) strcpy(slash + 1, "pcsx_trace.log");
    }
    f = fopen(g_LogPath, "w");
    if (f) fclose(f);

    TraceLog("[Step 1] PSX_Init");
    memset(&Config, 0, sizeof(Config));

    strcpy(Config.PluginsDir, pluginsDir);
    strcpy(Config.BiosDir, biosPath);
    strcpy(Config.Gpu, gpuDll);
    strcpy(Config.Spu, spuDll);
    strcpy(Config.Cdr, cdrDll);
    strcpy(Config.Pad1, padDll);
    strcpy(Config.Pad2, padDll);
    Config.Cpu =  CPU_INTERPRETER; // CPU_DYNAREC;
    Config.PsxAuto = 1;

    snprintf(searchPath, sizeof(searchPath), "%s*.bin", Config.BiosDir);
    hFind = FindFirstFileA(searchPath, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        strcpy(Config.Bios, fd.cFileName);
        Config.HLE = 0;
        FindClose(hFind);
        WriteLogLine("[BIOS] Using real BIOS file");
    }
    else {
        strcpy(Config.Bios, "HLE");
        Config.HLE = 1;
        WriteLogLine("[BIOS] Using HLE BIOS");
    }

    if (SysInit() == -1) return -1;
    return 0;
}

int __stdcall PSX_RunISO(const char* isoPath) {
    unsigned threadId = 0;
    char resolvedPath[MAX_PATH];

    if (g_Running) return -1;

    if (ResolveCueToBin(isoPath, resolvedPath, sizeof(resolvedPath)) < 0) {
        return -6;
    }

    g_VSyncCount = 0;
    TraceLog("[Step 2] SetIsoFile");
    SetIsoFile(resolvedPath);

    TraceLog("[Step 3] LoadPlugins");
    if (LoadPlugins() == -1) return -2;

    TraceLog("[Step 4] SPU_open(g_hRootWnd)");
    if (SPU_open(g_hRootWnd) < 0) {
        ReleasePlugins();
        return -3;
    }
    g_SpuOpened = 1;

    TraceLog("[Step 5] PAD1_open(&g_hRootWnd)");
    if (PAD1_open(&g_hRootWnd) < 0) {
        SPU_close();
        g_SpuOpened = 0;
        ReleasePlugins();
        return -4;
    }
    g_Pad1Opened = 1;

    if (PAD2_open(&g_hRootWnd) >= 0) {
        g_Pad2Opened = 1;
    }

    g_Running = 1;
    g_hThread = (HANDLE)_beginthreadex(NULL, 0, EmuThreadProc, NULL, 0, &threadId);
    if (g_hThread == NULL) {
        g_Running = 0;
        if (g_Pad2Opened) { PAD2_close(); g_Pad2Opened = 0; }
        if (g_Pad1Opened) { PAD1_close(); g_Pad1Opened = 0; }
        if (g_SpuOpened) { SPU_close(); g_SpuOpened = 0; }
        ReleasePlugins();
        return -5;
    }
    return 0;
}

void __stdcall PSX_Stop(void) {
    if (g_Running) {
        g_Running = 0;
        if (g_hThread) {
            WaitForSingleObject(g_hThread, 2000);
            CloseHandle(g_hThread);
            g_hThread = NULL;
        }
    }
    if (g_Pad2Opened) { PAD2_close(); g_Pad2Opened = 0; }
    if (g_Pad1Opened) { PAD1_close(); g_Pad1Opened = 0; }
    if (g_SpuOpened) { SPU_close(); g_SpuOpened = 0; }
    ReleasePlugins();
}

void __stdcall PSX_Shutdown(void) {
    PSX_Stop();
    if (g_hGpuWnd) {
        DestroyWindow(g_hGpuWnd);
        g_hGpuWnd = NULL;
    }
    SysClose();
}

void __stdcall PSX_ConfigSPU(void) {
    if (g_Running) {
        MessageBoxA(g_hRootWnd, "게임을 중지(Stop)한 상태에서 오디오 설정을 열어주세요.", "PCSXR", MB_ICONINFORMATION);
        return;
    }
    if (LoadPlugins() == 0) {
        if (SPU_configure) {
            SPU_configure();
        }
        ReleasePlugins();
    }
}