//! Phase 5.2 — proc-macros for orca-plugin-sdk.
//!
//! Two surfaces:
//!   - `#[orca_plugin]` — attribute macro on a struct that auto-emits
//!     `impl Plugin for X` from key=value args plus the
//!     `orca_plugin_sdk::export_plugin!(X);` call. Equivalent to
//!     hand-writing the impl + the export macro.
//!   - `#[orca_slot(DeviceAgent)]` — attribute macro on an impl block
//!     that wraps the body with `orca_plugin_sdk::device::register::<X>()?;`
//!     inside the Plugin::register so authors don't have to plumb it
//!     manually.
//!
//! Both are re-exported from `orca-plugin-sdk` as `#[orca::plugin]`
//! and `#[orca::slot]` via `pub use orca_plugin_macros::*` once 5.2
//! ships. Authors write:
//!
//! ```ignore
//! use orca_plugin_sdk::prelude::*;
//!
//! #[orca::plugin(
//!     id = "com.example.demo",
//!     version = "0.1.0",
//!     permissions = ["network", "ui_attach"],
//! )]
//! pub struct Demo;
//!
//! impl Demo {
//!     fn on_register(ctx: &orca::Ctx) -> orca::Result<()> {
//!         ctx.log_info("hello");
//!         Ok(())
//!     }
//! }
//! ```

use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::quote;
use syn::{
    Expr, ExprArray, ExprLit, ItemStruct, Lit, Meta, Token, parse_macro_input,
    punctuated::Punctuated,
};

/// Parsed body of `#[orca_plugin(id="...", version="...", ...)]`.
#[derive(Default)]
struct PluginArgs {
    id: Option<String>,
    version: Option<String>,
    name: Option<String>,
    author: Option<String>,
    description: Option<String>,
    /// Names of the ORCA_PERM_* bits (lowercase, e.g. "network").
    permissions: Vec<String>,
}

fn extract_lit_str(expr: &Expr) -> Option<String> {
    if let Expr::Lit(ExprLit {
        lit: Lit::Str(s), ..
    }) = expr
    {
        Some(s.value())
    } else {
        None
    }
}

fn extract_lit_str_array(expr: &Expr) -> Option<Vec<String>> {
    if let Expr::Array(ExprArray { elems, .. }) = expr {
        let mut out = Vec::with_capacity(elems.len());
        for e in elems {
            out.push(extract_lit_str(e)?);
        }
        Some(out)
    } else {
        None
    }
}

fn parse_args(input: TokenStream) -> syn::Result<PluginArgs> {
    let metas = syn::parse::Parser::parse(Punctuated::<Meta, Token![,]>::parse_terminated, input)?;

    let mut args = PluginArgs::default();

    for m in metas {
        let nv = match m {
            Meta::NameValue(nv) => nv,
            other => {
                return Err(syn::Error::new_spanned(
                    other,
                    "expected key = value (e.g. id = \"com.example.demo\")",
                ));
            }
        };
        let key = nv
            .path
            .get_ident()
            .map(|i| i.to_string())
            .unwrap_or_default();

        match key.as_str() {
            "id" => args.id = extract_lit_str(&nv.value),
            "version" => args.version = extract_lit_str(&nv.value),
            "name" => args.name = extract_lit_str(&nv.value),
            "author" => args.author = extract_lit_str(&nv.value),
            "description" => args.description = extract_lit_str(&nv.value),
            "permissions" => {
                args.permissions = extract_lit_str_array(&nv.value).ok_or_else(|| {
                    syn::Error::new_spanned(&nv.value, "permissions must be an array of strings")
                })?;
            }
            other => {
                return Err(syn::Error::new_spanned(
                    &nv.path,
                    format!("unknown #[orca_plugin] arg: {other}"),
                ));
            }
        }
    }

    if args.id.is_none() {
        return Err(syn::Error::new(
            proc_macro2::Span::call_site(),
            "#[orca_plugin] requires id = \"...\"",
        ));
    }
    if args.version.is_none() {
        return Err(syn::Error::new(
            proc_macro2::Span::call_site(),
            "#[orca_plugin] requires version = \"...\"",
        ));
    }
    Ok(args)
}

fn permissions_to_bits(perms: &[String]) -> TokenStream2 {
    let mut bits: u64 = 0;
    for p in perms {
        bits |= match p.as_str() {
            "network" => 1 << 0,
            "filesystem_read" => 1 << 1,
            "filesystem_write" => 1 << 2,
            "settings_read" => 1 << 3,
            "settings_write" => 1 << 4,
            "profiles_install" => 1 << 5,
            "device_control" => 1 << 6,
            "slice_intercept" => 1 << 7,
            "gcode_modify" => 1 << 8,
            "ui_attach" => 1 << 9,
            "events_raw" => 1 << 10,
            _ => 0,
        };
    }
    let bits_lit = proc_macro2::Literal::u64_unsuffixed(bits);
    quote! { #bits_lit }
}

/// The attribute macro entry point. Hooked as `#[orca_plugin]`; the
/// SDK re-exports as `#[orca::plugin]`.
#[proc_macro_attribute]
pub fn orca_plugin(args: TokenStream, input: TokenStream) -> TokenStream {
    let parsed = match parse_args(args) {
        Ok(a) => a,
        Err(e) => return e.to_compile_error().into(),
    };
    let item = parse_macro_input!(input as ItemStruct);

    let ident = &item.ident;
    let id_lit = parsed.id.unwrap();
    let version_lit = parsed.version.unwrap();
    let name_lit = parsed.name.unwrap_or_else(|| id_lit.clone());
    let author_lit = parsed.author.unwrap_or_default();
    let desc_lit = parsed.description.unwrap_or_default();
    let perms_bits = permissions_to_bits(&parsed.permissions);

    let expanded = quote! {
        #item

        impl ::orca_plugin_sdk::Plugin for #ident {
            const ID:          &'static str = #id_lit;
            const VERSION:     &'static str = #version_lit;
            const NAME:        &'static str = #name_lit;
            const AUTHOR:      &'static str = #author_lit;
            const DESCRIPTION: &'static str = #desc_lit;
            const PERMISSIONS: u64          = #perms_bits;

            fn register(ctx: &::orca_plugin_sdk::Ctx)
                -> ::orca_plugin_sdk::Result<()>
            {
                Self::on_register(ctx)
            }
        }

        ::orca_plugin_sdk::export_plugin!(#ident);
    };

    expanded.into()
}
