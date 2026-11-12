//! SatDump's Rust parts (fork). C++ reaches them only through `ffi` and the functions in
//! `rust/bridge`; see `ffi` for the rules every exported function follows.

#![deny(unsafe_op_in_unsafe_fn)]
#![warn(missing_docs)]

pub mod derand;
pub mod ffi;
