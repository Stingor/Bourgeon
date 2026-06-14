# Computes the SHA-256 of the file DLL, prints it, and writes it (hash only) to
# the file OUT. Invoked as a POST_BUILD step via `cmake -P` so the hash of the
# freshly-built ddraw.dll is available for conf/bourgeon_integrity.conf.
file(SHA256 "${DLL}" BOURGEON_SHA256)
file(WRITE "${OUT}" "${BOURGEON_SHA256}\n")

message(STATUS "----------------------------------------------------------------")
message(STATUS "ddraw.dll SHA-256: ${BOURGEON_SHA256}")
message(STATUS "  written to: ${OUT}")
message(STATUS "  server cfg: hash: ${BOURGEON_SHA256}")
message(STATUS "----------------------------------------------------------------")
