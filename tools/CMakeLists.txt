add_executable(discord_encrypt "discord_encrypt/main.cc")
target_compile_options(discord_encrypt PRIVATE "/MT")
# discord_key.h lives under src/; add it to the include path.
target_include_directories(discord_encrypt PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(discord_encrypt PRIVATE bcrypt)
