//! Breathing and heart rate from CSI amplitude: a port of RuView's `wifi-densepose-vitals` (ADR-021; MIT License, Copyright (c) 2024 rUv,
//! https://github.com/ruvnet/RuView) to `no_std` + `alloc` without dependencies. The algorithms are RuView's: an EMA static-component
//! remover per subcarrier, a 2nd-order resonator bandpass, zero crossings for breathing (0.1 - 0.5 Hz) and a two-stage bandpass with the
//! autocorrelation peak for the heart rate (0.8 - 2.0 Hz, 40 - 180 BPM).

use crate::mathf::{ceil, cos, exp, floor, round, sqrt, PI};
use alloc::collections::VecDeque;
use alloc::vec;
use alloc::vec::Vec;

fn clamp(x: f64, lo: f64, hi: f64) -> f64 {
    if x < lo { lo } else if x > hi { hi } else { x }
}
fn fabs(x: f64) -> f64 {
    if x < 0.0 { -x } else { x }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    Valid,
    Degraded,
    Unreliable,
}

#[derive(Clone, Copy, Debug)]
pub struct Estimate {
    pub bpm: f64,
    pub confidence: f64,
    pub status: Status,
}

/// RuView `CsiVitalPreprocessor`: residual = amplitude - EMA prediction (the static room removed).
pub struct Preprocessor {
    predictions: Vec<f64>,
    initialized: Vec<bool>,
    alpha: f64,
}

impl Preprocessor {
    pub fn new(n_subcarriers: usize, alpha: f64) -> Self {
        Preprocessor { predictions: vec![0.0; n_subcarriers], initialized: vec![false; n_subcarriers], alpha: clamp(alpha, 0.001, 0.999) }
    }
    pub fn process(&mut self, amplitudes: &[f64]) -> Vec<f64> {
        let n = amplitudes.len().min(self.predictions.len());
        let mut residuals = vec![0.0; n];
        for i in 0..n {
            if self.initialized[i] {
                residuals[i] = amplitudes[i] - self.predictions[i];
                self.predictions[i] = self.alpha * amplitudes[i] + (1.0 - self.alpha) * self.predictions[i];
            } else {
                self.predictions[i] = amplitudes[i];
                self.initialized[i] = true;
            }
        }
        residuals
    }
}

#[derive(Clone, Copy, Default, Debug)]
struct Iir {
    x1: f64,
    x2: f64,
    y1: f64,
    y2: f64,
}

/// One resonator step; None (and the state reset) when it is not finite (RuView ADR-158 self-healing guard).
fn resonate(s: &mut Iir, input: f64, r: f64, cos_w0: f64) -> Option<f64> {
    let out = (1.0 - r) * (input - s.x2) + 2.0 * r * cos_w0 * s.y1 - r * r * s.y2;
    if !out.is_finite() {
        *s = Iir::default();
        return None;
    }
    s.x2 = s.x1;
    s.x1 = input;
    s.y2 = s.y1;
    s.y1 = out;
    Some(out)
}

fn band(fs: f64, lo: f64, hi: f64) -> (f64, f64) {
    let (wl, wh) = (2.0 * PI * lo / fs, 2.0 * PI * hi / fs);
    (clamp(1.0 - (wh - wl) / 2.0, 0.0, 0.9999), cos((wl + wh) / 2.0))
}

/// RuView `BreathingExtractor`.
pub struct Breathing {
    hist: VecDeque<f64>,
    fs: f64,
    window: f64,
    lo: f64,
    hi: f64,
    iir: Iir,
}

impl Breathing {
    pub fn new(fs: f64, window_secs: f64) -> Self {
        Breathing { hist: VecDeque::new(), fs, window: window_secs, lo: 0.1, hi: 0.5, iir: Iir::default() }
    }
    pub fn reset(&mut self) {
        self.hist.clear();
        self.iir = Iir::default();
    }
    pub fn extract(&mut self, residuals: &[f64]) -> Option<Estimate> {
        let n = residuals.len();
        if n == 0 {
            return None;
        }
        let x = residuals.iter().sum::<f64>() / n as f64;   // uniform weights: RuView's fuse_weighted_residuals without weights
        let (r, c) = band(self.fs, self.lo, self.hi);
        let y = resonate(&mut self.iir, x, r, c)?;
        self.hist.push_back(y);
        while self.hist.len() > (self.fs * self.window) as usize {
            self.hist.pop_front();
        }
        if self.hist.len() < (self.fs * 10.0) as usize {
            return None;
        }
        let h = self.hist.make_contiguous();
        let crossings = h.windows(2).filter(|w| w[0] * w[1] < 0.0).count();
        let f = crossings as f64 / (2.0 * (h.len() as f64 / self.fs));
        if f < self.lo || f > self.hi {
            self.reset();   // reject and forget (RuView issue #1422)
            return None;
        }
        let confidence = breathing_confidence(self.hist.make_contiguous(), f, self.fs);
        let status = if confidence >= 0.7 { Status::Valid } else if confidence >= 0.4 { Status::Degraded } else { Status::Unreliable };
        Some(Estimate { bpm: f * 60.0, confidence, status })
    }
}

