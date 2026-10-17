// Copyright (c) 2026, Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "wallpaper.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
using Pixbuf = std::unique_ptr<GdkPixbuf, decltype(&g_object_unref)>;
using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct TemporaryDirectory {
  fs::path path;
  TemporaryDirectory() {
    std::string name = (fs::temp_directory_path() / "midnight-wallpaper-XXXXXX").string();
    require(mkdtemp(name.data()) != nullptr, "cannot create fixture directory");
    path = name;
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};

Surface render(const midnight::Wallpaper& wallpaper, bool fill) {
  Surface image(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 100),
                cairo_surface_destroy);
  require(cairo_surface_status(image.get()) == CAIRO_STATUS_SUCCESS,
          "cannot create rendering surface");
  Context context(cairo_create(image.get()), cairo_destroy);
  cairo_set_source_rgb(context.get(), 0, 0, 1);
  cairo_paint(context.get());
  wallpaper.paint(context.get(), 0, 0, 100, 100, fill);
  require(cairo_status(context.get()) == CAIRO_STATUS_SUCCESS, "Cairo paint failed");
  cairo_surface_flush(image.get());
  return image;
}

std::uint32_t pixel(cairo_surface_t* surface, int x, int y) {
  const auto* row = reinterpret_cast<const std::uint32_t*>(
      cairo_image_surface_get_data(surface) +
      y * cairo_image_surface_get_stride(surface));
  return row[x];
}

void save(GdkPixbuf* image, const fs::path& path, const char* format) {
  require(gdk_pixbuf_save(image, path.c_str(), format, nullptr, nullptr),
          "cannot save raster fixture");
}
} // namespace

int main() {
  try {
    TemporaryDirectory temporary;
    Pixbuf stripes(gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 200, 100),
                   g_object_unref);
    require(stripes != nullptr, "cannot allocate raster fixture");
    auto* data = gdk_pixbuf_get_pixels(stripes.get());
    const int stride = gdk_pixbuf_get_rowstride(stripes.get());
    for (int y = 0; y < 100; ++y) {
      for (int x = 0; x < 200; ++x) {
        auto* value = data + y * stride + x * 3;
        value[0] = x < 50 ? 255 : 0;
        value[1] = x >= 50 && x < 150 ? 255 : 0;
        value[2] = x >= 150 ? 255 : 0;
      }
    }
    const auto png = temporary.path / "stripes.png";
    const auto jpeg = temporary.path / "stripes.jpg";
    save(stripes.get(), png, "png");
    save(stripes.get(), jpeg, "jpeg");

    midnight::Wallpaper wallpaper;
    require(wallpaper.load(png, 100, 100), "PNG load failed");
    auto fit = render(wallpaper, false);
    require(pixel(fit.get(), 50, 10) == 0xff0000ff, "fit lost background letterbox");
    require(pixel(fit.get(), 10, 50) == 0xffff0000, "fit cropped left image edge");
    require(pixel(fit.get(), 50, 50) == 0xff00ff00, "fit center incorrect");
    auto fill = render(wallpaper, true);
    require(pixel(fill.get(), 10, 10) == 0xff00ff00, "fill did not center and crop");
    require(pixel(fill.get(), 90, 90) == 0xff00ff00, "fill did not cover rectangle");

    require(wallpaper.load(jpeg, 100, 100), "JPEG load failed");
    auto jpeg_render = render(wallpaper, true);
    const auto green = pixel(jpeg_render.get(), 50, 50);
    require(((green >> 8) & 255) > 240 && ((green >> 16) & 255) < 15 &&
                (green & 255) < 15, "JPEG pixel decode incorrect");

    // Two one-pixel frames: red then blue. The wallpaper must retain red.
    constexpr unsigned char gif[] = {
      'G','I','F','8','9','a', 1,0,1,0, 0x80,0,0,
      255,0,0, 0,0,255,
      0x21,0xf9,4,0,10,0,0,0,
      0x2c,0,0,0,0,1,0,1,0,0, 2,2,0x44,0x01,0,
      0x21,0xf9,4,0,10,0,0,0,
      0x2c,0,0,0,0,1,0,1,0,0, 2,2,0x4c,0x01,0,
      0x3b
    };
    const auto gif_path = temporary.path / "animated.gif";
    {
      std::ofstream output(gif_path, std::ios::binary);
      output.write(reinterpret_cast<const char*>(gif), sizeof(gif));
      require(static_cast<bool>(output), "cannot write GIF fixture");
    }
    require(wallpaper.load(gif_path, 100, 100), "GIF load failed");
    auto gif_render = render(wallpaper, true);
    require(pixel(gif_render.get(), 50, 50) == 0xffff0000,
            "GIF did not use its first frame");

    const auto svg = temporary.path / "transparent.svg";
    {
      std::ofstream output(svg);
      output << R"(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10"><rect width="20" height="10" fill="red" fill-opacity="0.5"/></svg>)";
      require(static_cast<bool>(output), "cannot write SVG fixture");
    }
    require(wallpaper.load(svg, 100, 100), "SVG load failed (SVG loader required)");
    auto svg_render = render(wallpaper, true);
    require(pixel(svg_render.get(), 50, 50) == 0xff80007f,
            "SVG alpha did not composite over desktop color");

    Pixbuf translucent(gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 4, 4),
                       g_object_unref);
    require(translucent != nullptr, "cannot allocate alpha fixture");
    gdk_pixbuf_fill(translucent.get(), 0xff000080);
    const auto alpha_png = temporary.path / "transparent.png";
    save(translucent.get(), alpha_png, "png");
    require(wallpaper.load(alpha_png, 100, 100), "transparent PNG load failed");
    auto alpha_render = render(wallpaper, true);
    require(pixel(alpha_render.get(), 50, 50) == 0xff80007f,
            "PNG premultiplied alpha incorrect");

    const auto invalid = temporary.path / "broken.png";
    {
      std::ofstream output(invalid);
      output << "not an image";
    }
    require(!wallpaper.load(invalid, 100, 100), "invalid image accepted");
    auto cleared = render(wallpaper, true);
    require(pixel(cleared.get(), 50, 50) == 0xff0000ff,
            "invalid image retained previous wallpaper");
    require(wallpaper.load(png, 100, 100), "PNG reload failed");
    require(!wallpaper.load(temporary.path / "missing.png", 100, 100),
            "missing image accepted");
    cleared = render(wallpaper, true);
    require(pixel(cleared.get(), 50, 50) == 0xff0000ff,
            "missing image retained previous wallpaper");
    require(!wallpaper.load(png, 0, 100), "invalid target size accepted");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "wallpaper test: " << error.what() << '\n';
    return 1;
  }
}
