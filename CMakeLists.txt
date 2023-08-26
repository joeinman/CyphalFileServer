cmake_minimum_required(VERSION 3.27.0)

# Create Project
project(CyphalFileServer VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")

# Transpile DSDL into C using Nunavut. Install Nunavut as follows: pip install nunavut.
# Alternatively, you can invoke the transpiler manually or use https://nunaweb.opencyphal.org.
find_package(nnvg REQUIRED)
create_dsdl_target(             # Generate the support library for generated C headers, which is "nunavut.h".
        "nunavut_support"
        c
        ${CMAKE_BINARY_DIR}/transpiled
        ""
        OFF
        little
        "only"
)
set(dsdl_root_namespace_dirs                # List all DSDL root namespaces to transpile here.
    ${CMAKE_SOURCE_DIR}/external/public_regulated_data_types/uavcan
    ${CMAKE_SOURCE_DIR}/external/public_regulated_data_types/reg
)
foreach (ns_dir ${dsdl_root_namespace_dirs})
    get_filename_component(ns ${ns_dir} NAME)
    message(STATUS "DSDL namespace ${ns} at ${ns_dir}")
    create_dsdl_target(
            "dsdl_${ns}"                    # CMake target name
            c                               # Target language to transpile into
            ${CMAKE_BINARY_DIR}/transpiled  # Destination directory (add it to the includes)
            ${ns_dir}                       # Source directory
            OFF                             # Disable variable array capacity override
            little                          # Endianness of the target platform (alternatives: "big", "any")
            "never"                         # Support files are generated once in the nunavut_support target (above)
            ${dsdl_root_namespace_dirs}     # Look-up DSDL namespaces
    )
    add_dependencies("dsdl_${ns}" nunavut_support)
endforeach ()
include_directories(SYSTEM ${CMAKE_BINARY_DIR}/transpiled)  # Make the transpiled headers available for inclusion.
add_definitions(-DNUNAVUT_ASSERT=assert)

add_library(libcanard
    external/libcanard/libcanard/canard.h
    external/libcanard/libcanard/canard.c
    external/libcanard/libcanard/_canard_cavl.h
)

target_include_directories(libcanard PUBLIC
    external/libcanard/libcanard
)

add_library(platform_specific_components
    external/platform_specific_components/socketcan/libcanard/src/socketcan.h
    external/platform_specific_components/socketcan/libcanard/src/socketcan.c
)

target_link_libraries(platform_specific_components
    libcanard
)

target_include_directories(platform_specific_components PUBLIC
    external/platform_specific_components/socketcan/libcanard/src
)

add_library(o1heap
    external/o1heap/o1heap/o1heap.h
    external/o1heap/o1heap/o1heap.c
)

target_include_directories(o1heap PUBLIC
    external/o1heap/o1heap
)

add_executable(CyphalFileServer
    "src/CyphalFileServer.cpp"
)

target_link_libraries(CyphalFileServer
    platform_specific_components
    libcanard
    o1heap
)

add_dependencies(CyphalFileServer
    dsdl_uavcan
    dsdl_reg
)