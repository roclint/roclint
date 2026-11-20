#include "HostController.hpp"

#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QUuid>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace roclint::host {
namespace {
using core::intent::WorkingIntentState;

template <typename T> QJsonArray strings(const T &values) {
    QJsonArray out;
    for (const auto &value : values)
        out.append(QString::fromStdString(value));
    return out;
}
QJsonObject stringMap(const std::map<std::string, std::string> &values) {
    QJsonObject out;
    for (const auto &[id, value] : values)
        out.insert(QString::fromStdString(id), QString::fromStdString(value));
    return out;
}
std::string encodeAnswers(const WorkingIntentState &state) {
    QJsonObject assumptions;
    for (const auto &[id, value] : state.assumptionDecisions)
        assumptions.insert(QString::fromStdString(id), value);
    QJsonArray superseded;
    for (const auto &item : state.supersededDecisions)
        superseded.append(QJsonObject{{"id", QString::fromStdString(item.itemId)},
                                      {"value", QString::fromStdString(item.previousValue)},
                                      {"kind", QString::fromStdString(item.previousKind)},
                                      {"time", qint64(item.supersededAtMs)}});
    return QJsonDocument(QJsonObject{{"choices", stringMap(state.confirmedAlternatives)},
                                     {"custom", stringMap(state.customResolutions)},
                                     {"unknown", stringMap(state.uncertaintyNotes)},
                                     {"assumptions", assumptions},
                                     {"kept", strings(state.keptOriginalItemIds)},
                                     {"skipped", strings(state.skippedItemIds)},
                                     {"presented", strings(state.presentedItemIds)},
                                     {"manual", state.manualModeRequested},
                                     {"rounds", qint64(state.roundsCompleted)},
                                     {"superseded", superseded}})
        .toJson(QJsonDocument::Compact)
        .toStdString();
}
QString q(const std::string &s) { return QString::fromStdString(s); }

QJsonObject encodeRecord(const core::intent::InterviewRecord &record) {
    QJsonArray choices;
    for (const auto &choice : record.choices)
        choices.append(
            QJsonObject{{"id", q(choice.id)}, {"label", q(choice.label)}, {"consequence", q(choice.consequence)}});
    return {{"id", q(record.id)},
            {"item", q(record.analysisItemId)},
            {"question", q(record.question)},
            {"consequence", q(record.consequence)},
            {"choices", choices},
            {"excerpt", q(record.sourceExcerpt)},
            {"ref", q(record.sourceRef)},
            {"origin", q(record.origin)},
            {"disposition", q(record.disposition)},
            {"answer", q(record.rawAnswer)},
            {"choice", q(record.selectedChoiceId)},
            {"previous", strings(record.previousAnswers)},
            {"asked", record.asked}};
}

core::intent::InterviewRecord decodeRecord(const QJsonObject &object) {
    auto get = [&](const char *key) { return object.value(key).toString().toStdString(); };
    core::intent::InterviewRecord record;
    record.id = get("id");
    record.analysisItemId = get("item");
    record.question = get("question");
    record.consequence = get("consequence");
    record.sourceExcerpt = get("excerpt");
    record.sourceRef = get("ref");
    record.origin = get("origin");
    record.disposition = get("disposition");
    record.rawAnswer = get("answer");
    record.selectedChoiceId = get("choice");
    record.asked = object["asked"].toBool();
    for (const auto &answer : object["previous"].toArray())
        record.previousAnswers.push_back(answer.toString().toStdString());
    for (const auto &value : object["choices"].toArray()) {
        const auto c = value.toObject();
        record.choices.push_back({c["id"].toString().toStdString(), c["label"].toString().toStdString(),
                                  c["consequence"].toString().toStdString()});
    }
    return record;
}

} // namespace

