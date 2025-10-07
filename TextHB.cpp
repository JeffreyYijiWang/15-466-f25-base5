//ChatGPT used to create this file
//reference from https://github.com/jialand/TheMuteLift/blob/main/TextHB.cpp
#include "TextHB.hpp"

#include "gl_compile_program.hpp"
#include "data_path.hpp"
#include "GL.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

#include <hb.h>
#include <hb-ft.h>

#include <cassert>
#include <cstring>
#include <cstddef>

static const char* VS = R"GLSL(
#version 330
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
uniform vec2 uScreen; // in pixels
void main(){
    vUV = aUV;
    // pixel -> NDC. Note: NDC y goes up; here (0,0) = top-left corner
    float x = (aPos.x / uScreen.x) * 2.0 - 1.0;
    float y = 1.0 - (aPos.y / uScreen.y) * 2.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
}
)GLSL";

static const char* FS = R"GLSL(
#version 330
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex; // R8, red channel as alpha
uniform vec3 uColor;
void main(){
    float a = texture(uTex, vUV).r;
    FragColor = vec4(uColor, a);
}
)GLSL";

bool TextHB::ensure_program(){
    if(prog) return true;
    prog = gl_compile_program(VS, FS);
    if(!prog) return false;
    uScreen_loc = glGetUniformLocation(prog, "uScreen");
    uColor_loc  = glGetUniformLocation(prog, "uColor");
    uTex_loc    = glGetUniformLocation(prog, "uTex");

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    // aPos(0), aUV(1)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float)*4, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float)*4, (void*)(sizeof(float)*2));
    glBindVertexArray(0);
    return true;
}

bool TextHB::init(const std::string& font_path, int pixel_size){
    px_size = pixel_size;

    if(!ensure_program()) return false;

    if(FT_Init_FreeType(&ft)) return false;
    if(FT_New_Face(ft, font_path.c_str(), 0, &face)) return false;
    if(FT_Set_Pixel_Sizes(face, 0, px_size)) return false;

    hb_font = hb_ft_font_create_referenced(face);
    hb_ft_font_set_funcs(hb_font); // use FT-provided metric functions
    return true;
}

void TextHB::shutdown(){
    for(auto &kv : cache){
        if(kv.second.tex) glDeleteTextures(1, &kv.second.tex);
    }
    cache.clear();
    if(hb_font){ hb_font_destroy(hb_font); hb_font = nullptr; }
    if(face){ FT_Done_Face(face); face = nullptr; }
    if(ft){ FT_Done_FreeType(ft); ft = nullptr; }
    if(vbo){ glDeleteBuffers(1, &vbo); vbo = 0; }
    if(vao){ glDeleteVertexArrays(1, &vao); vao = 0; }
    if(prog){ glDeleteProgram(prog); prog = 0; }
}

void TextHB::begin(glm::uvec2 screen_px){
    screen = screen_px;
    glUseProgram(prog);
    glUniform2f(uScreen_loc, float(screen.x), float(screen.y));
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(uTex_loc, 0);
    // Enable alpha blending for text rendering
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void TextHB::end(){
    glDisable(GL_BLEND);
    glUseProgram(0);
}

bool TextHB::load_glyph(unsigned glyph_index, GlyphTex& out){
    auto it = cache.find(glyph_index);
    if(it != cache.end()){ out = it->second; return true; }

    if(FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER)) return false;
    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap& bm = slot->bitmap;

    GlyphTex gt;
    gt.w = int(bm.width);
    gt.h = int(bm.rows);
    gt.bearingX = slot->bitmap_left;
    gt.bearingY = slot->bitmap_top;
    gt.advance = slot->advance.x / 64.0f;

    glGenTextures(1, &gt.tex);
    glBindTexture(GL_TEXTURE_2D, gt.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, gt.w, gt.h, 0, GL_RED, GL_UNSIGNED_BYTE, bm.buffer);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    cache[glyph_index] = gt;
    out = gt;
    return true;
}

// --- helpers: shape and collect per-cluster advances ---
struct HBCluster {
    uint32_t byte_start = 0;  // start byte index in original UTF-8
    uint32_t byte_end   = 0;  // end byte index (exclusive), set later
    float    advance_px = 0.0f;
    bool     is_space   = false; // basic: spaces/tabs (ASCII); CJK no-space lines will be false
};

