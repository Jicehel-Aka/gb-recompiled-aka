/**
 * @file aka_gb_main.cpp
 * @brief Lanceur Game Boy pour Gamebuino AKA (ESP-IDF) : sélecteur de ROM, affichage, son, boutons.
 *
 * Au-dessus du composant "gamebuino" (gb_core, gb_graphics, gb_audio_player) et de la couche gbrt_aka.
 *
 * - Sélecteur de ROM : navigue dans /sdcard/GB/ et ses sous-dossiers (*.gb, *.gbc)
 * - Image : RGB555 du PPU convertie directement dans `framebuffer` (BGR565 320x240), centrée en 1x
 *   ou agrandie 1,5x (L1 pour basculer), puis gb_graphics::update(). Saut d'affichage adaptatif si retard.
 * - Son : le PCM du runtime (stéréo 44100 Hz) est mixé en mono et servi au gb_audio_player par une « piste » à tampon circulaire.
 * - Sauvegardes : /sdcard/GBSAVES/<nom_rom>.sav (et .rtc pour les cartouches à horloge)
 *
 * Commandes en jeu : croix / joystick = croix, A = A, B = B, RUN = Start, C = Select,
 *                    L1 = zoom 1x / 1,5x, RUN + MENU (500 ms) = retour au sélecteur.
 * Dans le sélecteur : haut/bas = choisir, A = lancer ou ouvrir un dossier, B = dossier parent,
 *                     L1/R1 = page précédente/suivante, MENU (500 ms) = retour au loader.
 *
 * Projet  : gb-recompiled-aka — lecteur Game Boy / Game Boy Color pour la Gamebuino AKA (ESP32-S3).
 * Auteur  : Jicehel (Jicehel-Aka)
 * Licence : voir README.md (runtime gb-recompiled : MIT, © arcanite24 ; composant gamebuino : LGPL, non inclus).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gb_audio_player.h"
#include "gb_common.h"
#include "gb_core.h"
#include "gb_graphics.h"
#include "gbrt_aka.h"
#include "gbrt_browser.h"

#define GB_DIR   MOUNT_POINT "/GB"
#define SAVE_DIR MOUNT_POINT "/GBSAVES" /* hors de /GB : le sélecteur ne le montre pas */

static_assert(sizeof(gb_pixel) == 2, "USE_VIDEO_256_INDEXED n'est pas supporté par ce lecteur (BGR565 requis)");

/* ------------------------------------------------------------------ */
/* Matériel                                                            */
/* ------------------------------------------------------------------ */

static gb_core *g_core;       /* boutons, joystick, poll du matériel */
static gb_graphics *g_gfx;     /* écran : `framebuffer` + update() */
static gb_audio_player *g_audio; /* mixeur audio du composant gamebuino */

/* ------------------------------------------------------------------ */
/* Piste audio : tampon circulaire mono, alimenté par l'émulateur      */
/* ------------------------------------------------------------------ */

/* Piste du mixeur alimentée par l'émulateur : push_stereo() écrit (mixage mono L+R)/2), play_callback() lit à la demande du mixeur.
 * Si le tampon est plein on jette le surplus ; s'il est vide on renvoie GB_ERR (le mixeur n'envoie alors rien). */
class GbStreamTrack : public gb_audio_track_base {
public:
    void push_stereo(const int16_t *lr, size_t frames) {
        for (size_t i = 0; i < frames; ++i) {
            if (count_ >= kSize) return; /* plein : on jette le surplus */
            ring_[head_] = (int16_t)(((int32_t)lr[2 * i] + (int32_t)lr[2 * i + 1]) / 2);
            head_ = (head_ + 1) % kSize;
            ++count_;
        }
    }
    void reset() { head_ = tail_ = count_ = 0; }

    int play_callback(int16_t *out, uint16_t n) override {
        if (count_ == 0) return GB_ERR; /* rien à jouer : le mixeur n'envoie pas de tampon */
        for (uint16_t i = 0; i < n; ++i) {
            if (count_) {
                out[i] = ring_[tail_];
                tail_ = (tail_ + 1) % kSize;
                --count_;
            } else {
                out[i] = 0;
            }
        }
        return GB_OK;
    }
    void stop_playing() override { reset(); }
    bool is_playing() override { return count_ > 0; }

private:
    static const size_t kSize = 8192;
    int16_t ring_[kSize];
    size_t head_ = 0, tail_ = 0, count_ = 0;
};

