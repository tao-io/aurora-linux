// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Generate the qualified diagnostic at client-owned addresses for the C probe.
#[path = "../g16_dispatch.rs"]
mod dispatch;
fn main() {
    let directory = std::path::PathBuf::from(std::env::args().nth(1).unwrap());
    std::fs::write(directory.join("m4-shader.bin"), dispatch::image(0x10000040000).unwrap()).unwrap();
    std::fs::write(directory.join("m4-esl.bin"), dispatch::esl(0x10000010000).unwrap()).unwrap();
    std::fs::write(directory.join("m4-cdm.bin"), dispatch::cdm(0x10000020000).unwrap()).unwrap();
}
