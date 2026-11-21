//! Compiles the compute shaders with glslc (Vulkan SDK): $GLSLC, then $VULKAN_SDK/Bin, then the repo's tools/vulkan_sdk,
//! then PATH.
use std::path::PathBuf;
use std::process::Command;

fn glslc() -> PathBuf {
    if let Ok(p) = std::env::var("GLSLC") {
        return p.into();
    }
    let exe = if cfg!(windows) { "glslc.exe" } else { "glslc" };
    let mut cands = vec![];
    if let Ok(sdk) = std::env::var("VULKAN_SDK") {
        cands.push(PathBuf::from(&sdk).join("Bin").join(exe));
        cands.push(PathBuf::from(&sdk).join("bin").join(exe));
    }
    let here = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    for a in here.ancestors() {
        cands.push(a.join("tools").join("vulkan_sdk").join("Bin").join(exe));
    }
    cands.into_iter().find(|p| p.is_file()).unwrap_or_else(|| exe.into())
}

fn main() {
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let g = glslc();
    for s in ["bake", "points"] {
        let src = format!("shaders/{s}.comp");
        println!("cargo:rerun-if-changed={src}");
        let st = Command::new(&g)
            .args(["--target-env=vulkan1.2", "-O", &src, "-o"])
            .arg(out.join(format!("{s}.spv")))
            .status()
            .unwrap_or_else(|e| panic!("glslc ({}) not found: {e}; set GLSLC or VULKAN_SDK", g.display()));
        assert!(st.success(), "glslc failed on {src}");
    }
    println!("cargo:rerun-if-changed=shaders/trace.glsl");
    println!("cargo:rerun-if-env-changed=GLSLC");
}
