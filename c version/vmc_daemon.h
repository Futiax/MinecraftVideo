// Mode --daemon : mcmm tourne en demon persistant et pilote directement l'ecran
// virtuel, la cadence FPS, le seek/pause, le diff des tuiles et la dette reseau
// par joueur via un protocole binaire sur stdin (evenements Java -> C) et
// stdout (ordres reseau C -> Java).
//
// Ce fichier est inclus par mcmm.c apres la definition des fonctions de
// conversion (load_palette, prepare_lut, data_init, vd_find_layout,
// build_decode_cmd, next_frame, data_frame_to_ids, frame_to_ids).
#ifndef VMC_DAEMON_H
#define VMC_DAEMON_H

#include <stdarg.h>
#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <errno.h>
#endif

// ==========================================================================
// Constantes du protocole binaire Java <-> C (Big-Endian)
// En-tete de chaque paquet (dans les deux sens) :
//   [uint8 opcode] [uint32 payload_len]
// ==========================================================================

// Java -> C (stdin)
#define EVT_JOIN            0x01
#define EVT_LEAVE           0x02
#define EVT_STATES          0x03
#define EVT_CMD             0x04
#define EVT_WRITABLE        0x06

// C -> Java (stdout)
#define OP_SPAWN_SCREEN     0x10
#define OP_TILES_BUNDLE     0x11
#define OP_DESTROY          0x12
#define OP_CHAT             0x15
#define OP_POSTEFFECT       0x16
#define OP_FRAME_STATS      0x17
#define OP_SESSION_START    0x18
#define OP_SESSION_END      0x19

#define DAEMON_MAX_PLAYERS  64
#define DAEMON_CONSOLE_SLOT 0xFF
#define DAEMON_MAX_DIM      16
#define DAEMON_MAX_TILES    (DAEMON_MAX_DIM * DAEMON_MAX_DIM)
#define DAEMON_MAX_FPS      20
#define DAEMON_FAKE_ID_BASE 2000000000

enum {
    RENDER_NORMAL = 0,
    RENDER_DITHER = 1,
    RENDER_STRICT = 2,
    RENDER_DATA   = 3
};

enum {
    WALL_WAN = 0,   // 4x2, minecraft:vmc_data, distance 2.3
    WALL_LAN = 1    // 5x3, minecraft:vmc_data_lan, distance 2.7
};

// ==========================================================================
// Helpers d'encodage / decodage Big-Endian et I/O sur les pipes
// ==========================================================================

static inline uint16_t dm_get_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline int32_t dm_get_s32(const uint8_t *p) {
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                   | ((uint32_t)p[2] << 8)  | (uint32_t)p[3]);
}

