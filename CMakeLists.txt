cmake_minimum_required(VERSION 3.24)
project(OuterSSHClientWindows LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This starter uses Win32 and WebView2")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(OUTERSSH_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(OUTERSSH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(OuterSSH/OuterSSH EXCLUDE_FROM_ALL)

include(FetchContent)
FetchContent_Declare(webview2
    URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.3537.50/microsoft.web.webview2.1.0.3537.50.nupkg
    URL_HASH SHA256=5ea526bbd728adda0da4d31219267e96460494a427e4894c4e09d9f320f4b9aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(webview2)
if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "ARM64" OR CMAKE_CXX_COMPILER_TARGET MATCHES "aarch64|arm64"
   OR (NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID AND CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64"))
    set(WEBVIEW2_ARCH arm64)
else()
    set(WEBVIEW2_ARCH x64)
endif()

# Embeds OuterSSH's socket host display script in the executable.
set(SOCKET_HOST_DISPLAY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/OuterSSH/OuterSSH/web/SocketHostDisplay.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SOCKET_HOST_DISPLAY_SOURCE}")
file(READ "${SOCKET_HOST_DISPLAY_SOURCE}" SOCKET_HOST_DISPLAY_SCRIPT)
configure_file(src/SocketHostDisplayScript.h.in SocketHostDisplayScript.h @ONLY)

add_executable(outerssh-client WIN32 src/Main.cpp)
target_compile_definitions(outerssh-client PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_include_directories(outerssh-client PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${webview2_SOURCE_DIR}/build/native/include")
target_link_libraries(outerssh-client PRIVATE OuterSSH::OuterSSH
    "${webview2_SOURCE_DIR}/build/native/${WEBVIEW2_ARCH}/WebView2LoaderStatic.lib"
    comctl32 ole32 user32 version)

# Bundles helpers/outerssh-helper-<platform>.gz next to the executable.
add_custom_target(outerssh-client-helpers ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/helpers"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/helpers" "$<TARGET_FILE_DIR:outerssh-client>/helpers")
add_dependencies(outerssh-client-helpers outerssh-client)
