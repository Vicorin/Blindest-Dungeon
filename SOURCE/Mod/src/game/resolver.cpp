// game/resolver.cpp -- finds every game address and moved struct layout on a build the mod has never
// seen.

#include "game/resolver.h"
#include "game/x64decode.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

static const char* const kPlanLines[] = {
#include "game/locators.inc"
};
static const int kPlanCount = (int)(sizeof kPlanLines / sizeof kPlanLines[0]);

static const char* const kCritical[] = {
    "RESOLVER_RVA", "FOCUS_ID_RVA", "VEC_BEGIN_RVA", "VEC_END_RVA", "TBW_VFTABLE_RVA",
};

static const uintptr_t AX_LAY_REFUSED = 0x7FF00000;

static uintptr_t axRefusedStub() { return 0; }
alignas(16) static uint8_t g_zeroPage[0x10000];
static const uintptr_t kZeroInset = 0x1000;

struct Sec { std::string name; uint32_t va, vsz, rsz; };

struct Img {
    const uint8_t* m = nullptr;
    uint64_t base = 0;
    std::vector<Sec> secs;
    Sec text{}, rdata{};
    std::vector<std::pair<uint32_t, uint32_t>> pd;
    std::vector<std::pair<uint32_t, uint32_t>> refs, calls;
    std::vector<uint32_t> head, pos;                          // the 4-gram index of .text
    std::unordered_map<std::string, std::vector<uint32_t>> rttiNames;
    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> colsOfTd;  // td -> (col, offset)
    std::unordered_map<uint32_t, std::vector<uint32_t>> vtablesOfCol;

    uint32_t u32(uint32_t rva) const { uint32_t v; std::memcpy(&v, m + rva, 4); return v; }
    uint64_t u64(uint32_t rva) const { uint64_t v; std::memcpy(&v, m + rva, 8); return v; }
};

struct DI { AxIns i; uint32_t rva; const char* s; int slen; };

static uint32_t hash4(const uint8_t* p) {
    uint32_t v; std::memcpy(&v, p, 4);
    return (v * 2654435761u) >> 12;
}

static bool parseImage(Img& I, const uint8_t* mem, uint64_t base) {
    I.m = mem; I.base = base;
    if (mem[0] != 'M' || mem[1] != 'Z') return false;
    uint32_t e; std::memcpy(&e, mem + 0x3c, 4);
    uint16_t nsec, optsz; std::memcpy(&nsec, mem + e + 6, 2); std::memcpy(&optsz, mem + e + 20, 2);
    uint32_t off = e + 24 + optsz;
    Sec pdata{};
    for (int k = 0; k < nsec; ++k, off += 40) {
        Sec s;
        char nm[9] = {}; std::memcpy(nm, mem + off, 8); s.name = nm;
        std::memcpy(&s.vsz, mem + off + 8, 4); std::memcpy(&s.va, mem + off + 12, 4); std::memcpy(&s.rsz, mem + off + 16, 4);
        I.secs.push_back(s);
        if (s.name == ".text") I.text = s;
        else if (s.name == ".rdata") I.rdata = s;
        else if (s.name == ".pdata") pdata = s;
    }
    if (!I.text.rsz || !I.rdata.rsz || !pdata.vsz) return false;
    for (uint32_t o = 0; o + 12 <= pdata.vsz; o += 12) {
        uint32_t b = I.u32(pdata.va + o), en = I.u32(pdata.va + o + 4);
        if (b || en) I.pd.push_back({b, en});
    }
    std::sort(I.pd.begin(), I.pd.end());
    return !I.pd.empty();
}

static const std::pair<uint32_t, uint32_t>* enclosing(const Img& I, uint32_t rva) {
    size_t lo = 0, hi = I.pd.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (I.pd[mid].second <= rva) lo = mid + 1; else hi = mid;
    }
    return (lo < I.pd.size() && I.pd[lo].first <= rva && rva < I.pd[lo].second) ? &I.pd[lo] : nullptr;
}

static void decodeChunk(const Img& I, uint32_t b, uint32_t e, std::vector<DI>& out, bool wantStrings) {
    out.clear();
    uint32_t a = b;
    while (a < e) {
        DI d{}; d.rva = a;
        if (!axDecode(I.m + a, e - a, d.i)) break;
        if (wantStrings && d.i.hasMem && d.i.base == AX_REG_RIP) {
            uint32_t t = (uint32_t)(a + d.i.len + d.i.disp);
            if (t >= I.rdata.va && t < I.rdata.va + I.rdata.rsz) {
                const char* s = (const char*)I.m + t;
                int lim = (int)std::min<uint32_t>(80, I.rdata.va + I.rdata.rsz - t), n = 0;
                while (n < lim && s[n] && (unsigned char)s[n] >= 32 && (unsigned char)s[n] < 127) ++n;
                if (n >= 4 && n < lim && !s[n]) { d.s = s; d.slen = n; }
            }
        }
        a += d.i.len;
        out.push_back(d);
    }
}

