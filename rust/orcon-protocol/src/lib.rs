#![no_std]
//! Hardware-independent RAMSES-II / Orcon 15RF protocol.
//!
//! The original implementation is src/ramses_codec.{h,cpp}. This is a
//! standalone Rust implementation: no Arduino dependency and no RF side effects.
//! The existing firmware remains untouched during the migration.

pub const PREAMBLE_BYTE: u8 = 0x55;
pub const STOP_WIRE: u8 = 0x35;
pub const SYNC: [u8; 5] = [0xFF, 0x00, 0x33, 0x55, 0x53];
pub const MANCH_ENC: [u8; 16] = [
    0xAA, 0xA9, 0xA6, 0xA5, 0x9A, 0x99, 0x96, 0x95,
    0x6A, 0x69, 0x66, 0x65, 0x5A, 0x59, 0x56, 0x55,
];
const HEADER_FLAGS: [u8; 16] = [
    0x0F, 0x0C, 0x0D, 0x0B, 0x27, 0x24, 0x25, 0x23,
    0x47, 0x44, 0x45, 0x43, 0x17, 0x14, 0x15, 0x13,
];
pub const MAX_PAYLOAD: usize = 64;
pub const MAX_LOGICAL: usize = 96;
pub const MAX_WIRE: usize = 5 + 2 * MAX_LOGICAL + 1;
pub const MAX_ONAIR: usize = 32 + (MAX_WIRE + 8) * 2;

