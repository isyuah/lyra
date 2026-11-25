cmake_minimum_required(VERSION 3.28)

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${LINKER}"
    LIBRARIES "${LLVM}"
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR missing
    PRE_EXCLUDE_REGEXES "^(ld-linux-x86-64|lib(c|m|dl|pthread|rt|util|resolv))\\.so\\.")
if(missing)
    message(FATAL_ERROR "Cannot package Linux SDK dependencies: ${missing}")
endif()
file(WRITE "${OUTPUT}" "set(dependencies [==[${dependencies}]==])\n")
