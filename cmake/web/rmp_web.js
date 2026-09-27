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

// SAVES: IndexedDB mounted at /rmp_save/<game> BEFORE main() runs.
//
// <game> is [project] name, set by cmake/generated/rmp_web_name.js, which runs
// just before this file. Emscripten names the IndexedDB database after the
// mount point, so the name is what keeps two rmp games on one origin (every
// project site on user.github.io) from sharing one database and each other's
// slots.
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
// It must never hold the game back for good. With no IndexedDB at all (node,
// a file:// page in some browsers) the mount is not even tried: IDBFS asserts
// on it, and in a debug build an assert is abort(), after which main() never
// runs -- releasing the run dependency did not help. The folder is made
// anyway, so saves work for as long as the tab lasts. Where IndexedDB exists
// but refuses (a private window, storage switched off), the fill fails
// through its callback: the error is logged, the dependency released, and
// the game runs the same way. Needs -lidbfs.js, which rmp_add_game() links
// next to this file.
// After every write and remove, src/rmp/save.cpp calls Module.rmpPersist().
// One FS.syncfs at a time: a game that saves twice in a frame -- the score,
// then the settings -- would otherwise start two, and Emscripten reports that
// on console.error. A sync asked for while one runs is folded into one more
// run after it, which carries everything written meanwhile.
Module['rmpPersist'] = function () {
  var sync = Module['rmpSyncState'] || (Module['rmpSyncState'] = { busy: false, again: false });
  if (sync.busy) {
    sync.again = true;
    return;
  }
  sync.busy = true;
  sync.again = false;
  FS.syncfs(false, function (err) {
    if (err) console.warn('rmp::save: could not persist to IndexedDB: ' + err);
    sync.busy = false;
    if (sync.again) Module['rmpPersist']();
  });
};

Module['preRun'] = Module['preRun'] || [];
if (typeof Module['preRun'] === 'function') Module['preRun'] = [Module['preRun']];
Module['preRun'].push(function () {
  var dir = '/rmp_save/' + (Module['rmpSaveName'] || 'game');
  ['/rmp_save', dir].forEach(function (d) {
    try {
      FS.mkdir(d);
    } catch (e) {
      // already there
    }
  });
  if (typeof indexedDB === 'undefined') {
    console.warn('rmp::save: no IndexedDB here; saves last as long as the page does');
    return;
  }
  addRunDependency('rmp-save');
  var done = function (err) {
    if (err) console.warn('rmp::save: saves will not survive a reload: ' + err);
    removeRunDependency('rmp-save');
  };
  try {
    FS.mount(IDBFS, {}, dir);
    FS.syncfs(true, done);
  } catch (e) {
    done(e);
  }
});
