Babi
===

Usage: babi `<filename>`

Keys:

    CTRL-X: Quit (asks to save) (also CTRL-Q)
    CTRL-S: Save (also CTRL-O)
    CTRL-F: Find string in file (ESC to exit search, arrows to navigate) (also CTRL-W)
    CTRL-K: Cut line(s) to cutbuffer
    CTRL-U: Uncut
    CTRL-E: Erase line
    CTRL-L: Redraw screen

Babi does not depend on any library (not even curses). It uses fairly standard
VT100 (and similar terminals) escape sequences. 

Babi was originally written as kilo by Salvatore Sanfilippo aka antirez and is released
under the BSD 2 clause license.

## This fork

This fork was heavily modified by Urja Rannikko. The name is because that's
what i often typo nano as, and it sounds kinda cute and small.

My point is to make babi into a very small nano-like editor, for small embedded
device usage (maybe get it embedded into busybox at some point?)

DONE:
- (atleast slightly) optimized drawing and memory operations
  (did i read it draws the whole screen always with a buffer that is realloc'd on every
   append to it...)
- line cutting and pasting (CTRL-K and U from nano)
- CTRL-W to find
- nano-compatible keybindings (CTRL-X to save and quit, CTRL-O to save)
- improved save routine (doesnt need heap)
- attempt to save on crashy exits (into <filename>.babi_save)
- improved out of memory handling
- shell script syntax highlighting (shell scripts are what you're mostly editing on BusyBox systems anyways)

TODO:
- BusyBoxy feature configuration:
  - syntax highlighting
    - C
    - shell
  - line cut and paste
  - search

