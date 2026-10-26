#include "dect2/automation/script.h"
#include "dect2/automation/expr.h"
#include "dect2/automation/json_mini.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace dect2::automation {

namespace {
constexpr size_t kMemLimit = 16u << 20;
constexpr int kHookEvery = 1000;
constexpr int kMaxInstructions = 1000000;
constexpr int kMaxMillis = 250;
constexpr size_t kMaxText = 8000;
}  // namespace

struct ScriptHost::Impl {
    lua_State* L = nullptr;
    size_t memUsed = 0;
    ScriptApi* api = nullptr;
    int instr = 0;
    std::chrono::steady_clock::time_point deadline;
    std::string name;
};

namespace {
using Impl = ScriptHost::Impl;

void* allocator(void* ud, void* ptr, size_t osize, size_t nsize) {
    Impl* m = static_cast<Impl*>(ud);
    const size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        if (ptr) { m->memUsed -= old; free(ptr); }
        return nullptr;
    }
    if (m->memUsed - old + nsize > kMemLimit) return nullptr;
    void* p = realloc(ptr, nsize);
    if (p) m->memUsed = m->memUsed - old + nsize;
    return p;
}

Impl* self(lua_State* L) { return static_cast<Impl*>(lua_touserdata(L, lua_upvalueindex(1))); }

void hook(lua_State* L, lua_Debug*) {
    // the Impl is reachable through the registry
    lua_getfield(L, LUA_REGISTRYINDEX, "zorcom_impl");
    Impl* m = static_cast<Impl*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!m) return;
    m->instr += kHookEvery;
    if (m->instr > kMaxInstructions) luaL_error(L, "the script used too many instructions (1 million per event)");
    if (std::chrono::steady_clock::now() > m->deadline) luaL_error(L, "the script took too long (250 ms per event)");
}

std::string argText(lua_State* L, int i) {
    size_t n = 0;
    const char* s = luaL_optlstring(L, i, "", &n);
    return std::string(s, std::min(n, kMaxText));
}

int pushResult(lua_State* L, bool ok, const std::string& detail) {
    lua_pushboolean(L, ok);
    lua_pushlstring(L, detail.data(), detail.size());
    return 2;
}

int doAct(lua_State* L, const char* kind, std::map<std::string, std::string> p) {
    Impl* m = self(L);
    std::string detail;
    const bool ok = m->api->act(kind, p, detail);
    return pushResult(L, ok, detail);
}

std::string seconds(lua_State* L, int i) {
    double s = luaL_checknumber(L, i);
    if (!(s >= 1)) s = 1;
    if (s > 3600) s = 3600;
    return std::to_string((int)s);
}

// Lua value to JSON text (depth 8, 2000 nodes)
bool toJson(lua_State* L, int idx, json::Value& out, int depth, int& budget) {
    if (--budget < 0 || depth > 8) return false;
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL: out = json::Value(); return true;
    case LUA_TBOOLEAN: out = json::Value::boolean(lua_toboolean(L, idx)); return true;
    case LUA_TNUMBER: { const double d = lua_tonumber(L, idx); out = json::Value::number(std::isfinite(d) ? d : 0); return true; }
    case LUA_TSTRING: { size_t n; const char* s = lua_tolstring(L, idx, &n); out = json::Value::string(std::string(s, std::min(n, kMaxText))); return true; }
    case LUA_TTABLE: {
        const lua_Integer len = (lua_Integer)lua_rawlen(L, idx);
        bool isArray = len > 0;
        if (isArray) {   // every key must be 1..len
            lua_Integer count = 0;
            lua_pushnil(L);
            while (lua_next(L, idx)) { count++; if (!lua_isinteger(L, -2) || count > len + 1000) isArray = false; lua_pop(L, 1); if (!isArray) { lua_pop(L, 1); break; } }
            if (isArray && count != len) isArray = false;
        }
        if (isArray) {
            out = json::Value::array();
            for (lua_Integer i = 1; i <= len; i++) {
                lua_rawgeti(L, idx, i);
                json::Value v;
                const bool ok = toJson(L, -1, v, depth + 1, budget);
                lua_pop(L, 1);
                if (!ok) return false;
                out.push(std::move(v));
            }
            return true;
        }
        out = json::Value::object();
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                json::Value v;
                if (!toJson(L, -1, v, depth + 1, budget)) { lua_pop(L, 2); return false; }
                out.o[lua_tostring(L, -2)] = std::move(v);
            }
            lua_pop(L, 1);
        }
        return true;
    }
    default: out = json::Value::string(luaL_typename(L, idx)); return true;
    }
}

