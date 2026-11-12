cmake_minimum_required(VERSION 3.16)
project(dp104_openrgb VERSION 1.2.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

add_library(dp104_receiver STATIC src/firmware/dp104_receiver.c)
target_include_directories(dp104_receiver PUBLIC include src/firmware)
add_library(dp104_initdata STATIC src/firmware/dp104_initdata.c)
target_include_directories(dp104_initdata PUBLIC src/firmware)
add_executable(test_initdata tests/test_initdata.c)
target_link_libraries(test_initdata PRIVATE dp104_initdata)
add_test(NAME initdata_decoder COMMAND test_initdata)
add_library(dp104_twiddle STATIC src/firmware/dp104_twiddle.c)
target_include_directories(dp104_twiddle PUBLIC src/firmware)
add_executable(test_twiddle tests/test_twiddle.c)
target_link_libraries(test_twiddle PRIVATE dp104_twiddle)
add_test(NAME twiddle_boundary_contract COMMAND test_twiddle)
add_library(dp104_lz STATIC src/firmware/dp104_lz.c)
target_include_directories(dp104_lz PUBLIC src/firmware)
add_executable(test_lz tests/test_lz.c)
target_link_libraries(test_lz PRIVATE dp104_lz)
add_test(NAME lz_decoder COMMAND test_lz)
add_library(dp104_master_bridge STATIC src/firmware/dp104_master_bridge.c src/firmware/dp104_wire.c src/firmware/dp104_wire_stream.c src/firmware/dp104_usb_stage.c)
target_include_directories(dp104_master_bridge PUBLIC include src/firmware)
add_executable(test_receiver tests/test_receiver.c)
target_link_libraries(test_receiver PRIVATE dp104_receiver)
add_test(NAME receiver COMMAND test_receiver)

add_library(dp104_host STATIC src/host/dp104_host_client.cpp src/host/dp104_calibration.cpp)
target_include_directories(dp104_host PUBLIC include src/host)
add_executable(test_host tests/test_host.cpp)
target_link_libraries(test_host PRIVATE dp104_host)
add_test(NAME host COMMAND test_host)
add_executable(test_calibration tests/test_calibration.cpp)
target_link_libraries(test_calibration PRIVATE dp104_host)
add_test(NAME calibration_assertion COMMAND test_calibration)

add_executable(test_end_to_end tests/test_end_to_end.cpp)
target_link_libraries(test_end_to_end PRIVATE dp104_host dp104_receiver)
add_test(NAME end_to_end COMMAND test_end_to_end)
add_executable(test_master_bridge tests/test_master_bridge.cpp)
target_link_libraries(test_master_bridge PRIVATE dp104_host dp104_receiver dp104_master_bridge)
add_test(NAME master_bridge COMMAND test_master_bridge)
add_executable(test_wire tests/test_wire.cpp)
target_link_libraries(test_wire PRIVATE dp104_master_bridge)
add_test(NAME wire_records COMMAND test_wire)
add_executable(test_usb_stage tests/test_usb_stage.c)
target_link_libraries(test_usb_stage PRIVATE dp104_master_bridge)
add_test(NAME usb_staging_lifetime COMMAND test_usb_stage)
add_executable(test_dma_rx tests/test_dma_rx.c
    src/firmware/dp104_dma_rx.c src/firmware/dp104_wire.c)
target_include_directories(test_dma_rx PRIVATE include src/firmware)
add_test(NAME circular_dma_rx_cursor COMMAND test_dma_rx)
add_executable(test_plc_target tests/test_plc_target.c
    src/firmware/dp104_plc_target.c src/firmware/dp104_plc_diagnostic.c
    src/firmware/dp104_dma_rx.c)
target_compile_definitions(test_plc_target PRIVATE DP104_PLC_HOST_TEST=1)
target_link_libraries(test_plc_target PRIVATE dp104_receiver dp104_master_bridge dp104_twiddle)
add_test(NAME plc_target_binding COMMAND test_plc_target)
add_executable(test_master_target tests/test_master_target.c src/firmware/dp104_master_target.c)
target_compile_definitions(test_master_target PRIVATE DP104_MASTER_TARGET_HOST_TEST=1)
target_link_libraries(test_master_target PRIVATE dp104_master_bridge)
add_test(NAME master_target_binding COMMAND test_master_target)

# Compile against an external OpenRGB tree without changing or launching it.
option(DP104_CAPTURE_FAILURE_DIAGNOSTICS
       "Enable one-shot DP104 failure evidence capture in the OpenRGB adapter" OFF)
set(DP104_OPENRGB_SOURCE "" CACHE PATH "OpenRGB source tree for adapter compilation")
if(DP104_OPENRGB_SOURCE)
    if(NOT EXISTS "${DP104_OPENRGB_SOURCE}/RGBController/RGBController.h")
        message(FATAL_ERROR "DP104_OPENRGB_SOURCE must name an OpenRGB source root")
    endif()
    set(DP104_HIDAPI_INCLUDE "${DP104_OPENRGB_SOURCE}/dependencies/hidapi-win/include"
        CACHE PATH "Directory containing hidapi.h")
    add_library(dp104_openrgb_adapter OBJECT
        src/openrgb/Dp104HidTransport.cpp
        src/openrgb/RGBController_DP104.cpp
        src/openrgb/Dp104ControllerDetect.cpp)
    target_link_libraries(dp104_openrgb_adapter PRIVATE dp104_host)
    target_include_directories(dp104_openrgb_adapter PRIVATE
        src/openrgb "${DP104_OPENRGB_SOURCE}"
        "${DP104_OPENRGB_SOURCE}/RGBController"
        "${DP104_OPENRGB_SOURCE}/SPDAccessor"
        "${DP104_OPENRGB_SOURCE}/hidapi_wrapper"
        "${DP104_OPENRGB_SOURCE}/i2c_smbus"
        "${DP104_OPENRGB_SOURCE}/dependencies/json"
        "${DP104_OPENRGB_SOURCE}/dependencies/libusb-1.0.27/include"
        "${DP104_HIDAPI_INCLUDE}")
    if(DP104_CAPTURE_FAILURE_DIAGNOSTICS)
        target_compile_definitions(dp104_openrgb_adapter PRIVATE
            DP104_CAPTURE_FAILURE_DIAGNOSTICS=1)
    endif()
    add_executable(test_host_hid tests/test_host.cpp src/openrgb/Dp104HidTransport.cpp)
    target_link_libraries(test_host_hid PRIVATE dp104_host)
    target_include_directories(test_host_hid PRIVATE src/openrgb "${DP104_HIDAPI_INCLUDE}")
    target_compile_definitions(test_host_hid PRIVATE DP104_TEST_OPENRGB_HID=1)
    add_test(NAME host_hid COMMAND test_host_hid)
    add_executable(test_controller tests/test_controller.cpp
        src/openrgb/Dp104HidTransport.cpp src/openrgb/RGBController_DP104.cpp)
    target_link_libraries(test_controller PRIVATE dp104_host dp104_receiver)
    target_include_directories(test_controller PRIVATE
        tests/stubs src/openrgb "${DP104_HIDAPI_INCLUDE}")
    add_test(NAME controller_lifecycle COMMAND test_controller)
    add_executable(test_failure_capture_default tests/test_failure_capture.cpp
        src/openrgb/Dp104HidTransport.cpp)
    target_link_libraries(test_failure_capture_default PRIVATE dp104_host)
    target_include_directories(test_failure_capture_default PRIVATE
        src/openrgb "${DP104_HIDAPI_INCLUDE}")
    target_compile_definitions(test_failure_capture_default PRIVATE
        DP104_TEST_OPENRGB_HID=1 DP104_CAPTURE_FAILURE_DIAGNOSTICS=0)
    add_test(NAME failure_capture_default_behavior COMMAND test_failure_capture_default)
    add_executable(test_failure_capture tests/test_failure_capture.cpp
        src/openrgb/Dp104HidTransport.cpp)
    target_link_libraries(test_failure_capture PRIVATE dp104_host)
    target_include_directories(test_failure_capture PRIVATE
        src/openrgb "${DP104_HIDAPI_INCLUDE}")
    target_compile_definitions(test_failure_capture PRIVATE
        DP104_TEST_OPENRGB_HID=1 DP104_CAPTURE_FAILURE_DIAGNOSTICS=1)
    add_test(NAME failure_capture_diagnostics COMMAND test_failure_capture)
    if(MSVC)
        target_compile_definitions(dp104_openrgb_adapter PRIVATE NOMINMAX)
        target_compile_options(dp104_openrgb_adapter PRIVATE /W4)
    else()
        target_compile_options(dp104_openrgb_adapter PRIVATE -Wall -Wextra)
    endif()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME firmware_input_guard
             COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_target.py")
    add_test(NAME firmware_hook_plan
             COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_hook_plan.py")
    add_test(NAME initdata_codec
             COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_initdata_codec.py")
    add_test(NAME lz_codec
             COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lz_codec.py")
    add_test(NAME calibration_tool
             COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_calibration_tool.py")
    add_test(NAME master_image_composition
             COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_master_builder.py")
    add_test(NAME release_consistency
             COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release.py")
endif()

if(MSVC)
    foreach(target test_calibration test_plc_target test_master_target test_dma_rx)
        target_compile_options(${target} PRIVATE /W4 /WX)
    endforeach()
    target_compile_options(dp104_receiver PRIVATE /W4 /WX)
    target_compile_options(dp104_initdata PRIVATE /W4 /WX)
    target_compile_options(dp104_twiddle PRIVATE /W4 /WX)
    target_compile_options(dp104_lz PRIVATE /W4 /WX)
    target_compile_options(test_lz PRIVATE /W4 /WX)
    target_compile_options(test_twiddle PRIVATE /W4 /WX)
    target_compile_options(test_initdata PRIVATE /W4 /WX)
    target_compile_options(test_usb_stage PRIVATE /W4 /WX)
    target_compile_options(dp104_master_bridge PRIVATE /W4 /WX)
    target_compile_options(test_master_bridge PRIVATE /W4 /WX)
    target_compile_options(test_receiver PRIVATE /W4 /WX)
    target_compile_options(dp104_host PRIVATE /W4 /WX)
    target_compile_options(test_host PRIVATE /W4 /WX)
    target_compile_options(test_end_to_end PRIVATE /W4 /WX)
    if(TARGET test_failure_capture_default)
        target_compile_options(test_failure_capture_default PRIVATE /W4 /WX)
        target_compile_options(test_failure_capture PRIVATE /W4 /WX)
    endif()
else()
    foreach(target test_calibration test_plc_target test_master_target test_dma_rx)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endforeach()
    target_compile_options(dp104_receiver PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(dp104_initdata PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(dp104_twiddle PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(dp104_lz PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(test_lz PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(test_twiddle PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(test_initdata PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(test_usb_stage PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(dp104_master_bridge PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(test_master_bridge PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(test_receiver PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(dp104_host PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(test_host PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(test_end_to_end PRIVATE -Wall -Wextra -Werror -pedantic)
    if(TARGET test_failure_capture_default)
        target_compile_options(test_failure_capture_default PRIVATE
            -Wall -Wextra -Werror -pedantic)
        target_compile_options(test_failure_capture PRIVATE
            -Wall -Wextra -Werror -pedantic)
    endif()
endif()
