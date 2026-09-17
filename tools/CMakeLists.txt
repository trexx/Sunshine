cmake_minimum_required(VERSION 3.20)

project(sunshine_tools)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED CMAKE_CUDA_STANDARD)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

include_directories(
        "${CMAKE_SOURCE_DIR}"
        "${FFMPEG_INCLUDE_DIRS}"  # this is included only for logging
)

set(TOOL_SOURCES
        "${CMAKE_SOURCE_DIR}/src/logging.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.cpp"
)

add_executable(dxgi-info dxgi.cpp ${TOOL_SOURCES})
target_link_libraries(dxgi-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ${FFMPEG_LIBRARIES}  # this is included only for logging
        dxgi
        libdisplaydevice::display_device  # this is included only for logging
        ${PLATFORM_LIBRARIES}
)
target_compile_options(dxgi-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(audio-info audio.cpp ${TOOL_SOURCES})
target_link_libraries(audio-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ${FFMPEG_LIBRARIES}  # this is included only for logging
        libdisplaydevice::display_device  # this is included only for logging
        ksuser
        ${PLATFORM_LIBRARIES}
)
target_compile_options(audio-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(sunshinesvc sunshinesvc.cpp)
target_link_libraries(sunshinesvc
        ${CMAKE_THREAD_LIBS_INIT}
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshinesvc PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# HIDMaestro broker: a .NET helper that hosts the HIDMaestro SDK for controller lifecycle.
# Built with `dotnet publish` as a self-contained single-file executable; see
# tools/hidmaestro-broker/SunshineHidMaestroBroker.csproj and cmake/dependencies/hidmaestro.cmake.
if(SUNSHINE_ENABLE_HIDMAESTRO)
    set(HIDMAESTRO_BROKER_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/hidmaestro-broker/SunshineHidMaestroBroker.csproj")
    set(HIDMAESTRO_BROKER_PUBLISH_DIR "${CMAKE_BINARY_DIR}/hidmaestro/publish")
    set(HIDMAESTRO_BROKER_EXE "${HIDMAESTRO_BROKER_PUBLISH_DIR}/sunshine-hidmaestro-broker.exe")
    file(GLOB HIDMAESTRO_BROKER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/hidmaestro-broker/*.cs")

    add_custom_command(
            OUTPUT "${HIDMAESTRO_BROKER_EXE}"
            COMMAND "${DOTNET_EXECUTABLE}" publish "${HIDMAESTRO_BROKER_PROJECT}"
                    --configuration Release
                    --nologo
                    --output "${HIDMAESTRO_BROKER_PUBLISH_DIR}"
                    "-p:HidMaestroCoreDll=${HIDMAESTRO_CORE_DLL}"
                    "-p:Version=${CMAKE_PROJECT_VERSION}"
                    "-p:BaseIntermediateOutputPath=${CMAKE_BINARY_DIR}/hidmaestro/obj/"
                    "-p:BaseOutputPath=${CMAKE_BINARY_DIR}/hidmaestro/bin/"
            DEPENDS "${HIDMAESTRO_BROKER_PROJECT}" ${HIDMAESTRO_BROKER_SOURCES} "${HIDMAESTRO_CORE_DLL}"
            COMMENT "Publishing HIDMaestro broker"
            VERBATIM)
    add_custom_target(hidmaestro_broker ALL DEPENDS "${HIDMAESTRO_BROKER_EXE}")
    set(HIDMAESTRO_BROKER_EXE "${HIDMAESTRO_BROKER_EXE}" CACHE INTERNAL "Path to the published HIDMaestro broker executable")

    # Keep the build-tree layout identical to the install layout so Sunshine finds the broker next to itself.
    add_custom_command(TARGET hidmaestro_broker POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/tools"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${HIDMAESTRO_BROKER_EXE}" "${CMAKE_BINARY_DIR}/tools/sunshine-hidmaestro-broker.exe"
            VERBATIM)
endif()
