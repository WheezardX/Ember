#include "emberworld/heightfield.h"

#include <algorithm>
#include <cmath>

namespace emberworld {

Frame region_frame(const Region& r) {
    const Bounds e = r.extent();
    Frame f;
    f.anchor_x = 0.5 * (e.min_x + e.max_x);
    f.anchor_y = 0.5 * (e.min_y + e.max_y);
    f.anchor_z = r.heightmap.z_min;
    return f;
}

namespace {

// Mean of the valid pixels among the four around corner (ci, cj), in raster pixel indices of
// the corner's lower-right pixel. Fixed summation order -> identical results across tiles.
bool corner_height(const Raster& h, int col, int row, double& out) {
    double sum = 0.0;
    int n = 0;
    const int cols[2] = {col - 1, col};
    const int rows[2] = {row - 1, row};
    for (int ry : rows)
        for (int rx : cols) {
            if (rx < 0 || ry < 0 || rx >= h.width || ry >= h.height) continue;
            const double v = h.at(rx, ry);
            if (h.is_nodata(v) || !std::isfinite(v)) continue;
            sum += v;
            ++n;
        }
    if (n == 0) return false;
    out = sum / n;
    return true;
}

// Water at the corner between pixels (col-1..col, row-1..row): lowest level among them.
bool corner_water(const Raster* w, int col, int row, double& level) {
    if (!w) return false;
    bool any = false;
    for (int ry = row - 1; ry <= row; ++ry)
        for (int rx = col - 1; rx <= col; ++rx) {
            if (rx < 0 || ry < 0 || rx >= w->width || ry >= w->height) continue;
            const double v = w->at(rx, ry);
            if (w->is_nodata(v) || !std::isfinite(v)) continue;
            level = any ? std::min(level, v) : v;
            any = true;
        }
    return any;
}

// Corner height with the water rule applied (see MeshOptions::water).
bool corner_height_w(const Raster& h, const Raster* w, double bed, int col, int row, double& out) {
    double v = 0, lvl = 0;
    const bool land = corner_height(h, col, row, v);
    if (corner_water(w, col, row, lvl)) {
        out = land ? std::min(v, lvl - bed) : lvl - bed;
        return true;
    }
    if (land) out = v;
    return land;
}

}  // namespace

MeshResult build_tile_mesh(const Region& region, const TileEntry& tile, const Raster& height,
                           const Frame& frame, const MeshOptions& opt) {
    MeshResult res;
    const int tp = region.tile_px, ov = region.overlap_px;
    if (ov < 2) {
        res.error = "overlap_px < 2: need an apron ring for seam-exact normals";
        return res;
    }
    if (height.width != tp + 2 * ov || height.height != tp + 2 * ov) {
        res.error = "height raster is " + std::to_string(height.width) + "x" + std::to_string(height.height) +
                    ", expected " + std::to_string(tp + 2 * ov) + " square (tile_px + 2*overlap_px)";
        return res;
    }
    const double px = tile.content.width() / tp;  // metres per pixel at this LOD
    const Bounds ext = region.extent();
    TileMesh& m = res.mesh;
    const int N = tp + 1;
    m.grid_n = N;

    // Corner heights on an extended (N+2)^2 grid: index (i+1, j+1) for corner i, j in -1..tp+1.
    // Corners with no valid neighbouring pixel (outside the region, voids) are invalid: they
    // get no triangles, and for normals they borrow the nearest valid corner's height so the
    // region border does not shade like a cliff.
    const int E = N + 2;
    std::vector<double> z(static_cast<size_t>(E) * E, 0.0);
    std::vector<uint8_t> valid(static_cast<size_t>(E) * E, 0);
    for (int j = -1; j <= tp + 1; ++j)
        for (int i = -1; i <= tp + 1; ++i) {
            double v;
            const size_t k = static_cast<size_t>(j + 1) * E + (i + 1);
            if (corner_height_w(height, opt.water, opt.bed_depth_m, i + ov, j + ov, v)) {
                z[k] = v;
                valid[k] = 1;
            }
        }
    auto V = [&](int i, int j) { return valid[static_cast<size_t>(j + 1) * E + (i + 1)] != 0; };
    auto Zraw = [&](int i, int j) { return z[static_cast<size_t>(j + 1) * E + (i + 1)]; };
    // Height for shading: own value if valid, else the nearest valid 8-neighbour, else z_min.
    auto Z = [&](int i, int j) {
        if (V(i, j)) return Zraw(i, j);
        for (int r = 1; r <= 2; ++r)
            for (int dj = -r; dj <= r; ++dj)
                for (int di = -r; di <= r; ++di) {
                    const int ii = i + di, jj = j + dj;
                    if (ii < -1 || jj < -1 || ii > tp + 1 || jj > tp + 1) continue;
                    if (V(ii, jj)) return Zraw(ii, jj);
                }
        return region.heightmap.z_min;
    };
    for (int j = 0; j <= tp; ++j)
        for (int i = 0; i <= tp; ++i)
            if (!V(i, j)) ++m.nodata_corners;

    m.min_z = 1e300;  // over valid corners only
    m.max_z = -1e300;
    m.positions.reserve(static_cast<size_t>(N) * N);
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            const double wx = tile.content.min_x + i * px;
            const double wy = tile.content.max_y - j * px;
            const double wz = Z(i, j);
            if (V(i, j)) {
                m.min_z = std::min(m.min_z, wz);
                m.max_z = std::max(m.max_z, wz);
                Bounds& vb = m.valid_bounds;
                if (!m.has_valid) {
                    vb = {wx, wy, wx, wy};
                    m.has_valid = true;
                }
                vb.min_x = std::min(vb.min_x, wx);
                vb.max_x = std::max(vb.max_x, wx);
                vb.min_y = std::min(vb.min_y, wy);
                vb.max_y = std::max(vb.max_y, wy);
            }
            m.positions.push_back(frame.to_ue(wx, wy, wz));
            // Gradient in world metres; UE normal has Y = south, so dz/dy_south = -dz/dy_north.
            const double dzdx = (Z(i + 1, j) - Z(i - 1, j)) / (2.0 * px);
            const double dzdy_south = (Z(i, j + 1) - Z(i, j - 1)) / (2.0 * px);
            double nx = -dzdx, ny = -dzdy_south, nz = 1.0;
            const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
            m.normals.push_back({static_cast<float>(nx / len), static_cast<float>(ny / len),
                                 static_cast<float>(nz / len)});
            m.uv0.push_back({static_cast<float>(double(i) / tp), static_cast<float>(double(j) / tp)});
            m.uv1.push_back({static_cast<float>((wx - ext.min_x) / ext.width()),
                             static_cast<float>((ext.max_y - wy) / ext.height())});
        }

    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (opt.flip_winding) std::swap(b, c);
        m.indices.insert(m.indices.end(), {a, b, c});
    };
    for (int j = 0; j < tp; ++j)
        for (int i = 0; i < tp; ++i) {
            if (!V(i, j) || !V(i + 1, j) || !V(i, j + 1) || !V(i + 1, j + 1)) continue;
            const uint32_t a = j * N + i, b = a + 1, c = a + N, d = c + 1;
            // a b      (east ->)
            // c d      (south v)
            // UE front faces: for an up-facing triangle, cross(p1-p0, p2-p0).z < 0 in the UE
            // frame (left-handed, X east / Y south). Verified by render (HCP0 log) + unit test.
            tri(a, c, b);
            tri(b, c, d);
            m.surface_triangles += 2;
        }

    if (opt.skirts) {
        const double depth_m = opt.skirt_depth_m >= 0 ? opt.skirt_depth_m : std::max(4.0 * px, 1.0);
        const float depth_cm = static_cast<float>(depth_m * 100.0);
        // Walk the border; each edge segment gets a quad hanging down, emitted with both
        // windings so it is visible from either side (skirts only show through LOD cracks).
        std::vector<uint32_t> ring;
        for (int i = 0; i < tp; ++i) ring.push_back(i);                       // north, west->east
        for (int j = 0; j < tp; ++j) ring.push_back(j * N + tp);              // east, north->south
        for (int i = tp; i > 0; --i) ring.push_back(tp * N + i);              // south, east->west
        for (int j = tp; j > 0; --j) ring.push_back(j * N);                   // west, south->north
        const uint32_t base = static_cast<uint32_t>(m.positions.size());
        for (uint32_t k = 0; k < ring.size(); ++k) {
            Vec3f p = m.positions[ring[k]];
            p.z -= depth_cm;
            m.positions.push_back(p);
            m.normals.push_back(m.normals[ring[k]]);
            m.uv0.push_back(m.uv0[ring[k]]);
            m.uv1.push_back(m.uv1[ring[k]]);
        }
        const uint32_t R = static_cast<uint32_t>(ring.size());
        auto valid_idx = [&](uint32_t idx) { return V(static_cast<int>(idx % N), static_cast<int>(idx / N)); };
        for (uint32_t k = 0; k < R; ++k) {
            const uint32_t t0 = ring[k], t1 = ring[(k + 1) % R];
            if (!valid_idx(t0) || !valid_idx(t1)) continue;
            const uint32_t b0 = base + k, b1 = base + (k + 1) % R;
            m.indices.insert(m.indices.end(), {t0, b0, t1, t1, b0, b1, t0, t1, b0, t1, b1, b0});
            m.skirt_triangles += 4;
        }
    }
    return res;
}

