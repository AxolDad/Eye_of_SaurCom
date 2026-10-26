// dab_proto.h on top of the Rust crate rust/saurcom_dab: record arrays sized from the input length (every record needs a few bytes of input;
// the lists that overlapping descriptors of corrupt data could lengthen are not among them), turned into the vector / string forms of the header.
// Negative or zero lengths are 0 bytes.
#include "dect2/dab/dab_proto.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/dab.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace dect2::dabp {

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[28];
        saurcom_dab_struct_sizes(s);
        dect2::rsAbiCheck("saurcom_dab", s, {sizeof(SaurcomDabFigRef), sizeof(SaurcomDabFigHead), sizeof(SaurcomDabFig01Sub), sizeof(SaurcomDabFig03Packet), sizeof(SaurcomDabFig02Service), sizeof(SaurcomDabFig02Comp), sizeof(SaurcomDabFig013App), sizeof(SaurcomDabFig017Pty), sizeof(SaurcomDabFig018Support), sizeof(SaurcomDabFig019Switch), sizeof(SaurcomDabFig010Time), sizeof(SaurcomDabFig1Label), sizeof(SaurcomDabFig2Seg), sizeof(SaurcomDabMotTime), sizeof(SaurcomDabMotHeader), sizeof(SaurcomDabMotParam), sizeof(SaurcomDabMotGroup), sizeof(SaurcomDabDirEntry), sizeof(SaurcomDabPadBytes), sizeof(SaurcomDabPadSub), sizeof(SaurcomDabDlsStep), sizeof(SaurcomDabDlSegment), sizeof(SaurcomDabDlpTag), sizeof(SaurcomDabJlGroup), sizeof(SaurcomDabTocEntry), sizeof(SaurcomDabTocInfo), sizeof(SaurcomDabPacket), sizeof(SaurcomDabDrmPacket)});
    }
} abiCheck;
inline Range rg(const SaurcomDabRange& r) { return Range{r.off, r.len}; }
MotTime fromC(const SaurcomDabMotTime& c) {
    MotTime t;
    t.valid = c.valid; t.now = c.now; t.longForm = c.long_form; t.mjd = c.mjd; t.hour = c.hour; t.minute = c.minute; t.second = c.second; t.ms = c.ms;
    return t;
}
SaurcomDabMotTime toC(const MotTime& t) {
    SaurcomDabMotTime c;
    c.valid = t.valid; c.now = t.now; c.long_form = t.longForm; c.mjd = t.mjd; c.hour = t.hour; c.minute = t.minute; c.second = t.second; c.ms = t.ms;
    return c;
}
template <class T> std::vector<T> sized(size_t n, size_t per) { return std::vector<T>(n / per + 1); }
}

uint16_t crc16(const uint8_t* p, size_t n) { return saurcom_dab_crc16(p, n); }

std::vector<FigRef> fibWalk(const uint8_t* fib, size_t n, int& used) {
    SaurcomDabFigRef v[16];
    uint32_t u = 0;
    const size_t c = saurcom_dab_fib_walk(fib, n, v, 16, &u);
    used = (int)u;
    std::vector<FigRef> out;
    for (size_t i = 0; i < c && i < 16; i++) out.push_back(FigRef{v[i].type, v[i].off, v[i].len});
    return out;
}

FigHead figHead(int type, const uint8_t* d, size_t n) {
    SaurcomDabFigHead h;
    saurcom_dab_fig_head(type, d, n, &h);
    return FigHead{h.cn, h.oe, h.pd, h.ext};
}

bool fig0Eid(const uint8_t* d, size_t n, int& eid) { int32_t e; if (!saurcom_dab_fig0_eid(d, n, &e)) return false; eid = e; return true; }

std::vector<Fig01Sub> fig0Subchannels(const uint8_t* d, size_t n) {
    auto v = sized<SaurcomDabFig01Sub>(n, 3);
    const size_t c = std::min(saurcom_dab_fig0_subchannels(d, n, v.data(), v.size()), v.size());
    std::vector<Fig01Sub> out;
    for (size_t i = 0; i < c; i++) out.push_back(Fig01Sub{v[i].id, v[i].start, v[i].eep != 0, v[i].uep_table != 0, v[i].option, v[i].level, v[i].size, v[i].uep_index});
    return out;
}

