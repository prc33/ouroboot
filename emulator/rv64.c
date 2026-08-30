/* Minimal RV64IM/Sv39 machine for Ouroboot.  No host ABI or libc. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed int i32;
typedef signed long long i64;

#define RAM_BASE 0x80000000ULL
#define RAM_SIZE 0x08000000U
#define UART_BASE 0x10000000ULL
#define FETCH_BASE 0x10000100ULL
#define UART_CAP 4096U

#define SSTATUS 0x100
#define SIE 0x104
#define STVEC 0x105
#define SSCRATCH 0x140
#define SEPC 0x141
#define SCAUSE 0x142
#define STVAL 0x143
#define SIP 0x144
#define SATP 0x180
#define STIMECMP 0x14d
#define TIME 0xc01

#define STATUS_SIE (1ULL << 1)
#define STATUS_SPIE (1ULL << 5)
#define STATUS_SPP (1ULL << 8)
#define STATUS_SUM (1ULL << 18)
#define IP_STIP (1ULL << 5)

static u8 ram[RAM_SIZE];
static u64 x[32], f[32], csr[4096];
static u64 pc, ticks, timecmp;
static u32 privilege, halted, fault, tick_step, timer_steps;
static u64 fault_addr;

static u8 uart_in[UART_CAP], uart_out[UART_CAP];
static u32 in_read, in_write, out_read, out_write;
static u64 fetch_url, fetch_destination;
static u32 fetch_url_length, fetch_capacity, fetch_length, fetch_status;

static u64 tlb_tag[512], tlb_ppage[512];
static u64 fetch_vpage = ~0ULL, fetch_delta;
/* Spell unsigned 64-bit float conversions in terms of signed ones so the
 * tiny wasm32 backend needs no compiler-rt conversion helpers. */
