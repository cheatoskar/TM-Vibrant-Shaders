# Fails the build when a compiled pixel shader is longer than many D3D9 drivers accept.
# ps_3_0 only guarantees 512 instruction slots; NVIDIA and others report 4096, some 32768.
# A longer shader runs fine on a 32768 driver and fails CreatePixelShader everywhere else
# (1.3.0: PS_Lighting at 4718 slots switched the whole mod off on those PCs).
#   cmake -DHEADER=<fxc /Fh output> -DLIMIT=4096 -P check_shader_slots.cmake
file(READ "${HEADER}" text)
string(REGEX MATCH "approximately ([0-9]+) instruction slots? used" found "${text}")
if(NOT found)
  message(FATAL_ERROR "${HEADER}: no instruction slot count found")
endif()
if(CMAKE_MATCH_1 GREATER LIMIT)
  get_filename_component(entry "${HEADER}" NAME_WE)
  file(REMOVE "${HEADER}") # or the next build would take it as up to date
  message(FATAL_ERROR "${entry}: ${CMAKE_MATCH_1} instruction slots, more than ${LIMIT} (many D3D9 drivers reject it). Use [loop] instead of [unroll], or call expensive functions once.")
endif()
