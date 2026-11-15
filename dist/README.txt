Sketch Book (v8mnu22b) - Windows static recompilation
=====================================================

Run v8mnu22b.exe. Keep the vend folder next to it (it holds the game data, the
fake dongle and the helper tools in vend\bin). No install, no extra DLLs.

Controls
  C            insert coin
  1 - 8        booth panel buttons
  F11          fullscreen on/off (Alt+Enter works too)
  Ctrl+key     type a key straight to the game

Prints
  There is no photo printer, so each finished sketch is saved as a PNG in a
  "prints" folder next to the exe (prints\print_YYYYMMDD_HHMMSS.png).

Sketching your own picture
  Drag a picture (BMP, JPG, PNG or GIF) onto v8mnu22b.exe, or start it with
  the file name:
      v8mnu22b.exe C:\path\to\photo.jpg
  The game then sees that picture instead of a webcam. A video file works
  the same way. You can also drop a picture onto the game window while it
  is running; from then on that picture is used.

Camera
  Without a picture, the first webcam is used automatically (DirectShow, then
  Media Foundation). To pick a specific camera, give part of its name:
      set RT_CAMERA_DEVICE=Logitech
  RT_CAMERA does the same as giving a file on the command line, e.g. in cmd:
      set RT_CAMERA=C:\path\to\clip.mp4
      v8mnu22b.exe
  (Videos: anything Windows can play, e.g. MP4/WMV/AVI; it loops at its
  normal speed.)  Optional:
      RT_CAMERA_MIRROR=1   mirror the camera picture
      RT_CAMERA_FLIP=1     flip it upside down

Sound
  The voice prompts and the printing music play through the default sound
  device. The game has no other background music.

Screen
  The game switches between 640x480 and 800x600 itself; the window keeps its
  size and scales the picture. You can resize the window freely; the
  picture keeps its 4:3 shape, with black bars if the window is wider/taller.
  F11 or Alt+Enter switches fullscreen on and off (on the monitor the window
  is on). To start in fullscreen:
      v8mnu22b.exe --fullscreen      (or set VEND_FULLSCREEN=1)
      VEND_SCALE=2      open the window at twice the size
      VEND_ROOT         use a data folder other than .\vend

Logs go to vend\tmp\rt.log.
