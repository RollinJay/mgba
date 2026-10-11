# netplay-online: a Nakama client, direct connections between players, and the handoff
# from the launcher to an emulator (MIT license). The canonical copy lives in
# https://github.com/RollinJay/Nakama-Matchmaker; the mGBA and melonDS forks carry copies.
#
# The including project may set NETPLAY_ONLINE_QT to Qt5 or Qt6; otherwise Qt 6 is
# preferred. The GekkoNet adapter is built when a `gekkonet` target exists.
cmake_minimum_required(VERSION 3.10)

if(NOT NETPLAY_ONLINE_QT)
	find_package(Qt6 COMPONENTS Core Network QUIET)
	if(Qt6Network_FOUND)
		set(NETPLAY_ONLINE_QT Qt6)
	else()
		find_package(Qt5 COMPONENTS Core Network REQUIRED)
		set(NETPLAY_ONLINE_QT Qt5)
	endif()
else()
	find_package(${NETPLAY_ONLINE_QT} COMPONENTS Core Network REQUIRED)
endif()

add_library(netplay-online STATIC
	${CMAKE_CURRENT_SOURCE_DIR}/ConnectInfo.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/ConnectInfo.h
	${CMAKE_CURRENT_SOURCE_DIR}/MatchSession.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/MatchSession.h
	${CMAKE_CURRENT_SOURCE_DIR}/NakamaClient.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/NakamaClient.h
	${CMAKE_CURRENT_SOURCE_DIR}/PeerLinks.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/PeerLinks.h
	${CMAKE_CURRENT_SOURCE_DIR}/UdpSocket.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/UdpSocket.h
	${CMAKE_CURRENT_SOURCE_DIR}/WebSocket.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/WebSocket.h)
set_target_properties(netplay-online PROPERTIES
	AUTOMOC ON
	CXX_STANDARD 14
	CXX_STANDARD_REQUIRED ON
	POSITION_INDEPENDENT_CODE ON)
target_include_directories(netplay-online PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(netplay-online PUBLIC ${NETPLAY_ONLINE_QT}::Core ${NETPLAY_ONLINE_QT}::Network)
if(WIN32)
	target_link_libraries(netplay-online PUBLIC ws2_32)
endif()

if(TARGET gekkonet)
	add_library(netplay-online-gekko STATIC
		${CMAKE_CURRENT_SOURCE_DIR}/GekkoAdapter.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/GekkoAdapter.h)
	set_target_properties(netplay-online-gekko PROPERTIES
		CXX_STANDARD 14
		CXX_STANDARD_REQUIRED ON
		POSITION_INDEPENDENT_CODE ON)
	target_link_libraries(netplay-online-gekko PUBLIC netplay-online gekkonet)
endif()