static double i64_to_double(i64 n)
{
    return (double)n;
}
static double u64_to_double(u64 n)
{
    return i64_to_double((i64)(n >> 1)) * 2.0 + (u32)(n & 1);
}
static u64 double_to_u64(double n)
{
    return n < 9223372036854775808.0 ? (u64)(i64)n :
        ((u64)(i64)(n - 9223372036854775808.0) | (1ULL << 63));
}
u32 rv_ram(void) { return (u32)(unsigned long)ram; }
u32 rv_ram_size(void) { return RAM_SIZE; }
u64 rv_pc(void) { return pc; }
static void flush_tlb(void)
{
    u32 i;
    for (i = 0; i < 512; ++i) tlb_tag[i] = 0;
    fetch_vpage = ~0ULL;
}
static int physical_read(u64 addr, u32 size, u64 *value)
{
    u64 off = addr - RAM_BASE;
    if (off < RAM_SIZE && off + size <= RAM_SIZE) {
        const u8 *p = ram + (u32)off;
        if (size == 1) *value = *p;
        else if (size == 2) *value = *(u16 *)p;
        else if (size == 4) *value = *(u32 *)p;
        else *value = *(u64 *)p;
        return 1;
    }
    if (addr >= UART_BASE && addr < UART_BASE + 8 && size == 1) {
        u32 reg = (u32)(addr - UART_BASE);
        if (reg == 0) {
            if (in_read == in_write) {
                *value = 0;
            } else {
                *value = uart_in[in_read & (UART_CAP - 1)];
                ++in_read;
            }
        } else if (reg == 5) {
            *value = 0x20 | (in_read != in_write);
        } else *value = 0;
        return 1;
    }
    if (addr >= FETCH_BASE && addr < FETCH_BASE + 32 && size == 4) {
        u32 reg = (u32)(addr - FETCH_BASE);
        if (reg == 0) *value = fetch_status;
        else if (reg == 4) *value = (u32)fetch_url;
        else if (reg == 8) *value = fetch_url_length;
        else if (reg == 12) *value = (u32)fetch_destination;
        else if (reg == 16) *value = fetch_capacity;
        else if (reg == 20) *value = fetch_length;
        else *value = 0;
        return 1;
    }
    fault_addr = addr;
    fault = 2;
    return 0;
}
static int physical_write(u64 addr, u32 size, u64 value)
{
    u64 off = addr - RAM_BASE;
    if (off < RAM_SIZE && off + size <= RAM_SIZE) {
        u8 *p = ram + (u32)off;
        if (size == 1) *p = value;
        else if (size == 2) *(u16 *)p = value;
        else if (size == 4) *(u32 *)p = value;
        else *(u64 *)p = value;
        return 1;
    }
    if (addr >= UART_BASE && addr < UART_BASE + 8 && size == 1) {
        if (addr == UART_BASE && out_write - out_read < UART_CAP)
            uart_out[out_write++ & (UART_CAP - 1)] = value;
        return 1;
    }
    if (addr >= FETCH_BASE && addr < FETCH_BASE + 32 && size == 4) {
        u32 reg = (u32)(addr - FETCH_BASE);
        if (reg == 0) fetch_status = (u32)value;
        else if (reg == 4) fetch_url = (u32)value;
        else if (reg == 8) fetch_url_length = (u32)value;
        else if (reg == 12) fetch_destination = (u32)value;
        else if (reg == 16) fetch_capacity = (u32)value;
        return 1;
    }
    fault_addr = addr;
    fault = 2;
    return 0;
}
static int translate(u64 va, u32 access, u64 *pa)
{
    u64 satp = csr[SATP], table, pte = 0;
    u64 tag;
    u32 mode = satp >> 60, level, context, slot;
    if (mode == 0) { *pa = va; return 1; }
    if (mode != 8) { fault_addr = va; fault = 1; return 0; }

    context = privilege | ((csr[SSTATUS] & STATUS_SUM) ? 2 : 0);
    slot = ((u32)(va >> 12) & 255) + (access == 2 ? 256 : 0);
    tag = (((va >> 12) << 2) | context) + 1;
    if (tlb_tag[slot] == tag) {
        *pa = tlb_ppage[slot] | (va & 4095);
        return 1;
    }

    table = (satp & ((1ULL << 44) - 1)) << 12;
    for (level = 3; level-- != 0;) {
        u64 idx = (va >> (12 + 9 * level)) & 511;
        if (!physical_read(table + idx * 8, 8, &pte)) { fault_addr = va; fault = 1; return 0; }
        if (!(pte & 1)) { fault_addr = va; fault = 1; return 0; }
        if (pte & 14) break;
        table = (pte >> 10) << 12;
    }
    if (!(pte & 14) || (access == 0 && !(pte & 8)) ||
        (access == 1 && !(pte & 2)) || (access == 2 && !(pte & 4)) ||
        ((pte & 16) && privilege == 0 && !(csr[SSTATUS] & STATUS_SUM)) ||
        (!(pte & 16) && privilege == 1)) {
        fault_addr = va;
        fault = 1;
        return 0;
    }
    *pa = ((pte >> 10) << 12) | (va & 4095);
    tlb_tag[slot] = tag;
    tlb_ppage[slot] = *pa & ~4095ULL;
    return 1;
}
static int load(u64 va, u32 size, u32 access, u64 *v)
{
    u64 pa, off, tag;
    u32 context, slot;
    if ((va & 4095) + size > 4096) {
        u64 byte;
        *v = 0;
        for (u32 i = 0; i < size; i++) {
            if (!translate(va + i, access, &pa) || !physical_read(pa, 1, &byte)) return 0;
            *v |= byte << (i * 8);
        }
        return 1;
    }
    context = privilege | ((csr[SSTATUS] & STATUS_SUM) ? 2 : 0);
    slot = ((u32)(va >> 12) & 255) + (access == 2 ? 256 : 0);
    tag = (((va >> 12) << 2) | context) + 1;
    if (tlb_tag[slot] == tag) pa = tlb_ppage[slot] | (va & 4095);
    else if (!translate(va, access, &pa)) return 0;
    off = pa - RAM_BASE;
    if (off < RAM_SIZE && off + size <= RAM_SIZE) {
        const u8 *p = ram + (u32)off;
        if (size == 1) *v = *p;
        else if (size == 2) *v = *(u16 *)p;
        else if (size == 4) *v = *(u32 *)p;
        else *v = *(u64 *)p;
        return 1;
    }
    return physical_read(pa, size, v);
}
static int store(u64 va, u32 size, u64 v)
{
    u64 pa, off, tag;
    u32 context, slot;
    if ((va & 4095) + size > 4096) {
        for (u32 i = 0; i < size; i++) {
            if (!translate(va + i, 2, &pa) || !physical_write(pa, 1, v >> (i * 8))) return 0;
        }
        return 1;
    }
    context = privilege | ((csr[SSTATUS] & STATUS_SUM) ? 2 : 0);
    slot = ((u32)(va >> 12) & 255) + 256;
    tag = (((va >> 12) << 2) | context) + 1;
    if (tlb_tag[slot] == tag) pa = tlb_ppage[slot] | (va & 4095);
    else if (!translate(va, 2, &pa)) return 0;
    off = pa - RAM_BASE;
    if (off < RAM_SIZE && off + size <= RAM_SIZE) {
        u8 *p = ram + (u32)off;
        if (size == 1) *p = v;
        else if (size == 2) *(u16 *)p = v;
        else if (size == 4) *(u32 *)p = v;
        else *(u64 *)p = v;
        return 1;
    }
    return physical_write(pa, size, v);
}
static void set_csr(u32 n, u64 v)
{
    csr[n] = v;
    if (n == STIMECMP) {
        u64 steps = v > ticks && tick_step ?
            (v - ticks + tick_step - 1) / tick_step : !!v;
        timecmp = v;
        if (!v || ticks < v) csr[SIP] &= ~IP_STIP;
        timer_steps = steps > 0xffffffffU ? 0xffffffffU : (u32)steps;
    }
    if (n == SATP || n == SSTATUS) flush_tlb();
}
static void trap(u64 cause, u64 value, int interrupt)
{
    u64 status = csr[SSTATUS];
    csr[SEPC] = pc;
    csr[SCAUSE] = cause | (interrupt ? 1ULL << 63 : 0);
    csr[STVAL] = value;
    if (privilege == 0) status |= STATUS_SPP; else status &= ~STATUS_SPP;
    if (status & STATUS_SIE) status |= STATUS_SPIE; else status &= ~STATUS_SPIE;
    status &= ~STATUS_SIE;
    csr[SSTATUS] = status;
    privilege = 0;
    pc = csr[STVEC] & ~3ULL;
    halted = 0;
    flush_tlb();
}
static u64 mulhu(u64 a, u64 b)
{
    u64 a0 = (u32)a, a1 = a >> 32, b0 = (u32)b, b1 = b >> 32;
    u64 p0 = a0 * b0, p1 = a0 * b1, p2 = a1 * b0, p3 = a1 * b1;
    u64 carry = (p0 >> 32) + (u32)p1 + (u32)p2;
    return p3 + (p1 >> 32) + (p2 >> 32) + (carry >> 32);
}