static GbStreamTrack g_track;

/* ------------------------------------------------------------------ */
/* Lancement d'un jeu                                                  */
/* ------------------------------------------------------------------ */

static bool g_zoom = false;      /* false = 1x centré, true = 1,5x (bascule avec L1) */
static bool g_clear_pending = true; /* true : effacer l'écran avant la prochaine image (changement de zoom, nouveau jeu) */
static uint32_t g_exit_since = 0;    /* date (ms) où RUN+MENU est appuyé ensemble, 0 si non */

/* Chemin rapide : l'image RGB555 du PPU est convertie en BGR565 directement dans `framebuffer`
 * (aucun tampon intermédiaire), centrée en 1x ou agrandie 1,5x. */
static void gb_present_rgb555(const uint16_t *px, int w, int h, void *) { /* appelé une fois par image affichée */
    if (g_clear_pending) {
        memset(framebuffer, 0, sizeof(gb_pixel) * SCREEN_WIDTH * SCREEN_HEIGHT);
        g_clear_pending = false;
    }
    if (!g_zoom) {
        const int ox = (SCREEN_WIDTH - w) / 2, oy = (SCREEN_HEIGHT - h) / 2;
        for (int y = 0; y < h; ++y)
            gbrt_aka_rgb555_to_bgr565(px + (size_t)y * w, &framebuffer[(size_t)(oy + y) * SCREEN_WIDTH + ox], (size_t)w);
    } else {
        const int dw = w * 3 / 2, dh = h * 3 / 2;
        const int ox = (SCREEN_WIDTH - dw) / 2, oy = (SCREEN_HEIGHT - dh) / 2;
        static uint16_t line[GBRT_AKA_SCREEN_W];
        int last_sy = -1;
        for (int y = 0; y < dh; ++y) {
            int sy = y * 2 / 3;
            if (sy != last_sy) {
                gbrt_aka_rgb555_to_bgr565(px + (size_t)sy * w, line, (size_t)w);
                last_sy = sy;
            }
            gb_pixel *dst = &framebuffer[(size_t)(oy + y) * SCREEN_WIDTH + ox];
            for (int x = 0; x < dw; ++x) dst[x] = line[x * 2 / 3];
        }
    }
    g_gfx->update();
}

/* Toutes les 120 images, sur la liaison série : vitesse et charge. Charge > 100 % = trop lent. */
static void gb_report(float fps, int load, uint32_t skipped, void *) {
    printf("[gb] %.1f img/s, charge %d %%, affichages sautes %u\n", fps, load, (unsigned)skipped);
}

/* Lit les boutons (seul endroit du jeu qui appelle g_core->pool()) et les convertit en masque GBRT_AKA_BTN_* ;
 * L1 bascule le zoom. C = Select, RUN = Start. */
static uint8_t gb_read_buttons(void *) {
    g_core->pool();
    const uint16_t k = (uint16_t)(g_core->buttons.state() | g_core->joystick.state());
    if (g_core->buttons.pressed(gb_buttons::KEY_L1)) {
        g_zoom = !g_zoom;
        g_clear_pending = true;
    }
    uint8_t b = 0;
    if (k & gb_buttons::KEY_RIGHT) b |= GBRT_AKA_BTN_RIGHT;
    if (k & gb_buttons::KEY_LEFT)  b |= GBRT_AKA_BTN_LEFT;
    if (k & gb_buttons::KEY_UP)    b |= GBRT_AKA_BTN_UP;
    if (k & gb_buttons::KEY_DOWN)  b |= GBRT_AKA_BTN_DOWN;
    if (k & gb_buttons::KEY_A)     b |= GBRT_AKA_BTN_A;
    if (k & gb_buttons::KEY_B)     b |= GBRT_AKA_BTN_B;
    if (k & gb_buttons::KEY_C)     b |= GBRT_AKA_BTN_SELECT;
    if (k & gb_buttons::KEY_RUN)   b |= GBRT_AKA_BTN_START;
    return b;
}

