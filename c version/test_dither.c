/*
 * Verifie le tramage Floyd-Steinberg : la moyenne, et l'absence de couture
 * entre les cartes.
 *
 *   gcc -O2 -fopenmp test_dither.c -o test_dither -lz -lm && ./test_dither
 *
 * Palette a 2 couleurs (noir/blanc), image uniformement grise a 128 :
 *   - sans tramage, tout tombe sur la meme couleur -> moyenne 0 ou 255
 *   - avec tramage, l'erreur diffusee doit ramener la moyenne a ~128
 * C'est exactement la propriete pour laquelle on ajoute le tramage.
 *
 * La couture : un tramage fait tuile par tuile repart d'une erreur nulle a
 * chaque bord de carte, donc le motif redemarre tous les 128 px et la jonction
 * se voit en jeu. On le mesure en comparant a une passe raster de reference,
 * ecrite ici sans aucune notion de tuile : si convert_frame_dithered decoupe
 * avant de tramer, elle s'en ecarte. La meme comparaison attrape aussi une
 * erreur d'adressage dans le rangement par tuile, qui est la partie risquee.
 *
 * L'entree doit etre une rampe, pas un aplat : sur un gris uniforme le motif
 * de Floyd-Steinberg est periodique de periode 2, et comme 128 est pair les
 * deux versions tombent d'accord par accident.
 */

#define main mcmm_main
#include "mcmm.c"
#undef main

#include <assert.h>

#define MAPS_W 2
#define MAPS_H 2
#define FRAME_W (MAPS_W * MAP_SIZE)
#define FRAME_H (MAPS_H * MAP_SIZE)

static double mean_luma(const uint8_t *ids, size_t n) {
    long sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += pal_rgb[ids[i]][0]; /* canal R, la palette de test est grise */
    }
    return (double)sum / (double)n;
}

/* Floyd-Steinberg naif sur une image w*h, sortie a plat en out[y*w + x].
 * Aucune notion de tuile : c'est la reference. */
static void reference_dither(const uint8_t *rgb, int stride, int w, int h,
                             uint8_t *out) {
    int *err = (int *)calloc((size_t)(w + 2) * 3 * 2, sizeof(int));
    assert(err);
    int *cur = err, *nxt = err + (size_t)(w + 2) * 3;

    for (int y = 0; y < h; y++) {
        const uint8_t *line = rgb + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++) {
            int c[3];
            for (int k = 0; k < 3; k++) {
                int v = line[x * 3 + k] + cur[(x + 1) * 3 + k];
                c[k] = v < 0 ? 0 : (v > 255 ? 255 : v);
            }
            uint8_t id = color_lut[c[0]][c[1]][c[2]];
            out[(size_t)y * w + x] = id;
            for (int k = 0; k < 3; k++) {
                int e = c[k] - pal_rgb[id][k];
                cur[(x + 2) * 3 + k] += e * 7 / 16;
                nxt[(x    ) * 3 + k] += e * 3 / 16;
                nxt[(x + 1) * 3 + k] += e * 5 / 16;
                nxt[(x + 2) * 3 + k] += e     / 16;
            }
        }
        int *done = cur;
        cur = nxt;
        nxt = done;
        memset(nxt, 0, (size_t)(w + 2) * 3 * sizeof(int));
    }
    free(err);
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

    static uint8_t ids[MAPS_W * MAPS_H * MAP_PIXELS];

    /* --- La moyenne, sur un aplat gris --- */
    static uint8_t gray[FRAME_W * FRAME_H * 3];
    memset(gray, 128, sizeof(gray));

    g_dither = 0;
    frame_to_ids(gray, FRAME_W * 3, MAPS_W, MAPS_H, ids);
    double flat = mean_luma(ids, sizeof(ids));
    printf("sans tramage : moyenne = %.1f (attendu 0 ou 255)\n", flat);
    assert(flat < 1.0 || flat > 254.0);

    g_dither = 1;
    frame_to_ids(gray, FRAME_W * 3, MAPS_W, MAPS_H, ids);
    double dithered = mean_luma(ids, sizeof(ids));
    printf("avec tramage : moyenne = %.1f (attendu ~128)\n", dithered);
    assert(dithered > 124.0 && dithered < 132.0);

    /* Aucun id hors palette. */
    for (size_t i = 0; i < sizeof(ids); i++) {
        assert(ids[i] == 0 || ids[i] == 4);
    }

    /* --- La couture, sur une rampe diagonale --- */
    static uint8_t ramp[FRAME_W * FRAME_H * 3];
    for (int y = 0; y < FRAME_H; y++) {
        for (int x = 0; x < FRAME_W; x++) {
            int v = (x + y) * 255 / (FRAME_W + FRAME_H - 2);
            uint8_t *p = ramp + ((size_t)y * FRAME_W + x) * 3;
            p[0] = p[1] = p[2] = (uint8_t)v;
        }
    }

    static uint8_t ref[FRAME_W * FRAME_H];
    reference_dither(ramp, FRAME_W * 3, FRAME_W, FRAME_H, ref);
    frame_to_ids(ramp, FRAME_W * 3, MAPS_W, MAPS_H, ids);

    long diff = 0;
    for (int y = 0; y < FRAME_H; y++) {
        for (int x = 0; x < FRAME_W; x++) {
            size_t tiled = (size_t)(y / MAP_SIZE) * MAPS_W * MAP_PIXELS
                         + (size_t)(x / MAP_SIZE) * MAP_PIXELS
                         + (size_t)(y % MAP_SIZE) * MAP_SIZE + (x % MAP_SIZE);
            if (ids[tiled] != ref[(size_t)y * FRAME_W + x]) diff++;
        }
    }
    printf("passe pleine frame vs reference : %ld pixel(s) d'ecart\n", diff);
    assert(diff == 0);

    printf("OK\n");
    return 0;
}
