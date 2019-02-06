########################################################################
# Project setup
########################################################################
cmake_minimum_required (VERSION 2.8)

#Check if using correct Visual studio version
if(MSVC_TOOLSET_VERSION LESS 140)
	message(FATAL_ERROR "MSVC version should be atleast v140(Visual Studio 2015")
endif()

project (ExtIO_LimeSDR)

cmake_policy(SET CMP0015 NEW)
cmake_policy(SET CMP0021 NEW)

#select the release build by default
if(NOT CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE "Release")
   message(STATUS "Build type not specified: defaulting to release.")
endif(NOT CMAKE_BUILD_TYPE)
set(CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE} CACHE STRING "")

option (BUILD_SHARED_LIBS "Build shared library" ON)

#include modules for finding CyAPI
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")

message(STATUS "Make sure that BUILD_SHARED_LIBS option matches BUILD_SHARED_LIBS option when LimeSuite was built")
message(STATUS "Only LimeSuite source and build directories should be needed, everything else is found automatically")
########################################################################
# Find LimeSuite
########################################################################
find_path(LIMESUITE_BUILD_DIR
	NAMES LimeSuite
	PATHS NO_DEFAULT_PATH
)
find_path(LIMESUITE_SOURCE_DIR
	PATHS NO_DEFAULT_PATH
)

find_path(LIMESUITE_INCLUDE_DIR 
	NAMES LimeSuite.h
	HINTS ${LIMESUITE_SOURCE_DIR}/src/lime
)

find_library(LIMESUITE_LIB 
	NAMES LimeSuite limesuite
	HINTS ${LIMESUITE_BUILD_DIR}/src/Release
	${LIMESUITE_BUILD_DIR}/src/Debug
)
########################################################################
# Add shared dll
########################################################################
add_library(ExtIO_LimeSDR SHARED 
	${CMAKE_SOURCE_DIR}/src/ExtIO_LimeSDR.cpp 
	${CMAKE_SOURCE_DIR}/src/ExtIO_LimeSDR.h 
	${CMAKE_SOURCE_DIR}/src/ExtIO_LimeSDR.def 
	${CMAKE_SOURCE_DIR}/src/LC_ExtIO_Types.h 
	${CMAKE_SOURCE_DIR}/src/resource.h 
	${CMAKE_SOURCE_DIR}/src/ExtIO_LimeSDR.def 
	${CMAKE_SOURCE_DIR}/src/ExtIO_LimeSDR.rc
)
#add LimeSuite libraries
target_link_libraries(ExtIO_LimeSDR PUBLIC ${LIMESUITE_LIB})
target_include_directories(ExtIO_LimeSDR PUBLIC ${LIMESUITE_INCLUDE_DIR})
########################################################################
# Check if linking the dll statically and find required libraries
########################################################################
if(BUILD_SHARED_LIBS STREQUAL "OFF")
	#find FTD3XX
	find_path(FTD3XX_INCLUDE_DIR
		NAMES FTD3XX.h
		HINTS ${LIMESUITE_SOURCE_DIR}/src/ConnectionFTDI/FTD3XXLibrary
	)
	find_library(FTD3XX_LIB_STATIC
		NAMES FTD3XX.lib
		HINTS ${FTD3XX_INCLUDE_DIR}/Win32/Static
	)
	
	#find CyAPI
	include(FindCyAPI)
	
	#add libraries
	target_include_directories(ExtIO_LimeSDR PUBLIC ${FTD3XX_INCLUDE_DIR})
	target_link_libraries(ExtIO_LimeSDR PUBLIC ${FTD3XX_LIB_STATIC} ${CYAPI_LIBRARY})
endif()
