std::optional<core::intent::WorkingIntentState> HostController::checkpointAnswers(const storage::RestoredSession &row) {
    if (!row.fieldCheckpoint || !row.analysis || row.fieldCheckpoint->snapshotId != row.analysis->id ||
        row.fieldCheckpoint->revisionId != row.latestRevision.revision.revisionId().value() ||
        row.fieldCheckpoint->draft != row.latestRevision.revision.text())
        return {};
    const auto data = QJsonDocument::fromJson(QByteArray::fromStdString(row.fieldCheckpoint->answers)).object();
    if (data.isEmpty())
        return {};
    WorkingIntentState result;
    auto readMap = [&](const char *key, auto &map) {
        const auto items = data.value(key).toObject();
        for (auto it = items.begin(); it != items.end(); ++it)
            map[it.key().toStdString()] = it.value().toString().toStdString();
    };
    readMap("choices", result.confirmedAlternatives);
    readMap("custom", result.customResolutions);
    readMap("unknown", result.uncertaintyNotes);
    const auto assumptions = data.value("assumptions").toObject();
    for (auto it = assumptions.begin(); it != assumptions.end(); ++it)
        result.assumptionDecisions[it.key().toStdString()] = it.value().toBool();
    auto readList = [&](const char *key, auto &list) {
        for (const auto &value : data.value(key).toArray())
            list.push_back(value.toString().toStdString());
    };
    readList("kept", result.keptOriginalItemIds);
    readList("skipped", result.skippedItemIds);
    readList("presented", result.presentedItemIds);
    result.roundsCompleted = data.value("rounds").toInteger();
    result.manualModeRequested = data.value("manual").toBool();
    for (const auto &value : data.value("superseded").toArray()) {
        const auto item = value.toObject();
        result.supersededDecisions.push_back({item["id"].toString().toStdString(),
                                              item["value"].toString().toStdString(),
                                              item["kind"].toString().toStdString(), item["time"].toInteger()});
    }
    return result;
}

bool HostController::otherFieldVisible() const {
    return m_fieldSwitching || m_pipelineField != m_focusedField;
}
QString HostController::focusedFieldDraft() const { return m_fields.value(m_focusedField).draft; }
QString HostController::focusedFieldName() const { return m_fields.value(m_focusedField).source.field.appDisplayName; }
bool HostController::fieldReviewQueued() const { return m_explicitFields.contains(m_focusedField); }

void HostController::markFieldDirty(FieldSession &f) {
    ++f.generation;
    f.dirty = true;
    if (!m_fieldCheckpointTimer.isActive())
        m_fieldCheckpointTimer.start();
}

void HostController::rememberHudDraft(const QString &text) {
    auto it = m_fields.find(m_pipelineField.isEmpty() ? m_detachedField : m_pipelineField);
    if (it == m_fields.end() || otherFieldVisible())
        return;
    ++it->draftVersion;
    it->editedInHud = true;
    it->draft = text;
    it->revisionId.clear();
    it->snapshotId.clear();
    it->answers.clear();
    it->question.clear();
    it->choices.clear();
    markFieldDirty(*it);
    if (!m_pipelineField.isEmpty()) queueAutomaticField(m_pipelineField);
}

void HostController::checkpointWorkingAnswers() {
    if (m_hostState == HostState::Quitting)
        return;
    auto it = m_fields.find(m_pipelineField.isEmpty() ? m_detachedField : m_pipelineField);
    if (isDraftDirty() || it == m_fields.end() || !m_currentSnapshot || !m_revisionState ||
        it->sessionId.toStdString() != currentSessionId() || it->draft != currentRevisionText())
        return;
    it->revisionId = currentRevisionId();
    it->snapshotId = QString::fromStdString(m_currentSnapshot->id);
    auto data = QJsonDocument::fromJson(QByteArray::fromStdString(encodeAnswers(m_workingIntent))).object();
    QJsonArray records;
    for (const auto &record : m_interviewRecords)
        records.append(encodeRecord(record));
    data.insert("interview", QJsonObject{{"records", records},
                                         {"active", m_interviewActive},
                                         {"complete", m_interviewComplete},
                                         {"turns", m_interviewTurnsCompleted},
                                         {"question", m_interviewQuestion},
                                         {"choices", QJsonArray::fromVariantList(m_interviewChoices)},
                                         {"consequence", m_interviewConsequence},
                                         {"feedback", m_interviewFeedback},
                                         {"current", q(m_interviewCurrentRecordId)}});
    it->question = m_interviewActive ? m_interviewQuestion : m_activeQuestion;
    it->choices = m_interviewActive ? m_interviewChoices : m_activeAlternatives;
    it->answers = QJsonDocument(data).toJson(QJsonDocument::Compact).toStdString();
    markFieldDirty(*it);
}

