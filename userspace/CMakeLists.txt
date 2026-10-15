cmake_minimum_required(VERSION 3.16)

project(gxfpmoc C)

include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(MbedTLS REQUIRED)

add_library(gxfp
		src/io/dev.c
		src/algo/common.c
		src/algo/sensor_cfg.c
		src/algo/image/plain.c
		src/algo/image/decoder.c
		src/algo/image/fpn.c
		src/algo/payload/provision.c
		src/proto/goodix_proto.c
		src/proto/milan_hu_fdt.c
		src/tls/tls_engine_mbedtls.c
		src/tls/tls_service.c
		src/flow/device_recovery.c
		src/flow/fdt.c
		src/flow/session.c
		src/flow/chicago51b7.c
		src/cmd/tls_cmd.c
		src/cmd/goodix_xfer.c
		src/cmd/production_cmd.c
		src/cmd/device_recovery_cmd.c
		src/cmd/fdt_cmd.c
		src/cmd/mcu_state_cmd.c
		src/cmd/sensor_cfg_cmd.c
		src/cmd/register_cmd.c
)

target_include_directories(gxfp
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(gxfp PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(gxfp PRIVATE _GNU_SOURCE)
target_link_libraries(gxfp PUBLIC m)

add_executable(gxfp_psk_tool
		demos/psk_tool/main.c
)

target_link_libraries(gxfp_psk_tool PRIVATE gxfp)

target_compile_options(gxfp_psk_tool PRIVATE -Wall -Wextra -Wpedantic)

add_executable(gxfp_capture
		demos/capture/main.c
)

target_link_libraries(gxfp_capture PRIVATE gxfp)

target_compile_options(gxfp_capture PRIVATE -Wall -Wextra -Wpedantic)

add_executable(gxfp_recovery
		demos/recovery/main.c
)

target_link_libraries(gxfp_recovery PRIVATE gxfp)

target_compile_options(gxfp_recovery PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(gxfp PUBLIC MbedTLS::mbedtls)
target_link_libraries(gxfp_capture PRIVATE MbedTLS::mbedtls)
target_link_libraries(gxfp_recovery PRIVATE MbedTLS::mbedtls)

if(BUILD_TESTING)
	add_executable(gxfp_milan_hu_fdt_test tests/milan_hu_fdt_test.c)
	target_link_libraries(gxfp_milan_hu_fdt_test PRIVATE gxfp)
	add_test(NAME gxfp_milan_hu_fdt_codec COMMAND gxfp_milan_hu_fdt_test)

	add_executable(gxfp_fdt_retry_test tests/fdt_retry_test.c)
	target_link_libraries(gxfp_fdt_retry_test PRIVATE gxfp)
	add_test(NAME gxfp_fdt_retry_policy COMMAND gxfp_fdt_retry_test)

	add_executable(gxfp_chicago_hs_decode_test tests/chicago_hs_decode_test.c)
	target_link_libraries(gxfp_chicago_hs_decode_test PRIVATE gxfp)
	target_compile_options(gxfp_chicago_hs_decode_test PRIVATE -Wall -Wextra -Wpedantic)
	add_test(NAME gxfp_chicago_hs_decode COMMAND gxfp_chicago_hs_decode_test)

	# Interop with an independent TLS-PSK client; skipped without OpenSSL.
	find_package(OpenSSL)
	if(OpenSSL_FOUND)
		add_executable(gxfp_tls_psk_test tests/tls_psk_test.c)
		target_link_libraries(gxfp_tls_psk_test PRIVATE gxfp OpenSSL::SSL)
		target_compile_options(gxfp_tls_psk_test PRIVATE -Wall -Wextra -Wpedantic)
		add_test(NAME gxfp_tls_psk_interop COMMAND gxfp_tls_psk_test)
	endif()
endif()

add_executable(gxfp51b7_capture demos/chicago51b7/main.c)
target_link_libraries(gxfp51b7_capture PRIVATE gxfp)
target_compile_options(gxfp51b7_capture PRIVATE -Wall -Wextra -Wpedantic)