MeshResult load_tile_mesh(const Region& region, const TileEntry& tile, const Frame& frame,
                          const MeshOptions& opt) {
    TiffResult t = read_tiff(region.path(tile.height_tif));
    if (!t) {
        MeshResult r;
        r.error = t.error.message;
        return r;
    }
    return build_tile_mesh(region, tile, *t.raster, frame, opt);
}

bool sample_height(const Region& region, double wx, double wy, double& out_z) {
    const int lod = region.finest_lod();
    for (const TileEntry* t : region.tiles_at(lod)) {
        if (wx < t->content.min_x || wx > t->content.max_x || wy < t->content.min_y || wy > t->content.max_y)
            continue;
        TiffResult tr = read_tiff(region.path(t->height_tif));
        if (!tr) return false;
        const Raster& h = *tr.raster;
        const double px = t->content.width() / region.tile_px;
        const int i = static_cast<int>(std::lround((wx - t->content.min_x) / px));
        const int j = static_cast<int>(std::lround((t->content.max_y - wy) / px));
        double v;
        if (!corner_height(h, i + region.overlap_px, j + region.overlap_px, v)) v = region.heightmap.z_min;
        out_z = v;
        return true;
    }
    return false;
}

}  // namespace emberworld

namespace emberworld {

bool data_extent(const Region& region, Bounds& out, std::string& error) {
    const int tp = region.tile_px, ov = region.overlap_px;
    bool any = false;
    for (const TileEntry* t : region.tiles_at(region.finest_lod())) {
        TiffResult tr = read_tiff(region.path(t->height_tif));
        if (!tr) {
            error = tr.error.message;
            return false;
        }
        const Raster& h = *tr.raster;
        const double px = t->content.width() / tp;
        for (int j = 0; j <= tp; ++j)
            for (int i = 0; i <= tp; ++i) {
                double v;
                if (!corner_height(h, i + ov, j + ov, v)) continue;
                const double wx = t->content.min_x + i * px, wy = t->content.max_y - j * px;
                if (!any) {
                    out = {wx, wy, wx, wy};
                    any = true;
                }
                out.min_x = std::min(out.min_x, wx);
                out.max_x = std::max(out.max_x, wx);
                out.min_y = std::min(out.min_y, wy);
                out.max_y = std::max(out.max_y, wy);
            }
    }
    if (!any) {
        out = region.extent();
        error = "no valid data in the finest LOD";
        return false;
    }
    return true;
}

}  // namespace emberworld

