// Female half alone, for the legibility test: is a recessed faceted pocket readable in a
// shaded view, or does a concave feature just read as a dark hole with no orientation?
use <faceted_ridge_key.scad>

// The plate must be THICKER than the key is tall, or the "pocket" is a through-hole. The
// first version used 3 mm against a 4.5 mm key and cut straight through — caught only by
// rendering it. Minimum stock = H + clearance + pocket depth + a wall to print against.
PLATE = [30, 18, 8];
difference() {
    translate([-PLATE[0]/2, -PLATE[1]/2, -PLATE[2]]) cube(PLATE);
    ridge_key_female();
}
