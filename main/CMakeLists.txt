idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES bmds_audio bmds_clipboard bmds_net bmds_midi bmds_synth bmds_speech bmds_console bmds_drivers bmds_input bmds_kernel bmds_shell bmds_storage bmds_fs bmds_loader bmds_ui
                                     bmds_stats bmds_sysconf bmds_terminal bmds_panel bmds_widgets bmds_mplayer bmds_sysmon
                                     bmds_images bmds_imageview bmds_update bmds_updater) # built-in programs (/bin), linked whole

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/bmds.cmake)
bmds_target_options(${COMPONENT_LIB})
