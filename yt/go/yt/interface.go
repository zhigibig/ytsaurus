// Package yt defines interfaces of different YT services.
//
// All API methods follow the same conventions:
//   - First argument is context.Context.
//   - Last argument is pointer to Options struct.
//   - Other arguments are required parameters.
//
// Zero value of every Options struct corresponds to default values of parameters.
//
// You may pass nil as the last argument.
//
//   var ctx context.Context
//   var y yt.Client
//   p := ypath.Path("//foo/bar/@zog")
//
//   // These two calls do the same thing.
//   y.SetNode(ctx, p, 1, nil)
//   y.SetNode(ctx, p, 1, &yt.SetNodeOptions{})
//
// By default, client retries all transient errors 5 times. Use context.WithTimeout to provide timeout for api call.
//
// API methods are grouped into interfaces, according to part of the system they interact with:
//   - CypressClient           - cypress nodes
//   - LowLevelTxClient        - cypress transactions
//   - LockClient              - cypress locks
//   - LowLevelSchedulerClient - scheduler
//   - FileClient              - file operations
//   - TableClient             - table operations
//   - AdminClient             - misc administrative commands
//   - TabletClient            - dynamic tables
//
// Finally, yt.Client and yt.Tx provide high level api for transactions and embed interfaces of different subsystems.
package yt

import (
	"context"
	"io"

	"a.yandex-team.ru/yt/go/guid"
	"a.yandex-team.ru/yt/go/schema"
	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yson"
	"a.yandex-team.ru/yt/go/yterrors"
)

//go:generate yt-gen-client -interface interface.go -output internal/params.go

// TransactionOptions control transactional context of cypress command.
//
// Do not use this options directly. Use Transaction instead.
type TransactionOptions struct {
	TransactionID                      TxID `http:"transaction_id"`
	Ping                               bool `http:"ping"`
	PingAncestors                      bool `http:"ping_ancestor_transactions"`
	SuppressTransactionCoordinatorSync bool `http:"suppress_transaction_coordinator_sync"`
}

// AccessTrackingOptions suppresses update of "modification_time", "access_time" and
// "access_counter" cypress attributes.
type AccessTrackingOptions struct {
	SuppressAccessTracking       bool `http:"suppress_access_tracking"`
	SuppressModificationTracking bool `http:"suppress_modification_tracking"`
}

// MutatingOptions enable safe retries of cypress commands in the presence of network errors.
//
// MutatingOptions are managed internally by the library.
type MutatingOptions struct {
	MutationID MutationID `http:"mutation_id"`
	Retry      bool       `http:"retry"`
}

type ReadKind string

const (
	ReadFromLeader   ReadKind = "leader"
	ReadFromFollower ReadKind = "follower"
	ReadFromCache    ReadKind = "cache"
)

// ReadRetryOptions is marker for distinguishing requests that might be safely retried.
//
// TODO(prime@): rename to SafeRetryOptions
type ReadRetryOptions struct {
}

// MasterReadOptions specify where cypress read requests are routed.
//
// By default read requests are served from followers.
type MasterReadOptions struct {
	ReadFrom ReadKind `http:"read_from"`
}

type PrerequisiteRevision struct {
	Path          ypath.Path `yson:"path"`
	TransactionID TxID       `yson:"transaction_id"`
	Revision      Revision   `yson:"revision"`
}

type PrerequisiteOptions struct {
	TransactionIDs []TxID                 `http:"prerequisite_transaction_ids,omitnil"`
	Revisions      []PrerequisiteRevision `http:"prerequisite_revisions,omitnil"`
}

// TimeoutOptions specify request timeout that is propagated through internal YT RPC framework.
type TimeoutOptions struct {
	Timeout yson.Duration `http:"timeout"`
}

// CreateNodeOptions.
//
// See https://wiki.yandex-team.ru/yt/userdoc/api/#create
type CreateNodeOptions struct {
	Recursive      bool `http:"recursive"`
	IgnoreExisting bool `http:"ignore_existing"`
	Force          bool `http:"force"`

	Attributes map[string]interface{} `http:"attributes,omitnil"`

	*TransactionOptions
	*AccessTrackingOptions
	*MutatingOptions
	*PrerequisiteOptions
}

