; Co-extrusion physical theory test. Generated independently of the slicer.
; Bed 110x110 mm; nozzle 0.4 mm; filament 1.75 mm; layer 0.2; width 0.45.
; Requires a physically verified manual C zero before EACH file.
; Configured physical C limits [-360, 360] degrees.
; Firmware contract: absolute C degrees; C-only F in degrees/min; M83 filament-mm E.
; Machine mapping: commanded bed Y+ moves toward observer; relative nozzle Y is inverted.
; Color angles and C are physical clockwise-positive from +X, with +Y toward observer.
; No printer communication is performed by the generator.
; Test 03: 20x20x4 mm single-wall square, 20 layers, blue outward faces.
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
; LAYER:1 Z=0.200
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z2.200 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z0.200 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.200 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z0.200 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.200 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z0.200 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.200 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z0.200 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.200 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:2 Z=0.400
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z2.400 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z0.400 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.400 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z0.400 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.400 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z0.400 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.400 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z0.400 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.400 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:3 Z=0.600
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z2.600 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z0.600 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.600 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z0.600 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.600 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z0.600 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.600 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z0.600 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.600 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:4 Z=0.800
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z2.800 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z0.800 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.800 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z0.800 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.800 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z0.800 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.800 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z0.800 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z2.800 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:5 Z=1.000
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z3.000 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z1.000 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.000 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z1.000 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.000 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z1.000 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.000 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z1.000 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.000 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:6 Z=1.200
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z3.200 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z1.200 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.200 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z1.200 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.200 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z1.200 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.200 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z1.200 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.200 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:7 Z=1.400
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z3.400 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z1.400 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.400 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z1.400 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.400 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z1.400 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.400 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z1.400 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.400 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:8 Z=1.600
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z3.600 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z1.600 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.600 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z1.600 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.600 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z1.600 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.600 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z1.600 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.600 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:9 Z=1.800
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z3.800 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z1.800 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.800 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z1.800 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.800 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z1.800 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.800 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z1.800 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z3.800 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:10 Z=2.000
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z4.000 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z2.000 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.000 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z2.000 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.000 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z2.000 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.000 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z2.000 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.000 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:11 Z=2.200
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z4.200 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z2.200 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.200 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z2.200 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.200 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z2.200 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.200 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z2.200 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.200 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:12 Z=2.400
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z4.400 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z2.400 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.400 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z2.400 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.400 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z2.400 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.400 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z2.400 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.400 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:13 Z=2.600
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z4.600 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z2.600 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.600 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z2.600 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.600 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z2.600 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.600 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z2.600 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.600 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:14 Z=2.800
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z4.800 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z2.800 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.800 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z2.800 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.800 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z2.800 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.800 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z2.800 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z4.800 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:15 Z=3.000
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z5.000 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z3.000 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.000 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z3.000 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.000 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z3.000 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.000 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z3.000 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.000 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:16 Z=3.200
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z5.200 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z3.200 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.200 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z3.200 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.200 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z3.200 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.200 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z3.200 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.200 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:17 Z=3.400
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z5.400 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z3.400 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.400 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z3.400 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.400 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z3.400 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.400 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z3.400 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.400 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:18 Z=3.600
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z5.600 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z3.600 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.600 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z3.600 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.600 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z3.600 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.600 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z3.600 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.600 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:19 Z=3.800
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z5.800 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z3.800 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.800 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z3.800 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.800 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z3.800 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.800 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z3.800 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z5.800 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

; LAYER:20 Z=4.000
; Entry: G21, G90, M83, physically known C; nozzle retracted by RETRACT_MM.
; All four outward SIDE faces should be BLUE, theta=120 degrees.
; LAYER_TRAVEL_Z must be above the entire existing wall before any C rotation.
G1 Z6.000 F300
; Bed Y+ is toward observer: reflect commanded geometry Y into physical Y.
; Side 1: +X at commanded Y0, physical outward normal +Y, phi=90, C=-30.
G1 C-30 F5400
M400
G1 X25.000 Y35.000 F1800
G1 Z4.000 F300
G1 E0.6 F1200
G1 X45.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z6.000 F300
; Side 2: commanded +Y at X20, physical outward normal +X, phi=0, C=-120.
G1 C-120 F5400
M400
G1 Z4.000 F300
G1 E0.6 F1200
G1 X45.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z6.000 F300
; Side 3: -X at commanded Y20, physical outward normal -Y, phi=270, C=150.
; Absolute -120 -> +150 is a real +270-degree rotation, entirely within limits.
G1 C150 F5400
M400
G1 Z4.000 F300
G1 E0.6 F1200
G1 X25.000 Y55.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z6.000 F300
; Side 4: commanded -Y at X0, physical outward normal -X, phi=180, C=60.
G1 C60 F5400
M400
G1 Z4.000 F300
G1 E0.6 F1200
G1 X25.000 Y35.000 E0.67698 F900
G1 E-0.6 F1200
G1 Z6.000 F300
; Exit retracted, at X0/Y0 and LAYER_TRAVEL_Z. Add the next layer or final end block.

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
