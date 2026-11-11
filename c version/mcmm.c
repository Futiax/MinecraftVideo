// MinecraftVideo -- prototype C natif, remplace MCMM_client.pyx
//
// Dependances (compilation) : zlib, OpenMP
// Dependances (execution)   : ffmpeg et ffprobe dans le PATH
//   Linux   : sudo apt install ffmpeg   (ou equivalent dnf/pacman)
//   Windows : https://www.gyan.dev/ffmpeg/builds/ (ajouter bin/ au PATH)
//
// Compilation :
//   cmake -S . -B build && cmake --build build --config Release
//   gcc -O2 -fopenmp mcmm.c -o mcmm -lz -lm

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <process.h>
#define mkdir_p(path) _mkdir(path)
#define mcmm_getpid   _getpid
#else
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#define mcmm_getpid   getpid
#endif

#include <omp.h>
#include <zlib.h>

#include "vmc_data.h"

// ==========================================================================
// Constantes
// ==========================================================================

#define MAP_SIZE        128
#define MAP_PIXELS      (MAP_SIZE * MAP_SIZE)   // 16384
#define MAX_PALETTE     256
#define NUM_SHADES      4
#define DEFAULT_COLOR_ID 41
#define DATA_VERSION    4440
#define NUM_THREADS     4

// Nuances Minecraft pour chaque couleur de base
static const int SHADES[NUM_SHADES] = { 180, 220, 255, 135 };

// Protocole de streaming (--stream)
//
// En-tete, 16 octets big-endian, identique en v1 et v2 :
//   "MCMM" | u16 version | u16 w | u16 h | u16 fps | u32 reserve
//
// Puis, par frame :
//   v1 : w*h tuiles de 16384 octets (128x128 index de palette, ligne 0 = haut)
//   v2 : s64 pts_us  puis  les memes w*h tuiles
//
// pts_us est le PTS de la frame dans la source, en microsecondes, ABSOLU :
// une frame a 12,5 s porte le meme PTS qu'on l'ait atteinte avec ou sans
// --seek. C'est la seule horloge partagee entre mcmm et le ffmpeg audio que
// le plugin lance en parallele sur la meme source -- un compteur de frames ne
// marche que sur un fichier, pas sur un direct ou les deux connexions
// rejoignent le flux a des endroits differents.
//
// Avec --stats (/vmc debug du plugin), le u32 reserve de l'en-tete vaut
// STREAM_STATS, et chaque frame porte STREAM_STATS u32 entre pts_us et ses tuiles,
// les temps de cette frame en microsecondes. Dans le flux plutot que dans un fichier :
// le plugin les ecrit sur la meme ligne que ses propres temps, sans synchro.
//   0 ffmpeg     attente de ffmpeg (frame + PTS)
//   1 write      ecriture de la frame PRECEDENTE (bloque = mcmm attend le plugin)
//   2 cpu        CPU du process depuis la frame d'avant, tous threads (POSIX)
//   3 convert    conversion entiere ; le detail qui suit n'existe qu'en --data
//   4 dct        5 encode (quantif + Huffman a q = 0)
//   6 search     dichotomie si q = 0 ne tient pas, troncatures
//   7 assemble   en-tete + macroblocs bout a bout
//   8 symbols    mots -> symboles -> mur -> tuiles
//   9 q         10 bits du flux
#define STREAM_STATS        11
#define STREAM_MAGIC        "MCMM"
#define STREAM_VERSION      2
#define STREAM_HEADER_SIZE  16

// Flux de log : stdout en mode normal, stderr en mode --stream
// (stdout est reserve au flux binaire dans ce dernier cas)
static FILE *g_log = NULL;

// ==========================================================================
// Table de lookup 256^3 RGB -> ID couleur Minecraft
// ~16 Mo statique -- remplace PIL.Image.quantize() et l'ancienne LUT exacte
// ==========================================================================

static uint8_t color_lut[256][256][256];

// Table inverse mc_id -> RGB effectif, necessaire au tramage (l'erreur de
// quantification est la difference entre le pixel voulu et la couleur reelle
// de la palette qu'on lui a substituee).
static uint8_t pal_rgb[256][3];

// Tramage Floyd-Steinberg (--dither) : desactive par defaut
static int g_dither = 0;

// Mode strict (--strict, exclusif avec --dither) : aucune couleur inventee par la
// mise a l'echelle. Pour le pixel art et les QR, ou un module de 5 px doit rester
// 5 px pleins et pas 4 px + un lisere gris.
//
// POURQUOI la mise a l'echelle est faite ICI et pas par ffmpeg : le scale par defaut
// (bicubique) cree des gris a chaque bord, meme a x2 exact (22 couleurs de palette
// au lieu de 9 sur une mire QR). Et flags=neighbor ne suffit pas : en REDUCTION,
// swscale moyenne quand meme -- 274 couleurs nouvelles sur la mire divisee par 2,
// 7589 sur du bruit, avec bitexact, accurate_rnd, sws_dither=none ou en gbrp
// (verifie sur ffmpeg 8.0). En --strict ffmpeg sort donc chaque frame a sa taille
// d'ORIGINE, en PPM (l'en-tete donne la taille : pas de ffprobe, et une video
// pivotee arrive deja tournee), et strict_scale() la place.
// LIMITE : l'encodeur ppm fige la taille de la premiere frame. Une source dont la
// resolution change en cours de route (TS concatenes, changement de variante HLS)
// est redimensionnee par ffmpeg vers cette premiere taille, en bicubique : pour
// ces frames-la, plus de garantie.
static int g_strict = 0;

// Mode --data (avec --stream, exclusif avec --dither et --strict) : le mur porte
// l'image codee en DCT au lieu de l'image quantifiee. Voir data_frame_to_ids().
static int g_data = 0;

// --stats : voir le protocole de streaming.
static int g_stats = 0;

// Plus grand cote de source accepte en --strict : la frame entiere transite par le
// pipe et par la memoire avant reduction (8192^2 = 192 Mo en rgb24).
#define STRICT_MAX_SIDE 8192

// Palette : couleurs effectives apres application des nuances
typedef struct {
    uint8_t r, g, b;
    uint8_t mc_id;      // idx_base * 4 + shade_idx
} PaletteEntry;

static PaletteEntry palette[MAX_PALETTE * NUM_SHADES];
static int palette_count = 0;

// ==========================================================================
// Parsing JSON minimal pour preset_color_list.json
// (pas de dependance externe -- le format est simple et fixe)
// ==========================================================================

static int parse_hex(const char *hex, int *r, int *g, int *b) {
    if (*hex == '#') hex++;
    if (strlen(hex) < 6) return -1;
    char buf[3] = {0};
    buf[0] = hex[0]; buf[1] = hex[1]; *r = (int)strtol(buf, NULL, 16);
    buf[0] = hex[2]; buf[1] = hex[3]; *g = (int)strtol(buf, NULL, 16);
    buf[0] = hex[4]; buf[1] = hex[5]; *b = (int)strtol(buf, NULL, 16);
    return 0;
}

static int load_palette(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Erreur: impossible d'ouvrir %s\n", path); return -1; }

    char line[256];
    palette_count = 0;

    while (fgets(line, sizeof(line), f)) {
        // Cherche les lignes du type :  "42": "#ff00aa"
        char *quote1 = strchr(line, '"');
        if (!quote1) continue;
        char *quote2 = strchr(quote1 + 1, '"');
        if (!quote2) continue;

        *quote2 = '\0';
        int idx = atoi(quote1 + 1);
        *quote2 = '"';

        char *hash = strchr(quote2 + 1, '#');
        if (!hash) continue;

        int r, g, b;
        if (parse_hex(hash, &r, &g, &b) != 0) continue;

        // Une entree du JSON donne NUM_SHADES entrees de palette : Minecraft
        // stocke la nuance dans les 2 bits de poids faible de l'ID.
        for (int s = 0; s < NUM_SHADES; s++) {
            int sr = (r * SHADES[s]) / 255; if (sr > 255) sr = 255;
            int sg = (g * SHADES[s]) / 255; if (sg > 255) sg = 255;
            int sb = (b * SHADES[s]) / 255; if (sb > 255) sb = 255;

            palette[palette_count].r     = (uint8_t)sr;
            palette[palette_count].g     = (uint8_t)sg;
            palette[palette_count].b     = (uint8_t)sb;
            palette[palette_count].mc_id = (uint8_t)(idx * 4 + s);
            palette_count++;
        }
    }
    fclose(f);
    for (int i = 0; i < palette_count; i++) {
        pal_rgb[palette[i].mc_id][0] = palette[i].r;
        pal_rgb[palette[i].mc_id][1] = palette[i].g;
        pal_rgb[palette[i].mc_id][2] = palette[i].b;
    }
    fprintf(g_log, "Palette chargee: %d couleurs (%d base x %d nuances)\n",
            palette_count, palette_count / NUM_SHADES, NUM_SHADES);
    return 0;
}

// Pre-calcule la LUT complete -- nearest-neighbor dans l'espace RGB.
// Le calcul nu : la journalisation et le cache sont dans prepare_lut().
static void build_full_lut(void) {
    #pragma omp parallel for schedule(dynamic, 4) num_threads(NUM_THREADS)
    for (int r = 0; r < 256; r++) {
        for (int g = 0; g < 256; g++) {
            for (int b = 0; b < 256; b++) {
                int best_dist = INT32_MAX;
                uint8_t best_id = DEFAULT_COLOR_ID;

                for (int i = 0; i < palette_count; i++) {
                    int dr = r - palette[i].r;
                    int dg = g - palette[i].g;
                    int db = b - palette[i].b;
                    int dist = dr * dr + dg * dg + db * db;
                    if (dist < best_dist) {
                        best_dist = dist;
                        best_id = palette[i].mc_id;
                    }
                }
                color_lut[r][g][b] = best_id;
            }
        }
    }
}

// ==========================================================================
// Cache disque de la LUT
//
// La construire coute ~3,6 s a chaque lancement. Sur une VOD c'est paye une
// fois et ca passe. Mais VMC relance mcmm par tranche pour lire un direct : 3,6
// s toutes les 30 s, c'est 12 % de retard permanent et cumulatif, la lecture
// decroche du direct et ne s'en remet jamais. On range donc la LUT a cote du
// JSON de palette.
//
// La validite ne repose PAS sur un hash. Le fichier embarque la palette
// effective -- celle d'apres l'application des nuances -- et on la compare octet
// par octet a celle qu'on vient de parser. Une palette editee, un SHADES
// retouche, un NUM_SHADES different : tout cela change ces octets. Aucune
// collision n'est possible, la ou un hash laisse une probabilite de resservir
// une LUT etrangere, c'est-a-dire des couleurs fausses en silence.
// LUT_CACHE_VERSION couvre ce que la palette ne dit pas : un changement
// d'algorithme (metrique de distance, couleur par defaut).
// ==========================================================================

#define LUT_CACHE_MAGIC     "MCLU"
#define LUT_CACHE_VERSION   1u
#define LUT_BYTES           (256 * 256 * 256)

