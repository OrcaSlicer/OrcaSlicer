package Slic3r::Layer;
use Moose;

use XXX;

has 'id' => (
    is          => 'ro',
    isa         => 'Int',
    required    => 1,
);

has 'pointmap' => (
    traits      => ['Hash'],
    is          => 'rw',
    isa         => 'HashRef[Slic3r::Point]',
    default     => sub { {} },
    handles     => {
        points  => 'values',
    },
);

has 'lines' => (
    is      => 'rw',
    isa     => 'ArrayRef[Slic3r::Line]',
    default => sub { [] },
);

has 'surfaces' => (
    traits  => ['Array'],
    is      => 'rw',
    isa     => 'ArrayRef[Slic3r::Surface]',
    default => sub { [] },
);

sub z {
    my $self = shift;
    return $self->id * $Slic3r::layer_height / $Slic3r::resolution;
}

sub add_surface {
    my $self = shift;
    my (@vertices) = @_;
    
    my @points = map $self->add_point($_), @vertices;
    my $polyline = Slic3r::Polyline::Closed->new_from_points(@points);
    my @lines = map $self->add_line($_), @{ $polyline->lines };
    
    my $surface = Slic3r::Surface->new(
        contour => Slic3r::Polyline::Closed->new(lines => \@lines),
    );
    push @{ $self->surfaces }, $surface;
    
    return $surface;
}

sub add_line {
    my $self = shift;
    my ($a, $b) = @_;
    
    # we accept either a Line object or a couple of points
    my $line;
    if ($b) {
        ($a, $b) = map $self->add_point($_), ($a, $b);
        $line = Slic3r::Line->new(a => $a, b => $b);
    } elsif (ref $a eq 'Slic3r::Line') {
        $line = $a;
    }
    
    # check whether we already have such a line
    foreach my $point ($line->a, $line->b) {
        foreach my $existing_line (grep $_, @{$point->lines}) {
            return $existing_line 
                if $line->coincides_with($existing_line) && $line ne $existing_line;
        }
    }
    
    push @{ $self->lines }, $line;
    return $line;
}

sub add_point {
    my $self = shift;
    my ($point) = @_;
    
    # we accept either a Point object or a pair of coordinates
    if (ref $point eq 'ARRAY') {
        $point = Slic3r::Point->new('x' => $point->[0], 'y' => $point->[1]);
    }
    
    # check whether we already defined this point
    if (my $existing_point = $self->pointmap_get($point->x, $point->y)) { #)
        return $existing_point;
    }
    
    # define the new point
    $self->pointmap->{ $point->id } = $point; #}}
    
    return $point;
}

sub pointmap_get {
    my $self = shift;
    my ($x, $y) = @_;
    
    return $self->pointmap->{"$x,$y"};
}

sub remove_point {
    my $self = shift;
    my ($point) = @_;
    
    delete $self->pointmap->{ $point->id }; #}}
}

sub remove_line {
    my $self = shift;
    my ($line) = @_;
    @{ $self->lines } = grep $_ ne $line, @{ $self->lines };
}

sub remove_surface {
    my $self = shift;
    my ($surface) = @_;
    @{ $self->surfaces } = grep $_ ne $surface, @{ $self->surfaces };
}

# merge parallel and continuous lines
sub merge_continuous_lines {
    my $self = shift;
    
    my $finished = 0;
    CYCLE: while (!$finished) {
        foreach my $line (@{ $self->lines }) {
            # TODO: we shouldn't skip lines already included in polylines
            next if $line->polyline;
            my $slope = $line->slope;
            
            foreach my $point ($line->points) {
                # skip points connecting more than two lines
                next if @{ $point->lines } > 2;
                
                foreach my $neighbor_line (@{ $point->lines }) {
                    next if $neighbor_line eq $line;
                    
                    # skip line if it's not parallel to ours
                    my $neighbor_slope = $neighbor_line->slope;
                    next if (!defined $neighbor_slope &&  defined $slope)
                          || (defined $neighbor_slope && !defined $slope)
                          || (defined $neighbor_slope &&  defined $slope && $neighbor_slope != $slope);
                    
                    # create new line
                    my ($a, $b) = grep $_ ne $point, $line->points, $neighbor_line->points;
                    my $new_line = $self->add_line($a, $b);
                    printf "Merging continuous lines %s and %s into %s\n", 
                        $line->id, $neighbor_line->id, $new_line->id;
                    
                    # delete merged lines
                    $self->remove_line($_) for ($line, $neighbor_line);
                    
                    # restart cycle
                    next CYCLE;
                }
            }
        }
        $finished = 1;
    }
}