static inline float dm_get_f32(const uint8_t *p) {
    uint32_t u = (uint32_t)dm_get_s32(p);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static inline double dm_get_f64(const uint8_t *p) {
    uint64_t hi = (uint32_t)dm_get_s32(p);
    uint64_t lo = (uint32_t)dm_get_s32(p + 4);
    uint64_t u = (hi << 32) | lo;
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

static inline void dm_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void dm_put_s32(uint8_t *p, int32_t v) {
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)(u >> 24);
    p[1] = (uint8_t)(u >> 16);
    p[2] = (uint8_t)(u >> 8);
    p[3] = (uint8_t)u;
}

static inline void dm_put_u64(uint8_t *p, uint64_t v) {
    dm_put_s32(p,     (int32_t)(v >> 32));
    dm_put_s32(p + 4, (int32_t)(v & 0xFFFFFFFFu));
}

static inline void dm_put_f32(uint8_t *p, float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    dm_put_s32(p, (int32_t)u);
}

static inline void dm_put_f64(uint8_t *p, double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    dm_put_u64(p, u);
}

// Horloge monotone en nanosecondes pour le cadencement FPS.
static int64_t dm_now_ns(void) {
#ifdef _WIN32
    return (int64_t)(omp_get_wtime() * 1e9);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

// Lecture exacte de len octets sur stdin (bloquant une fois que l'en-tete a
// annonce un message). Retourne 0 si ok, -1 sur EOF/erreur.
static int dm_read_exact(void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t rem = len;
    while (rem > 0) {
#ifdef _WIN32
        size_t n = fread(p, 1, rem, stdin);
        if (n == 0) return -1;
#else
        ssize_t n = read(STDIN_FILENO, p, rem);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
#endif
        p += n;
        rem -= (size_t)n;
    }
    return 0;
}

// Ecriture d'un paquet vers Java sur stdout. Quitte proprement si le plugin
// ferme le pipe (arret serveur / disable).
static void dm_write_raw(const void *data, size_t len) {
    if (fwrite(data, 1, len, stdout) != len) {
        fprintf(stderr, "[daemon] Canal ferme par Java, arret.\n");
        exit(0);
    }
}

static void dm_send_header(uint8_t opcode, uint32_t payload_len) {
    uint8_t hdr[5];
    hdr[0] = opcode;
    dm_put_s32(hdr + 1, (int32_t)payload_len);
    dm_write_raw(hdr, sizeof(hdr));
}

static void dm_flush(void) {
    if (fflush(stdout) != 0) {
        fprintf(stderr, "[daemon] Erreur flush stdout, arret.\n");
        exit(0);
    }
}

// Envoie un message chat a un joueur (ou a la console si slot == 0xFF).
static void dm_chat(uint8_t slot, const char *msg) {
    size_t len = strlen(msg);
    if (len > 0xFFFF) len = 0xFFFF;
    dm_send_header(OP_CHAT, (uint32_t)(3 + len));
    uint8_t meta[3];
    meta[0] = slot;
    dm_put_u16(meta + 1, (uint16_t)len);
    dm_write_raw(meta, sizeof(meta));
    dm_write_raw(msg, len);
    dm_flush();
}

static void dm_chatf(uint8_t slot, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    dm_chat(slot, buf);
}

static void dm_send_posteffect(uint8_t slot, int enable, const char *effect) {
    if (!effect || !*effect) return;
    size_t len = strlen(effect);
    if (len > 0xFFFF) len = 0xFFFF;
    dm_send_header(OP_POSTEFFECT, (uint32_t)(4 + len));
    uint8_t meta[4];
    meta[0] = slot;
    meta[1] = (uint8_t)(enable ? 1 : 0);
    dm_put_u16(meta + 2, (uint16_t)len);
    dm_write_raw(meta, sizeof(meta));
    dm_write_raw(effect, len);
    dm_flush();
}

// ==========================================================================
// Etat global du demon (joueurs, reglages, session active)
// ==========================================================================

typedef struct {
    int     online;
    int     writable;
    int32_t world;
    double  x, y, z;
    float   yaw;
    char    name[64];
} DaemonPlayer;

typedef struct {
    int      active;
    int      paused;
    char     source[1024];
    int      render_mode;
    int      map_w;
    int      map_h;
    int      fps;
    int      total_maps;
    int      dst_w;
    int      dst_h;
    int      dst_stride;
    size_t   frame_bytes;

    // Geometrie de l'ecran dans le monde
    int32_t  world;
    double   center_x, center_y, center_z;
    double   range_sq;
    float    frame_yaw;
    int32_t  dir_data;
    int32_t  entity_ids[DAEMON_MAX_TILES];
    int32_t  map_ids[DAEMON_MAX_TILES];
    double   tile_x[DAEMON_MAX_TILES];
    double   tile_y[DAEMON_MAX_TILES];
    double   tile_z[DAEMON_MAX_TILES];
    char     effect[128];

    // Viewers et gestion de la saturation par joueur
    uint64_t viewers_mask;
    uint8_t  missed[DAEMON_MAX_PLAYERS][DAEMON_MAX_TILES];
    uint8_t  has_debt[DAEMON_MAX_PLAYERS];
    int      dropped_frames;

    // Buffers d'image et de tuiles
    uint8_t *dst_buf;
    uint8_t *cur_tiles;
    uint8_t *last_tiles;
    int      has_last_frame;

    // Sous-processus ffmpeg
    FILE    *pipe_fp;
#ifndef _WIN32
    pid_t    ffmpeg_pid;
#endif

    // Horloge et statistiques
    int64_t  play_start_ns;
    int64_t  pause_start_ns;
    int64_t  paused_accum_ns;
    int64_t  next_deadline_ns;
    int64_t  interval_ns;
    double   seek_base_s;
    int64_t  frames_in_segment;
    int64_t  frames_sent_total;
    double   last_conv_ms;
    double   last_write_s;
    clock_t  cpu_last;
} DaemonSession;

static DaemonPlayer  g_players[DAEMON_MAX_PLAYERS];
static DaemonSession g_sess;
static int32_t       g_next_fake_id = DAEMON_FAKE_ID_BASE;

// Reglages globaux (/vmc)
static int  g_cfg_w           = 4;
static int  g_cfg_h           = 3;
static int  g_cfg_fps         = 10;
static int  g_cfg_render      = RENDER_NORMAL;
static int  g_cfg_wall        = WALL_WAN;
static int  g_cfg_range       = 64;
static char g_cfg_post_effect[128] = "";

// ==========================================================================
// Gestion du sous-processus ffmpeg (ouverture et arret instantane)
// ==========================================================================

static FILE *dm_open_ffmpeg(const char *cmd
#ifndef _WIN32
                            , pid_t *out_pid
#endif
) {
#ifdef _WIN32
    return _popen(cmd, "rb");
#else
    int pfd[2];
    if (pipe(pfd) != 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return NULL;
    }
    if (pid == 0) {
        // Groupe de processus dedie : kill(-pid, SIGKILL) tuera sh ET ffmpeg
        // instantanement lors d'un /video seek ou /video stop.
        setpgid(0, 0);
        signal(SIGPIPE, SIG_DFL);
        close(pfd[0]);
        if (pfd[1] != STDOUT_FILENO) {
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[1]);
        }
        // Fermer stdin du fils pour qu'il ne lise jamais le canal Java -> C !
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull != STDIN_FILENO) close(devnull);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    *out_pid = pid;
    return fdopen(pfd[0], "r");
#endif
}

static void dm_close_ffmpeg(void) {
    if (!g_sess.pipe_fp) return;
#ifdef _WIN32
    _pclose(g_sess.pipe_fp);
    g_sess.pipe_fp = NULL;
#else
    if (g_sess.ffmpeg_pid > 0) {
        kill(-g_sess.ffmpeg_pid, SIGKILL);
    }
    fclose(g_sess.pipe_fp);
    g_sess.pipe_fp = NULL;
    if (g_sess.ffmpeg_pid > 0) {
        waitpid(g_sess.ffmpeg_pid, NULL, 0);
        g_sess.ffmpeg_pid = -1;
    }
#endif
}

// ==========================================================================
// Envoi des paquets d'ecran et de tuiles a Java
// ==========================================================================

static void dm_send_spawn_screen(uint8_t slot) {
    uint16_t n = (uint16_t)g_sess.total_maps;
    uint32_t payload_len = 1 + 4 + 4 + 2 + (uint32_t)n * 32;
    dm_send_header(OP_SPAWN_SCREEN, payload_len);

    uint8_t head[11];
    head[0] = slot;
    dm_put_f32(head + 1, g_sess.frame_yaw);
    dm_put_s32(head + 5, g_sess.dir_data);
    dm_put_u16(head + 9, n);
    dm_write_raw(head, sizeof(head));

    for (uint16_t i = 0; i < n; i++) {
        uint8_t item[32];
        dm_put_s32(item + 0,  g_sess.entity_ids[i]);
        dm_put_s32(item + 4,  g_sess.map_ids[i]);
        dm_put_f64(item + 8,  g_sess.tile_x[i]);
        dm_put_f64(item + 16, g_sess.tile_y[i]);
        dm_put_f64(item + 24, g_sess.tile_z[i]);
        dm_write_raw(item, sizeof(item));
    }
    dm_flush();
}

static void dm_send_destroy_screen(uint64_t viewer_mask) {
    if (viewer_mask == 0 || g_sess.total_maps <= 0) return;
    uint16_t n = (uint16_t)g_sess.total_maps;
    uint32_t payload_len = 8 + 2 + (uint32_t)n * 4;
    dm_send_header(OP_DESTROY, payload_len);

    uint8_t head[10];
    dm_put_u64(head, viewer_mask);
    dm_put_u16(head + 8, n);
    dm_write_raw(head, sizeof(head));

    for (uint16_t i = 0; i < n; i++) {
        uint8_t id_be[4];
        dm_put_s32(id_be, g_sess.entity_ids[i]);
        dm_write_raw(id_be, sizeof(id_be));
    }
    dm_flush();
}

// Envoie les tuiles selectionnees par tile_flags (ou toutes si tile_flags == NULL)
// aux joueurs presents dans viewer_mask.
static void dm_send_tiles_bundle(uint64_t viewer_mask, const uint8_t *tile_flags) {
    if (viewer_mask == 0 || !g_sess.cur_tiles) return;
    int total = g_sess.total_maps;
    uint16_t count = 0;
    for (int i = 0; i < total; i++) {
        if (!tile_flags || tile_flags[i]) count++;
    }
    if (count == 0) return;

    uint32_t payload_len = 8 + 2 + (uint32_t)count * (4 + MAP_PIXELS);
    dm_send_header(OP_TILES_BUNDLE, payload_len);

    uint8_t head[10];
    dm_put_u64(head, viewer_mask);
    dm_put_u16(head + 8, count);
    dm_write_raw(head, sizeof(head));

    for (int i = 0; i < total; i++) {
        if (tile_flags && !tile_flags[i]) continue;
        uint8_t id_be[4];
        dm_put_s32(id_be, g_sess.map_ids[i]);
        dm_write_raw(id_be, sizeof(id_be));
        dm_write_raw(g_sess.cur_tiles + (size_t)i * MAP_PIXELS, MAP_PIXELS);
    }
    dm_flush();
}

// ==========================================================================
// Gestion des viewers (portee, late joiners, rattrapage de dette)
// ==========================================================================

static int dm_player_in_range(int slot) {
    if (!g_players[slot].online || !g_sess.active) return 0;
    if (g_players[slot].world != g_sess.world) return 0;
    if (g_sess.range_sq <= 0.0) return 1;
    double dx = g_players[slot].x - g_sess.center_x;
    double dy = g_players[slot].y - g_sess.center_y;
    double dz = g_players[slot].z - g_sess.center_z;
    return (dx * dx + dy * dy + dz * dz) <= g_sess.range_sq;
}

static void dm_refresh_viewers(void) {
    if (!g_sess.active) return;
    for (int s = 0; s < DAEMON_MAX_PLAYERS; s++) {
        uint64_t bit = 1ULL << s;
        int was_viewer = (g_sess.viewers_mask & bit) != 0;
        int now_viewer = dm_player_in_range(s);

        if (now_viewer && !was_viewer) {
            g_sess.viewers_mask |= bit;
            memset(g_sess.missed[s], 0, sizeof(g_sess.missed[s]));
            g_sess.has_debt[s] = 0;
            dm_send_spawn_screen((uint8_t)s);
            if (g_sess.effect[0]) {
                dm_send_posteffect((uint8_t)s, 1, g_sess.effect);
            }
            if (g_sess.has_last_frame) {
                dm_send_tiles_bundle(bit, NULL);
            }
        } else if (!now_viewer && was_viewer) {
            g_sess.viewers_mask &= ~bit;
            memset(g_sess.missed[s], 0, sizeof(g_sess.missed[s]));
            g_sess.has_debt[s] = 0;
            if (g_players[s].online) {
                if (g_sess.effect[0]) {
                    dm_send_posteffect((uint8_t)s, 0, g_sess.effect);
                }
                dm_send_destroy_screen(bit);
            }
        } else if (now_viewer && g_sess.paused && g_sess.has_debt[s] && g_players[s].writable) {
            // En pause, si sa connexion redevient writable, on lui envoie ses tuiles manquees
            dm_send_tiles_bundle(bit, g_sess.missed[s]);
            memset(g_sess.missed[s], 0, sizeof(g_sess.missed[s]));
            g_sess.has_debt[s] = 0;
        }
    }
}

// ==========================================================================
// Cycle de vie d'une session video en C (start, stop, seek, tick)
// ==========================================================================

static void dm_stop_session(void) {
    if (!g_sess.active) return;
    dm_close_ffmpeg();
    if (g_sess.viewers_mask != 0) {
        if (g_sess.effect[0]) {
            for (int s = 0; s < DAEMON_MAX_PLAYERS; s++) {
                if ((g_sess.viewers_mask & (1ULL << s)) && g_players[s].online) {
                    dm_send_posteffect((uint8_t)s, 0, g_sess.effect);
                }
            }
        }
        dm_send_destroy_screen(g_sess.viewers_mask);
    }
    dm_send_header(OP_SESSION_END, 0);
    dm_flush();
    free(g_sess.dst_buf);    g_sess.dst_buf = NULL;
    free(g_sess.cur_tiles);  g_sess.cur_tiles = NULL;
    free(g_sess.last_tiles); g_sess.last_tiles = NULL;
    g_sess.active = 0;
    g_sess.paused = 0;
    g_sess.viewers_mask = 0;
    g_sess.has_last_frame = 0;
}

static int dm_start_ffmpeg_at(double seek_s) {
    dm_close_ffmpeg();
    g_dither = (g_sess.render_mode == RENDER_DITHER);
    g_strict = (g_sess.render_mode == RENDER_STRICT);
    g_data   = (g_sess.render_mode == RENDER_DATA);

    char cmd[4096];
    if (build_decode_cmd(cmd, sizeof(cmd), g_sess.source, g_sess.fps,
                         g_sess.dst_w, g_sess.dst_h, seek_s, NULL) != 0) {
        return -1;
    }
    g_sess.pipe_fp = dm_open_ffmpeg(cmd
#ifndef _WIN32
                                    , &g_sess.ffmpeg_pid
#endif
    );
    if (!g_sess.pipe_fp) return -1;

    g_sess.seek_base_s = seek_s;
    g_sess.frames_in_segment = 0;
    int64_t now = dm_now_ns();
    g_sess.play_start_ns = now;
    g_sess.paused_accum_ns = 0;
    if (g_sess.paused) {
        g_sess.pause_start_ns = now;
    }
    g_sess.next_deadline_ns = now;
    g_sess.cpu_last = clock();
    g_sess.last_write_s = 0.0;
    return 0;
}

static void dm_compute_screen_geometry(double ax, double ay, double az, float yaw,
                                       int w, int h, double distance) {
    // Direction que regarde le joueur : 0=SOUTH(+Z), 1=WEST(-X), 2=NORTH(-Z), 3=EAST(+X)
    float norm = fmodf(fmodf(yaw, 360.0f) + 360.0f, 360.0f);
    int p_modX = 0, p_modZ = 0;
    int rightX = 0, rightZ = 0;
    if (norm >= 315.0f || norm < 45.0f) {
        // Joueur regarde SOUTH -> ecran tourne vers NORTH
        p_modZ = 1;
        g_sess.dir_data = 2;
        g_sess.frame_yaw = 180.0f;
        rightX = -1;
    } else if (norm < 135.0f) {
        // Joueur regarde WEST -> ecran tourne vers EAST
        p_modX = -1;
        g_sess.dir_data = 5;
        g_sess.frame_yaw = 270.0f;
        rightZ = -1;
    } else if (norm < 225.0f) {
        // Joueur regarde NORTH -> ecran tourne vers SOUTH
        p_modZ = -1;
        g_sess.dir_data = 3;
        g_sess.frame_yaw = 0.0f;
        rightX = 1;
    } else {
        // Joueur regarde EAST -> ecran tourne vers WEST
        p_modX = 1;
        g_sess.dir_data = 4;
        g_sess.frame_yaw = 90.0f;
        rightZ = 1;
    }

    int alongX = (p_modX != 0);
    int forward = p_modX + p_modZ;
    double along = alongX ? ax : az;
    double toEdge = along - floor(along);
    if (forward > 0) toEdge = 1.0 - toEdge;
    int blocks = (int)llround(distance - toEdge + 0.0625);
    if (blocks < 1) blocks = 1;
    int plane = (int)floor(along) + forward * blocks;
    double side = alongX ? az : ax;
    int first = (int)llround(side - (rightX + rightZ) * (w - 1) / 2.0 - 0.5);
    int originX = alongX ? plane : first;
    int originZ = alongX ? first : plane;
    int baseY = (int)floor(ay);

    g_sess.center_x = originX + rightX * (w - 1) / 2.0 + 0.5;
    g_sess.center_y = baseY + (h - 1) / 2.0 + 0.5;
    g_sess.center_z = originZ + rightZ * (w - 1) / 2.0 + 0.5;

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int idx = row * w + col;
            int x = originX + rightX * col;
            int z = originZ + rightZ * col;
            int y = baseY + (h - 1 - row);
            if (g_next_fake_id > INT32_MAX - 4) g_next_fake_id = DAEMON_FAKE_ID_BASE;
            g_sess.entity_ids[idx] = g_next_fake_id++;
            g_sess.map_ids[idx]    = g_next_fake_id++;
            g_sess.tile_x[idx]     = x + 0.5;
            g_sess.tile_y[idx]     = y + 0.5;
            g_sess.tile_z[idx]     = z + 0.5;
        }
    }
}

