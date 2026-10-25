/*
 * 6502 interpreter for the NES Ricoh 2A03.
 *
 * Official opcodes only. Decimal arithmetic is disabled on the 2A03.
 * cpu_step() returns consumed CPU cycles, or -1 for an unsupported opcode.
 */
#include "nes.h"

#include <string.h>
#include <stdint.h>

#define NES_MAX_GAME_GENIE 32

typedef struct {
    uint16_t address;
    uint8_t value;
    uint8_t compare;
    uint8_t has_compare;
} game_genie_t;

static game_genie_t game_genie[NES_MAX_GAME_GENIE];
static int game_genie_count;

static const char game_genie_alphabet[] = "APZLGITYEOXUKSVN";

uint8_t nes_ram[0x800];
const uint8_t *nes_prg;
uint16_t nes_prg_mask = 0x7FFF;
int nes_nmi_pending;

static uint8_t A, X, Y, S, P;
static uint16_t PC;
static int stall;

#define F_C 0x01
#define F_Z 0x02
#define F_I 0x04
#define F_D 0x08
#define F_B 0x10
#define F_U 0x20
#define F_V 0x40
#define F_N 0x80

/* Official opcode base cycle counts. */
static const uint8_t cyc_tab[256] = {
    /* 0x00 */
    7,6,0,0,3,3,5,0,3,2,2,0,4,4,6,0,
    /* 0x10 */
    2,5,0,0,4,4,6,0,2,4,2,0,4,4,7,0,
    /* 0x20 */
    6,6,0,0,3,3,5,0,4,2,2,0,4,4,6,0,
    /* 0x30 */
    2,5,0,0,4,4,6,0,2,4,2,0,4,4,7,0,
    /* 0x40 */
    6,6,0,0,3,3,5,0,3,2,2,0,3,4,6,0,
    /* 0x50 */
    2,5,0,0,4,4,6,0,2,4,2,0,4,4,7,0,
    /* 0x60 */
    6,6,0,0,3,3,5,0,4,2,2,0,5,4,6,0,
    /* 0x70 */
    2,5,0,0,4,4,6,0,2,4,2,0,4,4,7,0,
    /* 0x80 */
    0,6,0,0,3,3,3,0,2,0,2,0,4,4,4,0,
    /* 0x90 */
    2,6,0,0,4,4,4,0,2,5,2,0,5,5,5,0,
    /* 0xA0 */
    2,6,2,0,3,3,3,0,2,2,2,0,4,4,4,0,
    /* 0xB0 */
    2,5,2,0,4,4,4,0,2,4,2,0,4,4,4,0,
    /* 0xC0 */
    2,6,0,0,3,3,5,0,2,2,2,0,4,4,6,0,
    /* 0xD0 */
    2,5,0,0,4,4,6,0,2,4,3,0,4,4,7,0,
    /* 0xE0 */
    2,6,0,0,3,3,5,0,2,2,2,0,4,4,6,0,
    /* 0xF0 */
    2,5,0,0,4,4,6,0,2,4,2,0,4,4,7,0
};

/* -------------------------------------------------------------------------
 * Game Genie
 * ------------------------------------------------------------------------- */

void nes_game_genie_clear(void)
{
    memset(game_genie, 0, sizeof game_genie);
    game_genie_count = 0;
}

int nes_game_genie_count(void)
{
    return game_genie_count;
}

static int game_genie_nibble(char ch)
{
    const char *p;

    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 'A');

    p = strchr(game_genie_alphabet, ch);

    return p ? (int)(p - game_genie_alphabet) : -1;
}

