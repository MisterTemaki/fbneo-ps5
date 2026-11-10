#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 MisterTemaki
"""Checks a RADV title's PIE link (the ELF before ps5-native-tool link): what it imports, what it may not call, and
which inputs the linker loaded.

  check_link.py imports <pie.elf> <sdk-lib-dir> [--allow-module NAME.sprx ...]
      Every NEEDED module must be one a title may load: libkernel, libSceLibcInternal, libSceVideoOut, AGC's two
      (libSceAgc, libSceAgcDriver: RADV submits through them), and any --allow-module. Never libkernel_sys or
      libkernel_web, which only a payload may load. Every imported symbol must be in the stub of a NEEDED, allowed
      module. No C++ runtime symbol may be imported (_Z..., __cxa_* but the C library's atexit ones, __gxx_*,
      _Unwind_*, __dynamic_cast): the eboot carries its own libc++ / libc++abi / libunwind, whole. Nor the C locale's
      table _DefaultRuneLocale: the platform layer's ps5rt_locale.o defines it (the second M2 review).

  check_link.py syscalls-rsp <libc.a> <out.rsp>
      Writes "-y <symbol>" for each function the SDK's libc.a(syscalls.o) defines (raw system calls, for payloads),
      a response file for the link, so that its --trace says who defines each of them.
  check_link.py syscalls <trace>
      Fails if syscalls.o was loaded, or if anything outside it is said to reference a symbol it defines.

  check_link.py trace <trace> --sdk-lib DIR --libc-allow FILE [--own DIR ...] [--archive PATH ...] [--require NAME ...]
      Every input the link loaded (the --trace lines) must be one of: an object under an --own folder; an --archive
      (and its members); the SDK's libc++.a, libc++abi.a, libunwind.a, libpthread.a; a system-module stub (*.so) of the
      SDK but libkernel_sys / libkernel_web; a member of the SDK's libc.a named in the allow-list. Anything else fails,
      and these are named: the payload SDK fork's libps5platform.a, BlackBearReloaded's start-up (proto/native,
      app_crt.o, app_cpp_runtime.o), the SDK's crt objects. Each --require must be among the loaded objects.

Exit status 0 when the check passes, 1 when it fails, 2 on a usage error.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

TITLE_MODULES = [
    "libkernel.sprx",
    "libSceLibcInternal.sprx",
    "libSceVideoOut.sprx",
    "libSceAgc.sprx",
    "libSceAgcDriver.sprx",
]
PAYLOAD_ONLY = {"libkernel_sys.sprx", "libkernel_web.sprx"}
LIBC_ATEXIT = {"__cxa_atexit", "__cxa_finalize", "__cxa_thread_atexit", "__cxa_thread_atexit_impl"}
SDK_RUNTIME_ARCHIVES = {"libc++.a", "libc++abi.a", "libunwind.a", "libpthread.a"}


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def needed_modules(elf):
    out = []
    for line in run(["readelf", "-dW", elf]).splitlines():
        m = re.search(r"\(NEEDED\).*\[(.+)\]", line)
        if m:
            out.append(m.group(1))
    return out


def dynamic_symbols(path, which):
    names = set()
    for line in run(["nm", "-D", which, path]).splitlines():
        fields = line.split()
        if fields:
            names.add(fields[-1].split("@")[0])
    return names


def is_cxx_runtime(sym):
    if sym.startswith(("_Z", "__gxx_", "_Unwind_")) or sym == "__dynamic_cast":
        return True
    return sym.startswith("__cxa_") and sym not in LIBC_ATEXIT


# data the platform layer defines (src/ps5rt_locale.c): never taken from the system
OWN_DATA = {"_DefaultRuneLocale"}


def cmd_imports(args):
    allowed = TITLE_MODULES + [m for m in args.allow_module if m not in TITLE_MODULES]
    errors = []
    mods = needed_modules(args.elf)
    print("check_link imports: NEEDED %s" % " ".join(mods))
    stubs = {}
    for m in mods:
        if m in PAYLOAD_ONLY:
            errors.append("NEEDED %s: only a payload may load it, never a title" % m)
        elif m not in allowed:
            errors.append("NEEDED %s: not a module this title may load (%s)" % (m, ", ".join(allowed)))
        else:
            stub = os.path.join(args.sdk_lib, m[: -len(".sprx")] + ".so")
            if not os.path.exists(stub):
                errors.append("NEEDED %s: no stub %s" % (m, stub))
            else:
                stubs[m] = dynamic_symbols(stub, "--defined-only")
    counts = {m: 0 for m in stubs}
    for sym in sorted(dynamic_symbols(args.elf, "--undefined-only")):
        if is_cxx_runtime(sym):
            errors.append("import %s: the C++ runtime must be the eboot's own, not a system module's" % sym)
            continue
        if sym in OWN_DATA:
            errors.append("import %s: the platform layer's (ps5rt_locale.o) must be the one linked" % sym)
            continue
        owner = next((m for m in mods if m in stubs and sym in stubs[m]), None)
        if owner:
            counts[owner] += 1
        else:
            errors.append("import %s: no allowed NEEDED module defines it" % sym)
    print("check_link imports: by module: %s" % ", ".join("%s %d" % kv for kv in counts.items()))
    return report("imports", errors)


def cmd_syscalls_rsp(args):
    with tempfile.TemporaryDirectory() as tmp:
        member = os.path.join(tmp, "syscalls.o")
        with open(member, "wb") as f:
            f.write(subprocess.run(["ar", "p", args.libc, "syscalls.o"], check=True, capture_output=True).stdout)
        syms = sorted({line.split()[-1] for line in run(["nm", "-g", "--defined-only", member]).splitlines()
                       if len(line.split()) >= 3})
    if not syms:
        print("check_link syscalls-rsp: %s has no syscalls.o symbols" % args.libc, file=sys.stderr)
        return 1
    with open(args.out + ".part", "w") as f:
        f.writelines("-y %s\n" % s for s in syms)
    os.replace(args.out + ".part", args.out)
    return 0


def trace_lines(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return [line.rstrip("\n") for line in f]


def cmd_syscalls(args):
    errors = []
    defined = set()
    refs = []
    for line in trace_lines(args.trace):
        if re.search(r"libc\.a\(syscalls\.o\)$", line):
            errors.append("libc.a(syscalls.o) is loaded")
        m = re.match(r"(.+): (lazy definition of|shared definition of|definition of|reference to) (\S+)$", line)
        if not m:
            continue
        who, what, sym = m.groups()
        if what == "definition of" and who.endswith("libc.a(syscalls.o)"):
            defined.add(sym)
        elif what == "reference to" and not who.endswith("libc.a(syscalls.o)") and who != "<internal>":
            refs.append((who, sym))
    for who, sym in refs:
        if sym in defined:
            errors.append("%s calls %s from libc.a(syscalls.o), a raw system call" % (who, sym))
    return report("syscalls", errors)


def load_allow(path):
    allow = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                fields = line.split(None, 2)
                allow[fields[0]] = fields[1] if len(fields) > 1 else "?"
    return allow


def under(path, folder):
    path, folder = os.path.realpath(path), os.path.realpath(folder)
    return path == folder or path.startswith(folder + os.sep)


def cmd_trace(args):
    allow = load_allow(args.libc_allow)
    sdk_lib = os.path.realpath(args.sdk_lib)
    archives = {os.path.realpath(a) for a in args.archive}
    errors = []
    loaded = []
    libc_used = {}
    for line in trace_lines(args.trace):
        if not line or ": " in line or line.startswith(("ld.lld", ">>>", "<internal>")):
            continue
        loaded.append(line)
        m = re.match(r"(.+)\(([^()]+)\)$", line)
        container, member = (m.group(1), m.group(2)) if m else (line, None)
        real = os.path.realpath(container)
        base = os.path.basename(real)
        if "libps5platform" in base:
            errors.append("%s: the payload SDK fork's platform layer (GPL-3.0-or-later)" % line)
        elif re.search(r"(^|/)proto/native/", line) or base in ("app_crt.o", "app_cpp_runtime.o"):
            errors.append("%s: BlackBearReloaded's boilerplate start-up (GPL-3.0-or-later)" % line)
        elif os.path.dirname(real) == sdk_lib and re.match(r"crt[\w]*\.o$", base):
            errors.append("%s: the payload SDK's start-up object" % line)
        elif real == os.path.join(sdk_lib, "libc.a"):
            if member not in allow:
                errors.append("%s: not in the libc.a allow-list (%s)" % (line, args.libc_allow))
            else:
                libc_used[member] = allow[member]
        elif real in archives:
            pass
        elif os.path.dirname(real) == sdk_lib and base in SDK_RUNTIME_ARCHIVES:
            pass
        elif os.path.dirname(real) == sdk_lib and base.endswith(".so") and member is None:
            if base[:-3] + ".sprx" in PAYLOAD_ONLY:
                errors.append("%s: a payload-only module's stub" % line)
        elif member is None and base.endswith(".o") and any(under(real, d) for d in args.own):
            pass
        else:
            errors.append("%s: not an input this link may have" % line)
    for name in args.require:
        if not any(os.path.basename(l) == name or l.endswith("(%s)" % name) for l in loaded):
            errors.append("%s is not in the link" % name)
    print("check_link trace: %d inputs; libc.a members: %s" % (
        len(loaded), ", ".join("%s (%s)" % kv for kv in sorted(libc_used.items())) or "none"))
    return report("trace", errors)


def report(what, errors):
    for e in errors:
        print("check_link %s: ERROR: %s" % (what, e))
    print("check_link %s: %s" % (what, "FAILED" if errors else "ok"))
    return 1 if errors else 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("imports")
    s.add_argument("elf")
    s.add_argument("sdk_lib")
    s.add_argument("--allow-module", action="append", default=[])
    s = sub.add_parser("syscalls-rsp")
    s.add_argument("libc")
    s.add_argument("out")
    s = sub.add_parser("syscalls")
    s.add_argument("trace")
    s = sub.add_parser("trace")
    s.add_argument("trace")
    s.add_argument("--sdk-lib", required=True)
    s.add_argument("--libc-allow", required=True)
    s.add_argument("--own", action="append", default=[])
    s.add_argument("--archive", action="append", default=[])
    s.add_argument("--require", action="append", default=[])
    args = p.parse_args()
    return {"imports": cmd_imports, "syscalls-rsp": cmd_syscalls_rsp, "syscalls": cmd_syscalls,
            "trace": cmd_trace}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
