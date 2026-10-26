//! Elonics E4000 tuner driver, a translation of `src/tuner_e4k.c` of librtlsdr 1.3.6 (RTL-SDR Blog fork).
//!
//! (C) 2011-2012 by Harald Welte <laforge@gnumonks.org>, (C) 2012 by Sylvain Munaut <tnt@246tNt.com>, (C) 2012 by Hoernchen <la@tfc-server.de>;
//! GPL-2.0-or-later. Register numbers, tables and integer conversions are those of the C (`uint8_t` truncations included, e.g. a failed register read
//! reads as 0xff inside the mask helper). The DC offset calibration helpers that the C never calls are not translated.

use crate::rtl::{RtlSdr, EINVAL};
use crate::transfer::UsbTransfer;

const E4K_I2C_ADDR: u8 = 0xc8;

const REG_MASTER1: u8 = 0x00;
const REG_CLK_INP: u8 = 0x05;
const REG_REF_CLK: u8 = 0x06;
const REG_SYNTH1: u8 = 0x07;
const REG_SYNTH3: u8 = 0x09;
const REG_SYNTH4: u8 = 0x0a;
const REG_SYNTH5: u8 = 0x0b;
const REG_SYNTH7: u8 = 0x0d;
const REG_FILT1: u8 = 0x10;
const REG_FILT2: u8 = 0x11;
const REG_FILT3: u8 = 0x12;
const REG_GAIN1: u8 = 0x14;
const REG_GAIN2: u8 = 0x15;
const REG_GAIN3: u8 = 0x16;
const REG_GAIN4: u8 = 0x17;
const REG_AGC1: u8 = 0x1a;
const REG_AGC4: u8 = 0x1d;
const REG_AGC5: u8 = 0x1e;
const REG_AGC6: u8 = 0x1f;
const REG_AGC7: u8 = 0x20;
const REG_AGC11: u8 = 0x24;
const REG_DC5: u8 = 0x2d;
const REG_DCTIME1: u8 = 0x70;
const REG_DCTIME2: u8 = 0x71;
const REG_BIAS: u8 = 0x78;
const REG_CLKOUT_PWDN: u8 = 0x7a;

const MASTER1_RESET: u8 = 1 << 0;
const MASTER1_NORM_STBY: u8 = 1 << 1;
const MASTER1_POR_DET: u8 = 1 << 2;
const FILT3_DISABLE: u8 = 1 << 5;
const AGC7_MIX_GAIN_AUTO: u8 = 1 << 0;
const AGC1_MOD_MASK: u8 = 0xF;
const AGC_MOD_SERIAL: u8 = 0x0;
const AGC_MOD_IF_SERIAL_LNA_AUTON: u8 = 0x9;

const WIDTH2MASK: [u8; 9] = [0, 1, 3, 7, 0xf, 0x1f, 0x3f, 0x7f, 0xff];

/// A register field: (register, shift, width).
struct Field(u8, u8, u8);

/// `enum e4k_band`
#[derive(Clone, Copy, PartialEq, Eq)]
enum Band {
    Vhf2 = 0,
    Vhf3 = 1,
    Uhf = 2,
    L = 3,
}

/// `enum e4k_if_filter`
#[derive(Clone, Copy)]
enum IfFilter {
    Mix = 0,
    Chan = 1,
    Rc = 2,
}