int nes_game_genie_add(const char *code)
{
    uint8_t n[8];
    size_t len;
    size_t i;
    uint16_t address;
    uint8_t value;
    uint8_t compare = 0;
    int has_compare;

    if (code == NULL)
        return -1;

    len = strlen(code);

    if (len != 6 && len != 8)
        return -1;

    if (game_genie_count >= NES_MAX_GAME_GENIE)
        return -1;

    for (i = 0; i < len; ++i) {
        int v = game_genie_nibble(code[i]);

        if (v < 0)
            return -1;

        n[i] = (uint8_t)v;
    }

    /*
     * Decode the ROM address from the Game Genie nibbles.
     */
    address = (uint16_t)(
        0x8000 |
        ((n[3] & 7) << 12) |
        ((n[5] & 7) << 8)  |
        ((n[4] & 8) << 8)  |
        ((n[2] & 7) << 4)  |
        ((n[1] & 8) << 4)  |
        (n[4] & 7)         |
        (n[3] & 8)
    );

    if (len == 6) {
        value = (uint8_t)(
            ((n[1] & 7) << 4) |
            ((n[0] & 8) << 4) |
            (n[0] & 7)        |
            (n[5] & 8)
        );

        has_compare = 0;
    } else {
        value = (uint8_t)(
            ((n[1] & 7) << 4) |
            ((n[0] & 8) << 4) |
            (n[0] & 7)        |
            (n[7] & 8)
        );

        compare = (uint8_t)(
            ((n[7] & 7) << 4) |
            ((n[6] & 8) << 4) |
            (n[6] & 7)        |
            (n[5] & 8)
        );

        /* A zero compare byte disables comparison. */
        has_compare = (compare != 0);
    }

    game_genie[game_genie_count].address = address;
    game_genie[game_genie_count].value = value;
    game_genie[game_genie_count].compare = compare;
    game_genie[game_genie_count].has_compare =
        (uint8_t)has_compare;

    game_genie_count++;

    return 0;
}

/* -------------------------------------------------------------------------
 * CPU memory bus
 * ------------------------------------------------------------------------- */

static inline uint8_t rd(uint16_t a)
{
    if (a < 0x2000)
        return nes_ram[a & 0x07FF];

    if (a >= 0x8000) {
        uint8_t original;
        int i;

        if (nes_prg == NULL)
            return 0;

        original = nes_prg[a & nes_prg_mask];

        for (i = 0; i < game_genie_count; ++i) {
            const game_genie_t *g = &game_genie[i];

            if (g->address != a)
                continue;

            if (g->has_compare && original != g->compare)
                continue;

            return g->value;
        }

        return original;
    }

    return nes_io_read(a);
}

static inline void wr(uint16_t a, uint8_t v)
{
    if (a < 0x2000)
        nes_ram[a & 0x07FF] = v;
    else
        nes_io_write(a, v);
}

static inline uint16_t rd16(uint16_t a)
{
    return (uint16_t)(
        rd(a) |
        ((uint16_t)rd((uint16_t)(a + 1)) << 8)
    );
}

static inline uint16_t rd16zp(uint8_t a)
{
    return (uint16_t)(
        rd(a) |
        ((uint16_t)rd((uint8_t)(a + 1)) << 8)
    );
}

static inline void push(uint8_t v)
{
    nes_ram[0x100 + S--] = v;
}

static inline uint8_t pull(void)
{
    return nes_ram[0x100 + ++S];
}

uint8_t cpu_peek(uint16_t a)
{
    return rd(a);
}

void cpu_stall(int c)
{
    if (c > 0)
        stall += c;
}

/* -------------------------------------------------------------------------
 * CPU helpers
 * ------------------------------------------------------------------------- */

static inline void nz(uint8_t v)
{
    P = (uint8_t)(
        (P & ~(F_N | F_Z)) |
        (v & F_N) |
        (v ? 0 : F_Z)
    );
}

static inline void do_adc(uint8_t v)
{
    unsigned sum = (unsigned)A + v + (P & F_C);
    uint8_t r = (uint8_t)sum;

    P = (uint8_t)(
        (P & ~(F_C | F_V)) |
        (sum > 0xFF ? F_C : 0) |
        ((~(A ^ v) & (A ^ r) & 0x80) ? F_V : 0)
    );

    A = r;
    nz(A);
}

static inline void do_cmp(uint8_t r, uint8_t v)
{
    P = (uint8_t)((P & ~F_C) | (r >= v ? F_C : 0));
    nz((uint8_t)(r - v));
}

static inline uint8_t do_asl(uint8_t v)
{
    P = (uint8_t)((P & ~F_C) | (v >> 7));
    v = (uint8_t)(v << 1);
    nz(v);
    return v;
}

static inline uint8_t do_lsr(uint8_t v)
{
    P = (uint8_t)((P & ~F_C) | (v & 1));
    v >>= 1;
    nz(v);
    return v;
}

