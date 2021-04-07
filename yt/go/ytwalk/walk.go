// Package ytwalk implements cypress traversal.
package ytwalk

import (
	"context"
	"errors"
	"fmt"
	"reflect"

	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yson"
	"a.yandex-team.ru/yt/go/yt"
)

// ErrSkipSubtree is sentinel value returned from OnNode.
var ErrSkipSubtree = errors.New("skip subtree")

type Walk struct {
	// Root is cypress path where walk starts.
	Root ypath.Path

	// Attributes are requested for each node.
	Attributes []string

	// Node is optional pointer to type that will hold deserialized cypress node.
	Node interface{}

	// OnNode is invoked for each node during traversal.
	OnNode func(path ypath.Path, node interface{}) error
}

type tree struct {
	Attributes map[string]interface{} `yson:",attrs"`
	Children   map[string]tree        `yson:",value"`
}

func Do(ctx context.Context, yc yt.Client, w *Walk) error {
	opts := &yt.GetNodeOptions{
		Attributes: append([]string{"opaque"}, w.Attributes...),
	}

	var t tree
	if err := yc.GetNode(ctx, w.Root, &t, opts); err != nil {
		return fmt.Errorf("walk %q failed: %w", w.Root, err)
	}

	var walk func(path ypath.Path, t tree) error
	walk = func(path ypath.Path, t tree) error {
		var node interface{}
		if w.Node != nil {
			node = reflect.New(reflect.TypeOf(w.Node).Elem()).Interface()

			ysonNode, _ := yson.Marshal(yson.ValueWithAttrs{Attrs: t.Attributes})
			if err := yson.Unmarshal(ysonNode, node); err != nil {
				return fmt.Errorf("walk %q failed: %w", path, err)
			}
		}

		err := w.OnNode(path, node)
		if err == ErrSkipSubtree {
			return nil
		} else if err != nil {
			return err
		}

		for name, child := range t.Children {
			if err := walk(path.Child(name), child); err != nil {
				return err
			}
		}

		return nil
	}

	return walk(w.Root, t)
}
