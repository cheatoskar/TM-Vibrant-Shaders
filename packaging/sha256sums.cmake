# Writes SHA256SUMS.txt for the release files in DIR (sha256sum format).
get_filename_component(DIR "${DIR}" ABSOLUTE)
set(files TM-Vibrant-Shaders.zip TMVibrantShaders.dll)
# The Horizon modules' zip (release_zip makes it: TMVS-<modules version>-horizon.zip).
file(GLOB horizon_zips RELATIVE "${DIR}" "${DIR}/TMVS-*-horizon.zip")
list(SORT horizon_zips)
list(APPEND files ${horizon_zips})
# The standalone clips mod, once clips_zip made it.
if(EXISTS "${DIR}/TM-Clips.zip")
  list(APPEND files TM-Clips.zip)
endif()
set(out "")
foreach(file ${files})
  file(SHA256 "${DIR}/${file}" hash)
  string(APPEND out "${hash}  ${file}\n")
endforeach()
# LF line endings, so `sha256sum -c SHA256SUMS.txt` works too.
file(CONFIGURE OUTPUT "${DIR}/SHA256SUMS.txt" CONTENT "${out}" NEWLINE_STYLE UNIX)
message(STATUS "SHA256SUMS.txt:\n${out}")
