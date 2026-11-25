cmake_minimum_required(VERSION 3.16)
project(TBS5580_DVB_Viewer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find FFmpeg and GLFW via PkgConfig
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libswscale
    libswresample
    libavutil
)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)
find_package(OpenGL REQUIRED)

include_directories(
    src
    vendor/imgui
    vendor/imgui/backends
    vendor/implot
    ${FFMPEG_INCLUDE_DIRS}
    ${GLFW_INCLUDE_DIRS}
)

set(IMGUI_SRCS
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_glfw.cpp
    vendor/imgui/backends/imgui_impl_opengl3.cpp
    vendor/implot/implot.cpp
    vendor/implot/implot_items.cpp
)

set(CLIENT_SRCS
    src/main.cpp
    src/net/TbsClient.cpp
    src/ts/TsParser.cpp
    src/video/VideoDecoder.cpp
    src/audio/AudioPlayer.cpp
    src/ui/MainWindow.cpp
    ${IMGUI_SRCS}
)

add_executable(TBS5580_DVB_Viewer ${CLIENT_SRCS})

if(APPLE)
    target_link_libraries(TBS5580_DVB_Viewer PRIVATE
        PkgConfig::FFMPEG
        PkgConfig::GLFW
        "-framework OpenGL"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreVideo"
    )
elseif(UNIX)
    find_package(ALSA QUIET)
    if(ALSA_FOUND)
        target_compile_definitions(TBS5580_DVB_Viewer PRIVATE HAVE_ALSA=1)
        target_link_libraries(TBS5580_DVB_Viewer PRIVATE
            PkgConfig::FFMPEG
            PkgConfig::GLFW
            OpenGL::GL
            ALSA::ALSA
            pthread
            dl
            m
        )
    else()
        target_link_libraries(TBS5580_DVB_Viewer PRIVATE
            PkgConfig::FFMPEG
            PkgConfig::GLFW
            OpenGL::GL
            pthread
            dl
            m
        )
    endif()
endif()

# Raspberry Pi / Linux streaming bridge daemon
set(SERVER_SRCS
    server_pi/main.cpp
    server_pi/DvbFrontend.cpp
)

add_executable(tbs_bridge ${SERVER_SRCS})
target_include_directories(tbs_bridge PRIVATE server_pi)
target_link_libraries(tbs_bridge PRIVATE pthread)
