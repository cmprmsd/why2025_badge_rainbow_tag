// Fast-path scaler for WHY2025 badge: manual integer scaling in RGB565 (2x/3x/4x, and 0.5x).
// Using a gray colorkey (32,32,32) to avoid clashes with rainbow colors.
// R=rotate 90° cw, S=cycle scale presets, Esc/Q/Enter/Space/Back=exit.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_pixels.h>

#define SHEET_PATH     "APPS:[rainbow_tag]sheet.bmp"   // 24-bit Windows BMP, gray bg (32,32,32)
#define SPRITE_COLS    8
#define SPRITE_ROWS    4
#define SPRITE_FPS     24

// Transparent colorkey (exact gray)
#define KEY_R 32
#define KEY_G 32
#define KEY_B 32

// Scale presets cycled by 'S'
static const float SCALE_OPTIONS[] = { 0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 5.0f, 6.0f };
#define SCALE_COUNT   ((int)(sizeof(SCALE_OPTIONS)/sizeof(SCALE_OPTIONS[0])))
#define DEFAULT_SCALE 2.0f

#define START_SPEED_X  1.8f
#define START_SPEED_Y  1.4f
#define DT_SLEEP_MS    16     // ~60fps pacing

typedef struct {
    SDL_Window  *window;
    SDL_Surface *win;        // cached window surface
    int          screen_w;
    int          screen_h;

    SDL_Surface *sheet_base;     // window format, colorkey applied
    SDL_Surface *sheet_rot[4];   // 0=0°, 1=90°, 2=180°, 3=270°

    // Base sheet size and per-frame size (ints)
    int          tex_w, tex_h;
    int          fw, fh;         // per-frame source size (base orientation)

    int          cols, rows, frames;

    // Animation timing (integer ms)
    Uint32       anim_step_ms_base;  // nominal (from SPRITE_FPS)
    Uint32       anim_step_ms_eff;   // effective (adaptive for big scales)
    Uint32       ms_accum;
    Uint32       last_ms;
    int          frame;

    // Motion
    float        x, y, vx, vy;

    // State
    int          rot;          // 0..3 (quarters)
    float        scale;
    int          scale_idx;

    // Dirty-rect clear
    Uint32       black;
    SDL_Rect     prev_dst;
    bool         has_prev;

    // Fast-path flags
    bool         is565;        // window format is RGB565?
    Uint16       key565;       // gray colorkey mapped to 565
} App;

/* ----------------- helpers ----------------- */

static void log_sizes(const App *a, int draw_w, int draw_h) {
    SDL_Log("SPRITE: %dx%d  grid %dx%d  frame %dx%d  draw %dx%d  fps=%u  rot=%d*90  scale=%.2f  is565=%d",
            a->tex_w, a->tex_h, a->cols, a->rows, a->fw, a->fh, draw_w, draw_h,
            (unsigned)(1000u / a->anim_step_ms_base), a->rot, (double)a->scale, (int)a->is565);
}

static int surface_bpp(SDL_Surface *s) {
    const SDL_PixelFormatDetails *d = SDL_GetPixelFormatDetails(s->format);
    return d ? (int)d->bytes_per_pixel : 4;
}

static SDL_Surface* rotate90_cw(SDL_Surface *src) {
    int sw = src->w, sh = src->h;
    Uint32 fmt = src->format;
    SDL_Surface *dst = SDL_CreateSurface(sh, sw, fmt);
    if (!dst) return NULL;

    int bpp = surface_bpp(src);
    if (SDL_LockSurface(src) < 0)  { SDL_DestroySurface(dst); return NULL; }
    if (SDL_LockSurface(dst) < 0)  { SDL_UnlockSurface(src); SDL_DestroySurface(dst); return NULL; }

    const Uint8 *sp = (const Uint8*)src->pixels;
    Uint8 *dp = (Uint8*)dst->pixels;

    for (int y = 0; y < sh; ++y) {
        const Uint8 *srow = sp + y * src->pitch;
        for (int x = 0; x < sw; ++x) {
            const Uint8 *spx = srow + x * bpp;
            int nx = sh - 1 - y;
            int ny = x;
            Uint8 *dpx = dp + ny * dst->pitch + nx * bpp;
            memcpy(dpx, spx, bpp);
        }
    }

    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
    return dst;
}