type CreateObjectOptions struct {
	Recursive      bool `http:"recursive"`
	IgnoreExisting bool `http:"ignore_existing"`
	Force          bool `http:"force"`

	Attributes map[string]interface{} `http:"attributes,omitnil"`

	*PrerequisiteOptions
	*AccessTrackingOptions
	*MutatingOptions
}

type NodeExistsOptions struct {
	*MasterReadOptions
	*TransactionOptions
	*AccessTrackingOptions
	*ReadRetryOptions
}

type RemoveNodeOptions struct {
	Recursive bool `http:"recursive"`
	Force     bool `http:"force"`

	*TransactionOptions
	*AccessTrackingOptions
	*PrerequisiteOptions
	*MutatingOptions
}

type GetNodeOptions struct {
	Attributes []string `http:"attributes,omitnil"`
	MaxSize    *int64   `http:"max_size,omitnil"`

	*TransactionOptions
	*AccessTrackingOptions
	*PrerequisiteOptions
	*MasterReadOptions

	*ReadRetryOptions
}

type SetNodeOptions struct {
	Recursive bool `http:"recursive"`
	Force     bool `http:"force"`

	*TransactionOptions
	*AccessTrackingOptions
	*MutatingOptions
	*PrerequisiteOptions
}

type MultisetAttributesOptions struct {
	*TransactionOptions
	*AccessTrackingOptions
	*MutatingOptions
	*PrerequisiteOptions
}

type ListNodeOptions struct {
	Attributes []string `http:"attributes,omitnil"`
	MaxSize    *int64   `http:"max_size,omitnil"`

	*TransactionOptions
	*MasterReadOptions
	*AccessTrackingOptions
	*PrerequisiteOptions

	*ReadRetryOptions
}

type CopyNodeOptions struct {
	Recursive      bool `http:"recursive"`
	IgnoreExisting bool `http:"ignore_existing"`
	Force          bool `http:"force"`

	PreserveAccount           *bool `http:"preserve_account,omitnil"`
	PreserveExpirationTime    *bool `http:"preserve_expiration_time,omitnil"`
	PreserveExpirationTimeout *bool `http:"preserve_expiration_timeout,omitnil"`
	PreserveCreationTime      *bool `http:"preserve_creation_time,omitnil"`
	PessimisticQuotaCheck     *bool `http:"pessimistic_quota_check,omitnil"`

	*TransactionOptions
	// *AccessTrackingOptions
	*MutatingOptions
	*PrerequisiteOptions
}

type MoveNodeOptions struct {
	Recursive bool `http:"recursive"`
	Force     bool `http:"force"`

	PreserveAccount           *bool `http:"preserve_account,omitnil"`
	PreserveExpirationTime    *bool `http:"preserve_expiration_time,omitnil"`
	PreserveExpirationTimeout *bool `http:"preserve_expiration_timeout,omitnil"`
	PessimisticQuotaCheck     *bool `http:"pessimistic_quota_check,omitnil"`

	*TransactionOptions
	// *AccessTrackingOptions
	*MutatingOptions
	*PrerequisiteOptions
}

type LinkNodeOptions struct {
	Recursive      bool `http:"recursive"`
	IgnoreExisting bool `http:"ignore_existing"`
	Force          bool `http:"force"`

	Attributes map[string]interface{} `http:"attributes,omitnil"`

	*TransactionOptions
	// *AccessTrackingOptions
	*MutatingOptions
	*PrerequisiteOptions
}

