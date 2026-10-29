package console

import (
	"context"
	"strings"
	"testing"
	"time"
)

func TestCleanLineBreaksAndEscapes(t *testing.T) {
	cases := map[string]string{
		"a\r\nb\r\n":                   "a\nb\n",
		"a\nb":                         "a\nb",
		"a\n\nb":                       "a\n\nb",
		"a\r\r\nb":                     "a\nb",
		"a\n\rb":                       "a\nb",
		"10%\r20%\r":                   "10%\n20%\n",
		"\x1b[1;32mroot\x1b[0m@m2:~# ": "root@m2:~# ",
		"\x1b]0;title\x07=> ":          "=> ",
		"x\x1b(By":                     "xy",
		"ab\bc":                        "abc",
		"한글":                           "한글",
	}
	for in, want := range cases {
		if got := Clean([]byte(in)).String; got != want {
			t.Errorf("Clean(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestCleanRawOffsetMapsBack(t *testing.T) {
	raw := []byte("\x1b[0mU-Boot\r\nlogin: ")
	text := Clean(raw)
	idx := strings.Index(text.String, "login:")
	end := text.RawOffset(idx + len("login:"))
	if got := string(raw[:end]); !strings.HasSuffix(got, "login:") {
		t.Fatalf("raw prefix %q does not end at the match", got)
	}
}

func TestCleanNoise(t *testing.T) {
	garbage := []byte{0x00, 0xff, 0xfe, 0x13, 0x80, 0x81, 0x02, 0x9f}
	text := Clean([]byte(strings.Repeat(string(garbage), 8)))
	if !text.LooksLikeNoise() {
		t.Fatalf("noise ratio %.2f not flagged", text.NoiseRatio())
	}
	if Clean([]byte(strings.Repeat("[    1.234] normal kernel log line\r\n", 4))).LooksLikeNoise() {
		t.Fatal("clean text flagged as noise")
	}
}

func TestBufferDiscardsOldDataAndReportsLoss(t *testing.T) {
	b := NewBuffer(8)
	b.Write([]byte("0123456789"))
	snap := b.Since(0)
	if string(snap.Data) != "23456789" || snap.Lost != 2 || snap.From != 2 || snap.End != 10 {
		t.Fatalf("unexpected snapshot %+v data=%q", snap, snap.Data)
	}
	if snap := b.Since(20); len(snap.Data) != 0 || snap.From != 10 {
		t.Fatalf("cursor past end not clamped: %+v", snap)
	}
}

func TestBufferWaitWakesOnWrite(t *testing.T) {
	b := NewBuffer(1024)
	go func() {
		time.Sleep(20 * time.Millisecond)
		b.Write([]byte("Hit any key to stop autoboot"))
	}()
	ok, snap := b.Wait(context.Background(), 0, time.Second, func(s Snapshot) bool {
		return strings.Contains(string(s.Data), "autoboot")
	})
	if !ok || snap.End == 0 {
		t.Fatalf("wait did not see the write: ok=%v snap=%+v", ok, snap)
	}
	ok, _ = b.Wait(context.Background(), b.End(), 30*time.Millisecond, func(s Snapshot) bool { return len(s.Data) > 0 })
	if ok {
		t.Fatal("wait succeeded without new data")
	}
}