std::vector<Fig03Packet> fig0Packet(const uint8_t* d, size_t n) {
    auto v = sized<SaurcomDabFig03Packet>(n, 5);
    const size_t c = std::min(saurcom_dab_fig0_packet(d, n, v.data(), v.size()), v.size());
    std::vector<Fig03Packet> out;
    for (size_t i = 0; i < c; i++) out.push_back(Fig03Packet{v[i].scid, v[i].dg != 0, v[i].dscty, v[i].sub_id, v[i].address});
    return out;
}

std::vector<Fig02Service> fig0Services(const uint8_t* d, size_t n) {
    auto s = sized<SaurcomDabFig02Service>(n, 3);
    auto c = sized<SaurcomDabFig02Comp>(n, 2);
    size_t nc = 0;
    const size_t ns = std::min(saurcom_dab_fig0_services(d, n, s.data(), s.size(), c.data(), c.size(), &nc), s.size());
    std::vector<Fig02Service> out;
    for (size_t i = 0; i < ns; i++) {
        Fig02Service sv;
        sv.sid = s[i].sid;
        for (uint32_t k = 0; k < s[i].ncomp && s[i].first + k < c.size(); k++) {
            const SaurcomDabFig02Comp& x = c[s[i].first + k];
            sv.comps.push_back(Fig02Comp{x.tmid, x.ascty, x.dscty, x.sub_id, x.scid, x.primary != 0});
        }
        out.push_back(std::move(sv));
    }
    return out;
}

std::vector<Fig013App> fig0UserApps(const uint8_t* d, size_t n) {
    auto v = sized<SaurcomDabFig013App>(n, 2);
    const size_t c = std::min(saurcom_dab_fig0_userapps(d, n, v.data(), v.size()), v.size());
    std::vector<Fig013App> out;
    for (size_t i = 0; i < c; i++) out.push_back(Fig013App{v[i].sid, v[i].scids, v[i].index, v[i].type, v[i].off, v[i].len});
    return out;
}

std::vector<Fig017Pty> fig0Pty(const uint8_t* d, size_t n) {
    auto v = sized<SaurcomDabFig017Pty>(n, 3);
    const size_t c = std::min(saurcom_dab_fig0_pty(d, n, v.data(), v.size()), v.size());
    std::vector<Fig017Pty> out;
    for (size_t i = 0; i < c; i++) out.push_back(Fig017Pty{v[i].sid, v[i].sd != 0, v[i].primary != 0, v[i].lang, v[i].pty});
    return out;
}

