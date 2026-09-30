/**
 * @file gbrt_aka.c
 * @brief Couche plateforme AKA : fait tourner une ROM Game Boy / Game Boy Color sur le runtime gb-recompiled.
 *
 * @details Ce fichier est le « pont » entre le runtime (CPU, PPU, APU, MBC) et l'application hôte (lanceur AKA
 * ou frontend PC). Il est volontairement portable C11 : il ne dépend que de la libc et des fonctions que
 * l'hôte lui passe dans la structure GbrtAkaHal (affichage, boutons, son, horloge).
 *
 * Contenu :
 *  - conversions d'image RGB555 / RGBA -> RGB565 / BGR565 (format de l'écran AKA) et agrandissement 1,5x ;
 *  - sauvegardes batterie (.sav) et horloge (.rtc) écrites de façon atomique (fichier .tmp puis renommage) ;
 *  - traduction des boutons de la console en registres joypad Game Boy, avec l'interruption joypad ;
 *  - chargement de la ROM et configuration DMG/CGB d'après l'en-tête de la cartouche (octet 0x143) ;
 *  - boucle principale gbrt_aka_run() cadencée à 59,73 Hz avec saut d'affichage adaptatif.
 *
 * Projet  : gb-recompiled-aka — lecteur Game Boy / Game Boy Color pour la Gamebuino AKA (ESP32-S3).
 * Auteur  : Jicehel (Jicehel-Aka)
 * Licence : voir README.md (runtime gb-recompiled : MIT, © arcanite24 ; composant gamebuino : LGPL, non inclus).
 */
#include "gbrt_aka.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "gbrt.h"
#include "platform_sdl.h" /* déclare g_joypad_buttons / g_joypad_dpad */
#include "ppu.h"

/* État des boutons lu par gbrt.c (actifs à 0) quand ctx->joypad est NULL : 0xFF = rien d'appuyé.
 * g_joypad_dpad : bits 0-3 = droite, gauche, haut, bas ; g_joypad_buttons : A, B, Select, Start. */
uint8_t g_joypad_buttons = 0xFF;
uint8_t g_joypad_dpad = 0xFF;

#define GB_CPU_HZ 4194304u      /* horloge du CPU Game Boy */
#define GB_FRAME_CYCLES 70224u  /* 154 lignes x 456 cycles : durée d'une image */
/* 16 742 us par frame Game Boy (soit 59,73 images/s). */
#define FRAME_US ((uint32_t)((uint64_t)GB_FRAME_CYCLES * 1000000u / GB_CPU_HZ))
#define ROM_MAX_BYTES (8u * 1024u * 1024u) /* plus grosse ROM Game Boy existante : 8 Mo */
#define AUDIO_QUEUE_FRAMES 1024u /* 60 fps -> ~735 échantillons/frame : marge de ~40 % */

/* État de la session en cours (une seule ROM à la fois, donc un seul exemplaire global). */
typedef struct AkaState {
    const GbrtAkaHal *hal;           /* fonctions fournies par l'hôte */
    const char *save_dir;            /* dossier des .sav/.rtc (NULL : sauvegardes désactivées) */
    int16_t audio[AUDIO_QUEUE_FRAMES * 2]; /* échantillons de l'image en cours, L/R entrelacés */
    size_t audio_frames;             /* nombre de paires L/R déjà produites pour cette image */
} AkaState;

static AkaState g_state;

/* ------------------------------------------------------------------ */
/* Conversions d'image                                                 */
/* ------------------------------------------------------------------ */

/* RGBA 0xAARRGGBB -> RGB565 : on garde les 5/6/5 bits de poids fort de chaque canal. */
void gbrt_aka_rgba_to_rgb565(const uint32_t *src, uint16_t *dst, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uint32_t p = src[i]; /* 0xAARRGGBB */
        dst[i] = (uint16_t)(((p >> 8) & 0xF800u) | ((p >> 5) & 0x07E0u) | ((p >> 3) & 0x001Fu));
    }
}

/* Tables 5 bits -> canal 8 bits (formule du runtime : v*255/31), puis réduction 565.
 * Construites une fois : l'exactitude vient de la formule, pas d'une approximation. */
