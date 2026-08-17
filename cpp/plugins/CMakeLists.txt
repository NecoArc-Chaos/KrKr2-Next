cmake_minimum_required(VERSION 3.19)
project(krkr2plugin)

add_library(${PROJECT_NAME} STATIC)

target_sources(${PROJECT_NAME} PUBLIC
    scriptsEx.cpp
    win32dialog.cpp
    dirlist.cpp
    csvParser.cpp
    layerExMovie.cpp
    varfile.cpp
    saveStruct.cpp
    getabout.cpp
    addFont.cpp
    wutcwf.cpp
    windowEx.cpp
    getSample.cpp
    layerExPerspective.cpp
    fftgraph.cpp
    LayerExBase.cpp
    xp3filter.cpp
    extrans.cpp
    textrender.cpp
    alphamovie.cpp
    layerExBTOA.cpp
    layerExImage.cpp
    layerExRaster.cpp
    layerExAreaAverage.cpp
    layerExLongExposure.cpp
    drawDeviceD2DCompat.cpp
    wfBasicEffectCompat.cpp
    wfTypicalDSPCompat.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${KRKR2CORE_PATH}/
    ${KRKR2CORE_PATH}/base
    ${KRKR2CORE_PATH}/base/win32
    ${KRKR2CORE_PATH}/environ
    ${KRKR2CORE_PATH}/environ/win32
    ${KRKR2CORE_PATH}/environ/android
    ${KRKR2CORE_PATH}/environ/sdl
    ${KRKR2CORE_PATH}/msg
    ${KRKR2CORE_PATH}/msg/win32
    ${KRKR2CORE_PATH}/extension
    ${KRKR2CORE_PATH}/sound
    ${KRKR2CORE_PATH}/sound/win32
    ${KRKR2CORE_PATH}/tjs2
    ${KRKR2CORE_PATH}/utils
    ${KRKR2CORE_PATH}/utils/win32
    ${KRKR2CORE_PATH}/visual
    ${KRKR2CORE_PATH}/visual/ARM
    ${KRKR2CORE_PATH}/visual/win32
    ${KRKR2CORE_PATH}/visual/RenderScript/rs
)

add_definitions(-D__STDC_CONSTANT_MACROS -DTJS_TEXT_OUT_CRLF)

# ---------------------------------------------------------------------------
# Optional Live2D Cubism SDK
# ---------------------------------------------------------------------------
option(KRKR2_ENABLE_LIVE2D
    "Build the optional Live2D Cubism plugin when its SDK is complete"
    ON)