static void buildIndexes(Img& I) {
    std::vector<DI> tmp;
    for (auto& c : I.pd) {
        if (c.first < I.text.va || c.first >= I.text.va + I.text.rsz) continue;
        uint32_t a = c.first;
        AxIns x;
        while (a < c.second && axDecode(I.m + a, c.second - a, x)) {
            if (x.cls == AXI_CALL && x.rel32) I.calls.push_back({(uint32_t)(a + x.len + x.rel), a});
            else if (x.hasMem && x.base == AX_REG_RIP) I.refs.push_back({(uint32_t)(a + x.len + x.disp), a});
            a += x.len;
        }
    }
    std::sort(I.refs.begin(), I.refs.end());
    std::sort(I.calls.begin(), I.calls.end());

    const uint8_t* t = I.m + I.text.va; uint32_t n = I.text.rsz;
    I.head.assign((1u << 20) + 1, 0);
    for (uint32_t p = 0; p + 4 <= n; ++p) I.head[hash4(t + p) + 1]++;
    for (uint32_t k = 0; k < (1u << 20); ++k) I.head[k + 1] += I.head[k];
    I.pos.resize(n >= 4 ? n - 3 : 0);
    std::vector<uint32_t> fill(I.head.begin(), I.head.end() - 1);
    for (uint32_t p = 0; p + 4 <= n; ++p) I.pos[fill[hash4(t + p)]++] = p;

    for (auto& s : I.secs) {
        if (s.name == ".text" || !s.rsz) continue;
        const uint8_t* d = I.m + s.va;
        for (uint32_t p = 0; p + 4 < s.rsz; ++p) {
            if (d[p] == '.' && d[p + 1] == '?' && d[p + 2] == 'A' && (p == 0 || d[p - 1] == 0)) {
                uint32_t q = p; while (q < s.rsz && d[q]) ++q;
                if (q < s.rsz) I.rttiNames[std::string((const char*)d + p, q - p)].push_back(s.va + p);
            }
        }
    }
    const uint8_t* rd = I.m + I.rdata.va; uint32_t rn = I.rdata.rsz;
    for (uint32_t p = 0; p + 0x18 <= rn; p += 4) {
        uint32_t sig, self; std::memcpy(&sig, rd + p, 4); std::memcpy(&self, rd + p + 0x14, 4);
        if (sig == 1 && self == I.rdata.va + p) {
            uint32_t off, td; std::memcpy(&off, rd + p + 4, 4); std::memcpy(&td, rd + p + 0xc, 4);
            I.colsOfTd[td].push_back({I.rdata.va + p, off});
        }
    }
    for (uint32_t q = 0; q + 8 <= rn; q += 8) {
        uint64_t v; std::memcpy(&v, rd + q, 8);
        if (v < I.base || v - I.base >= 0xffffffffull) continue;
        uint32_t col = (uint32_t)(v - I.base);
        if (col >= I.rdata.va && col < I.rdata.va + rn && ((col - I.rdata.va) & 3) == 0) {
            uint32_t sig, self; std::memcpy(&sig, I.m + col, 4); std::memcpy(&self, I.m + col + 0x14, 4);
            if (sig == 1 && self == col) I.vtablesOfCol[col].push_back(I.rdata.va + q + 8);
        }
    }
}

// ---- patterns ----
struct Pat { std::vector<uint8_t> b, lit; };

static int hexv(char c) { return c <= '9' ? c - '0' : c - 'A' + 10; }
static void parsePat(const std::string& s, Pat& p) {
    p.b.clear(); p.lit.clear();
    for (size_t k = 0; k + 1 < s.size(); k += 2) {
        if (s[k] == '?') { p.b.push_back(0); p.lit.push_back(0); }
        else { p.b.push_back((uint8_t)(hexv(s[k]) * 16 + hexv(s[k + 1]))); p.lit.push_back(1); }
    }
}

static bool matchAt(const uint8_t* t, const Pat& p) {
    for (size_t k = 0; k < p.b.size(); ++k) if (p.lit[k] && t[k] != p.b[k]) return false;
    return true;
}

// Text offsets of the matches, ascending, stopping at `limit`.
static void findPat(const Img& I, const Pat& p, std::vector<uint32_t>& hits, size_t limit) {
    hits.clear();
    const uint8_t* t = I.m + I.text.va; uint32_t n = I.text.rsz, len = (uint32_t)p.b.size();
    if (!len || len > n) return;
    int w = -1;
    for (uint32_t k = 0; k + 4 <= len; ++k)
        if (p.lit[k] && p.lit[k + 1] && p.lit[k + 2] && p.lit[k + 3]) { w = (int)k; break; }
    if (w < 0) {
        for (uint32_t q = 0; q + len <= n && hits.size() < limit; ++q) if (matchAt(t + q, p)) hits.push_back(q);
        return;
    }
    uint32_t h = hash4(p.b.data() + w);
    for (uint32_t k = I.head[h]; k < I.head[h + 1] && hits.size() < limit; ++k) {
        uint32_t q = I.pos[k];
        if (q < (uint32_t)w || q - w + len > n) continue;
        if (matchAt(t + q - w, p)) hits.push_back(q - w);
    }
}