const RF_FILT_CENTER_UHF: [u32; 16] = [
    360_000_000,
    380_000_000,
    405_000_000,
    425_000_000,
    450_000_000,
    475_000_000,
    505_000_000,
    540_000_000,
    575_000_000,
    615_000_000,
    670_000_000,
    720_000_000,
    760_000_000,
    840_000_000,
    890_000_000,
    970_000_000,
];
const RF_FILT_CENTER_L: [u32; 16] = [
    1_300_000_000,
    1_320_000_000,
    1_360_000_000,
    1_410_000_000,
    1_445_000_000,
    1_460_000_000,
    1_490_000_000,
    1_530_000_000,
    1_560_000_000,
    1_590_000_000,
    1_640_000_000,
    1_660_000_000,
    1_680_000_000,
    1_700_000_000,
    1_720_000_000,
    1_750_000_000,
];
const MIX_FILTER_BW: [u32; 16] = [
    27_000_000, 27_000_000, 27_000_000, 27_000_000, 27_000_000, 27_000_000, 27_000_000, 27_000_000, 4_600_000, 4_200_000, 3_800_000,
    3_400_000, 3_300_000, 2_700_000, 2_300_000, 1_900_000,
];
const IFRC_FILTER_BW: [u32; 16] = [
    21_400_000, 21_000_000, 17_600_000, 14_700_000, 12_400_000, 10_600_000, 9_000_000, 7_700_000, 6_400_000, 5_300_000, 4_400_000,
    3_400_000, 2_600_000, 1_800_000, 1_200_000, 1_000_000,
];
const IFCH_FILTER_BW: [u32; 32] = [
    5_500_000, 5_300_000, 5_000_000, 4_800_000, 4_600_000, 4_400_000, 4_300_000, 4_100_000, 3_900_000, 3_800_000, 3_700_000, 3_600_000,
    3_400_000, 3_300_000, 3_200_000, 3_100_000, 3_000_000, 2_950_000, 2_900_000, 2_800_000, 2_750_000, 2_700_000, 2_600_000, 2_550_000,
    2_500_000, 2_450_000, 2_400_000, 2_300_000, 2_280_000, 2_240_000, 2_200_000, 2_150_000,
];

const IF_FILTER_FIELDS: [Field; 3] = [Field(REG_FILT2, 4, 4), Field(REG_FILT3, 0, 5), Field(REG_FILT2, 0, 4)];

/// (frequency below which the entry applies, SYNTH7 value, multiplier)
const PLL_VARS: [(u32, u8, u8); 10] = [
    (72_400_000, (1 << 3) | 7, 48),
    (81_200_000, (1 << 3) | 6, 40),
    (108_300_000, (1 << 3) | 5, 32),
    (162_500_000, (1 << 3) | 4, 24),
    (216_600_000, (1 << 3) | 3, 16),
    (325_000_000, (1 << 3) | 2, 12),
    (350_000_000, (1 << 3) | 1, 8),
    (432_000_000, 3, 8),
    (667_000_000, 2, 6),
    (1_200_000_000, 1, 4),
];

const IF_STAGE1_GAIN: [i8; 2] = [-3, 6];
const IF_STAGE23_GAIN: [i8; 4] = [0, 3, 6, 9];
const IF_STAGE4_GAIN: [i8; 4] = [0, 1, 2, 2];
const IF_STAGE56_GAIN: [i8; 8] = [3, 6, 9, 12, 15, 15, 15, 15];
const IF_STAGE_GAIN_REGS: [Field; 7] = [
    Field(0, 0, 0),
    Field(REG_GAIN3, 0, 1),
    Field(REG_GAIN3, 1, 2),
    Field(REG_GAIN3, 3, 2),
    Field(REG_GAIN3, 5, 2),
    Field(REG_GAIN4, 0, 3),
    Field(REG_GAIN4, 3, 3),
];

/// (gain in 1/10 dB, register value) of the LNA.
const LNAGAIN: [(i32, u8); 13] =
    [(-50, 0), (-25, 1), (0, 4), (25, 5), (50, 6), (75, 7), (100, 8), (125, 9), (150, 10), (175, 11), (200, 12), (250, 13), (300, 14)];

/// PLL parameters (`struct e4k_pll_params`).
#[derive(Clone, Copy, Default)]
pub(crate) struct PllParams {
    pub(crate) fosc: u32,
    pub(crate) intended_flo: u32,
    pub(crate) flo: u32,
    x: u16,
    z: u8,
    r: u8,
    r_idx: u8,
}

