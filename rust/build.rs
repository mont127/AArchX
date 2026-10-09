use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

fn gen_ffi(root: &Path, out_dir: &Path) {
    let wrapper = root.join("rust/wrapper.h");
    println!("cargo:rerun-if-changed={}", wrapper.display());
    for entry in fs::read_dir(root.join("include/ocerz")).unwrap() {
        println!("cargo:rerun-if-changed={}", entry.unwrap().path().display());
    }

    let bindings = bindgen::Builder::default()
        .header(wrapper.display().to_string())
        .clang_arg(format!("-I{}", root.join("include").display()))
        .clang_args(["-std=c11", "-arch", "arm64"])
        .allowlist_file(".*/include/ocerz/.*")
        .derive_default(true)
        .layout_tests(true)
        .prepend_enum_name(false)
        .generate()
        .expect("bindgen failed over include/ocerz");

    bindings
        .write_to_file(out_dir.join("ffi.rs"))
        .expect("failed to write ffi.rs");
}

fn gen_ported(out_dir: &Path) {
    let ported = Path::new(env!("CARGO_MANIFEST_DIR")).join("src/ported");
    println!("cargo:rerun-if-changed={}", ported.display());

    let mut names: Vec<String> = Vec::new();
    if let Ok(entries) = fs::read_dir(&ported) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.extension().is_some_and(|e| e == "rs") {
                names.push(path.file_stem().unwrap().to_string_lossy().into_owned());
            } else if path.is_dir() && path.join("mod.rs").exists() {
                names.push(path.file_name().unwrap().to_string_lossy().into_owned());
            }
        }
    }
    names.sort();

    let mut out = String::new();
    for name in names {
        let abs = if ported.join(&name).is_dir() {
            ported.join(&name).join("mod.rs")
        } else {
            ported.join(format!("{name}.rs"))
        };
        out.push_str(&format!(
            "#[path = \"{}\"]\npub mod {};\n",
            abs.display(),
            name
        ));
    }
    fs::write(out_dir.join("ported_mods.rs"), out).expect("failed to write ported_mods.rs");
}

fn main() {
    let root = repo_root();
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    gen_ffi(&root, &out_dir);
    gen_ported(&out_dir);
}
