// wxl-equip-extension: client-side virtual M2 path table and file provider.
//
// Virtual paths encode (cmo × model × merged-geoset-filter × texture) in the filename so the
// engine's model hash table sees a distinct cache key for every unique combination. The host
// serve hook is bypassed; bytes are served directly from an in-process table. Filtering is still
// applied by OnM2SkinFinalize on the parsed rawTri — we serve the unmodified real file bytes.
//
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

#include "VirtualPath.hpp"

#include "runtime/storage/StorageHook.hpp"
#include "game/io/Io.hpp"
#include "offsets/engine/Io.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::scripts::equipextension
{
    namespace
    {
        static void VPathLog(const char* fmt, ...) noexcept
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
        // virtual path -> raw file bytes (both .mdx and 00.skin entries per model).
        // Accessed from the game's main thread only; no mutex needed.
        std::unordered_map<std::string, std::vector<uint8_t>> g_virtualBytes;

        // cmo -> list of virtual paths it owns, for O(n) cleanup on eviction.
        std::unordered_map<void*, std::vector<std::string>> g_cmoVPaths;

        // ─── Key building ─────────────────────────────────────────────────────

        // Sorts ids[0..n-1] ascending in place (insertion sort; n <= 16).
        static void SortIds(uint16_t* ids, uint32_t n) noexcept
        {
            for (uint32_t i = 1; i < n; ++i)
            {
                uint16_t k = ids[i];
                int32_t j = static_cast<int32_t>(i) - 1;
                while (j >= 0 && ids[j] > k) { ids[j + 1] = ids[j]; --j; }
                ids[j + 1] = k;
            }
        }

        // Writes a uint16 as decimal into *q, advancing q. Returns new q.
        static char* WriteU16(char* q, char* end, uint16_t v) noexcept
        {
            char tmp[8]; int len = 0;
            if (v == 0) { tmp[len++] = '0'; }
            else { while (v) { tmp[len++] = '0' + (v % 10); v /= 10; } }
            for (int a = 0, b = len - 1; a < b; ++a, --b) { char c = tmp[a]; tmp[a] = tmp[b]; tmp[b] = c; }
            for (int i = 0; i < len && q < end; ++i) *q++ = tmp[i];
            return q;
        }

        // Writes a uintptr_t as lowercase hex into *q, advancing q. Returns new q.
        static char* WriteHex(char* q, char* end, uintptr_t v) noexcept
        {
            char tmp[9]; int len = 0;
            const char* digits = "0123456789abcdef";
            do { tmp[len++] = digits[v & 0xF]; v >>= 4; } while (v);
            for (int a = 0, b = len - 1; a < b; ++a, --b) { char c = tmp[a]; tmp[a] = tmp[b]; tmp[b] = c; }
            for (int i = 0; i < len && q < end; ++i) *q++ = tmp[i];
            return q;
        }

        static char LowerAscii(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }

        // Builds the virtual .m2 key (sortedIds must already be sorted ascending).
        // Format: <stem>_wxl_<id0>_<id1>..._tex_<texbasename>_cmo<hex>.m2  (all lowercase)
        // The engine normalises all paths to lowercase and uses .m2; keys must match that form.
        static size_t BuildKey(char* out, size_t outSz, void* cmo,
                                const char* realMdxPath,
                                const uint16_t* sortedIds, uint32_t idCount,
                                const char* texPath) noexcept
        {
            if (!out || outSz == 0) return 0;
            char* q   = out;
            char* end = out + outSz - 1; // reserve one byte for null

            // Copy the stem (path without extension) — lowercase.
            const char* lastDot = nullptr;
            for (const char* p = realMdxPath; *p; ++p) if (*p == '.') lastDot = p;
            const char* stemEnd = lastDot ? lastDot : realMdxPath + std::strlen(realMdxPath);
            for (const char* p = realMdxPath; p < stemEnd && q < end; ) *q++ = LowerAscii(*p++);

            // _wxl_
            for (const char* s = "_wxl_"; *s && q < end; ) *q++ = *s++;

            // Sorted geoset IDs separated by '_'.
            for (uint32_t i = 0; i < idCount && q < end; ++i)
            {
                if (i > 0 && q < end) *q++ = '_';
                q = WriteU16(q, end, sortedIds[i]);
            }

            // _tex_<texbasename> (basename = filename without path prefix or extension) — lowercase.
            if (texPath && *texPath)
            {
                const char* base = texPath;
                for (const char* p = texPath; *p; ++p)
                    if (*p == '\\' || *p == '/') base = p + 1;
                const char* dot = nullptr;
                for (const char* p = base; *p; ++p) if (*p == '.') dot = p;
                const char* baseEnd = dot ? dot : base + std::strlen(base);

                for (const char* s = "_tex_"; *s && q < end; ) *q++ = *s++;
                for (const char* p = base; p < baseEnd && q < end; ) *q++ = LowerAscii(*p++);
            }

            // _cmo<hex> (lowercase)
            for (const char* s = "_cmo"; *s && q < end; ) *q++ = *s++;
            q = WriteHex(q, end, reinterpret_cast<uintptr_t>(cmo));

            // .m2  (engine requests all paths with .m2 extension, not .mdx)
            for (const char* s = ".m2"; *s && q < end; ) *q++ = *s++;

            *q = '\0';
            return static_cast<size_t>(q - out);
        }

        // ─── File reading ─────────────────────────────────────────────────────

        // Reads all bytes of a game archive file via the existing IO wrappers.
        // Uses kOpenWholeFile so the handle buffer holds the full content immediately.
        static bool ReadGameFile(const char* path, std::vector<uint8_t>& out) noexcept
        {
            namespace io    = wxl::game::io;
            namespace iooff = wxl::offsets::engine::io;

            void* handle = nullptr;
            if (!io::FileOpen(path, iooff::kOpenWholeFile, &handle) || !handle)
                return false;

            uint32_t sizeHigh = 0;
            uint32_t size     = io::FileSize(handle, &sizeHigh);
            bool ok = false;
            if (size > 0 && sizeHigh == 0)
            {
                out.resize(size);
                uint32_t got = 0;
                io::FileRead(handle, out.data(), size, &got);
                ok = (got == size);
                if (!ok) out.clear();
            }
            io::FileClose(handle);
            return ok;
        }

        // Derives the real skin path: strip extension (.m2 or .mdx), append 00.skin.
        static void RealSkinPath(char* out, size_t outSz, const char* realMdxPath) noexcept
        {
            const char* lastDot = nullptr;
            for (const char* p = realMdxPath; *p; ++p) if (*p == '.') lastDot = p;
            size_t stemLen = lastDot ? static_cast<size_t>(lastDot - realMdxPath)
                                     : std::strlen(realMdxPath);
            if (stemLen >= outSz) stemLen = outSz - 1;
            std::memcpy(out, realMdxPath, stemLen);
            const char* suffix = "00.skin";
            size_t rem = outSz - stemLen - 1;
            size_t suffLen = std::strlen(suffix);
            if (suffLen > rem) suffLen = rem;
            std::memcpy(out + stemLen, suffix, suffLen);
            out[stemLen + suffLen] = '\0';
        }

        // Derives the virtual skin path from a virtual .m2 key: strip .m2, append 00.skin.
        static void VirtualSkinPath(char* out, size_t outSz, const char* virtualM2Key) noexcept
        {
            RealSkinPath(out, outSz, virtualM2Key); // same operation: strip extension, append 00.skin
        }

        // Produces a lowercase .m2 path from a (possibly mixed-case, .mdx-extension) DBC path.
        // The host stores loose files as lowercase .m2; ReadGameFile must use that form.
        static void NormalizeRealPath(char* out, size_t outSz, const char* src) noexcept
        {
            char* dst  = out;
            char* dend = out + outSz - 1;
            while (*src && dst < dend) *dst++ = LowerAscii(*src++);
            *dst = '\0';
            // Replace trailing .mdx → .m2 (DBC uses .mdx; host stores as .m2).
            char* lastDot = nullptr;
            for (char* p = out; *p; ++p) if (*p == '.') lastDot = p;
            if (lastDot && std::strcmp(lastDot, ".mdx") == 0)
                { lastDot[1] = 'm'; lastDot[2] = '2'; lastDot[3] = '\0'; }
        }

        // ─── Client-side provider ─────────────────────────────────────────────

        static bool VirtualProvide(const char* name, std::vector<uint8_t>& out)
        {
            if (!name || !std::strstr(name, "_wxl_")) return false;
            VPathLog("  VirtualProvide: '%s'", name);
            auto it = g_virtualBytes.find(name);
            if (it == g_virtualBytes.end())
            {
                VPathLog("  VirtualProvide: NOT FOUND (table has %zu entries)", g_virtualBytes.size());
                return false;
            }
            VPathLog("  VirtualProvide: HIT (%zu bytes)", it->second.size());
            out = it->second;
            return true;
        }

        struct Registrar
        {
            Registrar() { wxl::runtime::storage::RegisterClientProvider(&VirtualProvide); }
        };
        static Registrar g_registrar;
    }

    // ─── Public API ───────────────────────────────────────────────────────────

    size_t VPathBuildKey(char* out, size_t outSz, void* cmo,
                         const char* realMdxPath,
                         const uint16_t* geoIds, uint32_t geoCount,
                         const char* texPath)
    {
        uint16_t sorted[16];
        uint32_t n = geoCount < 16 ? geoCount : 16;
        for (uint32_t i = 0; i < n; ++i) sorted[i] = geoIds[i];
        SortIds(sorted, n);
        return BuildKey(out, outSz, cmo, realMdxPath, sorted, n, texPath);
    }

    void VPathPopulate(void* cmo, const char* realMdxPath,
                       const uint16_t* geoIds, uint32_t geoCount,
                       const char* texPath)
    {
        uint16_t sorted[16];
        uint32_t n = geoCount < 16 ? geoCount : 16;
        for (uint32_t i = 0; i < n; ++i) sorted[i] = geoIds[i];
        SortIds(sorted, n);

        // Build virtual .mdx key.
        char vMdx[264];
        if (!BuildKey(vMdx, sizeof(vMdx), cmo, realMdxPath, sorted, n, texPath)) return;

        // No-op if already populated (same permutation on a re-equip cycle).
        if (g_virtualBytes.count(vMdx)) return;

        // Build virtual .skin key.
        char vSkin[264];
        VirtualSkinPath(vSkin, sizeof(vSkin), vMdx);

        // Normalise the real path for host I/O: lowercase, .m2 extension.
        // DBC paths are mixed-case .mdx; the host stores loose files as lowercase .m2.
        char normPath[264];
        NormalizeRealPath(normPath, sizeof(normPath), realMdxPath);

        // Read real .m2 bytes (via normalized path).
        std::vector<uint8_t> mdxBytes;
        if (!ReadGameFile(normPath, mdxBytes))
        {
            VPathLog("  VPathPopulate: mdx READ FAILED '%s'", normPath);
            return;
        }
        VPathLog("  VPathPopulate: mdx '%s' -> %zu bytes", normPath, mdxBytes.size());

        // Read real 00.skin bytes.
        char rSkin[264];
        RealSkinPath(rSkin, sizeof(rSkin), normPath);
        std::vector<uint8_t> skinBytes;
        ReadGameFile(rSkin, skinBytes); // skin may be absent for some models; that is OK
        VPathLog("  VPathPopulate: skin '%s' -> %zu bytes", rSkin, skinBytes.size());
        VPathLog("  VPathPopulate: vMdx='%s'", vMdx);
        VPathLog("  VPathPopulate: vSkin='%s'", vSkin);

        // Store in table.
        g_virtualBytes.emplace(vMdx,  std::move(mdxBytes));
        if (!skinBytes.empty())
            g_virtualBytes.emplace(vSkin, std::move(skinBytes));

        // Register both under cmo for cleanup.
        auto& paths = g_cmoVPaths[cmo];
        paths.emplace_back(vMdx);
        if (g_virtualBytes.count(vSkin)) paths.emplace_back(vSkin);
    }

    void VPathEvictCmo(void* cmo)
    {
        auto it = g_cmoVPaths.find(cmo);
        if (it == g_cmoVPaths.end()) return;
        for (const auto& path : it->second)
            g_virtualBytes.erase(path);
        g_cmoVPaths.erase(it);
    }
}
