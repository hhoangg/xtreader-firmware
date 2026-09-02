# Changelog

Release notes for XTReader firmware. The release workflow publishes the section
matching the pushed tag verbatim, so what is written here is what users read on
the GitHub release page. Write for someone holding the device, not for someone
reading the commit log: name the behaviour that changed, not the code that
changed. Anything with no user-visible effect belongs under "Under the hood".

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
