add_executable(discord_encrypt "discord_encrypt/main.cc")
target_compile_options(discord_encrypt PRIVATE "/MT")
target_link_libraries(discord_encrypt PRIVATE crypt32)