namespace emberworld {

bool SurfaceSampler::build(const Region& region, const TileEntry& tile, const Raster& height,
                           const Raster* water, double bed_depth_m) {
    const int tp = region.tile_px, ov = region.overlap_px;
    if (height.width != tp + 2 * ov || height.height != tp + 2 * ov) return false;
    content_ = tile.content;
    px_ = tile.content.width() / tp;
    n_ = tp + 1;
    z_.assign(static_cast<size_t>(n_) * n_, 0.0);
    valid_.assign(static_cast<size_t>(n_) * n_, 0);
    for (int j = 0; j < n_; ++j)
        for (int i = 0; i < n_; ++i) {
            double v;
            if (corner_height_w(height, water, bed_depth_m, i + ov, j + ov, v)) {
                z_[static_cast<size_t>(j) * n_ + i] = v;
                valid_[static_cast<size_t>(j) * n_ + i] = 1;
            }
        }
    return true;
}

bool SurfaceSampler::height_at(double wx, double wy, double& out_z) const {
    if (n_ == 0) return false;
    const double gx = (wx - content_.min_x) / px_, gy = (content_.max_y - wy) / px_;
    if (gx < 0 || gy < 0 || gx > n_ - 1 || gy > n_ - 1) return false;
    const int i = std::min(static_cast<int>(gx), n_ - 2), j = std::min(static_cast<int>(gy), n_ - 2);
    const double tx = gx - i, ty = gy - j;
    const size_t a = static_cast<size_t>(j) * n_ + i;
    if (!valid_[a] || !valid_[a + 1] || !valid_[a + n_] || !valid_[a + n_ + 1]) return false;
    const double top = z_[a] + (z_[a + 1] - z_[a]) * tx;
    const double bot = z_[a + n_] + (z_[a + n_ + 1] - z_[a + n_]) * tx;
    out_z = top + (bot - top) * ty;
    return true;
}

bool SurfaceSampler::surface_at(double wx, double wy, double& out_z) const {
    if (n_ == 0) return false;
    const double gx = (wx - content_.min_x) / px_, gy = (content_.max_y - wy) / px_;
    if (gx < 0 || gy < 0 || gx > n_ - 1 || gy > n_ - 1) return false;
    const int i = std::min(static_cast<int>(gx), n_ - 2), j = std::min(static_cast<int>(gy), n_ - 2);
    const double tx = gx - i, ty = gy - j;  // east, south within the quad
    const size_t a = static_cast<size_t>(j) * n_ + i, b = a + 1, c = a + n_, d = c + 1;
    if (!valid_[a] || !valid_[b] || !valid_[c] || !valid_[d]) return false;
    // build_tile_mesh: triangles (a, c, b) and (b, c, d) - the diagonal runs b (NE) to c (SW)
    if (tx + ty <= 1.0)
        out_z = z_[a] + (z_[b] - z_[a]) * tx + (z_[c] - z_[a]) * ty;
    else
        out_z = z_[d] + (z_[c] - z_[d]) * (1.0 - tx) + (z_[b] - z_[d]) * (1.0 - ty);
    return true;
}

}  // namespace emberworld