_Static_assert(sizeof(PaletteEntry) == 4,
               "la palette part telle quelle dans le cache, sans remplissage");

static void put_u32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t get_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

// "<palette>.lut" : un cache par fichier de palette. Le nom suit le JSON, donc
// deux palettes distinctes ne se marchent jamais dessus.
static int lut_cache_path(char *buf, size_t cap, const char *palette_path) {
    int n = snprintf(buf, cap, "%s.lut", palette_path);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

// Remplit color_lut et renvoie 0 si le cache correspond exactement a la palette
// en memoire. Absent, tronque, d'une autre version, d'une autre palette, plus
// long que prevu : -1, l'appelant reconstruit. Muet -- au premier lancement
// l'absence de cache est le cas normal, pas un incident.
static int load_lut_cache(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t head[12];
    size_t count = (size_t)palette_count;
    int ok = fread(head, 1, sizeof(head), f) == sizeof(head)
          && memcmp(head, LUT_CACHE_MAGIC, 4) == 0
          && get_u32_be(head + 4) == LUT_CACHE_VERSION
          && get_u32_be(head + 8) == (uint32_t)palette_count;

    if (ok) {
        PaletteEntry stored[MAX_PALETTE * NUM_SHADES];
        ok = fread(stored, sizeof(PaletteEntry), count, f) == count
          && memcmp(stored, palette, count * sizeof(PaletteEntry)) == 0;
    }
    if (ok) {
        ok = fread(color_lut, 1, LUT_BYTES, f) == LUT_BYTES;
    }
    if (ok) {
        // Rien ne doit suivre la LUT : un fichier plus long vient d'un autre
        // format et on vient de le lire de travers.
        ok = fgetc(f) == EOF;
    }
    fclose(f);
    return ok ? 0 : -1;
}

// Ecrit le cache. Temporaire puis rename() : deux mcmm peuvent demarrer en meme
// temps -- VMC en lance un par tranche et le raccord chevauche -- et un lecteur ne
// doit jamais tomber sur une LUT a moitie ecrite. Muet en cas d'echec : un
// dossier non inscriptible fait perdre l'optimisation, pas la conversion.
static void save_lut_cache(const char *path) {
    char tmp[1300];
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path,
                         (long)mcmm_getpid()) >= sizeof(tmp)) {
        return;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) return;

    uint8_t head[12];
    memcpy(head, LUT_CACHE_MAGIC, 4);
    put_u32_be(head + 4, LUT_CACHE_VERSION);
    put_u32_be(head + 8, (uint32_t)palette_count);

    size_t count = (size_t)palette_count;
    int ok = fwrite(head, 1, sizeof(head), f) == sizeof(head)
          && fwrite(palette, sizeof(PaletteEntry), count, f) == count
          && fwrite(color_lut, 1, LUT_BYTES, f) == LUT_BYTES;
    // fclose vide le tampon : c'est la que se voit un disque plein.
    ok = (fclose(f) == 0) && ok;

    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
    }
}

// Charge la LUT depuis le cache, ou la construit et la range pour la prochaine
// fois. palette_path a NULL desactive le cache.
static void prepare_lut(const char *palette_path) {
    char cache_path[1200];
    int cacheable = palette_path != NULL
                 && lut_cache_path(cache_path, sizeof(cache_path), palette_path) == 0;

    double start = omp_get_wtime();
    if (cacheable && load_lut_cache(cache_path) == 0) {
        fprintf(g_log, "LUT chargee depuis le cache en %.0f ms\n",
                (omp_get_wtime() - start) * 1000.0);
        return;
    }

    fprintf(g_log, "Construction de la LUT 256^3 (16 Mo)...\n");
    build_full_lut();
    fprintf(g_log, "LUT construite en %.2f s\n", omp_get_wtime() - start);

    if (cacheable) {
        save_lut_cache(cache_path);
    }
}

// ==========================================================================
// Ecrivain NBT minimal (Big-Endian, gzip)
// Remplace la dependance python-nbt
// ==========================================================================

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   capacity;
} NBTBuffer;

static void nbt_init(NBTBuffer *buf) {
    buf->capacity = 32768;  // 2x MAP_PIXELS : un .dat ne declenche jamais nbt_ensure
    buf->data = (uint8_t *)malloc(buf->capacity);
    buf->size = 0;
}

static void nbt_ensure(NBTBuffer *buf, size_t need) {
    while (buf->size + need > buf->capacity) {
        buf->capacity *= 2;
        buf->data = (uint8_t *)realloc(buf->data, buf->capacity);
    }
}

static void nbt_write_raw(NBTBuffer *buf, const void *src, size_t len) {
    nbt_ensure(buf, len);
    memcpy(buf->data + buf->size, src, len);
    buf->size += len;
}

static void nbt_write_byte(NBTBuffer *buf, uint8_t v) {
    nbt_write_raw(buf, &v, 1);
}

static void nbt_write_short_be(NBTBuffer *buf, int16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)(v & 0xFF) };
    nbt_write_raw(buf, b, 2);
}

static void nbt_write_int_be(NBTBuffer *buf, int32_t v) {
    uint8_t b[4] = {
        (uint8_t)((v >> 24) & 0xFF), (uint8_t)((v >> 16) & 0xFF),
        (uint8_t)((v >>  8) & 0xFF), (uint8_t)(v & 0xFF)
    };
    nbt_write_raw(buf, b, 4);
}

// Ecrit un tag nomme : type(1) + name_len(2) + name(n)
static void nbt_write_tag_header(NBTBuffer *buf, uint8_t tag_type, const char *name) {
    nbt_write_byte(buf, tag_type);
    int16_t len = (int16_t)strlen(name);
    nbt_write_short_be(buf, len);
    nbt_write_raw(buf, name, (size_t)len);
}

// Types NBT
#define TAG_END         0
#define TAG_BYTE        1
#define TAG_INT         3
#define TAG_BYTE_ARRAY  7
#define TAG_STRING      8
#define TAG_COMPOUND   10

static void nbt_write_named_byte(NBTBuffer *buf, const char *name, int8_t v) {
    nbt_write_tag_header(buf, TAG_BYTE, name);
    nbt_write_byte(buf, (uint8_t)v);
}

static void nbt_write_named_int(NBTBuffer *buf, const char *name, int32_t v) {
    nbt_write_tag_header(buf, TAG_INT, name);
    nbt_write_int_be(buf, v);
}

static void nbt_write_named_string(NBTBuffer *buf, const char *name, const char *val) {
    nbt_write_tag_header(buf, TAG_STRING, name);
    int16_t len = (int16_t)strlen(val);
    nbt_write_short_be(buf, len);
    nbt_write_raw(buf, val, (size_t)len);
}

static void nbt_write_named_byte_array(NBTBuffer *buf, const char *name,
                                        const uint8_t *arr, int32_t len) {
    nbt_write_tag_header(buf, TAG_BYTE_ARRAY, name);
    nbt_write_int_be(buf, len);
    nbt_write_raw(buf, arr, (size_t)len);
}

// Un map_N.dat : compound racine anonyme, DataVersion, puis le compound "data"
// que Minecraft lit. Le tout gzippe -- c'est le format des .dat vanilla.
static int write_map_dat(const char *filepath, const uint8_t *color_data) {
    NBTBuffer buf;
    nbt_init(&buf);

    nbt_write_tag_header(&buf, TAG_COMPOUND, "");   // racine : nom vide
    nbt_write_named_int(&buf, "DataVersion", DATA_VERSION);

    nbt_write_tag_header(&buf, TAG_COMPOUND, "data");
    nbt_write_named_int(&buf, "xCenter", 0);
    nbt_write_named_int(&buf, "zCenter", 0);
    nbt_write_named_byte(&buf, "trackingPosition", 0);
    nbt_write_named_byte(&buf, "unlimitedTracking", 0);
    nbt_write_named_string(&buf, "dimension", "futiax:videotomap");
    nbt_write_named_byte(&buf, "locked", 1);
    nbt_write_named_byte_array(&buf, "colors", color_data, MAP_PIXELS);
    nbt_write_byte(&buf, TAG_END);  // fin du compound "data"

    nbt_write_byte(&buf, TAG_END);  // fin du root compound

    gzFile gz = gzopen(filepath, "wb");
    if (!gz) {
        fprintf(stderr, "Erreur: impossible de creer %s\n", filepath);
        free(buf.data);
        return -1;
    }
    gzwrite(gz, buf.data, (unsigned int)buf.size);
    gzclose(gz);
    free(buf.data);
    return 0;
}

// ==========================================================================
// Traitement video avec FFmpeg libav*
// Remplace OpenCV + NumPy + PIL
// ==========================================================================

// mkdir -p en C. Les echecs sont ignores : un dossier deja la est le cas normal,
// et un dossier impossible se verra tout de suite a l'ouverture du fichier.
static void mkdirs(const char *path) {
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            mkdir_p(tmp);
            *p = '/';
        }
    }
    mkdir_p(tmp);
}

// Convertit une tuile 128x128 de la frame RGB en IDs couleur Minecraft
// via la LUT, dans un buffer de MAP_PIXELS (16384) octets fourni par
// l'appelant (index = y * 128 + x, meme layout que "colors" d'un .dat).
// Helper partage entre le chemin .dat et le chemin --stream.
static void convert_tile(const uint8_t *rgb_data, int stride,
                         int pixel_x, int pixel_y, uint8_t *out) {
    for (int y = 0; y < MAP_SIZE; y++) {
        const uint8_t *line = rgb_data + (size_t)(pixel_y + y) * (size_t)stride
                            + (size_t)pixel_x * 3;
        for (int x = 0; x < MAP_SIZE; x++) {
            uint8_t r = line[x * 3 + 0];
            uint8_t g = line[x * 3 + 1];
            uint8_t b = line[x * 3 + 2];
            out[y * MAP_SIZE + x] = color_lut[r][g][b];
        }
    }
}

