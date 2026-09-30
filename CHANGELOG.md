# Journal des changements

## 2026-09-30 — revue complète, ROM de test, SD_files, documentation

**Corrections**
- **Identifiant de sauvegarde** : il était coupé au *premier* point du nom (`super.rc.pro.am.gb` -> `super.sav`,
  `solar.striker.gb` -> `solar.sav`, donc collisions possibles). Il ne retire plus que la *dernière* extension. Les noms de plus de 63 caractères
  (limite du runtime) sont raccourcis et complétés par un hachage, pour que deux variantes régionales aux noms longs aient chacune leur sauvegarde.
  `gbrt_aka_save_id_from_path()` est publique et testée (`tests/test_saveid.c`).
  ⚠ Les anciennes sauvegardes de ROM dont le nom contient un point (hors extension) sont à renommer : `super.sav` -> `super.rc.pro.am.sav`.
- **Cadence sur la console** (`main/aka_gb_main.cpp`) : l'attente de fin d'image passait par `vTaskDelay(pdMS_TO_TICKS(us/1000))`, qui vaut 0 tick
  quand il reste moins d'un tick (10 ms avec `CONFIG_FREERTOS_HZ=100`, valeur par défaut) : l'émulation pouvait alors ne jamais attendre et
  tourner trop vite. L'attente dort maintenant par ticks entiers puis cède le processeur jusqu'à l'échéance mesurée par `esp_timer`.
  *Non testé sur la console (en panne) : à confirmer à l'écran, voir README.*

**Ajouts**
- `SD_files/` : cartouche `GB_EMULATOR` (`meta.json`, `screen.bmp`, `Picture.png`), ROM de test, sauvegardes.
- 8 ROM homebrew libres (6 en mode couleur exclusif) + 8 jeux GB fournis (Solar Striker, Burai Fighter Deluxe, Super R.C. Pro-Am et Batman extraits des firmwares META `.bin` avec `tools/extract_meta_rom.py` ; les `.bin` de Felix the Cat et Gargoyle's Quest contiennent exactement les mêmes ROM que les `.gb` fournis, vérifié par empreinte) + 5 gros jeux de test.
- `tools/run_tests.sh` : lance tous les tests, et toutes les ROM d'un dossier sous ASAN/UBSAN.
- CI : test des identifiants de sauvegarde ; `SD_files-<version>.zip` (firmware + meta + images + homebrew libres) joint aux releases.
- En-tête de fichier et commentaires de fonctions sur tous les fichiers ; `docs/ARCHITECTURE.md`, `docs/ROMS.md`.

**Vérifié** (PC) : 21 ROM, image et son identiques avant/après, 0 erreur ASAN/UBSAN ; voir `docs/ROMS.md`.
