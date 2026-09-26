# crosire/d3d8to9, vendored unmodified (see README.md / LICENSE.md).

# Third-party code: build it with upstream's own settings rather than the
# project-wide /W4 /permissive- and WIN32_LEAN_AND_MEAN (it uses ShellExecute).
set_directory_properties(PROPERTIES
    COMPILE_OPTIONS "/W3;/utf-8;/MP"
    COMPILE_DEFINITIONS "UNICODE;_UNICODE;NOMINMAX")

add_library(d3d8to9 STATIC
    source/d3d8.hpp
    source/d3d8to9.cpp
    source/d3d8to9.hpp
    source/d3d8to9_base.cpp
    source/d3d8to9_device.cpp
    source/d3d8to9_index_buffer.cpp
    source/d3d8to9_surface.cpp
    source/d3d8to9_swap_chain.cpp
    source/d3d8to9_texture.cpp
    source/d3d8to9_vertex_buffer.cpp
    source/d3d8to9_volume.cpp
    source/d3d8types.cpp
    source/d3d8types.hpp
    source/d3dx9.hpp
    source/interface_query.cpp
    source/interface_query.hpp)

target_compile_definitions(d3d8to9 PRIVATE D3D8TO9NOLOG)
target_include_directories(d3d8to9 PUBLIC source)
