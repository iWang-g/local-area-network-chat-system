cmake_minimum_required(VERSION 3.16)

project(qt-client VERSION 0.1 LANGUAGES CXX)

# 本工程仅使用 Qt Widgets（.ui + C++），不使用 QML / Qt Quick。
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # 并行编译（Ninja）时多进程写同一 vc*.pdb，缺 /FS 会触发 C2471「无法更新程序数据库」。
    add_compile_options(/utf-8 /Zc:__cplusplus /permissive- /FS)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network Svg)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network Svg)

set(PROJECT_SOURCES
    src/app/main.cpp
    src/utils/local_profile.cpp
    src/utils/local_profile.h
    src/utils/local_stickers.cpp
    src/utils/local_stickers.h
    src/utils/logger.cpp
    src/utils/logger.h
    src/net/lan_tcp_client.cpp
    src/net/lan_tcp_client.h
    src/style/app_style.cpp
    src/style/app_style.h
    src/ui/chat_main_widget.cpp
    src/ui/chat_main_widget.h
    src/ui/sticker_picker_popup.cpp
    src/ui/sticker_picker_popup.h
    src/ui/image_preview_dialog.cpp
    src/ui/image_preview_dialog.h
    src/ui/add_friend_search_dialog.cpp
    src/ui/add_friend_search_dialog.h
    src/ui/chat_history_dialog.cpp
    src/ui/chat_history_dialog.h
    src/ui/contacts_widget.cpp
    src/ui/contacts_widget.h
    src/ui/login_widget.cpp
    src/ui/login_widget.h
    src/ui/register_widget.cpp
    src/ui/register_widget.h
    src/ui/mainwindow.cpp
    src/ui/mainwindow.h
    src/ui/mainwindow.ui
    resources.qrc
    src/ui/network_debug_dialog.cpp
    src/ui/network_debug_dialog.h
    src/ui/profile_dialog.cpp
    src/ui/profile_dialog.h
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(qt-client
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
else()
    if(ANDROID)
        add_library(qt-client SHARED
            ${PROJECT_SOURCES}
        )
    else()
        add_executable(qt-client
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_include_directories(qt-client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(qt-client PRIVATE
    "PROJECT_ROOT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
)

target_link_libraries(qt-client PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Svg
)

if(${QT_VERSION} VERSION_LESS 6.1.0)
    set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.qt-client)
endif()
set_target_properties(qt-client PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS qt-client
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(qt-client)
endif()
