#!/usr/bin/env python3
"""
Analyze EfzRevival.dll v1.02e to find Jcc bytes guarding ExitProcess calls.

The DLL has image base 0x10000000.
We need to find the conditional jump instructions near ExitProcess call sites
in three functions:
  - sub_10072310 (EFZ_Rollback_BatchAdvanceSimple) at RVA 0x72310
  - sub_10072500 (EFZ_Main_RollbackLoopTick) at RVA 0x72500
  - sub_100742A0 at RVA 0x742A0
"""

import struct
import sys
import os

DLL_PATH = os.path.join(os.path.dirname(__file__), 
                         "..", "decompilations", "revival_binaries", "1.02e", "EfzRevival.dll")

def read_dll():
    with open(DLL_PATH, "rb") as f:
        return f.read()

def get_sections(data):
    """Parse PE sections to map RVA to file offset."""
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    # PE signature at e_lfanew, then COFF header
    coff_offset = e_lfanew + 4
    num_sections = struct.unpack_from("<H", data, coff_offset + 2)[0]
    opt_hdr_size = struct.unpack_from("<H", data, coff_offset + 16)[0]
    section_offset = coff_offset + 20 + opt_hdr_size
    
    sections = []
    for i in range(num_sections):
        off = section_offset + i * 40
        name = data[off:off+8].rstrip(b'\x00').decode('ascii', errors='replace')
        virt_size = struct.unpack_from("<I", data, off + 8)[0]
        virt_addr = struct.unpack_from("<I", data, off + 12)[0]
        raw_size = struct.unpack_from("<I", data, off + 16)[0]
        raw_ptr = struct.unpack_from("<I", data, off + 20)[0]
        sections.append((name, virt_addr, virt_size, raw_ptr, raw_size))
    return sections

def rva_to_file_offset(sections, rva):
    for name, va, vs, rp, rs in sections:
        if va <= rva < va + vs:
            return rp + (rva - va)
    return None

def find_iat_exitprocess(data, sections):
    """Find the IAT entry for ExitProcess by looking at imports."""
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    coff_offset = e_lfanew + 4
    opt_offset = coff_offset + 20
    
    # Check PE32 magic
    magic = struct.unpack_from("<H", data, opt_offset)[0]
    if magic == 0x10B:  # PE32
        import_dir_rva = struct.unpack_from("<I", data, opt_offset + 104)[0]
        import_dir_size = struct.unpack_from("<I", data, opt_offset + 108)[0]
    else:
        print(f"Unexpected PE magic: 0x{magic:04X}")
        return None
    
    if import_dir_rva == 0:
        return None
    
    imp_offset = rva_to_file_offset(sections, import_dir_rva)
    
    # Iterate import descriptors (20 bytes each)
    idx = 0
    while True:
        desc_off = imp_offset + idx * 20
        ilt_rva = struct.unpack_from("<I", data, desc_off)[0]
        name_rva = struct.unpack_from("<I", data, desc_off + 12)[0]
        iat_rva = struct.unpack_from("<I", data, desc_off + 16)[0]
        
        if ilt_rva == 0 and name_rva == 0 and iat_rva == 0:
            break
        
        # Read DLL name
        name_off = rva_to_file_offset(sections, name_rva)
        dll_name = b""
        j = 0
        while data[name_off + j] != 0:
            dll_name += bytes([data[name_off + j]])
            j += 1
        dll_name = dll_name.decode('ascii', errors='replace').lower()
        
        if "kernel32" in dll_name:
            # Scan ILT/IAT entries for ExitProcess
            ilt_off = rva_to_file_offset(sections, ilt_rva)
            entry_idx = 0
            while True:
                thunk = struct.unpack_from("<I", data, ilt_off + entry_idx * 4)[0]
                if thunk == 0:
                    break
                
                if thunk & 0x80000000:  # ordinal import
                    entry_idx += 1
                    continue
                
                # Name import - thunk is RVA to hint/name
                hint_off = rva_to_file_offset(sections, thunk)
                hint = struct.unpack_from("<H", data, hint_off)[0]
                fname = b""
                k = 0
                while data[hint_off + 2 + k] != 0:
                    fname += bytes([data[hint_off + 2 + k]])
                    k += 1
                fname = fname.decode('ascii', errors='replace')
                
                if fname == "ExitProcess":
                    actual_iat_rva = iat_rva + entry_idx * 4
                    print(f"  Found ExitProcess IAT entry at RVA 0x{actual_iat_rva:08X}")
                    print(f"  VA = 0x{0x10000000 + actual_iat_rva:08X}")
                    return actual_iat_rva
                
                entry_idx += 1
        
        idx += 1
    
    return None

