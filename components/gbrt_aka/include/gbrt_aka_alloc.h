/**
 * @file gbrt_aka_alloc.h
 * @brief Placement mémoire sur ESP32-S3 : SRAM interne pour les données chaudes, PSRAM pour la ROM.
 *
 * Inclus de force par le CMake du composant (option -include).
 * Par défaut ESP-IDF envoie toute allocation > 16 Ko en PSRAM (lente, via cache) : le PPU (69 Ko)
 * et la WRAM (32 Ko) s'y retrouveraient alors que ce sont les données les plus sollicitées.
 * - GBRT_CALLOC_FAST : SRAM interne (contexte, WRAM, VRAM, PPU), repli sur le tas normal si plein.
 * - GBRT_MALLOC_ROM  : PSRAM (la ROM, jusqu'à plusieurs Mo, en lecture seule), repli sur le tas normal.
 * Les blocs se libèrent avec free().
 *
 * Projet  : gb-recompiled-aka — lecteur Game Boy / Game Boy Color pour la Gamebuino AKA (ESP32-S3).
 * Auteur  : Jicehel (Jicehel-Aka)
 * Licence : voir README.md (runtime gb-recompiled : MIT, © arcanite24 ; composant gamebuino : LGPL, inclus dans components/gamebuino).
 */
#ifndef GBRT_AKA_ALLOC_H
#define GBRT_AKA_ALLOC_H

#include <stdlib.h>
#include "esp_heap_caps.h"

/* Alloue n octets mis à zéro en SRAM interne (accès rapide, sans cache) ; repli sur calloc() si la SRAM est pleine. */
static inline void *gbrt_aka_calloc_fast(size_t n) {
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}
/* Alloue n octets en PSRAM (grande capacité, plus lente) ; repli sur malloc() si indisponible. */
static inline void *gbrt_aka_malloc_rom(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

#define GBRT_CALLOC_FAST(n) gbrt_aka_calloc_fast(n)
#define GBRT_MALLOC_ROM(n) gbrt_aka_malloc_rom(n)

#endif