// Tramage Floyd-Steinberg sur la frame ENTIERE, ecrit directement dans le
// buffer de tuiles (tuile i a l'offset i*MAP_PIXELS, index y*128 + x).
//
// La diffusion d'erreur est sequentielle par construction : un pixel depend de
// son voisin de gauche et de la ligne du dessus. La faire tuile par tuile
// remettait l'erreur a zero tous les 128 px, donc la phase du tramage
// redemarrait pile a la jonction des cartes -- une couture nette, et d'autant
// plus visible que le motif de depart est identique sur toutes les tuiles.
// D'ou la passe unique en raster sur toute la largeur.
//
// Retourne -1 si le buffer d'erreur n'a pas pu etre alloue (~12 Ko).
static int convert_frame_dithered(const uint8_t *rgb_data, int stride,
                                  int map_w, int map_h, uint8_t *out) {
    int w = map_w * MAP_SIZE;
    int h = map_h * MAP_SIZE;

    // Erreur diffusee, en valeur reelle (pas en 1/16), sur deux lignes : la
    // courante et la suivante. Une colonne de garde de chaque cote evite un
    // test de bord dans la boucle chaude.
    size_t row_ints = (size_t)(w + 2) * 3;
    int *err = (int *)calloc(row_ints * 2, sizeof(int));
    if (!err) return -1;
    int *cur = err, *nxt = err + row_ints;

    for (int y = 0; y < h; y++) {
        const uint8_t *line = rgb_data + (size_t)y * (size_t)stride;
        // Debut de la ligne y dans la RANGEE de tuiles qui la contient ;
        // reste a sauter de MAP_PIXELS par tuile en avancant en x.
        uint8_t *tile_row = out + (size_t)(y / MAP_SIZE) * map_w * MAP_PIXELS
                                + (size_t)(y % MAP_SIZE) * MAP_SIZE;
        for (int x = 0; x < w; x++) {
            int c[3];
            for (int k = 0; k < 3; k++) {
                // Clamper AVANT de mesurer l'erreur : sinon un blanc sature
                // accumule une dette que la ligne suivante paye en gris sale.
                int v = line[x * 3 + k] + cur[(x + 1) * 3 + k];
                c[k] = v < 0 ? 0 : (v > 255 ? 255 : v);
            }
            uint8_t id = color_lut[c[0]][c[1]][c[2]];
            tile_row[(size_t)(x / MAP_SIZE) * MAP_PIXELS + (x % MAP_SIZE)] = id;

            for (int k = 0; k < 3; k++) {
                int e = c[k] - pal_rgb[id][k];
                cur[(x + 2) * 3 + k] += e * 7 / 16;   // droite
                nxt[(x    ) * 3 + k] += e * 3 / 16;   // bas-gauche
                nxt[(x + 1) * 3 + k] += e * 5 / 16;   // bas
                nxt[(x + 2) * 3 + k] += e     / 16;   // bas-droite
            }
        }
        int *done = cur;
        cur = nxt;
        nxt = done;
        memset(nxt, 0, row_ints * sizeof(int));
    }

    free(err);
    return 0;
}

// Remplit out (map_w*map_h*MAP_PIXELS octets) avec les IDs de palette de la
// frame, une tuile apres l'autre.
static void frame_to_ids(const uint8_t *rgb_data, int stride,
                         int map_w, int map_h, uint8_t *out) {
    if (g_dither &&
        convert_frame_dithered(rgb_data, stride, map_w, map_h, out) == 0) {
        return;
    }
    // Sans tramage il n'y a rien a propager d'une tuile a l'autre : les tuiles
    // sont independantes, on les repartit sur les threads.
    int total_maps = map_w * map_h;
    #pragma omp parallel for schedule(dynamic) num_threads(NUM_THREADS)
    for (int i = 0; i < total_maps; i++) {
        convert_tile(rgb_data, stride, (i % map_w) * MAP_SIZE,
                     (i / map_w) * MAP_SIZE, out + (size_t)i * MAP_PIXELS);
    }
}

// Convertit une frame RGB (map_w*128 x map_h*128) en fichiers map_*.dat.
// ids = buffer de map_w*map_h*MAP_PIXELS octets fourni par l'appelant.
static void frame_to_maps(const uint8_t *rgb_data, int stride,
                           int map_w, int map_h, uint8_t *ids,
                           int64_t base_map_id, const char *output_dir) {
    int total_maps = map_w * map_h;
    frame_to_ids(rgb_data, stride, map_w, map_h, ids);

    #pragma omp parallel for schedule(dynamic) num_threads(NUM_THREADS)
    for (int i = 0; i < total_maps; i++) {
        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/map_%lld.dat",
                 output_dir, (long long)(base_map_id + i));
        write_map_dat(filepath, ids + (size_t)i * MAP_PIXELS);
    }
}

// Reecrit le jukebox JSON en entier : tout est fige sauf length_in_seconds, que
// Minecraft utilise pour savoir quand le disque a fini de jouer.
static void update_jukebox_json(const char *json_path, double duration_sec) {
    FILE *f = fopen(json_path, "w");
    if (!f) { fprintf(stderr, "Avertissement: impossible d'ecrire %s\n", json_path); return; }
    fprintf(f,
        "{\n"
        "    \"comparator_output\": 11,\n"
        "    \"description\": \"videoone\",\n"
        "    \"length_in_seconds\": %.3f,\n"
        "    \"sound_event\": {\n"
        "        \"sound_id\": \"video:music_disc.videoone\",\n"
        "        \"range\": 64.0\n"
        "    }\n"
        "}\n", duration_sec);
    fclose(f);
}

// Quote un argument pour le shell invoque par popen()/system().
// Le chemin video peut venir d'une source non fiable (commande in-game du
// plugin serveur) : sans quoting strict c'est une injection shell.
// POSIX : simple quotes, ' interne echappe en '\'' -- impermeable pour sh.
// Windows : cmd.exe n'a pas d'echappement fiable ; doubles quotes et refus
// des caracteres dangereux. Retourne 0 si ok, -1 sinon.
static int shell_quote_arg(char *dst, size_t cap, const char *arg) {
    size_t j = 0;
#ifdef _WIN32
    if (strpbrk(arg, "\"%^&|<>")) {
        fprintf(stderr, "Erreur: caractere interdit dans l'argument: %s\n", arg);
        return -1;
    }
    if (j + 1 >= cap) return -1;
    dst[j++] = '"';
    for (const char *p = arg; *p; p++) {
        if (j + 1 >= cap) return -1;
        dst[j++] = *p;
    }
    if (j + 2 > cap) return -1;
    dst[j++] = '"';
#else
    if (j + 1 >= cap) return -1;
    dst[j++] = '\'';
    for (const char *p = arg; *p; p++) {
        if (*p == '\'') {
            if (j + 4 >= cap) return -1;
            dst[j++] = '\''; dst[j++] = '\\'; dst[j++] = '\''; dst[j++] = '\'';
        } else {
            if (j + 1 >= cap) return -1;
            dst[j++] = *p;
        }
    }
    if (j + 2 > cap) return -1;
    dst[j++] = '\'';
#endif
    dst[j] = '\0';
    return 0;
}

