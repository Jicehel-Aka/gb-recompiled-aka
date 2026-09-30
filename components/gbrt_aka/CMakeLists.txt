# ============================================================================
# Composant ESP-IDF gbrt_aka : runtime gb-recompiled (interpréteur) + couche plateforme AKA + navigateur de ROM.
#
# -O2 ; gbrt_aka_alloc.h est inclus de force pour placer données chaudes en SRAM et ROM en PSRAM ; GBRT_NO_RGB_FRAMEBUFFER économise 92 Ko.
# Projet : gb-recompiled-aka (Gamebuino AKA). Auteur : Jicehel. Voir README.md.
# ============================================================================
# Composant ESP-IDF : runtime gb-recompiled (interpréteur) + couche plateforme AKA.
file(GLOB GBRT_SRCS runtime/src/*.c)

idf_component_register(
    SRCS ${GBRT_SRCS} src/gbrt_aka.c src/gbrt_browser.c
    INCLUDE_DIRS include runtime/include
    REQUIRES esp_timer heap
    LDFRAGMENTS linker.lf
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2 -Wno-unused-function -Wno-unused-variable
    -include gbrt_aka_alloc.h)      # allocations : SRAM interne / PSRAM
# Pas de tampon RGBA de 92 Ko : le PPU garde seulement son image RGB555 (le lanceur la convertit).
target_compile_definitions(${COMPONENT_LIB} PRIVATE GBRT_NO_RGB_FRAMEBUFFER=1)
