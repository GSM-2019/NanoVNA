/*
 * Copyright (c) 2014-2015, TAKAHASHI Tomohiro (TTRFTECH) edy555@gmail.com
 * All rights reserved.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * The software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */
#include "hal.h"
#include "nanovna.h"
#include "si5351.h"

#define SI5351_I2C_ADDR   	(0x60<<1)

static void
si5351_write(uint8_t reg, uint8_t dat)
{
  int addr = SI5351_I2C_ADDR>>1;
  uint8_t buf[] = { reg, dat };
  i2cAcquireBus(&I2CD1);
  (void)i2cMasterTransmitTimeout(&I2CD1, addr, buf, 2, NULL, 0, 1000);
  i2cReleaseBus(&I2CD1);
}

static void
si5351_bulk_write(const uint8_t *buf, int len)
{
  int addr = SI5351_I2C_ADDR>>1;
  i2cAcquireBus(&I2CD1);
  (void)i2cMasterTransmitTimeout(&I2CD1, addr, buf, len, NULL, 0, 1000);
  i2cReleaseBus(&I2CD1);
}

// register addr, length, data, ...
const uint8_t si5351_configs[] = {
  2, SI5351_REG_3_OUTPUT_ENABLE_CONTROL, 0xff,
  4, SI5351_REG_16_CLK0_CONTROL, SI5351_CLK_POWERDOWN, SI5351_CLK_POWERDOWN, SI5351_CLK_POWERDOWN,
  2, SI5351_REG_183_CRYSTAL_LOAD, SI5351_CRYSTAL_LOAD_8PF,
  // setup PLL (26MHz * 32 = 832MHz, 32/2-2=14)
  9, SI5351_REG_26_PLL_A, /*P3*/0, 1, /*P1*/0, 14, 0, /*P3/P2*/0, 0, 0,
  // RESET PLL
  2, SI5351_REG_177_PLL_RESET, SI5351_PLL_RESET_A | SI5351_PLL_RESET_B,
  // setup multisynth (832MHz / 104 = 8MHz, 104/2-2=50)
  9, SI5351_REG_58_MULTISYNTH2, /*P3*/0, 1, /*P1*/0, 50, 0, /*P2|P3*/0, 0, 0,
  2, SI5351_REG_18_CLK2_CONTROL, SI5351_CLK_DRIVE_STRENGTH_2MA | SI5351_CLK_INPUT_MULTISYNTH_N | SI5351_CLK_INTEGER_MODE,
  2, SI5351_REG_3_OUTPUT_ENABLE_CONTROL, 0,
  0 // sentinel
};

void
si5351_init(void)
{
  const uint8_t *p = si5351_configs;
  while (*p) {
    uint8_t len = *p++;
    si5351_bulk_write(p, len);
    p += len;
  }
}

static const uint8_t disable_output[] = {
  SI5351_REG_16_CLK0_CONTROL,
  SI5351_CLK_POWERDOWN,
  SI5351_CLK_POWERDOWN,
  SI5351_CLK_POWERDOWN
};

void si5351_disable_output(void)
{
  si5351_write(SI5351_REG_3_OUTPUT_ENABLE_CONTROL, 0xff);
  si5351_bulk_write(disable_output, sizeof(disable_output));
}

void si5351_enable_output(void)
{
  si5351_write(SI5351_REG_3_OUTPUT_ENABLE_CONTROL, 0x00);
}

static void si5351_reset_pll(void)
{
  //si5351_write(SI5351_REG_177_PLL_RESET, SI5351_PLL_RESET_A | SI5351_PLL_RESET_B);
  si5351_write(SI5351_REG_177_PLL_RESET, 0xAC);
}

