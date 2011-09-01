package Slic3r::Point;
use Moose;
use Moose::Util::TypeConstraints;

subtype 'Slic3r::Point::Coordinate', as 'Int';
coerce 'Slic3r::Point::Coordinate', from 'Num', via { sprintf '%.0f', $_ };

has 'x' => (
    is          => 'ro',
    isa         => 'Slic3r::Point::Coordinate',
    required    => 1,
    coerce      => 1,
);

has 'y' => (
    is          => 'ro',
    isa         => 'Slic3r::Point::Coordinate',
    required    => 1,
    coerce      => 1,
);

# this array contains weak references, so it can contain undef's as well
has 'lines' => (
    is      => 'rw',
    isa     => 'ArrayRef[Slic3r::Line]',
    default => sub { [] },
);

sub id {
    my $self = shift;
    return $self->x . "," . $self->y; #;;
}

sub coincides_with {
    my $self = shift;
    my ($point) = @_;
    
    return $self->x == $point->x && $self->y == $point->y; #=
}

1;