type CypressClient interface {
	// http:verb:"create"
	// http:params:"path","type"
	CreateNode(
		ctx context.Context,
		path ypath.YPath,
		typ NodeType,
		options *CreateNodeOptions,
	) (id NodeID, err error)

	// http:verb:"create"
	// http:params:"type"
	CreateObject(
		ctx context.Context,
		typ NodeType,
		options *CreateObjectOptions,
	) (id NodeID, err error)

	// http:verb:"exists"
	// http:params:"path"
	NodeExists(
		ctx context.Context,
		path ypath.YPath,
		options *NodeExistsOptions,
	) (ok bool, err error)

	// http:verb:"remove"
	// http:params:"path"
	RemoveNode(
		ctx context.Context,
		path ypath.YPath,
		options *RemoveNodeOptions,
	) (err error)

	// http:verb:"get"
	// http:params:"path"
	// http:extra
	GetNode(
		ctx context.Context,
		path ypath.YPath,
		result interface{},
		options *GetNodeOptions,
	) (err error)

	// http:verb:"set"
	// http:params:"path"
	// http:extra
	SetNode(
		ctx context.Context,
		path ypath.YPath,
		value interface{},
		options *SetNodeOptions,
	) (err error)

	// http:verb:"multiset_attributes"
	// http:params:"path"
	// http:extra
	MultisetAttributes(
		ctx context.Context,
		path ypath.YPath,
		attributes map[string]interface{},
		options *MultisetAttributesOptions,
	) (err error)

	// http:verb:"list"
	// http:params:"path"
	// http:extra
	ListNode(
		ctx context.Context,
		path ypath.YPath,
		result interface{},
		options *ListNodeOptions,
	) (err error)

	// http:verb:"copy"
	// http:params:"source_path","destination_path"
	CopyNode(
		ctx context.Context,
		src ypath.YPath,
		dst ypath.YPath,
		options *CopyNodeOptions,
	) (id NodeID, err error)

	// http:verb:"move"
	// http:params:"source_path","destination_path"
	MoveNode(
		ctx context.Context,
		src ypath.YPath,
		dst ypath.YPath,
		options *MoveNodeOptions,
	) (id NodeID, err error)

	// http:verb:"link"
	// http:params:"target_path","link_path"
	LinkNode(
		ctx context.Context,
		target ypath.YPath,
		link ypath.YPath,
		options *LinkNodeOptions,
	) (id NodeID, err error)
}

type StartTxOptions struct {
	Timeout  *yson.Duration `http:"timeout,omitnil"`
	Deadline *yson.Time     `http:"deadline,omitnil"`

	Type   *TxType `http:"type,omitnil"`
	Sticky bool    `http:"sticky"`

	PrerequisiteTransactionIDs []TxID                 `http:"prerequisite_transaction_ids,omitnil"`
	Attributes                 map[string]interface{} `http:"attributes,omitnil"`

	*TransactionOptions
	*ReadRetryOptions
}

type PingTxOptions struct {
	*TransactionOptions
}

type AbortTxOptions struct {
	Sticky bool `http:"sticky"`

	*TransactionOptions
	*MutatingOptions
	*PrerequisiteOptions
}

type CommitTxOptions struct {
	Sticky bool `http:"sticky"`

	*MutatingOptions
	*PrerequisiteOptions
	*TransactionOptions
}

// LowLevelTxClient provides stateless interface to YT transactions.
//
// Clients should rarely use it directly.
type LowLevelTxClient interface {
	// http:verb:"start_transaction"
	StartTx(
		ctx context.Context,
		options *StartTxOptions,
	) (id TxID, err error)

	// http:verb:"start_transaction"
	StartTabletTx(
		ctx context.Context,
		options *StartTabletTxOptions,
	) (id TxID, err error)

	// http:verb:"ping_transaction"
	// http:params:"transaction_id"
	PingTx(
		ctx context.Context,
		id TxID,
		options *PingTxOptions,
	) (err error)

	// http:verb:"abort_transaction"
	// http:params:"transaction_id"
	AbortTx(
		ctx context.Context,
		id TxID,
		options *AbortTxOptions,
	) (err error)

	// http:verb:"commit_transaction"
	// http:params:"transaction_id"
	CommitTx(
		ctx context.Context,
		id TxID,
		options *CommitTxOptions,
	) (err error)
}

type WriteFileOptions struct {
	ComputeMD5 bool        `http:"compute_md5"`
	FileWriter interface{} `http:"file_writer"`

	*TransactionOptions
	*PrerequisiteOptions
}

type ReadFileOptions struct {
	Offset     *int64      `http:"offset,omitnil"`
	Length     *int64      `http:"length,omitnil"`
	FileReader interface{} `http:"file_reader"`

	*TransactionOptions
	*AccessTrackingOptions
}

type PutFileToCacheOptions struct {
	CachePath ypath.YPath `http:"cache_path"`

	*MasterReadOptions
	*MutatingOptions
	*PrerequisiteOptions
}

