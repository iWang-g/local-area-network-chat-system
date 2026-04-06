cmake_minimum_required(VERSION 3.16)

project(qt-client VERSION 0.1 LANGUAGES CXX)

# 本工程仅使用 Qt Widgets（.ui + C++），不使用 QML / Qt Quick。
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /Zc:__cplusplus /permissive-)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

set(PROJECT_SOURCES
    src/app/main.cpp
    src/utils/logger.cpp
    src/utils/logger.h
    src/net/lan_tcp_client.cpp
    src/net/lan_tcp_client.h
    src/style/app_style.cpp
    src/style/app_style.h
    src/ui/chat_main_widget.cpp
    src/ui/chat_main_widget.h
    src/ui/login_widget.cpp
    src/ui/login_widget.h
    src/ui/register_widget.cpp
    src/ui/register_widget.h
    src/ui/mainwindow.cpp
    src/ui/mainwindow.h
    src/ui/mainwindow.ui
    src/ui/network_debug_dialog.cpp
    src/ui/network_debug_dialog.h
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
