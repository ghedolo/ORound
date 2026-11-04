#pragma once

// Battery voltage on BAT_ADC (GPIO4, ADC1 channel 3), divider 200K / 200K: pin = VBAT / 2.
// On USB power without battery the value is the charger output, not a battery.

// Battery voltage in volts (average of several samples), 0 if the ADC is not available
float battery_volts(void);

// Charge 0..100 % from the voltage (typical LiPo discharge curve, at rest)
int battery_percent(float volts);

// Full screen on black: percent (large) and voltage below, yellow. Draws one frame and returns.
void battery_show(float volts);
