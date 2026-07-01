// wxl-equip-extension: client-side virtual M2 path table.
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

#pragma once

#include <cstddef>
#include <cstdint>

namespace wxl::scripts::equipextension
{
    /**
     * @brief Builds the virtual .mdx key for a collection M2 into out[outSz].
     *
     * Format: <stem>_wxl_<sorted_geosets>_tex_<texbasename>[_grp<HEX>]_cmo<HEX>.mdx
     * geoIds are sorted ascending internally; caller need not sort them.
     * @param out         destination buffer
     * @param outSz       destination buffer size in bytes
     * @param cmo         CharModelObject pointer, used as the character identifier
     * @param realMdxPath real archive path of the collection .mdx
     * @param geoIds      geoset IDs included in the filter (unsorted)
     * @param geoCount    number of IDs
     * @param texPath     full BLP path for the texture slot (may be empty)
     * @param variantKey  optional logical-model discriminator for entries that must not share cache keys
     * @return number of characters written (excluding null), or 0 on truncation
     */
    size_t VPathBuildKey(char* out, size_t outSz, void* cmo,
                         const char* realMdxPath,
                         const uint16_t* geoIds, uint32_t geoCount,
                         const char* texPath,
                         uint32_t variantKey = 0);

    /**
     * @brief Ensures the virtual .mdx and .skin bytes are in the client serve table.
     *
     * Reads the real .mdx and its 00.skin from the archive on first call for this key;
     * subsequent calls with the same key are no-ops. Registers both paths under cmo for
     * cleanup via VPathEvictCmo.
     * @param cmo         CharModelObject pointer
     * @param realMdxPath real archive path of the collection .mdx
     * @param geoIds      geoset IDs (unsorted)
     * @param geoCount    number of IDs
     * @param texPath     full BLP path for the texture slot (may be empty)
     * @param variantKey  optional logical-model discriminator for entries that must not share cache keys
     */
    void VPathPopulate(void* cmo, const char* realMdxPath,
                       const uint16_t* geoIds, uint32_t geoCount,
                       const char* texPath,
                       uint32_t variantKey = 0);

    /**
     * @brief Removes all virtual table entries owned by cmo.
     *
     * Call when the cmo's sceneNode goes null (character evicted from the scene).
     * @param cmo  CharModelObject pointer to evict
     */
    void VPathEvictCmo(void* cmo);
}
