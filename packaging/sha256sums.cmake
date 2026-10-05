# Writes SHA256SUMS.txt for the release files in DIR (sha256sum format).
set(files TM-Vibrant-Shaders.zip TMVibrantShaders.dll)
set(out "")
foreach(file ${files})
  file(SHA256 "${DIR}/${file}" hash)
  string(APPEND out "${hash}  ${file}\n")
endforeach()
file(WRITE "${DIR}/SHA256SUMS.txt" "${out}")
message(STATUS "SHA256SUMS.txt:\n${out}")
