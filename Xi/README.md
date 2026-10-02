# X Input extension

This directory contains TinyX's legacy X Input Extension implementation. It is
based on the `Xi/` implementation from X.Org Server 1.3.0.0 and retains that
code's permissive notices in each source file.

TinyX intentionally advertises and dispatches XInput 1.3 only. Requests added
by XInput 1.5 (device properties) and XInput 2 are not implemented and return
`BadRequest`. The memory DDX currently has only the core pointer and keyboard;
XInput lists those devices with `IsXPointer` and `IsXKeyboard`, as required by
the legacy protocol, but they cannot be opened as extension devices.

The sources have small compatibility adaptations for TinyX's allocator,
byte-swap helpers, window optional-data layout, and descriptor-free build.
