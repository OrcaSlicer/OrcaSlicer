package Slic3r::Polyline;
use Moose;

has 'lines' => (
    traits  => ['Array'],
    is      => 'rw',
    isa     => 'ArrayRef[Slic3r::Line]',
    default => sub { [] },
    handles => {
        add_line => 'push',
    },
);

after 'add_line' => sub {
    my $self = shift;
    
    # add a weak reference to this polyline in line objects
    # (avoid circular refs)
    $self->lines->[-1]->polyline($self);
};

sub BUILD {
    my $self = shift;
    $_->polyline($self) for @{ $self->lines };
}

sub id {
    my $self = shift;
    return join '-', map($_->id, $self->points);
}

sub new_from_points {
    my $class = shift;
    my (@points) = @_;
    
    # we accept Point objects or arrayrefs with point coordinates
    @points = map {
        ref $_ eq 'ARRAY' 
            ? Slic3r::Point->new('x' => $_->[0], 'y' => $_->[1])
            : $_
    } @points;
    
    my $polyline = __PACKAGE__->new;
    my $previous_point;
    $previous_point = $points[-1] if $class eq 'Slic3r::Polyline::Closed';
    foreach my $point (@points) {
        if ($previous_point) {
            my $line = Slic3r::Line->new(a => $previous_point, b => $point);
            $polyline->add_line($line);
        }
        $previous_point = $point;
    }
    
    return $polyline;
}

sub points {
    my $self = shift;
    my %points = ();
    $points{$_} = $_ for map $_->points, @{ $self->lines };
    return values %points;
}

1;
