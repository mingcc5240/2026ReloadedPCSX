#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gl/gl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CALLBACK_API __stdcall

#define VRAM_W 1024
#define VRAM_H 512

// OpenGL / Win32 핸들
static HWND   g_hwnd = NULL;
static HDC    g_hdc = NULL;
static HGLRC  g_hrc = NULL;
static GLuint g_vram_tex = 0;

// 창 크기 및 필터 설정
static int g_target_w = 800;
static int g_target_h = 600;
static int g_linear_filter = 0; // 0: GL_NEAREST, 1: GL_LINEAR

// 60 FPS 제한기 타이머 상태
static LARGE_INTEGER g_perf_freq;
static LARGE_INTEGER g_last_frame;
static int           g_timer_init = 0;

// PS1 1MB VRAM (1024x512, 16-bit 0BBBBBGGGGGRRRRR)
static uint16_t g_vram[VRAM_H][VRAM_W];
static uint32_t g_rgba_buf[VRAM_W * VRAM_H];
static int      g_vram_dirty = 1;

// GP1 상태 및 디스플레이 설정
static uint32_t g_gpu_stat = 0x14802000;
static int g_disp_disabled = 1;
static int g_disp_x = 0, g_disp_y = 0;
static int g_disp_w = 320, g_disp_h = 240;
static int g_disp_y1 = 16, g_disp_y2 = 256;
static int g_disp_interlace = 0;
static int g_disp_vres = 0;
static int g_is_24bit = 0;

// GP0 그리기 환경
static int g_draw_x1 = 0, g_draw_y1 = 0;
static int g_draw_x2 = 1023, g_draw_y2 = 511;
static int g_draw_off_x = 0, g_draw_off_y = 0;
static uint16_t g_texpage = 0;

// GP0 0xE2 텍스처 윈도우
static int g_tw_mask_x = 0, g_tw_mask_y = 0;
static int g_tw_off_x = 0, g_tw_off_y = 0;

// GP0 명령 파서 상태
static uint32_t g_cmd_buf[16];
static int      g_cmd_idx = 0;
static int      g_cmd_len = 0;

// 폴리라인 상태 머신
typedef struct {
    int x, y;
    int r, g, b;
    int u, v;
} Vertex;

static int      g_in_polyline = 0;
static uint8_t  g_poly_cmd = 0;
static uint32_t g_poly_col = 0;
static Vertex   g_poly_prev;
static int      g_poly_has_prev = 0;
static int      g_poly_expect_xy = 0;

// CPU <-> VRAM 전송 상태
static int g_xfer_mode = 0;
static int g_xfer_x = 0, g_xfer_y = 0;
static int g_xfer_w = 0, g_xfer_h = 0;
static int g_xfer_cur_x = 0, g_xfer_cur_y = 0;

// ------------------------------------------------------------
// 창 크기 조절 및 60 FPS 제한기
// ------------------------------------------------------------
static void ResizeWindow(int client_w, int client_h) {
    if (!g_hwnd) return;
    g_target_w = client_w;
    g_target_h = client_h;

    DWORD style = (DWORD)GetWindowLongA(g_hwnd, GWL_STYLE);
    DWORD exstyle = (DWORD)GetWindowLongA(g_hwnd, GWL_EXSTYLE);
    BOOL has_menu = (GetMenu(g_hwnd) != NULL);

    RECT rc = { 0, 0, client_w, client_h };
    AdjustWindowRectEx(&rc, style, has_menu, exstyle);

    int win_w = rc.right - rc.left;
    int win_h = rc.bottom - rc.top;
    SetWindowPos(g_hwnd, NULL, 0, 0, win_w, win_h,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void CheckHotkeys(void) {
    static int prev_f5 = 0, prev_f6 = 0, prev_f7 = 0, prev_f8 = 0, prev_f9 = 0;

    int cur_f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    int cur_f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    int cur_f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    int cur_f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    int cur_f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;

    if (cur_f5 && !prev_f5) ResizeWindow(640, 480);
    if (cur_f6 && !prev_f6) ResizeWindow(800, 600);
    if (cur_f7 && !prev_f7) ResizeWindow(1024, 768);
    if (cur_f8 && !prev_f8) ResizeWindow(1280, 960);
    if (cur_f9 && !prev_f9) {
        g_linear_filter = !g_linear_filter;
        if (g_vram_tex) {
            GLint mode = g_linear_filter ? GL_LINEAR : GL_NEAREST;
            glBindTexture(GL_TEXTURE_2D, g_vram_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mode);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mode);
        }
    }

    prev_f5 = cur_f5; prev_f6 = cur_f6; prev_f7 = cur_f7;
    prev_f8 = cur_f8; prev_f9 = cur_f9;
}

static void LimitFPS(void) {
    if (!g_timer_init) {
        QueryPerformanceFrequency(&g_perf_freq);
        QueryPerformanceCounter(&g_last_frame);
        g_timer_init = 1;
        return;
    }

    const double target_sec = 1.0 / 80.0;
    LARGE_INTEGER now;

    for (;;) {
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - g_last_frame.QuadPart) / (double)g_perf_freq.QuadPart;
        double remain = target_sec - elapsed;
        if (remain <= 0.0) break;
        if (remain > 0.002) {
            Sleep(1);
        }
    }

    QueryPerformanceCounter(&g_last_frame);
}