/// State of the tuner (`struct e4k_state`).
pub(crate) struct E4k {
    i2c_addr: u8,
    band: Band,
    pub(crate) vco: PllParams,
}

impl E4k {
    pub(crate) fn new() -> Self {
        E4k { i2c_addr: E4K_I2C_ADDR, band: Band::Vhf2, vco: PllParams::default() }
    }
}

fn unsigned_delta(a: u32, b: u32) -> u32 {
    a.abs_diff(b)
}

fn closest_arr_idx(arr: &[u32], freq: u32) -> i32 {
    let mut bi = 0;
    let mut best_delta = 0xffff_ffffu32;
    for (i, &a) in arr.iter().enumerate() {
        let delta = unsigned_delta(freq, a);
        if delta < best_delta {
            best_delta = delta;
            bi = i;
        }
    }
    bi as i32
}

/// The 4 bit index of the RF filter for the band and frequency.
fn choose_rf_filter(band: Band, freq: u32) -> i32 {
    match band {
        Band::Vhf2 | Band::Vhf3 => 0,
        Band::Uhf => closest_arr_idx(&RF_FILT_CENTER_UHF, freq),
        Band::L => closest_arr_idx(&RF_FILT_CENTER_L, freq),
    }
}

fn is_fosc_valid(fosc: u32) -> bool {
    (16_000_000..=30_000_000).contains(&fosc)
}

impl<T: UsbTransfer> RtlSdr<T> {
    /// Writes a register of the tuner chip (`e4k_reg_write`); 0 or -1.
    fn e4k_reg_write(&mut self, reg: u8, val: u8) -> i32 {
        let a = self.e4k.i2c_addr;
        let r = self.i2c_write(a, &[reg, val]);
        if r == 2 {
            0
        } else {
            -1
        }
    }

    /// Reads a register: the 8 bit value, or -1 (`e4k_reg_read`).
    fn e4k_reg_read(&mut self, reg: u8) -> i32 {
        let a = self.e4k.i2c_addr;
        if self.i2c_write(a, &[reg]) < 1 {
            return -1;
        }
        let mut data = [reg];
        if self.i2c_read(a, &mut data) < 1 {
            return -1;
        }
        data[0] as i32
    }

    /// Sets the masked bits of a register to `val` if they differ (`e4k_reg_set_mask`). A failed read counts as 0xff, like the C's `uint8_t tmp`.
    fn e4k_reg_set_mask(&mut self, reg: u8, mask: u8, val: u8) -> i32 {
        let tmp = self.e4k_reg_read(reg) as u8;
        if tmp & mask == val {
            return 0;
        }
        self.e4k_reg_write(reg, (tmp & !mask) | (val & mask))
    }

    /// Writes a field inside a register; reads the register once more first, like the C (`e4k_field_write`).
    fn e4k_field_write(&mut self, field: &Field, val: u8) -> i32 {
        let rc = self.e4k_reg_read(field.0);
        if rc < 0 {
            return rc;
        }
        let mask = WIDTH2MASK[(field.2 as usize).min(8)].wrapping_shl(field.1 as u32);
        self.e4k_reg_set_mask(field.0, mask, val.wrapping_shl(field.1 as u32))
    }

    fn e4k_rf_filter_set(&mut self) -> i32 {
        let rc = choose_rf_filter(self.e4k.band, self.e4k.vco.flo);
        if rc < 0 {
            return rc;
        }
        self.e4k_reg_set_mask(REG_FILT1, 0xF, rc as u8)
    }

    fn e4k_if_filter_bw_set(&mut self, filter: IfFilter, bandwidth: u32) -> i32 {
        let arr: &[u32] = match filter {
            IfFilter::Mix => &MIX_FILTER_BW,
            IfFilter::Chan => &IFCH_FILTER_BW,
            IfFilter::Rc => &IFRC_FILTER_BW,
        };
        let bw_idx = closest_arr_idx(arr, bandwidth);
        self.e4k_field_write(&IF_FILTER_FIELDS[filter as usize], bw_idx as u8)
    }

