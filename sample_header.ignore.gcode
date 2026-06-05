;START_OF_HEADER
;HEADER_VERSION:0.1
;FLAVOR:Griffin
;GENERATOR.NAME:Cura_SteamEngine
;GENERATOR.VERSION:2.1.99-internal.20160623
;GENERATOR.BUILD_DATE:2016-06-23
;TARGET_MACHINE.NAME:FDM Printer Base Description
;EXTRUDER_TRAIN.0.INITIAL_TEMPERATURE:210
;EXTRUDER_TRAIN.0.MATERIAL.VOLUME_USED:2060
;EXTRUDER_TRAIN.0.MATERIAL.GUID:506c9f0d-e3aa-4bd4-b2d2-23e2425b1aa9
;EXTRUDER_TRAIN.0.NOZZLE.DIAMETER:0.4;BUILD_PLATE.INITIAL_TEMPERATURE:70
;PRINT.TIME:474
;PRINT.SIZE.MIN.X:0
;PRINT.SIZE.MIN.Y:0
;PRINT.SIZE.MIN.Z:0
;PRINT.SIZE.MAX.X:215
;PRINT.SIZE.MAX.Y:215
;PRINT.SIZE.MAX.Z:200
;END_OF_HEADER



;SKIP_PROCEDURES:PRE_PRINT_SETUP,PURGE_MATERIAL_MISP,LOAD_MATERIAL_MISP,UNLOAD_MATERIAL_MISP_0,UNLOAD_MATERIAL_MISP_1,PREPARE_MISP_MATERIALS,DEPRIME_FOR_MATERIAL_CHANGE_MISP,SEND_FOLLOW_COMMAND_MISP,BREAK_FAILED_WIZARD,LOADING_FAILURE_RECOVERY_WIZARD,START_COOLDOWN_BED,START_COOLDOWN_HOTEND,SET_HOTEND_TEMPERATURE_WAIT




;SKIP_PROCEDURES:PRE_PRINT_SETUP



; NOTE: Do NOT remove the following header. It is necessary for the UM printer to not crash.
;START_OF_HEADER
;SKIP_PROCEDURES:PRE_PRINT_SETUP
;HEADER_VERSION:0.1
;FLAVOR:Griffin
;GENERATOR.NAME:OrcaSlicer
;GENERATOR.VERSION:5.11.0
;GENERATOR.BUILD_DATE:2016-06-23
;TARGET_MACHINE.NAME:[printer_model]
;EXTRUDER_TRAIN.[initial_extruder].INITIAL_TEMPERATURE:[first_layer_temperature[initial_extruder]]
;EXTRUDER_TRAIN.[initial_extruder].MATERIAL.VOLUME_USED:[extruded_volume_total]
;EXTRUDER_TRAIN.[initial_extruder].MATERIAL.GUID:506c9f0d-e3aa-4bd4-b2d2-23e2425b1aa9
;EXTRUDER_TRAIN.[initial_extruder].NOZZLE.DIAMETER:[nozzle_diameter[initial_extruder]]
;EXTRUDER_TRAIN.[initial_extruder].NOZZLE.NAME:AA [nozzle_diameter[initial_extruder]]
;BUILD_PLATE.INITIAL_TEMPERATURE:[bed_temperature_initial_layer_single]
;BUILD_VOLUME.TEMPERATURE:28
;PRINT.TIME:2
;PRINT.SIZE.MIN.X:0
;PRINT.SIZE.MIN.Y:0
;PRINT.SIZE.MIN.Z:0
;PRINT.SIZE.MAX.X:200
;PRINT.SIZE.MAX.Y:200
;PRINT.SIZE.MAX.Z:200
;END_OF_HEADER


; Sets the acceleration and stuff that we removed before the header in the post-processing script.
M73 P0 R0
;TODO change the R value into the total minutes of print

; Set accel
M201 X[machine_max_acceleration_x] Y[machine_max_acceleration_y] Z[machine_max_acceleration_z] E[machine_max_acceleration_e]

; Set max speed
M203 X[machine_max_speed_x] Y[machine_max_speed_y] Z[machine_max_speed_z] E[machine_max_speed_e]

; Set starting accel
M204 P[machine_max_acceleration_extruding] R[machine_max_acceleration_retracting] T[machine_max_acceleration_travel]

; Set jerk limits
M205 X[machine_max_jerk_x] Y[machine_max_jerk_y] Z[machine_max_jerk_z] E[machine_max_jerk_e]

; Now do the other regular start gcode stuff

G21; metric values
G90; absolute positioning
M82 ; set extruder to absolute mode
M107; start with the fan off

; M140 S[bed_temperature_initial_layer_single]; start bed heating

G28 X0 Y0 Z0; move X/Y/Z to endstops
G1 X1 Y6 F15000; move X/Y to start position
G1 Z35 F9000; move Z to start position

; Wait for bed and nozzle temperatures
; M190 S{hot_plate_temp_initial_layer[0] - 5}; wait for bed temperature - 5
; M140 S[bed_temperature_initial_layer_single]; continue bed heating
; M109 S[nozzle_temperature_initial_layer]; wait for nozzle temperature

; Purge and prime
M83; set extruder to relative mode
G92 E0; reset extrusion distance
G0 X0 Y1 F10000
G1 F150 E20 ; compress the bowden tube
G1 E-8 F1200
G0 X30 Y1 F5000
G0 F1200 Z{initial_layer_print_height/2}; Cut the connection to priming blob
G0 X100 F10000; disconnect with the prime blob
G0 X50; Avoid the metal clip holding the Ultimaker glass plate
G0 Z0.2 F720
G1 E8 F1200
G1 X80 E3 F1000; intro line 1
G1 X110 E4 F1000 ; intro line 2
G1 X140 F600; drag filament to decompress bowden tube
G1 X100 F3200; wipe backwards a bit
G1 X150 F3200; back to where there is no plastic: avoid dragging
G92 E0; reset extruder reference
M82; set extruder to absolute mode