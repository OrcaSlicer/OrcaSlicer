package Slic3r::Line;
use Moose;

use Scalar::Util qw(weaken);

has 'a' => (
    is          => 'ro',
    isa         => 'Slic3r::Point',
    required    => 1,
);

has 'b' => (
    is          => 'ro',
    isa         => 'Slic3r::Point',
    required    => 1,
);

has 'polyline' => (
    is          => 'rw',
    isa         => 'Slic3r::Polyline',
    weak_ref    => 1,
);

sub BUILD {
    my $self = shift;
    
    # add a weak reference to this line in point objects
    # (avoid circular refs)
    for ($self->a, $self->b) {
        push @{ $_->lines }, $self;
        weaken($_->lines->[-1]);
    }
}

sub id {
    my $self = shift;
    return $self->a->id . "-" . $self->b->id;
}

sub coincides_with {
    my $self = shift;
    my ($line) = @_;
    
    return ($self->a->coincides_with($line->a) && $self->b->coincides_with($line->b))
        || ($self->a->coincides_with($line->b) && $self->b->coincides_with($line->a));
}

sub slope {
    my $self = shift;
    return undef if $self->b->x == $self->a->x;  # line is vertical
    return ($self->b->y - $self->a->y) / ($self->b->x - $self->a->x); #)
}

sub neighbors {
    my $self = shift;
    return grep $_ && $_ ne $self, map @{$_->lines}, $self->a, $self->b;
}

sub next {
    my $self = shift;
    return +(grep $_ && $_ ne $self, @{$self->b->lines})[0];
}

sub points {
    my $self = shift;
    return ($self->a, $self->b);
}

1;