static void si5351_setupPLL(uint8_t pll, /* SI5351_PLL_A or SI5351_PLL_B */
                     uint8_t     mult,
                     uint32_t    num,
                     uint32_t    denom)
{
  /* Get the appropriate starting point for the PLL registers */
  static const uint8_t pllreg_base[] = {
    SI5351_REG_26_PLL_A,
    SI5351_REG_34_PLL_B
  };
  uint32_t P1;
  uint32_t P2;
  uint32_t P3;

  /* Feedback Multisynth Divider Equation
   * where: a = mult, b = num and c = denom
   * P1 register is an 18-bit value using following formula:
   * 	P1[17:0] = 128 * mult + floor(128*(num/denom)) - 512
   * P2 register is a 20-bit value using the following formula:
   * 	P2[19:0] = 128 * num - denom * floor(128*(num/denom))
   * P3 register is a 20-bit value using the following formula:
   * 	P3[19:0] = denom
   */

  /* Set the main PLL config registers */
  if (num == 0)
  {
    /* Integer mode */
    P1 = 128 * mult - 512;
    P2 = 0;
    P3 = 1;
  }
  else
  {
    /* Fractional mode */
    //P1 = (uint32_t)(128 * mult + floor(128 * ((float)num/(float)denom)) - 512);
    P1 = 128 * mult + ((128 * num) / denom) - 512;
    //P2 = (uint32_t)(128 * num - denom * floor(128 * ((float)num/(float)denom)));
    P2 = 128 * num - denom * ((128 * num) / denom);
    P3 = denom;
  }

  /* The datasheet is a nightmare of typos and inconsistencies here! */
  uint8_t reg[9];
  reg[0] = pllreg_base[pll];
  reg[1] = (P3 & 0x0000FF00) >> 8;
  reg[2] = (P3 & 0x000000FF);
  reg[3] = (P1 & 0x00030000) >> 16;
  reg[4] = (P1 & 0x0000FF00) >> 8;
  reg[5] = (P1 & 0x000000FF);
  reg[6] = ((P3 & 0x000F0000) >> 12) | ((P2 & 0x000F0000) >> 16);
  reg[7] = (P2 & 0x0000FF00) >> 8;
  reg[8] = (P2 & 0x000000FF);
  si5351_bulk_write(reg, 9);
}

static void
si5351_setupMultisynth(uint8_t     output,
                       uint8_t     pllSource,
                       uint32_t    div, // 4,6,8, 8+ ~ 900
                       uint32_t    num,
                       uint32_t    denom,
                       uint32_t    rdiv, // SI5351_R_DIV_1~128
                       uint8_t     drive_strength)
{
  /* Get the appropriate starting point for the PLL registers */
  static const uint8_t msreg_base[] = {
    SI5351_REG_42_MULTISYNTH0,
    SI5351_REG_50_MULTISYNTH1,
    SI5351_REG_58_MULTISYNTH2,
  };
  static const uint8_t clkctrl[] = {
    SI5351_REG_16_CLK0_CONTROL,
    SI5351_REG_17_CLK1_CONTROL,
    SI5351_REG_18_CLK2_CONTROL
  };
  uint8_t dat;

  uint32_t P1;
  uint32_t P2;
  uint32_t P3;
  uint32_t div4 = 0;

  /* Output Multisynth Divider Equations
   * where: a = div, b = num and c = denom
   * P1 register is an 18-bit value using following formula:
   * 	P1[17:0] = 128 * a + floor(128*(b/c)) - 512
   * P2 register is a 20-bit value using the following formula:
   * 	P2[19:0] = 128 * b - c * floor(128*(b/c))
   * P3 register is a 20-bit value using the following formula:
   * 	P3[19:0] = c
   */
  /* Set the main PLL config registers */
  if (div == 4) {
    div4 = SI5351_DIVBY4;
    P1 = P2 = 0;
    P3 = 1;
  } else if (num == 0) {
    /* Integer mode */
    P1 = 128 * div - 512;
    P2 = 0;
    P3 = 1;
  } else {
    /* Fractional mode */
    P1 = 128 * div + ((128 * num) / denom) - 512;
    P2 = 128 * num - denom * ((128 * num) / denom);
    P3 = denom;
  }

  /* Set the MSx config registers */
  uint8_t reg[9];
  reg[0] = msreg_base[output];
  reg[1] = (P3 & 0x0000FF00) >> 8;
  reg[2] = (P3 & 0x000000FF);
  reg[3] = ((P1 & 0x00030000) >> 16) | div4 | rdiv;
  reg[4] = (P1 & 0x0000FF00) >> 8;
  reg[5] = (P1 & 0x000000FF);
  reg[6] = ((P3 & 0x000F0000) >> 12) | ((P2 & 0x000F0000) >> 16);
  reg[7] = (P2 & 0x0000FF00) >> 8;
  reg[8] = (P2 & 0x000000FF);
  si5351_bulk_write(reg, 9);

  /* Configure the clk control and enable the output */
  dat = drive_strength | SI5351_CLK_INPUT_MULTISYNTH_N;
  if (pllSource == SI5351_PLL_B)
    dat |= SI5351_CLK_PLL_SELECT_B;
  if (num == 0)
    dat |= SI5351_CLK_INTEGER_MODE;
  si5351_write(clkctrl[output], dat);
}

