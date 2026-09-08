; Co-extrusion physical theory test. Generated independently of the slicer.
; Bed 110x110 mm; nozzle 0.4 mm; filament 1.75 mm; layer 0.2; width 0.45.
; Requires a physically verified manual C zero before EACH file.
; Configured physical C limits [-360, 360] degrees.
; Firmware contract: absolute C degrees; C-only F in degrees/min; M83 filament-mm E.
; Machine mapping: commanded bed Y+ moves toward observer; relative nozzle Y is inverted.
; Color angles and C are physical clockwise-positive from +X, with +Y toward observer.
; No printer communication is performed by the generator.
; Test 02: same layout as Test 01; all four TOP surfaces should be BLUE.
; theta_blue=120. Horizontal top phi=heading+180, hence C=heading+60 (mod 360).
; This bed moves toward the observer for commanded Y+.
; Commanded +X:C60; -X:C-120; +Y:C-30; -Y:C150.
; Every C rotation occurs retracted at travel Z, without simultaneous extrusion.
; START_PRINT must finish retracted by RETRACT_MM, with E in relative mode.
; Startup adapted from ssr.gcode supplied by the user.
; BEFORE RUN: manually align physical C=0. XYZ homing/leveling are automatic.
; Clear the bed. Confirm extrusion is measured in filament mm, not volume.
G21
G90
M201 X150 Y200 Z300 E800
M203 X250 Y250 Z5 E40
M204 P300 R500 T300
M205 X10.00 Y10.00 Z0.40 E5.00
M205 J0.100
M220 S100
M221 S100
M107
M140 S55
M104 S150
M190 S55
G28
G29
G90
M400
G92 C0 ; Declare the already established physical zero, not an unwind.
M104 S200
M109 S200
G90
M82
G92 E0
G1 Z2 F300
G1 X0 Y10 F3000
G1 Z0.28 F300
G1 Y90 E15 F1200
G1 X0.4 F3000
G1 Y10 E30 F1200
G92 E0
G1 E-0.6 F1200
G1 Z2 F300
M83
; Machine motion settings from ssr.gcode, with C capped at requested 90 deg/s.
M201 X300 Y800 Z50 C8000 E500
M203 X150 Y200 Z5 C90 E25
M204 P300 R500 T300
M205 J0.013
G90
G21
M83
; Entry to test body: already retracted by 0.6 mm.
G21
G90
M83
M400
G1 Z5.0 F300
; Line A: +X, ideal C=60.
G1 C60 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z0.2 F300
G1 E0.6 F1200
G1 X55.000 Y35.000 E1.01546 F900
G1 E-0.6 F1200
G1 Z5.0 F300
; Line B: -X, ideal C=-120.
G1 C-120 F5400
M400
G1 X55.000 Y43.000 F1800
G1 Z0.2 F300
G1 E0.6 F1200
G1 X25.000 Y43.000 E1.01546 F900
G1 E-0.6 F1200
G1 Z5.0 F300
; Line C: commanded +Y, relative nozzle -Y, ideal C=-30.
G1 C-30 F5400
M400
G1 X65.000 Y35.000 F1800
G1 Z0.2 F300
G1 E0.6 F1200
G1 X65.000 Y65.000 E1.01546 F900
G1 E-0.6 F1200
G1 Z5.0 F300
; Line D: commanded -Y, relative nozzle +Y, ideal C=150.
G1 C150 F5400
M400
G1 X73.000 Y65.000 F1800
G1 Z0.2 F300
G1 E0.6 F1200
G1 X73.000 Y35.000 E1.01546 F900
G1 E-0.6 F1200
G1 Z5.0 F300
M400
G1 C0 F5400
M400
; No additional C reset, homing, or motor release.
G1 Z8.000 F300
G1 X15.000 Y95.000 F1800
M400
M104 S0
M140 S0
M107
; E remains relative and retracted by 0.6 mm; motors remain enabled.