void HostController::restoreFieldInterview(const storage::RestoredSession &row) {
    if (!checkpointAnswers(row))
        return;
    const auto interview = QJsonDocument::fromJson(QByteArray::fromStdString(row.fieldCheckpoint->answers))
                               .object()
                               .value("interview")
                               .toObject();
    if (interview.isEmpty())
        return;
    m_interviewRecords.clear();
    for (const auto &record : interview["records"].toArray())
        m_interviewRecords.push_back(decodeRecord(record.toObject()));
    m_interviewActive = interview["active"].toBool();
    m_interviewComplete = interview["complete"].toBool();
    m_interviewTurnsCompleted = interview["turns"].toInt();
    m_interviewQuestion = interview["question"].toString();
    m_interviewChoices = interview["choices"].toArray().toVariantList();
    m_interviewConsequence = interview["consequence"].toString();
    m_interviewFeedback = interview["feedback"].toString();
    m_interviewCurrentRecordId = interview["current"].toString().toStdString();
    emit interviewChanged();
}

QString HostController::observeSessionField(const AmbientObservation &observation) {
    const auto &f = observation.field;
    const QString key = QStringLiteral("%1/%2/%3/%4/")
                            .arg(f.appIdentifier.toLower())
                            .arg(f.processId)
                            .arg(f.fieldToken)
                            .arg(m_workspaceEpoch) +
                        f.documentUrl;
    const bool changed = m_focusedField != key;
    if (changed && m_fields.contains(m_focusedField) && !m_fields.value(m_focusedField).invoked)
        m_fields.remove(m_focusedField);
    auto &slot = m_fields[key];
    const bool edited = slot.source.textHash != observation.textHash;
    if (!f.documentUrl.isEmpty() && (slot.source.field.documentUrl != f.documentUrl || changed)) {
        const auto file = QFileInfo(QUrl(f.documentUrl).toLocalFile()).canonicalFilePath();
        const auto root = QFileInfo(m_projectScanPath).canonicalFilePath();
        slot.project = !m_projectScanPath.isEmpty() && !file.isEmpty() && !root.isEmpty() &&
                               file.startsWith(root + QDir::separator())
                           ? root
                           : QString{};
    }
    slot.workspaceEpoch = m_workspaceEpoch;
    slot.source = observation;
    if (edited) {
        ++slot.draftVersion;
        slot.editedInHud = false;
        slot.draft = observation.prompt;
        slot.revisionId.clear();
        slot.snapshotId.clear();
        slot.answers.clear();
        slot.question.clear();
        slot.choices.clear();
        if (slot.invoked) {
            markFieldDirty(slot);
            queueAutomaticField(key);
        }
    }
    if (changed) {
        flushFieldCheckpoints();
        m_focusedField = key;
        m_fieldMatches.clear();
        if (!slot.invoked && isStorageAvailable())
            findFieldMatches(key);
    }
    if (changed || edited)
        emit fieldViewChanged();
    if (changed || (edited && slot.invoked && key != m_pipelineField))
        scheduleFieldWork();
    return key;
}

AsyncTask<void> HostController::findFieldMatches(QString key) {
    const auto slot = m_fields.value(key);
    auto result = co_await m_storage->call(&storage::PromptRepository::matchingFields,
                                           slot.source.field.appIdentifier.toStdString(), slot.project.toStdString());
    if (!result.ok() || m_focusedField != key || !m_fields.contains(key))
        co_return;
    m_fieldMatches.clear();
    for (const auto &checkpoint : result.value()) {
        const auto id = QString::fromStdString(checkpoint.sessionId);
        if (std::any_of(m_fields.cbegin(), m_fields.cend(), [&](const auto &f) { return f.sessionId == id; }))
            continue;
        m_fieldMatches.append(
            QVariantMap{{"sessionId", id}, {"preview", QString::fromStdString(checkpoint.draft).left(100)}});
    }
    emit fieldViewChanged();
}

void HostController::resumeFieldDraft(const QString &sessionId) {
    for (const auto &candidate : m_fieldMatches) {
        if (candidate.toMap().value("sessionId").toString() == sessionId) {
            restoreSessionForReview(sessionId, stateVersion());
            return;
        }
    }
}

AsyncTask<void> HostController::flushFieldCheckpoints() {
    if (!isStorageAvailable())
        co_return;
    while (m_fieldSaveTask && !m_fieldSaveTask->done()) {
        auto active = *m_fieldSaveTask;
        co_await active;
    }
    m_fieldSaveTask = writeFieldCheckpoints();
    auto writing = *m_fieldSaveTask;
    co_await writing;
}

