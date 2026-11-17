/*******************************************************************************
 * LuaLoader - mod.audio
 *
 * 两层能力：
 *   1. 原版音效：mod.audio.play_vanilla("Item1", x, y)
 *   2. 自定义音频：mod.audio.load("sound.wav") -> handle
 *        handle:play([volume[, pitch]])
 *        handle:play_at(x, y [, volume[, pitch]])
 *        handle:stop_all() / unload()
 *        mod.audio.pause/resume/stop(id) / set_volume(id, v) / is_playing(id)
 *        mod.audio.set_master_volume(v)
 *
 * 内核 audio_manager 只接收 PCM；这里负责解 WAV 并转成 float32。
 *******************************************************************************/

#include "api_internal.hpp"

#include <cmath>
#include <new>
#include <filesystem>
#include <fstream>
#include <vector>

namespace lualoader::lua_api {

namespace {

constexpr const char *AUDIO_METATABLE = "LuaLoader.Audio";

struct LuaAudio {
    terraria_audio_t audio;
    std::vector<float> pcm;
    std::string source_path;
    bool registered = false;
};

LuaAudio *check_audio(lua_State *L, int idx) {
    return static_cast<LuaAudio *>(luaL_checkudata(L, idx, AUDIO_METATABLE));
}

int audio_gc(lua_State *L) {
    auto *a = static_cast<LuaAudio *>(luaL_checkudata(L, 1, AUDIO_METATABLE));
    if (!a) return 0;
    if (a->registered && terraria_audio_manager_unregister) {
        terraria_audio_manager_stop_all(&a->audio);
        terraria_audio_manager_unregister(&a->audio);
        a->registered = false;
    }
    a->~LuaAudio();
    return 0;
}

std::string resolve_audio_path(const lua_mod_handle_t *handle, const std::string &rel) {
    namespace fs = std::filesystem;
    if (!handle || rel.empty()) return {};
    const fs::path r(rel);
    std::error_code ec;
    if (r.is_absolute())
        return fs::is_regular_file(r, ec) ? r.string() : std::string();
    for (const auto &part : r)
        if (part == "..") return {};

    std::vector<fs::path> bases;
    if (!handle->mod_dir.empty()) {
        bases.emplace_back(fs::path(handle->mod_dir) / "Audio");
        bases.emplace_back(fs::path(handle->mod_dir) / "Sound");
        bases.emplace_back(fs::path(handle->mod_dir));
    }
    if (!handle->private_dir.empty()) {
        bases.emplace_back(fs::path(handle->private_dir) / "Audio");
        bases.emplace_back(fs::path(handle->private_dir) / "Resources" / "Audio");
        bases.emplace_back(fs::path(handle->private_dir));
    }
    for (const auto &b : bases) {
        const fs::path full = b / r;
        if (fs::is_regular_file(full, ec)) return full.string();
    }
    return {};
}

uint32_t le32(const unsigned char *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
           ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
uint16_t le16(const unsigned char *p) {
    return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

bool decode_wav(const std::string &path, std::vector<float> &out,
                int &sample_rate, int &channels) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
    if (buf.size() < 12) return false;
    if (std::memcmp(buf.data(), "RIFF", 4) != 0 ||
        std::memcmp(buf.data() + 8, "WAVE", 4) != 0)
        return false;

    int format = 0, bits = 0;
    const unsigned char *data = nullptr;
    size_t data_size = 0;

    size_t pos = 12;
    while (pos + 8 <= buf.size()) {
        const unsigned char *chunk = buf.data() + pos;
        const uint32_t chunk_size = le32(chunk + 4);
        const unsigned char *body = chunk + 8;
        if (pos + 8 + chunk_size > buf.size()) break;

        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            format = le16(body + 0);
            channels = le16(body + 2);
            sample_rate = (int) le32(body + 4);
            bits = le16(body + 14);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data = body;
            data_size = chunk_size;
        }

        pos += 8 + chunk_size + (chunk_size & 1u);
    }

    if (!data || data_size == 0 || channels <= 0 || sample_rate <= 0)
        return false;
    if (!((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
          (format == 3 && bits == 32)))
        return false;

    const int bytes = bits / 8;
    const size_t frame_bytes = (size_t) bytes * (size_t) channels;
    if (frame_bytes == 0) return false;
    const size_t frames = data_size / frame_bytes;
    out.clear();
    out.reserve(frames * (size_t) channels);

    for (size_t i = 0; i < frames; ++i) {
        const unsigned char *p = data + i * frame_bytes;
        for (int c = 0; c < channels; ++c, p += bytes) {
            float v = 0.0f;
            if (format == 3 && bits == 32) {
                float tmp = 0.0f;
                std::memcpy(&tmp, p, 4);
                v = tmp;
            } else if (bits == 8) {
                v = ((float) p[0] - 128.0f) / 128.0f;
            } else if (bits == 16) {
                int16_t tmp = 0;
                std::memcpy(&tmp, p, 2);
                v = (float) tmp / 32768.0f;
            } else if (bits == 24) {
                int32_t tmp = (int32_t) ((uint32_t) p[0] << 8 |
                                         (uint32_t) p[1] << 16 |
                                         (uint32_t) p[2] << 24) >> 8;
                v = (float) tmp / 8388608.0f;
            } else {
                int32_t tmp = 0;
                std::memcpy(&tmp, p, 4);
                v = (float) ((double) tmp / 2147483648.0);
            }
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            out.push_back(v);
        }
    }
    return !out.empty();
}

} // namespace

patch_handle_t resolve_vanilla_sound(const char *name) {
    if (!name || !*name) return PATCH_NULL;
    patch_handle_t sound_cls = patchlib_type_get_type("Terraria.ID", "SoundID");
    if (!patchlib_is_valid(sound_cls)) return PATCH_NULL;
    patch_handle_t field = patchlib_type_get_field(sound_cls, name);
    patch_handle_t style = PATCH_NULL;
    if (patchlib_is_valid(field))
        patchlib_field_get_value(field, PATCH_NULL, &style);
    patchlib_free(field);
    patchlib_free(sound_cls);
    return style;
}

int l_audio_load(lua_State *L) {
    const char *rel = luaL_checkstring(L, 1);
    lua_mod_handle_t *handle = get_handle(L);
    const std::string path = resolve_audio_path(handle, rel);
    if (path.empty()) {
        lua_pushnil(L);
        lua_pushfstring(L, "找不到音频文件: %s", rel);
        return 2;
    }

    bool looping = false;
    float volume = 1.0f;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "looping");
        looping = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, 2, "volume");
        if (lua_isnumber(L, -1))
            volume = (float) lua_tonumber(L, -1);
        lua_pop(L, 1);
    }

    std::vector<float> pcm;
    int rate = 0, channels = 0;
    if (!decode_wav(path, pcm, rate, channels)) {
        lua_pushnil(L);
        lua_pushfstring(L, "不支持的音频格式（目前仅 WAV PCM/float）: %s", rel);
        return 2;
    }

    auto *ud = static_cast<LuaAudio *>(
            lua_newuserdatauv(L, sizeof(LuaAudio), 0));
    new (ud) LuaAudio();
    ud->pcm = std::move(pcm);
    ud->source_path = path;
    ud->audio = {};
    ud->audio.source_name = ud->source_path.c_str();
    ud->audio.sample_rate = rate;
    ud->audio.channels = channels;
    ud->audio.format = 5; /* ma_format_f32 */
    ud->audio.pcm_data = ud->pcm.data();
    ud->audio.pcm_frames = (unsigned long long) (ud->pcm.size() / (size_t) channels);
    ud->audio.looping = looping;
    ud->audio.volume = volume > 0.0f ? volume : 1.0f;

    luaL_getmetatable(L, AUDIO_METATABLE);
    lua_setmetatable(L, -2);

    if (!terraria_audio_manager_register ||
        !terraria_audio_manager_register(&ud->audio)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "audio_manager_register 失败");
        return 2;
    }
    ud->registered = true;
    LOG_INFO("[mod.audio] loaded {} ({} Hz, {} ch, {} frames)",
             path, rate, channels, (unsigned long long) ud->audio.pcm_frames);
    return 1;
}

