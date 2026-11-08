// SPDX-License-Identifier: GPL-3.0-or-later
// Part of frank-ecat (eCat3 on RP2350): https://github.com/rh1tech/frank-ecat
// Description: Palette of an 8-bit surface (ECAT_EMBEDDED), shared by the
// microcontroller renderer and the console check build

#pragma once

#include <cstdint>

// Hands out the palette indices MapRGB() returns when the surface holds one byte
// a dot. Index 0 is black from the start, so a display that clears a span with
// memset(0) still clears it to black, as it did with RGBA. A colour seen before
// gets its old index; a new one the next free index; once all are taken, the
// nearest colour there is (a machine whose palette changes at run time, such as
// the Agat palette card, can outgrow it only after hundreds of distinct colours).
class IndexedPalette
{
public:
    enum { SIZE = 255 };    // index 255 is the HDMI driver's border colour

    IndexedPalette() { clear(); }

    void clear()
    {
        m_count = 1;
        m_rgb[0] = 0;
    }

    // rgb = 0xRRGGBB. *added is set when the index is new (the caller loads it
    // into the hardware palette)
    uint8_t map(uint32_t rgb, bool * added = nullptr)
    {
        if (added) *added = false;
        for (unsigned i = 0; i < m_count; i++)
            if (m_rgb[i] == rgb) return (uint8_t)i;
        if (m_count < SIZE) {
            m_rgb[m_count] = rgb;
            if (added) *added = true;
            return (uint8_t)m_count++;
        }
        unsigned best = 0;
        uint32_t best_d = 0xFFFFFFFFu;
        for (unsigned i = 0; i < m_count; i++) {
            const int dr = (int)((m_rgb[i] >> 16) & 0xFF) - (int)((rgb >> 16) & 0xFF);
            const int dg = (int)((m_rgb[i] >> 8) & 0xFF) - (int)((rgb >> 8) & 0xFF);
            const int db = (int)(m_rgb[i] & 0xFF) - (int)(rgb & 0xFF);
            const uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
            if (d < best_d) { best_d = d; best = i; }
        }
        return (uint8_t)best;
    }

    uint32_t rgb(uint8_t index) const { return index < m_count ? m_rgb[index] : 0; }
    unsigned count() const { return m_count; }

private:
    uint32_t m_rgb[SIZE];
    unsigned m_count;
};
