//! PRX Type-A and Type-B relocation application.
//!
//! Reference: V1 reloc.py + PPSSPP Core/ELF/ElfReader.cpp
//! Type-A: 8-byte entries, segment-relative. HI16 must pair with subsequent LO16.
//! Type-B: Variable-length varint command stream from PT_PSPREL2 segment.

use crate::errors::ParseError;
use crate::types::RelocEntry;
use byteorder::{LittleEndian, ReadBytesExt};
use std::collections::BTreeMap;
use std::io::Cursor;

// MIPS relocation type constants
pub const R_MIPS_NONE: u8 = 0;
pub const R_MIPS_32: u8 = 2;
pub const R_MIPS_26: u8 = 4;
pub const R_MIPS_HI16: u8 = 5;
pub const R_MIPS_LO16: u8 = 6;

/// Relocation types this module knows how to apply.
///
/// `R_MIPS_NONE` is a defined no-op; `R_MIPS_HI16` is applied only when paired
/// with a following `R_MIPS_LO16` (an unpaired HI16 is counted as unhandled).
pub const HANDLED_RELOC_TYPES: [u8; 5] =
    [R_MIPS_NONE, R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16];

/// Returns true if `r_type` has an implementation in [`apply_relocations`].
///
/// Used by the recompile report to recompute unhandled-relocation counts from
/// the `relocations` array persisted in analysis.json (single source of truth
/// for what "handled" means).
pub fn is_handled_reloc_type(r_type: u8) -> bool {
    HANDLED_RELOC_TYPES.contains(&r_type)
}

/// Outcome statistics from one [`apply_relocations`] run.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct RelocStats {
    /// Entries applied, including `R_MIPS_NONE` no-ops and HI16/LO16 pairs.
    pub handled: u64,
    /// `r_type` -> count of entries whose target word was silently left
    /// untouched because the type has no implementation.
    pub unhandled: BTreeMap<u8, u64>,
}

/// Apply all relocations (Type-A then Type-B) to the mutable segment data slices.
///
/// `segments_data`: mutable references to each segment's byte data, indexed by segment number.
/// `segment_bases`: the virtual base address of each segment.
/// `type_a_entries`: parsed Type-A relocation entries.
/// `type_b_data`: raw bytes of the PT_PSPREL2 segment (may be empty if no Type-B).
///
/// Returns per-type statistics. Any relocation type without an implementation
/// leaves the target word untouched; each such type is reported with a single
/// aggregate `tracing::warn!` (once per type, with its count) and surfaced in
/// [`RelocStats::unhandled`] so the recompile report can carry the counts.
pub fn apply_relocations(
    segments_data: &mut [Vec<u8>],
    segment_bases: &[u32],
    type_a_entries: &[RelocEntry],
    type_b_data: &[u8],
) -> Result<RelocStats, ParseError> {
    let mut stats = RelocStats::default();
    if !type_a_entries.is_empty() {
        apply_type_a_inner(segments_data, type_a_entries, segment_bases, &mut stats);
    }
    if !type_b_data.is_empty() {
        let type_b_entries = parse_type_b_entries(type_b_data)?;
        apply_type_a_inner(segments_data, &type_b_entries, segment_bases, &mut stats);
    }
    for (&r_type, &count) in &stats.unhandled {
        tracing::warn!(
            "Unhandled relocation type {r_type}: {count} entries left untouched \
             (target words unchanged — addresses in affected code/data are unrelocated)"
        );
    }
    Ok(stats)
}

/// Parse Type-A relocation entries from raw 8-byte records.
///
/// Each entry: [offset:u32, r_info:u32] where r_info = (addr_base<<24)|(ofs_base<<16)|r_type
pub fn parse_type_a_entries(raw: &[u8]) -> Result<Vec<RelocEntry>, ParseError> {
    if raw.len() % 8 != 0 {
        return Err(ParseError::Reloc {
            message: format!("Type-A data length {} not multiple of 8", raw.len()),
        });
    }
    let mut cur = Cursor::new(raw);
    let mut entries = Vec::with_capacity(raw.len() / 8);
    while cur.position() < raw.len() as u64 {
        let offset = cur.read_u32::<LittleEndian>()?;
        let r_info = cur.read_u32::<LittleEndian>()?;
        entries.push(RelocEntry {
            offset,
            r_type: (r_info & 0xFF) as u8,
            ofs_base: ((r_info >> 16) & 0xFF) as u8,
            addr_base: ((r_info >> 24) & 0xFF) as u8,
        });
    }
    Ok(entries)
}

