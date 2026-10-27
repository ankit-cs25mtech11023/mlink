# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mlink_modem)

# The ring code is the Linux driver's, built verbatim (D19).
set(MLINK_DRIVER ${CMAKE_CURRENT_SOURCE_DIR}/../../driver)
target_sources(app PRIVATE src/main.c ${MLINK_DRIVER}/mlink_ring.c)
target_sources_ifdef(CONFIG_MLINK_MODEM_NET app PRIVATE src/net.c)
target_include_directories(app PRIVATE compat ${MLINK_DRIVER})
# Kernel C does arithmetic on void * (a GNU extension Kbuild allows).
set_source_files_properties(${MLINK_DRIVER}/mlink_ring.c PROPERTIES COMPILE_OPTIONS -Wno-pointer-arith)