int l_audio_play(lua_State *L) {
    LuaAudio *a = check_audio(L, 1);
    if (!a || !a->registered) return 0;
    const float volume = lua_isnumber(L, 2) ? (float) lua_tonumber(L, 2) : -1.0f;
    const float pitch = lua_isnumber(L, 3) ? (float) lua_tonumber(L, 3) : 1.0f;
    const int id = terraria_audio_manager_play(&a->audio, volume, pitch);
    if (id < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, id);
    return 1;
}

int l_audio_play_at(lua_State *L) {
    LuaAudio *a = check_audio(L, 1);
    if (!a || !a->registered) return 0;
    const float x = (float) luaL_checknumber(L, 2);
    const float y = (float) luaL_checknumber(L, 3);
    const float volume = lua_isnumber(L, 4) ? (float) lua_tonumber(L, 4) : -1.0f;
    const float pitch = lua_isnumber(L, 5) ? (float) lua_tonumber(L, 5) : 1.0f;
    const int id = terraria_audio_manager_play_at(&a->audio, volume, pitch, x, y);
    if (id < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, id);
    return 1;
}

int l_audio_stop_all(lua_State *L) {
    LuaAudio *a = check_audio(L, 1);
    if (a && a->registered) terraria_audio_manager_stop_all(&a->audio);
    return 0;
}

