package secret

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"regexp"
	"strings"

	"a.yandex-team.ru/yt/go/yterrors"
)

const getTokenPy = `
from __future__ import print_function

import os
from devtools.ya.test.programs.test_tool.lib import secret
mount_point = os.environ.get('YA_TEST_TOOL_SECRET_POINT')
print("YT_TOKEN", secret.get_secret(mount_point, 'YA_COMMON_YT_TOKEN'))
`

var tokenRe = regexp.MustCompile(` YT_TOKEN (\S*)`)

func GetYTToken() (string, error) {
	if os.Getenv("YA_TEST_TOOL_SECRET_POINT") == "" {
		return "", fmt.Errorf("secret YT token is not available")
	}

	testtool, ok := os.LookupEnv("TEST_TOOL")
	if !ok {
		return "", fmt.Errorf("test_tool binary is not found")
	}

	var stdout, stderr bytes.Buffer

	cmd := exec.Command(testtool)
	cmd.Env = append(os.Environ(), "Y_PYTHON_ENTRY_POINT=:repl")
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	cmd.Stdin = strings.NewReader(getTokenPy)

	if err := cmd.Run(); err != nil {
		return "", err
	}

	m := tokenRe.FindSubmatch(stdout.Bytes())
	if len(m) != 2 {
		return "", yterrors.Err("token not found in output",
			yterrors.Attr("stdout", stdout.String()),
			yterrors.Attr("stderr", stderr.String()))
	}

	return string(m[1]), nil
}