type GetFileFromCacheOptions struct {
	CachePath ypath.YPath `http:"cache_path"`

	*MasterReadOptions
	*ReadRetryOptions
}

type FileClient interface {
	// http:verb:"write_file"
	// http:params:"path"
	WriteFile(
		ctx context.Context,
		path ypath.YPath,
		options *WriteFileOptions,
	) (w io.WriteCloser, err error)

	// http:verb:"read_file"
	// http:params:"path"
	ReadFile(
		ctx context.Context,
		path ypath.YPath,
		options *ReadFileOptions,
	) (r io.ReadCloser, err error)

	// http:verb:"put_file_to_cache"
	// http:params:"path","md5"
	PutFileToCache(
		ctx context.Context,
		path ypath.YPath,
		md5 string,
		options *PutFileToCacheOptions,
	) (cachedPath ypath.YPath, err error)

	// http:verb:"get_file_from_cache"
	// http:params:"md5"
	GetFileFromCache(
		ctx context.Context,
		md5 string,
		options *GetFileFromCacheOptions,
	) (path ypath.YPath, err error)
}

type WriteTableOptions struct {
	TableWriter interface{} `http:"table_writer"`

	*TransactionOptions
	*AccessTrackingOptions
}

type ReadTableOptions struct {
	Unordered   bool        `http:"unordered"`
	TableReader interface{} `http:"table_reader"`

	ControlAttributes interface{} `http:"control_attributes,omitnil"`
	StartRowIndexOnly *bool       `http:"start_row_index_only,omitnil"`

	// Smart flag enables client side retries.
	//
	// When used outside of transaction, ReadTable automatically creates new transaction and
	// acquired table snapshot lock.
	//
	// When used inside of transaction, ReadTable does not create any additional transactions.
	//
	// Internal retries continue indefinitely. User must set appropriate timeout on request context.
	//
	// This is experimental option, that is currently off by default.
	Smart *bool

	*TransactionOptions
	*AccessTrackingOptions
}

type TableClient interface {
	// WriteTable opens low-level table writer. Use yt.WriteTable() function instead of calling this method directly.
	//
	// http:verb:"write_table"
	// http:params:"path"
	WriteTable(
		ctx context.Context,
		path ypath.YPath,
		options *WriteTableOptions,
	) (w TableWriter, err error)

	// http:verb:"read_table"
	// http:params:"path"
	ReadTable(
		ctx context.Context,
		path ypath.YPath,
		options *ReadTableOptions,
	) (r TableReader, err error)
}

type StartOperationOptions struct {
	*TransactionOptions
	*MutatingOptions
}

type AbortOperationOptions struct {
	AbortMessage *string `http:"abort_message,omitnil"`
}

type SuspendOperationOptions struct {
	AbortRunningJobs bool `http:"abort_running_jobs"`
}

type ResumeOperationOptions struct {
}

type CompleteOperationOptions struct {
}

type UpdateOperationParametersOptions struct {
}

type TransferAccountResourcesOptions struct {
	*MutatingOptions
}

type TransferPoolResourcesOptions struct {
	*MutatingOptions
}

type ListOperationsOptions struct {
	*MasterReadOptions

	*ReadRetryOptions

	FromTime *yson.Time      `http:"from_time,omitnil"`
	ToTime   *yson.Time      `http:"to_time,omitnil"`
	Cursor   *yson.Time      `http:"cursor_time,omitnil"`
	User     *string         `http:"user,omitnil"`
	State    *OperationState `http:"state,omitnil"`
	Type     *OperationType  `http:"type,omitnil"`
	Filter   *string         `http:"filter,omitnil"`
	Limit    *int            `http:"limit,omitnil"`
}

type ListJobsOptions struct {
	JobType         *JobType       `http:"job_type,omitnil"`
	JobState        *JobState      `http:"job_state,omitnil"`
	Address         *string        `http:"address,omitnil"`
	WithStderr      *bool          `http:"with_stderr,omitnil"`
	WithFailContext *bool          `http:"with_fail_context,omitnil"`
	SortField       *JobSortField  `http:"sort_field,omitnil"`
	SortOrder       *JobSortOrder  `http:"sort_order,omitnil"`
	Limit           *int           `http:"limit,omitnil"`
	Offset          *int           `http:"offset,omitnil"`
	DataSource      *JobDataSource `http:"data_source,omitnil"`
}

