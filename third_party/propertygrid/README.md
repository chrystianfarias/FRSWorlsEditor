# Win32 property grid

A property grid control written against the Win32 SDK alone, by David
MacDermot ("Win32 SDK PropertyGrid Made Easy", CodeProject), taken from the
fork at https://github.com/nptr/win32-property-grid (`propertyGrid.h`,
`propertyGrid.cpp`). The only change: two en dashes in comments of
`propertyGrid.h` re-encoded from Windows-1252 to UTF-8, since the project
compiles with `/utf-8`.

Licensed under the Code Project Open License 1.02 -- see `LICENSE`. It allows
use in any application, redistribution and modification.

The editor uses it for the properties panel (`src/ui/PropertiesPanel.cpp`).
