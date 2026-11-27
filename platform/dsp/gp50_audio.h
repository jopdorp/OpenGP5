/* GP-50 pin mux and codec setup from the V1.0.5 DSP startup trace. */
#define GP50_GPIO 0x40011000u
#define CODEC_IN  REG(GP50_GPIO + 0x3Cu)
#define CODEC_OE  REG(GP50_GPIO + 0x44u)
#define CODEC_SET REG(GP50_GPIO + 0x48u)
#define CODEC_CLR REG(GP50_GPIO + 0x4Cu)
#define CODEC_SDA (1u << 8)
#define CODEC_SCL (1u << 9)

static void gp50_dsp_pins(void)
{
    static const struct { uint8_t reg; uint32_t mask, bits; } mux[] = {
        {0x18, 0x77777777u, 0x33333333u}, {0x1c, 0x77777777u, 0x22222222u},
        {0x20, 0x07000070u, 0x05000020u}, {0x24, 0x77777777u, 0x44446666u},
        {0x54, 0x00777777u, 0x00222222u}, {0x58, 0x77707777u, 0x22203300u},
    };
    REG(GP50_GPIO + 0x30u) = 0x55555555u;
    REG(GP50_GPIO + 0x34u) = 0x55555555u;
    REG(GP50_GPIO + 0x6cu) = 0x55555555u;
    for (unsigned i = 0; i < sizeof mux / sizeof mux[0]; ++i) {
        unsigned reg = GP50_GPIO + mux[i].reg;
        REG(reg) = (REG(reg) & ~mux[i].mask) | mux[i].bits;
    }
}

static void gp50_delay(unsigned ticks)
{
    __asm volatile("1: nop\n subs %0, #1\n bne 1b" : "+r"(ticks) :: "cc", "memory");
}

static void gp50_dsp_clock(void)
{
    /* V1.0.5 startup (0x58ec and 0x8eb6). Keep a PLL already set by the resident. */
    REG(0x4000E014u) = 1u;
    uint32_t select = REG(0x4000E020u);
    if (REG(0x4000E054u) != 0x03000F00u ||
        REG(0x4000E058u) != 0x0B400000u ||
        REG(0x4000E05Cu) != 0x03002222u) {
        select &= ~3u;
        REG(0x4000E020u) = select;
        REG(0x4000E054u) = 0x03000F01u;
        REG(0x4000E058u) = 0x0B400000u;
        REG(0x4000E05Cu) = 0x03002222u;
        gp50_delay(200);
        REG(0x4000E054u) = 0x03000F00u;
        gp50_delay(200);
    }
    REG(0x4000E020u) = (select & ~0x80400u) | 2u; /* stock 0x5bba clears the retained clock bits */
    REG(0x4000E014u) = 0;
    REG(0x4000E00Cu) = 0x0001FC3Fu;
}

static void __attribute__((noinline)) codec_delay(unsigned ticks)
{
    /* V1.0.5 bit-bang delay at 0x7968. */
    __asm volatile("b 2f\n1: nop\n2: subs %0, %0, #1\n uxth %0, %0\n bcs 1b"
                   : "+l"(ticks) :: "cc", "memory");
}

static int codec_byte(uint8_t value)
{
    for (unsigned bit = 0; bit < 8; ++bit) {
        codec_delay(120);
        if (value & 0x80u) CODEC_SET = CODEC_SDA;
        else CODEC_CLR = CODEC_SDA;
        value <<= 1;
        codec_delay(120);
        CODEC_SET = CODEC_SCL;
        codec_delay(480);
        CODEC_CLR = CODEC_SCL;
    }
    codec_delay(480);
    CODEC_OE &= ~CODEC_SDA;
    __asm volatile("dsb" ::: "memory");
    CODEC_SET = CODEC_SCL;
    codec_delay(480);
    int ack = !(CODEC_IN & CODEC_SDA);
    CODEC_CLR = CODEC_SCL;
    CODEC_OE |= CODEC_SDA;
    return ack;
}

static unsigned gp50_codec_nacks;

static int gp50_codecs_init(void)
{
    static const uint8_t writes[][3] = {
        {0x26, 0x00, 0x01}, {0x26, 0x06, 0x00}, {0x26, 0x05, 0x1b},
        {0x26, 0x01, 0x00}, {0x26, 0x07, 0xc0}, {0x26, 0x09, 0x00},
        {0x26, 0x0d, 0xa0}, {0x26, 0x08, 0x11}, {0x26, 0x05, 0x13},
        {0x10, 0x13, 0x10},
    };
    CODEC_OE |= CODEC_SCL;
    CODEC_OE |= CODEC_SDA;
    CODEC_SET = CODEC_SCL;
    codec_delay(480);
    CODEC_SET = CODEC_SDA;
    unsigned nacks = 0;
    for (unsigned i = 0; i < sizeof writes / sizeof writes[0]; ++i) {
        CODEC_SET = CODEC_SCL;
        codec_delay(120);
        CODEC_SET = CODEC_SDA;
        codec_delay(480);
        CODEC_CLR = CODEC_SDA;
        codec_delay(480);
        CODEC_CLR = CODEC_SCL;
        int ok = codec_byte(writes[i][0]) && codec_byte(writes[i][1]) && codec_byte(writes[i][2]);
        codec_delay(120);
        CODEC_CLR = CODEC_SDA;
        codec_delay(240);
        CODEC_SET = CODEC_SCL;
        codec_delay(480);
        CODEC_SET = CODEC_SDA;
        if (!ok) nacks |= 1u << i;
    }
    gp50_codec_nacks = nacks;
    /* Stock runs audio when the 0x26 device is absent and 0x10 responds. */
    return nacks != (1u << (sizeof writes / sizeof writes[0])) - 1u;
}
