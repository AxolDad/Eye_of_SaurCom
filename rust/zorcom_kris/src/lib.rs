//! Eye of ZorCom Kris modes (KAVB, KAPP, KAFT, KAAL): the shared channel codes. A family of systematic quasi-cyclic IRA LDPC codes (n = 1920, 2304, 7680; rates 1/4 .. 9/10,
//! dual-diagonal parity, linear-time encoder, layered normalised min-sum decoder) and the K = 7 convolutional code (rate 1/2 and 1/4) with a soft Viterbi decoder.
//! `no_std` with `alloc` (the static library carries no Rust `std`); the allocator is libc malloc. Only `ffi` and `heap` may use `unsafe`.
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]
#![allow(clippy::needless_range_loop)]

extern crate alloc;

pub mod conv;
pub mod erasure;
pub mod ldpc;
pub mod xorshift;

#[allow(unsafe_code)]
pub mod ffi;

/// libc malloc as the global allocator (the code needs the heap for the cached code tables and decoder scratch). Not used by the unit tests, which run with `std`.
#[cfg(not(test))]
#[allow(unsafe_code)]
mod heap {
    use core::alloc::{GlobalAlloc, Layout};
    extern "C" {
        fn malloc(n: usize) -> *mut u8;
        fn calloc(n: usize, m: usize) -> *mut u8;
        fn realloc(p: *mut u8, n: usize) -> *mut u8;
        fn free(p: *mut u8);
    }
    pub struct Libc;
    // malloc returns memory aligned for any fundamental type (16 bytes on the 64-bit targets). A larger alignment (std's own cache-padded
    // types ask for 64 or 128 when this allocator serves a std program, e.g. the umbrella crate's tests) is over-allocated: the block
    // malloc gave is kept in the word just before the aligned pointer.
    const MALLOC_ALIGN: usize = 16;
    unsafe fn alloc_big(l: Layout, zero: bool) -> *mut u8 {
        let w = core::mem::size_of::<usize>();
        let Some(n) = l.size().checked_add(l.align() + w) else { return core::ptr::null_mut() };
        let raw = if zero { calloc(n, 1) } else { malloc(n) };
        if raw.is_null() {
            return raw;
        }
        let p = ((raw as usize + w + l.align() - 1) & !(l.align() - 1)) as *mut u8;
        (p as *mut usize).sub(1).write_unaligned(raw as usize);
        p
    }
    unsafe impl GlobalAlloc for Libc {
        unsafe fn alloc(&self, l: Layout) -> *mut u8 {
            if l.align() > MALLOC_ALIGN {
                return alloc_big(l, false);
            }
            malloc(l.size())
        }
        unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
            if l.align() > MALLOC_ALIGN {
                return alloc_big(l, true);
            }
            calloc(l.size(), 1)
        }
        unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
            if l.align() > MALLOC_ALIGN {
                free((p as *mut usize).sub(1).read_unaligned() as *mut u8);
            } else {
                free(p)
            }
        }
        unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
            if l.align() <= MALLOC_ALIGN {
                return realloc(p, n);
            }
            let Ok(nl) = Layout::from_size_align(n, l.align()) else { return core::ptr::null_mut() };
            let q = alloc_big(nl, false);
            if !q.is_null() {
                core::ptr::copy_nonoverlapping(p, q, l.size().min(n));
                self.dealloc(p, l);
            }
            q
        }
    }
    #[global_allocator]
    static A: Libc = Libc;
}
