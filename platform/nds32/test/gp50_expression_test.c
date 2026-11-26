#include "gp50_controls.h"
#include "gp5_firmware.h"
#include "hal/adc.h"
#include "hal/gpio.h"
#include <assert.h>

static int32_t adc;
static unsigned value, changes;

void fw_gp50_expression(unsigned next) { value = next; ++changes; }
void fw_gp50_encoder_turn(unsigned encoder, int detents) { (void)encoder; (void)detents; }
void fw_gp50_button(unsigned button, int down) { (void)button; (void)down; }
uint32_t gp5_ms(void) { return 0; }
int gp5_gpio_a_digital(unsigned pin) { (void)pin; return 0; }
int gp5_gpio_b_digital(unsigned pin) { (void)pin; return 0; }
void gp5_adc_init(void) { assert(0); }
int32_t gp5_adc_read(unsigned channel) { assert(channel == 12); return adc; }

static void sample(int32_t raw, unsigned polls)
{
    adc = raw;
    for (unsigned i = 0; i < polls; i++) gp50_expression_poll();
}

int main(void)
{
    unsigned raw;
    assert(!gp50_expression_read_raw(&raw));
    sample(-1, 1);
    assert(!gp50_expression_read_raw(&raw));
    sample(0, 1);
    sample(3, 32);
    assert(changes == 0);
    assert(gp50_expression_read_raw(&raw) && raw == 3);

    sample(200, 64);
    assert(value == 255);
    sample(0, 64);
    assert(value == 0);
    sample(250, 64);
    assert(value == 255);
    unsigned before = changes;
    sample(4096, 1);
    sample(-1, 1);
    assert(changes == before && gp50_expression_read_raw(&raw) && raw == 250);

    assert(!gp50_expression_set_range(400, 100));
    assert(!gp50_expression_set_range(100, 150));
    assert(!gp50_expression_set_range(100, 4096));
    assert(gp50_expression_set_range(100, 151));
    assert(gp50_expression_set_range(100, 400));
    sample(250, 1);
    assert(changes == before + 1 && value == 127);
    sample(400, 64);
    assert(value == 255);
    sample(100, 64);
    assert(value == 0);
}
