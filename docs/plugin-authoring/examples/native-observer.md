# Example — native pipeline observer (Rust)

A no-op observer that prints to stderr at every step. Lives in a single
file thanks to the Rust SDK + `#[orca_plugin]` macro.

```rust
use orca_plugin_sdk::prelude::*;

#[orca_plugin(
    id          = "com.example.native-observer",
    version     = "0.1.0",
    description = "Logs every pipeline step",
)]
pub struct Observer;

impl Observer {
    fn on_register(ctx: &Ctx) -> Result<()> {
        ctx.log_info("native-observer loaded");

        // Subscribe to high-level lifecycle events. These run on the
        // publishing thread; keep the body short.
        events::subscribe::<events::SlicingFinished, _>(|e| {
            // Payload pointers are valid for the duration of the call.
            // Convert to owned strings before stashing.
            eprintln!("slice {} finished, success={}", e.handle, e.success);
        });

        Ok(())
    }
}
```

The matching `plugin.json`:

```json
{
  "id":          "com.example.native-observer",
  "name":        "Native Observer",
  "version":     "0.1.0",
  "permissions": []
}
```

`cargo build --release` produces
`target/release/libcom_example_native_observer.so`. Rename it (or
configure your Cargo.toml's `[lib].name`) to match the manifest id so
PluginManager finds it on disk.
