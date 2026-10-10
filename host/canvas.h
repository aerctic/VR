// Tiny CPU 2D rasterizer for the VR loading panel: rectangles, a progress
// bar and text in a built-in 5x7 pixel font. Pixels are RGBA8 in sRGB.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrl {

// 0xRRGGBBAA
using Rgba = uint32_t;

class Canvas {
public:
    Canvas(int w, int h);

    int width() const { return w_; }
    int height() const { return h_; }
    const uint8_t* data() const { return px_.data(); }
    size_t size_bytes() const { return px_.size(); }

    void clear(Rgba c);
    void fill_rect(int x, int y, int w, int h, Rgba c);
    // Rectangle outline `t` pixels thick.
    void frame_rect(int x, int y, int w, int h, int t, Rgba c);
    void progress(int x, int y, int w, int h, float fraction, Rgba fg, Rgba bg);
    // Draws text at pixel scale `scale` (glyph cell = 6*scale x 8*scale).
    // Lowercase is drawn as uppercase. Returns the advance in pixels.
    int text(int x, int y, const std::string& s, int scale, Rgba c);
    static int text_width(const std::string& s, int scale);
    // Text truncated with "..." to fit max_w pixels.
    int text_fit(int x, int y, const std::string& s, int scale, Rgba c, int max_w);

    // For debugging / tests.
    bool save_ppm(const std::string& path) const;

private:
    void blend(int x, int y, Rgba c);

    int w_, h_;
    std::vector<uint8_t> px_;
};

// True if the font has a glyph for c (case-insensitive); others draw as '?'.
bool font_has_glyph(char c);

}  // namespace vrl
