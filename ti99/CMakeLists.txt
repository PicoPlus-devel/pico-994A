# =====================================================================================
# ti99core - TI-99/4A emulation core, ported from DS994a by wavemotion-dave.
#
# The CPU / VDP / CRU / SAMS / disk / RPK sources are upstream DS994a with the
# Nintendo DS platform layer replaced (see ti99_compat.h and ti99_fileio.h).
# sn76496.c is pico-smsplus's C PSG core, standing in for upstream's ARM32 assembly.
#
# See DS994a-README.md for the upstream copyright notice - it must be preserved.
# =====================================================================================
add_library(ti99core INTERFACE)

target_sources(ti99core INTERFACE
    # --- CPU, CRU and memory map -----------------------------------------------
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu/tms9900/tms9900.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu/tms9900/tms9901.c
    # --- Video -----------------------------------------------------------------
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu/tms9918a/tms9918a.c
    # --- Sound -----------------------------------------------------------------
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu/sn76496/sn76496.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu/sn76496/sn76496_shim.c
    # --- Peripherals -----------------------------------------------------------
    ${CMAKE_CURRENT_SOURCE_DIR}/SAMS.c
    ${CMAKE_CURRENT_SOURCE_DIR}/speech.c
    ${CMAKE_CURRENT_SOURCE_DIR}/disk.c
    ${CMAKE_CURRENT_SOURCE_DIR}/pcode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cassette.c
    # --- Cartridge loading -----------------------------------------------------
    ${CMAKE_CURRENT_SOURCE_DIR}/rpk/rpk.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rpk/lowzip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rpk/yxml.c
    ${CMAKE_CURRENT_SOURCE_DIR}/CRC32.c
    # --- Port glue -------------------------------------------------------------
    ${CMAKE_CURRENT_SOURCE_DIR}/ti99_pico.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ti99_fileio.c
)

target_include_directories(ti99core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu/tms9900
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu/tms9918a
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu/sn76496
    ${CMAKE_CURRENT_SOURCE_DIR}/rpk
)
