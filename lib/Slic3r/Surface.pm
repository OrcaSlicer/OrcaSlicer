package Slic3r::Surface;
use Moose;

use Moose::Util::TypeConstraints;

has 'contour' => (
    is          => 'ro',
    isa         => 'Slic3r::Polyline::Closed',
    required    => 1,
);

has 'holes' => (
    traits  => ['Array'],
    is      => 'rw',
    isa     => 'ArrayRef[Slic3r::Polyline::Closed]',
    default => sub { [] },
    handles => {
        'add_hole' => 'push',
    },
);

has 'surface_type' => (
    is      => 'rw',
    isa     => enum([qw(internal bottom top)]),
);

after 'add_hole' => sub {
    my $self = shift;
    
    # add a weak reference to this surface in polyline objects
    # (avoid circular refs)
    $self->holes->[-1]->hole_of($self);
};

sub BUILD {
    my $self = shift;
    
    # add a weak reference to this surface in polyline objects
    # (avoid circular refs)
    $self->contour->contour_of($self) if $self->contour;
    $_->hole_of($self) for @{ $self->holes };
}

sub id {
    my $self = shift;
    return $self->contour->id;
}

sub encloses_point {
    my $self = shift;
    my ($point) = @_;
    
    return 0 if !$self->contour->encloses_point($point);
    return 0 if grep $_->encloses_point($point), @{ $self->holes };
    return 1;
}

1;
