# 3D Scene Benchmark: High Level Design

## Why it exists

Rendering changes, such as the realistic view, shadows or SSAO, need a number to compare
before and after, and user reports of a slow viewport need a way to say how slow. The FPS
overlay and the render timings overlay show live values while someone drags the camera,
which varies from run to run with the model, the path of the mouse and the view.

The benchmark renders a fixed model along a fixed camera path in both 3D views, so two
runs on the same machine differ only by the code or the settings, and prints a report that
can be pasted into an issue.

## What it does

`run_scene_benchmark()` in `src/slic3r/GUI/SceneBenchmark.cpp` is reached from Help >
Benchmark 3D Scene, the command palette and Preferences > Graphics. After a confirmation
it starts a new project, which asks to save the current one if needed, loads the
OrcaSliced Combo handy model and arranges it. A small dialog in a corner of the 3D view
then shows the progress; every other window is disabled until the run ends, so a click
cannot change the scene being measured. Cancel or Esc stops the run.

The run goes through these stages, driven by a timer while it waits and by idle events
while it renders:

1. Loading: waits until the UI job worker is idle, so the arrange job has moved the
   objects. The orbit target is the center of the objects on the current plate, and the
   base zoom fits their bounding box in the viewport.
2. Prepare: renders the scene in the Prepare view.
3. Slicing: slices the plate and switches to Preview, then waits for the G-code preview
   to load. If slicing fails, the report holds Prepare alone.
4. Preview: renders the scene in the Preview view, with the slicing progress notification
   hidden.

The dialog then shows the report, with a button to copy it. A scene cut short, because its
view was hidden, is left out of the report.

## Rendering a scene

Each scene renders 30 warm-up frames, then the camera path twice, 360 frames each time.

- The first pass times the frames. A frame's time is the interval between the starts of
  consecutive benchmark frames, so it includes the event loop between them.
- The second pass averages the render timings. The frame profiler flushes the GL command
  queue after each section, which slows a frame down, so it only runs in this pass.
  `FrameProfiler::start_averaging()` flags every frame begun afterwards, and
  `finish_averaging()` waits for the flagged frames still on the GPU and returns the mean
  CPU and GPU time of each section per profiled frame.

The dialog renders one frame per idle event by calling `GLCanvas3D::render()`, which
redraws the whole scene. While `GLCanvas3D::set_benchmarking()` is on, the canvas does not
render from its own idle handler, so no other frame is drawn in between, and it skips the
picking pass and the FPS and render timings overlays, which depend on the mouse and on
preferences. The FPS cap does not apply, since it only paces idle redraws.

VSync is turned off for the scene through `wxGLCanvas::SetSwapInterval(0)`, so the frame
rate is what the GPU and CPU can reach rather than the display's refresh rate, and the
previous interval is restored afterwards. When the platform cannot report the current
interval (EGL), it is left as it is and the report says so.

The camera path makes two turns around the target while the view rises three times from
25 degrees below the plate to 85 degrees above it and the zoom goes twice between 0.6 and
1.4 times the base zoom. The camera stays at the default distance, so the perspective is
the same in every run. The camera the scene started with is restored at its end.

## The report

The report is plain English text, so it reads the same in every language:

- The version and build commit, the GPU and OpenGL version, the viewport size and camera
  type, and the graphics settings that change the cost of a frame: MSAA samples as read
  from the framebuffer, FXAA, the scene cache, VSync and the realistic view options.
- For each scene, the average FPS and the average, median, 95th percentile, 99th
  percentile and maximum frame time. Percentiles are nearest-rank, so each is a measured
  frame (`frame_time_stats()`).
- For each scene, the render timings table: the CPU and GPU milliseconds of each section
  of a frame, and their total. Without timer queries (OpenGL 3.3 or `ARB_timer_query`) the
  table says that the driver does not support them.