AsyncTask<void> HostController::writeFieldCheckpoints() {
    m_fieldSaveRunning = true;
    const auto pending = m_fields;
    for (auto entry = pending.cbegin(); entry != pending.cend(); ++entry) {
        const auto key = entry.key();
        const auto &slot = entry.value();
        if (!slot.invoked || !slot.dirty || slot.sessionId.isEmpty())
            continue;
        storage::FieldCheckpoint checkpoint{slot.sessionId.toStdString(),
                                            slot.source.field.appIdentifier.toStdString(),
                                            slot.project.toStdString(),
                                            slot.draft.toStdString(),
                                            slot.revisionId.toStdString(),
                                            slot.snapshotId.toStdString(),
                                            slot.answers,
                                            slot.generation};
        const auto status = co_await m_storage->call(&storage::PromptRepository::saveFieldCheckpoint, checkpoint);
        auto it = m_fields.find(key);
        if (it != m_fields.end() && it->sessionId == slot.sessionId && it->generation == slot.generation) {
            if (status.ok())
                it->dirty = false;
            else
                setLastError(status.error());
        }
    }
    m_fieldSaveRunning = false;
    finishQuitWhenRuntimesStopped();
}

void HostController::forgetFieldSessions(const QString &sessionId) {
    ++m_fieldRegistryEpoch;
    for (auto it = m_fields.begin(); it != m_fields.end();) {
        if (!sessionId.isEmpty() && it->sessionId != sessionId) {
            ++it;
            continue;
        }
        const auto key = it.key();
        m_explicitFields.removeAll(key);
        m_automaticFields.removeAll(key);
        if (m_detachedField == key) m_detachedField.clear();
        if (m_pipelineField == key)
            m_pipelineField.clear();
        if (m_focusedField == key)
            m_focusedField.clear();
        it = m_fields.erase(it);
    }
    m_fieldMatches.clear();
    emit fieldViewChanged();
}

void HostController::reviewFocusedField() {
    if (m_focusedField.isEmpty())
        return;
    admitFieldReview(m_focusedField, true);
}