static int64_t findUnique(const Img& I, const Pat& p) {
    std::vector<uint32_t> h;
    findPat(I, p, h, 2);
    return h.size() == 1 ? (int64_t)h[0] : -1;
}

static void stringRvas(const Img& I, const std::string& s, std::vector<uint32_t>& out) {
    out.clear();
    const uint8_t* d = I.m + I.rdata.va; uint32_t n = I.rdata.rsz, len = (uint32_t)s.size();
    if (!len || len + 2 > n) return;
    const char first = s[0];
    for (uint32_t p = 1; p + len < n; ) {
        const void* q = std::memchr(d + p, first, n - len - p);
        if (!q) break;
        p = (uint32_t)((const uint8_t*)q - d);
        if (d[p - 1] == 0 && d[p + len] == 0 && std::memcmp(d + p, s.data(), len) == 0) out.push_back(I.rdata.va + p);
        ++p;
    }
}

template <class V> static std::pair<typename V::const_iterator, typename V::const_iterator> rangeOf(const V& v, uint32_t t) {
    auto lo = std::lower_bound(v.begin(), v.end(), std::make_pair(t, (uint32_t)0));
    auto hi = std::lower_bound(lo, v.end(), std::make_pair(t + 1, (uint32_t)0));
    return {lo, hi};
}

static void vtablesByRtti(const Img& I, const std::string& name, uint32_t offset, std::vector<uint32_t>& out) {
    out.clear();
    auto it = I.rttiNames.find(name);
    if (it == I.rttiNames.end()) return;
    for (uint32_t r : it->second) {
        auto c = I.colsOfTd.find(r - 0x10);
        if (c == I.colsOfTd.end()) continue;
        for (auto& col : c->second) {
            if (col.second != offset) continue;
            auto v = I.vtablesOfCol.find(col.first);
            if (v != I.vtablesOfCol.end()) out.insert(out.end(), v->second.begin(), v->second.end());
        }
    }
}

static const std::pair<uint32_t, uint32_t>* fnByString(const Img& I, const std::string& s, uint32_t* refInsn = nullptr) {
    std::vector<uint32_t> r;
    stringRvas(I, s, r);
    if (r.size() != 1) return nullptr;
    auto rg = rangeOf(I.refs, r[0]);
    if (rg.second - rg.first != 1) return nullptr;
    if (refInsn) *refInsn = rg.first->second;
    return enclosing(I, rg.first->second);
}

// ---- results ----
struct AnchorOut { uint32_t steam; uint32_t rva; bool ok; char kind; std::string name; };
struct LayOut { std::string name; uint32_t ref; uint32_t val; bool ok; };

struct Results { std::vector<AnchorOut> anchors; std::vector<LayOut> lay; };
static Results& R() { static Results r; return r; }
static AxResolverStats g_stats;

static std::vector<std::string> split(const char* line) {
    std::vector<std::string> f; const char* a = line;
    for (const char* p = line;; ++p) {
        if (*p == '\t' || !*p) { f.emplace_back(a, p - a); if (!*p) break; a = p + 1; }
    }
    return f;
}
static uint32_t hx(const std::string& s) { return (uint32_t)std::strtoul(s.c_str(), nullptr, 16); }
static int dc(const std::string& s) { return std::atoi(s.c_str()); }

// One address being resolved, line by line.
struct Cur {
    bool open = false;
    uint32_t steam = 0; char kind = 'd'; std::string name;
    std::vector<uint32_t> ans;
    // caller set
    bool cs = false; int nCallers = 0; std::vector<std::set<uint32_t>> callerSets;
    // twin
    std::vector<uint32_t> cands; bool twin = false; std::map<int, std::set<uint32_t>> sib;
};

static void callTargets(const Img& I, const std::pair<uint32_t, uint32_t>& fn, std::set<uint32_t>& out) {
    uint32_t a = fn.first; AxIns x;
    while (a < fn.second && axDecode(I.m + a, fn.second - a, x)) {
        if (x.cls == AXI_CALL && x.rel32) out.insert((uint32_t)(a + x.len + x.rel));
        a += x.len;
    }
}

