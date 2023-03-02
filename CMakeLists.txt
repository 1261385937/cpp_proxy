project(cpp_proxy)

cmake_minimum_required(VERSION 3.8)

set(CMAKE_CXX_STANDARD 20)

#set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
#set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)


if(NOT CMAKE_BUILD_TYPE)
    SET(CMAKE_BUILD_TYPE Release)
endif()

if (MSVC)
    add_compile_options(/W4)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd ")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd /Zc:__cplusplus")

    set(CMAKE_C_FLAGS_RELEASE  "${CMAKE_C_FLAGS_RELEASE} /MT")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT /Zc:__cplusplus")
else ()
    set(c_extra_wall   "-Wno-deprecated-declarations -Wempty-body -Wconversion -Wreturn-type -Wparentheses -Wno-format -Wuninitialized -Wunreachable-code -Wunused-function -Wunused-value -Wunused-variable -Wno-pointer-sign")
    set(cxx_extra_wall "-Wno-deprecated-declarations -Wempty-body -Wconversion -Wreturn-type -Wparentheses -Wno-format -Wuninitialized -Wunreachable-code -Wunused-function -Wunused-value -Wunused-variable")

    set(CMAKE_C_FLAGS_DEBUG  "${CMAKE_C_FLAGS_DEBUG} -fPIC -Wl,--as-needed -O0 -g -Wall -Wextra  -Werror ${c_extra_wall}")
    set(CMAKE_C_FLAGS_RELEASE  "${CMAKE_C_FLAGS_RELEASE} -fPIC -Wl,--as-needed -O3 -Wall -Wextra -Werror ${c_extra_wall}")

    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fPIC -Wl,--as-needed -O0 -g -Wall -Wextra  -Werror ${cxx_extra_wall}")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fPIC -Wl,--as-needed -O3 -Wall -Wextra -Werror ${cxx_extra_wall}") 
endif ()


if (MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif ()
add_definitions(-DSPDLOG_ACTIVE_LEVEL=0)
add_definitions(-D_WIN32_WINNT=0x0601)
add_definitions(-DASIO_STANDALONE)
add_definitions(-D_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)


include_directories(${PROJECT_SOURCE_DIR}/src)
include_directories(${PROJECT_SOURCE_DIR}/3party/spdlog-1.9.2/include)
include_directories(${PROJECT_SOURCE_DIR}/3party/asio-1.24.0/include)
include_directories(${PROJECT_SOURCE_DIR}/3party/json-3.10.5/include)


file(GLOB_RECURSE src_files src/*.cpp src/*.h src/*.hpp)

#source_group(app  src/main.cpp)
#source_group(util src/util)
#source_group(client src/client)
#source_group(controller src/controller)
#source_group(capture src/capture)

source_group(TREE ${CMAKE_SOURCE_DIR} FILES ${src_files})

add_executable(${PROJECT_NAME} ${src_files}) 

if (MSVC)
    target_compile_options(${PROJECT_NAME}
        PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
    )

    target_link_libraries(${PROJECT_NAME} Crypt32 IPHLPAPI)
    # add_custom_command(
    #     TARGET ${PROJECT_NAME} POST_BUILD
    #     COMMAND ${CMAKE_COMMAND} -E copy_if_different
    #     "${PROJECT_SOURCE_DIR}/3party/npcap-sdk-1.12/Lib/x64/Packet.dll"
    #     "${PROJECT_SOURCE_DIR}/3party/npcap-sdk-1.12/Lib/x64/wpcap.dll"
    #     $<TARGET_FILE_DIR:new_pcap_agent>
    # )

    #set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS " /MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\" ")
    
else ()
    target_link_libraries(${PROJECT_NAME} -static-libgcc -static-libstdc++ dl pthread)
endif ()

