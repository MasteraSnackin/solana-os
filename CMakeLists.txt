cmake_minimum_required(VERSION 3.16)
# Solana OS builds on ESP-IDF master, pinned to the commit in esp-idf.commit
# (the one this release was verified on). Another commit of master is another
# ESP-IDF: the build stops instead of silently differing. To try a newer
# master, set SOLANA_OS_IDF_UNPINNED=1 (a warning instead), verify it on a
# badge, then move esp-idf.commit.
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/esp-idf.commit SOLANA_OS_IDF_COMMIT LIMIT_COUNT 1)
execute_process(COMMAND git -C "$ENV{IDF_PATH}" rev-parse HEAD
                OUTPUT_VARIABLE idf_commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
# A checkout in ESP-IDF moves its HEAD: configure again, so the check runs.
if(EXISTS "$ENV{IDF_PATH}/.git/HEAD")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "$ENV{IDF_PATH}/.git/HEAD")
endif()
if(NOT idf_commit STREQUAL SOLANA_OS_IDF_COMMIT)
    set(idf_msg "ESP-IDF at $ENV{IDF_PATH} is commit '${idf_commit}', Solana OS is pinned to "
                "${SOLANA_OS_IDF_COMMIT} (firmware/esp-idf.commit): git -C $ENV{IDF_PATH} checkout "
                "${SOLANA_OS_IDF_COMMIT} && git -C $ENV{IDF_PATH} submodule update --init --recursive")
    if("$ENV{SOLANA_OS_IDF_UNPINNED}" STREQUAL "1")
        message(WARNING ${idf_msg})
    else()
        message(FATAL_ERROR ${idf_msg})
    endif()
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# LVGL's list of dirty areas (32 by default; it isn't in Kconfig). Past its end
# LVGL redraws the whole screen, and a moved object takes two entries before
# overlapping ones are joined: 17 moving sprites did that. 256 holds every
# object a Lua app can make; bsp_display.c merges them (merge_dirty). 3.8 KB.
# The badge's only: the emulator (emulator/CMakeLists.txt) keeps LVGL's 32.
idf_build_set_property(COMPILE_DEFINITIONS "LV_INV_BUF_SIZE=256" APPEND)
project(solana_os)