static inline uint8_t do_rol(uint8_t v)
{
    uint8_t c = P & F_C;

    P = (uint8_t)((P & ~F_C) | (v >> 7));
    v = (uint8_t)((v << 1) | c);
    nz(v);

    return v;
}

static inline uint8_t do_ror(uint8_t v)
{
    uint8_t c = P & F_C;

    P = (uint8_t)((P & ~F_C) | (v & 1));
    v = (uint8_t)((v >> 1) | (c << 7));
    nz(v);

    return v;
}

static inline uint8_t do_inc(uint8_t v)
{
    v = (uint8_t)(v + 1);
    nz(v);
    return v;
}

static inline uint8_t do_dec(uint8_t v)
{
    v = (uint8_t)(v - 1);
    nz(v);
    return v;
}

/*
 * Clears RAM and resets the CPU.
 * nes_reset() remains the full-system reset entry point.
 */
void nes_power_on(void)
{
    memset(nes_ram, 0, sizeof nes_ram);
    cpu_reset();
}

void cpu_reset(void)
{
    A = X = Y = 0;
    S = 0xFD;
    P = F_I | F_U;
    PC = rd16(0xFFFC);
    nes_nmi_pending = 0;
    stall = 0;
}

/* -------------------------------------------------------------------------
 * Addressing modes
 * ------------------------------------------------------------------------- */

#define M_IMM \
    do { addr = PC++; } while (0)

#define M_ZP \
    do { addr = rd(PC++); } while (0)

#define M_ZPX \
    do { addr = (uint8_t)(rd(PC++) + X); } while (0)

#define M_ZPY \
    do { addr = (uint8_t)(rd(PC++) + Y); } while (0)

#define M_ABS \
    do { addr = rd16(PC); PC += 2; } while (0)

#define M_ABXR \
    do { \
        uint16_t b_ = rd16(PC); \
        PC += 2; \
        addr = (uint16_t)(b_ + X); \
        if ((b_ ^ addr) & 0xFF00) cyc++; \
    } while (0)

#define M_ABYR \
    do { \
        uint16_t b_ = rd16(PC); \
        PC += 2; \
        addr = (uint16_t)(b_ + Y); \
        if ((b_ ^ addr) & 0xFF00) cyc++; \
    } while (0)

#define M_ABXW \
    do { addr = (uint16_t)(rd16(PC) + X); PC += 2; } while (0)

#define M_ABYW \
    do { addr = (uint16_t)(rd16(PC) + Y); PC += 2; } while (0)

#define M_IZX \
    do { addr = rd16zp((uint8_t)(rd(PC++) + X)); } while (0)

#define M_IZYR \
    do { \
        uint16_t b_ = rd16zp(rd(PC++)); \
        addr = (uint16_t)(b_ + Y); \
        if ((b_ ^ addr) & 0xFF00) cyc++; \
    } while (0)

#define M_IZYW \
    do { addr = (uint16_t)(rd16zp(rd(PC++)) + Y); } while (0)

#define BRANCH(cond) do { \
    int8_t o_ = (int8_t)rd(PC++); \
    if (cond) { \
        uint16_t np_ = (uint16_t)(PC + o_); \
        cyc++; \
        if ((np_ ^ PC) & 0xFF00) cyc++; \
        PC = np_; \
    } \
} while (0)

#define ALU_CASES(base, STMT) \
    case (base) + 0x00: M_IZX; v = rd(addr); STMT; break; \
    case (base) + 0x04: M_ZP; v = rd(addr); STMT; break; \
    case (base) + 0x08: M_IMM; v = rd(addr); STMT; break; \
    case (base) + 0x0C: M_ABS; v = rd(addr); STMT; break; \
    case (base) + 0x10: M_IZYR; v = rd(addr); STMT; break; \
    case (base) + 0x14: M_ZPX; v = rd(addr); STMT; break; \
    case (base) + 0x18: M_ABYR; v = rd(addr); STMT; break; \
    case (base) + 0x1C: M_ABXR; v = rd(addr); STMT; break;