// Construit la commande ffmpeg de decodage rawvideo, commune aux modes
// .dat et --stream. seek_s > 0 insere un -ss AVANT -i (seek d'entree,
// rapide : ffmpeg saute au keyframe puis decode-avance jusqu'au point exact).
//
// stderr_redirect != NULL bascule sur la variante --stream, qui a besoin des
// PTS : le fragment est colle en fin de commande et detourne le stderr de
// ffmpeg (donc les lignes showinfo) vers le canal lateral prepare par
// l'appelant. NULL = chemin .dat, commande inchangee.
//
// En --strict : ni scale ni pad, des PPM a la taille de la source (voir
// g_strict), lus par next_frame().
static int build_decode_cmd(char *cmd, size_t cap, const char *video_path,
                            int fps, int dst_w, int dst_h, double seek_s,
                            const char *stderr_redirect) {
    char quoted[1200];
    char seek_part[64] = "";
    if (shell_quote_arg(quoted, sizeof(quoted), video_path) != 0) return -1;
    if (seek_s > 0.0) {
        // %.3f : pas de setlocale() dans mcmm, donc separateur '.' garanti
        int m = snprintf(seek_part, sizeof(seek_part), "-ss %.3f ", seek_s);
        if (m < 0 || (size_t)m >= sizeof(seek_part)) return -1;
    }

    // -pix_fmt rgb24 aussi en PPM : sans lui une source 16 bits sortirait en
    // rgb48 (maxval 65535), que read_ppm refuse.
    // --data : directement les plans du JPEG (BT.601 plage pleine, 4:2:0), ni RGB
    // chez ffmpeg ni reconversion chez nous.
    const char *out_fmt = g_strict ? "-f image2pipe -c:v ppm -pix_fmt rgb24 -"
                        : g_data   ? "-f rawvideo -pix_fmt yuvj420p -"
                                   : "-f rawvideo -pix_fmt rgb24 -";
    char scale_pad[256] = "";
    if (!g_strict) {
        int s = snprintf(scale_pad, sizeof(scale_pad),
                 "scale=%d:%d:force_original_aspect_ratio=decrease%s,"
                 "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black",
                 dst_w, dst_h,
                 g_data ? ":flags=bicubic+accurate_rnd:out_color_matrix=bt601:out_range=full:out_h_chr_pos=128:out_v_chr_pos=128,format=yuvj420p" : "",
                 dst_w, dst_h);
        if (s < 0 || (size_t)s >= sizeof(scale_pad)) return -1;
    }

    int n;
    if (!stderr_redirect) {
        // Chemin .dat : aucun besoin de PTS, commande inchangee.
        n = snprintf(cmd, cap,
                 "ffmpeg -v error %s-i %s -r %d %s%s%s %s",
                 seek_part, quoted, fps,
                 g_strict ? "" : "-vf \"", scale_pad, g_strict ? "" : "\"",
                 out_fmt);
        return (n < 0 || (size_t)n >= cap) ? -1 : 0;
    }

    // Chemin --stream. Trois details qui ne se devinent pas :
    //
    // - la conversion de cadence passe DANS le filtergraph (fps=N en tete) au
    // lieu de -r en sortie : showinfo doit voir exactement les frames muxees.
    // Place derriere un -r, il imprime au rythme de la source (une ligne
    // toutes les 1/30 s pour 10 fps demandes) et la correspondance
    // ligne <-> frame est perdue.
    // - -copyts garde les PTS de la source. Sans lui ffmpeg rebase a zero
    // apres un -ss, ce qui detruit exactement l'horloge qu'on vient chercher.
    // - -v info : showinfo journalise en AV_LOG_INFO, il est muet en -v error.
    // Les lignes qui ne sont pas des PTS sont reemises sur notre stderr par
    // read_pts_us(), le plugin ne perd donc aucun diagnostic ffmpeg.
    n = snprintf(cmd, cap,
             "ffmpeg -hide_banner -v info -copyts %s-i %s -vf \"fps=%d,%s%sshowinfo%s\""
             " %s %s",
             seek_part, quoted, fps, scale_pad, g_strict ? "" : ",",
             g_data ? "=checksum=0" : "",               // ffmpeg >= 5.1
             out_fmt, stderr_redirect);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

// Lit une frame PPM (P6, 8 bits) sur le pipe ffmpeg du mode --strict. *buf est
// agrandi a la taille de la source si besoin. Retourne 0, ou -1 en fin de flux
// ou sur un en-tete inattendu.
static int read_ppm(FILE *fp, uint8_t **buf, size_t *cap, int *w, int *h) {
    int maxval;
    // Pas d'espace apres le dernier %d : il avalerait aussi les premiers
    // pixels s'ils valent 9 a 13 ou 32 (tout isspace). Le seul blanc qui
    // separe l'en-tete des pixels part avec le fgetc.
    if (fscanf(fp, "P6 %d %d %d", w, h, &maxval) != 3 || fgetc(fp) == EOF) {
        return -1;
    }
    if (maxval != 255 || *w <= 0 || *h <= 0
            || *w > STRICT_MAX_SIDE || *h > STRICT_MAX_SIDE) {
        fprintf(stderr, "Erreur: frame %dx%d (maxval %d) refusee en --strict"
                        " (8 bits, %d px de cote au plus)\n",
                *w, *h, maxval, STRICT_MAX_SIDE);
        return -1;
    }
    size_t need = (size_t)*w * (size_t)*h * 3;
    if (need > *cap) {
        uint8_t *grown = (uint8_t *)realloc(*buf, need);
        if (!grown) {
            fprintf(stderr, "Erreur: allocation memoire echouee\n");
            return -1;
        }
        *buf = grown;
        *cap = need;
    }
    return fread(*buf, 1, need, fp) == need ? 0 : -1;
}

// Place la source (sw x sh) dans dst (dw x dh, rgb24 contigu) au plus proche
// voisin, a facteur ENTIER, centree sur fond noir :
//   - plus petite que l'ecran : chaque pixel devient un bloc up x up, up = le
//     plus grand entier qui tient (un QR a 5 px/module en sort a 5*up px pile) ;
//   - plus grande : un pixel sur down, down = le plus petit entier tel que
//     floor(cote / down) tienne -- le pixel CENTRAL de chaque bloc down x down.
//     floor et pas ceil : 257 px sur 128 donnent 128 px a 1/2 (la derniere
//     colonne n'est jamais lue), pas 85 px a 1/3.
// Dans les deux cas chaque pixel de sortie EST un pixel de la source.
// Ecrit dans *zw/*zh la taille de l'image placee et retourne up, ou -down.
static int strict_scale(const uint8_t *src, int sw, int sh,
                        uint8_t *dst, int dw, int dh, int *zw, int *zh) {
    int up = 1, down = 1;
    if (sw <= dw && sh <= dh) {
        up = dw / sw < dh / sh ? dw / sw : dh / sh;
    } else {
        int dx = sw / (dw + 1) + 1, dy = sh / (dh + 1) + 1;
        down = dx > dy ? dx : dy;
    }
    int ow = sw * up / down, oh = sh * up / down;
    int ox = (dw - ow) / 2, oy = (dh - oh) / 2;
    memset(dst, 0, (size_t)dw * (size_t)dh * 3);
    for (int y = 0; y < oh; y++) {
        // une seule formule pour les deux sens : up = 1 ou down = 1
        const uint8_t *srow = src + (size_t)((y * down + down / 2) / up) * sw * 3;
        uint8_t *drow = dst + ((size_t)(oy + y) * dw + ox) * 3;
        for (int x = 0; x < ow; x++) {
            memcpy(drow + (size_t)x * 3, srow + (size_t)((x * down + down / 2) / up) * 3, 3);
        }
    }
    *zw = ow;
    *zh = oh;
    return down > 1 ? -down : up;
}

// Frame suivante du pipe ffmpeg dans dst (dst_w x dst_h, rgb24, yuvj420p en --data). Retourne 0, ou
// -1 en fin de flux (on ne distingue pas fin de video et erreur).
static int next_frame(FILE *pipe_fp, uint8_t *dst, int dst_w, int dst_h) {
    if (!g_strict) {
        size_t bytes = (size_t)dst_w * (size_t)dst_h * 3;
        if (g_data) bytes /= 2;
        return fread(dst, 1, bytes, pipe_fp) == bytes ? 0 : -1;
    }
    // Tampon de la source garde d'une frame a l'autre, jamais libere : le
    // process se termine avec le flux.
    static uint8_t *src = NULL;
    static size_t cap = 0;
    static int last_w = 0, last_h = 0;
    int sw, sh, zw, zh;
    if (read_ppm(pipe_fp, &src, &cap, &sw, &sh) != 0) return -1;
    int f = strict_scale(src, sw, sh, dst, dst_w, dst_h, &zw, &zh);
    if (zw == 0 || zh == 0) {
        // un bandeau 2000x12 sur une map : 1/16 laisse 0 ligne, et sans ce test
        // le plugin recevait une frame toute noire comme si de rien n'etait
        fprintf(stderr, "Erreur: source %dx%d trop allongee pour %dx%d en --strict\n",
                sw, sh, dst_w, dst_h);
        return -1;
    }
    if (sw != last_w || sh != last_h) {
        // une ligne par taille de source, pas par frame : c'est LE renseignement
        // pour choisir une taille d'ecran qui tombe pile
        fprintf(g_log, "Strict: source %dx%d, %s%d -> %dx%d dans %dx%d\n",
                sw, sh, f > 0 ? "x" : "1/", f > 0 ? f : -f, zw, zh, dst_w, dst_h);
        last_w = sw;
        last_h = sh;
    }
    return 0;
}

// Vide la fin du journal ffmpeg sur notre stderr, une fois le decodage fini.
// Indispensable : une source illisible ne produit aucune frame, read_pts_us
// n'est donc jamais appele et le message d'erreur de ffmpeg resterait dans le
// fichier temporaire. Le plugin ne verrait qu'un flux vide, sans explication.
static void drain_pts_log(FILE *log_fp) {
    char line[1024];
    clearerr(log_fp);
    while (fgets(line, sizeof(line), log_fp)) {
        if (!strstr(line, "pts_time:")) {
            fputs(line, stderr);
        }
    }
}

// Extrait le prochain pts_time des lignes de log de ffmpeg, en microsecondes.
// Renvoie fallback_us si la source n'expose pas de PTS exploitable.
//
// L'ordre est garanti sans synchronisation : showinfo est le dernier filtre,
// il journalise donc avant que le muxeur n'ecrive les pixels de la meme frame,
// et stderr n'est pas tamponne. Quand l'appelant tient une frame complete, sa
// ligne est deja la.
static int64_t read_pts_us(FILE *log_fp, int64_t fallback_us) {
    char line[1024];
    // Le fichier grandit sous nos pieds. Si on l'a rattrape une fois, stdio
    // garde le drapeau EOF arme et toutes les lectures suivantes echouent :
    // sans ce clearerr un unique depassement ferait basculer tout le reste de
    // la video sur des PTS synthetises, en silence.
    clearerr(log_fp);
    while (fgets(line, sizeof(line), log_fp)) {
        const char *tag = strstr(line, "pts_time:");
        if (!tag) {
            fputs(line, stderr); // diagnostic ffmpeg : on le laisse remonter
            continue;
        }
        char *end = NULL;
        // strtod depend de la locale pour le separateur decimal ; mcmm
        // n'appelle jamais setlocale(), donc locale "C" et '.' garanti.
        double secs = strtod(tag + 9, &end);
        if (end == tag + 9) {
            return fallback_us; // "pts_time:N/A"
        }
        return (int64_t)(secs * 1e6 + (secs < 0 ? -0.5 : 0.5));
    }
    return fallback_us;
}

// 0.0 si ffprobe manque, echoue, ou si la source n'annonce pas de duree (cas
// normal d'un direct). C'est a l'appelant de choisir son repli.
static double get_video_duration(const char *video_path) {
    char cmd[2048], quoted[1200];
    if (shell_quote_arg(quoted, sizeof(quoted), video_path) != 0) return 0.0;
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 %s",
             quoted);
#ifdef _WIN32
    FILE *fp = _popen(cmd, "r");
#else
    FILE *fp = popen(cmd, "r");
#endif
    if (!fp) return 0.0;
    char buf[64] = {0};
    double dur = 0.0;
    if (fgets(buf, sizeof(buf) - 1, fp)) {
        dur = atof(buf);
    }
#ifdef _WIN32
    _pclose(fp);
#else
    pclose(fp);
#endif
    return dur;
}

// Mode .dat : on ecrit map_0.dat, map_1.dat... dans une sauvegarde locale. Tous
// les chemins sont en dur et relatifs au binaire -- ce mode sert au montage hors
// ligne, c'est --stream que le plugin serveur utilise.
static int process_video(const char *video_path, int map_w, int map_h, int target_fps) {
    // --- Chemins ---
    const char *mc_dir        = "../minecraft/saves/world";
    const char *rp_dir        = "../minecraft/resourcepacks/video_rp";
    char palette_path[1024], output_dir[1024], jukebox_path[1024], audio_path[1024];

    snprintf(palette_path, sizeof(palette_path),
             "%s/datapacks/palette/data/gameboy/mapcolors/colors/preset_color_list.json", mc_dir);
    snprintf(output_dir, sizeof(output_dir), "%s/data", mc_dir);
    snprintf(jukebox_path, sizeof(jukebox_path),
             "%s/datapacks/video_dp/data/video/jukebox_song/videoone.json", mc_dir);
    snprintf(audio_path, sizeof(audio_path),
             "%s/assets/video/sounds/records/videoone.ogg", rp_dir);

    mkdirs(output_dir);
    {
        char audio_parent[1024];
        strncpy(audio_parent, audio_path, sizeof(audio_parent));
        char *last_sep = strrchr(audio_parent, '/');
        if (!last_sep) last_sep = strrchr(audio_parent, '\\');
        if (last_sep) { *last_sep = '\0'; mkdirs(audio_parent); }
    }

    // --- Charger la palette ---
    if (load_palette(palette_path) != 0) return -1;
    prepare_lut(palette_path);

    // --- Duree : ETA de la console, et length_in_seconds du disque ---
    double duration = get_video_duration(video_path);
    if (duration <= 0.0) {
        duration = 120.0;   // repli arbitraire, le disque durera 2 min de trop
    }
    update_jukebox_json(jukebox_path, duration);

    // --- Piste audio, extraite en OGG dans le resource pack ---
    printf("Extraction de l'audio...\n");
    {
        char cmd_audio[4096], q_video[1200], q_audio[1200];
        if (shell_quote_arg(q_video, sizeof(q_video), video_path) != 0 ||
            shell_quote_arg(q_audio, sizeof(q_audio), audio_path) != 0) {
            fprintf(stderr, "Avertissement: chemin non quotable, audio ignore\n");
        } else {
            snprintf(cmd_audio, sizeof(cmd_audio),
                     "ffmpeg -y -i %s -vn -c:a libvorbis -q:a 4 %s",
                     q_video, q_audio);
            int ret = system(cmd_audio);
            if (ret == 0) printf("Audio extrait et converti en OGG (Vorbis)\n");
            else printf("Avertissement: ffmpeg a retourne le code %d\n", ret);
        }
    }

    // --- Lancer la lecture video avec ffmpeg via un pipe ---
    int dst_w = map_w * MAP_SIZE;
    int dst_h = map_h * MAP_SIZE;
    int dst_stride = dst_w * 3;
    size_t frame_bytes = (size_t)(dst_h * dst_stride);
    uint8_t *dst_buf = (uint8_t *)malloc(frame_bytes);
    uint8_t *ids_buf = (uint8_t *)malloc((size_t)map_w * map_h * MAP_PIXELS);
    if (!dst_buf || !ids_buf) {
        fprintf(stderr, "Erreur: allocation memoire echouee\n");
        free(dst_buf);
        free(ids_buf);
        return -1;
    }

    printf("Traitement: %dx%d cartes (%dx%d px) @ %d fps cible\n",
           map_w, map_h, dst_w, dst_h, target_fps);

    char cmd[4096];
    if (build_decode_cmd(cmd, sizeof(cmd), video_path, target_fps, dst_w, dst_h, 0.0, NULL) != 0) {
        fprintf(stderr, "Erreur: chemin video invalide\n");
        free(dst_buf);
        return -1;
    }

    FILE *pipe_fp = NULL;
#ifdef _WIN32
    pipe_fp = _popen(cmd, "rb");
#else
    pipe_fp = popen(cmd, "r");
#endif

    if (!pipe_fp) {
        fprintf(stderr, "Erreur: impossible d'ouvrir le pipe ffmpeg\n");
        free(dst_buf);
        return -1;
    }

    int64_t map_id = 0;
    int frame_count = 0;
    double start_time = omp_get_wtime();
    double est_total = duration * target_fps;

    while (1) {
        if (next_frame(pipe_fp, dst_buf, dst_w, dst_h) != 0) {
            break;
        }

        frame_count++;

        frame_to_maps(dst_buf, dst_stride, map_w, map_h, ids_buf,
                      map_id, output_dir);
        map_id += map_w * map_h;

        if (frame_count % 10 == 0) {
            double elapsed = omp_get_wtime() - start_time;
            double remaining = (elapsed / frame_count) * (est_total - frame_count);
            if (remaining < 0) remaining = 0;
            printf("\rFrame: %d/~%d | Restant: %.1fs | %.1fms/frame    ",
                   frame_count, (int)est_total, remaining,
                   (elapsed / frame_count) * 1000.0);
            fflush(stdout);
        }
    }

#ifdef _WIN32
    _pclose(pipe_fp);
#else
    pclose(pipe_fp);
#endif

    printf("\nConversion terminee! %d frames traitees.\n", frame_count);

    free(dst_buf);
    free(ids_buf);
    return 0;
}