# build polylines of lines which do not already belong to a surface
sub make_polylines {
    my $self = shift;
    
    # defensive programming: let's check that every point
    # connects at least two lines
    foreach my $point ($self->points) {
        if (grep $_, @{ $point->lines } < 2) {
            warn "Found point connecting less than 2 lines:";
            XXX $point;
        }
    }
    
    my $polylines = [];
    foreach my $line (@{ $self->lines }) {
        next if $line->polyline;
        
        my %points = map {$_ => $_} $line->points;
        my %visited_lines = ();
        my ($cur_line, $next_line) = ($line, undef);
        while (!$next_line || $next_line ne $line) {
            $visited_lines{ $cur_line } = $cur_line;
            
            $next_line = +(grep !$visited_lines{$_}, $cur_line->neighbors)[0]
                or last;
            
            $points{$_} = $_ for grep $_ ne $cur_line->a && $_ ne $cur_line->b, $next_line->points;
            $cur_line = $next_line;
        }
        
        printf "Discovered polyline of %d lines (%s)\n", scalar keys %points,
            join('-', map $_->id, values %visited_lines);
        push @$polylines, Slic3r::Polyline::Closed->new(lines => [values %visited_lines]);
    }
    
    return $polylines;
}

sub make_surfaces {
    my $self = shift;
    my ($polylines) = @_;
    
    # count how many other polylines enclose each polyline
    # even = contour; odd = hole
    my %enclosing_polylines = ();
    my %enclosing_polylines_count = ();
    my $max_depth = 0;
    foreach my $polyline (@$polylines) {
        # a polyline encloses another one if any point of it is enclosed
        # in the other
        my $point = $polyline->lines->[0]->a;
        $enclosing_polylines{$polyline} = 
            [ grep $_ ne $polyline && $_->encloses_point($point), @$polylines ];
        $enclosing_polylines_count{$polyline} = scalar @{ $enclosing_polylines{$polyline} };
        
        $max_depth = $enclosing_polylines_count{$polyline}
            if $enclosing_polylines_count{$polyline} > $max_depth;
    }
    
    # start looking at most inner polylines
    for (; $max_depth > -1; $max_depth--) {
        foreach my $polyline (@$polylines) {
            next if $polyline->contour_of or $polyline->hole_of;
            next unless $enclosing_polylines_count{$polyline} == $max_depth;
            
            my $surface;
            if ($enclosing_polylines_count{$polyline} % 2 == 0) {
                # this is a contour
                $surface = Slic3r::Surface->new(contour => $polyline);
            } else {
                # this is a hole
                # find the enclosing polyline having immediately close depth
                my ($contour) = grep $enclosing_polylines_count{$_} == ($max_depth-1), 
                    @{ $enclosing_polylines{$polyline} };
                
                if ($contour->contour_of) {
                    $surface = $contour->contour_of;
                    $surface->add_hole($polyline);
                } else {
                    $surface = Slic3r::Surface->new(
                        contour => $contour,
                        holes   => [$polyline],
                    );
                }
            }
            $surface->surface_type('internal');
            push @{ $self->surfaces }, $surface;
            
            printf "New surface: %s (holes: %s)\n", 
                $surface->id, join(', ', map $_->id, @{$surface->holes}) || 'none';
        }
    }
}

sub merge_contiguous_surfaces {
    my $self = shift;
    
    my $finished = 0;
    CYCLE: while (!$finished) {
        foreach my $surface (@{ $self->surfaces }) {
            # look for a surface sharing one edge with this one
            foreach my $neighbor_surface (@{ $self->surfaces }) {
                next if $surface eq $neighbor_surface;
                
                # find lines shared by the two surfaces (might be 0, 1, 2)
                my @common_lines = ();
                foreach my $line (@{ $neighbor_surface->contour->lines }) {
                    next unless grep $_ eq $line, @{ $surface->contour->lines };
                    push @common_lines, $line;
                }
                next if !@common_lines;
                
                # defensive programming
                if (@common_lines > 2) {
                    printf "Surfaces %s and %s share %d lines! How's it possible?\n",
                        $surface->id, $neighbor_surface->id, scalar @common_lines;
                }
                
                printf "Surfaces %s and %s share line/lines %s!\n",
                    $surface->id, $neighbor_surface->id,
                    join(', ', map $_->id, @common_lines);
                
                # defensive programming
                if ($surface->surface_type ne $neighbor_surface->surface_type) {
                    die "Surfaces %s and %s are of different types: %s, %s!\n",
                        $surface->id, $neighbor_surface->id,
                        $surface->surface_type, $neighbor_surface->surface_type;
                }
                
                # build new contour taking all lines of the surfaces' contours
                # and removing the ones that matched
                my @new_lines = map @{$_->contour->lines}, $surface, $neighbor_surface;
                foreach my $line (@common_lines) {
                    @new_lines = grep $_ ne $line, @new_lines;
                }
                my $new_contour = Slic3r::Polyline::Closed->new(
                    lines => [ @new_lines ],
                );
                
                # build new surface by combining all holes in the two surfaces
                my $new_surface = Slic3r::Surface->new(
                    contour         => $new_contour,
                    holes           => [ map @{$_->holes}, $surface, $neighbor_surface ],
                    surface_type    => $surface->surface_type,
                );
                
                printf "  merging into new surface %s\n", $new_surface->id;
                push @{ $self->surfaces }, $surface;
                
                $self->remove_surface($_) for ($surface, $neighbor_surface);
            }
        }
        $finished = 1;
    }
}

1;
