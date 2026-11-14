#include "terminal.h"

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>

struct Terminal::Saved {
    termios original;
};

Terminal::Terminal() {
    saved_ = new Saved;
    if (tcgetattr(STDIN_FILENO, &saved_->original) != 0) {
        return;  // stdin isn't a terminal
    }

    termios raw = saved_->original;
    // Deliver keys immediately and don't echo them. Ctrl-C still raises
    // SIGINT (ISIG stays on) so the program can shut down cleanly, and output
    // post-processing stays on so '\n' still returns the cursor to column 0.
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;   // read() returns immediately...
    raw.c_cc[VTIME] = 0;  // ...even when nothing has been typed
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        return;
    }
    changedMode_ = true;
    ok_ = true;

    // Alternate screen (so the user's scrollback is untouched), hide cursor,
    // clear.
    write("\x1b[?1049h\x1b[?25l\x1b[2J");
}

Terminal::~Terminal() {
    if (changedMode_) {
        write("\x1b[?25h\x1b[?1049l");  // show cursor, leave alternate screen
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_->original);
    }
    delete saved_;
}

bool Terminal::size(int& cols, int& rows) {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) {
        return false;
    }
    cols = ws.ws_col;
    rows = ws.ws_row;
    return true;
}

void Terminal::write(const std::string& text) {
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(STDOUT_FILENO, text.data() + done, text.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;  // the terminal went away; nothing useful left to do
        }
        done += static_cast<size_t>(n);
    }
}

std::string Terminal::readInput() {
    std::string bytes;
    char buf[64];
    for (;;) {
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        if (n <= 0) break;  // nothing (more) typed
        bytes.append(buf, static_cast<size_t>(n));
    }
    return bytes;
}
