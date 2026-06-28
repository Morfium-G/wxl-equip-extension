// wxl-equip-extension: equipment slot extension features for WarcraftXL.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "EquipExtension.hpp"
#include "VirtualPath.hpp"
#include "events/Event.hpp"
#include "game/Binding.hpp"
#include "game/m2/M2.hpp"
#include "offsets/game/DB2.hpp"
#include "offsets/game/M2.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace wxl::scripts::equipextension
{
    namespace ev   = wxl::events;
    namespace m2   = wxl::offsets::game::m2;
    namespace db2  = wxl::offsets::game::db2;
    namespace gm2  = wxl::game::m2;
    using wxl::game::Native;

    // ─── Data types ──────────────────────────────────────────────────────────────

    // List of skinSectionIds to show. count == 0 means "show all" (no filter applied).
    // WoW geoset IDs are two-part values (e.g. 1301 = group 13, option 01).
    struct GeosetFilter
    {
        uint16_t ids[16];
        uint32_t count;
    };

    // Maps collection bone index → character bone index (0xFF = unmatched).
    // Built by 3-pass matching: (1a) key_bone_id raw scan, (1b) BoneIndicesByID LUT,
    // (1c) BoneNameCRC, then (2) parent-chain propagation.
    struct BoneRemap
    {
        uint8_t count;
        uint8_t collToChar[48];
    };

    // One attachment entry per attached M2. Keyed by CharModelObject pointer in g_attached.
    struct AttachEntry
    {
        uint32_t    equipSlot = static_cast<uint32_t>(-1); // internal model slot (0-10)
        uint32_t    attachId  = static_cast<uint32_t>(-1); // WoW attachment point id
        void*       subObj    = nullptr; // cmo->sceneNode stamped at last rebuild
        void*       renderCtx = nullptr; // live render context; null = evicted/dead
        char        keyBuf[264] = {};        // real M2 path (used for matching/dedup)
        char        mangledKeyBuf[264] = {}; // virtual _wxl_ path for GetRenderCtx (collection only)
        char        texBuf[264] = {};    // BLP path for BindTexSlot on re-attach
        GeosetFilter geoFilter  = {};
        BoneRemap    boneRemap  = {};
        bool        perFrameLogged    = false; // suppress repeated per-frame noise after first log
        bool        charSweepApplied = false; // set when character-model PerFrame sweep first copies bones
        bool        bbpLogDone       = false; // suppress OnBuildBonePalette remap dump after first fire
    };

    // ─── File-scope state ─────────────────────────────────────────────────────────

    // cmo (CharModelObject*) → attached M2 entries for that character
    static std::unordered_map<void*, std::vector<AttachEntry>> g_attached;

    // Set to the cmo being processed in RebuildAllModels Phase1. OnM2SkinFinalize uses
    // this to apply the filter during a synchronous load that fires kFinalizeSkin inside
    // GetRenderCtx, before Phase1 has set entry.renderCtx.
    static void* g_currentRebuildCmo = nullptr;

    // ─── Slot config ──────────────────────────────────────────────────────────────

    struct SlotConfig
    {
        const char* folder;
        uint32_t    defAttach1;
        uint32_t    defAttach2;
        bool        defUseRace;
        bool        defUseGender;
    };

    // Indexed by internal model slot (0-10), matching sub_4f2640's first arg.
    static const SlotConfig kSlotConfig[11] = {
        { "Head",     11,  55, true,  true  }, // 0  HEAD
        { "Shoulder",  6,   5, false, false }, // 1  SHOULDER (Model1=left/6, Model2=right/5)
        { "Shirt",    34,  34, false, false }, // 2  SHIRT
        { "Chest",    34,  34, false, false }, // 3  CHEST
        { "Waist",    53,  53, false, false }, // 4  WAIST
        { "Leg",       9,  10, false, false }, // 5  LEGS
        { "Foot",     47,  48, false, false }, // 6  FEET
        { "Bracer",    3,   4, false, false }, // 7  WRIST
        { "Glove",     1,   2, false, false }, // 8  HAND
        { "Cape",     12,  12, false, false }, // 9  BACK
        { "Tabard",   34,  34, false, false }, // 10 TABARD
    };

    // WoW C++ equip slot (0-18) → internal model slot. UINT32_MAX = not handled here.
    static const uint32_t kEquipToModelSlot[19] = {
        0,                       // 0  HEAD
        static_cast<uint32_t>(-1), // 1  NECK
        1,                       // 2  SHOULDER
        2,                       // 3  SHIRT
        3,                       // 4  CHEST
        4,                       // 5  WAIST
        5,                       // 6  LEGS
        6,                       // 7  FEET
        7,                       // 8  WRIST
        8,                       // 9  HAND
        static_cast<uint32_t>(-1), // 10 deferred
        static_cast<uint32_t>(-1), // 11 deferred
        static_cast<uint32_t>(-1), // 12 deferred
        static_cast<uint32_t>(-1), // 13 deferred
        9,                       // 14 BACK
        static_cast<uint32_t>(-1), // 15 deferred (weapons)
        static_cast<uint32_t>(-1), // 16 deferred (weapons)
        static_cast<uint32_t>(-1), // 17 deferred (weapons)
        10,                      // 18 TABARD
    };

    // ─── Debug logging ───────────────────────────────────────────────────────────

    // Append a line to WarcraftXL_equip.log in the game directory.
    // fopen("a") is safe to call from any thread; disk I/O only, no WoW state touched.
    // Toggle off by deleting or renaming the log file between sessions.
    static void EquipLog(const char* fmt, ...) noexcept
    {
#pragma warning(suppress: 4996)
        FILE* f = std::fopen("WarcraftXL_equip.log", "a");
        if (!f) return;
        va_list ap; va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        va_end(ap);
        std::fputc('\n', f);
        std::fclose(f);
    }

    // ─── SEH helpers (no C++ objects — safe to use __try/__except) ───────────────

    static uint32_t GuardedReadU32(const void* ptr) noexcept
    {
        uint32_t v = 0;
        __try { v = *static_cast<const uint32_t*>(ptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return v;
    }

    static void* GuardedReadPtr(const void* addr) noexcept
    {
        void* v = nullptr;
        __try { v = *static_cast<void* const*>(addr); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return v;
    }

    // SEH-only wrapper: no C++ objects in this function, __try is safe.
    // Catches access violations from garbage/dangling hash-table buckets — see RebuildAllModels.
    // owner28 = *(subObj + kOffSceneNodeOwner): the actual CharModelObject that owns the
    // M2 hash table — NOT the slot-dispatch 'cmo' from hkSlotDispatch, which is a different
    // (wrapper) object whose +4 hash-table pointer is always null.
    static void* SafeGetRenderCtx(void* owner28, const char* keyBuf) noexcept
    {
        void* result = nullptr;
        __try { result = gm2::GetRenderCtx(owner28, const_cast<char*>(keyBuf)); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return result;
    }

    // SEH wrapper for BuildBoneRemap's pointer walks.
    // BoneRemap is a POD struct (trivially destructible), so __try is safe inside this function.
    static BoneRemap BuildBoneRemapGuarded(void* collRc, void* charRc) noexcept
    {
        BoneRemap r = {};
        std::memset(r.collToChar, 0xFF, sizeof(r.collToChar));
        __try
        {
            auto* collBytes = reinterpret_cast<uint8_t*>(collRc);
            auto* charBytes = reinterpret_cast<uint8_t*>(charRc);

            void* collM2  = *reinterpret_cast<void**>(collBytes + m2::kOffInstModel);
            void* charM2  = *reinterpret_cast<void**>(charBytes + m2::kOffInstModel);
            if (!collM2 || !charM2) return r;

            // M2AnimData = raw M2 file buffer = M2FileHeader (at m2_inst+0x150)
            auto* collHdr = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(collM2) + m2::kOffModelHeader);
            auto* charHdr = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(charM2) + m2::kOffModelHeader);
            if (!collHdr || !charHdr) return r;

            uint32_t collN    = *reinterpret_cast<uint32_t*>(collHdr + m2::kOffHdrBoneCount);
            uint8_t* collBase = *reinterpret_cast<uint8_t**>(collHdr + m2::kOffHdrBoneArray);

            uint32_t charN    = *reinterpret_cast<uint32_t*>(charHdr + m2::kOffHdrBoneCount);
            uint8_t* charBase = *reinterpret_cast<uint8_t**>(charHdr + m2::kOffHdrBoneArray);

            uint32_t charLutN = *reinterpret_cast<uint32_t*>(charHdr + m2::kOffHdrBoneIdxLutCount);
            int16_t* charLut  = *reinterpret_cast<int16_t**>(charHdr + m2::kOffHdrBoneIdxLutPtr);

            if (!collBase || collN == 0 || collN > 48) return r;
            r.count = static_cast<uint8_t>(collN);

            // Pass 1a: match by key_bone_id scan of char bone array
            if (charBase)
            {
                for (uint32_t i = 0; i < collN; ++i)
                {
                    int32_t key = *reinterpret_cast<int32_t*>(collBase + i * m2::kBoneStride + m2::kOffBoneKeyId);
                    if (key < 0) continue;
                    for (uint32_t j = 0; j < charN && j < 255; ++j)
                    {
                        if (*reinterpret_cast<int32_t*>(charBase + j * m2::kBoneStride + m2::kOffBoneKeyId) == key)
                        {
                            r.collToChar[i] = static_cast<uint8_t>(j);
                            break;
                        }
                    }
                }
            }

            // Pass 1b: BoneIndicesByID LUT for unmatched bones
            if (charLut)
            {
                for (uint32_t i = 0; i < collN; ++i)
                {
                    if (r.collToChar[i] != 0xFF) continue;
                    int32_t key = *reinterpret_cast<int32_t*>(collBase + i * m2::kBoneStride + m2::kOffBoneKeyId);
                    if (key < 0 || static_cast<uint32_t>(key) >= charLutN) continue;
                    int16_t ci = charLut[key];
                    if (ci >= 0) r.collToChar[i] = static_cast<uint8_t>(ci);
                }
            }

            // Pass 1c: bone name CRC match for remaining unmatched bones
            if (charBase)
            {
                for (uint32_t i = 0; i < collN; ++i)
                {
                    if (r.collToChar[i] != 0xFF) continue;
                    uint32_t collCrc = *reinterpret_cast<uint32_t*>(collBase + i * m2::kBoneStride + m2::kOffBoneNameCrc);
                    if (collCrc == 0) continue;
                    for (uint32_t j = 0; j < charN && j < 255; ++j)
                    {
                        uint32_t charCrc = *reinterpret_cast<uint32_t*>(charBase + j * m2::kBoneStride + m2::kOffBoneNameCrc);
                        if (charCrc == collCrc) { r.collToChar[i] = static_cast<uint8_t>(j); break; }
                    }
                }
            }

            // Pass 2: propagate matched values down unmatched parent chains (up to 8 iterations)
            for (int pass = 0; pass < 8; ++pass)
            {
                bool changed = false;
                for (uint32_t i = 0; i < collN; ++i)
                {
                    if (r.collToChar[i] != 0xFF) continue;
                    int16_t parent = *reinterpret_cast<int16_t*>(collBase + i * m2::kBoneStride + m2::kOffBoneParent);
                    if (parent < 0 || static_cast<uint32_t>(parent) >= collN) continue;
                    if (r.collToChar[parent] != 0xFF)
                    {
                        r.collToChar[i] = r.collToChar[parent];
                        changed = true;
                    }
                }
                if (!changed) break;
            }

            // If no bone was matched at all, signal "not ready" by resetting count to 0.
            // This happens when charBase is null (character M2 not yet parsed, e.g. initFlags=0x40).
            // count=0 causes all callers' retry guards to re-fire until the character is ready.
            {
                uint32_t matched = 0;
                for (uint32_t bi = 0; bi < r.count; ++bi)
                    if (r.collToChar[bi] != 0xFF) ++matched;
                if (matched == 0) r.count = 0;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return r;
    }


    // ─── Parse helpers ────────────────────────────────────────────────────────────

    // flags: 0x1=hide geoset, 0x2=no gender, 0x4=no race,
    //        0x8=append race to tex, 0x10=append gender to tex,
    //        0x20=model in subfolder, 0x40=new _Hu_F suffix

    static GeosetFilter ParseGeosetFilter(const char* spec)
    {
        GeosetFilter f = {};
        if (!spec || !*spec) return f;
        const char* p = spec;
        while (*p && f.count < 16)
        {
            uint32_t v = 0;
            while (*p >= '0' && *p <= '9') v = v * 10 + static_cast<uint32_t>(*p++ - '0');
            f.ids[f.count++] = static_cast<uint16_t>(v);
            if (*p == ',') p++;
            else break;
        }
        return f;
    }

    static void ParseAttachField(const char* start, const char* end,
                                  uint32_t* leftOut, uint32_t* rightOut)
    {
        uint32_t a = 0;
        const char* p = start;
        while (p < end && *p >= '0' && *p <= '9') a = a * 10 + static_cast<uint32_t>(*p++ - '0');
        *leftOut = a;
        if (p < end && *p == '|')
        {
            p++;
            uint32_t b = 0;
            while (p < end && *p >= '0' && *p <= '9') b = b * 10 + static_cast<uint32_t>(*p++ - '0');
            *rightOut = b;
        }
        else { *rightOut = a; }
    }

    // Icon2 format: "<attach1>:<attach2>:<flags>:<customfolder>" — each field optional.
    // Caller sets attach/flag defaults; only fields present in the string override them.
    // Guard: only call when icon2str starts with ':' or a digit.
    static void ParseIcon2(const char* icon2str,
                            uint32_t* attachA_l, uint32_t* attachA_r,
                            uint32_t* attachB_l, uint32_t* attachB_r,
                            uint32_t* flags,
                            char* customFolder, size_t customFolderSz)
    {
        *flags = 0;
        if (customFolder) customFolder[0] = '\0';
        if (!icon2str || !*icon2str) return;

        const char* f2 = icon2str;
        while (*f2 && *f2 != ':') f2++;
        if (icon2str != f2) ParseAttachField(icon2str, f2, attachA_l, attachA_r);
        if (*f2 != ':') return;

        const char* f3 = f2 + 1;
        while (*f3 && *f3 != ':') f3++;
        if (f2 + 1 != f3) ParseAttachField(f2 + 1, f3, attachB_l, attachB_r);
        if (*f3 != ':') return;

        const char* f4 = f3 + 1;
        while (*f4 && *f4 != ':') f4++;
        if (f3 + 1 != f4)
        {
            uint32_t v = 0;
            for (const char* p = f3 + 1; p < f4; p++) v = v * 10 + static_cast<uint32_t>(*p - '0');
            *flags = v;
        }
        if (*f4 != ':') return;

        if (customFolder && customFolderSz > 1)
        {
            size_t len = std::strlen(f4 + 1);
            if (len >= customFolderSz) len = customFolderSz - 1;
            std::memcpy(customFolder, f4 + 1, len);
            customFolder[len] = '\0';
        }
    }

    // ─── Path builders ────────────────────────────────────────────────────────────

    // Builds the virtual M2 path: "Item\ObjectComponents\<folder>\[<stem>\]<stem>_<race>[_]<gender>.mdx"
    // Folder priority: customFolder > (isCollection ? "Collections" : slotFolder).
    // flags: 0x2=no gender, 0x4=no race, 0x20=subfolder, 0x40=new _Hu_F underscore between race+gender.
    static void BuildSlotPath(char* buf, const char* modelName,
                               const char* raceCode, const char* genderStr,
                               uint32_t flags, const char* slotFolder, bool isCollection,
                               const char* customFolder = nullptr)
    {
        bool appendRace   = !(flags & 0x4);
        bool appendGender = !(flags & 0x2);
        bool subFolder    = (flags & 0x20) != 0;
        bool newSuffix    = (flags & 0x40) != 0;

        const char* folder = slotFolder;
        if (customFolder && *customFolder) folder = customFolder;
        else if (isCollection)             folder = "Collections";

        char* p = buf;
        for (const char* s = "Item\\ObjectComponents\\"; *s; ) *p++ = *s++;
        for (const char* s = folder; *s; )                     *p++ = *s++;
        *p++ = '\\';

        const char* lastDot = nullptr;
        for (const char* s = modelName; *s; s++) if (*s == '.') lastDot = s;
        size_t stemLen = lastDot ? static_cast<size_t>(lastDot - modelName) : std::strlen(modelName);

        if (subFolder)
        {
            for (size_t i = 0; i < stemLen; ++i) *p++ = modelName[i];
            *p++ = '\\';
        }
        for (size_t i = 0; i < stemLen; ++i) *p++ = modelName[i];

        if (!appendRace && !appendGender)
        {
            *p++ = '.'; *p++ = 'm'; *p++ = 'd'; *p++ = 'x'; *p = '\0';
            return;
        }

        *p++ = '_';
        if (appendRace)   for (const char* s = raceCode;  *s; ) *p++ = *s++;
        if (newSuffix && appendRace && appendGender) *p++ = '_';
        if (appendGender) for (const char* s = genderStr; *s; ) *p++ = *s++;
        *p++ = '.'; *p++ = 'm'; *p++ = 'd'; *p++ = 'x'; *p = '\0';
    }

    // Builds the virtual BLP texture path.
    // flags: 0x8=append race, 0x10=append gender, 0x20=subfolder, 0x40=new underscore.
    // modelStem: when 0x20 set, names the subfolder after the model stem (not the texture name).
    static void BuildTexPath(char* buf, size_t bufSz, const char* texName,
                              const char* raceCode, const char* genderStr,
                              uint32_t flags, const char* slotFolder, bool isCollection,
                              const char* customFolder = nullptr, const char* modelStem = nullptr)
    {
        bool appendRace   = (flags & 0x8) != 0;
        bool appendGender = (flags & 0x10) != 0;
        bool subFolder    = (flags & 0x20) != 0;
        bool newSuffix    = (flags & 0x40) != 0;

        const char* folder = slotFolder;
        if (customFolder && *customFolder) folder = customFolder;
        else if (isCollection)             folder = "Collections";

        const char* subBase = (modelStem && *modelStem) ? modelStem : texName;
        size_t subBaseLen = std::strlen(subBase);
        for (const char* s = subBase; *s; s++)
            if (*s == '.') { subBaseLen = static_cast<size_t>(s - subBase); break; }

        char tmp[264];
        char* p = tmp;
        for (const char* s = "Item\\ObjectComponents\\"; *s; ) *p++ = *s++;
        for (const char* s = folder; *s; )                     *p++ = *s++;
        *p++ = '\\';
        if (subFolder)
        {
            for (size_t i = 0; i < subBaseLen; ++i) *p++ = subBase[i];
            *p++ = '\\';
        }
        for (const char* s = texName; *s; ) *p++ = *s++;
        if (appendRace || appendGender)
        {
            *p++ = '_';
            if (appendRace)   for (const char* s = raceCode;  *s; ) *p++ = *s++;
            if (newSuffix && appendRace && appendGender) *p++ = '_';
            if (appendGender) for (const char* s = genderStr; *s; ) *p++ = *s++;
        }
        *p++ = '.'; *p++ = 'b'; *p++ = 'l'; *p++ = 'p'; *p = '\0';
        std::strncpy(buf, tmp, bufSz - 1);
        buf[bufSz - 1] = '\0';
    }

    // ─── Geoset filter ────────────────────────────────────────────────────────────

    // Zeros rawTri (skin->indices) for submeshes whose skinSectionId is not in the filter.
    // Must run before kFinalizeSkin builds the D3D index buffer — the GPU IB is static after
    // that point. Called from OnM2SkinFinalize (the primary path) and Phase3 (fallback for
    // the rare sync-load case where the model is already available at equip time).
    static void ApplyRawTriFilter(gm2::M2SkinProfile* skin, const GeosetFilter& filter) noexcept
    {
        if (!skin || filter.count == 0) return;
        __try
        {
            if (!skin->indices || !skin->submeshes || skin->submeshCount == 0) return;
            EquipLog("  ApplyRawTriFilter: submeshCount=%u indexCount=%u filter=[%u %u %u %u](n=%u)",
                     skin->submeshCount, skin->indexCount,
                     filter.count > 0 ? filter.ids[0] : 0,
                     filter.count > 1 ? filter.ids[1] : 0,
                     filter.count > 2 ? filter.ids[2] : 0,
                     filter.count > 3 ? filter.ids[3] : 0,
                     filter.count);
            for (uint32_t si = 0; si < skin->submeshCount; ++si)
            {
                uint16_t secId = skin->submeshes[si].skinSectionId;
                uint16_t lvl   = skin->submeshes[si].level;
                uint16_t start16 = skin->submeshes[si].indexStart;
                uint16_t count16 = skin->submeshes[si].indexCount;
                // skinSectionId=0 is the base/untagged mesh; always keep it.
                bool visible = (secId == 0);
                if (!visible)
                    for (uint32_t fi = 0; fi < filter.count; ++fi)
                        if (filter.ids[fi] == secId) { visible = true; break; }
                EquipLog("    submesh[%u]: skinSectionId=%u level=%u indexStart=%u indexCount=%u -> %s",
                         si, (uint32_t)secId, (uint32_t)lvl, (uint32_t)start16, (uint32_t)count16,
                         visible ? "KEEP" : "ZERO");
                if (!visible)
                {
                    uint32_t start = start16;
                    uint32_t count = count16;
                    if (count == 0 || start + count > skin->indexCount) continue;
                    std::memset(skin->indices + start, 0, count * sizeof(uint16_t));
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ─── Detach / rebuild ─────────────────────────────────────────────────────────

    // Erases all entries for (cmo, equipSlot) and issues the minimal set of DetachSlot calls
    // (deduplicating by attach_id to avoid double-detach corrupting the scene-graph linked list).
    static void DetachSlotEntries(void* cmo, uint32_t equipSlot)
    {
        auto it = g_attached.find(cmo);
        if (it == g_attached.end()) return;

        // Current live sub_obj (read fresh; may differ from stored sub_obj after re-login).
        void* curSubObj = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);

        struct DetachPair { void* subObj; uint32_t attachId; };
        DetachPair pairs[8]; uint32_t nPairs = 0;
        auto addPair = [&](void* so, uint32_t aid)
        {
            if (!so || aid == static_cast<uint32_t>(-1) || nPairs >= 8) return;
            for (uint32_t k = 0; k < nPairs; ++k)
                if (pairs[k].subObj == so && pairs[k].attachId == aid) return;
            pairs[nPairs++] = { so, aid };
        };

        auto& vec = it->second;
        for (size_t i = 0; i < vec.size(); )
        {
            AttachEntry& e = vec[i];
            if (e.equipSlot != equipSlot) { ++i; continue; }
            if (e.renderCtx)
                addPair(curSubObj ? curSubObj : e.subObj, e.attachId);
            vec.erase(vec.begin() + static_cast<ptrdiff_t>(i));
        }

        for (uint32_t k = 0; k < nPairs; ++k)
            gm2::DetachSlot(pairs[k].subObj, pairs[k].attachId);

        if (vec.empty()) g_attached.erase(it);
    }

    // Detaches all unique attach_ids in g_attached[cmo], then re-attaches each unique
    // (keyBuf, attachId, texBuf) combination via GetRenderCtx → (BindTexSlot) → AttachToScene.
    // render_ctx values in entries are rebuilt transient each time; called on every equip/unequip.
    static void RebuildAllModels(void* cmo)
    {
        auto it = g_attached.find(cmo);
        if (it == g_attached.end() || it->second.empty()) return;
        auto& entries = it->second;

        void* subObj = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);
        if (!subObj) { EquipLog("  RebuildAllModels: subObj null, bail"); return; }

        // GetRenderCtx (sub_81f8f0) must be called with the scene-node's owner (owner28), NOT
        // with our slot-dispatch 'cmo'. The slot-dispatch ECX is a wrapper object whose +4
        // hash-table pointer is always null. The real CMO — the object that owns the M2 hash
        // table — is the scene node's back-pointer at kOffSceneNodeOwner (+0x28). This matches
        // how vanilla's sub_4eaa70 calls sub_81f8f0: it dereferences *(sub_obj+0x28) first.
        // If owner28 is null the CMO is not yet fully initialised; bail so OnM2PerFrameUpdate
        // can retry via the pending-attach sweep.
        void* owner28 = GuardedReadPtr(reinterpret_cast<uint8_t*>(subObj) + m2::kOffSceneNodeOwner);
        if (!owner28) { EquipLog("  RebuildAllModels: owner28 null, bail (deferred)"); return; }

        EquipLog("  RebuildAllModels: %zu entries, subObj=0x%p owner28=0x%p",
                 entries.size(), subObj, owner28);

        // Reset all cached render_ctxes, stamp current subObj, reset per-frame log flag.
        for (auto& e : entries) { e.renderCtx = nullptr; e.subObj = subObj; e.perFrameLogged = false; }

        // Pre-phase: build virtual paths for collection entries and populate the serve table.
        // Each unique (keyBuf, attachId, texBuf) collection group gets one merged geoset filter and one
        // virtual key encoding (cmo × model × geosets × texture). GetRenderCtx with this key
        // guarantees a cache miss on every distinct filter/texture combination, forcing a fresh
        // async load and an OnM2SkinFinalize fire with the correct merged filter.
        for (size_t i = 0; i < entries.size(); ++i)
        {
            AttachEntry& e = entries[i];
            if (e.geoFilter.count == 0) continue;
            // Skip if a sibling already processed this (keyBuf, attachId, texBuf) group.
            bool done = false;
            for (size_t j = 0; j < i; ++j)
            {
                AttachEntry& prev = entries[j];
                if (prev.geoFilter.count > 0 && prev.attachId == e.attachId &&
                    std::strcmp(prev.keyBuf, e.keyBuf) == 0 &&
                    std::strcmp(prev.texBuf,  e.texBuf)  == 0) { done = true; break; }
            }
            if (done) continue;

            // Merge geoset IDs within this (keyBuf, attachId, texBuf) group.
            // texBuf is fixed per group — e.texBuf is the canonical texture for this virtual model.
            uint16_t mergedIds[16]; uint32_t mergedCount = 0;
            for (auto& e2 : entries)
            {
                if (e2.geoFilter.count == 0 || e2.attachId != e.attachId ||
                    std::strcmp(e2.keyBuf, e.keyBuf) != 0 ||
                    std::strcmp(e2.texBuf,  e.texBuf)  != 0) continue;
                for (uint32_t fi = 0; fi < e2.geoFilter.count && mergedCount < 16; ++fi)
                {
                    bool dup = false;
                    for (uint32_t fj = 0; fj < mergedCount; ++fj)
                        if (mergedIds[fj] == e2.geoFilter.ids[fi]) { dup = true; break; }
                    if (!dup) mergedIds[mergedCount++] = e2.geoFilter.ids[fi];
                }
            }

            // Build the virtual key and ensure bytes are in the serve table.
            char mangled[264];
            VPathBuildKey(mangled, sizeof(mangled), cmo, e.keyBuf, mergedIds, mergedCount, e.texBuf);
            VPathPopulate(cmo, e.keyBuf, mergedIds, mergedCount, e.texBuf);
            EquipLog("  VPath: '%s' tex='%s' -> '%s' (merged=%u)", e.keyBuf, e.texBuf, mangled, mergedCount);

            // Propagate the mangled key to all entries in this (keyBuf, attachId, texBuf) group.
            for (auto& e2 : entries)
            {
                if (e2.geoFilter.count > 0 && e2.attachId == e.attachId &&
                    std::strcmp(e2.keyBuf, e.keyBuf) == 0 &&
                    std::strcmp(e2.texBuf,  e.texBuf)  == 0)
                    std::memcpy(e2.mangledKeyBuf, mangled, sizeof(e2.mangledKeyBuf));
            }
        }

        // Phase 1 — GetRenderCtx before DetachSlot.
        // hkSlotDispatch (GameHooks.cpp) runs vanilla first: it calls GetRenderCtx + AttachToScene
        // + ReleaseRenderCtx, leaving refcount=1 held by the scene. If we called DetachSlot first
        // the scene would release its ref (refcount→0, node freed), leaving a dangling pointer in
        // the hash bucket. Subsequent GetRenderCtx reads that dangling value and crashes at
        // sub_81c390+0x170 (mov eax,[esi+0x14c]) where esi=0x206 (freed-heap garbage).
        // Getting our ref first keeps the node alive through the detach step.
        //
        // g_currentRebuildCmo enables OnM2SkinFinalize to apply the geoset filter during the
        // synchronous load that fires kFinalizeSkin inside GetRenderCtx for virtual-path entries
        // (in-process byte serving has no IPC delay, so the model loads and finalizes before
        // GetRenderCtx returns — before entry.renderCtx is set). Cleared after Phase1.
        g_currentRebuildCmo = cmo;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            AttachEntry& e = entries[i];
            if (e.renderCtx) continue;  // already set by a sibling

            const char* p1Key = (e.geoFilter.count > 0 && e.mangledKeyBuf[0])
                                 ? e.mangledKeyBuf : e.keyBuf;
            void* rctx = SafeGetRenderCtx(owner28, p1Key);
            EquipLog("  Phase1 GetRenderCtx key='%s' -> 0x%p", p1Key, rctx);
            if (!rctx) continue;

            // Propagate to all siblings sharing (keyBuf, attachId, texBuf).
            for (size_t j = i; j < entries.size(); ++j)
            {
                AttachEntry& e2 = entries[j];
                if (!e2.renderCtx && e2.attachId == e.attachId &&
                    std::strcmp(e2.keyBuf, e.keyBuf) == 0 &&
                    std::strcmp(e2.texBuf,  e.texBuf)  == 0)
                    e2.renderCtx = rctx;
            }
        }
        g_currentRebuildCmo = nullptr;

        // Phase 2 — DetachSlot all unique attach_ids (dedup avoids double-detach crash in sub_827560).
        // Also detach the slot config's vanilla default attach points: g_origSlotDispatch (which runs
        // before our event) calls sub_4eaa70 which attaches to defAttach1/defAttach2. Without removing
        // these, the vanilla model shows at its default position alongside our custom attachments.
        uint32_t detached[22] = {}; uint32_t nDetached = 0;
        auto detachOnce = [&](uint32_t aid) {
            if (aid == static_cast<uint32_t>(-1) || nDetached >= 22) return;
            for (uint32_t i = 0; i < nDetached; ++i) if (detached[i] == aid) return;
            gm2::DetachSlot(subObj, aid);
            detached[nDetached++] = aid;
        };
        for (auto& e : entries)
        {
            detachOnce(e.attachId);
            if (e.equipSlot < 11)
            {
                detachOnce(kSlotConfig[e.equipSlot].defAttach1);
                detachOnce(kSlotConfig[e.equipSlot].defAttach2);
            }
        }

        // Phase 3 — Bind texture, AttachToScene, geoset filter, release ref.
        // Process each unique (keyBuf, attachId, texBuf) once; siblings share the same render_ctx.
        for (size_t i = 0; i < entries.size(); ++i)
        {
            AttachEntry& e = entries[i];
            if (!e.renderCtx)
            {
                // renderCtx is null for two reasons:
                //   (a) Collection entry evicted by Phase 2.5 (model was loaded): re-acquire fresh rctx.
                //   (b) Non-collection entry whose Phase1 GetRenderCtx failed: skip.
                // Loading collection entries are NOT nulled by Phase 2.5; they reach here with a
                // non-null renderCtx (Phase1's rctx) and fall through to the attach below.
                if (e.geoFilter.count == 0) continue;
                const char* p3Key = e.mangledKeyBuf[0] ? e.mangledKeyBuf : e.keyBuf;
                void* rctx = SafeGetRenderCtx(owner28, p3Key);
                EquipLog("  Phase3[%zu] collection re-acquire key='%s' -> 0x%p", i, p3Key, rctx);
                if (!rctx) continue;
                e.renderCtx = rctx;
                for (size_t j = i + 1; j < entries.size(); ++j)
                {
                    AttachEntry& e2 = entries[j];
                    if (!e2.renderCtx && e2.geoFilter.count > 0 &&
                        e2.attachId == e.attachId &&
                        std::strcmp(e2.keyBuf, e.keyBuf) == 0 &&
                        std::strcmp(e2.texBuf,  e.texBuf)  == 0)
                        e2.renderCtx = rctx;
                }
            }

            // Skip siblings — only the first occurrence of each (keyBuf, attachId, texBuf) does the attach.
            bool alreadyAttached = false;
            for (size_t j = 0; j < i; ++j)
            {
                AttachEntry& prev = entries[j];
                if (prev.renderCtx && prev.attachId == e.attachId &&
                    std::strcmp(prev.keyBuf, e.keyBuf) == 0 &&
                    std::strcmp(prev.texBuf,  e.texBuf)  == 0)
                { alreadyAttached = true; break; }
            }
            if (alreadyAttached) continue;

            void* rctx = e.renderCtx;
            bool isCollection = (e.geoFilter.count > 0);

            if (e.texBuf[0])
            {
                void* tex = gm2::LoadResource(e.texBuf, 0);
                if (tex) { gm2::BindTexSlot(rctx, tex); gm2::ReleaseResource(tex); }
            }

            // subObj->initFlags & 1: when set (character fully initialised), sub_831630 checks the
            // BoneIndicesByID LUT; if the attach point is absent and zero2==0 it exits silently.
            // Passing forceAttach=true (zero2=1) bypasses the early-exit for non-standard points
            // (e.g. attach_id=19) that collection M2s use.
            uint32_t subInitFlags = GuardedReadU32(reinterpret_cast<uint8_t*>(subObj) + m2::kOffInstInitFlags);
            EquipLog("  Phase3[%zu] attach=%u rctx=0x%p subObj_initFlags=0x%X isCollection=%d",
                     i, e.attachId, rctx, subInitFlags, (int)isCollection);
            gm2::AttachToScene(rctx, subObj, e.attachId, isCollection);

            BoneRemap remap = BuildBoneRemapGuarded(rctx, subObj);
            EquipLog("  Phase3[%zu] boneRemap.count=%u", i, (uint32_t)remap.count);

            // Propagate boneRemap to siblings sharing (keyBuf, attachId, texBuf).
            for (auto& e2 : entries)
            {
                if (e2.attachId != e.attachId ||
                    std::strcmp(e2.keyBuf, e.keyBuf) != 0 ||
                    std::strcmp(e2.texBuf,  e.texBuf)  != 0) continue;
                if (remap.count > 0) e2.boneRemap = remap;
            }

            // Apply merged geoset filter for entries sharing this (keyBuf, attachId, texBuf).
            if (isCollection)
            {
                GeosetFilter merged = {};
                for (auto& e2 : entries)
                {
                    if (e2.attachId != e.attachId ||
                        std::strcmp(e2.keyBuf, e.keyBuf) != 0 ||
                        std::strcmp(e2.texBuf,  e.texBuf)  != 0) continue;
                    for (uint32_t fi = 0; fi < e2.geoFilter.count && merged.count < 16; ++fi)
                    {
                        bool dup = false;
                        for (uint32_t fj = 0; fj < merged.count; ++fj)
                            if (merged.ids[fj] == e2.geoFilter.ids[fi]) { dup = true; break; }
                        if (!dup) merged.ids[merged.count++] = e2.geoFilter.ids[fi];
                    }
                }
                EquipLog("  Phase3[%zu] geoFilter merged.count=%u ids=[%u %u %u %u]",
                         i, merged.count,
                         merged.count > 0 ? merged.ids[0] : 0,
                         merged.count > 1 ? merged.ids[1] : 0,
                         merged.count > 2 ? merged.ids[2] : 0,
                         merged.count > 3 ? merged.ids[3] : 0);
                void* mdl = GuardedReadPtr(reinterpret_cast<uint8_t*>(rctx) + m2::kOffInstModel);
                auto* skin = mdl ? gm2::Skin(mdl) : nullptr;
                if (skin)
                {
                    // The GPU IB was already built by kFinalizeSkin during Phase1's synchronous
                    // GetRenderCtx load. OnM2SkinFinalize (fallback path via g_currentRebuildCmo)
                    // applied the merged filter before the IB was written. Do not call
                    // gm2::FinalizeSkin here: it calls kFinalizeSkin (0x837A40) directly,
                    // bypassing sub_838490's post-call buffer-fill step. That creates a new empty
                    // D3D buffer that never gets filled, making the model invisible.
                    EquipLog("  Phase3[%zu] skin present: IB already built by synchronous load, skip re-finalize", i);
                }
                else
                {
                    EquipLog("  Phase3[%zu] skin null: OnM2SkinFinalize will apply filter async", i);
                }
            }

            // Release our Phase-1 ref. The scene holds a ref from AttachToScene, so rctx stays
            // alive. e.renderCtx continues to point to it and is used by OnM2PerFrameUpdate.
            gm2::ReleaseRenderCtx(rctx);
        }
    }

    // ─── Event handler implementations ───────────────────────────────────────────

    void EquipExtension::OnItemSlotChange(const ev::ItemSlotChangeArgs& a)
    {
        if (a.modelSlot >= 11) return;
        const SlotConfig& cfg = kSlotConfig[a.modelSlot];

        void* cmo = a.charModelObj;
        void* subObj = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);

        // Unequip path: display_id == 0 when the slot is being cleared via the equip hook.
        uint32_t displayId = 0;
        if (a.itemDataPtr)
            displayId = GuardedReadU32(a.itemDataPtr);

        EquipLog("--- OnItemSlotChange cmo=0x%p slot=%u idp=0x%p displayId=%u subObj=0x%p",
                 cmo, a.modelSlot, a.itemDataPtr, displayId, subObj);

        if (displayId == 0)
        {
            EquipLog("  unequip: detach+rebuild");
            DetachSlotEntries(cmo, a.modelSlot);
            if (subObj) RebuildAllModels(cmo);
            return;
        }

        // Look up the ItemDisplayInfo record.
        alignas(4) uint8_t dispBuf[db2::itemdisplayinfo::kRecordSize] = {};
        uint32_t ok = Native<db2::itemdisplayinfo::LookupFn>(db2::itemdisplayinfo::kLookup)(
            reinterpret_cast<void*>(db2::itemdisplayinfo::kStorageObject),
            nullptr, displayId, dispBuf);
        if (!ok)
        {
            EquipLog("  DBC lookup FAILED (displayId=%u not found)", displayId);
            return;
        }

        const char* modelName1 = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffModel1);
        const char* modelName2 = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffModel2);
        const char* texName1   = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffTex1);
        const char* texName2   = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffTex2);
        const char* icon2str   = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffIcon2);

        EquipLog("  DBC ok: model1='%s' model2='%s' tex1='%s' tex2='%s' icon2ptr=0x%p icon2='%s'",
                 modelName1 ? modelName1 : "(null)",
                 modelName2 ? modelName2 : "(null)",
                 texName1   ? texName1   : "(null)",
                 texName2   ? texName2   : "(null)",
                 static_cast<const void*>(icon2str),
                 (icon2str && reinterpret_cast<uintptr_t>(icon2str) > 0x10000 && *icon2str) ? icon2str : "(empty/invalid)");

        // Parse Icon2 attach config (start with defaults from slot config).
        uint32_t attachA_l = cfg.defAttach1, attachA_r = cfg.defAttach1;
        uint32_t attachB_l = cfg.defAttach2, attachB_r = cfg.defAttach2;
        uint32_t icon2flags = 0;
        char customFolder[64] = {};
        // Guard pointer range before dereferencing: a raw integer in g_disp+0x18 could be a
        // small non-null value (GeosetGroup integer) that would AV if dereferenced directly.
        if (icon2str && reinterpret_cast<uintptr_t>(icon2str) > 0x10000
            && *icon2str && (*icon2str == ':' || (*icon2str >= '0' && *icon2str <= '9')))
            ParseIcon2(icon2str, &attachA_l, &attachA_r, &attachB_l, &attachB_r,
                       &icon2flags, customFolder, sizeof(customFolder));

        // Use left-side attach values (0x80 sided-slot selection not yet implemented).
        uint32_t attachA = attachA_l;
        uint32_t attachB = attachB_l;

        EquipLog("  attachA=%u attachB=%u icon2flags=0x%X folder='%s'",
                 attachA, attachB, icon2flags, customFolder[0] ? customFolder : "(default)");

        // ChrRaces lookup for race code and gender string.
        uint32_t raceId = GuardedReadU32(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoRace);
        uint32_t low    = *reinterpret_cast<uint32_t*>(db2::chrraces::kMinId);
        uint32_t high   = *reinterpret_cast<uint32_t*>(db2::chrraces::kMaxId);
        if (raceId < low || raceId > high)
        {
            EquipLog("  raceId=%u out of range [%u,%u], bail", raceId, low, high);
            return;
        }
        uint8_t* idTable = *reinterpret_cast<uint8_t**>(db2::chrraces::kIdTable);
        void*    chrRec  = *reinterpret_cast<void**>(idTable + (raceId - low) * sizeof(void*));
        if (!chrRec)
        {
            EquipLog("  chrRec null for raceId=%u, bail", raceId);
            return;
        }

        const char* raceCode  = *reinterpret_cast<const char**>(reinterpret_cast<uint8_t*>(chrRec) + db2::chrraces::kOffRecordPrefix);
        uint32_t    genderIdx = GuardedReadU32(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoGender);
        const char* genderStr = *reinterpret_cast<const char**>(db2::genderstrings::kTable + genderIdx * sizeof(void*));
        if (!raceCode || !genderStr)
        {
            EquipLog("  raceCode or genderStr null (raceCode=0x%p genderStr=0x%p), bail",
                     static_cast<const void*>(raceCode), static_cast<const void*>(genderStr));
            return;
        }

        EquipLog("  race='%s' gender='%s'", raceCode, genderStr);

        if (!subObj)
        {
            EquipLog("  subObj null, bail");
            return;
        }

        // Remove stale entries for this model slot before adding new ones.
        DetachSlotEntries(cmo, a.modelSlot);

        // Helper: strip optional ":N[,M]" geoset suffix from the model name field.
        auto splitModelName = [](const char* name, char* stemOut, size_t stemSz, GeosetFilter* filterOut)
        {
            *filterOut = {};
            if (!name || !*name) { stemOut[0] = '\0'; return; }
            const char* colon = std::strchr(name, ':');
            if (colon)
            {
                size_t len = static_cast<size_t>(colon - name);
                if (len >= stemSz) len = stemSz - 1;
                std::memcpy(stemOut, name, len);
                stemOut[len] = '\0';
                *filterOut = ParseGeosetFilter(colon + 1);
            }
            else { std::strncpy(stemOut, name, stemSz - 1); stemOut[stemSz - 1] = '\0'; }
        };

        // Effective path flags: collection models always use race+gender; normal models use slot defaults.
        // Icon2 flags 0x2/0x4 can still suppress race/gender regardless.
        auto effectiveFlags = [&](bool isCollection) -> uint32_t
        {
            uint32_t ef = icon2flags;
            if (!isCollection)
            {
                if (!cfg.defUseRace)   ef |= 0x4;
                if (!cfg.defUseGender) ef |= 0x2;
            }
            return ef;
        };

        // Model 1
        if (modelName1 && *modelName1)
        {
            char stem1[264]; GeosetFilter geo1;
            splitModelName(modelName1, stem1, sizeof(stem1), &geo1);
            bool isCol1 = geo1.count > 0;
            uint32_t ef1 = effectiveFlags(isCol1);

            char m1Path[264];
            BuildSlotPath(m1Path, stem1, raceCode, genderStr, ef1, cfg.folder, isCol1, customFolder);

            char texPath1[264] = {};
            if (texName1 && *texName1)
                BuildTexPath(texPath1, sizeof(texPath1), texName1, raceCode, genderStr,
                             ef1, cfg.folder, isCol1, customFolder, stem1);

            EquipLog("  M1: attach=%u path='%s' tex='%s' geoCount=%u",
                     attachA, m1Path, texPath1[0] ? texPath1 : "(none)", geo1.count);

            AttachEntry e1 = {};
            e1.equipSlot = a.modelSlot;
            e1.attachId  = attachA;
            e1.subObj    = subObj;
            std::memcpy(e1.keyBuf, m1Path,   sizeof(e1.keyBuf));
            std::memcpy(e1.texBuf, texPath1, sizeof(e1.texBuf));
            e1.geoFilter = geo1;
            g_attached[cmo].push_back(e1);
        }
        else
        {
            EquipLog("  M1: no model name, skip");
        }

        // Model 2
        if (modelName2 && *modelName2 && attachB != static_cast<uint32_t>(-1))
        {
            char stem2[264]; GeosetFilter geo2;
            splitModelName(modelName2, stem2, sizeof(stem2), &geo2);
            bool isCol2 = geo2.count > 0;
            uint32_t ef2 = effectiveFlags(isCol2);

            char m2Path[264];
            BuildSlotPath(m2Path, stem2, raceCode, genderStr, ef2, cfg.folder, isCol2, customFolder);

            char texPath2[264] = {};
            if (texName2 && *texName2)
                BuildTexPath(texPath2, sizeof(texPath2), texName2, raceCode, genderStr,
                             ef2, cfg.folder, isCol2, customFolder, stem2);

            EquipLog("  M2: attach=%u path='%s' tex='%s' geoCount=%u",
                     attachB, m2Path, texPath2[0] ? texPath2 : "(none)", geo2.count);

            AttachEntry e2 = {};
            e2.equipSlot = a.modelSlot;
            e2.attachId  = attachB;
            e2.subObj    = subObj;
            std::memcpy(e2.keyBuf, m2Path,   sizeof(e2.keyBuf));
            std::memcpy(e2.texBuf, texPath2, sizeof(e2.texBuf));
            e2.geoFilter = geo2;
            g_attached[cmo].push_back(e2);
        }
        else
        {
            EquipLog("  M2: no model name or attachB==-1, skip");
        }

        EquipLog("  calling RebuildAllModels");
        RebuildAllModels(cmo);
    }

    void EquipExtension::OnItemSlotClear(const ev::ItemSlotClearArgs& a)
    {
        if (a.equipSlotWow >= 19) return;
        uint32_t modelSlot = kEquipToModelSlot[a.equipSlotWow];
        if (modelSlot == static_cast<uint32_t>(-1)) return;

        void* cmo = a.charModelObj;

        DetachSlotEntries(cmo, modelSlot);

        void* subObj = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);
        if (subObj) RebuildAllModels(cmo);
    }

    void EquipExtension::OnM2PerFrameUpdate(const ev::M2PerFrameUpdateArgs& a)
    {
        void* renderCtx = a.renderCtx;
        if (g_attached.empty()) return;

        // Purge dead CMOs: cmo->sceneNode is zeroed when WoW frees the character model object.
        // Without purging, a stale entry can mistakenly match a reused CMO address.
        for (auto it = g_attached.begin(); it != g_attached.end(); )
        {
            void* sub = GuardedReadPtr(reinterpret_cast<uint8_t*>(it->first) + m2::kOffCmoSceneNode);
            if (!sub) { VPathEvictCmo(it->first); it = g_attached.erase(it); }
            else      ++it;
        }
        if (g_attached.empty()) return;

        // Character-model PerFrame sweep: fires when the character's sceneNode is the current
        // renderCtx. Two responsibilities:
        //   1. Pending-attach rebuild (entries with null renderCtx).
        //   2. Per-frame bone copy driven from the character model itself.
        // Driving bone copy here guarantees it runs every frame regardless of whether the
        // collection model's own PerFrame has fired yet, avoiding the race where collBuf is
        // null on the collection model's first PerFrame and the entry's renderCtx changes
        // before collBuf becomes non-null.
        for (auto& [cmo, entries] : g_attached)
        {
            void* sceneNode = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);
            if (sceneNode != renderCtx) continue;

            bool hasPending = false;
            for (const auto& e : entries)
                if (!e.renderCtx) { hasPending = true; break; }
            if (hasPending) { RebuildAllModels(cmo); return; }

            // charBuf: character bone palette. Valid when the character model is rendering.
            auto* charBuf = reinterpret_cast<uint8_t*>(
                GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + m2::kOffInstBonePalette));
            if (charBuf)
            {
                for (auto& entry : entries)
                {
                    if (!entry.renderCtx || entry.boneRemap.count == 0) continue;
                    auto* collBuf = reinterpret_cast<uint8_t*>(
                        GuardedReadPtr(reinterpret_cast<uint8_t*>(entry.renderCtx) + m2::kOffInstBonePalette));
                    if (!collBuf) continue;
                    if (!entry.charSweepApplied)
                    {
                        uint32_t charInitF = GuardedReadU32(
                            reinterpret_cast<uint8_t*>(renderCtx) + m2::kOffInstInitFlags);
                        uint32_t collInitF = GuardedReadU32(
                            reinterpret_cast<uint8_t*>(entry.renderCtx) + m2::kOffInstInitFlags);
                        EquipLog("  CharSweep(bone): rctx=0x%p charInitFlags=0x%X collBuf=0x%p collInitFlags=0x%X -> APPLYING",
                                 entry.renderCtx, charInitF, static_cast<void*>(collBuf), collInitF);
                        entry.charSweepApplied = true;
                    }
                    const BoneRemap& remap = entry.boneRemap;
                    for (uint32_t bi = 0; bi < remap.count; ++bi)
                    {
                        uint8_t ci = remap.collToChar[bi];
                        if (ci == 0xFF) continue;
                        std::memcpy(collBuf + bi * m2::kBonePaletteStride,
                                    charBuf + ci * m2::kBonePaletteStride,
                                    m2::kBonePaletteStride);
                    }
                }
            }
            return;
        }

        for (auto& [cmo, entries] : g_attached)
        {
            for (auto& entry : entries)
            {
                if (entry.renderCtx != renderCtx) continue;

                // Liveness check: DetachSlot zeroes the model pointer (render_ctx+0x2c) without
                // freeing the render_ctx allocation. If evicted (by vanilla slot handlers sharing
                // the same attach point), re-attach immediately.
                void* m2Live = GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + m2::kOffInstModel);
                if (!m2Live)
                {
                    // Use owner28 (scene-node's back-pointer to its owning CMO) for GetRenderCtx,
                    // not the slot-dispatch 'cmo'. See RebuildAllModels for the full explanation.
                    void* owner28 = GuardedReadPtr(reinterpret_cast<uint8_t*>(entry.subObj) + m2::kOffSceneNodeOwner);
                    if (owner28 && entry.keyBuf[0])
                    {
                        // SafeGetRenderCtx guards against dangling hash-table buckets (same
                        // root cause as the RebuildAllModels equip crash — see that function).
                        const char* pfKey = (entry.geoFilter.count > 0 && entry.mangledKeyBuf[0])
                                             ? entry.mangledKeyBuf : entry.keyBuf;
                        void* rctx = SafeGetRenderCtx(owner28, pfKey);
                        if (rctx)
                        {
                            if (entry.texBuf[0])
                            {
                                void* tex = gm2::LoadResource(entry.texBuf, 0);
                                if (tex) { gm2::BindTexSlot(rctx, tex); gm2::ReleaseResource(tex); }
                            }
                            gm2::DetachSlot(entry.subObj, entry.attachId);
                            gm2::AttachToScene(rctx, entry.subObj, entry.attachId, entry.geoFilter.count > 0);
                            for (auto& e2 : entries)
                                if (e2.renderCtx == renderCtx) e2.renderCtx = rctx;
                            gm2::ReleaseRenderCtx(rctx);
                        }
                        else
                        {
                            for (auto& e2 : entries)
                                if (e2.renderCtx == renderCtx) e2.renderCtx = nullptr;
                        }
                    }
                    else
                    {
                        for (auto& e2 : entries)
                            if (e2.renderCtx == renderCtx) e2.renderCtx = nullptr;
                    }
                    return;
                }

                // Bone matrix copy via 3-pass remap table.
                // charCtx is the character's render_ctx (= cmo->sceneNode = the outer frame call).
                // The collection M2's bone buffer is overwritten with corresponding char matrices.
                void* charCtx = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);
                if (charCtx && charCtx != renderCtx)
                {
                    BoneRemap& remap = entry.boneRemap;
                    // Retry if: (a) never built (count=0), or (b) all entries are 0xFF.
                    // Case (b) occurs when Phase3 runs while charBase is uninitialized:
                    // the function throws inside a pass, __except catches it and returns
                    // count=N/all-0xFF. Phase3's propagation doesn't overwrite it on
                    // subsequent rebuilds because the count != 0 guard prevents updates.
                    bool remapBroken = (remap.count == 0);
                    if (!remapBroken && remap.count > 0)
                    {
                        bool allFF = true;
                        for (uint32_t bi = 0; allFF && bi < remap.count; ++bi)
                            allFF = (remap.collToChar[bi] == 0xFF);
                        remapBroken = allFF;
                    }
                    if (remapBroken)
                    {
                        BoneRemap tryRemap = BuildBoneRemapGuarded(renderCtx, charCtx);
                        if (tryRemap.count > 0)
                        {
                            // Only adopt if at least one bone was matched; otherwise charBase
                            // is still unavailable and we'd just pin the same broken remap.
                            bool hasMatch = false;
                            for (uint32_t bi = 0; !hasMatch && bi < tryRemap.count; ++bi)
                                hasMatch = (tryRemap.collToChar[bi] != 0xFF);
                            if (hasMatch)
                            {
                                remap = tryRemap;
                                // Propagate to ALL entries sharing this renderCtx — not just
                                // count==0 ones, since they may be stuck with the broken remap.
                                for (auto& e2 : entries)
                                    if (e2.renderCtx == renderCtx)
                                        e2.boneRemap = tryRemap;
                                uint32_t xff2 = 0;
                                for (uint32_t bi = 0; bi < tryRemap.count; ++bi)
                                    if (tryRemap.collToChar[bi] == 0xFF) ++xff2;
                                EquipLog("  PerFrame[retry-ok] rctx=0x%p count=%u xff=%u"
                                         " map0-7=[%u %u %u %u %u %u %u %u]",
                                         renderCtx, (uint32_t)tryRemap.count, xff2,
                                         tryRemap.count>0 ? (uint32_t)tryRemap.collToChar[0] : 255u,
                                         tryRemap.count>1 ? (uint32_t)tryRemap.collToChar[1] : 255u,
                                         tryRemap.count>2 ? (uint32_t)tryRemap.collToChar[2] : 255u,
                                         tryRemap.count>3 ? (uint32_t)tryRemap.collToChar[3] : 255u,
                                         tryRemap.count>4 ? (uint32_t)tryRemap.collToChar[4] : 255u,
                                         tryRemap.count>5 ? (uint32_t)tryRemap.collToChar[5] : 255u,
                                         tryRemap.count>6 ? (uint32_t)tryRemap.collToChar[6] : 255u,
                                         tryRemap.count>7 ? (uint32_t)tryRemap.collToChar[7] : 255u);
                            }
                        }
                    }

                    auto* charBuf = reinterpret_cast<uint8_t*>(
                        GuardedReadPtr(reinterpret_cast<uint8_t*>(charCtx) + m2::kOffInstBonePalette));
                    auto* collBuf = reinterpret_cast<uint8_t*>(
                        GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + m2::kOffInstBonePalette));

                    if (!entry.perFrameLogged)
                    {
                        uint32_t collInitF = GuardedReadU32(reinterpret_cast<uint8_t*>(renderCtx) + m2::kOffInstInitFlags);
                        uint32_t xff = 0;
                        for (uint32_t bi = 0; bi < remap.count; ++bi)
                            if (remap.collToChar[bi] == 0xFF) ++xff;
                        EquipLog("  PerFrame[first] rctx=0x%p flags=0x%X remap.count=%u xff=%u"
                                 " map0-7=[%u %u %u %u %u %u %u %u]"
                                 " charBuf=0x%p collBuf=0x%p",
                                 renderCtx, collInitF, (uint32_t)remap.count, xff,
                                 remap.count>0 ? (uint32_t)remap.collToChar[0] : 255u,
                                 remap.count>1 ? (uint32_t)remap.collToChar[1] : 255u,
                                 remap.count>2 ? (uint32_t)remap.collToChar[2] : 255u,
                                 remap.count>3 ? (uint32_t)remap.collToChar[3] : 255u,
                                 remap.count>4 ? (uint32_t)remap.collToChar[4] : 255u,
                                 remap.count>5 ? (uint32_t)remap.collToChar[5] : 255u,
                                 remap.count>6 ? (uint32_t)remap.collToChar[6] : 255u,
                                 remap.count>7 ? (uint32_t)remap.collToChar[7] : 255u,
                                 static_cast<void*>(charBuf), static_cast<void*>(collBuf));
                    }

                    if (charBuf && collBuf)
                    {
                        for (uint32_t i = 0; i < remap.count; ++i)
                        {
                            uint8_t ci = remap.collToChar[i];
                            if (ci == 0xFF) continue;
                            std::memcpy(collBuf + i * m2::kBonePaletteStride,
                                        charBuf + ci * m2::kBonePaletteStride,
                                        m2::kBonePaletteStride);
                        }
                    }
                }

                entry.perFrameLogged = true;
                return;
            }
        }
    }

    void EquipExtension::OnM2SkinFinalize(const ev::M2SkinFinalizeArgs& a)
    {
        void* model = a.model;
        if (!model || g_attached.empty()) return;

        // Primary path: match by renderCtx → model pointer. Reliable for async loads where
        // entry.renderCtx is set by Phase1 before kFinalizeSkin fires.
        for (auto& [cmo, entries] : g_attached)
        {
            for (auto& entry : entries)
            {
                if (!entry.renderCtx || entry.geoFilter.count == 0) continue;
                void* entryModel = GuardedReadPtr(
                    reinterpret_cast<uint8_t*>(entry.renderCtx) + m2::kOffInstModel);
                if (entryModel != model) continue;

                GeosetFilter merged = {};
                for (auto& e2 : entries)
                {
                    if (e2.renderCtx != entry.renderCtx) continue;
                    for (uint32_t fi = 0; fi < e2.geoFilter.count && merged.count < 16; ++fi)
                    {
                        bool dup = false;
                        for (uint32_t fj = 0; fj < merged.count; ++fj)
                            if (merged.ids[fj] == e2.geoFilter.ids[fi]) { dup = true; break; }
                        if (!dup) merged.ids[merged.count++] = e2.geoFilter.ids[fi];
                    }
                }

                auto* skin = gm2::Skin(model);
                EquipLog("  OnM2SkinFinalize(primary): model=0x%p skin=0x%p merged.count=%u ids=[%u %u %u %u]",
                         model, static_cast<void*>(skin), merged.count,
                         merged.count > 0 ? merged.ids[0] : 0,
                         merged.count > 1 ? merged.ids[1] : 0,
                         merged.count > 2 ? merged.ids[2] : 0,
                         merged.count > 3 ? merged.ids[3] : 0);
                if (skin) ApplyRawTriFilter(skin, merged);

                // Bone remap: apply immediately on async load completion.
                // OnM2SkinFinalize is the earliest moment entry.renderCtx is valid and the
                // collection model is parsed. PerFrame retries if collBuf is still null here.
                void* charCtx = GuardedReadPtr(
                    reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);
                if (charCtx && charCtx != entry.renderCtx)
                {
                    if (entry.boneRemap.count == 0)
                    {
                        BoneRemap tryRemap = BuildBoneRemapGuarded(entry.renderCtx, charCtx);
                        if (tryRemap.count > 0)
                        {
                            for (auto& e2 : entries)
                                if (e2.renderCtx == entry.renderCtx) e2.boneRemap = tryRemap;
                        }
                    }
                    const BoneRemap& remap = entry.boneRemap;
                    if (remap.count > 0)
                    {
                        auto* charBuf = reinterpret_cast<uint8_t*>(GuardedReadPtr(
                            reinterpret_cast<uint8_t*>(charCtx) + m2::kOffInstBonePalette));
                        auto* collBuf = reinterpret_cast<uint8_t*>(GuardedReadPtr(
                            reinterpret_cast<uint8_t*>(entry.renderCtx) + m2::kOffInstBonePalette));
                        EquipLog("  OnM2SkinFinalize(bone): charCtx=0x%p charBuf=0x%p collBuf=0x%p count=%u -> %s",
                             charCtx, static_cast<void*>(charBuf), static_cast<void*>(collBuf),
                             (uint32_t)remap.count,
                             (charBuf && collBuf) ? "APPLIED" : "DEFERRED-to-PerFrame");
                    if (charBuf && collBuf)
                        {
                            for (uint32_t bi = 0; bi < remap.count; ++bi)
                            {
                                uint8_t ci = remap.collToChar[bi];
                                if (ci == 0xFF) continue;
                                std::memcpy(collBuf + bi * m2::kBonePaletteStride,
                                            charBuf + ci * m2::kBonePaletteStride,
                                            m2::kBonePaletteStride);
                            }
                        }
                    }
                }
                return;
            }
        }

        // Fallback path: virtual-path models load SYNCHRONOUSLY inside Phase1's GetRenderCtx
        // (in-process VirtualProvide has no IPC delay), so kFinalizeSkin fires before Phase1
        // sets entry.renderCtx. Match by model path stem (kOffModelPathStem = inline char buf
        // at model+0x3C, set by kInit from the M2 file's internal name = the real path stem).
        if (!g_currentRebuildCmo) return;
        auto it2 = g_attached.find(g_currentRebuildCmo);
        if (it2 == g_attached.end()) return;

        __try
        {
            const char* modelStem = reinterpret_cast<const char*>(
                reinterpret_cast<uint8_t*>(model) + m2::kOffModelPathStem);
            // Sanity-check: first char must be printable ASCII (path starts with 'I' for
            // Item\... paths). Garbage pointers or uninitialised memory fail this check.
            if (!modelStem || static_cast<unsigned char>(modelStem[0]) < 0x20
                           || static_cast<unsigned char>(modelStem[0]) > 0x7E)
                return;

            EquipLog("  OnM2SkinFinalize(fallback) cmo=0x%p modelStem='%.80s'",
                     g_currentRebuildCmo, modelStem);

            GeosetFilter merged = {};
            bool found = false;
            for (auto& entry : it2->second)
            {
                if (entry.geoFilter.count == 0 || entry.renderCtx) continue;
                // modelStem (model+0x3C) may be a full path or just a filename stem, and may
                // differ in case from keyBuf (DBC uses mixed case; host files are lowercase).
                // Extract filename part from both sides, strip any extension, compare ignoring case.
                const char* msBase = modelStem;
                for (const char* p = modelStem; *p; ++p)
                    if (*p == '\\' || *p == '/') msBase = p + 1;
                const char* msDot = nullptr;
                for (const char* p = msBase; *p; ++p) if (*p == '.') msDot = p;
                size_t msLen = msDot ? static_cast<size_t>(msDot - msBase) : std::strlen(msBase);

                const char* fileBase = entry.keyBuf;
                for (const char* p = entry.keyBuf; *p; ++p)
                    if (*p == '\\' || *p == '/') fileBase = p + 1;
                const char* lastDot = nullptr;
                for (const char* p = fileBase; *p; ++p) if (*p == '.') lastDot = p;
                size_t stemLen = lastDot ? static_cast<size_t>(lastDot - fileBase)
                                         : std::strlen(fileBase);
                if (msLen != stemLen) continue;
                if (_strnicmp(msBase, fileBase, stemLen) != 0) continue;

                found = true;
                for (uint32_t fi = 0; fi < entry.geoFilter.count && merged.count < 16; ++fi)
                {
                    bool dup = false;
                    for (uint32_t fj = 0; fj < merged.count; ++fj)
                        if (merged.ids[fj] == entry.geoFilter.ids[fi]) { dup = true; break; }
                    if (!dup) merged.ids[merged.count++] = entry.geoFilter.ids[fi];
                }
            }

            if (found)
            {
                auto* skin = gm2::Skin(model);
                EquipLog("  OnM2SkinFinalize(fallback): skin=0x%p merged.count=%u ids=[%u %u %u %u]",
                         static_cast<void*>(skin), merged.count,
                         merged.count > 0 ? merged.ids[0] : 0,
                         merged.count > 1 ? merged.ids[1] : 0,
                         merged.count > 2 ? merged.ids[2] : 0,
                         merged.count > 3 ? merged.ids[3] : 0);
                if (skin) ApplyRawTriFilter(skin, merged);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ─── OnBuildBonePalette ───────────────────────────────────────────────────────
    // Fires POST-engine on every kBuildBonePalette call (one per M2 instance per frame).
    // Only acts on collection models (initFlags & 0x40000): the engine does not drive their
    // bone palette via parent transforms, so the outer scene-traversal at 0x821B4E fills them
    // with T-pose, overwriting any CharSweep copy from the character's PerFrameUpdate.
    // Re-applying here guarantees the copy is the last write before GPU upload.
    void EquipExtension::OnBuildBonePalette(const ev::BuildBonePaletteArgs& a)
    {
        if (g_attached.empty()) return;
        void* renderCtx = a.renderCtx;

        // Fast reject: only collection models need manual bone driving.
        uint32_t initF = GuardedReadU32(
            reinterpret_cast<uint8_t*>(renderCtx) + m2::kOffInstInitFlags);
        if (!(initF & 0x40000)) return;

        for (auto& [cmo, entries] : g_attached)
        {
            for (auto& entry : entries)
            {
                if (entry.renderCtx != renderCtx) continue;
                if (entry.boneRemap.count == 0) continue;
                void* charRctx = GuardedReadPtr(
                    reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);
                if (!charRctx) continue;
                auto* charBuf = reinterpret_cast<uint8_t*>(
                    GuardedReadPtr(reinterpret_cast<uint8_t*>(charRctx) + m2::kOffInstBonePalette));
                if (!charBuf) continue;
                auto* collBuf = reinterpret_cast<uint8_t*>(
                    GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + m2::kOffInstBonePalette));
                if (!collBuf) continue;
                const BoneRemap& remap = entry.boneRemap;

                if (!entry.bbpLogDone)
                {
                    uint32_t xff = 0;
                    for (uint32_t bi = 0; bi < remap.count; ++bi)
                        if (remap.collToChar[bi] == 0xFF) ++xff;
                    float m00 = *reinterpret_cast<const float*>(charBuf); // [0,0] of char bone 0
                    EquipLog("  OnBBP[first] collRctx=0x%p initF=0x%X count=%u xff=%u"
                             " map0-7=[%u %u %u %u %u %u %u %u] charBuf[0][0]=%.4f",
                             renderCtx, initF, (uint32_t)remap.count, xff,
                             remap.count>0 ? (uint32_t)remap.collToChar[0] : 255u,
                             remap.count>1 ? (uint32_t)remap.collToChar[1] : 255u,
                             remap.count>2 ? (uint32_t)remap.collToChar[2] : 255u,
                             remap.count>3 ? (uint32_t)remap.collToChar[3] : 255u,
                             remap.count>4 ? (uint32_t)remap.collToChar[4] : 255u,
                             remap.count>5 ? (uint32_t)remap.collToChar[5] : 255u,
                             remap.count>6 ? (uint32_t)remap.collToChar[6] : 255u,
                             remap.count>7 ? (uint32_t)remap.collToChar[7] : 255u,
                             m00);
                    entry.bbpLogDone = true;
                }

                for (uint32_t bi = 0; bi < remap.count; ++bi)
                {
                    uint8_t ci = remap.collToChar[bi];
                    if (ci == 0xFF) continue;
                    std::memcpy(collBuf + bi * m2::kBonePaletteStride,
                                charBuf + ci * m2::kBonePaletteStride,
                                m2::kBonePaletteStride);
                }
                return; // renderCtx is unique per (keyBuf,attachId,texBuf) group; one copy suffices
            }
        }
    }

    // ─── Constructor ──────────────────────────────────────────────────────────────

    EquipExtension::EquipExtension()
    {
        on<&EquipExtension::OnItemSlotChange>(ev::Event::OnItemSlotChange);
        on<&EquipExtension::OnItemSlotClear>(ev::Event::OnItemSlotClear);
        on<&EquipExtension::OnM2SkinFinalize>(ev::Event::OnM2SkinFinalize);
        on<&EquipExtension::OnM2PerFrameUpdate>(ev::Event::OnM2PerFrameUpdate);
        on<&EquipExtension::OnBuildBonePalette>(ev::Event::OnBuildBonePalette);
    }

    // Self-registration: file-scope instance binds handlers at DLL load via EventScript ctor.
    EquipExtension g_equipExtension;
}