def disasm_region(data, sections, rva_start, length, label=""):
    """Simple x86 disassembler for finding Jcc and call instructions."""
    file_off = rva_to_file_offset(sections, rva_start)
    if file_off is None:
        print(f"Cannot map RVA 0x{rva_start:X} to file offset")
        return
    
    region = data[file_off:file_off + length]
    
    print(f"\n{'='*70}")
    print(f"  Disassembly: {label}")
    print(f"  RVA 0x{rva_start:05X} - 0x{rva_start + length - 1:05X}")
    print(f"  File offset 0x{file_off:X}")
    print(f"{'='*70}")
    
    i = 0
    while i < length:
        rva = rva_start + i
        b = region[i]
        
        # Format raw bytes (up to 8)
        raw_start = i
        
        instr = ""
        size = 1
        
        # Recognize key instructions
        if b == 0xCC:
            instr = "int3"
        elif b == 0xC3:
            instr = "ret"
        elif b == 0xC2:
            if i + 2 < length:
                imm16 = struct.unpack_from("<H", region, i + 1)[0]
                instr = f"ret 0x{imm16:X}"
                size = 3
        elif b == 0x90:
            instr = "nop"
        elif b == 0xEB:
            if i + 1 < length:
                rel = struct.unpack_from("<b", region, i + 1)[0]
                target = rva + 2 + rel
                instr = f"jmp short 0x{target:05X}  (rel={rel:+d})"
                size = 2
        elif b in (0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B,
                   0x7C, 0x7D, 0x7E, 0x7F, 0x70, 0x71, 0x72, 0x73):
            jcc_names = {
                0x70: "jo", 0x71: "jno", 0x72: "jb", 0x73: "jnb",
                0x74: "jz/je", 0x75: "jnz/jne", 0x76: "jbe", 0x77: "ja",
                0x78: "js", 0x79: "jns", 0x7A: "jp", 0x7B: "jnp",
                0x7C: "jl", 0x7D: "jge", 0x7E: "jle", 0x7F: "jg"
            }
            if i + 1 < length:
                rel = struct.unpack_from("<b", region, i + 1)[0]
                target = rva + 2 + rel
                name = jcc_names.get(b, f"j?? (0x{b:02X})")
                instr = f"*** {name} 0x{target:05X}  (rel={rel:+d}) *** <-- Jcc at RVA 0x{rva:05X}, opcode 0x{b:02X}"
                size = 2
        elif b == 0x0F:
            if i + 1 < length:
                b2 = region[i + 1]
                if 0x80 <= b2 <= 0x8F:
                    jcc_names = {
                        0x80: "jo", 0x81: "jno", 0x82: "jb", 0x83: "jnb",
                        0x84: "jz/je", 0x85: "jnz/jne", 0x86: "jbe", 0x87: "ja",
                        0x88: "js", 0x89: "jns", 0x8A: "jp", 0x8B: "jnp",
                        0x8C: "jl", 0x8D: "jge", 0x8E: "jle", 0x8F: "jg"
                    }
                    if i + 5 < length:
                        rel32 = struct.unpack_from("<i", region, i + 2)[0]
                        target = rva + 6 + rel32
                        name = jcc_names.get(b2, f"j?? (0F {b2:02X})")
                        instr = f"*** {name} NEAR 0x{target:05X}  (rel={rel32:+d}) *** <-- Jcc at RVA 0x{rva:05X}, opcode 0x0F 0x{b2:02X}"
                        size = 6
                else:
                    instr = f"0F {b2:02X} ..."
                    size = 2
        elif b == 0xE8:  # call rel32
            if i + 4 < length:
                rel32 = struct.unpack_from("<i", region, i + 1)[0]
                target = rva + 5 + rel32
                instr = f"call 0x{target:05X}  (rel={rel32:+d})"
                size = 5
        elif b == 0xE9:  # jmp rel32
            if i + 4 < length:
                rel32 = struct.unpack_from("<i", region, i + 1)[0]
                target = rva + 5 + rel32
                instr = f"jmp near 0x{target:05X}  (rel={rel32:+d})"
                size = 5
        elif b == 0xFF:
            if i + 1 < length:
                modrm = region[i + 1]
                reg = (modrm >> 3) & 7
                mod = (modrm >> 6) & 3
                rm = modrm & 7
                if reg == 2:  # call
                    if mod == 0 and rm == 5:  # call [disp32]
                        if i + 5 < length:
                            disp32 = struct.unpack_from("<I", region, i + 2)[0]
                            eff_rva = disp32 - 0x10000000
                            instr = f"call dword ptr [0x{disp32:08X}]  (IAT RVA 0x{eff_rva:05X})"
                            size = 6
                    else:
                        instr = f"call [modrm=0x{modrm:02X}]"
                        size = 2
                elif reg == 4:  # jmp
                    if mod == 0 and rm == 5:
                        if i + 5 < length:
                            disp32 = struct.unpack_from("<I", region, i + 2)[0]
                            instr = f"jmp dword ptr [0x{disp32:08X}]"
                            size = 6
                    else:
                        instr = f"jmp [modrm=0x{modrm:02X}]"
                        size = 2
                elif reg == 6:  # push
                    if mod == 0 and rm == 5:
                        if i + 5 < length:
                            disp32 = struct.unpack_from("<I", region, i + 2)[0]
                            instr = f"push dword ptr [0x{disp32:08X}]"
                            size = 6
                else:
                    instr = f"FF {modrm:02X} (reg={reg})"
                    size = 2
        elif b == 0x55:
            instr = "push ebp"
        elif b == 0x8B and i + 1 < length and region[i+1] == 0xEC:
            instr = "mov ebp, esp"
            size = 2
        elif b == 0x56:
            instr = "push esi"
        elif b == 0x57:
            instr = "push edi"
        elif b == 0x53:
            instr = "push ebx"
        elif b == 0x5D:
            instr = "pop ebp"
        elif b == 0x5E:
            instr = "pop esi"
        elif b == 0x5F:
            instr = "pop edi"
        elif b == 0x5B:
            instr = "pop ebx"
        elif b == 0x51:
            instr = "push ecx"
        elif b == 0x52:
            instr = "push edx"
        elif b == 0x50:
            instr = "push eax"
        elif b == 0x6A:
            if i + 1 < length:
                imm8 = region[i + 1]
                instr = f"push 0x{imm8:02X}"
                size = 2
        elif b == 0x68:
            if i + 4 < length:
                imm32 = struct.unpack_from("<I", region, i + 1)[0]
                instr = f"push 0x{imm32:08X}"
                size = 5
        elif b == 0x83:
            if i + 2 < length:
                modrm = region[i + 1]
                reg = (modrm >> 3) & 7
                ops = {0: "add", 1: "or", 2: "adc", 3: "sbb", 4: "and", 5: "sub", 6: "xor", 7: "cmp"}
                op = ops.get(reg, f"op{reg}")
                imm8 = region[i + 2]
                mod = (modrm >> 6) & 3
                rm = modrm & 7
                if mod == 3:
                    regs = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
                    instr = f"{op} {regs[rm]}, 0x{imm8:02X}"
                    size = 3
                elif mod == 1:
                    if i + 3 < length:
                        disp8 = struct.unpack_from("<b", region, i + 2)[0]
                        imm8_val = region[i + 3]
                        regs = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
                        instr = f"{op} dword ptr [{regs[rm]}+0x{disp8 & 0xFF:02X}], 0x{imm8_val:02X}"
                        size = 4
                else:
                    instr = f"83 {modrm:02X} {imm8:02X}"
                    size = 3
        elif b == 0x85:  # test r/m32, r32
            if i + 1 < length:
                modrm = region[i + 1]
                mod = (modrm >> 6) & 3
                reg = (modrm >> 3) & 7
                rm = modrm & 7
                regs = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
                if mod == 3:
                    instr = f"test {regs[rm]}, {regs[reg]}"
                    size = 2
                else:
                    instr = f"test [modrm=0x{modrm:02X}], {regs[reg]}"
                    size = 2
        elif b == 0x84:  # test r/m8, r8
            if i + 1 < length:
                modrm = region[i + 1]
                instr = f"test r/m8 (modrm=0x{modrm:02X})"
                size = 2
        elif b == 0x8B:  # mov r32, r/m32
            if i + 1 < length:
                modrm = region[i + 1]
                mod = (modrm >> 6) & 3
                reg = (modrm >> 3) & 7
                rm = modrm & 7
                regs = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
                if mod == 3:
                    instr = f"mov {regs[reg]}, {regs[rm]}"
                    size = 2
                elif mod == 2:
                    if i + 5 < length:
                        disp32 = struct.unpack_from("<i", region, i + 2)[0]
                        instr = f"mov {regs[reg]}, [{regs[rm]}+0x{disp32 & 0xFFFFFFFF:X}]"
                        size = 6
                elif mod == 1:
                    if i + 2 < length:
                        disp8 = struct.unpack_from("<b", region, i + 2)[0]
                        instr = f"mov {regs[reg]}, [{regs[rm]}+0x{disp8 & 0xFF:02X}]"
                        size = 3
                elif mod == 0:
                    if rm == 5:
                        if i + 5 < length:
                            disp32 = struct.unpack_from("<I", region, i + 2)[0]
                            instr = f"mov {regs[reg]}, [0x{disp32:08X}]"
                            size = 6
                    else:
                        instr = f"mov {regs[reg]}, [{regs[rm]}]"
                        size = 2
        elif b == 0x3D:  # cmp eax, imm32
            if i + 4 < length:
                imm32 = struct.unpack_from("<I", region, i + 1)[0]
                instr = f"cmp eax, 0x{imm32:08X}"
                size = 5
        elif b == 0x83 and i + 2 < length:
            pass  # handled above
        elif b == 0xA1:  # mov eax, [moffs32]
            if i + 4 < length:
                addr = struct.unpack_from("<I", region, i + 1)[0]
                instr = f"mov eax, [0x{addr:08X}]"
                size = 5
        
        # Format output
        raw_bytes = ' '.join(f'{region[i+j]:02X}' for j in range(min(size, 8)))
        if not instr:
            instr = f"db 0x{b:02X}"
        
        va = 0x10000000 + rva
        print(f"  {va:08X}  (RVA {rva:05X})  {raw_bytes:<24s}  {instr}")
        
        i += size


