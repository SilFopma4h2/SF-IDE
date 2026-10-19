// Wake - asks the event loop to draw more frames.
//
// SF-IDE only redraws when something happens (input, process output, a
// running build). Code that animates or changes state outside an input
// event calls wakeUi() so the next frames are drawn. Safe to call from any
// thread.

#pragma once

void wakeUi(int frames = 2);
