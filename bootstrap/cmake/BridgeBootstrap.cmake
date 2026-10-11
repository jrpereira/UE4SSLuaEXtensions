# Generic bootstrap (main.dll) for a bridge product. The bootstrap's logic is
# shared; each product supplies its identity here.
#
#   bridge_read_product_version(<out-var> <version-file>)
#       Reads the quoted MAJOR.MINOR.PATCH from `#define <PREFIX>_VERSION "x.y.z"`.
#
#   add_bridge_bootstrap(
#       PRODUCT       <name>        ProductName of the implementation DLLs and their
#                                   file prefix (<name>-<ver>.dll); log prefix
#                                   "[<name> bootstrap]"; resource "<name> Bootstrap"
#       MAGIC         <0xXXXXXXXX>  descriptor magic the implementation reports
#       VERSION_FILE  <path>        header holding the product version
#       DIST_FOLDER   <folder>      build/dist/<folder>/dlls/main.dll
#       DISPLAY_NAME  <text>        CompanyName "<text> contributors",
#                                   FileDescription "<text> bootstrap"
#       SELECTOR_FILE <path>        configured main.json copied next to main.dll
#       ENABLED_FILE  <path>        enabled.txt copied to the mod folder
#       [SCRIPT_FILE   <path>]      Lua copied to Scripts/main.lua in the mod folder
#       [SETTINGS_FILE <path>]      mod_settings.ini copied to the mod folder
#       [TARGET       <name>]       CMake target and InternalName; default <PRODUCT>Bootstrap
#       [CLAIM_NAME   <name>])      per-process mutex Local\<claim>-<pid>-97b7e501;
#                                   default UE4SSXB-<PRODUCT>
#
# Identities must differ between products: the function rejects a repeated
# product, target, magic, claim or dist folder in one build.

function(bridge_read_product_version out_var version_file)
    file(STRINGS "${version_file}" version_line REGEX "^#define [A-Za-z0-9_]+_VERSION \"")
    list(LENGTH version_line version_line_count)
    if(NOT version_line_count EQUAL 1 OR NOT version_line MATCHES "\"([0-9]+\\.[0-9]+\\.[0-9]+)\"$")
        message(FATAL_ERROR "Invalid product version in ${version_file}")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(_bridge_bootstrap_claim kind value)
    get_property(claimed GLOBAL PROPERTY "BRIDGE_BOOTSTRAP_${kind}")
    string(TOLOWER "${value}" folded)
    if(folded IN_LIST claimed)
        message(FATAL_ERROR "add_bridge_bootstrap: ${kind} '${value}' is already used by another product")
    endif()
    set_property(GLOBAL APPEND PROPERTY "BRIDGE_BOOTSTRAP_${kind}" "${folded}")
endfunction()

