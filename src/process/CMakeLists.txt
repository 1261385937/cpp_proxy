project(process)


file(GLOB_RECURSE src_files ./*.cpp ./*.h ./*.hpp)

source_group(TREE ${CMAKE_SOURCE_DIR} FILES ${src_files})

add_executable(${PROJECT_NAME} ${src_files}) 

if (MSVC)
    target_compile_options(${PROJECT_NAME}
        PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
    )

    target_link_libraries(${PROJECT_NAME} Crypt32 IPHLPAPI)
    
else ()
    target_link_libraries(${PROJECT_NAME} -static-libgcc -static-libstdc++ dl pthread)
endif ()