static void dm_tick_frame(void) {
    if (!g_sess.active || g_sess.paused || !g_sess.pipe_fp) return;

    g_dither = (g_sess.render_mode == RENDER_DITHER);
    g_strict = (g_sess.render_mode == RENDER_STRICT);
    g_data   = (g_sess.render_mode == RENDER_DATA);

    double wait_start = omp_get_wtime();
    if (next_frame(g_sess.pipe_fp, g_sess.dst_buf, g_sess.dst_w, g_sess.dst_h) != 0) {
        fprintf(stderr, "[daemon] Fin du flux video (%lld frames envoyees).\n",
                (long long)g_sess.frames_sent_total);
        dm_stop_session();
        return;
    }

    // Ancrage de l'horloge du segment APRES le decodage de la 1re frame :
    // l'ouverture du conteneur par ffmpeg (~140 ms) ne doit pas etre comptee
    // comme un retard de 3 frames qui declencherait une rafale au demarrage.
    if (g_sess.frames_in_segment == 0) {
        int64_t anchor_ns = dm_now_ns();
        g_sess.play_start_ns    = anchor_ns;
        g_sess.next_deadline_ns = anchor_ns;
        g_sess.paused_accum_ns  = 0;
    }

    double conv_start = omp_get_wtime();
    if (g_data) {
        data_frame_to_ids(g_sess.dst_buf, g_sess.cur_tiles);
    } else {
        frame_to_ids(g_sess.dst_buf, g_sess.dst_stride,
                     g_sess.map_w, g_sess.map_h, g_sess.cur_tiles);
    }
    double conv_end = omp_get_wtime();
    g_sess.last_conv_ms = (conv_end - conv_start) * 1000.0;

    // Diff tuile par tuile (UNE fois par frame)
    double diff_start = omp_get_wtime();
    int total = g_sess.total_maps;
    uint8_t changed[DAEMON_MAX_TILES];
    int changed_count = 0;
    for (int i = 0; i < total; i++) {
        const uint8_t *cur = g_sess.cur_tiles + (size_t)i * MAP_PIXELS;
        const uint8_t *prv = g_sess.last_tiles + (size_t)i * MAP_PIXELS;
        if (!g_sess.has_last_frame || memcmp(cur, prv, MAP_PIXELS) != 0) {
            changed[i] = 1;
            changed_count++;
        } else {
            changed[i] = 0;
        }
    }
    memcpy(g_sess.last_tiles, g_sess.cur_tiles, (size_t)total * MAP_PIXELS);
    g_sess.has_last_frame = 1;
    double diff_end = omp_get_wtime();

    // Repartition entre viewers :
    // - viewer non-writable -> accumule dans missed[s], on saute son envoi
    // - viewer writable qui avait une dette -> recoit (changed | missed[s])
    // - viewer writable sans dette -> recoit le bundle partage de la frame
    double write_start = omp_get_wtime();
    uint64_t normal_mask = 0;
    int viewers_count = 0;
    int skipped_viewers = 0;
    for (int s = 0; s < DAEMON_MAX_PLAYERS; s++) {
        uint64_t bit = 1ULL << s;
        if (!(g_sess.viewers_mask & bit)) continue;
        if (!g_players[s].online) continue;
        viewers_count++;

        if (!g_players[s].writable) {
            for (int i = 0; i < total; i++) {
                if (changed[i]) {
                    g_sess.missed[s][i] = 1;
                    g_sess.has_debt[s] = 1;
                }
            }
            g_sess.dropped_frames++;
            skipped_viewers++;
            continue;
        }

        if (g_sess.has_debt[s]) {
            uint8_t union_tiles[DAEMON_MAX_TILES];
            for (int i = 0; i < total; i++) {
                union_tiles[i] = (uint8_t)(changed[i] | g_sess.missed[s][i]);
                g_sess.missed[s][i] = 0;
            }
            g_sess.has_debt[s] = 0;
            dm_send_tiles_bundle(bit, union_tiles);
        } else {
            normal_mask |= bit;
        }
    }

    if (normal_mask != 0 && changed_count > 0) {
        dm_send_tiles_bundle(normal_mask, changed);
    }
    double write_end = omp_get_wtime();

    g_sess.frames_in_segment++;
    g_sess.frames_sent_total++;
    g_sess.next_deadline_ns += g_sess.interval_ns;
    int64_t now = dm_now_ns();
    int64_t sleep_ns = g_sess.next_deadline_ns - now;

    // Envoi des statistiques de la frame a Java pour /vmc debug et /video status
    clock_t cpu_now = clock();
    uint32_t cpu_us = (uint32_t)((cpu_now - g_sess.cpu_last) * 1e6 / CLOCKS_PER_SEC);
    g_sess.cpu_last = cpu_now;

    int32_t stats_pkt[20] = {
        (int32_t)g_sess.frames_sent_total,
        (int32_t)(g_sess.seek_base_s * 1000.0),
        (int32_t)(g_sess.seek_base_s * 1000.0 + (double)(g_sess.frames_in_segment - 1) * 1000.0 / g_sess.fps),
        (int32_t)(g_sess.interval_ns / 1000),
        (int32_t)((conv_start - wait_start) * 1e6),
        (int32_t)(g_sess.last_write_s * 1e6),
        (int32_t)cpu_us,
        (int32_t)((conv_end - conv_start) * 1e6),
        g_data ? (int32_t)(vd_t[0] * 1e6) : 0,
        g_data ? (int32_t)(vd_t[1] * 1e6) : 0,
        g_data ? (int32_t)(vd_t[2] * 1e6) : 0,
        g_data ? (int32_t)(vd_t[3] * 1e6) : 0,
        g_data ? (int32_t)(vd_t[4] * 1e6) : 0,
        g_data ? (int32_t)vd_last_q : 0,
        g_data ? (int32_t)vd_last_bits : 0,
        (int32_t)((diff_end - diff_start) * 1e6),
        (int32_t)changed_count,
        (int32_t)viewers_count,
        (int32_t)skipped_viewers,
        (int32_t)(sleep_ns / 1000)
    };
    g_sess.last_write_s = write_end - write_start;

    uint8_t raw_stats[20 * 4];
    for (int i = 0; i < 20; i++) {
        dm_put_s32(raw_stats + i * 4, stats_pkt[i]);
    }
    dm_send_header(OP_FRAME_STATS, sizeof(raw_stats));
    dm_write_raw(raw_stats, sizeof(raw_stats));
    dm_flush();

    if (now - g_sess.next_deadline_ns > 2 * g_sess.interval_ns) {
        // Evite une rafale de rattrapage si le decodage a pris du retard
        g_sess.next_deadline_ns = now;
    }
}