const ADDR0: u8 = 0x01;
const ADDR1: u8 = 0x02;
const ADDR2: u8 = 0x04;
const RQ: u8 = 0x08;
const RP: u8 = 0x10;
const I: u8 = 0x20;
const W: u8 = 0x40;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MsgType { I, Rq, Rp, W }

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct DeviceId {
    pub class: u8,
    pub serial: u32,
}
impl DeviceId {
    pub fn raw(self) -> u32 {
        (u32::from(self.class & 0x3F) << 18) | (self.serial & 0x3FFFF)
    }
    pub fn from_raw(value: u32) -> Self {
        Self { class: ((value >> 18) & 0x3F) as u8, serial: value & 0x3FFFF }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Frame {
    pub kind: MsgType,
    pub has_addr: [bool; 3],
    pub addr: [DeviceId; 3],
    pub has_param0: bool,
    pub has_param1: bool,
    pub param0: u8,
    pub param1: u8,
    pub cmd: u16,
    pub len: u8,
    pub payload: [u8; MAX_PAYLOAD],
}
impl Default for Frame {
    fn default() -> Self {
        Self {
            kind: MsgType::I,
            has_addr: [true, true, true],
            addr: [DeviceId::default(); 3],
            has_param0: false,
            has_param1: false,
            param0: 0,
            param1: 0,
            cmd: 0,
            len: 0,
            payload: [0; MAX_PAYLOAD],
        }
    }
}

pub fn header_for_type(
    kind: MsgType, a0: bool, a1: bool, a2: bool, p0: bool, p1: bool,
) -> u8 {
    let mut flags = match kind {
        MsgType::I => I, MsgType::Rq => RQ, MsgType::Rp => RP, MsgType::W => W,
    };
    if a0 { flags |= ADDR0; }
    if a1 { flags |= ADDR1; }
    if a2 { flags |= ADDR2; }
    let index = HEADER_FLAGS.iter().position(|&x| x == flags).unwrap_or(4);
    let mut header = (index as u8) << 2;
    if p0 { header |= 2; }
    if p1 { header |= 1; }
    header
}

fn put(buf: &mut [u8], n: &mut usize, byte: u8) -> Option<()> {
    if *n >= buf.len() { return None; }
    buf[*n] = byte;
    *n += 1;
    Some(())
}

/// Encode a frame into logical bytes, including the zero-sum checksum.
/// Returns the written byte count, or None on invalid lengths / small buffer.
pub fn build_logical(frame: &Frame, out: &mut [u8]) -> Option<usize> {
    let payload_len = usize::from(frame.len);
    if payload_len > MAX_PAYLOAD { return None; }
    let mut tmp = [0u8; MAX_LOGICAL];
    let mut n = 0;
    put(&mut tmp, &mut n, header_for_type(
        frame.kind, frame.has_addr[0], frame.has_addr[1], frame.has_addr[2],
        frame.has_param0, frame.has_param1,
    ))?;
    for index in 0..3 {
        if !frame.has_addr[index] { continue; }
        let raw = frame.addr[index].raw();
        put(&mut tmp, &mut n, (raw >> 16) as u8)?;
        put(&mut tmp, &mut n, (raw >> 8) as u8)?;
        put(&mut tmp, &mut n, raw as u8)?;
    }
    if frame.has_param0 { put(&mut tmp, &mut n, frame.param0)?; }
    if frame.has_param1 { put(&mut tmp, &mut n, frame.param1)?; }
    put(&mut tmp, &mut n, (frame.cmd >> 8) as u8)?;
    put(&mut tmp, &mut n, frame.cmd as u8)?;
    put(&mut tmp, &mut n, frame.len)?;
    for &byte in &frame.payload[..payload_len] {
        put(&mut tmp, &mut n, byte)?;
    }
    let sum = tmp[..n].iter().fold(0u8, |acc, &b| acc.wrapping_add(b));
    put(&mut tmp, &mut n, 0u8.wrapping_sub(sum))?;
    if n > out.len() { return None; }
    out[..n].copy_from_slice(&tmp[..n]);
    Some(n)
}

/// Parse a logical packet only if checksum, address lengths and payload length match.
pub fn parse_logical(data: &[u8]) -> Option<Frame> {
    if data.len() < 5 || data.iter().fold(0u8, |acc, &b| acc.wrapping_add(b)) != 0 {
        return None;
    }
    let mut p = 0;
    let header = data[p]; p += 1;
    let flags = HEADER_FLAGS[((header >> 2) & 15) as usize];
    let kind = if flags & I != 0 { MsgType::I }
        else if flags & RQ != 0 { MsgType::Rq }
        else if flags & RP != 0 { MsgType::Rp }
        else { MsgType::W };
    let mut frame = Frame {
        kind,
        has_addr: [flags & ADDR0 != 0, flags & ADDR1 != 0, flags & ADDR2 != 0],
        ..Frame::default()
    };
    for i in 0..3 {
        if !frame.has_addr[i] { continue; }
        let bytes = data.get(p..p.checked_add(3)?)?;
        let raw = (u32::from(bytes[0]) << 16)
            | (u32::from(bytes[1]) << 8) | u32::from(bytes[2]);
        frame.addr[i] = DeviceId::from_raw(raw);
        p += 3;
    }
    frame.has_param0 = header & 2 != 0;
    frame.has_param1 = header & 1 != 0;
    if frame.has_param0 { frame.param0 = *data.get(p)?; p += 1; }
    if frame.has_param1 { frame.param1 = *data.get(p)?; p += 1; }
    let head = data.get(p..p.checked_add(3)?)?;
    frame.cmd = u16::from_be_bytes([head[0], head[1]]);
    let len = usize::from(head[2]);
    if len > MAX_PAYLOAD { return None; }
    p += 3;
    if p.checked_add(len)?.checked_add(1)? != data.len() { return None; }
    frame.len = len as u8;
    frame.payload[..len].copy_from_slice(data.get(p..p + len)?);
    Some(frame)
}

fn manchester_index(byte: u8) -> Option<u8> {
    MANCH_ENC.iter().position(|&candidate| candidate == byte).map(|i| i as u8)
}

/// Full SYNC prefix, Manchester nibble pairs, STOP marker.
pub fn wire_from_logical(logical: &[u8], out: &mut [u8]) -> Option<usize> {
    if logical.len() > MAX_LOGICAL { return None; }
    let n = SYNC.len().checked_add(logical.len().checked_mul(2)?)?.checked_add(1)?;
    if n > out.len() { return None; }
    out[..SYNC.len()].copy_from_slice(&SYNC);
    let mut p = SYNC.len();
    for &byte in logical {
        out[p] = MANCH_ENC[usize::from(byte >> 4)];
        out[p + 1] = MANCH_ENC[usize::from(byte & 15)];
        p += 2;
    }
    out[p] = STOP_WIRE;
    Some(n)
}

/// Mirrors original codec's stop/short-input behavior. Call parse_logical to
/// validate packet structure, including exact logical length.
pub fn logical_from_wire(wire: &[u8], out: &mut [u8]) -> Option<usize> {
    if wire.len() < 6 || !wire.starts_with(&SYNC) { return None; }
    let mut p = 5;
    let mut n = 0;
    while p + 1 < wire.len() {
        if wire[p] == STOP_WIRE { break; }
        let hi = manchester_index(wire[p])?;
        let lo = manchester_index(wire[p + 1])?;
        put(out, &mut n, (hi << 4) | lo)?;
        p += 2;
    }
    Some(n)
}

/// A complete decoded frame and stream progress from a UART byte stream.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Extracted {
    pub frame: Option<Frame>,
    pub consumed: usize,
    /// Offset of the 0x00 sync byte (the leading 0xFF may be lost on receive).
    pub sync_at: Option<usize>,
}

/// Scan the UART byte stream for SYNC[1..4], preserving incomplete tails.
pub fn extract_wire_frame(stream: &[u8]) -> Extracted {
    let partial = stream.len().saturating_sub(4);
    for i in 0..=stream.len().saturating_sub(4) {
        if stream.get(i..i + 4) != Some(&SYNC[1..]) { continue; }
        let mut logical = [0u8; MAX_LOGICAL];
        let (mut p, mut n) = (i + 4, 0usize);
        let mut garbled = false;
        loop {
            if p >= stream.len() {
                return Extracted { frame: None, consumed: i, sync_at: None };
            }
            if stream[p] == STOP_WIRE { break; }
            if n >= MAX_LOGICAL {
                garbled = true;
                break;
            }
            let Some(hi) = manchester_index(stream[p]) else {
                garbled = true;
                break;
            };
            if p + 1 >= stream.len() {
                return Extracted { frame: None, consumed: i, sync_at: None };
            }
            let Some(lo) = manchester_index(stream[p + 1]) else {
                garbled = true;
                break;
            };
            logical[n] = (hi << 4) | lo;
            n += 1;
            p += 2;
        }
        if !garbled {
            if let Some(frame) = parse_logical(&logical[..n]) {
                return Extracted {
                    frame: Some(frame), consumed: p + 1, sync_at: Some(i),
                };
            }
        }
    }
    Extracted { frame: None, consumed: partial, sync_at: None }
}

/// A packed on-air bitstream. bit_len counts padding too, matching C++.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Encoded {
    pub byte_len: usize,
    pub bit_len: usize,
}