#define RMW_CASES(base, FN) \
    case (base) + 0x00: M_ZP; v = rd(addr); wr(addr, v); wr(addr, FN(v)); break; \
    case (base) + 0x08: M_ABS; v = rd(addr); wr(addr, v); wr(addr, FN(v)); break; \
    case (base) + 0x10: M_ZPX; v = rd(addr); wr(addr, v); wr(addr, FN(v)); break; \
    case (base) + 0x18: M_ABXW; v = rd(addr); wr(addr, v); wr(addr, FN(v)); break;

/* -------------------------------------------------------------------------
 * Execute one CPU instruction
 * ------------------------------------------------------------------------- */

int cpu_step(void)
{
    int cyc;
    uint8_t op, v;
    uint16_t addr;

    /* NMI is edge-latched by the PPU. */
    if (nes_nmi_pending) {
        nes_nmi_pending = 0;

        push((uint8_t)(PC >> 8));
        push((uint8_t)PC);
        push((uint8_t)((P & ~F_B) | F_U));

        P |= F_I;
        PC = rd16(0xFFFA);

        cyc = 7 + stall;
        stall = 0;

        return cyc;
    }

    op = rd(PC++);
    cyc = cyc_tab[op];

    if (cyc == 0) {
        PC--;
        return -1;
    }

    switch (op) {
    ALU_CASES(0x01, { A |= v; nz(A); })       /* ORA */
    ALU_CASES(0x21, { A &= v; nz(A); })       /* AND */
    ALU_CASES(0x41, { A ^= v; nz(A); })       /* EOR */
    ALU_CASES(0x61, { do_adc(v); })           /* ADC */
    ALU_CASES(0xA1, { A = v; nz(A); })        /* LDA */
    ALU_CASES(0xC1, { do_cmp(A, v); })        /* CMP */
    ALU_CASES(0xE1, { do_adc((uint8_t)~v); }) /* SBC */

    /* STA */
    case 0x81: M_IZX; wr(addr, A); break;
    case 0x85: M_ZP; wr(addr, A); break;
    case 0x8D: M_ABS; wr(addr, A); break;
    case 0x91: M_IZYW; wr(addr, A); break;
    case 0x95: M_ZPX; wr(addr, A); break;
    case 0x99: M_ABYW; wr(addr, A); break;
    case 0x9D: M_ABXW; wr(addr, A); break;

    /* STX */
    case 0x86: M_ZP; wr(addr, X); break;
    case 0x8E: M_ABS; wr(addr, X); break;
    case 0x96: M_ZPY; wr(addr, X); break;

    /* STY */
    case 0x84: M_ZP; wr(addr, Y); break;
    case 0x8C: M_ABS; wr(addr, Y); break;
    case 0x94: M_ZPX; wr(addr, Y); break;

    /* LDX */
    case 0xA2: M_IMM; X = rd(addr); nz(X); break;
    case 0xA6: M_ZP; X = rd(addr); nz(X); break;
    case 0xAE: M_ABS; X = rd(addr); nz(X); break;
    case 0xB6: M_ZPY; X = rd(addr); nz(X); break;
    case 0xBE: M_ABYR; X = rd(addr); nz(X); break;

    /* LDY */
    case 0xA0: M_IMM; Y = rd(addr); nz(Y); break;
    case 0xA4: M_ZP; Y = rd(addr); nz(Y); break;
    case 0xAC: M_ABS; Y = rd(addr); nz(Y); break;
    case 0xB4: M_ZPX; Y = rd(addr); nz(Y); break;
    case 0xBC: M_ABXR; Y = rd(addr); nz(Y); break;

    /* CPX */
    case 0xE0: M_IMM; do_cmp(X, rd(addr)); break;
    case 0xE4: M_ZP; do_cmp(X, rd(addr)); break;
    case 0xEC: M_ABS; do_cmp(X, rd(addr)); break;

    /* CPY */
    case 0xC0: M_IMM; do_cmp(Y, rd(addr)); break;
    case 0xC4: M_ZP; do_cmp(Y, rd(addr)); break;
    case 0xCC: M_ABS; do_cmp(Y, rd(addr)); break;

    /* BIT */
    case 0x24:
        M_ZP;
        v = rd(addr);
        goto do_bit;

    case 0x2C:
        M_ABS;
        v = rd(addr);

    do_bit:
        P = (uint8_t)(
            (P & ~(F_N | F_V | F_Z)) |
            (v & (F_N | F_V)) |
            ((A & v) ? 0 : F_Z)
        );
        break;

    /* Read-modify-write instructions. */
    RMW_CASES(0x06, do_asl)
    RMW_CASES(0x26, do_rol)
    RMW_CASES(0x46, do_lsr)
    RMW_CASES(0x66, do_ror)
    RMW_CASES(0xC6, do_dec)
    RMW_CASES(0xE6, do_inc)

    /* Accumulator shifts and rotates. */
    case 0x0A: A = do_asl(A); break;
    case 0x2A: A = do_rol(A); break;
    case 0x4A: A = do_lsr(A); break;
    case 0x6A: A = do_ror(A); break;

    /* Register transfers. */
    case 0xAA: X = A; nz(X); break;
    case 0xA8: Y = A; nz(Y); break;
    case 0x8A: A = X; nz(A); break;
    case 0x98: A = Y; nz(A); break;
    case 0xBA: X = S; nz(X); break;
    case 0x9A: S = X; break;

    /* Register increments and decrements. */
    case 0xE8: X++; nz(X); break;
    case 0xCA: X--; nz(X); break;
    case 0xC8: Y++; nz(Y); break;
    case 0x88: Y--; nz(Y); break;

    /* Stack instructions. */
    case 0x48: push(A); break;
    case 0x08: push((uint8_t)(P | F_B | F_U)); break;
    case 0x68: A = pull(); nz(A); break;
    case 0x28: P = (uint8_t)((pull() & ~F_B) | F_U); break;

    /* Flag instructions. */
    case 0x18: P &= (uint8_t)~F_C; break;
    case 0x38: P |= F_C; break;
    case 0x58: P &= (uint8_t)~F_I; break;
    case 0x78: P |= F_I; break;
    case 0xB8: P &= (uint8_t)~F_V; break;
    case 0xD8: P &= (uint8_t)~F_D; break;
    case 0xF8: P |= F_D; break;

    /* Conditional branches. */
    case 0x10: BRANCH(!(P & F_N)); break;
    case 0x30: BRANCH( (P & F_N)); break;
    case 0x50: BRANCH(!(P & F_V)); break;
    case 0x70: BRANCH( (P & F_V)); break;
    case 0x90: BRANCH(!(P & F_C)); break;
    case 0xB0: BRANCH( (P & F_C)); break;
    case 0xD0: BRANCH(!(P & F_Z)); break;
    case 0xF0: BRANCH( (P & F_Z)); break;

    /* JMP absolute. */
    case 0x4C:
        PC = rd16(PC);
        break;

    /* JMP indirect, including the original 6502 page-wrap behaviour. */
    case 0x6C: {
        uint16_t p = rd16(PC);

        PC = (uint16_t)(
            rd(p) |
            ((uint16_t)rd(
                (uint16_t)((p & 0xFF00) |
                ((p + 1) & 0x00FF))
            ) << 8)
        );
        break;
    }

    /* JSR */
    case 0x20:
        addr = rd16(PC);
        PC++;

        push((uint8_t)(PC >> 8));
        push((uint8_t)PC);

        PC = addr;
        break;

    /* RTS */
    case 0x60: {
        uint8_t lo = pull();
        uint8_t hi = pull();

        PC = (uint16_t)(
            (((uint16_t)hi << 8) | lo) + 1
        );
        break;
    }

    /* RTI */
    case 0x40: {
        uint8_t lo;

        P = (uint8_t)((pull() & ~F_B) | F_U);
        lo = pull();

        PC = (uint16_t)(
            lo | ((uint16_t)pull() << 8)
        );
        break;
    }

    /* BRK */
    case 0x00:
        PC++; /* Skip the BRK padding byte. */

        push((uint8_t)(PC >> 8));
        push((uint8_t)PC);
        push((uint8_t)(P | F_B | F_U));

        P |= F_I;
        PC = rd16(0xFFFE);
        break;

    /* NOP */
    case 0xEA:
        break;

    default:
        PC--;
        return -1;
    }

    if (stall) {
        cyc += stall;
        stall = 0;
    }

    return cyc;
}
