# Find the wxWidgets module based on the information provided by the Perl Alien::wxWidgets module.

# Check for the Perl & PerlLib modules
include(LibFindMacros)
libfind_package(AlienWx Perl)
libfind_package(AlienWx PerlLibs)

if (AlienWx_DEBUG)
  message(STATUS "  AlienWx_FIND_COMPONENTS=${AlienWx_FIND_COMPONENTS}")
endif()

# Execute an Alien::Wx module to find the relevant information regarding
# the wxWidgets used by the Perl interpreter.
# Perl specific stuff
set(AlienWx_TEMP_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/AlienWx_TEMP_INCLUDE.txt)
execute_process(
    COMMAND ${PERL_EXECUTABLE} -e "
# Import Perl modules.
use strict;
use warnings;
use Text::ParseWords;

BEGIN {
  # CMake sets the environment variables CC and CXX to the detected C compiler.
  # There is an issue with the Perl ExtUtils::CBuilder, which does not handle whitespaces
  # in the paths correctly on Windows, so we rather drop the CMake auto-detected paths.
  delete \$ENV{CC};
  delete \$ENV{CXX};
}

use Alien::wxWidgets;
use ExtUtils::CppGuess;

# Test for a Visual Studio compiler
my \$cpp_guess = ExtUtils::CppGuess->new;
my \$mswin = \$^O eq 'MSWin32';
my \$msvc  = \$cpp_guess->is_msvc;

# List of wxWidgets components to be used.
my @components    = split /;/, '${AlienWx_FIND_COMPONENTS}';

# Query the available data from Alien::wxWidgets.
my \$version      = Alien::wxWidgets->version;
my \$config       = Alien::wxWidgets->config;
my \$compiler     = Alien::wxWidgets->compiler;
my \$linker       = Alien::wxWidgets->linker;
my \$include_path = ' ' . Alien::wxWidgets->include_path;
my \$defines      = ' ' . Alien::wxWidgets->defines;
my \$cflags       = Alien::wxWidgets->c_flags;
my \$linkflags    = Alien::wxWidgets->link_flags;
my \$libraries    = ' ' . Alien::wxWidgets->libraries(@components);
my \$gui_toolkit  = Alien::wxWidgets->config->{toolkit};
#my @libraries     = Alien::wxWidgets->link_libraries(@components);
#my @implib        = Alien::wxWidgets->import_libraries(@components);
#my @shrlib        = Alien::wxWidgets->shared_libraries(@components);
#my @keys          = Alien::wxWidgets->library_keys; # 'gl', 'adv', ...
#my \$library_path = Alien::wxWidgets->shared_library_path;
#my \$key          = Alien::wxWidgets->key;
#my \$prefix       = Alien::wxWidgets->prefix;

my \$filename     = '${AlienWx_TEMP_INCLUDE}';
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
  print \$fh \"set(AlienWx_\$varname \\\"\" . join(';', @words) . \"\\\")\\n\";    
}
cmake_set_var('VERSION', \$version);
\$include_path =~ s/ -I/ /g;
cmake_set_var('INCLUDE_DIRS', \$include_path);
\$libraries =~ s/ -L/ -LIBPATH:/g if \$msvc;
cmake_set_var('LIBRARIES', \$libraries);
#cmake_set_var('LIBRARY_DIRS', );
#\$defines =~ s/ -D/ /g;
cmake_set_var('DEFINITIONS', \$defines);
#cmake_set_var('DEFINITIONS_DEBUG', );
cmake_set_var('CXX_FLAGS', \$cflags);
cmake_set_var('GUI_TOOLKIT', \$gui_toolkit);
close \$fh;
")
include(${AlienWx_TEMP_INCLUDE})
file(REMOVE ${AlienWx_TEMP_INCLUDE})
unset(AlienWx_TEMP_INCLUDE)

if (AlienWx_DEBUG)
  message(STATUS "  AlienWx_VERSION           = ${AlienWx_VERSION}")
  message(STATUS "  AlienWx_INCLUDE_DIRS      = ${AlienWx_INCLUDE_DIRS}")
  message(STATUS "  AlienWx_LIBRARIES         = ${AlienWx_LIBRARIES}")
  message(STATUS "  AlienWx_LIBRARY_DIRS      = ${AlienWx_LIBRARY_DIRS}")
  message(STATUS "  AlienWx_DEFINITIONS       = ${AlienWx_DEFINITIONS}")
  message(STATUS "  AlienWx_DEFINITIONS_DEBUG = ${AlienWx_DEFINITIONS_DEBUG}")
  message(STATUS "  AlienWx_CXX_FLAGS         = ${AlienWx_CXX_FLAGS}")
  message(STATUS "  AlienWx_GUI_TOOLKIT       = ${AlienWx_GUI_TOOLKIT}")
endif()

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(AlienWx 
    REQUIRED_VARS AlienWx_INCLUDE_DIRS AlienWx_LIBRARIES
#    HANDLE_COMPONENTS
    VERSION_VAR AlienWx_VERSION)