static bool build_rotations_and_colorkey(App *a) {
    a->sheet_rot[0] = a->sheet_base;

    a->sheet_rot[1] = rotate90_cw(a->sheet_base);
    if (!a->sheet_rot[1]) { SDL_Log("rotate90_cw(90) failed: %s", SDL_GetError()); return false; }

    a->sheet_rot[2] = rotate90_cw(a->sheet_rot[1]);
    if (!a->sheet_rot[2]) { SDL_Log("rotate90_cw(180) failed: %s", SDL_GetError()); return false; }

    a->sheet_rot[3] = rotate90_cw(a->sheet_rot[2]);
    if (!a->sheet_rot[3]) { SDL_Log("rotate90_cw(270) failed: %s", SDL_GetError()); return false; }

    // Re-apply gray colorkey on rotated copies
    for (int i = 1; i < 4; ++i) {
        const SDL_PixelFormatDetails *d = SDL_GetPixelFormatDetails(a->sheet_rot[i]->format);
        Uint32 key = SDL_MapRGB(d, NULL, KEY_R, KEY_G, KEY_B);
        SDL_SetSurfaceColorKey(a->sheet_rot[i], true, key);
    }
    return true;
}

// For 0°/180°: cols=C, rows=R, frame=fw,fh. For 90°/270°: cols=R, rows=C, frame=fh,fw.
static inline void grid_for_rot(const App *a, int rot, int *cols, int *rows, int *fw, int *fh) {
    int r = rot & 3;
    if (r == 0 || r == 2) {
        if (cols) *cols = a->cols;
        if (rows) *rows = a->rows;
        if (fw)   *fw   = a->fw;
        if (fh)   *fh   = a->fh;
    } else {
        if (cols) *cols = a->rows;
        if (rows) *rows = a->cols;
        if (fw)   *fw   = a->fh;
        if (fh)   *fh   = a->fw;
    }
}

static inline void map_cell_for_rot(const App *a, int rot, int base_r, int base_c, int *out_r, int *out_c) {
    int C = a->cols, R = a->rows;
    int r = rot & 3, rr, cc;
    if (r == 0) { rr = base_r;              cc = base_c; }
    else if (r == 1) { rr = base_c;         cc = R - 1 - base_r; }
    else if (r == 2) { rr = R - 1 - base_r; cc = C - 1 - base_c; }
    else { rr = C - 1 - base_c;             cc = base_r; }
    if (out_r) *out_r = rr;
    if (out_c) *out_c = cc;
}

// Robust integer src rect: derive exact frame size from the rotated sheet; clamp last col/row.
static void get_src_for_frame_rot(const App *a, int idx, int rot, SDL_Rect *src) {
    SDL_Surface *sheet = a->sheet_rot[rot];
    int cols_r, rows_r, fw_r, fh_r;
    grid_for_rot(a, rot, &cols_r, &rows_r, &fw_r, &fh_r);

    int fw_i = sheet->w / cols_r;
    int fh_i = sheet->h / rows_r;

    int base_c = idx % a->cols;
    int base_r = idx / a->cols;
    int rr, cc; map_cell_for_rot(a, rot, base_r, base_c, &rr, &cc);

    int x = cc * fw_i;
    int y = rr * fh_i;

    int w = (cc == cols_r - 1) ? (sheet->w - x) : fw_i;
    int h = (rr == rows_r - 1) ? (sheet->h - y) : fh_i;

    if (x < 0) x = 0; if (y < 0) y = 0;
    if (w < 0) w = 0; if (h < 0) h = 0;
    if (x + w > sheet->w) w = sheet->w - x;
    if (y + h > sheet->h) h = sheet->h - y;

    src->x = x; src->y = y; src->w = w; src->h = h;
}

static void draw_size_for_rot(const App *a, int rot, int *dw, int *dh) {
    int fw_r, fh_r;
    grid_for_rot(a, rot, NULL, NULL, &fw_r, &fh_r);
    int w = (int)(fw_r * a->scale + 0.5f);
    int h = (int)(fh_r * a->scale + 0.5f);
    if (dw) *dw = w;
    if (dh) *dh = h;
}

static void clamp_xy_for_rot(App *a) {
    int dw, dh; draw_size_for_rot(a, a->rot, &dw, &dh);
    if (a->x < 0) a->x = 0;
    if (a->y < 0) a->y = 0;
    if (a->x + dw > a->screen_w) a->x = (float)(a->screen_w - dw);
    if (a->y + dh > a->screen_h) a->y = (float)(a->screen_h - dh);
}

