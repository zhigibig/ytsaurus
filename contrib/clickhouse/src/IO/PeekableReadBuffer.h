#pragma once
#include <IO/ReadBuffer.h>
#include <IO/BufferWithOwnMemory.h>
#include <stack>

namespace DB
{

/// Also allows to set checkpoint at some position in stream and come back to this position later.
/// When next() is called, saves data between checkpoint and current position to own memory and loads next data to sub-buffer
/// Sub-buffer should not be accessed directly during the lifetime of peekable buffer (unless
/// you reset() the state of peekable buffer after each change of underlying buffer)
/// If position() of peekable buffer is explicitly set to some position before checkpoint
/// (e.g. by istr.position() = prev_pos), behavior is undefined.
class PeekableReadBuffer : public BufferWithOwnMemory<ReadBuffer>
{
    friend class PeekableReadBufferCheckpoint;
public:
    explicit PeekableReadBuffer(ReadBuffer & sub_buf_, size_t start_size_ = 0);

    ~PeekableReadBuffer() override;

    void prefetch(Priority priority) override { sub_buf->prefetch(priority); }

    /// Sets checkpoint at current position
    ALWAYS_INLINE inline void setCheckpoint()
    {
        if (checkpoint)
        {
            /// Recursive checkpoints. We just remember offset from the
            /// first checkpoint to the current position.
            recursive_checkpoints_offsets.push(offsetFromCheckpoint());
            return;
        }

        checkpoint_in_own_memory = currentlyReadFromOwnMemory();
        if (!checkpoint_in_own_memory)
        {
            /// Don't need to store unread data anymore
            peeked_size = 0;
        }
        checkpoint.emplace(pos);
    }

    /// Forget checkpoint and all data between checkpoint and position
    ALWAYS_INLINE inline void dropCheckpoint()
    {
        assert(checkpoint);

        if (!recursive_checkpoints_offsets.empty())
        {
            recursive_checkpoints_offsets.pop();
            return;
        }

        if (!currentlyReadFromOwnMemory())
        {
            /// Don't need to store unread data anymore
            peeked_size = 0;
        }
        checkpoint = std::nullopt;
        checkpoint_in_own_memory = false;
    }

    /// Sets position at checkpoint.
    /// All pointers (such as this->buffer().end()) may be invalidated
    void rollbackToCheckpoint(bool drop = false);

    /// If checkpoint and current position are in different buffers, appends data from sub-buffer to own memory,
    /// so data between checkpoint and position will be in continuous memory.
    void makeContinuousMemoryFromCheckpointToPos();

    /// Returns true if there unread data extracted from sub-buffer in own memory.
    /// This data will be lost after destruction of peekable buffer.
    bool hasUnreadData() const;

    const ReadBuffer & getSubBuffer() const { return *sub_buf; }

private:
    bool nextImpl() override;

    void resetImpl();

    bool peekNext();

    inline bool useSubbufferOnly() const { return !peeked_size; }
    inline bool currentlyReadFromOwnMemory() const { return working_buffer.begin() != sub_buf->buffer().begin(); }
    inline bool checkpointInOwnMemory() const { return checkpoint_in_own_memory; }

    void checkStateCorrect() const;

    /// Makes possible to append `bytes_to_append` bytes to data in own memory.
    /// Updates all invalidated pointers and sizes.
    void resizeOwnMemoryIfNecessary(size_t bytes_to_append);

    char * getMemoryData() { return use_stack_memory ? stack_memory : memory.data(); }
    const char * getMemoryData() const { return use_stack_memory ? stack_memory : memory.data(); }

    size_t offsetFromCheckpointInOwnMemory() const;
    size_t offsetFromCheckpoint() const;


    ReadBuffer * sub_buf;
    size_t peeked_size = 0;
    std::optional<Position> checkpoint = std::nullopt;
    bool checkpoint_in_own_memory = false;

    /// To prevent expensive and in some cases unnecessary memory allocations on PeekableReadBuffer
    /// creation (for example if PeekableReadBuffer is often created or if we need to remember small amount of
    /// data after checkpoint), at the beginning we will use small amount of memory on stack and allocate
    /// larger buffer only if reserved memory is not enough.
    char stack_memory[PADDING_FOR_SIMD];
    bool use_stack_memory = true;

    std::stack<size_t> recursive_checkpoints_offsets;
};


class PeekableReadBufferCheckpoint : boost::noncopyable
{
    PeekableReadBuffer & buf;
    bool auto_rollback;
public:
    explicit PeekableReadBufferCheckpoint(PeekableReadBuffer & buf_, bool auto_rollback_ = false)
                : buf(buf_), auto_rollback(auto_rollback_) { buf.setCheckpoint(); }
    ~PeekableReadBufferCheckpoint()
    {
        if (!buf.checkpoint)
            return;
        if (auto_rollback)
            buf.rollbackToCheckpoint();
        buf.dropCheckpoint();
    }

};

}
