# Build steps every extension product shares. A product's CMakeLists.txt
# includes this file and calls the functions with its own names; the templates
# live in shared/templates and the UE4SS import list in shared/ue4ss-abi.
#
#   lex_embed_lua(<out-var> SOURCE <lua> NAMESPACE <ns> DELIMITER <tag>)
#       Generates generated/EmbeddedLuaAPI.hpp in the current binary folder:
#       the Lua file as raw-string chunks of LEX_LUA_CHUNK_SIZE bytes, because
#       MSVC limits one string literal to about 16 KB. <tag> is the raw-string
#       delimiter and must not occur in the file as ")<tag>".
#
#   lex_implementation_resource(<out-var> PRODUCT <name> DISPLAY_NAME <text> VERSION <x.y.z>)
#       Configures the implementation DLL's version resource.
#
#   lex_selector_file(<out-var> VERSION <x.y.z>)
#       Configures main.json, which tells the bootstrap which DLL to start.
#
#   lex_ue4ss_import_library(<out-var> <target-var>)   Windows only
#       The UE4SS import library from UE4SS-97b7e501.def, built once per build
#       tree. Sets <out-var> to the .lib and <target-var> to the target that
#       builds it.

get_filename_component(LEX_SHARED_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(LEX_REPO_ROOT "${LEX_SHARED_DIR}/.." ABSOLUTE)
set(LEX_LUA_CHUNK_SIZE 7000)

function(_lex_require prefix)
    foreach(name IN LISTS ARGN)
        if(NOT ${prefix}_${name})
            message(FATAL_ERROR "${CMAKE_CURRENT_FUNCTION}: ${name} is required")
        endif()
    endforeach()
endfunction()

function(lex_embed_lua out_var)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "SOURCE;NAMESPACE;DELIMITER" "")
    _lex_require(ARG SOURCE NAMESPACE DELIMITER)
    file(READ "${ARG_SOURCE}" lua)
    string(FIND "${lua}" ")${ARG_DELIMITER}" delimiter)
    if(NOT delimiter EQUAL -1)
        message(FATAL_ERROR "${ARG_SOURCE} contains the raw-string delimiter )${ARG_DELIMITER}")
    endif()
    string(LENGTH "${lua}" length)
    set(LEX_LUA_CHUNKS "")
    set(LEX_LUA_CHUNK_COUNT 0)
    set(offset 0)
    while(offset LESS length)
        string(SUBSTRING "${lua}" ${offset} ${LEX_LUA_CHUNK_SIZE} chunk)
        string(APPEND LEX_LUA_CHUNKS "    R\"${ARG_DELIMITER}(${chunk})${ARG_DELIMITER}\",\n")
        math(EXPR offset "${offset} + ${LEX_LUA_CHUNK_SIZE}")
        math(EXPR LEX_LUA_CHUNK_COUNT "${LEX_LUA_CHUNK_COUNT} + 1")
    endwhile()
    file(RELATIVE_PATH LEX_LUA_SOURCE "${LEX_REPO_ROOT}" "${ARG_SOURCE}")
    set(LEX_LUA_NAMESPACE "${ARG_NAMESPACE}")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedLuaAPI.hpp")
    configure_file("${LEX_SHARED_DIR}/templates/EmbeddedLuaAPI.hpp.in" "${header}" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ARG_SOURCE}")
    set(${out_var} "${header}" PARENT_SCOPE)
endfunction()

function(_lex_version_parts version)
    if(NOT version MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        message(FATAL_ERROR "Invalid product version ${version}")
    endif()
    set(LEX_VERSION "${version}" PARENT_SCOPE)
    set(LEX_VERSION_MAJOR "${CMAKE_MATCH_1}" PARENT_SCOPE)
    set(LEX_VERSION_MINOR "${CMAKE_MATCH_2}" PARENT_SCOPE)
    set(LEX_VERSION_PATCH "${CMAKE_MATCH_3}" PARENT_SCOPE)
endfunction()

function(lex_implementation_resource out_var)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "PRODUCT;DISPLAY_NAME;VERSION" "")
    _lex_require(ARG PRODUCT DISPLAY_NAME VERSION)
    _lex_version_parts("${ARG_VERSION}")
    set(LEX_PRODUCT "${ARG_PRODUCT}")
    set(LEX_DISPLAY_NAME "${ARG_DISPLAY_NAME}")
    set(resource "${CMAKE_CURRENT_BINARY_DIR}/Version.rc")
    configure_file("${LEX_SHARED_DIR}/templates/Implementation.rc.in" "${resource}" @ONLY)
    set(${out_var} "${resource}" PARENT_SCOPE)
endfunction()

function(lex_selector_file out_var)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "VERSION" "")
    _lex_require(ARG VERSION)
    _lex_version_parts("${ARG_VERSION}")
    set(selector "${CMAKE_CURRENT_BINARY_DIR}/main.json")
    configure_file("${LEX_SHARED_DIR}/templates/main.json.in" "${selector}" @ONLY)
    set(${out_var} "${selector}" PARENT_SCOPE)
endfunction()

function(lex_ue4ss_import_library out_var target_var)
    set(definition "${LEX_SHARED_DIR}/ue4ss-abi/UE4SS-97b7e501.def")
    set(library "${CMAKE_BINARY_DIR}/UE4SS-97b7e501.lib")
    if(NOT TARGET LexUE4SSImportLibrary)
        find_program(MSVC_LIB_EXE lib.exe REQUIRED)
        add_custom_command(OUTPUT "${library}"
            COMMAND "${MSVC_LIB_EXE}" /nologo /machine:x64 "/def:${definition}" "/out:${library}"
            DEPENDS "${definition}" VERBATIM)
        add_custom_target(LexUE4SSImportLibrary DEPENDS "${library}")
    endif()
    set(${out_var} "${library}" PARENT_SCOPE)
    set(${target_var} LexUE4SSImportLibrary PARENT_SCOPE)
endfunction()
