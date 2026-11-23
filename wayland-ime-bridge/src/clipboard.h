#ifndef TRIERARCH_CLIPBOARD_H
#define TRIERARCH_CLIPBOARD_H

/** Runs the Wayland clipboard watcher; returns the process exit status. */
int clipboard_main(const char *socket_path);

#endif
