/*
 * Verifie que le tramage Floyd-Steinberg conserve la moyenne.
 *
 *   gcc -O2 -fopenmp test_dither.c -o test_dither -lz -lm && ./test_dither
 *
 * Palette a 2 couleurs (noir/blanc), image uniformement grise a 128 :
 *   - sans tramage, tout tombe sur la meme couleur -> moyenne 0 ou 255
 *   - avec tramage, l'erreur diffusee doit ramener la moyenne a ~128
 * C'est exactement la propriete pour laquelle on ajoute le tramage.
 */

#define main mcmm_main
#include "mcmm.c"
#undef main

#include <assert.h>

static double mean_luma(const uint8_t *ids) {
    long sum = 0;
    for (int i = 0; i < MAP_PIXELS; i++) {
        sum += pal_rgb[ids[i]][0]; /* canal R, la palette de test est grise */
    }
    return (double)sum / MAP_PIXELS;
}

int main(void) {
    g_log = stdout;

    palette_count = 2;
    palette[0] = (PaletteEntry){ 0, 0, 0, 0 };
    palette[1] = (PaletteEntry){ 255, 255, 255, 4 };
    for (int i = 0; i < palette_count; i++) {
        pal_rgb[palette[i].mc_id][0] = palette[i].r;
        pal_rgb[palette[i].mc_id][1] = palette[i].g;
        pal_rgb[palette[i].mc_id][2] = palette[i].b;
    }
    build_full_lut();

    static uint8_t gray[MAP_SIZE * MAP_SIZE * 3];
    memset(gray, 128, sizeof(gray));
    uint8_t out[MAP_PIXELS];

    g_dither = 0;
    convert_tile(gray, MAP_SIZE * 3, 0, 0, out);
    double flat = mean_luma(out);
    printf("sans tramage : moyenne = %.1f (attendu 0 ou 255)\n", flat);
    assert(flat < 1.0 || flat > 254.0);

    g_dither = 1;
    convert_tile(gray, MAP_SIZE * 3, 0, 0, out);
    double dithered = mean_luma(out);
    printf("avec tramage : moyenne = %.1f (attendu ~128)\n", dithered);
    assert(dithered > 124.0 && dithered < 132.0);

    /* Aucun id hors palette. */
    for (int i = 0; i < MAP_PIXELS; i++) {
        assert(out[i] == 0 || out[i] == 4);
    }

    printf("OK\n");
    return 0;
}