#define XTALFREQ 26000000L
#define PLL_N 32
#define PLLFREQ (XTALFREQ * PLL_N)

static void
si5351_set_frequency_fixedpll(int channel, int pll, uint32_t pllfreq, uint32_t freq,
                              int rdiv, uint8_t drive_strength, int mul)
{
    int denom = freq;
    int div = (pllfreq * mul) / denom; // range: 8 ~ 1800
    int num = (pllfreq * mul) - denom * div;

    // cf. https://github.com/python/cpython/blob/master/Lib/fractions.py#L227
    int max_denominator = (1 << 20) - 1;
    if (denom > max_denominator) {
      int p0 = 0, q0 = 1, p1 = 1, q1 = 0;
      while (denom != 0) {
        int a = num / denom;
        int q2 = q0 + a*q1;
        if (q2 > max_denominator)
          break;
        int p2 = p0 + a*p1;
        p0 = p1; q0 = q1; p1 = p2; q1 = q2;
        int new_denom = num - a * denom;
        num = denom; denom = new_denom;
      }
      num = p1;
      denom = q1;
    }

    si5351_setupMultisynth(channel, pll, div, num, denom, rdiv, drive_strength);
}

static void
si5351_set_frequency_fixeddiv(int channel, int pll, uint32_t freq, int div,
                              uint8_t drive_strength, int mul)
{
    int denom = XTALFREQ * mul;
    int64_t pllfreq = (int64_t)freq * div;
    int multi = pllfreq / denom;
    int num = pllfreq - denom * multi;

    // cf. https://github.com/python/cpython/blob/master/Lib/fractions.py#L227
    int max_denominator = (1 << 20) - 1;
    if (denom > max_denominator) {
      int p0 = 0, q0 = 1, p1 = 1, q1 = 0;
      while (denom != 0) {
        int a = num / denom;
        int q2 = q0 + a*q1;
        if (q2 > max_denominator)
          break;
        int p2 = p0 + a*p1;
        p0 = p1; q0 = q1; p1 = p2; q1 = q2;
        int new_denom = num - a * denom;
        num = denom; denom = new_denom;
      }
      num = p1;
      denom = q1;
    }

    si5351_setupPLL(pll, multi, num, denom);
    si5351_setupMultisynth(channel, pll, div, 0, 1, SI5351_R_DIV_1, drive_strength);
}

/* 
 * 1~100MHz fixed PLL 900MHz, fractional divider
 * 100~150MHz fractional PLL 600-900MHz, fixed divider 6
 * 150~200MHz fractional PLL 600-900MHz, fixed divider 4
 */
#if 0
void
si5351_set_frequency(int channel, int freq, uint8_t drive_strength)
{
  if (freq <= 100000000) {
    si5351_setupPLL(SI5351_PLL_B, 32, 0, 1);
    si5351_set_frequency_fixedpll(channel, SI5351_PLL_B, PLLFREQ, freq, SI5351_R_DIV_1, drive_strength, 1);
  } else if (freq < 150000000) {
    si5351_set_frequency_fixeddiv(channel, SI5351_PLL_B, freq, 6, drive_strength, 1);
  } else {
    si5351_set_frequency_fixeddiv(channel, SI5351_PLL_B, freq, 4, drive_strength, 1);
  }
}
#endif

int current_band = -1;

#define DELAY_NORMAL 3
#define DELAY_BANDCHANGE 1
#define DELAY_LOWBAND 1

/*
 * configure output as follows:
 * CLK0: frequency + offset
 * CLK1: frequency
 * CLK2: fixed 8MHz
 */
