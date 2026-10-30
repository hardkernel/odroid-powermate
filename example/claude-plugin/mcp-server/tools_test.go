// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

package main

import (
	"regexp"
	"testing"
)

func TestDefaultPromptMatchesCommonConsoles(t *testing.T) {
	re := regexp.MustCompile(`(?:` + defaultPrompt + `)\z`)
	for _, prompt := range []string{"root@server:~# ", "odroid@m2:~$ ", "=> ", "grub> ", "Shell> ", "% "} {
		if !re.MatchString("output\n" + prompt) {
			t.Errorf("prompt %q not matched", prompt)
		}
	}
	for _, notPrompt := range []string{"server login: ", "Password: ", "[    1.0] probing #", "Loading..."} {
		if re.MatchString(notPrompt) {
			t.Errorf("%q matched as a prompt", notPrompt)
		}
	}
}

func TestLoginPrompt(t *testing.T) {
	for _, s := range []string{"\nserver login: ", "Password: ", "odroid-m2 login:"} {
		if !loginPrompt.MatchString(s) {
			t.Errorf("%q not detected as a login prompt", s)
		}
	}
	if loginPrompt.MatchString("Last login: Thu Oct 29\nroot@m2:~# ") {
		t.Error("shell prompt after a login banner detected as a login prompt")
	}
}

func TestStripEchoAndExitCode(t *testing.T) {
	line := `ls /nope; echo "__PM_RC=$?"`
	text := line + "\nls: cannot access '/nope': No such file or directory\n__PM_RC=2\nroot@m2:~# "
	body := stripPrompt(stripEcho(text, line))
	m := exitCodeLine.FindStringSubmatch(body)
	if m == nil || m[1] != "2" {
		t.Fatalf("exit code not found in %q", body)
	}
	if want := "ls: cannot access '/nope': No such file or directory\n__PM_RC=2\n"; body != want {
		t.Fatalf("body = %q, want %q", body, want)
	}
}

func TestTailKeepsNewestOnLineBoundary(t *testing.T) {
	if got, skipped := tail("line1\nline2\nline3\n", 12); got != "line2\nline3\n" || skipped != 6 {
		t.Fatalf("cut on a line start: tail = %q, %d", got, skipped)
	}
	long := "0123456789abcdefghij\n" + "x\n" + "last line here\n"
	if got, _ := tail(long, 20); got != "x\nlast line here\n" {
		t.Fatalf("cut mid-line: tail = %q", got)
	}
}
