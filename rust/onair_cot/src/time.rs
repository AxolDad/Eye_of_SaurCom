//! CoT times: UTC, ISO 8601 with milliseconds and a `Z` (`2026-10-23T12:34:56.789Z`). Days from the civil date by H. Hinnant's algorithm.

use crate::Buf;
use core::fmt::Write;

fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = y.div_euclid(400);
    let yoe = y - era * 400;
    let mp = (m + 9) % 12;
    let doy = (153 * mp + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

fn civil_from_days(z: i64) -> (i64, i64, i64) {
    let z = z + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    (yoe + era * 400 + i64::from(m <= 2), m, d)
}

/// `ms` since 1970-01-01 UTC as CoT time text.
pub fn write_iso(w: &mut Buf, ms: i64) -> core::fmt::Result {
    let (secs, milli) = (ms.div_euclid(1000), ms.rem_euclid(1000));
    let (days, sod) = (secs.div_euclid(86_400), secs.rem_euclid(86_400));
    let (y, mo, d) = civil_from_days(days);
    write!(w, "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", y, mo, d, sod / 3600, sod / 60 % 60, sod % 60, milli)
}

fn digits(s: &str) -> Option<i64> {
    if s.is_empty() || !s.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    s.parse().ok()
}

/// CoT time text (fraction optional, any number of fraction digits, `Z` required) as ms since 1970; None when malformed.
pub fn parse_iso(s: &str) -> Option<i64> {
    let s = s.trim().strip_suffix('Z')?;
    let (date, clock) = s.split_once('T')?;
    let mut dp = date.splitn(3, '-');
    let (y, mo, d) = (digits(dp.next()?)?, digits(dp.next()?)?, digits(dp.next()?)?);
    let (hms, frac) = clock.split_once('.').unwrap_or((clock, ""));
    let mut tp = hms.splitn(3, ':');
    let (h, mi, sec) = (digits(tp.next()?)?, digits(tp.next()?)?, digits(tp.next()?)?);
    if !(1..=12).contains(&mo) || !(1..=31).contains(&d) || h > 23 || mi > 59 || sec > 60 {
        return None;
    }
    let mut milli = 0i64;
    for (i, b) in frac.bytes().enumerate() {
        if !b.is_ascii_digit() {
            return None;
        }
        if i < 3 {
            milli = milli * 10 + i64::from(b - b'0');
        }
    }
    for _ in frac.len()..3 {
        milli *= 10;
    }
    Some(((days_from_civil(y, mo, d) * 86_400) + h * 3600 + mi * 60 + sec) * 1000 + milli)
}
