#ifndef READ_SESSION_IMPL
#error "Do not include this file directly"
#endif
// #include "read_session.h"

#include "persqueue_impl.h"
#include "common.h"

#define INCLUDE_YDB_INTERNAL_H
#include <ydb/public/sdk/cpp/client/impl/ydb_internal/logger/log.h>
#undef INCLUDE_YDB_INTERNAL_H

#include <google/protobuf/util/time_util.h>

#include <library/cpp/containers/disjoint_interval_tree/disjoint_interval_tree.h>
#include <util/generic/guid.h>
#include <util/generic/size_literals.h>
#include <util/generic/utility.h>
#include <util/generic/yexception.h>
#include <util/stream/mem.h>
#include <util/system/env.h>

#include <variant>

// Forward delcarations

namespace NYdb::NTopic {
    class TReadSession;
}

namespace NYdb::NPersQueue::NCompressionDetails {
    extern TString Decompress(const Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::DataBatch::MessageData& data);
}

namespace NYdb::NTopic::NCompressionDetails {
    extern TString Decompress(const Ydb::Topic::StreamReadMessage::ReadResponse::MessageData& data, Ydb::Topic::Codec codec);
}

namespace NYdb::NPersQueue {

static const bool RangesMode = !GetEnv("PQ_OFFSET_RANGES_MODE").empty();

template <typename TReaderCounters>
void MakeCountersNotNull(TReaderCounters& counters);
template <typename TReaderCounters>
bool HasNullCounters(TReaderCounters& counters);

template <bool UseMigrationProtocol>
class TErrorHandler : public IErrorHandler<UseMigrationProtocol> {
    using TReadSession = typename std::conditional_t<UseMigrationProtocol,
        NPersQueue::TReadSession,
        NTopic::TReadSession>;
public:
    TErrorHandler(std::weak_ptr<TReadSession> session)
        : Session(std::move(session))
    {
    }