static void closeAnchor(const Img& I, Cur& c) {
    if (!c.open) return;
    if (c.cs && c.callerSets.size() >= 2) {
        std::set<uint32_t> common = c.callerSets[0];
        for (size_t k = 1; k < c.callerSets.size(); ++k) {
            std::set<uint32_t> nx;
            std::set_intersection(common.begin(), common.end(), c.callerSets[k].begin(), c.callerSets[k].end(), std::inserter(nx, nx.begin()));
            common.swap(nx);
        }
        int lo = std::max(2, (c.nCallers + 1) / 2), hi = c.nCallers * 2;
        std::vector<uint32_t> narrowed;
        for (uint32_t t : common) {
            std::set<uint32_t> chunks;
            auto rg = rangeOf(I.calls, t);
            for (auto it = rg.first; it != rg.second; ++it) { auto f = enclosing(I, it->second); if (f) chunks.insert(f->first); }
            if ((int)chunks.size() >= lo && (int)chunks.size() <= hi) narrowed.push_back(t);
        }
        if (narrowed.size() == 1) c.ans.push_back(narrowed[0]);
    }
    if (c.twin) {
        std::set<uint32_t> picks;
        for (auto& kv : c.sib) if (kv.second.size() == 1) picks.insert((uint32_t)(*kv.second.begin() - kv.first));
        std::vector<uint32_t> chosen;
        for (uint32_t v : c.cands) if (picks.count(v)) chosen.push_back(v);
        std::sort(chosen.begin(), chosen.end()); chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
        if (chosen.size() == 1) c.ans.push_back(chosen[0]);
    }
    AnchorOut o; o.steam = c.steam; o.kind = c.kind; o.name = c.name; o.ok = false; o.rva = 0;
    if (!c.ans.empty()) {
        bool same = true;
        for (uint32_t v : c.ans) if (v != c.ans[0]) same = false;
        if (same) { o.ok = true; o.rva = c.ans[0]; }
    }
    R().anchors.push_back(o);
    c = Cur();
}

static void addRel(const Img& I, const std::vector<std::string>& f, std::vector<uint32_t>& ans, Pat& p, int fI) {
    // (isize, field offset, pattern): the rel32 / RIP target of the unique match
    parsePat(f[fI + 2], p);
    int64_t q = findUnique(I, p);
    if (q < 0) return;
    int32_t d; std::memcpy(&d, I.m + I.text.va + q + dc(f[fI + 1]), 4);
    ans.push_back((uint32_t)(I.text.va + q + dc(f[fI]) + d));
}

// ---- layouts: keytest3.py, line for line ----
struct Frame {
    std::string name; uint32_t size = 0;
    std::vector<std::string> wk, cp, cb;
    std::string stPrefix; bool st = false, stCtor = false, stSeg = false;
    std::vector<std::pair<uint32_t, std::string>> rk;
    std::vector<std::pair<uint32_t, int64_t>> smp;   // (ref, target), sorted
    bool built = false;
};

typedef std::map<std::string, std::set<int64_t>> KeyMap;
typedef std::map<std::string, std::set<std::pair<int, int64_t>>> BaseMap;

static bool objmem(const AxIns& i) { return i.hasMem && i.base != AX_REG_RIP && i.base != AX_REG_NONE && i.disp > 0; }
static bool isStack(int r) { return r == AX_REG_RSP || r == AX_REG_RBP; }
static bool isCmp(const AxIns& i) { return i.cls == AXI_CMP || i.cls == AXI_TEST; }

static void keymapBased(const std::vector<DI>& ins, BaseMap& out) {
    const int FWD = 10;
    for (int idx = 0; idx < (int)ins.size(); ++idx) {
        const DI& k = ins[idx];
        if ((k.i.cls != AXI_LEA && k.i.cls != AXI_MOV) || !k.s) continue;
        bool have = false; int reg = 0; int64_t d = 0;
        for (int q = idx - 1; q > std::max(idx - 8, -1); --q) {
            const DI& j = ins[q];
            if (j.i.cls == AXI_CALL || j.s) break;
            if (j.i.op0Mem) continue;
            if (j.i.op1Mem && objmem(j.i) && !isStack(j.i.base)) { have = true; reg = j.i.base; d = j.i.disp; break; }
        }
        if (!have) {
            bool seenCall = false;
            for (int q = idx + 1; q < std::min(idx + FWD, (int)ins.size()); ++q) {
                const DI& j = ins[q];
                if (j.i.cls == AXI_CALL) { seenCall = true; continue; }
                if (j.s) break;
                if (!seenCall) continue;
                if (j.i.op0Mem && objmem(j.i) && !isStack(j.i.base) && !isCmp(j.i)) { have = true; reg = j.i.base; d = j.i.disp; break; }
            }
        }
        if (have) out[std::string(k.s, k.slen)].insert({reg, d & 0xffffffffLL});
    }
}

static void segmap(const std::vector<DI>& ins, int64_t B, KeyMap& out) {
    std::vector<int> marks;
    for (int k = 0; k < (int)ins.size(); ++k) if (ins[k].s) marks.push_back(k);
    for (size_t n = 0; n < marks.size(); ++n) {
        int idx = marks[n], end = n + 1 < marks.size() ? marks[n + 1] : (int)ins.size();
        std::set<int64_t> st;
        for (int q = idx + 1; q < end; ++q) {
            const AxIns& j = ins[q].i;
            if (isCmp(j)) continue;
            if (j.op0Mem && j.base == AX_REG_RBP && j.disp >= B) st.insert(j.disp - B);
        }
        if (st.size() == 1) out[std::string(ins[idx].s, ins[idx].slen)].insert(*st.begin());
    }
}

