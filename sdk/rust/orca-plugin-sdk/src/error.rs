//! Plugin-side error type. Maps to/from orca_error_code_t at the FFI
//! boundary; plugin code uses `Result<T> = core::result::Result<T, Error>`.

use crate::abi;

/// Errors a plugin can return from any host-facing call. The discriminant
/// values are stable and match `orca_error_code_t` in c_api.h.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    Unknown = abi::ORCA_ERR_UNKNOWN as isize,
    InvalidArgument = abi::ORCA_ERR_INVALID_ARGUMENT as isize,
    NotFound = abi::ORCA_ERR_NOT_FOUND as isize,
    AlreadyExists = abi::ORCA_ERR_ALREADY_EXISTS as isize,
    IoError = abi::ORCA_ERR_IO as isize,
    ParseError = abi::ORCA_ERR_PARSE as isize,
    Cancelled = abi::ORCA_ERR_CANCELLED as isize,
    Unsupported = abi::ORCA_ERR_UNSUPPORTED as isize,
    PermissionDenied = abi::ORCA_ERR_PERMISSION_DENIED as isize,
}

impl Error {
    /// Map an orca_error_code_t back into the typed Error. Non-zero
    /// codes that don't match any documented variant become Unknown.
    pub fn from_code(rc: i32) -> Self {
        match rc {
            abi::ORCA_ERR_INVALID_ARGUMENT => Error::InvalidArgument,
            abi::ORCA_ERR_NOT_FOUND => Error::NotFound,
            abi::ORCA_ERR_ALREADY_EXISTS => Error::AlreadyExists,
            abi::ORCA_ERR_IO => Error::IoError,
            abi::ORCA_ERR_PARSE => Error::ParseError,
            abi::ORCA_ERR_CANCELLED => Error::Cancelled,
            abi::ORCA_ERR_UNSUPPORTED => Error::Unsupported,
            abi::ORCA_ERR_PERMISSION_DENIED => Error::PermissionDenied,
            _ => Error::Unknown,
        }
    }

    pub fn as_code(self) -> i32 {
        self as i32
    }
}

pub type Result<T> = core::result::Result<T, Error>;

/// Wrap an orca_error_code_t into a Result. ORCA_OK -> Ok(()).
pub fn check(rc: i32) -> Result<()> {
    if rc == abi::ORCA_OK {
        Ok(())
    } else {
        Err(Error::from_code(rc))
    }
}
