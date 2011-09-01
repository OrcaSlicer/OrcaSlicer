package Slic3r::STL;
use Moose;

use CAD::Format::STL;
use XXX;

use constant X => 0;
use constant Y => 1;
use constant Z => 2;

sub parse_file {
    my $self = shift;
    my ($file) = @_;
    
    my $print = Slic3r::Print->new;
    
    my $stl = CAD::Format::STL->new->load($file);
    
    # we only want to work with positive coordinates, so let's 
    # find our object extents to calculate coordinate displacements
    my @extents = (map [99999999, -99999999], X,Y,Z);
    foreach my $facet ($stl->part->facets) {
        my ($normal, @vertices) = @$facet;
        foreach my $vertex (@vertices) {
            for (X,Y,Z) {
                $extents[$_][0] = $vertex->[$_] if $vertex->[$_] < $extents[$_][0];
                $extents[$_][1] = $vertex->[$_] if $vertex->[$_] > $extents[$_][0];
            }
        }
    }
    
    # calculate the displacements needed to 
    # have lowest value for each axis at coordinate 0
    my @shift = map 0 - $extents[$_][0], X,Y,Z;
    printf "shift = %d, %d, %d\n", @shift[X,Y,Z];
    
    # process facets
    foreach my $facet ($stl->part->facets) {
        
        # transform vertex coordinates
        my ($normal, @vertices) = @$facet;
        foreach my $vertex (@vertices) {
            $vertex->[$_] = sprintf('%.0f', ($vertex->[$_] + $shift[$_]) / $Slic3r::resolution) 
                for X,Y,Z;
        }
        
        $self->_facet($print, @$facet);
    }
    
    print "\n==> PROCESSING SLICES:\n";
    foreach my $layer (@{ $print->layers }) {
        printf "Processing layer %d:\n", $layer->id;
    
        # merge parallel and continuous lines
        $layer->merge_continuous_lines;
        
        # build polylines of lines which do not already belong to a surface
        my $polylines = $layer->make_polylines;
        
        # build surfaces of polylines (distinguishing contours from holes)
        $layer->make_surfaces($polylines);
        
        # merge surfaces having a common line
        $layer->merge_contiguous_surfaces;
    }
    
    return $print;
}

sub _facet {
    my $self = shift;
    my ($print, $normal, @vertices) = @_;
    printf "\n==> FACET (%s):\n", join('-', map join(',', @$_), @vertices);
    
    # find the vertical extents of the facet
    my ($min_z, $max_z) = (99999999, -99999999);
    foreach my $vertex (@vertices) {
        $min_z = $vertex->[Z] if $vertex->[Z] < $min_z;
        $max_z = $vertex->[Z] if $vertex->[Z] > $max_z;
    }
    printf "z: min = %.0f, max = %.0f\n", $min_z, $max_z;
    
    # calculate the layer extents
    my ($min_layer, $max_layer) = map {$_ * $Slic3r::resolution / $Slic3r::layer_height} $min_z, $max_z;
    printf "layers: min = %.0f, max = %.0f\n", $min_layer, $max_layer;
    
    # is the facet horizontal?
    if ($min_layer == $max_layer) {
        printf "Facet is horizontal\n";
        my $layer = $print->layer($min_layer);
        my $surface = $layer->add_surface(@vertices);
        
        # to determine whether the surface is a top or bottom let's recompute
        # the normal using the right-hand rule
        # (this relies on the STL to be well-formed)
        # recompute the normal using the right-hand rule
        my $clockwise = ($vertices[2]->[X] - $vertices[0]->[X]) * ($vertices[1]->[Y] - $vertices[0]->[Y])
                      - ($vertices[1]->[X] - $vertices[0]->[X]) * ($vertices[2]->[Y] - $vertices[0]->[Y]);
        
        # defensive programming and/or input check
        if (($normal->[Z] > 0 && $clockwise < 0) || ($normal->[Z] < 0 && $clockwise > 0)) {
            die "STL normal and right-hand rule computation differ!\n";
        }
        if ($layer->id == 0 && $clockwise < 0) {
            die "Right-hand rule gives bad result for facets on base layer!\n";
        }
        
        $surface->surface_type($clockwise < 0 ? 'top' : 'bottom');
        
        return;
    }
    
    # build the three segments of the triangle facet
    my @edges = (
        [ $vertices[0], $vertices[1] ],
        [ $vertices[1], $vertices[2] ],
        [ $vertices[2], $vertices[0] ],
    );
    
    for (my $layer_id = $min_layer; $layer_id <= $max_layer; $layer_id++) {
        my $layer = $print->layer($layer_id);
        my $z = $layer->z;
        
        my @intersection_points = ();
        
        foreach my $edge (@edges) {
            my ($a, $b) = @$edge;
            if ($a->[Z] == $b->[Z] && $a->[Z] == $z) {
                # edge is horizontal and belongs to the current layer
                $layer->add_line([$a->[X], $a->[Y]], [$b->[X], $b->[Y]]);
                
            } elsif (($a->[Z] < $z && $b->[Z] > $z) || ($b->[Z] < $z && $a->[Z] > $z)) {
                # edge intersects the current layer; calculate intersection
                push @intersection_points, $layer->add_point([
                    $b->[X] + ($a->[X] - $b->[X]) * ($z - $b->[Z]) / ($a->[Z] - $b->[Z]),
                    $b->[Y] + ($a->[Y] - $b->[Y]) * ($z - $b->[Z]) / ($a->[Z] - $b->[Z]),
                ]);
            }
        }
        
        if (@intersection_points) {
            # defensive programming:
            die "Facets must intersect each plane 0 or 2 times" if @intersection_points != 2;
            
            # check whether the two points coincide due to resolution rounding
            if ($intersection_points[0]->coincides_with($intersection_points[1])) {
                printf "Points coincide; removing\n";
                $layer->remove_point($_) for @intersection_points;
                next;
            }
            
            # connect points:
            $layer->add_line(@intersection_points);
        }
    }
}

1;
