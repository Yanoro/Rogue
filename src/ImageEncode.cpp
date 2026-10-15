#include "ImageEncode.h"

#include <cstdint>

// For rlDrawRenderBatchActive(), which is the only way to force raylib to hand
// its queued geometry to the GPU before reading the framebuffer back. raylib.h
// does not re-export rlgl.h, so this is a deliberate direct include of a
// dependency the project already vendors.
#include "rlgl.h"

namespace {

const char BASE64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

std::string EncodeBase64(const unsigned char *data, std::size_t length) {
  if (data == nullptr || length == 0) {
    return "";
  }

  // Every 3 input bytes become 4 output characters, rounded up.
  std::string out;
  out.reserve(((length + 2) / 3) * 4);

  std::size_t i = 0;
  for (; i + 2 < length; i += 3) {
    const std::uint32_t chunk = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                static_cast<std::uint32_t>(data[i + 2]);
    out.push_back(BASE64_ALPHABET[(chunk >> 18) & 0x3F]);
    out.push_back(BASE64_ALPHABET[(chunk >> 12) & 0x3F]);
    out.push_back(BASE64_ALPHABET[(chunk >> 6) & 0x3F]);
    out.push_back(BASE64_ALPHABET[chunk & 0x3F]);
  }

  // The 1- or 2-byte tail is padded with '=' so the output length is a multiple
  // of four, which is what decoders (and Ollama's JSON parsing) expect.
  const std::size_t remaining = length - i;
  if (remaining == 1) {
    const std::uint32_t chunk = static_cast<std::uint32_t>(data[i]) << 16;
    out.push_back(BASE64_ALPHABET[(chunk >> 18) & 0x3F]);
    out.push_back(BASE64_ALPHABET[(chunk >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (remaining == 2) {
    const std::uint32_t chunk = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8);
    out.push_back(BASE64_ALPHABET[(chunk >> 18) & 0x3F]);
    out.push_back(BASE64_ALPHABET[(chunk >> 12) & 0x3F]);
    out.push_back(BASE64_ALPHABET[(chunk >> 6) & 0x3F]);
    out.push_back('=');
  }

  return out;
}

bool CaptureScreenImage(CapturedImage &out, Rectangle region) {
  out = CapturedImage{};

  // raylib queues geometry on the CPU and only hands it to the GPU when a batch
  // fills, when a state change forces it, or at EndDrawing. A read-back taken
  // mid-frame therefore sees whatever was flushed so far -- typically a cleared
  // or previous frame, i.e. a black screenshot. Flushing first makes the capture
  // match what has been drawn so far this frame. (Verified the hard way: without
  // this line the drawn rectangles came back as pure black.)
  rlDrawRenderBatchActive();

  Image screen = LoadImageFromScreen();
  if (screen.data == nullptr || screen.width <= 0 || screen.height <= 0) {
    // Nothing was drawn yet (or the window has no framebuffer), so there is no
    // honest image to send. Reporting this beats sending an empty PNG.
    if (screen.data != nullptr) {
      UnloadImage(screen);
    }
    return false;
  }

  Image captured = screen;
  bool capturedIsCopy = false;

  if (region.width > 0.0f && region.height > 0.0f) {
    // ImageFromImage copies the sub-rectangle, so `screen` still has to be
    // unloaded separately. A region that does not intersect the screen at all
    // would silently produce a zero-sized image, so reject it here instead.
    const Rectangle bounds{0.0f, 0.0f, static_cast<float>(screen.width),
                           static_cast<float>(screen.height)};
    if (!CheckCollisionRecs(region, bounds)) {
      UnloadImage(screen);
      return false;
    }
    captured = ImageFromImage(screen, region);
    capturedIsCopy = true;
  }

  bool ok = false;
  if (captured.data != nullptr && captured.width > 0 && captured.height > 0) {
    int pngSize = 0;
    unsigned char *pngBytes = ExportImageToMemory(captured, ".png", &pngSize);
    if (pngBytes != nullptr && pngSize > 0) {
      out.pngBytes.assign(pngBytes, pngBytes + pngSize);
      out.base64Png = EncodeBase64(pngBytes, static_cast<std::size_t>(pngSize));
      out.width = captured.width;
      out.height = captured.height;
      ok = !out.base64Png.empty();
    }
    // ExportImageToMemory returns a MemAlloc'd buffer that raylib does not own;
    // it is not freed by UnloadImage.
    if (pngBytes != nullptr) {
      MemFree(pngBytes);
    }
  }

  if (capturedIsCopy) {
    UnloadImage(captured);
  }
  UnloadImage(screen);

  if (!ok) {
    out = CapturedImage{};
  }
  return ok;
}
