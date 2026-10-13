# GekkoNet rollback netcode library (BSD 2-Clause), vendored from
# https://github.com/HeatXD/GekkoNet at commit 0d491a4524e10173fc5d2aae22d042d3ae9f2d44
# (GekkoLib only, without the ASIO-based default adapter). zpp_bits (MIT) is its
# serialization header.
file(GLOB GEKKONET_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(gekkonet STATIC ${GEKKONET_SRC})
set_target_properties(gekkonet PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	CXX_EXTENSIONS OFF
	POSITION_INDEPENDENT_CODE ON)
target_include_directories(gekkonet
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/private ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)
target_compile_definitions(gekkonet PUBLIC GEKKONET_STATIC GEKKONET_NO_ASIO)
if(NOT MSVC)
	target_compile_options(gekkonet PRIVATE -w)
endif()
