# Changelog

Release notes for XTReader firmware. The release workflow publishes the section
matching the pushed tag verbatim, so what is written here is what users read on
the GitHub release page. Write for someone holding the device, not for someone
reading the commit log: name the behaviour that changed, not the code that
changed. Anything with no user-visible effect belongs under "Under the hood".

## 1.8.0

### Everything this fork adds now lives behind one icon

The bottom strip has a fifth icon, the Xtreader X, sitting in the middle. It
opens a screen with three tabs that gathers every feature this fork adds on top
of CrossPoint, so they are in one place instead of scattered through Settings.

**Account** holds the server URL, which account this reader is paired to, and
Pair or Unlink. **Library** holds Request Books, the download queue (tap it to
cancel everything queued), Sync Now, and two settings that until now could only
be changed from the web page: whether progress is matched by filename or by file
content, and whether the reader sends book metadata when it syncs. **Wallpapers**
holds the gallery and the "synced wallpapers only" switch.

Account Sync used to be four levels deep, at Settings > System > Account Sync.
The wallpaper gallery sat under Display, next to sleep-screen settings it is only
loosely related to. Both are gone from Settings and live here now, one level
shallower than before.

### The library sync no longer freezes the reader

Waking the device used to mean staring at a "Syncing library" box for as long
as it took. Measured on hardware, that was **17 to 22 seconds** -- the whole
device unresponsive, because the sync ran on the same task that draws the
screen, so nothing could be drawn and no button could open anything.

The sync now runs in the background. The reader is usable the moment Home
appears: measured at 69 button presses over 25 seconds during a live sync, with
no missed input. What was a box in the middle of the screen is now a short line
of text in the top-left corner, level with the battery, that says what is
happening and gets out of the way.

The one thing that has not changed is how long the sync itself takes. It is the
same work over the same network; you just are not held hostage by it.

### The "continue from another device?" prompt stops crying wolf

Opening a book synced from a Kindle almost always asked whether you wanted to
jump to a position you were already at. The reader was comparing two numbers that
were never the same thing: KOReader counts a percentage in its own pages, this
reader counts it in bytes through the file. On a book with images or long
chapters they disagree by several percent on every page.

It now compares which chapter you are in, and only offers to move when that
actually differs. The dialog names the chapter instead of showing a percentage.

Two related fixes: answering the prompt is remembered, so the same book does not
ask again on the next open; and a reading position that failed to reach the
server before sleep is retried on the next sleep instead of being quietly
dropped.

### Fixed

- **Home's book list no longer reshuffles on every boot.** With more than five
  books waiting on the server, each sync re-discovered the ones the previous
  sync had pushed out, put them back at the top, and pushed out the current
  set -- so the order was different every time you turned the device on and
  nothing ever stayed put. The reader now remembers how far through the
  library it has already looked, so a book it has already shown you does not
  come back as new.
- Footnote links are drawn as superscript again.
- Two Perso-Arabic marks (U+0654, U+06D5) were missing from the interface fonts
  and rendered as blanks.
- The glyph cache no longer gives up when free memory is low, which is exactly
  when a page is hardest to draw.
- A last-resort chapter position could be written in a form the reader could not
  read back, losing the position it had just saved.

### Under the hood

- Six commits merged from upstream CrossPoint.
- The library sync runs on its own FreeRTOS task rather than on the render
  task, which is what makes the screen usable while it runs.
- Every row this fork had inserted into CrossPoint's Settings screen has been
  moved out of it, so that file now merges from upstream without a hand
  resolution.
- Settings lookup tables are compile-time constants, and the built-in fonts'
  content-hash IDs were stale and are regenerated.

## 1.7.0

### New books show up on Home, and download from there

A book added on the server used to be invisible until you remembered which
folder it landed in and walked there in the file browser. Home now carries three
book rows under the cover tile. A book the server has that this reader does not
appears there, and selecting it downloads it without leaving the screen.

The rows follow one order: whatever happened most recently. Open a book and it
moves to the top; a book that appears on the server goes to the top when it is
found. Opening an older book moves it back above a newer arrival, which is what
"recent" ought to mean.

Rows say what is happening -- on the server with its size, waiting with its place
in the queue, downloading, or failed with an invitation to try again. A failed
download used to revert silently to "on server", leaving no way to tell a failure
from a download that never started.

There is deliberately no percentage during a transfer. This reader has very
little memory free while downloading, and repainting the screen there crashes it.

### The four menu rows are now an icon strip

Browse, Recent, Transfer and Settings collapse into four icons along the bottom,
which is what makes room for the book rows. The selected icon's name is written
above the strip rather than under each icon, so it can be spelled out in full.
The cursor now wraps: right from Settings returns to the top.

### Also new

- Reader menu as a toolbar drawn over the page, and a control centre of quick
  settings, both from upstream CrossPoint.
- USB mass storage on the X4 Pro, and support for the Xteink X4 Classic board.
- Keyboard layout sets, selectable per language.
- The Sheet theme is now called XTReader Sheet -- it is this fork's, where the
  others come from CrossPoint.

### Fixed

- A book's download status no longer waits for you to leave the folder and come
  back. It said "on server" while downloading, and kept saying "downloading"
  after the file had landed.
- Books already on the device no longer show as "on server". The manifest's own
  flag is always false by design; the card is now what gets asked.
- The SD card is shut down properly on deep sleep.
- Wi-Fi power saving is disabled during a book download.
- Two KOSync Wi-Fi fixes, a ZipFile size underflow, a low-memory CSS cache
  retry, dictionary font caches released on exit, cold-boot power-button
  misclassification, and three multi-byte text fixes that matter for Vietnamese.

### Under the hood

- 46 commits merged from upstream CrossPoint, and the SDK moved with them.
- Built-in fonts are 323 KB smaller in flash.

## 1.6.0

### Continue reading from another device

Open a book and the reader now checks the server in the background for a
position saved somewhere else. If the newest position came from a *different*
device and is not where you are, it offers to jump there, showing the position
and the date it was saved. Declining keeps your place and does not ask again for
that book.

The check never delays opening a book, and it stays silent when there is no
Wi-Fi to reach the server with.

### Also new

- Features brought back from the X4 Pro beta.
- Settings languages are listed in a sensible order.

### Fixed

- Ubuntu Medium glyphs collided with each other in the UI.
- A tapped settings row kept the focus on touch boards.
- Dictionary definitions with styled HTML refused to show their first page.

### Under the hood

- Wi-Fi handling for the new progress check: it releases the radio when a book
  is closed, aborts cleanly on power-off, and no longer competes with the button
  poll for input.
- Progress uploads now carry the device's own id instead of a shared constant.
  That is what makes "another device" mean anything.
- Merged upstream CrossPoint Reader changes.
- The project is now called XTReader.
