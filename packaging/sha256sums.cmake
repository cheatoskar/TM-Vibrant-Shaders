# Writes SHA256SUMS.txt for the release files in DIR (sha256sum format).
set(files TM-Vibrant-Shaders.zip TMVibrantShaders.dll)
set(out "")
foreach(file ${files})
  file(SHA256 "${DIR}/${file}" hash)
  string(APPEND out "${hash}  ${file}\n")
endforeach()
# LF line endings, so `sha256sum -c SHA256SUMS.txt` works too.
file(CONFIGURE OUTPUT "${DIR}/SHA256SUMS.txt" CONTENT "${out}" NEWLINE_STYLE UNIX)
message(STATUS "SHA256SUMS.txt:\n${out}")
