# ImGuizmo

Latest stable tagged version is 1.83. Current master version is 1.84 WIP.

What started with the gizmo is now a collection of dear imgui widgets and more advanced controls.

## Guizmos

### ImViewGizmo

Manipulate view orientation with 1 single line of code

![Image of ImViewGizmo](http://i.imgur.com/7UVcyDd.gif)

### ImGuizmo

ImGizmo is a small (.h and .cpp) library built ontop of Dear ImGui that allow you to manipulate(Rotate & translate at the moment) 4x4 float matrices. No other dependancies. Coded with Immediate Mode (IM) philosophy in mind.

Built against DearImgui 1.53WIP

![Image of Rotation](http://i.imgur.com/y4mcVoT.gif)
![Image of Translation](http://i.imgur.com/o8q8iHq.gif)
![Image of Bounds](http://i.imgur.com/3Ez5LBr.gif)

There is now a sample for Win32/OpenGL ! With a binary in bin directory.
![Image of Sample](https://i.imgur.com/nXlzyqD.png)

### ImSequencer

A WIP little sequencer used to edit frame start/end for different events in a timeline.
![Image of Rotation](http://i.imgur.com/BeyNwCn.png)
Check the sample for the documentation. More to come...

### Graph Editor

Nodes + connections. Custom draw inside nodes is possible with the delegate system in place.
![Image of GraphEditor](Images/nodeeditor.jpg)

### API doc

Call BeginFrame right after ImGui_XXXX_NewFrame();

```C++
void BeginFrame();
```

return true if mouse cursor is over any gizmo control (axis, plan or screen component)

```C++
bool IsOver();**
```

return true if mouse IsOver or if the gizmo is in moving state

```C++
bool IsUsing();**
```

enable/disable the gizmo. Stay in the state until next call to Enable. gizmo is rendered with gray half transparent color when disabled

```C++
void Enable(bool enable);**
```

helper functions for manualy editing translation/rotation/scale with an input float
translation, rotation and scale float points to 3 floats each
Angles are in degrees (more suitable for human editing)
example:

```C++
 float matrixTranslation[3], matrixRotation[3], matrixScale[3];
 ImGuizmo::DecomposeMatrixToComponents(gizmoMatrix.m16, matrixTranslation, matrixRotation, matrixScale);
 ImGui::InputFloat3("Tr", matrixTranslation, 3);
 ImGui::InputFloat3("Rt", matrixRotation, 3);
 ImGui::InputFloat3("Sc", matrixScale, 3);
 ImGuizmo::RecomposeMatrixFromComponents(matrixTranslation, matrixRotation, matrixScale, gizmoMatrix.m16);
```

These functions have some numerical stability issues for now. Use with caution.

```C++
void DecomposeMatrixToComponents(const float *matrix, float *translation, float *rotation, float *scale);
void RecomposeMatrixFromComponents(const float *translation, const float *rotation, const float *scale, float *matrix);**
```

Render a cube with face color corresponding to face normal. Usefull for debug/test

```C++
void DrawCube(const float *view, const float *projection, float *matrix);**
```

Call it when you want a gizmo
Needs view and projection matrices.
Matrix parameter is the source matrix (where will be gizmo be drawn) and might be transformed by the function. Return deltaMatrix is optional. snap points to a float[3] for translation and to a single float for scale or rotation. Snap angle is in Euler Degrees.

```C++
    enum OPERATION
    {
        TRANSLATE,
        ROTATE,
        SCALE
    };

    enum MODE
    {
        LOCAL,
        WORLD
    };

void Manipulate(const float *view, const float *projection, OPERATION operation, MODE mode, float *matrix, float *deltaMatrix = 0, float *snap = 0);**
```

### ImGui Example

Code for :

![Image of dialog](http://i.imgur.com/GL5flN1.png)

```C++
void EditTransform(const Camera& camera, matrix_t& matrix)
{
    static ImGuizmo::OPERATION mCurrentGizmoOperation(ImGuizmo::ROTATE);
    static ImGuizmo::MODE mCurrentGizmoMode(ImGuizmo::WORLD);
    if (ImGui::IsKeyPressed(90))
        mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(69))
        mCurrentGizmoOperation = ImGuizmo::ROTATE;
    if (ImGui::IsKeyPressed(82)) // r Key
        mCurrentGizmoOperation = ImGuizmo::SCALE;
    if (ImGui::RadioButton("Translate", mCurrentGizmoOperation == ImGuizmo::TRANSLATE))
        mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate", mCurrentGizmoOperation == ImGuizmo::ROTATE))
        mCurrentGizmoOperation = ImGuizmo::ROTATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale", mCurrentGizmoOperation == ImGuizmo::SCALE))
        mCurrentGizmoOperation = ImGuizmo::SCALE;
    float matrixTranslation[3], matrixRotation[3], matrixScale[3];
    ImGuizmo::DecomposeMatrixToComponents(matrix.m16, matrixTranslation, matrixRotation, matrixScale);
    ImGui::InputFloat3("Tr", matrixTranslation, 3);
    ImGui::InputFloat3("Rt", matrixRotation, 3);
    ImGui::InputFloat3("Sc", matrixScale, 3);
    ImGuizmo::RecomposeMatrixFromComponents(matrixTranslation, matrixRotation, matrixScale, matrix.m16);

    if (mCurrentGizmoOperation != ImGuizmo::SCALE)
    {
        if (ImGui::RadioButton("Local", mCurrentGizmoMode == ImGuizmo::LOCAL))
            mCurrentGizmoMode = ImGuizmo::LOCAL;
        ImGui::SameLine();
        if (ImGui::RadioButton("World", mCurrentGizmoMode == ImGuizmo::WORLD))
            mCurrentGizmoMode = ImGuizmo::WORLD;
    }
    static bool useSnap(false);
    if (ImGui::IsKeyPressed(83))
        useSnap = !useSnap;
    ImGui::Checkbox("", &useSnap);
    ImGui::SameLine();
    vec_t snap;
    switch (mCurrentGizmoOperation)
    {
    case ImGuizmo::TRANSLATE:
        snap = config.mSnapTranslation;
        ImGui::InputFloat3("Snap", &snap.x);
        break;
    case ImGuizmo::ROTATE:
        snap = config.mSnapRotation;
        ImGui::InputFloat("Angle Snap", &snap.x);
        break;
    case ImGuizmo::SCALE:
        snap = config.mSnapScale;
        ImGui::InputFloat("Scale Snap", &snap.x);
        break;
    }
    ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
    ImGuizmo::Manipulate(camera.mView.m16, camera.mProjection.m16, mCurrentGizmoOperation, mCurrentGizmoMode, matrix.m16, NULL, useSnap ? &snap.x : NULL);
}
```

## Install

ImGuizmo can be installed via [vcpkg](https://github.com/microsoft/vcpkg) and used cmake

```bash
vcpkg install imguizmo
```

See the [vcpkg example](/vcpkg-example) for more details

## License

ImGuizmo is licensed under the MIT License, see [LICENSE](/LICENSE) for more information.
