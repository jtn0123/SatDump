//! SatDump's Rust parts (fork). C++ reaches them only through `ffi` and the functions in
//! `rust/bridge`; see `ffi` for the rules every exported function follows.

#![deny(unsafe_op_in_unsafe_fn)]
#![warn(missing_docs)]

pub mod derand;
pub mod ffi;

// Fork part "aos": the CCSDS AOS demuxer and its C functions
pub mod aos;
pub mod ffi_aos;
// LRPT (M6.3): the Meteor LRPT MSU-MR segment decoder and its C functions
pub mod ffi_lrpt;
pub mod lrpt;
