#!/usr/bin/env python3
"""Make the SM7150's clock table (branches and PLLs) from msm-4.14's drivers/clk/qcom/*-sdmmagpie.c, for
devices/sunfish/drivers/power/hakios.clocks_sm7150.c. Prints C to stdout.

usage: tools/clk-table.py <msm-4.14>/drivers/clk/qcom
"""
import re
import sys

BASES = [("gcc", 0x100000), ("dispcc", 0xaf00000), ("gpucc", 0x5090000), ("videocc", 0xab00000),
         ("camcc", 0xad00000), ("npucc", 0x9910000)]

block_re = re.compile(r"^static struct (clk_branch|clk_alpha_pll) (\w+) = \{\n(.*?)^\};", re.S | re.M)


def num(pattern, text):
    m = re.search(pattern, text)
    return int(m.group(1), 0) if m else None


def main(d):
    rows = []
    for cc, base in BASES:
        src = open(f"{d}/{cc}-sdmmagpie.c").read()
        for kind, name, body in block_re.findall(src):
            enable = num(r"\.enable_reg = (0x[0-9a-fA-F]+)", body)
            bit = num(r"\.enable_mask = BIT\((\d+)\)", body)
            if kind == "clk_branch":
                halt = num(r"\.halt_reg = (0x[0-9a-fA-F]+)", body)
                if halt is None or enable is None or bit is None:
                    continue
                rows.append((name, base, halt, enable, bit, 0))
            else:
                off = num(r"\.offset = (0x[0-9a-fA-F]+)", body)
                if off is None:
                    continue
                # a PLL: its MODE register is at the offset; bit 31 LOCK_DET, bit 0 OUTCTRL
                rows.append((name, base, off, enable if enable is not None else off, bit if bit is not None else 0, 1))
    print("/* Made by tools/clk-table.py from msm-4.14's drivers/clk/qcom/*-sdmmagpie.c: do not edit. */")
    print("static const struct clk_row clk_rows[] = {")
    for name, base, halt, enable, bit, pll in rows:
        print(f'\t{{ "{name}", {base:#x}, {halt:#x}, {enable:#x}, {bit}, {pll} }},')
    print("};")


main(sys.argv[1])