fn set_bit(buf: &mut [u8], bit: usize, one: bool) -> Option<()> {
    let byte = buf.get_mut(bit >> 3)?;
    let mask = 1 << (7 - (bit & 7));
    if one { *byte |= mask; } else { *byte &= !mask; }
    Some(())
}
fn get_bit(buf: &[u8], bit: usize) -> Option<bool> {
    Some((buf.get(bit >> 3)? >> (7 - (bit & 7))) & 1 != 0)
}
fn write_framed(buf: &mut [u8], bits: &mut usize, v: u8) -> Option<()> {
    set_bit(buf, *bits, false)?; *bits += 1;
    for i in 0..8 {
        set_bit(buf, *bits, (v >> i) & 1 != 0)?;
        *bits += 1;
    }
    set_bit(buf, *bits, true)?; *bits += 1;
    Some(())
}

pub fn encode_on_air(wire: &[u8], out: &mut [u8], preamble_bytes: usize) -> Option<Encoded> {
    let bits = (preamble_bytes.checked_add(wire.len())?).checked_mul(10)?;
    let padded = bits.checked_add(7)? & !7;
    if padded / 8 > out.len() { return None; }
    out.fill(0);
    let mut cursor = 0;
    for _ in 0..preamble_bytes { write_framed(out, &mut cursor, PREAMBLE_BYTE)?; }
    for &b in wire { write_framed(out, &mut cursor, b)?; }
    while cursor < padded { set_bit(out, cursor, true)?; cursor += 1; }
    Some(Encoded { byte_len: cursor / 8, bit_len: cursor })
}

fn deframe_at(buf: &[u8], total_bits: usize, offset: usize) -> Option<u8> {
    if offset.checked_add(10)? > total_bits
        || get_bit(buf, offset)?
        || !get_bit(buf, offset + 9)? {
        return None;
    }
    let mut byte = 0;
    for i in 0..8 {
        if get_bit(buf, offset + i + 1)? { byte |= 1 << i; }
    }
    Some(byte)
}

/// Find a framed 5-byte SYNC at any bit offset and recover subsequent UART bytes.
pub fn decode_on_air(data: &[u8], n_bits: usize, out: &mut [u8]) -> Option<usize> {
    let total_bits = if n_bits == 0 { data.len().checked_mul(8)? } else { n_bits };
    if total_bits > data.len().checked_mul(8)? { return None; }
    let mut sync_buf = [0u8; 8];
    let mut sync_bits = 0;
    for &byte in &SYNC { write_framed(&mut sync_buf, &mut sync_bits, byte)?; }
    for start in 0..=total_bits.saturating_sub(sync_bits) {
        if start + sync_bits > total_bits { break; }
        if !(0..sync_bits).all(|b| get_bit(data, start + b) == get_bit(&sync_buf, b)) {
            continue;
        }
        if out.len() < SYNC.len() { return None; }
        out[..SYNC.len()].copy_from_slice(&SYNC);
        let mut n = SYNC.len();
        let mut cursor = start + sync_bits;
        while cursor + 10 <= total_bits {
            let Some(byte) = deframe_at(data, total_bits, cursor) else { break; };
            if n >= out.len() { return None; }
            out[n] = byte;
            n += 1;
            cursor += 10;
            if byte == STOP_WIRE { return Some(n); }
        }
        return Some(n);
    }
    None
}

