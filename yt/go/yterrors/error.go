package yterrors

import (
	"fmt"
	"regexp"
	"sort"
	"strings"

	"golang.org/x/xerrors"

	"a.yandex-team.ru/yt/go/yson"
)

type ErrorCode int

func (e ErrorCode) MarshalYSON(w *yson.Writer) error {
	w.Int64(int64(e))
	return w.Err()
}

func (e *ErrorCode) UnmarshalYSON(r *yson.Reader) (err error) {
	var code int
	err = (&yson.Decoder{R: r}).Decode(&code)
	*e = ErrorCode(code)
	return
}

// Error is an implementation of built-in go error interface.
//
// YT errors are designed to be easily transferred over network and
// between different languages. Because of this, Error is a concrete
// type and not an interface.
//
// YT error consists of error code, error message, list of attributes
// and a list of inner errors.
//
// Since YT error might contain arbitrary nested tree structure, user
// should traverse the whole tree when searching for a specific error
// condition.
//
// Error supports brief and full formatting using %v and %+v format specifiers.
type Error struct {
	Code        ErrorCode              `yson:"code" json:"code"`
	Message     string                 `yson:"message" json:"message"`
	InnerErrors []*Error               `yson:"inner_errors" json:"inner_errors"`
	Attributes  map[string]interface{} `yson:"attributes" json:"attributes"`
}

// ContainsErrorCode returns true iff any of the nested errors has ErrorCode equal to errorCode.
//
// ContainsErrorCode invokes xerrors.As internally. It is safe to pass arbitrary error value to this function.
func ContainsErrorCode(err error, code ErrorCode) bool {
	return FindErrorCode(err, code) != nil
}

func FindErrorCode(err error, code ErrorCode) *Error {
	if err == nil {
		return nil
	}

	var ytErr *Error
	if ok := xerrors.As(err, &ytErr); ok {
		if code == ytErr.Code {
			return ytErr
		}

		for _, nested := range ytErr.InnerErrors {
			if ytErr = FindErrorCode(nested, code); ytErr != nil {
				return ytErr
			}
		}
	}

	return nil
}

// ContainsMessageRE returns true iff any of the nested errors has message matching provided regex.
//
// Use of this function is strongly discouraged, use ContainsErrorCode when possible.
func ContainsMessageRE(err error, messageRE *regexp.Regexp) bool {
	if err == nil {
		return false
	}

	var ytErr *Error
	if ok := xerrors.As(err, &ytErr); !ok {
		return false
	}

	var matchRecursive func(*Error) bool
	matchRecursive = func(ytErr *Error) bool {
		if messageRE.MatchString(ytErr.Message) {
			return true
		}

		for _, nested := range ytErr.InnerErrors {
			if matchRecursive(nested) {
				return true
			}
		}

		return false
	}

	return matchRecursive(ytErr)
}

func (yt *Error) Error() string {
	return fmt.Sprint(yt)
}

func (yt *Error) Format(s fmt.State, v rune) { xerrors.FormatError(yt, s, v) }

func (yt *Error) FormatError(p xerrors.Printer) (next error) {
	p.Printf("yt: %s", yt.Message)

	printAttrs := func(e *Error) {
		maxLen := 0
		if e.Code != 1 {
			maxLen = 4
		}

		for name := range e.Attributes {
			if len(name) > maxLen {
				maxLen = len(name)
			}
		}

		padding := func(name string) string {
			return strings.Repeat(" ", maxLen-len(name))
		}

		if e.Code != 1 {
			p.Printf("  code: %s%d\n", padding("code"), e.Code)
		}

		var names []string
		for name := range e.Attributes {
			names = append(names, name)
		}
		sort.Strings(names)

		for _, name := range names {
			p.Printf("  %s: %s%v\n", name, padding(name), e.Attributes[name])
		}
	}

	var visit func(*Error)
	visit = func(e *Error) {
		p.Printf("%s\n", e.Message)
		printAttrs(e)

		for _, inner := range e.InnerErrors {
			visit(inner)
		}
	}

	if p.Detail() {
		p.Printf("\n")
		printAttrs(yt)

		for _, inner := range yt.InnerErrors {
			visit(inner)
		}
	} else {
		// Recursing only into the last inner error, since user asked for brief error.
		if len(yt.InnerErrors) != 0 {
			return yt.InnerErrors[len(yt.InnerErrors)-1]
		}
	}

	return nil
}

type ErrorAttr struct {
	Name  string
	Value interface{}
}

// Err creates new error of type Error.
func Err(args ...interface{}) error {
	err := new(Error)
	err.Code = 1

	for _, arg := range args {
		switch v := arg.(type) {
		case ErrorCode:
			err.Code = v
		case string:
			err.Message = v
		case *Error:
			err.InnerErrors = append(err.InnerErrors, v)
		case ErrorAttr:
			if err.Attributes == nil {
				err.Attributes = map[string]interface{}{}
			}

			err.Attributes[v.Name] = v.Value
		case error:
			err.InnerErrors = append(err.InnerErrors, &Error{
				Code:    1,
				Message: v.Error(),
			})
		default:
			panic(fmt.Sprintf("can't create yt.Error from type %T", arg))
		}
	}

	return err
}