type JobStatus struct {
	ID              JobID              `yson:"id"`
	Type            string             `yson:"type"`
	State           string             `yson:"state"`
	Address         string             `yson:"address"`
	StartTime       yson.Time          `yson:"start_time"`
	FinishTime      yson.Time          `yson:"finish_time,omitempty"`
	FailContextSize int                `yson:"fail_context_size,omitempty"`
	Error           yterrors.Error     `yson:"error,omitempty"`
	Progress        float64            `yson:"progress,omitempty"`
	ExecAttributes  *JobExecAttributes `yson:"exec_attributes,omitempty"`
}

type JobExecAttributes struct {
	MediumName  string   `yson:"medium_name"`
	IPAddresses []string `yson:"ip_addresses"`
	SandboxPath string   `yson:"sandbox_path"`
	SlotIndex   int      `yson:"slot_index"`
}

type ListOperationsResult struct {
	Operations      []OperationStatus `yson:"operations"`
	Incomplete      bool              `yson:"incomplete"`
	PoolCounts      map[string]int    `yson:"pool_counts"`
	UserCounts      map[string]int    `yson:"user_counts"`
	StateCounts     map[string]int    `yson:"state_counts"`
	TypeCounts      map[string]int    `yson:"type_counts"`
	FailedJobsCount int               `yson:"failed_jobs_count"`
}

type ListJobsResult struct {
	Jobs []JobStatus `yson:"jobs"`
}

type GetJobStderrOptions struct {
}

type GetOperationOptions struct {
	Attributes     []string `http:"attributes,omitnil"`
	IncludeRuntime *bool    `http:"include_runtime,omitnil"`

	*MasterReadOptions

	*ReadRetryOptions
}

type OperationResult struct {
	Error *yterrors.Error `yson:"error"`
}

type OperationRuntimeParameters struct {
	ACL                          []ACE                  `yson:"acl"`
	SchedulingOptionsPerPoolTree map[string]interface{} `yson:"scheduling_options_per_pool_tree"`
	Annotations                  map[string]interface{} `yson:"annotations"`
	ErasedTrees                  []string               `yson:"erased_trees"`
}

type OperationStatus struct {
	ID                OperationID                `yson:"id"`
	State             OperationState             `yson:"state"`
	Result            *OperationResult           `yson:"result"`
	Type              OperationType              `yson:"type"`
	BriefSpec         map[string]interface{}     `yson:"brief_spec"`
	FullSpec          yson.RawValue              `yson:"full_spec"`
	StartTime         yson.Time                  `yson:"start_time"`
	Suspend           bool                       `yson:"suspend"`
	AuthenticatedUser string                     `yson:"authenticated_user"`
	RuntimeParameters OperationRuntimeParameters `yson:"runtime_parameters"`
}

type OperationStartClient interface {
	// http:verb:"start_operation"
	// http:params:"operation_type","spec"
	StartOperation(
		ctx context.Context,
		opType OperationType,
		spec interface{},
		options *StartOperationOptions,
	) (opID OperationID, err error)
}

// LowLevelSchedulerClient is stateless interface to the YT scheduler.
//
// Clients should use package mapreduce instead.
type LowLevelSchedulerClient interface {
	OperationStartClient

	// http:verb:"abort_operation"
	// http:params:"operation_id"
	AbortOperation(
		ctx context.Context,
		opID OperationID,
		options *AbortOperationOptions,
	) (err error)

	// http:verb:"suspend_operation"
	// http:params:"operation_id"
	SuspendOperation(
		ctx context.Context,
		opID OperationID,
		options *SuspendOperationOptions,
	) (err error)

	// http:verb:"resume_operation"
	// http:params:"operation_id"
	ResumeOperation(
		ctx context.Context,
		opID OperationID,
		options *ResumeOperationOptions,
	) (err error)

	// http:verb:"complete_operation"
	// http:params:"operation_id"
	CompleteOperation(
		ctx context.Context,
		opID OperationID,
		options *CompleteOperationOptions,
	) (err error)

	// http:verb:"update_operation_parameters"
	// http:params:"operation_id","parameters"
	UpdateOperationParameters(
		ctx context.Context,
		opID OperationID,
		params interface{},
		options *UpdateOperationParametersOptions,
	) (err error)

	// http:verb:"get_operation"
	// http:params:"operation_id"
	GetOperation(
		ctx context.Context,
		opID OperationID,
		options *GetOperationOptions,
	) (status *OperationStatus, err error)

	// http:verb:"list_operations"
	ListOperations(
		ctx context.Context,
		options *ListOperationsOptions,
	) (operations *ListOperationsResult, err error)

	// http:verb:"list_jobs"
	// http:params:"operation_id"
	ListJobs(
		ctx context.Context,
		opID OperationID,
		options *ListJobsOptions,
	) (r *ListJobsResult, err error)

	// http:verb:"get_job_stderr"
	// http:params:"operation_id","job_id"
	GetJobStderr(
		ctx context.Context,
		opID OperationID,
		jobID JobID,
		options *GetJobStderrOptions,
	) (r []byte, err error)
}

