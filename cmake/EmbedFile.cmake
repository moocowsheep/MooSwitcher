# Usage: cmake -DIN=<binary> -DOUT=<header> -DNAME=<symbol> -P EmbedFile.cmake
# Embeds a binary file as a uint8_t array in namespace moo::shaders.
file(READ ${IN} _hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
file(WRITE ${OUT} "// Generated from ${IN} - do not edit.
#pragma once
#include <cstddef>
#include <cstdint>
namespace moo::shaders {
inline constexpr uint8_t ${NAME}[] = {${_bytes}};
inline constexpr size_t ${NAME}_size = sizeof(${NAME});
}  // namespace moo::shaders
")