// ==========================================================================
// Parsing des commandes (/video et /vmc) directement en C
// ==========================================================================

static double dm_current_pos_s(void) {
    if (!g_sess.active) return 0.0;
    int64_t now = dm_now_ns();
    int64_t paused_ns = g_sess.paused_accum_ns
                      + (g_sess.paused ? (now - g_sess.pause_start_ns) : 0);
    double elapsed = (double)(now - g_sess.play_start_ns - paused_ns) / 1e9;
    if (elapsed < 0.0) elapsed = 0.0;
    return g_sess.seek_base_s + elapsed;
}

static void dm_format_time(char *buf, size_t cap, double sec) {
    if (sec < 0.0) sec = 0.0;
    int total = (int)sec;
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    if (h > 0) snprintf(buf, cap, "%d:%02d:%02d", h, m, s);
    else       snprintf(buf, cap, "%02d:%02d", m, s);
}

// Parse un argument de seek ("+10", "-5", "90", "01:30", "1:02:03").
// Retourne >= 0.0 si valide, -1.0 sinon.
static double dm_parse_seek_target(const char *arg, double cur_s) {
    if (!arg || !*arg) return -1.0;
    if (arg[0] == '+' || arg[0] == '-') {
        char *end = NULL;
        double delta = strtod(arg, &end);
        if (end == arg || *end != '\0') return -1.0;
        double res = cur_s + delta;
        return res < 0.0 ? 0.0 : res;
    }
    const char *c1 = strchr(arg, ':');
    if (!c1) {
        char *end = NULL;
        double val = strtod(arg, &end);
        if (end == arg || *end != '\0' || val < 0.0) return -1.0;
        return val;
    }
    const char *c2 = strchr(c1 + 1, ':');
    if (!c2) {
        int m = atoi(arg);
        double s = atof(c1 + 1);
        if (m < 0 || s < 0.0 || s >= 60.0) return -1.0;
        return m * 60.0 + s;
    }
    int h = atoi(arg);
    int m = atoi(c1 + 1);
    double s = atof(c2 + 1);
    if (h < 0 || m < 0 || m >= 60 || s < 0.0 || s >= 60.0) return -1.0;
    return h * 3600.0 + m * 60.0 + s;
}