// ==========================================================================
// Mode --data : l'image codee en JPEG de base sur un mur de maps (les murs
// connus : VD_LAYOUTS de vmc_data.h, 7x4 pour le WAN et 10x6 pour le LAN)
//
// POURQUOI : le goulot de la lecture est la bande passante du client, et une
// frame coute le meme nombre d'octets quoi qu'on mette dans les maps (459 Ko
// pour 7x4). Quantifiee sur 244 couleurs de palette, l'image gache ces octets ;
// ici chaque symbole (une couleur de map sur 2x2 pixels, lisible de loin) porte
// 7,75 bits d'un flux JPEG, soit un canva de 848x464 en vraies couleurs
// (1232x720 sur le mur LAN).
// Un JPEG de base : DCT 8x8, q GLOBAL (le plus fin qui tient dans le mur, par
// dichotomie), Huffman standard ; un index des longueurs pour que le decodeur
// trouve chaque macrobloc sans lire ceux d'avant.
//
// QUI DECODE : le post effect minecraft:vmc_data (vmc_data_lan) du pack VMC. Il
// detecte le mur a l'ecran, lit les symboles et dessine l'image au centre.
// Sans le pack le joueur voit un mur de bruit colore.
//
// LE FORMAT vit dans vmc_data.h, GENERE par tools/shader_test/gen_dct.py (repo VMC)
// en meme temps que le decodeur GLSL. Ce code reproduit encode_planes() de gen_dct.py,
// OCTET POUR OCTET (python3 gen_dct.py --mcmm <mcmm>), d'ou les memes operations en
// flottants 32 bits dans le meme ordre (dct8). Le decodeur, lui, n'exige pas cette
// egalite : c'est le test qui garde les deux codeurs synchrones. Changer le format =
// changer le script et regenerer, jamais a la main ici.
// ==========================================================================

// Bornes valables pour tous les murs de VD_LAYOUTS : un macrobloc 16x16 du canva
// par carre de 8x8 cellules de la zone de donnees.
#define VD_DW_MAX   (VD_MAX_GW - 2 * VD_DATA_X0)
#define VD_DH_MAX   (VD_MAX_GH - 2 * VD_DATA_Y0)
#define VD_MAX_MB   ((VD_DW_MAX / 8) * (VD_DH_MAX / 8))
#define VD_MAX_BITS (VD_DW_MAX * VD_DH_MAX / VD_WORD_SYMS * VD_WORD_BITS)
#define VD_LEN_MAX  ((1 << VD_LEN_BITS) - 1)

static float vd_qm[2][64];                       // Y, C : 1 / (Q 8 a(u) a(v)), indice u * 8 + v
static uint8_t vd_zz[64];                        // rang zigzag -> u * 8 + v
static uint8_t vd_wall[VD_MAX_WALL_H][VD_MAX_WALL_W];   // ids du mur, ligne 0 = haut
static uint8_t vd_nbits[2048];                   // longueur en bits de 0..2047
static double vd_t[5];                           // --stats : dct, encode, search, assemble, symbols (s)
static int vd_last_q, vd_last_bits;              // --stats
static const vd_layout *vd_L;                    // le mur choisi par data_init()
static float vd_z[VD_MAX_MB][6][64];             // coefficients AAN, u * 8 + v : Y0..Y3, Cb, Cr
static int16_t vd_q[VD_MAX_MB][6][64];           // quantifies au q de la frame
static int vd_len[VD_MAX_MB];                    // bits du flux de chaque macrobloc
// Un macrobloc avant truncate() : 6 x (DC + 63 AC d'au plus 26 bits) < 10 000 bits.
#define VD_ENC_MAX  1256
static uint8_t vd_enc[VD_MAX_MB][VD_ENC_MAX];    // le flux de chaque macrobloc
static uint8_t vd_stream[VD_MAX_BITS / 8 + 16];  // le flux du mur
static uint8_t vd_sym[VD_DW_MAX * VD_DH_MAX];    // ses symboles, ligne par ligne

// La zone de donnees est copiee ligne par ligne dans le mur.
_Static_assert(VD_CELL == 1, "modules de plus d'un pixel de map : recopier avec vd_module");

// Le mur de VD_LAYOUTS qui fait maps_x x maps_y maps, NULL si aucun : le
// decodeur du pack n'existe que pour ceux-la.
static const vd_layout *vd_find_layout(int maps_x, int maps_y) {
    for (int i = 0; i < VD_N_LAYOUTS; i++) {
        if (VD_LAYOUTS[i].maps_x == maps_x && VD_LAYOUTS[i].maps_y == maps_y) return &VD_LAYOUTS[i];
    }
    return NULL;
}

// Pose le module (mx, my) : VD_CELL x VD_CELL pixels de map, apres la marge.
static void vd_module(int mx, int my, uint8_t id) {
    for (int j = 0; j < VD_CELL; j++) {
        memset(&vd_wall[vd_L->y0 + my * VD_CELL + j][vd_L->x0 + mx * VD_CELL], id, VD_CELL);
    }
}

// Motif n x n modules centre en (cx, cy), en modules continus : sombre sauf
// l'anneau a n/2 - 1 du centre (repere 7x7, alignement 5x5), comme wall_ids.
static void vd_pattern(double cx, double cy, int n) {
    int h = n / 2, x0 = (int)(cx - n / 2.0), y0 = (int)(cy - n / 2.0);
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            int d = abs(i - h) > abs(j - h) ? abs(i - h) : abs(j - h);
            vd_module(x0 + i, y0 + j, d != h - 1 ? VD_DARK_ID : VD_LIGHT_ID);
        }
    }
}

// Tables, et toute la partie constante du mur (fond clair, reperes,
// alignements) : seules les cellules de donnees changent d'une frame a l'autre.
static void data_init(const vd_layout *L) {
    vd_L = L;
    for (int v = 1; v < 2048; v++) vd_nbits[v] = (uint8_t)(vd_nbits[v / 2] + 1);
    // aan[k] = racine(2) cos(k pi / 16), aan[0] = 1 : le gain de la DCT AAN (jfdctflt.c)
    static const double aan[8] = {1.0, 1.387039845, 1.306562965, 1.175875602,
                                  1.0, 0.785694958, 0.541196100, 0.275899379};
    for (int i = 0; i < 64; i++) {
        int u = i / 8, v = i % 8;
        vd_qm[0][i] = (float)(1.0 / (VD_QY[v * 8 + u] * 8.0 * aan[u] * aan[v]));
        vd_qm[1][i] = (float)(1.0 / (VD_QC[v * 8 + u] * 8.0 * aan[u] * aan[v]));
        vd_zz[i] = (uint8_t)(VD_ZZ_U[i] * 8 + VD_ZZ_V[i]);
    }
    memset(vd_wall, VD_LIGHT_ID, sizeof(vd_wall));
    for (int f = 0; f < VD_N_FINDERS; f++) vd_pattern(L->finders[f][0], L->finders[f][1], 7);
    for (int a = 0; a < VD_N_ALIGNS; a++) vd_pattern(L->aligns[a][0], L->aligns[a][1], 5);
}

// DCT AAN (5 multiplications, 29 additions, jfdctflt.c de libjpeg) sur le premier
// indice de b, les 8 colonnes a la fois. Gain : voir vd_qm.
static void vd_aan(float (*b)[8]) {
    #pragma omp simd
    for (int x = 0; x < 8; x++) {
        float t0 = b[0][x] + b[7][x], t7 = b[0][x] - b[7][x];
        float t1 = b[1][x] + b[6][x], t6 = b[1][x] - b[6][x];
        float t2 = b[2][x] + b[5][x], t5 = b[2][x] - b[5][x];
        float t3 = b[3][x] + b[4][x], t4 = b[3][x] - b[4][x];
        float e0 = t0 + t3, e3 = t0 - t3, e1 = t1 + t2, e2 = t1 - t2;
        float z1 = (e2 + e3) * 0.707106781f;
        b[0][x] = e0 + e1;
        b[4][x] = e0 - e1;
        b[2][x] = e3 + z1;
        b[6][x] = e3 - z1;
        float o0 = t4 + t5, o1 = t5 + t6, o2 = t6 + t7;
        float z5 = (o0 - o2) * 0.382683433f;
        float z2 = 0.541196100f * o0 + z5;
        float z4 = 1.306562965f * o2 + z5;
        float z3 = o1 * 0.707106781f;
        float z11 = t7 + z3, z13 = t7 - z3;
        b[5][x] = z13 + z2;
        b[3][x] = z13 - z2;
        b[1][x] = z11 + z4;
        b[7][x] = z11 - z4;
    }
}

// Bloc 8x8 d'un plan -> z[u * 8 + v].
static void vd_dct_block(const uint8_t *p, int stride, float *z) {
    float a[8][8], t[8][8];
    for (int y = 0; y < 8; y++, p += stride) {
        for (int x = 0; x < 8; x++) a[y][x] = (float)p[x] - 128.0f;
    }
    vd_aan(a);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) t[x][y] = a[y][x];
    }
    vd_aan(t);
    memcpy(z, t, sizeof(t));
}