static uint8_t s_r5[32], s_g6[32]; /* canal 5 bits -> 5 bits (rouge/bleu) ou 6 bits (vert) du format 565 */
static bool s_lut_ready;
static void lut_init(void) {
    for (int v = 0; v < 32; ++v) {
        const unsigned c8 = (unsigned)(v * 255 / 31);
        s_r5[v] = (uint8_t)(c8 >> 3);
        s_g6[v] = (uint8_t)(c8 >> 2);
    }
    s_lut_ready = true;
}

/* RGB555 (r = bits 0-4, v = 5-9, b = 10-14) -> BGR565 de l'écran AKA (bleu dans les bits hauts). Chemin le plus rapide. */
void gbrt_aka_rgb555_to_bgr565(const uint16_t *src, uint16_t *dst, size_t count) {
    if (!s_lut_ready) lut_init();
    for (size_t i = 0; i < count; ++i) {
        const uint32_t p = src[i];
        dst[i] = (uint16_t)(s_r5[p & 31u] | (s_g6[(p >> 5) & 31u] << 5) | (s_r5[(p >> 10) & 31u] << 11));
    }
}

/* RGB555 -> RGB565 classique (utile aux hôtes PC ou à d'autres écrans). */
void gbrt_aka_rgb555_to_rgb565(const uint16_t *src, uint16_t *dst, size_t count) {
    if (!s_lut_ready) lut_init();
    for (size_t i = 0; i < count; ++i) {
        const uint32_t p = src[i];
        dst[i] = (uint16_t)((s_r5[p & 31u] << 11) | (s_g6[(p >> 5) & 31u] << 5) | s_r5[(p >> 10) & 31u]);
    }
}

/* RGBA 0xAARRGGBB -> BGR565 : même résultat que lcd_color_rgb() du composant gamebuino. */
void gbrt_aka_rgba_to_bgr565(const uint32_t *src, uint16_t *dst, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uint32_t p = src[i]; /* 0xAARRGGBB */
        dst[i] = (uint16_t)(((p >> 19) & 0x001Fu) | ((p >> 5) & 0x07E0u) | ((p << 8) & 0xF800u));
    }
}

/* Agrandissement 1,5x au plus proche voisin : chaque pixel de sortie (x, y) copie le pixel source (x*2/3, y*2/3). */
void gbrt_aka_scale_1_5x(const uint16_t *src, uint16_t *dst) {
    const int dw = GBRT_AKA_SCREEN_W * 3 / 2;
    const int dh = GBRT_AKA_SCREEN_H * 3 / 2;
    for (int y = 0; y < dh; ++y) {
        const uint16_t *row = src + (size_t)(y * 2 / 3) * GBRT_AKA_SCREEN_W;
        uint16_t *out = dst + (size_t)y * dw;
        for (int x = 0; x < dw; ++x) out[x] = row[x * 2 / 3];
    }
}

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */

/* Callback du runtime : reçoit chaque échantillon stéréo produit par l'APU (44 100 Hz) et l'empile pour l'image en cours. */
static void aka_on_audio_sample(GBContext *ctx, int16_t l, int16_t r) {
    (void)ctx;
    AkaState *s = &g_state;
    if (s->audio_frames >= AUDIO_QUEUE_FRAMES) return; /* trop de retard : on jette */
    s->audio[s->audio_frames * 2] = l;
    s->audio[s->audio_frames * 2 + 1] = r;
    s->audio_frames++;
}

/* ------------------------------------------------------------------ */
/* Sauvegardes : fichiers de taille exacte, écriture via .tmp + rename */
/* ------------------------------------------------------------------ */

/* Construit "<save_dir>/<nom>.<ext>". Renvoie false si les sauvegardes sont désactivées ou si le chemin est trop long. */
static bool build_path(char *out, size_t cap, const char *rom_name, const char *ext) {
    if (!g_state.save_dir || !g_state.save_dir[0] || !rom_name || !rom_name[0]) return false;
    int n = snprintf(out, cap, "%s/%s.%s", g_state.save_dir, rom_name, ext);
    return n > 0 && (size_t)n < cap;
}

/* Lit un fichier de taille EXACTE `size`. Un fichier de taille différente est refusé (et marqué persistence_load_failed
 * pour que le runtime ne l'écrase pas à la fermeture) : on ne risque pas de détruire une sauvegarde d'un autre émulateur. */
