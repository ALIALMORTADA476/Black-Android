// Builds the game-ready binary viewmodel mesh `flamethrower.bmsh` from the
// high-poly OBJ+MTL source using color calibration and Quadric Error Metric
// (QEM) edge-collapse decimation.
//
// Usage:
//   build_flamethrower_mesh <input.obj> <input.mtl> <output.bmsh> [target_tris] [tex.ppm]

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

constexpr uint32_t kBmshMagic = 0x48534D42u;  // "BMSH"
constexpr uint32_t kBmshVersion = 1u;

struct BmshHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t vertex_count;
    uint32_t index_count;
    float nozzle_pos[3];
    float pilot_pos[3];
    float bounds_min[3];
    float bounds_max[3];
};

struct BmshVertex {
    float x, y, z;
    float nx, ny, nz;
    uint32_t bgra;
};

struct Material {
    float kd[3] = {0.35f, 0.35f, 0.35f};
    float ks[3] = {0.35f, 0.35f, 0.35f};
    float pm = 0.5f;
    float pr = 0.45f;
};

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
};

double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double LenSq(const Vec3& a) { return Dot(a, a); }
double Len(const Vec3& a) { return std::sqrt(LenSq(a)); }
Vec3 Norm(const Vec3& a) {
    double l = Len(a);
    return (l > 1e-12) ? Vec3{a.x / l, a.y / l, a.z / l} : Vec3{0, 1, 0};
}

// Symmetric 4x4 matrix packed into 10 doubles:
// [q0 q1 q2 q3]
// [   q4 q5 q6]
// [      q7 q8]
// [         q9]
struct SymMat {
    double m[10] = {};
    SymMat& operator+=(const SymMat& o) {
        for (int i = 0; i < 10; i++) m[i] += o.m[i];
        return *this;
    }
    SymMat operator+(const SymMat& o) const {
        SymMat r = *this;
        r += o;
        return r;
    }
};

SymMat PlaneQuadric(double a, double b, double c, double d) {
    SymMat q;
    q.m[0] = a * a; q.m[1] = a * b; q.m[2] = a * c; q.m[3] = a * d;
    q.m[4] = b * b; q.m[5] = b * c; q.m[6] = b * d;
    q.m[7] = c * c; q.m[8] = c * d;
    q.m[9] = d * d;
    return q;
}

double EvalQuadric(const SymMat& q, const Vec3& v) {
    return q.m[0] * v.x * v.x + 2.0 * q.m[1] * v.x * v.y + 2.0 * q.m[2] * v.x * v.z +
           2.0 * q.m[3] * v.x + q.m[4] * v.y * v.y + 2.0 * q.m[5] * v.y * v.z +
           2.0 * q.m[6] * v.y + q.m[7] * v.z * v.z + 2.0 * q.m[8] * v.z + q.m[9];
}

bool SolveSym3(const SymMat& q, Vec3& out) {
    double a00 = q.m[0], a01 = q.m[1], a02 = q.m[2], b0 = -q.m[3];
    double a11 = q.m[4], a12 = q.m[5], b1 = -q.m[6];
    double a22 = q.m[7], b2 = -q.m[8];

    double d0 = a11 * a22 - a12 * a12;
    double d1 = a01 * a22 - a12 * a02;
    double d2 = a01 * a12 - a11 * a02;
    double det = a00 * d0 - a01 * d1 + a02 * d2;
    if (std::fabs(det) < 1e-11) return false;

    double inv = 1.0 / det;
    out.x = (b0 * (a11 * a22 - a12 * a12) - a01 * (b1 * a22 - a12 * b2) +
             a02 * (b1 * a12 - a11 * b2)) *
            inv;
    out.y = (a00 * (b1 * a22 - a12 * b2) - b0 * (a01 * a22 - a12 * a02) +
             a02 * (a01 * b2 - b1 * a02)) *
            inv;
    out.z = (a00 * (a11 * b2 - b1 * a12) - a01 * (a01 * b2 - b1 * a02) +
             b0 * (a01 * a12 - a11 * a02)) *
            inv;
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

std::unordered_map<std::string, Material> LoadMtl(const std::filesystem::path& path) {
    std::unordered_map<std::string, Material> mats;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "ERROR: cannot open MTL\n");
        return mats;
    }
    std::string line, cur;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string tok;
        if (!(ss >> tok)) continue;
        if (tok == "newmtl") {
            ss >> cur;
            mats[cur] = Material{};
        } else if (!cur.empty()) {
            if (tok == "Kd") {
                ss >> mats[cur].kd[0] >> mats[cur].kd[1] >> mats[cur].kd[2];
            } else if (tok == "Ks") {
                ss >> mats[cur].ks[0] >> mats[cur].ks[1] >> mats[cur].ks[2];
            } else if (tok == "Pm") {
                ss >> mats[cur].pm;
            } else if (tok == "Pr") {
                ss >> mats[cur].pr;
            }
        }
    }
    return mats;
}

