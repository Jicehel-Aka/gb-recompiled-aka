# Modifications apportées au runtime gb-recompiled (MIT, (c) arcanite24)

Le dossier `components/gbrt_aka/runtime/` est une copie du runtime amont, sans SDL2/ImGui (`platform_sdl.cpp`
remplacé par `gbrt_aka.c`). Modifications, toutes conçues pour ne changer ni image ni son :

| Fichier | Changement | Effet |
|---|---|---|
| `gbrt.c` / `gbrt.h` | `gbrt_interpreter_hotspots_enabled` : coupe la table et le tri des « points chauds » de l'interpréteur (diagnostic pour le recompilateur) | ~10 % du temps CPU sur PC |
| `ppu.c` / `ppu.h` | `GBRT_NO_RGB_FRAMEBUFFER` : retire `rgb_framebuffer` (92 Ko) ; `ppu_get_color_framebuffer()` | PPU 161 520 -> 69 352 octets, une conversion image entière en moins par image |
| `gbrt.c` / `gbrt.h` | `gb_get_color_framebuffer()` | accès à l'image RGB555 |
| `gbrt.c` | `GBRT_CALLOC_FAST` / `GBRT_MALLOC_ROM` (libc par défaut) | sur ESP32 : contexte, WRAM, VRAM, PPU en SRAM interne, ROM en PSRAM |
| `differential.c` | comparaison RGBA conditionnée par `GBRT_NO_RGB_FRAMEBUFFER` | compilation sans le tampon |

Aucune de ces modifications ne touche à l'émulation elle-même. Vérification : `host_test/bench.c` compare l'empreinte de
chaque image (et du son) avant/après sur 6 ROM, 1500 images chacune : 0 différence.

Le mode `gbrt_benchmark_fast_tick_enabled` du runtime (synchronisation grossière) donne +30 à +70 % de vitesse mais change
l'image sur 4 des 6 jeux testés : il n'est **pas** activé.