// ------------------------------------------------------------
// 보조 함수: 좌표, 색상 변환, 반투명 블렌딩, 텍스처 샘플링
// ------------------------------------------------------------
static inline int sign_ext11(uint32_t v) {
    v &= 0x7FFu;
    return (v & 0x400u) ? (int)(v | ~0x7FFu) : (int)v;
}

static inline int clamp_int(int v, int lo, int hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static inline uint32_t rgb555_to_rgba8888(uint16_t c) {
    uint32_t r = ((c >> 0) & 0x1F) << 3;
    uint32_t g = ((c >> 5) & 0x1F) << 3;
    uint32_t b = ((c >> 10) & 0x1F) << 3;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static inline uint16_t make_rgb555(int r, int g, int b) {
    r = clamp_int(r, 0, 255) >> 3;
    g = clamp_int(g, 0, 255) >> 3;
    b = clamp_int(b, 0, 255) >> 3;
    return (uint16_t)((b << 10) | (g << 5) | r);
}

static inline uint16_t rgb888_to_rgb555(uint32_t rgb) {
    return make_rgb555((int)(rgb & 0xFF), (int)((rgb >> 8) & 0xFF), (int)((rgb >> 16) & 0xFF));
}

static inline uint16_t BlendPixel(uint16_t bg, uint16_t fg, int mode) {
    int br = (bg >> 0) & 0x1F;
    int bg_c = (bg >> 5) & 0x1F;
    int bb = (bg >> 10) & 0x1F;

    int fr = (fg >> 0) & 0x1F;
    int fg_c = (fg >> 5) & 0x1F;
    int fb = (fg >> 10) & 0x1F;

    int r, g, b;
    switch (mode & 3) {
    case 0: r = (br + fr) >> 1; g = (bg_c + fg_c) >> 1; b = (bb + fb) >> 1; break;
    case 1: r = br + fr;        g = bg_c + fg_c;        b = bb + fb;        break;
    case 2: r = br - fr;        g = bg_c - fg_c;        b = bb - fb;        break;
    default: r = br + (fr >> 2); g = bg_c + (fg_c >> 2); b = bb + (fb >> 2); break;
    }

    r = clamp_int(r, 0, 31);
    g = clamp_int(g, 0, 31);
    b = clamp_int(b, 0, 31);
    return (uint16_t)((b << 10) | (g << 5) | r);
}

static inline void PlotPixel(int x, int y, uint16_t color, int semi_trans, int blend_mode) {
    int vx = x & 0x3FF;
    int vy = y & 0x1FF;
    if (semi_trans) {
        g_vram[vy][vx] = BlendPixel(g_vram[vy][vx], color, blend_mode);
    }
    else {
        g_vram[vy][vx] = color;
    }
}

static inline uint16_t SampleTexture(int u, int v, uint16_t texpage, uint16_t clut) {
    int tp_x = (texpage & 0x0F) * 64;
    int tp_y = ((texpage >> 4) & 1) * 256;
    int depth = (texpage >> 7) & 3;
    int clut_x = (clut & 0x3F) * 16;
    int clut_y = (clut >> 6) & 0x1FF;

    u = ((u & ~(g_tw_mask_x * 8)) | ((g_tw_off_x & g_tw_mask_x) * 8)) & 0xFF;
    v = ((v & ~(g_tw_mask_y * 8)) | ((g_tw_off_y & g_tw_mask_y) * 8)) & 0xFF;

    if (depth == 0) {
        uint16_t word = g_vram[(tp_y + v) & 0x1FF][(tp_x + (u >> 2)) & 0x3FF];
        int idx = (word >> ((u & 3) * 4)) & 0x0F;
        return g_vram[clut_y][(clut_x + idx) & 0x3FF];
    }
    else if (depth == 1) {
        uint16_t word = g_vram[(tp_y + v) & 0x1FF][(tp_x + (u >> 1)) & 0x3FF];
        int idx = (word >> ((u & 1) * 8)) & 0xFF;
        return g_vram[clut_y][(clut_x + idx) & 0x3FF];
    }
    else {
        return g_vram[(tp_y + v) & 0x1FF][(tp_x + u) & 0x3FF];
    }
}

static inline uint16_t ModulateTexel(uint16_t tex, int r, int g, int b, int raw_tex) {
    if (raw_tex) return tex;
    int tr = ((tex >> 0) & 0x1F) << 3;
    int tg = ((tex >> 5) & 0x1F) << 3;
    int tb = ((tex >> 10) & 0x1F) << 3;
    return make_rgb555((tr * r) >> 7, (tg * g) >> 7, (tb * b) >> 7) | (tex & 0x8000u);
}

// ------------------------------------------------------------
// 삼각형 / 스프라이트 / 라인 래스터라이저
// ------------------------------------------------------------
static inline int EdgeFunc(int ax, int ay, int bx, int by, int cx, int cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static void DrawTriangle(Vertex v0, Vertex v1, Vertex v2,
    int textured, int raw_tex, int semi_trans,
    uint16_t texpage, uint16_t clut) {
    int area = EdgeFunc(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
    if (area == 0) return;
    if (area < 0) {
        Vertex tmp = v1; v1 = v2; v2 = tmp;
        area = -area;
    }

    int min_x = v0.x; if (v1.x < min_x) min_x = v1.x; if (v2.x < min_x) min_x = v2.x;
    int max_x = v0.x; if (v1.x > max_x) max_x = v1.x; if (v2.x > max_x) max_x = v2.x;
    int min_y = v0.y; if (v1.y < min_y) min_y = v1.y; if (v2.y < min_y) min_y = v2.y;
    int max_y = v0.y; if (v1.y > max_y) max_y = v1.y; if (v2.y > max_y) max_y = v2.y;

    if (min_x < g_draw_x1) min_x = g_draw_x1;
    if (min_y < g_draw_y1) min_y = g_draw_y1;
    if (max_x > g_draw_x2) max_x = g_draw_x2;
    if (max_y > g_draw_y2) max_y = g_draw_y2;
    if (min_x > max_x || min_y > max_y) return;

    int blend_mode = (texpage >> 5) & 3;

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            int w0 = EdgeFunc(v1.x, v1.y, v2.x, v2.y, x, y);
            int w1 = EdgeFunc(v2.x, v2.y, v0.x, v0.y, x, y);
            int w2 = EdgeFunc(v0.x, v0.y, v1.x, v1.y, x, y);

            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                int r = (w0 * v0.r + w1 * v1.r + w2 * v2.r) / area;
                int g = (w0 * v0.g + w1 * v1.g + w2 * v2.g) / area;
                int b = (w0 * v0.b + w1 * v1.b + w2 * v2.b) / area;

                if (textured) {
                    int u = (w0 * v0.u + w1 * v1.u + w2 * v2.u) / area;
                    int v = (w0 * v0.v + w1 * v1.v + w2 * v2.v) / area;
                    uint16_t tex = SampleTexture(u, v, texpage, clut);
                    if (tex == 0x0000) continue;
                    uint16_t col = ModulateTexel(tex, r, g, b, raw_tex);
                    int do_blend = semi_trans && ((tex & 0x8000u) != 0);
                    PlotPixel(x, y, col, do_blend, blend_mode);
                }
                else {
                    PlotPixel(x, y, make_rgb555(r, g, b), semi_trans, blend_mode);
                }
            }
        }
    }
    g_vram_dirty = 1;
}

static void DrawRectSprite(int x, int y, int w, int h, uint32_t color,
    int textured, int raw_tex, int semi_trans,
    int u0, int v0, uint16_t clut) {
    int r = color & 0xFF;
    int g = (color >> 8) & 0xFF;
    int b = (color >> 16) & 0xFF;
    uint16_t flat_col = make_rgb555(r, g, b);
    int blend_mode = (g_texpage >> 5) & 3;

    for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        if (py < g_draw_y1 || py > g_draw_y2) continue;
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            if (px < g_draw_x1 || px > g_draw_x2) continue;

            if (textured) {
                uint16_t tex = SampleTexture(u0 + dx, v0 + dy, g_texpage, clut);
                if (tex == 0x0000) continue;
                uint16_t col = ModulateTexel(tex, r, g, b, raw_tex);
                int do_blend = semi_trans && ((tex & 0x8000u) != 0);
                PlotPixel(px, py, col, do_blend, blend_mode);
            }
            else {
                PlotPixel(px, py, flat_col, semi_trans, blend_mode);
            }
        }
    }
    g_vram_dirty = 1;
}

