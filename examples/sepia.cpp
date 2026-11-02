// Image in, image out: the sepia filter from Gradio's docs.
#include "gradio_image.hpp"

#include <algorithm>

int main() {
    gr::Interface demo(
        [](const gr::FileData& img, double strength, const std::string& format) {
            gr::Pixels px = gr::load_pixels(img, 3);
            double s = strength / 100.0;
            for (int y = 0; y < px.height; ++y)
                for (int x = 0; x < px.width; ++x) {
                    uint8_t* p = px.at(x, y);
                    double r = p[0], g = p[1], b = p[2];
                    double sr = 0.393 * r + 0.769 * g + 0.189 * b;
                    double sg = 0.349 * r + 0.686 * g + 0.168 * b;
                    double sb = 0.272 * r + 0.534 * g + 0.131 * b;
                    p[0] = (uint8_t)std::min(255.0, r + (sr - r) * s);
                    p[1] = (uint8_t)std::min(255.0, g + (sg - g) * s);
                    p[2] = (uint8_t)std::min(255.0, b + (sb - b) * s);
                }
            gr::FileData out = format == "jpeg" ? gr::to_jpeg(px, 90, "sepia.jpg") : gr::to_png(px, "sepia.png");
            return std::make_tuple(out,
                                   std::to_string(px.width) + " x " + std::to_string(px.height));
        },
        {gr::Image("input"), gr::Slider("strength (%)", 0, 100, 100), gr::Radio("output format", {"png", "jpeg"}, "png")},
        {gr::Image("sepia"), gr::Textbox("size")},
        "Sepia");
    demo.launch();
}
