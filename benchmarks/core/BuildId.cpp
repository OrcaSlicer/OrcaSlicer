#include "core/BuildId.hpp"

// The only file in benchmarks/ that includes these, since git_commit_hash.h changes with every commit.
#include "git_commit_hash.h"
#include "orcabench_build.h"

namespace Slic3r { namespace Bench {

const char* const build_revision = GIT_COMMIT_HASH;
const bool        build_dirty    = GIT_COMMIT_SUFFIX[0] != '\0';

const char* const build_compiler         = ORCABENCH_COMPILER;
const char* const build_compiler_version = ORCABENCH_COMPILER_VERSION;
const char* const build_flags            = ORCABENCH_FLAGS;
const char* const build_config           = ORCABENCH_CONFIG;

}} // namespace Slic3r::Bench