static void DrawLine(Vertex v0, Vertex v1, int semi_trans) {
    int x0 = v0.x, y0 = v0.y;
    int x1 = v1.x, y1 = v1.y;
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    int steps = (dx > dy) ? dx : dy;
    int blend_mode = (g_texpage >> 5) & 3;

    for (int i = 0; ; i++) {
        if (x0 >= g_draw_x1 && x0 <= g_draw_x2 && y0 >= g_draw_y1 && y0 <= g_draw_y2) {
            int r = (steps > 0) ? (v0.r + (v1.r - v0.r) * i / steps) : v0.r;
            int g = (steps > 0) ? (v0.g + (v1.g - v0.g) * i / steps) : v0.g;
            int b = (steps > 0) ? (v0.b + (v1.b - v0.b) * i / steps) : v0.b;
            PlotPixel(x0, y0, make_rgb555(r, g, b), semi_trans, blend_mode);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = err << 1;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
    g_vram_dirty = 1;
}

// ------------------------------------------------------------
// 디스플레이 영역 업로드 (15비트 / 24비트 MDEC)
// ------------------------------------------------------------
static void UpdateDisplayHeight(void) {
    int h = g_disp_y2 - g_disp_y1;
    if (h < 16) h = 240;
    if (g_disp_vres && g_disp_interlace) {
        h <<= 1;
        if (h > 512) h = 512;
    }
    else {
        if (h > 256) h = 256;
    }
    g_disp_h = h;
}

static void UploadDisplayTexture(void) {
    if (!g_vram_tex) return;

    int w = clamp_int(g_disp_w, 1, VRAM_W);
    int h = clamp_int(g_disp_h, 1, VRAM_H);

    if (g_is_24bit) {
        for (int y = 0; y < h; y++) {
            const uint8_t* row_bytes = (const uint8_t*)g_vram[(g_disp_y + y) & 0x1FF];
            uint32_t* dst = &g_rgba_buf[y * w];
            int byte_off = (g_disp_x * 2) & 2047;
            for (int x = 0; x < w; x++) {
                uint32_t r = row_bytes[(byte_off + 0) & 2047];
                uint32_t g = row_bytes[(byte_off + 1) & 2047];
                uint32_t b = row_bytes[(byte_off + 2) & 2047];
                dst[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
                byte_off = (byte_off + 3) & 2047;
            }
        }
    }
    else {
        for (int y = 0; y < h; y++) {
            const uint16_t* src_row = g_vram[(g_disp_y + y) & 0x1FF];
            uint32_t* dst = &g_rgba_buf[y * w];
            for (int x = 0; x < w; x++) {
                dst[x] = rgb555_to_rgba8888(src_row[(g_disp_x + x) & 0x3FF]);
            }
        }
    }

    glBindTexture(GL_TEXTURE_2D, g_vram_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
        GL_RGBA, GL_UNSIGNED_BYTE, g_rgba_buf);
    g_vram_dirty = 0;
}

// ------------------------------------------------------------
// GP0 명령 길이 및 실행기
// ------------------------------------------------------------
static int GetGP0CommandLength(uint8_t cmd) {
    if (cmd == 0x02) return 3;
    if (cmd >= 0x20 && cmd <= 0x23) return 4;
    if (cmd >= 0x24 && cmd <= 0x27) return 7;
    if (cmd >= 0x28 && cmd <= 0x2B) return 5;
    if (cmd >= 0x2C && cmd <= 0x2F) return 9;
    if (cmd >= 0x30 && cmd <= 0x33) return 6;
    if (cmd >= 0x34 && cmd <= 0x37) return 9;
    if (cmd >= 0x38 && cmd <= 0x3B) return 8;
    if (cmd >= 0x3C && cmd <= 0x3F) return 12;
    if (cmd >= 0x40 && cmd <= 0x47) return 3;
    if (cmd >= 0x50 && cmd <= 0x57) return 4;
    if (cmd >= 0x60 && cmd <= 0x63) return 3;
    if (cmd >= 0x64 && cmd <= 0x67) return 4;
    if (cmd >= 0x68 && cmd <= 0x6B) return 2;
    if (cmd >= 0x70 && cmd <= 0x73) return 2;
    if (cmd >= 0x74 && cmd <= 0x77) return 3;
    if (cmd >= 0x78 && cmd <= 0x7B) return 2;
    if (cmd >= 0x7C && cmd <= 0x7F) return 3;
    if (cmd >= 0x80 && cmd <= 0x9F) return 4;
    if (cmd >= 0xA0 && cmd <= 0xBF) return 3;
    if (cmd >= 0xC0 && cmd <= 0xDF) return 3;
    return 1;
}

static Vertex MakeVertex(uint32_t col, uint32_t xy, uint32_t uv) {
    Vertex v;
    v.x = sign_ext11(xy & 0xFFFF) + g_draw_off_x;
    v.y = sign_ext11(xy >> 16) + g_draw_off_y;
    v.r = col & 0xFF;
    v.g = (col >> 8) & 0xFF;
    v.b = (col >> 16) & 0xFF;
    v.u = uv & 0xFF;
    v.v = (uv >> 8) & 0xFF;
    return v;
}

static void ExecuteGP0Command(void) {
    uint8_t cmd = (uint8_t)(g_cmd_buf[0] >> 24);

    if (cmd == 0x02) {
        uint16_t color = rgb888_to_rgb555(g_cmd_buf[0]);
        int x = (g_cmd_buf[1] & 0x3F0);
        int y = (g_cmd_buf[1] >> 16) & 0x1FF;
        int w = ((g_cmd_buf[2] & 0x3FF) + 0x0F) & ~0x0F;
        int h = (g_cmd_buf[2] >> 16) & 0x1FF;
        for (int dy = 0; dy < h; dy++) {
            int py = (y + dy) & 0x1FF;
            for (int dx = 0; dx < w; dx++) {
                g_vram[py][(x + dx) & 0x3FF] = color;
            }
        }
        g_vram_dirty = 1;
    }
    else if (cmd >= 0x20 && cmd <= 0x3F) {
        int gouraud = (cmd & 0x10) != 0;
        int quad = (cmd & 0x08) != 0;
        int textured = (cmd & 0x04) != 0;
        int semi_trans = (cmd & 0x02) != 0;
        int raw_tex = (cmd & 0x01) != 0;
        int count = quad ? 4 : 3;

        Vertex v[4];
        uint16_t clut = 0, texpage = g_texpage;
        int idx = 0;

        for (int i = 0; i < count; i++) {
            uint32_t col = (i == 0 || gouraud) ? g_cmd_buf[idx++] : g_cmd_buf[0];
            uint32_t xy = g_cmd_buf[idx++];
            uint32_t uv = textured ? g_cmd_buf[idx++] : 0;
            if (textured && i == 0) clut = (uint16_t)(uv >> 16);
            if (textured && i == 1) texpage = (uint16_t)(uv >> 16);
            v[i] = MakeVertex(col, xy, uv);
        }

        DrawTriangle(v[0], v[1], v[2], textured, raw_tex, semi_trans, texpage, clut);
        if (quad) {
            DrawTriangle(v[1], v[2], v[3], textured, raw_tex, semi_trans, texpage, clut);
        }
    }
    else if (cmd >= 0x40 && cmd <= 0x47) {
        int semi_trans = (cmd & 0x02) != 0;
        DrawLine(MakeVertex(g_cmd_buf[0], g_cmd_buf[1], 0),
            MakeVertex(g_cmd_buf[0], g_cmd_buf[2], 0), semi_trans);
    }
    else if (cmd >= 0x50 && cmd <= 0x57) {
        int semi_trans = (cmd & 0x02) != 0;
        DrawLine(MakeVertex(g_cmd_buf[0], g_cmd_buf[1], 0),
            MakeVertex(g_cmd_buf[2], g_cmd_buf[3], 0), semi_trans);
    }
    else if (cmd >= 0x60 && cmd <= 0x7F) {
        int textured = (cmd & 0x04) != 0;
        int semi_trans = (cmd & 0x02) != 0;
        int raw_tex = (cmd & 0x01) != 0;
        int size_type = (cmd >> 3) & 3;

        int x = sign_ext11(g_cmd_buf[1] & 0xFFFF) + g_draw_off_x;
        int y = sign_ext11(g_cmd_buf[1] >> 16) + g_draw_off_y;
        int u = 0, v = 0, w = 1, h = 1;
        uint16_t clut = 0;
        int idx = 2;

        if (textured) {
            uint32_t uv = g_cmd_buf[idx++];
            u = uv & 0xFF;
            v = (uv >> 8) & 0xFF;
            clut = (uint16_t)(uv >> 16);
        }
        if (size_type == 0) {
            uint32_t wh = g_cmd_buf[idx++];
            w = wh & 0x3FF;
            h = (wh >> 16) & 0x1FF;
        }
        else if (size_type == 1) { w = 1;  h = 1; }
        else if (size_type == 2) { w = 8;  h = 8; }
        else if (size_type == 3) { w = 16; h = 16; }

        DrawRectSprite(x, y, w, h, g_cmd_buf[0], textured, raw_tex, semi_trans, u, v, clut);
    }
    else if (cmd >= 0x80 && cmd <= 0x9F) {
        int sx = g_cmd_buf[1] & 0x3FF, sy = (g_cmd_buf[1] >> 16) & 0x1FF;
        int dx = g_cmd_buf[2] & 0x3FF, dy = (g_cmd_buf[2] >> 16) & 0x1FF;
        int w = g_cmd_buf[3] & 0x3FF; if (w == 0) w = 1024;
        int h = (g_cmd_buf[3] >> 16) & 0x1FF; if (h == 0) h = 512;
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                g_vram[(dy + row) & 0x1FF][(dx + col) & 0x3FF] =
                    g_vram[(sy + row) & 0x1FF][(sx + col) & 0x3FF];
            }
        }
        g_vram_dirty = 1;
    }
    else if (cmd >= 0xA0 && cmd <= 0xBF) {
        g_xfer_x = g_cmd_buf[1] & 0x3FF;
        g_xfer_y = (g_cmd_buf[1] >> 16) & 0x1FF;
        g_xfer_w = g_cmd_buf[2] & 0x3FF; if (g_xfer_w == 0) g_xfer_w = 1024;
        g_xfer_h = (g_cmd_buf[2] >> 16) & 0x1FF; if (g_xfer_h == 0) g_xfer_h = 512;
        g_xfer_cur_x = g_xfer_cur_y = 0;
        g_xfer_mode = 1;
    }
    else if (cmd >= 0xC0 && cmd <= 0xDF) {
        g_xfer_x = g_cmd_buf[1] & 0x3FF;
        g_xfer_y = (g_cmd_buf[1] >> 16) & 0x1FF;
        g_xfer_w = g_cmd_buf[2] & 0x3FF; if (g_xfer_w == 0) g_xfer_w = 1024;
        g_xfer_h = (g_cmd_buf[2] >> 16) & 0x1FF; if (g_xfer_h == 0) g_xfer_h = 512;
        g_xfer_cur_x = g_xfer_cur_y = 0;
        g_xfer_mode = 2;
    }
    else if (cmd == 0xE1) {
        g_texpage = (uint16_t)(g_cmd_buf[0] & 0x7FF);
    }
    else if (cmd == 0xE2) {
        g_tw_mask_x = (g_cmd_buf[0] >> 0) & 0x1F;
        g_tw_mask_y = (g_cmd_buf[0] >> 5) & 0x1F;
        g_tw_off_x = (g_cmd_buf[0] >> 10) & 0x1F;
        g_tw_off_y = (g_cmd_buf[0] >> 15) & 0x1F;
    }
    else if (cmd == 0xE3) {
        g_draw_x1 = g_cmd_buf[0] & 0x3FF;
        g_draw_y1 = (g_cmd_buf[0] >> 10) & 0x1FF;
    }
    else if (cmd == 0xE4) {
        g_draw_x2 = g_cmd_buf[0] & 0x3FF;
        g_draw_y2 = (g_cmd_buf[0] >> 10) & 0x1FF;
    }
    else if (cmd == 0xE5) {
        g_draw_off_x = sign_ext11(g_cmd_buf[0]);
        g_draw_off_y = sign_ext11(g_cmd_buf[0] >> 11);
    }
}

static void WriteVramHalfword(uint16_t val) {
    g_vram[(g_xfer_y + g_xfer_cur_y) & 0x1FF][(g_xfer_x + g_xfer_cur_x) & 0x3FF] = val;
    if (++g_xfer_cur_x >= g_xfer_w) {
        g_xfer_cur_x = 0;
        if (++g_xfer_cur_y >= g_xfer_h) {
            g_xfer_mode = 0;
            g_vram_dirty = 1;
        }
    }
}

static uint16_t ReadVramHalfword(void) {
    uint16_t val = g_vram[(g_xfer_y + g_xfer_cur_y) & 0x1FF][(g_xfer_x + g_xfer_cur_x) & 0x3FF];
    if (++g_xfer_cur_x >= g_xfer_w) {
        g_xfer_cur_x = 0;
        if (++g_xfer_cur_y >= g_xfer_h) {
            g_xfer_mode = 0;
        }
    }
    return val;
}

static void ProcessGP0Word(uint32_t data) {
    if (g_xfer_mode == 1) {
        WriteVramHalfword((uint16_t)(data & 0xFFFF));
        if (g_xfer_mode == 1) WriteVramHalfword((uint16_t)(data >> 16));
        return;
    }

    if (g_in_polyline) {
        int gouraud = (g_poly_cmd & 0x10) != 0;
        int semi_trans = (g_poly_cmd & 0x02) != 0;

        if ((!gouraud || !g_poly_expect_xy) && (data & 0xF000F000u) == 0x50005000u) {
            g_in_polyline = 0;
            g_cmd_idx = 0;
            return;
        }
        if (gouraud && !g_poly_expect_xy) {
            g_poly_col = data;
            g_poly_expect_xy = 1;
        }
        else {
            Vertex cur = MakeVertex(g_poly_col, data, 0);
            if (g_poly_has_prev) DrawLine(g_poly_prev, cur, semi_trans);
            g_poly_prev = cur;
            g_poly_has_prev = 1;
            g_poly_expect_xy = 0;
        }
        return;
    }

    if (g_cmd_idx == 0) {
        uint8_t cmd = (uint8_t)(data >> 24);
        if ((cmd >= 0x48 && cmd <= 0x4F) || (cmd >= 0x58 && cmd <= 0x5F)) {
            g_in_polyline = 1;
            g_poly_cmd = cmd;
            g_poly_col = data;
            g_poly_has_prev = 0;
            g_poly_expect_xy = 1;
            return;
        }
        g_cmd_len = GetGP0CommandLength(cmd);
    }

    if (g_cmd_idx < 16) g_cmd_buf[g_cmd_idx] = data;
    if (++g_cmd_idx >= g_cmd_len) {
        ExecuteGP0Command();
        g_cmd_idx = 0;
    }
}

// ------------------------------------------------------------
// PSEmu Pro API
// ------------------------------------------------------------
uint32_t CALLBACK_API PSEgetLibType(void) { return 2; }
char* CALLBACK_API PSEgetLibName(void) { return "MiniGL PSX GPU Plugin (Step 7)"; }
uint32_t CALLBACK_API PSEgetLibVersion(void) { return (1 << 16) | (0 << 8) | 7; }

long CALLBACK_API GPUinit(void) {
    memset(g_vram, 0, sizeof(g_vram));
    g_vram_dirty = 1;
    g_timer_init = 0;
    return 0;
}
long CALLBACK_API GPUshutdown(void) { return 0; }
long CALLBACK_API GPUtest(void) { return 0; }
long CALLBACK_API GPUconfigure(void) {
    MessageBoxA(NULL,
        "MiniGL GPU Plugin - Step 7 (60 FPS + Resize)\n\n"
        "Key (Running Game):\n"
        "F5 : 640 x 480\n"
        "F6 : 800 x 600 (basic)\n"
        "F7 : 1024 x 768\n"
        "F8 : 1280 x 960\n"
        "F9 : Nearest / Linear filter",
        "Configure", MB_OK);
    return 0;
}
void CALLBACK_API GPUabout(void) {
    MessageBoxA(NULL, "MiniGL PSX OpenGL Plugin\nStep 7: 60 FPS Limiter & Resizable Window", "About", MB_OK);
}

long CALLBACK_API GPUopen(HWND hwnd) {
    g_hwnd = hwnd;
    ResizeWindow(g_target_w, g_target_h);

    g_hdc = GetDC(g_hwnd);
    if (!g_hdc) return -1;

    PIXELFORMATDESCRIPTOR pfd;
    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 16;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pf = ChoosePixelFormat(g_hdc, &pfd);
    if (!pf || !SetPixelFormat(g_hdc, pf, &pfd)) return -1;

    g_hrc = wglCreateContext(g_hdc);
    if (!g_hrc || !wglMakeCurrent(g_hdc, g_hrc)) return -1;

    glGenTextures(1, &g_vram_tex);
    glBindTexture(GL_TEXTURE_2D, g_vram_tex);
    GLint mode = g_linear_filter ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, VRAM_W, VRAM_H, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    g_vram_dirty = 1;
    g_timer_init = 0;
    return 0;
}

long CALLBACK_API GPUclose(void) {
    if (g_hrc) {
        if (g_vram_tex) { glDeleteTextures(1, &g_vram_tex); g_vram_tex = 0; }
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(g_hrc);
        g_hrc = NULL;
    }
    if (g_hdc && g_hwnd) { ReleaseDC(g_hwnd, g_hdc); g_hdc = NULL; }
    return 0;
}

void CALLBACK_API GPUupdateLace(void) {
    if (!g_hdc || !g_hrc) return;

    CheckHotkeys();

    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int win_w = (rc.right - rc.left) > 0 ? (rc.right - rc.left) : g_target_w;
    int win_h = (rc.bottom - rc.top) > 0 ? (rc.bottom - rc.top) : g_target_h;

    // 전체 배경을 검은색으로 클리어
    glViewport(0, 0, win_w, win_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // 4:3 비율 유지 뷰포트 계산
    int vp_w = win_w;
    int vp_h = (win_w * 3) / 4;
    if (vp_h > win_h) {
        vp_h = win_h;
        vp_w = (win_h * 4) / 3;
    }
    int vp_x = (win_w - vp_w) / 2;
    int vp_y = (win_h - vp_h) / 2;

    glViewport(vp_x, vp_y, vp_w, vp_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 1.0, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    if (!g_disp_disabled && g_vram_tex) {
        UploadDisplayTexture();

        float u1 = (float)clamp_int(g_disp_w, 1, VRAM_W) / (float)VRAM_W;
        float v1 = (float)clamp_int(g_disp_h, 1, VRAM_H) / (float)VRAM_H;

        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_vram_tex);
        glColor3f(1.0f, 1.0f, 1.0f);

        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, 0.0f);
        glTexCoord2f(u1, 0.0f); glVertex2f(1.0f, 0.0f);
        glTexCoord2f(u1, v1);   glVertex2f(1.0f, 1.0f);
        glTexCoord2f(0.0f, v1);   glVertex2f(0.0f, 1.0f);
        glEnd();

        glDisable(GL_TEXTURE_2D);
    }

    SwapBuffers(g_hdc);
    LimitFPS();
}

void CALLBACK_API GPUwriteStatus(uint32_t gdata) {
    uint32_t cmd = (gdata >> 24) & 0xFF;
    switch (cmd) {
    case 0x00:
        g_gpu_stat = 0x14802000;
        g_disp_disabled = 1;
        g_disp_x = g_disp_y = 0;
        g_disp_w = 320; g_disp_h = 240;
        g_disp_y1 = 16; g_disp_y2 = 256;
        g_disp_vres = g_disp_interlace = g_is_24bit = 0;
        g_tw_mask_x = g_tw_mask_y = g_tw_off_x = g_tw_off_y = 0;
        g_cmd_idx = g_xfer_mode = g_in_polyline = 0;
        break;
    case 0x01:
        g_cmd_idx = g_xfer_mode = g_in_polyline = 0;
        break;
    case 0x03:
        g_disp_disabled = (gdata & 1);
        g_gpu_stat = (g_gpu_stat & ~(1u << 23)) | ((gdata & 1) << 23);
        break;
    case 0x05:
        g_disp_x = gdata & 0x3FE;
        g_disp_y = (gdata >> 10) & 0x1FF;
        g_vram_dirty = 1;
        break;
    case 0x07:
        g_disp_y1 = gdata & 0x3FF;
        g_disp_y2 = (gdata >> 10) & 0x3FF;
        UpdateDisplayHeight();
        g_vram_dirty = 1;
        break;
    case 0x08: {
        static const int hres_table[4] = { 256, 320, 512, 640 };
        int hr2 = (gdata >> 6) & 1;
        int hr1 = gdata & 3;
        g_disp_w = hr2 ? 368 : hres_table[hr1];
        g_disp_vres = (gdata >> 2) & 1;
        g_disp_interlace = (gdata >> 5) & 1;
        g_is_24bit = (gdata >> 4) & 1;
        UpdateDisplayHeight();
        g_vram_dirty = 1;
        break;
    }
    default: break;
    }
}

uint32_t CALLBACK_API GPUreadStatus(void) {
    g_gpu_stat ^= 0x80000000u;
    return g_gpu_stat | 0x1C000000u;
}

void CALLBACK_API GPUwriteData(uint32_t gdata) { ProcessGP0Word(gdata); }
void CALLBACK_API GPUwriteDataMem(uint32_t* pMem, int iSize) {
    if (!pMem || iSize <= 0) return;
    for (int i = 0; i < iSize; i++) ProcessGP0Word(pMem[i]);
}
uint32_t CALLBACK_API GPUreadData(void) {
    if (g_xfer_mode == 2) {
        uint32_t lo = ReadVramHalfword();
        uint32_t hi = (g_xfer_mode == 2) ? ReadVramHalfword() : 0;
        return lo | (hi << 16);
    }
    return 0;
}
void CALLBACK_API GPUreadDataMem(uint32_t* pMem, int iSize) {
    if (!pMem || iSize <= 0) return;
    for (int i = 0; i < iSize; i++) pMem[i] = GPUreadData();
}

long CALLBACK_API GPUdmaChain(uint32_t* baseAddrL, uint32_t addr) {
    if (!baseAddrL) return 0;
    int safety = 0;
    addr &= 0x1FFFFCu;
    while (addr != 0xFFFFFFu && safety++ < 65536) {
        uint32_t header = baseAddrL[addr >> 2];
        uint32_t count = header >> 24;
        uint32_t next = header & 0xFFFFFFu;
        for (uint32_t i = 1; i <= count; i++) {
            ProcessGP0Word(baseAddrL[((addr + (i << 2)) & 0x1FFFFCu) >> 2]);
        }
        if (next == 0xFFFFFFu || (next & 0x800000u)) break;
        addr = next & 0x1FFFFCu;
    }
    return 0;
}

long CALLBACK_API GPUfreeze(uint32_t ulGetFreezeData, void* pF) {
    (void)ulGetFreezeData; (void)pF;
    return 1;
}