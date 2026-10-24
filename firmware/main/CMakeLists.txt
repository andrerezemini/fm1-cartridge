set(sources "main.c" "usb_midi_host.c" "slots.c"
    "../../core/cartridge_protocol.c" "../../core/usb_midi.c" "../../core/usb_descriptors.c")
# (unconditional: the requirements are expanded before the sdkconfig is read)
set(requires usb esp_timer app_update esp_partition esp_app_format esp_driver_uart nvs_flash bootloader_support)
set(includes "." "../../core/include" "../../synth")
set(ldfragments)
if(CONFIG_FM1_ENABLE_UAC_TX)
    list(APPEND sources "audio_tx.c")
    list(APPEND requires espressif__usb_host_uac)
endif()
if(CONFIG_FM1_AUDIO_SYNTH)
    list(APPEND sources "../../synth/cartridge_remote.c" "../../synth/cartridge_state.c" "persist.c")
endif()
# Each port is one translation unit over its vendored upstream sources (-fwrapv: the fixed-point
# DSP relies on wraparound); only the large delay lines go to PSRAM (the .lf fragments).
if(CONFIG_FM1_AUDIO_CLIPPER)
    list(APPEND sources "../../synth/clipper_synth.c")
    list(APPEND includes "../../third_party/clipper/generated")
    list(APPEND ldfragments "clipper.lf")
    set_source_files_properties("../../synth/clipper_synth.c" PROPERTIES
        COMPILE_OPTIONS "-fwrapv;-Wno-unused-function;-Wno-unused-variable")
endif()
if(CONFIG_FM1_AUDIO_FELUCCA)
    list(APPEND sources "../../synth/felucca_synth.c")
    list(APPEND includes "../../third_party/felucca/generated")
    list(APPEND ldfragments "felucca.lf")
    set_source_files_properties("../../synth/felucca_synth.c" PROPERTIES
        COMPILE_OPTIONS "-fwrapv;-Wno-unused-function;-Wno-unused-variable")
endif()
if(CONFIG_FM1_AUDIO_MELODEE)
    list(APPEND sources "../../synth/melodee_synth.c")
    list(APPEND includes "../../third_party/melodee/generated")
    list(APPEND ldfragments "melodee.lf")
    # (-Wno-misleading-indentation: upstream's dense one-line style, an error under GCC's -Werror)
    set_source_files_properties("../../synth/melodee_synth.c" PROPERTIES
        COMPILE_OPTIONS "-fwrapv;-ffp-contract=off;-Wno-unused-function;-Wno-unused-variable;-Wno-misleading-indentation")
endif()
idf_component_register(SRCS ${sources}
    INCLUDE_DIRS ${includes}
    REQUIRES ${requires}
    LDFRAGMENTS ${ldfragments})