int l_json(lua_State* L) {
    json::Value v;
    int budget = 2000;
    if (!toJson(L, 1, v, 0, budget)) return luaL_error(L, "json(): table too deep or too large");
    const std::string t = json::dump(v);
    lua_pushlstring(L, t.data(), std::min(t.size(), kMaxText * 2));
    return 1;
}

int l_notify(lua_State* L) { return doAct(L, "notify", {{"title", argText(L, 1)}, {"body", argText(L, 2)}}); }
int l_sound(lua_State* L) { return doAct(L, "sound", {{"file", argText(L, 1)}}); }
int l_record_iq(lua_State* L) { return doAct(L, "record_iq", {{"seconds", seconds(L, 1)}}); }
int l_record_audio(lua_State* L) { return doAct(L, "record_audio", {{"seconds", seconds(L, 1)}}); }
int l_save_rewind(lua_State* L) { return doAct(L, "save_rewind", {{"seconds", seconds(L, 1)}}); }
int l_tune(lua_State* L) {
    char b[48];
    snprintf(b, sizeof b, "%.6f", luaL_checknumber(L, 1) / 1e6);
    return doAct(L, "tune", {{"freq_mhz", b}, {"mode", argText(L, 2)}});
}
int l_memory(lua_State* L) { return doAct(L, "memory", {{"name", argText(L, 1)}}); }
int l_screenshot(lua_State* L) { return doAct(L, "screenshot", {}); }
int l_webhook(lua_State* L) {
    std::string url = argText(L, 1), body;
    if (lua_istable(L, 2)) {
        json::Value v;
        int budget = 2000;
        if (!toJson(L, 2, v, 0, budget)) return luaL_error(L, "webhook(): table too deep or too large");
        body = json::dump(v);
    } else {
        body = argText(L, 2);
        if (body.empty()) body = "{}";
    }
    return doAct(L, "webhook", {{"url", url}, {"body", body}});
}
int l_log(lua_State* L) { self(L)->api->note(argText(L, 1)); return 0; }
int l_print(lua_State* L) {
    std::string t;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n && t.size() < 300; i++) {
        size_t len;
        const char* s = luaL_tolstring(L, i, &len);
        t += (i > 1 ? "  " : "") + std::string(s, len);
        lua_pop(L, 1);
    }
    self(L)->api->note(t);
    return 0;
}
int l_log_row(lua_State* L) {
    const std::string file = argText(L, 1);
    std::string cells;
    const int n = lua_gettop(L);
    for (int i = 2; i <= n && i < 32; i++) {
        size_t len;
        const char* s = luaL_tolstring(L, i, &len);
        std::string c(s, std::min<size_t>(len, 500));
        lua_pop(L, 1);
        for (char& ch : c) if (ch == '\n' || ch == '\r') ch = ' ';
        cells += (i > 2 ? "\n" : "") + c;
    }
    return doAct(L, "log", {{"file", file}, {"cells", cells}});
}
int l_distance(lua_State* L) {
    double la, lo;
    if (!self(L)->api->station(la, lo)) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, geo::distanceKm(la, lo, luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
    return 1;
}
int l_bearing(lua_State* L) {
    double la, lo;
    if (!self(L)->api->station(la, lo)) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, geo::bearingDeg(la, lo, luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
    return 1;
}
int l_now(lua_State* L) { lua_pushnumber(L, self(L)->api->now()); return 1; }
int l_hour(lua_State* L) {
    const long long t = (long long)std::floor(self(L)->api->now()) + self(L)->api->tzOffsetSec();
    const long long day = (long long)std::floor((double)t / 86400.0);
    lua_pushinteger(L, (lua_Integer)((t - day * 86400) / 3600));
    return 1;
}

// string additions: plain text helpers (the pattern functions are removed)
std::string lowerOf(const char* s, size_t n) { std::string o(s, n); for (char& c : o) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return o; }
int s_contains(lua_State* L) {
    size_t a, b; const char* s = luaL_checklstring(L, 1, &a); const char* t = luaL_checklstring(L, 2, &b);
    lua_pushboolean(L, lowerOf(s, a).find(lowerOf(t, b)) != std::string::npos);
    return 1;
}
int s_starts(lua_State* L) {
    size_t a, b; const char* s = luaL_checklstring(L, 1, &a); const char* t = luaL_checklstring(L, 2, &b);
    lua_pushboolean(L, a >= b && lowerOf(s, a).compare(0, b, lowerOf(t, b)) == 0);
    return 1;
}
int s_ends(lua_State* L) {
    size_t a, b; const char* s = luaL_checklstring(L, 1, &a); const char* t = luaL_checklstring(L, 2, &b);
    lua_pushboolean(L, a >= b && lowerOf(s, a).compare(a - b, b, lowerOf(t, b)) == 0);
    return 1;
}
int s_matches(lua_State* L) {
    size_t a, b; const char* s = luaL_checklstring(L, 1, &a); const char* p = luaL_checklstring(L, 2, &b);
    lua_pushboolean(L, globMatch(std::string(p, b), std::string(s, a)));
    return 1;
}
int s_replace(lua_State* L) {
    size_t a, b, c;
    const char* s = luaL_checklstring(L, 1, &a); const char* f = luaL_checklstring(L, 2, &b); const char* t = luaL_checklstring(L, 3, &c);
    std::string o(s, a);
    if (b) {
        std::string r;
        size_t i = 0;
        const std::string from(f, b), to(t, c);
        for (;;) {
            const size_t j = o.find(from, i);
            if (j == std::string::npos) break;
            r.append(o, i, j - i); r += to; i = j + b;
            if (r.size() > kMaxText * 4) return luaL_error(L, "replace(): the result is too long");
        }
        r.append(o, i, std::string::npos);
        o = r;
    }
    lua_pushlstring(L, o.data(), o.size());
    return 1;
}
int s_find(lua_State* L) {   // plain text only: string.find(s, text [, init])
    size_t a, b; const char* s = luaL_checklstring(L, 1, &a); const char* t = luaL_checklstring(L, 2, &b);
    lua_Integer init = luaL_optinteger(L, 3, 1);
    if (init < 1) init = 1;
    if ((size_t)init - 1 > a) { lua_pushnil(L); return 1; }
    const std::string h(s, a), n(t, b);
    const size_t j = h.find(n, (size_t)init - 1);
    if (j == std::string::npos) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)j + 1);
    lua_pushinteger(L, (lua_Integer)(j + b));
    return 2;
}

