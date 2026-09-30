# Journal des changements

## 2026-09-30 — revue complète, ROM de test, SD_files, documentation

**Corrections**
- **Identifiant de sauvegarde** : il était coupé au *premier* point du nom (`super.rc.pro.am.gb` -> `super.sav`,
  `solar.striker.gb` -> `solar.sav`, donc collisions possibles). Il ne retire plus que la *dernière* extension. Les noms de plus de 63 caractères
  (limite du runtime) sont raccourcis et complétés par un hachage, pour que deux variantes régionales aux noms longs aient chacune leur sauvegarde.
  `gbrt_aka_save_id_from_path()` est publique et testée (`tests/test_saveid.c`).
  ⚠ Les anciennes sauvegardes de ROM dont le nom contient un point (hors extension) sont à renommer : `super.sav` -> `super.rc.pro.am.sav`.
- **Cadence sur la console** (`main/aka_gb_main.cpp`) : l'attente de fin d'image passait par `vTaskDelay(pdMS_TO_TICKS(us/1000))`, qui vaut 0 tick
  quand il reste moins d'un tick : avec le tick par défaut d'ESP-IDF (10 ms) l'émulation pouvait ne jamais attendre. Avec `CONFIG_FREERTOS_HZ=1000` (tes projets, et `sdkconfig.defaults` ici)
  ce n'était pas un problème : la modification est une **précaution**, sans effet attendu à 1 ms. L'attente dort par ticks entiers puis cède le processeur jusqu'à l'échéance `esp_timer`.
  *Non testé sur la console.*
- **Composant `gamebuino` ancien** : la version fournie (sans `gb_err.h`) éteint la console à chaque appui sur RUN (`gb_buttons::update()` appelle `gb_ll_expander_power_off()`),
  donc Start = mort du jeu. Le projet embarque la version récente d'AKA-Love (RUN sans effet d'arrêt par défaut) et le lanceur appelle `set_run_power_off(false)` ; il refuse de compiler avec l'ancienne.
- **`GB_OK` / `GB_ERR`** : le lanceur les utilise maintenant via `gb_err.h` (contrat du mixeur : 0 = tampon rempli) ; `gb_core::init()` est testé (arrêt propre si le matériel est en échec).

**Ajouts**
- `components/` complété : `gamebuino` (version récente), `aka_runtime`, `aka_font`. Le lanceur a été compilé (`-Wall -Wextra -Werror`) contre les vrais en-têtes du composant, avec de simples bouchons pour les seuls en-têtes ESP-IDF.
- `sdkconfig.defaults` et `partitions.csv` (repris d'AKA-Love) : noms longs FAT, tick 1 ms, pile principale 8 Ko (le défaut de 3,5 Ko est trop juste pour le lecteur), -O2, PSRAM octale.
- `SD_files/` : cartouche `GB_EMULATOR` (`meta.json`, `screen.bmp`, `Picture.png`), ROM de test, sauvegardes.
- 8 ROM homebrew libres (6 en mode couleur exclusif) + 8 jeux GB fournis (Solar Striker, Burai Fighter Deluxe, Super R.C. Pro-Am et Batman extraits des firmwares META `.bin` avec `tools/extract_meta_rom.py` ; les `.bin` de Felix the Cat et Gargoyle's Quest contiennent exactement les mêmes ROM que les `.gb` fournis, vérifié par empreinte) + 5 gros jeux de test.
- `tools/run_tests.sh` : lance tous les tests, et toutes les ROM d'un dossier sous ASAN/UBSAN.
- CI : test des identifiants de sauvegarde ; `SD_files-<version>.zip` (firmware + meta + images + homebrew libres) joint aux releases.
- En-tête de fichier et commentaires de fonctions sur tous les fichiers ; `docs/ARCHITECTURE.md`, `docs/ROMS.md`.

**Vérifié** (PC) : 21 ROM, image et son identiques avant/après, 0 erreur ASAN/UBSAN ; voir `docs/ROMS.md`.