/* Reçoit l'audio d'une image (stéréo 44,1 kHz), l'empile dans la piste puis laisse le mixeur consommer. */
static void gb_audio_write(const int16_t *lr, size_t frames, void *) {
    g_track.push_stereo(lr, frames);
    g_audio->pool();
}

/* Horloge microseconde (esp_timer, précise au µs). */
static uint64_t gb_now_us(void *) { return (uint64_t)esp_timer_get_time(); }

/* Attente de fin d'image. vTaskDelay() n'a que la résolution du tick FreeRTOS (10 ms si CONFIG_FREERTOS_HZ=100,
 * valeur par défaut d'ESP-IDF) : avec pdMS_TO_TICKS(us / 1000) une attente de 9 ms tombait à 0 tick, donc pas
 * d'attente du tout, et l'émulation pouvait tourner plus vite que la console d'origine. On dort donc par ticks entiers
 * tant qu'il reste plus d'un tick, puis on cède le processeur (taskYIELD) jusqu'à l'échéance exacte mesurée par esp_timer. */
static void gb_sleep_us(uint32_t us, void *) {
    const uint64_t end = (uint64_t)esp_timer_get_time() + us;
    const uint32_t tick_us = (uint32_t)(portTICK_PERIOD_MS * 1000);
    for (;;) {
        const uint64_t now = (uint64_t)esp_timer_get_time();
        if (now >= end) break;
        if (end - now > tick_us) vTaskDelay(1);
        else taskYIELD();
    }
}

/* Vrai quand RUN et MENU sont tenus ensemble depuis 500 ms : retour au sélecteur de ROM (et écriture de la sauvegarde). */
static bool gb_should_quit(void *) {
    const uint16_t k = g_core->buttons.state();
    const uint16_t both = gb_buttons::KEY_RUN | gb_buttons::KEY_MENU;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if ((k & both) == both) {
        if (g_exit_since == 0) g_exit_since = now;
        return now - g_exit_since >= 500;
    }
    g_exit_since = 0;
    return false;
}

/* ------------------------------------------------------------------ */
/* Sélecteur de ROM                                                    */
/* ------------------------------------------------------------------ */

/* Texte à la position (x, y) dans la couleur donnée. */
static void draw_text(int x, int y, const char *s, uint16_t color) {
    g_gfx->setColor(color);
    g_gfx->move_cursor((uint16_t)x, (uint16_t)y);
    g_gfx->print_str(s);
}

