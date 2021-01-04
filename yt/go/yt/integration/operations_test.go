package integration

import (
	"context"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/stretchr/testify/require"

	"a.yandex-team.ru/library/go/ptr"
	"a.yandex-team.ru/library/go/slices"
	"a.yandex-team.ru/yt/go/guid"
	"a.yandex-team.ru/yt/go/mapreduce"
	"a.yandex-team.ru/yt/go/mapreduce/spec"
	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yttest"
)

func TestOperation(t *testing.T) {
	t.Parallel()

	env, cancel := yttest.NewEnv(t)
	defer cancel()

	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()

	inTable := tmpPath()
	outTable := tmpPath()

	for _, p := range []ypath.Path{inTable, outTable} {
		_, err := env.YT.CreateNode(ctx, p, yt.NodeTable, nil)
		require.NoError(t, err)
	}

	w, err := env.YT.WriteTable(ctx, inTable, nil)
	require.NoError(t, err)
	require.NoError(t, w.Write(map[string]interface{}{"a": int64(1)}))
	require.NoError(t, w.Commit())

	spec := map[string]interface{}{
		"input_table_paths":  []ypath.Path{inTable},
		"output_table_paths": []ypath.Path{outTable},
		"mapper": map[string]interface{}{
			"input_format":  "yson",
			"output_format": "yson",
			"command":       "cat -",
		},
	}

	opID, err := env.YT.StartOperation(ctx, yt.OperationMap, spec, nil)
	require.NoError(t, err)

	for {
		time.Sleep(time.Second)

		status, err := env.YT.GetOperation(ctx, opID, nil)
		require.NoError(t, err)

		if status.State == yt.StateCompleted {
			break
		}
	}

	r, err := env.YT.ReadTable(ctx, outTable, nil)
	require.NoError(t, err)
	defer func() { _ = r.Close() }()

	var row interface{}
	require.True(t, r.Next())
	require.NoError(t, r.Scan(&row))
	require.Equal(t, map[string]interface{}{"a": int64(1)}, row)

	require.False(t, r.Next())
	require.NoError(t, r.Err())
}

func TestOperationWithStderr(t *testing.T) {
	t.Parallel()

	env, cancel := yttest.NewEnv(t)
	defer cancel()

	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()

	inTable := tmpPath()
	outTable := tmpPath()

	for _, p := range []ypath.Path{inTable, outTable} {
		_, err := env.YT.CreateNode(ctx, p, yt.NodeTable, nil)
		require.NoError(t, err)
	}

	w, err := env.YT.WriteTable(ctx, inTable, nil)
	require.NoError(t, err)
	require.NoError(t, w.Write(map[string]interface{}{"a": int64(1)}))
	require.NoError(t, w.Commit())

	spec := map[string]interface{}{
		"input_table_paths":  []ypath.Path{inTable},
		"output_table_paths": []ypath.Path{outTable},
		"mapper": map[string]interface{}{
			"input_format":  "yson",
			"output_format": "yson",
			"command":       "echo hello >> /dev/stderr",
		},
	}

	opID, err := env.YT.StartOperation(ctx, yt.OperationMap, spec, nil)
	require.NoError(t, err)
	for {
		time.Sleep(time.Second)
		status, err := env.YT.GetOperation(ctx, opID, nil)
		require.NoError(t, err)
		if status.State == yt.StateCompleted {
			break
		}
	}
	jobs, err := env.YT.ListJobs(ctx, opID, nil)
	require.NoError(t, err)
	for _, job := range jobs.Jobs {
		stderr, err := env.YT.GetJobStderr(ctx, opID, job.ID, nil)
		require.NoError(t, err)
		require.Equal(t, []byte("hello\n"), stderr)
	}
}

func TestListOperations(t *testing.T) {
	t.Parallel()

	env, cancel := yttest.NewEnv(t)
	defer cancel()

	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()

	inTable := tmpPath()
	outTable := tmpPath()

	for _, p := range []ypath.Path{inTable, outTable} {
		_, err := env.YT.CreateNode(ctx, p, yt.NodeTable, nil)
		require.NoError(t, err)
	}

	w, err := env.YT.WriteTable(ctx, inTable, nil)
	require.NoError(t, err)
	require.NoError(t, w.Write(map[string]interface{}{"a": int64(1)}))
	require.NoError(t, w.Commit())

	spec := map[string]interface{}{
		"input_table_paths":  []ypath.Path{inTable},
		"output_table_paths": []ypath.Path{outTable},
		"mapper": map[string]interface{}{
			"input_format":  "yson",
			"output_format": "yson",
			"command":       "echo hello >> /dev/stderr",
		},
	}

	opID, err := env.YT.StartOperation(ctx, yt.OperationMap, spec, nil)
	require.NoError(t, err)
	for {
		time.Sleep(time.Second)
		status, err := env.YT.GetOperation(ctx, opID, nil)
		require.NoError(t, err)
		if status.State == yt.StateCompleted {
			break
		}
	}
	ops, err := env.YT.ListOperations(ctx, nil)
	require.NoError(t, err)
	found := false
	for _, op := range ops.Operations {
		if op.ID == opID {
			found = true
		}
	}
	require.True(t, found, "Operation list must contain operation ID: %v", opID.String())
}

func TestListAllOperations(t *testing.T) {
	t.Parallel()

	env, cancel := yttest.NewEnv(t)
	defer cancel()

	ctx, cancel := context.WithTimeout(context.Background(), time.Minute*2)
	defer cancel()

	opAnnotation := guid.New().String()

	var opIDs []yt.OperationID
	for i := 0; i < 10; i++ {
		s := spec.Vanilla().
			AddVanillaTask("job", 1).
			AddAnnotations(map[string]interface{}{
				"annotation": opAnnotation,
			})
		s.MaxFailedJobCount = 1

		op, err := env.MR.Vanilla(s, map[string]mapreduce.Job{"job": &HelloJob{}})
		require.NoError(t, err)
		require.NoError(t, op.Wait())

		opIDs = append(opIDs, op.ID())
	}

	operations, err := yt.ListAllOperations(ctx, env.YT, &yt.ListOperationsOptions{
		Limit:  ptr.Int(2),
		Filter: &opAnnotation,
	})
	require.NoError(t, err)

	var found []yt.OperationID
	for _, op := range operations {
		found = append(found, op.ID)
	}
	slices.Reverse(found)

	require.Equal(t, opIDs, found)
}

func TestListAllJobs(t *testing.T) {
	t.Parallel()

	env, cancel := yttest.NewEnv(t)
	defer cancel()

	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()

	s := spec.Vanilla().AddVanillaTask("job", 10)
	s.MaxFailedJobCount = 1

	op, err := env.MR.Vanilla(s, map[string]mapreduce.Job{"job": &HelloJob{}})
	require.NoError(t, err)
	require.NoError(t, op.Wait())

	jobs, err := yt.ListAllJobs(ctx, env.YT, op.ID(), &yt.ListJobsOptions{
		Limit: ptr.Int(2),
	})
	require.NoError(t, err)
	require.Len(t, jobs, 10)
}

type HelloJob struct {
	mapreduce.Untyped
}

func (c *HelloJob) Do(ctx mapreduce.JobContext, in mapreduce.Reader, out []mapreduce.Writer) error {
	_, err := fmt.Fprint(os.Stderr, "Hello!\n")
	return err
}

func init() {
	mapreduce.Register(&HelloJob{})
}

func TestMain(m *testing.M) {
	if mapreduce.InsideJob() {
		os.Exit(mapreduce.JobMain())
	}

	os.Exit(m.Run())
}
