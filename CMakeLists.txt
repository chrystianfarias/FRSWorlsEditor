cmake_minimum_required(VERSION 3.21)
project(FRSWorldEditor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

add_executable(FRSWorldEditor WIN32
    src/App.cpp

    src/core/Log.cpp
    src/core/FileMap.cpp

    src/io/CollisionExport.cpp
    src/io/CollisionImport.cpp
    src/io/CollisionEdit.cpp
    src/io/CollisionGenerate.cpp
    src/io/MeshOverlay.cpp
    src/io/ModelCopy.cpp
    src/io/ObjMesh.cpp
    src/io/PlacementWrite.cpp
    src/io/SectionRebuild.cpp
    src/io/SectionEdits.cpp
    src/io/SectionSplice.cpp
    src/io/TextureImport.cpp
    src/io/TpkBuild.cpp
    src/io/WorldProfiles.cpp
    src/io/WorldFile.cpp
    src/io/SelectionExport.cpp
    src/io/SelectionImport.cpp
    src/nfs/ChunkReader.cpp
    src/nfs/Jdlz.cpp
    src/nfs/Collision.cpp
    src/nfs/Preculler.cpp
    src/nfs/Scenery.cpp
    src/nfs/CityZones.cpp
    src/nfs/FreeRoam.cpp
    src/nfs/ModelCategory.cpp
    src/nfs/SceneryEdit.cpp
    src/nfs/SectionTable.cpp
    src/nfs/SolidList.cpp
    src/nfs/TextureDecode.cpp
    src/nfs/TexturePack.cpp
    src/nfs/EaCompress.cpp
    src/nfs/WorldAnimation.cpp
    src/nfs/World.cpp

    # MINIMAP PREVIEW (temporary)
    src/minimap/MinimapBuildings.cpp
    src/minimap/MinimapPreview.cpp

    src/render/D3D9Context.cpp
    src/render/GpuResources.cpp
    src/render/Camera.cpp
    src/render/CollisionMesh.cpp
    src/render/WorldRenderer.cpp
    src/render/GameLook.cpp
    src/render/Gizmo.cpp
    src/render/ModelThumbnails.cpp

    src/ui/EditsPanel.cpp
    src/ui/EmbeddedGame.cpp
    src/ui/CrashReport.cpp
    src/ui/HiddenItems.cpp
    src/ui/ZoneTools.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindowShell.cpp
    src/ui/OnlineLibrary.cpp
    src/online/Sketchfab.cpp
    src/net/Http.cpp
    src/io/GlbToObj.cpp
    src/io/LightBake.cpp
    src/ui/OverlayBrowser.cpp
    src/ui/PlacementDialogs.cpp
    src/ui/PropertiesPanel.cpp
    src/ui/RoadsExtension.cpp
    src/roads/RoadNetwork.cpp
    src/roads/RoadBake.cpp
    src/roads/RoadRoutes.cpp
    src/nfs/Routes.cpp
    src/terrain/Terrain.cpp
    src/terrain/GroundDeform.cpp
    src/ui/BlenderLink.cpp
    src/ui/CollisionTools.cpp
    src/ui/MeshTools.cpp
    src/ui/ModelCatalog.cpp
    src/ui/SetupWizard.cpp
    src/ui/WorldDialogs.cpp

    third_party/propertygrid/propertyGrid.cpp
    src/ui/ImGuiLayer.cpp
    third_party/phosphor/PhosphorFont.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_impl_dx9.cpp
    third_party/imgui/imgui_impl_win32.cpp
    src/ui/TextureBrowser.cpp
    src/ui/Viewport.cpp
    src/ui/App.rc
)

target_include_directories(FRSWorldEditor PRIVATE src)

# Test drive (src/drive): NFSU2's vehicle physics, collision, camera and car
# models, ported from the bit-exact C# port (NFSU2Unity). Plain float
# arithmetic must stay unfused there: /fp:precise, never /fp:fast.
file(GLOB_RECURSE FRS_DRIVE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/drive/*.cpp")
target_sources(FRSWorldEditor PRIVATE ${FRS_DRIVE_SOURCES})
if(MSVC)
    set_source_files_properties(${FRS_DRIVE_SOURCES} PROPERTIES COMPILE_OPTIONS "/fp:precise")
endif()

# Extensions (src/ui/EditorExtension.h): a build with an extensions folder
# beside this file -- FRS_EXTENSIONS_DIR, or ./private by default -- compiles
# what its editor.cmake adds; this repository's own build has none.
set(FRS_EXTENSIONS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/private" CACHE PATH "Editor extensions, if any")
option(FRS_WITH_EXTENSIONS "Build the extensions in FRS_EXTENSIONS_DIR when it is there" ON)
if(FRS_WITH_EXTENSIONS AND EXISTS "${FRS_EXTENSIONS_DIR}/editor.cmake")
    include("${FRS_EXTENSIONS_DIR}/editor.cmake")
    message(STATUS "FRS World Editor: extensions from ${FRS_EXTENSIONS_DIR}")
else()
    target_sources(FRSWorldEditor PRIVATE src/ui/NoExtensions.cpp)
endif()

target_compile_definitions(FRSWorldEditor PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(FRSWorldEditor PRIVATE
    d3d9
    comctl32
    shlwapi
    dwmapi
    shell32
    ole32
    comdlg32
    windowscodecs
    bcrypt
    advapi32
    uxtheme
)

if(MSVC)
    target_compile_options(FRSWorldEditor PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8)
    target_link_options(FRSWorldEditor PRIVATE /SUBSYSTEM:WINDOWS)
    # Symbols beside the Release build too, so a crash dump says where it died.
    target_compile_options(FRSWorldEditor PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(FRSWorldEditor PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
    set_property(TARGET FRSWorldEditor PROPERTY MSVC_DEBUG_INFORMATION_FORMAT
                 "$<IF:$<CONFIG:Debug,RelWithDebInfo>,ProgramDatabase,>")
    # Enable the common-controls v6 manifest so the UI uses the modern theme.
    set_property(TARGET FRSWorldEditor PROPERTY VS_DPI_AWARE "PerMonitor")
    # The C runtime is linked in, so the editor runs on a machine without the
    # Visual C++ Redistributable.
    set_property(TARGET FRSWorldEditor PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Third-party code is built as its author wrote it: without the project's
# warning level and conformance mode.
if(MSVC)
    set_source_files_properties(third_party/propertygrid/propertyGrid.cpp PROPERTIES
        COMPILE_OPTIONS "/W0;/permissive;/Zc:strictStrings-")
endif()

# Dear ImGui (docking branch, MIT): the panels over the viewport.
target_include_directories(FRSWorldEditor PRIVATE third_party/imgui third_party/phosphor
    third_party/cgltf third_party/json)
# Sketchfab: HTTPS, the login's local callback, the token kept with DPAPI.
target_link_libraries(FRSWorldEditor PRIVATE winhttp ws2_32 crypt32)
target_link_libraries(FRSWorldEditor PRIVATE imm32 dwmapi)
set_source_files_properties(third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_impl_dx9.cpp third_party/imgui/imgui_impl_win32.cpp
    PROPERTIES COMPILE_OPTIONS "/W0")