pub fn encode_frame(frame: &Frame, out: &mut [u8], preamble_bytes: usize) -> Option<Encoded> {
    let mut logical = [0u8; MAX_LOGICAL];
    let logical_len = build_logical(frame, &mut logical)?;
    let mut wire = [0u8; MAX_WIRE];
    let wire_len = wire_from_logical(&logical[..logical_len], &mut wire)?;
    encode_on_air(&wire[..wire_len], out, preamble_bytes)
}

pub fn decode_frame(data: &[u8], n_bits: usize) -> Option<Frame> {
    let mut wire = [0u8; MAX_WIRE];
    let wire_len = decode_on_air(data, n_bits, &mut wire)?;
    let mut logical = [0u8; MAX_LOGICAL];
    let logical_len = logical_from_wire(&wire[..wire_len], &mut logical)?;
    parse_logical(&logical[..logical_len])
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fan(mode: u8) -> Frame {
        let mut f = Frame {
            addr: [
                DeviceId { class: 0x1D, serial: 0x012345 },
                DeviceId { class: 0x20, serial: 0x02BCDE },
                DeviceId { class: 0x1D, serial: 0x012345 },
            ],
            cmd: 0x22F1,
            len: 3,
            ..Frame::default()
        };
        f.payload[..3].copy_from_slice(&[0x00, mode, 0x07]);
        f
    }

    #[test]
    fn logical_checksum_and_structure() {
        let f = fan(3);
        let mut bytes = [0; MAX_LOGICAL];
        let n = build_logical(&f, &mut bytes).unwrap();
        assert_eq!(bytes[..n].iter().fold(0u8, |a, &b| a.wrapping_add(b)), 0);
        assert_eq!(parse_logical(&bytes[..n]), Some(f));
        bytes[2] ^= 0xFF;
        assert!(parse_logical(&bytes[..n]).is_none());
        bytes[2] ^= 0xFF;
        assert!(parse_logical(&bytes[..n - 1]).is_none());
        assert!(parse_logical(&bytes[..n + 1]).is_none());
    }

    #[test]
    fn all_message_types() {
        for kind in [MsgType::I, MsgType::Rq, MsgType::Rp, MsgType::W] {
            let f = Frame { kind, ..fan(1) };
            let mut out = [0; MAX_LOGICAL];
            let n = build_logical(&f, &mut out).unwrap();
            assert_eq!(parse_logical(&out[..n]), Some(f));
        }
    }

    #[test]
    fn manchester_unique_and_stop_reserved() {
        for (i, &a) in MANCH_ENC.iter().enumerate() {
            assert_ne!(a, STOP_WIRE);
            for &b in &MANCH_ENC[i + 1..] { assert_ne!(a, b); }
        }
    }

    #[test]
    fn full_roundtrip_different_frames_and_preambles() {
        let cases: &[(u16, &[u8])] = &[
            (0x22F1, &[0, 3, 7]),
            (0x22F1, &[0, 4, 7]),
            (0x1298, &[0, 3, 0x20]),
            (0x31E0, &[0, 0, 0, 1, 0, 0x1E, 0, 0]),
        ];
        for &(cmd, payload) in cases {
            let mut f = fan(2);
            f.cmd = cmd;
            f.len = payload.len() as u8;
            f.payload[..payload.len()].copy_from_slice(payload);
            for preamble in 3..=10 {
                let mut out = [0; MAX_ONAIR];
                let written = encode_frame(&f, &mut out, preamble).unwrap();
                assert_eq!(decode_frame(&out[..written.byte_len], written.bit_len), Some(f));
            }
        }
    }

    #[test]
    fn uart_stream_can_lose_leading_ff_sync() {
        let f = fan(3);
        let mut logical = [0; MAX_LOGICAL];
        let n = build_logical(&f, &mut logical).unwrap();
        let mut wire = [0; MAX_WIRE];
        let w = wire_from_logical(&logical[..n], &mut wire).unwrap();
        let mut received = [0u8; MAX_WIRE + 6];
        received[0] = 0x55;
        received[1] = 0x55;
        received[2..2 + w - 1].copy_from_slice(&wire[1..w]);
        let result = extract_wire_frame(&received[..2 + w - 1]);
        assert_eq!(result.frame, Some(f));
        assert_eq!(result.sync_at, Some(2));
        assert_eq!(result.consumed, 2 + w - 1);
    }

    #[test]
    fn rejects_overlong_payload_and_tiny_buffers() {
        let f = Frame { len: 65, ..fan(1) };
        assert!(build_logical(&f, &mut [0u8; MAX_LOGICAL]).is_none());
        assert!(build_logical(&fan(1), &mut [0u8; 3]).is_none());
    }
}
