#!/usr/bin/env python3
# FBNeo PS5: the core's sources, from FBNeo's own build list (makefile.burn_rules), arcade drivers only.
#
#   fbneo_sources.py <fbneo root> drivers   -> the driver .cpp files (for gamelist.pl: driverlist.h)
#   fbneo_sources.py <fbneo root> sources   -> every source to compile (relative to <fbneo root>)
#
# SPDX-License-Identifier: MIT
import os
import re
import sys

# The home-console drivers left out (this port is for arcade games); megadrive.cpp itself stays, the Mega Drive
# arcade bootlegs (d_mdarcbl.cpp) run on it.
CONSOLE_DRIVERS = {
    'd_nes', 'nes', 'd_snes', 'apu', 'cart', 'cpu', 'cx4', 'dma', 'dsp', 'epsonrtc', 'gsu', 'input', 'ppu', 'snes',
    'snes_other', 'spc', 'spc7110', 'msu1', 'msu1_backend', 'msu1_decoders', 'st018', 'sa1', 'sdd1', 'cpu_sa1',
    'statehandler', 'd_gba', 'gba', 'd_megadrive', 'd_pce', 'd_sms', 'd_msx', 'd_coleco', 'd_sg1000', 'd_spectrum',
    'spectrum', 'd_channelf',
}
# the console drivers' own support code (nothing in the arcade drivers uses it)
CONSOLE_DEPS = {'pce', 'pce_cd', 'vdc', 'sms', 'smspio', 'smssystem', 'smsvdp', 'smsfmintf', 'smsrender', 'smssound',
                'smstms'}
# FBNeo's frontend-side code the drivers use: lowpass2 (the sound filter some drivers have)
EXTRA_SOURCES = ['src/intf/audio/lowpass2.cpp']
CONSOLE_DIRS = {'burn/drv/nes', 'burn/drv/snes', 'burn/drv/gba', 'burn/drv/msx', 'burn/drv/coleco',
                'burn/drv/sg1000', 'burn/drv/spectrum', 'burn/drv/channelf'}


def make_var(text, name):
    m = re.search(r'^' + name + r'\s*=\s*((?:.*\\\n)*.*)$', text, re.M)
    return m.group(1).replace('\\\n', ' ').split()


def main():
    root, what = sys.argv[1], sys.argv[2]
    text = open(os.path.join(root, 'makefile.burn_rules')).read()
    alldir = [d for d in make_var(text, 'alldir') if d not in CONSOLE_DIRS]
    drvsrc = [o[:-2] for o in make_var(text, 'drvsrc') if o[:-2] not in CONSOLE_DRIVERS]
    depobj = [o[:-2] for o in make_var(text, 'depobj') if o[:-2] not in CONSOLE_DEPS]

    def find(stem):
        for d in alldir:
            for ext in ('.cpp', '.c'):
                p = os.path.join('src', d, stem + ext)
                if os.path.isfile(os.path.join(root, p)):
                    return p
        sys.exit('fbneo_sources.py: no source for ' + stem)

    drivers = [find(s) for s in drvsrc]
    if what == 'drivers':
        print('\n'.join(p for p in drivers if os.path.basename(p).startswith('d_')))
        return
    seen, out = set(), []
    for p in drivers + [find(s) for s in depobj] + ['src/cpu/m68k/m68kcpu.c'] + EXTRA_SOURCES:
        if p not in seen:
            seen.add(p)
            out.append(p)
    print('\n'.join(out))


main()
