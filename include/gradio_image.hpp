// gradio_image.hpp - optional pixel access for gr::Image values (stb_image / stb_image_write).
//
//   gr::Pixels px = gr::load_pixels(file);      // decode PNG/JPEG/BMP/GIF to RGB(A) bytes
//   ...
//   return gr::to_png(px);                       // FileData ready to return from a handler
#pragma once

#include "gradio.hpp"

#ifndef GRADIO_NO_STB_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "stb_image.h"
#include "stb_image_write.h"

#include <cstdint>

namespace gr {

struct Pixels {
    int width = 0, height = 0, channels = 0;  // channels: 1 gray, 3 RGB, 4 RGBA
    std::vector<uint8_t> data;                // row-major, width * height * channels

    uint8_t* at(int x, int y) { return &data[((size_t)y * width + x) * channels]; }
    const uint8_t* at(int x, int y) const { return &data[((size_t)y * width + x) * channels]; }
};

// channels = 0 keeps the file's own channel count.
inline Pixels load_pixels(const FileData& f, int channels = 3) {
    if (f.empty()) throw Error("no image");
    std::string bytes = f.read();
    int w, h, n;
    unsigned char* p = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()), (int)bytes.size(),
                                             &w, &h, &n, channels);
    if (!p) throw Error(std::string("cannot decode image: ") + stbi_failure_reason());
    Pixels px;
    px.width = w;
    px.height = h;
    px.channels = channels ? channels : n;
    px.data.assign(p, p + (size_t)w * h * px.channels);
    stbi_image_free(p);
    return px;
}

inline FileData to_png(const Pixels& px, const std::string& name = "image.png") {
    std::string out;
    auto sink = [](void* ctx, void* data, int size) {
        static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), (size_t)size);
    };
    if (!stbi_write_png_to_func(sink, &out, px.width, px.height, px.channels, px.data.data(), px.width * px.channels))
        throw Error("cannot encode PNG");
    return FileData::from_bytes(std::move(out), "image/png", name);
}

}  // namespace gr
