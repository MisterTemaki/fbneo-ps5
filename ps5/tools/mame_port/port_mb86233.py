#!/usr/bin/env python3
# Makes src/cpu/mb86233/mb86233_ops.inc from MAME's src/devices/cpu/mb86233/mb86233.cpp: lines 177-1231 (the gpio
# inputs, device_reset and everything after: the ALU, the addressing, the registers and execute_run), kept as they are
# but for MAME's framework. Run once; the result is committed.
import sys

src = open(sys.argv[1]).read().split('\n')
text = '\n'.join(src[176:])

rep = [
    ('mb86233_device::', 'mb86233_core::'),
    ('debugger_instruction_hook(m_ppc);', '/* (debugger hook) */'),
    ('util::sext(', 'mb86233_sext('),
    ('logerror(', 'MB86233_LOG('),
]
for a, b in rep:
    text = text.replace(a, b)

# float -> integer (cfxd): x86's result for NaN / out of range, without C++'s undefined behaviour
for f in ['roundf', 'ceilf', 'floorf']:
    text = text.replace('m_alu_r1 = s32(%s(u2f(m_d)));' % f, 'm_alu_r1 = mb86233_f2i32(%s(u2f(m_d)));' % f)
text = text.replace('case 3: m_alu_r1 = s32(u2f(m_d)); break;', 'case 3: m_alu_r1 = mb86233_f2i32(u2f(m_d)); break;')
# shifts by the 8-bit sft register: C++ leaves a shift of 32 or more undefined; x86 masks the count to 5 bits
text = text.replace('m_alu_r1 = m_d >> m_sft;', 'm_alu_r1 = m_d >> (m_sft & 31);')
text = text.replace('m_alu_r1 = m_d << m_sft;', 'm_alu_r1 = m_d << (m_sft & 31);')
text = text.replace('m_alu_r1 = s32(m_d) >> m_sft;', 'm_alu_r1 = s32(m_d) >> (m_sft & 31);')
text = text.replace('m_alu_r1 = s32(m_d) << m_sft;', 'm_alu_r1 = u32(m_d) << (m_sft & 31);')

for bad in ['util::', 'machine()', 'fatalerror']:
    assert bad not in text, bad

hdr = '''// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
//
// FBNeo PS5 2.3: MAME's src/devices/cpu/mb86233/mb86233.cpp (mamedev/mame b7f483f), lines 177-1231, made by a script:
// MAME's code but for its framework (the class is mb86233_core, logerror logs through MB86233_LOG, util::sext is a
// local helper, cfxd's float -> integer conversion gives x86's result without undefined behaviour, and the shifts by
// sft use its low 5 bits as x86 does). Included by mb86233.cpp only.
'''
open(sys.argv[2], 'w').write(hdr + '\n' + text + '\n')