uint32_t PackBgra(float r, float g, float b) {
    auto u = [](float c) -> uint32_t {
        return uint32_t(std::clamp(int(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f)), 0, 255));
    };
    return (0xFFu << 24) | (u(r) << 16) | (u(g) << 8) | u(b);
}

struct PpmImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgb;
    bool valid() const { return width > 0 && height > 0 && rgb.size() == size_t(width) * size_t(height) * 3u; }
};

bool LoadPpm(const std::filesystem::path& path, PpmImage& out) {
#ifdef _WIN32
    FILE* f = _wfopen(path.c_str(), L"rb");
#else
    FILE* f = std::fopen(path.string().c_str(), "rb");
#endif
    if (!f) return false;
    auto next_tok = [&](std::string& tok) -> bool {
        tok.clear();
        int c;
        while ((c = std::fgetc(f)) != EOF) {
            if (c == '#') {
                while ((c = std::fgetc(f)) != EOF && c != '\n') {}
                continue;
            }
            if (c > ' ') {
                tok.push_back(char(c));
                break;
            }
        }
        if (tok.empty()) return false;
        while ((c = std::fgetc(f)) != EOF && c > ' ') tok.push_back(char(c));
        return true;
    };
    std::string magic, ws, hs, mvs;
    if (!next_tok(magic) || magic != "P6" || !next_tok(ws) || !next_tok(hs) || !next_tok(mvs)) {
        std::fclose(f);
        return false;
    }
    out.width = std::atoi(ws.c_str());
    out.height = std::atoi(hs.c_str());
    const int maxv = std::atoi(mvs.c_str());
    if (out.width <= 0 || out.height <= 0 || maxv <= 0 || maxv > 255) {
        std::fclose(f);
        return false;
    }
    out.rgb.resize(size_t(out.width) * size_t(out.height) * 3u);
    const size_t got = std::fread(out.rgb.data(), 1, out.rgb.size(), f);
    std::fclose(f);
    return got == out.rgb.size();
}

void SamplePpmBilinear(const PpmImage& img, float u, float v, float out_rgb[3]) {
    const float fx = std::clamp(u, 0.0f, 1.0f) * float(img.width - 1);
    const float fy = std::clamp(1.0f - v, 0.0f, 1.0f) * float(img.height - 1);
    const int x0 = std::clamp(int(std::floor(fx)), 0, img.width - 1);
    const int y0 = std::clamp(int(std::floor(fy)), 0, img.height - 1);
    const int x1 = std::min(x0 + 1, img.width - 1);
    const int y1 = std::min(y0 + 1, img.height - 1);
    const float tx = fx - float(x0);
    const float ty = fy - float(y0);
    auto at = [&](int x, int y, int c) -> float {
        return float(img.rgb[(size_t(y) * size_t(img.width) + size_t(x)) * 3u + size_t(c)]) / 255.0f;
    };
    for (int c = 0; c < 3; c++) {
        const float a = at(x0, y0, c) * (1.0f - tx) + at(x1, y0, c) * tx;
        const float b = at(x0, y1, c) * (1.0f - tx) + at(x1, y1, c) * tx;
        out_rgb[c] = a * (1.0f - ty) + b * ty;
    }
}

bool ParseObjFaceVertex(const char*& p, int& out_v, int& out_vt) {
    out_v = -1;
    out_vt = -1;
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p || *p == '\r' || *p == '\n') return false;
    char* end = nullptr;
    long v = std::strtol(p, &end, 10);
    if (end == p) return false;
    out_v = int(v) - 1;
    p = end;
    if (*p == '/') {
        ++p;
        if (*p != '/' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            long vt = std::strtol(p, &end, 10);
            if (end != p) {
                out_vt = int(vt) - 1;
                p = end;
            }
        }
    }
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
    return out_v >= 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::filesystem::path> args;
