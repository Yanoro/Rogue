# Debug Improvements Features (Created via Cronjob)

## Features to Implement

1. Make debug window manager state carry over 
2. Camera system is not properly handling the case when the map is too small. Ideally it should center on the map and give a bird's eyes view of the entire map.
3. Add a debug window that allows one to reload the current map with one of the different Json map files on the directory
4. Create a window that enables toggling two flags for drawing the outer and inner rectangles of DrawAscii objects (Drawn in Game::ECSInitRenderSystems)
5. Create a debug window that shows logs as well as all the associated helper functions(writeToLog, clearLog, etc)