static const char *dm_render_name(int mode) {
    switch (mode) {
        case RENDER_DITHER: return "dither";
        case RENDER_STRICT: return "strict";
        case RENDER_DATA:   return "data";
        default:            return "normal";
    }
}

// Decoupe une ligne en tokens (gere les guillemets autour d'une URL/chemin).
static int dm_tokenize(char *line, char **tokens, int max_tokens) {
    int n = 0;
    char *p = line;
    while (*p && n < max_tokens) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            tokens[n++] = p;
            while (*p && *p != q) p++;
            if (*p == q) *p++ = '\0';
        } else {
            tokens[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    }
    return n;
}

static void dm_cmd_video_play(uint8_t slot, int32_t world,
                              double x, double y, double z, float yaw,
                              int argc, char **argv) {
    if (slot == DAEMON_CONSOLE_SLOT) {
        dm_chat(slot, "[MinecraftVideo] Only players can start a video.");
        return;
    }
    if (argc < 3) {
        dm_chat(slot, "Usage: /video play <url-or-path> [w] [h] [fps]");
        return;
    }

    int w = g_cfg_w;
    int h = g_cfg_h;
    int fps = g_cfg_fps;
    if (argc >= 4) w = atoi(argv[3]);
    if (argc >= 5) h = atoi(argv[4]);
    if (argc >= 6) fps = atoi(argv[5]);

    int render = g_cfg_render;
    double distance;
    const char *effect = g_cfg_post_effect;

    if (render == RENDER_DATA) {
        if (g_cfg_wall == WALL_LAN) {
            w = 5; h = 3; distance = 2.7; effect = "minecraft:vmc_data_lan";
        } else {
            w = 4; h = 2; distance = 2.3; effect = "minecraft:vmc_data";
        }
    } else {
        if (w < 1 || w > DAEMON_MAX_DIM || h < 1 || h > DAEMON_MAX_DIM) {
            dm_chatf(slot, "[MinecraftVideo] Screen size must be between 1x1 and %dx%d maps.",
                     DAEMON_MAX_DIM, DAEMON_MAX_DIM);
            return;
        }
        double wd = w / 2.489, hd = h / 1.400;
        double m = wd > hd ? wd : hd;
        distance = 1.5 * m;
        if (distance < 2.0) distance = 2.0;
    }

    if (fps < 1 || fps > DAEMON_MAX_FPS) {
        dm_chatf(slot, "[MinecraftVideo] fps must be between 1 and %d.", DAEMON_MAX_FPS);
        return;
    }

    dm_stop_session();

    memset(&g_sess, 0, sizeof(g_sess));
#ifndef _WIN32
    g_sess.ffmpeg_pid = -1;
#endif
    snprintf(g_sess.source, sizeof(g_sess.source), "%s", argv[2]);
    g_sess.render_mode = render;
    g_sess.map_w = w;
    g_sess.map_h = h;
    g_sess.fps = fps;
    g_sess.total_maps = w * h;
    g_sess.world = world;
    g_sess.range_sq = g_cfg_range <= 0 ? 0.0 : (double)g_cfg_range * (double)g_cfg_range;
    snprintf(g_sess.effect, sizeof(g_sess.effect), "%s", effect ? effect : "");
    g_sess.interval_ns = 1000000000LL / fps;

    if (render == RENDER_DATA) {
        const vd_layout *L = vd_find_layout(w, h);
        if (!L) {
            dm_chat(slot, "[MinecraftVideo] Internal error: invalid data wall layout.");
            return;
        }
        data_init(L);
        g_sess.dst_w = L->canvas_w;
        g_sess.dst_h = L->canvas_h;
        g_sess.dst_stride = g_sess.dst_w * 3;
        g_sess.frame_bytes = (size_t)g_sess.dst_w * (size_t)g_sess.dst_h * 3 / 2;
    } else {
        g_sess.dst_w = w * MAP_SIZE;
        g_sess.dst_h = h * MAP_SIZE;
        g_sess.dst_stride = g_sess.dst_w * 3;
        g_sess.frame_bytes = (size_t)g_sess.dst_h * (size_t)g_sess.dst_stride;
    }

    size_t tiles_bytes = (size_t)g_sess.total_maps * MAP_PIXELS;
    g_sess.dst_buf    = (uint8_t *)malloc(g_sess.frame_bytes);
    g_sess.cur_tiles  = (uint8_t *)malloc(tiles_bytes);
    g_sess.last_tiles = (uint8_t *)malloc(tiles_bytes);
    if (!g_sess.dst_buf || !g_sess.cur_tiles || !g_sess.last_tiles) {
        dm_stop_session();
        dm_chat(slot, "[MinecraftVideo] Memory allocation failed in C daemon.");
        return;
    }

    dm_compute_screen_geometry(x, y, z, yaw, w, h, distance);
    g_sess.active = 1;

    // Met a jour la position du joueur qui lance la commande pour qu'il soit vu a portee
    if (slot < DAEMON_MAX_PLAYERS) {
        g_players[slot].online = 1;
        g_players[slot].world = world;
        g_players[slot].x = x;
        g_players[slot].y = y;
        g_players[slot].z = z;
        g_players[slot].yaw = yaw;
    }

    if (dm_start_ffmpeg_at(0.0) != 0) {
        dm_stop_session();
        dm_chat(slot, "[MinecraftVideo] Failed to start ffmpeg pipe.");
        return;
    }

    // Informe Java des parametres effectifs de la session (pour l'en-tete du CSV debug)
    {
        size_t slen = strlen(g_sess.source);
        if (slen > 0xFFFF) slen = 0xFFFF;
        dm_send_header(OP_SESSION_START, (uint32_t)(10 + slen));
        uint8_t smeta[10];
        dm_put_u16(smeta + 0, (uint16_t)w);
        dm_put_u16(smeta + 2, (uint16_t)h);
        dm_put_u16(smeta + 4, (uint16_t)fps);
        smeta[6] = (uint8_t)render;
        smeta[7] = (uint8_t)g_cfg_wall;
        dm_put_u16(smeta + 8, (uint16_t)slen);
        dm_write_raw(smeta, sizeof(smeta));
        dm_write_raw(g_sess.source, slen);
        dm_flush();
    }

    dm_refresh_viewers();
    dm_chatf(slot, "[MinecraftVideo] [C-Daemon] Starting %s on a %dx%d screen at %d fps (%s)...",
             g_sess.source, w, h, fps, dm_render_name(render));
}

