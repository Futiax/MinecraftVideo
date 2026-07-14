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
#define mkdir_p(path) _mkdir(path)
#else
#include <sys/types.h>
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

/* ============================================================================
 * Table de lookup 256³ RGB → ID couleur Minecraft
 * ~16 Mo statique — remplace PIL.Image.quantize() et l'ancienne LUT exacte
 * ========================================================================= */

static uint8_t color_lut[256][256][256];

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
    printf("Palette chargee: %d couleurs (%d base x %d nuances)\n",
           palette_count, palette_count / NUM_SHADES, NUM_SHADES);
    return 0;
}

/* Pré-calcule la LUT complète — nearest-neighbor dans l'espace RGB */
static void build_full_lut(void) {
    printf("Construction de la LUT 256^3 (16 Mo)...\n");
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
    printf("LUT construite en %.2f s\n", elapsed);
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

/* Convertit une frame RGB (target_w*128 x target_h*128) en fichiers map_*.dat */
static void frame_to_maps(const uint8_t *rgb_data, int stride,
                           int map_w, int map_h,
                           int64_t base_map_id, const char *output_dir) {
    int total_maps = map_w * map_h;

    #pragma omp parallel for schedule(dynamic) num_threads(NUM_THREADS)
    for (int i = 0; i < total_maps; i++) {
        int row = i / map_w;
        int col = i % map_w;
        int pixel_y = row * MAP_SIZE;
        int pixel_x = col * MAP_SIZE;

        uint8_t id_array[MAP_PIXELS];

        for (int y = 0; y < MAP_SIZE; y++) {
            const uint8_t *line = rgb_data + (pixel_y + y) * stride + pixel_x * 3;
            for (int x = 0; x < MAP_SIZE; x++) {
                uint8_t r = line[x * 3 + 0];
                uint8_t g = line[x * 3 + 1];
                uint8_t b = line[x * 3 + 2];
                id_array[y * MAP_SIZE + x] = color_lut[r][g][b];
            }
        }

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

/* Récupère la durée de la vidéo via ffprobe */
static double get_video_duration(const char *video_path) {
    char cmd[1024];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 \"%s\"",
             video_path);
    FILE *fp = _popen(cmd, "r");
#else
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 \"%s\"",
             video_path);
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
        char cmd_audio[2048];
        snprintf(cmd_audio, sizeof(cmd_audio),
                 "ffmpeg -y -i \"%s\" -vn -c:a libvorbis -q:a 4 \"%s\"",
                 video_path, audio_path);
        int ret = system(cmd_audio);
        if (ret == 0) printf("Audio extrait et converti en OGG (Vorbis)\n");
        else printf("Avertissement: ffmpeg a retourne le code %d\n", ret);
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
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -v error -i \"%s\" -r %d -vf \"scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black\" -f rawvideo -pix_fmt rgb24 -",
             video_path, target_fps, dst_w, dst_h, dst_w, dst_h);

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
 * Point d'entrée
 * ========================================================================= */

int main(int argc, char *argv[]) {
    printf("=== MinecraftVideo (C natif) ===\n\n");

    char video_path[512];
    int framerate, width, height;

    if (argc >= 5) {
        /* Mode ligne de commande */
        strncpy(video_path, argv[1], sizeof(video_path) - 1);
        width     = atoi(argv[2]);
        height    = atoi(argv[3]);
        framerate = atoi(argv[4]);
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

    if (framerate < 1 || framerate > 20 || width <= 0 || height <= 0) {
        fprintf(stderr, "Valeurs invalides!\n");
        return 1;
    }

    return process_video(video_path, width, height, framerate);
}
