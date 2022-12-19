package tech.ytsaurus.client.operations;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.List;
import java.util.Optional;

import javax.annotation.Nullable;

import tech.ytsaurus.client.TransactionalClient;
import tech.ytsaurus.client.request.CreateNode;
import tech.ytsaurus.core.DataSize;
import tech.ytsaurus.core.cypress.CypressNodeType;
import tech.ytsaurus.core.cypress.YPath;
import tech.ytsaurus.ysontree.YTreeBuilder;

import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;

/**
 * Immutable merge spec.
 */
@NonNullApi
@NonNullFields
public class MergeSpec extends SystemOperationSpecBase implements Spec {
    @Nullable
    private final Integer jobCount;
    private final MergeMode mergeMode;
    private final boolean combineChunks;

    private final List<String> mergeBy;
    @Nullable
    private final DataSize maxDataSizePerJob;
    @Nullable
    private final JobIo jobIo;

    public MergeSpec(List<YPath> inputTables, YPath outputTable) {
        this(builder().setInputTables(inputTables).setOutputTable(outputTable));
    }

    protected <T extends BuilderBase<T>> MergeSpec(BuilderBase<T> builder) {
        super(builder);
        jobCount = builder.jobCount;
        mergeMode = builder.mergeMode;
        combineChunks = builder.combineChunks;
        mergeBy = builder.mergeBy;
        maxDataSizePerJob = builder.maxDataSizePerJob;
        jobIo = builder.jobIo;
    }

    @Override
    public int hashCode() {
        return super.hashCode();
    }

    @Override
    public boolean equals(@Nullable Object obj) {
        if (obj == null) {
            return false;
        }
        if (getClass() != obj.getClass()) {
            return false;
        }

        MergeSpec spec = (MergeSpec) obj;

        return super.equals(obj)
                && Optional.ofNullable(jobCount).equals(Optional.ofNullable(spec.jobCount))
                && mergeMode.equals(spec.mergeMode)
                && combineChunks == spec.combineChunks
                && mergeBy.equals(spec.mergeBy)
                && Optional.ofNullable(maxDataSizePerJob).equals(Optional.ofNullable(spec.maxDataSizePerJob))
                && Optional.ofNullable(jobIo).equals(Optional.ofNullable(spec.jobIo));
    }

    public Optional<Integer> getJobCount() {
        return Optional.ofNullable(jobCount);
    }

    public MergeMode getMergeMode() {
        return mergeMode;
    }

    public boolean isCombineChunks() {
        return combineChunks;
    }

    public List<String> getMergeBy() {
        return mergeBy;
    }

    public Optional<DataSize> getMaxDataSizePerJob() {
        return Optional.ofNullable(maxDataSizePerJob);
    }

    public Optional<JobIo> getJobIo() {
        return Optional.ofNullable(jobIo);
    }

    /**
     * Create output table if it doesn't exist and convert spec to yson.
     */
    @Override
    public YTreeBuilder prepare(YTreeBuilder builder, TransactionalClient yt, SpecPreparationContext context) {
        yt.createNode(CreateNode.builder()
                .setPath(getOutputTable())
                .setType(CypressNodeType.TABLE)
                .setAttributes(getOutputTableAttributes())
                .setRecursive(true)
                .setIgnoreExisting(true)
                .build()).join();

        return builder.beginMap()
                .when(jobCount != null, b -> b.key("job_count").value(jobCount))
                .key("mode").value(mergeMode.value())
                .key("combine_chunks").value(combineChunks)
                .when(!mergeBy.isEmpty(), b -> b.key("merge_by").value(mergeBy))
                .when(maxDataSizePerJob != null, b -> b.key("max_data_size_per_job")
                        .value(maxDataSizePerJob.toBytes()))
                .when(jobIo != null, b -> b.key("job_io").value(jobIo.prepare()))
                .apply(b -> toTree(b, context))
                .endMap();
    }

    public static BuilderBase<?> builder() {
        return new Builder();
    }

    protected static class Builder extends BuilderBase<Builder> {
        @Override
        protected Builder self() {
            return this;
        }
    }

    // BuilderBase was taken out because there is another client
    // which we need to support too and which use the same MergeSpec class.
    @NonNullApi
    @NonNullFields
    public abstract static class BuilderBase<T extends BuilderBase<T>> extends SystemOperationSpecBase.Builder<T> {
        private @Nullable
        Integer jobCount;
        private MergeMode mergeMode = MergeMode.UNORDERED;
        private boolean combineChunks = false;

        private List<String> mergeBy = new ArrayList<>();
        private @Nullable
        DataSize maxDataSizePerJob;
        private @Nullable
        JobIo jobIo;

        public MergeSpec build() {
            return new MergeSpec(this);
        }

        public T setJobCount(@Nullable Integer jobCount) {
            this.jobCount = jobCount;
            return self();
        }

        public T setMergeMode(MergeMode mergeMode) {
            this.mergeMode = mergeMode;
            return self();
        }

        public T setCombineChunks(boolean combineChunks) {
            this.combineChunks = combineChunks;
            return self();
        }

        public T setMergeBy(Collection<String> mergeBy) {
            this.mergeBy = new ArrayList<>(mergeBy);
            return self();
        }

        public T setMergeBy(String... mergeBy) {
            return setMergeBy(Arrays.asList(mergeBy));
        }

        public T setMaxDataSizePerJob(DataSize maxDataSizePerJob) {
            this.maxDataSizePerJob = maxDataSizePerJob;
            return self();
        }

        /**
         * Set job I/O options.
         * @see JobIo
         */
        public T setJobIo(JobIo jobIo) {
            this.jobIo = jobIo;
            return self();
        }
    }
}