static void crossSegmap(const std::vector<DI>& ins, KeyMap& out) {
    std::vector<int> marks;
    for (int k = 0; k < (int)ins.size(); ++k) if (ins[k].s) marks.push_back(k);
    for (size_t n = 0; n < marks.size(); ++n) {
        int idx = marks[n], end = n + 1 < marks.size() ? marks[n + 1] : (int)ins.size();
        std::string s(ins[idx].s, ins[idx].slen);
        for (int q = idx + 1; q < end; ++q) {
            const AxIns& j = ins[q].i;
            if (isCmp(j) || (j.op0Mem && j.base != AX_REG_RBP)) continue;
            if (j.op0Mem) out[s].insert(j.disp);
            else if (j.cls == AXI_LEA && j.op1Mem && j.base == AX_REG_RBP && q + 1 < end && ins[q + 1].i.cls == AXI_CALL) out[s].insert(j.disp);
        }
    }
}

static void stackBase(const std::vector<DI>& ins, const std::string& prefix, bool ctor, std::set<int64_t>& found) {
    for (int idx = 0; idx < (int)ins.size(); ++idx) {
        if (!ins[idx].s || ins[idx].slen < (int)prefix.size() || std::memcmp(ins[idx].s, prefix.data(), prefix.size()) != 0) continue;
        for (int n = idx + 1; n < std::min(idx + 7, (int)ins.size()); ++n) {
            const AxIns& j = ins[n].i;
            if (j.cls == AXI_LEA && j.op1Mem && j.base == AX_REG_RBP) {
                if (ctor) { if (j.reg == AX_REG_RCX && n + 1 < (int)ins.size() && ins[n + 1].i.cls == AXI_CALL) found.insert(j.disp); }
                else found.insert(j.disp);
                break;
            }
        }
    }
}

static void witnesses(const Img& I, const std::vector<std::string>& keys, std::vector<std::pair<uint32_t, uint32_t>>& out) {
    std::set<std::pair<uint32_t, uint32_t>> fs; std::vector<uint32_t> r;
    for (auto& k : keys) {
        stringRvas(I, k, r);
        for (uint32_t t : r) {
            auto rg = rangeOf(I.refs, t);
            for (auto it = rg.first; it != rg.second; ++it) { auto f = enclosing(I, it->second); if (f) fs.insert(*f); }
        }
    }
    out.assign(fs.begin(), fs.end());
}

static void buildFrame(const Img& I, Frame& F) {
    if (F.built) return;
    F.built = true;
    KeyMap km; std::vector<DI> ins;
    std::vector<std::pair<uint32_t, uint32_t>> W;
    witnesses(I, F.wk, W);
    for (auto& fn : W) {
        decodeChunk(I, fn.first, fn.second, ins, true);
        if (F.st) {
            std::set<int64_t> Bs;
            stackBase(ins, F.stPrefix, F.stCtor, Bs);
            if (Bs.size() != 1) continue;
            if (F.stSeg) { KeyMap seg; segmap(ins, *Bs.begin(), seg); for (auto& kv : seg) km[kv.first].insert(kv.second.begin(), kv.second.end()); }
            continue;
        }
        BaseMap m; keymapBased(ins, m);
        std::set<int> own;
        for (auto& a : F.wk) { auto it = m.find(a); if (it != m.end()) for (auto& pr : it->second) own.insert(pr.first); }
        for (auto& kv : m) for (auto& pr : kv.second) if (own.count(pr.first)) km[kv.first].insert(pr.second);
    }
    if (!F.cp.empty()) {
        std::vector<std::pair<uint32_t, uint32_t>> X;
        witnesses(I, F.cp, X);
        for (auto& fn : X) {
            decodeChunk(I, fn.first, fn.second, ins, true);
            KeyMap seg; crossSegmap(ins, seg);
            std::map<int64_t, int> Bs; int total = 0;
            for (auto& k : F.cb) {
                auto a = seg.find(k); auto b = km.find(k);
                if (a != seg.end() && b != km.end() && a->second.size() == 1 && b->second.size() == 1) { Bs[*a->second.begin() - *b->second.begin()]++; ++total; }
            }
            if (Bs.size() != 1 || total < 2) continue;
            int64_t B = Bs.begin()->first;
            for (auto& kv : seg)
                if (kv.second.size() == 1 && !km.count(kv.first) && *kv.second.begin() - B >= 0) km[kv.first].insert(*kv.second.begin() - B);
        }
    }
    std::map<uint32_t, std::set<int64_t>> by;
    for (auto& rk : F.rk) { auto it = km.find(rk.second); if (it != km.end() && it->second.size() == 1) by[rk.first].insert(*it->second.begin()); }
    if (!F.st || F.stSeg) by[0].insert(0);
    for (auto& kv : by) if (kv.second.size() == 1) F.smp.push_back({kv.first, *kv.second.begin()});
}

