cmake_minimum_required(VERSION 3.20)

project(FrostbiteUniversalSdkBridge LANGUAGES CXX)

option(AEGIS_TEMPLATE_ENABLE_SAMPLE_DATA "Show sample time, skybox, and fog rows before a real SDK is wired" ON)

add_library(FrostbiteUniversalSdkBridge SHARED
    src/FrostbiteUniversalSdkBridge.cpp
    src/SdkBindings.cpp
)

target_compile_features(FrostbiteUniversalSdkBridge PRIVATE cxx_std_17)

target_compile_definitions(FrostbiteUniversalSdkBridge PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if (AEGIS_TEMPLATE_ENABLE_SAMPLE_DATA)
    target_compile_definitions(FrostbiteUniversalSdkBridge PRIVATE AEGIS_TEMPLATE_ENABLE_SAMPLE_DATA=1)
else()
    target_compile_definitions(FrostbiteUniversalSdkBridge PRIVATE AEGIS_TEMPLATE_ENABLE_SAMPLE_DATA=0)
endif()

target_include_directories(FrostbiteUniversalSdkBridge PRIVATE
    include
    "${FROSTBITE_UNIVERSAL_INCLUDE_DIR}"
    "${GENERATED_SDK_DIR}"
)

if (MSVC)
    target_compile_options(FrostbiteUniversalSdkBridge PRIVATE /W4 /permissive-)
endif()