function(add_bridge_bootstrap)
    set(one_value PRODUCT MAGIC VERSION_FILE DIST_FOLDER DISPLAY_NAME SELECTOR_FILE ENABLED_FILE
        SCRIPT_FILE SETTINGS_FILE TARGET CLAIM_NAME)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "${one_value}" "")
    get_filename_component(source_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "add_bridge_bootstrap: unexpected arguments ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    foreach(required PRODUCT MAGIC VERSION_FILE DIST_FOLDER DISPLAY_NAME SELECTOR_FILE ENABLED_FILE)
        if(NOT ARG_${required})
            message(FATAL_ERROR "add_bridge_bootstrap: ${required} is required")
        endif()
    endforeach()
    if(NOT ARG_TARGET)
        set(ARG_TARGET "${ARG_PRODUCT}Bootstrap")
    endif()
    if(NOT ARG_CLAIM_NAME)
        set(ARG_CLAIM_NAME "UE4SSXB-${ARG_PRODUCT}")
    endif()

    # These values end up in C++ and resource string literals and in file names.
    if(NOT ARG_PRODUCT MATCHES "^[A-Za-z][A-Za-z0-9]*$")
        message(FATAL_ERROR "add_bridge_bootstrap: PRODUCT must be ASCII letters and digits")
    endif()
    string(LENGTH "${ARG_MAGIC}" magic_length)
    if(NOT ARG_MAGIC MATCHES "^0x[0-9A-Fa-f]+$" OR magic_length GREATER 10)
        message(FATAL_ERROR "add_bridge_bootstrap: MAGIC must be a 32-bit hexadecimal value such as 0x4C454231")
    endif()
    if(NOT ARG_CLAIM_NAME MATCHES "^[A-Za-z0-9-]+$")
        message(FATAL_ERROR "add_bridge_bootstrap: CLAIM_NAME must be ASCII letters, digits and hyphens")
    endif()
    if(NOT ARG_DISPLAY_NAME MATCHES "^[A-Za-z0-9 ._-]+$")
        message(FATAL_ERROR "add_bridge_bootstrap: DISPLAY_NAME has unsupported characters")
    endif()
    if(NOT ARG_DIST_FOLDER MATCHES "^[A-Za-z0-9_.-]+$")
        message(FATAL_ERROR "add_bridge_bootstrap: DIST_FOLDER must be a single folder name")
    endif()
    string(TOLOWER "${ARG_MAGIC}" magic_folded)
    math(EXPR magic_value "${magic_folded}" OUTPUT_FORMAT HEXADECIMAL)
    _bridge_bootstrap_claim(PRODUCT "${ARG_PRODUCT}")
    _bridge_bootstrap_claim(TARGET "${ARG_TARGET}")
    _bridge_bootstrap_claim(MAGIC "${magic_value}")
    _bridge_bootstrap_claim(CLAIM_NAME "${ARG_CLAIM_NAME}")
    _bridge_bootstrap_claim(DIST_FOLDER "${ARG_DIST_FOLDER}")

    bridge_read_product_version(version "${ARG_VERSION_FILE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ARG_VERSION_FILE}")
    string(REPLACE "." ";" version_parts "${version}")
    list(GET version_parts 0 BRIDGE_BOOTSTRAP_VERSION_MAJOR)
    list(GET version_parts 1 BRIDGE_BOOTSTRAP_VERSION_MINOR)
    list(GET version_parts 2 BRIDGE_BOOTSTRAP_VERSION_PATCH)
    set(BRIDGE_BOOTSTRAP_VERSION "${version}")
    set(BRIDGE_BOOTSTRAP_PRODUCT "${ARG_PRODUCT}")
    set(BRIDGE_BOOTSTRAP_DISPLAY_NAME "${ARG_DISPLAY_NAME}")
    set(BRIDGE_BOOTSTRAP_TARGET "${ARG_TARGET}")
    set(BRIDGE_BOOTSTRAP_CLAIM_NAME "${ARG_CLAIM_NAME}")
    set(BRIDGE_BOOTSTRAP_MAGIC "${ARG_MAGIC}")

    set(generated "${CMAKE_CURRENT_BINARY_DIR}/${ARG_TARGET}")
    configure_file("${source_dir}/cmake/BootstrapIdentity.hpp.in"
        "${generated}/BootstrapIdentity.hpp" @ONLY)

    if(NOT WIN32)
        return()
    endif()
    if(NOT MSVC)
        message(FATAL_ERROR "The bootstrap must be built with MSVC.")
    endif()
    enable_language(RC)
    configure_file("${source_dir}/resources/Version.rc.in" "${generated}/Version.rc" @ONLY)

    set(dist "${CMAKE_BINARY_DIR}/dist/${ARG_DIST_FOLDER}")
    add_library(${ARG_TARGET} SHARED "${source_dir}/src/Bootstrap.cpp" "${generated}/Version.rc")
    target_compile_features(${ARG_TARGET} PRIVATE cxx_std_20)
    target_include_directories(${ARG_TARGET} PRIVATE
        "${generated}"
        "${source_dir}/include"
        "${source_dir}/../contract")
    target_link_libraries(${ARG_TARGET} PRIVATE kernel32 version)
    target_compile_options(${ARG_TARGET} PRIVATE /W4 /WX /permissive-)
    set_property(TARGET ${ARG_TARGET} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    set_target_properties(${ARG_TARGET} PROPERTIES
        OUTPUT_NAME main
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${dist}/dlls"
    )
    add_custom_command(TARGET ${ARG_TARGET} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ARG_SELECTOR_FILE}" "${dist}/dlls/main.json"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ARG_ENABLED_FILE}" "${dist}/enabled.txt"
    )
    if(ARG_SCRIPT_FILE)
        add_custom_command(TARGET ${ARG_TARGET} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${dist}/Scripts"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ARG_SCRIPT_FILE}" "${dist}/Scripts/main.lua"
        )
    endif()
    if(ARG_SETTINGS_FILE)
        add_custom_command(TARGET ${ARG_TARGET} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ARG_SETTINGS_FILE}" "${dist}/mod_settings.ini"
        )
    endif()
endfunction()
