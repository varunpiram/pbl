cmake_minimum_required(VERSION 3.20)
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
project(productivity_blocker LANGUAGES CXX OBJCXX)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_library(blocker src/resource.cpp src/schedule.cpp src/storage.cpp src/executor.cpp
    src/dns.cpp src/hosts.cpp src/process.cpp src/macos_app.cpp src/macos_website.cpp src/macos_environment.cpp)
target_include_directories(blocker PUBLIC include)
target_link_libraries(blocker PUBLIC resolv)
target_compile_options(blocker PRIVATE -Wall -Wextra -Wpedantic)
add_executable(blocker-worker src/worker.cpp src/network_watch.cpp)
target_compile_options(blocker-worker PRIVATE -fblocks)
target_link_libraries(blocker-worker PRIVATE blocker "-framework SystemConfiguration" "-framework CoreFoundation")
set(PBL_CLIENT_REQUIREMENT "never" CACHE STRING "Code signing requirement for authorized UI clients")
string(REPLACE "\\" "\\\\" PBL_ESCAPED_REQUIREMENT "${PBL_CLIENT_REQUIREMENT}")
string(REPLACE "\"" "\\\"" PBL_ESCAPED_REQUIREMENT "${PBL_ESCAPED_REQUIREMENT}")
add_executable(blocker-helper bridge/helper.mm)
target_compile_options(blocker-helper PRIVATE -fobjc-arc)
target_compile_definitions(blocker-helper PRIVATE PBL_CLIENT_REQUIREMENT="${PBL_ESCAPED_REQUIREMENT}")
target_link_libraries(blocker-helper PRIVATE blocker "-framework Foundation")
include(CTest)
if(BUILD_TESTING)
    add_executable(live-website-test tests/live_website.cpp)
    target_link_libraries(live-website-test PRIVATE blocker)
    add_executable(website-test tests/website.cpp)
    target_link_libraries(website-test PRIVATE blocker)
    add_test(NAME website COMMAND website-test)
    add_executable(recurrence-test tests/recurrence.cpp)
    target_link_libraries(recurrence-test PRIVATE blocker)
    add_test(NAME recurrence COMMAND recurrence-test)
    add_executable(xpc-auth-helper bridge/helper.mm)
    target_compile_options(xpc-auth-helper PRIVATE -fobjc-arc)
    target_compile_definitions(xpc-auth-helper PRIVATE PBL_AUTH_TEST=1)
    target_link_libraries(xpc-auth-helper PRIVATE blocker "-framework Foundation")
    add_executable(xpc-auth-client tests/xpc_client.mm)
    target_include_directories(xpc-auth-client PRIVATE bridge include)
    target_compile_options(xpc-auth-client PRIVATE -fobjc-arc)
    target_link_libraries(xpc-auth-client PRIVATE "-framework Foundation")
endif()

option(PBL_BUILD_APP "Build the native macOS app" ON)
if(PBL_BUILD_APP)
    find_program(SWIFTC_EXECUTABLE swiftc REQUIRED)
    set(APP_BUNDLE "${CMAKE_BINARY_DIR}/pbl.app")
    file(MAKE_DIRECTORY "${APP_BUNDLE}/Contents/MacOS")
    file(MAKE_DIRECTORY "${APP_BUNDLE}/Contents/Resources")
    configure_file(assets/AppIcon.icns "${APP_BUNDLE}/Contents/Resources/AppIcon.icns" COPYONLY)
    configure_file(ui/Info.plist "${APP_BUNDLE}/Contents/Info.plist" COPYONLY)
    file(GLOB UI_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/ui/*.swift")
    add_custom_command(
        OUTPUT "${APP_BUNDLE}/Contents/MacOS/pbl"
        COMMAND "${SWIFTC_EXECUTABLE}" -O -whole-module-optimization -parse-as-library
            -target "${CMAKE_SYSTEM_PROCESSOR}-apple-macosx14.0"
            ${UI_SOURCES}
            "${CMAKE_SOURCE_DIR}/bridge/BackendClient.swift"
            -I "${CMAKE_SOURCE_DIR}/include" -import-objc-header "${CMAKE_SOURCE_DIR}/bridge/BackendProtocol.h"
            -o "${APP_BUNDLE}/Contents/MacOS/pbl"
        DEPENDS ${UI_SOURCES} bridge/BackendClient.swift bridge/BackendProtocol.h include/config.h
        VERBATIM
    )
    add_custom_target(pbl-app DEPENDS "${APP_BUNDLE}/Contents/MacOS/pbl")
endif()