/// Parse Type-B variable-length command stream into RelocEntry list.
///
/// The stream begins with two lookup tables, then a command byte sequence.
/// Each command byte: high nibble = cmd type, low nibble = argument.
pub fn parse_type_b_entries(raw: &[u8]) -> Result<Vec<RelocEntry>, ParseError> {
    if raw.is_empty() {
        return Ok(vec![]);
    }
    let mut pos = 0usize;

    let part1_count = raw[pos] as usize;
    pos += 1;
    let part1 = &raw[pos..pos + part1_count];
    pos += part1_count;

    let part2_count = raw[pos] as usize;
    pos += 1;
    let part2 = &raw[pos..pos + part2_count];
    pos += part2_count;

    let mut entries = Vec::new();
    let mut current_seg: u8 = 0;
    let mut current_offset: u32 = 0;

    while pos < raw.len() {
        let byte = raw[pos];
        pos += 1;
        let cmd = (byte >> 4) & 0xF;
        let nibble = (byte & 0xF) as u32;

        match cmd {
            0 => {
                current_seg = 0;
                current_offset = 0;
            }
            1 => {
                current_seg = if (nibble as usize) < part1.len() {
                    part1[nibble as usize]
                } else {
                    0
                };
            }
            2 => {
                let advance = if nibble != 0 {
                    nibble * 4
                } else {
                    read_varint(raw, &mut pos)? * 4
                };
                current_offset = current_offset.wrapping_add(advance);
            }
            cmd_val => {
                // RELOCATE: advance offset by nibble*4, then emit entry
                current_offset = current_offset.wrapping_add(nibble * 4);
                let type_idx = (cmd_val - 3) as usize;
                let r_type = if type_idx < part2.len() { part2[type_idx] } else { 0 };
                entries.push(RelocEntry {
                    offset: current_offset,
                    r_type,
                    ofs_base: current_seg,
                    addr_base: 0,
                });
            }
        }
    }
    Ok(entries)
}

/// Sign-extend the low 16 bits of a u32 to i32.
fn sign_extend_16(value: u32) -> i32 {
    ((value & 0xFFFF) as i16) as i32
}

/// Apply a single relocation entry (R_MIPS_NONE, R_MIPS_32, R_MIPS_26, or R_MIPS_LO16).
///
/// Types without an implementation (including an unpaired R_MIPS_HI16 routed
/// here) leave the word untouched and are counted in `stats.unhandled`.
fn apply_single(
    data: &mut [Vec<u8>],
    entry: &RelocEntry,
    bases: &[u32],
    stats: &mut RelocStats,
) {
    let seg_data = &mut data[entry.ofs_base as usize];
    let off = entry.offset as usize;
    let base = bases[entry.addr_base as usize];
    let word = u32::from_le_bytes(seg_data[off..off + 4].try_into().unwrap());
    let new_word = match entry.r_type {
        R_MIPS_NONE => Some(word), // defined no-op
        R_MIPS_32 => Some(word.wrapping_add(base)),
        R_MIPS_26 => {
            let target = ((word & 0x03FF_FFFF) << 2).wrapping_add(base);
            Some((word & 0xFC00_0000) | ((target >> 2) & 0x03FF_FFFF))
        }
        R_MIPS_LO16 => {
            let lo = sign_extend_16(word) as u32;
            let new_lo = lo.wrapping_add(base) & 0xFFFF;
            Some((word & 0xFFFF_0000) | new_lo)
        }
        _ => None,
    };
    match new_word {
        Some(w) => {
            stats.handled += 1;
            seg_data[off..off + 4].copy_from_slice(&w.to_le_bytes());
        }
        None => {
            *stats.unhandled.entry(entry.r_type).or_insert(0) += 1;
        }
    }
}

/// Apply HI16/LO16 pairs following loadcore semantics.
///
/// All pending HI16 entries are combined with the first subsequent LO16 entry.
/// The combined 32-bit address = (hi_bits << 16) + sign_extend(lo_bits) + segment_base.
fn apply_hi16_pairs(
    data: &mut [Vec<u8>],
    hi_entries: &[&RelocEntry],
    lo: &RelocEntry,
    bases: &[u32],
    stats: &mut RelocStats,
) {
    stats.handled += hi_entries.len() as u64 + 1; // all HI16 entries + the LO16
    let lo_seg = lo.ofs_base as usize;
    let lo_off = lo.offset as usize;
    let lo_op = u32::from_le_bytes(data[lo_seg][lo_off..lo_off + 4].try_into().unwrap());
    let alo = sign_extend_16(lo_op) as i64;
    let base = bases[hi_entries[0].addr_base as usize] as i64;

    let mut last_lo_val = 0u32;
    for hi in hi_entries {
        let hi_seg = hi.ofs_base as usize;
        let hi_off = hi.offset as usize;
        let hi_op = u32::from_le_bytes(data[hi_seg][hi_off..hi_off + 4].try_into().unwrap());
        let ahi = ((hi_op & 0xFFFF) as i64) << 16;
        let combined = (ahi + alo + base) as u32;
        let lo_val = combined & 0xFFFF;
        let hi_val =
            ((combined.wrapping_sub(sign_extend_16(lo_val) as u32)) >> 16) & 0xFFFF;
        let new_hi_op = (hi_op & 0xFFFF_0000) | hi_val;
        data[hi_seg][hi_off..hi_off + 4].copy_from_slice(&new_hi_op.to_le_bytes());
        last_lo_val = lo_val;
    }
    let new_lo_op = (lo_op & 0xFFFF_0000) | last_lo_val;
    data[lo_seg][lo_off..lo_off + 4].copy_from_slice(&new_lo_op.to_le_bytes());
}

