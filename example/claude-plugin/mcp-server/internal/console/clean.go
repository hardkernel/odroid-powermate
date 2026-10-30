// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

package console

import (
	"unicode/utf8"
)

// Text is UART bytes made readable for a language model.
type Text struct {
	String string
	// RawEnd[i] is the offset just past the raw byte that produced String[i], so a
	// match in String can be turned back into a buffer cursor.
	RawEnd []int
	// Noise counts raw bytes that were neither text nor a known terminal control
	// sequence: invalid UTF-8, NUL, stray control characters.
	Noise int
	Total int
}

// NoiseRatio is the fraction of raw bytes that looked like line noise.
func (t Text) NoiseRatio() float64 {
	if t.Total == 0 {
		return 0
	}
	return float64(t.Noise) / float64(t.Total)
}

// LooksLikeNoise reports output that is mostly garbage. On PowerMate this usually
// means the UART wire is loose or missing, or the target's GND is not shared with
// the PowerMate output, rather than a wrong baud rate in the target.
func (t Text) LooksLikeNoise() bool {
	return t.Total >= 32 && t.NoiseRatio() > 0.15
}

// RawOffset converts an index into String to the raw offset just past that byte.
func (t Text) RawOffset(index int) int {
	if index <= 0 || len(t.RawEnd) == 0 {
		return 0
	}
	if index > len(t.RawEnd) {
		index = len(t.RawEnd)
	}
	return t.RawEnd[index-1]
}

// Clean strips ANSI escape sequences, normalises line endings and replaces bytes
// that are not text. Line-drawing and colour codes from U-Boot, GRUB, EDK2 menus and
// shells are removed so patterns and the model see plain text.
func Clean(raw []byte) Text {
	out := make([]byte, 0, len(raw))
	ends := make([]int, 0, len(raw))
	noise := 0
	// lastBreak is 'n' after a newline from LF or CRLF, 'r' after one from a lone CR,
	// and 0 after anything else.
	var lastBreak byte
	emit := func(s string, end int) {
		for i := 0; i < len(s); i++ {
			out = append(out, s[i])
			ends = append(ends, end)
		}
		lastBreak = 0
	}

	for i := 0; i < len(raw); {
		c := raw[i]
		switch {
		case c == 0x1b:
			i = skipEscape(raw, i)
		case c == '\r':
			// CRLF and LF are line breaks. A lone CR (progress bars, menu redraws) is
			// also turned into one so overwritten text is not glued together, but a
			// CR next to another break (LF CR, CR CR LF) is the same break and dropped.
			if i+1 < len(raw) && raw[i+1] == '\n' {
				i += 2
				if lastBreak != 'r' {
					emit("\n", i)
				}
				lastBreak = 'n'
				continue
			}
			i++
			if lastBreak == 0 {
				emit("\n", i)
			}
			lastBreak = 'r'
		case c == '\n':
			i++
			if lastBreak != 'r' {
				emit("\n", i)
			}
			lastBreak = 'n'
		case c == '\t':
			i++
			emit("\t", i)
		case c == '\b' || c == 0x07:
			i++ // backspace and bell carry no text
		case c < 0x20 || c == 0x7f:
			i++
			noise++
		case c < 0x80:
			i++
			emit(string(c), i)
		default:
			r, size := utf8.DecodeRune(raw[i:])
			if r == utf8.RuneError && size <= 1 {
				i++
				noise++
				emit("�", i)
				continue
			}
			i += size
			emit(string(r), i)
		}
	}
	return Text{String: string(out), RawEnd: ends, Noise: noise, Total: len(raw)}
}

// skipEscape returns the index after the escape sequence starting at raw[i].
func skipEscape(raw []byte, i int) int {
	if i+1 >= len(raw) {
		return i + 1
	}
	switch raw[i+1] {
	case '[': // CSI: parameters then a final byte in 0x40..0x7e
		j := i + 2
		for j < len(raw) && (raw[j] < 0x40 || raw[j] > 0x7e) {
			j++
		}
		return min(j+1, len(raw))
	case ']': // OSC: terminated by BEL or ST (ESC \)
		for j := i + 2; j < len(raw); j++ {
			if raw[j] == 0x07 {
				return j + 1
			}
			if raw[j] == 0x1b && j+1 < len(raw) && raw[j+1] == '\\' {
				return j + 2
			}
		}
		return len(raw)
	case '(', ')', '*', '+': // charset designation, one more byte
		return min(i+3, len(raw))
	default: // two-byte sequence such as ESC 7, ESC =
		return i + 2
	}
}