int l_audio_unload(lua_State *L) {
    LuaAudio *a = check_audio(L, 1);
    if (a && a->registered) {
        terraria_audio_manager_stop_all(&a->audio);
        terraria_audio_manager_unregister(&a->audio);
        a->registered = false;
    }
    return 0;
}

int l_audio_pause(lua_State *L) { terraria_audio_manager_pause((int) luaL_checkinteger(L, 1)); return 0; }
int l_audio_resume(lua_State *L) { terraria_audio_manager_resume((int) luaL_checkinteger(L, 1)); return 0; }
int l_audio_stop(lua_State *L) { terraria_audio_manager_stop((int) luaL_checkinteger(L, 1)); return 0; }

int l_audio_set_volume(lua_State *L) {
    terraria_audio_manager_set_volume((int) luaL_checkinteger(L, 1),
                                      (float) luaL_checknumber(L, 2));
    return 0;
}

int l_audio_is_playing(lua_State *L) {
    lua_pushboolean(L, terraria_audio_manager_is_playing(
            (int) luaL_checkinteger(L, 1)) ? 1 : 0);
    return 1;
}

int l_audio_set_master_volume(lua_State *L) {
    terraria_audio_manager_set_master_volume((float) luaL_checknumber(L, 1));
    return 0;
}

namespace {

int l_audio_play_vanilla(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    const int x = lua_isnumber(L, 2) ? (int) lua_tointeger(L, 2) : -1;
    const int y = lua_isnumber(L, 3) ? (int) lua_tointeger(L, 3) : -1;
    const float pitch = lua_isnumber(L, 4) ? (float) lua_tonumber(L, 4) : 0.0f;
    const float volume = lua_isnumber(L, 5) ? (float) lua_tonumber(L, 5) : 1.0f;

    patch_handle_t style = resolve_vanilla_sound(name);
    if (!patchlib_is_valid(style)) {
        lua_pushnil(L);
        lua_pushfstring(L, "找不到 SoundID.%s", name);
        return 2;
    }
    patch_handle_t engine = patchlib_type_get_type("Terraria.Audio", "SoundEngine");
    patch_handle_t method = patchlib_is_valid(engine)
            ? patchlib_type_get_method_by_param_count(engine, "PlaySound", 5)
            : PATCH_NULL;
    bool ok = false;
    if (patchlib_is_valid(method)) {
        void *args[5] = {&style, (void *) &x, (void *) &y,
                         (void *) &pitch, (void *) &volume};
        ok = patchlib_method_invoke_args(method, PATCH_NULL, PATCH_NULL, args);
    }
    patchlib_free(method);
    patchlib_free(engine);
    patchlib_free(style);
    if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, "SoundEngine.PlaySound 调用失败");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

const luaL_Reg audio_methods[] = {
        {"play", l_audio_play},
        {"play_at", l_audio_play_at},
        {"stop_all", l_audio_stop_all},
        {"unload", l_audio_unload},
        {nullptr, nullptr},
};

const luaL_Reg audio_functions[] = {
        {"load", l_audio_load},
        {"play_vanilla", l_audio_play_vanilla},
        {"pause", l_audio_pause},
        {"resume", l_audio_resume},
        {"stop", l_audio_stop},
        {"set_volume", l_audio_set_volume},
        {"is_playing", l_audio_is_playing},
        {"set_master_volume", l_audio_set_master_volume},
        {nullptr, nullptr},
};

} // namespace

void add_audio(lua_State *L) {
    luaL_newmetatable(L, AUDIO_METATABLE);
    lua_pushcfunction(L, audio_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, audio_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    luaL_setfuncs(L, audio_functions, 0);
}

} // namespace lualoader::lua_api
