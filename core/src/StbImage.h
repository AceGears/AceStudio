#pragma once
// Private wrapper so only one translation unit owns the stb implementation.
// PNG only: GIF is decoded by AceStudio's own decoder (AnimCodec.cpp), since
// fuzzing found a use-after-free in stb_image's GIF loader.
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stb_image.h>
