// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Bounded one-thread store used to qualify the owned M4 submission path.
//! This is a diagnostic shader, not a userspace compiler or a Vulkan claim.
//! MOV immediate fields follow the local applegpu M3 decoder; the store/stop
//! body and HAL200 load-shader/CDM grammar match the qualified M4 source.

pub(crate) const VALUE: u32 = 0x44a72000;
pub(crate) const SENTINEL: u32 = 0xdeadbeef;

fn mov32(register: u64, value: u32) -> [u8; 8] {
    let v = value as u64;
    (0x2800c | (register << 4) | ((v & 0x7f) << 8)
        | (((v >> 7) & 0xf) << 33) | (((v >> 11) & 3) << 42)
        | (((v >> 13) & 0xfff) << 48) | (((v >> 25) & 0x7f) << 25)).to_le_bytes()
}

pub(crate) fn shader(output: u64) -> Result<[u8; 42], ()> {
    if output == 0 || output >= 1 << 42 || output & 3 != 0 { return Err(()); }
    let mut code = [0; 42];
    code[..8].copy_from_slice(&mov32(0, output as u32));
    code[8..16].copy_from_slice(&mov32(1, (output >> 32) as u32));
    code[16..24].copy_from_slice(&mov32(2, VALUE));
    code[24..].copy_from_slice(&[0xe7, 0, 0x54, 4, 0, 0, 0, 0,
        0x11, 1, 0, 0x90, 8, 0, 0x0e, 0, 0, 0]);
    Ok(code)
}

pub(crate) fn esl(shader: u64) -> Result<[u8; 32], ()> {
    if shader == 0 || shader >= 1 << 42 || shader & 63 != 0 { return Err(()); }
    let mut out = [0; 32];
    out[..8].copy_from_slice(&0x412a0077u64.to_le_bytes());
    out[8..16].copy_from_slice(&((shader << 17) | 0x2a0177).to_le_bytes());
    // Retain the M4 progress-marker resource control and no constant program.
    out[16..28].copy_from_slice(&[6, 0, 0xf7, 0, 0x2a, 0, 0, 0, 0, 0, 0, 0]);
    out[28] = 0x0e;
    Ok(out)
}

pub(crate) fn cdm(esl: u64) -> Result<[u8; 48], ()> {
    if esl == 0 || esl >= 1 << 42 || esl & 63 != 0 { return Err(()); }
    let words = [1u32 << 19, ((esl >> 16) & 0xffc00000) as u32,
        (esl >> 6) as u32, 0x40000001, 1, 1, 1, 1, 1, 1, 0x60000160, 0x40000000];
    let mut out = [0; 48];
    for (word, bytes) in words.iter().zip(out.chunks_exact_mut(4)) {
        bytes.copy_from_slice(&word.to_le_bytes());
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn independently_decoded_m4_store_fixture() {
        assert_eq!(shader(0x10000090000).unwrap(), [
            0x0c,0x80,0x02,0,0,0,0x48,0, 0x1c,0x80,0x02,0,4,0,0,0,
            0x2c,0x80,0x02,0x44,0,0,0x39,5, 0xe7,0,0x54,4,0,0,0,0,
            0x11,1,0,0x90,8,0,0x0e,0,0,0]);
        assert!(shader(0).is_err());
        assert!(shader(1 << 42).is_err());
    }
    #[test]
    fn qualified_m4_load_shader_record() {
        let encoded = esl(0x200013c040).unwrap();
        assert_eq!(&encoded[8..28], &[0x77,1,0xaa,0x80,0x27,0,0x40,0,
            6,0,0xf7,0,0x2a,0,0,0,0,0,0,0]);
        assert!(esl(65).is_err());
        assert!(cdm(63).is_err());
    }
}