static void dm_handle_video_cmd(uint8_t slot, int32_t world,
                                double x, double y, double z, float yaw,
                                int argc, char **argv) {
    if (argc < 2) {
        dm_chat(slot, "[MinecraftVideo] /video <play|pause|resume|seek|stop|status>");
        return;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "play") == 0) {
        dm_cmd_video_play(slot, world, x, y, z, yaw, argc, argv);
    } else if (strcmp(sub, "stop") == 0) {
        if (!g_sess.active) {
            dm_chat(slot, "[MinecraftVideo] No video is playing.");
            return;
        }
        dm_stop_session();
        dm_chat(slot, "[MinecraftVideo] Stopped.");
    } else if (strcmp(sub, "pause") == 0) {
        if (!g_sess.active) {
            dm_chat(slot, "[MinecraftVideo] No video is playing.");
            return;
        }
        if (g_sess.paused) {
            dm_chat(slot, "[MinecraftVideo] Already paused.");
            return;
        }
        g_sess.paused = 1;
        g_sess.pause_start_ns = dm_now_ns();
        dm_chat(slot, "[MinecraftVideo] Paused.");
    } else if (strcmp(sub, "resume") == 0 || strcmp(sub, "unpause") == 0) {
        if (!g_sess.active) {
            dm_chat(slot, "[MinecraftVideo] No video is playing.");
            return;
        }
        if (!g_sess.paused) {
            dm_chat(slot, "[MinecraftVideo] Not paused.");
            return;
        }
        int64_t now = dm_now_ns();
        int64_t delta = now - g_sess.pause_start_ns;
        g_sess.paused_accum_ns += delta;
        g_sess.next_deadline_ns += delta;
        g_sess.paused = 0;
        dm_chat(slot, "[MinecraftVideo] Resumed.");
    } else if (strcmp(sub, "seek") == 0) {
        if (!g_sess.active) {
            dm_chat(slot, "[MinecraftVideo] No video is playing.");
            return;
        }
        if (argc < 3) {
            dm_chat(slot, "Usage: /video seek <+s|-s|[hh:]mm:ss>");
            return;
        }
        double target = dm_parse_seek_target(argv[2], dm_current_pos_s());
        if (target < 0.0) {
            dm_chat(slot, "Invalid seek target. Examples: +10, -30, 1:30, 01:02:03");
            return;
        }
        char tbuf[32];
        dm_format_time(tbuf, sizeof(tbuf), target);
        if (dm_start_ffmpeg_at(target) != 0) {
            dm_chat(slot, "[MinecraftVideo] Failed to seek.");
            return;
        }
        dm_chatf(slot, "[MinecraftVideo] Seeking to %s (instant C seek, LUT in RAM)...", tbuf);
    } else if (strcmp(sub, "status") == 0) {
        if (!g_sess.active) {
            dm_chat(slot, "[MinecraftVideo] No video is playing.");
            return;
        }
        char pos_str[32];
        dm_format_time(pos_str, sizeof(pos_str), dm_current_pos_s());
        int viewers = 0;
        for (int s = 0; s < DAEMON_MAX_PLAYERS; s++) {
            if (g_sess.viewers_mask & (1ULL << s)) viewers++;
        }
        dm_chatf(slot, "[MinecraftVideo] [C-Daemon] Source: %s%s",
                 g_sess.source, g_sess.paused ? " [PAUSED]" : "");
        dm_chatf(slot, "  Screen: %dx%d maps @ %d fps (%s) | Viewers: %d",
                 g_sess.map_w, g_sess.map_h, g_sess.fps,
                 dm_render_name(g_sess.render_mode), viewers);
        dm_chatf(slot, "  Position: %s | Frames: %lld | Conv: %.2f ms/frame | Dropped: %d",
                 pos_str, (long long)g_sess.frames_sent_total,
                 g_sess.last_conv_ms, g_sess.dropped_frames);
    } else {
        dm_chat(slot, "[MinecraftVideo] /video <play|pause|resume|seek|stop|status>");
    }
}

