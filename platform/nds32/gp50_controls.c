#include "gp50_controls.h"
#include "gp5_firmware.h"
#include "hal/gpio.h"

#define LOAD GP5_PIN(17)
#define DATA_OUT GP5_PIN(28)
#define DATA_IN GP5_PIN(4)
#define CLOCK GP5_PIN(5)
#define PARA_ENCODER (GP5_PIN(2) | GP5_PIN(3))

/* V1.0.5 cold USB-independent start with empty retained flags: both rings
 * white, stock power-control bits 3 and 4 set. Emulator only. */
#define COLD_OUTPUT 0x3f18u

static const uint8_t encoder_bits[] = { 4, 0, 2 };
static const uint8_t button_bits[] = { 6, 7, 12, 13, 15 };
static struct {
    unsigned ab;
    int detents;
} encoders[sizeof encoder_bits];
static uint32_t quiet[sizeof button_bits];
static uint16_t buttons;
uint16_t gp50_control_input;

/* V1.0.5 0x4851e. Shift the high output byte first and receive the low input
 * byte first. The duplicate writes retain the stock latch and clock timing. */
static uint16_t shift_controls(void)
{
    uint16_t in = 0;
    gp5_reg_bits_clear(GP5_GPIO_A_OUT, LOAD);
    gp5_reg_bits_clear(GP5_GPIO_A_OUT, LOAD);
    gp5_reg_bits_set(GP5_GPIO_A_OUT, LOAD);
    for (unsigned i = 0; i < 16; i++) {
        if (COLD_OUTPUT & (0x8000u >> i)) gp5_reg_bits_set(GP5_GPIO_A_OUT, DATA_OUT);
        else gp5_reg_bits_clear(GP5_GPIO_A_OUT, DATA_OUT);
        if (gp5_reg_get(GP5_GPIO_B_IN) & DATA_IN) in |= (0x80u >> (i & 7u)) << (i & 8u);
        gp5_reg_bits_set(GP5_GPIO_B_OUT, CLOCK);
        gp5_reg_bits_set(GP5_GPIO_B_OUT, CLOCK);
        gp5_reg_bits_clear(GP5_GPIO_B_OUT, CLOCK);
    }
    gp5_reg_bits_clear(GP5_GPIO_A_OUT, LOAD);
    return in;
}

void gp50_controls_init(void)
{
    gp5_reg_bits_clear(GP5_GPIO_A_ANA_EN, DATA_OUT);
    gp5_gpio_a_digital(15);
    gp5_gpio_b_digital(5);
    gp5_reg_bits_clear(0x400100F8u, 0x00007700u); /* B2/B3 function select */
    gp5_reg_bits_clear(GP5_GPIO_A_OUT, DATA_OUT);
    gp5_reg_bits_set(GP5_GPIO_A_OUT, LOAD);
    gp5_reg_bits_clear(GP5_GPIO_B_OUT, CLOCK);
    gp5_reg_bits_set(GP5_GPIO_A_IE, GP50_PARA_PIN);
    gp5_reg_bits_clear(GP5_GPIO_A_IE, LOAD | DATA_OUT);
    gp5_reg_bits_set(GP5_GPIO_A_OE, LOAD | DATA_OUT);
    gp5_reg_bits_clear(GP5_GPIO_A_OE, GP50_PARA_PIN);
    gp5_reg_bits_set(GP5_GPIO_A_PU, LOAD | DATA_OUT);
    gp5_reg_bits_clear(GP5_GPIO_A_PU, GP50_PARA_PIN);
    gp5_reg_bits_clear(GP5_GPIO_A_PD, LOAD | DATA_OUT | GP50_PARA_PIN);
    gp5_reg_bits_set(GP5_GPIO_B_IE, PARA_ENCODER | DATA_IN);
    gp5_reg_bits_clear(GP5_GPIO_B_IE, CLOCK);
    gp5_reg_bits_set(GP5_GPIO_B_OE, CLOCK);
    gp5_reg_bits_clear(GP5_GPIO_B_OE, PARA_ENCODER | DATA_IN);
    gp5_reg_bits_set(GP5_GPIO_B_PU, CLOCK | DATA_IN);
    gp5_reg_bits_clear(GP5_GPIO_B_PU, PARA_ENCODER);
    gp5_reg_bits_clear(GP5_GPIO_B_PD, CLOCK | DATA_IN | PARA_ENCODER);
    gp50_control_input = shift_controls();
    for (unsigned i = 0; i < sizeof encoder_bits; i++)
        encoders[i].ab = (gp50_control_input >> encoder_bits[i]) & 3u;
}

void gp50_controls_sample(void)
{
    gp50_control_input = shift_controls();
    for (unsigned i = 0; i < sizeof encoder_bits; i++) {
        unsigned ab = (gp50_control_input >> encoder_bits[i]) & 3u;
        encoders[i].detents += gp50_encoder_delta(encoders[i].ab, ab);
        encoders[i].ab = ab;
    }
}

void gp50_controls_dispatch(void)
{
    uint32_t now = gp5_ms();
    uint16_t pressed = (uint16_t)~gp50_control_input;
    for (unsigned i = 0; i < sizeof encoder_bits; i++) {
        if (encoders[i].detents) {
            fw_gp50_encoder_turn(i, encoders[i].detents);
            encoders[i].detents = 0;
        }
    }
    for (unsigned i = 0; i < sizeof button_bits; i++) {
        unsigned mask = 1u << button_bits[i];
        if (((buttons ^ pressed) & mask) && (int32_t)(now - quiet[i]) >= 0) {
            buttons ^= mask;
            quiet[i] = now + 30u;
            fw_gp50_button(i, (buttons & mask) != 0);
        }
    }
}
