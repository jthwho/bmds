# bmds_files — what a file manager does (devplan/filebrowser.md §4f): a folder's entries read in
# batches and kept sorted (FolderModel), and copies, moves, removals, the trash, renames and new
# files with progress and questions (FileOps); a window's Back and Forward (NavigationHistory) and
# folders.conf (FolderSettings). No GUI: the panel's folder windows and the desktop
# are built on it.
#
# Portable: also built as a plain CMake library for the host tests (tests/host), on RAM disks.

set(BMDS_FILES_SRCS
        src/fileops.cpp
        src/foldermodel.cpp
        src/foldersettings.cpp
        src/folderwatches.cpp
        src/navigationhistory.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_FILES_SRCS}
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_fs)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_files STATIC ${BMDS_FILES_SRCS})
        target_include_directories(bmds_files PUBLIC include)
        target_link_libraries(bmds_files PUBLIC bmds_core bmds_fs)
        bmds_target_options(bmds_files)
endif()
