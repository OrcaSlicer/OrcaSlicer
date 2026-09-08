; Co-extrusion physical theory test. Generated independently of the slicer.
; Bed 110x110 mm; nozzle 0.4 mm; filament 1.75 mm; layer 0.2; width 0.45.
; Requires a physically verified manual C zero before EACH file.
; Configured physical C limits [-360, 360] degrees.
; Firmware contract: absolute C degrees; C-only F in degrees/min; M83 filament-mm E.
; Machine mapping: commanded bed Y+ moves toward observer; relative nozzle Y is inverted.
; Color angles and C are physical clockwise-positive from +X, with +Y toward observer.
; No printer communication is performed by the generator.
; Test 00: C direction and repeatable physical zero. NO extrusion.
; Start with the nozzle clear of the bed/parts and C physically at its known zero.
; C must use absolute degrees; standalone C feedrate must use degrees/minute.
G21
G90
M400
G92 C0 ; ONLY after manually establishing physical C=0
G90
M400
G1 C0 F5400
M400
G4 P2000
; Viewed from above: clockwise 30 degrees.
G1 C30 F5400
M400
G4 P3000
G1 C0 F5400
M400
G4 P2000
; Viewed from above: counter-clockwise 30 degrees.
G1 C-30 F5400
M400
G4 P3000
G1 C0 F5400
M400
; Finish at physical C=0. No G92 C, heating, homing, or motor release added here.
