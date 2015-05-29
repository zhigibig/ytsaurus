#ifndef PERSISTENT_QUEUE_INL_H_
#error "Direct inclusion of this file is not allowed, include persistent_queue.h"
#endif
#undef PERSISTENT_QUEUE_INL_H_

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class T, size_t ChunkSize>
TPersistentQueueIterator<T, ChunkSize>::TPersistentQueueIterator()
{  }

template <class T, size_t ChunkSize>
TPersistentQueueIterator<T, ChunkSize>& TPersistentQueueIterator<T, ChunkSize>::operator++()
{
    YASSERT(CurrentChunk_);
    YASSERT(CurrentIndex_ >= 0 && CurrentIndex_ < ChunkSize);

    ++CurrentIndex_;
    if (CurrentIndex_ == ChunkSize) {
        CurrentChunk_ = CurrentChunk_->Next;
        CurrentIndex_ = 0;
    }

    return *this;
}

template <class T, size_t ChunkSize>
TPersistentQueueIterator<T, ChunkSize> TPersistentQueueIterator<T, ChunkSize>::operator++(int)
{
    auto result = *this;
    ++(*this);
    return result;
}

template <class T, size_t ChunkSize>
T& TPersistentQueueIterator<T, ChunkSize>::operator*() const
{
    return CurrentChunk_->Elements[CurrentIndex_];
}

template <class T, size_t ChunkSize>
bool TPersistentQueueIterator<T, ChunkSize>::operator==(const TPersistentQueueIterator& other) const
{
    return CurrentChunk_ == other.CurrentChunk_ && CurrentIndex_ == other.CurrentIndex_;
}

template <class T, size_t ChunkSize>
bool TPersistentQueueIterator<T, ChunkSize>::operator!=(const TPersistentQueueIterator& other) const
{
    return !(*this == other);
}

template <class T, size_t ChunkSize>
TPersistentQueueIterator<T, ChunkSize>::TPersistentQueueIterator(
    TChunkPtr chunk,
    size_t index)
    : CurrentChunk_(std::move(chunk))
      , CurrentIndex_(index)
{ }

////////////////////////////////////////////////////////////////////////////////

template <class T, size_t ChunkSize>
size_t TPersistentQueueBase<T, ChunkSize>::Size() const
{
    return Size_;
}

template <class T, size_t ChunkSize>
bool TPersistentQueueBase<T, ChunkSize>::Empty() const
{
    return Size_ == 0;
}

template <class T, size_t ChunkSize>
auto TPersistentQueueBase<T, ChunkSize>::Begin() const -> TIterator
{
    return Tail_;
}

template <class T, size_t ChunkSize>
auto TPersistentQueueBase<T, ChunkSize>::End() const -> TIterator
{
    return Head_;
}

template <class T, size_t ChunkSize>
auto TPersistentQueueBase<T, ChunkSize>::begin() const -> TIterator
{
    return Begin();
}

template <class T, size_t ChunkSize>
auto TPersistentQueueBase<T, ChunkSize>::end() const -> TIterator
{
    return End();
}

////////////////////////////////////////////////////////////////////////////////

template <class T, size_t ChunkSize>
void TPersistentQueue<T, ChunkSize>::Enqueue(T value)
{
    auto& head = this->Head_;
    auto& tail = this->Tail_;
    auto& size = this->Size_;

    if (!head.CurrentChunk_) {
        auto chunk = New<TChunk>();
        head.CurrentChunk_ = tail.CurrentChunk_ = chunk;
        head.CurrentIndex_ = tail.CurrentIndex_ = 0;
    }

    head.CurrentChunk_->Elements[head.CurrentIndex_++] = std::move(value);
    ++size;

    if (head.CurrentIndex_ == ChunkSize) {
        auto chunk = New<TChunk>();
        head.CurrentChunk_->Next = chunk;
        head.CurrentChunk_ = chunk;
        head.CurrentIndex_ = 0;
    }
}

template <class T, size_t ChunkSize>
T TPersistentQueue<T, ChunkSize>::Dequeue()
{
    auto& tail = this->Tail_;
    auto& size = this->Size_;

    YASSERT(size != 0);

    auto result = std::move(tail.CurrentChunk_->Elements[tail.CurrentIndex_++]);
    --size;

    if (tail.CurrentIndex_ == ChunkSize) {
        tail.CurrentChunk_ = tail.CurrentChunk_->Next;
        tail.CurrentIndex_ = 0;
    }

    return result;
}

template <class T, size_t ChunkSize>
auto TPersistentQueue<T, ChunkSize>::MakeSnapshot() const -> TSnapshot
{
    TSnapshot snapshot;
    snapshot.Head_ = this->Head_;
    snapshot.Tail_ = this->Tail_;
    snapshot.Size_ = this->Size_;
    return snapshot;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