#ifdef _WIN32
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    for (int i = 0; i < wargc; i++) args.emplace_back(wargv[i]);
    if (wargv) LocalFree(wargv);
#else
    for (int i = 0; i < argc; i++) args.emplace_back(argv[i]);
#endif
    if (args.size() < 4) {
        std::fprintf(stderr, "Usage: %s <obj> <mtl> <out.bmsh> [target_tris] [tex.ppm]\n", argv[0]);
        return 1;
    }
    const std::filesystem::path obj_path = args[1];
    const std::filesystem::path mtl_path = args[2];
    const std::filesystem::path out_path = args[3];
    const size_t target_tris = (args.size() >= 5) ? size_t(std::atoi(args[4].string().c_str())) : 18500u;
    PpmImage tex_img;
    if (args.size() >= 6) {
        if (LoadPpm(args[5], tex_img)) {
            std::printf("Loaded concept texture %dx%d\n", tex_img.width, tex_img.height);
        } else {
            std::fprintf(stderr, "WARN: failed to open texture PPM, falling back to procedural palette\n");
        }
    }

    auto t0 = std::chrono::steady_clock::now();
    auto mats = LoadMtl(mtl_path);
    std::vector<Material> mat_list;
    std::unordered_map<std::string, int> mat_id_map;
    mat_list.reserve(mats.size() + 1);
    mat_list.push_back(Material{});
    for (const auto& kv : mats) {
        mat_id_map[kv.first] = int(mat_list.size());
        mat_list.push_back(kv.second);
    }

#ifdef _WIN32
    FILE* fp = _wfopen(obj_path.c_str(), L"rb");
#else
    FILE* fp = std::fopen(obj_path.c_str(), "rb");
