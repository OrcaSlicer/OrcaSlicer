# Retraction test

Retraction is the process of pulling the filament back into the nozzle to prevent oozing and stringing during non-print moves. If the retraction length is too short, it may not effectively prevent oozing, while if it's too long, it can lead to clogs or under-extrusion. Filaments like PETG and TPU are more prone to stringing, so they may require longer retraction lengths compared to PLA or ABS.

This test generates a retraction tower automatically. The retraction tower is a vertical structure with multiple notches, each printed at a different retraction length. After the print is complete, we can examine each section of the tower to determine the optimal retraction length for the filament. The optimal retraction length is the shortest one that produces the cleanest tower.

![retraction_test](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/retraction/retraction_test.gif?raw=true)

![retraction_test_menu](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/retraction/retraction_test_menu.png?raw=true)

In the dialog, you can select the start and end retraction length, as well as the retraction length increment step. The default values are 0mm for the start retraction length, 2mm for the end retraction length, and 0.1mm for the step. These values are suitable for most direct drive extruders. However, for Bowden extruders, you may want to increase the start and end retraction lengths to 1mm and 6mm, respectively, and set the step to 0.2mm.

![retraction_test_print](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/retraction/retraction_test_print.jpg?raw=true)

> [!NOTE]
> When testing filaments such as PLA or ABS that have minimal oozing, the retraction settings can be highly effective. You may find that the retraction tower appears clean right from the start. In such situations, setting the retraction length to 0.2mm - 0.4mm using OrcaSlicer should suffice.
> On the other hand, if there is still a lot of stringing at the top of the tower, it is recommended to dry your filament and ensure that your nozzle is properly installed without any leaks.

> [!TIP]
> @ItsDeidara has made a html to help with the calculation. Check it out if those equations give you a headache [here](https://github.com/ItsDeidara/Orca-Slicer-Assistant).
