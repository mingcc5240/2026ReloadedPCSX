#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gl/gl.h>
#include <stdint.h>
#include <string.h>

#define CALLBACK_API __stdcall

// 전역 상태
static HWND  g_hwnd = NULL;
static HDC   g_hdc  = NULL;
static HGLRC g_hrc  = NULL;

// PS1 GPU 상태 레지스터 (bit 26, 27, 28 = Ready 상태여야 BIOS가 멈추지 않음)
static uint32_t g_gpu_stat = 0x14802000;

// ------------------------------------------------------------
// 1. PSEmu Pro 플러그인 메타데이터
// ------------------------------------------------------------
uint32_t CALLBACK_API PSEgetLibType(void) {
    return 2; // PSE_LT_GPU = 2
}

char* CALLBACK_API PSEgetLibName(void) {
    return "MiniGL PSX GPU Plugin (Step 1)";
}

uint32_t CALLBACK_API PSEgetLibVersion(void) {
    return (1 << 16) | (0 << 8) | 1; // v1.0.1
}

// ------------------------------------------------------------
// 2. 초기화 / 종료 / 설정
// ------------------------------------------------------------
long CALLBACK_API GPUinit(void) {
    return 0;
}

long CALLBACK_API GPUshutdown(void) {
    return 0;
}

long CALLBACK_API GPUtest(void) {
    return 0;
}

long CALLBACK_API GPUconfigure(void) {
    MessageBoxA(NULL, "MiniGL GPU Plugin - no stetting (Step 1)", "Configure", MB_OK);
    return 0;
}

void CALLBACK_API GPUabout(void) {
    MessageBoxA(NULL, "MiniGL PSX OpenGL Plugin\nBuilt from scratch.", "About", MB_OK);
}

// ------------------------------------------------------------
// 3. OpenGL 컨텍스트 생성 및 해제
// ------------------------------------------------------------
long CALLBACK_API GPUopen(HWND hwnd) {
    g_hwnd = hwnd;
    g_hdc  = GetDC(g_hwnd);
    if (!g_hdc) return -1;

    PIXELFORMATDESCRIPTOR pfd;
    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 16;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pf = ChoosePixelFormat(g_hdc, &pfd);
    if (!pf || !SetPixelFormat(g_hdc, pf, &pfd)) {
        return -1;
    }

    g_hrc = wglCreateContext(g_hdc);
    if (!g_hrc || !wglMakeCurrent(g_hdc, g_hrc)) {
        return -1;
    }

    // 기본 뷰포트 및 직교 투영 설정
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int w = (rc.right - rc.left) > 0 ? (rc.right - rc.left) : 640;
    int h = (rc.bottom - rc.top) > 0 ? (rc.bottom - rc.top) : 480;

    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 1.0, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    return 0;
}

long CALLBACK_API GPUclose(void) {
    if (g_hrc) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(g_hrc);
        g_hrc = NULL;
    }
    if (g_hdc && g_hwnd) {
        ReleaseDC(g_hwnd, g_hdc);
        g_hdc = NULL;
    }
    return 0;
}

// ------------------------------------------------------------
// 4. 매 프레임(VSync) 화면 갱신
// ------------------------------------------------------------
void CALLBACK_API GPUupdateLace(void) {
    if (!g_hdc || !g_hrc) return;

    // 1단계 테스트: 짙은 파란색 배경 + 중앙 노란 사각형 출력
    glClearColor(0.05f, 0.15f, 0.35f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glColor3f(1.0f, 0.85f, 0.2f);
    glBegin(GL_QUADS);
        glVertex2f(0.35f, 0.35f);
        glVertex2f(0.65f, 0.35f);
        glVertex2f(0.65f, 0.65f);
        glVertex2f(0.35f, 0.65f);
    glEnd();

    SwapBuffers(g_hdc);
}

// ------------------------------------------------------------
// 5. GPU 명령 및 DMA 스텁 (BIOS가 멈추지 않도록 Ready 유지)
// ------------------------------------------------------------
void CALLBACK_API GPUwriteStatus(uint32_t gdata) {
    (void)gdata;
}

uint32_t CALLBACK_API GPUreadStatus(void) {
    // Ready to receive Cmd (bit 26), Send VRAM (bit 27), Receive DMA (bit 28)
    return g_gpu_stat | 0x1C000000;
}

void CALLBACK_API GPUwriteData(uint32_t gdata) {
    (void)gdata;
}

void CALLBACK_API GPUwriteDataMem(uint32_t *pMem, int iSize) {
    (void)pMem;
    (void)iSize;
}

uint32_t CALLBACK_API GPUreadData(void) {
    return 0;
}

void CALLBACK_API GPUreadDataMem(uint32_t *pMem, int iSize) {
    if (pMem && iSize > 0) memset(pMem, 0, iSize * sizeof(uint32_t));
}

long CALLBACK_API GPUdmaChain(uint32_t *baseAddrL, uint32_t addr) {
    (void)baseAddrL;
    (void)addr;
    return 0;
}

long CALLBACK_API GPUfreeze(uint32_t ulGetFreezeData, void *pF) {
    (void)ulGetFreezeData;
    (void)pF;
    return 1;
}