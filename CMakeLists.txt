cmake_minimum_required(VERSION 3.5.0)
project(xonotic C CXX)

set(CMAKE_AUTOMOC ON)

execute_process(
    COMMAND dpkg-architecture -qDEB_HOST_MULTIARCH
    OUTPUT_VARIABLE ARCH_TRIPLET
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(QT_IMPORTS_DIR "lib/${ARCH_TRIPLET}")

set(PROJECT_NAME "xonotic")
set(FULL_PROJECT_NAME "xonotic.ratrad")
set(DATA_DIR "share/${PROJECT_NAME}")
set(DESKTOP_FILE_NAME ${PROJECT_NAME}.desktop)

find_package(Qt5 REQUIRED COMPONENTS Core Qml Quick Network)

configure_file(manifest.json.in ${CMAKE_CURRENT_BINARY_DIR}/manifest.json)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/manifest.json DESTINATION ${CMAKE_INSTALL_PREFIX})
install(FILES ${PROJECT_NAME}.apparmor DESTINATION ${DATA_DIR})

install(DIRECTORY assets DESTINATION ${DATA_DIR})
install(DIRECTORY qml    DESTINATION ${DATA_DIR}/xonotic)

add_library(${PROJECT_NAME}backend SHARED
    src/launcher.cpp
    src/backend.cpp
)
target_link_libraries(${PROJECT_NAME}backend Qt5::Core Qt5::Qml Qt5::Network)
set_target_properties(${PROJECT_NAME}backend PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib/${ARCH_TRIPLET}/xonotic"
)
install(TARGETS ${PROJECT_NAME}backend DESTINATION lib/${ARCH_TRIPLET}/xonotic)
install(FILES src/qmldir DESTINATION lib/${ARCH_TRIPLET}/xonotic)

configure_file(${DESKTOP_FILE_NAME}.in ${CMAKE_CURRENT_BINARY_DIR}/${DESKTOP_FILE_NAME} @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${DESKTOP_FILE_NAME} DESTINATION ${DATA_DIR})
