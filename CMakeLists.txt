cmake_minimum_required(VERSION 3.20)
project(fm1_cartridge VERSION 0.1.0 LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(FM1_SANITIZE "Enable ASan and UBSan for native verification" OFF)
if(FM1_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(fm1_protocol core/usb_midi.c core/usb_descriptors.c core/audio_return.c core/cartridge_protocol.c)
target_include_directories(fm1_protocol PUBLIC core/include)
target_compile_options(fm1_protocol PRIVATE -Wall -Wextra -Werror)

# One library per port: the adapter in one translation unit over its vendored upstream sources
# (-fwrapv: the fixed-point DSP relies on wraparound) plus the cartridge server and the state record
# they all share.
function(fm1_synth_library name vendor)
    add_library(${name} synth/${name}.c synth/cartridge_remote.c synth/cartridge_state.c)
    target_include_directories(${name} PUBLIC synth core/include PRIVATE third_party/${vendor}/generated)
    target_compile_options(${name} PRIVATE -fwrapv -Wno-unused-function -Wno-unused-variable ${ARGN})
    if(FM1_SANITIZE)
        # Upstream fixed-point DSP deliberately uses signed shifts/wraparound.
        # Keep bounds, alignment, use-after-free and other UB instrumentation.
        target_compile_options(${name} PRIVATE -fno-sanitize=shift-base,signed-integer-overflow)
    endif()
    target_link_libraries(${name} PUBLIC m)
endfunction()
fm1_synth_library(clipper_synth clipper)
fm1_synth_library(felucca_synth felucca)
fm1_synth_library(melodee_synth melodee -ffp-contract=off)

enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
# the simulator and the generic tests, once per port
function(fm1_port_targets port engines revision)
    add_executable(cartridge_sim_${port} sim/main.c)
    target_link_libraries(cartridge_sim_${port} PRIVATE fm1_protocol ${port}_synth)
    target_compile_options(cartridge_sim_${port} PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(cartridge_sim_${port} PRIVATE
        FM1_SIM_SYNTH_API=${port}_synth_api FM1_SIM_SYNTH_HEADER="${port}_synth.h")
    foreach(kind synth cartridge)
        add_executable(test_${kind}_${port} tests/test_${kind}.c)
        target_compile_options(test_${kind}_${port} PRIVATE -UNDEBUG)
        target_compile_definitions(test_${kind}_${port} PRIVATE
            FM1_TEST_SYNTH_API=${port}_synth_api FM1_TEST_SYNTH_HEADER="${port}_synth.h"
            FM1_TEST_ENGINES=${engines}u FM1_TEST_REVISION="${revision}")
        target_link_libraries(test_${kind}_${port} PRIVATE fm1_protocol ${port}_synth)
        add_test(NAME ${kind}_${port} COMMAND test_${kind}_${port})
    endforeach()
    add_test(NAME simulation_${port} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_simulation.py $<TARGET_FILE:cartridge_sim_${port}>)
endfunction()
fm1_port_targets(clipper 23 e69067211c4690f34e494b5b896e4585163dcc2b)
fm1_port_targets(felucca 13 63f88ec18b71c6ed4371d03d6f04dd93f4193ad3)
fm1_port_targets(melodee 5 398ada1e09d864c12f3b778a43e45c7c2a8031b8)
# the Clipper's own simulator name and tests, as documented
add_executable(cartridge_sim sim/main.c)
target_link_libraries(cartridge_sim PRIVATE fm1_protocol clipper_synth)
target_compile_options(cartridge_sim PRIVATE -Wall -Wextra -Werror)
foreach(test_name protocol audio_return clipper)
    add_executable(test_${test_name} tests/test_${test_name}.c)
    target_compile_options(test_${test_name} PRIVATE -UNDEBUG)
    target_link_libraries(test_${test_name} PRIVATE fm1_protocol clipper_synth)
    add_test(NAME ${test_name} COMMAND test_${test_name})
endforeach()
add_test(NAME upstream_integrity COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_upstream.py)
# The audio return end to end, ESP32 ring and URBs to Sloop's receiver, with the buffers the firmware's
# sources set (audio_tx.c, Kconfig.projbuild, sdkconfig.defaults; usb_host_uac's 3 URBs unless overridden);
# an edit to any of them configures again.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/main/audio_tx.c ${CMAKE_CURRENT_SOURCE_DIR}/firmware/main/Kconfig.projbuild
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/sdkconfig.defaults)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/firmware/main/audio_tx.c fm1_audio_tx)
string(REGEX MATCH "#define AUDIO_FRAMES ([0-9]+)u" fm1_match "${fm1_audio_tx}")
set(fm1_pipe_frames ${CMAKE_MATCH_1})
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/firmware/main/Kconfig.projbuild fm1_kconfig)
string(FIND "${fm1_kconfig}" "config FM1_UAC_BUFFER_BYTES" fm1_at)
if(fm1_at LESS 0)
    message(FATAL_ERROR "config FM1_UAC_BUFFER_BYTES not found in firmware/main/Kconfig.projbuild")
endif()
string(SUBSTRING "${fm1_kconfig}" ${fm1_at} -1 fm1_kconfig)
string(REGEX MATCH "default ([0-9]+)" fm1_match "${fm1_kconfig}")
set(fm1_pipe_ring ${CMAKE_MATCH_1})
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/firmware/sdkconfig.defaults fm1_defaults)
string(REGEX MATCH "CONFIG_UAC_NUM_PACKETS_PER_URB=([0-9]+)" fm1_match "${fm1_defaults}")
set(fm1_pipe_packets ${CMAKE_MATCH_1})
set(fm1_pipe_urbs 3)
if(fm1_defaults MATCHES "CONFIG_UAC_NUM_ISOC_URBS=([0-9]+)")
    set(fm1_pipe_urbs ${CMAKE_MATCH_1})
endif()
if(NOT fm1_pipe_frames OR NOT fm1_pipe_ring OR NOT fm1_pipe_packets)
    message(FATAL_ERROR "the audio return's buffer sizes were not found in the firmware sources")
endif()
add_executable(test_pipeline tests/test_pipeline.c)
target_compile_options(test_pipeline PRIVATE -UNDEBUG)
target_compile_definitions(test_pipeline PRIVATE PIPE_FRAMES=${fm1_pipe_frames}u PIPE_RING_BYTES=${fm1_pipe_ring}u
    PIPE_URBS=${fm1_pipe_urbs}u PIPE_PACKETS=${fm1_pipe_packets}u)
add_test(NAME pipeline COMMAND test_pipeline)
# the wire format both devices speak: the ESP32's copy and Sloop's must be byte-identical
add_test(NAME wire_identity COMMAND ${CMAKE_COMMAND} -E compare_files
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include/fm1/cartridge_wire.h
    ${CMAKE_CURRENT_SOURCE_DIR}/sloop-fm1/firmware/src/cartridge_wire.h)