static double get_f64(u32 r) { union { u64 u; double d; } v; v.u = f[r]; return v.d; }
static void set_f64(u32 r, double d) { union { u64 u; double d; } v; v.d = d; f[r] = v.u; }
static float get_f32(u32 r)
{
    union { u32 u; float f; } v;
    if ((f[r] >> 32) != 0xffffffffU) return 0.0f / 0.0f;
    v.u = f[r]; return v.f;
}
static void set_f32(u32 r, float n)
{
    union { u32 u; float f; } v;
    v.f = n; f[r] = 0xffffffff00000000ULL | v.u;
}

static i32 sext32(u32 v, u32 bits) { return (i32)(v << (32 - bits)) >> (32 - bits); }

static int execute_system(u32 insn, u32 rd, u32 f3, u32 rs1, u64 next)
{
    u32 imm = insn >> 20;
    if (!f3) {
        if (imm == 0) { trap(privilege ? 8 : 9, 0, 0); return 1; }
        if (imm == 1) { trap(3, pc, 0); return 1; }
        if (imm == 0x102) {
            u64 s = csr[SSTATUS];
            privilege = (s & STATUS_SPP) ? 0 : 1;
            if (s & STATUS_SPIE) s |= STATUS_SIE; else s &= ~STATUS_SIE;
            csr[SSTATUS] = (s | STATUS_SPIE) & ~STATUS_SPP;
            pc = csr[SEPC]; flush_tlb(); return 1;
        }
        if (imm == 0x105) { halted = 1; pc = next; return 1; }
        if ((insn >> 25) == 9) { flush_tlb(); pc = next; return 1; }
        return 0;
    } else {
        u32 n = imm & 4095, kind = f3 & 3;
        u64 old = n == TIME ? ticks : csr[n];
        u64 src = f3 >= 5 ? rs1 : x[rs1], value;
        if (kind == 1) value = src;
        else if (kind == 2) value = old | src;
        else if (kind == 3) value = old & ~src;
        else return 0;
        if (kind == 1 || rs1) set_csr(n, value);
        if (rd) x[rd] = old;
        pc = next;
        return 1;
    }
}