// Macrobloc k (ligne par ligne, vd_L->sx par ligne) du canva yuvj420p -> vd_z[k].
static void data_dct_mb(const uint8_t *yuv, int k) {
    const int cw = vd_L->canvas_w, ch = vd_L->canvas_h, mx = k % vd_L->sx, my = k / vd_L->sx;
    for (int b = 0; b < 4; b++) {
        vd_dct_block(yuv + (size_t)(my * 16 + (b >> 1) * 8) * cw + mx * 16 + (b & 1) * 8, cw, vd_z[k][b]);
    }
    const uint8_t *cb = yuv + (size_t)cw * ch, *cr = cb + (size_t)(cw / 2) * (ch / 2);
    vd_dct_block(cb + (size_t)(my * 8) * (cw / 2) + mx * 8, cw / 2, vd_z[k][4]);
    vd_dct_block(cr + (size_t)(my * 8) * (cw / 2) + mx * 8, cw / 2, vd_z[k][5]);
}

// Pas de quantification du q : m[t][u * 8 + v] * coefficient AAN = coefficient / (Q s).
static void vd_q_table(int qv, float m[2][64]) {
    float is = (float)(1.0 / pow(2.0, (qv - VD_Q_OFF) / (double)VD_Q_DIV));
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < 64; i++) m[t][i] = vd_qm[t][i] * is;
    }
}

// floor(c m + 0,5) borne a +-VD_AMP_MAX, rangee en zigzag. Borner AVANT l'arrondi
// donne le meme entier (les bornes sont entieres) et tient t dans la plage ou la
// troncature en int + correction vaut floor().
static void vd_quant_mb(int k, float m[2][64], int16_t q[6][64]) {
    for (int c = 0; c < 6; c++) {
        const float *z = vd_z[k][c], *mc = m[c < 4 ? 0 : 1];
        int r[64];
        #pragma omp simd
        for (int i = 0; i < 64; i++) {
            float t = z[i] * mc[i] + 0.5f;
            t = t < -VD_AMP_MAX ? -VD_AMP_MAX : (t > VD_AMP_MAX ? VD_AMP_MAX : t);
            int v = (int)t;
            r[i] = v - (t < v);
        }
        for (int i = 0; i < 64; i++) q[c][i] = (int16_t)r[vd_zz[i]];
    }
}

static inline int vd_bitlen(int v) {
    return vd_nbits[abs(v)];
}

// Bits d'un bloc : DC (categorie Huffman + amplitude), puis (zeros, taille)
// Huffman + amplitude, ZRL, EOB. Tables tdc / tac de VD_HUF_LEN.
static int vd_block_bits(const int16_t *z, int dc, int tdc, int tac) {
    int c = vd_bitlen(dc), bits = VD_HUF_LEN[tdc][c] + c, run = 0, last = 0;
    for (int i = 1; i < 64; i++) {
        if (!z[i]) {
            run++;
            continue;
        }
        int s = vd_bitlen(z[i]);
        bits += (run >> 4) * VD_HUF_LEN[tac][0xF0] + VD_HUF_LEN[tac][((run & 15) << 4) | s] + s;
        run = 0;
        last = i;
    }
    return bits + (last < 63 ? VD_HUF_LEN[tac][0x00] : 0);
}

// mb_bits() : Y0 absolu, Y1..Y3 en difference du precedent, Cb et Cr absolus.
static int vd_mb_bits(int16_t q[6][64]) {
    int bits = 0;
    for (int b = 0; b < 4; b++) bits += vd_block_bits(q[b], q[b][0] - (b ? q[b - 1][0] : 0), 0, 2);
    for (int c = 4; c < 6; c++) bits += vd_block_bits(q[c], q[c][0], 1, 3);
    return bits;
}

// truncate() : un macrobloc trop long perd ses plus hautes frequences (le meme
// rang zigzag dans les 6 blocs, en partant de 63) jusqu'a tenir. -> ses bits.
static int vd_truncate(int16_t q[6][64]) {
    int bits = vd_mb_bits(q);
    for (int cut = 63; cut > 0 && bits > VD_LEN_MAX; cut--) {
        for (int c = 0; c < 6; c++) {
            for (int i = cut; i < 64; i++) q[c][i] = 0;
        }
        bits = vd_mb_bits(q);
    }
    return bits;
}

// Ecrivain de bits, poids fort d'abord, par mots de 32 bits : n <= 32.
typedef struct { uint8_t *p; uint64_t acc; int n; } vd_bw;

static inline void vd_be32(uint8_t *p, uint32_t t) {
    p[0] = (uint8_t)(t >> 24); p[1] = (uint8_t)(t >> 16); p[2] = (uint8_t)(t >> 8); p[3] = (uint8_t)t;
}

static inline void vd_put(vd_bw *w, uint32_t v, int n) {
    w->acc = (w->acc << n) | v;
    w->n += n;
    if (w->n >= 32) {
        w->n -= 32;
        vd_be32(w->p, (uint32_t)(w->acc >> w->n));
        w->p += 4;
    }
}

static inline void vd_put_huf(vd_bw *w, int t, int sym) {
    vd_put(w, (uint32_t)VD_HUF_CODE[t][sym], VD_HUF_LEN[t][sym]);
}

// Amplitude JPEG : v > 0 tel quel, v < 0 -> v + 2^n - 1.
static inline void vd_put_amp(vd_bw *w, int v, int n) {
    vd_put(w, (uint32_t)(v > 0 ? v : v + (1 << n) - 1), n);
}

static void vd_put_block(vd_bw *w, const int16_t *z, int dc, int tdc, int tac) {
    int c = vd_bitlen(dc), run = 0, last = 0;
    vd_put_huf(w, tdc, c);
    vd_put_amp(w, dc, c);
    for (int i = 1; i < 64; i++) {
        if (!z[i]) {
            run++;
            continue;
        }
        for (; run >= 16; run -= 16) vd_put_huf(w, tac, 0xF0);
        int s = vd_bitlen(z[i]);
        vd_put_huf(w, tac, (run << 4) | s);
        vd_put_amp(w, z[i], s);
        run = 0;
        last = i;
    }
    if (last < 63) vd_put_huf(w, tac, 0x00);
}

// vd_q[k] -> vd_enc[k], complete a l'octet par des zeros. -> ses bits.
static int vd_encode_mb(int k) {
    vd_bw w = { vd_enc[k], 0, 0 };
    for (int b = 0; b < 4; b++) vd_put_block(&w, vd_q[k][b], vd_q[k][b][0] - (b ? vd_q[k][b - 1][0] : 0), 0, 2);
    for (int c = 4; c < 6; c++) vd_put_block(&w, vd_q[k][c], vd_q[k][c][0], 1, 3);
    int bits = (int)(w.p - vd_enc[k]) * 8 + w.n;
    vd_be32(w.p, (uint32_t)(w.acc << (32 - w.n)));   // n < 32
    return bits;
}

// Quantifie et code tous les macroblocs au pas q -> la somme de leurs bits (chacun
// compte pour LEN_MAX au plus : truncate() le ramenera la).
static long vd_encode_all(int qv, int n) {
    float m[2][64];
    vd_q_table(qv, m);
    long total = 0;
    #pragma omp parallel for schedule(static) reduction(+:total) num_threads(NUM_THREADS)
    for (int k = 0; k < n; k++) {
        vd_quant_mb(k, m, vd_q[k]);
        vd_len[k] = vd_encode_mb(k);
        total += vd_len[k] < VD_LEN_MAX ? vd_len[k] : VD_LEN_MAX;
    }
    return total;
}

// Le flux de tous les macroblocs au pas q tient-il dans room bits ? Sans le coder.
static int data_fits(int qv, int n, int room) {
    float m[2][64];
    vd_q_table(qv, m);
    long total = 0;
    #pragma omp parallel for schedule(static) reduction(+:total) num_threads(NUM_THREADS)
    for (int k = 0; k < n; k++) {
        int16_t q[6][64];
        vd_quant_mb(k, m, q);
        int b = vd_mb_bits(q);
        total += b < VD_LEN_MAX ? b : VD_LEN_MAX;
    }
    return total <= room;
}