void setFn(lua_State* L, Impl* m, const char* name, lua_CFunction f) {
    lua_pushlightuserdata(L, m);
    lua_pushcclosure(L, f, 1);
    lua_setglobal(L, name);
}

void openSandbox(Impl* m) {
    lua_State* L = m->L;
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1); lua_pop(L, 1);
    luaL_requiref(L, "utf8", luaopen_utf8, 1); lua_pop(L, 1);
    for (const char* n : {"dofile", "loadfile", "load", "collectgarbage", "print", "getmetatable", "setmetatable", "rawequal", "pcall", "xpcall"}) { lua_pushnil(L); lua_setglobal(L, n); }
    // math.random is fine (seeded by the VM); nothing else to remove there
    lua_getglobal(L, "string");
    for (const char* n : {"dump", "match", "gmatch", "gsub", "pack", "unpack", "packsize"}) { lua_pushnil(L); lua_setfield(L, -2, n); }
    lua_pushcfunction(L, s_find); lua_setfield(L, -2, "find");
    lua_pushcfunction(L, s_contains); lua_setfield(L, -2, "contains");
    lua_pushcfunction(L, s_starts); lua_setfield(L, -2, "starts_with");
    lua_pushcfunction(L, s_ends); lua_setfield(L, -2, "ends_with");
    lua_pushcfunction(L, s_matches); lua_setfield(L, -2, "matches");
    lua_pushcfunction(L, s_replace); lua_setfield(L, -2, "replace");
    lua_pop(L, 1);
    // the strings' metatable stays, with __index = the reduced string table
    lua_pushlightuserdata(L, m);
    lua_setfield(L, LUA_REGISTRYINDEX, "zorcom_impl");
    setFn(L, m, "notify", l_notify); setFn(L, m, "sound", l_sound); setFn(L, m, "record_iq", l_record_iq); setFn(L, m, "record_audio", l_record_audio); setFn(L, m, "save_rewind", l_save_rewind);
    setFn(L, m, "tune", l_tune); setFn(L, m, "memory", l_memory); setFn(L, m, "screenshot", l_screenshot); setFn(L, m, "webhook", l_webhook);
    setFn(L, m, "log", l_log); setFn(L, m, "print", l_print); setFn(L, m, "log_row", l_log_row); setFn(L, m, "distance_km", l_distance); setFn(L, m, "bearing_deg", l_bearing);
    setFn(L, m, "now", l_now); setFn(L, m, "hour", l_hour); setFn(L, m, "json", l_json);
}

