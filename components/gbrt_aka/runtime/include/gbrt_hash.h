/**
 * @file gbrt_hash.h
 * @brief Interface SHA-256 du runtime.
 *
 * Projet  : gb-recompiled-aka — lecteur Game Boy / Game Boy Color pour la Gamebuino AKA (ESP32-S3).
 * Origine : runtime gb-recompiled (arcanite24, licence MIT), voir LICENSE ; modifications AKA listées dans PATCHES.md.
 * Rôle AKA : Support interne.
 */

#ifndef GBRT_HASH_H
#define GBRT_HASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void gbrt_sha256(const uint8_t* data, size_t size, uint8_t digest[32]);
int gbrt_sha256_matches_hex(
    const uint8_t* data,
    size_t size,
    const char* expected_hex);

#ifdef __cplusplus
}
#endif

#endif
