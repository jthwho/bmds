# bmds_storage's part of the project's configuration (devplan/update.md §1).
#
# IDF checks an app against the smallest app partition. Ours are two sizes: the main firmware's
# ota_0 (4 MB) and the recovery firmware's ota_1 (2 MB), each built on its own. So the check is
# against the partition the build is for: ota_0, or ota_1 in the recovery build
# (BMDS_APP_PARTITION). This replaces partition_table's function of the same name, which is only
# called for that check; it is included after partition_table's (IDF includes the project_include
# files in the order of the components' requirements).

if(NOT DEFINED BMDS_APP_PARTITION)
        set(BMDS_APP_PARTITION ota_0)
endif()

function(partition_table_add_check_size_target target_name)
        set(args BINARY_PATH PARTITION_TYPE PARTITION_SUBTYPE)
        set(multi_args DEPENDS)
        cmake_parse_arguments(CMD "" "${args}" "${multi_args}" ${ARGN})
        if(NOT CMD_PARTITION_SUBTYPE AND CMD_PARTITION_TYPE STREQUAL "app")
                set(CMD_PARTITION_SUBTYPE ${BMDS_APP_PARTITION})
        endif()
        idf_build_get_property(python PYTHON)
        idf_build_get_property(table_bin PARTITION_TABLE_BIN_PATH)
        set(subtype_arg)
        if(CMD_PARTITION_SUBTYPE)
                set(subtype_arg --subtype ${CMD_PARTITION_SUBTYPE})
        endif()
        set(command ${python} ${PARTITION_TABLE_CHECK_SIZES_TOOL_PATH} --offset ${PARTITION_TABLE_OFFSET}
                    partition --type ${CMD_PARTITION_TYPE} ${subtype_arg} ${table_bin} ${CMD_BINARY_PATH})
        add_custom_target(${target_name} COMMAND ${command} DEPENDS ${CMD_DEPENDS} partition_table_bin)
endfunction()
