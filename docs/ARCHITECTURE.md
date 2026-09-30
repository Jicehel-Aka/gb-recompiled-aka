# Architecture

```
            ┌───────────────────────── console AKA ─────────────────────────┐   ┌──────── PC ────────┐
            │ main/aka_gb_main.cpp   sélecteur, écran, boutons, son, loader │   │ pc/main_pc.c (SDL2) │
            └───────────────┬─────────────────────────────┬─────────────────┘   └─────────┬──────────┘
                            │ GbrtAkaHal (fonctions de l'hôte)                              │
            ┌───────────────▼─────────────────────────────▼───────────────────────────────▼──────────┐
            │ components/gbrt_aka/src/gbrt_aka.c    boucle 59,73 Hz, entrées, .sav/.rtc, conversions  │
            │ components/gbrt_aka/src/gbrt_browser.c  navigation dossiers (partagée AKA / PC)          │
            └───────────────┬────────────────────────────────────────────────────────────────────────┘
                            │ API du runtime (gb_run_cycles, gb_reset_frame, callbacks)
            ┌───────────────▼────────────────────────────────────────────────────────────────────────┐
            │ components/gbrt_aka/runtime/   gbrt.c (bus, MBC, timers, DMA)  interpreter.c (SM83)      │
            │                                ppu.c (PPU au cycle)  audio.c (APU)                        │
            └─────────────────────────────────────────────────────────────────────────────────────────┘
```

## Les couches

**Hôte** (`main/aka_gb_main.cpp` sur la console, `pc/main_pc.c` sur PC) : dessine le sélecteur, lit les boutons, affiche l'image, joue le son, gère le retour au loader.
Il fournit à la couche suivante une structure `GbrtAkaHal` :

| Champ | Rôle |
|---|---|
| `read_buttons` | masque `GBRT_AKA_BTN_*` (1 = appuyé) — obligatoire |
| `time_us`, `sleep_us` | horloge et attente de cadence — obligatoires |
| `present_rgb555` (ou `present_rgba`, `present`) | reçoit l'image 160x144 ; `present_rgb555` est le chemin le plus léger (pas de tampon RGBA) |
| `audio_write` | échantillons stéréo 44 100 Hz entrelacés, une image à la fois |
| `should_quit` | vrai pour quitter proprement (écrit la RAM batterie) |
| `auto_frameskip`, `report`, `pixel_bgr565` | saut d'affichage, rapport de charge, format de pixel |

**Couche AKA** (`gbrt_aka.c`) : `gbrt_aka_run(hal, rom, save_dir, max_frames)` lit la ROM, choisit DMG ou CGB d'après l'octet `0x143` de l'en-tête
(`0x80` = compatible couleur → mode couleur ; `0xC0` = couleur seulement), crée le contexte du runtime, puis boucle :
entrées → une image émulée (`gb_run_cycles` jusqu'à `frame_done`, 70 224 cycles) → affichage (sauté si en retard) → audio → attente jusqu'à l'échéance (16 742 µs).
À la sortie, `gb_context_destroy` écrit la RAM batterie.

**Runtime** (`components/gbrt_aka/runtime/`, amont gb-recompiled, MIT) : émulation proprement dite. Sur l'AKA seul l'interpréteur sert (`gb_dispatch` retombe sur `gb_interpret`).
Les modules « port », « sémantique », « présentation », « mods de données » et « correctifs natifs » sont l'infrastructure du projet amont : inutilisés ici, ils sont éliminés par `--gc-sections` s'ils ne sont pas référencés.
Les modifications faites pour l'AKA sont listées dans [PATCHES.md](../PATCHES.md) ; aucune ne touche à l'émulation elle-même.

## Précision et cadence

Le runtime synchronise le PPU à chaque instruction du CPU (PPU précis au cycle, STAT/LYC, HALT) ; c'est ce qui permet de lancer 21 jeux sans réglage par jeu.
La cadence est celle de la console d'origine : 59,73 images/s. En cas de retard la couche AKA saute l'**affichage** (jamais l'émulation, donc jamais le son) sur 2 images de suite au plus, puis se resynchronise si le retard dépasse 2 images.

## Mémoire (ESP32-S3)

| Bloc | Taille | Où | Pourquoi |
|---|---|---|---|
| PPU | 69 352 o | SRAM interne | accédé à chaque point de l'image |
| Contexte + WRAM + VRAM | ~16 Ko + 32 Ko + 16 Ko | SRAM interne | données chaudes (`GBRT_CALLOC_FAST`) |
| ROM | jusqu'à plusieurs Mo | PSRAM | lecture seule (`GBRT_MALLOC_ROM`) |
| `framebuffer` 320x240 BGR565 | 153 600 o | fourni par le composant `gamebuino` | écran |
| Piste audio | 16 Ko (8192 échantillons mono) | RAM statique | tampon circulaire |

Sans ce placement ESP-IDF enverrait en PSRAM tout bloc > 16 Ko, donc le PPU et la WRAM (`components/gbrt_aka/include/gbrt_aka_alloc.h`).

## Fichiers du dépôt

| Chemin | Contenu |
|---|---|
| `main/aka_gb_main.cpp` | lanceur AKA |
| `components/gbrt_aka/src`, `include` | couche AKA, navigateur, allocation, interface publique |
| `components/gbrt_aka/runtime` | runtime gb-recompiled (sources `src/`, en-têtes `include/`) |
| `components/gamebuino`, `aka_runtime`, `aka_font` | bibliothèque AKA (écran, boutons, son, SD), socle des portages, police accentuée — fournis, non modifiés |
| `components/gbrt_aka/linker.lf`, `Kconfig` | code chaud en IRAM, options menuconfig |
| `sdkconfig.defaults`, `partitions.csv` | réglages carte AKA (repris d'AKA-Love) et table de partitions avec le loader OTA_1 |
| `pc/` | frontend SDL2 + police 8x8 |
| `tests/`, `host_test/` | tests automatiques et bancs de mesure |
| `tools/` | extraction de ROM des `.bin` META, ROM synthétiques, `run_tests.sh` |
| `SD_files/` | ce qui se copie sur la carte SD (cartouche `GB_EMULATOR`, ROM, sauvegardes) |
| `.github/workflows/` | CI PC, CI console, release |