quint64 HostController::admitFieldReview(const QString &key, bool requestReview) {
    m_fields[key].invoked = true;
    return enqueueStorageCommand("queueFieldReview", [this, key, requestReview]() -> AsyncTask<core::Status> {
        if (!m_fields.contains(key))
            co_return core::makeError(core::ErrorCode::StaleVersion, "The field changed.");
        auto slot = m_fields.value(key);
        if (slot.sessionId.isEmpty() && key != m_pipelineField) {
            const core::SessionId id("sess_" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
            const auto now = QDateTime::currentMSecsSinceEpoch();
            const core::PromptRevision root(id, core::RevisionState::makeRevisionId(id, 1), std::nullopt,
                                            slot.draft.toStdString(), now);
            auto status = co_await m_repository->createSessionWithRootRevision(root, now);
            if (!status.ok())
                co_return status;
            if (!m_fields.contains(key))
                co_return core::makeError(core::ErrorCode::StaleVersion, "The field changed.");
            m_fields[key].sessionId = QString::fromStdString(id.value());
            m_fields[key].revisionId = QString::fromStdString(root.revisionId().value());
            markFieldDirty(m_fields[key]);
            co_await flushFieldCheckpoints();
            refreshStorageStats();
        }
        if (requestReview && !m_explicitFields.contains(key))
            m_explicitFields.append(key);
        m_automaticFields.removeAll(key);
        emit fieldViewChanged();
        co_return core::Status::success();
    });
}

void HostController::queueAutomaticField(const QString &key) {
    auto &field = m_fields[key];
    if (field.draft.trimmed().isEmpty() || m_explicitFields.contains(key)) {
        m_automaticFields.removeAll(key);
        return;
    }
    if (!m_automaticFields.contains(key))
        m_automaticFields.append(key);
    field.automaticDue = QDeadlineTimer(800);
    if (!m_fieldReviewTimer.isActive())
        m_fieldReviewTimer.start(800);
}

void HostController::scheduleFieldWork() {
    if (m_fieldSwitching || !isStorageAvailable() || m_hostState == HostState::Quitting || isAnalyzing() ||
        m_proposalStatus == ProposalUiStatus::Generating || m_reviewState == ReviewState::Running || m_inFlight ||
        m_interviewBusy || m_pendingDelivery || m_commandRunning || !m_storageCommands.empty())
        return;
    QString key;
    const bool explicitRequest = !m_explicitFields.isEmpty();
    bool review = explicitRequest;
    if (explicitRequest) {
        key = m_explicitFields.takeFirst();
    } else {
        qint64 wait = -1;
        for (const auto &pending : std::as_const(m_automaticFields)) {
            const auto remaining = m_fields.value(pending).automaticDue.remainingTime();
            if (remaining == 0) {
                key = pending;
                break;
            }
            if (wait < 0 || remaining < wait)
                wait = remaining;
        }
        if (!key.isEmpty()) {
            review = true;
            m_automaticFields.removeAll(key);
        } else {
            if (wait >= 0)
                m_fieldReviewTimer.start(static_cast<int>(wait));
            if (otherFieldVisible() && m_fields.value(m_focusedField).invoked)
                key = m_focusedField;
        }
    }
    if (key.isEmpty())
        return;
    if (m_fields.value(key).workspaceEpoch != m_workspaceEpoch) {
        if (!m_explicitFields.isEmpty() || !m_automaticFields.isEmpty())
            QTimer::singleShot(0, this, &HostController::scheduleFieldWork);
        return;
    }
    m_fieldSwitching = true;
    const auto epoch = m_fieldRegistryEpoch;
    emit fieldViewChanged();
    const auto id = enqueueStorageCommand(
        "activateField",
        [this, key, review, explicitRequest, epoch]() -> AsyncTask<core::Status> {
            core::Status status = core::Status::success();
            if (epoch == m_fieldRegistryEpoch && m_fields.contains(key) &&
                m_fields.value(key).workspaceEpoch == m_workspaceEpoch)
                status = co_await activateField(key, review, explicitRequest);
            m_fieldSwitching = false;
            if (m_pipelineField == key && m_revisionState && m_captureTarget && m_fields.contains(key) &&
                m_captureTarget->textHash != m_fields.value(key).source.textHash) {
                const auto field = m_fields.value(key);
                noteDraftEditedInternal(stateVersion(), false);
                m_captureTarget = field.source;
                if (m_focusedField == key) emit ambientDraftUpdated(field.draft);
            }
            emit fieldViewChanged();
            co_return status;
        },
        true);
    if (!id) {
        m_fieldSwitching = false;
        emit fieldViewChanged();
    }
}

AsyncTask<core::Status> HostController::activateField(QString key, bool review, bool explicitRequest) {
    const auto epoch = m_workspaceEpoch;
    auto slot = m_fields.value(key);
    if (key != m_pipelineField || !m_revisionState) {
        checkpointWorkingAnswers();
        co_await flushFieldCheckpoints();
        if (epoch != m_workspaceEpoch || !m_fields.contains(key))
            co_return core::makeError(core::ErrorCode::StaleVersion, "The workspace changed.");
        m_pipelineField.clear();
        m_currentApproval.reset(); // Parking a field preserves its durable approval.
        resetSessionState();
        m_pipelineField = key;
        if (!slot.sessionId.isEmpty()) {
            auto restored = co_await restoreSessionOperation(slot.sessionId, 0);
            if (!restored.ok())
                co_return restored;
        } else if (!slot.draft.trimmed().isEmpty()) {
            auto created = co_await ensureSession(slot.draft.toStdString(), QDateTime::currentMSecsSinceEpoch());
            if (!created.ok())
                co_return created;
            m_fields[key].sessionId = QString::fromStdString(currentSessionId());
            m_fields[key].revisionId = currentRevisionId();
            markFieldDirty(m_fields[key]);
        }
    }
    if (epoch != m_workspaceEpoch || !m_fields.contains(key))
        co_return core::makeError(core::ErrorCode::StaleVersion, "The workspace changed.");
    slot = m_fields.value(key);
    if (m_hostContextEnabled) {
        const auto &field = slot.source.field;
        m_hostContext = HostContextObservation{HostContextDetector::mapIdentityToCategory(field.appIdentifier),
                                               field.appDisplayName, field.appIdentifier};
        emit hostContextChanged();
    }
    m_captureTarget = slot.source;
    m_captureSource = CaptureSource::Ambient;
    m_widgetDraftEdited = slot.editedInHud;
    m_followAmbientDraft = !slot.editedInHud;
    if (m_revisionState && currentRevisionText() != slot.draft && !isDraftDirty()) {
        noteDraftEditedInternal(stateVersion(), false);
    }
    if (auto status = co_await flushDraftEdits(); !status.ok())
        co_return status;
    emit captureSourceChanged();
    if (m_focusedField == key)
        emit ambientDraftUpdated(slot.draft);
    if (review && !slot.draft.trimmed().isEmpty()) {
        co_return co_await analyzePromptOperation(slot.draft, stateVersion(),
                                                  explicitRequest ? core::analysis::ReviewTrigger::ExplicitReview
                                                                  : core::analysis::ReviewTrigger::Automatic);
    }
    co_return core::Status::success();
}
} // namespace roclint::host