static int find_closest_scale_index(float target) {
    int best = 0; float bestdiff = 1e9f;
    for (int i = 0; i < SCALE_COUNT; ++i) {
        float d = target - SCALE_OPTIONS[i]; if (d < 0) d = -d;
        if (d < bestdiff) { bestdiff = d; best = i; }
    }
    return best;
}

// Adaptive animation rate at large scales (keep things smooth when we touch lots of pixels)
static void update_anim_budget(App *a) {
    float s = a->scale;
    int mul = 1;
    if (s >= 3.5f) mul = 3;
    else if (s >= 2.5f) mul = 2;
    a->anim_step_ms_eff = a->anim_step_ms_base * (Uint32)mul;
}

/* ----------- super-fast RGB565 integer scaling paths ----------- */

// Return true if we handled the blit via fast path, false to let caller fallback.
static bool blit_colorkey_scale_fast_RGB565(SDL_Surface *src, const SDL_Rect *sr,
                                            SDL_Surface *dst, const SDL_Rect *dr,
                                            Uint16 key565, float scale)
{
    // Only when both are 565 and scale is 0.5, 2, 3, or 4
    const SDL_PixelFormatDetails *sd = SDL_GetPixelFormatDetails(src->format);
    const SDL_PixelFormatDetails *dd = SDL_GetPixelFormatDetails(dst->format);
    if (!sd || !dd || sd->bytes_per_pixel != 2 || dd->bytes_per_pixel != 2) return false;

    int iscale = 0;
    bool half = false;
    if      (scale == 2.0f) iscale = 2;
    else if (scale == 3.0f) iscale = 3;
    else if (scale == 4.0f) iscale = 4;
    else if (scale == 0.5f) half = true;
    else return false;

    if (SDL_LockSurface(src) < 0)  return false;
    if (SDL_LockSurface(dst) < 0)  { SDL_UnlockSurface(src); return false; }

    const Uint8 *sp = (const Uint8*)src->pixels;
    Uint8 *dp = (Uint8*)dst->pixels;

    const int spitch = src->pitch;
    const int dpitch = dst->pitch;

    if (!half) {
        // Upscale by integer factor
        for (int sy = 0; sy < sr->h; ++sy) {
            const Uint16 *srow = (const Uint16*)(sp + (sr->y + sy) * spitch) + sr->x;
            for (int vy = 0; vy < iscale; ++vy) {
                Uint16 *drow = (Uint16*)(dp + (dr->y + sy * iscale + vy) * dpitch) + dr->x;
                int dx = 0;
                for (int sx = 0; sx < sr->w; ++sx) {
                    Uint16 pix = srow[sx];
                    if (pix == key565) {
                        dx += iscale;     // skip transparent run
                    } else {
                        // replicate horizontally
                        for (int i = 0; i < iscale; ++i) {
                            drow[dx + i] = pix;
                        }
                        dx += iscale;
                    }
                }
            }
        }
    } else {
        // Downscale by 2: nearest (sample every other source pixel)
        for (int sy = 0; sy < sr->h; sy += 2) {
            const Uint16 *srow = (const Uint16*)(sp + (sr->y + sy) * spitch) + sr->x;
            Uint16 *drow = (Uint16*)(dp + (dr->y + (sy >> 1)) * dpitch) + dr->x;
            int dx = 0;
            for (int sx = 0; sx < sr->w; sx += 2) {
                Uint16 pix = srow[sx];
                if (pix != key565) drow[dx] = pix;
                dx += 1;
            }
        }
    }

    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
    return true;
}

/* ----------------- SDL callbacks ----------------- */

