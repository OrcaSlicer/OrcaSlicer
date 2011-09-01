_Q: Yet another RepRap slicer?_

A: Yes.

# Slic3r

## What's it?

Slic3r is (er, will be) an STL-to-GCODE translator for RepRap 3D printers, 
like Enrique's Skeinforge or RevK's E3D.

## Why another one? Why Perl?

The goal is to build something more maintainable and flexible than both
Skeinforge and E3D. The code makes extensive use of object-oriented 
programming to achieve some level of abstraction instead of working with
raw geometry and low-level data structures.
This should help to maintain code, fix bugs and implement new and better
algorithms in the future.
Of course, Perl's not that fast as C and usage of modules like Moose make
everything quite memory-hungry, but I'm happy with it. My goal is a "rapid
prototyping" architecture for a slicer.

Also, http://xkcd.com/224/

## What's its current status?

Slic3r can now successfully parse and analyze an STL file by slicing it in
layers and representing internally the following features:

* holes in surfaces;
* external top/bottom surfaces.

This kind of abstraction will allow to implement particular logic and allow the
user to specify custom options.

I need to implement algorithms to produce perimeter outlines and surface fill.

Future goals include support material, options to control bridges, skirt, cool.

## Can I help?

Sure! Send patches and/or drop me a line at aar@cpan.org. You can also 
find me in #RepRap on FreeNode with the nickname _Sound_.

## What's Slic3r license?

Slic3r is dual-licensed under the _Perl Artistic License_ and the _AGPLv3_.
The author is Alessandro Ranellucci (that's me).