type AddMemberOptions struct {
	*MutatingOptions
	*PrerequisiteOptions
}

type RemoveMemberOptions struct {
	*MutatingOptions
	*PrerequisiteOptions
}

type AdminClient interface {
	// http:verb:"add_member"
	// http:params:"group","member"
	AddMember(
		ctx context.Context,
		group string,
		member string,
		options *AddMemberOptions,
	) (err error)

	// http:verb:"remove_member"
	// http:params:"group","member"
	RemoveMember(
		ctx context.Context,
		group string,
		member string,
		options *RemoveMemberOptions,
	) (err error)

	// http:verb:"transfer_account_resources"
	// http:params:"source_account","destination_account","resource_delta"
	TransferAccountResources(
		ctx context.Context,
		srcAccount string,
		dstAccount string,
		resourceDelta interface{},
		options *TransferAccountResourcesOptions,
	) (err error)

	// http:verb:"transfer_pool_resources"
	// http:params:"source_pool","destination_pool","pool_tree","resource_delta"
	TransferPoolResources(
		ctx context.Context,
		srcPool string,
		dstPool string,
		poolTree string,
		resourceDelta interface{},
		options *TransferPoolResourcesOptions,
	) (err error)
}

type LockNodeOptions struct {
	Waitable     bool    `http:"waitable"`
	ChildKey     *string `http:"child_key,omitnil"`
	AttributeKey *string `http:"attribute_key,omitnil"`

	*TransactionOptions
	*MutatingOptions
}

type UnlockNodeOptions struct {
	*TransactionOptions
	*MutatingOptions
}

type LockResult struct {
	NodeID NodeID    `yson:"node_id"`
	LockID guid.GUID `yson:"lock_id"`
}

type LockClient interface {
	// http:verb:"lock"
	// http:params:"path","mode"
	LockNode(
		ctx context.Context,
		path ypath.YPath,
		mode LockMode,
		options *LockNodeOptions,
	) (res LockResult, err error)

	// http:verb:"unlock"
	// http:params:"path"
	UnlockNode(
		ctx context.Context,
		path ypath.YPath,
		options *UnlockNodeOptions,
	) (err error)
}

type TabletRangeOptions struct {
	FirstTabletIndex int `http:"first_tablet_index"`
	LastTabletIndex  int `http:"last_tablet_index"`
}

type MountTableOptions struct {
	*TabletRangeOptions
	*MutatingOptions

	CellID        *guid.GUID  `http:"cell_id,omitnil"`
	TargetCellIDs []guid.GUID `http:"target_cell_ids,omitnil"`
	Freeze        bool        `http:"freeze"`
}

type UnmountTableOptions struct {
	*TabletRangeOptions
	*MutatingOptions

	Force bool `http:"force"`
}

type RemountTableOptions struct {
	*TabletRangeOptions
	*MutatingOptions
}

type FreezeTableOptions struct {
	*TabletRangeOptions
	*MutatingOptions
}

type UnfreezeTableOptions struct {
	*TabletRangeOptions
	*MutatingOptions
}

type ReshardTableOptions struct {
	*TabletRangeOptions
	*MutatingOptions

	PivotKeys   interface{} `http:"pivot_keys,omitnil"`
	TabletCount *int        `http:"tablet_count,omitnil"`
}