    fn e4k_if_filter_chan_enable(&mut self, on: bool) -> i32 {
        self.e4k_reg_set_mask(REG_FILT3, FILT3_DISABLE, if on { 0 } else { FILT3_DISABLE })
    }

    fn e4k_band_set(&mut self, band: Band) -> i32 {
        match band {
            Band::Vhf2 | Band::Vhf3 | Band::Uhf => {
                self.e4k_reg_write(REG_BIAS, 3);
            }
            Band::L => {
                self.e4k_reg_write(REG_BIAS, 0);
            }
        }
        // workaround: reset this register before writing to it, or there is a gap between 325-350 MHz
        self.e4k_reg_set_mask(REG_SYNTH1, 0x06, 0);
        let rc = self.e4k_reg_set_mask(REG_SYNTH1, 0x06, (band as u8) << 1);
        if rc >= 0 {
            self.e4k.band = band;
        }
        rc
    }

    /// PLL parameters for the target frequency; the actual PLL frequency, 0 for an invalid crystal (`e4k_compute_pll_params`).
    fn e4k_compute_pll_params(oscp: &mut PllParams, fosc: u32, intended_flo: u32) -> u32 {
        let mut r: u8 = 2;
        oscp.r_idx = 0;
        if !is_fosc_valid(fosc) {
            return 0;
        }
        for &(f, synth7, mult) in PLL_VARS.iter() {
            if intended_flo < f {
                oscp.r_idx = synth7;
                r = mult;
                break;
            }
        }
        // flo(max) = 1700MHz, R(max) = 48, we need 64bit
        let intended_fvco = intended_flo as u64 * r as u64;
        // integral component of the multiplier
        let z = intended_fvco / fosc as u64;
        // fractional part
        let remainder = intended_fvco - (fosc as u64 * z);
        let x = ((remainder * 65536) / fosc as u64) as u32;
        // compute_flo(fosc, z (uint8), x (uint16), r)
        let (zb, xw) = (z as u8, x as u16);
        let fvco_z = fosc as u64 * zb as u64;
        let fvco_x = (fosc as u64 * xw as u64) / 65536;
        let fvco = fvco_z + fvco_x;
        let flo: u32 = if fvco == 0 { (-EINVAL) as u32 } else { (fvco / r as u64) as u32 };
        oscp.fosc = fosc;
        oscp.flo = flo;
        oscp.intended_flo = intended_flo;
        oscp.r = r;
        oscp.x = x as u16;
        oscp.z = z as u8;
        flo
    }

    fn e4k_tune_params(&mut self, p: &PllParams) -> i32 {
        // program R + 3phase/2phase
        self.e4k_reg_write(REG_SYNTH7, p.r_idx);
        // program Z
        self.e4k_reg_write(REG_SYNTH3, p.z);
        // program X
        self.e4k_reg_write(REG_SYNTH4, (p.x & 0xff) as u8);
        self.e4k_reg_write(REG_SYNTH5, (p.x >> 8) as u8);
        // we're in auto calibration mode, so there's no need to trigger it
        self.e4k.vco = *p;
        // set the band
        let flo = self.e4k.vco.flo;
        if flo < 140_000_000 {
            self.e4k_band_set(Band::Vhf2);
        } else if flo < 350_000_000 {
            self.e4k_band_set(Band::Vhf3);
        } else if flo < 1_135_000_000 {
            self.e4k_band_set(Band::Uhf);
        } else {
            self.e4k_band_set(Band::L);
        }
        // select and set proper RF filter
        self.e4k_rf_filter_set();
        self.e4k.vco.flo as i32
    }

