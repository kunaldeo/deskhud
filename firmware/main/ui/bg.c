// Full-screen backdrop: a dark base with soft colored light pools, rendered on the device so it
// follows the theme and accent color. Gaussians are separable, so each pool costs one multiply
// per pixel; ordered dithering hides RGB565 banding.
#include <math.h>
#include <string.h>
#include "board.h"
#include "esp_heap_caps.h"
#include "settings.h"
#include "theme.h"

typedef struct {
    float cx, cy, r;  // center (fraction of screen), radius (fraction of width)
    uint32_t color;
    float strength;
} pool_t;

static lv_image_dsc_t s_dsc;
static uint16_t *s_px;

static void unpack(uint32_t c, float *r, float *g, float *b)
{
    *r = (c >> 16) & 0xff;
    *g = (c >> 8) & 0xff;
    *b = c & 0xff;
}

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    float ar, ag, ab, br, bg, bb;
    unpack(a, &ar, &ag, &ab);
    unpack(b, &br, &bg, &bb);
    return ((uint32_t)(ar + (br - ar) * t) << 16) | ((uint32_t)(ag + (bg - ag) * t) << 8) | (uint32_t)(ab + (bb - ab) * t);
}

const lv_image_dsc_t *bg_render(void)
{
    const int W = BOARD_W, H = BOARD_H;
    if (!s_px) s_px = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    if (!s_px) return NULL;
    settings_t s = settings_get();
    uint32_t base, acc = s.accent;  // paper / graphite pick up the accent; midnight / aurora have fixed light
    pool_t pools[3];
    float vignette = 0.55f;
    bool grain = false;
    if (!strcmp(s.theme, "newsprint")) {  // ivory stock with paper grain, no lighting
        base = 0xf0eee6;
        pools[0] = (pool_t){.5f, .5f, .5f, base, 0};
        pools[1] = pools[0];
        pools[2] = pools[0];
        vignette = 0.06f;
        grain = true;
    } else if (!strcmp(s.theme, "paper")) {  // warm stock, a soft daylight from the top left
        base = 0xf7f5f0;
        pools[0] = (pool_t){.15f, -.10f, .60f, 0xffffff, .70f};
        pools[1] = (pool_t){.90f, 1.10f, .50f, 0xefe9dd, .60f};
        pools[2] = (pool_t){.5f, .5f, .1f, base, 0};
        vignette = 0.06f;
    } else if (!strcmp(s.theme, "ink")) {  // warm charcoal, a faint lamp glow
        base = 0x141311;
        pools[0] = (pool_t){.20f, -.10f, .60f, 0x24221d, .80f};
        pools[1] = (pool_t){.85f, 1.10f, .50f, 0x1b1a17, .60f};
        pools[2] = (pool_t){.5f, .5f, .1f, base, 0};
        vignette = 0.35f;
    } else if (!strcmp(s.theme, "graphite")) {
        base = 0x0c0d10;
        pools[0] = (pool_t){.5f, -.1f, .7f, 0x2a2b31, .8f};
        pools[1] = (pool_t){.1f, .1f, .35f, mix(base, acc, .18f), .6f};
        pools[2] = (pool_t){.5f, 1.1f, .6f, 0x18181b, .6f};
    } else if (!strcmp(s.theme, "aurora")) {
        base = 0x030a0b;
        pools[0] = (pool_t){.18f, -.05f, .55f, 0x0f9f6e, .85f};  // green band
        pools[1] = (pool_t){.62f, .05f, .45f, 0x0e8f9c, .70f};   // teal
        pools[2] = (pool_t){.95f, .85f, .45f, 0x8b2fd6, .65f};   // violet
    } else {  // midnight: fixed indigo palette, independent of the accent
        base = 0x050812;
        pools[0] = (pool_t){.10f, .00f, .50f, 0x1e3a8a, .70f};   // blue
        pools[1] = (pool_t){.90f, .10f, .42f, 0x3730a3, .60f};   // indigo
        pools[2] = (pool_t){.55f, 1.10f, .55f, 0x1e1b4b, .55f};  // deep violet
    }
    // Row by row: per-pool horizontal / vertical falloff tables, then blend each pool into the row.
    typedef struct {
        float ex[3][BOARD_W], ey[3][BOARD_H], rr[BOARD_W], rg[BOARD_W], rb[BOARD_W];
    } scratch_t;  // ~31 KB: PSRAM, internal RAM is precious
    scratch_t *sc = heap_caps_malloc(sizeof(scratch_t), MALLOC_CAP_SPIRAM);
    if (!sc) return NULL;
    float (*ex)[BOARD_W] = sc->ex, (*ey)[BOARD_H] = sc->ey, *rr = sc->rr, *rg = sc->rg, *rb = sc->rb;
    float pc[3][3];
    for (int p = 0; p < 3; p++) {
        float rx = pools[p].r * W, ry = pools[p].r * W * 0.8f;
        for (int x = 0; x < W; x++) {
            float d = (x - pools[p].cx * W) / rx;
            ex[p][x] = expf(-d * d * 2.2f);
        }
        for (int y = 0; y < H; y++) {
            float d = (y - pools[p].cy * H) / ry;
            ey[p][y] = expf(-d * d * 2.2f) * pools[p].strength * 0.9f;
        }
        unpack(pools[p].color, &pc[p][0], &pc[p][1], &pc[p][2]);
    }
    float br, bgc, bb;
    unpack(base, &br, &bgc, &bb);
    static const int8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            rr[x] = br;
            rg[x] = bgc;
            rb[x] = bb;
        }
        for (int p = 0; p < 3; p++) {
            float ky = ey[p][y];
            if (ky < 0.002f) continue;
            for (int x = 0; x < W; x++) {
                float k = ex[p][x] * ky;
                rr[x] += k * (pc[p][0] - rr[x]);
                rg[x] += k * (pc[p][1] - rg[x]);
                rb[x] += k * (pc[p][2] - rb[x]);
            }
        }
        float vy = (float)y / H - 0.5f;
        uint16_t *row = s_px + y * W;
        for (int x = 0; x < W; x++) {
            float vx = (float)x / W - 0.5f;
            float v = sqrtf(vx * vx * 1.2f + vy * vy) - 0.35f;
            float m = 1.0f - (v > 0 ? v : 0) * vignette;
            float t = bayer[y & 3][x & 3] / 16.0f - 0.5f;
            if (grain) {  // cheap hash noise: paper fibres
                uint32_t h = (uint32_t)(x * 374761393u + y * 668265263u);
                h = (h ^ (h >> 13)) * 1274126177u;
                t += ((int)((h >> 24) & 15) - 7.5f) * 0.55f;
            }
            int r = (int)(rr[x] * m + t * 8), g = (int)(rg[x] * m + t * 4), b = (int)(rb[x] * m + t * 8);
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            row[x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        }
    }
    heap_caps_free(sc);
    s_dsc = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = W, .h = H, .stride = W * 2},
        .data_size = W * H * 2,
        .data = (const uint8_t *)s_px,
    };
    return &s_dsc;
}
