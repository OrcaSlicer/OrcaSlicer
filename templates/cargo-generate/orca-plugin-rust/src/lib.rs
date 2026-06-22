//! {{project-name}} — Orca plugin scaffolded from the cargo-generate template.
//!
//! Build with `cargo build --release` and drop the resulting
//! target/release/lib{{project-name}}.{so,dll,dylib} alongside
//! `plugin.json` under <data_dir>/plugins/{{plugin_id}}/.

use orca_plugin_sdk::prelude::*;

#[orca_plugin(
    id          = "{{plugin_id}}",
    version     = "0.1.0",
    description = "{{plugin_description}}",
    permissions = [],
)]
pub struct Plugin;

impl Plugin {
    /// Called once after the engine validates the ABI handshake.
    /// Replace the body with real registration logic — subscribe to
    /// events, register slot vtables, attach device agents, etc.
    fn on_register(ctx: &Ctx) -> Result<()> {
        ctx.log_info("{{project-name}} loaded");
        Ok(())
    }
}
