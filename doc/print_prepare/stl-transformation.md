# STL Transformation

OrcaSlicer primarily relies on STL meshes for slicing, but STL files may come with several limitations.

Typically, STL files feature a low polygon count, which can adversely affect print quality.
In contrast, using STEP files offers a higher-quality mesh that more accurately represents the original design. However, be aware that both high-polygon STL and STEP files can increase slicing time.

![stl-transformation-smooth-rough](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/STL-Transformation/stl-transformation-smooth-rough.png?raw=true)

## Importing STEP files

This setting determines how STEP files are converted into STL files and is displayed during the STEP file import process.

If you don't see this when opening a STEP file, check [Don't show again](#dont-show-again) below.

![stl-transformation](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/STL-Transformation/stl-transformation.png?raw=true)

### Parameters:

The transformation uses [Linear Deflection and Angular Deflection](https://dev.opencascade.org/doc/overview/html/occt_user_guides__mesh.html) parameters to control the mesh quality.
A finer mesh will result in a more accurate representation of the original surface, but it will also increase the file size and processing time.

![stl-transformation-params](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/STL-Transformation/stl-transformation-params.png?raw=true)

- **Linear Deflection:** Specifies the maximum distance allowed between the original surface and its polygonal approximation. Lower values produce a mesh that more accurately follows the original curvature.
- **Angular Deflection:** Defines the maximum allowable angle difference between the actual surface and its tessellated counterpart. Smaller angular deflection values yield a more precise mesh.

#### Split compound and compsolid into multiple objects

Enabling this option will split the imported 3D file into separate objects. This is especially useful for adjusting individual object positions, tweaking print settings, or optimizing the model through simplification.

![stl-transformation-split](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/STL-Transformation/stl-transformation-split.png?raw=true)

#### Don't show again

This option will hide the STL transformation dialog when opening a STEP file.
To restore the dialog, go to "Preferences" (Ctrl + P) > "Show the STEP mesh parameter setting dialog".

![stl-transformation-enable](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/STL-Transformation/stl-transformation-enable.png?raw=true)

## Simplify model

When working with models that have a high polygon count, the Simplify Model option can significantly reduce complexity and help decrease slicing times.

This function is especially useful for improving the performance of the slicer or achieving a specific faceted look for artistic or technical reasons.

To access the Simplify Model option, right-click on the object to simplify in the "Prepare" menu.

![simplify-menu](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/STL-Transformation/simplify-menu.png?raw=true)

It is recommended to enable the "Show Wireframe" option when running a simplification process to visually inspect the outcome. However, be cautious: overly aggressive simplification may lead to noticeable detail loss, increased ringing, or other printing issues.

### You can Simplify your model using the following options

- **Detail Level:** Control the level of detail in the simplified model by choosing from five preset options. This setting allows for a balance between mesh fidelity and performance.
- **Decimate Ratio:** Adjust the ratio between the original model's polygon count and that of the simplified model. For instance, a decimate ratio of 0.5 will yield a model with approximately half the original number of polygons.