static SDL_AppResult app_key(SDL_Scancode code, App *a) {
    if (code == SDL_SCANCODE_ESCAPE || code == SDL_SCANCODE_Q ||
        code == SDL_SCANCODE_AC_BACK || code == SDL_SCANCODE_RETURN ||
        code == SDL_SCANCODE_SPACE) {
        SDL_Log("Exit key pressed (scancode=%d)", (int)code);
        return SDL_APP_SUCCESS;
    }
    if (code == SDL_SCANCODE_R) {
        a->rot = (a->rot + 1) & 3;
        a->has_prev = false;
        clamp_xy_for_rot(a);
        int dw, dh; draw_size_for_rot(a, a->rot, &dw, &dh);
        SDL_Log("Rotated to %d*90 cw; draw %dx%d (scale=%.2f)", a->rot, dw, dh, (double)a->scale);
        SDL_FillSurfaceRect(a->win, NULL, a->black);
        return SDL_APP_CONTINUE;
    }
    if (code == SDL_SCANCODE_S) {
        a->scale_idx = (a->scale_idx + 1) % SCALE_COUNT;
        a->scale     = SCALE_OPTIONS[a->scale_idx];
        update_anim_budget(a);
        a->has_prev = false;
        clamp_xy_for_rot(a);
        int dw, dh; draw_size_for_rot(a, a->rot, &dw, &dh);
        SDL_Log("Scale %.2f; draw %dx%d; anim step %ums",
                (double)a->scale, dw, dh, (unsigned)a->anim_step_ms_eff);
        SDL_FillSurfaceRect(a->win, NULL, a->black);
        return SDL_APP_CONTINUE;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    App *a = (App *)appstate;

    Uint32 now   = (Uint32)SDL_GetTicks();
    Uint32 delta = now - a->last_ms;
    a->last_ms   = now;

    // step animation with effective ms step
    a->ms_accum += delta;
    while (a->ms_accum >= a->anim_step_ms_eff) {
        a->ms_accum -= a->anim_step_ms_eff;
        a->frame = (a->frame + 1) % a->frames;
    }

    // move & bounce
    int dw, dh; draw_size_for_rot(a, a->rot, &dw, &dh);
    a->x += a->vx;
    a->y += a->vy;
    if (a->x < 0)                 { a->x = 0;                               a->vx = -a->vx; }
    if (a->x + dw > a->screen_w)  { a->x = (float)(a->screen_w - dw);       a->vx = -a->vx; }
    if (a->y < 0)                 { a->y = 0;                               a->vy = -a->vy; }
    if (a->y + dh > a->screen_h)  { a->y = (float)(a->screen_h - dh);       a->vy = -a->vy; }

    // Dirty-rect clear
    if (a->has_prev) {
        SDL_FillSurfaceRect(a->win, &a->prev_dst, a->black);
    }

    // src/dst
    SDL_Surface *sheet = a->sheet_rot[a->rot];
    SDL_Rect src; get_src_for_frame_rot(a, a->frame, a->rot, &src);
    SDL_Rect dst = { (int)a->x, (int)a->y, dw, dh };

    bool blit_ok = true;

    if (a->scale == 1.0f) {
        // Unscaled blit (colorkey path is fast)
        dst.w = src.w; dst.h = src.h;
        blit_ok = SDL_BlitSurface(sheet, &src, a->win, &dst);
    } else {
        // Fast integer 565 paths: 0.5x, 2x, 3x, 4x
        if (a->is565) {
            if (!blit_colorkey_scale_fast_RGB565(sheet, &src, a->win, &dst, a->key565, a->scale)) {
                // Fallback to SDL scaler for odd scales (1.5x, 2.5x, 3.5x, 5x, 6x)
                blit_ok = SDL_BlitSurfaceScaled(sheet, &src, a->win, &dst, SDL_SCALEMODE_NEAREST);
            }
        } else {
            blit_ok = SDL_BlitSurfaceScaled(sheet, &src, a->win, &dst, SDL_SCALEMODE_NEAREST);
        }
    }

    if (!blit_ok) {
        SDL_Log("Blit failed: %s (src %d,%d %dx%d  dst %d,%d %dx%d)",
                SDL_GetError(), src.x, src.y, src.w, src.h, dst.x, dst.y, dst.w, dst.h);
    }

    // Track dirty rect
    a->prev_dst = dst;
    a->has_prev = true;

    // Update (full surface; safe on all SDL3 builds)
    SDL_UpdateWindowSurface(a->window);

    SDL_Delay(DT_SLEEP_MS);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
    (void)argc; (void)argv;
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_DEBUG);

    if (!SDL_SetAppMetadata("Tag Bounce (fast RGB565 scaler, gray key)", "3.2", "org.why2025.badge.tag_bounce"))
        return SDL_APP_FAILURE;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    App *a = (App *)SDL_calloc(1, sizeof(App));
    if (!a) return SDL_APP_FAILURE;
    *appstate = a;

    a->window = SDL_CreateWindow("Tag Bounce (fast)", 720, 720, SDL_WINDOW_FULLSCREEN);
    if (!a->window) { SDL_Log("CreateWindow failed: %s", SDL_GetError()); return SDL_APP_FAILURE; }

    // Cache window surface once
    a->win = SDL_GetWindowSurface(a->window);
    if (!a->win) { SDL_Log("GetWindowSurface failed at init: %s", SDL_GetError()); return SDL_APP_FAILURE; }
    a->screen_w = a->win->w;
    a->screen_h = a->win->h;

    const SDL_PixelFormatDetails *wd = SDL_GetPixelFormatDetails(a->win->format);
    a->black = SDL_MapRGB(wd, NULL, 0, 0, 0);
    a->is565 = (wd && wd->format == SDL_PIXELFORMAT_RGB565 && wd->bytes_per_pixel == 2);

    SDL_Log("Using window surface %dx%d format=%s bpp=%d (is565=%d)",
            a->screen_w, a->screen_h, SDL_GetPixelFormatName(a->win->format),
            wd ? (int)wd->bytes_per_pixel : -1, (int)a->is565);

    // Clear once
    SDL_FillSurfaceRect(a->win, NULL, a->black);

    // Load BMP
    SDL_Surface *surf = SDL_LoadBMP(SHEET_PATH);
    if (!surf) { SDL_Log("SDL_LoadBMP failed for %s: %s", SHEET_PATH, SDL_GetError()); return SDL_APP_FAILURE; }

    // Set gray colorkey on the source (BMP format)
    const SDL_PixelFormatDetails *sd = SDL_GetPixelFormatDetails(surf->format);
    Uint32 key_src = SDL_MapRGB(sd, NULL, KEY_R, KEY_G, KEY_B);
    SDL_SetSurfaceColorKey(surf, true, key_src);

    // Convert to window format + reapply gray colorkey
    a->sheet_base = SDL_ConvertSurface(surf, a->win->format);
    SDL_DestroySurface(surf);
    if (!a->sheet_base) { SDL_Log("SDL_ConvertSurface failed: %s", SDL_GetError()); return SDL_APP_FAILURE; }

    const SDL_PixelFormatDetails *cd = SDL_GetPixelFormatDetails(a->sheet_base->format);
    Uint32 key_conv = SDL_MapRGB(cd, NULL, KEY_R, KEY_G, KEY_B);
    SDL_SetSurfaceColorKey(a->sheet_base, true, key_conv);

    // If RGB565, cache the 16-bit key for the fast path
    if (a->is565) {
        a->key565 = (Uint16)key_conv;
    }

    // Base sizes (ints)
    a->tex_w = a->sheet_base->w;
    a->tex_h = a->sheet_base->h;
    a->cols  = SPRITE_COLS;
    a->rows  = SPRITE_ROWS;
    a->frames = a->cols * a->rows;
    a->fw = a->tex_w / a->cols;
    a->fh = a->tex_h / a->rows;

    // Build rotated variants and reapply gray key
    if (!build_rotations_and_colorkey(a)) return SDL_APP_FAILURE;

    a->anim_step_ms_base = (Uint32)(1000u / (Uint32)SPRITE_FPS);
    a->anim_step_ms_eff  = a->anim_step_ms_base;
    a->ms_accum = 0;
    a->last_ms  = (Uint32)SDL_GetTicks();
    a->frame = 0;
    a->rot = 0;

    // Init scale to DEFAULT_SCALE
    a->scale_idx = find_closest_scale_index(DEFAULT_SCALE);
    a->scale     = SCALE_OPTIONS[a->scale_idx];
    update_anim_budget(a);

    // Start centered
    int dw, dh; draw_size_for_rot(a, a->rot, &dw, &dh);
    a->x = (a->screen_w - dw) * 0.5f;
    a->y = (a->screen_h - dh) * 0.5f;
    a->vx = START_SPEED_X;
    a->vy = START_SPEED_Y;

    a->has_prev = false;
    log_sizes(a, dw, dh);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *ev) {
    App *a = (App *)appstate;
    if (ev->type == SDL_EVENT_QUIT)     return SDL_APP_SUCCESS;
    if (ev->type == SDL_EVENT_KEY_DOWN) return app_key(ev->key.scancode, a);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    (void)result;
    if (!appstate) return;
    App *a = (App *)appstate;

    for (int i = 1; i < 4; ++i) if (a->sheet_rot[i]) SDL_DestroySurface(a->sheet_rot[i]);
    if (a->sheet_base) SDL_DestroySurface(a->sheet_base);

    if (a->window) SDL_DestroyWindow(a->window);
    SDL_free(a);
    SDL_Quit();
}