#endif
    if (!fp) {
        std::fprintf(stderr, "ERROR: cannot open OBJ\n");
        return 1;
    }

    struct RawVert {
        Vec3 pos;
        Vec3 obj_pos;
        float r = 0.4f, g = 0.4f, b = 0.4f;
        double u_sum = 0.0, v_sum = 0.0;
        int uv_hits = 0;
        float mat_accum[3] = {0, 0, 0};
        float pm_sum = 0.0f;
        int mat_hits = 0;
        int first_mat = -1;
        bool mat_seam = false;
    };
    struct Tri {
        uint32_t v[3];
        int mat = 0;
        bool alive = true;
    };

    std::vector<RawVert> verts;
    verts.reserve(205000);
    std::vector<std::array<float, 2>> vts;
    vts.reserve(205000);
    std::vector<Tri> tris;
    tris.reserve(390000);

    std::vector<char> buf(4 * 1024 * 1024);
    std::setvbuf(fp, buf.data(), _IOFBF, buf.size());

    char line[1024];
    int cur_mat = 0;
    while (std::fgets(line, sizeof(line), fp)) {
        if (line[0] == 'v' && line[1] == ' ') {
            const char* p = line + 2;
            char* e = nullptr;
            double x = std::strtod(p, &e); p = e;
            double y = std::strtod(p, &e); p = e;
            double z = std::strtod(p, &e); p = e;
            double r = std::strtod(p, &e); p = e;
            double g = std::strtod(p, &e); p = e;
            double b = std::strtod(p, &e);
            RawVert v;
            v.pos = {x, y, z};
            v.obj_pos = {x, y, z};
            if (e != p) {
                v.r = float(r);
                v.g = float(g);
                v.b = float(b);
            }
            verts.push_back(v);
        } else if (line[0] == 'v' && line[1] == 't' && line[2] == ' ') {
            const char* p = line + 3;
            char* e = nullptr;
            float u = float(std::strtod(p, &e)); p = e;
            float v = float(std::strtod(p, &e));
            vts.push_back({u, v});
        } else if (line[0] == 'f' && line[1] == ' ') {
            const char* p = line + 2;
            int i0 = -1, i1 = -1, i2 = -1;
            int vt0 = -1, vt1 = -1, vt2 = -1;
            if (ParseObjFaceVertex(p, i0, vt0) &&
                ParseObjFaceVertex(p, i1, vt1) &&
                ParseObjFaceVertex(p, i2, vt2) &&
                size_t(i0) < verts.size() && size_t(i1) < verts.size() && size_t(i2) < verts.size() &&
                i0 != i1 && i1 != i2 && i0 != i2) {
                Tri t{uint32_t(i0), uint32_t(i1), uint32_t(i2), cur_mat, true};
                tris.push_back(t);
                const int vti[3] = {vt0, vt1, vt2};
                const Material& m = mat_list[size_t(cur_mat)];
                for (int k = 0; k < 3; k++) {
                    RawVert& rv = verts[t.v[k]];
                    rv.mat_accum[0] += m.kd[0];
                    rv.mat_accum[1] += m.kd[1];
                    rv.mat_accum[2] += m.kd[2];
                    rv.pm_sum += m.pm;
                    rv.mat_hits++;
                    if (vti[k] >= 0 && size_t(vti[k]) < vts.size()) {
                        rv.u_sum += vts[size_t(vti[k])][0];
                        rv.v_sum += vts[size_t(vti[k])][1];
                        rv.uv_hits++;
                    }
                    if (rv.first_mat < 0) rv.first_mat = cur_mat;
                    else if (rv.first_mat != cur_mat) rv.mat_seam = true;
                }
            }
        } else if (std::strncmp(line, "usemtl ", 7) == 0) {
            std::string name(line + 7);
            while (!name.empty() && (name.back() == '\r' || name.back() == '\n' || name.back() == ' '))
                name.pop_back();
            auto it = mat_id_map.find(name);
            cur_mat = (it != mat_id_map.end()) ? it->second : 0;
        }
    }
    std::fclose(fp);

    const size_t raw_v_count = verts.size();
    const size_t raw_t_count = tris.size();

    // Scale to meters and orient into left-handed D3D8 viewmodel coordinates:
    // Pivot slightly ahead of pistol grip at raw (-5, 0, 0), proper rotation (det=+1):
    //   x_local = -z_obj * scale
    //   y_local =  y_obj * scale
    //   z_local =  x_obj * scale   (+X_obj muzzle points down +Z_local forward!)
    constexpr double kScale = 0.0076;
    constexpr double kPivotX = -5.0;
    for (auto& v : verts) {
        Vec3 o = v.pos;
        v.pos = {
            -o.z * kScale,
            o.y * kScale,
            (o.x - kPivotX) * kScale
        };
    }

    // Calibrate colors BEFORE decimation using the user's 2K concept sheet UVs (when provided)
    // paired with gentle crevice AO & highlight shoulder compression so white specular peaks never blow out.
    struct Col3 { double r, g, b; };
    std::vector<Col3> vcols(verts.size());
    const bool use_tex = tex_img.valid();
    for (size_t i = 0; i < verts.size(); i++) {
        const auto& rv = verts[i];
        float kd[3] = {rv.r, rv.g, rv.b};
        float pm = 0.5f;
        if (rv.mat_hits > 0) {
            float inv = 1.0f / float(rv.mat_hits);
            kd[0] = rv.mat_accum[0] * inv;
            kd[1] = rv.mat_accum[1] * inv;
            kd[2] = rv.mat_accum[2] * inv;
            pm = rv.pm_sum * inv;
        }
        const float raw[3] = {rv.r, rv.g, rv.b};
        const float lum_raw = 0.299f * raw[0] + 0.587f * raw[1] + 0.114f * raw[2];
        float c[3] = {0.25f, 0.25f, 0.25f};

        // Use flamethrower_weapon.mtl Kd colors as the authoritative palette (preserving
        // heat-blued cyan/blue nozzle, copper/bronze fittings, olive fuel tanks, and dark steel),
        // with subtle crevice AO + gentle metallic shoulder curve so viewmodel lighting stays natural.
        const float kd_lum = 0.299f * kd[0] + 0.587f * kd[1] + 0.114f * kd[2];
        const float ao = 0.84f + 0.18f * std::clamp(lum_raw, 0.0f, 1.0f);
        float tex[3] = {raw[0], raw[1], raw[2]};
        if (use_tex && (rv.uv_hits > 0 || vts.size() == verts.size())) {
            const float u = (rv.uv_hits > 0) ? float(rv.u_sum / double(rv.uv_hits)) : vts[i][0];
            const float v = (rv.uv_hits > 0) ? float(rv.v_sum / double(rv.uv_hits)) : vts[i][1];
            SamplePpmBilinear(tex_img, u, v, tex);
        }
        const float metal_lift = (pm >= 0.70f && kd_lum > 0.36f) ? 1.08f : 1.00f;
        for (int k = 0; k < 3; k++) {
            const float sat_kd = std::max(0.0f, kd_lum + (kd[k] - kd_lum) * 1.25f);
            const float base_col = use_tex ? (0.86f * sat_kd + 0.14f * tex[k])
                                           : (0.90f * sat_kd + 0.10f * raw[k]);
            const float shaded = base_col * ao * metal_lift;
            const float tone = shaded / (1.0f + 0.18f * shaded);
            c[k] = std::clamp(tone, 0.045f, 0.66f);
        }
        vcols[i] = {c[0], c[1], c[2]};
    }

    // Build adjacency and quadrics for QEM decimation
    std::vector<SymMat> Q(verts.size());
    std::vector<std::vector<uint32_t>> v_tris(verts.size());
    for (size_t ti = 0; ti < tris.size(); ti++) {
        const Tri& t = tris[ti];
        for (int k = 0; k < 3; k++) v_tris[t.v[k]].push_back(uint32_t(ti));
        const Vec3& p0 = verts[t.v[0]].pos;
        const Vec3& p1 = verts[t.v[1]].pos;
        const Vec3& p2 = verts[t.v[2]].pos;
        Vec3 n = Cross(p1 - p0, p2 - p0);
        double area2 = Len(n);
        if (area2 < 1e-14) continue;
        Vec3 un = n * (1.0 / area2);
        double d = -Dot(un, p0);
        // Area-weighted plane quadric keeps broad surfaces smooth while holding silhouettes
        SymMat q = PlaneQuadric(un.x, un.y, un.z, d);
        double w = area2 * 1e4;
        for (int m = 0; m < 10; m++) q.m[m] *= w;
        for (int k = 0; k < 3; k++) Q[t.v[k]] += q;
    }

    // Boundary edge preservation: mark open edges (incident to only 1 face)
    std::unordered_map<uint64_t, uint32_t> edge_face_count;
    edge_face_count.reserve(tris.size() * 2);
    auto EdgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
        if (a > b) std::swap(a, b);
        return (uint64_t(a) << 32) | uint64_t(b);
    };
    for (const Tri& t : tris) {
        edge_face_count[EdgeKey(t.v[0], t.v[1])]++;
        edge_face_count[EdgeKey(t.v[1], t.v[2])]++;
        edge_face_count[EdgeKey(t.v[2], t.v[0])]++;
    }
    for (const auto& kv : edge_face_count) {
        if (kv.second == 1u) {
            uint32_t a = uint32_t(kv.first >> 32);
            uint32_t b = uint32_t(kv.first & 0xFFFFFFFFu);
            Vec3 e = verts[b].pos - verts[a].pos;
            double el = Len(e);
            if (el < 1e-10) continue;
            Vec3 bn = Cross(e, Vec3{0, 1, 0});
            if (LenSq(bn) < 1e-12) bn = Cross(e, Vec3{1, 0, 0});
            bn = Norm(bn);
            SymMat bq = PlaneQuadric(bn.x, bn.y, bn.z, -Dot(bn, verts[a].pos));
            for (int m = 0; m < 10; m++) bq.m[m] *= 0.25;
            Q[a] += bq;
            Q[b] += bq;
        }
    }

    std::vector<uint8_t> v_alive(verts.size(), 1);
    std::vector<uint32_t> v_rev(verts.size(), 0);
    std::vector<Vec3> v_pos(verts.size());
    for (size_t i = 0; i < verts.size(); i++) v_pos[i] = verts[i].pos;

    struct Cand {
        double cost;
        uint32_t a, b;
        uint32_t ra, rb;
        Vec3 target;
        bool operator>(const Cand& o) const { return cost > o.cost; }
    };

    auto ComputeEdgeCand = [&](uint32_t a, uint32_t b, Cand& out) {
        if (a > b) std::swap(a, b);
        SymMat qsum = Q[a] + Q[b];
        Vec3 opt;
        bool solved = SolveSym3(qsum, opt);
        double edge_len = Len(v_pos[a] - v_pos[b]);
        // Reject wild extrapolations
        if (!solved || Len(opt - (v_pos[a] + v_pos[b]) * 0.5) > std::max(edge_len * 2.0, 0.015)) {
            Vec3 mid = (v_pos[a] + v_pos[b]) * 0.5;
            double c0 = EvalQuadric(qsum, v_pos[a]);
            double c1 = EvalQuadric(qsum, v_pos[b]);
            double cm = EvalQuadric(qsum, mid);
            if (c0 <= c1 && c0 <= cm) opt = v_pos[a];
            else if (c1 <= cm) opt = v_pos[b];
            else opt = mid;
        }
        double err = std::max(0.0, EvalQuadric(qsum, opt));
        // Add mild regularizer so degenerate zero-error skinny faces don't swallow whole cylinders unevenly
        err += edge_len * edge_len * 0.025;
        // Preserve sharp material/color transitions
        double dr = vcols[a].r - vcols[b].r;
        double dg = vcols[a].g - vcols[b].g;
        double db = vcols[a].b - vcols[b].b;
        double col_diff2 = dr * dr + dg * dg + db * db;
        err += col_diff2 * 0.12;
        if (verts[a].mat_seam != verts[b].mat_seam) err += 0.008;

        out.cost = err;
        out.a = a;
        out.b = b;
        out.ra = v_rev[a];
        out.rb = v_rev[b];
        out.target = opt;
    };

    std::priority_queue<Cand, std::vector<Cand>, std::greater<Cand>> pq;
    for (const auto& kv : edge_face_count) {
        uint32_t a = uint32_t(kv.first >> 32);
        uint32_t b = uint32_t(kv.first & 0xFFFFFFFFu);
        Cand c;
        ComputeEdgeCand(a, b, c);
        pq.push(c);
    }
    edge_face_count.clear();

    size_t live_tris = tris.size();
    std::vector<uint32_t> check_neighbors;
    check_neighbors.reserve(64);
    std::vector<uint32_t> merged_tris;
    merged_tris.reserve(64);

    while (live_tris > target_tris && !pq.empty()) {
        Cand c = pq.top();
        pq.pop();
        if (!v_alive[c.a] || !v_alive[c.b]) continue;
        if (v_rev[c.a] != c.ra || v_rev[c.b] != c.rb) continue;

        // Triangle flip check: ensure neither vertex's surviving non-shared faces flip > ~85 deg
        bool flip = false;
        auto check_vert = [&](uint32_t vid, uint32_t other) {
            for (uint32_t ti : v_tris[vid]) {
                const Tri& t = tris[ti];
                if (!t.alive) continue;
                if (t.v[0] == other || t.v[1] == other || t.v[2] == other) continue;
                Vec3 p[3] = {v_pos[t.v[0]], v_pos[t.v[1]], v_pos[t.v[2]]};
                Vec3 n_old = Cross(p[1] - p[0], p[2] - p[0]);
                for (int k = 0; k < 3; k++) if (t.v[k] == vid) p[k] = c.target;
                Vec3 n_new = Cross(p[1] - p[0], p[2] - p[0]);
                if (Dot(n_old, n_new) <= 1e-18) { flip = true; return; }
            }
        };
        check_vert(c.a, c.b);
        if (!flip) check_vert(c.b, c.a);
        if (flip) continue;

        // Collapse b -> a at c.target
        double wa = 0.5;
        double dta = Len(c.target - v_pos[c.a]);
        double dtb = Len(c.target - v_pos[c.b]);
        if (dta + dtb > 1e-12) wa = dtb / (dta + dtb);
        vcols[c.a].r = vcols[c.a].r * wa + vcols[c.b].r * (1.0 - wa);
        vcols[c.a].g = vcols[c.a].g * wa + vcols[c.b].g * (1.0 - wa);
        vcols[c.a].b = vcols[c.a].b * wa + vcols[c.b].b * (1.0 - wa);

        v_pos[c.a] = c.target;
        Q[c.a] += Q[c.b];
        v_alive[c.b] = 0;
        v_rev[c.a]++;
        v_rev[c.b]++;

        merged_tris.clear();
        for (uint32_t ti : v_tris[c.a]) {
            Tri& t = tris[ti];
            if (!t.alive) continue;
            if (t.v[0] == c.b || t.v[1] == c.b || t.v[2] == c.b) {
                t.alive = false;
                live_tris--;
            } else {
                merged_tris.push_back(ti);
            }
        }
        for (uint32_t ti : v_tris[c.b]) {
            Tri& t = tris[ti];
            if (!t.alive) continue;
            if (t.v[0] == c.a || t.v[1] == c.a || t.v[2] == c.a) {
                t.alive = false;
                live_tris--;
                continue;
            }
            for (int k = 0; k < 3; k++) if (t.v[k] == c.b) t.v[k] = c.a;
            if (t.v[0] == t.v[1] || t.v[1] == t.v[2] || t.v[0] == t.v[2]) {
                t.alive = false;
                live_tris--;
                continue;
            }
            merged_tris.push_back(ti);
        }
        v_tris[c.a].swap(merged_tris);
        std::vector<uint32_t>().swap(v_tris[c.b]);

        // Re-enqueue incident edges of c.a
        check_neighbors.clear();
        for (uint32_t ti : v_tris[c.a]) {
            const Tri& t = tris[ti];
            for (int k = 0; k < 3; k++) {
                uint32_t nv = t.v[k];
                if (nv != c.a && v_alive[nv]) check_neighbors.push_back(nv);
            }
        }
        std::sort(check_neighbors.begin(), check_neighbors.end());
        check_neighbors.erase(std::unique(check_neighbors.begin(), check_neighbors.end()), check_neighbors.end());
        for (uint32_t nv : check_neighbors) {
            Cand nc;
            ComputeEdgeCand(c.a, nv, nc);
            pq.push(nc);
        }
    }

    // Compact live vertices & triangles
    std::vector<int32_t> remap(verts.size(), -1);
    std::vector<uint32_t> final_verts_old;
    final_verts_old.reserve(24000);
    std::vector<uint16_t> final_indices;
    final_indices.reserve(live_tris * 3);

    for (const Tri& t : tris) {
        if (!t.alive) continue;
        for (int k = 0; k < 3; k++) {
            uint32_t ov = t.v[k];
            if (remap[ov] < 0) {
                remap[ov] = int32_t(final_verts_old.size());
                final_verts_old.push_back(ov);
            }
            final_indices.push_back(uint16_t(remap[ov]));
        }
    }

    if (final_verts_old.size() > 65535u) {
        std::fprintf(stderr, "ERROR: too many verts (%zu > 65535)\n", final_verts_old.size());
        return 2;
    }

    // Compute smooth area-weighted vertex normals
    std::vector<Vec3> normals(final_verts_old.size(), {0, 0, 0});
    for (size_t i = 0; i + 2 < final_indices.size(); i += 3) {
        uint16_t i0 = final_indices[i], i1 = final_indices[i + 1], i2 = final_indices[i + 2];
        const Vec3& p0 = v_pos[final_verts_old[i0]];
        const Vec3& p1 = v_pos[final_verts_old[i1]];
        const Vec3& p2 = v_pos[final_verts_old[i2]];
        Vec3 fn = Cross(p1 - p0, p2 - p0);
        normals[i0] = normals[i0] + fn;
        normals[i1] = normals[i1] + fn;
        normals[i2] = normals[i2] + fn;
    }

    BmshHeader hdr{};
    hdr.magic = kBmshMagic;
    hdr.version = kBmshVersion;
    hdr.vertex_count = uint32_t(final_verts_old.size());
    hdr.index_count = uint32_t(final_indices.size());
    hdr.bounds_min[0] = hdr.bounds_min[1] = hdr.bounds_min[2] = 1e9f;
    hdr.bounds_max[0] = hdr.bounds_max[1] = hdr.bounds_max[2] = -1e9f;

    std::vector<BmshVertex> out_verts(final_verts_old.size());
    double avg_r = 0, avg_g = 0, avg_b = 0;
    double nozzle_x = 0, nozzle_y = 0;
    int nozzle_hits = 0;
    double max_z = -1e9;

    for (size_t i = 0; i < final_verts_old.size(); i++) {
        uint32_t ov = final_verts_old[i];
        const Vec3& p = v_pos[ov];
        Vec3 n = Norm(normals[i]);
        BmshVertex& bv = out_verts[i];
        bv.x = float(p.x);
        bv.y = float(p.y);
        bv.z = float(p.z);
        bv.nx = float(n.x);
        bv.ny = float(n.y);
        bv.nz = float(n.z);
        bv.bgra = PackBgra(float(vcols[ov].r), float(vcols[ov].g), float(vcols[ov].b));

        avg_r += vcols[ov].r;
        avg_g += vcols[ov].g;
        avg_b += vcols[ov].b;

        hdr.bounds_min[0] = std::min(hdr.bounds_min[0], bv.x);
        hdr.bounds_min[1] = std::min(hdr.bounds_min[1], bv.y);
        hdr.bounds_min[2] = std::min(hdr.bounds_min[2], bv.z);
        hdr.bounds_max[0] = std::max(hdr.bounds_max[0], bv.x);
        hdr.bounds_max[1] = std::max(hdr.bounds_max[1], bv.y);
        hdr.bounds_max[2] = std::max(hdr.bounds_max[2], bv.z);
        max_z = std::max(max_z, p.z);
    }

    for (size_t i = 0; i < final_verts_old.size(); i++) {
        const BmshVertex& bv = out_verts[i];
        if (bv.z >= float(max_z) - 0.020f) {
            nozzle_x += bv.x;
            nozzle_y += bv.y;
            nozzle_hits++;
        }
    }
    if (nozzle_hits > 0) {
        hdr.nozzle_pos[0] = float(nozzle_x / nozzle_hits);
        hdr.nozzle_pos[1] = float(nozzle_y / nozzle_hits);
        hdr.nozzle_pos[2] = float(max_z) + 0.012f;
    } else {
        hdr.nozzle_pos[0] = 0.0f;
        hdr.nozzle_pos[1] = 0.0f;
        hdr.nozzle_pos[2] = hdr.bounds_max[2];
    }
    hdr.pilot_pos[0] = hdr.nozzle_pos[0];
    hdr.pilot_pos[1] = hdr.nozzle_pos[1] - 0.022f;
    hdr.pilot_pos[2] = hdr.nozzle_pos[2] - 0.014f;

