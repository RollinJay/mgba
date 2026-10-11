# Rollback netplay for linked GBAs (see doc/netplay.md): profiles, the GekkoNet session
# around a GBA link machine, and a UDP adapter. Linked by frontends and test tools.
add_library(${BINARY_NAME}-netplay STATIC
	${CMAKE_CURRENT_SOURCE_DIR}/profile.c
	${CMAKE_CURRENT_SOURCE_DIR}/session.c
	${CMAKE_CURRENT_SOURCE_DIR}/udp.c)
target_link_libraries(${BINARY_NAME}-netplay PUBLIC ${BINARY_NAME} gekkonet)
if(WIN32)
	target_link_libraries(${BINARY_NAME}-netplay PUBLIC ws2_32)
endif()
set_target_properties(${BINARY_NAME}-netplay PROPERTIES
	COMPILE_DEFINITIONS "${OS_DEFINES};${FEATURE_DEFINES};${FUNCTION_DEFINES}"
	POSITION_INDEPENDENT_CODE ON)
