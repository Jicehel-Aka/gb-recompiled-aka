# gb-recompiled-aka — émulateur Game Boy / Game Boy Color pour Gamebuino AKA

Lecteur de ROM Game Boy et Game Boy Color pour la Gamebuino AKA (ESP32-S3), construit sur le runtime de
[gb-recompiled](https://github.com/arcanite24/gb-recompiled) (MIT). Il tourne en **mode interpréteur** :
la ROM est lue sur la carte SD, aucun code recompilé n'est nécessaire. Une version PC (Windows / Linux, SDL2) utilise exactement le même code d'émulation.

- Jeux **Game Boy** et **Game Boy Color** (mode couleur automatique d'après l'en-tête de la cartouche), cartouches ROM seule, MBC1, MBC2, MBC3 (avec horloge), MBC5.
- Son stéréo 44,1 kHz (mixé en mono sur l'AKA), sauvegardes batterie `.sav` et horloge `.rtc` sur la carte SD.
- Sélecteur de ROM avec sous-dossiers, zoom 1x / 1,5x, saut d'affichage adaptatif si la console prend du retard.

Documentation : [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (comment c'est construit) · [docs/ROMS.md](docs/ROMS.md) (ROM fournies et résultats de test) ·
[PATCHES.md](PATCHES.md) (changements du runtime amont) · [CHANGELOG.md](CHANGELOG.md).

## Installer sur la console

1. Copie **ton** `components/gamebuino/` (celui de pAKAman / Asteria) dans `components/` de ce dossier.
2. Compile comme un projet ESP-IDF habituel (`idf.py build`) ; la PSRAM doit être activée (`sdkconfig.defaults` de tes autres jeux AKA ; réglages de vitesse suggérés dans `sdkconfig.perf.example`).
3. Copie le contenu de `SD_files/` à la racine de la carte SD : `GB_EMULATOR/` (la cartouche du loader : ajoute-y `build/gb_recompiled_aka.bin` renommé `firmware.bin`)
   et `GB/` (tes ROM, en vrac ou rangées en sous-dossiers). `GBSAVES/` est créé tout seul. Détail : [SD_files/README.txt](SD_files/README.txt).

La CI GitHub produit tout cela : un tag `v*` publie `gb-recompiled-aka-<version>.bin`, `SD_files-<version>.zip` (firmware + meta.json + images + homebrew libres),
et les archives Linux / Windows.

### Les `.bin` de la META

Les `.bin` de la Gamebuino META sont des firmwares ARM Cortex-M0+ : ils ne peuvent pas s'exécuter sur l'ESP32-S3 (Xtensa). Chacun contient en revanche la ROM Game Boy d'origine, copiée telle quelle.
`tools/extract_meta_rom.py` la retrouve (en-tête et checksums vérifiés) :

    python3 tools/extract_meta_rom.py solar_striker.bin solar_striker.gb

## Commandes

| Console AKA | Action |
|---|---|
| Croix / joystick, A, B | Croix, A, B |
| RUN | Start |
| C | Select |
| L1 | zoom 1x (centré) / 1,5x |
| RUN + MENU (500 ms) | retour au sélecteur de ROM (la sauvegarde est écrite) |
| Sélecteur : haut/bas, A | choisir, lancer ou ouvrir un dossier |
| Sélecteur : B | dossier parent (L1/R1 : page précédente/suivante) |
| Sélecteur : MENU long | retour au loader |

Sur PC : flèches ou WASD, X/Espace = A, Z/B = B, Entrée = Start, Retour arrière/Maj droite = Select, F11 plein écran, Échap = retour au lanceur ([pc/LISEZMOI-PC.txt](pc/LISEZMOI-PC.txt)).

## Sauvegardes

`/GBSAVES/<nom_de_la_rom>.sav` (RAM batterie) et `.rtc` (horloge des cartouches MBC3). Le nom est celui du fichier ROM sans dossier ni **dernière** extension ;
au-delà de 63 caractères il est raccourci et complété d'un hachage (deux ROM de même nom dans deux dossiers partagent donc leur sauvegarde : renomme l'une des deux).
Un fichier de taille inattendue est refusé sans être écrasé. L'écriture est atomique (`.tmp` puis renommage) : une coupure de courant garde l'ancienne sauvegarde.

## Versions PC et releases GitHub

- `pc/` : frontend SDL2 (Windows / Linux) avec le même lanceur à dossiers que l'AKA (dossier `roms/` à côté du programme). `gb_recompiled_pc` ouvre le lanceur, `gb_recompiled_pc ma_rom.gb` lance une ROM directement.
  Compilation : `cmake -S pc -B build-pc && cmake --build build-pc` (paquet `libsdl2-dev`).
- `.github/workflows/build-pc.yml` : compile Linux et Windows (MSYS2/MinGW), lance les tests et un test de fumée avec une ROM synthétique, produit `.tar.gz` et `.zip`.
- `.github/workflows/build-aka.yml` : compile le firmware ESP32-S3 (`IDF_VERSION` en tête du fichier, v5.4 par défaut : mets la tienne) ; exige dans le dépôt `components/gamebuino/` et `sdkconfig.defaults` (+ la table de partitions si elle y est référencée).
- `.github/workflows/release.yml` : sur un tag `v*` (ou lancement manuel), publie la release. Publier : `git tag v1.0.0 && git push origin v1.0.0`.

## Tests

    tools/run_tests.sh                      # tests unitaires + ROM synthétiques (Linux / MSYS2, aucune dépendance SDL)
    tools/run_tests.sh SD_files/GB 900      # + chaque ROM du dossier pendant 900 images sous ASAN/UBSAN

| Test | Vérifie |
|---|---|
| `tests/test_browser.c` | tri, filtre .gb/.gbc, navigation, limites du navigateur de dossiers (ASAN/UBSAN) |
| `tests/test_convert.c` | les 32 768 couleurs RGB555 donnent les mêmes 565 par le chemin direct et par le chemin RGBA |
| `tests/test_saveid.c` | nom de sauvegarde : dossiers, points multiples, noms longs |
| `tests/test_cgb.c` | mode couleur : palettes, banques VRAM/WRAM, double vitesse, sprites (ROM de `tools/make_cgb_test_rom.py`) |
| `host_test/frameskip_test.c` | saut d'affichage adaptatif avec horloge virtuelle |
| `host_test/host_test.c` | banc PC sans SDL : vitesse, son, capture PPM d'une image |
| `host_test/bench.c` | empreinte de chaque image et du son : prouve qu'une optimisation ne change rien |

## Optimisation

Ce qui est en place, et ce qui a été vérifié :

| Mesure | Effet | Vérification |
|---|---|---|
| PPU sans tampon RGBA (`GBRT_NO_RGB_FRAMEBUFFER`) | 161 520 -> 69 352 octets, une conversion d'image en moins | `bench` : 0 différence |
| Table des « points chauds » de l'interpréteur coupée | ~10 % du temps CPU | idem |
| Données chaudes (contexte, WRAM, VRAM, PPU) en SRAM interne, ROM en PSRAM | évite que le PPU et la WRAM partent en PSRAM lente | lecture du code (non mesurable sur PC) |
| Code chaud du PPU, de l'interpréteur et de l'audio en IRAM (option `GBRT_AKA_IRAM`) | évite les défauts du cache d'instructions | non mesurable sur PC |
| Conversion RGB555 -> BGR565 par tables (3 lectures par pixel, directement dans `framebuffer`) | pas de tampon intermédiaire | exhaustif sur 32 768 couleurs |
| Saut d'affichage adaptatif (2 images de suite au plus) | si retard : évite conversion + transfert écran, émulation et son continuent | `frameskip_test` |

Mesuré sur PC le 30/09/2026 (21 ROM, interpréteur, sans limitation de cadence) : de ~700 à ~2 300 images/s selon le jeu, soit 12 à 38 fois la vitesse réelle (59,73 images/s). Profil : PPU ~25-35 %, `gb_tick`/timers ~15 %, audio ~10 %.
Le PPU est précis au cycle et déjà groupé par « spans » stables : aller plus vite demanderait de relâcher la précision (mode `gbrt_benchmark_fast_tick_enabled` : +30 à +70 % mais l'image change sur 4 jeux sur 6, **donc désactivé**).
Un raccourci de lecture de ROM (éviter la table de mods) a été essayé puis retiré : gain non mesurable, code amont inchangé.

**Non vérifié sur la console** (la console est en panne ; tout ce qui suit reste à confirmer dès qu'elle revient) : build ESP-IDF complet, écran, boutons, son, vitesse, mémoire. Points à surveiller :

- **Mémoire** : PPU 69 352 o + contexte 16 240 o + `framebuffer` 320x240 (153 600 o). Si `malloc` manque de SRAM, mettre le PPU en PSRAM. À la lecture d'une ROM, le fichier est d'abord lu en mémoire puis copié dans le contexte : crête = 2 x la taille de la ROM en PSRAM (2 Mo pour Pokémon Jaune ; une ROM de 8 Mo ne tiendrait pas).
- **Vitesse** : si 60 images/s ne tiennent pas à 160 MHz : 240 MHz, `GBRT_AKA_IRAM_CORE` (gbrt.c en IRAM, ~50-70 Ko), saut d'images. Le rapport de charge s'affiche toutes les 120 images sur la liaison série (`charge > 100 %` = trop lent).
- **Cadence** : `set_refresh_rate(60)` évite le battement avec le vsync ; l'attente de fin d'image (`gb_sleep_us`) a été corrigée pour ne pas dépendre du tick FreeRTOS, à confirmer à l'écran.
- **Audio** : si le mixeur du gamebuino sature ou craque, réduire le volume de la piste (`add_track(..., volume)`).

## Limites connues

- Mappers pris en charge : ROM seule, MBC1, MBC2, MBC3 (RTC), MBC5 (vérifié dans `gbrt.c`). MMM01, MBC6, MBC7, HuC1/HuC3, Game Boy Camera, TAMA5 ne sont pas gérés.
- Pas de Super Game Boy (les jeux « SGB Enhanced » tournent en Game Boy normal), pas de câble link, pas de sauvegarde d'état instantanée dans le lanceur.
- Les noms de ROM de 96 caractères ou plus sont ignorés par le sélecteur ; 512 entrées maximum par dossier.

## Licences

Runtime : MIT, © arcanite24 (`components/gbrt_aka/runtime/LICENSE`). Le composant `gamebuino` (LGPL, Gamebuino) n'est pas inclus.
Les ROM homebrew de `SD_files/GB/GBC_Homebrew` gardent la licence de leurs auteurs ([LICENCES.txt](SD_files/GB/GBC_Homebrew/LICENCES.txt)).
Les jeux commerciaux fournis pour les tests (`Jeux_GB/`, `Gros_jeux_test/`) sont exclus du dépôt git par `.gitignore` : ne les publie pas.