#ifdef _WIN32
    FILE* out = _wfopen(out_path.c_str(), L"wb");
#else
    FILE* out = std::fopen(out_path.c_str(), "wb");
#endif
    if (!out) {
        std::fprintf(stderr, "ERROR: cannot write output bmsh\n");
        return 3;
    }
    std::fwrite(&hdr, sizeof(hdr), 1, out);
    std::fwrite(out_verts.data(), sizeof(BmshVertex), out_verts.size(), out);
    std::fwrite(final_indices.data(), sizeof(uint16_t), final_indices.size(), out);
    std::fclose(out);

    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    size_t bytes = sizeof(hdr) + out_verts.size() * sizeof(BmshVertex) + final_indices.size() * sizeof(uint16_t);
    double inv_n = final_verts_old.empty() ? 1.0 : 1.0 / double(final_verts_old.size());

    std::printf("Built flamethrower.bmsh in %.1f ms\n", ms);
    std::printf("  Raw:   %zu verts, %zu tris\n", raw_v_count, raw_t_count);
    std::printf("  Final: %u verts, %u tris (%zu bytes = %.1f KB)\n",
                hdr.vertex_count, hdr.index_count / 3, bytes, double(bytes) / 1024.0);
    std::printf("  Bounds X:[%.3f, %.3f] Y:[%.3f, %.3f] Z:[%.3f, %.3f]\n",
                hdr.bounds_min[0], hdr.bounds_max[0],
                hdr.bounds_min[1], hdr.bounds_max[1],
                hdr.bounds_min[2], hdr.bounds_max[2]);
    std::printf("  Nozzle: (%.4f, %.4f, %.4f)  Pilot: (%.4f, %.4f, %.4f)\n",
                hdr.nozzle_pos[0], hdr.nozzle_pos[1], hdr.nozzle_pos[2],
                hdr.pilot_pos[0], hdr.pilot_pos[1], hdr.pilot_pos[2]);
    std::printf("  Calibrated avg RGB: (%.3f, %.3f, %.3f)\n",
                avg_r * inv_n, avg_g * inv_n, avg_b * inv_n);
    return 0;
}
