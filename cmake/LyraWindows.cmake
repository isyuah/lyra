include(ExternalProject)

function(lyra_stage_windows_sdk)
    set(LYRA_WINDOWS_SYSROOT "$ENV{LYRA_WINDOWS_SYSROOT}" CACHE PATH "Root of the x86-64 LLVM-MinGW UCRT toolchain")
    set(LYRA_WINDOWS_TARGET_PORTS "$ENV{LYRA_WINDOWS_TARGET_PORTS}" CACHE PATH "vcpkg x64-mingw-static installation prefix")
    if(NOT DEFINED LYRA_WINDOWS_REDIST AND DEFINED ENV{VCToolsRedistDir})
        file(GLOB redistCandidates "$ENV{VCToolsRedistDir}/x64/Microsoft.VC*.CRT")
        list(SORT redistCandidates COMPARE NATURAL ORDER DESCENDING)
        if(redistCandidates)
            list(GET redistCandidates 0 redist)
        endif()
    endif()
    set(LYRA_WINDOWS_REDIST "${redist}" CACHE PATH "Redistributable x64 VC CRT DLL directory")
    set(bin "${LYRA_WINDOWS_SYSROOT}/bin")
    set(cc "${bin}/x86_64-w64-mingw32-clang.exe")
    set(cxx "${bin}/x86_64-w64-mingw32-clang++.exe")
    if(NOT EXISTS "${cc}" OR NOT EXISTS "${cxx}" OR NOT IS_DIRECTORY "${LYRA_WINDOWS_TARGET_PORTS}")
        message(FATAL_ERROR "Set LYRA_WINDOWS_SYSROOT to LLVM-MinGW UCRT and LYRA_WINDOWS_TARGET_PORTS to the vcpkg x64-mingw-static prefix (see the Windows build instructions).")
    endif()
    find_program(LYRA_LLD NAMES lld-link HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
    lyra_stage_linker("${LYRA_LLD}" lld-link.exe)
    foreach(dll IN ITEMS msvcp140.dll vcruntime140.dll vcruntime140_1.dll)
        lyra_stage_sdk_input("${LYRA_WINDOWS_REDIST}/${dll}" "bin/${dll}")
        lyra_stage_sdk_input("${LYRA_WINDOWS_REDIST}/${dll}" "libexec/lyra/${dll}")
    endforeach()

    set(syslib "${LYRA_WINDOWS_SYSROOT}/x86_64-w64-mingw32/lib")
    foreach(input IN ITEMS crt2u.o crtbegin.o crtend.o libc++.a libunwind.a libmingw32.a libmingwex.a libmoldname.a libmsvcrt.a
            libws2_32.a libiphlpapi.a libuserenv.a libpsapi.a libadvapi32.a libuser32.a libdbghelp.a libole32.a libshell32.a libkernel32.a
            libbcrypt.a libcrypt32.a libgdi32.a)
        lyra_stage_sdk_input("${syslib}/${input}" "lib/lyra/target/${input}")
    endforeach()
    execute_process(COMMAND "${cc}" --print-libgcc-file-name OUTPUT_VARIABLE builtins OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    lyra_stage_sdk_input("${builtins}" lib/lyra/target/libclang_rt.builtins.a)
    install(FILES "${LYRA_WINDOWS_SYSROOT}/LICENSE.TXT" DESTINATION share/lyra/licenses/llvm-mingw)
    install(DIRECTORY "${LYRA_WINDOWS_SYSROOT}/x86_64-w64-mingw32/share/mingw32/" DESTINATION share/lyra/licenses/mingw-w64
        FILES_MATCHING PATTERN "COPYING*")

    ExternalProject_Add(lyraRuntime
        SOURCE_DIR "${CMAKE_SOURCE_DIR}/runtime"
        BINARY_DIR "${CMAKE_BINARY_DIR}/windows-runtime"
        INSTALL_DIR "${CMAKE_BINARY_DIR}"
        CMAKE_ARGS "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}" "-DCMAKE_C_COMPILER=${cc}" "-DCMAKE_CXX_COMPILER=${cxx}"
            "-DCMAKE_PREFIX_PATH=${LYRA_WINDOWS_TARGET_PORTS}" "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DBUILD_TESTING=${BUILD_TESTING}" "-DLYRA_LLHTTP_SOURCE_DIR=${LYRA_LLHTTP_SOURCE_DIR}"
            "-DLYRA_LLHTTP_OFFLINE=${LYRA_LLHTTP_OFFLINE}" "-DFETCHCONTENT_SOURCE_DIR_LYRA_TOMLC17=${FETCHCONTENT_SOURCE_DIR_LYRA_TOMLC17}"
            "-DFETCHCONTENT_FULLY_DISCONNECTED=${FETCHCONTENT_FULLY_DISCONNECTED}" "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
        BUILD_ALWAYS TRUE)
    foreach(archive IN ITEMS liblyraRuntime.a libllhttp.a libgc.a libuv.a libjansson.a libsqlite3.a
            liblyraHttpClient.a liblyraHttpTls.a liblyraCompression.a libcurl.a libcares.a libssl.a libcrypto.a libzs.a)
        install(FILES "${CMAKE_BINARY_DIR}/lib/${archive}" DESTINATION lib)
    endforeach()
    install(PROGRAMS "${CMAKE_BINARY_DIR}/libexec/lyra/lyra-registry-download.exe" DESTINATION libexec/lyra)
    foreach(port IN ITEMS bdwgc libuv jansson sqlite3 llhttp tomlc17 curl c-ares openssl zlib)
        install(DIRECTORY "${CMAKE_BINARY_DIR}/share/lyra/licenses/${port}" DESTINATION share/lyra/licenses)
    endforeach()
endfunction()

function(lyra_relocate_llvm_dia)
    if(NOT TARGET LLVMDebugInfoPDB)
        return()
    endif()
    get_target_property(libraries LLVMDebugInfoPDB INTERFACE_LINK_LIBRARIES)
    foreach(library IN LISTS libraries)
        if(IS_ABSOLUTE "${library}" AND library MATCHES "/diaguids\\.lib$" AND NOT EXISTS "${library}")
            find_library(LYRA_DIA_GUIDS NAMES diaguids HINTS "$ENV{VSINSTALLDIR}/DIA SDK/lib/amd64" REQUIRED)
            list(REMOVE_ITEM libraries "${library}")
            list(APPEND libraries "${LYRA_DIA_GUIDS}")
        endif()
    endforeach()
    set_property(TARGET LLVMDebugInfoPDB PROPERTY INTERFACE_LINK_LIBRARIES "${libraries}")
endfunction()
