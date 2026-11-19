# MIDI Image Player

A silent Windows x64 VST3 instrument built with JUCE.

## What it does

- Receives MIDI notes from the host.
- Uses the twelve chromatic pitch classes (Do through Si), ignoring octave.
- Shows one image while the corresponding MIDI key is held.
- Hides the image when the active key is released.
- Loads PNG, JPG and JPEG images through the editor.
- Stores image file paths in the plugin state so a DAW project can restore them.
- Produces silence and consumes MIDI events.

## Build

The GitHub Actions workflow builds the VST3 bundle on Windows x64. The resulting artifact is named:

`MIDI-Image-Player-CLEAN-VST3-Windows-x64`

## Installation

Copy the complete `.vst3` bundle into:

`C:\Program Files\Common Files\VST3`

Then rescan plugins in the DAW.

Note: the plugin stores image paths, not copies of the image files. Keep the original image files in place.
