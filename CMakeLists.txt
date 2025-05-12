CMAKE_MINIMUM_REQUIRED(VERSION 3.20)

PROJECT(UPP_SP2)

# pouzivame standard C++23 - pokud jej vas kompilator nepodporuje, zmente nejhure na 17 (C++17)
SET(CMAKE_CXX_STANDARD 23)

# najdeme vsechny soubory v podadresari src
FILE(GLOB_RECURSE src src/*.cpp src/*.h)

# nastavime, ze nechceme pouzivat SSL ve vychozim stavu - pokud chcete, muzete zapnout, abyste se dostali na https
SET(USE_SSL OFF CACHE BOOL "Use SSL")

include_directories("C:/Program Files (x86)/Microsoft SDKs/MPI/Include")
link_directories("C:/Program Files (x86)/Microsoft SDKs/MPI/Lib/x64")

IF(USE_SSL)
	ADD_DEFINITIONS(-DUSE_SSL)
	# najdeme OpenSSL
	FIND_PACKAGE(OpenSSL REQUIRED)

	INCLUDE_DIRECTORIES("${OPENSSL_INCLUDE_DIR}")
ENDIF()

INCLUDE_DIRECTORIES("dep/cpp-httplib")

ADD_EXECUTABLE(UPP-SP2 ${src})

# pokud chceme pouzivat SSL, musime prilinkovat OpenSSL
IF(USE_SSL)
	TARGET_LINK_LIBRARIES(UPP-SP2 OpenSSL::SSL OpenSSL::Crypto)
ENDIF()

IF(WIN32)
    TARGET_LINK_LIBRARIES(UPP-SP2 Ws2_32)
ENDIF()