static void dm_handle_vmc_cmd(uint8_t slot, int argc, char **argv) {
    int quiet = (slot == DAEMON_CONSOLE_SLOT);
    if (argc < 2) {
        dm_chatf(slot, "[MinecraftVideo] [C-Daemon] Settings: %dx%d maps @ %d fps, render %s (wall %s)",
                 g_cfg_w, g_cfg_h, g_cfg_fps, dm_render_name(g_cfg_render),
                 g_cfg_wall == WALL_LAN ? "lan 5x3" : "wan 4x2");
        dm_chat(slot, "  /vmc size <w> <h> | /vmc fps <1-20> | /vmc render <normal|dither|strict|data> | /vmc data <wan|lan>");
        return;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "size") == 0) {
        if (argc < 4) {
            if (!quiet) dm_chatf(slot, "[MinecraftVideo] Default size: %dx%d maps.", g_cfg_w, g_cfg_h);
            return;
        }
        int w = atoi(argv[2]), h = atoi(argv[3]);
        if (w < 1 || w > DAEMON_MAX_DIM || h < 1 || h > DAEMON_MAX_DIM) {
            if (!quiet) dm_chatf(slot, "[MinecraftVideo] Size must be between 1x1 and %dx%d.",
                     DAEMON_MAX_DIM, DAEMON_MAX_DIM);
            return;
        }
        g_cfg_w = w; g_cfg_h = h;
        if (!quiet) dm_chatf(slot, "[MinecraftVideo] Default screen size set to %dx%d maps.", w, h);
    } else if (strcmp(sub, "fps") == 0) {
        if (argc < 3) {
            if (!quiet) dm_chatf(slot, "[MinecraftVideo] Default fps: %d.", g_cfg_fps);
            return;
        }
        int fps = atoi(argv[2]);
        if (fps < 1 || fps > DAEMON_MAX_FPS) {
            if (!quiet) dm_chatf(slot, "[MinecraftVideo] fps must be between 1 and %d.", DAEMON_MAX_FPS);
            return;
        }
        g_cfg_fps = fps;
        if (!quiet) dm_chatf(slot, "[MinecraftVideo] Default frame rate set to %d fps.", fps);
    } else if (strcmp(sub, "render") == 0 || strcmp(sub, "dither") == 0) {
        if (argc < 3) {
            if (!quiet) dm_chatf(slot, "[MinecraftVideo] Render mode: %s.", dm_render_name(g_cfg_render));
            return;
        }
        const char *m = argv[2];
        if (strcmp(m, "normal") == 0 || strcmp(m, "off") == 0) g_cfg_render = RENDER_NORMAL;
        else if (strcmp(m, "dither") == 0 || strcmp(m, "on") == 0) g_cfg_render = RENDER_DITHER;
        else if (strcmp(m, "strict") == 0) g_cfg_render = RENDER_STRICT;
        else if (strcmp(m, "data") == 0) g_cfg_render = RENDER_DATA;
        else {
            if (!quiet) dm_chat(slot, "[MinecraftVideo] Render mode must be normal, dither, strict or data.");
            return;
        }
        if (!quiet) dm_chatf(slot, "[MinecraftVideo] Render mode set to %s.", dm_render_name(g_cfg_render));
    } else if (strcmp(sub, "data") == 0) {
        if (argc < 3) {
            if (!quiet) dm_chatf(slot, "[MinecraftVideo] Data wall: %s.", g_cfg_wall == WALL_LAN ? "lan (5x3)" : "wan (4x2)");
            return;
        }
        if (strcmp(argv[2], "wan") == 0) g_cfg_wall = WALL_WAN;
        else if (strcmp(argv[2], "lan") == 0) g_cfg_wall = WALL_LAN;
        else {
            if (!quiet) dm_chat(slot, "[MinecraftVideo] Data wall must be wan or lan.");
            return;
        }
        if (!quiet) dm_chatf(slot, "[MinecraftVideo] Data wall set to %s.", g_cfg_wall == WALL_LAN ? "lan (5x3)" : "wan (4x2)");
    } else {
        if (!quiet) dm_chat(slot, "[MinecraftVideo] Unknown /vmc subcommand.");
    }
}

// ==========================================================================
// Reception des messages d'evenements depuis Java (stdin)
// ==========================================================================