if(KRKR2_ENABLE_LIVE2D)
    set(CUBISM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cubism")
    set(CUBISM_CORE_DIR "${CUBISM_DIR}/Core")
    set(CUBISM_FW_DIR "${CUBISM_DIR}/Framework")
    set(CUBISM_MISSING)

    foreach(_cubism_required_file IN ITEMS
            "${CUBISM_DIR}/Core/include/Live2DCubismCore.h"
            "${CUBISM_FW_DIR}/CubismFramework.hpp"
            "${CUBISM_FW_DIR}/CubismModelSettingJson.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/krkrlive2d.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/krkrgles.cpp")
        if(NOT EXISTS "${_cubism_required_file}")
            list(APPEND CUBISM_MISSING "${_cubism_required_file}")
        endif()
    endforeach()

    file(GLOB_RECURSE CUBISM_FW_SOURCES CONFIGURE_DEPENDS
        "${CUBISM_FW_DIR}/*.cpp")
    list(FILTER CUBISM_FW_SOURCES EXCLUDE REGEX
        ".*/D3D9/.*|.*/D3D11/.*|.*/Vulkan/.*|.*/Metal/.*|.*/Cocos2d/.*")
    if(NOT CUBISM_FW_SOURCES)
        list(APPEND CUBISM_MISSING "${CUBISM_FW_DIR}/*.cpp")
    endif()

    if(ANDROID)
        set(CUBISM_CORE_ARCH "${CMAKE_ANDROID_ARCH_ABI}")
        if(NOT CUBISM_CORE_ARCH AND DEFINED ANDROID_ABI)
            set(CUBISM_CORE_ARCH "${ANDROID_ABI}")
        endif()
        set(CUBISM_CORE_LIB
            "${CUBISM_CORE_DIR}/lib/android/${CUBISM_CORE_ARCH}/libLive2DCubismCore.a")
        if(NOT EXISTS "${CUBISM_CORE_LIB}")
            list(APPEND CUBISM_MISSING "${CUBISM_CORE_LIB}")
        endif()
    else()
        list(APPEND CUBISM_MISSING "Android target")
    endif()

    if(CUBISM_MISSING)
        list(JOIN CUBISM_MISSING ", " CUBISM_MISSING_TEXT)
        message(STATUS "Live2D Cubism plugin disabled; missing: ${CUBISM_MISSING_TEXT}")
        # Provide anchor stubs so the linker can resolve symbols declared in
        # engine_api.cpp even when krkrgles.cpp / krkrlive2d.cpp are not built.
        target_sources(${PROJECT_NAME} PRIVATE
            krkrlive2d_stub.cpp
            krkrgles_stub.cpp)
    else()
        add_library(CubismFramework STATIC ${CUBISM_FW_SOURCES})
        target_include_directories(CubismFramework PUBLIC
            "${CUBISM_FW_DIR}"
            "${CUBISM_CORE_DIR}/include")
        target_compile_definitions(CubismFramework PUBLIC CSM_TARGET_ANDROID_ES2)

        find_package(unofficial-angle CONFIG QUIET)
        if(unofficial-angle_FOUND)
            target_link_libraries(CubismFramework PRIVATE
                unofficial::angle::libEGL
                unofficial::angle::libGLESv2)
            get_target_property(_EGL_INCS
                unofficial::angle::libEGL INTERFACE_INCLUDE_DIRECTORIES)
            if(_EGL_INCS)
                target_include_directories(CubismFramework PRIVATE ${_EGL_INCS})
            endif()
        else()
            target_link_libraries(CubismFramework PRIVATE EGL GLESv2)
        endif()

        find_package(PNG REQUIRED)
        find_package(unofficial-minizip CONFIG REQUIRED)
        target_link_libraries(CubismFramework PUBLIC "${CUBISM_CORE_LIB}")

        target_sources(${PROJECT_NAME} PRIVATE
            krkrlive2d.cpp
            krkrgles.cpp)
        target_include_directories(${PROJECT_NAME} PUBLIC
            "${CUBISM_FW_DIR}"
            "${CUBISM_CORE_DIR}/include")
        target_compile_definitions(${PROJECT_NAME} PUBLIC CSM_TARGET_ANDROID_ES2)
        target_link_libraries(${PROJECT_NAME} PUBLIC
            CubismFramework
            PNG::PNG
            unofficial::minizip::minizip)
    endif()
else()
    target_sources(${PROJECT_NAME} PRIVATE
        krkrlive2d_stub.cpp
        krkrgles_stub.cpp)
endif()

target_include_directories(${PROJECT_NAME} PUBLIC ${KRKR2PLUGIN_PATH} ${KRKR2PLUGIN_PATH}/simplebinder)
target_link_libraries(${PROJECT_NAME} PUBLIC krkr2core)

add_subdirectory(psdfile)
add_subdirectory(psbfile)
add_subdirectory(layerex_draw)
add_subdirectory(motionplayer)
add_subdirectory(kagparserex)

#add_subdirectory(json)
add_subdirectory(fstat)
#add_subdirectory(steam)
#add_subdirectory(DrawDeviceForSteam)
target_link_libraries(${PROJECT_NAME} PUBLIC
        psdfile
        psbfile
        layerExDraw
        motionplayer
        kagparserex
        fstat
#        json steam DrawDeviceForSteam
)

find_package(fmt CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE fmt::fmt)

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)