static bool place(uint32_t v, const std::vector<std::pair<uint32_t, int64_t>>& smp, int64_t& out) {
    auto it = std::lower_bound(smp.begin(), smp.end(), std::make_pair(v, (int64_t)INT64_MIN));
    if (it != smp.end() && it->first == v) { out = it->second; return true; }
    if (it == smp.begin() || it == smp.end()) return false;
    auto below = it - 1;
    if (below->second - (int64_t)below->first != it->second - (int64_t)it->first) return false;
    if (!(below->second < it->second)) return false;
    out = (int64_t)v + (below->second - (int64_t)below->first);
    return true;
}

static void boundsOf(uint32_t v, const std::vector<std::pair<uint32_t, int64_t>>& smp, bool& hasLo, int64_t& lo, bool& hasHi, int64_t& hi) {
    auto it = std::lower_bound(smp.begin(), smp.end(), std::make_pair(v, (int64_t)INT64_MIN));
    hasLo = it != smp.begin(); if (hasLo) lo = (it - 1)->second;
    hasHi = it != smp.end(); if (hasHi) hi = it->second;
}

static const int MIN_VOTES = 3;
static bool voteOf(const std::map<uint32_t, int>& ans, uint32_t& top) {
    if (ans.empty()) return false;
    int total = 0, best = 0;
    for (auto& kv : ans) { total += kv.second; if (kv.second > best) { best = kv.second; top = kv.first; } }
    if (ans.size() == 1) return true;
    return best * 3 >= total * 2 && best >= MIN_VOTES;
}
static bool voteStrict(const std::map<uint32_t, int>& ans, uint32_t& top) {
    if (ans.empty()) return false;
    int total = 0, best = 0;
    for (auto& kv : ans) { total += kv.second; if (kv.second > best) { best = kv.second; top = kv.first; } }
    return best * 3 >= total * 2 && best >= MIN_VOTES;        // typedsite: three votes always
}