/* Affiche un message sur 1 ou 2 lignes pendant `ms` millisecondes (erreurs de lancement, dossier absent). */
static void show_message(const char *l1, const char *l2, uint32_t ms) {
    g_gfx->clear(color_black);
    draw_text(16, 100, l1, color_yellow);
    if (l2) draw_text(16, 116, l2, color_white);
    g_gfx->update();
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* Redémarre sur la partition OTA_1 qui contient le loader de la console (ne revient pas si elle existe). */
static void return_to_loader() {
    const esp_partition_t *loader =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    if (loader) {
        esp_ota_set_boot_partition(loader);
        esp_restart();
    }
}

/* Renvoie true et remplit `path` si une ROM est choisie, false pour quitter vers le loader. */
/* Sélecteur de ROM, affiché à 60 Hz. Haut/bas = choisir, L1/R1 = page, A = ouvrir/lancer, B = dossier parent. */
static bool pick_rom(GbrtBrowser *br, std::string &path) {
    const int rows = 17;
    uint32_t menu_since = 0;
    gbrt_browser_move(br, 0, rows);
    for (;;) {
        g_core->pool();
        const bool down = g_core->buttons.pressed(gb_buttons::KEY_DOWN) || g_core->joystick.pressed(gb_buttons::KEY_DOWN);
        const bool up = g_core->buttons.pressed(gb_buttons::KEY_UP) || g_core->joystick.pressed(gb_buttons::KEY_UP);
        if (down) gbrt_browser_move(br, 1, rows);
        if (up) gbrt_browser_move(br, -1, rows);
        if (g_core->buttons.pressed(gb_buttons::KEY_L1)) gbrt_browser_move(br, -rows, rows);
        if (g_core->buttons.pressed(gb_buttons::KEY_R1)) gbrt_browser_move(br, rows, rows);
        if (g_core->buttons.pressed(gb_buttons::KEY_A)) {
            char full[GBRT_BROWSER_PATH_MAX];
            int r = gbrt_browser_activate(br, full, sizeof(full));
            if (r == 1) { path = full; return true; }
            gbrt_browser_move(br, 0, rows);
        }
        if (g_core->buttons.pressed(gb_buttons::KEY_B) && gbrt_browser_up(br))
            gbrt_browser_move(br, 0, rows);

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (g_core->buttons.state() & gb_buttons::KEY_MENU) {
            if (!menu_since) menu_since = now;
            if (now - menu_since >= 500) return false;
        } else {
            menu_since = 0;
        }

        g_gfx->clear(color_black);
        char head[64];
        snprintf(head, sizeof(head), "Game Boy  %.40s", gbrt_browser_rel(br));
        draw_text(8, 6, head, color_yellow);
        if (br->count == 0) {
            draw_text(8, 40, "Dossier vide", color_white);
            draw_text(8, 56, "B : dossier parent", color_gray);
        }
        for (int i = 0; i < rows && br->top + i < br->count; ++i) {
            const int idx = br->top + i;
            const GbrtEntry &e = br->entries[idx];
            char line[64];
            snprintf(line, sizeof(line), "%c %.38s%s", (idx == br->sel) ? '>' : ' ', e.name, e.is_dir ? "/" : "");
            uint16_t col = (idx == br->sel) ? color_white : (e.is_dir ? color_lightblue : color_gray);
            draw_text(8, 26 + i * 11, line, col);
        }
        if (br->truncated) draw_text(8, 214, "Liste tronquee (512 max)", color_orange);
        draw_text(8, 228, "A:jouer/ouvrir B:retour MENU long:loader", color_darkgray);
        g_gfx->update();
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

/* ------------------------------------------------------------------ */

/* Point d'entrée ESP-IDF : initialise la console, crée /GB et /GBSAVES, puis boucle sélecteur -> jeu -> sélecteur. */
extern "C" void app_main(void) {
    static gb_core core;
    static gb_graphics gfx;
    static gb_audio_player audio;
    g_core = &core;
    g_gfx = &gfx;
    g_audio = &audio;

    core.init();
    gfx.set_refresh_rate(60); /* ~59,73 Hz natif : évite le battement avec le vsync 70/35 Hz */
    audio.add_track(&g_track, 1.0f);
    mkdir(GB_DIR, 0777);
    mkdir(SAVE_DIR, 0777);

    GbrtBrowser browser;
    if (!gbrt_browser_open(&browser, GB_DIR)) {
        show_message("Dossier /GB introuvable", "Cree-le sur la carte SD", 3000);
        return_to_loader();
        return;
    }

    for (;;) {
        std::string path;
        if (!pick_rom(&browser, path)) {
            return_to_loader();
            continue;
        }
        g_clear_pending = true;
        g_exit_since = 0;
        g_track.reset();

        GbrtAkaHal hal = {};
        hal.read_buttons = gb_read_buttons;
        hal.audio_write = gb_audio_write;
        hal.time_us = gb_now_us;
        hal.sleep_us = gb_sleep_us;
        hal.should_quit = gb_should_quit;
        hal.present_rgb555 = gb_present_rgb555;
        hal.auto_frameskip = true;
        hal.report = gb_report;

        int rc = gbrt_aka_run(&hal, path.c_str(), SAVE_DIR, 0);
        if (rc != GBRT_AKA_OK) {
            char msg[32];
            snprintf(msg, sizeof(msg), "Erreur %d", rc);
            show_message("Impossible de lancer la ROM", msg, 2500);
        }
        /* Attend le relâchement de RUN+MENU (2 s max) : sinon MENU, encore tenu, renverrait au loader. */
        for (int i = 0; i < 125; ++i) {
            g_core->pool();
            if (g_core->buttons.state() == 0) break;
            vTaskDelay(pdMS_TO_TICKS(16));
        }
    }
}
