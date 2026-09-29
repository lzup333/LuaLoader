# Dear ImGui (vendored) — 核心 + OpenGL3 后端
# 说明: imgui_impl_opengl3 自带 GL 函数加载器, 无需链接 libGL。
add_library(imgui STATIC
        imgui.cpp
        imgui_draw.cpp
        imgui_tables.cpp
        imgui_widgets.cpp
        imgui_demo.cpp
        backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/backends
)

# Android/WebGL 走 OpenGL ES 3.0，后端需在编译期选择 ES 路径
if (ANDROID)
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3=1)
endif()

set_target_properties(imgui PROPERTIES
        CXX_STANDARD 11
        POSITION_INDEPENDENT_CODE ON
)