static void runPlans(Img& I) {
    R().anchors.clear(); R().lay.clear();
    Cur c; Pat p; std::vector<uint32_t> hits, vts;
    std::vector<Frame> frames;
    int curLay = -1, layFrame = -1; std::map<uint32_t, int> votes; bool layIsSize = false; uint32_t layRef = 0;
    std::string layName;

    auto closeLay = [&]() {
        if (curLay < 0) return;
        LayOut o; o.name = layName; o.ref = layRef; o.ok = false; o.val = 0;
        uint32_t top = 0;
        if (layIsSize) {
            if (voteOf(votes, top)) { o.ok = true; o.val = top; }
        } else {
            Frame& F = frames[layFrame]; int64_t a;
            buildFrame(I, F);
            if (place(layRef, F.smp, a)) { o.ok = true; o.val = (uint32_t)a; }
            else if (voteStrict(votes, top)) {
                bool hl, hh; int64_t lo = 0, hi = 0;
                boundsOf(layRef, F.smp, hl, lo, hh, hi);
                if (!((hl && (int64_t)top <= lo) || (hh && (int64_t)top >= hi))) { o.ok = true; o.val = top; }
            }
        }
        R().lay.push_back(o);
        curLay = -1; votes.clear();
    };

    for (int n = 0; n < kPlanCount; ++n) {
        std::vector<std::string> f = split(kPlanLines[n]);
        const std::string& t = f[0];
        if (t == "A") {
            closeAnchor(I, c);
            c.open = true; c.steam = hx(f[1]); c.kind = f[2][0]; c.name = f[3];
        } else if (t == "RT") {
            vtablesByRtti(I, f[2], (uint32_t)dc(f[1]), vts);
            if (vts.size() == 1) c.ans.push_back(vts[0]);
            else if (vts.size() > 1) { c.ans.push_back(vts[0]); c.ans.push_back(vts[1]); }     // ambiguous: a built-in disagreement
        } else if (t == "BO") {
            parsePat(f[1], p);
            int64_t q = findUnique(I, p);
            if (q >= 0) c.ans.push_back((uint32_t)(I.text.va + q));
        } else if (t == "CA" || t == "RF") {
            addRel(I, f, c.ans, p, 1);
        } else if (t == "SL") {
            vtablesByRtti(I, f[3], (uint32_t)dc(f[1]), vts);
            if (vts.size() == 1) c.ans.push_back((uint32_t)(I.u64(vts[0] + 8 * dc(f[2])) - I.base));
        } else if (t == "TC") {
            vtablesByRtti(I, f[4], (uint32_t)dc(f[1]), vts);
            if (vts.size() == 1) {
                auto fn = enclosing(I, (uint32_t)(I.u64(vts[0] + 8 * dc(f[2])) - I.base));
                if (fn) {
                    std::vector<uint32_t> fl; uint32_t a = fn->first; AxIns x;
                    while (a < fn->second && axDecode(I.m + a, fn->second - a, x)) {
                        if ((x.cls == AXI_CALL || x.cls == AXI_JMP) && x.rel32) {
                            uint32_t tg = (uint32_t)(a + x.len + x.rel);
                            if (tg < fn->first || tg >= fn->second) fl.push_back(tg);
                        }
                        a += x.len;
                    }
                    int k = dc(f[3]);
                    if (k < (int)fl.size()) c.ans.push_back(fl[k]);
                }
            }
        } else if (t == "KR") {
            uint32_t sinsn = 0;
            auto fn = fnByString(I, f[3], &sinsn);
            if (fn) {
                std::vector<std::pair<uint32_t, uint32_t>> dl; uint32_t a = fn->first; AxIns x;
                while (a < fn->second && axDecode(I.m + a, fn->second - a, x)) {
                    if (!(x.cls == AXI_CALL && x.rel32) && x.hasMem && x.base == AX_REG_RIP) dl.push_back({a, (uint32_t)(a + x.len + x.disp)});
                    a += x.len;
                }
                int ka = dc(f[1]), kr = dc(f[2]);
                if (ka >= 0 && ka < (int)dl.size()) c.ans.push_back(dl[ka].second);
                std::vector<uint32_t> after, before;
                for (auto& d : dl) { if (d.first > sinsn) after.push_back(d.second); else if (d.first < sinsn) before.push_back(d.second); }
                if (kr > 0 && kr <= (int)after.size()) c.ans.push_back(after[kr - 1]);
                if (kr < 0 && -kr <= (int)before.size()) c.ans.push_back(before[before.size() + kr]);
            }
        } else if (t == "SR") {
            std::vector<uint64_t> sig; bool good = true; std::vector<uint32_t> r;
            for (size_t k = 1; k < f.size(); ++k) { stringRvas(I, f[k], r); if (r.size() != 1) good = false; else sig.push_back(I.base + r[0]); }
            if (good && !sig.empty()) {
                for (auto& s : I.secs) {
                    if (s.name == ".text" || s.rsz < sig.size() * 8) continue;
                    const uint8_t* d = I.m + s.va;
                    for (uint32_t q = 0; q + sig.size() * 8 <= s.rsz; ++q)
                        if (std::memcmp(d + q, sig.data(), sig.size() * 8) == 0) c.ans.push_back(s.va + q);
                }
            }
        } else if (t == "CS") {
            c.cs = true;
        } else if (t == "CL") {
            ++c.nCallers;
            std::set<uint32_t> fns;
            for (size_t k = 1; k < f.size(); ++k) { auto fn = fnByString(I, f[k]); if (fn) fns.insert(fn->first); }
            if (fns.size() == 1) {
                auto fn = enclosing(I, *fns.begin()); std::set<uint32_t> tg;
                if (fn) { callTargets(I, *fn, tg); c.callerSets.push_back(tg); }
            }
        } else if (t == "CP" && !c.name.empty() && c.cs) {
            ++c.nCallers;
            parsePat(f[1], p);
            int64_t q = findUnique(I, p);
            if (q >= 0) {
                auto fn = enclosing(I, (uint32_t)(I.text.va + q)); std::set<uint32_t> tg;
                if (fn) { callTargets(I, *fn, tg); c.callerSets.push_back(tg); }
            }
        } else if (t == "TW") {
            c.twin = true;
            parsePat(f[4], p);
            findPat(I, p, hits, 64);
            for (uint32_t q : hits) {
                int32_t d; std::memcpy(&d, I.m + I.text.va + q + dc(f[1]) + dc(f[3]), 4);
                c.cands.push_back((uint32_t)(I.text.va + q + dc(f[1]) + dc(f[2]) + d));
            }
        } else if (t == "SB") {
            parsePat(f[4], p);
            int64_t q = findUnique(I, p);
            if (q >= 0) {
                int32_t d; std::memcpy(&d, I.m + I.text.va + q + dc(f[3]), 4);
                c.sib[dc(f[1])].insert((uint32_t)(I.text.va + q + dc(f[2]) + d));
            } else c.sib[dc(f[1])];
        } else if (t == "F") {
            closeAnchor(I, c); closeLay();
            Frame F; F.name = f[1]; F.size = hx(f[2]); frames.push_back(F);
        } else if (t == "WK") { frames.back().wk.push_back(f[1]);
        } else if (t == "ST") { Frame& F = frames.back(); F.st = true; F.stPrefix = f[1]; F.stCtor = f[2] == "1"; F.stSeg = f[3] == "1";
        } else if (t == "CP") { frames.back().cp.push_back(f[1]);
        } else if (t == "CB") { frames.back().cb.push_back(f[1]);
        } else if (t == "RK") { frames.back().rk.push_back({hx(f[1]), f[2]});
        } else if (t == "L") {
            closeLay();
            curLay = (int)R().lay.size(); layName = f[1]; layFrame = dc(f[2]); layRef = hx(f[3]); layIsSize = false;
        } else if (t == "TS") {
            parsePat(f[2], p);
            int64_t q = findUnique(I, p);
            if (q >= 0) { int32_t d; std::memcpy(&d, I.m + I.text.va + q + dc(f[1]), 4); votes[(uint32_t)d]++; }
        } else if (t == "Z") {
            closeLay();
            curLay = (int)R().lay.size(); layName = f[1]; layFrame = -1; layRef = hx(f[2]); layIsSize = true;
        } else if (t == "SZ") {
            parsePat(f[2], p);
            int64_t q = findUnique(I, p);
            if (q >= 0) { uint32_t d; std::memcpy(&d, I.m + I.text.va + q + dc(f[1]), 4); votes[d]++; }
        }
    }
    closeAnchor(I, c); closeLay();
    std::sort(R().anchors.begin(), R().anchors.end(), [](const AnchorOut& a, const AnchorOut& b) { return a.steam < b.steam; });
}

