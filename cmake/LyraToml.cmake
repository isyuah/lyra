include(FetchContent)

function(lyra_enable_toml runtime)
    FetchContent_Declare(lyra_tomlc17
        URL https://codeload.github.com/cktan/tomlc17/tar.gz/f6c98d0f69993a18e75cb8cc6cad672f0942c107
        URL_HASH SHA256=2619b380bf735c7eb327ad3b988e9785f6fe9ac6a53d7b7543caeda87e71e2a6
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(lyra_tomlc17)
    target_sources(${runtime} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../runtime/toml.cpp" "${lyra_tomlc17_SOURCE_DIR}/src/tomlc17.c")
    target_include_directories(${runtime} PRIVATE "${lyra_tomlc17_SOURCE_DIR}/src")
    target_compile_features(${runtime} PRIVATE c_std_17)
    install(FILES "${lyra_tomlc17_SOURCE_DIR}/LICENSE" DESTINATION share/lyra/licenses/tomlc17)
endfunction()
