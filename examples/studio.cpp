// ImageEditor, Model3D and a ScatterPlot.
#include "gradio_image.hpp"

#include <cmath>
#include <sstream>

// A torus as Wavefront OBJ text.
static std::string torus_obj(int seg, double R, double r) {
    std::ostringstream o;
    const double pi = 3.14159265358979323846;
    for (int i = 0; i < seg; ++i)
        for (int j = 0; j < seg / 2; ++j) {
            double u = 2 * pi * i / seg, v = 2 * pi * j / (seg / 2);
            o << "v " << (R + r * std::cos(v)) * std::cos(u) << " " << r * std::sin(v) << " "
              << (R + r * std::cos(v)) * std::sin(u) << "\n";
        }
    int n = seg / 2;
    for (int i = 0; i < seg; ++i)
        for (int j = 0; j < n; ++j) {
            int a = i * n + j + 1, b = ((i + 1) % seg) * n + j + 1, c = ((i + 1) % seg) * n + (j + 1) % n + 1,
                d = i * n + (j + 1) % n + 1;
            o << "f " << a << " " << b << " " << c << " " << d << "\n";
        }
    return o.str();
}

int main() {
    gr::Blocks demo("Studio");
    demo.add(gr::Markdown("# Studio"));
    demo.tabs([&] {
        demo.tab("Draw", [&] {
            int ed = demo.add(gr::ImageEditor("draw on the image"));
            int go = demo.add(gr::Button("Extract drawing", "primary"));
            int mask = demo.add(gr::Image("mask of the strokes"));
            int info = demo.add(gr::Textbox("painted"));
            demo.click(go,
                       [](const gr::EditorValue& v) {
                           if (v.layers.empty()) throw gr::Error("nothing drawn");
                           gr::Pixels layer = gr::load_pixels(v.layers[0], 4);
                           gr::Pixels m{layer.width, layer.height, 1, std::vector<uint8_t>((size_t)layer.width * layer.height)};
                           size_t n = 0;
                           for (size_t i = 0; i < m.data.size(); ++i)
                               if (layer.data[i * 4 + 3] > 0) m.data[i] = 255, ++n;
                           std::string pct = std::to_string(100.0 * n / m.data.size());
                           return std::make_tuple(gr::to_png(m, "mask.png"),
                                                  std::to_string(n) + " pixels (" + pct.substr(0, pct.find('.') + 2) + "%)");
                       },
                       {ed}, {mask, info}, "extract");
        });
        demo.tab("3D", [&] {
            int seg = demo.add(gr::Slider("segments", 6, 48, 24, 2));
            int make = demo.add(gr::Button("Make torus", "primary"));
            int view = demo.add(gr::Model3D("torus.obj (drag to rotate)"));
            demo.click(make,
                       [](double s) { return gr::FileData::from_bytes(torus_obj((int)s, 1.0, 0.35), "model/obj", "torus.obj"); },
                       {seg}, {view}, "torus");
        });
        demo.tab("Plot", [&] {
            int n = demo.add(gr::Slider("points", 10, 300, 100));
            int go = demo.add(gr::Button("Scatter", "primary"));
            int plot = demo.add(gr::ScatterPlot("spiral", "x", "y", "arm"));
            demo.click(go,
                       [](double count) {
                           gr::DataFrame df{{"x", "y", "arm"}, {}};
                           for (int i = 0; i < (int)count; ++i) {
                               double t = i * 0.15, arm = i % 2;
                               double a = t + arm * 3.14159265;
                               df.data.push_back({t * std::cos(a), t * std::sin(a), arm ? "B" : "A"});
                           }
                           return df;
                       },
                       {n}, {plot}, "scatter");
        });
    });
    demo.launch();
}