static void computeStats() {
    g_stats = AxResolverStats{};
    g_stats.anchors = (int)R().anchors.size();
    for (auto& a : R().anchors) if (a.ok) ++g_stats.anchorsOk;
    g_stats.layouts = (int)R().lay.size();
    for (auto& l : R().lay) if (l.ok) ++g_stats.layoutsOk;
    g_stats.criticalOk = true;
    for (const char* nm : kCritical) {
        bool found = false;
        for (auto& a : R().anchors) if (a.name == nm && a.ok) found = true;
        if (!found) g_stats.criticalOk = false;
    }
}

}  // namespace

bool axResolverRun(const uint8_t* mem, uint64_t base) {
    Img I;
    R().anchors.clear(); R().lay.clear(); g_stats = AxResolverStats{};
    if (!parseImage(I, mem, base)) return false;
    buildIndexes(I);
    runPlans(I);
    computeStats();
    return true;
}

AxResolverStats axResolverStats() { return g_stats; }

void axResolverForEachRefused(void (*fn)(const char*, const char*)) {
    for (auto& a : R().anchors) if (!a.ok) fn("address", a.name.c_str());
    for (auto& l : R().lay) if (!l.ok) fn("layout", l.name.c_str());
}

void axResolverDump(FILE* f) {
    for (auto& a : R().anchors) {
        if (a.ok) std::fprintf(f, "A\t%s\t%x\t%x\n", a.name.c_str(), a.steam, a.rva);
        else std::fprintf(f, "A\t%s\t%x\t-\n", a.name.c_str(), a.steam);
    }
    for (auto& l : R().lay) {
        if (l.ok) std::fprintf(f, "L\t%s\t%x\t%x\n", l.name.c_str(), l.ref, l.val);
        else std::fprintf(f, "L\t%s\t%x\t-\n", l.name.c_str(), l.ref);
    }
}

#ifndef AX_RESOLVER_HARNESS
#include <windows.h>

static bool axResolverGuarded(const uint8_t* base) {
    __try { return axResolverRun(base, reinterpret_cast<uint64_t>(base)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void axResolverEnsure() {
    static bool done = [] {
        auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        bool ok = false;
        try { ok = axResolverGuarded(base); } catch (...) { ok = false; }
        if (!ok) { R().anchors.clear(); R().lay.clear(); g_stats = AxResolverStats{}; }
        return ok;
    }();
    (void)done;
}

bool axResolverUsable() {
    axResolverEnsure();
    return g_stats.criticalOk && g_stats.anchorsOk * 10 >= g_stats.anchors * 9;
}

uintptr_t axResolvedRva(uintptr_t steamRva, bool zeroIfRefused) {
    axResolverEnsure();
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto it = std::lower_bound(R().anchors.begin(), R().anchors.end(), (uint32_t)steamRva,
                               [](const AnchorOut& a, uint32_t v) { return a.steam < v; });
    if (it != R().anchors.end() && it->steam == (uint32_t)steamRva) {
        if (it->ok) return it->rva;
        if (zeroIfRefused) return 0;
        if (it->kind == 'f') return reinterpret_cast<uintptr_t>(&axRefusedStub) - base;
    }
    if (zeroIfRefused) return 0;
    return reinterpret_cast<uintptr_t>(g_zeroPage + kZeroInset) - base;
}

uintptr_t axResolvedLayout(int id, uintptr_t refVal) {
    axResolverEnsure();
    if (id >= 0 && id < (int)R().lay.size() && R().lay[id].ok) return R().lay[id].val;
    return id >= 0 && id < (int)R().lay.size() ? AX_LAY_REFUSED : refVal;
}
#endif