type AlterTableOptions struct {
	*MutatingOptions

	Schema            *schema.Schema `http:"schema,omitnil"`
	Dynamic           *bool          `http:"dynamic,omitnil"`
	UpstreamReplicaID *guid.GUID     `http:"upstream_replica_id,omitnil"`
}

type AlterTableReplicaOptions struct {
	Enabled *bool             `http:"enabled,omitnil"`
	Mode    *TableReplicaMode `http:"mode,omitnil"`
}

// Tx is high level API for master transactions.
//
// Create new tx by calling BeginTx() method on Client or other Tx.
//
// Cleanup of started tx is responsibility of the user. Tx is terminated, either by calling Commit() or Abort(),
// or by canceling ctx passed to BeginTx().
//
// Unterminated tx will result in goroutine leak.
type Tx interface {
	CypressClient
	FileClient
	TableClient
	LockClient

	OperationStartClient

	ID() TxID
	Commit() error
	Abort() error

	// Finished returns a channel that is closed when transaction finishes, either because it was committed or aborted.
	Finished() <-chan struct{}

	// BeginTx creates nested transaction.
	BeginTx(ctx context.Context, options *StartTxOptions) (tx Tx, err error)
}

type LookupRowsOptions struct {
	// KeepMissingRows changes handling of missing rows.
	//
	// When this flag is not set, missing rows are skipped.
	// When flag is set, nil values are returned for missing rows.
	KeepMissingRows bool `http:"keep_missing_rows"`

	// Timestamp overrides timestamp for lookup.
	//
	// By default, lookup returns data from the point in time when
	// transaction was started.
	Timestamp *Timestamp `http:"timestamp,omitnil"`

	// Columns filters columns in lookup result.
	Columns []string `http:"column_names,omitnil"`

	*TransactionOptions
}

type InsertRowsOptions struct {
	Atomicity          *Atomicity `http:"atomicity,omitnil"`
	RequireSyncReplica *bool      `http:"require_sync_replica,omitnil"`
	Update             *bool      `http:"update,omitnil"`

	*TransactionOptions
}

type LockRowsOptions struct {
	*TransactionOptions
}

type DeleteRowsOptions struct {
	RequireSyncReplica *bool `http:"require_sync_replica,omitnil"`

	*TransactionOptions
}

type SelectRowsOptions struct {
	FailOnIncompleteResult *bool `http:"fail_on_incomplete_result,omitnil"`
	InputRowLimit          *int  `http:"input_row_limit,omitnil"`
	OutputRowLimit         *int  `http:"output_row_limit,omitnil"`

	Timestamp *Timestamp `http:"timestamp,omitnil"`

	*TransactionOptions
	*TimeoutOptions
}

type StartTabletTxOptions struct {
	Atomicity *Atomicity     `http:"atomicity,omitnil"`
	Timeout   *yson.Duration `http:"timeout,omitnil"`

	Type   TxType `http:"type"`
	Sticky bool   `http:"sticky"`
}

type TabletClient interface {
	// SelectRows executes query.
	//
	// Use strconv.Quote or %q format to escape string literals inside query.
	//
	// http:verb:"select_rows"
	// http:params:"query"
	SelectRows(
		ctx context.Context,
		query string,
		options *SelectRowsOptions,
	) (r TableReader, err error)

	// LookupRows performs lookup by key.
	//
	// Rows are returned in the order of provided keys.
	//
	// By default, returns one row for each key that is present in the table.
	//
	// When KeepMissingRows option is set, returns exactly len(keys) number of rows. If key is missing
	// from the table, corresponding row will be nil.
	//
	// http:verb:"lookup_rows"
	// http:params:"path"
	// http:extra
	LookupRows(
		ctx context.Context,
		path ypath.Path,
		keys []interface{},
		options *LookupRowsOptions,
	) (r TableReader, err error)

	// LockRows acquires lock for given keys, without changing row values.
	//
	// http:verb:"lock_rows"
	// http:params:"path","locks","lock_type"
	// http:extra
	LockRows(
		ctx context.Context,
		path ypath.Path,
		locks []string,
		lockType LockType,
		keys []interface{},
		options *LockRowsOptions,
	) (err error)

	// http:verb:"insert_rows"
	// http:params:"path"
	// http:extra
	InsertRows(
		ctx context.Context,
		path ypath.Path,
		rows []interface{},
		options *InsertRowsOptions,
	) (err error)

	// InsertRowBatch is API optimized for performance.
	//
	// It works just like InsertRows, but allows serializing rows ahead of time.
	InsertRowBatch(
		ctx context.Context,
		path ypath.Path,
		rowBatch RowBatch,
		options *InsertRowsOptions,
	) (err error)

	// http:verb:"delete_rows"
	// http:params:"path"
	// http:extra
	DeleteRows(
		ctx context.Context,
		path ypath.Path,
		keys []interface{},
		options *DeleteRowsOptions,
	) (err error)
}