static bool file_load_exact(GBContext *ctx, const char *path, void *data, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t *tmp = (uint8_t *)malloc(size);
    if (!tmp) { fclose(f); return false; }
    size_t got = fread(tmp, 1, size, f);
    int extra = fgetc(f);
    fclose(f);
    if (got != size || extra != EOF) {
        /* Fichier invalide : on n'écrase pas automatiquement. */
        if (ctx) ctx->persistence_load_failed = true;
        free(tmp);
        return false;
    }
    memcpy(data, tmp, size);
    free(tmp);
    return true;
}

/* Écrit dans "<path>.tmp" puis remplace le fichier final : une coupure de courant en pleine écriture laisse
 * l'ancienne sauvegarde intacte (le renommage ne remplace pas un fichier existant sur FAT, d'où le remove()). */
static bool file_save_atomic(const char *path, const void *data, size_t size) {
    char tmp[300];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return false;
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    ok = (fflush(f) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp); return false; }
    remove(path); /* rename() n'écrase pas sur FAT */
    if (rename(tmp, path) != 0) { remove(tmp); return false; }
    return true;
}

/* Les quatre callbacks suivants branchent le runtime sur les fichiers <save_dir>/<nom>.sav (RAM batterie) et .rtc (horloge). */
static bool aka_load_battery(GBContext *ctx, const char *name, void *data, size_t size) {
    char p[300];
    return build_path(p, sizeof(p), name, "sav") && file_load_exact(ctx, p, data, size);
}
static bool aka_save_battery(GBContext *ctx, const char *name, const void *data, size_t size) {
    (void)ctx;
    char p[300];
    return build_path(p, sizeof(p), name, "sav") && file_save_atomic(p, data, size);
}
static bool aka_load_rtc(GBContext *ctx, const char *name, void *data, size_t size) {
    char p[300];
    return build_path(p, sizeof(p), name, "rtc") && file_load_exact(ctx, p, data, size);
}
static bool aka_save_rtc(GBContext *ctx, const char *name, const void *data, size_t size) {
    (void)ctx;
    char p[300];
    return build_path(p, sizeof(p), name, "rtc") && file_save_atomic(p, data, size);
}

/* ------------------------------------------------------------------ */
/* Entrées                                                             */
/* ------------------------------------------------------------------ */

/* Traduit le masque de boutons de la console (1 = appuyé) en lignes JOYP Game Boy (0 = appuyé) et lève
 * l'interruption joypad sur front descendant d'une ligne sélectionnée ; réveille aussi le CPU en HALT. */
static void apply_buttons(GBContext *ctx, uint8_t pressed) {
    uint8_t dpad = 0xFF, btn = 0xFF;
    if (pressed & GBRT_AKA_BTN_RIGHT)  dpad &= (uint8_t)~0x01;
    if (pressed & GBRT_AKA_BTN_LEFT)   dpad &= (uint8_t)~0x02;
    if (pressed & GBRT_AKA_BTN_UP)     dpad &= (uint8_t)~0x04;
    if (pressed & GBRT_AKA_BTN_DOWN)   dpad &= (uint8_t)~0x08;
    if (pressed & GBRT_AKA_BTN_A)      btn &= (uint8_t)~0x01;
    if (pressed & GBRT_AKA_BTN_B)      btn &= (uint8_t)~0x02;
    if (pressed & GBRT_AKA_BTN_SELECT) btn &= (uint8_t)~0x04;
    if (pressed & GBRT_AKA_BTN_START)  btn &= (uint8_t)~0x08;

    /* Interruption joypad : front descendant sur une ligne sélectionnée via JOYP. */
    uint8_t joyp = ctx->io[0x00];
    bool dpad_sel = !(joyp & 0x10);
    bool btn_sel = !(joyp & 0x20);
    bool edge = (dpad_sel && ((g_joypad_dpad & (uint8_t)~dpad) & 0x0F)) ||
                (btn_sel && ((g_joypad_buttons & (uint8_t)~btn) & 0x0F));

    g_joypad_dpad = dpad;
    g_joypad_buttons = btn;
    if (edge) {
        ctx->io[0x0F] |= 0x10;
        if (ctx->halted) ctx->halted = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Chargement de la ROM                                                */
/* ------------------------------------------------------------------ */

/* Lit toute la ROM en mémoire (malloc). Refuse < 0x150 octets (pas d'en-tête complet) ou > 8 Mo. */
static int read_rom(const char *path, uint8_t **out, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return GBRT_AKA_ERR_ROM_OPEN;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return GBRT_AKA_ERR_ROM_OPEN; }
    long sz = ftell(f);
    if (sz < 0x150 || (size_t)sz > ROM_MAX_BYTES) { fclose(f); return GBRT_AKA_ERR_ROM_SIZE; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return GBRT_AKA_ERR_NOMEM; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return GBRT_AKA_ERR_ROM_OPEN; }
    fclose(f);
    *out = buf;
    *out_size = (size_t)sz;
    return GBRT_AKA_OK;
}

/* Identifiant de sauvegarde = nom du fichier ROM sans dossier ni extension (la DERNIÈRE seulement :
 * "super.rc.pro.am.gb" -> "super.rc.pro.am", et non "super"). Le contexte du runtime limite l'identifiant à 63 caractères :
 * un nom plus long est raccourci puis complété par un hachage FNV-1a du nom complet, pour que deux
 * ROM aux noms longs et presque identiques (ex. variantes régionales) gardent des sauvegardes distinctes. */
void gbrt_aka_save_id_from_path(const char *path, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!path) return;
    const char *base = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    size_t len = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot != base) len = (size_t)(dot - base);

    if (len + 1 <= cap) { /* tient tel quel */
        memcpy(out, base, len);
        out[len] = 0;
        return;
    }
    /* Trop long : préfixe + '_' + 7 chiffres hexa du hachage (nécessite cap >= 10). */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) { h ^= (uint8_t)base[i]; h *= 16777619u; }
    if (cap < 10) { memcpy(out, base, cap - 1); out[cap - 1] = 0; return; }
    const size_t keep = cap - 9;
    memcpy(out, base, keep);
    snprintf(out + keep, 9, "_%07x", (unsigned)(h & 0x0FFFFFFFu));
}