// Return clusters ordered by input order; advances already merged per cluster.
static void hb_shape_to_clusters(hb_font_t* hb_font, std::string_view s,
                                 std::vector<HBCluster>& out) {
    out.clear();
    if (s.empty()) return;

    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, s.data(), (int)s.size(), 0, (int)s.size());
    hb_buffer_guess_segment_properties(buf);

    hb_feature_t features[] = {
        {HB_TAG('k','e','r','n'), 1, 0, ~0u},
        {HB_TAG('l','i','g','a'), 1, 0, ~0u},
    };
    hb_shape(hb_font, buf, features, sizeof(features)/sizeof(features[0]));

    unsigned int count = 0;
    auto* infos = hb_buffer_get_glyph_infos(buf, &count);
    auto* pos   = hb_buffer_get_glyph_positions(buf, &count);

    // Merge glyphs with the same cluster (cluster = byte index in input by default)
    std::vector<HBCluster> clusters;
    clusters.reserve(count ? count : 1);

    auto flush_cluster = [&](uint32_t byte_start, float adv){
        HBCluster c;
        c.byte_start = byte_start;
        c.advance_px = adv;
        // basic space detection (ASCII space/tab); for non-ASCII we keep false
        if (byte_start < s.size()) {
            unsigned char ch = (unsigned char)s[byte_start];
            c.is_space = (ch == ' ' || ch == '\t');
        }
        clusters.push_back(c);
    };

    if (count > 0) {
        uint32_t cur = infos[0].cluster;
        float cur_adv = 0.0f;
        for (unsigned i=0;i<count;i++) {
            uint32_t cl = infos[i].cluster;
            if (cl != cur) {
                flush_cluster(cur, cur_adv);
                cur = cl;
                cur_adv = 0.0f;
            }
            cur_adv += pos[i].x_advance / 64.0f;
        }
        flush_cluster(cur, cur_adv);
    }

    // set byte_end of each cluster by looking at next start; last ends at s.size()
    for (size_t i=0;i<clusters.size();++i) {
        uint32_t end = (i+1<clusters.size()) ? clusters[i+1].byte_start : (uint32_t)s.size();
        clusters[i].byte_end = end;
    }

    out.swap(clusters);
    hb_buffer_destroy(buf);
}

float TextHB::measure_text(std::string_view s) const {
    std::vector<HBCluster> cl;
    hb_shape_to_clusters(hb_font, s, cl);
    float w = 0.0f;
    for (auto const& c : cl) w += c.advance_px;
    return w;
}

float TextHB::measure_text(std::u8string_view s8) const {
    auto p = reinterpret_cast<const char*>(s8.data());
    return measure_text(std::string_view(p, s8.size()));
}

// Greedy wrap by clusters; prefer breaking at spaces; if none, break at last fitting cluster.
void TextHB::wrap_text(std::string_view s, float max_width_px, std::vector<std::string>& out_lines) const {
    out_lines.clear();
    if (s.empty()) return;

    // Split explicit paragraphs by '\n' first; each paragraph wrapped independently:
    size_t para_start = 0;
    while (para_start <= s.size()) {
        size_t nl = s.find('\n', para_start);
        std::string_view para = (nl==std::string::npos)
            ? s.substr(para_start)
            : s.substr(para_start, nl - para_start);

        std::vector<HBCluster> cl;
        hb_shape_to_clusters(hb_font, para, cl);

        size_t n = cl.size();
        size_t line_begin = 0;
        float  line_w = 0.0f;
        std::ptrdiff_t last_space = -1;

        auto make_line = [&](size_t a, size_t b){ // [a,b] inclusive clusters -> string
            if (a > b || b >= cl.size()) return std::string();
            uint32_t start = cl[a].byte_start;
            uint32_t end   = cl[b].byte_end;

            // trim leading/trailing spaces around the break:
            while (a <= b && cl[a].is_space) { start = cl[a].byte_end; ++a; }
            while (b >= a && cl[b].is_space) { end   = cl[b].byte_start; --b; }
            if (start > end) start = end;
            return std::string(para.substr(start, end - start));
        };

        for (size_t i=0;i<n;i++){
            if (cl[i].is_space) last_space = static_cast<std::ptrdiff_t>(i);

            float new_w = line_w + cl[i].advance_px;
            if (new_w <= max_width_px) {
                line_w = new_w;
                continue;
            }

            // overflow -> decide break point
            size_t break_at;
            if (last_space >= static_cast<std::ptrdiff_t>(line_begin)) {
                break_at = static_cast<size_t>(last_space); // break at last space
            } else {
                // no spaces in this segment: break before current cluster (unless first)
                break_at = (i == line_begin) ? i : (i - 1);
            }

            // emit line
            {
                std::string line = make_line(line_begin, break_at);
                out_lines.push_back(std::move(line));
            }

            // next line starts after break_at; skip leading spaces
            line_begin = break_at + 1;
            while (line_begin < n && cl[line_begin].is_space) ++line_begin;

            // reset accumulators
            i = line_begin ? (line_begin - 1) : 0; // loop will ++i
            line_w = 0.0f;
            last_space = -1;
        }

        // tail line
        if (line_begin < n) {
            std::string line = make_line(line_begin, n - 1);
            out_lines.push_back(std::move(line));
        } else if (n == 0) {
            out_lines.emplace_back(); // empty paragraph
        }

        if (nl == std::string::npos) break;
        para_start = nl + 1;

        // keep paragraph break as an empty line if there was a blank line in input
        if (para_start < s.size() && s[para_start] == '\n') {
            out_lines.emplace_back();
        }
    }
}

