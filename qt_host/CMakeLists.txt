cmake_minimum_required(VERSION 3.21)
project(qt_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 默认带控制台（bring-up 时能看 qDebug）；-DQT_HOST_CONSOLE=OFF 可切成纯 GUI
option(QT_HOST_CONSOLE "Attach a console window (useful for debugging)" ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

set(SOURCES
    src/main.cpp
    src/jclink.cpp
    src/tcp_transport.cpp
    src/serial_transport.cpp
    src/link_session.cpp
    src/image_frame.cpp
    src/h264_player.cpp
    src/video_view.cpp
    src/main_window.cpp
    src/selftest.cpp
)

# 头文件也要进 target：带 Q_OBJECT 的头（transport.h 等）需经 AUTOMOC 生成 moc，
# 否则链接期报 "undefined reference to ITransport::staticMetaObject / vtable"
set(HEADERS
    src/jclink.h
    src/transport.h
    src/tcp_transport.h
    src/serial_transport.h
    src/link_session.h
    src/image_frame.h
    src/h264_player.h
    src/video_view.h
    src/main_window.h
    src/selftest.h
)

if(QT_HOST_CONSOLE)
    add_executable(qt_host ${SOURCES} ${HEADERS})            # 带控制台
else()
    add_executable(qt_host WIN32 ${SOURCES} ${HEADERS})      # 纯 GUI
endif()

target_include_directories(qt_host PRIVATE src)
target_link_libraries(qt_host PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

# Windows 串口后端要 SetupAPI
if(WIN32)
    target_link_libraries(qt_host PRIVATE setupapi advapi32)
endif()
