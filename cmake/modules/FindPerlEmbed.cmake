# Find the dependencies for linking with the Perl runtime library.

# Check for the Perl & PerlLib modules
include(LibFindMacros)
libfind_package(PerlEmbed Perl)
libfind_package(PerlEmbed PerlLibs)

# Execute an Alien::Wx module to find the relevant information regarding
# the wxWidgets used by the Perl interpreter.
# Perl specific stuff
set(PerlEmbed_TEMP_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/PerlEmbed_TEMP_INCLUDE.txt)
execute_process(
    COMMAND ${PERL_EXECUTABLE} -MExtUtils::Embed -e "
# Import Perl modules.
use strict;
use warnings;
use Config;
use Text::ParseWords;
use ExtUtils::CppGuess;

# Test for a Visual Studio compiler
my \$cpp_guess = ExtUtils::CppGuess->new;
my \$mswin = \$^O eq 'MSWin32';
my \$msvc  = \$cpp_guess->is_msvc;

# Query the available data from Alien::wxWidgets.
my \$ccflags;
my \$ldflags;
{ local *STDOUT; open STDOUT, '>', \\\$ccflags; ccflags; }
{ local *STDOUT; open STDOUT, '>', \\\$ldflags; ldopts; }
\$ccflags = ' ' . \$ccflags;
\$ldflags = ' ' . \$ldflags;

my \$filename     = '${PerlEmbed_TEMP_INCLUDE}';
open(my $fh, '>', \$filename) or die \"Could not open file '\$filename' \$!\";

# Convert a space separated lists to CMake semicolon separated lists,
# escape the backslashes,
# export the resulting list to a temp file.
sub cmake_set_var {
  my (\$varname, \$content) = @_;
  # Remove line separators.
  \$content =~ s/\\r|\\n//g;
  # Escape the path separators.
  \$content =~ s/\\\\/\\\\\\\\\\\\\\\\/g;
  my @words = shellwords(\$content); 
  print \$fh \"set(PerlEmbed_\$varname \\\"\" . join(';', @words) . \"\\\")\\n\";
}
cmake_set_var('ARCHNAME',   \$Config{archname});
cmake_set_var('CCFLAGS',    \$ccflags);
\$ldflags =~ s/ -L/ -LIBPATH:/g if \$msvc;
cmake_set_var('LD',         \$Config{ld});
cmake_set_var('LDFLAGS',    \$ldflags);
cmake_set_var('CCCDLFLAGS', \$Config{cccdlflags});
cmake_set_var('LDDLFLAGS',  \$Config{lddlflags});
cmake_set_var('DLEXT',      \$Config{dlext});
close \$fh;
")
include(${PerlEmbed_TEMP_INCLUDE})
file(REMOVE ${PerlEmbed_TEMP_INCLUDE})
unset(PerlEmbed_TEMP_INCLUDE)

if (PerlEmbed_DEBUG)
  # First show the configuration extracted by FindPerl & FindPerlLibs:
  message(STATUS " PERL_INCLUDE_PATH      = ${PERL_INCLUDE_PATH}")
  message(STATUS " PERL_LIBRARY           = ${PERL_LIBRARY}")
  message(STATUS " PERL_EXECUTABLE        = ${PERL_EXECUTABLE}")
  message(STATUS " PERL_SITESEARCH        = ${PERL_SITESEARCH}")
  message(STATUS " PERL_SITELIB           = ${PERL_SITELIB}")
  message(STATUS " PERL_VENDORARCH        = ${PERL_VENDORARCH}")
  message(STATUS " PERL_VENDORLIB         = ${PERL_VENDORLIB}")
  message(STATUS " PERL_ARCHLIB           = ${PERL_ARCHLIB}")
  message(STATUS " PERL_PRIVLIB           = ${PERL_PRIVLIB}")
  message(STATUS " PERL_EXTRA_C_FLAGS     = ${PERL_EXTRA_C_FLAGS}")
  # Second show the configuration extracted by this module (FindPerlEmbed):
  message(STATUS " PerlEmbed_ARCHNAME     = ${PerlEmbed_ARCHNAME}")
  message(STATUS " PerlEmbed_CCFLAGS      = ${PerlEmbed_CCFLAGS}")
  message(STATUS " PerlEmbed_CCCDLFLAGS   = ${PerlEmbed_CCCDLFLAGS}")
  message(STATUS " LD                     = ${PerlEmbed_LD}")
  message(STATUS " PerlEmbed_LDFLAGS      = ${PerlEmbed_LDFLAGS}")
  message(STATUS " PerlEmbed_LDDLFLAGS    = ${PerlEmbed_LDDLFLAGS}")
endif()

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(PerlEmbed 
  REQUIRED_VARS PerlEmbed_CCFLAGS PerlEmbed_LDFLAGS
  VERSION_VAR PERL_VERSION)