static u64 execute(u32 insn, u64 at)
{
    u32 op = insn & 127, rd = insn >> 7 & 31, f3 = insn >> 12 & 7;
    u32 rs1 = insn >> 15 & 31;
    u64 v = 0, next = at + 4, addr;
    i32 imm;
    int ok = 1;

    if (op == 0x03) {
        addr = x[rs1] + ((i32)insn >> 20);
        if (f3 == 0) ok = load(addr, 1, 1, &v), v = (i64)(signed char)v;
        else if (f3 == 1) ok = load(addr, 2, 1, &v), v = (i64)(short)v;
        else if (f3 == 2) ok = load(addr, 4, 1, &v), v = (i64)(i32)v;
        else if (f3 <= 6) ok = load(addr, 1U << (f3 & 3), 1, &v);
        else ok = 0;
        if (!ok) { pc = at; trap(fault == 2 ? 5 : 13, fault_addr, 0); return pc; }
        goto write;
    }
    if (op == 0x13 && f3 == 0) { if (rd) x[rd] = x[rs1] +
        ((i32)insn >> 20); x[0] = 0; return next; }
    if (op == 0x23) {
        u32 rs2 = insn >> 20 & 31;
        imm = sext32((insn >> 7 & 31) | (insn >> 25 << 5), 12);
        if (f3 <= 3) ok = store(x[rs1] + imm, 1U << f3, x[rs2]);
        else ok = 0;
        if (!ok) { pc = at; trap(fault == 2 ? 7 : 15, fault_addr, 0); return pc; }
        x[0] = 0; return next; }
    if (op == 0x6f) {
        imm = ((insn >> 21 & 1023) << 1) | ((insn >> 20 & 1) << 11) |
            ((insn >> 12 & 255) << 12) | ((i32)insn >> 31 << 20);
        v = next; next = at + imm; goto write; }
    if (op == 0x63) {
        u64 a = x[rs1], b = x[insn >> 20 & 31];
        imm = ((insn >> 8 & 15) << 1) | ((insn >> 25 & 63) << 5) |
            ((insn >> 7 & 1) << 11) | ((i32)insn >> 31 << 12);
        if ((f3 == 0 && a == b) || (f3 == 1 && a != b) ||
            (f3 == 4 && (i64)a < (i64)b) || (f3 == 5 && (i64)a >= (i64)b) ||
            (f3 == 6 && a < b) || (f3 == 7 && a >= b)) next = at + imm;
        else if (f3 == 2 || f3 == 3) { pc = at; trap(2, insn, 0); return pc; }
        x[0] = 0; return next; }
    u32 rs2 = insn >> 20 & 31, f7 = insn >> 25;
    u64 a = x[rs1], b = x[rs2];
    switch (op) {
    case 0x37: v = (i64)(i32)(insn & 0xfffff000); goto write;
    case 0x17: v = at + (i64)(i32)(insn & 0xfffff000); goto write;
    case 0x67: v = next; next = (a + ((i32)insn >> 20)) & ~1ULL; goto write;
    case 0x13:
        imm = (i32)insn >> 20;
        if (f3 == 1) v = a << (insn >> 20 & 63);
        else if (f3 == 2) v = (i64)a < imm;
        else if (f3 == 3) v = a < (u64)(i64)imm;
        else if (f3 == 4) v = a ^ (i64)imm;
        else if (f3 == 5) v = f7 & 32 ? (u64)((i64)a >> (insn >> 20 & 63)) : a >> (insn >> 20 & 63);
        else if (f3 == 6) v = a | (i64)imm;
        else if (f3 == 7) v = a & (i64)imm;
        else ok = 0;
        goto write;
    case 0x1b:
        imm = (i32)insn >> 20;
        if (f3 == 0) v = (i64)(i32)((u32)a + imm);
        else if (f3 == 1) v = (i64)(i32)((u32)a << (insn >> 20 & 31));
        else if (f3 == 5) v = (i64)(i32)(f7 & 32 ? (i32)a >> (insn >> 20 & 31) : (u32)a >> (insn >> 20 & 31));
        else ok = 0;
        goto write;
    case 0x33:
        if (f7 == 1) {
            if (f3 == 0) v = a * b;
            else if (f3 == 1) v = mulhu(a, b) - ((i64)a < 0 ? b : 0) - ((i64)b < 0 ? a : 0);
            else if (f3 == 2) v = mulhu(a, b) - ((i64)a < 0 ? b : 0);
            else if (f3 == 3) v = mulhu(a, b);
            else if (f3 == 4) v = !b ? ~0ULL : (a == 1ULL << 63 && b == ~0ULL ? a : (i64)a / (i64)b);
            else if (f3 == 5) v = !b ? ~0ULL : a / b;
            else if (f3 == 6) v = !b ? a : (a == 1ULL << 63 && b == ~0ULL ? 0 : (i64)a % (i64)b);
            else if (f3 == 7) v = !b ? a : a % b;
        } else if (f3 == 0) v = f7 & 32 ? a - b : a + b;
        else if (f3 == 1) v = a << (b & 63);
        else if (f3 == 2) v = (i64)a < (i64)b;
        else if (f3 == 3) v = a < b;
        else if (f3 == 4) v = a ^ b;
        else if (f3 == 5) v = f7 & 32 ? (u64)((i64)a >> (b & 63)) : a >> (b & 63);
        else if (f3 == 6) v = a | b;
        else if (f3 == 7) v = a & b;
        goto write;
    case 0x3b: {
        i32 aa = a, bb = b, w = 0; u32 sh = bb & 31;
        if (f7 == 1) {
            if (f3 == 0) w = (u32)aa * (u32)bb;
            else if (f3 == 4) w = !bb ? -1 : (aa == (-2147483647 - 1) && bb == -1 ? aa : aa / bb);
            else if (f3 == 5) w = !bb ? -1 : (u32)aa / (u32)bb;
            else if (f3 == 6) w = !bb ? aa : (aa == (-2147483647 - 1) && bb == -1 ? 0 : aa % bb);
            else if (f3 == 7) w = !bb ? aa : (u32)aa % (u32)bb;
            else ok = 0;
        } else if (f3 == 0) w = f7 & 32 ? aa - bb : aa + bb;
        else if (f3 == 1) w = (u32)aa << sh;
        else if (f3 == 5) w = f7 & 32 ? aa >> sh : (u32)aa >> sh;
        else ok = 0;
        v = (i64)w; goto write;
    }
    case 0x2f: {
        u32 fn = f7 >> 2, size = f3 == 2 ? 4 : f3 == 3 ? 8 : 0;
        u64 old, value;
        if (!size || (fn == 2 && rs2)) { ok = 0; break; }
        ok = load(a, size, 1, &old);
        if (!ok) { pc = at; trap(fault == 2 ? 5 : 13, fault_addr, 0); return pc; }
        v = size == 4 ? (i64)(i32)old : old;
        if (fn == 2) goto write;                 /* lr */
        if (fn == 3) {                            /* sc: reservation always holds */
            ok = store(a, size, b);
            if (!ok) { pc = at; trap(fault == 2 ? 7 : 15, fault_addr, 0); return pc; }
            v = 0; goto write;
        }
        if (size == 4) old = (u32)old, b = (u32)b;
        if (fn == 0) value = old + b;
        else if (fn == 1) value = b;
        else if (fn == 4) value = old ^ b;
        else if (fn == 8) value = old | b;
        else if (fn == 12) value = old & b;
        else if (fn == 16) value = size == 4 ? ((i32)old < (i32)b ? old : b) : ((i64)old < (i64)b ? old : b);
        else if (fn == 20) value = size == 4 ? ((i32)old > (i32)b ? old : b) : ((i64)old > (i64)b ? old : b);
        else if (fn == 24) value = old < b ? old : b;
        else if (fn == 28) value = old > b ? old : b;
        else { ok = 0; break; }
        ok = store(a, size, value);
        if (!ok) { pc = at; trap(fault == 2 ? 7 : 15, fault_addr, 0); return pc; }
        goto write;
    }
    case 0x0f: break;
    case 0x07:
        addr = a + ((i32)insn >> 20);
        if (f3 == 2) { ok = load(addr, 4, 1, &v); f[rd] = 0xffffffff00000000ULL | (u32)v; }
        else if (f3 == 3) ok = load(addr, 8, 1, &f[rd]);
        else ok = 0;
        if (!ok) { pc = at; trap(fault == 2 ? 5 : 13, fault_addr, 0); return pc; }
        break;
    case 0x27:
        imm = sext32((insn >> 7 & 31) | (insn >> 25 << 5), 12);
        addr = a + imm;
        if (f3 == 2) ok = store(addr, 4, f[rs2]);
        else if (f3 == 3) ok = store(addr, 8, f[rs2]);
        else ok = 0;
        if (!ok) { pc = at; trap(fault == 2 ? 7 : 15, fault_addr, 0); return pc; }
        break;
    case 0x53:
        if (f7 == 0x00) set_f32(rd, get_f32(rs1) + get_f32(rs2));
        else if (f7 == 0x01) set_f64(rd, get_f64(rs1) + get_f64(rs2));
        else if (f7 == 0x04) set_f32(rd, get_f32(rs1) - get_f32(rs2));
        else if (f7 == 0x05) set_f64(rd, get_f64(rs1) - get_f64(rs2));
        else if (f7 == 0x08) set_f32(rd, get_f32(rs1) * get_f32(rs2));
        else if (f7 == 0x09) set_f64(rd, get_f64(rs1) * get_f64(rs2));
        else if (f7 == 0x0c) set_f32(rd, get_f32(rs1) / get_f32(rs2));
        else if (f7 == 0x0d) set_f64(rd, get_f64(rs1) / get_f64(rs2));
        else if (f7 == 0x10 && f3 == 0)
            f[rd] = 0xffffffff00000000ULL | ((u32)f[rs1] & 0x7fffffffU) | ((u32)f[rs2] & 0x80000000U);
        else if (f7 == 0x11 && f3 == 0)
            f[rd] = (f[rs1] & 0x7fffffffffffffffULL) | (f[rs2] & 0x8000000000000000ULL);
        else if (f7 == 0x20 && rs2 == 1) set_f32(rd, get_f64(rs1));
        else if (f7 == 0x21 && rs2 == 0) set_f64(rd, get_f32(rs1));
        else if ((f7 == 0x50 || f7 == 0x51) && f3 <= 2) {
            double aa = f7 & 1 ? get_f64(rs1) : get_f32(rs1);
            double bb = f7 & 1 ? get_f64(rs2) : get_f32(rs2);
            v = f3 == 2 ? aa == bb : (f3 == 1 ? aa < bb : aa <= bb);
            goto write;
        } else if (f7 == 0x60 || f7 == 0x61) {
            double n = f7 & 1 ? get_f64(rs1) : get_f32(rs1);
            if (rs2 == 0) v = (i64)(i32)n;
            else if (rs2 == 1) v = (u32)n;
            else if (rs2 == 2) v = (i64)n;
            else if (rs2 == 3) v = double_to_u64(n);
            else ok = 0;
            goto write;
        } else if (f7 == 0x68 || f7 == 0x69) {
            double n;
            if (rs2 == 0) n = (i32)x[rs1];
            else if (rs2 == 1) n = (u32)x[rs1];
            else if (rs2 == 2) n = (i64)x[rs1];
            else if (rs2 == 3) n = u64_to_double(x[rs1]);
            else { ok = 0; break; }
            if (f7 & 1) set_f64(rd, n); else set_f32(rd, n);
        } else if (f7 == 0x70 && rs2 == 0 && f3 == 0) { v = (i64)(i32)f[rs1]; goto write; }
        else if (f7 == 0x71 && rs2 == 0 && f3 == 0) { v = f[rs1]; goto write; }
        else if (f7 == 0x78 && rs2 == 0 && f3 == 0) f[rd] = 0xffffffff00000000ULL | (u32)x[rs1];
        else if (f7 == 0x79 && rs2 == 0 && f3 == 0) f[rd] = x[rs1];
        else ok = 0;
        break;
    case 0x73:
        pc = at;
        if (!execute_system(insn, rd, f3, rs1, next)) ok = 0;
        return pc;
    default: ok = 0;
    }
    if (!ok) { pc = at; trap(2, insn, 0); return pc; }
    x[0] = 0; return next;
write:
    if (!ok) { pc = at; trap(2, insn, 0); return pc; }
    if (rd) x[rd] = v;
    x[0] = 0; return next;
}

