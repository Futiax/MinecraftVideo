/*
 * MinecraftVideo — Prototype C natif
 * Remplace MCMM_client.pyx sans dépendances Python
 *
 * Dépendances (compilation) : zlib, OpenMP
 * Dépendances (exécution)   : ffmpeg et ffprobe dans le PATH
 *   Linux   : sudo apt install ffmpeg   (ou équivalent dnf/pacman)
 *   Windows : https://www.gyan.dev/ffmpeg/builds/ (ajouter bin/ au PATH)
 *
 * Compilation (CMake, toutes plateformes) :
 *   cmake -S . -B build && cmake --build build --config Release
 *
 * Compilation (GCC / Linux / MinGW, direct) :
 *   gcc -O2 -fopenmp mcmm.c -o mcmm -lz -lm
 */

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
#define mkdir_p(path) _mkdir(path)
#else
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#endif

#include <omp.h>
#include <zlib.h>

/* ============================================================================
 * Constantes
 * ========================================================================= */

#define MAP_SIZE        128
#define MAP_PIXELS      (MAP_SIZE * MAP_SIZE)   /* 16384 */
#define MAX_PALETTE     256
#define NUM_SHADES      4
#define DEFAULT_COLOR_ID 41
#define DATA_VERSION    4440
#define NUM_THREADS     4

/* Nuances Minecraft pour chaque couleur de base */
static const int SHADES[NUM_SHADES] = { 180, 220, 255, 135 };

/* Protocole de streaming (--stream)
 *
 * En-tete, 16 octets big-endian, identique en v1 et v2 :
 *   "MCMM" | u16 version | u16 w | u16 h | u16 fps | u32 reserve
 *
 * Puis, par frame :
 *   v1 : w*h tuiles de 16384 octets (128x128 index de palette, ligne 0 = haut)
 *   v2 : s64 pts_us  puis  les memes w*h tuiles
 *
 * pts_us est le PTS de la frame dans la source, en microsecondes, ABSOLU :
 * une frame a 12,5 s porte le meme PTS qu'on l'ait atteinte avec ou sans
 * --seek. C'est la seule horloge partagee entre mcmm et le ffmpeg audio que
 * le plugin lance en parallele sur la meme source — un compteur de frames ne
 * marche que sur un fichier, pas sur un direct ou les deux connexions
 * rejoignent le flux a des endroits differents. */
#define STREAM_MAGIC        "MCMM"
#define STREAM_VERSION      2
#define STREAM_HEADER_SIZE  16

/* Flux de log : stdout en mode normal, stderr en mode --stream
 * (stdout est reserve au flux binaire dans ce dernier cas) */
static FILE *g_log = NULL;

/* ============================================================================
 * Table de lookup 256³ RGB → ID couleur Minecraft
 * ~16 Mo statique — remplace PIL.Image.quantize() et l'ancienne LUT exacte
 * ========================================================================= */

static uint8_t color_lut[256][256][256];

/* Table inverse mc_id -> RGB effectif, necessaire au tramage (l'erreur de
 * quantification est la difference entre le pixel voulu et la couleur reelle
 * de la palette qu'on lui a substituee). */
static uint8_t pal_rgb[256][3];

/* Tramage Floyd-Steinberg (--dither) : desactive par defaut */
static int g_dither = 0;

/* Palette : couleurs effectives après application des nuances */
typedef struct {
    uint8_t r, g, b;
    uint8_t mc_id;      /* idx_base * 4 + shade_idx */
} PaletteEntry;

static PaletteEntry palette[MAX_PALETTE * NUM_SHADES];
static int palette_count = 0;

/* ============================================================================
 * Parsing JSON minimal pour preset_color_list.json
 * (pas de dépendance externe — le format est simple et fixe)
 * ========================================================================= */

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
        /* Cherche les lignes du type :  "42": "#ff00aa"  */
        char *quote1 = strchr(line, '"');
        if (!quote1) continue;
        char *quote2 = strchr(quote1 + 1, '"');
        if (!quote2) continue;

        /* Extraire l'index */
        *quote2 = '\0';
        int idx = atoi(quote1 + 1);
        *quote2 = '"';

        /* Chercher le # du code hex */
        char *hash = strchr(quote2 + 1, '#');
        if (!hash) continue;

        int r, g, b;
        if (parse_hex(hash, &r, &g, &b) != 0) continue;

        /* Générer les 4 nuances */
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

