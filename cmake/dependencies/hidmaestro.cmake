# HIDMaestro dependency for the Windows gamepad broker.
#
# HIDMaestro (https://github.com/hifihedgehog/HIDMaestro, MIT) is a user-mode virtual controller
# driver with a .NET SDK. Sunshine hosts that SDK in a small helper process
# (tools/hidmaestro-broker) and talks to the driver's shared memory directly. The SDK ships only
# as a prebuilt assembly inside the GitHub release archive, so it is downloaded here, verified
# against a pinned SHA256 and handed to the broker build.

if(NOT SUNSHINE_ENABLE_HIDMAESTRO)
    return()
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    message(STATUS "HIDMaestro broker disabled: HIDMaestro only ships win-x64 binaries (target is ${CMAKE_SYSTEM_PROCESSOR})")
    set(SUNSHINE_ENABLE_HIDMAESTRO OFF CACHE BOOL "" FORCE)
    return()
endif()

# The MSYS2 shell used on Windows does not inherit the Windows PATH, so look in the usual
# install location and wherever actions/setup-dotnet points DOTNET_ROOT.
find_program(DOTNET_EXECUTABLE dotnet HINTS "$ENV{DOTNET_ROOT}" "C:/Program Files/dotnet" "$ENV{ProgramFiles}/dotnet")
if(NOT DOTNET_EXECUTABLE)
    message(WARNING "HIDMaestro broker disabled: the .NET SDK (dotnet) was not found. Install the .NET 10 SDK or configure with -DSUNSHINE_ENABLE_HIDMAESTRO=OFF.")
    set(SUNSHINE_ENABLE_HIDMAESTRO OFF CACHE BOOL "" FORCE)
    return()
endif()

set(HIDMAESTRO_VERSION "1.8.0" CACHE STRING "HIDMaestro release to bundle.")
set(HIDMAESTRO_ARCHIVE_URL
        "https://github.com/hifihedgehog/HIDMaestro/releases/download/v${HIDMAESTRO_VERSION}/HIDMaestro-v${HIDMAESTRO_VERSION}.zip"
        CACHE STRING "URL of the HIDMaestro release archive.")
set(HIDMAESTRO_ARCHIVE_SHA256
        "1e5f5019c20e4be8f922c7aa5a86ee87eb01f7aa851fe38daea14d0ce4fd8240"
        CACHE STRING "SHA256 of the HIDMaestro release archive.")
set(HIDMAESTRO_CORE_SHA256
        "4c4ad3c2a70837566794f4c2f2b671fb5a0a2b4523fe180a0f7e4ec6d482bd18"
        CACHE STRING "SHA256 of HIDMaestro.Core.dll inside the release archive.")

set(HIDMAESTRO_DIR "${CMAKE_BINARY_DIR}/hidmaestro")
set(HIDMAESTRO_ARCHIVE "${HIDMAESTRO_DIR}/HIDMaestro-v${HIDMAESTRO_VERSION}.zip")
set(HIDMAESTRO_CORE_DLL "${HIDMAESTRO_DIR}/HIDMaestro.Core.dll" CACHE FILEPATH "Path to the prebuilt HIDMaestro.Core.dll.")
file(MAKE_DIRECTORY "${HIDMAESTRO_DIR}")

set(_hidmaestro_core_ok FALSE)
if(EXISTS "${HIDMAESTRO_CORE_DLL}")
    file(SHA256 "${HIDMAESTRO_CORE_DLL}" _hidmaestro_core_actual)
    if(_hidmaestro_core_actual STREQUAL HIDMAESTRO_CORE_SHA256)
        set(_hidmaestro_core_ok TRUE)
    else()
        message(STATUS "HIDMaestro.Core.dll hash mismatch; re-downloading")
        file(REMOVE "${HIDMAESTRO_CORE_DLL}")
    endif()
endif()

if(NOT _hidmaestro_core_ok)
    if(NOT EXISTS "${HIDMAESTRO_ARCHIVE}")
        message(STATUS "Downloading HIDMaestro v${HIDMAESTRO_VERSION} from ${HIDMAESTRO_ARCHIVE_URL}")
        file(DOWNLOAD "${HIDMAESTRO_ARCHIVE_URL}" "${HIDMAESTRO_ARCHIVE}"
                EXPECTED_HASH SHA256=${HIDMAESTRO_ARCHIVE_SHA256}
                SHOW_PROGRESS
                STATUS _hidmaestro_download_status)
        list(GET _hidmaestro_download_status 0 _hidmaestro_download_code)
        if(NOT _hidmaestro_download_code EQUAL 0)
            list(GET _hidmaestro_download_status 1 _hidmaestro_download_message)
            file(REMOVE "${HIDMAESTRO_ARCHIVE}")
            message(FATAL_ERROR "Failed to download HIDMaestro: ${_hidmaestro_download_message}. "
                    "Configure with -DSUNSHINE_ENABLE_HIDMAESTRO=OFF to build without HIDMaestro support.")
        endif()
    endif()

    # Only the SDK assembly at the archive root is needed; the archive also carries two sample apps.
    file(ARCHIVE_EXTRACT INPUT "${HIDMAESTRO_ARCHIVE}" DESTINATION "${HIDMAESTRO_DIR}" PATTERNS "HIDMaestro.Core.dll")
    if(NOT EXISTS "${HIDMAESTRO_CORE_DLL}")
        message(FATAL_ERROR "HIDMaestro.Core.dll was not found in ${HIDMAESTRO_ARCHIVE}")
    endif()
    file(SHA256 "${HIDMAESTRO_CORE_DLL}" _hidmaestro_core_actual)
    if(NOT _hidmaestro_core_actual STREQUAL HIDMAESTRO_CORE_SHA256)
        message(FATAL_ERROR "HIDMaestro.Core.dll hash mismatch: expected ${HIDMAESTRO_CORE_SHA256}, got ${_hidmaestro_core_actual}")
    endif()
endif()

message(STATUS "HIDMaestro v${HIDMAESTRO_VERSION} SDK: ${HIDMAESTRO_CORE_DLL}")
list(APPEND SUNSHINE_DEFINITIONS HIDMAESTRO_VERSION="${HIDMAESTRO_VERSION}")