void TextHB::wrap_text(std::u8string_view s8, float max_width_px, std::vector<std::string>& out_lines) const {
    auto p = reinterpret_cast<const char*>(s8.data());
    wrap_text(std::string_view(p, s8.size()), max_width_px, out_lines);
}


void TextHB::draw_text(const std::string& utf8, float x, float y_baseline, const glm::vec3& rgb){
    // 1) HarfBuzz shaping
    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, utf8.c_str(), int(utf8.size()), 0, int(utf8.size()));
    hb_buffer_guess_segment_properties(buf); // auto-detect script/direction/language
    // Optional: enable common OpenType features (ligatures, kerning)
    hb_feature_t features[] = {
        {HB_TAG('k','e','r','n'), 1, 0, ~0u},
        {HB_TAG('l','i','g','a'), 1, 0, ~0u},
    };
    hb_shape(hb_font, buf, features, sizeof(features)/sizeof(features[0]));

    unsigned int count = 0;
    auto* infos = hb_buffer_get_glyph_infos(buf, &count);
    auto* pos   = hb_buffer_get_glyph_positions(buf, &count);

    glUseProgram(prog);
    glUniform3f(uColor_loc, rgb.r, rgb.g, rgb.b);
    glBindVertexArray(vao);

    float pen_x = x, pen_y = y_baseline;

    // std::vector<float> verts; // [x,y,u,v] * 6
    // verts.reserve(6*4);

    for(unsigned i=0;i<count;i++){
        unsigned glyph_index = infos[i].codepoint;
        float x_advance = pos[i].x_advance / 64.0f;
        float y_advance = pos[i].y_advance / 64.0f;
        float x_offset  = pos[i].x_offset  / 64.0f;
        float y_offset  = pos[i].y_offset  / 64.0f;

        GlyphTex g;
        if(!load_glyph(glyph_index, g)) continue;

        float x0 = pen_x + x_offset + float(g.bearingX);
        float y0 = pen_y - y_offset - float(g.bearingY);
        float x1 = x0 + float(g.w);
        float y1 = y0 + float(g.h);

        // two triangles = 6 vertices
        float quad[] = {
            x0,y0, 0.0f,0.0f,
            x1,y0, 1.0f,0.0f,
            x1,y1, 1.0f,1.0f,

            x0,y0, 0.0f,0.0f,
            x1,y1, 1.0f,1.0f,
            x0,y1, 0.0f,1.0f
        };

        glBindTexture(GL_TEXTURE_2D, g.tex);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        pen_x += x_advance;
        pen_y += y_advance;
    }

    hb_buffer_destroy(buf);
    glBindVertexArray(0);
}

void TextHB::draw_text(std::string_view s, float x, float y, const glm::vec3& rgb) {
    draw_text(std::string(s), x, y, rgb);
}

void TextHB::draw_text(std::u8string_view s8, float x, float y, const glm::vec3& rgb) {
    const char* p = reinterpret_cast<const char*>(s8.data());
    draw_text(std::string(p, p + s8.size()), x, y, rgb);
}

void TextHB::draw_text(const char* s, float x, float y, const glm::vec3& rgb) {
    draw_text(std::string_view(s), x, y, rgb);
}