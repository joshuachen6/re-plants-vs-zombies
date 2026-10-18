// Filesystem setup for the web build. Runs inside the Emscripten module, before main().
//
//   /pvz           the game's working directory. Holds main.pak and properties/, either bundled
//                  into the build (PVZ_WEB_DATA_DIR) or copied from a folder the player picks.
//   /pvz/savedata  IndexedDB-backed, so profiles, saved games and settings survive reloads.
//
// main() is not run automatically (INVOKE_RUN=0): the page calls Module.startGame() once it
// has a user gesture, which browsers require before they let the game play audio.

var PVZ_DIR = '/pvz';
var SAVE_DIR = PVZ_DIR + '/savedata';

var gameStarted = false;
var saveSyncRunning = false;
var saveSyncQueued = false;

function persistSaveData() {
  if (saveSyncRunning) {
    saveSyncQueued = true;
    return;
  }
  saveSyncRunning = true;
  FS.syncfs(false, function (err) {
    if (err) console.error('Saving to IndexedDB failed:', err);
    saveSyncRunning = false;
    if (saveSyncQueued) {
      saveSyncQueued = false;
      persistSaveData();
    }
  });
}

// The game only writes its settings and profile when it shuts down, which a closed tab never does
function saveAndPersist() {
  if (gameStarted && !ABORT) {
    try {
      _SexyWebSaveState();
    } catch (e) {
      console.error('Saving game state failed:', e);
    }
  }
  persistSaveData();
}

Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  FS.mkdirTree(SAVE_DIR);
  FS.mount(IDBFS, {}, SAVE_DIR);

  addRunDependency('pvz-savedata');
  FS.syncfs(true, function (err) {
    if (err) console.error('Loading save data from IndexedDB failed:', err);
    removeRunDependency('pvz-savedata');

    // Only start writing back once the stored data has been loaded, or we'd overwrite it
    setInterval(persistSaveData, 5000);
    document.addEventListener('visibilitychange', function () {
      if (document.visibilityState === 'hidden') saveAndPersist();
    });
    window.addEventListener('pagehide', saveAndPersist);
  });
});

Module['hasGameData'] = function () {
  return FS.analyzePath(PVZ_DIR + '/main.pak').exists ||
         FS.analyzePath(PVZ_DIR + '/properties/resources.xml').exists;
};

// Copies the files from an <input type="file" webkitdirectory> into /pvz. The top-level folder
// the player picked is stripped, so "Plants vs. Zombies/main.pak" becomes "/pvz/main.pak".
Module['installGameData'] = async function (files) {
  for (var i = 0; i < files.length; i++) {
    var file = files[i];
    var parts = (file.webkitRelativePath || file.name).split('/');
    if (parts.length > 1) parts.shift();
    if (parts[0] && parts[0].toLowerCase() === 'savedata') continue;

    var path = PVZ_DIR + '/' + parts.join('/');
    FS.mkdirTree(path.substring(0, path.lastIndexOf('/')));
    FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
  }
  return Module['hasGameData']();
};

Module['startGame'] = function () {
  gameStarted = true;
  callMain([]);
};
