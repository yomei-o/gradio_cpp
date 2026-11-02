// Image in (upload or webcam) -> AnnotatedImage with boxes, plus a histogram LinePlot and a BarPlot.
// The "detector" just marks the brightest cells of a grid: swap in a real model's boxes.
#include "gradio_image.hpp"

#include <algorithm>

int main() {
    gr::Interface demo(
        [](const gr::FileData& img, double grid, double keep) {
            gr::Pixels px = gr::load_pixels(img, 3);
            int g = std::max(2, (int)grid);
            struct Cell { int x1, y1, x2, y2; double lum; };
            std::vector<Cell> cells;
            std::vector<double> hist(32, 0);
            for (int gy = 0; gy < g; ++gy)
                for (int gx = 0; gx < g; ++gx) {
                    Cell c{gx * px.width / g, gy * px.height / g, (gx + 1) * px.width / g - 1, (gy + 1) * px.height / g - 1, 0};
                    double sum = 0;
                    for (int y = c.y1; y <= c.y2; ++y)
                        for (int x = c.x1; x <= c.x2; ++x) {
                            const uint8_t* p = px.at(x, y);
                            double l = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
                            sum += l;
                            hist[(int)l / 8] += 1;
                        }
                    c.lum = sum / ((double)(c.x2 - c.x1 + 1) * (c.y2 - c.y1 + 1));
                    cells.push_back(c);
                }
            std::sort(cells.begin(), cells.end(), [](auto& a, auto& b) { return a.lum > b.lum; });
            gr::AnnotatedImageValue ann(px);
            int k = std::min((int)keep, (int)cells.size());
            for (int i = 0; i < k; ++i)
                ann.box(cells[i].x1, cells[i].y1, cells[i].x2, cells[i].y2, i < k / 2 ? "brightest" : "bright");
            ann.box(cells.back().x1, cells.back().y1, cells.back().x2, cells.back().y2, "darkest");

            gr::DataFrame h{{"luminance", "pixels"}, {}};
            for (int i = 0; i < 32; ++i) h.data.push_back({i * 8 + 4, hist[i]});
            gr::DataFrame top{{"cell", "luminance"}, {}};
            for (int i = 0; i < k; ++i) top.data.push_back({"#" + std::to_string(i + 1), cells[i].lum});
            return std::make_tuple(ann, h, top);
        },
        {gr::Image("image"), gr::Slider("grid", 2, 12, 6), gr::Slider("boxes", 1, 12, 4)},
        {gr::AnnotatedImage("bright cells"), gr::LinePlot("histogram", "luminance", "pixels"),
         gr::BarPlot("top cells", "cell", "luminance")},
        "Vision demo", "Upload an image or use the **Webcam** tab; hover the legend to highlight a label.");
    demo.launch();
}
