#!/usr/bin/perl

use strict;
use warnings;

BEGIN {
    use FindBin;
    use lib "$FindBin::Bin/lib";
}

use Slic3r;
use XXX;

my $stl_parser = Slic3r::STL->new;
my $print = $stl_parser->parse_file("testcube20mm.stl");

#XXX $print;

__END__
