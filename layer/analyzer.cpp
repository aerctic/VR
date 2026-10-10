#include "analyzer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "log.h"

namespace vrl {

namespace {

constexpr VkDeviceSize kStride = 16;            // float4x4 alignment in cbuffers / std140
constexpr VkDeviceSize kChunk = 1 << 20;        // bytes scanned per region lock
constexpr uint64_t kSearchBudget = 48ull << 20; // bytes per search tick
constexpr int64_t kNeighbourhood = 1024;        // view / VP search radius around the projection
constexpr size_t kMaxHitsPerPass = 4096;

bool read_matrix(const uint8_t* base, VkDeviceSize size, int64_t off, float out[16]) {
    if (off < 0 || VkDeviceSize(off) + 64 > size) return false;
    memcpy(out, base + off, 64);
    return true;
}

void rotate_vec(const float q[4], const float v[3], float out[3]) {
    // v' = v + 2w(u x v) + 2(u x (u x v))
    const float u[3] = {q[0], q[1], q[2]}, w = q[3];
    float c1[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    float c2[3] = {u[1] * c1[2] - u[2] * c1[1], u[2] * c1[0] - u[0] * c1[2], u[0] * c1[1] - u[1] * c1[0]};
    for (int i = 0; i < 3; ++i) out[i] = v[i] + 2.0f * w * c1[i] + 2.0f * c2[i];
}

uint64_t loc_key(uint64_t region, VkDeviceSize offset) { return (region << 40) ^ uint64_t(offset); }

}  // namespace

Analyzer::Analyzer() {
    if (const char* w = getenv("VRLAYER_INJECT_WINDOW_KB")) inject_window_ = VkDeviceSize(atoi(w)) << 10;
    camera_msg_.rotation[3] = 1.0f;
    thread_ = std::thread(&Analyzer::thread_main, this);
}

Analyzer::~Analyzer() {
    {
        std::lock_guard<std::mutex> l(state_mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

// ---------------------------------------------------------------------------
// Memory tracking
// ---------------------------------------------------------------------------

void Analyzer::on_allocate(VkDeviceMemory mem, VkDeviceSize size) {
    std::lock_guard<std::mutex> l(regions_mtx_);
    Region& r = regions_[mem];
    r = Region{};
    r.id = next_region_id_++;
    r.alloc_size = size;
}

void Analyzer::on_free(VkDeviceMemory mem) { regions_.erase(mem); }  // region_lock() held by caller

void Analyzer::on_map(VkDeviceMemory mem, VkDeviceSize offset, VkDeviceSize size, void* ptr) {
    std::lock_guard<std::mutex> l(regions_mtx_);
    auto it = regions_.find(mem);
    if (it == regions_.end() || !ptr) return;
    Region& r = it->second;
    r.ptr = static_cast<uint8_t*>(ptr);
    r.size = size == VK_WHOLE_SIZE ? (r.alloc_size > offset ? r.alloc_size - offset : 0) : size;
}

void Analyzer::on_unmap(VkDeviceMemory mem) {  // region_lock() held by caller
    auto it = regions_.find(mem);
    if (it == regions_.end()) return;
    it->second.ptr = nullptr;
    it->second.size = 0;
}

void Analyzer::on_viewport(const VkViewport* v, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (v[i].height < 0.0f && -v[i].height >= 64.0f) viewport_flipped_ = true;
}

// ---------------------------------------------------------------------------
// Background search / tracking
// ---------------------------------------------------------------------------

void Analyzer::thread_main() {
    for (;;) {
        State st;
        {
            std::unique_lock<std::mutex> l(state_mtx_);
            cv_.wait_for(l, std::chrono::milliseconds(50), [&] { return stop_ || kick_; });
            if (stop_) return;
            kick_ = false;
            st = state_;
            if (extent_.width == 0) continue;  // nothing presented yet
        }
        bool restart = false;
        {
            std::lock_guard<std::mutex> l(state_mtx_);
            restart = restart_requested_;
            restart_requested_ = false;
        }
        if (restart) {
            pass_regions_.clear();
            pass_index_ = 0;
            proj_candidates_.clear();
            last_hit_locs_.clear();
            reverify_ = false;
        }
        if (st == State::Searching) {
            search_step();
        } else {
            track();
            if (reverify_) search_step();  // keeps tracking the current camera meanwhile
        }
    }
}

void Analyzer::search_step() {
    float aspect;
    {
        std::lock_guard<std::mutex> l(state_mtx_);
        aspect = float(extent_.width) / float(extent_.height);
    }
    if (pass_regions_.empty()) {
        // Start a new pass over everything that is mapped right now.
        {
            std::lock_guard<std::mutex> l(state_mtx_);
            inline_candidates_.clear();  // only count what this pass sees live
        }
        proj_candidates_.clear();
        pass_index_ = 0;
        pass_offset_ = 0;
        pass_bytes_ = 0;
        pass_total_ = 0;
        std::lock_guard<std::mutex> l(regions_mtx_);
        for (auto& [mem, r] : regions_)
            if (r.ptr && r.size >= 64) {
                pass_regions_.push_back(r.id);
                pass_total_ += r.size;
            }
        if (pass_regions_.empty()) return;
    }

    uint64_t done = 0;
    std::vector<std::pair<Bits, Hit>> found;
    while (done < kSearchBudget && pass_index_ < pass_regions_.size()) {
        std::lock_guard<std::mutex> l(regions_mtx_);
        const Region* r = nullptr;
        for (auto& [mem, reg] : regions_)
            if (reg.id == pass_regions_[pass_index_]) r = &reg;
        if (!r || !r->ptr || pass_offset_ >= r->size) {
            ++pass_index_;
            pass_offset_ = 0;
            continue;
        }
        VkDeviceSize end = std::min(r->size, pass_offset_ + kChunk);
        for (VkDeviceSize off = pass_offset_; off + 64 <= end; off += kStride) {
            const uint8_t* p = r->ptr + off;
            uint32_t last;
            memcpy(&last, p + 60, 4);
            if (last != 0 && last != 0x80000000u) continue;  // m[15] must be +-0
            float m[16];
            memcpy(m, p, 64);
            ProjectionInfo pi;
            if (!detect_projection(m, &pi) || std::fabs(pi.aspect - aspect) > 0.06f * aspect) continue;
            if (found.size() < kMaxHitsPerPass) {
                Bits b;
                memcpy(b.data(), m, 64);
                found.push_back({b, Hit{r->id, off}});
            }
        }
        done += end - pass_offset_;
        pass_bytes_ += end - pass_offset_;
        pass_offset_ = end;
        if (pass_offset_ >= r->size) {
            ++pass_index_;
            pass_offset_ = 0;
        }
    }
    for (auto& [b, h] : found) {
        auto& v = proj_candidates_[b];
        if (v.size() < 512) v.push_back(h);
    }
    {
        std::lock_guard<std::mutex> l(state_mtx_);
        scanned_total_ += done;
        progress_ = pass_total_ ? uint32_t(std::min<uint64_t>(99, pass_bytes_ * 100 / pass_total_)) : 0;
        report_dirty_ = true;
    }
    if (pass_index_ >= pass_regions_.size()) {
        finish_search_pass();
        pass_regions_.clear();
    }
}

static uint64_t neighbourhood_hash(const uint8_t* base, VkDeviceSize size, VkDeviceSize off) {
    // FNV-1a over the constant block around a projection copy: live camera
    // blocks also carry per-frame data (time, other matrices) that changes.
    VkDeviceSize b = off > 512 ? off - 512 : 0, e = std::min(size, off + 576);
    uint64_t h = 1469598103934665603ull;
    for (VkDeviceSize i = b; i < e; ++i) h = (h ^ base[i]) * 1099511628211ull;
    return h;
}

void Analyzer::wait_presents(uint64_t n, int timeout_ms) {
    std::unique_lock<std::mutex> l(state_mtx_);
    const uint64_t target = presents_ + n;
    cv_.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return stop_ || presents_ >= target; });
}

void Analyzer::finish_search_pass() {
    // Candidates: every projection seen in this pass, in memory or inline.
    struct Cand {
        Bits bits;
        size_t count;
        bool inl;
        size_t fresh = 0;
    };
    std::vector<Cand> cands;
    for (auto& [b, hits] : proj_candidates_) cands.push_back({b, hits.size(), false});
    std::map<Bits, int> inline_copy;
    {
        std::lock_guard<std::mutex> l(state_mtx_);
        inline_copy = inline_candidates_;
    }
    for (auto& [b, n] : inline_copy) cands.push_back({b, size_t(n), true});
    if (cands.empty()) {
        std::lock_guard<std::mutex> l(state_mtx_);
        if (state_ == State::Searching)
            snprintf(status_, sizeof(status_), "searching %llu MB: no 3D camera yet (get into gameplay)",
                     static_cast<unsigned long long>(pass_total_ >> 20));
        reverify_ = false;
        last_fresh_ = std::chrono::steady_clock::now();
        report_dirty_ = true;
        return;
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.count > b.count; });
    if (cands.size() > 4) cands.resize(4);

    // Freshness: a menu leaves its camera blocks behind in memory, the live
    // camera keeps writing new copies (ring buffers) or new contents around
    // them. Snapshot, let a few frames render, and compare.
    struct Snap {
        std::set<uint64_t> locs;
        std::map<uint64_t, uint64_t> hashes;
        std::set<uint64_t> regions;
    };
    std::vector<Snap> snaps(cands.size());
    {
        std::lock_guard<std::mutex> l(regions_mtx_);
        std::unordered_map<uint64_t, const Region*> by_id;
        for (auto& [mem, r] : regions_) by_id[r.id] = &r;
        for (size_t i = 0; i < cands.size(); ++i) {
            if (cands[i].inl) continue;
            for (const Hit& h : proj_candidates_[cands[i].bits]) {
                auto it = by_id.find(h.region);
                if (it == by_id.end() || !it->second->ptr) continue;
                uint64_t key = loc_key(h.region, h.offset);
                snaps[i].locs.insert(key);
                snaps[i].regions.insert(h.region);
                if (snaps[i].hashes.size() < 64)
                    snaps[i].hashes[key] = neighbourhood_hash(it->second->ptr, it->second->size, h.offset);
            }
        }
    }
    wait_presents(3, 400);
    std::map<Bits, std::vector<Hit>> rescanned;
    {
        std::lock_guard<std::mutex> l(regions_mtx_);
        std::unordered_map<uint64_t, const Region*> by_id;
        for (auto& [mem, r] : regions_) by_id[r.id] = &r;
        for (size_t i = 0; i < cands.size(); ++i) {
            if (cands[i].inl) {
                cands[i].fresh = cands[i].count;  // seen in command buffers during this pass: live
                continue;
            }
            std::vector<Hit>& now = rescanned[cands[i].bits];
            for (uint64_t id : snaps[i].regions) {
                auto it = by_id.find(id);
                if (it == by_id.end() || !it->second->ptr) continue;
                scan_for_bits(*it->second, cands[i].bits, 0, it->second->size, &now);
            }
            for (const Hit& h : now) {
                uint64_t key = loc_key(h.region, h.offset);
                auto hs = snaps[i].hashes.find(key);
                if (!snaps[i].locs.count(key)) {
                    cands[i].fresh++;  // a new copy was written
                } else if (hs != snaps[i].hashes.end()) {
                    const Region* r = by_id[h.region];
                    if (neighbourhood_hash(r->ptr, r->size, h.offset) != hs->second) cands[i].fresh++;
                }
            }
        }
    }
    size_t pick = 0;
    for (size_t i = 1; i < cands.size(); ++i)
        if (cands[i].fresh > cands[pick].fresh) pick = i;
    if (cands.size() > 1)
        VRL_DBG("layer", "camera candidates: %zu, picked #%zu (%zu copies, %zu fresh)", cands.size(), pick,
                cands[pick].count, cands[pick].fresh);
    const Bits* best = &cands[pick].bits;
    const size_t best_count = cands[pick].count;
    const bool best_inline = cands[pick].inl;
    const size_t best_fresh = cands[pick].fresh;
    if (!best_inline && !rescanned[*best].empty()) proj_candidates_[*best] = rescanned[*best];

    Bits pbits = *best;
    ProjectionInfo pi;
    float pm[16];
    memcpy(pm, pbits.data(), 64);
    detect_projection(pm, &pi);
    const Mat4 P = load_matrix(pm, pi.order);
    std::vector<Hit> hits = best_inline ? std::vector<Hit>{} : proj_candidates_[pbits];
    proj_candidates_.clear();

    // Look for the view matrix (and P*V, V^-1) at a consistent offset.
    using Pair = std::pair<int64_t, int64_t>;  // (view offset, other offset)
    std::map<int64_t, int> view_tally;
    std::map<Pair, int> vp_tally, inv_tally;
    std::set<uint64_t> regions_hit;
    std::unordered_map<uint64_t, VkDeviceSize> cursors;
    {
        std::lock_guard<std::mutex> l(regions_mtx_);
        std::unordered_map<uint64_t, const Region*> by_id;
        for (auto& [mem, r] : regions_) by_id[r.id] = &r;
        size_t checked = 0;
        for (const Hit& h : hits) {
            auto it = by_id.find(h.region);
            if (it == by_id.end() || !it->second->ptr) continue;
            const Region& r = *it->second;
            regions_hit.insert(h.region);
            cursors[h.region] = std::max(cursors[h.region], h.offset);
            if (++checked > 256) continue;
            for (int64_t rel = -kNeighbourhood; rel <= kNeighbourhood; rel += int64_t(kStride)) {
                if (rel > -64 && rel < 64) continue;
                float m[16];
                Mat4 v;
                if (!read_matrix(r.ptr, r.size, int64_t(h.offset) + rel, m) || !detect_rigid(m, pi.order, &v)) continue;
                view_tally[rel]++;
                const Mat4 pv = mat_mul(P, v), inv = rigid_inverse(v);
                for (int64_t rel2 = -kNeighbourhood; rel2 <= kNeighbourhood; rel2 += int64_t(kStride)) {
                    float m2[16];
                    if (rel2 == rel || (rel2 > -64 && rel2 < 64)) continue;
                    if (!read_matrix(r.ptr, r.size, int64_t(h.offset) + rel2, m2)) continue;
                    Mat4 x = load_matrix(m2, pi.order);
                    if (mat_near(x, pv, 2e-3f)) vp_tally[{rel, rel2}]++;
                    if (mat_near(x, inv, 2e-3f)) inv_tally[{rel, rel2}]++;
                }
            }
        }
    }
    auto best_of = [](const std::map<int64_t, int>& t, int64_t* key) {
        int n = 0;
        for (auto& [k, c] : t)
            if (c > n || (c == n && std::llabs(k) < std::llabs(*key))) {
                n = c;
                *key = k;
            }
        return n;
    };
    int64_t vrel = 0, vp_key = 0, inv_key = 0;
    int vn = best_of(view_tally, &vrel);
    // P*V and V^-1 only count when they pair with the chosen view offset.
    std::map<int64_t, int> vp_for_view, inv_for_view;
    for (auto& [k, c] : vp_tally)
        if (k.first == vrel) vp_for_view[k.second] += c;
    for (auto& [k, c] : inv_tally)
        if (k.first == vrel) inv_for_view[k.second] += c;
    int vpn = best_of(vp_for_view, &vp_key);
    int invn = best_of(inv_for_view, &inv_key);

    // Stable identity of this camera, so the compositor notices switches
    // (main menu -> level).
    uint32_t id = 2166136261u;
    for (uint32_t w : pbits) id = (id ^ w) * 16777619u;
    id ^= uint32_t(vrel * 2654435761ll) ^ (best_inline ? 0x80000000u : 0u);
    if (id == 0) id = 1;

    std::lock_guard<std::mutex> l(state_mtx_);
    reverify_ = false;
    last_fresh_ = std::chrono::steady_clock::now();
    if (state_ == State::Tracking && id == camera_id_) {
        // Re-verification found the same camera: keep everything as it is.
        if (!best_inline) {
            hot_regions_.assign(regions_hit.begin(), regions_hit.end());
            for (auto& [r, c] : cursors) cursor_[r] = c;
        }
        return;
    }
    if (have_proj_ && camera_id_ != id) {
        VRL_LOG("layer", "camera changed (%zu fresh copies): %s", best_fresh,
                best_fresh ? "the live camera replaces the old one" : "the old camera disappeared");
        patched_.clear();
        last_view_seen_.clear();
        camera_valid_ = false;
    }
    camera_id_ = id;
    have_proj_ = true;
    proj_bits_ = pbits;
    proj_ = pi;
    proj_inline_ = best_inline;
    if (best_inline) {
        have_view_ = inline_have_view_;
        view_rel_ = inline_view_rel_;
    } else {
        have_view_ = vn > 0 && size_t(vn) * 2 >= std::min<size_t>(hits.size(), 256);
        view_rel_ = vrel;
        have_vp_ = have_view_ && vpn * 2 >= vn;
        vp_rel_ = vp_key;
        have_inv_ = have_view_ && invn * 2 >= vn;
        inv_rel_ = inv_key;
        hot_regions_.assign(regions_hit.begin(), regions_hit.end());
        cursor_ = cursors;
    }
    state_ = State::Tracking;
    frames_without_hits_ = 0;
    snprintf(status_, sizeof(status_), "camera: %.0fx%.0f deg FOV, %s%s", pi.hfov_deg, pi.vfov_deg,
             have_view_ || proj_inline_ ? "view matrix found" : "view matrix not found",
             have_vp_ ? " (+view-proj)" : "");
    report_dirty_ = true;
    VRL_LOG("layer", "camera projection: hfov %.1f vfov %.1f near %g far %g %s-z, %zu copies; view %s (offset %lld)%s%s",
            pi.hfov_deg, pi.vfov_deg, pi.near_d, pi.far_d, pi.reversed ? "reversed" : "standard", best_count,
            have_view_ ? "found" : "not found", static_cast<long long>(view_rel_), have_vp_ ? ", view-proj" : "",
            have_inv_ ? ", inverse view" : "");
}

void Analyzer::scan_for_anchor(const Region& r, VkDeviceSize begin, VkDeviceSize end, std::vector<Hit>* hits) const {
    scan_for_bits(r, proj_bits_, begin, end, hits);
}

void Analyzer::scan_for_bits(const Region& r, const Bits& bits, VkDeviceSize begin, VkDeviceSize end,
                             std::vector<Hit>* hits) {
    uint64_t first;
    memcpy(&first, bits.data(), 8);
    end = std::min(end, r.size);
    begin -= begin % kStride;
    for (VkDeviceSize off = begin; off + 64 <= end; off += kStride) {
        uint64_t v;
        memcpy(&v, r.ptr + off, 8);
        if (v != first || memcmp(r.ptr + off, bits.data(), 64) != 0) continue;
        hits->push_back(Hit{r.id, off});
    }
}

void Analyzer::track() {
    std::vector<uint64_t> hot;
    int64_t vrel;
    bool have_view, inline_cam;
    std::unordered_map<uint64_t, VkDeviceSize> cursors;
    {
        std::lock_guard<std::mutex> l(state_mtx_);
        hot = hot_regions_;
        vrel = view_rel_;
        have_view = have_view_;
        inline_cam = proj_inline_;
        cursors = cursor_;
    }
    if (inline_cam) return;  // tracked in on_inline_data

    std::vector<Hit> hits;
    bool found_camera = false;
    size_t fresh = 0;
    ++track_count_;
    float rot[9], pos[3];
    {
        std::lock_guard<std::mutex> rl(regions_mtx_);
        for (uint64_t id : hot) {
            const Region* r = nullptr;
            for (auto& [mem, reg] : regions_)
                if (reg.id == id) r = &reg;
            if (!r || !r->ptr) continue;
            size_t before = hits.size();
            // Near the last hit most of the time (ring buffers advance slowly);
            // the whole region now and then to recover.
            VkDeviceSize cur = cursors.count(id) ? cursors[id] : 0;
            if (track_count_ % 30 == 0 || r->size <= (8u << 20)) {
                scan_for_anchor(*r, 0, r->size, &hits);
            } else {
                VkDeviceSize begin = cur > (256u << 10) ? cur - (256u << 10) : 0;
                scan_for_anchor(*r, begin, begin + (8u << 20), &hits);
                if (begin + (8u << 20) > r->size) scan_for_anchor(*r, 0, begin + (8u << 20) - r->size, &hits);
            }
            for (size_t i = before; i < hits.size(); ++i) cursors[id] = std::max(cursors[id], hits[i].offset);
        }
        if (have_view) {
            std::lock_guard<std::mutex> l(state_mtx_);
            // Prefer a copy whose view changed since the last scan: older ring
            // buffer copies still hold previous frames.
            const Mat4* pick = nullptr;
            Mat4 chosen;
            std::unordered_map<uint64_t, const Region*> by_id;
            for (auto& [mem, reg] : regions_) by_id[reg.id] = &reg;
            for (const Hit& h : hits) {
                auto rit = by_id.find(h.region);
                const Region* r = rit == by_id.end() ? nullptr : rit->second;
                float m[16];
                if (!r || !read_matrix(r->ptr, r->size, int64_t(h.offset) + vrel, m)) continue;
                uint64_t key = loc_key(h.region, VkDeviceSize(int64_t(h.offset) + vrel));
                Bits cur;
                memcpy(cur.data(), m, 64);
                auto p = patched_.find(key);
                if (p != patched_.end() && p->second.second == cur) memcpy(m, p->second.first.data(), 64);
                Mat4 v;
                if (!detect_rigid(m, proj_.order, &v)) continue;
                Bits orig;
                memcpy(orig.data(), m, 64);
                auto& seen = last_view_seen_[key];
                bool changed = seen != orig;
                if (changed) ++fresh;
                seen = orig;
                if (!pick || changed) {
                    chosen = v;
                    pick = &chosen;
                }
            }
            if (last_view_seen_.size() > 8192) last_view_seen_.clear();
            if (pick) {
                Mat4 c = rigid_inverse(chosen);
                for (int r = 0; r < 3; ++r) {
                    for (int k = 0; k < 3; ++k) rot[r * 3 + k] = c.at(r, k);
                    pos[r] = c.at(r, 3);
                }
                found_camera = true;
            }
        }
    }

    // New copies also count as fresh (ring buffers).
    std::unordered_set<uint64_t> locs;
    for (const Hit& h : hits) {
        uint64_t key = loc_key(h.region, h.offset);
        locs.insert(key);
        if (!last_hit_locs_.count(key)) ++fresh;
    }
    last_hit_locs_ = std::move(locs);
    const auto now = std::chrono::steady_clock::now();
    if (fresh) last_fresh_ = now;
    else if (!reverify_ && now - last_fresh_ > std::chrono::seconds(3)) {
        VRL_DBG("layer", "tracked camera is stale, looking for a live one");
        reverify_ = true;  // keep tracking while a background pass looks for a live camera
    }

    std::lock_guard<std::mutex> l(state_mtx_);
    if (state_ != State::Tracking) return;
    fresh_hits_ = uint32_t(fresh);
    cursor_ = cursors;
    last_hits_ = uint32_t(hits.size());
    if (hits.empty()) {
        if (++frames_without_hits_ > 120) {
            VRL_LOG("layer", "camera lost, searching again");
            state_ = State::Searching;
            have_proj_ = have_view_ = have_vp_ = have_inv_ = false;
            camera_valid_ = false;
            snprintf(status_, sizeof(status_), "camera lost (loading screen or cutscene?), searching again");
            report_dirty_ = true;
        }
        return;
    }
    frames_without_hits_ = 0;
    if (found_camera) {
        memcpy(cam_rot_, rot, sizeof(rot));
        memcpy(cam_pos_, pos, sizeof(pos));
        camera_valid_ = true;
    }
}

// ---------------------------------------------------------------------------
// Inline data + injection
// ---------------------------------------------------------------------------

Mat4 Analyzer::current_delta() const {
    const CameraMsg& c = camera_msg_;
    float t[3] = {c.translation[0], c.translation[1], c.translation[2]};
    if (CameraMode(c.mode) == CameraMode::Alternate && eye_ != 0) {
        const float e[3] = {eye_ == 1 ? -0.5f * c.ipd : 0.5f * c.ipd, 0, 0};
        float off[3];
        rotate_vec(c.rotation, e, off);
        for (int i = 0; i < 3; ++i) t[i] += off[i];
    }
    return camera_delta(camera_basis(proj_, viewport_flipped_), c.rotation, t,
                        c.units_per_metre > 0 ? c.units_per_metre : 1.0f);
}

bool Analyzer::on_inline_data(const void* data, uint32_t size, std::vector<uint8_t>* patched) {
    if (size < 64) return false;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    std::lock_guard<std::mutex> l(state_mtx_);
    if (state_ == State::Searching) {
        if (extent_.width == 0) return false;
        float aspect = float(extent_.width) / float(extent_.height);
        for (uint32_t off = 0; off + 64 <= size; off += 4) {
            float m[16];
            memcpy(m, bytes + off, 64);
            ProjectionInfo pi;
            if (!maybe_projection(m) || !detect_projection(m, &pi) || std::fabs(pi.aspect - aspect) > 0.06f * aspect)
                continue;
            Bits b;
            memcpy(b.data(), m, 64);
            int& n = inline_candidates_[b];
            if (n < 1000000) ++n;
            // Remember where the view sits relative to it in this block.
            for (int64_t rel = -int64_t(off); rel + int64_t(off) + 64 <= int64_t(size); rel += 4) {
                if (rel > -64 && rel < 64) continue;
                float v[16];
                Mat4 vm;
                memcpy(v, bytes + off + rel, 64);
                if (detect_rigid(v, pi.order, &vm)) {
                    inline_view_rel_ = rel;
                    inline_have_view_ = true;
                    break;
                }
            }
        }
        return false;
    }
    if (!proj_inline_ || !have_view_) return false;
    for (uint32_t off = 0; off + 64 <= size; off += 4) {
        if (memcmp(bytes + off, proj_bits_.data(), 64) != 0) continue;
        int64_t vo = int64_t(off) + view_rel_;
        if (vo < 0 || vo + 64 > int64_t(size)) continue;
        float m[16];
        memcpy(m, bytes + vo, 64);
        Mat4 v;
        if (!detect_rigid(m, proj_.order, &v)) continue;
        Mat4 c = rigid_inverse(v);
        for (int r = 0; r < 3; ++r) {
            for (int k = 0; k < 3; ++k) cam_rot_[r * 3 + k] = c.at(r, k);
            cam_pos_[r] = c.at(r, 3);
        }
        camera_valid_ = true;
        last_hits_ = 1;
        frames_without_hits_ = 0;
        if (CameraMode(camera_msg_.mode) == CameraMode::Off) return false;
        patched->assign(bytes, bytes + size);
        float out[16];
        store_matrix(out, inject_view(v, current_delta()), proj_.order);
        memcpy(patched->data() + vo, out, 64);
        patched_this_frame_++;
        frame_seq_ = camera_msg_.seq;
        frame_eye_ = CameraMode(camera_msg_.mode) == CameraMode::Alternate ? eye_ : 0;
        return true;
    }
    return false;
}

void Analyzer::patch_at(uint8_t* base, VkDeviceSize size, VkDeviceSize hit, uint64_t region, const Mat4& d) {
    int64_t vo = int64_t(hit) + view_rel_;
    float m[16];
    if (!read_matrix(base, size, vo, m)) return;
    uint64_t key = loc_key(region, VkDeviceSize(vo));
    Bits cur;
    memcpy(cur.data(), m, 64);
    auto it = patched_.find(key);
    if (it != patched_.end() && it->second.second == cur) return;  // already ours
    Mat4 v;
    if (!detect_rigid(m, proj_.order, &v)) return;
    const Mat4 v2 = inject_view(v, d);
    float out[16];
    store_matrix(out, v2, proj_.order);
    memcpy(base + vo, out, 64);
    Bits written;
    memcpy(written.data(), out, 64);
    if (patched_.size() > 50000) patched_.clear();
    patched_[key] = {cur, written};
    if (have_vp_) {
        float pm[16];
        memcpy(pm, proj_bits_.data(), 64);
        store_matrix(out, mat_mul(load_matrix(pm, proj_.order), v2), proj_.order);
        int64_t o = int64_t(hit) + vp_rel_;
        if (o >= 0 && VkDeviceSize(o) + 64 <= size) memcpy(base + o, out, 64);
    }
    if (have_inv_) {
        store_matrix(out, rigid_inverse(v2), proj_.order);
        int64_t o = int64_t(hit) + inv_rel_;
        if (o >= 0 && VkDeviceSize(o) + 64 <= size) memcpy(base + o, out, 64);
    }
    patched_this_frame_++;
    frame_seq_ = camera_msg_.seq;
    frame_eye_ = CameraMode(camera_msg_.mode) == CameraMode::Alternate ? eye_ : 0;
}

void Analyzer::before_submit() {
    {
        std::lock_guard<std::mutex> l(state_mtx_);
        if (state_ != State::Tracking || !have_view_ || proj_inline_ ||
            CameraMode(camera_msg_.mode) == CameraMode::Off)
            return;
    }
    std::lock_guard<std::mutex> rl(regions_mtx_);
    std::lock_guard<std::mutex> l(state_mtx_);
    const Mat4 d = current_delta();
    for (uint64_t id : hot_regions_) {
        Region* r = nullptr;
        for (auto& [mem, reg] : regions_)
            if (reg.id == id) r = &reg;
        if (!r || !r->ptr) continue;
        // Ring buffers advance from the last place the camera was written.
        VkDeviceSize cur = cursor_[id];
        VkDeviceSize begin = cur > (64u << 10) ? cur - (64u << 10) : 0;
        std::vector<Hit> hits;
        scan_for_anchor(*r, begin, begin + inject_window_, &hits);
        if (begin + inject_window_ > r->size && begin > 0)  // wrapped around
            scan_for_anchor(*r, 0, std::min(begin, begin + inject_window_ - r->size), &hits);
        for (const Hit& h : hits) {
            patch_at(r->ptr, r->size, h.offset, id, d);
            if (h.offset > cursor_[id] || cursor_[id] - h.offset > r->size / 2) cursor_[id] = h.offset;
        }
    }
}

// ---------------------------------------------------------------------------
// Per-frame bookkeeping
// ---------------------------------------------------------------------------

bool Analyzer::on_present(VkExtent2D extent, bool depth_selected) {
    std::lock_guard<std::mutex> l(state_mtx_);
    if (extent.width != extent_.width || extent.height != extent_.height) report_dirty_ = true;
    if (depth_selected != depth_selected_) report_dirty_ = true;
    extent_ = extent;
    depth_selected_ = depth_selected;
    if (patched_this_frame_ != patched_last_frame_) report_dirty_ = true;
    patched_last_frame_ = patched_this_frame_;
    patched_this_frame_ = 0;
    // frame_seq_/frame_eye_ describe the frame being presented now.
    ++presents_;
    presented_seq_ = frame_seq_;
    presented_eye_ = frame_eye_;
    frame_seq_ = frame_eye_ = 0;
    eye_ = CameraMode(camera_msg_.mode) == CameraMode::Alternate ? (eye_ == 1 ? 2 : 1) : 0;
    kick_ = true;
    cv_.notify_all();
    bool dirty = report_dirty_;
    return dirty;
}

void Analyzer::restart() {
    std::lock_guard<std::mutex> l(state_mtx_);
    state_ = State::Searching;
    have_proj_ = have_view_ = have_vp_ = have_inv_ = proj_inline_ = false;
    camera_valid_ = false;
    inline_candidates_.clear();
    patched_.clear();
    last_view_seen_.clear();
    hot_regions_.clear();
    viewport_flipped_ = false;
    inline_have_view_ = false;
    restart_requested_ = true;
    scanned_total_ = 0;
    progress_ = 0;
    snprintf(status_, sizeof(status_), "searching again");
    report_dirty_ = true;
    VRL_LOG("layer", "camera analysis restarted");
}

void Analyzer::set_camera(const CameraMsg& m) {
    std::lock_guard<std::mutex> l(state_mtx_);
    if (m.mode != camera_msg_.mode) report_dirty_ = true;
    camera_msg_ = m;
}

AnalysisMsg Analyzer::report() {
    uint64_t mapped = 0;
    {
        std::lock_guard<std::mutex> rl(regions_mtx_);
        for (auto& [mem, r] : regions_) mapped += r.ptr ? r.size : 0;
    }
    std::lock_guard<std::mutex> l(state_mtx_);
    auto m = make_msg<AnalysisMsg>(MsgType::Analysis);
    if (have_proj_) m.flags |= kAnalysisProjection;
    if (have_proj_ && have_view_) m.flags |= kAnalysisView;
    if (have_vp_) m.flags |= kAnalysisViewProj;
    if (have_inv_) m.flags |= kAnalysisInverseView;
    if (proj_inline_) m.flags |= kAnalysisPushConstants;
    if (viewport_flipped_) m.flags |= kAnalysisViewportFlip;
    if (depth_selected_) m.flags |= kAnalysisDepth;
    if (patched_last_frame_ > 0) m.flags |= kAnalysisInjecting;
    m.matrix_order = uint32_t(proj_.order);
    m.hfov_deg = proj_.hfov_deg;
    m.vfov_deg = proj_.vfov_deg;
    m.aspect = proj_.aspect;
    m.near_d = proj_.near_d;
    m.far_d = proj_.far_d;
    m.depth_a = proj_.depth_a;
    m.depth_b = proj_.depth_b;
    CameraBasis b = camera_basis(proj_, viewport_flipped_);
    m.basis_up = b.up;
    m.basis_fwd = b.fwd;
    m.reversed = proj_.reversed;
    m.scanned_mb = uint32_t(scanned_total_ >> 20);
    m.mapped_mb = uint32_t(mapped >> 20);
    m.proj_hits = last_hits_;
    m.patched = patched_last_frame_;
    m.progress = state_ == State::Tracking ? 100u : progress_;
    m.camera_id = have_proj_ ? camera_id_ : 0;
    m.fresh = fresh_hits_;
    snprintf(m.status, sizeof(m.status), "%s", status_);
    report_dirty_ = false;
    return m;
}

bool Analyzer::camera(float rot[9], float pos[3]) {
    std::lock_guard<std::mutex> l(state_mtx_);
    if (!camera_valid_) return false;
    memcpy(rot, cam_rot_, sizeof(cam_rot_));
    memcpy(pos, cam_pos_, sizeof(cam_pos_));
    return true;
}

void Analyzer::frame_injection(uint32_t* seq, uint32_t* eye) {
    std::lock_guard<std::mutex> l(state_mtx_);
    *seq = presented_seq_;
    *eye = presented_eye_;
}

}  // namespace vrl