// Le q le plus fin qui tient, sachant que q = 0 ne tient pas (a 255 tout tient : il ne reste que
// les DC). Galop a partir de prev, le q de la frame precedente, puis dichotomie : q bouge peu d'une
// frame a l'autre, ~3 essais au lieu de 8. Le meme parcours que search_q() de gen_dct.py : si le
// nombre de bits n'est pas monotone en q, deux recherches differentes peuvent s'arreter sur des q
// differents.
static int data_search_q(int prev, int n, int room) {
    int lo = 1, hi = 255;
    int q = prev < 1 ? 1 : prev > 254 ? 254 : prev;
    if (data_fits(q, n, room)) {
        hi = q;
        for (int step = 1; lo < hi; step *= 2) {
            q = hi - step;
            if (q < lo) break;
            if (!data_fits(q, n, room)) { lo = q + 1; break; }
            hi = q;
        }
    } else {
        lo = q + 1;
        for (int step = 1; lo < hi; step *= 2) {
            q = lo + step - 1;
            if (q >= hi) break;
            if (data_fits(q, n, room)) { hi = q; break; }
            lo = q + 1;
        }
    }
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (data_fits(mid, n, room)) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

// Canva yuvj420p (vd_L->canvas_w x canvas_h) -> out, le mur decoupe en tuiles
// comme frame_to_ids : encode(), wall_ids() et wall_tiles() de gen_dct.py.
// data_init() d'abord.
static void data_frame_to_ids(const uint8_t *yuv, uint8_t *out) {
    const int n = vd_L->sx * vd_L->sy, dw = vd_L->dw;
    const int capacity = vd_L->dw * vd_L->dh / VD_WORD_SYMS * VD_WORD_BITS;
    const int head = VD_QB_BITS + vd_L->sy * VD_ROW_BITS + n * VD_LEN_BITS;

    double t0 = omp_get_wtime(), t1;
    #pragma omp parallel for schedule(static) num_threads(NUM_THREADS)
    for (int k = 0; k < n; k++) data_dct_mb(yuv, k);
    t1 = omp_get_wtime(); vd_t[0] = t1 - t0; t0 = t1;

    // Le q le plus fin qui tient. q = 0 d'abord : la plupart des videos y tiennent,
    // et le codage de cet essai sert directement. Sinon on part du q de la frame
    // precedente (data_search_q).
    int lo = 0;
    int fits_q0 = vd_encode_all(0, n) <= capacity - head;
    t1 = omp_get_wtime(); vd_t[1] = t1 - t0; t0 = t1;
    if (!fits_q0) {
        lo = data_search_q(vd_last_q, n, capacity - head);
        vd_encode_all(lo, n);
    }
    #pragma omp parallel for schedule(dynamic, 64) num_threads(NUM_THREADS)
    for (int k = 0; k < n; k++) {
        if (vd_len[k] > VD_LEN_MAX) {
            vd_truncate(vd_q[k]);
            vd_len[k] = vd_encode_mb(k);
        }
    }

    t1 = omp_get_wtime(); vd_t[2] = t1 - t0; t0 = t1;

    // En-tete : q, debut de chaque ligne de macroblocs (depuis le debut des
    // donnees), longueur de chaque macrobloc. Puis les macroblocs mis bout a bout.
    vd_bw w = { vd_stream, 0, 0 };
    vd_put(&w, (uint32_t)lo, VD_QB_BITS);
    int row = 0;
    for (int y = 0; y < vd_L->sy; y++) {
        vd_put(&w, (uint32_t)row, VD_ROW_BITS);
        for (int x = 0; x < vd_L->sx; x++) row += vd_len[y * vd_L->sx + x];
    }
    for (int k = 0; k < n; k++) vd_put(&w, (uint32_t)vd_len[k], VD_LEN_BITS);
    for (int k = 0; k < n; k++) {
        const uint8_t *e = vd_enc[k];
        int left = vd_len[k];
        for (; left > 0; left -= 32, e += 4) {
            uint32_t t = ((uint32_t)e[0] << 24) | ((uint32_t)e[1] << 16) | ((uint32_t)e[2] << 8) | e[3];
            if (left >= 32) vd_put(&w, t, 32);
            else vd_put(&w, t >> (32 - left), left);
        }
    }
    int used = ((int)(w.p - vd_stream) * 8 + w.n + VD_WORD_BITS - 1) / VD_WORD_BITS;
    vd_be32(w.p, (uint32_t)(w.acc << (32 - w.n)));
    memset(w.p + 4, 0, 8);                       // le dernier mot lit au-dela
    vd_last_q = lo;
    vd_last_bits = (int)(w.p - vd_stream) * 8 + w.n;
    t1 = omp_get_wtime(); vd_t[3] = t1 - t0; t0 = t1;

    // Mot w = bits [31w, 31w + 31), poids fort d'abord -> symboles 4w..4w+3 en
    // base 244 (poids faible d'abord), cellule c = (c % dw, c / dw) de la zone.
    // Les mots au-dela du flux valent 0.
    int words = capacity / VD_WORD_BITS;
    #pragma omp parallel for schedule(static) num_threads(NUM_THREADS)
    for (int i = 0; i < used; i++) {
        int off = i * VD_WORD_BITS;
        const uint8_t *p = vd_stream + (off >> 3);
        uint64_t a = ((uint64_t)p[0] << 32) | ((uint64_t)p[1] << 24) | ((uint64_t)p[2] << 16)
                   | ((uint64_t)p[3] << 8) | p[4];
        uint32_t v = (uint32_t)(a >> (40 - VD_WORD_BITS - (off & 7))) & ((1u << VD_WORD_BITS) - 1);
        for (int s4 = 0; s4 < VD_WORD_SYMS; s4++, v /= VD_ALPHA) {
            vd_sym[i * VD_WORD_SYMS + s4] = (uint8_t)(v % VD_ALPHA + 4);
        }
    }
    memset(vd_sym + used * VD_WORD_SYMS, 4, (size_t)(words - used) * VD_WORD_SYMS);
    for (int r = 0; r * dw < words * VD_WORD_SYMS; r++) {
        memcpy(&vd_wall[vd_L->y0 + VD_DATA_Y0 + r][vd_L->x0 + VD_DATA_X0], vd_sym + r * dw, (size_t)dw);
    }
    for (int y = 0; y < vd_L->wall_h; y++) {
        for (int tx = 0; tx < vd_L->maps_x; tx++) {
            memcpy(out + ((size_t)(y / MAP_SIZE) * vd_L->maps_x + tx) * MAP_PIXELS
                       + (size_t)(y % MAP_SIZE) * MAP_SIZE,
                   &vd_wall[y][tx * MAP_SIZE], MAP_SIZE);
        }
    }
    vd_t[4] = omp_get_wtime() - t0;
}

// ==========================================================================
// Mode --stream : frames binaires sur stdout pour un plugin serveur
//
// Le format est decrit une seule fois, en tete de fichier avec STREAM_VERSION.
// NE PAS le redocumenter ici : la copie qui trainait a cet endroit annoncait
// encore la v1 sans PTS, longtemps apres le passage a la v2.
//
// Aucun marqueur de frame : les tailles sont fixes, on lit 8 octets de PTS puis
// map_w*map_h*16384 octets, et EOF = fin de la video.
// ==========================================================================

// Ecrit sur stdout en verifiant le retour de fwrite. Si le consommateur a
// ferme le pipe (ecriture courte), on le note sur stderr et on quitte
// proprement avec le code 0.
static void stream_write(FILE *pipe_fp, const void *data, size_t len) {
    if (fwrite(data, 1, len, stdout) != len) {
        fprintf(stderr, "Flux ferme par le consommateur, arret propre.\n");
        if (pipe_fp) {
#ifdef _WIN32
            _pclose(pipe_fp);
#else
            pclose(pipe_fp);
#endif
        }
        exit(0);
    }
}

static void stream_put_u16_be(uint8_t *p, unsigned int v) {
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}

static void stream_put_u32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void stream_put_s64_be(uint8_t *p, int64_t v) {
    uint64_t u = (uint64_t)v; // complement a deux, defini sur les non signes
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((u >> (56 - 8 * i)) & 0xFF);
    }
}

#ifdef _WIN32
// Chemin du journal PTS, en statique pour l'atexit. Windows uniquement : sur
// POSIX le fichier est delie des sa creation, il n'y a rien a supprimer.
static char g_pts_log_path[512];

static void remove_pts_log(void) {
    if (g_pts_log_path[0]) {
        remove(g_pts_log_path);
    }
}
#endif

static int stream_video(const char *video_path, const char *palette_path,
                        int map_w, int map_h, int target_fps, double seek_s) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    // --- Palette + LUT (logs sur stderr via g_log) ---
    // Inutiles en --data (les symboles sont des ids fixes) : on economise la
    // construction de la LUT, plusieurs secondes sans cache.
    if (g_data) {
        data_init(vd_find_layout(map_w, map_h));
    } else {
        if (load_palette(palette_path) != 0) return 1;
        prepare_lut(palette_path);
    }

    // --- En-tete du protocole ---
    uint8_t header[STREAM_HEADER_SIZE] = {0};
    memcpy(header, STREAM_MAGIC, 4);
    stream_put_u16_be(header + 4,  STREAM_VERSION);
    stream_put_u16_be(header + 6,  (unsigned int)map_w);
    stream_put_u16_be(header + 8,  (unsigned int)map_h);
    stream_put_u16_be(header + 10, (unsigned int)target_fps);
    // octets 12-15 : reserve = 0 (deja mis a zero), sauf --stats
    if (g_stats) stream_put_u32_be(header + 12, STREAM_STATS);
    stream_write(NULL, header, sizeof(header));
    if (fflush(stdout) != 0) {
        fprintf(stderr, "Flux ferme par le consommateur, arret propre.\n");
        exit(0);
    }

    // --- Buffers ---
    // En --data ffmpeg sort le canva du codeur, pas l'image des maps (main a
    // deja verifie que map_w x map_h est un mur de VD_LAYOUTS).
    int dst_w = g_data ? vd_L->canvas_w : map_w * MAP_SIZE;
    int dst_h = g_data ? vd_L->canvas_h : map_h * MAP_SIZE;
    int dst_stride = dst_w * 3;
    size_t frame_bytes = g_data ? (size_t)dst_w * dst_h * 3 / 2 : (size_t)dst_h * (size_t)dst_stride;
    int total_maps = map_w * map_h;
    size_t out_bytes = (size_t)total_maps * MAP_PIXELS;

    uint8_t *dst_buf = (uint8_t *)malloc(frame_bytes);
    uint8_t *out_buf = (uint8_t *)malloc(out_bytes);
    if (!dst_buf || !out_buf) {
        fprintf(stderr, "Erreur: allocation memoire echouee\n");
        free(dst_buf);
        free(out_buf);
        return 1;
    }

    fprintf(stderr, "Streaming: %dx%d cartes (%dx%d px) @ %d fps cible\n",
            map_w, map_h, dst_w, dst_h, target_fps);

    // --- Canal lateral des PTS ---------------------------------------------
    // rawvideo ne transporte aucun timestamp et popen() ne fournit qu'un seul
    // tube, deja pris par les pixels. On detourne donc le stderr de ffmpeg
    // (ou showinfo imprime les PTS) vers un fichier qu'on relit en parallele.
    //
    // Un fichier et pas un second tube : il faudrait alterner les lectures sur
    // les deux descripteurs sous peine d'interblocage des que ffmpeg remplit
    // les 64 Ko du tube -- un blocage vaut bien pire qu'un fichier temporaire.
    //
    // Sur POSIX le fichier est delie tout de suite : plus aucun nom, les deux
    // descripteurs (le notre en lecture, celui herite par le shell en
    // ecriture) gardent l'inode vivante et le noyau la recupere a la
    // fermeture. Rien a nettoyer, meme quand le plugin nous tue par SIGKILL --
    // ce qu'il fait a chaque stop et a chaque seek.
    //
    // Un echec ici n'est pas fatal : on replie sur des PTS synthetises, ce qui
    // vaut mieux que pas de flux du tout.
    FILE *pts_fp = NULL;
    char redirect[600] = ""; // assez large pour "2>" + un chemin Windows cite
    const char *stderr_redirect = NULL;
#ifdef _WIN32
    // cmd.exe ne sait pas heriter un descripteur nomme : on passe par un
    // chemin, supprime par l'atexit.
    char *tmp_name = _tempnam(NULL, "mcmm-pts-");
    if (tmp_name) {
        snprintf(g_pts_log_path, sizeof(g_pts_log_path), "%s", tmp_name);
        free(tmp_name);
        char q_log[512];
        FILE *sink = fopen(g_pts_log_path, "w");
        if (sink) {
            fclose(sink);
            pts_fp = fopen(g_pts_log_path, "r");
        }
        if (pts_fp && shell_quote_arg(q_log, sizeof(q_log), g_pts_log_path) == 0
                && (size_t)snprintf(redirect, sizeof(redirect), "2>%s", q_log)
                        < sizeof(redirect)) {
            atexit(remove_pts_log);
            stderr_redirect = redirect;
        }
    }