fn breathing_confidence(h: &[f64], f: f64, fs: f64) -> f64 {
    if h.len() < 4 || f.is_nan() || fs.is_nan() || f <= 0.0 || fs <= 0.0 {
        return 0.0;
    }
    let period = round(fs / f) as usize;
    if period == 0 || period >= h.len().saturating_sub(1) {
        return 0.0;
    }
    let overlap = h.len() - period;
    let (a, b) = (&h[..overlap], &h[period..]);
    let (ma, mb) = (a.iter().sum::<f64>() / overlap as f64, b.iter().sum::<f64>() / overlap as f64);
    let (mut cov, mut ea, mut eb) = (0.0, 0.0, 0.0);
    for (&x, &y) in a.iter().zip(b) {
        let (cx, cy) = (x - ma, y - mb);
        cov += cx * cy;
        ea += cx * cx;
        eb += cy * cy;
    }
    let den = sqrt(ea * eb);
    if den < 1e-15 {
        return 0.0;
    }
    let regularity = clamp(cov / den, 0.0, 1.0);
    let coverage = clamp(h.len() as f64 / period as f64 - 1.0, 0.0, 1.0);
    clamp(regularity * coverage, 0.0, 1.0)
}

/// RuView `HeartRateExtractor`.
pub struct HeartRate {
    hist: VecDeque<f64>,
    single: VecDeque<f64>,
    fs: f64,
    window: f64,
    lo: f64,
    hi: f64,
    iir: [Iir; 2],
}

const MIN_SUBCARRIERS: usize = 4;

impl HeartRate {
    pub fn new(fs: f64, window_secs: f64) -> Self {
        HeartRate { hist: VecDeque::new(), single: VecDeque::new(), fs, window: window_secs, lo: 0.8, hi: 2.0, iir: [Iir::default(); 2] }
    }
    pub fn extract(&mut self, residuals: &[f64], phases: &[f64]) -> Option<Estimate> {
        let n = residuals.len();
        if n == 0 {
            return None;
        }
        let x = phase_coherence(residuals, phases);
        let (r, c) = band(self.fs, self.lo, self.hi);
        let Some(s1) = resonate(&mut self.iir[0], x, r, c) else { self.iir = [Iir::default(); 2]; return None };
        let Some(s2) = resonate(&mut self.iir[1], s1, r, c) else { self.iir = [Iir::default(); 2]; return None };
        self.hist.push_back(s2);
        self.single.push_back(s1);
        while self.hist.len() > (self.fs * self.window) as usize {
            self.hist.pop_front();
            self.single.pop_front();
        }
        if self.hist.len() < (self.fs * 5.0) as usize {
            return None;
        }
        let (period, peak) = acf_peak(self.hist.make_contiguous(), self.fs, self.lo, self.hi);
        if period == 0 {
            return None;
        }
        let corroboration = acf_at(self.single.make_contiguous(), period);
        let peak = peak.min(corroboration.max(0.0));
        let bpm = self.fs / period as f64 * 60.0;
        if !(40.0..=180.0).contains(&bpm) {
            return None;
        }
        let sub = if n >= MIN_SUBCARRIERS { 1.0 } else { n as f64 / MIN_SUBCARRIERS as f64 };
        let confidence = clamp(peak * sub, 0.0, 1.0);
        let status = if confidence >= 0.6 && n >= MIN_SUBCARRIERS { Status::Valid } else if confidence >= 0.3 { Status::Degraded } else { Status::Unreliable };
        Some(Estimate { bpm, confidence, status })
    }
}

fn phase_coherence(residuals: &[f64], phases: &[f64]) -> f64 {
    let n = residuals.len();
    if n <= 1 {
        return residuals.first().copied().unwrap_or(0.0);
    }
    let (mut sum, mut total) = (0.0, 0.0);
    for (i, &r) in residuals.iter().enumerate() {
        let j = if i + 1 < n { i + 1 } else { i - 1 };
        let coh = match (phases.get(i), phases.get(j)) {
            (Some(a), Some(b)) => exp(-fabs(b - a)),
            _ => 1.0,
        };
        sum += r * coh;
        total += coh;
    }
    if total > 1e-15 { sum / total } else { 0.0 }
}

fn acf_at(s: &[f64], lag: usize) -> f64 {
    if s.is_empty() || lag >= s.len() {
        return 0.0;
    }
    let m = s.iter().sum::<f64>() / s.len() as f64;
    let e: f64 = s.iter().map(|x| (x - m) * (x - m)).sum();
    if !e.is_finite() || e < 1e-15 {
        return 0.0;
    }
    s.iter().zip(&s[lag..]).map(|(x, y)| (x - m) * (y - m)).sum::<f64>() / e
}

fn acf_peak(s: &[f64], fs: f64, lo: f64, hi: f64) -> (usize, f64) {
    let n = s.len();
    if n < 4 {
        return (0, 0.0);
    }
    let min_lag = floor(fs / hi) as usize;
    let max_lag = (ceil(fs / lo) as usize).min(n / 2);
    if min_lag == 0 || min_lag >= max_lag || min_lag >= n {
        return (0, 0.0);
    }
    let m = s.iter().sum::<f64>() / n as f64;
    let acf0: f64 = s.iter().map(|&x| (x - m) * (x - m)).sum();
    if !acf0.is_finite() || acf0 < 1e-15 {
        return (0, 0.0);
    }
    let acf = |lag: usize| s.iter().take(n - lag).enumerate().map(|(i, &x)| (x - m) * (s[i + lag] - m)).sum::<f64>() / acf0;
    let (mut best_lag, mut best) = (0, 0.0);
    let (mut prev, mut cur) = (acf(min_lag - 1), acf(min_lag));
    for lag in min_lag..=max_lag {
        let next = acf(lag + 1);
        if cur > prev && cur >= next && cur > best {
            best = cur;
            best_lag = lag;
        }
        prev = cur;
        cur = next;
    }
    if best > 0.0 { (best_lag, best) } else { (0, 0.0) }
}