    /// Tunes to `freq` Hz; 0, -1 when the PLL does not lock, -22 for an invalid crystal (`e4k_tune_freq`).
    fn e4k_tune_freq(&mut self, freq: u32) -> i32 {
        let mut p = PllParams::default();
        // determine PLL parameters
        let rc = Self::e4k_compute_pll_params(&mut p, self.e4k.vco.fosc, freq);
        if rc == 0 {
            return -EINVAL;
        }
        // actually tune to those parameters
        self.e4k_tune_params(&p);
        // check PLL lock (the C tests an unsigned copy of the read value)
        let rc = self.e4k_reg_read(REG_SYNTH1) as u32;
        if rc & 0x01 == 0 {
            return -1;
        }
        0
    }

    fn e4k_set_lna_gain(&mut self, gain: i32) -> i32 {
        for &(g, v) in LNAGAIN.iter() {
            if g == gain {
                self.e4k_reg_set_mask(REG_GAIN1, 0xf, v);
                return gain;
            }
        }
        -EINVAL
    }

    fn e4k_enable_manual_gain(&mut self, manual: u8) -> i32 {
        if manual != 0 {
            // Set LNA mode to manual
            self.e4k_reg_set_mask(REG_AGC1, AGC1_MOD_MASK, AGC_MOD_SERIAL);
            // Set Mixer Gain Control to manual
            self.e4k_reg_set_mask(REG_AGC7, AGC7_MIX_GAIN_AUTO, 0);
        } else {
            // Set LNA mode to auto
            self.e4k_reg_set_mask(REG_AGC1, AGC1_MOD_MASK, AGC_MOD_IF_SERIAL_LNA_AUTON);
            // Set Mixer Gain Control to auto
            self.e4k_reg_set_mask(REG_AGC7, AGC7_MIX_GAIN_AUTO, 1);
            self.e4k_reg_set_mask(REG_AGC11, 0x7, 0);
        }
        0
    }

    /// Index of `val` in the gain table of IF stage `stage`, or -EINVAL.
    fn find_stage_gain(stage: u8, val: i8) -> i32 {
        let arr: &[i8] = match stage {
            1 => &IF_STAGE1_GAIN,
            2 | 3 => &IF_STAGE23_GAIN,
            4 => &IF_STAGE4_GAIN,
            5 | 6 => &IF_STAGE56_GAIN,
            _ => return -EINVAL,
        };
        for (i, &a) in arr.iter().enumerate() {
            if a == val {
                return i as i32;
            }
        }
        -EINVAL
    }

    /// Gain of one IF stage (1..6) in dB (`e4k_if_gain_set`).
    fn e4k_if_gain_set(&mut self, stage: u8, value: i8) -> i32 {
        let rc = Self::find_stage_gain(stage, value);
        if rc < 0 {
            return rc;
        }
        let f = &IF_STAGE_GAIN_REGS[stage as usize];
        let mask = WIDTH2MASK[f.2 as usize].wrapping_shl(f.1 as u32);
        self.e4k_reg_set_mask(f.0, mask, (rc as u8).wrapping_shl(f.1 as u32))
    }

    fn e4k_mixer_gain_set(&mut self, value: i8) -> i32 {
        let bit = match value {
            4 => 0,
            12 => 1,
            _ => return -EINVAL,
        };
        self.e4k_reg_set_mask(REG_GAIN2, 1, bit)
    }

    fn e4k_standby(&mut self, enable: bool) -> i32 {
        self.e4k_reg_set_mask(REG_MASTER1, MASTER1_NORM_STBY, if enable { 0 } else { MASTER1_NORM_STBY });
        0
    }