void pushEvent(lua_State* L, const Event& ev) {
    lua_createtable(L, 0, (int)ev.f.size() + 5);
    for (const auto& kv : ev.f) {
        switch (kv.second.t) {
        case Value::Num:
            if (kv.second.n == std::floor(kv.second.n) && std::fabs(kv.second.n) < 9e15) lua_pushinteger(L, (lua_Integer)kv.second.n);   // whole numbers are integers: 12000, not 12000.0
            else lua_pushnumber(L, kv.second.n);
            break;
        case Value::Str: lua_pushlstring(L, kv.second.s.data(), kv.second.s.size()); break;
        case Value::Bool: lua_pushboolean(L, kv.second.b); break;
        default: continue;
        }
        lua_setfield(L, -2, kv.first.c_str());
    }
    lua_pushlstring(L, ev.type.data(), ev.type.size()); lua_setfield(L, -2, "type");
    lua_pushlstring(L, ev.mode.data(), ev.mode.size()); lua_setfield(L, -2, "mode");
    lua_pushnumber(L, ev.freqHz); lua_setfield(L, -2, "freq");
    lua_pushnumber(L, ev.time); lua_setfield(L, -2, "time");
}

void arm(Impl* m) {
    m->instr = 0;
    m->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kMaxMillis);
    lua_sethook(m->L, hook, LUA_MASKCOUNT, kHookEvery);
}
void disarm(Impl* m) { lua_sethook(m->L, nullptr, 0, 0); }

std::string takeError(lua_State* L) {
    size_t n;
    const char* s = lua_tolstring(L, -1, &n);
    std::string e = s ? std::string(s, std::min<size_t>(n, 400)) : "error";
    lua_pop(L, 1);
    return e;
}
}  // namespace

ScriptHost::ScriptHost(const Script& s, ScriptApi* api) : p_(new Impl) {
    p_->api = api;
    p_->name = s.name;
    p_->L = lua_newstate(allocator, p_.get());
    if (!p_->L) { err_ = "cannot start Lua"; return; }
    openSandbox(p_.get());
    lua_State* L = p_->L;
    if (s.code.size() >= 1 && (unsigned char)s.code[0] == 0x1B) { err_ = "compiled Lua is not accepted, only source text"; return; }
    arm(p_.get());
    int rc = luaL_loadbufferx(L, s.code.data(), s.code.size(), "=script", "t");   // "t": text only, never bytecode
    if (rc == LUA_OK) rc = lua_pcall(L, 0, 0, 0);
    disarm(p_.get());
    if (rc != LUA_OK) { err_ = takeError(L); return; }
    lua_getglobal(L, "on_event");
    const bool fn = lua_isfunction(L, -1);
    lua_pop(L, 1);
    if (!fn) err_ = "the script must define function on_event(e)";
}

ScriptHost::~ScriptHost() { if (p_ && p_->L) lua_close(p_->L); }

bool ScriptHost::onEvent(const Event& ev) {
    if (!loaded() || !p_->L) return false;
    lua_State* L = p_->L;
    lua_getglobal(L, "on_event");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); lastRunError_ = "on_event is not a function any more"; errors_++; return false; }
    pushEvent(L, ev);
    arm(p_.get());
    const int rc = lua_pcall(L, 1, 0, 0);
    disarm(p_.get());
    if (rc != LUA_OK) {
        const std::string e = takeError(L);
        lua_gc(L, LUA_GCCOLLECT);
        errors_++;
        lastRunError_ = e;
        return false;
    }
    return true;
}

std::string checkScriptSyntax(const std::string& code) {
    if (!code.empty() && (unsigned char)code[0] == 0x1B) return "compiled Lua is not accepted, only source text";
    Impl m;
    m.L = lua_newstate(allocator, &m);
    if (!m.L) return "cannot start Lua";
    const int rc = luaL_loadbufferx(m.L, code.data(), code.size(), "=script", "t");
    std::string e;
    if (rc != LUA_OK) e = takeError(m.L);
    lua_close(m.L);
    return e;
}

}  // namespace dect2::automation