/* ------------------------------------------------------------------ */
/* Boucle principale                                                   */
/* ------------------------------------------------------------------ */

/* Déroulement : valider les arguments -> lire la ROM -> configurer DMG/CGB -> créer le contexte -> boucle d'images
 * (entrées, émulation d'une image, affichage, audio, attente de cadence) -> détruire le contexte (écrit la RAM batterie). */
int gbrt_aka_run(const GbrtAkaHal *hal, const char *rom_path,
                 const char *save_dir, uint32_t max_frames) {
    if (!hal || !(hal->present || hal->present_rgba || hal->present_rgb555) || !hal->read_buttons || !hal->time_us ||
        !hal->sleep_us || !rom_path)
        return GBRT_AKA_ERR_ARGS;

    uint8_t *rom = NULL;
    size_t rom_size = 0;
    int rc = read_rom(rom_path, &rom, &rom_size);
    if (rc != GBRT_AKA_OK) return rc;

    /* Même configuration que <jeu>_default_config() du code généré. */
    const uint8_t cgb_flag = rom[0x143];
    GBConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cartridge_supports_cgb = (cgb_flag == 0x80 || cgb_flag == 0xC0);
    cfg.cartridge_requires_cgb = (cgb_flag == 0xC0);
    cfg.model = cfg.cartridge_supports_cgb ? GB_MODEL_CGB : GB_MODEL_DMG;
    cfg.enable_audio = hal->audio_write != NULL;
    cfg.enable_serial = true;
    cfg.speed_percent = 100;

    /* Comptabilité de diagnostic de l'interpréteur : inutile ici, coûteuse (~10 % du temps). */
    gbrt_interpreter_hotspots_enabled = false;
    /* Sans callback RGBA, le PPU n'a pas à produire l'image RGBA. */
    gbrt_rgb_framebuffer_enabled = !hal->present_rgb555;

    memset(&g_state, 0, sizeof(g_state));
    g_state.hal = hal;
    g_state.save_dir = save_dir;
    g_joypad_buttons = 0xFF;
    g_joypad_dpad = 0xFF;

    GBContext *ctx = gb_context_create(&cfg);
    if (!ctx) { free(rom); return GBRT_AKA_ERR_CONTEXT; }

    char save_id[64];
    gbrt_aka_save_id_from_path(rom_path, save_id, sizeof(save_id));
    gb_context_set_save_id(ctx, save_id);

    GBPlatformCallbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_audio_sample = aka_on_audio_sample;
    if (save_dir && save_dir[0]) {
        cb.load_battery_ram = aka_load_battery;
        cb.save_battery_ram = aka_save_battery;
        cb.load_rtc_data = aka_load_rtc;
        cb.save_rtc_data = aka_save_rtc;
    }
    gb_set_platform_callbacks(ctx, &cb);

    /* Équivalent de <jeu>_init(ctx). gb_context_load_rom copie la ROM. */
    if (!gb_context_load_rom(ctx, rom, rom_size)) {
        gb_context_destroy(ctx);
        free(rom);
        return GBRT_AKA_ERR_NOMEM;
    }
    free(rom); /* le contexte en garde sa propre copie */
    ctx->mbc_type = ctx->rom[0x147];
    gb_context_reset(ctx, true);

    /* Tampon 16 bits (46 Ko) seulement si l'application ne prend pas le RGBA directement. */
    uint16_t *frame565 = NULL;
    if (!hal->present_rgb555 && !hal->present_rgba)
        frame565 = (uint16_t *)malloc(sizeof(uint16_t) * GB_FRAMEBUFFER_SIZE);
    if (!hal->present_rgb555 && !hal->present_rgba && !frame565) {
        gb_context_destroy(ctx);
        return GBRT_AKA_ERR_NOMEM;
    }

    uint64_t next_deadline = hal->time_us(hal->user);
    uint32_t frames = 0;
    /* Mesure de charge et saut d'affichage adaptatif. */
    uint64_t rep_t0 = next_deadline, busy_us = 0;
    uint32_t rep_frames = 0, skipped_total = 0, skip_streak = 0;
    bool skip_present = false;
    for (;;) {
        if (hal->should_quit && hal->should_quit(hal->user)) break;
        if (max_frames && frames >= max_frames) break;

        const uint64_t frame_t0 = hal->time_us(hal->user);
        apply_buttons(ctx, hal->read_buttons(hal->user));

        g_state.audio_frames = 0;
        gb_reset_frame(ctx);
        ctx->stopped = 0;
        while (!ctx->frame_done) gb_run_cycles(ctx, 0xFFFFFFFFu);

        const uint32_t *fb = (skip_present || hal->present_rgb555) ? NULL : gb_get_framebuffer(ctx);
        if (skip_present) {
            ++skipped_total;
        } else if (hal->present_rgb555) {
            const uint16_t *c555 = gb_get_color_framebuffer(ctx);
            if (c555) hal->present_rgb555(c555, GBRT_AKA_SCREEN_W, GBRT_AKA_SCREEN_H, hal->user);
        } else if (fb) {
            if (hal->present_rgba) {
                hal->present_rgba(fb, GBRT_AKA_SCREEN_W, GBRT_AKA_SCREEN_H, hal->user);
            } else {
                if (hal->pixel_bgr565)
                    gbrt_aka_rgba_to_bgr565(fb, frame565, GB_FRAMEBUFFER_SIZE);
                else
                    gbrt_aka_rgba_to_rgb565(fb, frame565, GB_FRAMEBUFFER_SIZE);
                hal->present(frame565, GBRT_AKA_SCREEN_W, GBRT_AKA_SCREEN_H, hal->user);
            }
        }
        if (hal->audio_write && g_state.audio_frames)
            hal->audio_write(g_state.audio, g_state.audio_frames, hal->user);
        ++frames;

        /* Cadence 59,73 Hz. En cas de retard, on ne rattrape pas au-delà de 2 frames. */
        next_deadline += FRAME_US;
        uint64_t now = hal->time_us(hal->user);
        busy_us += now - frame_t0;
        const bool late = now > next_deadline;
        if (hal->auto_frameskip && late && skip_streak < 2) { skip_present = true; ++skip_streak; }
        else { skip_present = false; skip_streak = 0; }
        if (++rep_frames == 120) {
            if (hal->report) {
                const double wall = (double)(now - rep_t0);
                hal->report(wall > 0 ? (float)(rep_frames * 1e6 / wall) : 0.0f,
                            (int)(busy_us * 100 / ((uint64_t)FRAME_US * rep_frames)), skipped_total, hal->user);
            }
            rep_t0 = now; busy_us = 0; rep_frames = 0;
        }
        if (next_deadline > now)
            hal->sleep_us((uint32_t)(next_deadline - now), hal->user);
        else if (now - next_deadline > 2u * FRAME_US)
            next_deadline = now;
    }

    free(frame565);
    gb_context_destroy(ctx); /* écrit la RAM batterie */
    return GBRT_AKA_OK;
}
