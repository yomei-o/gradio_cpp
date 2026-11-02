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

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>

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

// Value of an AnnotatedImage output: the image plus labelled boxes / masks.
// Each annotation is sent as its own RGBA overlay PNG (Gradio's format:
// {"image": FileData, "annotations": [{"image": FileData, "label": str}]}).
class AnnotatedImageValue {
public:
    explicit AnnotatedImageValue(const Pixels& image) : image_(to_png(image)), w_(image.width), h_(image.height) {}

    // Box in pixel coordinates (x1, y1) - (x2, y2): outlined, lightly filled.
    AnnotatedImageValue& box(int x1, int y1, int x2, int y2, const std::string& label, int thickness = 0) {
        if (thickness <= 0) thickness = std::max(2, std::min(w_, h_) / 200);
        x1 = std::clamp(x1, 0, w_ - 1), x2 = std::clamp(x2, 0, w_ - 1);
        y1 = std::clamp(y1, 0, h_ - 1), y2 = std::clamp(y2, 0, h_ - 1);
        if (x1 > x2) std::swap(x1, x2);
        if (y1 > y2) std::swap(y1, y2);
        Pixels m = blank();
        auto c = color(label);
        for (int y = y1; y <= y2; ++y)
            for (int x = x1; x <= x2; ++x) {
                bool edge = x - x1 < thickness || x2 - x < thickness || y - y1 < thickness || y2 - y < thickness;
                uint8_t* p = m.at(x, y);
                p[0] = c[0], p[1] = c[1], p[2] = c[2], p[3] = edge ? 255 : 60;
            }
        items_.push_back({to_png(m, "box.png"), label});
        return *this;
    }

    // Mask: same size as the image, 1 channel, non-zero = inside.
    AnnotatedImageValue& mask(const Pixels& mask, const std::string& label) {
        if (mask.width != w_ || mask.height != h_ || mask.channels != 1) throw Error("mask must be 1-channel, image size");
        Pixels m = blank();
        auto c = color(label);
        for (size_t i = 0; i < (size_t)w_ * h_; ++i)
            if (mask.data[i]) {
                uint8_t* p = &m.data[i * 4];
                p[0] = c[0], p[1] = c[1], p[2] = c[2], p[3] = 120;
            }
        items_.push_back({to_png(m, "mask.png"), label});
        return *this;
    }

    friend void to_json(json& j, const AnnotatedImageValue& v) {
        json a = json::array();
        for (auto& [f, l] : v.items_) a.push_back({{"image", f}, {"label", l}});
        j = {{"image", v.image_}, {"annotations", a}};
    }

private:
    Pixels blank() const { return Pixels{w_, h_, 4, std::vector<uint8_t>((size_t)w_ * h_ * 4, 0)}; }
    std::array<uint8_t, 3> color(const std::string& label) {
        static const std::array<std::array<uint8_t, 3>, 10> pal = {{{255, 99, 71}, {30, 144, 255}, {50, 205, 50},
                                                                    {255, 165, 0}, {186, 85, 211}, {0, 206, 209},
                                                                    {255, 20, 147}, {154, 205, 50}, {255, 215, 0},
                                                                    {100, 149, 237}}};
        auto it = colors_.find(label);
        if (it == colors_.end()) it = colors_.emplace(label, pal[colors_.size() % pal.size()]).first;
        return it->second;
    }

    FileData image_;
    int w_, h_;
    std::vector<std::pair<FileData, std::string>> items_;
    std::map<std::string, std::array<uint8_t, 3>> colors_;
};

}  // namespace gr
