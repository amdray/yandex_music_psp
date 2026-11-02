#ifndef YM_UI_GLOW_PLUGIN_H
#define YM_UI_GLOW_PLUGIN_H

#include "ui/ui_glow_bg.h"

#define GLOW_W 240
#define GLOW_H 136
#define GLOW_PITCH 256
#define GLOW_TEX_H 256
#define GLOW_PALETTE 4
#define WG_MAXC 120
#define WG_MAXR 68

/* Входы от UI и общий выход: текстура либо цвета вершин сетки. */
typedef struct {
    unsigned frame, pulse_until;
    int playing, liked;
    float like_clock;
    unsigned char pal[GLOW_PALETTE][3], avg[3];
    unsigned char *texture;
    unsigned int *rgb;
    int cols, rows, draw_cols, draw_rows;
} GlowState;

typedef struct {
    LocaleKey name;
    int textured;
    float scrim_floor;
    void (*update)(float dt);
    void (*draw)(float x, float y, float w, float h);
} GlowPlugin;

typedef struct {
    unsigned int color;
    short x, y, z, pad;
} GlowMeshVert;

extern GlowState g_glow;
extern const GlowPlugin glow_classic, glow_strands, glow_web;
extern const GlowPlugin glow_spectrum, glow_scope, glow_radial;

void glow_grid_lod_feed(float ms);
void glow_grid_draw(float x, float y, float w, float h);
void glow_classic_update(float dim);
void glow_classic_set_palette(void);

#endif
