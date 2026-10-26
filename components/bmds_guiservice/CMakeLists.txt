# bmds_guiservice — the GUI service (devplan/guiprograms.md): the toolkit run for programs that
# speak the GUI protocol on a connection (gui_open), each connection its own application.
# Registered with the loader at boot (GuiOpener): the toolkit depends on the loader, so the
# loader can't depend on this.

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS src/canvassurface.cpp
                                    src/guiatoms.cpp
                                    src/guiclasses.cpp
                                    src/guiconnection.cpp
                                    src/guilistener.cpp
                                    src/guiservice.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_kernel
                               PRIV_REQUIRES bmds_fs bmds_gui bmds_loader bmds_net bmds_sysconf bmds_ui)
        bmds_target_options(${COMPONENT_LIB})
endif()
