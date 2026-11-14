#pragma once

// The one OS-specific piece of the desktop front end: putting a POSIX
// terminal (Linux, macOS, WSL) into a mode where we can read single key
// presses and draw full-screen, and reliably putting it back afterwards.

#include <string>

class Terminal {
public:
    // Switches to raw-ish input (no line buffering, no echo), the alternate
    // screen, and a hidden cursor. Check ok() afterwards.
    Terminal();
    // Restores everything the constructor changed.
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    bool ok() const { return ok_; }

    // Size of the terminal window in characters. False if it can't be read.
    static bool size(int& cols, int& rows);

    // Write text to the terminal.
    void write(const std::string& text);

    // Whatever bytes have been typed since the last call. Never blocks.
    std::string readInput();

private:
    bool ok_ = false;
    bool changedMode_ = false;
    struct Saved;     // the original terminal settings
    Saved* saved_ = nullptr;
};
