// Linker glue so plugin .so files dlopen'd by the verifier can resolve
// `orca_registry_set_manifest` / `orca_registry_add_slot` / etc. against
// the verifier process's own exports. Without -rdynamic the binary's own
// symbol table isn't visible to dlopen'd modules on Linux.
//
// macOS Mach-O exports binary symbols by default — no flag needed.
// Windows uses LoadLibrary which resolves through a static import table;
// the verifier doesn't support native-plugin ABI checks on Windows yet
// (the abi check module SKIPs with a documented reason in that case).
fn main() {
    #[cfg(target_os = "linux")]
    {
        println!("cargo:rustc-link-arg-bins=-rdynamic");
    }
}
