#!/usr/bin/env python3
# Makes src/cpu/i960/i960_ops.inc from MAME's src/devices/cpu/i960/i960.cpp: the instruction code kept as it is,
# only MAME's framework calls replaced (see the list below). Run once; the result is committed.
import re
import sys

src = open(sys.argv[1]).read().split('\n')
out = []
# lines 57 .. 2379 (1-based): the unaligned helpers through execute_set_input
body = src[56:2379]
text = '\n'.join(body)

rep = [
    ('i960_cpu_device::', 'i960_core::'),
    ('debugger_instruction_hook(m_IP);', '/* (debugger hook) */'),
    ('standard_irq_callback(irqline, m_IP);', '/* (irq acknowledge callback: none) */'),
    ('util::sext(', 'i960_sext('),
    ('std::rotl(', 'i960_rotl('),
    ('mulu_32x32(t1, t2)', '(uint64_t(t1) * uint64_t(t2))'),
    ('fatalerror(', 'I960_FATAL('),
    ('logerror(', 'I960_LOG('),
]
for a, b in rep:
    text = text.replace(a, b)

# get_ea: the default case of MEMB returned nothing after fatalerror (which does not return in MAME)
text = text.replace('''		default:
			I960_FATAL("I960: %x: unhandled MEMB mode %x\\n", m_PIP, mode);
		}''', '''		default:
			I960_FATAL("I960: %x: unhandled MEMB mode %x\\n", m_PIP, mode);
			return 0;
		}''')

# float -> integer conversions: x86's result for NaN / out of range (0x80000000...), without C++'s undefined behaviour
text = text.replace('set_ri(opcode, (int32_t)round_to_int(t1f));', 'set_ri(opcode, i960_d2i32(round_to_int(t1f)));')
text = text.replace('set_ri64(opcode, (int64_t)round_to_int(t1f));', 'set_ri64(opcode, i960_d2i64(round_to_int(t1f)));')
text = text.replace('set_ri(opcode, (int32_t)t1f);', 'set_ri(opcode, i960_d2i32(t1f));')
text = text.replace('set_ri64(opcode, (int64_t)t1f);', 'set_ri64(opcode, i960_d2i64(t1f));')

# integer division by zero (and INT_MIN / -1) stops a host process (x86 #DE): MAME would crash there, the port
# gives 0 (divo's own "HACK!" already did); the i960 would raise an arithmetic fault, which MAME does not emulate
text = text.replace('set_ri(opcode, t2%t1);', 'set_ri(opcode, t1 ? t2%t1 : 0);')
text = text.replace('set_ri(opcode, ((int32_t)t2)%((int32_t)t1));', 'set_ri(opcode, i960_rem_s(t2, t1));')
text = text.replace('set_ri(opcode, ((int32_t)t2)/((int32_t)t1));', 'set_ri(opcode, i960_div_s(t2, t1));')
text = text.replace('dst = src2 - ((src2/src1)*src1);', 'dst = int32_t(uint32_t(src2) - uint32_t(i960_div_s(src2, src1))*uint32_t(src1));')
text = text.replace('dst += src1;', 'dst = int32_t(uint32_t(dst) + uint32_t(src1));')
text = text.replace('set_ri2(opcode, src2 % src1, src2 / src1);', 'set_ri2(opcode, src1 ? src2 % src1 : 0, src1 ? src2 / src1 : 0);')
text = text.replace('set_ri(opcode, ((int32_t)t2)*((int32_t)t1));', 'set_ri(opcode, uint32_t(t2)*uint32_t(t1));')

# execute_run: an interrupt raised while the CPU runs (by one of its own accesses) is taken before the next
# instruction, as MAME does by ending the timeslice there (execute_run then starts again and checks it first)
old_loop = """	while(m_icount > 0) {
		m_PIP = m_IP;"""
new_loop = """	while(m_icount > 0) {
		if(m_irq_raised) {
			m_irq_raised = false;
			if(m_stall_state.burst_mode == false)
				check_immediate_irqs();
		}
		m_PIP = m_IP;"""
assert text.count(old_loop) == 1
text = text.replace(old_loop, new_loop)
assert text.count('I960_FATAL(') > 20
for bad in ['util::', 'machine()', 'fatalerror', 'std::rotl']:
    assert bad not in text, bad

hdr = '''// license:BSD-3-Clause
// copyright-holders:Farfetch'd, R. Belmont
//
// FBNeo PS5 2.3: the i960 instructions of MAME's src/devices/cpu/i960/i960.cpp (mamedev/mame b7f483f), lines
// 57-2379, made by a script (ported verbatim but for MAME's framework: the class is i960_core, fatalerror/logerror
// log through I960_FATAL/I960_LOG and carry on, util::sext/std::rotl/mulu_32x32 are local helpers, float -> integer
// conversions give x86's result without undefined behaviour, an integer division by zero gives 0 instead of
// stopping the process, and execute_run takes an interrupt raised while it runs before the next instruction, where
// MAME would end the timeslice). Included by i960.cpp only.
'''
open(sys.argv[2], 'w').write(hdr + '\n' + text + '\n')