static int dm_handle_stdin_packet(void) {
    uint8_t hdr[5];
    if (dm_read_exact(hdr, sizeof(hdr)) != 0) {
        return -1; // EOF : Java a ferme le pipe
    }
    uint8_t opcode = hdr[0];
    uint32_t len = (uint32_t)dm_get_s32(hdr + 1);
    if (len > 65536) {
        fprintf(stderr, "[daemon] Paquet stdin trop grand (%u octets), arret.\n", len);
        return -1;
    }
    uint8_t *buf = NULL;
    if (len > 0) {
        buf = (uint8_t *)malloc(len + 1);
        if (!buf) return -1;
        if (dm_read_exact(buf, len) != 0) {
            free(buf);
            return -1;
        }
        buf[len] = '\0';
    }

    switch (opcode) {
        case EVT_JOIN: {
            if (len >= 3) {
                uint8_t slot = buf[0];
                uint16_t nlen = dm_get_u16(buf + 1);
                if (slot < DAEMON_MAX_PLAYERS && (uint32_t)(3 + nlen) <= len) {
                    g_players[slot].online = 1;
                    g_players[slot].writable = 1;
                    size_t copy = nlen < sizeof(g_players[slot].name) - 1
                                ? nlen : sizeof(g_players[slot].name) - 1;
                    memcpy(g_players[slot].name, buf + 3, copy);
                    g_players[slot].name[copy] = '\0';
                }
            }
            break;
        }
        case EVT_LEAVE: {
            if (len >= 1) {
                uint8_t slot = buf[0];
                if (slot < DAEMON_MAX_PLAYERS) {
                    g_players[slot].online = 0;
                    g_sess.viewers_mask &= ~(1ULL << slot);
                    memset(g_sess.missed[slot], 0, sizeof(g_sess.missed[slot]));
                    g_sess.has_debt[slot] = 0;
                }
            }
            break;
        }
        case EVT_STATES: {
            if (len >= 1) {
                uint8_t count = buf[0];
                size_t off = 1;
                for (uint8_t i = 0; i < count && off + 34 <= len; i++, off += 34) {
                    uint8_t slot = buf[off];
                    int32_t world = dm_get_s32(buf + off + 1);
                    uint8_t writable = buf[off + 5];
                    double x = dm_get_f64(buf + off + 6);
                    double y = dm_get_f64(buf + off + 14);
                    double z = dm_get_f64(buf + off + 22);
                    float yaw = dm_get_f32(buf + off + 30);
                    if (slot < DAEMON_MAX_PLAYERS) {
                        g_players[slot].online   = 1;
                        g_players[slot].world    = world;
                        g_players[slot].writable = writable != 0;
                        g_players[slot].x        = x;
                        g_players[slot].y        = y;
                        g_players[slot].z        = z;
                        g_players[slot].yaw      = yaw;
                    }
                }
                dm_refresh_viewers();
            }
            break;
        }
        case EVT_CMD: {
            if (len >= 35) {
                uint8_t slot  = buf[0];
                int32_t world = dm_get_s32(buf + 1);
                double  x     = dm_get_f64(buf + 5);
                double  y     = dm_get_f64(buf + 13);
                double  z     = dm_get_f64(buf + 21);
                float   yaw   = dm_get_f32(buf + 29);
                uint16_t clen = dm_get_u16(buf + 33);
                if ((uint32_t)(35 + clen) <= len) {
                    char *cmdline = (char *)(buf + 35);
                    cmdline[clen] = '\0';
                    char *argv[16];
                    int argc = dm_tokenize(cmdline, argv, 16);
                    if (argc > 0) {
                        if (strcmp(argv[0], "video") == 0) {
                            dm_handle_video_cmd(slot, world, x, y, z, yaw, argc, argv);
                        } else if (strcmp(argv[0], "vmc") == 0) {
                            dm_handle_vmc_cmd(slot, argc, argv);
                        }
                    }
                }
            }
            break;
        }
        case EVT_WRITABLE: {
            if (len >= 2) {
                uint8_t slot = buf[0];
                uint8_t writable = buf[1];
                if (slot < DAEMON_MAX_PLAYERS && g_players[slot].online) {
                    g_players[slot].writable = writable != 0;
                    if (g_sess.active && g_sess.paused && g_sess.has_debt[slot] && g_players[slot].writable) {
                        dm_send_tiles_bundle(1ULL << slot, g_sess.missed[slot]);
                        memset(g_sess.missed[slot], 0, sizeof(g_sess.missed[slot]));
                        g_sess.has_debt[slot] = 0;
                    }
                }
            }
            break;
        }
        default:
            fprintf(stderr, "[daemon] Opcode inconnu: 0x%02x\n", opcode);
            break;
    }

    free(buf);
    return 0;
}

// ==========================================================================
// Boucle principale du demon C (poll sur stdin + timer monotonic des frames)
// ==========================================================================

static int run_daemon(const char *palette_path) {
#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    g_log = stderr;

    if (load_palette(palette_path) != 0) {
        return 1;
    }
    prepare_lut(palette_path);
    fprintf(stderr, "[daemon] Pret. En attente des evenements sur stdin...\n");

    while (1) {
        int timeout_ms = 200;
        if (g_sess.active && !g_sess.paused) {
            int64_t rem_ns = g_sess.next_deadline_ns - dm_now_ns();
            if (rem_ns <= 0) {
                timeout_ms = 0;
            } else {
                int64_t ms = rem_ns / 1000000LL;
                timeout_ms = ms > 200 ? 200 : (int)ms;
            }
        }

#ifdef _WIN32
        // Sous Windows sans poll() sur un pipe anonyme : lecture directe si au repos
        if (!g_sess.active || g_sess.paused) {
            if (dm_handle_stdin_packet() != 0) break;
        }
#else
        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0 && errno != EINTR) {
            break;
        }
        if (pr > 0) {
            if (pfd.revents & POLLIN) {
                if (dm_handle_stdin_packet() != 0) {
                    break;
                }
                // Draine tous les paquets deja arrives sur stdin avant de calculer la frame
                while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                    if (dm_handle_stdin_packet() != 0) {
                        goto daemon_exit;
                    }
                }
            } else if (pfd.revents & (POLLHUP | POLLERR)) {
                break;
            }
        }
#endif

        if (g_sess.active && !g_sess.paused && dm_now_ns() >= g_sess.next_deadline_ns) {
            dm_tick_frame();
        }
    }

daemon_exit:
    dm_stop_session();
    fprintf(stderr, "[daemon] Arret propre.\n");
    return 0;
}

#endif // VMC_DAEMON_H
