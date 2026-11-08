// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2023-2025 Mikhail Revzin <p3.141592653589793238462643@gmail.com>
// Part of the eCat3 project: https://github.com/Ptr314/ecat3
// Description: Agat-9 memory manager

#pragma once

#include "emulator/core.h"

class Agat9Mapper:public AddressableDevice
{
private:
    Port * m_pm;
    RAM * m_ram;
    ROM * m_bios;
    RAM * m_page_map;
    Port * rom_mode;

    unsigned _WR;        // ЗП
    unsigned _RD;        // ЧТ
    unsigned _HD;        // HD
    unsigned _D8_4;
    unsigned _bl_RAM;
    unsigned _bl_ROM;
    unsigned m_bios_mask;

#ifdef ECAT_EMBEDDED
    //The PROM decoded per 4 KB page, for reads and for writes: what do_access()
    //works out for every access depends on the page, the switches, PM and the
    //page map only. Rebuilt when any of them changes
    struct Page { uint8_t kind; bool writable; uint32_t ram_base; };   // kind: 0 nothing, 1 ROM, 2 RAM
    enum { PAGE_NONE = 0, PAGE_ROM = 1, PAGE_RAM = 2 };
    Page m_pages[2][16];
    unsigned m_pages_key = ~0u;
    bool m_pages_stale = true;
    uint8_t * m_ram_buf = nullptr;
    unsigned m_ram_size = 0;
    void build_pages(unsigned key, unsigned PM);
#endif
    unsigned do_access(unsigned rd_wr, unsigned address, unsigned data);
    void C08X(unsigned rd_wr, unsigned address);

public:
    Agat9Mapper(InterfaceManager *im, EmulatorConfigDevice *cd);
    void reset(bool cold) override;
    emulator::Result load_config(SystemData *sd) override;
    unsigned get_value(unsigned address) override;
    void set_value(unsigned address, unsigned value, bool force=false) override;
#ifdef ECAT_EMBEDDED
    void memory_callback(unsigned int callback_id, unsigned int address) override;
    //Passes accesses on to memory; its switches are set by an access, not by time
    bool timeless_access() const override { return true; }
#endif
    //The memory, not the C08X switches
    bool debug_writable(unsigned int address) const override { return address < 0xC000 || address >= 0xD000; }

    std::vector<DeviceFieldInfo> get_device_fields() override;
    bool get_field(const std::string &field, unsigned int from, unsigned int to, DeviceFieldValue &out) override;
    void save_state(StateWriter &w) override;
    emulator::Result load_state(const StateReader &r) override;
};

ComputerDevice * create_agat_9_mapper(InterfaceManager *im, EmulatorConfigDevice *cd);

