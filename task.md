The current codebase contains numerous patches required to run on Linux, particularly with Wayland. Since we have now migrated to wxWidgets 3.3.2, please thoroughly investigate whether there are better ways to support native Wayland without breaking compatibility with X11 systems. Your task is to create a comprehensive and detailed plan for this transition. Ensure that all proposed solutions are validated against recent online documentation and community discussions whenever a decision about a fix or implementation strategy is required.

Avoid overengineering, keep the solution clean and narrow but proper and good for long term

The current OS is Ubuntu 25.04 with Wayland.
Test: launch the applciation and make sure it launches correctly.