#else
    const char *tmp_dir = getenv("TMPDIR");
    if (!tmp_dir || !*tmp_dir) tmp_dir = "/tmp";
    char tmp_path[512];
    if ((size_t)snprintf(tmp_path, sizeof(tmp_path), "%s/mcmm-pts-XXXXXX",
                         tmp_dir) < sizeof(tmp_path)) {
        // mkstemp plutot qu'un nom previsible : O_EXCL et mode 0600, donc pas
        // de course sur un lien symbolique pre-pose dans un /tmp partage.
        int write_fd = mkstemp(tmp_path);
        if (write_fd >= 0) {
            pts_fp = fopen(tmp_path, "r"); // avant le unlink, tant qu'il a un nom
            unlink(tmp_path);
            if (pts_fp) {
                // Le shell herite de write_fd (popen ne pose pas FD_CLOEXEC)
                // et y renvoie le stderr de ffmpeg.
                snprintf(redirect, sizeof(redirect), "2>&%d", write_fd);
                stderr_redirect = redirect;
            } else {
                close(write_fd);
            }
        }
    }
#endif
    if (!stderr_redirect) {
        fprintf(stderr, "Avertissement: canal PTS indisponible,"
                        " repli sur des PTS synthetises.\n");
        if (pts_fp) {
            fclose(pts_fp);
            pts_fp = NULL;
        }
    }

    char cmd[4096];
    if (build_decode_cmd(cmd, sizeof(cmd), video_path, target_fps,
                         dst_w, dst_h, seek_s, stderr_redirect) != 0) {
        fprintf(stderr, "Erreur: chemin video invalide\n");
        if (pts_fp) fclose(pts_fp);
        free(dst_buf);
        free(out_buf);
        return 1;
    }

    FILE *pipe_fp = NULL;
#ifdef _WIN32
    pipe_fp = _popen(cmd, "rb");
#else
    pipe_fp = popen(cmd, "r");
#endif
    if (!pipe_fp) {
        fprintf(stderr, "Erreur: impossible d'ouvrir le pipe ffmpeg\n");
        free(dst_buf);
        free(out_buf);
        return 1;
    }

    int frame_count = 0;
    double conv_s = 0.0; // temps de conversion cumule : doit tenir dans 1/fps
    double write_s = 0.0;           // --stats : ecriture de la frame precedente
    clock_t cpu_last = clock();     // --stats. CPU du process sous POSIX (temps ecoule sous Windows)
    while (1) {
        double wait_start = omp_get_wtime();
        if (next_frame(pipe_fp, dst_buf, dst_w, dst_h) != 0) {
            break;
        }

        // PTS de CETTE frame : sa ligne showinfo est deja ecrite, ffmpeg
        // journalise avant de muxer. Repli absolu (seek inclus) et non pas
        // relatif au demarrage : une frame doit porter le meme PTS qu'on
        // l'ait atteinte avec ou sans --seek, meme quand on l'a synthetise.
        int64_t pts_us = (int64_t)(seek_s * 1e6)
                       + (int64_t)frame_count * 1000000 / target_fps;
        if (pts_fp) {
            pts_us = read_pts_us(pts_fp, pts_us);
        }

        double conv_start = omp_get_wtime();
        if (g_data) {
            data_frame_to_ids(dst_buf, out_buf);
        } else {
            frame_to_ids(dst_buf, dst_stride, map_w, map_h, out_buf);
        }
        double conv_end = omp_get_wtime();
        conv_s += conv_end - conv_start;

        // Flush apres chaque frame : sans lui stdio garderait la frame dans son
        // tampon de 4 Ko et le plugin afficherait avec un retard variable.
        uint8_t pts_be[8];
        stream_put_s64_be(pts_be, pts_us);
        stream_write(pipe_fp, pts_be, sizeof(pts_be));
        if (g_stats) {
            clock_t cpu_now = clock();
            uint32_t st[STREAM_STATS] = {
                (uint32_t)((conv_start - wait_start) * 1e6), (uint32_t)(write_s * 1e6),
                (uint32_t)((cpu_now - cpu_last) * 1e6 / CLOCKS_PER_SEC),
                (uint32_t)((conv_end - conv_start) * 1e6),
                (uint32_t)(vd_t[0] * 1e6), (uint32_t)(vd_t[1] * 1e6), (uint32_t)(vd_t[2] * 1e6),
                (uint32_t)(vd_t[3] * 1e6), (uint32_t)(vd_t[4] * 1e6),
                (uint32_t)vd_last_q, (uint32_t)vd_last_bits,
            };
            uint8_t be[STREAM_STATS * 4];
            for (int i = 0; i < STREAM_STATS; i++) stream_put_u32_be(be + i * 4, st[i]);
            stream_write(pipe_fp, be, sizeof(be));
            cpu_last = cpu_now;
        }
        stream_write(pipe_fp, out_buf, out_bytes);
        if (fflush(stdout) != 0) {
            fprintf(stderr, "Flux ferme par le consommateur, arret propre.\n");
#ifdef _WIN32
            _pclose(pipe_fp);
#else
            pclose(pipe_fp);
#endif
            exit(0);
        }
        write_s = omp_get_wtime() - conv_end;

        frame_count++;
        if (frame_count % 100 == 0) {
            fprintf(stderr, "Frames envoyees: %d\n", frame_count);
        }
    }

#ifdef _WIN32
    _pclose(pipe_fp);
#else
    pclose(pipe_fp);
#endif

    // Apres le pclose : ffmpeg a fini d'ecrire, y compris son message d'erreur
    // si c'est lui qui a coupe court.
    if (pts_fp) {
        drain_pts_log(pts_fp);
        fclose(pts_fp);
    }
    fprintf(stderr, "Flux termine: %d frames envoyees (conversion %.1f ms/frame).\n",
            frame_count, frame_count ? conv_s * 1000.0 / frame_count : 0.0);

    free(dst_buf);
    free(out_buf);
    return 0;
}

#include "vmc_daemon.h"

// ==========================================================================
// Point d'entree
// ==========================================================================

int main(int argc, char *argv[]) {
    int stream_mode = 0;
    int daemon_mode = 0;
    const char *palette_arg = NULL;
    double seek_arg = 0.0;
    const char *positional[4] = { NULL, NULL, NULL, NULL };
    int npos = 0;

    // Parsing simple : les flags peuvent apparaitre avant les positionnels
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stream") == 0) {
            stream_mode = 1;
        } else if (strcmp(argv[i], "--daemon") == 0) {
            daemon_mode = 1;
        } else if (strcmp(argv[i], "--palette") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Erreur: --palette requiert un chemin de fichier JSON\n");
                return 1;
            }
            palette_arg = argv[++i];
        } else if (strcmp(argv[i], "--dither") == 0) {
            g_dither = 1;
        } else if (strcmp(argv[i], "--strict") == 0) {
            g_strict = 1;
        } else if (strcmp(argv[i], "--data") == 0) {
            g_data = 1;
        } else if (strcmp(argv[i], "--stats") == 0) {
            g_stats = 1;
        } else if (strcmp(argv[i], "--seek") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Erreur: --seek requiert un temps en secondes\n");
                return 1;
            }
            seek_arg = atof(argv[++i]);
            if (seek_arg < 0.0) seek_arg = 0.0;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            // Sans ce garde-fou une option inconnue devient un positionnel :
            // un binaire trop vieux pour un flag recent prenait "--dither"
            // pour le chemin de la video et echouait dans ffmpeg.
            fprintf(stderr, "Erreur: option inconnue: %s\n", argv[i]);
            return 1;
        } else if (npos < 4) {
            positional[npos++] = argv[i];
        } else {
            fprintf(stderr, "Avertissement: argument supplementaire ignore: %s\n", argv[i]);
        }
    }

    if (g_dither && g_strict) {
        // le tramage invente des couleurs, exactement ce que --strict interdit
        fprintf(stderr, "Erreur: --dither et --strict s'excluent\n");
        return 1;
    }
    if (g_data && (g_dither || g_strict)) {
        // --data ne quantifie plus l'image : ni tramage ni placement strict
        fprintf(stderr, "Erreur: --data exclut --dither et --strict\n");
        return 1;
    }
    if (g_data && !stream_mode) {
        fprintf(stderr, "Erreur: --data requiert --stream\n");
        return 1;
    }

    // En mode --stream ou --daemon, stdout est reserve au flux binaire
    g_log = (stream_mode || daemon_mode) ? stderr : stdout;
    fprintf(g_log, "=== MinecraftVideo (C natif) ===\n\n");

    if (daemon_mode) {
        if (!palette_arg) {
            fprintf(stderr, "Erreur: --palette <palette.json> est requis en mode --daemon\n");
            return 1;
        }
        return run_daemon(palette_arg);
    }

    if (stream_mode && !palette_arg && !g_data) {
        fprintf(stderr, "Erreur: --palette <palette.json> est requis en mode --stream\n");
        return 1;
    }
    if (stream_mode && npos < 4) {
        fprintf(stderr, "Usage: mcmm --stream --palette <palette.json>"
                        " [--seek <secondes>] [--dither|--strict|--data]"
                        " <video> <largeur> <hauteur> <fps>\n");
        return 1;
    }

    char video_path[512];
    int framerate, width, height;

    if (npos >= 4) {
        // Mode ligne de commande
        strncpy(video_path, positional[0], sizeof(video_path) - 1);
        video_path[sizeof(video_path) - 1] = '\0';
        width     = atoi(positional[1]);
        height    = atoi(positional[2]);
        framerate = atoi(positional[3]);
    } else {
        // Mode interactif
        printf("Chemin de la video : ");
        if (!fgets(video_path, sizeof(video_path), stdin)) return 1;
        video_path[strcspn(video_path, "\r\n")] = '\0';

        printf("Largeur en nombre de cartes : ");
        if (scanf("%d", &width) != 1) return 1;
        printf("Hauteur en nombre de cartes : ");
        if (scanf("%d", &height) != 1) return 1;
        printf("Framerate desire [1-20] : ");
        if (scanf("%d", &framerate) != 1) return 1;
    }

    // Le mode --stream (lecture temps reel par le plugin serveur) tolere un
    // framerate plus eleve que le mode .dat ; les deux plafonds doivent rester
    // alignes avec ceux du plugin (VideoCommand.MAX_FPS / MAX_DIMENSION).
    int max_fps = stream_mode ? 60 : 20;
    if (framerate < 1 || framerate > max_fps || width <= 0 || height <= 0) {
        fprintf(stderr, "Valeurs invalides! (fps 1-%d, largeur/hauteur > 0)\n", max_fps);
        return 1;
    }

    if (stream_mode) {
        if (width > 0xFFFF || height > 0xFFFF) {
            fprintf(stderr, "Erreur: largeur/hauteur trop grandes pour l'en-tete (max 65535)\n");
            return 1;
        }
        if (g_data && !vd_find_layout(width, height)) {
            // le decodeur du pack est genere pour ces murs-la, aucune autre taille
            fprintf(stderr, "Erreur: --data exige un mur de");
            for (int i = 0; i < VD_N_LAYOUTS; i++) {
                fprintf(stderr, "%s %dx%d", i ? " ou" : "", VD_LAYOUTS[i].maps_x, VD_LAYOUTS[i].maps_y);
            }
            fprintf(stderr, " cartes (recu %dx%d)\n", width, height);
            return 1;
        }
        return stream_video(video_path, palette_arg, width, height, framerate, seek_arg);
    }

    return process_video(video_path, width, height, framerate);
}