namespace emberworld {

MeshResult build_water_mesh(const Region& region, const TileEntry& tile, const Raster& level,
                            const Frame& frame, double lift_m) {
    // One connected surface: a (tp+1)^2 corner grid shared by every water cell, each corner at
    // the mean level of the water cells around it (apron cells included, so a neighbouring tile
    // computes the same corner). Per-cell flat quads at their own level left a step and a crack
    // at every level change - the "strips with gaps" on Lake Kachess (8g).
    MeshResult res;
    const int tp = region.tile_px, ov = region.overlap_px;
    if (level.width != tp + 2 * ov || level.height != tp + 2 * ov) {
        res.error = "water raster size does not match the tile grid";
        return res;
    }
    const double px = tile.content.width() / tp;
    auto lv = [&](int i, int j, double& out) {  // cell (i, j) in content coordinates
        const int x = i + ov, y = j + ov;
        if (x < 0 || y < 0 || x >= level.width || y >= level.height) return false;
        const double v = level.at(x, y);
        if (level.is_nodata(v) || !std::isfinite(v)) return false;
        out = v;
        return true;
    };
    const int N = tp + 1;
    std::vector<int64_t> index(static_cast<size_t>(N) * N, -1);
    TileMesh& m = res.mesh;
    m.min_z = 1e300;
    m.max_z = -1e300;
    auto corner = [&](int ci, int cj) -> uint32_t {  // corner at the NW of cell (ci, cj)
        int64_t& idx = index[static_cast<size_t>(cj) * N + ci];
        if (idx >= 0) return static_cast<uint32_t>(idx);
        double sum = 0.0, v = 0.0;
        int n = 0;
        for (int dj = -1; dj <= 0; ++dj)
            for (int di = -1; di <= 0; ++di)
                if (lv(ci + di, cj + dj, v)) {
                    sum += v;
                    ++n;
                }
        const double z = (n ? sum / n : 0.0) + lift_m;
        const double x = tile.content.min_x + ci * px, y = tile.content.max_y - cj * px;
        idx = static_cast<int64_t>(m.positions.size());
        m.positions.push_back(frame.to_ue(x, y, z));
        m.normals.push_back({0.0f, 0.0f, 1.0f});
        m.uv0.push_back({static_cast<float>(x / 100.0), static_cast<float>(-y / 100.0)});
        m.uv1.push_back({0.0f, 0.0f});
        m.min_z = std::min(m.min_z, z);
        m.max_z = std::max(m.max_z, z);
        return static_cast<uint32_t>(idx);
    };
    double v = 0.0;
    for (int j = 0; j < tp; ++j)
        for (int i = 0; i < tp; ++i) {
            if (!lv(i, j, v)) continue;
            const uint32_t a = corner(i, j), b = corner(i + 1, j), c = corner(i, j + 1), d = corner(i + 1, j + 1);
            // same front-face rule as the terrain (see build_tile_mesh)
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
            m.surface_triangles += 2;
        }
    return res;
}

}  // namespace emberworld
