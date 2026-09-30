/**
 * @file bench.c
 * @brief Banc de mesure : vitesse + empreinte de CHAQUE image et hachage du son.
 *
 * Sert à prouver qu'une optimisation ne change aucun pixel ni aucun échantillon (comparer les fichiers d'empreintes).
 * usage : bench rom.gb frames [empreintes.txt]   (BENCH_PATH=c555|rgba565|rgba, GBRT_FAST=1 pour le mode rapide)
 *
 * Projet  : gb-recompiled-aka — lecteur Game Boy / Game Boy Color pour la Gamebuino AKA (ESP32-S3).
 * Auteur  : Jicehel (Jicehel-Aka)
 * Licence : voir README.md (runtime gb-recompiled : MIT, © arcanite24 ; composant gamebuino : LGPL, non inclus).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gbrt_aka.h"
#include "gbrt.h"

static uint32_t frame; static FILE *hf; static uint64_t audio_sum;
/* Trois chemins d'affichage comparables : RGBA direct, RGBA -> BGR565, RGB555 -> BGR565 ; chacun écrit « image empreinte FNV-1a ». */
static void present_rgba(const uint32_t *px, int w, int h, void *u) {
    (void)u; uint64_t x = 1469598103934665603ull;
    for (int i = 0; i < w * h; i++) { x ^= px[i]; x *= 1099511628211ull; }
    if (hf) fprintf(hf, "%u %016llx\n", frame, (unsigned long long)x);
    frame++;
}
static void present_rgba565(const uint32_t *px, int w, int h, void *u) {
    static uint16_t t[160 * 144]; gbrt_aka_rgba_to_bgr565(px, t, (size_t)w * h);
    uint64_t x = 1469598103934665603ull; for (int i = 0; i < w * h; i++) { x ^= t[i]; x *= 1099511628211ull; }
    if (hf) fprintf(hf, "%u %016llx\n", frame, (unsigned long long)x); frame++; (void)u;
}
static void present_c555(const uint16_t *px, int w, int h, void *u) {
    static uint16_t t[160 * 144]; gbrt_aka_rgb555_to_bgr565(px, t, (size_t)w * h);
    uint64_t x = 1469598103934665603ull; for (int i = 0; i < w * h; i++) { x ^= t[i]; x *= 1099511628211ull; }
    if (hf) fprintf(hf, "%u %016llx\n", frame, (unsigned long long)x); frame++; (void)u;
}
/* Même script de touches que host_test : Start/A en alternance dès l'image 120. */
static uint8_t buttons(void *u) {
    (void)u; /* même script que host_test : Start/A en alternance */
    if (frame < 120) return 0;
    uint32_t k = (frame - 120) / 20;
    if (k >= 12 || (frame - 120) % 20 >= 6) return 0;
    return (k & 1) ? GBRT_AKA_BTN_A : GBRT_AKA_BTN_START;
}
/* Cumule un hachage de tout le son produit : deux exécutions identiques donnent le même hachage. */
static void audio(const int16_t *s, size_t n, void *u) { (void)u; for (size_t i = 0; i < n * 2; i++) audio_sum = audio_sum * 31 + (uint16_t)s[i]; }
static uint64_t now_us(void *u) { (void)u; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + t.tv_nsec / 1000; }
static void sleep_us(uint32_t us, void *u) { (void)us; (void)u; }

/* Exécute N images sans attente (vitesse brute) et affiche ROM, code retour, images/s, hachage du son. */
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    if (argc >= 4) hf = fopen(argv[3], "w");
    if (getenv("GBRT_FAST")) gbrt_benchmark_fast_tick_enabled = true;
    GbrtAkaHal hal; memset(&hal, 0, sizeof hal);
    hal.read_buttons = buttons; hal.audio_write = audio; hal.time_us = now_us; hal.sleep_us = sleep_us; const char *mode = getenv("BENCH_PATH");
    if (mode && !strcmp(mode, "c555")) hal.present_rgb555 = present_c555;
    else if (mode && !strcmp(mode, "rgba565")) hal.present_rgba = present_rgba565;
    else hal.present_rgba = present_rgba;
    uint64_t t0 = now_us(NULL);
    int rc = gbrt_aka_run(&hal, argv[1], NULL, (uint32_t)atoi(argv[2]));
    double s = (now_us(NULL) - t0) / 1e6;
    printf("%-34s rc=%d %5.0f fps  audio=%016llx\n", argv[1] + (strrchr(argv[1], '/') ? strrchr(argv[1], '/') - argv[1] + 1 : 0), rc, frame / s, (unsigned long long)audio_sum);
    if (hf) fclose(hf);
    return rc;
}
