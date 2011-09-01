package Slic3r::Polyline::Closed;
use Moose;

extends 'Slic3r::Polyline';

has 'contour_of' => (
    is          => 'rw',
    isa         => 'Slic3r::Surface',
    weak_ref    => 1,
);

has 'hole_of' => (
    is          => 'rw',
    isa         => 'Slic3r::Surface',
    weak_ref    => 1,
);

override 'new_from_points' => sub {
    my $class = shift;
    my $polyline = super();
    
    # polylines must be always closed, otherwise it means that our object is not manifold!
    die "Polylines must be closed! Object not manifold?\n"
        if ($polyline->lines->[0]->a != $polyline->lines->[-1]->b);
    
    return $polyline;
};

sub encloses_point {
    my $self = shift;
    my ($point) = @_;
    
    my @xy = map { $_->x, $_->y } $self->points; #}}
    my ($x, $y) = ($point->x, $point->y); #))
    
    # Derived from the comp.graphics.algorithms FAQ,
    # courtesy of Wm. Randolph Franklin
    my $n = @xy / 2;                        # Number of points in polygon
    my @i = map { 2*$_ } 0..(@xy/2);        # The even indices of @xy
    my @x = map { $xy[$_]     } @i;         # Even indices: x-coordinates
    my @y = map { $xy[$_ + 1] } @i;         # Odd indices:  y-coordinates
    
    my ($i, $j);
    my $side = 0;                           # 0 = outside; 1 = inside
    for ($i = 0, $j = $n - 1; $i < $n; $j = $i++) {
        if (
            # If the y is between the (y-) borders...
            ($y[$i] <= $y && $y < $y[$j]) || ($y[$j] <= $y && $y < $y[$i])
            and
            # ...the (x,y) to infinity line crosses the edge
            # from the ith point to the jth point...
            ($x < ($x[$j] - $x[$i]) * ($y - $y[$i]) / ($y[$j] - $y[$i]) + $x[$i])
        ) {
            $side = not $side;  # Jump the fence
        }
    }
    
    return $side;
}

1;