/// Apply Type-A relocations, accumulating HI16 entries and pairing with LO16.
fn apply_type_a_inner(
    data: &mut [Vec<u8>],
    entries: &[RelocEntry],
    bases: &[u32],
    stats: &mut RelocStats,
) {
    let mut pending_hi16: Vec<&RelocEntry> = Vec::new();
    for entry in entries {
        if entry.r_type == R_MIPS_HI16 {
            pending_hi16.push(entry);
        } else if !pending_hi16.is_empty() {
            if entry.r_type == R_MIPS_LO16 {
                apply_hi16_pairs(data, &pending_hi16, entry, bases, stats);
            } else {
                // Unpaired HI16 — apply_single has no HI16 arm, so these are
                // counted as unhandled (the silent-failure path issue #37 surfaces).
                for hi in &pending_hi16 {
                    apply_single(data, hi, bases, stats);
                }
                apply_single(data, entry, bases, stats);
            }
            pending_hi16.clear();
        } else {
            apply_single(data, entry, bases, stats);
        }
    }
}

/// Read a variable-length integer from the byte stream.
///
/// Each byte contributes 7 bits; bit 7 set means more bytes follow.
fn read_varint(data: &[u8], pos: &mut usize) -> Result<u32, ParseError> {
    let mut result = 0u32;
    let mut shift = 0u32;
    loop {
        if *pos >= data.len() {
            return Err(ParseError::Reloc {
                message: "varint truncated".into(),
            });
        }
        let b = data[*pos];
        *pos += 1;
        result |= ((b & 0x7F) as u32) << shift;
        shift += 7;
        if b & 0x80 == 0 {
            break;
        }
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(offset: u32, r_type: u8) -> RelocEntry {
        RelocEntry { offset, r_type, ofs_base: 0, addr_base: 0 }
    }

    #[test]
    fn unknown_r_type_is_counted_and_word_untouched() {
        // R_MIPS_GPREL16 (7) has no implementation — the silent path #37 surfaces.
        let mut segs = vec![0x1122_3344u32.to_le_bytes().to_vec()];
        let entries = vec![entry(0, 7), entry(0, 7), entry(0, 9)];
        let stats = apply_relocations(&mut segs, &[0x0880_0000], &entries, &[]).unwrap();
        assert_eq!(stats.unhandled.get(&7), Some(&2));
        assert_eq!(stats.unhandled.get(&9), Some(&1));
        assert_eq!(stats.handled, 0);
        let word = u32::from_le_bytes(segs[0][0..4].try_into().unwrap());
        assert_eq!(word, 0x1122_3344, "unhandled reloc must leave the word untouched");
    }

    #[test]
    fn handled_types_are_applied_and_counted() {
        // R_MIPS_32 at offset 0, R_MIPS_NONE at offset 4.
        let mut segs = vec![{
            let mut v = 0x0000_0010u32.to_le_bytes().to_vec();
            v.extend_from_slice(&0xDEAD_BEEFu32.to_le_bytes());
            v
        }];
        let entries = vec![entry(0, R_MIPS_32), entry(4, R_MIPS_NONE)];
        let stats = apply_relocations(&mut segs, &[0x0880_0000], &entries, &[]).unwrap();
        assert_eq!(stats.handled, 2);
        assert!(stats.unhandled.is_empty());
        let word = u32::from_le_bytes(segs[0][0..4].try_into().unwrap());
        assert_eq!(word, 0x0880_0010, "R_MIPS_32 adds the segment base");
    }

    #[test]
    fn hi16_lo16_pair_counts_as_handled() {
        // hi: lui-style word, lo: addiu-style word; base 0x08800000.
        let mut segs = vec![{
            let mut v = 0x3C04_0000u32.to_le_bytes().to_vec(); // lui a0, 0x0000
            v.extend_from_slice(&0x2484_0010u32.to_le_bytes()); // addiu a0, a0, 0x10
            v
        }];
        let entries = vec![entry(0, R_MIPS_HI16), entry(4, R_MIPS_LO16)];
        let stats = apply_relocations(&mut segs, &[0x0880_0000], &entries, &[]).unwrap();
        assert_eq!(stats.handled, 2);
        assert!(stats.unhandled.is_empty());
        let hi = u32::from_le_bytes(segs[0][0..4].try_into().unwrap());
        let lo = u32::from_le_bytes(segs[0][4..8].try_into().unwrap());
        assert_eq!(hi & 0xFFFF, 0x0880);
        assert_eq!(lo & 0xFFFF, 0x0010);
    }

    #[test]
    fn handled_type_predicate_matches_implementation() {
        for t in HANDLED_RELOC_TYPES {
            assert!(is_handled_reloc_type(t));
        }
        assert!(!is_handled_reloc_type(7));
        assert!(!is_handled_reloc_type(255));
    }
}
