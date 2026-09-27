// The framework's half of every web build, run before main(). Linked by
// rmp_add_game() in CMakeLists.txt with --pre-js.
//
// AUDIO: A KEY PRESS UNLOCKS IT TOO. Browsers keep a page's audio suspended
// until the player does something, and miniaudio -- raylib's audio backend --
// resumes it only on 'click' and 'touchend'. A game played with the keyboard
// from its title screen on would stay silent until the first mouse click,
// although a key press is exactly the kind of gesture the browser accepts.
// So a key press asks miniaudio to unlock as well. Its unlock() resumes every
// device it has started, and does nothing where there is none yet (rmp::audio
// opens the device lazily, on the first sound): this listener stays until a
// press finds miniaudio there, and then leaves. A gamepad cannot do this --
// browsers do not count its buttons as a gesture -- and that is the
// browser's rule, not ours.
(function () {
  if (typeof document === 'undefined') return;
  var onKey = function () {
    if (typeof window.miniaudio === 'undefined' || typeof window.miniaudio.unlock !== 'function') {
      return;
    }
    window.miniaudio.unlock();
    document.removeEventListener('keydown', onKey, true);
  };
  document.addEventListener('keydown', onKey, true);
})();