std::vector<Fig018Support> fig0AnnSupport(const uint8_t* d, size_t n) {
    auto v = sized<SaurcomDabFig018Support>(n, 5);
    std::vector<uint8_t> cl(n + 1);
    size_t ncl = 0;
    const size_t c = std::min(saurcom_dab_fig0_ann_support(d, n, v.data(), v.size(), cl.data(), cl.size(), &ncl), v.size());
    std::vector<Fig018Support> out;
    for (size_t i = 0; i < c; i++) {
        Fig018Support s;
        s.sid = v[i].sid; s.asu = v[i].asu;
        for (uint32_t k = 0; k < v[i].ncl && v[i].first + k < cl.size(); k++) s.clusters.push_back(cl[v[i].first + k]);
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<Fig019Switch> fig0AnnSwitch(const uint8_t* d, size_t n) {
    auto v = sized<SaurcomDabFig019Switch>(n, 4);
    const size_t c = std::min(saurcom_dab_fig0_ann_switch(d, n, v.data(), v.size()), v.size());
    std::vector<Fig019Switch> out;
    for (size_t i = 0; i < c; i++) out.push_back(Fig019Switch{v[i].cluster, v[i].asw, v[i].new_flag != 0, v[i].region != 0, v[i].sub_id});
    return out;
}

bool fig0Time(const uint8_t* d, size_t n, Fig010Time& t) {
    SaurcomDabFig010Time c;
    if (!saurcom_dab_fig0_time(d, n, &c)) return false;
    t.mjd = c.mjd; t.hour = c.hour; t.minute = c.minute; t.second = c.second; t.utc = c.utc;
    return true;
}

bool fig1(const uint8_t* d, size_t n, Fig1Label& l) {
    SaurcomDabFig1Label c;
    if (!saurcom_dab_fig1(d, n, &c)) return false;
    l.ext = c.ext; l.charset = c.charset; l.scids = c.scids; l.sid = c.sid; l.off = c.off;
    return true;
}

bool fig2(const uint8_t* d, size_t n, Fig2Seg& s) {
    SaurcomDabFig2Seg c;
    if (!saurcom_dab_fig2(d, n, &c)) return false;
    s.ext = c.ext; s.toggle = c.toggle; s.seg = c.seg; s.rfu = c.rfu; s.scids = c.scids; s.sid = c.sid; s.ucs2 = c.ucs2 != 0; s.total = c.total; s.valid = c.valid != 0;
    s.textOff = c.text_off; s.textLen = c.text_len;
    return true;
}

size_t motParseTime(const uint8_t* d, size_t n, MotTime& t) {
    SaurcomDabMotTime c;
    const size_t u = saurcom_dab_mot_parse_time(d, n, &c);
    t = fromC(c);
    return u;
}

std::string motTimeText(const MotTime& t) {
    const SaurcomDabMotTime c = toC(t);
    uint8_t b[128];
    return std::string((const char*)b, saurcom_dab_mot_time_text(&c, b, sizeof b));
}

bool motParseHeader(const uint8_t* h, size_t n, MotHeader& o) {
    std::vector<SaurcomDabMotParam> pv(n / 1 + 1);
    std::vector<uint8_t> cap(n * 3 + 16);
    SaurcomDabMotHeader c;
    if (!saurcom_dab_mot_parse_header(h, n, &c, pv.data(), pv.size(), cap.data(), cap.size())) return false;
    o = MotHeader();
    o.bodySize = c.body_size; o.headerSize = c.header_size; o.contentType = c.content_type; o.subtype = c.subtype;
    for (uint32_t i = 0; i < c.n_params && i < pv.size(); i++) o.params.push_back(MotParamRef{pv[i].id, rg(pv[i].data)});
    o.hasName = c.has_name != 0; o.hasSlide = c.has_slide != 0; o.hasTrigger = c.has_trigger != 0; o.hasExpire = c.has_expire != 0;
    o.categoryId = c.category_id; o.slideId = c.slide_id;
    o.name = rg(c.name); o.category = rg(c.category); o.clickUrl = rg(c.click_url); o.altUrl = rg(c.alt_url);
    o.caption.assign((const char*)cap.data(), std::min<size_t>(c.caption_len, cap.size()));
    o.trigger = fromC(c.trigger); o.expire = fromC(c.expire);
    return true;
}

std::string motContentTypeName(int type, int sub) {
    uint8_t b[32];
    return std::string((const char*)b, saurcom_dab_mot_content_type_name(type, sub, b, sizeof b));
}

MotGroup motParseGroup(const uint8_t* d, size_t n) {
    SaurcomDabMotGroup c;
    saurcom_dab_mot_parse_group(d, n, &c);
    MotGroup g;
    g.status = c.status; g.last = c.last != 0; g.type = c.type; g.segNo = c.seg_no; g.tid = c.tid; g.off = c.off; g.len = c.len;
    return g;
}

bool motParseDirectory(const uint8_t* d, size_t n, std::vector<DirEntry>& out) {
    out.clear();
    std::vector<SaurcomDabDirEntry> v(n / 9 + 1);
    const size_t c = saurcom_dab_mot_parse_directory(d, n, v.data(), v.size());
    if (c == SIZE_MAX) return false;
    for (size_t i = 0; i < c && i < v.size(); i++) out.push_back(DirEntry{v[i].tid, v[i].off, v[i].size});
    return true;
}

bool padFromAu(const uint8_t* au, size_t n, PadBytes& out) {
    out = PadBytes();
    uint8_t xp[512];
    SaurcomDabPadBytes c;
    if (!saurcom_dab_pad_from_au(au, n, xp, sizeof xp, &c)) return false;
    out.xpad.assign(xp, xp + std::min<size_t>(c.xlen, sizeof xp));
    out.fpad[0] = c.fpad[0]; out.fpad[1] = c.fpad[1];
    return true;
}

bool padFromMp2(const uint8_t* f, size_t n, int& lastCont, PadBytes& out) {
    out = PadBytes();
    uint8_t xp[512];
    SaurcomDabPadBytes c;
    int32_t lc = lastCont;
    const bool ok = saurcom_dab_pad_from_mp2(f, n, &lc, xp, sizeof xp, &c);
    lastCont = lc;
    if (!ok) return false;
    out.xpad.assign(xp, xp + std::min<size_t>(c.xlen, sizeof xp));
    out.fpad[0] = c.fpad[0]; out.fpad[1] = c.fpad[1];
    return true;
}

std::vector<PadSub> padSplit(const uint8_t* xp, size_t n, const uint8_t fpad[2], int& lastType) {
    SaurcomDabPadSub v[5];
    int32_t lt = lastType;
    const size_t c = saurcom_dab_pad_split(xp, n, fpad, &lt, v, 5);
    lastType = lt;
    std::vector<PadSub> out;
    for (size_t i = 0; i < c && i < 5; i++) out.push_back(PadSub{v[i].type, v[i].off, v[i].len});
    return out;
}

int padDgLength(const uint8_t* d, size_t n, int& len) { int32_t l = len; const int r = saurcom_dab_pad_dg_length(d, n, &l); len = l; return r; }

DlsStep dlsStep(const uint8_t* d, size_t n) { SaurcomDabDlsStep c; saurcom_dab_pad_dls_step(d, n, &c); return DlsStep{c.action, c.seg_len}; }

DlSegment dlSegment(const uint8_t* d, size_t n) {
    SaurcomDabDlSegment c;
    saurcom_dab_pad_dl_segment(d, n, &c);
    DlSegment s;
    s.toggle = c.toggle; s.first = c.first != 0; s.last = c.last != 0; s.cmd = c.cmd != 0; s.code = c.code; s.segNo = c.seg_no; s.charset = c.charset;
    return s;
}

bool dlPlusTags(const uint8_t* d, size_t n, bool& running, std::vector<DlpTag>& out) {
    out.clear();
    SaurcomDabDlpTag v[4];
    uint8_t r = 0;
    const size_t c = saurcom_dab_pad_dlplus_tags(d, n, v, &r);
    if (c == SIZE_MAX) return false;
    for (size_t i = 0; i < c && i < 4; i++) out.push_back(DlpTag{v[i].type, v[i].start, v[i].len});
    running = r != 0;
    return true;
}

JlGroup jlDataGroup(const uint8_t* d, size_t n) { SaurcomDabJlGroup c; saurcom_dab_jl_data_group(d, n, &c); return JlGroup{c.status, c.type, c.field_len}; }

bool jlParseToc(const uint8_t* d, size_t n, JlTocInfo& info, std::vector<JlTocEntry>& entries) {
    entries.clear();
    std::vector<SaurcomDabTocEntry> v(2048);
    SaurcomDabTocInfo c;
    if (!saurcom_dab_jl_parse_toc(d, n, &c, v.data(), v.size())) return false;
    info.revision = c.revision; info.blockCount = c.block_count; info.blockIndex = c.block_index; info.objectCount = c.object_count; info.timeoutMin = c.timeout_min;
    for (uint32_t i = 0; i < c.n && i < v.size(); i++) entries.push_back(JlTocEntry{v[i].id, v[i].desc});
    return true;
}

bool jlUserAppData(const uint8_t* d, size_t n, int& version, int& extHeader, int& entry) {
    int32_t v, x, e;
    if (!saurcom_dab_jl_user_app_data(d, n, &v, &x, &e)) return false;
    version = v; extHeader = x; entry = e;
    return true;
}

DabPacket dabPacket(const uint8_t* p, size_t n) {
    SaurcomDabPacket c;
    saurcom_dab_packet(p, n, &c);
    DabPacket k;
    k.crcOk = c.crc_ok != 0; k.first = c.first != 0; k.last = c.last != 0; k.cmd = c.cmd != 0; k.ci = c.ci; k.addr = c.addr; k.ul = c.ul;
    return k;
}

DrmPacket drmPacket(const uint8_t* p, size_t n) {
    SaurcomDabDrmPacket c;
    saurcom_dab_drm_packet(p, n, &c);
    DrmPacket k;
    k.crcOk = c.crc_ok != 0; k.first = c.first != 0; k.last = c.last != 0; k.lenBad = c.len_bad != 0; k.id = c.id; k.ci = c.ci; k.n = c.n; k.at = c.at;
    return k;
}

} // namespace dect2::dabp