    fn e4k_init(&mut self) -> i32 {
        // make a dummy i2c read or write command, will not be ACKed!
        self.e4k_reg_read(0);
        // Make sure we reset everything and clear POR indicator
        self.e4k_reg_write(REG_MASTER1, MASTER1_RESET | MASTER1_NORM_STBY | MASTER1_POR_DET);
        // Configure clock input
        self.e4k_reg_write(REG_CLK_INP, 0x00);
        // Disable clock output
        self.e4k_reg_write(REG_REF_CLK, 0x00);
        self.e4k_reg_write(REG_CLKOUT_PWDN, 0x96);
        // Write some magic values into registers
        self.e4k_reg_write(0x7e, 0x01);
        self.e4k_reg_write(0x7f, 0xfe);
        self.e4k_reg_write(0x82, 0x00);
        self.e4k_reg_write(0x86, 0x50); // polarity A
        self.e4k_reg_write(0x87, 0x20);
        self.e4k_reg_write(0x88, 0x01);
        self.e4k_reg_write(0x9f, 0x7f);
        self.e4k_reg_write(0xa0, 0x07);
        // Set LNA mode to manual
        self.e4k_reg_write(REG_AGC4, 0x10); // High threshold
        self.e4k_reg_write(REG_AGC5, 0x04); // Low threshold
        self.e4k_reg_write(REG_AGC6, 0x1a); // LNA calib + loop rate
        self.e4k_reg_set_mask(REG_AGC1, AGC1_MOD_MASK, AGC_MOD_SERIAL);
        // Set Mixer Gain Control to manual
        self.e4k_reg_set_mask(REG_AGC7, AGC7_MIX_GAIN_AUTO, 0);
        // Use auto-gain as default
        self.e4k_enable_manual_gain(0);
        // Select moderate gain levels
        self.e4k_if_gain_set(1, 6);
        self.e4k_if_gain_set(2, 0);
        self.e4k_if_gain_set(3, 0);
        self.e4k_if_gain_set(4, 0);
        self.e4k_if_gain_set(5, 9);
        self.e4k_if_gain_set(6, 9);
        // Set the most narrow filter we can possibly use
        self.e4k_if_filter_bw_set(IfFilter::Mix, 1_900_000);
        self.e4k_if_filter_bw_set(IfFilter::Rc, 1_000_000);
        self.e4k_if_filter_bw_set(IfFilter::Chan, 2_150_000);
        self.e4k_if_filter_chan_enable(true);
        // Disable time variant DC correction and LUT
        self.e4k_reg_set_mask(REG_DC5, 0x03, 0);
        self.e4k_reg_set_mask(REG_DCTIME1, 0x03, 0);
        self.e4k_reg_set_mask(REG_DCTIME2, 0x03, 0);
        0
    }

    // ---- the entries of the tuner table (`e4000_*` in librtlsdr.c) ---------------------------------------------------------------------------------

    pub(crate) fn e4000_init(&mut self) -> i32 {
        self.e4k.i2c_addr = E4K_I2C_ADDR;
        self.e4k.vco.fosc = self.get_xtal_freq().1;
        self.e4k_init()
    }

    pub(crate) fn e4000_exit(&mut self) -> i32 {
        self.e4k_standby(true)
    }

    pub(crate) fn e4000_set_freq(&mut self, freq: u32) -> i32 {
        self.e4k_tune_freq(freq)
    }

    pub(crate) fn e4000_set_bw(&mut self, bw: i32) -> i32 {
        let mut r = 0;
        r |= self.e4k_if_filter_bw_set(IfFilter::Mix, bw as u32);
        r |= self.e4k_if_filter_bw_set(IfFilter::Rc, bw as u32);
        r |= self.e4k_if_filter_bw_set(IfFilter::Chan, bw as u32);
        r
    }

    pub(crate) fn e4000_set_gain(&mut self, gain: i32) -> i32 {
        let mixgain: i32 = if gain > 340 { 12 } else { 4 };
        if self.e4k_set_lna_gain(300.min(gain.wrapping_sub(mixgain * 10))) == -EINVAL {
            return -1;
        }
        if self.e4k_mixer_gain_set(mixgain as i8) == -EINVAL {
            return -1;
        }
        0
    }

    pub(crate) fn e4000_set_if_gain(&mut self, stage: i32, gain: i32) -> i32 {
        self.e4k_if_gain_set(stage as u8, (gain / 10) as i8)
    }

    pub(crate) fn e4000_set_gain_mode(&mut self, manual: i32) -> i32 {
        self.e4k_enable_manual_gain(manual as u8)
    }
}
