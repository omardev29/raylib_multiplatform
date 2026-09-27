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

// SAVES: IndexedDB mounted at /rmp_save BEFORE main() runs.
//
// Emscripten's files live in memory, so a save written there is gone when the
// tab reloads -- a bug nobody sees in development, because nobody reloads.
// IDBFS mirrors a folder into the browser's IndexedDB, but only when asked:
// FS.syncfs(true) fills the folder from IndexedDB, and src/rmp/save.cpp calls
// FS.syncfs(false) after every write to push it back.
//
// The fill is asynchronous, and the game reads its save in its first scene's
// _ready(), which runs inside main(). So main() is held back with a run
// dependency until the fill has finished -- otherwise the first read of every
// session would find nothing and a player's progress would look deleted.
//
// It must never hold the game back for good. With no IndexedDB (a private
// window in some browsers, a file:// page, storage switched off) the mount or
// the fill fails; the error is logged, the dependency is released, and the
// game runs with saves that last as long as the tab does. Needs -lidbfs.js,
// which rmp_add_game() links next to this file.
Module['preRun'] = Module['preRun'] || [];
if (typeof Module['preRun'] === 'function') Module['preRun'] = [Module['preRun']];
Module['preRun'].push(function () {
  addRunDependency('rmp-save');
  var done = function (err) {
    if (err) console.warn('rmp::save: saves will not survive a reload: ' + err);
    removeRunDependency('rmp-save');
  };
  try {
    FS.mkdir('/rmp_save');
    FS.mount(IDBFS, {}, '/rmp_save');
    FS.syncfs(true, done);
  } catch (e) {
    done(e);
  }
});
