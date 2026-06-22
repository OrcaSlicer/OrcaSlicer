//! Resolves the verifier's `path` argument into an on-disk plugin root.
//!
//! Two input shapes:
//!   1. A directory: pass through, no temp dir.
//!   2. A `.orcaplugin` (or `.zip`) file: extract into a tempdir, return
//!      its root. The plugin lives at the zip's top-level directory (the
//!      Makefile in engine/plugins/<your-plugin> stages
//!      `staged/<id>/` and zips THAT — so the extracted tree has a single
//!      `<id>/` directory at the top with manifest.json + binaries inside).
//!      If the zip extracts to a single top-level dir, descend into it.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use tempfile::TempDir;

pub struct Unpacked {
    pub root: PathBuf,
    /// Held to keep the tempdir alive for the lifetime of the verifier run.
    /// Dropped on exit, which removes the unpacked tree.
    pub _tmp: Option<TempDir>,
}

pub fn resolve(input: &Path) -> io::Result<Unpacked> {
    let meta = fs::metadata(input)?;
    if meta.is_dir() {
        return Ok(Unpacked {
            root: input.to_path_buf(),
            _tmp: None,
        });
    }

    let tmp = TempDir::new()?;
    let target = tmp.path().to_path_buf();
    extract_zip(input, &target)?;

    // If the zip contained a single top-level directory, descend into it.
    let entries: Vec<_> = fs::read_dir(&target)?.collect::<Result<_, _>>()?;
    let root = if entries.len() == 1 && entries[0].file_type()?.is_dir() {
        entries[0].path()
    } else {
        target
    };

    Ok(Unpacked {
        root,
        _tmp: Some(tmp),
    })
}

fn extract_zip(archive: &Path, dest: &Path) -> io::Result<()> {
    let file = fs::File::open(archive)?;
    let mut zip =
        zip::ZipArchive::new(file).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;

    for i in 0..zip.len() {
        let mut entry = zip
            .by_index(i)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
        let raw_name = entry.enclosed_name().ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!("zip entry {i} has an invalid path (possible zip-slip)"),
            )
        })?;
        let out_path = dest.join(raw_name);

        if entry.is_dir() {
            fs::create_dir_all(&out_path)?;
            continue;
        }
        if let Some(parent) = out_path.parent() {
            fs::create_dir_all(parent)?;
        }
        let mut out = fs::File::create(&out_path)?;
        io::copy(&mut entry, &mut out)?;

        // Preserve executable bit on Unix — plugin .so/.dylib needs +x for
        // some loaders to be happy (Linux dlopen tolerates non-+x, macOS is
        // pickier).
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            if let Some(mode) = entry.unix_mode() {
                let _ = fs::set_permissions(&out_path, fs::Permissions::from_mode(mode));
            }
        }
    }
    Ok(())
}