#define CLK2_FREQUENCY 8000000L
int
si5351_set_frequency_with_offset(uint32_t freq, int offset, uint8_t drive_strength)
{
  int band;
  int delay = DELAY_NORMAL;
  uint32_t ofreq = freq + offset;
  uint32_t mul = 1, omul = 1;
  uint32_t rdiv = SI5351_R_DIV_1;
  if (freq >= config.harmonic_freq_threshold * 7U) {
    mul = 9;
    omul = 11;
  } else if (freq >= config.harmonic_freq_threshold * 5U) {
    mul = 7;
    omul = 9;
  } else if (freq >= config.harmonic_freq_threshold * 3U) {
    mul = 5;
    omul = 7;
  } else if (freq >= config.harmonic_freq_threshold) {
    mul = 3;
    omul = 5;
  }
  if ((freq / mul) < 100000000U) {
    band = 0;
  } else if ((freq / mul) < 150000000U) {
    band = 1;
  } else {
    band = 2;
  }
  if (freq <= 500000U) {
    rdiv = SI5351_R_DIV_64;
  } else if (freq <= 4000000U) {
    rdiv = SI5351_R_DIV_8;
  }

#if 1
  if (current_band != band)
    si5351_disable_output();
#endif

  switch (band) {
  case 0:
    // fractional divider mode. only PLL A is used.
    if (current_band == 1 || current_band == 2)
      si5351_setupPLL(SI5351_PLL_A, 32, 0, 1);
    // Set PLL twice on changing from band 2
    if (current_band == 2) 
      si5351_setupPLL(SI5351_PLL_A, 32, 0, 1);

    if (rdiv == SI5351_R_DIV_8) {
      freq *= 8;
      ofreq *= 8;
    } else if (rdiv == SI5351_R_DIV_64) {
      freq *= 64;
      ofreq *= 64;
    }

    si5351_set_frequency_fixedpll(0, SI5351_PLL_A, PLLFREQ, ofreq,
                                  rdiv, drive_strength, omul);
    si5351_set_frequency_fixedpll(1, SI5351_PLL_A, PLLFREQ, freq,
                                  rdiv, drive_strength, mul);
    //if (current_band != 0)
      si5351_set_frequency_fixedpll(2, SI5351_PLL_A, PLLFREQ, CLK2_FREQUENCY,
                                    SI5351_R_DIV_1, SI5351_CLK_DRIVE_STRENGTH_2MA, 1);
    break;

  case 1:
    // Set PLL twice on changing from band 2
    if (current_band == 2) {
      si5351_set_frequency_fixeddiv(0, SI5351_PLL_A, ofreq, 6, drive_strength, omul);
      si5351_set_frequency_fixeddiv(1, SI5351_PLL_B, freq, 6, drive_strength, mul);
    }

    // div by 6 mode. both PLL A and B are dedicated for CLK0, CLK1
    si5351_set_frequency_fixeddiv(0, SI5351_PLL_A, ofreq, 6, drive_strength, omul);
    si5351_set_frequency_fixeddiv(1, SI5351_PLL_B, freq, 6, drive_strength, mul);
    si5351_set_frequency_fixedpll(2, SI5351_PLL_B, freq / mul * 6, CLK2_FREQUENCY,
                                  SI5351_R_DIV_1, SI5351_CLK_DRIVE_STRENGTH_2MA, 1);
    break;

  case 2:
    // div by 4 mode. both PLL A and B are dedicated for CLK0, CLK1
    si5351_set_frequency_fixeddiv(0, SI5351_PLL_A, ofreq, 4, drive_strength, omul);
    si5351_set_frequency_fixeddiv(1, SI5351_PLL_B, freq, 4, drive_strength, mul);
    si5351_set_frequency_fixedpll(2, SI5351_PLL_B, freq / mul * 4, CLK2_FREQUENCY,
                                  SI5351_R_DIV_1, SI5351_CLK_DRIVE_STRENGTH_2MA, 1);
    break;
  }

  if (current_band != band) {
    si5351_reset_pll();
#if 1
    si5351_enable_output();
#endif
    delay += DELAY_BANDCHANGE;
  }
  if (band == 0)
    delay += DELAY_LOWBAND;

  current_band = band;
  return delay;
}