def find_exitprocess_calls(data, sections, iat_rva, rva_start, rva_end):
    """Find all FF 15 calls to ExitProcess IAT in a range."""
    iat_va = 0x10000000 + iat_rva
    iat_bytes = struct.pack("<I", iat_va)
    call_prefix = b'\xFF\x15'
    pattern = call_prefix + iat_bytes
    
    file_start = rva_to_file_offset(sections, rva_start)
    file_end = rva_to_file_offset(sections, rva_end)
    
    results = []
    region = data[file_start:file_end]
    pos = 0
    while True:
        idx = region.find(pattern, pos)
        if idx == -1:
            break
        call_rva = rva_start + idx
        results.append(call_rva)
        pos = idx + 1
    
    return results


def main():
    print("Loading DLL:", os.path.abspath(DLL_PATH))
    data = read_dll()
    print(f"DLL size: {len(data)} bytes")
    
    sections = get_sections(data)
    print("\nSections:")
    for name, va, vs, rp, rs in sections:
        print(f"  {name:8s}  VA=0x{va:08X}  VSize=0x{vs:X}  RawPtr=0x{rp:X}  RawSize=0x{rs:X}")
    
    print("\n--- Finding ExitProcess IAT entry ---")
    iat_rva = find_iat_exitprocess(data, sections)
    
    if iat_rva:
        print(f"\n--- Searching for all ExitProcess calls in .text section ---")
        # Find .text section bounds
        for name, va, vs, rp, rs in sections:
            if name == ".text":
                calls = find_exitprocess_calls(data, sections, iat_rva, va, va + vs)
                print(f"\nFound {len(calls)} ExitProcess call sites:")
                for c in calls:
                    print(f"  RVA 0x{c:05X}  (VA 0x{0x10000000 + c:08X})")
    
    # Now disassemble the three target functions
    print("\n\n" + "="*70)
    print("  SITE 2: EFZ_Rollback_BatchAdvanceSimple (sub_10072310)")
    print("="*70)
    disasm_region(data, sections, 0x72310, 0xA0, "sub_10072310 - first 0xA0 bytes")
    
    print("\n\n" + "="*70)
    print("  SITE 3: EFZ_Main_RollbackLoopTick (sub_10072500)")
    print("="*70)
    disasm_region(data, sections, 0x72500, 0x100, "sub_10072500 - first 0x100 bytes")
    
    print("\n\n" + "="*70)
    print("  SITE 4: sub_100742A0")
    print("="*70)
    disasm_region(data, sections, 0x742A0, 0x180, "sub_100742A0 - first 0x180 bytes")
    
    # Disassemble the ExitProcess block areas for Sites 3 and 4
    print("\n\n" + "="*70)
    print("  SITE 3 ExitProcess block (target of jumps): around RVA 0x727D7")
    print("="*70)
    disasm_region(data, sections, 0x727D0, 0x40, "ExitProcess block for sub_10072500")
    
    print("\n\n" + "="*70)
    print("  SITE 4 ExitProcess block (target of jumps): around RVA 0x74490")
    print("="*70)
    disasm_region(data, sections, 0x74488, 0x40, "ExitProcess block for sub_100742A0")
    
    # Also dump raw bytes at key Jcc locations for verification
    print("\n\n" + "="*70)
    print("  RAW BYTE VERIFICATION")
    print("="*70)
    
    sites = [
        ("Site 2 Jcc", 0x7231F, 8),
        ("Site 3 Jcc #1", 0x7251B, 8),
        ("Site 3 Jcc #2", 0x7252E, 8),
        ("Site 4 Jcc #1", 0x742E1, 8),
        ("Site 4 Jcc #2", 0x742F4, 8),
        ("Site 4 Jcc #3", 0x74301, 8),
        ("Site 3 ExitProcess call", 0x727F8, 8),
        ("Site 4 ExitProcess call", 0x744B1, 8),
        ("Site 2 ExitProcess call", 0x72323, 8),
        ("Unknown ExitProcess call", 0x4B66C, 8),
    ]
    
    for label, rva, count in sites:
        fo = rva_to_file_offset(sections, rva)
        raw = data[fo:fo+count]
        hex_str = ' '.join(f'{b:02X}' for b in raw)
        print(f"  {label}: RVA 0x{rva:05X} → {hex_str}")
    
    # Also show the already-patched sites for reference
    print("\n\n" + "="*70)
    print("  ALREADY PATCHED: Site at RVA 0x7909D")
    print("="*70)
    disasm_region(data, sections, 0x79090, 0x30, "around RVA 0x7909D")
    
    print("\n\n" + "="*70)
    print("  ALREADY PATCHED: Site at RVA 0x7215D")
    print("="*70)
    disasm_region(data, sections, 0x72150, 0x50, "around RVA 0x7215D")
    
    # Disassemble around the unknown ExitProcess site
    print("\n\n" + "="*70)
    print("  UNKNOWN: ExitProcess call at RVA 0x4B66C")
    print("="*70)
    disasm_region(data, sections, 0x4B650, 0x40, "around RVA 0x4B66C")

if __name__ == "__main__":
    main()
