package Slic3r::Print;
use Moose;

has 'layers' => (
    traits  => ['Array'],
    is      => 'rw',
    isa     => 'ArrayRef[Slic3r::Layer]',
    default => sub { [] },
    handles => {
        layer_count => 'count',
        add_layer   => 'push',
    },
);

sub layer {
    my $self = shift;
    my ($layer_id) = @_;
    
    # extend our print by creating all necessary layers
    if ($self->layer_count < $layer_id + 1) {
        for (my $i = $self->layer_count; $i <= $layer_id; $i++) {
            $self->add_layer(Slic3r::Layer->new(id => $i));
        }
    }
    
    return $self->layers->[$layer_id];
}

1;
