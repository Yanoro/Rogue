#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "raylib.h"

// Helpers for turning what is currently on screen into something an HTTP JSON
// payload can carry.
//
// raylib already gives us the two hard parts -- LoadImageFromScreen() reads the
// framebuffer back and ExportImageToMemory() encodes it -- so the only thing
// missing for a vision model is the base64 step. Keeping that here means the
// providers stay about requests and this stays about pixels.

// Standard base64 (RFC 4648, with '=' padding, no line breaks). Empty input
// yields an empty string.
std::string EncodeBase64(const unsigned char *data, std::size_t length);

// One screenshot, in both the forms the callers need: `base64Png` goes into the
// JSON request, `pngBytes` is the same PNG before encoding and can be written to
// disk so a human can check what the model was actually shown.
struct CapturedImage {
  std::string base64Png;
  std::vector<unsigned char> pngBytes;
  int width = 0;
  int height = 0;

  bool Empty() const { return base64Png.empty(); }
};

// Reads the framebuffer back and encodes it as a PNG.
//
// `region` is in image (i.e. screen) pixels with the origin top-left. A region
// with zero width or height means "the whole image". The pixels are copied, so
// the caller does not have to keep the framebuffer intact afterwards.
//
// Returns false when the framebuffer has nothing in it, encoding fails, or
// `region` lies wholly outside the screen. On failure `out` is left empty.
//
// Note that this reads whatever is *currently* in the framebuffer, not what will
// be drawn later in the frame; see the call-site comments in VisionChatWindow for
// why that distinction matters.
bool CaptureScreenImage(CapturedImage &out, Rectangle region = {0, 0, 0, 0});
