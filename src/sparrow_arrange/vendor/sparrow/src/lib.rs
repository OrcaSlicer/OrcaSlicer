use numfmt::{Formatter, Precision, Scales};

pub mod consts;
pub mod eval;
pub mod optimizer;
pub mod quantify;
pub mod sample;
pub mod util;

static FMT: fn() -> Formatter = || -> Formatter {
    Formatter::new()
        .scales(Scales::short())
        .precision(Precision::Significance(3))
};