type MountClient interface {
	// http:verb:"mount_table"
	// http:params:"path"
	MountTable(
		ctx context.Context,
		path ypath.Path,
		options *MountTableOptions,
	) (err error)

	// http:verb:"unmount_table"
	// http:params:"path"
	UnmountTable(
		ctx context.Context,
		path ypath.Path,
		options *UnmountTableOptions,
	) (err error)

	// http:verb:"remount_table"
	// http:params:"path"
	RemountTable(
		ctx context.Context,
		path ypath.Path,
		options *RemountTableOptions,
	) (err error)

	// http:verb:"reshard_table"
	// http:params:"path"
	ReshardTable(
		ctx context.Context,
		path ypath.Path,
		options *ReshardTableOptions,
	) (err error)

	// http:verb:"alter_table"
	// http:params:"path"
	AlterTable(
		ctx context.Context,
		path ypath.Path,
		options *AlterTableOptions,
	) (err error)

	// http:verb:"freeze_table"
	// http:params:"path"
	FreezeTable(
		ctx context.Context,
		path ypath.Path,
		options *FreezeTableOptions,
	) (err error)

	// http:verb:"unfreeze_table"
	// http:params:"path"
	UnfreezeTable(
		ctx context.Context,
		path ypath.Path,
		options *UnfreezeTableOptions,
	) (err error)

	// http:verb:"alter_table_replica"
	// http:params:"replica_id"
	AlterTableReplica(
		ctx context.Context,
		id NodeID,
		options *AlterTableReplicaOptions,
	) (err error)
}

type TabletTx interface {
	TabletClient

	ID() TxID
	Commit() error
	Abort() error
}

type LocateSkynetShareOptions struct{}

type GenerateTimestampOptions struct{}

type GetInSyncReplicasOptions struct{}

type Client interface {
	CypressClient
	FileClient
	TableClient

	// BeginTx creates new tx.
	//
	// Tx lifetime is bound to ctx. Tx is automatically aborted when ctx is canceled.
	//
	// Active transaction consumes resources, client must finish transaction by calling Commit(), Abort()
	// or canceling ctx passed to BeginTx.
	BeginTx(ctx context.Context, options *StartTxOptions) (tx Tx, err error)

	// BeginTabletTx creates new tablet transaction.
	//
	// Tx lifetime is bound to ctx. Tx is automatically aborted when ctx is canceled.
	//
	// Active transaction consumes resources, client must finish transaction by calling Commit(), Abort()
	// or canceling ctx passed to BeginTabletTx.
	BeginTabletTx(ctx context.Context, options *StartTabletTxOptions) (tx TabletTx, err error)

	TabletClient
	MountClient

	// http:verb:"generate_timestamp"
	GenerateTimestamp(ctx context.Context, options *GenerateTimestampOptions) (ts Timestamp, err error)

	LowLevelTxClient
	LowLevelSchedulerClient

	AdminClient

	// http:verb:"locate_skynet_share"
	// http:params:"path"
	LocateSkynetShare(
		ctx context.Context,
		path ypath.YPath,
		options *LocateSkynetShareOptions,
	) (l ShareLocation, err error)

	// http:verb:"get_in_sync_replicas"
	// http:params:"path","timestamp"
	// http:extra
	GetInSyncReplicas(
		ctx context.Context,
		path ypath.Path,
		ts Timestamp,
		keys []interface{},
		options *GetInSyncReplicasOptions,
	) (ids []NodeID, err error)

	NewRowBatchWriter() RowBatchWriter

	// Stop() cancels and waits for completion of all background activity associated with this client.
	//
	// All transactions tracked by this client are aborted.
	Stop()
}