void rv_init(u64 entry)
{
    u32 i;
    for (i = 0; i < 32; ++i) x[i] = f[i] = 0;
    for (i = 0; i < 4096; ++i) csr[i] = 0;
    pc = entry; ticks = timecmp = 0; privilege = halted = fault = timer_steps = 0;
    in_read = in_write = out_read = out_write = 0;
    fetch_url = fetch_destination = 0;
    fetch_url_length = fetch_capacity = fetch_length = fetch_status = 0;
    flush_tlb();
}

u32 rv_run(u32 count, u32 time_advance)
{
    u32 i, until, stopped;
    u64 now = ticks, here = pc;
    tick_step = time_advance;
    if (timecmp) set_csr(STIMECMP, timecmp);
    until = timer_steps; stopped = halted;
    for (i = 0; i < count; ++i) {
        u64 raw, tag;
        u32 context, slot;
        now += time_advance;
        if (until && !--until) {
            csr[SIP] |= IP_STIP;
            if ((privilege || (csr[SSTATUS] & STATUS_SIE)) &&
                (csr[SIP] & csr[SIE] & IP_STIP)) {
                ticks = now; timer_steps = until; pc = here;
                trap(5, 0, 1); here = pc; stopped = 0; continue;
            } else until = 1;
        }
        if (stopped) continue;
        if ((here ^ fetch_vpage) >> 12) {
            if (!load(here, 4, 0, &raw)) { pc = here;
                trap(fault == 2 ? 1 : 12, fault_addr, 0); here = pc; continue; }
            context = privilege | ((csr[SSTATUS] & STATUS_SUM) ? 2 : 0);
            slot = (u32)(here >> 12) & 255;
            tag = (((here >> 12) << 2) | context) + 1;
            if (tlb_tag[slot] == tag && tlb_ppage[slot] - RAM_BASE < RAM_SIZE) {
                fetch_vpage = here & ~4095ULL;
                fetch_delta = tlb_ppage[slot] - fetch_vpage;
            }
        } else raw = *(u32 *)(ram + (u32)(here + fetch_delta - RAM_BASE));
        if (((u32)raw & 127) == 0x73) {
            ticks = now; timer_steps = until;
            here = execute((u32)raw, here);
            until = timer_steps; stopped = halted;
        } else here = execute((u32)raw, here);
    }
    ticks = now; timer_steps = until; halted = stopped; pc = here;
    return fault;
}

void rv_input(u32 byte)
{
    if (in_write - in_read < UART_CAP) uart_in[in_write++ & (UART_CAP - 1)] = byte;
}

u32 rv_output_count(void) { return out_write - out_read; }
u32 rv_output(void) { return out_read == out_write ? 0xffffffffU : uart_out[out_read++ & (UART_CAP - 1)]; }
u32 rv_fetch_status(void) { return fetch_status; }
u32 rv_fetch_url(void) { return (u32)fetch_url; }
u32 rv_fetch_url_length(void) { return fetch_url_length; }
u32 rv_fetch_destination(void) { return (u32)fetch_destination; }
u32 rv_fetch_capacity(void) { return fetch_capacity; }
void rv_fetch_complete(u32 length, u32 status) { fetch_length = length; fetch_status = status; }