    void AbortSession(TASessionClosedEvent<UseMigrationProtocol>&& closeEvent) override;

private:
    std::weak_ptr<TReadSession> Session;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TPartitionStreamImpl

template<bool UseMigrationProtocol>
TLog TPartitionStreamImpl<UseMigrationProtocol>::GetLog() const {
    if (auto session = Session.lock()) {
        return session->GetLog();
    }
    return {};
}

template<bool UseMigrationProtocol>
void TPartitionStreamImpl<UseMigrationProtocol>::Commit(ui64 startOffset, ui64 endOffset) {
    std::vector<std::pair<ui64, ui64>> toCommit;
    if (auto sessionShared = Session.lock()) {
        Y_VERIFY(endOffset > startOffset);
        with_lock(sessionShared->Lock) {
            if (!AddToCommitRanges(startOffset, endOffset, true)) // Add range for real commit always.
                return;

            Y_VERIFY(!Commits.Empty());
            for (auto c : Commits) {
                if (c.first >= endOffset) break; // Commit only gaps before client range.
                toCommit.emplace_back(c);
            }
            Commits.EraseInterval(0, endOffset); // Drop only committed ranges;
        }
        for (auto range: toCommit) {
            sessionShared->Commit(this, range.first, range.second);
        }
    }
}

template<bool UseMigrationProtocol>
void TPartitionStreamImpl<UseMigrationProtocol>::RequestStatus() {
    if (auto sessionShared = Session.lock()) {
        sessionShared->RequestPartitionStreamStatus(this);
    }
}

template<bool UseMigrationProtocol>
void TPartitionStreamImpl<UseMigrationProtocol>::ConfirmCreate(TMaybe<ui64> readOffset, TMaybe<ui64> commitOffset) {
    if (auto sessionShared = Session.lock()) {
        sessionShared->ConfirmPartitionStreamCreate(this, readOffset, commitOffset);
    }
}

template<bool UseMigrationProtocol>
void TPartitionStreamImpl<UseMigrationProtocol>::ConfirmDestroy() {
    if (auto sessionShared = Session.lock()) {
        sessionShared->ConfirmPartitionStreamDestroy(this);
    }
}

template<bool UseMigrationProtocol>
void TPartitionStreamImpl<UseMigrationProtocol>::StopReading() {
    Y_FAIL("Not implemented"); // TODO
}

template<bool UseMigrationProtocol>
void TPartitionStreamImpl<UseMigrationProtocol>::ResumeReading() {
    Y_FAIL("Not implemented"); // TODO
}

template<bool UseMigrationProtocol>
void TPartitionStreamImpl<UseMigrationProtocol>::SignalReadyEvents(TReadSessionEventsQueue<UseMigrationProtocol>* queue, TDeferredActions<UseMigrationProtocol>& deferred) {
    Y_VERIFY(queue);
    EventsQueue.SignalReadyEvents(*this, *queue, deferred);
}

template<bool UseMigrationProtocol>
void TPartitionStreamImpl<UseMigrationProtocol>::DeleteNotReadyTail(TDeferredActions<UseMigrationProtocol>& deferred)
{
    EventsQueue.DeleteNotReadyTail(deferred);
}

template<bool UseMigrationProtocol>
void TRawPartitionStreamEventQueue<UseMigrationProtocol>::SignalReadyEvents(TPartitionStreamImpl<UseMigrationProtocol>& stream,
                                                                            TReadSessionEventsQueue<UseMigrationProtocol>& queue,
                                                                            TDeferredActions<UseMigrationProtocol>& deferred)
{
    while (!NotReady.empty() && NotReady.front().IsReady()) {
        auto& event = NotReady.front();

        queue.SignalEventImpl(&stream, deferred, event.IsDataEvent());

        Ready.push_back(std::move(event));
        NotReady.pop_front();
    }
}

template<bool UseMigrationProtocol>
void TRawPartitionStreamEventQueue<UseMigrationProtocol>::DeleteNotReadyTail(TDeferredActions<UseMigrationProtocol>& deferred)
{
    std::deque<TRawPartitionStreamEvent<UseMigrationProtocol>> ready;

    auto i = NotReady.begin();
    for (; (i != NotReady.end()) && i->IsReady(); ++i) {
        ready.push_back(std::move(*i));
    }

    std::vector<TDataDecompressionInfoPtr<UseMigrationProtocol>> infos;

    for (; i != NotReady.end(); ++i) {
        if (i->IsDataEvent()) {
            infos.push_back(i->GetDataEvent().GetParent());
        }
    }

    deferred.DeferDestroyDecompressionInfos(std::move(infos));

    swap(ready, NotReady);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TSingleClusterReadSessionImpl

template<bool UseMigrationProtocol>
TStringBuilder TSingleClusterReadSessionImpl<UseMigrationProtocol>::GetLogPrefix() const {
    return TStringBuilder() << GetDatabaseLogPrefix(Database) << "[" << SessionId << "] [" << ClusterName << "] ";
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::Start() {
    Settings.DecompressionExecutor_->Start();
    Settings.EventHandlers_.HandlersExecutor_->Start();
    if (!Reconnect(TPlainStatus())) {
        ErrorHandler->AbortSession(EStatus::ABORTED, "Driver is stopping");
    }
}

template<bool UseMigrationProtocol>
bool TSingleClusterReadSessionImpl<UseMigrationProtocol>::Reconnect(const TPlainStatus& status) {
    TDuration delay = TDuration::Zero();
    NGrpc::IQueueClientContextPtr delayContext = nullptr;
    NGrpc::IQueueClientContextPtr connectContext = ClientContext->CreateContext();
    NGrpc::IQueueClientContextPtr connectTimeoutContext = ClientContext->CreateContext();
    if (!connectContext || !connectTimeoutContext) {
        return false;
    }

    // Previous operations contexts.
    NGrpc::IQueueClientContextPtr prevConnectContext;
    NGrpc::IQueueClientContextPtr prevConnectTimeoutContext;
    NGrpc::IQueueClientContextPtr prevConnectDelayContext;

    if (!status.Ok()) {
        Log.Write(TLOG_INFO, GetLogPrefix() << "Got error. Status: " << status.Status
                                            << ". Description: " << IssuesSingleLineString(status.Issues));
    }

    TDeferredActions<UseMigrationProtocol> deferred;
    with_lock (Lock) {
        if (Aborting) {
            Cancel(connectContext);
            Cancel(connectTimeoutContext);
            return false;
        }
        Processor = nullptr;
        WaitingReadResponse = false;
        ServerMessage = std::make_shared<TServerMessage<UseMigrationProtocol>>();
        ++ConnectionGeneration;

        ReadSizeBudget += ReadSizeServerDelta;
        ReadSizeServerDelta = 0;

        if (!RetryState) {
            RetryState = Settings.RetryPolicy_->CreateRetryState();
        }
        if (!status.Ok()) {
            TMaybe<TDuration> nextDelay = RetryState->GetNextRetryDelay(status.Status);
            if (!nextDelay) {
                return false;
            }
            delay = *nextDelay;
            delayContext = ClientContext->CreateContext();
            if (!delayContext) {
                return false;
            }
        }

        Log.Write(TLOG_DEBUG, GetLogPrefix() << "Reconnecting session to cluster " << ClusterName << " in " << delay);

        ++ConnectionAttemptsDone;

        // Set new context
        prevConnectContext = std::exchange(ConnectContext, connectContext);
        prevConnectTimeoutContext = std::exchange(ConnectTimeoutContext, connectTimeoutContext);
        prevConnectDelayContext = std::exchange(ConnectDelayContext, delayContext);

        Y_ASSERT(ConnectContext);
        Y_ASSERT(ConnectTimeoutContext);
        Y_ASSERT((delay == TDuration::Zero()) == !ConnectDelayContext);

        // Destroy all partition streams before connecting.
        DestroyAllPartitionStreamsImpl(deferred);
    }

    // Cancel previous operations.
    Cancel(prevConnectContext);
    Cancel(prevConnectTimeoutContext);
    Cancel(prevConnectDelayContext);

    auto connectCallback = [weakThis = TSingleClusterReadSessionImpl<UseMigrationProtocol>::weak_from_this(),
                            connectContext = connectContext](TPlainStatus&& st, typename IProcessor::TPtr&& processor) {
        if (auto sharedThis = weakThis.lock()) {
            sharedThis->OnConnect(std::move(st), std::move(processor),
                                  connectContext); // OnConnect could be called inplace!
        }
    };

    auto connectTimeoutCallback = [weakThis = TSingleClusterReadSessionImpl<UseMigrationProtocol>::weak_from_this(),
                                   connectTimeoutContext = connectTimeoutContext](bool ok) {
        if (ok) {
            if (auto sharedThis = weakThis.lock()) {
                sharedThis->OnConnectTimeout(connectTimeoutContext);
            }
        }
    };

    Y_ASSERT(connectContext);
    Y_ASSERT(connectTimeoutContext);
    Y_ASSERT((delay == TDuration::Zero()) == !delayContext);
    ConnectionFactory->CreateProcessor(
        std::move(connectCallback),
        TRpcRequestSettings::Make(Settings),
        std::move(connectContext),
        TDuration::Seconds(30) /* connect timeout */, // TODO: make connect timeout setting.
        std::move(connectTimeoutContext),
        std::move(connectTimeoutCallback),
        delay,
        std::move(delayContext));
    return true;
}

template <bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::BreakConnectionAndReconnectImpl(
    TPlainStatus&& status, TDeferredActions<UseMigrationProtocol>& deferred) {
    Log.Write(TLOG_INFO,
              GetLogPrefix() << "Break connection due to unexpected message from server. Status: " << status.Status
                             << ", Issues: \"" << IssuesSingleLineString(status.Issues) << "\"");

    Processor->Cancel();
    Processor = nullptr;
    RetryState = Settings.RetryPolicy_->CreateRetryState(); // Explicitly create retry state to determine whether we should connect to server again.

    deferred.DeferReconnection(TSingleClusterReadSessionImpl<UseMigrationProtocol>::shared_from_this(), ErrorHandler, std::move(status));
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::OnConnectTimeout(const NGrpc::IQueueClientContextPtr& connectTimeoutContext) {
    with_lock (Lock) {
        if (ConnectTimeoutContext == connectTimeoutContext) {
            Cancel(ConnectContext);
            ConnectContext = nullptr;
            ConnectTimeoutContext = nullptr;
            ConnectDelayContext = nullptr;

            if (Closing || Aborting) {
                CallCloseCallbackImpl();
                return;
            }
        } else {
            return;
        }
    }

    ++*Settings.Counters_->Errors;
    TStringBuilder description;
    description << "Failed to establish connection to server. Attempts done: " << ConnectionAttemptsDone;
    if (!Reconnect(TPlainStatus(EStatus::TIMEOUT, description))) {
        ErrorHandler->AbortSession(EStatus::TIMEOUT, description);
    }
}

template <bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::OnConnect(
    TPlainStatus&& st, typename IProcessor::TPtr&& processor, const NGrpc::IQueueClientContextPtr& connectContext) {
    TDeferredActions<UseMigrationProtocol> deferred;
    with_lock (Lock) {
        if (ConnectContext == connectContext) {
            Cancel(ConnectTimeoutContext);
            ConnectContext = nullptr;
            ConnectTimeoutContext = nullptr;
            ConnectDelayContext = nullptr;

            if (Closing || Aborting) {
                CallCloseCallbackImpl();
                return;
            }

            if (st.Ok()) {
                Processor = std::move(processor);
                ConnectionAttemptsDone = 0;
                InitImpl(deferred);
                return;
            }
        } else {
            return;
        }
    }

    if (!st.Ok()) {
        ++*Settings.Counters_->Errors;
        if (!Reconnect(st)) {
            ErrorHandler->AbortSession(
                st.Status, MakeIssueWithSubIssues(TStringBuilder() << "Failed to establish connection to server \""
                                                                   << st.Endpoint << "\" ( cluster " << ClusterName
                                                                   << "). Attempts done: " << ConnectionAttemptsDone,
                                                  st.Issues));
        }
    }
}

template<>
inline void TSingleClusterReadSessionImpl<true>::InitImpl(TDeferredActions<true>& deferred) { // Assumes that we're under lock.
    Log.Write(TLOG_DEBUG, GetLogPrefix() << "Successfully connected. Initializing session");
    TClientMessage<true> req;
    auto& init = *req.mutable_init_request();
    init.set_ranges_mode(GetRangesMode());
    for (const TTopicReadSettings& topic : Settings.Topics_) {
        auto* topicSettings = init.add_topics_read_settings();
        topicSettings->set_topic(topic.Path_);
        if (topic.StartingMessageTimestamp_) {
            topicSettings->set_start_from_written_at_ms(topic.StartingMessageTimestamp_->MilliSeconds());
        }
        for (ui64 groupId : topic.PartitionGroupIds_) {
            topicSettings->add_partition_group_ids(groupId);
        }
    }
    init.set_consumer(Settings.ConsumerName_);
    init.set_read_only_original(Settings.ReadOnlyOriginal_);
    init.mutable_read_params()->set_max_read_size(Settings.MaxMemoryUsageBytes_);
    if (Settings.MaxTimeLag_) {
        init.set_max_lag_duration_ms(Settings.MaxTimeLag_->MilliSeconds());
    }
    if (Settings.StartingMessageTimestamp_) {
        init.set_start_from_written_at_ms(Settings.StartingMessageTimestamp_->MilliSeconds());
    }

    WriteToProcessorImpl(std::move(req));
    ReadFromProcessorImpl(deferred);
}

template<>
inline void TSingleClusterReadSessionImpl<false>::InitImpl(TDeferredActions<false>& deferred) { // Assumes that we're under lock.
    Log.Write(TLOG_DEBUG, GetLogPrefix() << "Successfully connected. Initializing session");
    TClientMessage<false> req;
    auto& init = *req.mutable_init_request();

    init.set_consumer(Settings.ConsumerName_);

    for (const NTopic::TTopicReadSettings& topic : Settings.Topics_) {
        auto* topicSettings = init.add_topics_read_settings();
        topicSettings->set_path(topic.Path_);
        for (ui64 partitionId : topic.PartitionIds_) {
            topicSettings->add_partition_ids(partitionId);
        }

        if (topic.ReadFromTimestamp_) {
            *topicSettings->mutable_read_from() =
                ::google::protobuf::util::TimeUtil::MillisecondsToTimestamp(topic.ReadFromTimestamp_->MilliSeconds());
        } else if (Settings.ReadFromTimestamp_) {
            *topicSettings->mutable_read_from() =
                ::google::protobuf::util::TimeUtil::MillisecondsToTimestamp(Settings.ReadFromTimestamp_->MilliSeconds());
        }

        if (topic.MaxLag_) {
            *topicSettings->mutable_max_lag() =
                ::google::protobuf::util::TimeUtil::MillisecondsToDuration(topic.MaxLag_->MilliSeconds());
        } else if (Settings.ReadFromTimestamp_) {
            *topicSettings->mutable_max_lag() =
                ::google::protobuf::util::TimeUtil::MillisecondsToDuration(Settings.MaxLag_->MilliSeconds());
        }
    }

    WriteToProcessorImpl(std::move(req));
    ReadFromProcessorImpl(deferred);
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::ContinueReadingDataImpl() { // Assumes that we're under lock.
    if (!Closing
        && !Aborting
        && !WaitingReadResponse
        && !DataReadingSuspended
        && Processor
        && CompressedDataSize < GetCompressedDataSizeLimit()
        && static_cast<size_t>(CompressedDataSize + DecompressedDataSize) < Settings.MaxMemoryUsageBytes_)
    {
        TClientMessage<UseMigrationProtocol> req;
        if constexpr (UseMigrationProtocol) {
            req.mutable_read();
        } else {
            if (ReadSizeBudget <= 0 || ReadSizeServerDelta + ReadSizeBudget <= 0) {
                return;
            }
            req.mutable_read_request()->set_bytes_size(ReadSizeBudget);
            ReadSizeServerDelta += ReadSizeBudget;

            ReadSizeBudget = 0;
        }

        WriteToProcessorImpl(std::move(req));
        WaitingReadResponse = true;
    }
}

template<bool UseMigrationProtocol>
ui64 GetPartitionStreamId(const TPartitionStreamImpl<UseMigrationProtocol>* partitionStream) {
    if constexpr (UseMigrationProtocol) {
        return partitionStream->GetPartitionStreamId();
    } else {
        return partitionStream->GetPartitionSessionId();
    }
}

template<bool UseMigrationProtocol>
TString GetCluster(const TPartitionStreamImpl<UseMigrationProtocol>* partitionStream) {
    if constexpr (UseMigrationProtocol) {
        return partitionStream->GetCluster();
    } else {
        return "-";
    }
}

template<bool UseMigrationProtocol>
bool TSingleClusterReadSessionImpl<UseMigrationProtocol>::IsActualPartitionStreamImpl(const TPartitionStreamImpl<UseMigrationProtocol>* partitionStream) { // Assumes that we're under lock.
    auto actualPartitionStreamIt = PartitionStreams.find(partitionStream->GetAssignId());
    return actualPartitionStreamIt != PartitionStreams.end()
        && GetPartitionStreamId(actualPartitionStreamIt->second.Get()) == GetPartitionStreamId(partitionStream);
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::ConfirmPartitionStreamCreate(const TPartitionStreamImpl<UseMigrationProtocol>* partitionStream, TMaybe<ui64> readOffset, TMaybe<ui64> commitOffset) {
    TStringBuilder commitOffsetLogStr;
    if (commitOffset) {
        commitOffsetLogStr << ". Commit offset: " << *commitOffset;
    }
    Log.Write(
        TLOG_INFO,
        GetLogPrefix() << "Confirm partition stream create. Partition stream id: " << GetPartitionStreamId(partitionStream)
            << ". Cluster: \"" << GetCluster(partitionStream) << "\". Topic: \"" << partitionStream->GetTopicPath()
            << "\". Partition: " << partitionStream->GetPartitionId()
            << ". Read offset: " << readOffset << commitOffsetLogStr
    );

    with_lock (Lock) {
        if (Aborting || Closing || !IsActualPartitionStreamImpl(partitionStream)) { // Got previous incarnation.
            Log.Write(
                TLOG_DEBUG,
                GetLogPrefix() << "Skip partition stream create confirm. Partition stream id: "
                    << GetPartitionStreamId(partitionStream)
            );
            return;
        }

        TClientMessage<UseMigrationProtocol> req;

        if constexpr (UseMigrationProtocol) {
            auto& startRead = *req.mutable_start_read();
            startRead.mutable_topic()->set_path(partitionStream->GetTopicPath());
            startRead.set_cluster(partitionStream->GetCluster());
            startRead.set_partition(partitionStream->GetPartitionId());
            startRead.set_assign_id(partitionStream->GetAssignId());
            if (readOffset) {
                startRead.set_read_offset(*readOffset);
            }
            if (commitOffset) {
                startRead.set_commit_offset(*commitOffset);
            }
        } else {
            auto& startRead = *req.mutable_start_partition_session_response();
            startRead.set_partition_session_id(partitionStream->GetAssignId());
            if (readOffset) {
                startRead.set_read_offset(*readOffset);
            }
            if (commitOffset) {
                startRead.set_commit_offset(*commitOffset);
            }
        }

        WriteToProcessorImpl(std::move(req));
    }
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::ConfirmPartitionStreamDestroy(TPartitionStreamImpl<UseMigrationProtocol>* partitionStream) {
    Log.Write(
        TLOG_INFO,
        GetLogPrefix() << "Confirm partition stream destroy. Partition stream id: "
            << GetPartitionStreamId(partitionStream)
            << ". Cluster: \"" << GetCluster(partitionStream) << "\". Topic: \"" << partitionStream->GetTopicPath()
            << "\". Partition: " << partitionStream->GetPartitionId()
    );

    TDeferredActions<UseMigrationProtocol> deferred;
    with_lock (Lock) {
        if (Aborting || Closing || !IsActualPartitionStreamImpl(partitionStream)) { // Got previous incarnation.
            Log.Write(
                TLOG_DEBUG,
                GetLogPrefix() << "Skip partition stream destroy confirm. Partition stream id: "
                    << GetPartitionStreamId(partitionStream)
            );
            return;
        }

        using TClosedEvent = std::conditional_t<
            UseMigrationProtocol,
                NPersQueue::TReadSessionEvent::TPartitionStreamClosedEvent,
                NTopic::TReadSessionEvent::TPartitionSessionClosedEvent
        >;

        CookieMapping.RemoveMapping(GetPartitionStreamId(partitionStream));
        PartitionStreams.erase(partitionStream->GetAssignId());

        if constexpr (UseMigrationProtocol) {
            EventsQueue->PushEvent(partitionStream, TSingleClusterReadSessionImpl<UseMigrationProtocol>::weak_from_this(),
                                   TClosedEvent(partitionStream, TClosedEvent::EReason::DestroyConfirmedByUser),
                                   deferred);
        } else {
            EventsQueue->PushEvent(partitionStream, TSingleClusterReadSessionImpl<UseMigrationProtocol>::weak_from_this(),
                                   TClosedEvent(partitionStream, TClosedEvent::EReason::StopConfirmedByUser),
                                   deferred);
        }

        TClientMessage<UseMigrationProtocol> req;

        if constexpr (UseMigrationProtocol) {
            auto& released = *req.mutable_released();
            released.mutable_topic()->set_path(partitionStream->GetTopicPath());
            released.set_cluster(partitionStream->GetCluster());
            released.set_partition(partitionStream->GetPartitionId());
            released.set_assign_id(partitionStream->GetAssignId());
        } else {
            auto& released = *req.mutable_stop_partition_session_response();
            released.set_partition_session_id(partitionStream->GetAssignId());
        }

        WriteToProcessorImpl(std::move(req));
    }
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::Commit(const TPartitionStreamImpl<UseMigrationProtocol>* partitionStream, ui64 startOffset, ui64 endOffset) {
    Log.Write(
        TLOG_DEBUG,
        GetLogPrefix() << "Commit offsets [" << startOffset << ", " << endOffset
            << "). Partition stream id: " << GetPartitionStreamId(partitionStream)
    );
    with_lock (Lock) {
        if (Aborting || Closing || !IsActualPartitionStreamImpl(partitionStream)) { // Got previous incarnation.
            return;
        }
        TClientMessage<UseMigrationProtocol> req;
        bool hasSomethingToCommit = false;

        if constexpr (UseMigrationProtocol) {
            if (GetRangesMode()) {
                hasSomethingToCommit = true;
                auto* range = req.mutable_commit()->add_offset_ranges();
                range->set_assign_id(partitionStream->GetAssignId());
                range->set_start_offset(startOffset);
                range->set_end_offset(endOffset);
            } else {
                for (ui64 offset = startOffset; offset < endOffset; ++offset) {
                    typename TPartitionCookieMapping::TCookie::TPtr cookie = CookieMapping.CommitOffset(GetPartitionStreamId(partitionStream), offset);
                    if (cookie) {
                        hasSomethingToCommit = true;
                        auto* cookieInfo = req.mutable_commit()->add_cookies();
                        cookieInfo->set_assign_id(partitionStream->GetAssignId());
                        cookieInfo->set_partition_cookie(cookie->Cookie);
                    }
                }
            }
        } else {
            hasSomethingToCommit = true;
            auto* part_commit = req.mutable_commit_offset_request()->add_commit_offsets();
            part_commit->set_partition_session_id(partitionStream->GetAssignId());
            auto* range = part_commit->add_offsets();
            range->set_start(startOffset);
            range->set_end(endOffset);
        }

        if (hasSomethingToCommit) {
            WriteToProcessorImpl(std::move(req));
        }
    }
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::RequestPartitionStreamStatus(const TPartitionStreamImpl<UseMigrationProtocol>* partitionStream) {
    Log.Write(
        TLOG_DEBUG,
        GetLogPrefix() << "Requesting status for partition stream id: " << GetPartitionStreamId(partitionStream)
    );
    with_lock (Lock) {
        if (Aborting || Closing || !IsActualPartitionStreamImpl(partitionStream)) { // Got previous incarnation.
            return;
        }

        TClientMessage<UseMigrationProtocol> req;

        if constexpr (UseMigrationProtocol) {
            auto& status = *req.mutable_status();
            status.mutable_topic()->set_path(partitionStream->GetTopicPath());
            status.set_cluster(partitionStream->GetCluster());
            status.set_partition(partitionStream->GetPartitionId());
            status.set_assign_id(partitionStream->GetAssignId());
        } else {
            auto& status = *req.mutable_partition_session_status_request();
            status.set_partition_session_id(partitionStream->GetAssignId());
        }

        WriteToProcessorImpl(std::move(req));
    }
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::OnUserRetrievedEvent(i64 decompressedSize, size_t messagesCount)
{
    Log.Write(TLOG_DEBUG, GetLogPrefix()
                          << "The application data is transferred to the client. Number of messages "
                          << messagesCount
                          << ", size "
                          << decompressedSize
                          << " bytes");

    *Settings.Counters_->MessagesInflight -= messagesCount;
    *Settings.Counters_->BytesInflightTotal -= decompressedSize;
    *Settings.Counters_->BytesInflightUncompressed -= decompressedSize;

    TDeferredActions<UseMigrationProtocol> deferred;
    with_lock (Lock) {
        UpdateMemoryUsageStatisticsImpl();

        Y_VERIFY(decompressedSize <= DecompressedDataSize);
        DecompressedDataSize -= decompressedSize;

        ContinueReadingDataImpl();
        StartDecompressionTasksImpl(deferred);
    }
}

template <bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::WriteToProcessorImpl(
    TClientMessage<UseMigrationProtocol>&& req) { // Assumes that we're under lock.

    if (Processor) {
        Processor->Write(std::move(req));
    }
}

template<bool UseMigrationProtocol>
bool TSingleClusterReadSessionImpl<UseMigrationProtocol>::HasCommitsInflightImpl() const {
    for (const auto& [id, partitionStream] : PartitionStreams) {
        if (partitionStream->HasCommitsInflight())
            return true;
    }
    return false;
}

template <bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::ReadFromProcessorImpl(
    TDeferredActions<UseMigrationProtocol>& deferred) { // Assumes that we're under lock.
    if (Closing && !HasCommitsInflightImpl()) {
        Processor->Cancel();
        CallCloseCallbackImpl();
        return;
    }

    if (Processor) {
        ServerMessage->Clear();

        auto callback = [weakThis = TSingleClusterReadSessionImpl<UseMigrationProtocol>::weak_from_this(),
                         connectionGeneration = ConnectionGeneration,
                         // Capture message & processor not to read in freed memory.
                         serverMessage = ServerMessage,
                         processor = Processor](NGrpc::TGrpcStatus&& grpcStatus) {
            if (auto sharedThis = weakThis.lock()) {
                sharedThis->OnReadDone(std::move(grpcStatus), connectionGeneration);
            }
        };

        deferred.DeferReadFromProcessor(Processor, ServerMessage.get(), std::move(callback));
    }
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::OnReadDone(NGrpc::TGrpcStatus&& grpcStatus, size_t connectionGeneration) {
    TPlainStatus errorStatus;
    if (!grpcStatus.Ok()) {
        errorStatus = TPlainStatus(std::move(grpcStatus));
    }

    TDeferredActions<UseMigrationProtocol> deferred;
    with_lock (Lock) {
        if (Aborting) {
            return;
        }

        if (connectionGeneration != ConnectionGeneration) {
            return; // Message from previous connection. Ignore.
        }
        if (errorStatus.Ok()) {
            if (IsErrorMessage(*ServerMessage)) {
                errorStatus = MakeErrorFromProto(*ServerMessage);
            } else {

                if constexpr (UseMigrationProtocol) {
                    switch (ServerMessage->response_case()) {
                    case Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::kInitResponse:
                        OnReadDoneImpl(std::move(*ServerMessage->mutable_init_response()), deferred);
                        break;
                    case Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::kDataBatch:
                        OnReadDoneImpl(std::move(*ServerMessage->mutable_data_batch()), deferred);
                        break;
                    case Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::kAssigned:
                        OnReadDoneImpl(std::move(*ServerMessage->mutable_assigned()), deferred);
                        break;
                    case Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::kRelease:
                        OnReadDoneImpl(std::move(*ServerMessage->mutable_release()), deferred);
                        break;
                    case Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::kCommitted:
                        OnReadDoneImpl(std::move(*ServerMessage->mutable_committed()), deferred);
                        break;
                    case Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::kPartitionStatus:
                        OnReadDoneImpl(std::move(*ServerMessage->mutable_partition_status()), deferred);
                        break;
                    case Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::RESPONSE_NOT_SET:
                        errorStatus = TPlainStatus::Internal("Unexpected response from server");
                        break;
                    }
                } else {
                    switch (ServerMessage->server_message_case()) {
                    case TServerMessage<false>::kInitResponse:
                        OnReadDoneImpl(std::move(*ServerMessage->mutable_init_response()), deferred);
                        break;
                    case TServerMessage<false>::kReadResponse:
                        OnReadDoneImpl(std::move(*ServerMessage->mutable_read_response()), deferred);
                        break;
                    case TServerMessage<false>::kStartPartitionSessionRequest:
                        OnReadDoneImpl(std::move(*ServerMessage->mutable_start_partition_session_request()), deferred);
                        break;
                    case TServerMessage<false>::kStopPartitionSessionRequest:
                        OnReadDoneImpl(std::move(*ServerMessage->mutable_stop_partition_session_request()), deferred);
                        break;
                    case TServerMessage<false>::kCommitOffsetResponse:
                        OnReadDoneImpl(std::move(*ServerMessage->mutable_commit_offset_response()), deferred);
                        break;
                    case TServerMessage<false>::kPartitionSessionStatusResponse:
                        OnReadDoneImpl(std::move(*ServerMessage->mutable_partition_session_status_response()), deferred);
                        break;
                    case TServerMessage<false>::kUpdateTokenResponse:
                        OnReadDoneImpl(std::move(*ServerMessage->mutable_update_token_response()), deferred);
                        break;
                    case TServerMessage<false>::SERVER_MESSAGE_NOT_SET:
                        errorStatus = TPlainStatus::Internal("Unexpected response from server");
                        break;
                    }
                }

                if (errorStatus.Ok()) {
                    ReadFromProcessorImpl(deferred); // Read next.
                }
            }
        }
    }
    if (!errorStatus.Ok()) {
        ++*Settings.Counters_->Errors;

        if (!Reconnect(errorStatus)) {
            ErrorHandler->AbortSession(std::move(errorStatus));
        }
    }
}

template <>
template <>
inline void TSingleClusterReadSessionImpl<true>::OnReadDoneImpl(
    Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::InitResponse&& msg,
    TDeferredActions<true>& deferred) { // Assumes that we're under lock.
    Y_UNUSED(deferred);

    Log.Write(TLOG_INFO, GetLogPrefix() << "Server session id: " << msg.session_id());

    RetryState = nullptr;

    // Successful init. Do nothing.
    ContinueReadingDataImpl();
}

template <>
template <>
inline void TSingleClusterReadSessionImpl<true>::OnReadDoneImpl(
    Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::DataBatch&& msg,
    TDeferredActions<true>& deferred) { // Assumes that we're under lock.
    if (Closing || Aborting) {
        return; // Don't process new data.
    }
    UpdateMemoryUsageStatisticsImpl();
    for (TPartitionData<true>& partitionData : *msg.mutable_partition_data()) {
        auto partitionStreamIt = PartitionStreams.find(partitionData.cookie().assign_id());
        if (partitionStreamIt == PartitionStreams.end()) {
            ++*Settings.Counters_->Errors;
            BreakConnectionAndReconnectImpl(EStatus::INTERNAL_ERROR,
                                            TStringBuilder()
                                                << "Got unexpected partition stream data message. Topic: "
                                                << partitionData.topic() << ". Partition: " << partitionData.partition()
                                                << " AssignId: " << partitionData.cookie().assign_id(),
                                            deferred);
            return;
        }
        const TIntrusivePtr<TPartitionStreamImpl<true>>& partitionStream = partitionStreamIt->second;

        typename TPartitionCookieMapping::TCookie::TPtr cookie = MakeIntrusive<typename TPartitionCookieMapping::TCookie>(partitionData.cookie().partition_cookie(), partitionStream);

        ui64 firstOffset = std::numeric_limits<ui64>::max();
        ui64 currentOffset = std::numeric_limits<ui64>::max();
        ui64 desiredOffset = partitionStream->GetFirstNotReadOffset();
        for (const Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::DataBatch::Batch& batch : partitionData.batches()) {
            // Validate messages.
            for (const Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::DataBatch::MessageData& messageData : batch.message_data()) {
                // Check offsets continuity.
                if (messageData.offset() != desiredOffset) {
                    bool res = partitionStream->AddToCommitRanges(desiredOffset, messageData.offset(), GetRangesMode());
                    Y_VERIFY(res);
                }

                if (firstOffset == std::numeric_limits<ui64>::max()) {
                    firstOffset = messageData.offset();
                }
                currentOffset = messageData.offset();
                desiredOffset = currentOffset + 1;
                partitionStream->UpdateMaxReadOffset(currentOffset);
                const i64 messageSize = static_cast<i64>(messageData.data().size());
                CompressedDataSize += messageSize;
                *Settings.Counters_->BytesInflightTotal += messageSize;
                *Settings.Counters_->BytesInflightCompressed += messageSize;
                ++*Settings.Counters_->MessagesInflight;
            }
        }
        if (firstOffset == std::numeric_limits<ui64>::max()) {
            BreakConnectionAndReconnectImpl(EStatus::INTERNAL_ERROR,
                                            TStringBuilder() << "Got empty data message. Topic: "
                                                << partitionData.topic()
                                                << ". Partition: " << partitionData.partition()
                                                << " message: " << msg,
                                            deferred);
            return;
        }
        cookie->SetOffsetRange(std::make_pair(firstOffset, desiredOffset));
        partitionStream->SetFirstNotReadOffset(desiredOffset);
        if (!CookieMapping.AddMapping(cookie)) {
            BreakConnectionAndReconnectImpl(EStatus::INTERNAL_ERROR,
                                            TStringBuilder() << "Got unexpected data message. Topic: "
                                                << partitionData.topic()
                                                << ". Partition: " << partitionData.partition()
                                                << ". Cookie mapping already has such cookie",
                                            deferred);
            return;
        }

        auto decompressionInfo = std::make_shared<TDataDecompressionInfo<true>>(std::move(partitionData),
                                                                                shared_from_this(),
                                                                                Settings.Decompress_);
        Y_VERIFY(decompressionInfo);

        decompressionInfo->PlanDecompressionTasks(AverageCompressionRatio,
                                                  partitionStream);

        DecompressionQueue.emplace_back(decompressionInfo, partitionStream);
        StartDecompressionTasksImpl(deferred);
    }

    WaitingReadResponse = false;
    ContinueReadingDataImpl();
}

template <>
template <>
inline void TSingleClusterReadSessionImpl<true>::OnReadDoneImpl(
    Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::Assigned&& msg,
    TDeferredActions<true>& deferred) { // Assumes that we're under lock.
    auto partitionStream = MakeIntrusive<TPartitionStreamImpl<true>>(
        NextPartitionStreamId, msg.topic().path(), msg.cluster(),
        msg.partition() + 1, // Group.
        msg.partition(),     // Partition.
        msg.assign_id(), msg.read_offset(), weak_from_this(),
        ErrorHandler);
    NextPartitionStreamId += PartitionStreamIdStep;

    // Renew partition stream.
    TIntrusivePtr<TPartitionStreamImpl<true>>& currentPartitionStream =
        PartitionStreams[partitionStream->GetAssignId()];
    if (currentPartitionStream) {
        CookieMapping.RemoveMapping(currentPartitionStream->GetPartitionStreamId());
        EventsQueue->PushEvent(
            currentPartitionStream, weak_from_this(),
             TReadSessionEvent::TPartitionStreamClosedEvent(
                 currentPartitionStream, TReadSessionEvent::TPartitionStreamClosedEvent::EReason::Lost),
            deferred);
    }
    currentPartitionStream = partitionStream;

    // Send event to user.
    EventsQueue->PushEvent(
        partitionStream, weak_from_this(),
         TReadSessionEvent::TCreatePartitionStreamEvent(partitionStream, msg.read_offset(), msg.end_offset()),
        deferred);
}

template <>
template <>
inline void TSingleClusterReadSessionImpl<true>::OnReadDoneImpl(
    Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::Release&& msg,
    TDeferredActions<true>& deferred) { // Assumes that we're under lock.
    auto partitionStreamIt = PartitionStreams.find(msg.assign_id());
    if (partitionStreamIt == PartitionStreams.end()) {
        return;
    }
    TIntrusivePtr<TPartitionStreamImpl<true>> partitionStream = partitionStreamIt->second;
    if (msg.forceful_release()) {
        PartitionStreams.erase(msg.assign_id());
        CookieMapping.RemoveMapping(partitionStream->GetPartitionStreamId());
        EventsQueue->PushEvent(partitionStream, weak_from_this(),
                                TReadSessionEvent::TPartitionStreamClosedEvent(
                                    partitionStream, TReadSessionEvent::TPartitionStreamClosedEvent::EReason::Lost),
                               deferred);
    } else {
        EventsQueue->PushEvent(
            partitionStream, weak_from_this(),
             TReadSessionEvent::TDestroyPartitionStreamEvent(std::move(partitionStream), msg.commit_offset()),
            deferred);
    }
}

template <>
template <>
inline void TSingleClusterReadSessionImpl<true>::OnReadDoneImpl(
    Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::Committed&& msg,
    TDeferredActions<true>& deferred) { // Assumes that we're under lock.

    Log.Write(TLOG_DEBUG, GetLogPrefix() << "Committed response: " << msg);

    TMap<ui64, TIntrusivePtr<TPartitionStreamImpl<true>>> partitionStreams;
    for (const Ydb::PersQueue::V1::CommitCookie& cookieProto : msg.cookies()) {
        typename TPartitionCookieMapping::TCookie::TPtr cookie = CookieMapping.RetrieveCommittedCookie(cookieProto);
        if (cookie) {
            cookie->PartitionStream->UpdateMaxCommittedOffset(cookie->OffsetRange.second);
            partitionStreams[cookie->PartitionStream->GetPartitionStreamId()] = cookie->PartitionStream;
        }
    }
    for (auto& [id, partitionStream] : partitionStreams) {
        EventsQueue->PushEvent(
            partitionStream, weak_from_this(),
             TReadSessionEvent::TCommitAcknowledgementEvent(partitionStream, partitionStream->GetMaxCommittedOffset()),
            deferred);
    }

    for (const auto& rangeProto : msg.offset_ranges()) {
        auto partitionStreamIt = PartitionStreams.find(rangeProto.assign_id());
        if (partitionStreamIt != PartitionStreams.end()) {
            auto partitionStream = partitionStreamIt->second;
            partitionStream->UpdateMaxCommittedOffset(rangeProto.end_offset());
            EventsQueue->PushEvent(
                partitionStream, weak_from_this(),
                 TReadSessionEvent::TCommitAcknowledgementEvent(partitionStream, rangeProto.end_offset()),
                deferred);
        }
    }
}

template <>
template <>
inline void TSingleClusterReadSessionImpl<true>::OnReadDoneImpl(
    Ydb::PersQueue::V1::MigrationStreamingReadServerMessage::PartitionStatus&& msg,
    TDeferredActions<true>& deferred) { // Assumes that we're under lock.
    auto partitionStreamIt = PartitionStreams.find(msg.assign_id());
    if (partitionStreamIt == PartitionStreams.end()) {
        return;
    }
    EventsQueue->PushEvent(partitionStreamIt->second, weak_from_this(),
                            TReadSessionEvent::TPartitionStreamStatusEvent(
                                partitionStreamIt->second, msg.committed_offset(),
                                0, // TODO: support read offset in status
                                msg.end_offset(), TInstant::MilliSeconds(msg.write_watermark_ms())),
                           deferred);
}

//////////////

template <>
template <>
inline void TSingleClusterReadSessionImpl<false>::OnReadDoneImpl(
    Ydb::Topic::StreamReadMessage::InitResponse&& msg,
    TDeferredActions<false>& deferred) { // Assumes that we're under lock.
    Y_UNUSED(deferred);

    RetryState = nullptr;

    Log.Write(TLOG_INFO, GetLogPrefix() << "Server session id: " << msg.session_id());

    // Successful init. Do nothing.
    ContinueReadingDataImpl();
}

template <>
template <>
inline void TSingleClusterReadSessionImpl<false>::OnReadDoneImpl(
    Ydb::Topic::StreamReadMessage::ReadResponse&& msg,
    TDeferredActions<false>& deferred) { // Assumes that we're under lock.
    if (Closing || Aborting) {
        return; // Don't process new data.
    }

    i64 serverBytesSize = msg.bytes_size();
    ReadSizeServerDelta -= serverBytesSize;

    UpdateMemoryUsageStatisticsImpl();
    for (TPartitionData<false>& partitionData : *msg.mutable_partition_data()) {
        auto partitionStreamIt = PartitionStreams.find(partitionData.partition_session_id());
        if (partitionStreamIt == PartitionStreams.end()) {
            ++*Settings.Counters_->Errors;
            BreakConnectionAndReconnectImpl(EStatus::INTERNAL_ERROR,
                                            TStringBuilder() << "Got unexpected partition stream data message. "
                                            << "PartitionSessionId: " << partitionData.partition_session_id(),
                                            deferred);
            return;
        }
        const TIntrusivePtr<TPartitionStreamImpl<false>>& partitionStream = partitionStreamIt->second;

        i64 firstOffset = std::numeric_limits<i64>::max();
        i64 currentOffset = std::numeric_limits<i64>::max();
        i64 desiredOffset = partitionStream->GetFirstNotReadOffset();
        for (const auto& batch : partitionData.batches()) {
            // Validate messages.
            for (const auto& messageData : batch.message_data()) {
                // Check offsets continuity.
                if (messageData.offset() != desiredOffset) {
                    bool res = partitionStream->AddToCommitRanges(desiredOffset, messageData.offset(), GetRangesMode());
                    Y_VERIFY(res);
                }

                if (firstOffset == std::numeric_limits<i64>::max()) {
                    firstOffset = messageData.offset();
                }
                currentOffset = messageData.offset();
                desiredOffset = currentOffset + 1;
                partitionStream->UpdateMaxReadOffset(currentOffset);
                const i64 messageSize = static_cast<i64>(messageData.data().size());
                CompressedDataSize += messageSize;
                *Settings.Counters_->BytesInflightTotal += messageSize;
                *Settings.Counters_->BytesInflightCompressed += messageSize;
                ++*Settings.Counters_->MessagesInflight;
            }
        }
        if (firstOffset == std::numeric_limits<i64>::max()) {
            BreakConnectionAndReconnectImpl(EStatus::INTERNAL_ERROR,
                                            TStringBuilder() << "Got empty data message. "
                                                << "PartitionSessionId: " << partitionData.partition_session_id()
                                                << " message: " << msg,
                                            deferred);
            return;
        }
        partitionStream->SetFirstNotReadOffset(desiredOffset);

        auto decompressionInfo = std::make_shared<TDataDecompressionInfo<false>>(std::move(partitionData),
                                                                                 shared_from_this(),
                                                                                 Settings.Decompress_,
                                                                                 serverBytesSize);
        // TODO (ildar-khisam@): share serverBytesSize between partitions data according to their actual sizes;
        //                       for now whole serverBytesSize goes with first (and only) partition data.
        serverBytesSize = 0;
        Y_VERIFY(decompressionInfo);

        decompressionInfo->PlanDecompressionTasks(AverageCompressionRatio,
                                                  partitionStream);
        DecompressionQueue.emplace_back(decompressionInfo, partitionStream);
        StartDecompressionTasksImpl(deferred);
    }

    WaitingReadResponse = false;
    ContinueReadingDataImpl();
}

template <>
template <>
inline void TSingleClusterReadSessionImpl<false>::OnReadDoneImpl(
    Ydb::Topic::StreamReadMessage::StartPartitionSessionRequest&& msg,
    TDeferredActions<false>& deferred) { // Assumes that we're under lock.
    auto partitionStream = MakeIntrusive<TPartitionStreamImpl<false>>(
        NextPartitionStreamId, msg.partition_session().path(), msg.partition_session().partition_id(),
        msg.partition_session().partition_session_id(), msg.committed_offset(),
        weak_from_this(), ErrorHandler);
    NextPartitionStreamId += PartitionStreamIdStep;

    // Renew partition stream.
    TIntrusivePtr<TPartitionStreamImpl<false>>& currentPartitionStream = PartitionStreams[partitionStream->GetAssignId()];
    if (currentPartitionStream) {
        EventsQueue->PushEvent(
            currentPartitionStream, weak_from_this(),
             NTopic::TReadSessionEvent::TPartitionSessionClosedEvent(
                 currentPartitionStream, NTopic::TReadSessionEvent::TPartitionSessionClosedEvent::EReason::Lost),
            deferred);
    }
    currentPartitionStream = partitionStream;

    // Send event to user.
    EventsQueue->PushEvent(partitionStream, weak_from_this(),
                            NTopic::TReadSessionEvent::TStartPartitionSessionEvent(
                                partitionStream, msg.committed_offset(), msg.partition_offsets().end()),
                           deferred);
}

template <>
template <>
inline void TSingleClusterReadSessionImpl<false>::OnReadDoneImpl(
    Ydb::Topic::StreamReadMessage::StopPartitionSessionRequest&& msg,
    TDeferredActions<false>& deferred) { // Assumes that we're under lock.
    auto partitionStreamIt = PartitionStreams.find(msg.partition_session_id());
    if (partitionStreamIt == PartitionStreams.end()) {
        return;
    }
    TIntrusivePtr<TPartitionStreamImpl<false>> partitionStream = partitionStreamIt->second;
    if (!msg.graceful()) {
        PartitionStreams.erase(msg.partition_session_id());
        EventsQueue->PushEvent(partitionStream, weak_from_this(),
                                NTopic::TReadSessionEvent::TPartitionSessionClosedEvent(
                                    partitionStream, NTopic::TReadSessionEvent::TPartitionSessionClosedEvent::EReason::Lost),
                               deferred);
    } else {
        EventsQueue->PushEvent(
            partitionStream, weak_from_this(),
             NTopic::TReadSessionEvent::TStopPartitionSessionEvent(std::move(partitionStream), msg.committed_offset()),
            deferred);
    }
}

template <>
template <>
inline void TSingleClusterReadSessionImpl<false>::OnReadDoneImpl(
    Ydb::Topic::StreamReadMessage::CommitOffsetResponse&& msg,
    TDeferredActions<false>& deferred) { // Assumes that we're under lock.

    Log.Write(TLOG_DEBUG, GetLogPrefix() << "Committed response: " << msg);

    for (const auto& rangeProto : msg.partitions_committed_offsets()) {
        auto partitionStreamIt = PartitionStreams.find(rangeProto.partition_session_id());
        if (partitionStreamIt != PartitionStreams.end()) {
            auto partitionStream = partitionStreamIt->second;
            partitionStream->UpdateMaxCommittedOffset(rangeProto.committed_offset());
            EventsQueue->PushEvent(partitionStream, weak_from_this(),
                                    NTopic::TReadSessionEvent::TCommitOffsetAcknowledgementEvent(
                                        partitionStream, rangeProto.committed_offset()),
                                   deferred);
        }
    }
}

template <>
template <>
inline void TSingleClusterReadSessionImpl<false>::OnReadDoneImpl(
    Ydb::Topic::StreamReadMessage::PartitionSessionStatusResponse&& msg,
    TDeferredActions<false>& deferred) { // Assumes that we're under lock.
    auto partitionStreamIt = PartitionStreams.find(msg.partition_session_id());
    if (partitionStreamIt == PartitionStreams.end()) {
        return;
    }
    EventsQueue->PushEvent(partitionStreamIt->second, weak_from_this(),
                            NTopic::TReadSessionEvent::TPartitionSessionStatusEvent(
                                partitionStreamIt->second, msg.committed_offset(),
                                0, // TODO: support read offset in status
                                msg.partition_offsets().end(),
                                TInstant::MilliSeconds(::google::protobuf::util::TimeUtil::TimestampToMilliseconds(
                                    msg.write_time_high_watermark()))),
                           deferred);
}

template <>
template <>
inline void TSingleClusterReadSessionImpl<false>::OnReadDoneImpl(
    Ydb::Topic::UpdateTokenResponse&& msg,
    TDeferredActions<false>& deferred) { // Assumes that we're under lock.
    // TODO
    Y_UNUSED(msg, deferred);
}

//////////////

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::StartDecompressionTasksImpl(TDeferredActions<UseMigrationProtocol>& deferred) {
    UpdateMemoryUsageStatisticsImpl();
    const i64 limit = GetDecompressedDataSizeLimit();
    Y_VERIFY(limit > 0);
    while (DecompressedDataSize < limit
           && (static_cast<size_t>(CompressedDataSize + DecompressedDataSize) < Settings.MaxMemoryUsageBytes_
               || DecompressedDataSize == 0 /* Allow decompression of at least one message even if memory is full. */)
           && !DecompressionQueue.empty())
    {
        TDecompressionQueueItem& current = DecompressionQueue.front();
        auto sentToDecompress = current.BatchInfo->StartDecompressionTasks(Settings.DecompressionExecutor_,
                                                                           Max(limit - DecompressedDataSize, static_cast<i64>(1)),
                                                                           deferred);
        DecompressedDataSize += sentToDecompress;
        if (current.BatchInfo->AllDecompressionTasksStarted()) {
            DecompressionQueue.pop_front();
        } else {
            break;
        }
    }
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::DestroyAllPartitionStreamsImpl(TDeferredActions<UseMigrationProtocol>& deferred) {
    using TClosedEvent = std::conditional_t<
        UseMigrationProtocol,
            NPersQueue::TReadSessionEvent::TPartitionStreamClosedEvent,
            NTopic::TReadSessionEvent::TPartitionSessionClosedEvent
    >;

    for (auto&& [key, partitionStream] : PartitionStreams) {
        EventsQueue->PushEvent(partitionStream, TSingleClusterReadSessionImpl<UseMigrationProtocol>::weak_from_this(),
                                TClosedEvent(std::move(partitionStream), TClosedEvent::EReason::ConnectionLost),
                               deferred);
    }
    PartitionStreams.clear();
    CookieMapping.ClearMapping();
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::OnCreateNewDecompressionTask() {
    ++DecompressionTasksInflight;
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::OnDecompressionInfoDestroy(i64 compressedSize, i64 decompressedSize, i64 messagesCount)
{
    *Settings.Counters_->MessagesInflight -= messagesCount;
    *Settings.Counters_->BytesInflightUncompressed -= decompressedSize;
    *Settings.Counters_->BytesInflightCompressed -= compressedSize;
    *Settings.Counters_->BytesInflightTotal -= (compressedSize + decompressedSize);

    CompressedDataSize -= compressedSize;
    DecompressedDataSize -= decompressedSize;
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::OnDataDecompressed(i64 sourceSize, i64 estimatedDecompressedSize, i64 decompressedSize, size_t messagesCount, i64 serverBytesSize) {
    TDeferredActions<UseMigrationProtocol> deferred;

    Y_VERIFY(DecompressionTasksInflight > 0);
    --DecompressionTasksInflight;

    *Settings.Counters_->BytesRead += decompressedSize;
    *Settings.Counters_->BytesReadCompressed += sourceSize;
    *Settings.Counters_->MessagesRead += messagesCount;
    *Settings.Counters_->BytesInflightUncompressed += decompressedSize;
    *Settings.Counters_->BytesInflightCompressed -= sourceSize;
    *Settings.Counters_->BytesInflightTotal += (decompressedSize - sourceSize);

    with_lock (Lock) {
        UpdateMemoryUsageStatisticsImpl();
        CompressedDataSize -= sourceSize;
        DecompressedDataSize += decompressedSize - estimatedDecompressedSize;
        constexpr double weight = 0.6;
        if (sourceSize > 0) {
            AverageCompressionRatio = weight * static_cast<double>(decompressedSize) / static_cast<double>(sourceSize) + (1 - weight) * AverageCompressionRatio;
        }
        if (Aborting) {
            return;
        }
        if constexpr (!UseMigrationProtocol) {
            ReadSizeBudget += serverBytesSize;
        }
        ContinueReadingDataImpl();
        StartDecompressionTasksImpl(deferred);
    }
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::Abort() {
    Log.Write(TLOG_DEBUG, GetLogPrefix() << "Abort session to cluster");

    with_lock (Lock) {
        if (!Aborting) {
            Aborting = true;
            CloseCallback = {};

            // Cancel(ClientContext); // Don't cancel, because this is used only as factory for other contexts.
            Cancel(ConnectContext);
            Cancel(ConnectTimeoutContext);
            Cancel(ConnectDelayContext);

            if (ClientContext) {
                ClientContext->Cancel();
                ClientContext.reset();
            }

            if (Processor) {
                Processor->Cancel();
            }
        }
    }
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::Close(std::function<void()> callback) {
    with_lock (Lock) {
        if (Aborting) {
            callback();
        }

        if (!Closing) {
            Closing = true;

            CloseCallback = std::move(callback);

            Cancel(ConnectContext);
            Cancel(ConnectTimeoutContext);
            Cancel(ConnectDelayContext);

            if (!Processor) {
                CallCloseCallbackImpl();
            } else {
                if (!HasCommitsInflightImpl()) {
                    Processor->Cancel();
                    CallCloseCallbackImpl();
                }
            }
        }
    }
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::CallCloseCallbackImpl() {
    if (CloseCallback) {
        CloseCallback();
        CloseCallback = {};
    }
    Aborting = true; // So abort call will have no effect.
    if (ClientContext) {
        ClientContext->Cancel();
        ClientContext.reset();
    }
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::StopReadingData() {
    with_lock (Lock) {
        DataReadingSuspended = true;
    }
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::ResumeReadingData() {
    with_lock (Lock) {
        if (DataReadingSuspended) {
            DataReadingSuspended = false;
            ContinueReadingDataImpl();
        }
    }
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::DumpStatisticsToLog(TLogElement& log) {
    with_lock (Lock) {
        // cluster:topic:partition:stream-id:read-offset:committed-offset
        for (auto&& [key, partitionStream] : PartitionStreams) {
            if constexpr (UseMigrationProtocol) {
                log << " "
                    << ClusterName
                    << ':' << partitionStream->GetTopicPath()
                    << ':' << partitionStream->GetPartitionId()
                    << ':' << partitionStream->GetPartitionStreamId()
                    << ':' << partitionStream->GetMaxReadOffset()
                    << ':' << partitionStream->GetMaxCommittedOffset();
            } else {
                log << " "
                    << "-"
                    << ':' << partitionStream->GetTopicPath()
                    << ':' << partitionStream->GetPartitionId()
                    << ':' << partitionStream->GetPartitionSessionId()
                    << ':' << partitionStream->GetMaxReadOffset()
                    << ':' << partitionStream->GetMaxCommittedOffset();
            }
        }
    }
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::UpdateMemoryUsageStatisticsImpl() {
    const TInstant now = TInstant::Now();
    const ui64 delta = (now - UsageStatisticsLastUpdateTime).MilliSeconds();
    UsageStatisticsLastUpdateTime = now;
    const double percent = 100.0 / static_cast<double>(Settings.MaxMemoryUsageBytes_);

    Settings.Counters_->TotalBytesInflightUsageByTime->Collect((DecompressedDataSize + CompressedDataSize) * percent, delta);
    Settings.Counters_->UncompressedBytesInflightUsageByTime->Collect(DecompressedDataSize * percent, delta);
    Settings.Counters_->CompressedBytesInflightUsageByTime->Collect(CompressedDataSize * percent, delta);
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::UpdateMemoryUsageStatistics() {
    with_lock (Lock) {
        UpdateMemoryUsageStatisticsImpl();
    }
}

template<bool UseMigrationProtocol>
bool TSingleClusterReadSessionImpl<UseMigrationProtocol>::GetRangesMode() const {
    if constexpr (UseMigrationProtocol) {
        return Settings.RangesMode_.GetOrElse(RangesMode);
    } else {
        return true;
    }
}

template<bool UseMigrationProtocol>
bool TSingleClusterReadSessionImpl<UseMigrationProtocol>::TPartitionCookieMapping::AddMapping(const typename TCookie::TPtr& cookie) {
    if (!Cookies.emplace(cookie->GetKey(), cookie).second) {
        return false;
    }
    for (ui64 offset = cookie->OffsetRange.first; offset < cookie->OffsetRange.second; ++offset) {
        if (!UncommittedOffsetToCookie.emplace(std::make_pair(cookie->PartitionStream->GetPartitionStreamId(), offset), cookie).second) {
            return false;
        }
    }
    PartitionStreamIdToCookie.emplace(cookie->PartitionStream->GetPartitionStreamId(), cookie);
    return true;
}

template<bool UseMigrationProtocol>
typename TSingleClusterReadSessionImpl<UseMigrationProtocol>::TPartitionCookieMapping::TCookie::TPtr TSingleClusterReadSessionImpl<UseMigrationProtocol>::TPartitionCookieMapping::CommitOffset(ui64 partitionStreamId, ui64 offset) {
    auto cookieIt = UncommittedOffsetToCookie.find(std::make_pair(partitionStreamId, offset));
    if (cookieIt != UncommittedOffsetToCookie.end()) {
        typename TCookie::TPtr cookie;
        if (!--cookieIt->second->UncommittedMessagesLeft) {
            ++CommitInflight;
            cookie = cookieIt->second;
        }
        UncommittedOffsetToCookie.erase(cookieIt);
        return cookie;
    } else {
        ThrowFatalError(TStringBuilder() << "Invalid offset " << offset << ". Partition stream id: " << partitionStreamId << Endl);
    }
    // If offset wasn't found, there might be already hard released partition.
    // This situation is OK.
    return nullptr;
}

template<bool UseMigrationProtocol>
typename TSingleClusterReadSessionImpl<UseMigrationProtocol>::TPartitionCookieMapping::TCookie::TPtr TSingleClusterReadSessionImpl<UseMigrationProtocol>::TPartitionCookieMapping::RetrieveCommittedCookie(const Ydb::PersQueue::V1::CommitCookie& cookieProto) {
    typename TCookie::TPtr cookieInfo;
    auto cookieIt = Cookies.find(typename TCookie::TKey(cookieProto.assign_id(), cookieProto.partition_cookie()));
    if (cookieIt != Cookies.end()) {
        --CommitInflight;
        cookieInfo = cookieIt->second;
        Cookies.erase(cookieIt);

        auto [rangeBegin, rangeEnd] = PartitionStreamIdToCookie.equal_range(cookieInfo->PartitionStream->GetPartitionStreamId());
        for (auto i = rangeBegin; i != rangeEnd; ++i) {
            if (i->second == cookieInfo) {
                PartitionStreamIdToCookie.erase(i);
                break;
            }
        }
    }
    return cookieInfo;
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::TPartitionCookieMapping::RemoveMapping(ui64 partitionStreamId) {
    auto [rangeBegin, rangeEnd] = PartitionStreamIdToCookie.equal_range(partitionStreamId);
    for (auto i = rangeBegin; i != rangeEnd; ++i) {
        typename TCookie::TPtr cookie = i->second;
        Cookies.erase(cookie->GetKey());
        for (ui64 offset = cookie->OffsetRange.first; offset < cookie->OffsetRange.second; ++offset) {
            UncommittedOffsetToCookie.erase(std::make_pair(partitionStreamId, offset));
        }
    }
    PartitionStreamIdToCookie.erase(rangeBegin, rangeEnd);
}

template<bool UseMigrationProtocol>
void TSingleClusterReadSessionImpl<UseMigrationProtocol>::TPartitionCookieMapping::ClearMapping() {
    Cookies.clear();
    UncommittedOffsetToCookie.clear();
    PartitionStreamIdToCookie.clear();
    CommitInflight = 0;
}

template<bool UseMigrationProtocol>
bool TSingleClusterReadSessionImpl<UseMigrationProtocol>::TPartitionCookieMapping::HasUnacknowledgedCookies() const {
    return CommitInflight != 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TReadSessionEventInfo

template<bool UseMigrationProtocol>
TReadSessionEventInfo<UseMigrationProtocol>::TReadSessionEventInfo(TIntrusivePtr<TPartitionStreamImpl<UseMigrationProtocol>> partitionStream,
                                                                   std::weak_ptr<IUserRetrievedEventCallback<UseMigrationProtocol>> session,
                                                                   TEvent event)
    : PartitionStream(std::move(partitionStream))
    , Event(std::move(event))
    , Session(std::move(session))
{
}

template<bool UseMigrationProtocol>
TReadSessionEventInfo<UseMigrationProtocol>::TReadSessionEventInfo(TIntrusivePtr<TPartitionStreamImpl<UseMigrationProtocol>> partitionStream,
                                                                   std::weak_ptr<IUserRetrievedEventCallback<UseMigrationProtocol>> session,
                                                                   bool hasDataEvents)
    : PartitionStream(std::move(partitionStream))
    , HasDataEvents(hasDataEvents)
    , EventsCount(1)
    , Session(std::move(session))
{
}

template<bool UseMigrationProtocol>
bool TReadSessionEventInfo<UseMigrationProtocol>::IsEmpty() const {
    return !PartitionStream || !PartitionStream->HasEvents();
}

template<bool UseMigrationProtocol>
bool TReadSessionEventInfo<UseMigrationProtocol>::IsDataEvent() const {
    return !IsEmpty() && PartitionStream->TopEvent().IsDataEvent();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TReadSessionEventsQueue

template <bool UseMigrationProtocol>
TReadSessionEventsQueue<UseMigrationProtocol>::TReadSessionEventsQueue(
    const TAReadSessionSettings<UseMigrationProtocol>& settings,
    std::weak_ptr<IUserRetrievedEventCallback<UseMigrationProtocol>> session)
    : TParent(settings)
    , Session(std::move(session)) {
    const auto& h = TParent::Settings.EventHandlers_;

    if constexpr (UseMigrationProtocol) {
        HasEventCallbacks = (h.CommonHandler_
                             || h.DataReceivedHandler_
                             || h.CommitAcknowledgementHandler_
                             || h.CreatePartitionStreamHandler_
                             || h.DestroyPartitionStreamHandler_
                             || h.PartitionStreamStatusHandler_
                             || h.PartitionStreamClosedHandler_
                             || h.SessionClosedHandler_);
    } else {
        HasEventCallbacks = (h.CommonHandler_
                             || h.DataReceivedHandler_
                             || h.CommitOffsetAcknowledgementHandler_
                             || h.StartPartitionSessionHandler_
                             || h.StopPartitionSessionHandler_
                             || h.PartitionSessionStatusHandler_
                             || h.PartitionSessionClosedHandler_
                             || h.SessionClosedHandler_);
    }
}

template <bool UseMigrationProtocol>
void TReadSessionEventsQueue<UseMigrationProtocol>::PushEvent(TIntrusivePtr<TPartitionStreamImpl<UseMigrationProtocol>> stream,
                                                              std::weak_ptr<IUserRetrievedEventCallback<UseMigrationProtocol>> /*session*/,
                                                              typename TAReadSessionEvent<UseMigrationProtocol>::TEvent event,
                                                              TDeferredActions<UseMigrationProtocol>& deferred)
{
    if (TParent::Closed) {
        return;
    }

    with_lock (TParent::Mutex) {
        using TClosedEvent = std::conditional_t<
            UseMigrationProtocol,
                NPersQueue::TReadSessionEvent::TPartitionStreamClosedEvent,
                NTopic::TReadSessionEvent::TPartitionSessionClosedEvent
        >;

        if (std::holds_alternative<TClosedEvent>(event)) {
            stream->DeleteNotReadyTail(deferred);
        }

        stream->InsertEvent(std::move(event));
        Y_ASSERT(stream->HasEvents());

        SignalReadyEventsImpl(stream.Get(), deferred);
    }
}

template <bool UseMigrationProtocol>
void TReadSessionEventsQueue<UseMigrationProtocol>::SignalEventImpl(
    TIntrusivePtr<TPartitionStreamImpl<UseMigrationProtocol>> partitionStream,
    TDeferredActions<UseMigrationProtocol>& deferred,
    bool isDataEvent)
{
    if (TParent::Closed) {
        return;
    }

    auto session = partitionStream->GetSession();

    if (TParent::Events.empty()) {
        TParent::Events.emplace(std::move(partitionStream), std::move(session), isDataEvent);
    } else {
        auto& event = TParent::Events.back();
        if (event.HasDataEvents
            && isDataEvent
            && (event.PartitionStream == partitionStream)) {
            ++event.EventsCount;
        } else {
            TParent::Events.emplace(std::move(partitionStream), std::move(session), isDataEvent);
        }
    }

    SignalWaiterImpl(deferred);
}

template <bool UseMigrationProtocol>
void TReadSessionEventsQueue<UseMigrationProtocol>::PushDataEvent(TIntrusivePtr<TPartitionStreamImpl<UseMigrationProtocol>> partitionStream,
                                                                  size_t batch,
                                                                  size_t message,
                                                                  TDataDecompressionInfoPtr<UseMigrationProtocol> parent,
                                                                  std::atomic<bool>& ready)
{
    if (this->Closed) {
        return;
    }

    with_lock (TParent::Mutex) {
        partitionStream->InsertDataEvent(batch, message, parent, ready);
    }
}

template <bool UseMigrationProtocol>
typename TAReadSessionEvent<UseMigrationProtocol>::TDataReceivedEvent TReadSessionEventsQueue<UseMigrationProtocol>::GetDataEventImpl(
    TIntrusivePtr<TPartitionStreamImpl<UseMigrationProtocol>> stream,
    size_t* maxByteSize,
    TUserRetrievedEventInfoAccumulator<UseMigrationProtocol>* accumulator) { // Assumes that we're under lock.
    TVector<typename TAReadSessionEvent<UseMigrationProtocol>::TDataReceivedEvent::TMessage> messages;
    TVector<typename TAReadSessionEvent<UseMigrationProtocol>::TDataReceivedEvent::TCompressedMessage> compressedMessages;

    Y_VERIFY(!TParent::Events.empty());

    auto& event = TParent::Events.front();

    Y_VERIFY(event.PartitionStream == stream);
    Y_VERIFY(event.EventsCount > 0);

    for (; (event.EventsCount > 0) && (*maxByteSize > 0); --event.EventsCount) {
        stream->TopEvent().GetDataEvent().TakeData(stream, &messages, &compressedMessages, maxByteSize, accumulator);
        stream->PopEvent();
    }

    if (event.EventsCount == 0) {
        TParent::Events.pop();
    }

    Y_VERIFY(!messages.empty() || !compressedMessages.empty());

    return {std::move(messages), std::move(compressedMessages), stream};
}

template <bool UseMigrationProtocol>
void TReadSessionEventsQueue<UseMigrationProtocol>::SignalReadyEvents(
    TPartitionStreamImpl<UseMigrationProtocol>* partitionStream) {
    Y_ASSERT(partitionStream);
    TDeferredActions<UseMigrationProtocol> deferred;
    with_lock (TParent::Mutex) {
        SignalReadyEventsImpl(partitionStream, deferred);
    }
}

template <bool UseMigrationProtocol>
void TReadSessionEventsQueue<UseMigrationProtocol>::SignalReadyEventsImpl(
    TPartitionStreamImpl<UseMigrationProtocol>* partitionStream, TDeferredActions<UseMigrationProtocol>& deferred) {
    partitionStream->SignalReadyEvents(this, deferred);
    ApplyCallbacksToReadyEventsImpl(deferred);
}

template <bool UseMigrationProtocol>
bool TReadSessionEventsQueue<UseMigrationProtocol>::ApplyCallbacksToReadyEventsImpl(
    TDeferredActions<UseMigrationProtocol>& deferred) {
    if (!HasEventCallbacks) {
        return false;
    }
    bool applied = false;
    while (HasCallbackForNextEventImpl()) {
        size_t maxSize = std::numeric_limits<size_t>::max();
        TReadSessionEventInfo<UseMigrationProtocol> eventInfo = GetEventImpl(&maxSize);
        applied = true;
        if (!ApplyHandler(eventInfo, deferred)) { // Close session event.
            break;
        }
    }
    return applied;
}

template <bool UseMigrationProtocol>
struct THasCallbackForEventVisitor {
    explicit THasCallbackForEventVisitor(const TAReadSessionSettings<UseMigrationProtocol>& settings)
        : Settings(settings) {
    }

    template <typename TEv>
    inline bool operator()(const TEv&);

    const TAReadSessionSettings<UseMigrationProtocol>& Settings;
};

#define DEFINE_HANDLER(use_migration_protocol, type, handler)                           \
    template <>                                                                         \
    template <>                                                                         \
    inline bool THasCallbackForEventVisitor<use_migration_protocol>::operator()(const type&) { \
        return bool(Settings.EventHandlers_.handler);                                   \
    }                                                                                   \
    /**/

DEFINE_HANDLER(true, TReadSessionEvent::TDataReceivedEvent, DataReceivedHandler_);
DEFINE_HANDLER(true, TReadSessionEvent::TCommitAcknowledgementEvent, CommitAcknowledgementHandler_);
DEFINE_HANDLER(true, TReadSessionEvent::TCreatePartitionStreamEvent, CreatePartitionStreamHandler_);
DEFINE_HANDLER(true, TReadSessionEvent::TDestroyPartitionStreamEvent, DestroyPartitionStreamHandler_);
DEFINE_HANDLER(true, TReadSessionEvent::TPartitionStreamStatusEvent, PartitionStreamStatusHandler_);
DEFINE_HANDLER(true, TReadSessionEvent::TPartitionStreamClosedEvent, PartitionStreamClosedHandler_);
DEFINE_HANDLER(true, TSessionClosedEvent, SessionClosedHandler_);

DEFINE_HANDLER(false, NTopic::TReadSessionEvent::TDataReceivedEvent, DataReceivedHandler_);
DEFINE_HANDLER(false, NTopic::TReadSessionEvent::TCommitOffsetAcknowledgementEvent, CommitOffsetAcknowledgementHandler_);
DEFINE_HANDLER(false, NTopic::TReadSessionEvent::TStartPartitionSessionEvent, StartPartitionSessionHandler_);
DEFINE_HANDLER(false, NTopic::TReadSessionEvent::TStopPartitionSessionEvent, StopPartitionSessionHandler_);
DEFINE_HANDLER(false, NTopic::TReadSessionEvent::TPartitionSessionStatusEvent, PartitionSessionStatusHandler_);
DEFINE_HANDLER(false, NTopic::TReadSessionEvent::TPartitionSessionClosedEvent, PartitionSessionClosedHandler_);
DEFINE_HANDLER(false, NTopic::TSessionClosedEvent, SessionClosedHandler_);

#undef DEFINE_HANDLER


template<bool UseMigrationProtocol>
bool TReadSessionEventsQueue<UseMigrationProtocol>::HasCallbackForNextEventImpl() const {
    if (!TParent::HasEventsImpl()) {
        return false;
    }
    if (TParent::Settings.EventHandlers_.CommonHandler_) {
        return true;
    }

    if (!TParent::Events.empty()) {
        const TReadSessionEventInfo<UseMigrationProtocol>& topEvent = TParent::Events.front();
        const typename TAReadSessionEvent<UseMigrationProtocol>::TEvent* event = nullptr;
        if (topEvent.Event) {
            event = &*topEvent.Event;
        } else if (topEvent.PartitionStream && topEvent.PartitionStream->HasEvents()) {
            const TRawPartitionStreamEvent<UseMigrationProtocol>& partitionStreamTopEvent = topEvent.PartitionStream->TopEvent();
            if (partitionStreamTopEvent.IsDataEvent()) {
                return bool(TParent::Settings.EventHandlers_.DataReceivedHandler_);
            } else {
                event = &partitionStreamTopEvent.GetEvent();
            }
        }

        if (!event) {
            return false;
        }

        THasCallbackForEventVisitor<UseMigrationProtocol> visitor(TParent::Settings);
        return std::visit(visitor, *event);
    } else if (TParent::CloseEvent) {
        return bool(TParent::Settings.EventHandlers_.SessionClosedHandler_);
    }
    Y_ASSERT(false);
    return false;
}

template<bool UseMigrationProtocol>
void TReadSessionEventsQueue<UseMigrationProtocol>::ClearAllEvents() {
    TDeferredActions<UseMigrationProtocol> deferred;
    with_lock (TParent::Mutex) {
        while (!TParent::Events.empty()) {
            auto& event = TParent::Events.front();
            if (event.PartitionStream && event.PartitionStream->HasEvents()) {
                event.PartitionStream->PopEvent();
            }
            TParent::Events.pop();
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDataDecompressionInfo

template<bool UseMigrationProtocol>
TDataDecompressionInfo<UseMigrationProtocol>::TDataDecompressionInfo(
    TPartitionData<UseMigrationProtocol>&& msg,
    std::weak_ptr<TSingleClusterReadSessionImpl<UseMigrationProtocol>> session,
    bool doDecompress,
    i64 serverBytesSize
)
    : ServerMessage(std::move(msg))
    , Session(std::move(session))
    , DoDecompress(doDecompress)
    , ServerBytesSize(serverBytesSize)
{
    i64 compressedSize = 0;
    i64 messagesCount = 0;

    for (const auto& batch : ServerMessage.batches()) {
        for (const auto& messageData : batch.message_data()) {
            compressedSize += messageData.data().size();
            ++messagesCount;
        }
    }

    MessagesInflight = messagesCount;
    SourceDataNotProcessed = compressedSize;
    CompressedDataSize = compressedSize;

    BuildBatchesMeta();
}

template<bool UseMigrationProtocol>
TDataDecompressionInfo<UseMigrationProtocol>::~TDataDecompressionInfo()
{
    if (auto session = Session.lock()) {
        session->OnDecompressionInfoDestroy(CompressedDataSize, DecompressedDataSize, MessagesInflight);
    }
}

template<bool UseMigrationProtocol>
void TDataDecompressionInfo<UseMigrationProtocol>::BuildBatchesMeta() {
    BatchesMeta.reserve(ServerMessage.batches_size());
    for (const auto& batch : ServerMessage.batches()) {
        // Extra fields.
        typename TAWriteSessionMeta<UseMigrationProtocol>::TPtr meta = MakeIntrusive<TAWriteSessionMeta<UseMigrationProtocol>>();

        if constexpr (UseMigrationProtocol) {
            meta->Fields.reserve(batch.extra_fields_size());
            for (const Ydb::PersQueue::V1::KeyValue& kv : batch.extra_fields()) {
                meta->Fields.emplace(kv.key(), kv.value());
            }
        } else {
            meta->Fields.reserve(batch.write_session_meta_size());
            for (const auto& [key, value] : batch.write_session_meta()) {
                meta->Fields.emplace(key, value);
            }
        }

        BatchesMeta.emplace_back(std::move(meta));
    }
}

template<bool UseMigrationProtocol>
void TDataDecompressionInfo<UseMigrationProtocol>::PutDecompressionError(std::exception_ptr error, size_t batch, size_t message) {
    if (!DecompressionErrorsStructCreated) {
        with_lock (DecompressionErrorsStructLock) {
            DecompressionErrors.resize(ServerMessage.batches_size());
            for (size_t batch = 0; batch < static_cast<size_t>(ServerMessage.batches_size()); ++batch) {
                DecompressionErrors[batch].resize(static_cast<size_t>(ServerMessage.batches(batch).message_data_size()));
            }

            // Set barrier.
            DecompressionErrorsStructCreated = true;
        }
    }
    Y_ASSERT(batch < DecompressionErrors.size());
    Y_ASSERT(message < DecompressionErrors[batch].size());
    DecompressionErrors[batch][message] = std::move(error);
}

template<bool UseMigrationProtocol>
std::exception_ptr TDataDecompressionInfo<UseMigrationProtocol>::GetDecompressionError(size_t batch, size_t message) {
    if (!DecompressionErrorsStructCreated) {
        return {};
    }
    Y_ASSERT(batch < DecompressionErrors.size());
    Y_ASSERT(message < DecompressionErrors[batch].size());
    return DecompressionErrors[batch][message];
}

template <bool UseMigrationProtocol>
i64 TDataDecompressionInfo<UseMigrationProtocol>::StartDecompressionTasks(
    const typename IAExecutor<UseMigrationProtocol>::TPtr& executor, i64 availableMemory,
    TDeferredActions<UseMigrationProtocol>& deferred)
{
    auto session = Session.lock();
    Y_ASSERT(session);

    i64 used = 0;

    while (availableMemory > 0 && !Tasks.empty()) {
        auto& task = Tasks.front();

        used += task.GetEstimatedDecompressedSize();
        availableMemory -= task.GetEstimatedDecompressedSize();

        session->OnCreateNewDecompressionTask();

        deferred.DeferStartExecutorTask(executor, std::move(task));
        Tasks.pop_front();
    }

    return used;
}

template<bool UseMigrationProtocol>
void TDataDecompressionInfo<UseMigrationProtocol>::PlanDecompressionTasks(double averageCompressionRatio,
                                                                          TIntrusivePtr<TPartitionStreamImpl<UseMigrationProtocol>> partitionStream)
{
    constexpr size_t TASK_LIMIT = 512_KB;

    auto session = Session.lock();
    Y_ASSERT(session);

    ReadyThresholds.emplace_back();

    TDecompressionTask task(TDataDecompressionInfo::shared_from_this(), partitionStream, &ReadyThresholds.back());

    while (CurrentDecompressingMessage.first < static_cast<size_t>(ServerMessage.batches_size())) {
        const auto& batch = ServerMessage.batches(CurrentDecompressingMessage.first);

        if (CurrentDecompressingMessage.second < static_cast<size_t>(batch.message_data_size())) {
            const auto& messageData = batch.message_data(CurrentDecompressingMessage.second);
            const i64 size = static_cast<i64>(messageData.data().size());
            const i64 estimatedDecompressedSize = messageData.uncompressed_size()
                                                      ? static_cast<i64>(messageData.uncompressed_size())
                                                      : static_cast<i64>(size * averageCompressionRatio);
            Y_VERIFY(estimatedDecompressedSize >= 0);

            task.Add(CurrentDecompressingMessage.first, CurrentDecompressingMessage.second, size, estimatedDecompressedSize);

            session->GetEventsQueue()->PushDataEvent(partitionStream,
                                                     CurrentDecompressingMessage.first,
                                                     CurrentDecompressingMessage.second,
                                                     TDataDecompressionInfo::shared_from_this(),
                                                     ReadyThresholds.back().Ready);
        }

        ++CurrentDecompressingMessage.second;

        if (CurrentDecompressingMessage.second >= static_cast<size_t>(batch.message_data_size())) { // next batch
            ++CurrentDecompressingMessage.first;
            CurrentDecompressingMessage.second = 0;
        }

        if (task.AddedDataSize() >= TASK_LIMIT) {
            Tasks.push_back(std::move(task));

            ReadyThresholds.emplace_back();
            task = TDecompressionTask(TDataDecompressionInfo::shared_from_this(), partitionStream, &ReadyThresholds.back());
        }
    }

    if (task.AddedMessagesCount() > 0) {
        Tasks.push_back(std::move(task));
    } else {
        ReadyThresholds.pop_back(); // Revert.
    }
}

template<bool UseMigrationProtocol>
void TDataDecompressionEvent<UseMigrationProtocol>::TakeData(TIntrusivePtr<TPartitionStreamImpl<UseMigrationProtocol>> partitionStream,
                                                             TVector<typename TAReadSessionEvent<UseMigrationProtocol>::TDataReceivedEvent::TMessage>* messages,
                                                             TVector<typename TAReadSessionEvent<UseMigrationProtocol>::TDataReceivedEvent::TCompressedMessage>* compressedMessages,
                                                             size_t* maxByteSize,
                                                             TUserRetrievedEventInfoAccumulator<UseMigrationProtocol>* accumulator) const
{
    auto& msg = Parent->GetServerMessage();
    i64 minOffset = Max<i64>();
    i64 maxOffset = 0;
    auto& batch = *msg.mutable_batches(Batch);
    const auto& meta = Parent->GetBatchMeta(Batch);

    const TInstant batchWriteTimestamp = [&batch](){
        if constexpr (UseMigrationProtocol) {
            return TInstant::MilliSeconds(batch.write_timestamp_ms());
        } else {
            return TInstant::MilliSeconds(::google::protobuf::util::TimeUtil::TimestampToMilliseconds(batch.written_at()));
        }
    }();
    auto& messageData = *batch.mutable_message_data(Message);

    minOffset = Min(minOffset, static_cast<i64>(messageData.offset()));
    maxOffset = Max(maxOffset, static_cast<i64>(messageData.offset()));

    if constexpr (UseMigrationProtocol) {
        TReadSessionEvent::TDataReceivedEvent::TMessageInformation messageInfo(messageData.offset(),
                                                                               batch.source_id(),
                                                                               messageData.seq_no(),
                                                                               TInstant::MilliSeconds(messageData.create_timestamp_ms()),
                                                                               batchWriteTimestamp,
                                                                               batch.ip(),
                                                                               meta,
                                                                               messageData.uncompressed_size());

        if (Parent->GetDoDecompress()) {
            messages->emplace_back(messageData.data(),
                                   Parent->GetDecompressionError(Batch, Message),
                                   messageInfo,
                                   partitionStream,
                                   messageData.partition_key(),
                                   messageData.explicit_hash());
        } else {
            compressedMessages->emplace_back(static_cast<ECodec>(messageData.codec()),
                                             messageData.data(),
                                             TVector<TReadSessionEvent::TDataReceivedEvent::TMessageInformation>{messageInfo},
                                             partitionStream,
                                             messageData.partition_key(),
                                             messageData.explicit_hash());
        }
    } else {
        NTopic::TReadSessionEvent::TDataReceivedEvent::TMessageInformation messageInfo(messageData.offset(),
                                                                                       batch.producer_id(),
                                                                                       messageData.seq_no(),
                                                                                       TInstant::MilliSeconds(::google::protobuf::util::TimeUtil::TimestampToMilliseconds(messageData.created_at())),
                                                                                       batchWriteTimestamp,
                                                                                       meta,
                                                                                       messageData.uncompressed_size(),
                                                                                       messageData.message_group_id());

        if (Parent->GetDoDecompress()) {
            messages->emplace_back(messageData.data(),
                                   Parent->GetDecompressionError(Batch, Message),
                                   messageInfo,
                                   partitionStream);
        } else {
            compressedMessages->emplace_back(static_cast<NTopic::ECodec>(batch.codec()),
                                             messageData.data(),
                                             messageInfo,
                                             partitionStream);
        }
    }

    *maxByteSize -= Min(*maxByteSize, messageData.data().size());

    if (accumulator) {
        accumulator->OnTakeData(Parent, messageData.data().size());
    }

    // Clear data to free internal session's memory.
    messageData.clear_data();

    partitionStream->GetLog().Write(TLOG_DEBUG, TStringBuilder()
                                    << "Take Data. Partition " << partitionStream->GetPartitionId()
                                    << ". Read: {" << Batch << ", " << Message << "} ("
                                    << minOffset << "-" << maxOffset << ")");
}

template<bool UseMigrationProtocol>
bool TDataDecompressionInfo<UseMigrationProtocol>::HasReadyUnreadData() const {
    TMaybe<std::pair<size_t, size_t>> threshold = GetReadyThreshold();
    if (!threshold) {
        return false;
    }
    return CurrentReadingMessage <= *threshold;
}

template<bool UseMigrationProtocol>
void TDataDecompressionInfo<UseMigrationProtocol>::OnDataDecompressed(i64 sourceSize, i64 estimatedDecompressedSize, i64 decompressedSize, size_t messagesCount, i64 serverBytesSize)
{
    CompressedDataSize -= sourceSize;
    DecompressedDataSize += decompressedSize;

    if (auto session = Session.lock()) {
        session->OnDataDecompressed(sourceSize, estimatedDecompressedSize, decompressedSize, messagesCount, serverBytesSize);
    }
}

template<bool UseMigrationProtocol>
void TDataDecompressionInfo<UseMigrationProtocol>::OnUserRetrievedEvent(i64 decompressedSize, size_t messagesCount)
{
    MessagesInflight -= messagesCount;
    DecompressedDataSize -= decompressedSize;

    if (auto session = Session.lock()) {
        session->OnUserRetrievedEvent(decompressedSize, messagesCount);
    }
}

template <bool UseMigrationProtocol>
void TDataDecompressionInfo<UseMigrationProtocol>::TDecompressionTask::Add(size_t batch, size_t message,
                                                                           size_t sourceDataSize,
                                                                           size_t estimatedDecompressedSize) {
    if (Messages.empty() || Messages.back().Batch != batch) {
        Messages.push_back({ batch, { message, message + 1 } });
    }
    Messages.back().MessageRange.second = message + 1;
    SourceDataSize += sourceDataSize;
    EstimatedDecompressedSize += estimatedDecompressedSize;
    Ready->Batch = batch;
    Ready->Message = message;
}

template <bool UseMigrationProtocol>
TDataDecompressionInfo<UseMigrationProtocol>::TDecompressionTask::TDecompressionTask(
    TDataDecompressionInfo::TPtr parent, TIntrusivePtr<TPartitionStreamImpl<UseMigrationProtocol>> partitionStream,
    TReadyMessageThreshold* ready)
    : Parent(std::move(parent))
    , PartitionStream(std::move(partitionStream))
    , Ready(ready) {
}

template<bool UseMigrationProtocol>
void TDataDecompressionInfo<UseMigrationProtocol>::TDecompressionTask::operator()() {
    i64 minOffset = Max<i64>();
    i64 maxOffset = 0;
    const i64 partition_id = [this](){
        if constexpr (UseMigrationProtocol) {
            return Parent->ServerMessage.partition();
        } else {
            return Parent->ServerMessage.partition_session_id();
        }
    }();
    i64 dataProcessed = 0;
    size_t messagesProcessed = 0;
    for (const TMessageRange& messages : Messages) {
        auto& batch = *Parent->ServerMessage.mutable_batches(messages.Batch);
        for (size_t i = messages.MessageRange.first; i < messages.MessageRange.second; ++i) {
            auto& data = *batch.mutable_message_data(i);

            ++messagesProcessed;
            dataProcessed += static_cast<i64>(data.data().size());
            minOffset = Min(minOffset, static_cast<i64>(data.offset()));
            maxOffset = Max(maxOffset, static_cast<i64>(data.offset()));

            try {

                if constexpr (UseMigrationProtocol) {
                    if (Parent->DoDecompress
                        && data.codec() != Ydb::PersQueue::V1::CODEC_RAW
                        && data.codec() != Ydb::PersQueue::V1::CODEC_UNSPECIFIED
                    ) {
                        TString decompressed = NCompressionDetails::Decompress(data);
                        data.set_data(decompressed);
                        data.set_codec(Ydb::PersQueue::V1::CODEC_RAW);
                    }
                } else {
                    if (Parent->DoDecompress
                        && static_cast<Ydb::Topic::Codec>(batch.codec()) != Ydb::Topic::CODEC_RAW
                        && static_cast<Ydb::Topic::Codec>(batch.codec()) != Ydb::Topic::CODEC_UNSPECIFIED
                    ) {
                        TString decompressed = ::NYdb::NTopic::NCompressionDetails::Decompress(data, static_cast<Ydb::Topic::Codec>(batch.codec()));
                        data.set_data(decompressed);
                    }
                }

                DecompressedSize += data.data().size();
            } catch (...) {
                Parent->PutDecompressionError(std::current_exception(), messages.Batch, i);
                data.clear_data(); // Free memory, because we don't count it.

                std::shared_ptr<TSingleClusterReadSessionImpl<UseMigrationProtocol>> session = Parent->Session.lock();
                if (session) {
                    session->GetLog() << TLOG_INFO << "Error decompressing data: " << CurrentExceptionMessage();
                }
            }
        }
    }
    if (auto session = Parent->Session.lock()) {
        session->GetLog().Write(TLOG_DEBUG, TStringBuilder() << "Decompression task done. Partition/PartitionSessionId: "
                                                             << partition_id << " (" << minOffset << "-"
                                                             << maxOffset << ")");
    }
    Y_ASSERT(dataProcessed == SourceDataSize);
    std::shared_ptr<TSingleClusterReadSessionImpl<UseMigrationProtocol>> session = Parent->Session.lock();

    Parent->OnDataDecompressed(SourceDataSize, EstimatedDecompressedSize, DecompressedSize, messagesProcessed, Parent->ServerBytesSize);

    Parent->SourceDataNotProcessed -= dataProcessed;
    Ready->Ready = true;

    if (session) {
        session->GetEventsQueue()->SignalReadyEvents(PartitionStream.Get());
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDeferredActions

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::DeferReadFromProcessor(const typename IProcessor<UseMigrationProtocol>::TPtr& processor,
                                              TServerMessage<UseMigrationProtocol>* dst,
                                              typename IProcessor<UseMigrationProtocol>::TReadCallback callback)
{
    Y_ASSERT(!Processor);
    Y_ASSERT(!ReadDst);
    Y_ASSERT(!ReadCallback);
    Processor = processor;
    ReadDst = dst;
    ReadCallback = std::move(callback);
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::DeferStartExecutorTask(const typename IAExecutor<UseMigrationProtocol>::TPtr& executor, typename IAExecutor<UseMigrationProtocol>::TFunction task) {
    ExecutorsTasks.emplace_back(executor, std::move(task));
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::DeferAbortSession(const typename IErrorHandler<UseMigrationProtocol>::TPtr& errorHandler, TASessionClosedEvent<UseMigrationProtocol>&& closeEvent) {
    ErrorHandler = errorHandler;
    SessionClosedEvent.ConstructInPlace(std::move(closeEvent));
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::DeferAbortSession(const typename IErrorHandler<UseMigrationProtocol>::TPtr& errorHandler, EStatus statusCode, NYql::TIssues&& issues) {
    DeferAbortSession(errorHandler, TASessionClosedEvent<UseMigrationProtocol>(statusCode, std::move(issues)));
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::DeferAbortSession(const typename IErrorHandler<UseMigrationProtocol>::TPtr& errorHandler, EStatus statusCode, const TString& message) {
    NYql::TIssues issues;
    issues.AddIssue(message);
    DeferAbortSession(errorHandler, statusCode, std::move(issues));
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::DeferAbortSession(const typename IErrorHandler<UseMigrationProtocol>::TPtr& errorHandler, TPlainStatus&& status) {
    DeferAbortSession(errorHandler, TASessionClosedEvent<UseMigrationProtocol>(std::move(status)));
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::DeferReconnection(std::shared_ptr<TSingleClusterReadSessionImpl<UseMigrationProtocol>> session, const typename IErrorHandler<UseMigrationProtocol>::TPtr& errorHandler, TPlainStatus&& status) {
    Session = std::move(session);
    ErrorHandler = errorHandler;
    ReconnectionStatus = std::move(status);
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::DeferStartSession(std::shared_ptr<TSingleClusterReadSessionImpl<UseMigrationProtocol>> session) {
    Sessions.push_back(std::move(session));
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::DeferSignalWaiter(TWaiter&& waiter) {
    Waiters.emplace_back(std::move(waiter));
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::DeferDestroyDecompressionInfos(std::vector<TDataDecompressionInfoPtr<UseMigrationProtocol>>&& infos)
{
    DecompressionInfos = std::move(infos);
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::DoActions() {
    Read();
    StartExecutorTasks();
    AbortSession();
    Reconnect();
    SignalWaiters();
    StartSessions();
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::StartSessions() {
    for (auto& session : Sessions) {
        session->Start();
    }
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::Read() {
    if (ReadDst) {
        Y_ASSERT(Processor);
        Y_ASSERT(ReadCallback);
        Processor->Read(ReadDst, std::move(ReadCallback));
    }
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::StartExecutorTasks() {
    for (auto&& [executor, task] : ExecutorsTasks) {
        executor->Post(std::move(task));
    }
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::AbortSession() {
    if (SessionClosedEvent) {
        Y_ASSERT(ErrorHandler);
        ErrorHandler->AbortSession(std::move(*SessionClosedEvent));
    }
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::Reconnect() {
    if (Session) {
        Y_ASSERT(ErrorHandler);
        if (!Session->Reconnect(ReconnectionStatus)) {
            ErrorHandler->AbortSession(std::move(ReconnectionStatus));
        }
    }
}

template<bool UseMigrationProtocol>
void TDeferredActions<UseMigrationProtocol>::SignalWaiters() {
    for (auto& w : Waiters) {
        w.Signal();
    }
}

template<bool UseMigrationProtocol>
void TErrorHandler<UseMigrationProtocol>::AbortSession(TASessionClosedEvent<UseMigrationProtocol>&& closeEvent) {
    if (auto session = Session.lock()) {
        session->Abort(std::move(closeEvent));
    }
}

#define HISTOGRAM_SETUP ::NMonitoring::ExplicitHistogram({0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100})

template <typename TReaderCounters>
void MakeCountersNotNull(TReaderCounters& counters) {
    if (!counters.Errors) {
        counters.Errors = MakeIntrusive<::NMonitoring::TCounterForPtr>(true);
    }

    if (!counters.CurrentSessionLifetimeMs) {
        counters.CurrentSessionLifetimeMs = MakeIntrusive<::NMonitoring::TCounterForPtr>(false);
    }

    if (!counters.BytesRead) {
        counters.BytesRead = MakeIntrusive<::NMonitoring::TCounterForPtr>(true);
    }

    if (!counters.MessagesRead) {
        counters.MessagesRead = MakeIntrusive<::NMonitoring::TCounterForPtr>(true);
    }

    if (!counters.BytesReadCompressed) {
        counters.BytesReadCompressed = MakeIntrusive<::NMonitoring::TCounterForPtr>(true);
    }

    if (!counters.BytesInflightUncompressed) {
        counters.BytesInflightUncompressed = MakeIntrusive<::NMonitoring::TCounterForPtr>(false);
    }

    if (!counters.BytesInflightCompressed) {
        counters.BytesInflightCompressed = MakeIntrusive<::NMonitoring::TCounterForPtr>(false);
    }

    if (!counters.BytesInflightTotal) {
        counters.BytesInflightTotal = MakeIntrusive<::NMonitoring::TCounterForPtr>(false);
    }

    if (!counters.MessagesInflight) {
        counters.MessagesInflight = MakeIntrusive<::NMonitoring::TCounterForPtr>(false);
    }


    if (!counters.TotalBytesInflightUsageByTime) {
        counters.TotalBytesInflightUsageByTime = MakeIntrusive<::NMonitoring::THistogramCounter>(HISTOGRAM_SETUP);
    }

    if (!counters.UncompressedBytesInflightUsageByTime) {
        counters.UncompressedBytesInflightUsageByTime = MakeIntrusive<::NMonitoring::THistogramCounter>(HISTOGRAM_SETUP);
    }

    if (!counters.CompressedBytesInflightUsageByTime) {
        counters.CompressedBytesInflightUsageByTime = MakeIntrusive<::NMonitoring::THistogramCounter>(HISTOGRAM_SETUP);
    }
}

#undef HISTOGRAM_SETUP

template <typename TReaderCounters>
bool HasNullCounters(TReaderCounters& counters) {
    return !counters.Errors
        || !counters.CurrentSessionLifetimeMs
        || !counters.BytesRead
        || !counters.MessagesRead
        || !counters.BytesReadCompressed
        || !counters.BytesInflightUncompressed
        || !counters.BytesInflightCompressed
        || !counters.BytesInflightTotal
        || !counters.MessagesInflight
        || !counters.TotalBytesInflightUsageByTime
        || !counters.UncompressedBytesInflightUsageByTime
        || !counters.CompressedBytesInflightUsageByTime;
}

} // namespace NYdb::NPersQueue
