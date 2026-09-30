use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn watch_tree(path: &Path) {
    let Ok(entries) = fs::read_dir(path) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.file_name().is_some_and(|name| {
            let name = name.to_string_lossy();
            name == ".git" || name == "target" || name == "build" || name.starts_with("build-")
        }) {
            continue;
        }
        if path.is_dir() {
            watch_tree(&path);
        } else if path
            .extension()
            .is_some_and(|extension| matches!(extension.to_str(), Some("c" | "h" | "in")))
            || path
                .file_name()
                .is_some_and(|name| name == "CMakeLists.txt")
        {
            println!("cargo:rerun-if-changed={}", path.display());
        }
    }
}

fn main() {
    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.join("../..");
    watch_tree(&root);
    println!("cargo:rerun-if-env-changed=TINYX_XORGPROTO_INCLUDE_DIR");
    println!("cargo:rerun-if-env-changed=TINYX_XTRANS_INCLUDE_DIR");

    let mut config = cmake::Config::new(&root);
    config
        .define("TINYX_BUILD_TESTS", "OFF")
        .define("TINYX_BUILD_EXAMPLE", "OFF")
        .define("TINYX_BUILD_WASM", "OFF");
    let common_include = [
        "/opt/homebrew/include",
        "/usr/local/include",
        "/usr/include",
    ]
    .into_iter()
    .map(Path::new)
    .find(|path| path.join("X11/Xproto.h").is_file());
    if let Some(path) = env::var_os("TINYX_XORGPROTO_INCLUDE_DIR") {
        config.define("TINYX_XORGPROTO_INCLUDE_DIR", path);
    } else if let Some(path) = common_include {
        config.define("TINYX_XORGPROTO_INCLUDE_DIR", path);
    }
    if let Some(path) = env::var_os("TINYX_XTRANS_INCLUDE_DIR") {
        config.define("TINYX_XTRANS_INCLUDE_DIR", path);
    } else if let Some(path) =
        common_include.filter(|path| path.join("X11/Xtrans/Xtrans.h").is_file())
    {
        config.define("TINYX_XTRANS_INCLUDE_DIR", path);
    }

    let destination = config.build();
    println!(
        "cargo:rustc-link-search=native={}",
        destination.join("lib").display()
    );
    println!(
        "cargo:rustc-link-search=native={}",
        destination.join("lib64").display()
    );
    println!("cargo:rustc-link-lib=static=tinyx");
    if !cfg!(target_os = "windows") {
        println!("cargo:rustc-link-lib=m");
    }
}
