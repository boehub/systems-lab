"""Errors raised by the Nibble compiler."""


class NibbleError(Exception):
    """A mistake in the source program, with where it happened."""

    def __init__(self, message, line=None, col=None):
        self.message = message
        self.line = line
        self.col = col
        where = f"line {line}: " if line else ""
        super().__init__(f"{where}{message}")

    def render(self, source):
        """The message plus the offending source line and a caret under it."""
        if not self.line:
            return str(self)
        lines = source.splitlines()
        if not 1 <= self.line <= len(lines):
            return str(self)
        text = lines[self.line - 1]
        out = [str(self), "    " + text]
        if self.col:
            out.append("    " + " " * (self.col - 1) + "^")
        return "\n".join(out)
