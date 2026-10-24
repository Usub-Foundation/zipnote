cmake_minimum_required(VERSION 3.21)
project(ZipNotes VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(ZN_WITH_WEBENGINE "Render Mermaid and edit Excalidraw through Qt WebEngine" ON)
option(ZN_BUILD_TESTS "Build unit tests" ON)
option(ZN_BUILD_APP "Build the desktop application" ON)

# 6.8: QTextImageFormat::setMaximumWidth and image alt text, used for diagrams in notes
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Svg Concurrent)

if(ZN_WITH_WEBENGINE)
    find_package(Qt6 6.8 QUIET COMPONENTS WebEngineWidgets WebChannel)
    if(NOT Qt6WebEngineWidgets_FOUND OR NOT Qt6WebChannel_FOUND)
        message(WARNING "Qt WebEngine not found: Mermaid preview and Excalidraw editor are disabled")
        set(ZN_WITH_WEBENGINE OFF)
    endif()
endif()

# libsodium: vcpkg ships a CMake config, Linux/macOS distributions ship pkg-config.
find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
    set(ZN_SODIUM unofficial-sodium::sodium)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.18)
    set(ZN_SODIUM PkgConfig::SODIUM)
endif()

find_package(libzip CONFIG REQUIRED)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------- core
# Storage, crypto and search. Depends on QtCore only, so it can be reused by a CLI or a mobile shell.
add_library(zn_core STATIC
    src/core/Crypto.cpp
    src/core/Vault.cpp
    src/core/Manifest.cpp
    src/core/NoteText.cpp
    src/core/TextNormalizer.cpp
    src/core/SearchQuery.cpp
    src/core/SearchIndex.cpp
)
target_include_directories(zn_core PUBLIC src/core)
target_link_libraries(zn_core PUBLIC Qt6::Core PRIVATE ${ZN_SODIUM} libzip::zip)

# ---------------------------------------------------------------- document model (QtGui, no widgets)
add_library(zn_doc STATIC
    src/app/MarkdownIO.cpp
    src/app/NoteDocument.cpp
)
target_include_directories(zn_doc PUBLIC src/app)
target_link_libraries(zn_doc PUBLIC zn_core Qt6::Gui)

# ---------------------------------------------------------------- UI (everything but main.cpp, so tests can drive it)
add_library(zn_ui STATIC
    src/app/Theme.cpp
    src/app/Icons.cpp
    src/app/VaultController.cpp
    src/app/DemoContent.cpp
    src/app/WelcomeWidget.cpp
    src/app/MainWindow.cpp
    src/app/SidebarView.cpp
    src/app/NoteListModel.cpp
    src/app/NoteListView.cpp
    src/app/SearchField.cpp
    src/app/NoteEditor.cpp
    src/app/FindBar.cpp
    src/app/DiagramService.cpp
    src/app/DiagramEditorDialog.cpp
)
target_link_libraries(zn_ui PUBLIC zn_doc Qt6::Widgets Qt6::Svg Qt6::Concurrent)
if(ZN_WITH_WEBENGINE)
    target_sources(zn_ui PRIVATE src/app/WebDiagramRenderer.cpp src/app/ExcalidrawDialog.cpp)
    target_compile_definitions(zn_ui PUBLIC ZN_WITH_WEBENGINE=1)
    target_link_libraries(zn_ui PUBLIC Qt6::WebEngineWidgets Qt6::WebChannel)
endif()

# ---------------------------------------------------------------- application
if(ZN_BUILD_APP)
    set(ZN_APP_SOURCES src/app/main.cpp resources/resources.qrc)
    if(APPLE)
        set_source_files_properties(resources/icons/zipnotes.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        list(APPEND ZN_APP_SOURCES resources/icons/zipnotes.icns)
    endif()
    if(ZN_WITH_WEBENGINE)
        # ~11 MB of JavaScript and fonts: compiled straight into an object file instead of a C++ array
        qt_add_big_resources(ZN_WEB_RESOURCES resources/web.qrc)
        list(APPEND ZN_APP_SOURCES ${ZN_WEB_RESOURCES})
    endif()
    qt_add_executable(zipnotes WIN32 MACOSX_BUNDLE ${ZN_APP_SOURCES})
    target_link_libraries(zipnotes PRIVATE zn_ui)
    target_compile_definitions(zipnotes PRIVATE PROJECT_VERSION_STRING="${PROJECT_VERSION}")
    set_target_properties(zipnotes PROPERTIES
        OUTPUT_NAME ZipNotes
        MACOSX_BUNDLE_GUI_IDENTIFIER org.zipnotes.ZipNotes
        MACOSX_BUNDLE_BUNDLE_NAME ZipNotes
        MACOSX_BUNDLE_ICON_FILE zipnotes.icns
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    )
    install(TARGETS zipnotes BUNDLE DESTINATION . RUNTIME DESTINATION bin)
endif()

# ---------------------------------------------------------------- tests
if(ZN_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    set(ZN_TESTS crypto vault search notetext markdown controller editor mainwindow)
    foreach(t ${ZN_TESTS})
        add_executable(test_${t} tests/test_${t}.cpp)
        if(t STREQUAL "markdown")
            target_link_libraries(test_${t} PRIVATE zn_doc Qt6::Test)
        elseif(t STREQUAL "controller" OR t STREQUAL "editor" OR t STREQUAL "mainwindow")
            target_link_libraries(test_${t} PRIVATE zn_ui Qt6::Test)
        else()
            target_link_libraries(test_${t} PRIVATE zn_core Qt6::Test)
        endif()
        if(t STREQUAL "vault")
            target_link_libraries(test_${t} PRIVATE libzip::zip)
        endif()
        add_test(NAME ${t} COMMAND test_${t})
        # freed memory is scribbled over (glibc), so a use-after-free crashes here as it does on macOS
        set_tests_properties(${t} PROPERTIES ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;MALLOC_PERTURB_=165;GLIBC_TUNABLES=glibc.malloc.tcache_count=0")
    endforeach()
endif()
