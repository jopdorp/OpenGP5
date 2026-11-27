#include "gp50_controls.h"
#include "gp5_firmware.h"
#include "hal/adc.h"
#include "hal/gpio.h"
#include "hal/tick.h"

#define LOAD GP5_PIN(17)
#define DATA_OUT GP5_PIN(28)
#define DATA_IN GP5_PIN(4)
#define CLOCK GP5_PIN(5)
#define CHARGE_INPUT (1u << 10)
#define PARA_ENCODER (GP5_PIN(2) | GP5_PIN(3))

/* V1.0.5 cold start with empty retained flags. */
#define COLD_OUTPUT 0x3f18u
#define UPDATE_OUTPUT_BIT (1u << 1)
#define RUN_OUTPUT_BITS (UPDATE_OUTPUT_BIT | (1u << 7))

static const uint8_t encoder_bits[] = { 4, 0, 2 };
static const uint8_t button_bits[] = { 6, 7, 12, 13, 15 };
static struct {
    unsigned ab;
    int detents;
} encoders[sizeof encoder_bits];
static uint32_t quiet[sizeof button_bits];
static uint16_t buttons;
static uint16_t control_output = COLD_OUTPUT;
uint16_t gp50_control_input;
static gp50_status_t status = { -1, -1, GP50_BT_OFF };
static unsigned expression_raw, expression_filtered_q4, expression_last, expression_pending;
static unsigned expression_heel, expression_toe = GP50_EXPRESSION_DEFAULT_TOE, expression_reported;
static int expression_sampled, expression_remap;
extern void fw_gp50_expression(unsigned value) __attribute__((weak));

/* V1.0.5 0x4851e. Shift the high output byte first and receive the low input
 * byte first. The duplicate writes retain the stock latch and clock timing. */
static uint16_t shift_controls(uint16_t output)
{
    uint16_t in = 0;
    gp5_reg_bits_clear(GP5_GPIO_A_OUT, LOAD);
    gp5_reg_bits_clear(GP5_GPIO_A_OUT, LOAD);
    gp5_reg_bits_set(GP5_GPIO_A_OUT, LOAD);
    for (unsigned i = 0; i < 16; i++) {
        if (output & (0x8000u >> i)) gp5_reg_bits_set(GP5_GPIO_A_OUT, DATA_OUT);
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
    shift_controls(control_output);
    /* V1.0.5 enters Run at 0x27d7c before its first DSP version query. */
    gp5_delay_ms(50);
    control_output |= RUN_OUTPUT_BITS;
    gp50_control_input = shift_controls(control_output);
    for (unsigned i = 0; i < sizeof encoder_bits; i++)
        encoders[i].ab = (gp50_control_input >> encoder_bits[i]) & 3u;
}

void gp50_controls_sample(void)
{
    gp50_control_input = shift_controls(control_output);
    for (unsigned i = 0; i < sizeof encoder_bits; i++) {
        unsigned ab = (gp50_control_input >> encoder_bits[i]) & 3u;
        encoders[i].detents += gp50_encoder_delta(encoders[i].ab, ab);
        encoders[i].ab = ab;
    }
}

void gp50_controls_prepare_update(void)
{
    /* V1.0.5 clears and clocks out bit 1 twice, waiting 100 ticks after each. */
    control_output &= ~UPDATE_OUTPUT_BIT;
    for (unsigned i = 0; i < 2; i++) {
        shift_controls(control_output);
        gp5_delay_ms(100);
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

void gp50_analog_init(void)
{
    /* V1.0.5 0x47580: voltage ADC10/A29 and current ADC11/A30.
     * Shift bit 6 already selects expression ADC12/A31 when used. */
    uint32_t pins = GP5_PIN(29) | GP5_PIN(30);
    if (fw_gp50_expression) pins |= GP5_PIN(31);
    gp5_reg_bits_set(GP5_GPIO_A_ANA_EN, pins);
    gp5_adc_init();
}

gp50_status_t gp50_status(void) { return status; }

void gp50_power_poll(void)
{
    static uint32_t next;
    static unsigned charge_changes;
    uint32_t now = gp5_ms();
    if ((int32_t)(now - next) < 0) return;
    next = now + 100;

    /* V1.0.5 0x14784: the active-low charger input debounces independently of the ADC. */
    int charge = !(gp50_control_input & CHARGE_INPUT);
    if (charge == status.charging) charge_changes = 0;
    else if (status.charging < 0 || ++charge_changes == 6) {
        status.charging = charge;
        charge_changes = 0;
    }

    int voltage = gp5_adc_read(10);
    int current = gp5_adc_read(11);
    if ((unsigned)voltage > 4095u || (unsigned)current > 4095u) return;

    /* V1.0.5 0x14640 and 0x14950: compensate for current before applying hysteresis. */
    static const unsigned rising[] = { 3630, 3680, 3812, 3956 };
    static const unsigned falling[] = { 3615, 3665, 3797, 3928 };
    int current_ua = current * 359 - 489000;
    int adjusted_uv = voltage * 109022 / 100 - current_ua * 1563 / 5000;
    int bars = 0;
    for (unsigned i = 0; i < sizeof rising / sizeof rising[0]; ++i)
        bars += current_ua > 0 ? adjusted_uv > (int)rising[i] * 1000 :
                                adjusted_uv >= (int)falling[i] * 1000;
    if (status.battery < 0 || (current_ua > 0 ? bars > status.battery : bars < status.battery))
        status.battery = bars;
}

int gp50_expression_read_raw(unsigned *value)
{
    if (!expression_sampled) return 0;
    *value = expression_raw;
    return 1;
}

int gp50_expression_set_range(unsigned heel, unsigned toe)
{
    if (!gp50_expression_range_valid(heel, toe)) return 0;
    if (heel != expression_heel || toe != expression_toe) expression_remap = expression_sampled;
    expression_heel = heel;
    expression_toe = toe;
    return 1;
}

static unsigned expression_value(unsigned raw)
{
    if (raw <= expression_heel) return 0;
    if (raw >= expression_toe) return 255;
    return (raw - expression_heel) * 255u / (expression_toe - expression_heel);
}

void gp50_expression_poll(void)
{
    if (!fw_gp50_expression) return;
    unsigned sample = (unsigned)gp5_adc_read(12);
    if (sample > GP50_EXPRESSION_MAX_RAW) return;
    expression_raw = sample;
    if (!expression_sampled) {
        expression_filtered_q4 = sample << 4;
        expression_last = sample;
        expression_reported = expression_value(sample);
        expression_sampled = 1;
        return;
    }

    /* V1.0.5 0x4763c uses a 1/4 EMA. Extra fraction bits let a stable heel
     * or toe settle exactly, and reaching a stop completes the sweep. */
    expression_filtered_q4 = ((sample << 4) + 3u * expression_filtered_q4) >> 2;
    unsigned filtered = (expression_filtered_q4 + 8u) >> 4;
    unsigned delta = filtered > expression_last ? filtered - expression_last : expression_last - filtered;
    int reached_stop = (filtered <= expression_heel && expression_reported != 0u)
                    || (filtered >= expression_toe && expression_reported != 255u);
    if (!expression_remap && !reached_stop) {
        if (delta < 16u) { expression_pending = 0; return; }
        if (expression_pending++ < 21u && delta < 64u) return;
    }
    expression_last = filtered;
    expression_pending = 0;
    expression_remap = 0;
    unsigned value = expression_value(expression_last);
    if (value != expression_reported) {
        expression_reported = value;
        fw_gp50_expression(value);
    }
}