/* Pré-calcule la LUT complète — nearest-neighbor dans l'espace RGB */
static void build_full_lut(void) {
    fprintf(g_log, "Construction de la LUT 256^3 (16 Mo)...\n");
    double start = omp_get_wtime();

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

    double elapsed = omp_get_wtime() - start;
    fprintf(g_log, "LUT construite en %.2f s\n", elapsed);
}

/* ============================================================================
 * Écrivain NBT minimal (Big-Endian, gzip)
 * Remplace la dépendance python-nbt
 * ========================================================================= */

/* Buffer dynamique pour construire le NBT en mémoire avant compression */
typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   capacity;
} NBTBuffer;

static void nbt_init(NBTBuffer *buf) {
    buf->capacity = 32768;
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

/* Écrit un tag nommé : type(1) + name_len(2) + name(n) */
static void nbt_write_tag_header(NBTBuffer *buf, uint8_t tag_type, const char *name) {
    nbt_write_byte(buf, tag_type);
    int16_t len = (int16_t)strlen(name);
    nbt_write_short_be(buf, len);
    nbt_write_raw(buf, name, (size_t)len);
}

/* Types NBT */
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

/* Écrit un fichier map_N.dat complet compressé en gzip */
static int write_map_dat(const char *filepath, const uint8_t *color_data) {
    NBTBuffer buf;
    nbt_init(&buf);

    /* Root compound (nom vide pour le root) */
    nbt_write_tag_header(&buf, TAG_COMPOUND, "");

    /* DataVersion */
    nbt_write_named_int(&buf, "DataVersion", DATA_VERSION);

    /* data compound */
    nbt_write_tag_header(&buf, TAG_COMPOUND, "data");
    nbt_write_named_int(&buf, "xCenter", 0);
    nbt_write_named_int(&buf, "zCenter", 0);
    nbt_write_named_byte(&buf, "trackingPosition", 0);
    nbt_write_named_byte(&buf, "unlimitedTracking", 0);
    nbt_write_named_string(&buf, "dimension", "futiax:videotomap");
    nbt_write_named_byte(&buf, "locked", 1);
    nbt_write_named_byte_array(&buf, "colors", color_data, MAP_PIXELS);
    nbt_write_byte(&buf, TAG_END);  /* fin du compound "data" */

    nbt_write_byte(&buf, TAG_END);  /* fin du root compound */

    /* Compression gzip */
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

/* ============================================================================
 * Traitement vidéo avec FFmpeg libav*
 * Remplace OpenCV + NumPy + PIL
 * ========================================================================= */

/* Crée les répertoires parents récursivement */
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

/* Convertit une tuile 128x128 de la frame RGB en IDs couleur Minecraft
 * via la LUT, dans un buffer de MAP_PIXELS (16384) octets fourni par
 * l'appelant (index = y * 128 + x, meme layout que "colors" d'un .dat).
 * Helper partagé entre le chemin .dat et le chemin --stream. */
static void convert_tile_dithered(const uint8_t *rgb_data, int stride,
                                  int pixel_x, int pixel_y, uint8_t *out) {
    /* Erreur diffusee, en valeur reelle (pas en 1/16). [0] = ligne courante,
     * [1] = ligne suivante. Une colonne de garde de chaque cote evite un test
     * de bord dans la boucle chaude. ~3 Ko de pile, par thread OpenMP. */
    int err[2][MAP_SIZE + 2][3];
    memset(err, 0, sizeof(err));

    for (int y = 0; y < MAP_SIZE; y++) {
        const uint8_t *line = rgb_data + (size_t)(pixel_y + y) * (size_t)stride
                            + (size_t)pixel_x * 3;
        for (int x = 0; x < MAP_SIZE; x++) {
            int c[3];
            for (int k = 0; k < 3; k++) {
                /* Clamper AVANT de mesurer l'erreur : sinon un blanc sature
                 * accumule une dette que la ligne suivante paye en gris sale. */
                int v = line[x * 3 + k] + err[0][x + 1][k];
                c[k] = v < 0 ? 0 : (v > 255 ? 255 : v);
            }
            uint8_t id = color_lut[c[0]][c[1]][c[2]];
            out[y * MAP_SIZE + x] = id;

            for (int k = 0; k < 3; k++) {
                int e = c[k] - pal_rgb[id][k];
                err[0][x + 2][k] += e * 7 / 16;   /* droite      */
                err[1][x    ][k] += e * 3 / 16;   /* bas-gauche  */
                err[1][x + 1][k] += e * 5 / 16;   /* bas         */
                err[1][x + 2][k] += e     / 16;   /* bas-droite  */
            }
        }
        memcpy(err[0], err[1], sizeof(err[0]));
        memset(err[1], 0, sizeof(err[1]));
    }
}

static void convert_tile(const uint8_t *rgb_data, int stride,
                         int pixel_x, int pixel_y, uint8_t *out) {
    if (g_dither) {
        /* ponytail: l'erreur repart de zero au bord de chaque tuile 128x128
         * (les tuiles sont converties en parallele). La phase du tramage se
         * reinitialise donc tous les 128 px; invisible en pratique. Passer a
         * une passe pleine frame (serie) si une couture apparait un jour. */
        convert_tile_dithered(rgb_data, stride, pixel_x, pixel_y, out);
        return;
    }
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

/* Convertit une frame RGB (target_w*128 x target_h*128) en fichiers map_*.dat */
static void frame_to_maps(const uint8_t *rgb_data, int stride,
                           int map_w, int map_h,
                           int64_t base_map_id, const char *output_dir) {
    int total_maps = map_w * map_h;

    #pragma omp parallel for schedule(dynamic) num_threads(NUM_THREADS)
    for (int i = 0; i < total_maps; i++) {
        int row = i / map_w;
        int col = i % map_w;

        uint8_t id_array[MAP_PIXELS];
        convert_tile(rgb_data, stride, col * MAP_SIZE, row * MAP_SIZE, id_array);

        int64_t map_id = base_map_id + i;
        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/map_%lld.dat",
                 output_dir, (long long)map_id);
        write_map_dat(filepath, id_array);
    }
}

/* Met à jour le fichier jukebox JSON avec la durée de la vidéo */
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

/* Quote un argument pour le shell invoque par popen()/system().
 * Le chemin video peut venir d'une source non fiable (commande in-game du
 * plugin serveur) : sans quoting strict c'est une injection shell.
 * POSIX : simple quotes, ' interne echappe en '\'' — impermeable pour sh.
 * Windows : cmd.exe n'a pas d'echappement fiable ; doubles quotes et refus
 * des caracteres dangereux. Retourne 0 si ok, -1 sinon. */
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

/* Construit la commande ffmpeg de decodage rawvideo, commune aux modes
 * .dat et --stream. seek_s > 0 insere un -ss AVANT -i (seek d'entree,
 * rapide : ffmpeg saute au keyframe puis decode-avance jusqu'au point exact).
 *
 * stderr_redirect != NULL bascule sur la variante --stream, qui a besoin des
 * PTS : le fragment est colle en fin de commande et detourne le stderr de
 * ffmpeg (donc les lignes showinfo) vers le canal lateral prepare par
 * l'appelant. NULL = chemin .dat, commande inchangee. */
static int build_decode_cmd(char *cmd, size_t cap, const char *video_path,
                            int fps, int dst_w, int dst_h, double seek_s,
                            const char *stderr_redirect) {
    char quoted[1200];
    char seek_part[64] = "";
    if (shell_quote_arg(quoted, sizeof(quoted), video_path) != 0) return -1;
    if (seek_s > 0.0) {
        /* %.3f : pas de setlocale() dans mcmm, donc separateur '.' garanti */
        int m = snprintf(seek_part, sizeof(seek_part), "-ss %.3f ", seek_s);
        if (m < 0 || (size_t)m >= sizeof(seek_part)) return -1;
    }

    char scale_pad[256];
    int s = snprintf(scale_pad, sizeof(scale_pad),
             "scale=%d:%d:force_original_aspect_ratio=decrease,"
             "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black",
             dst_w, dst_h, dst_w, dst_h);
    if (s < 0 || (size_t)s >= sizeof(scale_pad)) return -1;

    int n;
    if (!stderr_redirect) {
        /* Chemin .dat : aucun besoin de PTS, commande inchangee. */
        n = snprintf(cmd, cap,
                 "ffmpeg -v error %s-i %s -r %d -vf \"%s\""
                 " -f rawvideo -pix_fmt rgb24 -",
                 seek_part, quoted, fps, scale_pad);
        return (n < 0 || (size_t)n >= cap) ? -1 : 0;
    }

    /* Chemin --stream. Trois details qui ne se devinent pas :
     *
     * - la conversion de cadence passe DANS le filtergraph (fps=N en tete) au
     *   lieu de -r en sortie : showinfo doit voir exactement les frames muxees.
     *   Place derriere un -r, il imprime au rythme de la source (une ligne
     *   toutes les 1/30 s pour 10 fps demandes) et la correspondance
     *   ligne <-> frame est perdue.
     * - -copyts garde les PTS de la source. Sans lui ffmpeg rebase a zero
     *   apres un -ss, ce qui detruit exactement l'horloge qu'on vient chercher.
     * - -v info : showinfo journalise en AV_LOG_INFO, il est muet en -v error.
     *   Les lignes qui ne sont pas des PTS sont reemises sur notre stderr par
     *   read_pts_us(), le plugin ne perd donc aucun diagnostic ffmpeg. */
    n = snprintf(cmd, cap,
             "ffmpeg -hide_banner -v info -copyts %s-i %s -vf \"fps=%d,%s,showinfo\""
             " -f rawvideo -pix_fmt rgb24 - %s",
             seek_part, quoted, fps, scale_pad, stderr_redirect);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* Extrait le prochain pts_time des lignes de log de ffmpeg, en microsecondes.
 * Renvoie fallback_us si la source n'expose pas de PTS exploitable.
 *
 * L'ordre est garanti sans synchronisation : showinfo est le dernier filtre,
 * il journalise donc avant que le muxeur n'ecrive les pixels de la meme frame,
 * et stderr n'est pas tamponne. Quand l'appelant tient une frame complete, sa
 * ligne est deja la. */
/* Vide la fin du journal ffmpeg sur notre stderr, une fois le decodage fini.
 * Indispensable : une source illisible ne produit aucune frame, read_pts_us
 * n'est donc jamais appele et le message d'erreur de ffmpeg resterait dans le
 * fichier temporaire. Le plugin ne verrait qu'un flux vide, sans explication. */
static void drain_pts_log(FILE *log_fp) {
    char line[1024];
    clearerr(log_fp);
    while (fgets(line, sizeof(line), log_fp)) {
        if (!strstr(line, "pts_time:")) {
            fputs(line, stderr);
        }
    }
}

static int64_t read_pts_us(FILE *log_fp, int64_t fallback_us) {
    char line[1024];
    /* Le fichier grandit sous nos pieds. Si on l'a rattrape une fois, stdio
     * garde le drapeau EOF arme et toutes les lectures suivantes echouent :
     * sans ce clearerr un unique depassement ferait basculer tout le reste de
     * la video sur des PTS synthetises, en silence. */
    clearerr(log_fp);
    while (fgets(line, sizeof(line), log_fp)) {
        const char *tag = strstr(line, "pts_time:");
        if (!tag) {
            fputs(line, stderr); /* diagnostic ffmpeg : on le laisse remonter */
            continue;
        }
        char *end = NULL;
        /* strtod depend de la locale pour le separateur decimal ; mcmm
         * n'appelle jamais setlocale(), donc locale "C" et '.' garanti. */
        double secs = strtod(tag + 9, &end);
        if (end == tag + 9) {
            return fallback_us; /* "pts_time:N/A" */
        }
        return (int64_t)(secs * 1e6 + (secs < 0 ? -0.5 : 0.5));
    }
    return fallback_us;
}

/* Récupère la durée de la vidéo via ffprobe */
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

/* Point d'entrée principal du traitement vidéo */
static int process_video(const char *video_path, int map_w, int map_h, int target_fps) {
    /* --- Chemins --- */
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
    /* Créer le dossier parent de l'audio */
    {
        char audio_parent[1024];
        strncpy(audio_parent, audio_path, sizeof(audio_parent));
        char *last_sep = strrchr(audio_parent, '/');
        if (!last_sep) last_sep = strrchr(audio_parent, '\\');
        if (last_sep) { *last_sep = '\0'; mkdirs(audio_parent); }
    }

    /* --- Charger la palette --- */
    if (load_palette(palette_path) != 0) return -1;
    build_full_lut();

    /* --- Récupérer la durée de la vidéo --- */
    double duration = get_video_duration(video_path);
    if (duration <= 0.0) {
        duration = 120.0; /* Valeur par défaut si échec */
    }

    /* Mettre à jour le jukebox JSON */
    update_jukebox_json(jukebox_path, duration);

    /* Extraction audio via commande ffmpeg */
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

    /* --- Lancer la lecture vidéo avec ffmpeg via un pipe --- */
    int dst_w = map_w * MAP_SIZE;
    int dst_h = map_h * MAP_SIZE;
    int dst_stride = dst_w * 3;
    size_t frame_bytes = (size_t)(dst_h * dst_stride);
    uint8_t *dst_buf = (uint8_t *)malloc(frame_bytes);
    if (!dst_buf) {
        fprintf(stderr, "Erreur: allocation memoire echouee\n");
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
        size_t bytes_read = fread(dst_buf, 1, frame_bytes, pipe_fp);
        if (bytes_read < frame_bytes) {
            break; /* Fin de la vidéo ou erreur */
        }

        frame_count++;

        /* Convertir en maps et écrire les fichiers */
        frame_to_maps(dst_buf, dst_stride, map_w, map_h, map_id, output_dir);
        map_id += map_w * map_h;

        /* Progression */
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
    return 0;
}

/* ============================================================================
 * Mode --stream : frames binaires sur stdout pour un plugin serveur
 *
 * En-tete de 16 octets (big-endian) :
 *   octets 0-3   magic "MCMM"
 *   octets 4-5   uint16 version = 1
 *   octets 6-7   uint16 map_w
 *   octets 8-9   uint16 map_h
 *   octets 10-11 uint16 fps
 *   octets 12-15 uint32 reserve = 0
 * Puis, par frame decodee : map_w*map_h buffers de 16384 octets exactement
 * (IDs couleur 128x128, index = y*128+x), tuiles en ordre row-major
 * (tuile i = row*map_w + col, row 0 = HAUT de l'image).
 * Pas de marqueur par frame (tailles fixes). EOF = fin de la video.
 * ========================================================================= */

/* Ecrit sur stdout en verifiant le retour de fwrite. Si le consommateur a
 * ferme le pipe (ecriture courte), on le note sur stderr et on quitte
 * proprement avec le code 0. */
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

static void stream_put_s64_be(uint8_t *p, int64_t v) {
    uint64_t u = (uint64_t)v; /* complement a deux, defini sur les non signes */
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((u >> (56 - 8 * i)) & 0xFF);
    }
}

#ifdef _WIN32
/* Chemin du journal PTS, en statique pour l'atexit. Windows uniquement : sur
 * POSIX le fichier est delie des sa creation, il n'y a rien a supprimer. */
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

    /* --- Palette + LUT (logs sur stderr via g_log) --- */
    if (load_palette(palette_path) != 0) return 1;
    build_full_lut();

    /* --- En-tete du protocole --- */
    uint8_t header[STREAM_HEADER_SIZE] = {0};
    memcpy(header, STREAM_MAGIC, 4);
    stream_put_u16_be(header + 4,  STREAM_VERSION);
    stream_put_u16_be(header + 6,  (unsigned int)map_w);
    stream_put_u16_be(header + 8,  (unsigned int)map_h);
    stream_put_u16_be(header + 10, (unsigned int)target_fps);
    /* octets 12-15 : reserve = 0 (deja mis a zero) */
    stream_write(NULL, header, sizeof(header));
    if (fflush(stdout) != 0) {
        fprintf(stderr, "Flux ferme par le consommateur, arret propre.\n");
        exit(0);
    }

    /* --- Buffers --- */
    int dst_w = map_w * MAP_SIZE;
    int dst_h = map_h * MAP_SIZE;
    int dst_stride = dst_w * 3;
    size_t frame_bytes = (size_t)dst_h * (size_t)dst_stride;
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

    /* --- Decodage via ffmpeg (rawvideo RGB24 sur un pipe) --- */
    /* --- Canal lateral des PTS ---------------------------------------------
     * rawvideo ne transporte aucun timestamp et popen() ne fournit qu'un seul
     * tube, deja pris par les pixels. On detourne donc le stderr de ffmpeg
     * (ou showinfo imprime les PTS) vers un fichier qu'on relit en parallele.
     *
     * Un fichier et pas un second tube : il faudrait alterner les lectures sur
     * les deux descripteurs sous peine d'interblocage des que ffmpeg remplit
     * les 64 Ko du tube — un blocage vaut bien pire qu'un fichier temporaire.
     *
     * Sur POSIX le fichier est delie tout de suite : plus aucun nom, les deux
     * descripteurs (le notre en lecture, celui herite par le shell en
     * ecriture) gardent l'inode vivante et le noyau la recupere a la
     * fermeture. Rien a nettoyer, meme quand le plugin nous tue par SIGKILL —
     * ce qu'il fait a chaque stop et a chaque seek.
     *
     * Un echec ici n'est pas fatal : on replie sur des PTS synthetises, ce qui
     * vaut mieux que pas de flux du tout. */
    FILE *pts_fp = NULL;
    char redirect[600] = ""; /* assez large pour "2>" + un chemin Windows cite */
    const char *stderr_redirect = NULL;
#ifdef _WIN32
    /* cmd.exe ne sait pas heriter un descripteur nomme : on passe par un
     * chemin, supprime par l'atexit. */
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
        /* mkstemp plutot qu'un nom previsible : O_EXCL et mode 0600, donc pas
         * de course sur un lien symbolique pre-pose dans un /tmp partage. */
        int write_fd = mkstemp(tmp_path);
        if (write_fd >= 0) {
            pts_fp = fopen(tmp_path, "r"); /* avant le unlink, tant qu'il a un nom */
            unlink(tmp_path);
            if (pts_fp) {
                /* Le shell herite de write_fd (popen ne pose pas FD_CLOEXEC)
                 * et y renvoie le stderr de ffmpeg. */
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
    while (1) {
        size_t bytes_read = fread(dst_buf, 1, frame_bytes, pipe_fp);
        if (bytes_read < frame_bytes) {
            break; /* Fin de la video ou erreur */
        }

        /* PTS de CETTE frame : sa ligne showinfo est deja ecrite, ffmpeg
         * journalise avant de muxer. Repli absolu (seek inclus) et non pas
         * relatif au demarrage : une frame doit porter le meme PTS qu'on
         * l'ait atteinte avec ou sans --seek, meme quand on l'a synthetise. */
        int64_t pts_us = (int64_t)(seek_s * 1e6)
                       + (int64_t)frame_count * 1000000 / target_fps;
        if (pts_fp) {
            pts_us = read_pts_us(pts_fp, pts_us);
        }

        /* Conversion de toutes les tuiles de la frame en parallele,
         * dans un buffer contigu de map_w*map_h*16384 octets */
        #pragma omp parallel for schedule(dynamic) num_threads(NUM_THREADS)
        for (int i = 0; i < total_maps; i++) {
            int row = i / map_w;
            int col = i % map_w;
            convert_tile(dst_buf, dst_stride, col * MAP_SIZE, row * MAP_SIZE,
                         out_buf + (size_t)i * MAP_PIXELS);
        }

        /* Ecriture sequentielle + flush apres chaque frame */
        uint8_t pts_be[8];
        stream_put_s64_be(pts_be, pts_us);
        stream_write(pipe_fp, pts_be, sizeof(pts_be));
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

    /* Apres le pclose : ffmpeg a fini d'ecrire, y compris son message d'erreur
     * si c'est lui qui a coupe court. */
    if (pts_fp) {
        drain_pts_log(pts_fp);
        fclose(pts_fp);
    }
    fprintf(stderr, "Flux termine: %d frames envoyees.\n", frame_count);

    free(dst_buf);
    free(out_buf);
    return 0;
}

/* ============================================================================
 * Point d'entrée
 * ========================================================================= */

int main(int argc, char *argv[]) {
    int stream_mode = 0;
    const char *palette_arg = NULL;
    double seek_arg = 0.0;
    const char *positional[4] = { NULL, NULL, NULL, NULL };
    int npos = 0;

    /* Parsing simple : les flags peuvent apparaitre avant les positionnels */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stream") == 0) {
            stream_mode = 1;
        } else if (strcmp(argv[i], "--palette") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Erreur: --palette requiert un chemin de fichier JSON\n");
                return 1;
            }
            palette_arg = argv[++i];
        } else if (strcmp(argv[i], "--dither") == 0) {
            g_dither = 1;
        } else if (strcmp(argv[i], "--seek") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Erreur: --seek requiert un temps en secondes\n");
                return 1;
            }
            seek_arg = atof(argv[++i]);
            if (seek_arg < 0.0) seek_arg = 0.0;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            /* Sans ce garde-fou une option inconnue devient un positionnel :
             * un binaire trop vieux pour un flag recent prenait "--dither"
             * pour le chemin de la video et echouait dans ffmpeg. */
            fprintf(stderr, "Erreur: option inconnue: %s\n", argv[i]);
            return 1;
        } else if (npos < 4) {
            positional[npos++] = argv[i];
        } else {
            fprintf(stderr, "Avertissement: argument supplementaire ignore: %s\n", argv[i]);
        }
    }

    /* En mode --stream, stdout est reserve au flux binaire */
    g_log = stream_mode ? stderr : stdout;
    fprintf(g_log, "=== MinecraftVideo (C natif) ===\n\n");

    if (stream_mode && !palette_arg) {
        fprintf(stderr, "Erreur: --palette <palette.json> est requis en mode --stream\n");
        return 1;
    }
    if (stream_mode && npos < 4) {
        fprintf(stderr, "Usage: mcmm --stream --palette <palette.json>"
                        " [--seek <secondes>] [--dither]"
                        " <video> <largeur> <hauteur> <fps>\n");
        return 1;
    }

    char video_path[512];
    int framerate, width, height;

    if (npos >= 4) {
        /* Mode ligne de commande */
        strncpy(video_path, positional[0], sizeof(video_path) - 1);
        video_path[sizeof(video_path) - 1] = '\0';
        width     = atoi(positional[1]);
        height    = atoi(positional[2]);
        framerate = atoi(positional[3]);
    } else {
        /* Mode interactif */
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

    /* Le mode --stream (lecture temps reel par le plugin serveur) tolere un
     * framerate plus eleve que le mode .dat ; les deux plafonds doivent rester
     * alignes avec ceux du plugin (VideoCommand.MAX_FPS / MAX_DIMENSION). */
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
        return stream_video(video_path, palette_arg, width, height, framerate, seek_arg);
    }

    return process_video(video_path, width, height, framerate);
}
