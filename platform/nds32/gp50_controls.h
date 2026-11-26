#ifndef GP50_CONTROLS_H
#define GP50_CONTROLS_H
#include <stdint.h>

#define GP50_PARA_PIN (1u << 15)          /* port A */
#define GP50_FS1_BIT (1u << 14)           /* shift input */

void gp50_controls_init(void);
void gp50_controls_sample(void);
void gp50_controls_dispatch(void);
void gp50_controls_prepare_update(void);
void gp50_expression_init(void);
void gp50_expression_poll(void);
extern uint16_t gp50_control_input;

/* Count a detent on the transition to both phases low. Clockwise increases. */
static inline int gp50_encoder_delta(unsigned previous, unsigned ab)
{
    return ab == 0 ? (previous == 2) - (previous == 1) : 0;
}

#endif
